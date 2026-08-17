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
            } else if (!fClassName.empty() && impl_->hasDunder(fClassName, "__str__") &&
                       (exprVal->getType() == impl_->i8PtrType ||
                        exprVal->getType()->isPointerTy())) {
                strVal = impl_->callDunder(fClassName, "__str__", exprVal);
            } else if (!fClassName.empty() && impl_->hasDunder(fClassName, "__repr__") &&
                       (exprVal->getType() == impl_->i8PtrType ||
                        exprVal->getType()->isPointerTy())) {
                strVal = impl_->callDunder(fClassName, "__repr__", exprVal);
            } else if (exprVal->getType() == impl_->i8PtrType ||
                       exprVal->getType()->isPointerTy()) {
                std::string creprFn = impl_->containerReprFn(part.expr.get());
                if (!creprFn.empty()) {
                    strVal = impl_->builder->CreateCall(
                        impl_->runtimeFuncs[creprFn], {exprVal}, "ctos");
                } else {
                    strVal = exprVal;
                    lastPartBorrowedStr = partBorrows(exprVal);
                }
            } else if (exprVal->getType() == impl_->i1Type) {
                llvm::Value* ext = impl_->builder->CreateZExt(exprVal, impl_->i64Type);
                strVal = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_bool_to_str"], {ext}, "btos");
            } else if (exprVal->getType() == impl_->f64Type) {
                strVal = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_float_to_str"], {exprVal}, "ftos");
            } else if (exprVal->getType() == impl_->boxType) {
                strVal = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_box_to_str"], {exprVal}, "btos.any");
            } else {
                strVal = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_int_to_str"], {exprVal}, "itos");
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

static std::string precedingAttrName(const std::string& val, size_t bangPos) {
    auto isNameChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_';
    };
    auto isWs = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    size_t k = bangPos;
    if (k > 0 && (val[k-1] == '"' || val[k-1] == '\'')) k--;
    while (k > 0 && isWs(val[k-1])) k--;
    if (k == 0 || val[k-1] != '=') return "";
    k--;
    while (k > 0 && isWs(val[k-1])) k--;
    size_t nameEnd = k;
    while (k > 0 && isNameChar(val[k-1])) k--;
    return val.substr(k, nameEnd - k);
}

