/// Dragon CodeGen - Core Expressions (Name, Binary, Unary, If, Walrus, ChainedComp)
#include "../CodeGenImpl.h"
#include "llvm/IR/Intrinsics.h"

namespace dragon {

void CodeGen::visit(NameExpr& node) {
    // Module-typed names (e.g. `controllers` after `import controllers.health`)
    // have no runtime representation. They only legitimately appear as the
    // base of an AttributeExpr, which short-circuits before reaching here.
    // If we get here it means TypeChecker missed a misuse - surface it as
    // an internal error rather than silently falling through to the function-
    // lookup branch below (which would happily return any function that
    // happens to share the module's name).
    if (node.type && node.type->kind() == Type::Kind::Module) {
        impl_->addError("module '" + node.name + "' used as a runtime value",
                        node.location());
        impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
        return;
    }

    // Special names
    if (node.name == "True") {
        impl_->lastValue = llvm::ConstantInt::get(impl_->i1Type, 1);
        return;
    }
    if (node.name == "False") {
        impl_->lastValue = llvm::ConstantInt::get(impl_->i1Type, 0);
        return;
    }
    if (node.name == "None") {
        impl_->lastValue = llvm::ConstantPointerNull::get(
            llvm::PointerType::getUnqual(*impl_->context));
        return;
    }

    // Bare `__doc__` resolves to the current module's docstring - Python
    // parity (the compiler synthesizes module __doc__ as a name binding).
    // Re-uses the same `.rodata` constant cache that `<mod>.__doc__` uses
    // in Attributes.cpp so repeated reads share storage.
    if (node.name == "__doc__") {
        const std::string& modName = impl_->currentModuleName;
        auto cIt = impl_->moduleDocConstants.find(modName);
        if (cIt != impl_->moduleDocConstants.end()) {
            impl_->lastValue = cIt->second;
            return;
        }
        auto dIt = impl_->moduleDocstrings.find(modName);
        if (dIt == impl_->moduleDocstrings.end()) {
            impl_->lastValue = llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(impl_->i8PtrType));
            return;
        }
        auto* docStr = impl_->builder->CreateGlobalString(
            dIt->second, "module_doc_" + (modName.empty() ? "main" : modName));
        auto* casted = llvm::ConstantExpr::getBitCast(
            llvm::cast<llvm::Constant>(docStr), impl_->i8PtrType);
        impl_->moduleDocConstants[modName] = casted;
        impl_->lastValue = casted;
        return;
    }

    // Decision 025: Class name in value context -> load descriptor global
    // Only when NOT resolving a call target (call dispatch handles class names directly).
    // Exception classes are excluded: as a value they lower to their integer type
    // code (next block), not their descriptor, so exception-type matching (e.g.
    // assertRaises(SSLError, fn)) works for user-defined exceptions the same way it
    // already does for built-ins. Without this guard a user exception class (which
    // is in classNames) would shadow the code path and yield a descriptor pointer,
    // which __exc_matches can never range-match against an exception code.
    if (!impl_->resolvingCallTarget && impl_->classNames.count(node.name) &&
        !impl_->isExcType(node.name)) {
        auto descIt = impl_->classDescriptorGlobals.find(node.name);
        if (descIt != impl_->classDescriptorGlobals.end()) {
            impl_->lastValue = impl_->builder->CreateLoad(
                impl_->i64Type, descIt->second, node.name + "_desc");
            return;
        }
    }

    // First-class exception classes: a bare exception name in value context
    // (e.g. `assertRaises(ValueError, fn)`) lowers to its integer type code.
    // Speed-king: the integer-code + [lo,hi] range model keeps exception-type
    // matching a single range compare via dragon_exc_matches - no descriptor
    // walk, no RTTI. `raise X(...)` / `except X` don't reach here (handled by
    // statement codegen), so this only fires when the name is used as a value.
    if (!impl_->resolvingCallTarget && impl_->isExcType(node.name) &&
        !impl_->lookupVar(node.name)) {
        impl_->lastValue = llvm::ConstantInt::get(
            impl_->i64Type, impl_->excTypeCode(node.name));
        impl_->lastValueIsType = true;
        return;
    }

    auto* alloca = impl_->lookupVar(node.name);
    if (!alloca) {
        // Check module-level globals (.dr: always; .py: only if `global` declared)
        auto* gv = impl_->lookupModuleGlobal(node.name);
        if (gv && impl_->shouldUseModuleGlobal(node.name)) {
            impl_->lastValue = impl_->builder->CreateLoad(
                gv->getValueType(), gv, node.name);
            return;
        }
        // May be a function name (for function pointers/references).
        // Resolution order mirrors CallExpr's same-module path:
        //  1. importedFuncAliases (`from mod import fn` brought it in)
        //  2. mangleFunc(currentModule, name) - same-module Dragon def
        //  3. userFuncName(name) - extern-C or entry-module def
        llvm::Function* func = nullptr;
        std::string aliasSym = impl_->lookupImportedAlias(node.name);
        if (!aliasSym.empty()) {
            func = impl_->module->getFunction(aliasSym);
        }
        if (!func) {
            func = impl_->module->getFunction(
                Impl::mangleFunc(impl_->currentModuleName, node.name));
        }
        if (!func) {
            func = impl_->module->getFunction(Impl::userFuncName(node.name));
        }
        if (func) {
            impl_->lastValue = func;
            return;
        }
        impl_->addError("Undefined variable: " + node.name, node.location());
        impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
        return;
    }
    // D027.1: cell-backed names (nonlocal-mutable) - alloca holds the
    // cell pointer; route through dragon_cell_get so reads chain to
    // the same backing slot the writer mutates.
    if (impl_->isCellBacked(node.name)) {
        impl_->lastValue = impl_->emitCellRead(
            alloca, impl_->lookupVarKind(node.name), node.name);
        return;
    }
    impl_->lastValue = impl_->builder->CreateLoad(
        alloca->getAllocatedType(), alloca, node.name);
}

