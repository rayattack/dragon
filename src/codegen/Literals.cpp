/// Dragon CodeGen - Literal Expressions
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
        // Segments arrive in node.fstringParts pre-parsed and type-checked; lower each.
        std::vector<llvm::Value*> parts;
        // True when the (only) part is a bare borrowed str: consumers own f-string
        // results, so a lone borrowed part needs the retain at the bottom.
        bool lastPartBorrowedStr = false;
        for (auto& part : node.fstringParts) {
            lastPartBorrowedStr = false;
            if (part.kind == FStringPart::Kind::Literal) {
                // The Parser stores segments raw: process escapes (honours rf"..."),
                // then emitStringLiteralBytes so non-ASCII becomes a heap DragonString, not Latin-1.
                std::string processed = impl_->processEscapes(part.literal, node.isRaw);
                parts.push_back(impl_->emitStringLiteralBytes(processed));
                continue;
            }

            if (!part.expr) {
                // Parser couldn't parse the interpolation; preserve raw rendering.
                parts.push_back(impl_->emitStringLiteralBytes("{}"));
                continue;
            }

            std::string fClassName = impl_->resolveExprClassName(part.expr.get());
            part.expr->accept(*this);
            llvm::Value* exprVal = impl_->lastValue;

            // An as-is str part is BORROWED only when its expr/value actually borrow:
            // an owned call result carries the +1 already; the retain would double-count it.
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
                // Str format spec `[[fill]align][width][s]`: pad via the ljust/rjust/center
                // helpers; a numeric spec (e.g. `.2f`) on a str is an error, not a no-op.
                const std::string& s = formatSpec;
                size_t p = 0;
                char fill = ' ';
                char align = '<';  // Python default alignment for strings
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
                    p++; typeOk = true;  // explicit `s` type is valid for str
                }
                if (!typeOk || align == '=') {
                    // `=` alignment is numeric-only; any leftover spec is a non-str conversion.
                    impl_->addError("invalid format spec '" + s +
                                    "' for str value", node.location());
                    strVal = exprVal;
                    lastPartBorrowedStr = partBorrows(exprVal);
                } else if (!sawWidth) {
                    // alignment with no width is a no-op pad - emit the str as-is.
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
                // A container pointer is not a string: render via its repr (owned
                // result, balanced by the concat-intermediate decref below).
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
                // D039: Any/Union interpolation dispatches on tag; dragon_box_to_str
                // returns an owned string balanced by the concat decref rule below.
                strVal = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_box_to_str"], {exprVal}, "btos.any");
            } else {
                strVal = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_int_to_str"], {exprVal}, "itos");
            }
            parts.push_back(strVal);
        }

        // Chain parts with dragon_str_concat, decrefing intermediates
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
                // Decref the previous concat intermediate (k>1: prev is a concat
                // result, not parts[0], which may be a GlobalString literal).
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
            // Also decref parts[0] when it was an OWNED conversion result.
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
    // ASCII: raw C-string global. Non-ASCII: lazily-interned heap DragonString,
    // shared via emitStringLiteralBytes so segments don't double-encode.
    impl_->lastValue = impl_->emitStringLiteralBytes(processed);
}

// D031: returns the attribute name when the `!{...}` at val[bangPos] sits in an
// HTML attribute value (else ""); gates event handlers and in-attribute binding suppression.
static std::string precedingAttrName(const std::string& val, size_t bangPos) {
    auto isNameChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_';
    };
    auto isWs = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    size_t k = bangPos;
    if (k > 0 && (val[k-1] == '"' || val[k-1] == '\'')) k--;     // optional opening quote
    while (k > 0 && isWs(val[k-1])) k--;
    if (k == 0 || val[k-1] != '=') return "";                    // attribute assignment
    k--;
    while (k > 0 && isWs(val[k-1])) k--;
    size_t nameEnd = k;
    while (k > 0 && isNameChar(val[k-1])) k--;
    return val.substr(k, nameEnd - k);
}

// True when the interpolation sits in an event-attribute value (`onclick=`, `oninput=`...).
static bool isEventAttrContext(const std::string& val, size_t bangPos) {
    std::string attr = precedingAttrName(val, bangPos);
    return attr.size() > 2 && (attr[0] == 'o' || attr[0] == 'O') &&
           (attr[1] == 'n' || attr[1] == 'N');
}

