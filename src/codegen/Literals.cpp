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
        // F-string segments are pre-parsed by the Parser into node.fstringParts so
        // Sema/TypeChecker have already walked the interpolations (closure capture
        // analysis, type inference). Here we just lower each segment.
        std::vector<llvm::Value*> parts;
        // True when the (only) part was lowered as a bare borrowed str value
        // (identity, no conversion call). A multi-part f-string concats - the
        // result is a fresh owned string either way - but a SINGLE borrowed
        // part would flow out as the f-string's result, and consumers treat
        // f-string results as owned (isBorrowedHeapExpr): they'd release a
        // reference we never took. See the retain at the bottom.
        bool lastPartBorrowedStr = false;
        for (auto& part : node.fstringParts) {
            lastPartBorrowedStr = false;
            if (part.kind == FStringPart::Kind::Literal) {
                // Process backslash escapes (\n, \t, \\, ...) exactly as a
                // normal string literal does - the Parser stores f-string
                // literal segments raw. Honour the r-prefix (rf"...") via
                // node.isRaw. Then route through emitStringLiteralBytes so a
                // segment containing non-ASCII (e.g. f"x - {y}") becomes a
                // kind=1/4 heap DragonString rather than a raw C-string that
                // dragon_str_concat would mis-decode as Latin-1.
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

            // A str part emitted as-is (no conversion call) is only BORROWED
            // when its expr/value actually borrow. An owned call result
            // (`f"{cfg.get('host', '?')}"` - the getter increfs per #19)
            // already carries the +1 the f-string result hands out; marking
            // it borrowed made the single-part retain below double-count it,
            // leaking the dict's stored value once per evaluation.
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
                // A str value with a format spec - Python's mini-language for
                // strings is `[[fill]align][width][type]`, where the only valid
                // type char is `s`. Honour column padding (<, >, ^, = + width)
                // by routing through the exported ljust/rjust/center helpers;
                // a numeric-only spec (e.g. `.2f`, `d`) on a str is an error,
                // not a silent no-op. Parse here so we can reuse the str-pad
                // runtime without a new dragon_str_format symbol.
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
                    // `=` alignment is numeric-only; any leftover (e.g. `.2f`)
                    // is a non-string conversion applied to a str.
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
                // A container (list/dict/set/tuple) is a pointer but not a
                // string - render it via its repr instead of feeding the
                // container header to dragon_str_concat (which produced empty
                // output). The result is an owned heap string, decref'd by the
                // concat-intermediate rule below.
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
                // D039: Any / Union interpolation - dispatch on tag at runtime.
                // Without this the box was being shoved through dragon_int_to_str
                // (signature mismatch / LLVM verify failure). dragon_box_to_str
                // returns an owned heap DragonString that the existing concat
                // chain's decref-on-CallInst rule will balance.
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
            // Single borrowed str part (f"{e}"): the part IS the f-string's
            // result, but consumers own f-string results by convention - hand
            // them their own +1 (identity, no copy). Without this, storing
            // f"{e}" stole the source's reference and scope cleanup
            // over-released it (UAF on the exception slot after `out = f"{e}"`).
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_str_retain"], {parts[0]}, "fstr.retain");
        } else {
            llvm::Value* result = parts[0];
            for (size_t k = 1; k < parts.size(); k++) {
                llvm::Value* prev = result;
                result = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_str_concat"], {prev, parts[k]}, "fstr");
                // (note: parts[k] may be a `load` of the utf8 literal global, which
                // is `isa<CallInst>` false - so the existing decref-of-CallInst
                // heuristic below will correctly leave the immortal interned
                // string alone.)
                // Decref the previous concat intermediate (k>1 means prev is a concat
                // result, not parts[0] which could be a GlobalString literal)
                if (k > 1 && impl_->options.gcMode == GCMode::RC) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_decref_str"], {prev});
                }
                // Decref only OWNED converstion results consumed by this concat
                // (dragon_int_to_str etc.). The old `isa<CallInst>` heuristic was
                // wrong: a borrowed interpolation like {d['k']} lowers to
                // dragon_dict_get_str_ptr - ALSO a CallInst, but the dict keeps
                // that +1 - so decref'ing it here freed the dict's stored value
                // (f"x {d['k']} y" -> UAF, then d['k'] reads freed memory).
                // isOwnedStrResult knows the borrowed str returners and excludes
                // them.
                if (impl_->options.gcMode == GCMode::RC &&
                    impl_->isOwnedStrResult(parts[k])) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_decref_str"], {parts[k]});
                }
            }
            // Also decref parts[0] if it was an OWNED conversion result (not a
            // literal and not a borrowed dict/field/foreign string).
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
    // shared via emitStringLiteralBytes so template / f-string segments don't
    // double-encode the same byte sequence. See helper docs for details.
    impl_->lastValue = impl_->emitStringLiteralBytes(processed);
}