void CodeGen::visit(BinaryExpr& node) {
    // Desugar `not in` / `is not` into NOT(IN) / NOT(IS). We re-enter visit
    // with the op temporarily rewritten so the same dispatch covers all of
    // __contains__ dunder, set/string membership, and pointer-identity paths
    // for free; then we coerce the result to i1 and invert. RAII restores
    // the op so the AST is unchanged on return.
    if (node.op.type() == TokenType::NOT_IN || node.op.type() == TokenType::IS_NOT) {
        const Token saved = node.op;
        const TokenType inner = (saved.type() == TokenType::NOT_IN)
            ? TokenType::IN : TokenType::IS;
        struct Restore {
            BinaryExpr& n; Token o;
            ~Restore() { n.op = o; }
        } restore{node, saved};
        node.op = Token(inner, saved.lexeme(), saved.location());
        visit(node);
        llvm::Value* v = impl_->lastValue;
        if (v->getType() == impl_->i64Type)
            v = impl_->builder->CreateICmpNE(
                v, llvm::ConstantInt::get(impl_->i64Type, 0), "tobool");
        impl_->lastValue = impl_->builder->CreateNot(v, "negcmp");
        return;
    }

    // Short-circuit for 'and' and 'or'
    if (node.op.type() == TokenType::AND || node.op.type() == TokenType::OR) {
        node.left->accept(*this);
        llvm::Value* lhs = impl_->lastValue;

        // Convert to i1 if needed
        if (lhs->getType() == impl_->i64Type) {
            lhs = impl_->builder->CreateICmpNE(
                lhs, llvm::ConstantInt::get(impl_->i64Type, 0), "tobool");
        } else if (lhs->getType() == impl_->f64Type) {
            lhs = impl_->builder->CreateFCmpONE(
                lhs, llvm::ConstantFP::get(impl_->f64Type, 0.0), "tobool");
        }

        auto* func = impl_->currentFunction;
        auto* rhsBlock = llvm::BasicBlock::Create(*impl_->context, "rhs", func);
        auto* mergeBlock = llvm::BasicBlock::Create(*impl_->context, "merge", func);

        if (node.op.type() == TokenType::AND) {
            impl_->builder->CreateCondBr(lhs, rhsBlock, mergeBlock);
        } else {
            impl_->builder->CreateCondBr(lhs, mergeBlock, rhsBlock);
        }

        auto* lhsBlock = impl_->builder->GetInsertBlock();

        impl_->builder->SetInsertPoint(rhsBlock);
        node.right->accept(*this);
        llvm::Value* rhs = impl_->lastValue;
        if (rhs->getType() == impl_->i64Type) {
            rhs = impl_->builder->CreateICmpNE(
                rhs, llvm::ConstantInt::get(impl_->i64Type, 0), "tobool");
        } else if (rhs->getType() == impl_->f64Type) {
            rhs = impl_->builder->CreateFCmpONE(
                rhs, llvm::ConstantFP::get(impl_->f64Type, 0.0), "tobool");
        }
        impl_->builder->CreateBr(mergeBlock);
        rhsBlock = impl_->builder->GetInsertBlock();

        impl_->builder->SetInsertPoint(mergeBlock);
        auto* phi = impl_->builder->CreatePHI(impl_->i1Type, 2);
        phi->addIncoming(lhs, lhsBlock);
        phi->addIncoming(rhs, rhsBlock);
        impl_->lastValue = phi;
        return;
    }

    // Class-based IntEnum / StrEnum comparison: members ARE their value for
    // comparison purposes (Python parity: `Color.RED == 1`, `Color.RED ==
    // Color.GREEN`, ordering). Rewrite an IntEnum/StrEnum operand `e` to
    // `e.value` so the normal int/str comparison codegen handles every case
    // (member-vs-scalar AND member-vs-member) with no boxing. Plain Enum is
    // left as-is -> pointer identity (the default __eq__ path below).
    {
        auto opk = node.op.type();
        bool isCmp = opk == TokenType::EQUAL_EQUAL || opk == TokenType::NOT_EQUAL ||
                     opk == TokenType::LESS || opk == TokenType::LESS_EQUAL ||
                     opk == TokenType::GREATER || opk == TokenType::GREATER_EQUAL;
        if (isCmp) {
            auto isValueEnum = [&](Expr* e) -> bool {
                if (!e || !e->type) return false;
                auto* inst = dynamic_cast<InstanceType*>(e->type.get());
                if (!inst || !inst->classType) return false;
                auto it = impl_->enumKind.find(inst->classType->name);
                return it != impl_->enumKind.end() && it->second != Impl::EnumKind::Plain;
            };
            auto wrapValue = [&](std::unique_ptr<Expr>& operand) {
                if (!isValueEnum(operand.get())) return;
                auto attr = std::make_unique<AttributeExpr>();
                SourceLocation l = operand->location();
                attr->setLocation(l);
                attr->attribute = "value";
                attr->object = std::move(operand);
                operand = std::move(attr);
            };
            wrapValue(node.left);
            wrapValue(node.right);
        }
    }

    // Resolve class name from LHS AST before visiting (for dunder dispatch)
    std::string lhsClassName = impl_->resolveExprClassName(node.left.get());

    node.left->accept(*this);
    llvm::Value* lhs = impl_->lastValue;
    node.right->accept(*this);
    llvm::Value* rhs = impl_->lastValue;

    auto op = node.op.type();

    // Union (`T | None`) compared to `none` - extract the tag and compare to
    // TAG_NONE (4). Required because the box ({i64,i64}) and the i8* null
    // produced by NoneLiteral are different LLVM types.
    if (op == TokenType::EQUAL_EQUAL || op == TokenType::NOT_EQUAL) {
        bool lhsIsNone = dynamic_cast<NoneLiteral*>(node.left.get()) != nullptr;
        bool rhsIsNone = dynamic_cast<NoneLiteral*>(node.right.get()) != nullptr;
        bool lhsIsBox = lhs->getType() == impl_->boxType;
        bool rhsIsBox = rhs->getType() == impl_->boxType;
        if ((lhsIsBox && rhsIsNone) || (rhsIsBox && lhsIsNone)) {
            llvm::Value* box = lhsIsBox ? lhs : rhs;
            auto* tag = impl_->boxTag(box);
            auto* tagNone = llvm::ConstantInt::get(impl_->i64Type, 4); // TAG_NONE
            impl_->lastValue = (op == TokenType::EQUAL_EQUAL)
                ? impl_->builder->CreateICmpEQ(tag, tagNone, "is.none")
                : impl_->builder->CreateICmpNE(tag, tagNone, "not.none");
            return;
        }
        // D039 Phase 10: box == box / box != box - runtime tag-then-payload
        // compare via dragon_box_eq. LLVM's ICmp doesn't accept struct types,
        // so we MUST route through the helper here (otherwise AssertOK trips).
        if (lhsIsBox && rhsIsBox) {
            auto* eqI64 = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_box_eq"], {lhs, rhs}, "box.eq");
            auto* eqBool = impl_->builder->CreateICmpNE(
                eqI64, llvm::ConstantInt::get(impl_->i64Type, 0), "box.eq.bool");
            impl_->lastValue = (op == TokenType::EQUAL_EQUAL)
                ? eqBool
                : impl_->builder->CreateNot(eqBool, "box.ne");
            return;
        }
        // Gap #9: box vs native-typed (`x == ""` where x is Any/Union, "" is
        // a ptr-shaped str literal). LLVM ICmp rejects the type mismatch
        // between {i64,i64} and the native ptr/i64/f64/i1. Box the native
        // side using its AST-derived tag, then dispatch through dragon_box_eq
        // - same tag-then-payload semantics as box==box. Symmetric across
        // operand order. Skips when the native side is `None` (already handled
        // above) or a class instance dispatching via dunder (handled below).
        if (lhsIsBox != rhsIsBox) {
            Expr* nativeExpr = lhsIsBox ? node.right.get() : node.left.get();
            llvm::Value* boxVal  = lhsIsBox ? lhs : rhs;
            llvm::Value* nativeVal = lhsIsBox ? rhs : lhs;
            // Skip dunder-dispatching class instances; they're handled by the
            // dunder block below where the box side would be rejected anyway.
            std::string nativeClassName =
                impl_->resolveExprClassName(nativeExpr);
            bool isDunderClass = !nativeClassName.empty() &&
                                 impl_->hasDunder(nativeClassName, "__eq__");
            if (!isDunderClass) {
                llvm::Value* nativeTag = impl_->emitTagForExpr(nativeExpr, *this);
                llvm::Value* nativeBox = impl_->makeBox(nativeTag, nativeVal);
                auto* eqI64 = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_box_eq"], {boxVal, nativeBox},
                    "box.eq.mixed");
                auto* eqBool = impl_->builder->CreateICmpNE(
                    eqI64, llvm::ConstantInt::get(impl_->i64Type, 0),
                    "box.eq.mixed.bool");
                impl_->lastValue = (op == TokenType::EQUAL_EQUAL)
                    ? eqBool
                    : impl_->builder->CreateNot(eqBool, "box.ne.mixed");
                return;
            }
        }
    }

    // Dunder dispatch for class instances (comparison + arithmetic operators)
    if (!lhsClassName.empty() && (lhs->getType() == impl_->i8PtrType || lhs->getType()->isPointerTy())) {
        // Map operator to dunder method name
        std::string dunder;
        switch (op) {
            // Comparison dunders
            case TokenType::EQUAL_EQUAL: dunder = "__eq__"; break;
            case TokenType::NOT_EQUAL:   dunder = "__ne__"; break;
            case TokenType::LESS:        dunder = "__lt__"; break;
            case TokenType::LESS_EQUAL:  dunder = "__le__"; break;
            case TokenType::GREATER:     dunder = "__gt__"; break;
            case TokenType::GREATER_EQUAL: dunder = "__ge__"; break;
            // Arithmetic dunders
            case TokenType::PLUS:         dunder = "__add__"; break;
            case TokenType::MINUS:        dunder = "__sub__"; break;
            case TokenType::STAR:         dunder = "__mul__"; break;
            case TokenType::SLASH:        dunder = "__truediv__"; break;
            case TokenType::DOUBLE_SLASH: dunder = "__floordiv__"; break;
            case TokenType::PERCENT:      dunder = "__mod__"; break;
            case TokenType::POWER:        dunder = "__pow__"; break;
            default: break;
        }
        bool isComparison = (op == TokenType::EQUAL_EQUAL || op == TokenType::NOT_EQUAL ||
                             op == TokenType::LESS || op == TokenType::LESS_EQUAL ||
                             op == TokenType::GREATER || op == TokenType::GREATER_EQUAL);
        if (!dunder.empty()) {
            if (impl_->hasDunder(lhsClassName, dunder)) {
                auto* result = impl_->callDunder(lhsClassName, dunder, lhs, {rhs});
                // Comparison dunders: normalize to i1 (bool)
                if (isComparison && result->getType() == impl_->i64Type)
                    result = impl_->builder->CreateICmpNE(result, llvm::ConstantInt::get(impl_->i64Type, 0));
                // Arithmetic dunders: use raw result (i8*/i64/f64)
                impl_->lastValue = result;
                return;
            }
            // Fallback for __ne__: negate __eq__ if available
            if (dunder == "__ne__" && impl_->hasDunder(lhsClassName, "__eq__")) {
                auto* eqResult = impl_->callDunder(lhsClassName, "__eq__", lhs, {rhs});
                if (eqResult->getType() == impl_->i64Type)
                    eqResult = impl_->builder->CreateICmpNE(eqResult, llvm::ConstantInt::get(impl_->i64Type, 0));
                impl_->lastValue = impl_->builder->CreateNot(eqResult, "ne");
                return;
            }
            // Fallback for __gt__: use other.__lt__(self) - only if same class
            if (dunder == "__gt__" && impl_->hasDunder(lhsClassName, "__lt__")) {
                // Swap operands: rhs.__lt__(lhs)
                auto* result = impl_->callDunder(lhsClassName, "__lt__", rhs, {lhs});
                if (result->getType() == impl_->i64Type)
                    result = impl_->builder->CreateICmpNE(result, llvm::ConstantInt::get(impl_->i64Type, 0));
                impl_->lastValue = result;
                return;
            }
            // Fallback for __ge__: not __lt__
            if (dunder == "__ge__" && impl_->hasDunder(lhsClassName, "__lt__")) {
                auto* ltResult = impl_->callDunder(lhsClassName, "__lt__", lhs, {rhs});
                if (ltResult->getType() == impl_->i64Type)
                    ltResult = impl_->builder->CreateICmpNE(ltResult, llvm::ConstantInt::get(impl_->i64Type, 0));
                impl_->lastValue = impl_->builder->CreateNot(ltResult, "ge");
                return;
            }
            // Fallback for __le__: __lt__ or __eq__
            if (dunder == "__le__" && impl_->hasDunder(lhsClassName, "__lt__") && impl_->hasDunder(lhsClassName, "__eq__")) {
                auto* ltResult = impl_->callDunder(lhsClassName, "__lt__", lhs, {rhs});
                if (ltResult->getType() == impl_->i64Type)
                    ltResult = impl_->builder->CreateICmpNE(ltResult, llvm::ConstantInt::get(impl_->i64Type, 0));
                auto* eqResult = impl_->callDunder(lhsClassName, "__eq__", lhs, {rhs});
                if (eqResult->getType() == impl_->i64Type)
                    eqResult = impl_->builder->CreateICmpNE(eqResult, llvm::ConstantInt::get(impl_->i64Type, 0));
                impl_->lastValue = impl_->builder->CreateOr(ltResult, eqResult, "le");
                return;
            }
            // Default for __eq__/__ne__ on class instances: pointer equality
            if (dunder == "__eq__") {
                impl_->lastValue = impl_->builder->CreateICmpEQ(lhs, rhs, "ptreq");
                return;
            }
            if (dunder == "__ne__") {
                impl_->lastValue = impl_->builder->CreateICmpNE(lhs, rhs, "ptrne");
                return;
            }
        }
    }

    // D039 Phase 11: box arithmetic. If either operand is an Any/Union box and
    // the op is arithmetic, the result type depends on the runtime tags, so we
    // can't lower to a native binop - dispatch through dragon_box_binop, which
    // boxes the native side and returns a result box (numeric tower, str/bytes
    // concat+repeat, list repeat; TypeError otherwise). Placed AFTER the class
    // dunder dispatch (so `instance + Any` still calls __add__) but BEFORE the
    // type-specific bytes/str/set paths below - those use OR-of-operand tests
    // (`lhsIsBytes || rhsIsBytes`) and would otherwise intercept a `box + bytes`
    // / `box + str` with a native concat call on a {i64,i64} operand. The
    // == / != box paths above already returned.
    {
        bool eitherBox = (lhs->getType() == impl_->boxType ||
                          rhs->getType() == impl_->boxType);
        if (eitherBox) {
            int64_t opcode = impl_->binopOpcodeForToken(op);
            if (opcode >= 0) {
                impl_->lastValue = impl_->emitBoxBinop(
                    *this, node.left.get(), lhs, node.right.get(), rhs, opcode);
                return;
            }
            // D039 Phase 11b: box ordering (< <= > >=). `==`/`!=` already
            // returned above; ordering had no handler and crashed on an ICmp of
            // a {i64,i64}. Route through dragon_box_cmp (numeric/str/bytes;
            // TypeError otherwise) and compare the three-way result to 0.
            int64_t cmpOp = -1;
            switch (op) {
                case TokenType::LESS:          cmpOp = 0; break;
                case TokenType::LESS_EQUAL:    cmpOp = 1; break;
                case TokenType::GREATER:       cmpOp = 2; break;
                case TokenType::GREATER_EQUAL: cmpOp = 3; break;
                default: break;
            }
            if (cmpOp >= 0) {
                auto* cmp = impl_->emitBoxCmp(
                    *this, node.left.get(), lhs, node.right.get(), rhs, cmpOp);
                auto* zero = llvm::ConstantInt::get(impl_->i64Type, 0);
                switch (op) {
                    case TokenType::LESS:
                        impl_->lastValue = impl_->builder->CreateICmpSLT(cmp, zero); break;
                    case TokenType::LESS_EQUAL:
                        impl_->lastValue = impl_->builder->CreateICmpSLE(cmp, zero); break;
                    case TokenType::GREATER:
                        impl_->lastValue = impl_->builder->CreateICmpSGT(cmp, zero); break;
                    default:
                        impl_->lastValue = impl_->builder->CreateICmpSGE(cmp, zero); break;
                }
                return;
            }
        }
    }

    // [H5] Set ordering operators (`<`, `<=`, `>`, `>=`) are subset/superset
    // tests (Python parity), NOT pointer-address comparisons. A single
    // comparison `a <= b` is a BinaryExpr - the parser only builds a
    // ChainedCompExpr for 2+ chained operators (Parser.cpp:332) - so this is
    // the path the H5 repro actually takes. Without it both set ptrs are
    // PtrToInt'd and fall into the integer ICmpSLE/etc. branch below, comparing
    // heap addresses. `a.issubset(b)` lowers to issubset(a, b) = (a ⊆ b), so:
    //  a <= b -> issubset(a, b); a >= b -> issubset(b, a)
    //  a < b -> (a ⊆ b) ∧ ¬(b ⊆ a); a > b -> (b ⊆ a) ∧ ¬(a ⊆ b)
    {
        auto isSetOperand = [&](Expr* e) -> bool {
            if (!e) return false;
            if (dynamic_cast<SetExpr*>(e) || dynamic_cast<SetCompExpr*>(e)) return true;
            if (auto* ne = dynamic_cast<NameExpr*>(e))
                return impl_->lookupVarKind(ne->name) == Impl::VarKind::Set;
            return impl_->resolveExprVarKind(e) == Impl::VarKind::Set;
        };
        bool isOrdering = op == TokenType::LESS || op == TokenType::LESS_EQUAL ||
                          op == TokenType::GREATER || op == TokenType::GREATER_EQUAL;
        if (isOrdering && lhs->getType()->isPointerTy() && rhs->getType()->isPointerTy() &&
            isSetOperand(node.left.get()) && isSetOperand(node.right.get())) {
            auto* subsetFn = impl_->runtimeFuncs["dragon_set_issubset"];
            auto subset = [&](llvm::Value* x, llvm::Value* y) -> llvm::Value* {
                auto* r = impl_->builder->CreateCall(subsetFn, {x, y}, "setsub");
                return impl_->builder->CreateICmpNE(
                    r, llvm::ConstantInt::get(impl_->i64Type, 0), "setsub.b");
            };
            switch (op) {
                case TokenType::LESS_EQUAL:
                    impl_->lastValue = subset(lhs, rhs);
                    break;
                case TokenType::GREATER_EQUAL:
                    impl_->lastValue = subset(rhs, lhs);
                    break;
                case TokenType::LESS:
                    impl_->lastValue = impl_->builder->CreateAnd(
                        subset(lhs, rhs),
                        impl_->builder->CreateNot(subset(rhs, lhs), "n"), "psub");
                    break;
                default:  // GREATER
                    impl_->lastValue = impl_->builder->CreateAnd(
                        subset(rhs, lhs),
                        impl_->builder->CreateNot(subset(lhs, rhs), "n"), "psup");
                    break;
            }
            return;
        }
    }

    // Bytes operators - must check before string since both are i8*.
    // Use the central exprIsBytes helper so call expressions (`bytes(...)`,
    // user fn returning bytes), attribute access (`self.buf`), nested
    // BinaryExpr, and method calls all flow into the bytes path. Without
    // this, e.g. `bytes(l1) + bytes(l2)` falls through to dragon_str_concat
    // and produces an empty result.
    {
        bool lhsIsBytes = impl_->exprIsBytes(node.left.get());
        bool rhsIsBytes = impl_->exprIsBytes(node.right.get());

        if (lhsIsBytes || rhsIsBytes) {
            if (op == TokenType::PLUS) {
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_bytes_concat"], {lhs, rhs}, "bytescat");
                // Decref owned intermediate bytes operands from chained
                // expressions ((a + b) + c) - the str/list paths already do this;
                // bytes did not, leaking the inner temp per op (leaks.md #10).
                // isOwnedPtrResult screens out borrowed Names/fields. bytes carry
                // a DragonObjectHeader, so dragon_decref dispatches to bytes free.
                if (impl_->options.gcMode == GCMode::RC) {
                    if (impl_->isOwnedPtrResult(lhs))
                        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {lhs});
                    if (impl_->isOwnedPtrResult(rhs))
                        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {rhs});
                }
                return;
            }
            if (op == TokenType::STAR) {
                llvm::Value* bv = lhsIsBytes ? lhs : rhs;
                llvm::Value* iv = lhsIsBytes ? rhs : lhs;
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_bytes_repeat"], {bv, iv}, "bytesrep");
                // Drop an owned bytes operand temp ((a + b) * 3); the count is a
                // scalar, no decref. Mirrors the str-repeat path.
                if (impl_->options.gcMode == GCMode::RC && impl_->isOwnedPtrResult(bv))
                    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {bv});
                return;
            }
            if (op == TokenType::EQUAL_EQUAL) {
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_bytes_eq"], {lhs, rhs}, "byteseq");
                impl_->lastValue = impl_->builder->CreateICmpNE(
                    impl_->lastValue, llvm::ConstantInt::get(impl_->i64Type, 0));
                return;
            }
            if (op == TokenType::NOT_EQUAL) {
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_bytes_eq"], {lhs, rhs}, "byteseq");
                impl_->lastValue = impl_->builder->CreateICmpEQ(
                    impl_->lastValue, llvm::ConstantInt::get(impl_->i64Type, 0));
                return;
            }
            if (op == TokenType::LESS || op == TokenType::GREATER ||
                op == TokenType::LESS_EQUAL || op == TokenType::GREATER_EQUAL) {
                auto* cmp = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_bytes_cmp"], {lhs, rhs}, "bytescmp");
                auto zero = llvm::ConstantInt::get(impl_->i64Type, 0);
                if (op == TokenType::LESS)
                    impl_->lastValue = impl_->builder->CreateICmpSLT(cmp, zero);
                else if (op == TokenType::GREATER)
                    impl_->lastValue = impl_->builder->CreateICmpSGT(cmp, zero);
                else if (op == TokenType::LESS_EQUAL)
                    impl_->lastValue = impl_->builder->CreateICmpSLE(cmp, zero);
                else
                    impl_->lastValue = impl_->builder->CreateICmpSGE(cmp, zero);
                return;
            }
        }
    }

    // List concatenation: list + list -> fresh list. The TypeChecker admits `+`
    // for two list operands; str/bytes/numeric were handled (and returned)
    // above. MUST precede the i8*+i8* string-concat fallthrough below, which
    // would otherwise misread two list pointers as C-strings.
    if (op == TokenType::PLUS) {
        auto isListOperand = [&](Expr* e, llvm::Value* v) -> bool {
            if (e && e->type && e->type->kind() == Type::Kind::List) return true;
            if (dynamic_cast<ListExpr*>(e) || dynamic_cast<ListCompExpr*>(e)) return true;
            if (v->getType()->isPointerTy())
                return impl_->resolveExprVarKind(e) == Impl::VarKind::List;
            return false;
        };
        if (isListOperand(node.left.get(), lhs) && isListOperand(node.right.get(), rhs)) {
            if (lhs->getType() == impl_->i64Type)
                lhs = impl_->builder->CreateIntToPtr(lhs, impl_->i8PtrType);
            if (rhs->getType() == impl_->i64Type)
                rhs = impl_->builder->CreateIntToPtr(rhs, impl_->i8PtrType);
            auto* result = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_concat"], {lhs, rhs}, "listcat");
            // Decref owned temporary operands (e.g. [1,2] + xs). Mirrors the
            // repeat path: only literal/comprehension operands are temporaries;
            // a bare variable is borrowed and must not be decref'd.
            if (impl_->options.gcMode == GCMode::RC) {
                if (dynamic_cast<ListExpr*>(node.left.get()) ||
                    dynamic_cast<ListCompExpr*>(node.left.get()))
                    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {lhs});
                if (dynamic_cast<ListExpr*>(node.right.get()) ||
                    dynamic_cast<ListCompExpr*>(node.right.get()))
                    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {rhs});
            }
            impl_->lastValue = result;
            return;
        }
    }

    // List repetition: list * int or int * list
    if (op == TokenType::STAR) {
        bool lhsIsList = dynamic_cast<ListExpr*>(node.left.get()) ||
                         dynamic_cast<ListCompExpr*>(node.left.get());
        if (!lhsIsList)
            if (auto* ln = dynamic_cast<NameExpr*>(node.left.get()))
                lhsIsList = impl_->lookupVarKind(ln->name) == Impl::VarKind::List;
        bool rhsIsList = dynamic_cast<ListExpr*>(node.right.get()) ||
                         dynamic_cast<ListCompExpr*>(node.right.get());
        if (!rhsIsList)
            if (auto* rn = dynamic_cast<NameExpr*>(node.right.get()))
                rhsIsList = impl_->lookupVarKind(rn->name) == Impl::VarKind::List;

        if (lhsIsList || rhsIsList) {
            llvm::Value* listVal = lhsIsList ? lhs : rhs;
            llvm::Value* countVal = lhsIsList ? rhs : lhs;
            bool sourceIsTemp = lhsIsList
                ? (dynamic_cast<ListExpr*>(node.left.get()) ||
                   dynamic_cast<ListCompExpr*>(node.left.get()))
                : (dynamic_cast<ListExpr*>(node.right.get()) ||
                   dynamic_cast<ListCompExpr*>(node.right.get()));
            if (listVal->getType() == impl_->i64Type)
                listVal = impl_->builder->CreateIntToPtr(listVal, impl_->i8PtrType);
            if (countVal->getType() == impl_->i1Type)
                countVal = impl_->builder->CreateZExt(countVal, impl_->i64Type);
            auto* repeatFn = impl_->runtimeFuncs["dragon_list_repeat"];
            auto* result = impl_->builder->CreateCall(repeatFn, {listVal, countVal}, "listrepeat");
            if (sourceIsTemp && impl_->options.gcMode == GCMode::RC) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref"], {listVal});
            }
            impl_->lastValue = result;
            return;
        }
    }

    // String repetition: str * int or int * str (Python `"ab" * 3`).
    // Bytes and lists were handled (and returned) above, so the i8* operand
    // here is a string. Without this case, str*int fell through to the mixed
    // ptr/i64 arithmetic path below, which did PtrToInt(str) then CreateMul -
    // multiplying the string's ADDRESS by the count and yielding a garbage
    // pointer that segfaults the moment it is used as a str.
    if (op == TokenType::STAR) {
        auto isStrOperand = [&](Expr* e, llvm::Value* v) -> bool {
            if (e && e->type && e->type->kind() == Type::Kind::Str) return true;
            if (auto* lit = dynamic_cast<StringLiteral*>(e)) return !lit->isBytes;
            if (v->getType() == impl_->i8PtrType) {
                auto k = impl_->resolveExprVarKind(e);
                return k == Impl::VarKind::Str || k == Impl::VarKind::StrLiteral;
            }
            return false;
        };
        auto isIntOperand = [&](llvm::Value* v) -> bool {
            return v->getType() == impl_->i64Type || v->getType() == impl_->i1Type;
        };
        bool lhsStr = isStrOperand(node.left.get(), lhs);
        bool rhsStr = isStrOperand(node.right.get(), rhs);
        if ((lhsStr && isIntOperand(rhs)) || (rhsStr && isIntOperand(lhs))) {
            llvm::Value* strVal = lhsStr ? lhs : rhs;
            llvm::Value* countVal = lhsStr ? rhs : lhs;
            Expr* strExpr = lhsStr ? node.left.get() : node.right.get();
            bool strIsTemp = impl_->isOwnedStrResult(strVal);
            if (countVal->getType() == impl_->i1Type)
                countVal = impl_->builder->CreateZExt(countVal, impl_->i64Type);

            // Peephole: literal string * constant int folds to a baked literal,
            // so a banner/padding like `"=" * 80` costs zero runtime alloc +
            // memcpy. Reuses the exact processEscapes + emitStringLiteralBytes
            // path of a plain string literal, so the folded bytes are identical
            // (escapes, non-ASCII interning). A 64 KiB cap leaves pathologically
            // large repeats to the runtime call (whose own guard handles the
            // MemoryError case) rather than bloating the object file.
            if (auto* strLit = dynamic_cast<StringLiteral*>(strExpr)) {
                auto* countConst = llvm::dyn_cast<llvm::ConstantInt>(countVal);
                if (countConst && !strLit->isFString && !strLit->isBytes) {
                    int64_t c = countConst->getSExtValue();
                    std::string unit = impl_->processEscapes(strLit->value, strLit->isRaw);
                    constexpr uint64_t kMaxFoldBytes = 64 * 1024;
                    bool overLimit = c > 0 && !unit.empty() &&
                        (uint64_t)c > kMaxFoldBytes / unit.size();
                    if (!overLimit) {
                        std::string folded;
                        if (c > 0) {
                            folded.reserve(unit.size() * (size_t)c);
                            for (int64_t i = 0; i < c; i++) folded += unit;
                        }
                        impl_->lastValue = impl_->emitStringLiteralBytes(folded);
                        return;
                    }
                }
            }

            if (strVal->getType() != impl_->i8PtrType)
                strVal = impl_->builder->CreateIntToPtr(strVal, impl_->i8PtrType);
            auto* result = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_str_repeat"], {strVal, countVal}, "strrep");
            // Decref an owned intermediate operand (e.g. ("a"+"b") * 3) so the
            // consumed temporary isn't leaked, mirroring str concat.
            if (strIsTemp && impl_->options.gcMode == GCMode::RC) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref_str"], {strVal});
            }
            impl_->lastValue = result;
            return;
        }
    }

    // Deep container equality: `list == list` and `dict == dict` where both
    // operands are direct pointers (not boxed). Routes through
    // dragon_list_eq / dragon_dict_eq so the comparison is element-wise
    // (Python semantics) instead of pointer identity. Must run BEFORE the
    // i8*-i8* string-eq fallthrough below, otherwise list/dict pointers
    // would be misread as C-strings by dragon_str_eq.
    if ((op == TokenType::EQUAL_EQUAL || op == TokenType::NOT_EQUAL) &&
        lhs->getType()->isPointerTy() && rhs->getType()->isPointerTy()) {
        auto isListLike = [&](Expr* e) -> bool {
            if (!e) return false;
            if (dynamic_cast<ListExpr*>(e) || dynamic_cast<ListCompExpr*>(e)) return true;
            if (e->type && e->type->kind() == Type::Kind::List) return true;
            return impl_->resolveExprVarKind(e) == Impl::VarKind::List;
        };
        auto isDictLike = [&](Expr* e) -> bool {
            if (!e) return false;
            if (dynamic_cast<DictExpr*>(e)) return true;
            if (e->type && e->type->kind() == Type::Kind::Dict) return true;
            return impl_->resolveExprVarKind(e) == Impl::VarKind::Dict;
        };
        bool bothList = isListLike(node.left.get()) && isListLike(node.right.get());
        bool bothDict = isDictLike(node.left.get()) && isDictLike(node.right.get());
        if (bothList || bothDict) {
            const char* fnName = "dragon_list_eq";
            if (bothDict) {
                // Pick str-vs-int-keyed variant from the static dict type.
                bool intKeyed = false;
                auto checkIntKeyed = [&](Expr* e) {
                    if (!e || !e->type) return;
                    if (auto* dt = dynamic_cast<DictType*>(e->type.get())) {
                        if (dt->keyType && dt->keyType->kind() == Type::Kind::Int)
                            intKeyed = true;
                    }
                };
                checkIntKeyed(node.left.get());
                checkIntKeyed(node.right.get());
                fnName = intKeyed ? "dragon_dict_int_eq" : "dragon_dict_eq";
            }
            auto* eqI64 = impl_->builder->CreateCall(
                impl_->runtimeFuncs[fnName], {lhs, rhs}, "container.eq");
            auto* eqBool = impl_->builder->CreateICmpNE(
                eqI64, llvm::ConstantInt::get(impl_->i64Type, 0), "container.eq.bool");
            impl_->lastValue = (op == TokenType::EQUAL_EQUAL)
                ? eqBool
                : impl_->builder->CreateNot(eqBool, "container.ne");
            return;
        }
    }

    // Lexicographic list ordering: `list < list` (also <= > >=) where both
    // operands are direct pointers. Python compares element-wise; the default
    // pointer-address comparison further below is silently wrong. Routes through
    // dragon_list_cmp (which recurses via dragon_box_cmp per element and raises
    // TypeError on an incomparable element pair). Sets were already handled by
    // the H5 subset path above; dict ordering stays unsupported (Python raises),
    // so it falls through.
    if ((op == TokenType::LESS || op == TokenType::LESS_EQUAL ||
         op == TokenType::GREATER || op == TokenType::GREATER_EQUAL) &&
        lhs->getType()->isPointerTy() && rhs->getType()->isPointerTy()) {
        auto isListLike = [&](Expr* e) -> bool {
            if (!e) return false;
            if (dynamic_cast<ListExpr*>(e) || dynamic_cast<ListCompExpr*>(e)) return true;
            if (e->type && e->type->kind() == Type::Kind::List) return true;
            return impl_->resolveExprVarKind(e) == Impl::VarKind::List;
        };
        if (isListLike(node.left.get()) && isListLike(node.right.get())) {
            auto* cmp = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_cmp"], {lhs, rhs}, "list.cmp");
            auto* zero = llvm::ConstantInt::get(impl_->i64Type, 0);
            switch (op) {
                case TokenType::LESS:
                    impl_->lastValue = impl_->builder->CreateICmpSLT(cmp, zero); break;
                case TokenType::LESS_EQUAL:
                    impl_->lastValue = impl_->builder->CreateICmpSLE(cmp, zero); break;
                case TokenType::GREATER:
                    impl_->lastValue = impl_->builder->CreateICmpSGT(cmp, zero); break;
                default:
                    impl_->lastValue = impl_->builder->CreateICmpSGE(cmp, zero); break;
            }
            return;
        }
    }

    // String concatenation
    if (lhs->getType() == impl_->i8PtrType && rhs->getType() == impl_->i8PtrType) {
        if (op == TokenType::PLUS) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_str_concat"], {lhs, rhs}, "strcat");
            // Decref owned intermediate string operands from chained
            // expressions (e.g. (a + b) + c) so they aren't leaked once the
            // concat consumes them. See isOwnedStrResult for the ownership
            // convention and borrowed-returner blocklist.
            if (impl_->options.gcMode == GCMode::RC) {
                if (impl_->isOwnedStrResult(lhs)) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_decref_str"], {lhs});
                }
                if (impl_->isOwnedStrResult(rhs)) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_decref_str"], {rhs});
                }
            }
            return;
        }
        if (op == TokenType::EQUAL_EQUAL) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_str_eq"], {lhs, rhs}, "streq");
            impl_->lastValue = impl_->builder->CreateICmpNE(
                impl_->lastValue, llvm::ConstantInt::get(impl_->i64Type, 0));
            return;
        }
        if (op == TokenType::NOT_EQUAL) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_str_eq"], {lhs, rhs}, "streq");
            impl_->lastValue = impl_->builder->CreateICmpEQ(
                impl_->lastValue, llvm::ConstantInt::get(impl_->i64Type, 0));
            return;
        }
        // String ordering: <, >, <=, >= via dragon_str_cmp (strcmp wrapper)
        if (op == TokenType::LESS || op == TokenType::GREATER ||
            op == TokenType::LESS_EQUAL || op == TokenType::GREATER_EQUAL) {
            auto* cmp = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_str_cmp"], {lhs, rhs}, "strcmp");
            auto* zero = llvm::ConstantInt::get(impl_->i64Type, 0);
            if (op == TokenType::LESS)
                impl_->lastValue = impl_->builder->CreateICmpSLT(cmp, zero);
            else if (op == TokenType::GREATER)
                impl_->lastValue = impl_->builder->CreateICmpSGT(cmp, zero);
            else if (op == TokenType::LESS_EQUAL)
                impl_->lastValue = impl_->builder->CreateICmpSLE(cmp, zero);
            else
                impl_->lastValue = impl_->builder->CreateICmpSGE(cmp, zero);
            return;
        }
    }

    // 'in' operator: check membership in a set or string containment
    if (op == TokenType::IN) {
        // __contains__ dunder dispatch for class instances (RHS)
        std::string rhsClassName = impl_->resolveExprClassName(node.right.get());
        if (!rhsClassName.empty() && impl_->hasDunder(rhsClassName, "__contains__") &&
            (rhs->getType() == impl_->i8PtrType || rhs->getType()->isPointerTy())) {
            auto* result = impl_->callDunder(rhsClassName, "__contains__", rhs, {lhs});
            if (result->getType() == impl_->i64Type)
                result = impl_->builder->CreateICmpNE(result, llvm::ConstantInt::get(impl_->i64Type, 0));
            impl_->lastValue = result;
            return;
        }

        // Resolve the RHS kind once so attribute / subscript exprs (e.g.
        // `obj.field`, `obj.field[i]`) get the same treatment as bare locals.
        // Without this, `"k" in r.params` fell through to dragon_str_contains
        // (returning 0 for any dict pointer), silently breaking membership.
        Impl::VarKind rhsKind = impl_->resolveExprVarKind(node.right.get());

        // Check if RHS is a set
        bool isSet = dynamic_cast<SetExpr*>(node.right.get()) != nullptr ||
                     rhsKind == Impl::VarKind::Set;
        if (isSet) {
            // left value (i64), right is set ptr
            llvm::Value* val = lhs;
            if (val->getType() == impl_->i1Type)
                val = impl_->builder->CreateZExt(val, impl_->i64Type);
            else if (val->getType() == impl_->f64Type)
                val = impl_->builder->CreateBitCast(val, impl_->i64Type);
            else if (val->getType()->isPointerTy())
                val = impl_->builder->CreatePtrToInt(val, impl_->i64Type);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_set_contains"], {rhs, val}, "setcontains");
            // Convert i64 result to i1 for boolean context
            impl_->lastValue = impl_->builder->CreateICmpNE(
                impl_->lastValue, llvm::ConstantInt::get(impl_->i64Type, 0), "inbool");
            return;
        }
        // Bytes containment: int_value in bytes or bytes in bytes.
        // D030 §5: bytes-ness is identified by the static type / AST shape;
        // VarKind::Bytes was deleted (slots use the generic-heap VarKind::List).
        {
            bool rhsIsBytes = node.right && node.right->type &&
                              node.right->type->kind() == Type::Kind::Bytes;
            if (!rhsIsBytes)
                if (auto* rl = dynamic_cast<StringLiteral*>(node.right.get()))
                    rhsIsBytes = rl->isBytes;
            (void)rhsKind;
            if (rhsIsBytes) {
                if (lhs->getType()->isPointerTy()) {
                    // bytes in bytes
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_bytes_contains_bytes"], {rhs, lhs}, "bytescontains");
                } else {
                    // int in bytes
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_bytes_contains"], {rhs, lhs}, "bytescontains");
                }
                impl_->lastValue = impl_->builder->CreateICmpNE(
                    impl_->lastValue, llvm::ConstantInt::get(impl_->i64Type, 0), "inbool");
                return;
            }
        }
        // Dict membership: "key" in dict - dispatch on dict_has_key.
        // D030 Phase 3.G: int-keyed dicts route through dragon_dict_int_has_key
        // so the key crosses at i64 instead of being misread as a string ptr.
        {
            bool rhsIsDict = rhsKind == Impl::VarKind::Dict ||
                             dynamic_cast<DictExpr*>(node.right.get()) != nullptr;
            if (!rhsIsDict && node.right && node.right->type &&
                node.right->type->kind() == Type::Kind::Dict) {
                rhsIsDict = true;
            }
            if (rhsIsDict) {
                bool intKeyed = impl_->dictKeyIsInt(node.right.get());
                if (!intKeyed && node.right && node.right->type &&
                    node.right->type->kind() == Type::Kind::Dict) {
                    if (auto* dt = dynamic_cast<DictType*>(node.right->type.get())) {
                        if (dt->keyType && dt->keyType->kind() == Type::Kind::Int)
                            intKeyed = true;
                    }
                }
                if (intKeyed && rhs->getType()->isPointerTy()) {
                    llvm::Value* k = lhs;
                    if (k->getType() == impl_->i1Type)
                        k = impl_->builder->CreateZExt(k, impl_->i64Type);
                    else if (k->getType()->isPointerTy())
                        k = impl_->builder->CreatePtrToInt(k, impl_->i64Type);
                    else if (k->getType() != impl_->i64Type)
                        k = impl_->builder->CreateZExtOrTrunc(k, impl_->i64Type);
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_dict_int_has_key"], {rhs, k}, "dicthas.i");
                    impl_->lastValue = impl_->builder->CreateICmpNE(
                        impl_->lastValue, llvm::ConstantInt::get(impl_->i64Type, 0), "inbool");
                    return;
                }
                if (lhs->getType()->isPointerTy() && rhs->getType()->isPointerTy()) {
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_dict_has_key"], {rhs, lhs}, "dicthas");
                    impl_->lastValue = impl_->builder->CreateICmpNE(
                        impl_->lastValue, llvm::ConstantInt::get(impl_->i64Type, 0), "inbool");
                    return;
                }
            }
        }
        // Deque membership: must precede the list block - a deque is typed as
        // ListType, and dragon_list_contains would walk the DragonDeque header
        // as a list (str elements then compare as misread pointers, always
        // False). The deque runtime compares by its stored element tag over
        // the circular window.
        {
            bool rhsIsDeque = rhsKind == Impl::VarKind::Deque;
            if (!rhsIsDeque) {
                if (auto* rn = dynamic_cast<NameExpr*>(node.right.get())) {
                    auto dqIt = impl_->varClassNames.find(rn->name);
                    rhsIsDeque = dqIt != impl_->varClassNames.end() &&
                                 dqIt->second == "__Deque";
                }
            }
            if (rhsIsDeque && rhs->getType()->isPointerTy()) {
                llvm::Value* val = lhs;
                if (val->getType() == impl_->f64Type)
                    val = impl_->builder->CreateBitCast(val, impl_->i64Type);
                else if (val->getType() == impl_->i1Type)
                    val = impl_->builder->CreateZExt(val, impl_->i64Type);
                else if (val->getType()->isPointerTy())
                    val = impl_->builder->CreatePtrToInt(val, impl_->i64Type);
                else if (val->getType() != impl_->i64Type)
                    val = impl_->builder->CreateZExtOrTrunc(val, impl_->i64Type);
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_deque_contains"], {rhs, val},
                    "dequecontains");
                impl_->lastValue = impl_->builder->CreateICmpNE(
                    impl_->lastValue, llvm::ConstantInt::get(impl_->i64Type, 0),
                    "inbool");
                return;
            }
        }
        // List membership: value in list. Must precede the string-containment
        // fallback - a str-in-str-list has ptr LHS and ptr RHS and would
        // otherwise be misrouted to dragon_str_contains. The runtime compares
        // by the list's element tag (content equality for str/bytes elements),
        // so int/float/str lists all work. Bytes/dict/set RHS already returned
        // above, so a remaining list-kind RHS is a real list.
        {
            bool rhsIsList = rhsKind == Impl::VarKind::List ||
                             dynamic_cast<ListExpr*>(node.right.get()) != nullptr;
            if (!rhsIsList && node.right && node.right->type &&
                node.right->type->kind() == Type::Kind::List) {
                rhsIsList = true;
            }
            if (rhsIsList && rhs->getType()->isPointerTy()) {
                llvm::Value* val = lhs;
                if (val->getType() == impl_->f64Type)
                    val = impl_->builder->CreateBitCast(val, impl_->i64Type);
                else if (val->getType() == impl_->i1Type)
                    val = impl_->builder->CreateZExt(val, impl_->i64Type);
                else if (val->getType()->isPointerTy())
                    val = impl_->builder->CreatePtrToInt(val, impl_->i64Type);
                else if (val->getType() != impl_->i64Type)
                    val = impl_->builder->CreateZExtOrTrunc(val, impl_->i64Type);
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_list_contains"], {rhs, val}, "listcontains");
                impl_->lastValue = impl_->builder->CreateICmpNE(
                    impl_->lastValue, llvm::ConstantInt::get(impl_->i64Type, 0), "inbool");
                return;
            }
        }
        // String containment: "sub" in "string"
        if (lhs->getType()->isPointerTy() && rhs->getType()->isPointerTy()) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_str_contains"], {rhs, lhs}, "strcontains");
            impl_->lastValue = impl_->builder->CreateICmpNE(
                impl_->lastValue, llvm::ConstantInt::get(impl_->i64Type, 0), "inbool");
            return;
        }
        impl_->lastValue = llvm::ConstantInt::get(impl_->i1Type, 0);
        return;
    }

    // 'is' identity comparison: raw pointer / integer equality after coercion.
    // Matches the chained-comparison IS path so single and chained forms agree.
    if (op == TokenType::IS) {
        // D039 Phase 6: `box is None` / `None is box` - compare the box's tag
        // to TAG_NONE (4). Without this, ICmp fires on mismatched types (a
        // {i64, i64} box vs an i8* null) and LLVM trips its AssertOK.
        // Mirrors the EQUAL_EQUAL box-vs-none path above.
        bool lhsIsNone = dynamic_cast<NoneLiteral*>(node.left.get()) != nullptr;
        bool rhsIsNone = dynamic_cast<NoneLiteral*>(node.right.get()) != nullptr;
        bool lhsIsBox = lhs->getType() == impl_->boxType;
        bool rhsIsBox = rhs->getType() == impl_->boxType;
        if ((lhsIsBox && rhsIsNone) || (rhsIsBox && lhsIsNone)) {
            llvm::Value* box = lhsIsBox ? lhs : rhs;
            auto* tag = impl_->boxTag(box);
            auto* tagNone = llvm::ConstantInt::get(impl_->i64Type, 4); // TAG_NONE
            impl_->lastValue = impl_->builder->CreateICmpEQ(tag, tagNone, "is.none");
            return;
        }
        llvm::Value* lv = lhs;
        llvm::Value* rv = rhs;
        if (lv->getType() == impl_->i1Type)
            lv = impl_->builder->CreateZExt(lv, impl_->i64Type);
        if (rv->getType() == impl_->i1Type)
            rv = impl_->builder->CreateZExt(rv, impl_->i64Type);
        if (lv->getType()->isPointerTy())
            lv = impl_->builder->CreatePtrToInt(lv, impl_->i64Type);
        if (rv->getType()->isPointerTy())
            rv = impl_->builder->CreatePtrToInt(rv, impl_->i64Type);
        if (lv->getType() == impl_->f64Type)
            lv = impl_->builder->CreateBitCast(lv, impl_->i64Type);
        if (rv->getType() == impl_->f64Type)
            rv = impl_->builder->CreateBitCast(rv, impl_->i64Type);
        impl_->lastValue = impl_->builder->CreateICmpEQ(lv, rv, "is");
        return;
    }

    // Float promotion
    bool isFloat = (lhs->getType() == impl_->f64Type || rhs->getType() == impl_->f64Type);
    if (isFloat) {
        if (lhs->getType() == impl_->i64Type)
            lhs = impl_->builder->CreateSIToFP(lhs, impl_->f64Type, "itof");
        if (lhs->getType() == impl_->i1Type)
            lhs = impl_->builder->CreateUIToFP(lhs, impl_->f64Type, "btof");
        if (rhs->getType() == impl_->i64Type)
            rhs = impl_->builder->CreateSIToFP(rhs, impl_->f64Type, "itof");
        if (rhs->getType() == impl_->i1Type)
            rhs = impl_->builder->CreateUIToFP(rhs, impl_->f64Type, "btof");
    }

    // Bool to int promotion for arithmetic
    if (!isFloat) {
        if (lhs->getType() == impl_->i1Type)
            lhs = impl_->builder->CreateZExt(lhs, impl_->i64Type, "btoi");
        if (rhs->getType() == impl_->i1Type)
            rhs = impl_->builder->CreateZExt(rhs, impl_->i64Type, "btoi");
    }

    // True division always returns float
    if (op == TokenType::SLASH && !isFloat) {
        lhs = impl_->builder->CreateSIToFP(lhs, impl_->f64Type, "itof");
        rhs = impl_->builder->CreateSIToFP(rhs, impl_->f64Type, "itof");
        isFloat = true;
    }

    if (isFloat) {
        switch (op) {
            case TokenType::PLUS:
                impl_->lastValue = impl_->builder->CreateFAdd(lhs, rhs, "fadd");
                return;
            case TokenType::MINUS:
                impl_->lastValue = impl_->builder->CreateFSub(lhs, rhs, "fsub");
                return;
            case TokenType::STAR:
                impl_->lastValue = impl_->builder->CreateFMul(lhs, rhs, "fmul");
                return;
            case TokenType::SLASH:
                impl_->lastValue = impl_->builder->CreateFDiv(lhs, rhs, "fdiv");
                return;
            case TokenType::POWER:
                // Float exponentiation (e.g. `n ** 0.5`). Without this the op
                // fell through to the integer switch and called dragon_pow_int
                // on f64 operands - an LLVM type-mismatch verify failure.
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_pow_float"], {lhs, rhs}, "fpow");
                return;
            case TokenType::DOUBLE_SLASH:
                // Python float floor-division: floor(a/b). Previously fell through
                // to the integer path below and emitted ICmp on f64 operands -
                // crashing the compiler with an LLVM type-mismatch assertion.
                impl_->lastValue = impl_->emitFloatFloorDiv(lhs, rhs);
                return;
            case TokenType::PERCENT:
                // Python float modulo (sign follows the divisor); same prior crash.
                impl_->lastValue = impl_->emitFloatMod(lhs, rhs);
                return;
            case TokenType::EQUAL_EQUAL:
                impl_->lastValue = impl_->builder->CreateFCmpOEQ(lhs, rhs, "feq");
                return;
            case TokenType::NOT_EQUAL:
                impl_->lastValue = impl_->builder->CreateFCmpONE(lhs, rhs, "fne");
                return;
            case TokenType::LESS:
                impl_->lastValue = impl_->builder->CreateFCmpOLT(lhs, rhs, "flt");
                return;
            case TokenType::LESS_EQUAL:
                impl_->lastValue = impl_->builder->CreateFCmpOLE(lhs, rhs, "fle");
                return;
            case TokenType::GREATER:
                impl_->lastValue = impl_->builder->CreateFCmpOGT(lhs, rhs, "fgt");
                return;
            case TokenType::GREATER_EQUAL:
                impl_->lastValue = impl_->builder->CreateFCmpOGE(lhs, rhs, "fge");
                return;
            default: break;
        }
    }

    // Mixed ptr/i64 coercion: handles cases where one operand is a pointer
    // (e.g., string literal or string field loaded as ptr) and the other is i64
    // (e.g., struct field with missing type annotation defaulting to i64).
    // For equality/inequality on strings, route to dragon_str_eq.
    // For other ops, coerce ptr->i64 via PtrToInt so LLVM IR is well-formed.
    if ((lhs->getType()->isPointerTy() && rhs->getType() == impl_->i64Type) ||
        (lhs->getType() == impl_->i64Type && rhs->getType()->isPointerTy())) {
        if (op == TokenType::EQUAL_EQUAL || op == TokenType::NOT_EQUAL) {
            // One side is ptr (likely a string), the other is i64 (likely a string
            // stored via i64-boxed field). Coerce i64->ptr and call dragon_str_eq.
            if (lhs->getType() == impl_->i64Type)
                lhs = impl_->builder->CreateIntToPtr(lhs, impl_->i8PtrType);
            if (rhs->getType() == impl_->i64Type)
                rhs = impl_->builder->CreateIntToPtr(rhs, impl_->i8PtrType);
            auto* streqResult = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_str_eq"], {lhs, rhs}, "streq");
            if (op == TokenType::EQUAL_EQUAL) {
                impl_->lastValue = impl_->builder->CreateICmpNE(
                    streqResult, llvm::ConstantInt::get(impl_->i64Type, 0));
            } else {
                impl_->lastValue = impl_->builder->CreateICmpEQ(
                    streqResult, llvm::ConstantInt::get(impl_->i64Type, 0));
            }
            return;
        }
        if (op == TokenType::PLUS) {
            // Mixed ptr/i64 with +: likely string concatenation where one side
            // is a string field loaded as i64. Coerce i64->ptr and call str_concat.
            if (lhs->getType() == impl_->i64Type)
                lhs = impl_->builder->CreateIntToPtr(lhs, impl_->i8PtrType);
            if (rhs->getType() == impl_->i64Type)
                rhs = impl_->builder->CreateIntToPtr(rhs, impl_->i8PtrType);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_str_concat"], {lhs, rhs}, "strcat");
            return;
        }
        // For non-equality ops, coerce ptr->i64
        if (lhs->getType()->isPointerTy())
            lhs = impl_->builder->CreatePtrToInt(lhs, impl_->i64Type);
        if (rhs->getType()->isPointerTy())
            rhs = impl_->builder->CreatePtrToInt(rhs, impl_->i64Type);
    }

    // 4.6 --check-overflow: emit `llvm.s{add,sub,mul}.with.overflow.i64`
    // intrinsics and raise OverflowError on overflow. The intrinsic call
    // returns `{i64 result, i1 overflowed}`; we branch on the flag and raise
    // through dragon_raise_exc(22, ...) (OverflowError parent code = 22).
    auto emitCheckedIntOp = [&](llvm::Intrinsic::ID id, const char* name,
                                const char* msg) {
        auto* fn = llvm::Intrinsic::getOrInsertDeclaration(
            impl_->module.get(), id, {impl_->i64Type});
        auto* res = impl_->builder->CreateCall(fn, {lhs, rhs}, name);
        auto* val = impl_->builder->CreateExtractValue(res, {0}, std::string(name) + ".v");
        auto* ovf = impl_->builder->CreateExtractValue(res, {1}, std::string(name) + ".ovf");
        auto* func = impl_->currentFunction;
        auto* okBB = llvm::BasicBlock::Create(*impl_->context, std::string(name) + ".ok", func);
        auto* badBB = llvm::BasicBlock::Create(*impl_->context, std::string(name) + ".ovf", func);
        impl_->builder->CreateCondBr(ovf, badBB, okBB);
        impl_->builder->SetInsertPoint(badBB);
        auto* msgPtr = impl_->builder->CreateGlobalString(msg);
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_raise_exc_cstr"],
            {llvm::ConstantInt::get(impl_->i64Type, /*OverflowError*/22), msgPtr});
        impl_->builder->CreateUnreachable();
        impl_->builder->SetInsertPoint(okBB);
        return val;
    };

    // Integer operations
    switch (op) {
        case TokenType::PLUS:
            impl_->lastValue = impl_->options.checkOverflow
                ? emitCheckedIntOp(llvm::Intrinsic::sadd_with_overflow, "add",
                                   "OverflowError: integer addition overflowed")
                : (llvm::Value*)impl_->builder->CreateAdd(lhs, rhs, "add");
            return;
        case TokenType::MINUS:
            impl_->lastValue = impl_->options.checkOverflow
                ? emitCheckedIntOp(llvm::Intrinsic::ssub_with_overflow, "sub",
                                   "OverflowError: integer subtraction overflowed")
                : (llvm::Value*)impl_->builder->CreateSub(lhs, rhs, "sub");
            return;
        case TokenType::STAR:
            impl_->lastValue = impl_->options.checkOverflow
                ? emitCheckedIntOp(llvm::Intrinsic::smul_with_overflow, "mul",
                                   "OverflowError: integer multiplication overflowed")
                : (llvm::Value*)impl_->builder->CreateMul(lhs, rhs, "mul");
            return;
        case TokenType::PERCENT:
            impl_->lastValue = impl_->emitIntMod(lhs, rhs);
            return;
        case TokenType::DOUBLE_SLASH:
            impl_->lastValue = impl_->emitIntFloorDiv(lhs, rhs);
            return;
        case TokenType::POWER: {
            const char* fnName = impl_->options.checkOverflow
                ? "dragon_pow_int_checked" : "dragon_pow_int";
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs[fnName], {lhs, rhs}, "pow");
            return;
        }
        case TokenType::EQUAL_EQUAL:
            impl_->lastValue = impl_->builder->CreateICmpEQ(lhs, rhs, "eq");
            return;
        case TokenType::NOT_EQUAL:
            impl_->lastValue = impl_->builder->CreateICmpNE(lhs, rhs, "ne");
            return;
        case TokenType::LESS:
            impl_->lastValue = impl_->builder->CreateICmpSLT(lhs, rhs, "lt");
            return;
        case TokenType::LESS_EQUAL:
            impl_->lastValue = impl_->builder->CreateICmpSLE(lhs, rhs, "le");
            return;
        case TokenType::GREATER:
            impl_->lastValue = impl_->builder->CreateICmpSGT(lhs, rhs, "gt");
            return;
        case TokenType::GREATER_EQUAL:
            impl_->lastValue = impl_->builder->CreateICmpSGE(lhs, rhs, "ge");
            return;
        case TokenType::AMPERSAND:
            impl_->lastValue = impl_->builder->CreateAnd(lhs, rhs, "and");
            return;
        case TokenType::PIPE:
            impl_->lastValue = impl_->builder->CreateOr(lhs, rhs, "or");
            return;
        case TokenType::CARET:
            impl_->lastValue = impl_->builder->CreateXor(lhs, rhs, "xor");
            return;
        case TokenType::LEFT_SHIFT:
            impl_->lastValue = impl_->builder->CreateShl(lhs, rhs, "shl");
            return;
        case TokenType::RIGHT_SHIFT:
            impl_->lastValue = impl_->builder->CreateAShr(lhs, rhs, "shr");
            return;
        default:
            impl_->addError("Unsupported binary operator", node.location());
            impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
            return;
    }
}

