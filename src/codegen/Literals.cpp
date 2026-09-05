#include "../CodeGenImpl.h"

namespace dragon {

void CodeGen::visit(IntegerLiteral& node) {
    impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, node.value);
}

void CodeGen::visit(FloatLiteral& node) {
    impl_->lastValue = llvm::ConstantFP::get(impl_->f64Type, node.value);
}

void CodeGen::visit(StringLiteral& node) {
    if (node.isFString) {
        std::vector<llvm::Value*> parts;
        bool lastPartBorrowedStr = false;
        for (auto& part : node.fstringParts) {
            lastPartBorrowedStr = false;
            if (part.kind == FStringPart::Kind::Literal) {
                std::string processed = impl_->processEscapes(part.literal, node.isRaw);
                parts.push_back(impl_->emitStringLiteralBytes(processed));
                continue;
            }

            if (!part.expr) {
                parts.push_back(impl_->emitStringLiteralBytes("{}"));
                continue;
            }

            std::string fClassName = impl_->resolveExprClassName(part.expr.get());
            part.expr->accept(*this);
            llvm::Value* exprVal = impl_->lastValue;

            auto partBorrows = [&](llvm::Value* v) {
                return Impl::isBorrowedHeapExpr(part.expr.get()) ||
                       !impl_->isOwnedStrResult(v);
            };

            const std::string& formatSpec = part.formatSpec;
            llvm::Value* strVal;
            if (!formatSpec.empty() && exprVal->getType() == impl_->f64Type) {
                auto* specStr = impl_->builder->CreateGlobalString(formatSpec, "fmtspec");
                auto fmtFn = impl_->runtimeFuncs["dragon_float_format"];
                strVal = impl_->builder->CreateCall(fmtFn, {exprVal, specStr}, "ffmt");
            } else if (!formatSpec.empty() && exprVal->getType() == impl_->i64Type) {
                auto* specStr = impl_->builder->CreateGlobalString(formatSpec, "fmtspec");
                auto fmtFn = impl_->runtimeFuncs["dragon_int_format"];
                strVal = impl_->builder->CreateCall(fmtFn, {exprVal, specStr}, "ifmt");
            } else if (!formatSpec.empty() && exprVal->getType() == impl_->i1Type) {
                llvm::Value* ext = impl_->builder->CreateZExt(exprVal, impl_->i64Type);
                auto* specStr = impl_->builder->CreateGlobalString(formatSpec, "fmtspec");
                auto fmtFn = impl_->runtimeFuncs["dragon_int_format"];
                strVal = impl_->builder->CreateCall(fmtFn, {ext, specStr}, "ifmt");
            } else if (!formatSpec.empty() && exprVal->getType()->isPointerTy() &&
                       (impl_->resolveExprVarKind(part.expr.get()) == Impl::VarKind::Str ||
                        (part.expr->type &&
                         part.expr->type->kind() == Type::Kind::Str))) {
                const std::string& s = formatSpec;
                size_t p = 0;
                char fill = ' ';
                char align = '<';
                bool sawAlign = false;
                auto isAlign = [](char c) {
                    return c == '<' || c == '>' || c == '^' || c == '=';
                };
                if (s.size() - p >= 2 && isAlign(s[p + 1])) {
                    fill = s[p]; align = s[p + 1]; p += 2; sawAlign = true;
                } else if (s.size() - p >= 1 && isAlign(s[p])) {
                    align = s[p]; p += 1; sawAlign = true;
                }
                long width = 0;
                bool sawWidth = false;
                while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
                    width = width * 10 + (s[p] - '0'); p++; sawWidth = true;
                }
                bool typeOk = (p == s.size());
                if (!typeOk && p == s.size() - 1 && s[p] == 's') {
                    p++; typeOk = true;
                }
                if (!typeOk || align == '=') {
                    impl_->addError("invalid format spec '" + s +
                                    "' for str value", node.location());
                    strVal = exprVal;
                    lastPartBorrowedStr = partBorrows(exprVal);
                } else if (!sawWidth) {
                    (void)sawAlign;
                    strVal = exprVal;
                    lastPartBorrowedStr = partBorrows(exprVal);
                } else {
                    std::string rt = align == '>' ? "dragon_str_rjust"
                                   : align == '^' ? "dragon_str_center"
                                                  : "dragon_str_ljust";
                    auto* fn = impl_->getOrDeclareRuntime(rt,
                        llvm::FunctionType::get(impl_->i8PtrType,
                            {impl_->i8PtrType, impl_->i64Type,
                             llvm::Type::getInt8Ty(*impl_->context)}, false));
                    auto* widthV = llvm::ConstantInt::get(impl_->i64Type, width);
                    auto* fillV = llvm::ConstantInt::get(
                        llvm::Type::getInt8Ty(*impl_->context),
                        static_cast<uint8_t>(fill));
                    strVal = impl_->builder->CreateCall(fn, {exprVal, widthV, fillV},
                                                        "strpad");
                }
            } else {
                auto rendered = impl_->emitRenderToStr(
                    part.expr.get(), part.expr->type.get(), exprVal, fClassName);
                if (!rendered.value) {
                    impl_->addError(
                        fClassName.empty()
                            ? unknownRenderTypeMessage("f-string interpolation")
                            : unrenderableClassMessage("f-string interpolation",
                                                       fClassName),
                        node.location());
                    strVal = impl_->emitStringLiteralBytes("");
                } else {
                    strVal = rendered.value;
                    if (rendered.value == exprVal)
                        lastPartBorrowedStr = partBorrows(exprVal);
                    if (rendered.consumedSource)
                        impl_->emitDecrefByKind(
                            exprVal,
                            impl_->renderedSourceDrainKind(part.expr.get(), exprVal));
                }
            }
            parts.push_back(strVal);
        }

        if (parts.empty()) {
            impl_->lastValue = impl_->builder->CreateGlobalString("");
        } else if (parts.size() == 1 && lastPartBorrowedStr &&
                   impl_->options.gcMode == GCMode::RC) {
            // Single borrowed part: consumers own f-string results, so hand out our
            // own +1 - else `out = f"{e}"` steals the source's ref (UAF on over-release).
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_str_retain"], {parts[0]}, "fstr.retain");
        } else {
            llvm::Value* result = parts[0];
            for (size_t k = 1; k < parts.size(); k++) {
                llvm::Value* prev = result;
                result = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_str_concat"], {prev, parts[k]}, "fstr");
                if (k > 1 && impl_->options.gcMode == GCMode::RC) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_decref_str"], {prev});
                }
                // Decref only OWNED conversion results: a borrowed part like {d['k']}
                // is also a CallInst but the dict keeps that +1 - decref'ing it here is a UAF.
                if (impl_->options.gcMode == GCMode::RC &&
                    impl_->isOwnedStrResult(parts[k])) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_decref_str"], {parts[k]});
                }
            }
            if (parts.size() > 1 && impl_->options.gcMode == GCMode::RC &&
                impl_->isOwnedStrResult(parts[0])) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref_str"], {parts[0]});
            }
            impl_->lastValue = result;
        }
        return;
    }
    if (node.isBytes) {
        std::string processed = impl_->processEscapes(node.value, node.isRaw);
        auto* dataPtr = impl_->builder->CreateGlobalString(
            llvm::StringRef(processed.data(), processed.size()));
        auto* lenVal = llvm::ConstantInt::get(impl_->i64Type, (int64_t)processed.size());
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_bytes_from_literal"], {dataPtr, lenVal}, "bytes");
        return;
    }
    std::string processed = impl_->processEscapes(node.value, node.isRaw);
    impl_->lastValue = impl_->emitStringLiteralBytes(processed);
}

