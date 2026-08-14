/// Dragon CodeGen - Augmented + Annotated Assignment (AugAssign, AnnAssign)
#include "../CodeGenImpl.h"

namespace dragon {

void CodeGen::visit(AugAssignStmt& node) {
    impl_->lastClosureCallableType = nullptr;
    impl_->lastValueIsType = false;
    if (auto* name = dynamic_cast<NameExpr*>(node.target.get())) {
        // x OP= rhs stays non-neg only if x was non-neg, the op preserves
        // non-negativity, and the rhs is non-neg.
        bool keepNonNeg = false;
        if (impl_->knownNonNeg.count(name->name)) {
            auto op = node.op.type();
            if ((op == TokenType::PLUS || op == TokenType::STAR ||
                 op == TokenType::POWER || op == TokenType::DOUBLE_SLASH ||
                 op == TokenType::PERCENT) &&
                node.value && impl_->isExprDefinitelyNonNeg(node.value.get())) {
                keepNonNeg = true;
            }
        }
        if (keepNonNeg) impl_->knownNonNeg.insert(name->name);
        else            impl_->knownNonNeg.erase(name->name);
        auto* alloca = impl_->lookupVar(name->name);
        llvm::GlobalVariable* gv = nullptr;
        llvm::Value* storeTarget = nullptr;
        llvm::Type* loadType = nullptr;

        if (alloca) {
            storeTarget = alloca;
            loadType = alloca->getAllocatedType();
        } else {
            gv = impl_->lookupModuleGlobal(name->name);
            if (gv && impl_->shouldUseModuleGlobal(name->name)) {
                storeTarget = gv;
                loadType = gv->getValueType();
            } else {
                return;
            }
        }

        auto varKind = impl_->lookupVarKind(name->name);
        const bool isCell = (alloca != nullptr) && impl_->isCellBacked(name->name);
        auto storeBack = [&](llvm::Value* result, Impl::VarKind newKind,
                             bool newIsBorrowed) {
            if (isCell)
                impl_->emitCellWrite(alloca, newKind, result, name->name);
            else
                impl_->storeWithRCOverwrite(storeTarget, loadType, result,
                                            varKind, newKind, newIsBorrowed,
                                            name->name);
        };

        llvm::Value* current = isCell
            ? impl_->emitCellRead(alloca, varKind, name->name)
            : impl_->builder->CreateLoad(loadType, storeTarget, name->name);
        node.value->accept(*this);
        llvm::Value* rhs = impl_->lastValue;

        // Dunder dispatch for class instance augmented assignment
        std::string augClassName = impl_->resolveExprClassName(node.target.get());
        if (!augClassName.empty() &&
            (current->getType() == impl_->i8PtrType || current->getType()->isPointerTy())) {
            std::string iDunder, dunder;
            switch (node.op.type()) {
                case TokenType::PLUS_EQUAL:         iDunder = "__iadd__"; dunder = "__add__"; break;
                case TokenType::MINUS_EQUAL:        iDunder = "__isub__"; dunder = "__sub__"; break;
                case TokenType::STAR_EQUAL:         iDunder = "__imul__"; dunder = "__mul__"; break;
                case TokenType::SLASH_EQUAL:        iDunder = "__itruediv__"; dunder = "__truediv__"; break;
                case TokenType::DOUBLE_SLASH_EQUAL: iDunder = "__ifloordiv__"; dunder = "__floordiv__"; break;
                case TokenType::PERCENT_EQUAL:      iDunder = "__imod__"; dunder = "__mod__"; break;
                case TokenType::POWER_EQUAL:        iDunder = "__ipow__"; dunder = "__pow__"; break;
                default: break;
            }
            if (!iDunder.empty()) {
                llvm::Value* result = nullptr;
                if (impl_->hasDunder(augClassName, iDunder))
                    result = impl_->callDunder(augClassName, iDunder, current, {rhs});
                else if (impl_->hasDunder(augClassName, dunder))
                    result = impl_->callDunder(augClassName, dunder, current, {rhs});
                if (result) {
                    storeBack(result, varKind, /*newIsBorrowed=*/false);
                    return;
                }
            }
        }

        // str +=: amortized in-place append keeps an accumulator loop O(n);
        // the helper owns the old-value decref and the owned-rhs cleanup.
        if ((varKind == Impl::VarKind::Str || varKind == Impl::VarKind::StrLiteral)
            && node.op.type() == TokenType::PLUS_EQUAL) {
            if (isCell) {
                // A cell-backed str has no stable slot to append in place:
                // concat and write back through the cell (carries the RC).
                llvm::Value* cat = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_str_concat"], {current, rhs}, "strcat");
                storeBack(cat, Impl::VarKind::Str, /*newIsBorrowed=*/false);
            } else {
                impl_->emitStrAppendInplace(storeTarget, current, rhs, name->name);
            }
            return;
        }
        // str *= int, else it falls into the numeric branch and emits
        // CreateMul(ptr, i64) - invalid IR. Fresh string, so the slot becomes Str.
        if ((varKind == Impl::VarKind::Str || varKind == Impl::VarKind::StrLiteral)
            && node.op.type() == TokenType::STAR_EQUAL) {
            llvm::Value* countVal = rhs;
            if (countVal->getType() == impl_->i1Type)
                countVal = impl_->builder->CreateZExt(countVal, impl_->i64Type);
            llvm::Value* result = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_str_repeat"], {current, countVal}, "strrep");
            storeBack(result, Impl::VarKind::Str, /*newIsBorrowed=*/false);
            if (!isCell) {
                if (auto* ai = llvm::dyn_cast<llvm::AllocaInst>(storeTarget)) {
                    impl_->setVar(name->name, ai, Impl::VarKind::Str);
                } else {
                    impl_->moduleGlobalKinds[impl_->globalKeyOrOwn(name->name)] =
                        Impl::VarKind::Str;
                }
            }
            return;
        }
        // D030: bytes' VarKind collapses into the generic-heap cohort, so only
        // the TypeChecker-propagated static type separates bytes += from list +=.
        bool targetIsBytes = node.target && node.target->type &&
                             node.target->type->kind() == Type::Kind::Bytes;
        if (targetIsBytes && node.op.type() == TokenType::PLUS_EQUAL) {
            llvm::Value* result = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_concat"], {current, rhs}, "bytescat");
            storeBack(result, varKind, /*newIsBorrowed=*/false);
            // bytes_concat copies both operands, so an owned rhs temp is otherwise
            // orphaned; isOwnedPtrResult screens out borrowed names/fields.
            if (impl_->options.gcMode == GCMode::RC && impl_->isOwnedPtrResult(rhs))
                impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {rhs});
            return;
        }

        // list += is in-place extend: it mutates the SAME object, so no store
        // back; else it falls into the numeric switch and emits add ptr (invalid IR).
        bool targetIsList = node.target && node.target->type &&
                            node.target->type->kind() == Type::Kind::List;
        if (targetIsList && node.op.type() == TokenType::PLUS_EQUAL) {
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_extend"], {current, rhs});
            // extend copies elements but not the container: decref an owned
            // literal/comprehension rhs; a bare variable is borrowed - leave it.
            if (impl_->options.gcMode == GCMode::RC &&
                (dynamic_cast<ListExpr*>(node.value.get()) ||
                 dynamic_cast<ListCompExpr*>(node.value.get()))) {
                impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {rhs});
            }
            return;
        }

        // D039: box arithmetic when either side is an Any/Union box - route
        // through dragon_box_binop, then checked-unbox into the slot's type.
        {
            int64_t opcode = impl_->binopOpcodeForToken(node.op.type());
            bool eitherBox = (current->getType() == impl_->boxType ||
                              rhs->getType() == impl_->boxType);
            if (opcode >= 0 && eitherBox) {
                llvm::Value* resultBox = impl_->emitBoxBinop(
                    *this, node.target.get(), current, node.value.get(), rhs,
                    opcode);
                llvm::Value* stored = impl_->unboxBoxResultChecked(
                    resultBox, loadType, varKind, Impl::kNoListElemCheck,
                    node.target->type ? node.target->type->kind()
                                      : Type::Kind::Unknown);
                storeBack(stored, varKind, /*newIsBorrowed=*/false);
                return;
            }
        }

        bool isFloat = (current->getType() == impl_->f64Type ||
                        rhs->getType() == impl_->f64Type);

        if (isFloat) {
            if (current->getType() == impl_->i64Type)
                current = impl_->builder->CreateSIToFP(current, impl_->f64Type);
            if (rhs->getType() == impl_->i64Type)
                rhs = impl_->builder->CreateSIToFP(rhs, impl_->f64Type);
        }

