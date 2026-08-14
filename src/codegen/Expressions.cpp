/// Dragon CodeGen - Core Expressions (Name, Binary, Unary, If, Walrus, ChainedComp)
#include "../CodeGenImpl.h"
#include "llvm/IR/Intrinsics.h"

namespace dragon {

void CodeGen::visit(NameExpr& node) {
    // `dub x` (docs/002 2.7): containers deep-copy; immutable payloads (str/bytes/
    // tuples) identity-retain through a CALL so the result classifies owned.
    if (node.isDubMarked && impl_->options.gcMode == GCMode::RC) {
        node.isDubMarked = false;
        node.accept(*this);  // plain load (single re-entry)
        node.isDubMarked = true;
        llvm::Value* v = impl_->lastValue;
        auto k = node.type ? node.type->kind() : Type::Kind::Unknown;
        const char* fn = nullptr;
        switch (k) {
            case Type::Kind::Str:   fn = "dragon_str_retain"; break;
            case Type::Kind::List:  fn = "dragon_list_deep_copy"; break;
            case Type::Kind::Dict:  fn = "dragon_dict_deep_copy"; break;
            case Type::Kind::Set:   fn = "dragon_set_copy"; break;
            case Type::Kind::Bytes:
            case Type::Kind::Tuple: fn = "dragon_obj_retain"; break;
            default: break;  // E11 rejected everything else upstream
        }
        if (fn && v->getType()->isPointerTy()) {
            auto* callee = impl_->getOrDeclareRuntime(fn,
                llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType},
                                        false));
            impl_->lastValue =
                impl_->builder->CreateCall(callee, {impl_->toI8Ptr(v)}, "dub");
        }
        return;
    }
    // Module-typed names have no runtime representation; reaching here means
    // the TypeChecker missed a misuse, so raise an internal error instead of falling through.
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

    // Bare `__doc__` resolves to the module's docstring (Python parity); reuses
    // the same `.rodata` cache as `<mod>.__doc__` in Attributes.cpp.
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

    // Decision 025: a class name in value context loads its descriptor global,
    // except exception classes, which lower to their integer type code so exc_matches range-checks work.
    if (!impl_->resolvingCallTarget && impl_->classNames.count(node.name) &&
        !impl_->isExcType(node.name)) {
        auto descIt = impl_->classDescriptorGlobalsBySym.find(impl_->classSym(node.name));
        if (descIt != impl_->classDescriptorGlobalsBySym.end()) {
            impl_->lastValue = impl_->builder->CreateLoad(
                impl_->i64Type, descIt->second, node.name + "_desc");
            return;
        }
    }

    // A bare exception name in value context lowers to its integer type code,
    // so dragon_exc_matches is a single range compare (no descriptor walk, no RTTI).
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
        // May be a function name (function pointers/references); resolution
        // mirrors CallExpr: importedFuncAliases, then mangleFunc(currentModule), then userFuncName.
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
    // D027.1: cell-backed names (nonlocal-mutable) route through dragon_cell_get
    // so reads chain to the same backing slot the writer mutates.
    if (impl_->isCellBacked(node.name)) {
        impl_->lastValue = impl_->emitCellRead(
            alloca, impl_->lookupVarKind(node.name), node.name);
        return;
    }
    impl_->lastValue = impl_->builder->CreateLoad(
        alloca->getAllocatedType(), alloca, node.name);
}