static std::string spliceSite(const std::string& exprText) {
    return "template splice `!{" + exprText + "}`";
}

static std::string contextFilterTrustClass(const std::string& filterName) {
    if (filterName == "html") return "HTML";
    if (filterName == "sql") return "SQL";
    if (filterName == "url") return "URL";
    return std::string();
}

static std::string templateRenderError(const std::string& exprText,
                                       const std::string& className) {
    if (!className.empty())
        return unrenderableClassMessage(spliceSite(exprText), className);
    return unknownRenderTypeMessage(spliceSite(exprText));
}

void CodeGen::visit(TemplateExpr& node) {
    if (!node.contentType.empty() && !node.isContentAlias) {
        std::string ownMod = impl_->resolveClassOwningModule(node.contentType);
        if (impl_->resolveMethodFunction(ownMod, node.contentType, "build")) {
            emitSqlTemplate(node, node.contentType);
            return;
        }
    }

    std::string effContent = node.contentType;
    if (effContent.empty() && node.isContentAlias &&
        !impl_->templateContextStack.empty()) {
        effContent = impl_->templateContextStack.back();
    }
    impl_->templateContextStack.push_back(effContent);

    auto emitStringify = [&](Expr* src, llvm::Value* v, const std::string& cls,
                             bool wantOwned) -> llvm::Value* {
        auto rendered = impl_->emitRenderToStr(
            src, src ? src->type.get() : nullptr, v, cls);
        if (!rendered.value) {
            impl_->addError(cls.empty()
                                ? unknownRenderTypeMessage("reactive interpolation")
                                : unrenderableClassMessage("reactive interpolation",
                                                           cls),
                            node.location());
            return impl_->emitStringLiteralBytes("");
        }
        if (rendered.consumedSource)
            impl_->emitDecrefByKind(v, impl_->renderedSourceDrainKind(src, v));
        if (wantOwned && !rendered.owned && impl_->options.gcMode == GCMode::RC)
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_incref_str"],
                                       {rendered.value});
        return rendered.value;
    };

    auto isSignalReceiver = [&](Expr* recv) -> bool {
        if (!recv || !recv->type) return false;
        auto* instance = dynamic_cast<InstanceType*>(recv->type.get());
        if (!instance || !instance->classType) return false;
        const ClassType& signalClass = *instance->classType;
        if (signalClass.genericOrigin != kSignalTemplateKey) return false;
        return impl_->hasDunder(signalClass.name, kSignalCallDunder) ||
               impl_->hasDunder(signalClass.name, kSignalGetMethod);
    };

    std::function<void(Expr*, bool&, bool&)> analyzeReactive =
        [&](Expr* e, bool& signalRead, bool& localRef) {
        if (!e) return;
        if (auto* n = dynamic_cast<NameExpr*>(e)) {
            if (impl_->lookupVar(n->name)) localRef = true;
            return;
        }
        if (auto* call = dynamic_cast<CallExpr*>(e)) {
            if (call->args.empty()) {
                if (isSignalReceiver(call->callee.get())) signalRead = true;
                if (auto* attr = dynamic_cast<AttributeExpr*>(call->callee.get())) {
                    if ((attr->attribute == "get" || attr->attribute == "__call__") &&
                        isSignalReceiver(attr->object.get()))
                        signalRead = true;
                }
            }
            analyzeReactive(call->callee.get(), signalRead, localRef);
            for (auto& a : call->args) analyzeReactive(a.get(), signalRead, localRef);
            for (auto& kw : call->kwArgs) analyzeReactive(kw.second.get(), signalRead, localRef);
            return;
        }
        if (auto* bin = dynamic_cast<BinaryExpr*>(e)) {
            analyzeReactive(bin->left.get(), signalRead, localRef);
            analyzeReactive(bin->right.get(), signalRead, localRef);
            return;
        }
        if (auto* un = dynamic_cast<UnaryExpr*>(e)) {
            analyzeReactive(un->operand.get(), signalRead, localRef);
            return;
        }
        if (auto* attr = dynamic_cast<AttributeExpr*>(e)) {
            analyzeReactive(attr->object.get(), signalRead, localRef);
            return;
        }
        if (auto* sub = dynamic_cast<SubscriptExpr*>(e)) {
            analyzeReactive(sub->object.get(), signalRead, localRef);
            analyzeReactive(sub->index.get(), signalRead, localRef);
            return;
        }
    };

    const std::string& val = node.body;
    std::vector<llvm::Value*> parts;
    for (auto& tp : node.templateParts) {
        if (tp.kind == TemplatePart::Kind::Literal) {
            if (!tp.literal.empty())
                parts.push_back(impl_->emitStringLiteralBytes(tp.literal));
        } else {
            const size_t bangPos = tp.bangPos;
            const std::string& exprText = tp.exprText;

            std::string filterName = tp.filterName;
            if (tp.isSpread) {
                if (filterName.empty()) {
                    filterName = "join";
                } else if (filterName != "raw") {
                    impl_->addError(
                        "Template spread `!{*expr}` cannot be combined with "
                        "an explicit `| " + filterName + "` filter",
                        node.location());
                }
            }

            bool blockMode = (tp.kind == TemplatePart::Kind::Block);
            auto& blockStmts = tp.blockStmts;
            Expr* fExpr = tp.expr.get();

            if (blockMode) {
                llvm::Value* buf = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_list_new_ptr"],
                    {llvm::ConstantInt::get(impl_->i64Type, 0),
                     llvm::ConstantInt::get(impl_->i64Type, TAG_STR)},
                    "tpl_blk_buf");
                impl_->templateBlockBufferStack.push_back(buf);
                for (auto& stmt : blockStmts) {
                    stmt->accept(*this);
                }
                impl_->templateBlockBufferStack.pop_back();
                llvm::Value* emptySep = impl_->builder->CreateGlobalString("");
                llvm::Value* joined = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_str_join_ptr"],
                    {emptySep, buf}, "tpl_blk_joined");
                impl_->emitDecrefByKind(buf, Impl::VarKind::List);
                parts.push_back(joined);
                continue;
            }

            if (fExpr) {
                std::string fClassName = impl_->resolveExprClassName(fExpr);

                fExpr->accept(*this);
                llvm::Value* exprVal = impl_->lastValue;

                if (isEventAttrContext(val, bangPos) &&
                    (dynamic_cast<LambdaExpr*>(fExpr) ||
                     llvm::isa<llvm::Function>(exprVal))) {
                    auto* regFn = impl_->module->getFunction("ui__register_callback");
                    if (!regFn) {
                        impl_->addError(
                            "event-handler interpolation (e.g. `onclick=!{...}`) "
                            "requires `import ui`", node.location());
                        parts.push_back(impl_->emitStringLiteralBytes(""));
                        continue;
                    }
                    llvm::Value* cb = exprVal;
                    if (llvm::isa<llvm::Function>(cb)) {
                        auto* fnI8 = impl_->builder->CreateBitCast(cb, impl_->i8PtrType);
                        auto* nullEnv = llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(impl_->i8PtrType));
                        cb = impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_closure_create"],
                            {fnI8, nullEnv}, "evh.wrap");
                    } else if (!cb->getType()->isPointerTy()) {
                        cb = impl_->builder->CreateIntToPtr(cb, impl_->i8PtrType);
                    }
                    auto* paramTy = regFn->getFunctionType()->getParamType(0);
                    if (cb->getType() != paramTy)
                        cb = impl_->builder->CreateBitCast(cb, paramTy);
                    llvm::Value* cid =
                        impl_->builder->CreateCall(regFn, {cb}, "evh.id");
                    parts.push_back(
                        impl_->emitStringLiteralBytes("window.dr.invoke("));
                    parts.push_back(impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_int_to_str"], {cid}, "evh.id.str"));
                    parts.push_back(impl_->emitStringLiteralBytes(")"));
                    continue;
                }

                {
                    bool inAttr = !precedingAttrName(val, bangPos).empty();
                    bool signalRead = false, localRef = false;
                    if (!inAttr) analyzeReactive(fExpr, signalRead, localRef);
                    if (signalRead) {
                        auto* bindFn = impl_->module->getFunction("ui__bind_text");
                        if (!bindFn) {
                            impl_->addError(
                                "reactive interpolation (e.g. `!{count()}` over a "
                                "Signal) requires `import ui`", node.location());
                            parts.push_back(impl_->emitStringLiteralBytes(""));
                            continue;
                        }
                        if (localRef) {
                            impl_->addError(
                                "reactive interpolation `!{" + exprText + "}` may "
                                "reference only module-global names in this release; "
                                "it reads a local. Move the Signal (and anything it "
                                "derives from) to module scope, or use an explicit "
                                "`effect()`.", node.location());
                            parts.push_back(impl_->emitStringLiteralBytes(""));
                            continue;
                        }

                        llvm::Value* staticStr =
                            emitStringify(fExpr, exprVal, fClassName, false);

                        std::string fnName =
                            "__dragon_reactive_" + std::to_string(impl_->lambdaCounter++);
                        auto* fnTy = llvm::FunctionType::get(impl_->i8PtrType, {}, false);
                        auto* renderFn = llvm::Function::Create(
                            fnTy, llvm::Function::InternalLinkage, fnName,
                            impl_->module.get());
                        {
                            auto* prevFunc = impl_->currentFunction;
                            auto* prevBlock = impl_->builder->GetInsertBlock();
                            auto savedScopes = std::move(impl_->scopes);
                            impl_->scopes.clear();
                            auto savedCellPromoted = std::move(impl_->cellPromotedLocals);
                            impl_->cellPromotedLocals.clear();

                            impl_->currentFunction = renderFn;
                            auto* rEntry = llvm::BasicBlock::Create(
                                *impl_->context, "entry", renderFn);
                            impl_->builder->SetInsertPoint(rEntry);
                            impl_->pushScope();

                            std::string rcls = impl_->resolveExprClassName(fExpr);
                            fExpr->accept(*this);
                            llvm::Value* rval = impl_->lastValue;
                            llvm::Value* rstr = emitStringify(fExpr, rval, rcls, true);
                            impl_->emitScopeCleanup();
                            impl_->builder->CreateRet(rstr);

                            impl_->popScope();
                            impl_->scopes = std::move(savedScopes);
                            impl_->cellPromotedLocals = std::move(savedCellPromoted);
                            impl_->currentFunction = prevFunc;
                            if (prevBlock) impl_->builder->SetInsertPoint(prevBlock);
                        }

                        auto* fnI8 = impl_->builder->CreateBitCast(renderFn, impl_->i8PtrType);
                        auto* nullEnv = llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(impl_->i8PtrType));
                        llvm::Value* closure = impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_closure_create"],
                            {fnI8, nullEnv}, "rx.wrap");
                        auto* bindParamTy = bindFn->getFunctionType()->getParamType(0);
                        if (closure->getType() != bindParamTy)
                            closure = impl_->builder->CreateBitCast(closure, bindParamTy);
                        llvm::Value* nid =
                            impl_->builder->CreateCall(bindFn, {closure}, "rx.nid");

                        parts.push_back(impl_->emitStringLiteralBytes("<span data-dr=\""));
                        parts.push_back(impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_int_to_str"], {nid}, "rx.nid.str"));
                        parts.push_back(impl_->emitStringLiteralBytes("\">"));
                        parts.push_back(staticStr);
                        parts.push_back(impl_->emitStringLiteralBytes("</span>"));
                        continue;
                    }
                }

                bool isJoinFilter = (filterName == "join" ||
                                     filterName.rfind("join(", 0) == 0);
                if (isJoinFilter || (tp.isSpread && filterName == "raw")) {
                    parts.push_back(emitTemplateJoin(node, tp, fExpr, exprVal,
                                                     effContent,
                                                     filterName == "raw"));
                    continue;
                }

                auto rendered = impl_->emitRenderToStr(
                    fExpr, fExpr->type.get(), exprVal, fClassName);
                if (!rendered.value) {
                    impl_->addError(templateRenderError(exprText, fClassName),
                                    node.location());
                    parts.push_back(impl_->emitStringLiteralBytes(""));
                    continue;
                }
                llvm::Value* strVal = rendered.value;
                bool strValOwned = rendered.owned;
                if (rendered.consumedSource)
                    impl_->emitDecrefByKind(exprVal,
                                            impl_->renderedSourceDrainKind(fExpr, exprVal));

                auto applyFilter = [&](const std::string& fnKey, const std::string& twine) {
                    llvm::Value* prev = strVal;
                    strVal = impl_->builder->CreateCall(
                        impl_->runtimeFuncs[fnKey], {strVal}, twine);
                    if (strValOwned && impl_->options.gcMode == GCMode::RC) {
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_decref_str"], {prev});
                    }
                    strValOwned = true;
                };

                if (!filterName.empty()) {
                    if (filterName == "raw") {
                    } else if (filterName == "html" || filterName == "sql" ||
                               filterName == "url") {
                        if (filterName == "html")
                            applyFilter("dragon_template_escape_html", "esc_html");
                        else if (filterName == "sql")
                            applyFilter("dragon_template_escape_sql", "esc_sql");
                        else
                            applyFilter("dragon_template_escape_url", "esc_url");
                        strVal = impl_->emitContentEscape(
                            effContent, strVal, strValOwned,
                            contextFilterTrustClass(filterName));
                    } else {
                        auto* filterFunc = impl_->module->getFunction(
                            impl_->resolveCalleeSymbol(filterName));
                        if (!filterFunc) {
                            impl_->addError("Unknown template filter: " + filterName,
                                            node.location());
                            parts.push_back(impl_->emitStringLiteralBytes(""));
                            continue;
                        }
                        llvm::Value* prev = strVal;
                        llvm::Value* filtered = impl_->builder->CreateCall(
                            filterFunc, {strVal}, "filter_" + filterName);
                        if (strValOwned && impl_->options.gcMode == GCMode::RC) {
                            impl_->builder->CreateCall(
                                impl_->runtimeFuncs["dragon_decref_str"], {prev});
                        }
                        if (tp.filterReturnClass.empty()) {
                            strVal = filtered;
                            strValOwned = true;
                        } else {
                            auto filteredStr = impl_->emitRenderToStr(
                                nullptr, nullptr, filtered, tp.filterReturnClass);
                            if (!filteredStr.value) {
                                impl_->addError(
                                    unrenderableClassMessage(spliceSite(exprText),
                                                             tp.filterReturnClass),
                                    node.location());
                                parts.push_back(impl_->emitStringLiteralBytes(""));
                                continue;
                            }
                            if (filteredStr.consumedSource &&
                                impl_->isOwnedPtrResult(filtered))
                                impl_->emitDecrefByKind(filtered, Impl::VarKind::List);
                            strVal = filteredStr.value;
                            strValOwned = filteredStr.owned;
                        }
                        strVal = impl_->emitContentEscape(effContent, strVal,
                                                          strValOwned,
                                                          tp.filterReturnClass);
                    }
                } else {
                    strVal = impl_->emitContentEscape(effContent, strVal,
                                                      strValOwned, fClassName);
                }

                parts.push_back(strVal);
            } else {
                parts.push_back(impl_->emitStringLiteralBytes("!{" + exprText + "}"));
            }
        }
    }

    llvm::Value* result;
    if (parts.empty()) {
        result = impl_->emitStringLiteralBytes("");
    } else {
        result = parts[0];
        for (size_t k = 1; k < parts.size(); k++) {
            llvm::Value* prev = result;
            result = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_str_concat"], {prev, parts[k]}, "tpl");
            if (k > 1 && impl_->options.gcMode == GCMode::RC) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref_str"], {prev});
            }
            if (impl_->options.gcMode == GCMode::RC &&
                impl_->isOwnedStrResult(parts[k])) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref_str"], {parts[k]});
            }
        }
        if (parts.size() > 1 && impl_->options.gcMode == GCMode::RC &&
            impl_->isOwnedStrResult(parts[0])) {
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_decref_str"], {parts[0]});
        }
    }
    if (!node.contentType.empty() && !node.isContentAlias) {
        std::string symPrefix = impl_->classSymPrefix(node.contentType);

        std::string validateFn = symPrefix + "_validate";
        auto* valFunc = impl_->module->getFunction(validateFn);
        if (valFunc) {
            impl_->builder->CreateCall(valFunc, {result});
        }

        std::string newFn = symPrefix + "_new";
        auto* ctorFunc = impl_->module->getFunction(newFn);
        if (ctorFunc) {
            llvm::Value* innerStr = result;
            result = impl_->builder->CreateCall(ctorFunc, {innerStr}, "tpl_inst");
            if (impl_->options.gcMode == GCMode::RC &&
                impl_->isOwnedStrResult(innerStr)) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref_str"], {innerStr});
            }
        }
    }
    impl_->lastValue = result;

    if (!impl_->templateContextStack.empty()) {
        impl_->templateContextStack.pop_back();
    }
}