        llvm::Value* result = nullptr;
        switch (node.op.type()) {
            case TokenType::PLUS_EQUAL:
                result = isFloat ? impl_->builder->CreateFAdd(current, rhs)
                                 : impl_->builder->CreateAdd(current, rhs);
                break;
            case TokenType::MINUS_EQUAL:
                result = isFloat ? impl_->builder->CreateFSub(current, rhs)
                                 : impl_->builder->CreateSub(current, rhs);
                break;
            case TokenType::STAR_EQUAL:
                result = isFloat ? impl_->builder->CreateFMul(current, rhs)
                                 : impl_->builder->CreateMul(current, rhs);
                break;
            case TokenType::SLASH_EQUAL:
                if (!isFloat) {
                    current = impl_->builder->CreateSIToFP(current, impl_->f64Type);
                    rhs = impl_->builder->CreateSIToFP(rhs, impl_->f64Type);
                }
                result = impl_->builder->CreateFDiv(current, rhs);
                break;
            case TokenType::DOUBLE_SLASH_EQUAL:
                result = impl_->emitIntFloorDiv(current, rhs);
                break;
            case TokenType::PERCENT_EQUAL:
                result = impl_->emitIntMod(current, rhs);
                break;
            case TokenType::POWER_EQUAL:
                result = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_pow_int"], {current, rhs}, "pow");
                break;
            case TokenType::AMPERSAND_EQUAL:
                result = impl_->builder->CreateAnd(current, rhs, "and");
                break;
            case TokenType::PIPE_EQUAL:
                result = impl_->builder->CreateOr(current, rhs, "or");
                break;
            case TokenType::CARET_EQUAL:
                result = impl_->builder->CreateXor(current, rhs, "xor");
                break;
            case TokenType::LEFT_SHIFT_EQUAL:
                result = impl_->builder->CreateShl(current, rhs, "shl");
                break;
            case TokenType::RIGHT_SHIFT_EQUAL:
                result = impl_->builder->CreateAShr(current, rhs, "shr");
                break;
            default:
                result = current;
                break;
        }
        if (result) {
            storeBack(result, varKind, /*newIsBorrowed=*/false);
        }
    } else if (auto* sub = dynamic_cast<SubscriptExpr*>(node.target.get())) {
        // d[key] OP= value / lst[i] OP= value. KeyError-if-absent is preserved
        // by the runtime get (Python reads d[k] before the op).
        Type::Kind vk = impl_->resolveDictValueKind(sub->object.get());
        bool intKeyed = impl_->dictKeyIsInt(sub->object.get());
        bool strKeyed = !intKeyed;
        // str-keyed dict, int value: fused single-probe read-modify-write.
        if (strKeyed && vk == Type::Kind::Int) {
            int opc = -1;
            switch (node.op.type()) {
                case TokenType::PLUS_EQUAL:         opc = 0; break;
                case TokenType::MINUS_EQUAL:        opc = 1; break;
                case TokenType::STAR_EQUAL:         opc = 2; break;
                case TokenType::DOUBLE_SLASH_EQUAL: opc = 3; break;
                case TokenType::PERCENT_EQUAL:      opc = 4; break;
                case TokenType::AMPERSAND_EQUAL:    opc = 5; break;
                case TokenType::PIPE_EQUAL:         opc = 6; break;
                case TokenType::CARET_EQUAL:        opc = 7; break;
                case TokenType::LEFT_SHIFT_EQUAL:   opc = 8; break;
                case TokenType::RIGHT_SHIFT_EQUAL:  opc = 9; break;
                default: break;
            }
            if (opc >= 0) {
                sub->object->accept(*this);
                llvm::Value* dict = impl_->lastValue;
                sub->index->accept(*this);
                llvm::Value* key = impl_->lastValue;
                node.value->accept(*this);
                llvm::Value* operand = impl_->lastValue;
                if (operand->getType() == impl_->i1Type)
                    operand = impl_->builder->CreateZExt(operand, impl_->i64Type);
                if (operand->getType() == impl_->i64Type) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_dict_str_iaug_i64"],
                        {dict, key, operand,
                         llvm::ConstantInt::get(impl_->i64Type, opc)});
                    if (impl_->options.gcMode == GCMode::RC &&
                        impl_->isOwnedStrResult(key))
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_decref_str"], {key});
                    return;
                }
            }
        }
        // int-keyed dict, int value: get + op + set (no fused iaug for int
        // keys). Int keys are not heap, so no key decref.
        if (intKeyed && vk == Type::Kind::Int) {
            sub->object->accept(*this);
            llvm::Value* dict = impl_->lastValue;
            sub->index->accept(*this);
            llvm::Value* key = impl_->lastValue;
            if (key->getType() == impl_->i1Type)
                key = impl_->builder->CreateZExt(key, impl_->i64Type);
            llvm::Value* tagInt = llvm::ConstantInt::get(impl_->i64Type, /*TAG_INT*/0);
            llvm::Value* cur = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_int_get_checked"],
                {dict, key, tagInt}, "augi.cur");
            node.value->accept(*this);
            llvm::Value* rhs = impl_->lastValue;
            if (rhs->getType() == impl_->i1Type)
                rhs = impl_->builder->CreateZExt(rhs, impl_->i64Type);
            if (rhs->getType() == impl_->i64Type) {
                llvm::Value* res = impl_->emitIntAugOp(cur, rhs, node.op.type());
                if (res) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_dict_int_set_tagged"],
                        {dict, key, res, tagInt});
                    return;
                }
            }
        }
        // Dict with FLOAT value (str- OR int-keyed): get + float op + set.
        // emitFloatAugOp covers += -= *= /= //= %= (Python float floor/mod).
        if (vk == Type::Kind::Float) {
            sub->object->accept(*this);
            llvm::Value* dict = impl_->lastValue;
            sub->index->accept(*this);
            llvm::Value* key = impl_->lastValue;
            llvm::Value* cur = nullptr;
            if (intKeyed) {
                if (key->getType() == impl_->i1Type)
                    key = impl_->builder->CreateZExt(key, impl_->i64Type);
                cur = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_int_get_f64"], {dict, key}, "augf.cur");
            } else {
                cur = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_get_str_f64"], {dict, key}, "augf.cur");
            }
            node.value->accept(*this);
            llvm::Value* rhs = impl_->coerceToF64(impl_->lastValue);
            llvm::Value* res = rhs ? impl_->emitFloatAugOp(cur, rhs, node.op.type()) : nullptr;
            if (res) {
                if (intKeyed) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_dict_int_set_f64"], {dict, key, res});
                } else {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_dict_set_str_f64"], {dict, key, res});
                    if (impl_->options.gcMode == GCMode::RC &&
                        impl_->isOwnedStrResult(key))
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_decref_str"], {key});
                }
                return;
            }
        }
        // Dict str/bytes value, `+=` only: get_*_ptr borrows the current value,
        // concat allocates a fresh +1, set_*_ptr decrefs the old and adopts it.
        if ((vk == Type::Kind::Str || vk == Type::Kind::Bytes) &&
            node.op.type() == TokenType::PLUS_EQUAL) {
            bool isBytesVal = (vk == Type::Kind::Bytes);
            llvm::Value* tagV = llvm::ConstantInt::get(
                impl_->i64Type, isBytesVal ? /*TAG_BYTES*/7 : /*TAG_STR*/1);
            const char* concatFn = isBytesVal ? "dragon_bytes_concat" : "dragon_str_concat";
            sub->object->accept(*this);
            llvm::Value* dict = impl_->lastValue;
            sub->index->accept(*this);
            llvm::Value* key = impl_->lastValue;
            llvm::Value* cur = nullptr;
            if (intKeyed) {
                if (key->getType() == impl_->i1Type)
                    key = impl_->builder->CreateZExt(key, impl_->i64Type);
                cur = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_int_get_ptr"], {dict, key, tagV}, "augs.cur");
            } else {
                cur = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_get_str_ptr"], {dict, key, tagV}, "augs.cur");
            }
            node.value->accept(*this);
            llvm::Value* rhs = impl_->lastValue;
            if (rhs->getType() == impl_->i64Type)
                rhs = impl_->builder->CreateIntToPtr(rhs, impl_->i8PtrType);
            llvm::Value* newVal = impl_->builder->CreateCall(
                impl_->runtimeFuncs[concatFn], {cur, rhs}, "augs.cat");
            if (intKeyed)
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_int_set_ptr"], {dict, key, newVal, tagV});
            else
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_set_str_ptr"], {dict, key, newVal, tagV});
            if (impl_->options.gcMode == GCMode::RC) {
                if (impl_->isOwnedStrResult(rhs))
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs[isBytesVal ? "dragon_decref" : "dragon_decref_str"], {rhs});
                if (!intKeyed && impl_->isOwnedStrResult(key))
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_decref_str"], {key});
            }
            return;
        }
        // lst[i] OP= value: single-eval of list + index so get and set hit the
        // same element; dragon_list_get/set handle negative indices + bounds.
        bool isList = false;
        Type::Kind elemKind = Type::Kind::Int;
        if (auto* objName = dynamic_cast<NameExpr*>(sub->object.get())) {
            if (impl_->lookupVarKind(objName->name) == Impl::VarKind::List) {
                isList = true;
                auto it = impl_->varListElemKinds.find(objName->name);
                if (it != impl_->varListElemKinds.end()) elemKind = it->second;
            }
        }
        if (!isList && sub->object->type &&
            sub->object->type->kind() == Type::Kind::List) {
            isList = true;
            elemKind = impl_->getIterableElementKind(sub->object.get());
        }
        if (isList && (elemKind == Type::Kind::Int || elemKind == Type::Kind::Bool)) {
            sub->object->accept(*this);
            llvm::Value* list = impl_->lastValue;
            sub->index->accept(*this);
            llvm::Value* idx = impl_->lastValue;
            if (idx->getType() == impl_->i1Type)
                idx = impl_->builder->CreateZExt(idx, impl_->i64Type);
            llvm::Value* cur = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_get"], {list, idx}, "augl.cur");
            node.value->accept(*this);
            llvm::Value* rhs = impl_->lastValue;
            if (rhs->getType() == impl_->i1Type)
                rhs = impl_->builder->CreateZExt(rhs, impl_->i64Type);
            if (rhs->getType() == impl_->i64Type) {
                llvm::Value* res = impl_->emitIntAugOp(cur, rhs, node.op.type());
                if (res) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_list_set"], {list, idx, res});
                    return;
                }
            }
        } else if (isList && elemKind == Type::Kind::Float) {
            sub->object->accept(*this);
            llvm::Value* list = impl_->lastValue;
            sub->index->accept(*this);
            llvm::Value* idx = impl_->lastValue;
            if (idx->getType() == impl_->i1Type)
                idx = impl_->builder->CreateZExt(idx, impl_->i64Type);
            llvm::Value* cur = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_get_f64"], {list, idx}, "auglf.cur");
            node.value->accept(*this);
            llvm::Value* rhs = impl_->coerceToF64(impl_->lastValue);
            llvm::Value* res = rhs ? impl_->emitFloatAugOp(cur, rhs, node.op.type()) : nullptr;
            if (res) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_list_set_f64"], {list, idx, res});
                return;
            }
        } else if (isList && (elemKind == Type::Kind::Str || elemKind == Type::Kind::Bytes)
                   && node.op.type() == TokenType::PLUS_EQUAL) {
            // lst[i] += s for str/bytes elements: get_ptr borrows, concat
            // allocates a fresh +1, set_ptr decrefs the old and adopts it.
            bool isBytesElem = (elemKind == Type::Kind::Bytes);
            const char* concatFn = isBytesElem ? "dragon_bytes_concat" : "dragon_str_concat";
            sub->object->accept(*this);
            llvm::Value* list = impl_->lastValue;
            sub->index->accept(*this);
            llvm::Value* idx = impl_->lastValue;
            if (idx->getType() == impl_->i1Type)
                idx = impl_->builder->CreateZExt(idx, impl_->i64Type);
            llvm::Value* cur = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_get_ptr"], {list, idx}, "augls.cur");
            node.value->accept(*this);
            llvm::Value* rhs = impl_->lastValue;
            if (rhs->getType() == impl_->i64Type)
                rhs = impl_->builder->CreateIntToPtr(rhs, impl_->i8PtrType);
            llvm::Value* newVal = impl_->builder->CreateCall(
                impl_->runtimeFuncs[concatFn], {cur, rhs}, "augls.cat");
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_set_ptr"], {list, idx, newVal});
            if (impl_->options.gcMode == GCMode::RC && impl_->isOwnedStrResult(rhs))
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs[isBytesElem ? "dragon_decref" : "dragon_decref_str"], {rhs});
            return;
        }
    } else if (auto* attr = dynamic_cast<AttributeExpr*>(node.target.get())) {
        // obj.field OP= value. Still a silent no-op: @property fields (need
        // getter/setter dispatch) and heap-typed static fields.
        auto* objName = dynamic_cast<NameExpr*>(attr->object.get());
        {
            // Static field `ClassName.field OP= value`, stored as a module
            // global. Numeric only; heap static fields fall through.
            auto sfIt = objName ? impl_->staticFieldGlobalsBySym.find(impl_->classSym(objName->name))
                                : impl_->staticFieldGlobalsBySym.end();
            if (sfIt != impl_->staticFieldGlobalsBySym.end()) {
                auto gvIt = sfIt->second.find(attr->attribute);
                if (gvIt != sfIt->second.end()) {
                    llvm::GlobalVariable* gv = gvIt->second;
                    llvm::Type* ft = gv->getValueType();
                    if (ft == impl_->i64Type || ft == impl_->f64Type) {
                        llvm::Value* cur = impl_->builder->CreateLoad(ft, gv, attr->attribute);
                        node.value->accept(*this);
                        llvm::Value* rhs = impl_->lastValue;
                        llvm::Value* res = nullptr;
                        if (ft == impl_->i64Type) {
                            if (rhs->getType() == impl_->i1Type)
                                rhs = impl_->builder->CreateZExt(rhs, impl_->i64Type);
                            if (rhs->getType() == impl_->i64Type)
                                res = impl_->emitIntAugOp(cur, rhs, node.op.type());
                        } else {
                            rhs = impl_->coerceToF64(rhs);
                            if (rhs) res = impl_->emitFloatAugOp(cur, rhs, node.op.type());
                        }
                        if (res) {
                            impl_->builder->CreateStore(res, gv);
                            return;
                        }
                    }
                }
            }
            std::string className;
            if (objName && objName->name == "self" && !impl_->currentClassName.empty())
                className = impl_->currentClassName;
            else if (objName) {
                auto vit = impl_->varClassNames.find(objName->name);
                if (vit != impl_->varClassNames.end()) className = vit->second;
            } else {
                // Nested base (`a.b.n += 1`): resolve the base expression's
                // static class, else the compound store silently no-ops.
                className = impl_->resolveExprClassName(attr->object.get());
            }
            if (!className.empty()) {
                auto structIt = impl_->classStructTypesBySym.find(impl_->classSym(className));
                auto fieldIt = impl_->classFieldIndicesBySym.find(impl_->classSym(className));
                if (structIt != impl_->classStructTypesBySym.end() &&
                    fieldIt != impl_->classFieldIndicesBySym.end()) {
                    auto idxIt = fieldIt->second.find(attr->attribute);
                    if (idxIt != fieldIt->second.end()) {
                        llvm::Type* fieldType =
                            impl_->classFieldTypesBySym[impl_->classSym(className)][attr->attribute];
                        // Numeric fields: load + op + plain store. Heap str/bytes
                        // fields: concat + RC-overwrite store (below).
                        if (fieldType == impl_->i64Type || fieldType == impl_->f64Type) {
                            attr->object->accept(*this);
                            llvm::Value* objPtr = impl_->lastValue;
                            if (!objPtr->getType()->isPointerTy())
                                objPtr = impl_->builder->CreateIntToPtr(objPtr, impl_->i8PtrType);
                            llvm::Value* gep = impl_->builder->CreateStructGEP(
                                structIt->second, objPtr, idxIt->second,
                                attr->attribute + "_ptr");
                            llvm::Value* cur = impl_->builder->CreateLoad(
                                fieldType, gep, attr->attribute);
                            node.value->accept(*this);
                            llvm::Value* rhs = impl_->lastValue;
                            llvm::Value* res = nullptr;
                            if (fieldType == impl_->i64Type) {
                                if (rhs->getType() == impl_->i1Type)
                                    rhs = impl_->builder->CreateZExt(rhs, impl_->i64Type);
                                if (rhs->getType() == impl_->i64Type)
                                    res = impl_->emitIntAugOp(cur, rhs, node.op.type());
                            } else {  // f64 field
                                if (rhs->getType() == impl_->i1Type)
                                    rhs = impl_->builder->CreateZExt(rhs, impl_->i64Type);
                                if (rhs->getType() == impl_->i64Type)
                                    rhs = impl_->builder->CreateSIToFP(rhs, impl_->f64Type);
                                if (rhs->getType() == impl_->f64Type) {
                                    switch (node.op.type()) {
                                        case TokenType::PLUS_EQUAL:  res = impl_->builder->CreateFAdd(cur, rhs, "augo"); break;
                                        case TokenType::MINUS_EQUAL: res = impl_->builder->CreateFSub(cur, rhs, "augo"); break;
                                        case TokenType::STAR_EQUAL:  res = impl_->builder->CreateFMul(cur, rhs, "augo"); break;
                                        case TokenType::SLASH_EQUAL: res = impl_->builder->CreateFDiv(cur, rhs, "augo"); break;
                                        default: break;
                                    }
                                }
                            }
                            if (res) {
                                impl_->builder->CreateStore(res, gep);
                                return;
                            }
                        } else if (node.op.type() == TokenType::PLUS_EQUAL &&
                                   fieldType->isPointerTy()) {
                            // Bytes fields collapse onto VarKind::List, so exprIsBytes
                            // disambiguates; an actual list field falls through (no-op).
                            bool isBytesField = impl_->exprIsBytes(node.target.get());
                            Impl::VarKind fkind = Impl::VarKind::Other;
                            auto fkIt = impl_->classFieldKindsBySym.find(impl_->classSym(className));
                            if (fkIt != impl_->classFieldKindsBySym.end()) {
                                auto f2 = fkIt->second.find(attr->attribute);
                                if (f2 != fkIt->second.end()) fkind = f2->second;
                            }
                            bool isStrField = (fkind == Impl::VarKind::Str ||
                                               fkind == Impl::VarKind::StrLiteral);
                            if (isStrField || isBytesField) {
                                const char* concatFn = isBytesField
                                    ? "dragon_bytes_concat" : "dragon_str_concat";
                                attr->object->accept(*this);
                                llvm::Value* objPtr = impl_->lastValue;
                                if (!objPtr->getType()->isPointerTy())
                                    objPtr = impl_->builder->CreateIntToPtr(objPtr, impl_->i8PtrType);
                                llvm::Value* gep = impl_->builder->CreateStructGEP(
                                    structIt->second, objPtr, idxIt->second,
                                    attr->attribute + "_ptr");
                                llvm::Value* cur = impl_->builder->CreateLoad(
                                    fieldType, gep, attr->attribute);
                                node.value->accept(*this);
                                llvm::Value* rhs = impl_->lastValue;
                                if (rhs->getType() == impl_->i64Type)
                                    rhs = impl_->builder->CreateIntToPtr(rhs, impl_->i8PtrType);
                                llvm::Value* newVal = impl_->builder->CreateCall(
                                    impl_->runtimeFuncs[concatFn], {cur, rhs}, "augo.cat");
                                Impl::VarKind heapKind = isBytesField
                                    ? Impl::VarKind::List : Impl::VarKind::Str;
                                impl_->storeWithRCOverwrite(
                                    gep, fieldType, newVal, heapKind, heapKind,
                                    /*newIsBorrowed=*/false, attr->attribute);
                                if (impl_->options.gcMode == GCMode::RC &&
                                    impl_->isOwnedStrResult(rhs))
                                    impl_->builder->CreateCall(
                                        impl_->runtimeFuncs[isBytesField
                                            ? "dragon_decref" : "dragon_decref_str"], {rhs});
                                return;
                            }
                        }
                    }
                }
            }
        }
    }
}