void CodeGen::visit(BinaryExpr& node) {
    // Desugars `not in`/`is not` into NOT(IN)/NOT(IS) by re-entering visit with
    // the op rewritten, then inverting the i1 result; RAII restores the op after.
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

    // IntEnum/StrEnum members compare by value (Python parity): rewrites an
    // operand `e` to `e.value` so normal int/str comparison codegen handles it, no boxing.
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
                auto it = impl_->enumKindBySym.find(
                    inst->classType->definingModule.empty()
                        ? impl_->classSym(inst->classType->name)
                        : Impl::mangleClass(inst->classType->definingModule,
                                            inst->classType->name));
                return it != impl_->enumKindBySym.end() && it->second != Impl::EnumKind::Plain;
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

    // Union (`T | None`) vs `none`: extract the tag and compare to TAG_NONE (4),
    // since the box ({i64,i64}) and NoneLiteral's i8* null are different LLVM types.
    if (op == TokenType::EQUAL_EQUAL || op == TokenType::NOT_EQUAL) {
        bool lhsIsNone = dynamic_cast<NoneLiteral*>(node.left.get()) != nullptr;
        bool rhsIsNone = dynamic_cast<NoneLiteral*>(node.right.get()) != nullptr;
        bool lhsIsBox = lhs->getType() == impl_->boxType;
        bool rhsIsBox = rhs->getType() == impl_->boxType;
        if ((lhsIsBox && rhsIsNone) || (rhsIsBox && lhsIsNone)) {
            llvm::Value* box = lhsIsBox ? lhs : rhs;
            auto* tag = impl_->boxTag(box);
            auto* tagNone = llvm::ConstantInt::get(impl_->i64Type, TAG_NONE); // TAG_NONE
            impl_->lastValue = (op == TokenType::EQUAL_EQUAL)
                ? impl_->builder->CreateICmpEQ(tag, tagNone, "is.none")
                : impl_->builder->CreateICmpNE(tag, tagNone, "not.none");
            return;
        }
        // box==box/box!=box: tag-then-payload compare via dragon_box_eq, since
        // LLVM's ICmp rejects struct types (AssertOK would trip).
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
        // Gap #9: box vs native (`x == ""` where x is Any): boxes the native side
        // by its AST-derived tag, then dispatches through dragon_box_eq like box==box.
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

    // Box arithmetic dispatches through dragon_box_binop (result type depends on
    // runtime tags); must run after dunder dispatch but before the bytes/str/set paths below.
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
            // Box ordering (< <= > >=) had no handler and crashed on ICmp of a
            // {i64,i64}; routes through dragon_box_cmp and compares the three-way result to 0.
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

    // [H5] Set ordering (< <= > >=) is subset/superset (Python parity), not a
    // pointer compare: a<=b -> issubset(a,b); a<b -> issubset(a,b) & !issubset(b,a) (mirrored for >/>=).
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

    // Bytes ops must be checked before string (both are i8*); exprIsBytes covers
    // calls/attrs/nested exprs, or e.g. `bytes(l1)+bytes(l2)` falls into str_concat.
    {
        bool lhsIsBytes = impl_->exprIsBytes(node.left.get());
        bool rhsIsBytes = impl_->exprIsBytes(node.right.get());

        if (lhsIsBytes || rhsIsBytes) {
            if (op == TokenType::PLUS) {
                std::vector<llvm::Value*> opBases;
                if (impl_->options.gcMode == GCMode::RC) {
                    if (impl_->isOwnedPtrResult(lhs))
                        impl_->pushTempCleanupByKind(lhs, Impl::VarKind::List, opBases);
                    if (impl_->isOwnedPtrResult(rhs))
                        impl_->pushTempCleanupByKind(rhs, Impl::VarKind::List, opBases);
                }
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_bytes_concat"], {lhs, rhs}, "bytescat");
                impl_->popArgTempCleanups(opBases);
                // Decrefs owned intermediate bytes operands from chained exprs
                // ((a+b)+c); the str/list paths already did this, bytes didn't (leaked).
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
                std::vector<llvm::Value*> opBases;
                if (impl_->options.gcMode == GCMode::RC && impl_->isOwnedPtrResult(bv))
                    impl_->pushTempCleanupByKind(bv, Impl::VarKind::List, opBases);
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_bytes_repeat"], {bv, iv}, "bytesrep");
                impl_->popArgTempCleanups(opBases);
                // Drop an owned bytes operand temp ((a + b) * 3); the count is a
                // scalar, no decref. Mirrors the str-repeat path.
                if (impl_->options.gcMode == GCMode::RC && impl_->isOwnedPtrResult(bv))
                    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {bv});
                return;
            }
            // Drops owned bytes operand temps consumed only by == / != (the eq/cmp
            // helpers only READ), mirroring the str-compare path below.
            auto releaseOwnedBytesCmpOperands = [&]() {
                if (impl_->options.gcMode != GCMode::RC) return;
                if (!Impl::isBorrowedHeapExpr(node.left.get()) &&
                    impl_->isOwnedPtrResult(lhs))
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_decref"], {lhs});
                if (!Impl::isBorrowedHeapExpr(node.right.get()) &&
                    impl_->isOwnedPtrResult(rhs))
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_decref"], {rhs});
            };
            if (op == TokenType::EQUAL_EQUAL) {
                auto* eq = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_bytes_eq"], {lhs, rhs}, "byteseq");
                auto* res = impl_->builder->CreateICmpNE(
                    eq, llvm::ConstantInt::get(impl_->i64Type, 0));
                releaseOwnedBytesCmpOperands();
                impl_->lastValue = res;
                return;
            }
            if (op == TokenType::NOT_EQUAL) {
                auto* eq = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_bytes_eq"], {lhs, rhs}, "byteseq");
                auto* res = impl_->builder->CreateICmpEQ(
                    eq, llvm::ConstantInt::get(impl_->i64Type, 0));
                releaseOwnedBytesCmpOperands();
                impl_->lastValue = res;
                return;
            }
            if (op == TokenType::LESS || op == TokenType::GREATER ||
                op == TokenType::LESS_EQUAL || op == TokenType::GREATER_EQUAL) {
                auto* cmp = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_bytes_cmp"], {lhs, rhs}, "bytescmp");
                auto zero = llvm::ConstantInt::get(impl_->i64Type, 0);
                llvm::Value* res;
                if (op == TokenType::LESS)
                    res = impl_->builder->CreateICmpSLT(cmp, zero);
                else if (op == TokenType::GREATER)
                    res = impl_->builder->CreateICmpSGT(cmp, zero);
                else if (op == TokenType::LESS_EQUAL)
                    res = impl_->builder->CreateICmpSLE(cmp, zero);
                else
                    res = impl_->builder->CreateICmpSGE(cmp, zero);
                releaseOwnedBytesCmpOperands();
                impl_->lastValue = res;
                return;
            }
        }
    }

    // list+list makes a fresh list; must precede the i8*+i8* string-concat
    // fallthrough below, which would misread two list pointers as C-strings.
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
            bool lhsOwned = false, rhsOwned = false;
            if (impl_->options.gcMode == GCMode::RC) {
                lhsOwned = dynamic_cast<ListExpr*>(node.left.get()) ||
                           dynamic_cast<ListCompExpr*>(node.left.get()) ||
                           impl_->ownedTempDrainKind(node.left.get(), lhs) !=
                               Impl::VarKind::Other;
                rhsOwned = dynamic_cast<ListExpr*>(node.right.get()) ||
                           dynamic_cast<ListCompExpr*>(node.right.get()) ||
                           impl_->ownedTempDrainKind(node.right.get(), rhs) !=
                               Impl::VarKind::Other;
            }
            std::vector<llvm::Value*> opBases;
            if (lhsOwned) impl_->pushTempCleanupByKind(lhs, Impl::VarKind::List, opBases);
            if (rhsOwned) impl_->pushTempCleanupByKind(rhs, Impl::VarKind::List, opBases);
            auto* result = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_concat"], {lhs, rhs}, "listcat");
            impl_->popArgTempCleanups(opBases);
            // Decrefs owned temp operands: literals/comprehensions plus nested-
            // concat results via ownedTempDrainKind (`a+b+[5]` leaked the inner concat's +1).
            if (lhsOwned)
                impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {lhs});
            if (rhsOwned)
                impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {rhs});
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
            std::vector<llvm::Value*> opBases;
            if (sourceIsTemp && impl_->options.gcMode == GCMode::RC)
                impl_->pushTempCleanupByKind(listVal, Impl::VarKind::List, opBases);
            auto* result = impl_->builder->CreateCall(repeatFn, {listVal, countVal}, "listrepeat");
            impl_->popArgTempCleanups(opBases);
            if (sourceIsTemp && impl_->options.gcMode == GCMode::RC) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref"], {listVal});
            }
            impl_->lastValue = result;
            return;
        }
    }

    // str*int/int*str (bytes/lists already handled above); without this it fell
    // to the ptr/i64 path, multiplying the string's ADDRESS and segfaulting.
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

            // Peephole: literal-string * constant-int folds to a baked literal
            // (e.g. `"=" * 80`), zero runtime cost; a 64 KiB cap defers huge repeats to the runtime call.
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
            std::vector<llvm::Value*> opBases;
            if (strIsTemp && impl_->options.gcMode == GCMode::RC)
                impl_->pushTempCleanupByKind(strVal, Impl::VarKind::Str, opBases);
            auto* result = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_str_repeat"], {strVal, countVal}, "strrep");
            impl_->popArgTempCleanups(opBases);
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

    // list==list/dict==dict (unboxed) route through dragon_list_eq/dict_eq for
    // element-wise equality; must precede the i8*-i8* string-eq fallthrough below.
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

    // list<list (also <= > >=) routes through dragon_list_cmp for element-wise
    // ordering; the default pointer compare below would be silently wrong.
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
            std::vector<llvm::Value*> opBases;
            if (impl_->options.gcMode == GCMode::RC) {
                if (impl_->isOwnedStrResult(lhs))
                    impl_->pushTempCleanupByKind(lhs, Impl::VarKind::Str, opBases);
                if (impl_->isOwnedStrResult(rhs))
                    impl_->pushTempCleanupByKind(rhs, Impl::VarKind::Str, opBases);
            }
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_str_concat"], {lhs, rhs}, "strcat");
            impl_->popArgTempCleanups(opBases);
            // Decrefs owned intermediate string operands from chained concats
            // ((a+b)+c); see isOwnedStrResult for ownership convention and borrowed-returner blocklist.
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
        // Owned heap-string temps used as compare operands (slices/concats/
        // f-strings) carry a +1 the compare only READS; drain after, gated on the expr not just the value.
        auto releaseOwnedStrCmpOperands = [&]() {
            if (impl_->options.gcMode != GCMode::RC) return;
            if (!Impl::isBorrowedHeapExpr(node.left.get()) &&
                impl_->isOwnedStrResult(lhs))
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref_str"], {lhs});
            if (!Impl::isBorrowedHeapExpr(node.right.get()) &&
                impl_->isOwnedStrResult(rhs))
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref_str"], {rhs});
        };
        if (op == TokenType::EQUAL_EQUAL) {
            auto* eq = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_str_eq"], {lhs, rhs}, "streq");
            auto* res = impl_->builder->CreateICmpNE(
                eq, llvm::ConstantInt::get(impl_->i64Type, 0));
            releaseOwnedStrCmpOperands();
            impl_->lastValue = res;
            return;
        }
        if (op == TokenType::NOT_EQUAL) {
            auto* eq = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_str_eq"], {lhs, rhs}, "streq");
            auto* res = impl_->builder->CreateICmpEQ(
                eq, llvm::ConstantInt::get(impl_->i64Type, 0));
            releaseOwnedStrCmpOperands();
            impl_->lastValue = res;
            return;
        }
        // String ordering: <, >, <=, >= via dragon_str_cmp (strcmp wrapper)
        if (op == TokenType::LESS || op == TokenType::GREATER ||
            op == TokenType::LESS_EQUAL || op == TokenType::GREATER_EQUAL) {
            auto* cmp = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_str_cmp"], {lhs, rhs}, "strcmp");
            auto* zero = llvm::ConstantInt::get(impl_->i64Type, 0);
            llvm::Value* res;
            if (op == TokenType::LESS)
                res = impl_->builder->CreateICmpSLT(cmp, zero);
            else if (op == TokenType::GREATER)
                res = impl_->builder->CreateICmpSGT(cmp, zero);
            else if (op == TokenType::LESS_EQUAL)
                res = impl_->builder->CreateICmpSLE(cmp, zero);
            else
                res = impl_->builder->CreateICmpSGE(cmp, zero);
            releaseOwnedStrCmpOperands();
            impl_->lastValue = res;
            return;
        }
    }

    // 'in' operator: check membership in a set or string containment
    if (op == TokenType::IN) {
        // `in` only READS its operands; an owned heap temp on either side
        // (`f() in hay`) leaks unless drained (ownedTempDrainKind); skipped in chains (would UAF the next link).
        auto releaseOwnedInOperands = [&]() {
            if (impl_->options.gcMode != GCMode::RC) return;
            Impl::VarKind lk = impl_->ownedTempDrainKind(node.left.get(), lhs);
            if (lk != Impl::VarKind::Other) impl_->emitDecrefByKind(lhs, lk);
            Impl::VarKind rk = impl_->ownedTempDrainKind(node.right.get(), rhs);
            if (rk != Impl::VarKind::Other) impl_->emitDecrefByKind(rhs, rk);
        };
        // __contains__ dunder dispatch for class instances (RHS)
        std::string rhsClassName = impl_->resolveExprClassName(node.right.get());
        if (!rhsClassName.empty() && impl_->hasDunder(rhsClassName, "__contains__") &&
            (rhs->getType() == impl_->i8PtrType || rhs->getType()->isPointerTy())) {
            auto* result = impl_->callDunder(rhsClassName, "__contains__", rhs, {lhs});
            if (result->getType() == impl_->i64Type)
                result = impl_->builder->CreateICmpNE(result, llvm::ConstantInt::get(impl_->i64Type, 0));
            releaseOwnedInOperands();
            impl_->lastValue = result;
            return;
        }

        // Resolves the RHS kind once so attr/subscript exprs get the same
        // treatment as bare locals; else `"k" in r.params` fell through to dragon_str_contains (always False).
        Impl::VarKind rhsKind = impl_->resolveExprVarKind(node.right.get());

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
            impl_->lastValue = impl_->builder->CreateICmpNE(
                impl_->lastValue, llvm::ConstantInt::get(impl_->i64Type, 0), "inbool");
            releaseOwnedInOperands();
            return;
        }
        // Bytes containment (int/bytes in bytes): D030 S5 identifies bytes-ness
        // by static type/AST shape since VarKind::Bytes was deleted (slots use VarKind::List).
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
                releaseOwnedInOperands();
                return;
            }
        }
        // Dict membership dispatches on dict_has_key; D030 Phase 3.G routes
        // int-keyed dicts through dragon_dict_int_has_key so the key crosses at i64.
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
                    releaseOwnedInOperands();
                    return;
                }
                if (lhs->getType()->isPointerTy() && rhs->getType()->isPointerTy()) {
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_dict_has_key"], {rhs, lhs}, "dicthas");
                    impl_->lastValue = impl_->builder->CreateICmpNE(
                        impl_->lastValue, llvm::ConstantInt::get(impl_->i64Type, 0), "inbool");
                    releaseOwnedInOperands();
                    return;
                }
            }
        }
        // Deque membership must precede the list block: a deque is typed as
        // ListType, and dragon_list_contains would misread its header as list data (always False).
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
                releaseOwnedInOperands();
                return;
            }
        }
        // List membership must precede string-containment: a str-in-str-list has
        // ptr LHS/RHS and would misroute to dragon_str_contains otherwise.
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
                releaseOwnedInOperands();
                return;
            }
        }
        // String containment: "sub" in "string"
        if (lhs->getType()->isPointerTy() && rhs->getType()->isPointerTy()) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_str_contains"], {rhs, lhs}, "strcontains");
            impl_->lastValue = impl_->builder->CreateICmpNE(
                impl_->lastValue, llvm::ConstantInt::get(impl_->i64Type, 0), "inbool");
            releaseOwnedInOperands();
            return;
        }
        impl_->addError(
            "internal error: 'in' lowered with no container dispatch path; "
            "the front end should have rejected the operand types",
            node.location());
        releaseOwnedInOperands();
        impl_->lastValue = llvm::ConstantInt::get(impl_->i1Type, 0);
        return;
    }

    // 'is' identity comparison: raw pointer / integer equality after coercion.
    // Matches the chained-comparison IS path so single and chained forms agree.
    if (op == TokenType::IS) {
        // `box is None`: compares the box's tag to TAG_NONE (4); an ICmp of
        // {i64,i64} vs i8* null would trip LLVM's AssertOK.
        bool lhsIsNone = dynamic_cast<NoneLiteral*>(node.left.get()) != nullptr;
        bool rhsIsNone = dynamic_cast<NoneLiteral*>(node.right.get()) != nullptr;
        bool lhsIsBox = lhs->getType() == impl_->boxType;
        bool rhsIsBox = rhs->getType() == impl_->boxType;
        if ((lhsIsBox && rhsIsNone) || (rhsIsBox && lhsIsNone)) {
            llvm::Value* box = lhsIsBox ? lhs : rhs;
            auto* tag = impl_->boxTag(box);
            auto* tagNone = llvm::ConstantInt::get(impl_->i64Type, TAG_NONE); // TAG_NONE
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
                // Float exponentiation (`n ** 0.5`); without this it fell to the
                // integer switch and called dragon_pow_int on f64 operands (verify failure).
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_pow_float"], {lhs, rhs}, "fpow");
                return;
            case TokenType::DOUBLE_SLASH:
                // Float floor-division floor(a/b); previously fell to the integer
                // path and emitted ICmp on f64 operands (type-mismatch assertion crash).
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

    // Mixed ptr/i64 (e.g. a str field defaulting to i64): ==/!= routes to
    // dragon_str_eq; other ops coerce ptr->i64 via PtrToInt.
    if ((lhs->getType()->isPointerTy() && rhs->getType() == impl_->i64Type) ||
        (lhs->getType() == impl_->i64Type && rhs->getType()->isPointerTy())) {
        if (op == TokenType::EQUAL_EQUAL || op == TokenType::NOT_EQUAL) {
            // One side is ptr (likely a string), the other is i64 (likely a string
            // stored via i64-boxed field). Coerce i64->ptr and call dragon_str_eq.
            llvm::Value* origL = lhs;
            llvm::Value* origR = rhs;
            if (lhs->getType() == impl_->i64Type)
                lhs = impl_->builder->CreateIntToPtr(lhs, impl_->i8PtrType);
            if (rhs->getType() == impl_->i64Type)
                rhs = impl_->builder->CreateIntToPtr(rhs, impl_->i8PtrType);
            auto* streqResult = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_str_eq"], {lhs, rhs}, "streq");
            // Drains owned str temps (a method's str result reaches here i64-
            // shaped, so `p.peek() == "a"` leaked per compare); gated on non-borrow expr, str type, non-borrowed-returner callee.
            if (impl_->options.gcMode == GCMode::RC) {
                auto drainMixed = [&](Expr* e, llvm::Value* orig,
                                      llvm::Value* coerced) {
                    if (!e || Impl::isBorrowedHeapExpr(e)) return;
                    if (!e->type || e->type->kind() != Type::Kind::Str) return;
                    auto* call = llvm::dyn_cast<llvm::CallInst>(orig);
                    if (!call) return;
                    auto* fn = call->getCalledFunction();
                    if (fn && impl_->isBorrowedStrReturnerName(fn->getName().str()))
                        return;
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_decref_str"], {coerced});
                };
                drainMixed(node.left.get(), origL, lhs);
                drainMixed(node.right.get(), origR, rhs);
            }
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

    // --check-overflow emits llvm.s{add,sub,mul}.with.overflow.i64 and raises
    // OverflowError (code 22) via dragon_raise_exc when the overflow flag is set.
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

/// Chained comparisons (a < b < c) desugar to (a op b) and (b op c) and ...,
/// each operand evaluated once, short-circuiting to i1 on the first false.
void CodeGen::visit(ChainedCompExpr& node) {
    if (node.operands.size() < 2 || node.operators.empty()) {
        impl_->lastValue = llvm::ConstantInt::get(impl_->i1Type, 1);
        return;
    }

    auto* func = impl_->currentFunction;
    auto* endBB = llvm::BasicBlock::Create(*impl_->context, "chain.end", func);

    // [H5] Detects a set operand via tracked VarKind (mirrors the `in` path): a
    // set[T] var is modeled as ListType, so type->kind() alone would miss it.
    auto isSetOperand = [&](Expr* e) -> bool {
        if (!e) return false;
        if (dynamic_cast<SetExpr*>(e) || dynamic_cast<SetCompExpr*>(e)) return true;
        if (auto* ne = dynamic_cast<NameExpr*>(e))
            return impl_->lookupVarKind(ne->name) == Impl::VarKind::Set;
        if (e->type && e->type->kind() == Type::Kind::Set) return true;
        return false;
    };
    // Mirrors the single-comparison list detection so chained list comparisons
    // get element-wise semantics instead of a pointer-address compare.
    auto isListOperand = [&](Expr* e) -> bool {
        if (!e) return false;
        if (dynamic_cast<ListExpr*>(e) || dynamic_cast<ListCompExpr*>(e)) return true;
        if (e->type && e->type->kind() == Type::Kind::List) return true;
        return impl_->resolveExprVarKind(e) == Impl::VarKind::List;
    };
    // Mirrors the single-comparison string detection; without it, chained string
    // equality fell to pointer identity (test_rc_chained_compare.dr pins the bug).
    auto isStrOperand = [&](Expr* e) -> bool {
        if (!e) return false;
        if (auto* sl = dynamic_cast<StringLiteral*>(e)) return !sl->isBytes;
        if (e->type && e->type->kind() == Type::Kind::Str) return true;
        if (auto* ne = dynamic_cast<NameExpr*>(e)) {
            auto k = impl_->lookupVarKind(ne->name);
            return k == Impl::VarKind::Str || k == Impl::VarKind::StrLiteral;
        }
        return false;
    };
    // Owned heap temps (`"a"+b == c == c`) carry a +1 the compare only reads;
    // each is drained exactly once at its last use, gated by ownedTempDrainKind (borrows skipped).
    auto drainOwnedOperand = [&](Expr* e, llvm::Value* v) {
        if (impl_->options.gcMode != GCMode::RC || !e || !v) return;
        Impl::VarKind dk = impl_->ownedTempDrainKind(e, v);
        if (dk != Impl::VarKind::Other) impl_->emitDecrefByKind(v, dk);
    };
    auto operandNeedsDrain = [&](Expr* e, llvm::Value* v) -> bool {
        if (impl_->options.gcMode != GCMode::RC || !e || !v) return false;
        return impl_->ownedTempDrainKind(e, v) != Impl::VarKind::Other;
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
            // Native list comparison in a chained expr: ordering via
            // dragon_list_cmp, equality via dragon_list_eq, both element-wise.
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
            // A box operand in a chained comparison would ICmp a {i64,i64}
            // struct and crash; mirrors the BinaryExpr box paths (dragon_box_cmp/dragon_box_eq).
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
        } else if (prevVal->getType()->isPointerTy() && curVal->getType()->isPointerTy() &&
                   isStrOperand(node.operands[i].get()) &&
                   isStrOperand(node.operands[i + 1].get()) &&
                   (opType == TokenType::LESS || opType == TokenType::LESS_EQUAL ||
                    opType == TokenType::GREATER || opType == TokenType::GREATER_EQUAL ||
                    opType == TokenType::EQUAL_EQUAL || opType == TokenType::NOT_EQUAL)) {
            // Native string comparison in a chained expr: equality via
            // dragon_str_eq, ordering via dragon_str_cmp, mirroring the BinaryExpr path.
            if (opType == TokenType::EQUAL_EQUAL || opType == TokenType::NOT_EQUAL) {
                auto* eq = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_str_eq"], {prevVal, curVal}, "str.eq");
                auto* eqb = impl_->builder->CreateICmpNE(
                    eq, llvm::ConstantInt::get(impl_->i64Type, 0), "str.eq.b");
                cmpResult = (opType == TokenType::EQUAL_EQUAL)
                    ? eqb : impl_->builder->CreateNot(eqb, "str.ne");
            } else {
                auto* cmp = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_str_cmp"], {prevVal, curVal}, "str.cmp");
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

        // The PREV operand had its last use in the comparison above - drain it
        // here so both the continue and short-circuit paths release it.
        drainOwnedOperand(node.operands[i].get(), prevVal);

        // Short-circuit: if this comparison is false, jump to endBB with false
        if (i < node.operators.size() - 1) {
            // Not the last comparison: when the CUR operand is an owned temp,
            // the false edge is its last use too, so drain via a dedicated block (curVal doesn't dominate endBB there).
            auto* nextBB = llvm::BasicBlock::Create(
                *impl_->context, "chain.next", func);
            if (operandNeedsDrain(node.operands[i + 1].get(), curVal)) {
                auto* failBB = llvm::BasicBlock::Create(
                    *impl_->context, "chain.fail", func);
                impl_->builder->CreateCondBr(cmpResult, nextBB, failBB);
                impl_->builder->SetInsertPoint(failBB);
                drainOwnedOperand(node.operands[i + 1].get(), curVal);
                impl_->builder->CreateBr(endBB);
                phiIncoming.push_back(
                    {llvm::ConstantInt::get(impl_->i1Type, 0), failBB});
            } else {
                impl_->builder->CreateCondBr(cmpResult, nextBB, endBB);
                // Record the false edge for the PHI
                phiIncoming.push_back(
                    {llvm::ConstantInt::get(impl_->i1Type, 0),
                     impl_->builder->GetInsertBlock()});
            }
            impl_->builder->SetInsertPoint(nextBB);
        } else {
            // Last comparison: the CUR operand's last use was this comparison,
            // so drain in-block (covers both PHI edges).
            drainOwnedOperand(node.operands[i + 1].get(), curVal);
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

/// Walrus operator (name := value): evaluates the RHS, assigns it to the named
/// variable (creating it if needed), and yields the value as the expression's result.
void CodeGen::visit(WalrusExpr& node) {
    // Evaluate the value expression
    node.value->accept(*this);
    llvm::Value* val = impl_->lastValue;

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

    // Walrus returns a LOAD from the slot, not the raw RHS SSA value: the slot
    // adopted the RHS's +1, so value-gated consumers must see a borrow or they double-free it (test_rc_walrus.dr).
    impl_->lastValue = impl_->builder->CreateLoad(
        alloca->getAllocatedType(), alloca, node.name + ".walrus");
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
            // Routes through the shared truthiness rule so int/float/bool and
            // pointers/containers all invert correctly (old code crashed on a raw pointer and ignored container emptiness).
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
    // D030 Phase 4: isinstance narrowing applies to ternary branches like IfStmt;
    // without it, a union-typed operand stays boxed in both branches and the PHI fails to merge unbox-result with the raw box.
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
        else if (typeName->name == "list") {
            // Bare-Any list narrowing keeps the box binding (layout unknown);
            // gates on a DECLARED list member since a bare Any param also registers.
            auto membIt = impl_->unionMemberKinds.find(argName->name);
            bool declaredListMember =
                membIt != impl_->unionMemberKinds.end() &&
                std::find(membIt->second.begin(), membIt->second.end(),
                          Impl::VarKind::List) != membIt->second.end();
            if (!declaredListMember)
                return {"", Impl::VarKind::Other};
            nk = Impl::VarKind::List;
        }
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

    // Normalizes each branch to +1 owned before the merge, else a ternary mixing
    // a borrowed value with a literal (`xs[0] if c else "lit"`) skips the incref and use-after-frees the slot's only +1.
    // The retain must flow through the identity-retain CALL (not a void incref):
    // arg-position consumers require an owned CALL to classify the PHI owned, or the retained value leaks once per eval (gzip ternary bug).
    auto normalizeBranchOwnership = [&](Expr* branchExpr,
                                        llvm::Value* val) -> llvm::Value* {
        if (impl_->options.gcMode != GCMode::RC) return val;
        if (!val || !val->getType()->isPointerTy()) return val;
        Type::Kind tk = node.type ? node.type->kind() : Type::Kind::Unknown;
        const char* fn = nullptr;
        switch (tk) {
            case Type::Kind::Str:      fn = "dragon_str_retain"; break;
            case Type::Kind::Bytes:
            case Type::Kind::List:
            case Type::Kind::Dict:
            case Type::Kind::Set:
            case Type::Kind::Tuple:
            case Type::Kind::Instance: fn = "dragon_obj_retain"; break;
            default: {
                // Pre-existing behavior for kinds without a returning retain
                // (closures): in-place incref of borrowed heap arms.
                if (!Impl::isBorrowedHeapExpr(branchExpr)) return val;
                Impl::VarKind k = impl_->resolveExprVarKind(branchExpr);
                if (!Impl::isHeapKind(k) || k == Impl::VarKind::Union) return val;
                impl_->emitIncrefByKind(val, k);
                return val;
            }
        }
        // An arm that is already an owned call result (fresh concat, method
        // call) keeps its +1 and its owned classification as-is.
        bool alreadyOwned = !Impl::isBorrowedHeapExpr(branchExpr) &&
            (tk == Type::Kind::Str ? impl_->isOwnedStrResult(val)
                                   : impl_->isOwnedPtrResult(val));
        if (alreadyOwned) return val;
        auto* callee = impl_->getOrDeclareRuntime(fn,
            llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType},
                                    false));
        return impl_->builder->CreateCall(callee, {impl_->toI8Ptr(val)},
                                          "tern.retain");
    };

    // Enters narrowing for varName: unboxes the current box value and shadows
    // the local with a typed alloca, mirroring the IfStmt branch-entry logic.
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
    thenVal = normalizeBranchOwnership(node.thenExpr.get(), thenVal);
    if (thenNarrowed) {
        impl_->emitScopeCleanup();
        impl_->popScope();
    }
    // Defers the branch to mergeBB: a type mismatch between arms may need
    // coercion/boxing emitted in the arm's own block so it dominates the PHI.
    llvm::BasicBlock* thenEnd = impl_->builder->GetInsertBlock();

    impl_->builder->SetInsertPoint(elseBB);
    bool elseNarrowed = false;
    if (!narrowVar.empty()) {
        elseNarrowed = enterNarrowing(narrowVar, computeElseKind(narrowVar, narrowKind));
    }
    node.elseExpr->accept(*this);
    llvm::Value* elseVal = impl_->lastValue;
    elseVal = normalizeBranchOwnership(node.elseExpr.get(), elseVal);
    if (elseNarrowed) {
        impl_->emitScopeCleanup();
        impl_->popScope();
    }
    llvm::BasicBlock* elseEnd = impl_->builder->GetInsertBlock();

    // Unifies arm types for the PHI (arms flow at native LLVM types: i1/i64/f64/
    // ptr/box). Numeric arms widen to a common type (stays unboxed, commandment #1); otherwise both arms box into the {i64,i64} union.
    // Coercion is emitted in each arm's own end block so it dominates the PHI;
    // the prior code only handled i64->f64 and crashed on any other mismatch (the H12 ternary crash).
    auto isNumeric = [&](llvm::Type* t) {
        return t == impl_->i1Type || t == impl_->i64Type || t == impl_->f64Type;
    };
    auto boxArm = [&](Expr* e, llvm::Value* v, llvm::BasicBlock* bb) -> llvm::Value* {
        if (v->getType() == impl_->boxType) return v;  // already a box
        impl_->builder->SetInsertPoint(bb);
        return impl_->makeBox(impl_->emitTagForExpr(e, *this), v);
    };
    bool typesDiffer = thenVal->getType() != elseVal->getType();
    // node.type is a UnionType when arms have different Dragon types even if
    // they share an LLVM type (e.g. str|None are both ptr); a raw-ptr PHI there would be misread by a box-shaped Any slot.
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
        // Heterogeneous arms (ptr, None, instance, box) or a same-ptr union
        // (str|None) box both arms into the {i64,i64} union box the consumer expects.
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