llvm::Value* CodeGen::emitTemplateJoin(TemplateExpr& node, const TemplatePart& part,
                                       Expr* listExpr, llvm::Value* listVal,
                                       const std::string& contentType,
                                       bool elementsRaw) {
    auto& b = *impl_->builder;
    const std::string site = spliceSite(part.exprText);

    llvm::Value* sepVal = nullptr;
    bool sepOwned = false;
    Expr* sepExpr = part.separatorExpr.get();
    if (!sepExpr) {
        sepVal = b.CreateGlobalString("");
    } else {
        std::string sepClass = impl_->resolveExprClassName(sepExpr);
        sepExpr->accept(*this);
        llvm::Value* sepSource = impl_->lastValue;
        auto sepRendered = impl_->emitRenderToStr(sepExpr, sepExpr->type.get(),
                                                  sepSource, sepClass);
        if (!sepRendered.value) {
            impl_->addError(templateRenderError(part.exprText, sepClass),
                            node.location());
            sepVal = b.CreateGlobalString("");
        } else {
            sepVal = sepRendered.value;
            sepOwned = sepRendered.owned;
            if (sepRendered.consumedSource)
                impl_->emitDecrefByKind(
                    sepSource, impl_->renderedSourceDrainKind(sepExpr, sepSource));
            if (!elementsRaw)
                sepVal = impl_->emitContentEscape(contentType, sepVal, sepOwned,
                                                  sepClass);
        }
    }

    const Type* elemType = nullptr;
    if (listExpr->type)
        if (auto* listType = dynamic_cast<ListType*>(listExpr->type.get()))
            elemType = listType->elementType.get();
    Type::Kind elemKind = elemType ? elemType->kind() : Type::Kind::Unknown;
    std::string elemClass = elemType ? impl_->renderClassName(elemType) : "";
    if (!elemType) {
        if (auto* listName = dynamic_cast<NameExpr*>(listExpr)) {
            auto kindIt = impl_->varListElemKinds.find(listName->name);
            if (kindIt != impl_->varListElemKinds.end()) elemKind = kindIt->second;
            auto classIt = impl_->varListElemClassName.find(listName->name);
            if (classIt != impl_->varListElemClassName.end()) {
                elemClass = classIt->second;
                elemKind = Type::Kind::Instance;
            }
        }
    }

    bool sameContentType = !elemClass.empty() && elemClass == contentType;
    bool needsEscape = !contentType.empty() && !elementsRaw && !sameContentType;

    if (elemKind == Type::Kind::Str && !needsEscape) {
        llvm::Value* joined = b.CreateCall(
            impl_->runtimeFuncs["dragon_str_join_ptr"], {sepVal, listVal}, "tpl_join");
        if (sepOwned && impl_->options.gcMode == GCMode::RC)
            b.CreateCall(impl_->runtimeFuncs["dragon_decref_str"], {sepVal});
        impl_->emitDecrefByKind(listVal, impl_->ownedTempDrainKind(listExpr, listVal));
        return joined;
    }

    if (elemKind == Type::Kind::Unknown) {
        impl_->addError(unknownRenderTypeMessage(site), node.location());
        return impl_->emitStringLiteralBytes("");
    }

    llvm::Function* func = impl_->currentFunction;
    llvm::Value* buf = b.CreateCall(
        impl_->runtimeFuncs["dragon_list_new_ptr"],
        {llvm::ConstantInt::get(impl_->i64Type, 0),
         llvm::ConstantInt::get(impl_->i64Type, TAG_STR)}, "tpl_join_buf");
    auto* idxSlot = impl_->createEntryAlloca(func, "tpl.join.i", impl_->i64Type);
    b.CreateStore(llvm::ConstantInt::get(impl_->i64Type, 0), idxSlot);
    llvm::Value* count = b.CreateCall(
        impl_->runtimeFuncs["dragon_list_len"], {listVal}, "tpl.join.n");

    auto* condBB = llvm::BasicBlock::Create(*impl_->context, "tpl.join.cond", func);
    auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, "tpl.join.body", func);
    auto* endBB = llvm::BasicBlock::Create(*impl_->context, "tpl.join.end", func);

    b.CreateBr(condBB);
    b.SetInsertPoint(condBB);
    llvm::Value* idx = b.CreateLoad(impl_->i64Type, idxSlot, "tpl.join.idx");
    b.CreateCondBr(b.CreateICmpSLT(idx, count, "tpl.join.more"), bodyBB, endBB);

    b.SetInsertPoint(bodyBB);
    auto* elemSlot = impl_->bindListElemByTypeKind(func, listVal, idx,
                                                   "tpl.join.elem", elemKind);
    llvm::Value* elem = b.CreateLoad(elemSlot->getAllocatedType(), elemSlot,
                                     "tpl.join.e");
    auto rendered = impl_->emitRenderToStr(nullptr, elemType, elem, elemClass);
    if (!rendered.value) {
        impl_->addError(elemClass.empty()
                            ? unrenderableTypeMessage(site, "list element")
                            : unrenderableClassMessage(site, elemClass),
                        node.location());
        b.CreateBr(endBB);
        b.SetInsertPoint(endBB);
        return impl_->emitStringLiteralBytes("");
    }
    llvm::Value* elemStr = rendered.value;
    bool elemOwned = rendered.owned;
    if (!elementsRaw)
        elemStr = impl_->emitContentEscape(contentType, elemStr, elemOwned, elemClass);
    if (!elemOwned && impl_->options.gcMode == GCMode::RC)
        b.CreateCall(impl_->runtimeFuncs["dragon_incref_str"], {elemStr});
    b.CreateCall(impl_->runtimeFuncs["dragon_list_append_ptr"], {buf, elemStr});
    b.CreateStore(b.CreateAdd(idx, llvm::ConstantInt::get(impl_->i64Type, 1),
                              "tpl.join.next"), idxSlot);
    b.CreateBr(condBB);

    b.SetInsertPoint(endBB);
    llvm::Value* joined = b.CreateCall(
        impl_->runtimeFuncs["dragon_str_join_ptr"], {sepVal, buf}, "tpl_join");
    impl_->emitDecrefByKind(buf, Impl::VarKind::List);
    if (sepOwned && impl_->options.gcMode == GCMode::RC)
        b.CreateCall(impl_->runtimeFuncs["dragon_decref_str"], {sepVal});
    impl_->emitDecrefByKind(listVal, impl_->ownedTempDrainKind(listExpr, listVal));
    return joined;
}