void CodeGen::visit(AnnAssignStmt& node) {
    // Reset the one-shot RHS signals: a lambda or bare exception name consumed
    // earlier as a call arg must not leak its kind into this assignment.
    impl_->lastClosureCallableType = nullptr;
    impl_->lastValueIsType = false;

    llvm::Type* varType = impl_->typeExprToLLVM(node.annotation.get());
    auto varKind = impl_->typeExprToKind(node.annotation.get());

    // D025: classes are not values, so binding a class name is rejected.
    // Exempt: exception class names lower to an exception-type token, not a class.
    if (varKind == Impl::VarKind::Type) {
        if (auto* rhsName = dynamic_cast<NameExpr*>(node.value.get())) {
            if (impl_->classNames.count(rhsName->name) &&
                !impl_->isExcType(rhsName->name)) {
                impl_->addError(
                    "classes are not values: cannot bind class '" +
                    rhsName->name + "' to a variable. Construct instances with "
                    "the class name directly (e.g. " + rhsName->name + "(...)).",
                    node.location());
            }
        }
    }

    // A generator-returning call binds a Generator regardless of the surface
    // annotation, else `for x in g` over `g: ptr = gen()` iterates nothing.
    if (auto* callV = dynamic_cast<CallExpr*>(node.value.get()))
        if (auto* cn = dynamic_cast<NameExpr*>(callV->callee.get()))
            if (impl_->generatorFunctions.count(impl_->resolveCalleeSymbol(cn->name)))
                varKind = Impl::VarKind::Generator;

    // deque(): tag VarKind::Deque + "__Deque" so method and len() dispatch reach
    // dragon_deque_*, else the list path reads the deque header as a list and OOMs.
    if (auto* cv = dynamic_cast<CallExpr*>(node.value.get()))
        if (auto* cn = dynamic_cast<NameExpr*>(cv->callee.get()))
            if (cn->name == "deque") {
                varKind = Impl::VarKind::Deque;
                if (auto* tgt = dynamic_cast<NameExpr*>(node.target.get()))
                    impl_->varClassNames[tgt->name] = "__Deque";
            }

    // Track non-negativity for the subscript fast path.
    if (auto* name = dynamic_cast<NameExpr*>(node.target.get())) {
        if (node.value && impl_->isExprDefinitelyNonNeg(node.value.get()))
            impl_->knownNonNeg.insert(name->name);
        else
            impl_->knownNonNeg.erase(name->name);
        // D025: track ptr-typed slots so CallExpr's indirect-call fallback can
        // distinguish a function pointer from a possible class descriptor.
        impl_->trackPtrParam(name->name, node.annotation.get());
    }

    // A dict-access RHS with a specific annotation arms the checked-get tag.
    // D030: derive it from Type::Kind so bytes stays distinct from list/dict.
    if (node.value) {
        int64_t tag = node.annotation
            ? Impl::typeKindToTag(impl_->typeExprToTypeKind(node.annotation.get()))
            : Impl::varKindToTag(varKind);
        if (tag >= 0) {
            bool rhsIsDictAccess = false;
            if (auto* sub = dynamic_cast<SubscriptExpr*>(node.value.get())) {
                if (auto* n = dynamic_cast<NameExpr*>(sub->object.get()))
                    rhsIsDictAccess = impl_->lookupVarKind(n->name) == Impl::VarKind::Dict;
            } else if (auto* attr = dynamic_cast<AttributeExpr*>(node.value.get())) {
                if (auto* n = dynamic_cast<NameExpr*>(attr->object.get()))
                    rhsIsDictAccess = impl_->lookupVarKind(n->name) == Impl::VarKind::Dict;
            }
            if (rhsIsDictAccess) {
                impl_->pendingDictCheckTag = tag;
                // For a list-annotated LHS, ride the representation check
                // along: tag 5 alone can't tell DragonList from DragonListBox.
                impl_->pendingListViewElemTag =
                    impl_->listViewWantElemTag(node.annotation.get());
            }
        }
    }

    // Track list element type for subscript unboxing (D020)
    if (varKind == Impl::VarKind::List) {
        if (auto* generic = dynamic_cast<GenericTypeExpr*>(node.annotation.get())) {
            if (!generic->typeArgs.empty()) {
                // D030: direct Type::Kind from the annotation.
                Type::Kind elemTypeKind = impl_->typeExprToTypeKind(generic->typeArgs[0].get());
                auto elemVarKind = impl_->typeExprToKind(generic->typeArgs[0].get());
                if (auto* name = dynamic_cast<NameExpr*>(node.target.get())) {
                    impl_->varListElemKinds[name->name] = elemTypeKind;
                    // D025: list[type] - iteration yields class descriptors
                    if (elemVarKind == Impl::VarKind::Type)
                        impl_->varListElemIsType.insert(name->name);
                    // list[Callable[[...], R]] - record element FunctionType so
                    // for-loop sites can register callableTypes for the loop var.
                    if (auto* cte = dynamic_cast<CallableTypeExpr*>(generic->typeArgs[0].get())) {
                        impl_->varListElemCallableType[name->name] =
                            impl_->callableTypeExprToFnType(cte);
                    }
                    // list[ClassName]: record the element class so attribute
                    // access on subscript/iteration results typed-loads fields.
                    if (auto* elemNamed = dynamic_cast<NamedTypeExpr*>(generic->typeArgs[0].get())) {
                        std::string cn = impl_->resolveAnnotationClassName(elemNamed->name);
                        if (!cn.empty()) {
                            impl_->varListElemClassName[name->name] = cn;
                        }
                    }
                }
            }
        }
    }
    // D025: dict[K, type] subscript yields class descriptors. D030: key/value
    // Type::Kind tracking routes int-keyed dicts to dragon_dict_int_*.
    if (varKind == Impl::VarKind::Dict) {
        if (auto* generic = dynamic_cast<GenericTypeExpr*>(node.annotation.get())) {
            if (generic->typeArgs.size() == 2) {
                auto valVarKind = impl_->typeExprToKind(generic->typeArgs[1].get());
                Type::Kind keyTypeKind = impl_->typeExprToTypeKind(generic->typeArgs[0].get());
                Type::Kind valTypeKind = impl_->typeExprToTypeKind(generic->typeArgs[1].get());
                if (auto* name = dynamic_cast<NameExpr*>(node.target.get())) {
                    if (valVarKind == Impl::VarKind::Type) {
                        impl_->varDictValueIsType.insert(name->name);
                    }
                    impl_->varDictKeyKinds[name->name] = keyTypeKind;
                    impl_->varDictValueKinds[name->name] = valTypeKind;
                }
            }
        }
    }

    // Check if annotation is a TypedDict - treat as Dict at runtime
    std::string typedDictClassName;
    if (auto* namedType = dynamic_cast<NamedTypeExpr*>(node.annotation.get())) {
        if (impl_->typedDictClassesBySym.count(impl_->classSym(namedType->name))) {
            typedDictClassName = namedType->name;
            varKind = Impl::VarKind::Dict;  // TypedDict is a dict at runtime
            varType = impl_->i8PtrType;     // ptr (DragonDict*)
        }
    }

    // Track the annotation's class name for field/method access; for a
    // niche-optimized `Foo | None` (a bare ptr) pull the class out of the union.
    std::string annotClassName;
    if (auto* namedType = dynamic_cast<NamedTypeExpr*>(node.annotation.get())) {
        // Accepts bare `Foo` or dotted `mod.Foo`; returns the bare class name
        // either way so downstream lookups find the same struct.
        annotClassName = impl_->resolveAnnotationClassName(namedType->name);
    } else if (dynamic_cast<UnionTypeExpr*>(node.annotation.get())) {
        annotClassName = impl_->typeExprUnionClassName(node.annotation.get());
    } else if (dynamic_cast<GenericTypeExpr*>(node.annotation.get())) {
        // D044: `x: Box[int]` is a stamped generic instantiation; track its class
        // so member dispatch works even when the RHS isn't a direct construction.
        annotClassName = impl_->genericInstanceClassName(node.annotation.get());
    }

    // `self.x: T = value` annotated field assignment: without this branch the
    // field silently stays null and later reads crash.
    if (auto* attrTarget = dynamic_cast<AttributeExpr*>(node.target.get())) {
        if (auto* objName = dynamic_cast<NameExpr*>(attrTarget->object.get())) {
            std::string className;
            if (objName->name == "self" && !impl_->currentClassName.empty()) {
                className = impl_->currentClassName;
            } else {
                auto vit = impl_->varClassNames.find(objName->name);
                if (vit != impl_->varClassNames.end()) className = vit->second;
            }
            if (!className.empty()) {
                auto structIt = impl_->classStructTypesBySym.find(impl_->classSym(className));
                auto fieldIt = impl_->classFieldIndicesBySym.find(impl_->classSym(className));
                if (structIt != impl_->classStructTypesBySym.end() &&
                    fieldIt != impl_->classFieldIndicesBySym.end()) {
                    auto idxIt = fieldIt->second.find(attrTarget->attribute);
                    if (idxIt != fieldIt->second.end()) {
                        if (!node.value) return;  // declaration only; nothing to store
                        node.value->accept(*this);
                        llvm::Value* val = impl_->lastValue;
                        attrTarget->object->accept(*this);
                        llvm::Value* objPtr = impl_->lastValue;
                        auto* gep = impl_->builder->CreateStructGEP(
                            structIt->second, objPtr, idxIt->second,
                            attrTarget->attribute + "_ptr");
                        auto* fieldType =
                            impl_->classFieldTypesBySym[impl_->classSym(className)][attrTarget->attribute];
                        if (val->getType() != fieldType) {
                            if (fieldType == impl_->f64Type && val->getType() == impl_->i64Type)
                                val = impl_->builder->CreateSIToFP(val, impl_->f64Type);
                            else if (fieldType == impl_->i64Type && val->getType() == impl_->i1Type)
                                val = impl_->builder->CreateZExt(val, impl_->i64Type);
                            else if (fieldType == impl_->i64Type && val->getType() == impl_->f64Type)
                                val = impl_->builder->CreateFPToSI(val, impl_->i64Type);
                        }
                        Impl::VarKind fieldKind = Impl::VarKind::Other;
                        auto fkIt = impl_->classFieldKindsBySym.find(impl_->classSym(className));
                        if (fkIt != impl_->classFieldKindsBySym.end()) {
                            auto fkIt2 = fkIt->second.find(attrTarget->attribute);
                            if (fkIt2 != fkIt->second.end()) fieldKind = fkIt2->second;
                        }
                        Impl::VarKind newKind = fieldKind;
                        if (auto* sl = dynamic_cast<StringLiteral*>(node.value.get()))
                            newKind = (sl->isBytes ? Impl::VarKind::List : Impl::VarKind::StrLiteral);
                        bool rhsBorrowed = Impl::isBorrowedHeapExpr(node.value.get());
                        impl_->emitFieldSharedBarrier(objPtr, val, fieldKind);
                        impl_->storeWithRCOverwrite(
                            gep, fieldType, val, fieldKind, newKind, rhsBorrowed,
                            className + "." + attrTarget->attribute);
                        // `self._f = own x`: the field adopted the +1; null the
                        // source so scope exit sees nothing.
                        impl_->emitMoveOutIfMarked(node.value.get());
                        return;
                    }
                }
            }
        }
    }

    if (auto* name = dynamic_cast<NameExpr*>(node.target.get())) {
        // A Task annotation binds a vthread handle: tag __Thread so .join() /
        // .is_alive() dispatch reaches the runtime; T is recovered at the call site.
        {
            bool isTaskAnnot = false;
            if (auto* nt = dynamic_cast<NamedTypeExpr*>(node.annotation.get()))
                isTaskAnnot = (nt->name == "Task");
            else if (auto* gt = dynamic_cast<GenericTypeExpr*>(node.annotation.get())) {
                if (auto* gb = dynamic_cast<NamedTypeExpr*>(gt->base.get()))
                    isTaskAnnot = (gb->name == "Task");
            }
            if (isTaskAnnot) {
                impl_->varClassNames[name->name] = "__Thread";
                if (!impl_->scopes.empty() && impl_->detachableTaskDecls.count(&node))
                    impl_->scopes.back().detachOnExit.insert(name->name);
            }

            if (auto* nt = dynamic_cast<NamedTypeExpr*>(node.annotation.get()))
                if (nt->name == "Lock") {
                    impl_->varClassNames[name->name] = "__Lock";
                    bool modLevel = (impl_->currentFunction == impl_->mainFunction) && (impl_->scopes.size() <= impl_->moduleBodyScopeDepth);
                    if (!modLevel && !impl_->scopes.empty())
                        impl_->scopes.back().lockDestroyOnExit.insert(name->name);
                }
        }

        bool isModuleLevel = (impl_->currentFunction == impl_->mainFunction) && (impl_->scopes.size() <= impl_->moduleBodyScopeDepth);

        if (isModuleLevel) {
            std::string gKey = impl_->globalKeyOrOwn(name->name);
            auto* gv = impl_->lookupModuleGlobal(name->name);
            bool firstInitOfForwardGlobal = impl_->entryGlobalsAwaitingInit.erase(gKey) > 0;
            bool hadExistingGlobal = (gv != nullptr) && !firstInitOfForwardGlobal;
            Impl::VarKind oldKind = hadExistingGlobal ? impl_->lookupVarKind(name->name) : Impl::VarKind::Other;
            if (!gv) {
                gv = new llvm::GlobalVariable(
                    *impl_->module, varType, /*isConstant=*/false,
                    llvm::GlobalValue::InternalLinkage,
                    llvm::Constant::getNullValue(varType),
                    "global." + gKey);
                impl_->moduleGlobals[gKey] = gv;
            }
            if (auto* callableAnnot = dynamic_cast<CallableTypeExpr*>(node.annotation.get())) {
                if (auto* rhsCall = dynamic_cast<CallExpr*>(node.value.get())) {
                    if (auto* rhsCallee = dynamic_cast<NameExpr*>(rhsCall->callee.get())) {
                        if (rhsCallee->name == "getattr" ||
                            impl_->funcReturnsClosure.count(impl_->resolveCalleeSymbol(rhsCallee->name))) {
                            impl_->callableTypes[name->name] = impl_->callableTypeExprToFnType(callableAnnot);
                            varKind = Impl::VarKind::Closure;
                        }
                    }
                }
            }
            impl_->moduleGlobalKinds[gKey] = varKind;

            if (varKind == Impl::VarKind::Union) {
                impl_->unionMemberKinds[name->name] = impl_->typeExprToUnionMembers(node.annotation.get());
                std::string ucn = impl_->typeExprUnionClassName(node.annotation.get());
                if (!ucn.empty())
                    impl_->varClassNames[name->name] = ucn;
            }

            if (node.value) {
                node.value->accept(*this);
                llvm::Value* val = impl_->lastValue;
                if (val->getType() != varType) {
                    if (varType == impl_->f64Type && val->getType() == impl_->i64Type)
                        val = impl_->builder->CreateSIToFP(val, impl_->f64Type);
                    else if (varType == impl_->i64Type && val->getType() == impl_->i1Type)
                        val = impl_->builder->CreateZExt(val, impl_->i64Type);
                    else if (varType == impl_->i64Type && val->getType() == impl_->f64Type)
                        val = impl_->builder->CreateFPToSI(val, impl_->i64Type);
                }
                bool rhsBorrowed = Impl::isBorrowedHeapExpr(node.value.get());
                if (varKind == Impl::VarKind::Union && gv->getValueType() == impl_->boxType && val->getType() != impl_->boxType) {
                    auto* newTag = impl_->emitTagForExpr(node.value.get(), *this);
                    if (impl_->options.gcMode == GCMode::RC && rhsBorrowed) {
                        auto* newPayloadI64 = impl_->nativeToPayloadI64(val);
                        impl_->emitUnionIncref(newPayloadI64, newTag);
                    }
                    if (impl_->options.gcMode == GCMode::RC &&
                        oldKind == Impl::VarKind::Union) {
                        auto* oldBox = impl_->builder->CreateLoad( impl_->boxType, gv, "old.box");
                        auto* oldTag = impl_->boxTag(oldBox, "old.tag");
                        auto* oldPayload = impl_->boxPayloadI64(oldBox, "old.payload");
                        impl_->emitUnionDecref(oldPayload, oldTag);
                    }
                    val = impl_->makeBox(newTag, val);
                    impl_->builder->CreateStore(val, gv);
                } else {
                    if (val->getType() == impl_->boxType && gv->getValueType() != impl_->boxType) {
                        if (impl_->options.gcMode == GCMode::RC && !impl_->isOwnedBoxResult(val) && gv->getValueType()->isPointerTy())
                            rhsBorrowed = true;
                        llvm::Value* unboxed = impl_->unboxBoxResultChecked(
                            val, gv->getValueType(), varKind,
                            impl_->listViewWantElemTag(node.annotation.get()),
                            impl_->typeExprToTypeKind(node.annotation.get()));
                        if (unboxed->getType() == impl_->boxType)
                            unboxed = impl_->boxPayloadAsKind(
                                val, Impl::typeKindToVarKind(
                                         gv->getValueType() == impl_->f64Type ? Type::Kind::Float :
                                         gv->getValueType() == impl_->i1Type ? Type::Kind::Bool :
                                         gv->getValueType()->isPointerTy() ? Type::Kind::Str :
                                         Type::Kind::Int));
                        val = unboxed;
                    }
                    impl_->storeWithRCOverwrite(
                        gv, gv->getValueType(), val, oldKind, varKind, rhsBorrowed, name->name);
                }
            }

            if (!typedDictClassName.empty()) {
                impl_->varTypedDictClass[name->name] = typedDictClassName;
            }

            bool globalIsBoxSlot = (gv->getValueType() == impl_->boxType);
            if (!annotClassName.empty()) {
                std::string ownMod = impl_->resolveClassOwningModule(annotClassName);
                impl_->varClassNames[name->name] = annotClassName;
                impl_->varClassOwningModule[name->name] = ownMod;
                impl_->moduleGlobalClassNames[gKey] = {annotClassName, ownMod};
                if (impl_->options.gcMode == GCMode::RC && !globalIsBoxSlot)
                    impl_->moduleGlobalKinds[gKey] = Impl::VarKind::ClassInstance;
            } else if (node.value) {
                if (auto* callVal = dynamic_cast<CallExpr*>(node.value.get())) {
                    if (auto* calleeName = dynamic_cast<NameExpr*>(callVal->callee.get())) {
                        if (impl_->classNames.count(calleeName->name)) {
                            std::string ownMod = impl_->resolveClassOwningModule(calleeName->name);
                            impl_->varClassNames[name->name] = calleeName->name;
                            impl_->varClassOwningModule[name->name] = ownMod;
                            impl_->moduleGlobalClassNames[gKey] = {calleeName->name, ownMod};
                            if (impl_->options.gcMode == GCMode::RC && !globalIsBoxSlot)
                                impl_->moduleGlobalKinds[gKey] = Impl::VarKind::ClassInstance;
                        }
                    }
                }
                if (auto cls = impl_->recordVarClassFromValue(name->name, node.value.get());
                    !cls.empty()) {
                    impl_->moduleGlobalClassNames[gKey] =
                        {cls, impl_->varClassOwningModule[name->name]};
                    // Same box-slot guard as above.
                    if (impl_->options.gcMode == GCMode::RC &&
                        impl_->classNames.count(cls) && !globalIsBoxSlot)
                        impl_->moduleGlobalKinds[gKey] = Impl::VarKind::ClassInstance;
                }
            }

            if (impl_->options.gcMode == GCMode::RC && node.value) {
                Impl::VarKind storedKind = varKind;
                auto mgkIt = impl_->moduleGlobalKinds.find(gKey);
                if (mgkIt != impl_->moduleGlobalKinds.end())
                    storedKind = mgkIt->second;
                if (storedKind == Impl::VarKind::Str && gv->getValueType()->isPointerTy()) {
                    auto* stored = impl_->builder->CreateLoad(gv->getValueType(), gv, name->name + ".shr");
                    if (node.isConst || dynamic_cast<StringLiteral*>(node.value.get()))
                        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_str_make_immortal"], {stored});
                    else
                        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_mark_shared_str"], {stored});
                } else if (storedKind == Impl::VarKind::Union && gv->getValueType() == impl_->boxType) {
                    auto* box = impl_->builder->CreateLoad(impl_->boxType, gv, name->name + ".shrbox");
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_mark_shared_boxed"],
                        {impl_->boxTag(box, name->name + ".shrtag"),
                         impl_->boxPayloadI64(box, name->name + ".shrpay")});
                } else if (Impl::isHeapKind(storedKind) && gv->getValueType()->isPointerTy()) {
                    auto* stored = impl_->builder->CreateLoad( gv->getValueType(), gv, name->name + ".shr");
                    impl_->emitMarkSharedGlobal(stored, storedKind);
                }
            }
        } else {
            if (impl_->cellPromotedLocals.count(name->name)) {
                auto* alloca = impl_->lookupVarInCurrentScope(name->name);
                if (!alloca) {
                    alloca = impl_->createEntryAlloca(impl_->currentFunction, name->name, impl_->i8PtrType);
                }
                impl_->setVar(name->name, alloca, varKind);
                impl_->markCellBacked(name->name);
                if (node.value) {
                    node.value->accept(*this);
                    llvm::Value* rhs = impl_->lastValue;
                    // Shape coercions so the cell stores the right-width value.
                    if (varKind == Impl::VarKind::Bool && rhs->getType() == impl_->i64Type)
                        rhs = impl_->builder->CreateICmpNE(
                            rhs, llvm::ConstantInt::get(impl_->i64Type, 0), "tobool");
                    else if (varKind == Impl::VarKind::Int && rhs->getType() == impl_->i1Type)
                        rhs = impl_->builder->CreateZExt(rhs, impl_->i64Type, "boolext");
                    else if (varKind == Impl::VarKind::Float && rhs->getType() == impl_->i64Type)
                        rhs = impl_->builder->CreateSIToFP(rhs, impl_->f64Type, "i2f");
                    // A borrowed RHS (e.g. a list element) increfs first so the
                    // cell holds its own balanced +1.
                    if (Impl::isBorrowedHeapExpr(node.value.get()))
                        impl_->emitIncrefByKind(rhs, varKind);
                    auto* initI64 = impl_->nativeToCellI64(rhs, varKind);
                    Type::Kind cellTypeKind = node.annotation
                        ? impl_->typeExprToTypeKind(node.annotation.get())
                        : Type::Kind::Unknown;
                    auto* cell = impl_->emitCellAlloc(initI64, varKind, cellTypeKind);
                    impl_->builder->CreateStore(cell, alloca);
                } else {
                    // Declared without initializer - alloc a zero-valued cell.
                    auto* zero = llvm::ConstantInt::get(impl_->i64Type, 0);
                    Type::Kind cellTypeKind = node.annotation
                        ? impl_->typeExprToTypeKind(node.annotation.get())
                        : Type::Kind::Unknown;
                    auto* cell = impl_->emitCellAlloc(zero, varKind, cellTypeKind);
                    impl_->builder->CreateStore(cell, alloca);
                }
                return;
            }

            // A `:`-declaration binds a NEW variable: reuse a slot only in THIS
            // scope, else the shadow overwrites (and can type-pun) the outer x.
            auto* alloca = impl_->lookupVarInCurrentScope(name->name);
            bool hadExistingSlot = (alloca != nullptr);
            Impl::VarKind oldKind = hadExistingSlot
                ? impl_->lookupVarKind(name->name)
                : Impl::VarKind::Other;
            if (!alloca) {
                alloca = impl_->createEntryAlloca(
                    impl_->currentFunction, name->name, varType);
            }
            impl_->setVar(name->name, alloca, varKind);

            // D033: `Callable[...] = getattr(...)` yields a bound DragonClosure;
            // mark Closure, else the thunk is called without env and segfaults.
            if (auto* callableAnnot =
                    dynamic_cast<CallableTypeExpr*>(node.annotation.get())) {
                if (auto* rhsCall = dynamic_cast<CallExpr*>(node.value.get())) {
                    if (auto* rhsCallee = dynamic_cast<NameExpr*>(rhsCall->callee.get())) {
                        if (rhsCallee->name == "getattr" ||
                            impl_->funcReturnsClosure.count(impl_->resolveCalleeSymbol(rhsCallee->name))) {
                            // getattr and closure-returning fns (D027) yield a
                            // closure: mark Closure so the call site appends env.
                            impl_->callableTypes[name->name] =
                                impl_->callableTypeExprToFnType(callableAnnot);
                            impl_->setVar(name->name, alloca, Impl::VarKind::Closure);
                            varKind = Impl::VarKind::Closure;
                        }
                    }
                }
            }

            // D030: a Union local's alloca is box-typed (tag + payload in one
            // struct); unionMemberKinds feeds isinstance narrowing.
            if (varKind == Impl::VarKind::Union) {
                impl_->unionMemberKinds[name->name] =
                    impl_->typeExprToUnionMembers(node.annotation.get());
                // Track the class member of the union (e.g. Foo in `Foo | None`)
                // so narrowing can resolve attribute access to the right class.
                std::string ucn = impl_->typeExprUnionClassName(node.annotation.get());
                if (!ucn.empty())
                    impl_->varClassNames[name->name] = ucn;
            }

            if (node.value) {
                node.value->accept(*this);
                llvm::Value* val = impl_->lastValue;
                llvm::Type* allocType = alloca->getAllocatedType();

                // D039: a box RHS into a concrete native slot unboxes with a
                // runtime tag check, raising TypeError on mismatch.
                bool ownedBoxUnboxed = false;
                llvm::Value* ownedBoxPayload = nullptr;
                llvm::Value* ownedBoxTag = nullptr;
                // A BORROWED box unboxed into an owned HEAP slot (str/list/dict/
                // instance) must take its own reference (see the store below).
                bool borrowedBoxToHeapSlot = false;
                if (val->getType() == impl_->boxType && allocType != impl_->boxType) {
                    int64_t expectedTag = -1;
                    const char* tagName = "value";
                    if (allocType == impl_->i64Type) {
                        expectedTag = 0; tagName = "int";
                    } else if (allocType == impl_->f64Type) {
                        expectedTag = 2; tagName = "float";
                    } else if (allocType == impl_->i1Type) {
                        expectedTag = 3; tagName = "bool";
                    } else if (allocType == impl_->i8PtrType &&
                               impl_->typeExprToTypeKind(node.annotation.get())
                                   == Type::Kind::Bytes) {
                        // Bytes collapses onto VarKind::List (D030 §5); only the
                        // annotation identifies it, else the check misreports.
                        expectedTag = TAG_BYTES; tagName = "bytes";
                    } else if (allocType == impl_->i8PtrType) {
                        // Pointer LHS: expected tag from the slot's VarKind.
                        if (varKind == Impl::VarKind::Str ||
                            varKind == Impl::VarKind::StrLiteral) {
                            expectedTag = 1; tagName = "str";
                        } else if (varKind == Impl::VarKind::List) {
                            expectedTag = 5; tagName = "list";
                        } else if (varKind == Impl::VarKind::Dict) {
                            expectedTag = 6; tagName = "dict";
                        } else if (varKind == Impl::VarKind::ClassInstance) {
                            expectedTag = 7; tagName = "class";
                        }
                    }
                    if (expectedTag >= 0) {
                        auto* func = impl_->currentFunction;
                        auto* tagV = impl_->boxTag(val, "ub.tag");
                        auto* expectedV = llvm::ConstantInt::get(
                            impl_->i64Type, expectedTag);
                        auto* match = impl_->builder->CreateICmpEQ(
                            tagV, expectedV, "ub.match");
                        auto* okBB = llvm::BasicBlock::Create(
                            *impl_->context, "ub.ok", func);
                        auto* failBB = llvm::BasicBlock::Create(
                            *impl_->context, "ub.fail", func);
                        impl_->builder->CreateCondBr(match, okBB, failBB);

                        // Fail path: raise TypeError. The message is a global
                        // literal; the exception machinery borrows it.
                        impl_->builder->SetInsertPoint(failBB);
                        std::string msg = std::string("TypeError: expected ")
                                          + tagName + " but got value with "
                                          "different runtime type";
                        auto* msgStr = impl_->builder->CreateGlobalString(msg);
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_raise_exc_cstr"],
                            {llvm::ConstantInt::get(impl_->i64Type, 80),
                             msgStr});
                        impl_->builder->CreateUnreachable();

                        // OK path: extract payload as the target native type.
                        impl_->builder->SetInsertPoint(okBB);
                        auto* payloadI64 = impl_->boxPayloadI64(val, "ub.payload");
                        // Capture the OWNED box's +1 before `val` is reassigned to
                        // the bare payload; released after the store below.
                        bool boxIsOwned = impl_->options.gcMode == GCMode::RC &&
                                          impl_->isOwnedBoxResult(val);
                        if (boxIsOwned) {
                            ownedBoxUnboxed = true;
                            ownedBoxPayload = payloadI64;
                            ownedBoxTag = tagV;
                        }
                        // A borrowed box into an owned heap slot must take its own ref,
                        // else double-free; isBorrowedHeapExpr misses ternary sources.
                        if (impl_->options.gcMode == GCMode::RC && !boxIsOwned &&
                            allocType == impl_->i8PtrType)
                            borrowedBoxToHeapSlot = true;
                        if (allocType == impl_->i64Type) {
                            val = payloadI64;
                        } else if (allocType == impl_->f64Type) {
                            val = impl_->builder->CreateBitCast(
                                payloadI64, impl_->f64Type, "ub.f64");
                        } else if (allocType == impl_->i1Type) {
                            val = impl_->builder->CreateICmpNE(
                                payloadI64,
                                llvm::ConstantInt::get(impl_->i64Type, 0),
                                "ub.bool");
                        } else if (allocType == impl_->i8PtrType) {
                            val = impl_->builder->CreateIntToPtr(
                                payloadI64, impl_->i8PtrType, "ub.ptr");
                            // A list-tagged payload must also match the element
                            // representation: the wrong stride corrupts silently.
                            if (expectedTag == 5) {
                                int64_t want = impl_->listViewWantElemTag(
                                    node.annotation.get());
                                if (want != Impl::kNoListElemCheck)
                                    impl_->builder->CreateCall(
                                        impl_->runtimeFuncs
                                            ["dragon_list_view_check"],
                                        {val, llvm::ConstantInt::get(
                                                  impl_->i64Type, want)});
                            }
                        }
                    }
                }

                if (val->getType() != allocType) {
                    if (allocType == impl_->f64Type && val->getType() == impl_->i64Type)
                        val = impl_->builder->CreateSIToFP(val, impl_->f64Type);
                    else if (allocType == impl_->i64Type && val->getType() == impl_->i1Type)
                        val = impl_->builder->CreateZExt(val, impl_->i64Type);
                    else if (allocType == impl_->i64Type && val->getType() == impl_->f64Type)
                        val = impl_->builder->CreateFPToSI(val, impl_->i64Type);
                    // An i64 expression (e.g. startswith) into a bool alloca needs
                    // explicit i1 coercion, else the store silently corrupts.
                    else if (allocType == impl_->i1Type && val->getType() == impl_->i64Type)
                        val = impl_->builder->CreateICmpNE(
                            val, llvm::ConstantInt::get(impl_->i64Type, 0), "tobool");
                    else if (allocType == impl_->i1Type && val->getType() == impl_->f64Type)
                        val = impl_->builder->CreateFCmpONE(
                            val, llvm::ConstantFP::get(impl_->f64Type, 0.0), "tobool");
                }
                // D030: Union local - box tag + native val; RC increfs the new
                // payload and decrefs the old box's payload, both tag-gated.
                bool rhsBorrowed = Impl::isBorrowedHeapExpr(node.value.get()) ||
                                   borrowedBoxToHeapSlot;
                if (varKind == Impl::VarKind::Union) {
                    // D039: a box RHS forwards directly - never compute a fresh
                    // tag for it or wrap a box inside a box.
                    if (val->getType() == impl_->boxType) {
                        if (impl_->options.gcMode == GCMode::RC) {
                            // Decref old box's payload (only if reassignment)
                            if (oldKind == Impl::VarKind::Union) {
                                auto* oldBox = impl_->builder->CreateLoad(
                                    impl_->boxType, alloca, "old.box");
                                auto* oldTag = impl_->boxTag(oldBox, "old.tag");
                                auto* oldPayload = impl_->boxPayloadI64(oldBox, "old.payload");
                                impl_->emitUnionDecref(oldPayload, oldTag);
                            }
                            // A borrowed box (the dict still owns the +1) increfs for
                            // the slot's own ref; an owned temporary's +1 is adopted.
                            auto* newTag = impl_->boxTag(val, "new.tag");
                            auto* newPayload = impl_->boxPayloadI64(val, "new.payload");
                            if (!impl_->isOwnedBoxResult(val))
                                impl_->emitUnionIncref(newPayload, newTag);
                        }
                        impl_->builder->CreateStore(val, alloca);
                        // Register/refresh the unwind cleanup snapshot (union local
                        // - carries the box tag for conditional decref on unwind).
                        if (impl_->options.gcMode == GCMode::RC) {
                            auto* clTag = impl_->boxTag(val, "cl.tag");
                            auto* clPayload = impl_->boxPayloadI64(val, "cl.payload");
                            if (oldKind == Impl::VarKind::Union)
                                impl_->emitCleanupUpdate(name->name, clPayload, clTag);
                            else
                                impl_->emitCleanupPush(name->name, clPayload,
                                                       Impl::DCLEAN_UNION, clTag);
                        }
                    } else {
                        auto* newTag = impl_->emitTagForExpr(node.value.get(), *this);
                        if (impl_->options.gcMode == GCMode::RC) {
                            // Decref old box's payload (only if reassignment)
                            if (oldKind == Impl::VarKind::Union) {
                                auto* oldBox = impl_->builder->CreateLoad(
                                    impl_->boxType, alloca, "old.box");
                                auto* oldTag = impl_->boxTag(oldBox, "old.tag");
                                auto* oldPayload = impl_->boxPayloadI64(oldBox, "old.payload");
                                impl_->emitUnionDecref(oldPayload, oldTag);
                            }
                            // Incref new heap payload if RHS is borrowed
                            if (rhsBorrowed) {
                                auto* newPayloadI64 = impl_->nativeToPayloadI64(val);
                                impl_->emitUnionIncref(newPayloadI64, newTag);
                            }
                        }
                        llvm::Value* boxVal = impl_->makeBox(newTag, val);
                        impl_->builder->CreateStore(boxVal, alloca);
                        // Register/refresh the unwind cleanup snapshot (union local).
                        if (impl_->options.gcMode == GCMode::RC) {
                            auto* clPayload = impl_->boxPayloadI64(boxVal, "cl.payload");
                            if (oldKind == Impl::VarKind::Union)
                                impl_->emitCleanupUpdate(name->name, clPayload, newTag);
                            else
                                impl_->emitCleanupPush(name->name, clPayload,
                                                       Impl::DCLEAN_UNION, newTag);
                        }
                    }
                } else {
                    impl_->storeWithRCOverwrite(
                        alloca, allocType, val, oldKind, varKind, rhsBorrowed, name->name);
                }
                // Drop the owned box's +1 only when the store increfed its own ref:
                // adopted +1s double-free (A/B-proven UAF, test_box_arithmetic).
                if (ownedBoxUnboxed && rhsBorrowed)
                    impl_->emitUnionDecref(ownedBoxPayload, ownedBoxTag);

                // D027: closure from capturing lambda (reassignment)
                if (impl_->lastClosureCallableType) {
                    impl_->callableTypes[name->name] = impl_->lastClosureCallableType;
                    impl_->setVar(name->name, alloca, Impl::VarKind::Closure);
                    impl_->lastClosureCallableType = nullptr;
                }
                // Track callable types for first-class function support
                else if (auto* lambdaFn = llvm::dyn_cast<llvm::Function>(val)) {
                    impl_->callableTypes[name->name] = lambdaFn->getFunctionType();
                } else if (auto* rhsNameE = dynamic_cast<NameExpr*>(node.value.get())) {
                    auto* refFunc = impl_->module->getFunction(rhsNameE->name);
                    if (refFunc)
                        impl_->callableTypes[name->name] = refFunc->getFunctionType();
                    auto ctIt = impl_->callableTypes.find(rhsNameE->name);
                    if (ctIt != impl_->callableTypes.end())
                        impl_->callableTypes[name->name] = ctIt->second;
                }

                if (!typedDictClassName.empty()) {
                    impl_->varTypedDictClass[name->name] = typedDictClassName;
                }

                // A concrete annotation is authoritative: the RHS class guesses must
                // not override it, else a stale entry decrefs a str as ClassInstance.
                bool targetMayBeClass =
                    varKind == Impl::VarKind::ClassInstance ||
                    varKind == Impl::VarKind::Union ||
                    varKind == Impl::VarKind::Other;
                if (!annotClassName.empty()) {
                    impl_->varClassNames[name->name] = annotClassName;
                    // varKind is already ClassInstance via typeExprToKind.
                } else if (targetMayBeClass) {
                    if (auto* callVal = dynamic_cast<CallExpr*>(node.value.get())) {
                        if (auto* calleeName = dynamic_cast<NameExpr*>(callVal->callee.get())) {
                            if (impl_->classNames.count(calleeName->name)) {
                                impl_->varClassNames[name->name] = calleeName->name;
                                // ClassInstance kind for scope-exit decref, never on a
                                // box slot: an Any box stays Union, else cleanup decrefs the tag word.
                                if (impl_->options.gcMode == GCMode::RC &&
                                    alloca->getAllocatedType() != impl_->boxType)
                                    impl_->setVar(name->name, alloca, Impl::VarKind::ClassInstance);
                            }
                        }
                    }
                }
                // Gated on targetMayBeClass for the same reason as above.
                if (targetMayBeClass) {
                    if (auto cls = impl_->recordVarClassFromValue(name->name, node.value.get());
                        !cls.empty()) {
                        // Same box-slot guard as above.
                        if (impl_->options.gcMode == GCMode::RC &&
                            impl_->classNames.count(cls) &&
                            alloca->getAllocatedType() != impl_->boxType)
                            impl_->setVar(name->name, alloca, Impl::VarKind::ClassInstance);
                    }
                }
            } else {
                impl_->storeWithRCOverwrite(
                    alloca, alloca->getAllocatedType(),
                    llvm::Constant::getNullValue(alloca->getAllocatedType()),
                    oldKind, varKind, /*newIsBorrowed=*/false, name->name);
                if (!annotClassName.empty()) {
                    impl_->varClassNames[name->name] = annotClassName;
                    // varKind is already ClassInstance via typeExprToKind.
                }
            }
        }
    }

    // A stack-constructed RHS marks the local so scope cleanup skips its decref;
    // cleared unconditionally so the flag never leaks into a later statement.
    if (impl_->lastWasStackInstance) {
        impl_->lastWasStackInstance = false;
        if (auto* nm = dynamic_cast<NameExpr*>(node.target.get()))
            impl_->markStackAllocated(nm->name);
    }
}

} // namespace dragon