// Decision 031: if the `!{...}` at `val[bangPos]` sits in the value of an HTML
// attribute - `onclick="!{...}"`, `value=!{...}` - return that attribute's name
// (else ""). Scans the literal text backward over an optional opening quote, the
// `=`, and the attribute name. Used both to detect event handlers (#3) and to
// suppress reactive text-binding inside attribute values (#4 - a `<span>` wrap
// is invalid there).
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

// #3: the interpolation sits in an event-attribute value (`onclick=`, `oninput=`...).
static bool isEventAttrContext(const std::string& val, size_t bangPos) {
    std::string attr = precedingAttrName(val, bangPos);
    return attr.size() > 2 && (attr[0] == 'o' || attr[0] == 'O') &&
           (attr[1] == 'n' || attr[1] == 'N');
}

void CodeGen::visit(TemplateExpr& node) {
    // Parse template body for !{expr} segments and lower to dragon_str_concat chains.
    // Mirrors the f-string implementation but scans for !{ instead of {.
    //
    // D017 Phase 4.B effective-content-type rule:
    //  - explicit `template[X] { ... }` -> effContent = "X"
    //  - `:{ ... }` content alias inside a !{} block -> inherit from
    //  templateContextStack top (the enclosing template[X])
    //  - explicit `template { ... }` (untyped) or top-level :{} with no
    //  enclosing context -> effContent = "" (untyped str)
    //
    // We push the effective content type on the context stack so that
    // ANY template visited recursively (via !{} block-mode parseBlock that
    // visits a `:{...}` ExprStmt) sees the inherited type at its top.
    // D032: content types that declare `build` (SQL) use parameter-extraction
    // lowering - the literal text becomes a canonical $$N string and each
    // !{expr} becomes a native-typed bound parameter, never escape-and-concat.
    // Only the explicit `template[X] { ... }` site routes here; `:{}` content
    // aliases keep the string path below.
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

    // #4 (Decision 031) reactive text-binding helpers. A text-content
    // interpolation that reads a module-global `ui.Signal` - `<h1>!{count()}</h1>`
    // - is lowered to a `<span data-dr="N">` wrapping the value, plus a
    // synthesized `() -> str` render closure registered via `ui.bind_text` so the
    // node auto-patches on every `Signal.set()`. The lambdas below are used by the
    // reactive branch inside the scan loop.

    // Stringify a visited value to a Dragon `str`, mirroring the non-reactive
    // path (dunder __str__/__repr__, then native int/float/bool/str). When
    // `wantOwned`, a borrowed str (a Signal's own `_value`) is increfed so the
    // result is safe to return out of the render function.
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

    // Walk an interpolation expression, recording whether it reads a Signal
    // (`s()` / `s.get()`) and whether it references any *local* name. Phase 0
    // supports reactive interpolations over module-global Signals only: the
    // synthesized render function reads globals by name (no capture), but a
    // local would need codegen-synthesized capture analysis (the unimplemented
    // `fire{}`/`thread{}` capture path) - so a local reference is a clean error,
    // never a silent miscompile.
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
    // Lower each pre-parsed segment. The Parser split node.body into
    // templateParts once and the TypeChecker walked every interpolation, so
    // each `!{expr}` now flows AT its native type - a `!{p[0]}` tuple-subscript
    // materializes the value, not the raw i64 pointer it used to print.
    for (auto& tp : node.templateParts) {
        if (tp.kind == TemplatePart::Kind::Literal) {
            // Literal text must go through emitStringLiteralBytes because
            // template bodies often carry UTF-8 (accents, box-drawing) a raw
            // C-string would misdecode once a kind=4 operand joins the chain.
            if (!tp.literal.empty())
                parts.push_back(impl_->emitStringLiteralBytes(tp.literal));
        } else {
            const size_t bangPos = tp.bangPos;  // #3: event-attr (onclick=!{h}) detection
            const std::string& exprText = tp.exprText;

            // D017 Phase 4.B `!{*expr}` spread sugar desugars to `| join`
            // (empty sep). The Parser recorded the raw filter + spread flag;
            // apply the join defaulting and the combined-filter rejection here.
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
                // Block-interp lowering: allocate a runtime list[str] buffer,
                // push it on the buffer stack, visit each statement (which
                // emits its IR; any `:{ ... }` ExprStmt inside appends its
                // rendered string to the top buffer - see visit(ExprStmt)),
                // pop the buffer, and emit dragon_str_join_ptr to flatten.
                // The joined string is this `!{...}`'s value.
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
                // dragon_str_join_ptr BORROWS the buffer (reads + copies bytes,
                // frees nothing) and `joined` is a fresh +1. The buffer owns one
                // ref per appended fragment (dragon_list_append_ptr transfers),
                // so it must be released here or the list + buffer + every
                // rendered fragment leaks per render. list_destroy
                // decrefs each element, leaving `joined` independent.
                impl_->emitDecrefByKind(buf, Impl::VarKind::List);
                parts.push_back(joined);
                continue;
            }

            if (fExpr) {
                // Check for class instance __str__ before visiting
                std::string fClassName = impl_->resolveExprClassName(fExpr);

                // Visit the expression to generate LLVM IR
                fExpr->accept(*this);
                llvm::Value* exprVal = impl_->lastValue;

                // #3 (Decision 031): an interpolated event handler -
                // `onclick="!{lambda...}"` or `onclick="!{some_fn}"`. Register the
                // callable (returns an int id) and emit `window.dr.invoke(<id>)`
                // into the markup instead of stringifying it. Gated on an
                // unambiguous callable (a lambda literal or a bare function value),
                // so `onclick="!{a_js_string}"` still interpolates normally.
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
                    // Wrap a bare fn into a DragonClosure(fn, null) so the registry
                    // always holds a uniform refcounted callable (ADR 046).
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

                // #4 (Decision 031): a reactive text-content interpolation -
                // `<h1>!{count()}</h1>` reading a module-global Signal. Wrap the
                // value in `<span data-dr="N">` and register a synthesized render
                // closure via `ui.bind_text` so the node auto-patches on every
                // `Signal.set()`. Suppressed inside attribute values (a span wrap
                // is invalid there - handled by the normal stringify below).
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

                        // (1) Static initial value baked into the span. The DOM is
                        //  not loaded when the binding first runs, so its first
                        //  patch is a no-op - this baked value is what shows
                        //  until the first Signal.set().
                        llvm::Value* staticStr =
                            emitStringify(exprVal, fClassName, /*wantOwned=*/false);

                        // (2) Synthesize a capture-free `() -> str` render fn that
                        //  re-evaluates the interpolation. The Signal is a module
                        //  global (read by name in any function), so no closure
                        //  capture is needed - sidestepping synthesized-capture
                        //  analysis, which doesn't exist (the local-ref guard
                        //  above enforces this Phase-0 boundary).
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

                        // (3) Wrap the bare render fn as a uniform refcounted
                        //  closure (ADR 046) and register it; bind_text returns
                        //  the fresh node id.
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

                        // (4) Emit `<span data-dr="<nid>"><static-value></span>`.
                        parts.push_back(impl_->emitStringLiteralBytes("<span data-dr=\""));
                        parts.push_back(impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_int_to_str"], {nid}, "rx.nid.str"));
                        parts.push_back(impl_->emitStringLiteralBytes("\">"));
                        parts.push_back(staticStr);
                        parts.push_back(impl_->emitStringLiteralBytes("</span>"));
                        continue;
                    }
                }

                // Convert to string based on type.
                // Track whether strVal is freshly allocated ("owned" - caller decrefs)
                // or borrowed (already-tracked pointer). Filters return a fresh string,
                // so the pre-filter owned strVal must be decref'd before being replaced.
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
                    // An interpolated str can be a BORROWED value (a Name / field
                    // -> isOwnedStrResult false) or an OWNED temp (str(n), a + b,
                    // f() -> a fresh +1). The pre-fix code hardcoded `false`, so a
                    // typed template's auto-escape (applyFilter) skipped decref'ing
                    // the owned temp -> one leaked str per render (
                    // e.g. `!{str(n)}`). Classify honestly so applyFilter releases
                    // an owned source and leaves a borrowed one alone.
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
                        // D017 Phase 4.C - list[str] join. The expression's
                        // value (strVal) is the list pointer itself; we do
                        // NOT stringify it. dragon_str_join_ptr walks the
                        // typed DragonListPtr without per-element decode.
                        //
                        // `join` (no arg) -> empty separator
                        // `join(sep_expr)` -> sep_expr is any Dragon expression
                        // evaluating to a str, lexed and visited inline.
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
                    // Typed template auto-escape: apply X.escape() where X is
                    // the EFFECTIVE content type - either the explicit
                    // template[X] type or the type inherited by a `:{}`
                    // content fragment from its enclosing template[X].
                    // - Symbol is mangled via the content type's owning module
                    //  so stdlib imports like `from template import HTML`
                    //  resolve to `template__HTML_escape`, not `HTML_escape`.
                    // - Parent walk: a subclass that doesn't override escape
                    //  inherits its ancestor's via resolveMethodFunction.
                    //  Required by D017 Phase 4 §"Compiler Resolution" so a
                    //  user `class MyHTML(HTML)` automatically gets HTML's
                    //  escape applied.
                    // - Same-type skip avoids double-escape on `X` interpolated
                    //  inside `template[X]`.
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
        // Typed template: validate and wrap result in content type instance.
        // Both symbol prefixes are resolved via the content type's owning
        // module so stdlib-imported types (mangled `template__HTML_*`) and
        // entry-module types (bare `Foo_*`) work uniformly.
        //
        // Critically, we only wrap on EXPLICIT `template[X] { ... }` - a
        // `:{ ... }` content fragment returns the raw str so its caller (an
        // outer block buffer) can keep accumulating string parts. Inheriting
        // the escape function is correct; inheriting the instance wrap is
        // not (it would mismatch the buffer's element type).
        if (!node.contentType.empty() && !node.isContentAlias) {
            std::string symPrefix = impl_->classSymPrefix(node.contentType);

            // Call ContentType.validate(result) if the leaf class defines
            // its own validate. We deliberately don't parent-walk here:
            // Template's default validate is a no-op, so emitting a call to
            // it on every typed template would be wasted instructions. Users
            // who want validation override it on their content type.
            std::string validateFn = symPrefix + "_validate";
            auto* valFunc = impl_->module->getFunction(validateFn);
            if (valFunc) {
                impl_->builder->CreateCall(valFunc, {result});
            }

            // Wrap string in content type instance via ContentType_new(result).
            // Dragon doesn't auto-inherit constructors, so direct lookup is
            // the right semantics - every Template subclass redeclares its
            // `def(inner: str)`. If the constructor is missing, leave the
            // result as a raw str (TypeChecker would have flagged it).
            std::string newFn = symPrefix + "_new";
            auto* ctorFunc = impl_->module->getFunction(newFn);
            if (ctorFunc) {
                llvm::Value* innerStr = result;
                result = impl_->builder->CreateCall(ctorFunc, {innerStr}, "tpl_inst");
                // The content-type ctor RETAINS (increfs) the inner string into
                // its field (the borrowed-param store convention), so the owned
                // concat temp we built above is still the caller's +1. Drop it or
                // a typed template leaks one string per render.
                // A literal-only template's `result` is a GlobalString (not an
                // owned CallInst) - isOwnedStrResult screens it out.
                if (impl_->options.gcMode == GCMode::RC &&
                    impl_->isOwnedStrResult(innerStr)) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_decref_str"], {innerStr});
                }
            }
        }
        impl_->lastValue = result;
    }

    // D017 Phase 4.B - pop the effective content type we pushed at function
    // entry. Pairs with the push at the top of this visit.
    if (!impl_->templateContextStack.empty()) {
        impl_->templateContextStack.pop_back();
    }
}