static bool isEventAttrContext(const std::string& val, size_t bangPos) {
    std::string attr = precedingAttrName(val, bangPos);
    return attr.size() > 2 && (attr[0] == 'o' || attr[0] == 'O') &&
           (attr[1] == 'n' || attr[1] == 'N');
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

    auto emitStringify = [&](llvm::Value* v, const std::string& cls,
                             bool wantOwned) -> llvm::Value* {
        llvm::Value* s;
        bool owned = true;
        if (!cls.empty() && impl_->hasDunder(cls, "__str__") && v->getType()->isPointerTy()) {
            s = impl_->callDunder(cls, "__str__", v);
        } else if (!cls.empty() && impl_->hasDunder(cls, "__repr__") && v->getType()->isPointerTy()) {
            s = impl_->callDunder(cls, "__repr__", v);
        } else if (v->getType()->isPointerTy()) {
            s = v; owned = false;
        } else if (v->getType() == impl_->i1Type) {
            s = impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_bool_to_str"],
                {impl_->builder->CreateZExt(v, impl_->i64Type)}, "btos");
        } else if (v->getType() == impl_->f64Type) {
            s = impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_float_to_str"], {v}, "ftos");
        } else {
            s = impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_int_to_str"], {v}, "itos");
        }
        if (wantOwned && !owned && impl_->options.gcMode == GCMode::RC) {
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_incref_str"], {s});
        }
        return s;
    };

    auto isSignalReceiver = [&](Expr* recv) -> bool {
        std::string cls = impl_->resolveExprClassName(recv);
        if (cls.empty()) {
            if (auto* n = dynamic_cast<NameExpr*>(recv)) {
                auto it = impl_->varClassNames.find(n->name);
                if (it != impl_->varClassNames.end()) cls = it->second;
            }
        }
        if (cls.empty()) return false;
        std::string base = cls;
        size_t br = base.find('[');
        if (br != std::string::npos) base = base.substr(0, br);
        if (base != "Signal") return false;
        return impl_->hasDunder(cls, "__call__") || impl_->hasDunder(cls, "get");
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
                            emitStringify(exprVal, fClassName, false);

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
                            llvm::Value* rstr = emitStringify(rval, rcls, true);
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

                llvm::Value* strVal;
                bool strValOwned = true;
                if (!fClassName.empty() && impl_->hasDunder(fClassName, "__str__") &&
                    (exprVal->getType() == impl_->i8PtrType || exprVal->getType()->isPointerTy())) {
                    strVal = impl_->callDunder(fClassName, "__str__", exprVal);
                } else if (!fClassName.empty() && impl_->hasDunder(fClassName, "__repr__") &&
                           (exprVal->getType() == impl_->i8PtrType || exprVal->getType()->isPointerTy())) {
                    strVal = impl_->callDunder(fClassName, "__repr__", exprVal);
                } else if (exprVal->getType() == impl_->i8PtrType || exprVal->getType()->isPointerTy()) {
                    strVal = exprVal;
                    strValOwned = impl_->isOwnedStrResult(exprVal);
                } else if (exprVal->getType() == impl_->i1Type) {
                    llvm::Value* ext = impl_->builder->CreateZExt(exprVal, impl_->i64Type);
                    strVal = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_bool_to_str"], {ext}, "btos");
                } else if (exprVal->getType() == impl_->f64Type) {
                    strVal = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_float_to_str"], {exprVal}, "ftos");
                } else {
                    strVal = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_int_to_str"], {exprVal}, "itos");
                }

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
                    } else if (filterName == "html") {
                        applyFilter("dragon_template_escape_html", "esc_html");
                    } else if (filterName == "sql") {
                        applyFilter("dragon_template_escape_sql", "esc_sql");
                    } else if (filterName == "url") {
                        applyFilter("dragon_template_escape_url", "esc_url");
                    } else if (filterName == "join" ||
                               filterName.rfind("join(", 0) == 0) {
                        std::string sepText;
                        if (filterName.size() > 5 && filterName[4] == '(') {
                            auto closeParen = filterName.rfind(')');
                            if (closeParen != std::string::npos && closeParen > 5) {
                                sepText = filterName.substr(5, closeParen - 5);
                            }
                        }
                        llvm::Value* sepVal;
                        if (sepText.find_first_not_of(" \t\n\r") == std::string::npos) {
                            sepVal = impl_->builder->CreateGlobalString("");
                        } else {
                            LexerOptions sLexOpts;
                            sLexOpts.filename = "<template-join-sep>";
                            Lexer sLexer(sepText, sLexOpts);
                            auto sTokens = sLexer.tokenize();
                            ParserOptions sOpts;
                            sOpts.isDragonFile = true;
                            Parser sParser(std::move(sTokens), sOpts);
                            auto sExpr = sParser.parseExpression();
                            if (sExpr && !sParser.hasErrors()) {
                                sExpr->accept(*this);
                                sepVal = impl_->lastValue;
                            } else {
                                impl_->addError(
                                    "Template `| join(...)` separator must be a "
                                    "valid Dragon expression",
                                    node.location());
                                sepVal = impl_->builder->CreateGlobalString("");
                            }
                        }
                        llvm::Value* joined = impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_str_join_ptr"],
                            {sepVal, strVal}, "tpl_join");
                        strVal = joined;
                        strValOwned = true;
                    } else {
                        auto* filterFunc = impl_->module->getFunction(filterName);
                        if (filterFunc) {
                            llvm::Value* prev = strVal;
                            strVal = impl_->builder->CreateCall(
                                filterFunc, {strVal}, "filter_" + filterName);
                            if (strValOwned && impl_->options.gcMode == GCMode::RC) {
                                impl_->builder->CreateCall(
                                    impl_->runtimeFuncs["dragon_decref_str"], {prev});
                            }
                            strValOwned = true;
                        } else {
                            impl_->addError("Unknown template filter: " + filterName,
                                            node.location());
                        }
                    }
                } else if (!effContent.empty()) {
                    bool sameType = (!fClassName.empty() &&
                                     fClassName == effContent);
                    if (!sameType) {
                        std::string ownMod = impl_->resolveClassOwningModule(effContent);
                        auto* escFunc = impl_->resolveMethodFunction(
                            ownMod, effContent, "escape");
                        if (escFunc) {
                            llvm::Value* prev = strVal;
                            strVal = impl_->builder->CreateCall(
                                escFunc, {strVal}, "auto_esc");
                            if (strValOwned && impl_->options.gcMode == GCMode::RC) {
                                impl_->builder->CreateCall(
                                    impl_->runtimeFuncs["dragon_decref_str"], {prev});
                            }
                            strValOwned = true;
                        }
                    }
                }

                parts.push_back(strVal);
            } else {
                parts.push_back(impl_->emitStringLiteralBytes("!{" + exprText + "}"));
            }
        }
    }

    if (parts.empty()) {
        impl_->lastValue = impl_->builder->CreateGlobalString("");
    } else {
        llvm::Value* result = parts[0];
        for (size_t k = 1; k < parts.size(); k++) {
            llvm::Value* prev = result;
            result = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_str_concat"], {prev, parts[k]}, "tpl");
            if (k > 1 && impl_->options.gcMode == GCMode::RC) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref_str"], {prev});
            }
            if (impl_->options.gcMode == GCMode::RC &&
                llvm::isa<llvm::CallInst>(parts[k])) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref_str"], {parts[k]});
            }
        }
        if (parts.size() > 1 && impl_->options.gcMode == GCMode::RC &&
            llvm::isa<llvm::CallInst>(parts[0])) {
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_decref_str"], {parts[0]});
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
    }

    if (!impl_->templateContextStack.empty()) {
        impl_->templateContextStack.pop_back();
    }
}