/// Generates LLVM IR for Python-style chained comparisons (e.g., a < b < c).
///
/// Chained comparisons desugar to: (a op[0] b) and (b op[1] c) and ...
/// with each intermediate operand evaluated exactly once, and short-circuit
/// semantics: if any comparison is false, the whole expression is false.
///
/// IR pattern:
///  eval operands[0]
///  eval operands[1] -> temp
///  cmp operands[0] op[0] operands[1] -> if false, goto endBB
///  eval operands[2] -> temp
///  cmp operands[1] op[1] operands[2] -> if false, goto endBB
///  ...
///  goto endBB (result = true)
///  endBB: phi [false from any fail, true from last success]
///
/// Result type: i1.
void CodeGen::visit(ChainedCompExpr& node) {
    if (node.operands.size() < 2 || node.operators.empty()) {
        impl_->lastValue = llvm::ConstantInt::get(impl_->i1Type, 1);
        return;
    }

    auto* func = impl_->currentFunction;
    auto* endBB = llvm::BasicBlock::Create(*impl_->context, "chain.end", func);

    // [H5] Detect a set operand. For a variable, use the tracked VarKind (the
    // `in`-operator path does the same): a set[T] var carries VarKind::Set even
    // though its static type is modeled as a ListType, so checking type->kind()
    // alone would miss it and fall through to pointer-address comparison.
    auto isSetOperand = [&](Expr* e) -> bool {
        if (!e) return false;
        if (dynamic_cast<SetExpr*>(e) || dynamic_cast<SetCompExpr*>(e)) return true;
        if (auto* ne = dynamic_cast<NameExpr*>(e))
            return impl_->lookupVarKind(ne->name) == Impl::VarKind::Set;
        if (e->type && e->type->kind() == Type::Kind::Set) return true;
        return false;
    };
    // Mirror of the single-comparison list detection - so chained list
    // comparisons (`a < b < c`, `a == b == c`) get element-wise semantics
    // instead of the pointer-address compare the numeric branch would emit.
    auto isListOperand = [&](Expr* e) -> bool {
        if (!e) return false;
        if (dynamic_cast<ListExpr*>(e) || dynamic_cast<ListCompExpr*>(e)) return true;
        if (e->type && e->type->kind() == Type::Kind::List) return true;
        return impl_->resolveExprVarKind(e) == Impl::VarKind::List;
    };

    // Collect incoming edges for the PHI node at endBB.
    // Each edge provides an i1 value (true or false) and the block it came from.
    std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> phiIncoming;

    // Evaluate first operand
    node.operands[0]->accept(*this);
    llvm::Value* prevVal = impl_->lastValue;

    for (size_t i = 0; i < node.operators.size(); ++i) {
        // Evaluate the next operand
        node.operands[i + 1]->accept(*this);
        llvm::Value* curVal = impl_->lastValue;

        // Perform the comparison between prevVal and curVal
        llvm::Value* cmpResult = nullptr;
        auto opTypeRaw = node.operators[i].type();
        // `not in` and `is not` reuse the IN/IS emission and invert at the end
        // of this slot - keeps the per-slot short-circuit logic uniform.
        const bool negateSlot =
            (opTypeRaw == TokenType::NOT_IN || opTypeRaw == TokenType::IS_NOT);
        const TokenType opType =
            (opTypeRaw == TokenType::NOT_IN) ? TokenType::IN
            : (opTypeRaw == TokenType::IS_NOT) ? TokenType::IS
            : opTypeRaw;

        // Handle 'in' operator: membership test on set or string containment
        if (opType == TokenType::IN) {
            // Check if RHS is a set
            bool isSet = dynamic_cast<SetExpr*>(node.operands[i + 1].get()) != nullptr;
            if (!isSet) {
                if (auto* rhsName = dynamic_cast<NameExpr*>(node.operands[i + 1].get())) {
                    isSet = impl_->lookupVarKind(rhsName->name) == Impl::VarKind::Set;
                }
            }
            // Check if RHS is a list (mirrors the single-`in` path).
            bool rhsIsList = dynamic_cast<ListExpr*>(node.operands[i + 1].get()) != nullptr;
            if (!rhsIsList) {
                if (auto* rhsName = dynamic_cast<NameExpr*>(node.operands[i + 1].get()))
                    rhsIsList = impl_->lookupVarKind(rhsName->name) == Impl::VarKind::List;
            }
            if (!rhsIsList && node.operands[i + 1] && node.operands[i + 1]->type &&
                node.operands[i + 1]->type->kind() == Type::Kind::List) {
                rhsIsList = true;
            }
            if (isSet) {
                llvm::Value* val = prevVal;
                if (val->getType() == impl_->i1Type)
                    val = impl_->builder->CreateZExt(val, impl_->i64Type);
                else if (val->getType() == impl_->f64Type)
                    val = impl_->builder->CreateBitCast(val, impl_->i64Type);
                else if (val->getType()->isPointerTy())
                    val = impl_->builder->CreatePtrToInt(val, impl_->i64Type);
                auto* containsResult = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_set_contains"], {curVal, val}, "setcontains");
                cmpResult = impl_->builder->CreateICmpNE(
                    containsResult, llvm::ConstantInt::get(impl_->i64Type, 0), "inbool");
            } else if (rhsIsList && curVal->getType()->isPointerTy()) {
                llvm::Value* val = prevVal;
                if (val->getType() == impl_->i1Type)
                    val = impl_->builder->CreateZExt(val, impl_->i64Type);
                else if (val->getType() == impl_->f64Type)
                    val = impl_->builder->CreateBitCast(val, impl_->i64Type);
                else if (val->getType()->isPointerTy())
                    val = impl_->builder->CreatePtrToInt(val, impl_->i64Type);
                else if (val->getType() != impl_->i64Type)
                    val = impl_->builder->CreateZExtOrTrunc(val, impl_->i64Type);
                auto* containsResult = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_list_contains"], {curVal, val}, "listcontains");
                cmpResult = impl_->builder->CreateICmpNE(
                    containsResult, llvm::ConstantInt::get(impl_->i64Type, 0), "inbool");
            } else if (prevVal->getType()->isPointerTy() && curVal->getType()->isPointerTy()) {
                // String containment
                auto* containsResult = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_str_contains"], {curVal, prevVal}, "strcontains");
                cmpResult = impl_->builder->CreateICmpNE(
                    containsResult, llvm::ConstantInt::get(impl_->i64Type, 0), "inbool");
            } else {
                cmpResult = llvm::ConstantInt::get(impl_->i1Type, 0);
            }
        } else if (opType == TokenType::IS) {
            // Identity comparison: compare raw pointer/integer values
            llvm::Value* lv = prevVal;
            llvm::Value* rv = curVal;
            // Promote to i64 for uniform comparison
            if (lv->getType() == impl_->i1Type)
                lv = impl_->builder->CreateZExt(lv, impl_->i64Type);
            if (rv->getType() == impl_->i1Type)
                rv = impl_->builder->CreateZExt(rv, impl_->i64Type);
            if (lv->getType()->isPointerTy())
                lv = impl_->builder->CreatePtrToInt(lv, impl_->i64Type);
            if (rv->getType()->isPointerTy())
                rv = impl_->builder->CreatePtrToInt(rv, impl_->i64Type);
            cmpResult = impl_->builder->CreateICmpEQ(lv, rv, "is");
        } else if ((opType == TokenType::LESS || opType == TokenType::LESS_EQUAL ||
                    opType == TokenType::GREATER || opType == TokenType::GREATER_EQUAL) &&
                   prevVal->getType()->isPointerTy() && curVal->getType()->isPointerTy() &&
                   isSetOperand(node.operands[i].get()) &&
                   isSetOperand(node.operands[i + 1].get())) {
            // [H5] set subset/superset ordering.
            auto* subsetFn = impl_->runtimeFuncs["dragon_set_issubset"];
            auto callSubset = [&](llvm::Value* x, llvm::Value* y) -> llvm::Value* {
                auto* r = impl_->builder->CreateCall(subsetFn, {x, y}, "setsub");
                return impl_->builder->CreateICmpNE(
                    r, llvm::ConstantInt::get(impl_->i64Type, 0), "setsub.b");
            };
            switch (opType) {
                case TokenType::LESS_EQUAL: cmpResult = callSubset(prevVal, curVal); break;
                case TokenType::GREATER_EQUAL: cmpResult = callSubset(curVal, prevVal); break;
                case TokenType::LESS: {
                    auto* sub = callSubset(prevVal, curVal);
                    auto* rev = callSubset(curVal, prevVal);
                    cmpResult = impl_->builder->CreateAnd(
                        sub, impl_->builder->CreateNot(rev, "n"), "psub");
                    break;
                }
                default: {
                    auto* sup = callSubset(curVal, prevVal);
                    auto* fwd = callSubset(prevVal, curVal);
                    cmpResult = impl_->builder->CreateAnd(
                        sup, impl_->builder->CreateNot(fwd, "n"), "psup");
                    break;
                }
            }
        } else if (prevVal->getType()->isPointerTy() && curVal->getType()->isPointerTy() &&
                   isListOperand(node.operands[i].get()) &&
                   isListOperand(node.operands[i + 1].get()) &&
                   (opType == TokenType::LESS || opType == TokenType::LESS_EQUAL ||
                    opType == TokenType::GREATER || opType == TokenType::GREATER_EQUAL ||
                    opType == TokenType::EQUAL_EQUAL || opType == TokenType::NOT_EQUAL)) {
            // Native list comparison in a chained expr. Ordering via
            // dragon_list_cmp, equality via dragon_list_eq - both element-wise
            // (the numeric branch would pointer-compare).
            if (opType == TokenType::EQUAL_EQUAL || opType == TokenType::NOT_EQUAL) {
                auto* eq = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_list_eq"], {prevVal, curVal}, "list.eq");
                auto* eqb = impl_->builder->CreateICmpNE(
                    eq, llvm::ConstantInt::get(impl_->i64Type, 0), "list.eq.b");
                cmpResult = (opType == TokenType::EQUAL_EQUAL)
                    ? eqb : impl_->builder->CreateNot(eqb, "list.ne");
            } else {
                auto* cmp = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_list_cmp"], {prevVal, curVal}, "list.cmp");
                auto* zero = llvm::ConstantInt::get(impl_->i64Type, 0);
                switch (opType) {
                    case TokenType::LESS:
                        cmpResult = impl_->builder->CreateICmpSLT(cmp, zero); break;
                    case TokenType::LESS_EQUAL:
                        cmpResult = impl_->builder->CreateICmpSLE(cmp, zero); break;
                    case TokenType::GREATER:
                        cmpResult = impl_->builder->CreateICmpSGT(cmp, zero); break;
                    default:
                        cmpResult = impl_->builder->CreateICmpSGE(cmp, zero); break;
                }
            }
        } else if ((prevVal->getType() == impl_->boxType ||
                    curVal->getType() == impl_->boxType) &&
                   (opType == TokenType::LESS || opType == TokenType::LESS_EQUAL ||
                    opType == TokenType::GREATER || opType == TokenType::GREATER_EQUAL ||
                    opType == TokenType::EQUAL_EQUAL || opType == TokenType::NOT_EQUAL)) {
            // D039 Phase 11b: box operand in a chained comparison (`a < b < c`
            // on Any). The numeric branch below would ICmp a {i64,i64} struct
            // and crash. Mirror the single-comparison BinaryExpr box paths:
            // ordering via dragon_box_cmp, equality via dragon_box_eq.
            if (opType == TokenType::EQUAL_EQUAL || opType == TokenType::NOT_EQUAL) {
                llvm::Value* ba = impl_->boxNativeOperand(*this, node.operands[i].get(), prevVal);
                llvm::Value* bb = impl_->boxNativeOperand(*this, node.operands[i + 1].get(), curVal);
                auto* eq = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_box_eq"], {ba, bb}, "box.eq");
                auto* eqb = impl_->builder->CreateICmpNE(
                    eq, llvm::ConstantInt::get(impl_->i64Type, 0), "box.eq.b");
                cmpResult = (opType == TokenType::EQUAL_EQUAL)
                    ? eqb : impl_->builder->CreateNot(eqb, "box.ne");
            } else {
                int64_t cmpOp = (opType == TokenType::LESS) ? 0
                              : (opType == TokenType::LESS_EQUAL) ? 1
                              : (opType == TokenType::GREATER) ? 2 : 3;
                auto* cmp = impl_->emitBoxCmp(
                    *this, node.operands[i].get(), prevVal,
                    node.operands[i + 1].get(), curVal, cmpOp);
                auto* zero = llvm::ConstantInt::get(impl_->i64Type, 0);
                switch (opType) {
                    case TokenType::LESS:
                        cmpResult = impl_->builder->CreateICmpSLT(cmp, zero); break;
                    case TokenType::LESS_EQUAL:
                        cmpResult = impl_->builder->CreateICmpSLE(cmp, zero); break;
                    case TokenType::GREATER:
                        cmpResult = impl_->builder->CreateICmpSGT(cmp, zero); break;
                    default:
                        cmpResult = impl_->builder->CreateICmpSGE(cmp, zero); break;
                }
            }
        } else {
            // Numeric comparisons: handle float promotion and int/bool types
            llvm::Value* lv = prevVal;
            llvm::Value* rv = curVal;

            bool isFloat = (lv->getType() == impl_->f64Type ||
                            rv->getType() == impl_->f64Type);
            if (isFloat) {
                // Promote to f64
                if (lv->getType() == impl_->i64Type)
                    lv = impl_->builder->CreateSIToFP(lv, impl_->f64Type, "itof");
                if (lv->getType() == impl_->i1Type)
                    lv = impl_->builder->CreateUIToFP(lv, impl_->f64Type, "btof");
                if (rv->getType() == impl_->i64Type)
                    rv = impl_->builder->CreateSIToFP(rv, impl_->f64Type, "itof");
                if (rv->getType() == impl_->i1Type)
                    rv = impl_->builder->CreateUIToFP(rv, impl_->f64Type, "btof");

                switch (opType) {
                    case TokenType::LESS:
                        cmpResult = impl_->builder->CreateFCmpOLT(lv, rv, "flt");
                        break;
                    case TokenType::LESS_EQUAL:
                        cmpResult = impl_->builder->CreateFCmpOLE(lv, rv, "fle");
                        break;
                    case TokenType::GREATER:
                        cmpResult = impl_->builder->CreateFCmpOGT(lv, rv, "fgt");
                        break;
                    case TokenType::GREATER_EQUAL:
                        cmpResult = impl_->builder->CreateFCmpOGE(lv, rv, "fge");
                        break;
                    case TokenType::EQUAL_EQUAL:
                        cmpResult = impl_->builder->CreateFCmpOEQ(lv, rv, "feq");
                        break;
                    case TokenType::NOT_EQUAL:
                        cmpResult = impl_->builder->CreateFCmpONE(lv, rv, "fne");
                        break;
                    default:
                        impl_->addError("Unsupported chained comparison operator",
                                        node.location());
                        cmpResult = llvm::ConstantInt::get(impl_->i1Type, 0);
                        break;
                }
            } else {
                // Integer comparisons: promote bool to i64
                if (lv->getType() == impl_->i1Type)
                    lv = impl_->builder->CreateZExt(lv, impl_->i64Type, "btoi");
                if (rv->getType() == impl_->i1Type)
                    rv = impl_->builder->CreateZExt(rv, impl_->i64Type, "btoi");

                switch (opType) {
                    case TokenType::LESS:
                        cmpResult = impl_->builder->CreateICmpSLT(lv, rv, "lt");
                        break;
                    case TokenType::LESS_EQUAL:
                        cmpResult = impl_->builder->CreateICmpSLE(lv, rv, "le");
                        break;
                    case TokenType::GREATER:
                        cmpResult = impl_->builder->CreateICmpSGT(lv, rv, "gt");
                        break;
                    case TokenType::GREATER_EQUAL:
                        cmpResult = impl_->builder->CreateICmpSGE(lv, rv, "ge");
                        break;
                    case TokenType::EQUAL_EQUAL:
                        cmpResult = impl_->builder->CreateICmpEQ(lv, rv, "eq");
                        break;
                    case TokenType::NOT_EQUAL:
                        cmpResult = impl_->builder->CreateICmpNE(lv, rv, "ne");
                        break;
                    default:
                        impl_->addError("Unsupported chained comparison operator",
                                        node.location());
                        cmpResult = llvm::ConstantInt::get(impl_->i1Type, 0);
                        break;
                }
            }
        }

        if (negateSlot) {
            cmpResult = impl_->builder->CreateNot(cmpResult, "negcmp");
        }

        // Short-circuit: if this comparison is false, jump to endBB with false
        if (i < node.operators.size() - 1) {
            // Not the last comparison - need to short-circuit
            auto* nextBB = llvm::BasicBlock::Create(
                *impl_->context, "chain.next", func);
            impl_->builder->CreateCondBr(cmpResult, nextBB, endBB);
            // Record the false edge for the PHI
            phiIncoming.push_back(
                {llvm::ConstantInt::get(impl_->i1Type, 0),
                 impl_->builder->GetInsertBlock()});
            impl_->builder->SetInsertPoint(nextBB);
        } else {
            // Last comparison - its result is the chain result on this path
            impl_->builder->CreateBr(endBB);
            phiIncoming.push_back({cmpResult, impl_->builder->GetInsertBlock()});
        }

        // The current value becomes the left operand of the next comparison.
        // Use curVal (NOT the promoted lv/rv) so the original type is preserved.
        prevVal = curVal;
    }

    // Build the PHI at endBB
    impl_->builder->SetInsertPoint(endBB);
    auto* phi = impl_->builder->CreatePHI(impl_->i1Type,
                                           phiIncoming.size(), "chain.result");
    for (auto& [val, block] : phiIncoming) {
        phi->addIncoming(val, block);
    }
    impl_->lastValue = phi;
}