// D032 - parameter-extraction lowering for content types declaring `build`.
// The literal text is constant-folded into an interned canonical `$$N` string
// (+ a compile-time FNV-1a hash constant); each !{expr} becomes a native-typed
// bound parameter appended to a list[Any] (DragonListBox, 16B/elem inline, no
// stringify). The value is `<contentType>_new(canonical, hash, params)`. This
// is the SQL fast path; nested-SQL composition is a follow-up (errors for now).
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

            // Composition: a nested content-type value (SQL inside SQL) is the
            // runtime-splice path - not yet implemented.
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
            // Literal run - copied verbatim into the canonical query text.
            size_t lstart = i;
            while (i < val.size()) {
                if (val[i] == '!' && i + 1 < val.size() &&
                    (val[i+1] == '{' || val[i+1] == '!')) break;
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

    // Release our owned temp ref on `params`: the constructor's `self.params =
    // params` store increfs it into the field (params is a borrowed NameExpr
    // there), so the listbox is left with exactly the field's reference.
    if (impl_->options.gcMode == GCMode::RC)
        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {params});

    impl_->lastValue = sqlVal;
}

void CodeGen::visit(TemplateFileExpr& node) {
    // Compile-time file template: read the file, then process as inline template.
    // Path is resolved relative to the source file being compiled.
    std::string resolvedPath = node.filePath;

    // If relative, resolve against source file directory
    if (!resolvedPath.empty() && resolvedPath[0] != '/') {
        std::string sourceFile = node.location().filename;
        if (!sourceFile.empty()) {
            size_t lastSlash = sourceFile.find_last_of('/');
            if (lastSlash != std::string::npos) {
                resolvedPath = sourceFile.substr(0, lastSlash + 1) + resolvedPath;
            }
        }
    }

    // Read the file at compile time
    std::ifstream file(resolvedPath);
    if (!file.is_open()) {
        impl_->addError("Cannot open template file: " + resolvedPath, node.location());
        impl_->lastValue = impl_->builder->CreateGlobalString("");
        return;
    }
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    file.close();

    // Delegate to TemplateExpr logic by creating a temporary node. The Parser
    // never saw this file's contents, so parse the body into parts here. These
    // parts are NOT type-checked (the file is read at codegen time, after the
    // TypeChecker has run), so a file-template interpolation whose lowering
    // needs a static type stays untyped - the same pre-existing limitation as
    // before; inline `template[X] { ... }` interpolations are fully typed.
    TemplateExpr tmp;
    tmp.setLocation(node.location());
    tmp.body = std::move(content);
    tmp.contentType = node.contentType;
    tmp.templateParts = Parser::parseTemplateBody(
        tmp.body, tmp.location(), /*isDragonFile=*/true);
    visit(tmp);
}

void CodeGen::visit(BooleanLiteral& node) {
    impl_->lastValue = llvm::ConstantInt::get(impl_->i1Type, node.value ? 1 : 0);
}

void CodeGen::visit(NoneLiteral&) {
    impl_->lastValue = llvm::ConstantPointerNull::get(
        llvm::PointerType::getUnqual(*impl_->context));
}

//===----------------------------------------------------------------------===//
// Visitor: Expressions
//===----------------------------------------------------------------------===//

} // namespace dragon