void CodeGen::emitSqlTemplate(TemplateExpr& node, const std::string& contentType) {
    const std::string& val = node.body;

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

            LexerOptions fLexOpts; fLexOpts.filename = "<sql-template>";
            Lexer fLexer(exprText, fLexOpts);
            auto fTokens = fLexer.tokenize();
            ParserOptions fOpts; fOpts.isDragonFile = true;
            Parser fParser(std::move(fTokens), fOpts);
            auto fExpr = fParser.parseExpression();
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
    std::string resolvedPath = node.filePath;

    if (!resolvedPath.empty() && resolvedPath[0] != '/') {
        std::string sourceFile = node.location().filename;
        if (!sourceFile.empty()) {
            size_t lastSlash = sourceFile.find_last_of('/');
            if (lastSlash != std::string::npos) {
                resolvedPath = sourceFile.substr(0, lastSlash + 1) + resolvedPath;
            }
        }
    }

    std::ifstream file(resolvedPath);
    if (!file.is_open()) {
        impl_->addError("Cannot open template file: " + resolvedPath, node.location());
        impl_->lastValue = impl_->builder->CreateGlobalString("");
        return;
    }
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    file.close();

    TemplateExpr tmp;
    tmp.setLocation(node.location());
    tmp.body = std::move(content);
    tmp.contentType = node.contentType;
    std::vector<std::string> bodyErrors;
    tmp.templateParts = Parser::parseTemplateBody(
        tmp.body, tmp.location(), true, &bodyErrors);
    for (const auto& e : bodyErrors)
        impl_->addError("template file '" + node.filePath + "': " + e,
                        node.location());
    visit(tmp);
}

void CodeGen::visit(BooleanLiteral& node) {
    impl_->lastValue = llvm::ConstantInt::get(impl_->i1Type, node.value ? 1 : 0);
}

void CodeGen::visit(NoneLiteral&) {
    impl_->lastValue = llvm::ConstantPointerNull::get(
        llvm::PointerType::getUnqual(*impl_->context));
}

}