void CodeGen::emitSqlTemplate(TemplateExpr& node, const std::string& contentType) {
    const std::string& val = node.body;

    TemplateIncludeContext sqlIncludes;
    llvm::Value* params = impl_->builder->CreateCall(
        impl_->runtimeFuncs["dragon_list_box_new"],
        {llvm::ConstantInt::get(impl_->i64Type, 0)}, "sql.params");

    std::string canonical;
    int paramIndex = 0;

    size_t i = 0;
    while (i < val.size()) {
        if (val[i] == '!' && i + 1 < val.size() && val[i+1] == '!' &&
            i + 2 < val.size() && val[i+2] == '{') {
            canonical += "!{"; i += 3;
        } else if (val[i] == '!' && i + 1 < val.size() && val[i+1] == '!' &&
                   i + 2 < val.size() && val[i+2] == '}') {
            canonical += "}"; i += 3;
        } else if (val[i] == '!' && i + 1 < val.size() && val[i+1] == '{') {
            size_t start = i + 2;
            int depth = 1;
            size_t j = start;
            while (j < val.size() && depth > 0) {
                if (val[j] == '{') depth++;
                else if (val[j] == '}') depth--;
                if (depth > 0) j++;
            }
            if (depth > 0) {
                impl_->addError("template[" + contentType + "]: unterminated "
                                "'!{' parameter slot: no matching '}' before "
                                "the end of the template. Write '!!{' for a "
                                "literal '!{'.", node.location());
                break;
            }
            std::string exprText = val.substr(start, j - start);
            i = j + 1;

            LexerOptions fLexOpts;
            fLexOpts.filename = node.location().filename;
            Lexer fLexer(exprText, fLexOpts);
            auto fTokens = fLexer.tokenize();
            ParserOptions fOpts;
            fOpts.isDragonFile = true;
            fOpts.filename = node.location().filename;
            fOpts.templateIncludes = &sqlIncludes;
            Parser fParser(std::move(fTokens), fOpts);
            auto fExpr = fParser.parseExpression();
            for (auto& includeError : sqlIncludes.errors)
                impl_->addError("template[" + contentType + "]: " + includeError,
                                node.location());
            const bool sqlIncludeFailed = !sqlIncludes.errors.empty();
            sqlIncludes.errors.clear();
            if (sqlIncludeFailed) {
                canonical += "$$" + std::to_string(paramIndex++);
                continue;
            }
            if (!fExpr || fParser.hasErrors()) {
                impl_->addError("template[" + contentType + "]: each !{...} must be "
                                "a single bound expression (block interpolation in "
                                "SQL templates is not supported)", node.location());
                canonical += "$$" + std::to_string(paramIndex++);
                continue;
            }

            if (impl_->resolveExprClassName(fExpr.get()) == contentType) {
                impl_->addError("template[" + contentType + "]: composing a nested "
                                + contentType + " value (!{sql_expr}) is not "
                                "implemented yet", node.location());
                continue;
            }

            fExpr->accept(*this);
            llvm::Value* exprVal = impl_->lastValue;

            llvm::Type* t = exprVal->getType();
            int64_t tag;
            if (dynamic_cast<NoneLiteral*>(fExpr.get())) tag = TAG_NONE;
            else if (t == impl_->f64Type) tag = TAG_FLOAT;
            else if (t == impl_->i1Type) tag = TAG_BOOL;
            else if (t->isPointerTy()) tag = TAG_STR;
            else tag = TAG_INT;

            if (tag == 1 && t->isPointerTy()) {
                exprVal = impl_->ensureHeapString(exprVal, fExpr.get());
                if (impl_->options.gcMode == GCMode::RC &&
                    Impl::isBorrowedHeapExpr(fExpr.get()))
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_incref_str"], {exprVal});
            }

            llvm::Value* payload = impl_->nativeToPayloadI64(exprVal);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_box_append"],
                {params, llvm::ConstantInt::get(impl_->i64Type, tag), payload});

            canonical += "$$" + std::to_string(paramIndex++);
        } else {
            size_t lstart = i;
            while (i < val.size()) {
                if (val[i] == '!' && i + 1 < val.size()) {
                    if (val[i+1] == '{') break;
                    if (val[i+1] == '!' && i + 2 < val.size() &&
                        (val[i+2] == '{' || val[i+2] == '}')) break;
                }
                i++;
            }
            canonical += val.substr(lstart, i - lstart);
        }
    }

    llvm::Value* canonVal = impl_->internSqlCanonical(canonical);
    llvm::Value* hashVal = llvm::ConstantInt::get(
        impl_->i64Type, (int64_t)impl_->sqlCanonicalHash(canonical));

    std::string newFn = impl_->classSymPrefix(contentType) + "_new";
    auto* ctorFunc = impl_->module->getFunction(newFn);
    if (!ctorFunc) {
        impl_->addError("template[" + contentType + "]: missing constructor " +
                        newFn + "(canonical, hash, params)", node.location());
        impl_->lastValue = params;
        return;
    }
    llvm::Value* sqlVal = impl_->builder->CreateCall(
        ctorFunc, {canonVal, hashVal, params}, "sql.value");

    if (impl_->options.gcMode == GCMode::RC)
        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {params});

    impl_->lastValue = sqlVal;
}

void CodeGen::visit(TemplateFileExpr& node) {
    if (!node.expansion) {
        impl_->addError("template file '" + node.filePath +
                        "' was not loaded; see the earlier compile error",
                        node.location());
        impl_->lastValue = impl_->builder->CreateGlobalString("");
        return;
    }
    visit(*node.expansion);
}

void CodeGen::visit(BooleanLiteral& node) {
    impl_->lastValue = llvm::ConstantInt::get(impl_->i1Type, node.value ? 1 : 0);
}

void CodeGen::visit(NoneLiteral&) {
    impl_->lastValue = llvm::ConstantPointerNull::get(
        llvm::PointerType::getUnqual(*impl_->context));
}

}