void CodeGen::visit(TemplateExpr& node) {
    // D032: a content type declaring `build` (SQL) lowers via parameter
    // extraction, never escape-and-concat; `:{}` content aliases keep the string path.
    if (!node.contentType.empty() && !node.isContentAlias) {
        std::string ownMod = impl_->resolveClassOwningModule(node.contentType);
        if (impl_->resolveMethodFunction(ownMod, node.contentType, "build")) {
            emitSqlTemplate(node, node.contentType);
            return;
        }
    }

    // D017: a `:{}` content alias inherits the enclosing template[X]'s type; the
    // push makes recursively visited templates see it at the stack top.
    std::string effContent = node.contentType;
    if (effContent.empty() && node.isContentAlias &&
        !impl_->templateContextStack.empty()) {
        effContent = impl_->templateContextStack.back();
    }
    impl_->templateContextStack.push_back(effContent);

    // D031 reactive text-binding helpers: a text interpolation reading a module-global
    // `ui.Signal` lowers to a `<span data-dr="N">` wrap plus a `ui.bind_text` render closure.

    // Stringify a visited value to str, mirroring the non-reactive path. When
    // `wantOwned`, a borrowed str is increfed so the render fn may return it.
    auto emitStringify = [&](llvm::Value* v, const std::string& cls,
                             bool wantOwned) -> llvm::Value* {
        llvm::Value* s;
        bool owned = true;
        if (!cls.empty() && impl_->hasDunder(cls, "__str__") && v->getType()->isPointerTy()) {
            s = impl_->callDunder(cls, "__str__", v);
        } else if (!cls.empty() && impl_->hasDunder(cls, "__repr__") && v->getType()->isPointerTy()) {
            s = impl_->callDunder(cls, "__repr__", v);
        } else if (v->getType()->isPointerTy()) {
            s = v; owned = false;                       // borrowed str
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

    // Is `recv` a `ui.Signal` instance? (base class name "Signal" carrying a
    // `__call__`/`get` reader). Gates reactive detection.
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

    // Records whether the expression reads a Signal and whether it touches a local:
    // the capture-free render fn reads globals only, so a local is a clean error, not a miscompile.
    std::function<void(Expr*, bool&, bool&)> analyzeReactive =
        [&](Expr* e, bool& signalRead, bool& localRef) {
        if (!e) return;
        if (auto* n = dynamic_cast<NameExpr*>(e)) {
            if (impl_->lookupVar(n->name)) localRef = true;   // an in-scope local alloca
            return;
        }
        if (auto* call = dynamic_cast<CallExpr*>(e)) {
            // `s()` - callee is the Signal itself.
            if (call->args.empty()) {
                if (isSignalReceiver(call->callee.get())) signalRead = true;
                // `s.get()` / `s.__call__()` - callee is an attribute of a Signal.
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

    const std::string& val = node.body;  // kept for event-attr / reactive context scans
    std::vector<llvm::Value*> parts;
    // Segments arrive pre-parsed in templateParts and type-checked, so each
    // `!{expr}` flows at its native type.
    for (auto& tp : node.templateParts) {
        if (tp.kind == TemplatePart::Kind::Literal) {
            // emitStringLiteralBytes: template UTF-8 in a raw C-string would
            // misdecode once a kind=4 operand joins the concat chain.
            if (!tp.literal.empty())
                parts.push_back(impl_->emitStringLiteralBytes(tp.literal));
        } else {
            const size_t bangPos = tp.bangPos;  // event-attr (onclick=!{h}) detection
            const std::string& exprText = tp.exprText;

            // D017: `!{*expr}` spread desugars to `| join` (empty sep); any
            // explicit filter other than `raw` is rejected below.
            std::string filterName = tp.filterName;
            if (tp.isSpread) {
                if (filterName.empty()) {
                    filterName = "join";  // implicit empty sep
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
                // Block interpolation renders into a pushed list[str] buffer (a
                // `:{...}` ExprStmt appends to the top buffer, see visit(ExprStmt)), then joins it.
                llvm::Value* buf = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_list_new_ptr"],
                    {llvm::ConstantInt::get(impl_->i64Type, 0),
                     llvm::ConstantInt::get(impl_->i64Type, 1)},  // TAG_STR
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
                // dragon_str_join_ptr BORROWS the buffer and `joined` is a fresh +1:
                // release the buffer (decrefs every fragment) or each render leaks it all.
                impl_->emitDecrefByKind(buf, Impl::VarKind::List);
                parts.push_back(joined);
                continue;
            }

            if (fExpr) {
                std::string fClassName = impl_->resolveExprClassName(fExpr);

                fExpr->accept(*this);
                llvm::Value* exprVal = impl_->lastValue;

                // D031 event handler: register the callable, emit `window.dr.invoke(<id>)`.
                // Gated on an unambiguous callable so `onclick="!{a_js_string}"` still interpolates.
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
                    // D046: wrap a bare fn as DragonClosure(fn, null) so the registry
                    // holds a uniform refcounted callable.
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

                // D031 reactive text binding: `<span data-dr="N">` wrap plus a
                // `ui.bind_text` render closure; suppressed inside attribute values (span is invalid there).
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

                        // Static value baked into the span: the binding's first patch
                        // predates DOM load, so this shows until the first Signal.set().
                        llvm::Value* staticStr =
                            emitStringify(exprVal, fClassName, /*wantOwned=*/false);

                        // Synthesize a capture-free `() -> str` render fn: the Signal
                        // is a module global read by name (the local-ref guard above enforces this).
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
                            llvm::Value* rstr = emitStringify(rval, rcls, /*wantOwned=*/true);
                            impl_->emitScopeCleanup();
                            impl_->builder->CreateRet(rstr);

                            impl_->popScope();
                            impl_->scopes = std::move(savedScopes);
                            impl_->cellPromotedLocals = std::move(savedCellPromoted);
                            impl_->currentFunction = prevFunc;
                            if (prevBlock) impl_->builder->SetInsertPoint(prevBlock);
                        }

                        // D046: wrap the render fn as a refcounted closure and
                        // register it; bind_text returns the fresh node id.
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

                        // Emit `<span data-dr="<nid>"><static-value></span>`.
                        parts.push_back(impl_->emitStringLiteralBytes("<span data-dr=\""));
                        parts.push_back(impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_int_to_str"], {nid}, "rx.nid.str"));
                        parts.push_back(impl_->emitStringLiteralBytes("\">"));
                        parts.push_back(staticStr);
                        parts.push_back(impl_->emitStringLiteralBytes("</span>"));
                        continue;
                    }
                }

                // strValOwned: fresh ("owned") vs borrowed. Filters return a fresh
                // string, so an owned pre-filter strVal is decref'd when replaced.
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
                    // Classify borrowed (Name/field) vs owned temp (str(n), a + b) honestly.
                    // Pre-fix: hardcoded false made auto-escape skip the decref, one leaked str per render.
                    strValOwned = impl_->isOwnedStrResult(exprVal);
                } else if (exprVal->getType() == impl_->i1Type) {
                    llvm::Value* ext = impl_->builder->CreateZExt(exprVal, impl_->i64Type);
                    strVal = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_bool_to_str"], {ext}, "btos");
                } else if (exprVal->getType() == impl_->f64Type) {
                    strVal = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_float_to_str"], {exprVal}, "ftos");
                } else {
                    // Default: int
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

                // Apply pipe filter or typed auto-escape
                if (!filterName.empty()) {
                    if (filterName == "raw") {
                        // Explicit opt-out - no escaping
                    } else if (filterName == "html") {
                        applyFilter("dragon_template_escape_html", "esc_html");
                    } else if (filterName == "sql") {
                        applyFilter("dragon_template_escape_sql", "esc_sql");
                    } else if (filterName == "url") {
                        applyFilter("dragon_template_escape_url", "esc_url");
                    } else if (filterName == "join" ||
                               filterName.rfind("join(", 0) == 0) {
                        // D017 list[str] join: strVal is the list pointer, never
                        // stringified. `join` -> empty sep; `join(sep)` lexes/visits sep as a Dragon expr.
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
                        // User-defined filter function: look up by name
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
                    // Auto-escape via the effective content type's escape(), owning-module
                    // mangled and parent-walked (D017); the same-type skip avoids double-escape.
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
                // Parse failed; emit raw text as fallback
                parts.push_back(impl_->emitStringLiteralBytes("!{" + exprText + "}"));
            }
        }
    }

    // Chain parts with dragon_str_concat, decrefing intermediates
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
        // Only an EXPLICIT `template[X]` wraps in its content-type instance; a
        // `:{}` fragment stays a raw str so the outer block buffer keeps its element type.
        if (!node.contentType.empty() && !node.isContentAlias) {
            std::string symPrefix = impl_->classSymPrefix(node.contentType);

            // Deliberately no parent walk for validate: Template's default is a
            // no-op, so only a leaf-defined override earns the call.
            std::string validateFn = symPrefix + "_validate";
            auto* valFunc = impl_->module->getFunction(validateFn);
            if (valFunc) {
                impl_->builder->CreateCall(valFunc, {result});
            }

            // Direct ctor lookup: Dragon doesn't auto-inherit constructors. A
            // missing ctor leaves the raw str (the TypeChecker already flagged it).
            std::string newFn = symPrefix + "_new";
            auto* ctorFunc = impl_->module->getFunction(newFn);
            if (ctorFunc) {
                llvm::Value* innerStr = result;
                result = impl_->builder->CreateCall(ctorFunc, {innerStr}, "tpl_inst");
                // The ctor RETAINS the inner string into its field, so drop our owned
                // concat temp or each render leaks one str; isOwnedStrResult screens out literals.
                if (impl_->options.gcMode == GCMode::RC &&
                    impl_->isOwnedStrResult(innerStr)) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_decref_str"], {innerStr});
                }
            }
        }
        impl_->lastValue = result;
    }

    // Pop the effective content type pushed at the top of this visit.
    if (!impl_->templateContextStack.empty()) {
        impl_->templateContextStack.pop_back();
    }
}

// D032 parameter extraction: literal text constant-folds to an interned canonical
// `$$N` string + FNV-1a hash; each !{expr} becomes a native-typed bound parameter, never stringified.
void CodeGen::emitSqlTemplate(TemplateExpr& node, const std::string& contentType) {
    const std::string& val = node.body;

    // params list[Any] - appends happen as we scan !{expr} slots.
    llvm::Value* params = impl_->builder->CreateCall(
        impl_->runtimeFuncs["dragon_list_box_new"],
        {llvm::ConstantInt::get(impl_->i64Type, 0)}, "sql.params");

    std::string canonical;   // dialect-free $$N text, assembled at compile time
    int paramIndex = 0;

    size_t i = 0;
    while (i < val.size()) {
        if (val[i] == '!' && i + 1 < val.size() && val[i+1] == '!' &&
            i + 2 < val.size() && val[i+2] == '{') {
            canonical += "!{"; i += 3;                       // escaped !!{
        } else if (val[i] == '!' && i + 1 < val.size() && val[i+1] == '!' &&
                   i + 2 < val.size() && val[i+2] == '}') {
            canonical += "}"; i += 3;                        // escaped !!}
        } else if (val[i] == '!' && i + 1 < val.size() && val[i+1] == '{') {
            // !{expr} - a bound parameter slot.
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

            // Parse the slot body as a single expression.
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

            // Nested content-type composition (SQL inside SQL) is not implemented yet.
            if (impl_->resolveExprClassName(fExpr.get()) == contentType) {
                impl_->addError("template[" + contentType + "]: composing a nested "
                                + contentType + " value (!{sql_expr}) is not "
                                "implemented yet", node.location());
                continue;
            }

            fExpr->accept(*this);
            llvm::Value* exprVal = impl_->lastValue;

            // Native value -> {tag, payload-i64}, mirroring list[Any] append.
            llvm::Type* t = exprVal->getType();
            int64_t tag;
            if (dynamic_cast<NoneLiteral*>(fExpr.get())) tag = 4;   // TAG_NONE
            else if (t == impl_->f64Type) tag = 2;                 // TAG_FLOAT
            else if (t == impl_->i1Type) tag = 3;                  // TAG_BOOL
            else if (t->isPointerTy()) tag = 1;                    // TAG_STR (default ptr)
            else tag = 0;                                          // TAG_INT

            // Strings need a heap DragonString so the list-owned ref lands
            // somewhere; borrowed heap sources get an incref (Model-B append).
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
            // Literal run: stop only at `!{`, `!!{`, `!!}` - a bare `!!` (Postgres)
            // is literal text, and breaking on it without consuming spins forever.
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

    // Constant-fold the invariant half: interned canonical literal + FNV-1a hash.
    llvm::Value* canonVal = impl_->internSqlCanonical(canonical);
    llvm::Value* hashVal = llvm::ConstantInt::get(
        impl_->i64Type, (int64_t)impl_->sqlCanonicalHash(canonical));

    // Construct the value: <contentType>_new(canonical, hash, params).
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

    // The ctor's `self.params = params` store increfs into the field, so drop
    // our owned temp ref, leaving exactly the field's reference.
    if (impl_->options.gcMode == GCMode::RC)
        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {params});

    impl_->lastValue = sqlVal;
}

void CodeGen::visit(TemplateFileExpr& node) {
    // Compile-time file template; the path resolves relative to the compiling source file.
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

    // Delegate via a temporary TemplateExpr, parsing the body here. The parts are
    // NOT type-checked (the file is read after the TypeChecker runs), so they lower untyped.
    TemplateExpr tmp;
    tmp.setLocation(node.location());
    tmp.body = std::move(content);
    tmp.contentType = node.contentType;
    std::vector<std::string> bodyErrors;
    tmp.templateParts = Parser::parseTemplateBody(
        tmp.body, tmp.location(), /*isDragonFile=*/true, &bodyErrors);
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

} // namespace dragon