/// Generates LLVM IR for the walrus operator (name := value).
///
/// The walrus operator evaluates the RHS expression, assigns it to the named
/// variable (creating the variable if it doesn't exist), and yields the value
/// as its result. This allows assignments inside expressions, e.g.:
///  if (n := len(items)) > 0 { ... }
///
/// Implementation:
///  1. Evaluate node.value -> val
///  2. Look up or create alloca for node.name
///  3. Store val into the alloca
///  4. Set lastValue = val (walrus expression returns the assigned value)
///  5. Infer VarKind from the LLVM type of val
void CodeGen::visit(WalrusExpr& node) {
    // Evaluate the value expression
    node.value->accept(*this);
    llvm::Value* val = impl_->lastValue;

    // Determine the LLVM type for the variable
    llvm::Type* valType = val->getType();

    // Infer VarKind from the LLVM type and the RHS expression
    Impl::VarKind kind = Impl::VarKind::Other;
    if (valType == impl_->i64Type) {
        kind = Impl::VarKind::Int;
    } else if (valType == impl_->f64Type) {
        kind = Impl::VarKind::Float;
    } else if (valType == impl_->i1Type) {
        kind = Impl::VarKind::Bool;
    } else if (valType->isPointerTy()) {
        // Check the RHS expression to disambiguate pointer types
        if (auto* sl = dynamic_cast<StringLiteral*>(node.value.get())) {
            kind = sl->isBytes ? Impl::VarKind::List : Impl::VarKind::StrLiteral;  // D030 §5: bytes uses generic-heap VarKind; bytes-ness flows via Type::Kind
        } else if (dynamic_cast<ListExpr*>(node.value.get()) ||
                   dynamic_cast<ListCompExpr*>(node.value.get())) {
            kind = Impl::VarKind::List;
        } else if (dynamic_cast<DictExpr*>(node.value.get()) ||
                   dynamic_cast<DictCompExpr*>(node.value.get())) {
            kind = Impl::VarKind::Dict;
        } else if (dynamic_cast<TupleExpr*>(node.value.get())) {
            kind = Impl::VarKind::Tuple;
        } else if (dynamic_cast<SetExpr*>(node.value.get()) ||
                   dynamic_cast<SetCompExpr*>(node.value.get())) {
            kind = Impl::VarKind::Set;
        } else if (auto* rhsName = dynamic_cast<NameExpr*>(node.value.get())) {
            // Propagate VarKind from the source variable
            kind = impl_->lookupVarKind(rhsName->name);
        } else {
            kind = Impl::VarKind::Str;  // default for ptr: assume dynamic string
        }
    }

    // Look up existing variable or create a new alloca
    auto* alloca = impl_->lookupVar(node.name);
    bool hadExistingSlot = (alloca != nullptr);
    if (!alloca) {
        alloca = impl_->createEntryAlloca(
            impl_->currentFunction, node.name, valType);
        impl_->setVar(node.name, alloca, kind);
    }

    // Store with RC overwrite semantics for heap values.
    Impl::VarKind oldKind = hadExistingSlot
        ? impl_->lookupVarKind(node.name)
        : Impl::VarKind::Other;
    bool rhsBorrowed = Impl::isBorrowedHeapExpr(node.value.get());
    impl_->storeWithRCOverwrite(
        alloca, alloca->getAllocatedType(), val, oldKind, kind, rhsBorrowed, node.name);

    // Walrus expression returns the assigned value
    impl_->lastValue = val;
}

void CodeGen::visit(UnaryExpr& node) {
    // Resolve class name before visiting (for dunder dispatch)
    std::string operandClassName = impl_->resolveExprClassName(node.operand.get());

    node.operand->accept(*this);
    llvm::Value* operand = impl_->lastValue;

    // Dunder dispatch for class instances (unary operators)
    if (!operandClassName.empty() &&
        (operand->getType() == impl_->i8PtrType || operand->getType()->isPointerTy())) {
        if (node.op.type() == TokenType::MINUS && impl_->hasDunder(operandClassName, "__neg__")) {
            impl_->lastValue = impl_->callDunder(operandClassName, "__neg__", operand);
            return;
        }
        if (node.op.type() == TokenType::PLUS && impl_->hasDunder(operandClassName, "__pos__")) {
            impl_->lastValue = impl_->callDunder(operandClassName, "__pos__", operand);
            return;
        }
    }

    switch (node.op.type()) {
        case TokenType::MINUS:
            if (operand->getType() == impl_->f64Type) {
                impl_->lastValue = impl_->builder->CreateFNeg(operand, "fneg");
            } else {
                if (operand->getType() == impl_->i1Type)
                    operand = impl_->builder->CreateZExt(operand, impl_->i64Type);
                impl_->lastValue = impl_->builder->CreateNeg(operand, "neg");
            }
            return;
        case TokenType::NOT: {
            // Route through the shared truthiness rule so int/float/bool AND
            // pointers/containers all invert correctly. The old code only
            // handled i64/f64 and left a pointer operand unconverted, feeding a
            // raw pointer to CreateNot (bitwise-not on a ptr -> crash); it also
            // ignored container emptiness (`not []` is True in Python).
            llvm::Value* boolVal = impl_->toBool(operand, node.operand.get());
            impl_->lastValue = impl_->builder->CreateNot(boolVal, "not");
            return;
        }
        case TokenType::TILDE:
            if (operand->getType() == impl_->i1Type)
                operand = impl_->builder->CreateZExt(operand, impl_->i64Type);
            impl_->lastValue = impl_->builder->CreateNot(operand, "bitnot");
            return;
        default:
            impl_->lastValue = operand;
            return;
    }
}

void CodeGen::visit(IfExpr& node) {
    // Ternary: thenExpr if condition else elseExpr -> cond ? then : else
    //
    // D030 Phase 4: isinstance narrowing applies to ternary branches the
    // same way it does to IfStmt (see Statements.cpp:visit(IfStmt)) - without
    // this, `X.method() if isinstance(u, T) else u` where u is union-typed
    // sees u as the boxed value in both branches, so method dispatch on the
    // unboxed-payload-of-T fails and the PHI tries to merge unbox-result (ptr)
    // with the raw box ({i8,i64}) - assertion in PHINode::setIncomingValue.
    // Mirror the IfStmt narrowing helper rather than re-invent it.
    auto detectNarrowing = [this](Expr* cond) -> std::pair<std::string, Impl::VarKind> {
        if (auto* bin = dynamic_cast<BinaryExpr*>(cond)) {
            if (bin->op.type() == TokenType::NOT_EQUAL) {
                auto* lhsName = dynamic_cast<NameExpr*>(bin->left.get());
                auto* rhsName = dynamic_cast<NameExpr*>(bin->right.get());
                bool lhsIsNone = dynamic_cast<NoneLiteral*>(bin->left.get()) != nullptr;
                bool rhsIsNone = dynamic_cast<NoneLiteral*>(bin->right.get()) != nullptr;
                NameExpr* unionName = nullptr;
                if (lhsName && rhsIsNone) unionName = lhsName;
                else if (rhsName && lhsIsNone) unionName = rhsName;
                if (unionName &&
                    impl_->lookupVarKind(unionName->name) == Impl::VarKind::Union) {
                    auto membIt = impl_->unionMemberKinds.find(unionName->name);
                    if (membIt != impl_->unionMemberKinds.end() &&
                        membIt->second.size() == 2) {
                        Impl::VarKind nk = (membIt->second[0] == Impl::VarKind::Other)
                            ? membIt->second[1] : membIt->second[0];
                        if (nk != Impl::VarKind::Other)
                            return {unionName->name, nk};
                    }
                }
            }
        }
        auto* call = dynamic_cast<CallExpr*>(cond);
        if (!call) return {"", Impl::VarKind::Other};
        auto* callee = dynamic_cast<NameExpr*>(call->callee.get());
        if (!callee || callee->name != "isinstance" || call->args.size() != 2)
            return {"", Impl::VarKind::Other};
        auto* argName = dynamic_cast<NameExpr*>(call->args[0].get());
        if (!argName || impl_->lookupVarKind(argName->name) != Impl::VarKind::Union)
            return {"", Impl::VarKind::Other};
        auto* typeName = dynamic_cast<NameExpr*>(call->args[1].get());
        if (!typeName) return {"", Impl::VarKind::Other};
        Impl::VarKind nk = Impl::VarKind::Other;
        if (typeName->name == "int")        nk = Impl::VarKind::Int;
        else if (typeName->name == "float") nk = Impl::VarKind::Float;
        else if (typeName->name == "bool")  nk = Impl::VarKind::Bool;
        else if (typeName->name == "str")   nk = Impl::VarKind::Str;
        else if (typeName->name == "bytes") nk = Impl::VarKind::List;  // D030 §5: bytes/list share generic-heap dispatch
        else if (typeName->name == "list")  nk = Impl::VarKind::List;
        else if (typeName->name == "dict")  nk = Impl::VarKind::Dict;
        else if (typeName->name == "tuple") nk = Impl::VarKind::Tuple;
        else if (typeName->name == "set")   nk = Impl::VarKind::Set;
        else if (impl_->classNames.count(typeName->name)) nk = Impl::VarKind::ClassInstance;
        if (nk == Impl::VarKind::Other) return {"", Impl::VarKind::Other};
        return {argName->name, nk};
    };

    auto computeElseKind = [this](const std::string& varName, Impl::VarKind matchedKind) -> Impl::VarKind {
        auto membIt = impl_->unionMemberKinds.find(varName);
        if (membIt == impl_->unionMemberKinds.end()) return Impl::VarKind::Union;
        auto& members = membIt->second;
        if (members.size() == 2) {
            return (members[0] == matchedKind) ? members[1] : members[0];
        }
        return Impl::VarKind::Union;
    };

    node.condition->accept(*this);
    llvm::Value* cond = impl_->lastValue;

    // Convert to i1
    if (cond->getType() == impl_->i64Type) {
        cond = impl_->builder->CreateICmpNE(
            cond, llvm::ConstantInt::get(impl_->i64Type, 0));
    } else if (cond->getType() == impl_->f64Type) {
        cond = impl_->builder->CreateFCmpONE(
            cond, llvm::ConstantFP::get(impl_->f64Type, 0.0));
    }

    auto [narrowVar, narrowKind] = detectNarrowing(node.condition.get());

    auto* func = impl_->currentFunction;
    auto* thenBB = llvm::BasicBlock::Create(*impl_->context, "ifthen", func);
    auto* elseBB = llvm::BasicBlock::Create(*impl_->context, "ifelse", func);
    auto* mergeBB = llvm::BasicBlock::Create(*impl_->context, "ifmerge", func);

    impl_->builder->CreateCondBr(cond, thenBB, elseBB);

    // Normalize each branch to "+1 owned" before the merge so the PHI result
    // has a single, consistent ownership story for downstream consume sites.
    // Without this, a ternary like `xs[0] if c else "lit"` mixes a borrowed
    // subscript value with a literal - `isBorrowedHeapExpr(IfExpr)` returns
    // false, so the consumer skips its incref, and scope-exit decref of the
    // owning slot drops the only +1 of the list element (use-after-free that
    // surfaces on the next allocation reusing the freed slab).
    auto normalizeBranchOwnership = [&](Expr* branchExpr, llvm::Value* val) {
        if (!val || !val->getType()->isPointerTy()) return;
        if (!Impl::isBorrowedHeapExpr(branchExpr)) return;
        Impl::VarKind k = impl_->resolveExprVarKind(branchExpr);
        if (!Impl::isHeapKind(k) || k == Impl::VarKind::Union) return;
        impl_->emitIncrefByKind(val, k);
    };

    // Helper: enter narrowing for varName with the given kind. Unboxes the
    // current box value and shadows the local with a typed alloca, mirroring
    // the IfStmt branch-entry logic.
    auto enterNarrowing = [&](const std::string& varName, Impl::VarKind kind) -> bool {
        if (varName.empty() || kind == Impl::VarKind::Union) return false;
        impl_->pushScope();
        auto* localAlloca = impl_->lookupVar(varName);
        llvm::Value* slotPtr = localAlloca;
        bool slotIsBox = (localAlloca && localAlloca->getAllocatedType() == impl_->boxType);
        if (!slotPtr) {
            if (auto* gv = impl_->lookupModuleGlobal(varName)) {
                slotPtr = gv;
                slotIsBox = (gv->getValueType() == impl_->boxType);
            }
        }
        if (slotPtr && slotIsBox) {
            auto* box = impl_->builder->CreateLoad(
                impl_->boxType, slotPtr, varName + ".box.narrow");
            llvm::Value* payload = impl_->boxPayloadAsKind(box, kind);
            auto* narrowedAlloca = impl_->createEntryAlloca(
                func, varName + ".narrowed", payload->getType());
            impl_->builder->CreateStore(payload, narrowedAlloca);
            impl_->setVar(varName, narrowedAlloca, kind);
            impl_->scopes.back().borrowed.insert(varName);
        } else if (localAlloca) {
            impl_->setVar(varName, localAlloca, kind);
        }
        return true;
    };

    impl_->builder->SetInsertPoint(thenBB);
    bool thenNarrowed = enterNarrowing(narrowVar, narrowKind);
    node.thenExpr->accept(*this);
    llvm::Value* thenVal = impl_->lastValue;
    normalizeBranchOwnership(node.thenExpr.get(), thenVal);
    if (thenNarrowed) {
        impl_->emitScopeCleanup();
        impl_->popScope();
    }
    // Defer the branch to mergeBB: a type mismatch between the two arms may
    // require coercing/boxing this arm's value, and those instructions must be
    // emitted in this arm's block (before its terminator) so they dominate the
    // PHI. Capture the arm's end block now; terminate after unification below.
    llvm::BasicBlock* thenEnd = impl_->builder->GetInsertBlock();

    impl_->builder->SetInsertPoint(elseBB);
    bool elseNarrowed = false;
    if (!narrowVar.empty()) {
        elseNarrowed = enterNarrowing(narrowVar, computeElseKind(narrowVar, narrowKind));
    }
    node.elseExpr->accept(*this);
    llvm::Value* elseVal = impl_->lastValue;
    normalizeBranchOwnership(node.elseExpr.get(), elseVal);
    if (elseNarrowed) {
        impl_->emitScopeCleanup();
        impl_->popScope();
    }
    llvm::BasicBlock* elseEnd = impl_->builder->GetInsertBlock();

    // Unify the two arm types for the PHI. Arms flow at their native LLVM types
    // (bool=i1, int=i64, float=f64, str/heap=ptr, Any/Union=box), so a
    // bool|int / str|int / str|None ternary reaches here with mismatched types.
    // node.type is a UnionType when the arms differ (TypeChecker visit(IfExpr)).
    //  - both arms numeric (i1/i64/f64): widen to the common numeric type so a
    //  pure-numeric ternary stays unboxed (commandment #1);
    //  - otherwise (a ptr/box is involved): box BOTH arms into the {i64,i64}
    //  union box, matching the Union/Any result the consumer expects.
    // Coercion/boxing is emitted in each arm's end block so it dominates the
    // PHI. The prior code only converted i64->f64 and aborted LLVM verification
    // on any other mismatch (the H12 ternary crash: bool|int, str|int, ...).
    // Scalar numeric arms (bool/int/float) widen to a common numeric PHI; this
    // keeps the ternary unboxed (commandment #1) and avoids a box the consumer
    // would have to unbox to a native int/float (a path the runtime does poorly,
    // and which for an int target reads the slot at the box's tag - wrong).
    auto isNumeric = [&](llvm::Type* t) {
        return t == impl_->i1Type || t == impl_->i64Type || t == impl_->f64Type;
    };
    auto boxArm = [&](Expr* e, llvm::Value* v, llvm::BasicBlock* bb) -> llvm::Value* {
        if (v->getType() == impl_->boxType) return v;  // already a box
        impl_->builder->SetInsertPoint(bb);
        return impl_->makeBox(impl_->emitTagForExpr(e, *this), v);
    };
    bool typesDiffer = thenVal->getType() != elseVal->getType();
    // node.type is a UnionType when the arms have different Dragon types
    // (TypeChecker visit(IfExpr)). Two arms can share an LLVM type yet still be
    // a union - e.g. str|None are both ptr; a raw-ptr PHI there would be misread
    // as garbage by the box-shaped Any slot the consumer assigns into.
    bool nodeIsUnion = node.type && node.type->kind() == Type::Kind::Union;
    llvm::Type* resultType = thenVal->getType();
    if (typesDiffer && isNumeric(thenVal->getType()) && isNumeric(elseVal->getType())) {
        bool anyFloat = thenVal->getType() == impl_->f64Type ||
                        elseVal->getType() == impl_->f64Type;
        resultType = anyFloat ? impl_->f64Type : impl_->i64Type;
        auto widen = [&](llvm::Value* v, llvm::BasicBlock* bb) -> llvm::Value* {
            if (v->getType() == resultType) return v;
            impl_->builder->SetInsertPoint(bb);
            if (resultType == impl_->f64Type) {
                if (v->getType() == impl_->i1Type)  // bool is unsigned 0/1
                    v = impl_->builder->CreateZExt(v, impl_->i64Type, "b2i");
                return impl_->builder->CreateSIToFP(v, impl_->f64Type, "i2f");
            }
            return impl_->builder->CreateZExt(v, impl_->i64Type, "b2i");  // i1 -> i64
        };
        thenVal = widen(thenVal, thenEnd);
        elseVal = widen(elseVal, elseEnd);
    } else if (typesDiffer || (nodeIsUnion && thenVal->getType() != impl_->boxType)) {
        // Heterogeneous arms (ptr, None, instance, box) or a same-ptr union like
        // str|None: box both into the {i64,i64} union box matching the Union/Any
        // result the consumer expects. (Pure numeric unions are handled above.)
        resultType = impl_->boxType;
        thenVal = boxArm(node.thenExpr.get(), thenVal, thenEnd);
        elseVal = boxArm(node.elseExpr.get(), elseVal, elseEnd);
    }
    // else: arms share an LLVM type and the result isn't a union -> PHI at that
    // type (the unchanged fast path).

    // Terminate both arms now that their values are unified to resultType.
    impl_->builder->SetInsertPoint(thenEnd);
    impl_->builder->CreateBr(mergeBB);
    thenEnd = impl_->builder->GetInsertBlock();
    impl_->builder->SetInsertPoint(elseEnd);
    impl_->builder->CreateBr(mergeBB);
    elseEnd = impl_->builder->GetInsertBlock();

    impl_->builder->SetInsertPoint(mergeBB);
    auto* phi = impl_->builder->CreatePHI(resultType, 2, "ternary");
    phi->addIncoming(thenVal, thenEnd);
    phi->addIncoming(elseVal, elseEnd);
    impl_->lastValue = phi;
}

} // namespace dragon
