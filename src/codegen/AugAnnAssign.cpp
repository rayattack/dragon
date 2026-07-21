/// Dragon CodeGen - Augmented + Annotated Assignment (AugAssign, AnnAssign)
/// Split from Assign.cpp (file-size policy): pure code motion.
#include "../CodeGenImpl.h"

namespace dragon {

void CodeGen::visit(AugAssignStmt& node) {
    if (auto* name = dynamic_cast<NameExpr*>(node.target.get())) {
        // 6.12(B): augmented assignment is `x = x OP rhs`. The result is
        // non-neg iff x was non-neg AND the op preserves non-negativity AND
        // the rhs is non-neg. We reuse the BinaryExpr non-neg classifier by
        // synthesizing the same shape - but it's cheaper to just check the
        // op + rhs here and require x to already be non-neg.
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
        // Check local scope first, then module globals
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
        // D027.1: a `nonlocal` (cell-backed) target keeps its value in a heap
        // cell; the alloca holds the cell pointer, not the value. Read and write
        // through the cell ops so `n += 1` updates the shared backing slot
        // instead of doing arithmetic on the raw pointer. storeBack() funnels
        // every write below (numeric, str, bytes, dunder) through the correct
        // path - cell write vs the normal RC-overwrite store.
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

        // String += (concatenation). Route through the amortized in-place
        // append so a `s += x` accumulator loop is O(n), not O(n²).
        // The helper handles the plain store (the entry point owns the old
        // value's decref), the owned-rhs cleanup, and the Str VarKind update.
        if ((varKind == Impl::VarKind::Str || varKind == Impl::VarKind::StrLiteral)
            && node.op.type() == TokenType::PLUS_EQUAL) {
            if (isCell) {
                // A cell-backed str has no stable slot to append in place;
                // concat and write back through the cell - the same shape as
                // `nonlocal s; s = s + x`, which carries the correct RC.
                llvm::Value* cat = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_str_concat"], {current, rhs}, "strcat");
                storeBack(cat, Impl::VarKind::Str, /*newIsBorrowed=*/false);
            } else {
                impl_->emitStrAppendInplace(storeTarget, current, rhs, name->name);
            }
            return;
        }
        // String *= int (repetition), mirroring `"ab" * 3` in the BinaryExpr
        // path. Without this, str *= int fell through to the numeric branch
        // below and emitted CreateMul(ptr, i64) - invalid LLVM IR. The result
        // is a freshly allocated dynamic string, so the slot becomes Str.
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
                    impl_->moduleGlobalKinds[name->name] = Impl::VarKind::Str;
                }
            }
            return;
        }
        // D030 §5: bytes-typed `+=` dispatch via static Type::Kind. The slot's
        // VarKind has collapsed into the generic-heap cohort (e.g. List),
        // so VarKind alone can no longer disambiguate `bytes += bytes` from
        // `list += list`. The TypeChecker-propagated target type is the
        // source of truth.
        bool targetIsBytes = node.target && node.target->type &&
                             node.target->type->kind() == Type::Kind::Bytes;
        if (targetIsBytes && node.op.type() == TokenType::PLUS_EQUAL) {
            llvm::Value* result = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_concat"], {current, rhs}, "bytescat");
            storeBack(result, varKind, /*newIsBorrowed=*/false);
            // bytes_concat copies both operands; an owned rhs temp (call
            // result, literal, nested concat) is otherwise orphaned - the
            // same drain the BinaryExpr bytes path carries. isOwnedPtrResult
            // screens out borrowed names/fields; storeBack already released
            // the old slot value, so only rhs needs draining here.
            if (impl_->options.gcMode == GCMode::RC && impl_->isOwnedPtrResult(rhs))
                impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {rhs});
            return;
        }

        // List `+=` is Python's in-place extend (`a.extend(b)`): it mutates the
        // SAME object and rebinds nothing, so we call dragon_list_extend on the
        // loaded pointer and do NOT store back. Without this a list slot fell
        // through to the numeric switch below and emitted `add ptr` (invalid
        // IR - an LLVM verifier crash). Like bytes, the
        // TypeChecker-propagated target type disambiguates list from bytes.
        bool targetIsList = node.target && node.target->type &&
                            node.target->type->kind() == Type::Kind::List;
        if (targetIsList && node.op.type() == TokenType::PLUS_EQUAL) {
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_extend"], {current, rhs});
            // A literal/comprehension rhs is an owned temporary: extend copies
            // its elements (increfing each) but not the container, so decref the
            // leftover container. A bare variable is borrowed - leave it.
            if (impl_->options.gcMode == GCMode::RC &&
                (dynamic_cast<ListExpr*>(node.value.get()) ||
                 dynamic_cast<ListCompExpr*>(node.value.get()))) {
                impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {rhs});
            }
            return;
        }

        // D039 Phase 11: box arithmetic for `x OP= y` where x (the loaded slot)
        // and/or y is an Any/Union box. Route through dragon_box_binop (the same
        // path as a box BinaryExpr), then unbox the result into the slot's
        // native type with a runtime TypeError check (Phase-7a semantics) - so
        // `total: int; total += anyVal` works and promotes/raises like Python.
        // An Any slot (loadType is the box type) stores the result box directly.
        {
            int64_t opcode = impl_->binopOpcodeForToken(node.op.type());
            bool eitherBox = (current->getType() == impl_->boxType ||
                              rhs->getType() == impl_->boxType);
            if (opcode >= 0 && eitherBox) {
                llvm::Value* resultBox = impl_->emitBoxBinop(
                    *this, node.target.get(), current, node.value.get(), rhs,
                    opcode);
                llvm::Value* stored = impl_->unboxBoxResultChecked(
                    resultBox, loadType, varKind);
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
        // `d[key] OP= value` / `lst[i] OP= value`. AugAssign once handled only
        // NameExpr targets, so subscript aug-assign silently no-op'd (a wrong-
        // answer bug). Now covered: str- AND int-keyed dicts with int values
        // (str-keyed int uses the fused single-probe dragon_dict_str_iaug_i64),
        // float values (get + emitFloatAugOp + set, incl. //= %=), and str/bytes
        // values (concat-assign via get_*_ptr + str/bytes_concat + set_*_ptr);
        // plus list elements of int/bool/float/str/bytes. KeyError-if-absent is
        // preserved by the runtime get (Python reads d[k] before the op).
        Type::Kind vk = impl_->resolveDictValueKind(sub->object.get());
        bool intKeyed = impl_->dictKeyIsInt(sub->object.get());
        bool strKeyed = !intKeyed;
        // STR-keyed dict, INT value: fused single-probe read-modify-write
        // (dragon_dict_str_iaug_i64 does the op in one hash+probe).
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
        // INT-keyed dict, INT value: get + int op + set (no fused iaug yet for
        // int keys; the get/set are still a single hash+probe each). Int keys
        // are not heap, so no key decref.
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
        // Dict with STR/BYTES value: concat-assign `d[k] += s` (only `+=` is
        // meaningful for str/bytes). get_*_ptr borrows the current value; concat
        // allocates a fresh +1; set_*_ptr decrefs the old value and stores the
        // new one (ownership transfers). Works for str- and int-keyed dicts.
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
        // List element aug-assign: `lst[i] OP= value`. Single-eval of list +
        // index, then typed get + op + typed set. dragon_list_get/set handle
        // negative indices + bounds, and applying the SAME idx to both targets
        // the same element. Numeric element types only (int/bool via i64,
        // float via f64); str/bytes-element lists via concat above the list block.
        bool isList = false;
        Type::Kind elemKind = Type::Kind::Int;
        if (auto* objName = dynamic_cast<NameExpr*>(sub->object.get())) {
            if (impl_->lookupVarKind(objName->name) == Impl::VarKind::List) {
                isList = true;
                auto it = impl_->varListElemKinds.find(objName->name);
                if (it != impl_->varListElemKinds.end()) elemKind = it->second;
            }
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
            // str/bytes element concat-assign: `lst[i] += s`. get_ptr borrows the
            // element; concat allocates a fresh +1; set_ptr decrefs the old element
            // and stores the new one (ownership transfers). Only `+=` is meaningful.
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
        // `obj.field OP= value` (e.g. `self.count += 1`). Was a silent no-op
        // (AugAssign handled only Name + Subscript targets). Now covered:
        // numeric instance fields (load + op + store), numeric STATIC fields
        // (ClassName.field via staticFieldGlobals), and heap str/bytes instance
        // fields (concat + storeWithRCOverwrite). Still no-op (todo.md): @property
        // fields (need getter/setter dispatch) and heap-typed static fields.
        auto* objName = dynamic_cast<NameExpr*>(attr->object.get());
        {
            // Static class field: `ClassName.field OP= value`. Stored as a module
            // global in staticFieldGlobals (keyed by class name). Numeric only here;
            // heap static fields are rare and fall through (todo).
            auto sfIt = objName ? impl_->staticFieldGlobals.find(objName->name)
                                : impl_->staticFieldGlobals.end();
            if (sfIt != impl_->staticFieldGlobals.end()) {
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
                // static class, exactly like the plain-assign path. Without
                // this the compound store silently no-ops.
                className = impl_->resolveExprClassName(attr->object.get());
            }
            if (!className.empty()) {
                auto structIt = impl_->classStructTypes.find(className);
                auto fieldIt = impl_->classFieldIndices.find(className);
                if (structIt != impl_->classStructTypes.end() &&
                    fieldIt != impl_->classFieldIndices.end()) {
                    auto idxIt = fieldIt->second.find(attr->attribute);
                    if (idxIt != fieldIt->second.end()) {
                        llvm::Type* fieldType =
                            impl_->classFieldTypes[className][attr->attribute];
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
                            // Heap str/bytes field concat: `self.buf += more`.
                            // str fields carry VarKind::Str/StrLiteral; bytes fields
                            // collapse onto VarKind::List, so disambiguate via the
                            // static type (exprIsBytes). An actual list field has no
                            // simple concat-assign here and falls through (no-op).
                            bool isBytesField = impl_->exprIsBytes(node.target.get());
                            Impl::VarKind fkind = Impl::VarKind::Other;
                            auto fkIt = impl_->classFieldKinds.find(className);
                            if (fkIt != impl_->classFieldKinds.end()) {
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
    // Reset the one-shot closure-type flag at entry (see visit(AssignStmt)):
    // a lambda consumed earlier as a call arg must not leak its closure type
    // into this annotated assignment.
    impl_->lastClosureCallableType = nullptr;

    // Annotated assignment: x: int = 42, or p: Point = Point(3, 4)
    llvm::Type* varType = impl_->typeExprToLLVM(node.annotation.get());
    auto varKind = impl_->typeExprToKind(node.annotation.get());

    // ADR 025: classes are not values. Binding a class name to a variable
    // (`X: type = SomeClass`) is rejected - Dragon has no first-class class
    // values and no compile-time class aliases. Construct with the class name
    // directly. (Exception class names are exempt: as a `: type` value they
    // lower to an integer exception-type token for assertRaises/except
    // matching, not a class value.)
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

    // A generator-returning call binds a Generator regardless of the
    // annotation's surface type. There is no spellable generator type, so the
    // annotation is the lowering type (a `ptr`); without re-tagging here,
    // `g: ptr = gen()` lost its generator-ness and `for x in g` iterated
    // nothing. Mirrors the generator detection in visit(AssignStmt).
    if (auto* callV = dynamic_cast<CallExpr*>(node.value.get()))
        if (auto* cn = dynamic_cast<NameExpr*>(callV->callee.get()))
            if (impl_->generatorFunctions.count(impl_->resolveCalleeSymbol(cn->name)))
                varKind = Impl::VarKind::Generator;

    // `d: deque[int] = deque()` - the deque annotation resolves to a list-like
    // type, but the value is a real DragonDeque. Tag the binding VarKind::Deque
    // and varClassNames "__Deque" so append/appendleft/pop/popleft dispatch
    // (CallMethods) and len() (CallBuiltins) reach the dragon_deque_* runtime
    // instead of misrouting through the list path (which reads the deque header
    // as a list and OOMs). Mirrors the bare `d = deque()` hooks in AssignStmt.
    if (auto* cv = dynamic_cast<CallExpr*>(node.value.get()))
        if (auto* cn = dynamic_cast<NameExpr*>(cv->callee.get()))
            if (cn->name == "deque") {
                varKind = Impl::VarKind::Deque;
                if (auto* tgt = dynamic_cast<NameExpr*>(node.target.get()))
                    impl_->varClassNames[tgt->name] = "__Deque";
            }

    // 6.12(B): track non-negativity for subsequent subscript fast path.
    if (auto* name = dynamic_cast<NameExpr*>(node.target.get())) {
        if (node.value && impl_->isExprDefinitelyNonNeg(node.value.get()))
            impl_->knownNonNeg.insert(name->name);
        else
            impl_->knownNonNeg.erase(name->name);
        // D025: track ptr-typed locals/globals so the indirect-call fallback
        // in CallExpr can distinguish a function pointer from an unannotated
        // value that may be a class descriptor.
        impl_->trackPtrParam(name->name, node.annotation.get());
    }

    // Runtime checked dict access: if RHS is a dict subscript or dict dot-access
    // and the annotation gives a specific type, set pending tag for checked get.
    // D030 §5: tag derivation from the source annotation's Type::Kind keeps
    // bytes-typed slots distinguishable from list/dict/etc even after the
    // VarKind::Bytes layer collapses into the generic-heap cohort.
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

    // Track list element type for subscript unboxing (D020 Bugs 2/4)
    if (varKind == Impl::VarKind::List) {
        if (auto* generic = dynamic_cast<GenericTypeExpr*>(node.annotation.get())) {
            if (!generic->typeArgs.empty()) {
                // D030 §5: direct Type::Kind from the annotation.
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
                    // `members: list[TarInfo] = ...` - also record the
                    // element class name so subscript / iteration on the
                    // local resolves attribute access on the elements
                    // (`members[0].data` needs to know it's a TarInfo to
                    // typed-load the bytes field). Mirrors the same
                    // tracking that fires for class-body and ctor-body
                    // list[ClassName] annotations.
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
    // D025: track dict[K, type] so subscript value is a class descriptor
    // Track dict[K, V] value Type::Kind for items() unpack.
    // D030 Phase 3.G: track dict[K, V] key Type::Kind so subscript / `in` /
    // print dispatch route int-keyed dicts to the dragon_dict_int_* family.
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
        if (impl_->typedDictClasses.count(namedType->name)) {
            typedDictClassName = namedType->name;
            varKind = Impl::VarKind::Dict;  // TypedDict is a dict at runtime
            varType = impl_->i8PtrType;     // ptr (DragonDict*)
        }
    }

    // Check if annotation is a class name - track for field/method access.
    // Also handles niche-optimized `Foo | None` (which lowers to a bare ptr
    // and reads as VarKind::ClassInstance) - pull the class name out of the
    // union so attribute access on `r.field` finds the right struct.
    std::string annotClassName;
    if (auto* namedType = dynamic_cast<NamedTypeExpr*>(node.annotation.get())) {
        // resolveAnnotationClassName accepts both bare `Foo` and dotted
        // `mod.Foo`, returning the bare class name in either case so
        // downstream attribute/method lookups find the same struct.
        annotClassName = impl_->resolveAnnotationClassName(namedType->name);
    } else if (dynamic_cast<UnionTypeExpr*>(node.annotation.get())) {
        annotClassName = impl_->typeExprUnionClassName(node.annotation.get());
    } else if (dynamic_cast<GenericTypeExpr*>(node.annotation.get())) {
        // D044 - `x: Box[int]` annotates a stamped generic instantiation. Track
        // its class so `x.method()` / `x.field` dispatch to `Box[int]`'s struct,
        // even when the RHS isn't a direct construction (e.g. a method return).
        annotClassName = impl_->genericInstanceClassName(node.annotation.get());
    }

    // `self.x: T = value` (and `instance.x: T = value`) - annotated field
    // assignment. Without this branch the AnnAssignStmt visitor only handled
    // NameExpr targets, so class fields declared with `self.x: T = v` in
    // constructors silently lost the value (the field stayed null), causing
    // crashes when later code touched the field. Mirrors AssignStmt's
    // attribute-target branch so behavior matches across forms.
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
                auto structIt = impl_->classStructTypes.find(className);
                auto fieldIt = impl_->classFieldIndices.find(className);
                if (structIt != impl_->classStructTypes.end() &&
                    fieldIt != impl_->classFieldIndices.end()) {
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
                            impl_->classFieldTypes[className][attrTarget->attribute];
                        if (val->getType() != fieldType) {
                            if (fieldType == impl_->f64Type && val->getType() == impl_->i64Type)
                                val = impl_->builder->CreateSIToFP(val, impl_->f64Type);
                            else if (fieldType == impl_->i64Type && val->getType() == impl_->i1Type)
                                val = impl_->builder->CreateZExt(val, impl_->i64Type);
                            else if (fieldType == impl_->i64Type && val->getType() == impl_->f64Type)
                                val = impl_->builder->CreateFPToSI(val, impl_->i64Type);
                        }
                        Impl::VarKind fieldKind = Impl::VarKind::Other;
                        auto fkIt = impl_->classFieldKinds.find(className);
                        if (fkIt != impl_->classFieldKinds.end()) {
                            auto fkIt2 = fkIt->second.find(attrTarget->attribute);
                            if (fkIt2 != fkIt->second.end()) fieldKind = fkIt2->second;
                        }
                        Impl::VarKind newKind = fieldKind;
                        if (auto* sl = dynamic_cast<StringLiteral*>(node.value.get()))
                            newKind = (sl->isBytes ? Impl::VarKind::List : Impl::VarKind::StrLiteral);
                        bool rhsBorrowed = Impl::isBorrowedHeapExpr(node.value.get());
                        // Field write barrier - see the Assign.cpp twin
                        impl_->emitFieldSharedBarrier(objPtr, val, fieldKind);
                        impl_->storeWithRCOverwrite(
                            gep, fieldType, val, fieldKind, newKind, rhsBorrowed,
                            className + "." + attrTarget->attribute);
                        // `self._f = own x`: the field adopted the +1
                        // (rhsBorrowed=false via isBorrowedHeapExpr's move
                        // exception); null the source so scope exit sees
                        // nothing (docs/002 2.4 row 3).
                        impl_->emitMoveOutIfMarked(node.value.get());
                        return;
                    }
                }
            }
        }
    }

    if (auto* name = dynamic_cast<NameExpr*>(node.target.get())) {
        // `t: Task[T] = fire f()` (or bare `t: Task = ...`): the value is a
        // vthread handle (ptr). Tag as __Thread so .join()/.is_alive() dispatch
        // (CallMethods.cpp) reaches the runtime, mirroring the legacy
        // `t = fire ...` path. The result type T is recovered at the call site
        // from the AST node type (D030 native-typed result coercion).
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
                // Task-detach tail: a bound fire-and-forget Task that provably
                // never escapes or joins leaks its handle ref. The escape pre-pass
                // (computeStackAllocSites) recorded such decls; arm a scope-exit
                // dragon_vthread_detach for this one. setVar below puts the alloca
                // in the same (current) scope, so detachOnExit aligns with it.
                if (!impl_->scopes.empty() && impl_->detachableTaskDecls.count(&node))
                    impl_->scopes.back().detachOnExit.insert(name->name);
            }

            // `lock: Lock = Lock()` - tag __Lock so acquire/release/try_lock
            // dispatch (CallMethods.cpp) and with-statement lowering reach the
            // dragon_lock_* runtime, mirroring the bare `lock = Lock()` path.
            if (auto* nt = dynamic_cast<NamedTypeExpr*>(node.annotation.get()))
                if (nt->name == "Lock") {
                    impl_->varClassNames[name->name] = "__Lock";
                    // docs/002 2.10: the scope owns a bare Lock LOCAL - arm
                    // the null-gated destroy at scope exit. Module-level
                    // Locks live for the process (not armed).
                    bool modLevel =
                        (impl_->currentFunction == impl_->mainFunction) &&
                        (impl_->scopes.size() <= impl_->moduleBodyScopeDepth);
                    if (!modLevel && !impl_->scopes.empty())
                        impl_->scopes.back().lockDestroyOnExit.insert(
                            name->name);
                }
        }

        // Module-level declaration: create LLVM GlobalVariable only (no local alloca).
        // This way all access (from main and from functions) goes through the global.
        // Only a declaration AT the module body's top level is a global; one nested
        // in a top-level block scope is block-local (D: block scoping).
        bool isModuleLevel = (impl_->currentFunction == impl_->mainFunction) &&
                             (impl_->scopes.size() <= impl_->moduleBodyScopeDepth);

        if (isModuleLevel) {
            // Reuse existing forward-declared global (from dependency modules)
            // or create a new GlobalVariable for module-level var
            auto* gv = impl_->lookupModuleGlobal(name->name);
            // A global we forward-declared for entry-module method resolution
            // but never initialized is, semantically, a fresh definition here -
            // its only prior "value" is the null initializer, so it must not be
            // treated as an RC overwrite (no decref of old). Erase on first init
            // so later reassignments take the normal overwrite path.
            bool firstInitOfForwardGlobal =
                impl_->entryGlobalsAwaitingInit.erase(name->name) > 0;
            bool hadExistingGlobal = (gv != nullptr) && !firstInitOfForwardGlobal;
            Impl::VarKind oldKind = hadExistingGlobal
                ? impl_->lookupVarKind(name->name)
                : Impl::VarKind::Other;
            if (!gv) {
                gv = new llvm::GlobalVariable(
                    *impl_->module, varType, /*isConstant=*/false,
                    llvm::GlobalValue::InternalLinkage,
                    llvm::Constant::getNullValue(varType),
                    "global." + name->name);
                impl_->moduleGlobals[name->name] = gv;
            }
            // D033 Phase 3: Callable[...] = getattr(obj, name) at module
            // scope. Mirror the local-scope path: register the callable type
            // and switch kind to Closure so the call site appends env.
            if (auto* callableAnnot =
                    dynamic_cast<CallableTypeExpr*>(node.annotation.get())) {
                if (auto* rhsCall = dynamic_cast<CallExpr*>(node.value.get())) {
                    if (auto* rhsCallee = dynamic_cast<NameExpr*>(rhsCall->callee.get())) {
                        if (rhsCallee->name == "getattr" ||
                            impl_->funcReturnsClosure.count(rhsCallee->name)) {
                            // getattr(obj, m) returns a bound DragonClosure; a
                            // call to a closure-returning function (D027) returns
                            // a capturing closure. Either way, mark the var
                            // Closure so the call site extracts fn+env instead
                            // of executing the env object as code.
                            impl_->callableTypes[name->name] =
                                impl_->callableTypeExprToFnType(callableAnnot);
                            varKind = Impl::VarKind::Closure;
                        }
                    }
                }
            }
            impl_->moduleGlobalKinds[name->name] = varKind;

            // Union module global: track member kinds + class member so
            // isinstance narrowing - including the else-branch complement
            // (computeElseKind) - works identically to the local-scope case.
            // Mirrors the local Union path further below.
            if (varKind == Impl::VarKind::Union) {
                impl_->unionMemberKinds[name->name] =
                    impl_->typeExprToUnionMembers(node.annotation.get());
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
                // Any / Union module global: the global slot is %dragon.box
                // (16 bytes: {i64 tag, i64 payload}). A bare ptr/i64 store
                // would only write 8 bytes, leaving the tag field at zero
                // (= TAG_INT) and corrupting later reads. Wrap the value in a
                // box with the right tag before storing, mirroring the
                // function-local Union path above (lines 1964-2008).
                if (varKind == Impl::VarKind::Union &&
                    gv->getValueType() == impl_->boxType &&
                    val->getType() != impl_->boxType) {
                    auto* newTag = impl_->emitTagForExpr(node.value.get(), *this);
                    if (impl_->options.gcMode == GCMode::RC && rhsBorrowed) {
                        auto* newPayloadI64 = impl_->nativeToPayloadI64(val);
                        impl_->emitUnionIncref(newPayloadI64, newTag);
                    }
                    // Decref old box's payload on reassignment.
                    if (impl_->options.gcMode == GCMode::RC &&
                        oldKind == Impl::VarKind::Union) {
                        auto* oldBox = impl_->builder->CreateLoad(
                            impl_->boxType, gv, "old.box");
                        auto* oldTag = impl_->boxTag(oldBox, "old.tag");
                        auto* oldPayload = impl_->boxPayloadI64(oldBox, "old.payload");
                        impl_->emitUnionDecref(oldPayload, oldTag);
                    }
                    val = impl_->makeBox(newTag, val);
                    impl_->builder->CreateStore(val, gv);
                } else {
                    // D039: box -> native unboxing for a typed MODULE-GLOBAL slot
                    // - the mirror of the local-scope path (~line 1265). When the
                    // RHS is a {tag, payload} box (e.g. narrowing an `Any` global
                    // `const s: str = anyGlobal`) but the global slot is a native
                    // type (ptr/i64/f64/i1), extract the payload at the slot's
                    // type. Without this, a 16-byte box was stored into the 8-byte
                    // global slot: the load read back the TAG field as the value
                    // (a bogus pointer / int) AND overran the adjacent global, so
                    // any USE of the narrowed global segfaulted. Block-local and
                    // function-local narrows already went through the local path;
                    // only a bare module-top-level narrow hit this gap.
                    //
                    // The extraction is CHECKED (unboxBoxResultChecked): the
                    // old bare boxPayloadAsKind trusted the annotation, so a
                    // module-scope `s: str = xs[0]` over a mistyped element
                    // increfed a garbage payload (SEGV in dragon_incref_str)
                    // instead of raising TypeError like the local path. The
                    // list representation check rides along via
                    // listViewWantElemTag (DragonList vs DragonListBox).
                    if (val->getType() == impl_->boxType &&
                        gv->getValueType() != impl_->boxType) {
                        // A BORROWED box unboxed into an owned heap-pointer global
                        // must take its own reference (mirror of the local Phase 7a
                        // fix): else the global adopts a value the source container
                        // still owns and dangles when that container is freed.
                        // isBorrowedHeapExpr(expr) is false for a ternary source, so
                        // key the incref off the box's real ownership instead.
                        if (impl_->options.gcMode == GCMode::RC &&
                            !impl_->isOwnedBoxResult(val) &&
                            gv->getValueType()->isPointerTy())
                            rhsBorrowed = true;
                        llvm::Value* unboxed = impl_->unboxBoxResultChecked(
                            val, gv->getValueType(), varKind,
                            impl_->listViewWantElemTag(node.annotation.get()));
                        // Kinds with no tag mapping (tuple/set/...) come back
                        // as the box - extract at the slot's shape unchecked,
                        // matching the previous behavior for those kinds.
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
            // GV is already zero-initialized by its initializer (no explicit store needed for default)

            // Track TypedDict class name
            if (!typedDictClassName.empty()) {
                impl_->varTypedDictClass[name->name] = typedDictClassName;
            }

            // Track class name for instances. The ClassInstance kind override
            // is gated off BOX slots throughout: an `Any`-annotated global
            // stores a 16-byte {tag, payload} box, and its kind must stay
            // Union so the D018 marking hook below (and any RC handling)
            // extracts the payload instead of misreading the tag word as a
            // pointer - the same misclassification that SEGV'd Any-boxed
            // LOCALS at scope exit.
            bool globalIsBoxSlot = (gv->getValueType() == impl_->boxType);
            if (!annotClassName.empty()) {
                impl_->varClassNames[name->name] = annotClassName;
                impl_->varClassOwningModule[name->name] =
                    impl_->resolveClassOwningModule(annotClassName);
                // GC Phase 3: mark module global as ClassInstance
                if (impl_->options.gcMode == GCMode::RC && !globalIsBoxSlot)
                    impl_->moduleGlobalKinds[name->name] = Impl::VarKind::ClassInstance;
            } else if (node.value) {
                if (auto* callVal = dynamic_cast<CallExpr*>(node.value.get())) {
                    if (auto* calleeName = dynamic_cast<NameExpr*>(callVal->callee.get())) {
                        if (impl_->classNames.count(calleeName->name)) {
                            impl_->varClassNames[name->name] = calleeName->name;
                            impl_->varClassOwningModule[name->name] =
                                impl_->resolveClassOwningModule(calleeName->name);
                            // GC Phase 3: mark module global as ClassInstance
                            if (impl_->options.gcMode == GCMode::RC && !globalIsBoxSlot)
                                impl_->moduleGlobalKinds[name->name] = Impl::VarKind::ClassInstance;
                        }
                    }
                }
                // Fallback: detect class instances from complex expressions
                if (!impl_->varClassNames.count(name->name)) {
                    auto cls = impl_->resolveExprClassName(node.value.get());
                    if (!cls.empty()) {
                        impl_->varClassNames[name->name] = cls;
                        std::string owningMod;
                        if (auto* call = dynamic_cast<CallExpr*>(node.value.get())) {
                            if (auto* attrCallee = dynamic_cast<AttributeExpr*>(call->callee.get())) {
                                if (attrCallee->object && attrCallee->object->type &&
                                    attrCallee->object->type->kind() == Type::Kind::Module) {
                                    owningMod = static_cast<ModuleType&>(*attrCallee->object->type).name;
                                } else if (auto* recvName = dynamic_cast<NameExpr*>(attrCallee->object.get())) {
                                    auto rmIt = impl_->varClassOwningModule.find(recvName->name);
                                    if (rmIt != impl_->varClassOwningModule.end()) owningMod = rmIt->second;
                                }
                            }
                        }
                        if (owningMod.empty())
                            owningMod = impl_->resolveClassOwningModule(cls);
                        impl_->varClassOwningModule[name->name] = owningMod;
                        // Same box-slot guard as above.
                        if (impl_->options.gcMode == GCMode::RC &&
                            impl_->classNames.count(cls) && !globalIsBoxSlot)
                            impl_->moduleGlobalKinds[name->name] = Impl::VarKind::ClassInstance;
                    }
                }
            }

            // A module global is read from every vthread by name, it never
            // crosses a `fire` boundary, so the fire-site mark never sees it
            // i.e. (emitAtomicIncref). Dragon's default refcount is
            // nay atomic so two handler vthreads on different OS workers doing
            // plain incref/decref on the same unmarked global tear the count,
            // the object is freed early, and another worker reads freed
            // memory (heap use-after-free / `unaligned tcache chunk`).
            // Runs after the ClassInstance kind fix-ups above so instances
            // route to their per-class deep walker.
            //   - const str: promote to IMMORTAL - zero RC traffic forever.
            //     Sound because `const` reassignment is a compile error and
            //     str is an immutable leaf, so the value is program-lifetime
            //     anyway. Generalizes the earlier literal-RHS-only promotion
            //     (a literal RHS stays immortal-promoted even without const:
            //     a literal never needs freeing on reassignment).
            //   - non-const str: mark SHARED (immortalizing a reassignable
            //     slot would leak every replaced value).
            //   - containers/instances: mark SHARED deep, never immortal -
            //     the SHARED store barriers in list/dict then propagate the
            //     flag to values inserted later, which immortality would not.
            //   - Union box slot: tag-dispatched mark of the payload.
            if (impl_->options.gcMode == GCMode::RC && node.value) {
                Impl::VarKind storedKind = varKind;
                auto mgkIt = impl_->moduleGlobalKinds.find(name->name);
                if (mgkIt != impl_->moduleGlobalKinds.end())
                    storedKind = mgkIt->second;
                if (storedKind == Impl::VarKind::Str &&
                    gv->getValueType()->isPointerTy()) {
                    auto* stored = impl_->builder->CreateLoad(
                        gv->getValueType(), gv, name->name + ".shr");
                    if (node.isConst ||
                        dynamic_cast<StringLiteral*>(node.value.get()))
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_str_make_immortal"],
                            {stored});
                    else
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_mark_shared_str"],
                            {stored});
                } else if (storedKind == Impl::VarKind::Union &&
                           gv->getValueType() == impl_->boxType) {
                    auto* box = impl_->builder->CreateLoad(
                        impl_->boxType, gv, name->name + ".shrbox");
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_mark_shared_boxed"],
                        {impl_->boxTag(box, name->name + ".shrtag"),
                         impl_->boxPayloadI64(box, name->name + ".shrpay")});
                } else if (Impl::isHeapKind(storedKind) &&
                           gv->getValueType()->isPointerTy()) {
                    auto* stored = impl_->builder->CreateLoad(
                        gv->getValueType(), gv, name->name + ".shr");
                    impl_->emitMarkSharedGlobal(stored, storedKind);
                }
            }
        } else {
            // D027.1: cell-promoted local - `nonlocal s` in a nested fn means
            // `s` here in the owning function lives in a heap-allocated cell.
            // The alloca is i8*-typed (cell pointer); the initial value goes
            // into the cell and every read/write routes through cell ops.
            // This bypasses storeWithRCOverwrite and the union/typed-dict
            // machinery - those don't apply to a cell-boxed scalar/heap slot.
            if (impl_->cellPromotedLocals.count(name->name)) {
                // Same shadow-vs-alias rule as the plain-local path below:
                // reuse the cell-pointer slot only within the current scope. A
                // `:`-declaration that shadows a cell-promoted name from an
                // ENCLOSING scope gets its own fresh cell, leaving the outer
                // (captured) cell intact - otherwise the shadow would overwrite
                // the enclosing binding's cell pointer.
                auto* alloca = impl_->lookupVarInCurrentScope(name->name);
                if (!alloca) {
                    alloca = impl_->createEntryAlloca(
                        impl_->currentFunction, name->name, impl_->i8PtrType);
                }
                impl_->setVar(name->name, alloca, varKind);
                impl_->markCellBacked(name->name);
                if (node.value) {
                    node.value->accept(*this);
                    llvm::Value* rhs = impl_->lastValue;
                    // Mirror the small coercions used elsewhere so a Bool cell
                    // initialised from `expr >= 0` (i1) or a Float cell from an
                    // int literal store the right-shape value.
                    if (varKind == Impl::VarKind::Bool && rhs->getType() == impl_->i64Type)
                        rhs = impl_->builder->CreateICmpNE(
                            rhs, llvm::ConstantInt::get(impl_->i64Type, 0), "tobool");
                    else if (varKind == Impl::VarKind::Int && rhs->getType() == impl_->i1Type)
                        rhs = impl_->builder->CreateZExt(rhs, impl_->i64Type, "boolext");
                    else if (varKind == Impl::VarKind::Float && rhs->getType() == impl_->i64Type)
                        rhs = impl_->builder->CreateSIToFP(rhs, impl_->f64Type, "i2f");
                    // Take ownership of the RHS into the cell. If the RHS was
                    // borrowed from another owner (e.g. a list element), incref
                    // first so the cell holds a balanced +1.
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

            // A `:`-declaration binds a NEW variable in the current
            // (innermost) scope. Reuse a slot only if the name already lives in
            // THIS scope; a binding that resolves only in an ENCLOSING scope
            // must be shadowed with a fresh alloca, never aliased - otherwise
            // `x: str = ...` inside a nested block would overwrite (and, on a
            // type change, type-pun) the outer `x`. Bare `x = ...` (AssignStmt)
            // is the separate path that mutates the enclosing binding.
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

            // D033 Phase 3: `Callable[...] = getattr(obj, name)` - the runtime
            // returns a bound DragonClosure; treat the var as a closure so the
            // call site (CallExpr.cpp VarKind::Closure branch) appends env and
            // unpacks self. Without this, the bare-fn-pointer path calls the
            // thunk without an env arg and the thunk segfaults loading self.
            // Other Callable producers (raw fn pointers, free functions) keep
            // their existing kind; we only switch when we can tell the RHS
            // really is a closure.
            if (auto* callableAnnot =
                    dynamic_cast<CallableTypeExpr*>(node.annotation.get())) {
                if (auto* rhsCall = dynamic_cast<CallExpr*>(node.value.get())) {
                    if (auto* rhsCallee = dynamic_cast<NameExpr*>(rhsCall->callee.get())) {
                        if (rhsCallee->name == "getattr" ||
                            impl_->funcReturnsClosure.count(rhsCallee->name)) {
                            // getattr(obj, m) returns a bound DragonClosure; a
                            // call to a closure-returning function (D027) returns
                            // a capturing closure. Mark the var Closure so the
                            // call site appends env and unpacks correctly,
                            // instead of executing the env object as code (the
                            // escaping-named-def SIGSEGV).
                            impl_->callableTypes[name->name] =
                                impl_->callableTypeExprToFnType(callableAnnot);
                            impl_->setVar(name->name, alloca, Impl::VarKind::Closure);
                            varKind = Impl::VarKind::Closure;
                        }
                    }
                }
            }

            // D030 Phase 4: Union local. The alloca is box-typed
            // (typeExprToLLVM returns boxType for UnionTypeExpr) - tag and
            // payload live in one struct value. unionMemberKinds is still
            // tracked so isinstance narrowing knows the static membership.
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

                // D039 Phase 7a: box -> concrete unbox with runtime TypeError.
                // When the RHS evaluates to a box (Any local, list[Any]
                // subscript, dict[str, Any] subscript without checkTag, etc.)
                // and the LHS is a concrete native type, emit:
                //  1. Compare box.tag to expected tag for allocType
                //  2. On match: extract payload, cast to allocType, fall
                //  through to the existing coercion + store path.
                //  3. On mismatch: dragon_raise_exc(80, "TypeError: ...")
                //
                // pendingDictCheckTag-style dispatch (Phase 2) handles the
                // dict[str, Any] case at the subscript-read site. This branch
                // catches every other box-source path uniformly.
                //
                // An OWNED box result (dragon_box_subscript / box_binop on an
                // `Any` value) unboxed into a native slot below carries a +1 on
                // its payload that the store neither adopts nor releases (it
                // increfs for the slot's own ref, or - Other-kind - ignores),
                // orphaning the box's +1 (one leaked payload per
                // `x: str = anyVal[k]`). Capture it here and release after the
                // store. Gated on isOwnedBoxResult: a BORROWED element read
                // (dict_get_box / list_box_get, e.g. `const s: str = typedDict[k]`
                // over a parsed dict[str,Any]) carries no +1 -> skip (double-free).
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
                    } else if (allocType == impl_->i8PtrType) {
                        // Pointer LHS: typeKindToTag from the source annotation
                        // when available; default to TAG_STR for str-typed.
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

                        // Fail path: raise TypeError. The message string is a
                        // global literal - the exception machinery stores it
                        // by borrowed pointer.
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
                        // A BORROWED box (dragon_dict_get_box / list_box_get, or a
                        // ternary/PHI whose arms include one) unboxed into an owned
                        // HEAP slot (str/list/dict/instance) yields a borrowed
                        // payload: the container still holds the +1. The slot must
                        // take its OWN reference, else scope cleanup decrefs a value
                        // the container still owns - a double-free that surfaces when
                        // the container is destroyed (dragon_dict_destroy reads the
                        // freed string header). isBorrowedHeapExpr(expr) below is
                        // false for a ternary source, so key the incref off the box's
                        // real ownership instead. Scalar slots (int/float/bool) copy
                        // the payload and need no ref - gate on the pointer slot.
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
                            // A list-tagged payload must also match the
                            // target's element REPRESENTATION (DragonList vs
                            // DragonListBox, elem_tag) - the box tag alone
                            // cannot tell them apart and reading one at the
                            // other's stride corrupts silently.
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
                    // Storing an i64-returning expression
                    // (e.g. `name.startswith("-")` returns i64) into a `bool`
                    // alloca needs explicit i1 coercion. Without this, the store
                    // is type-mismatched and LLVM either errors or - worse -
                    // silently produces wrong output (bool fields in argparse's
                    // parse_args break exactly this way).
                    else if (allocType == impl_->i1Type && val->getType() == impl_->i64Type)
                        val = impl_->builder->CreateICmpNE(
                            val, llvm::ConstantInt::get(impl_->i64Type, 0), "tobool");
                    else if (allocType == impl_->i1Type && val->getType() == impl_->f64Type)
                        val = impl_->builder->CreateFCmpONE(
                            val, llvm::ConstantFP::get(impl_->f64Type, 0.0), "tobool");
                }
                // D030 Phase 4: Union local - build box from tag + native val.
                // RC: incref new heap payload (conditional on tag), decref old
                // box's payload (conditional on its tag) by loading the old
                // box and extracting tag. Then store the new box.
                bool rhsBorrowed = Impl::isBorrowedHeapExpr(node.value.get()) ||
                                   borrowedBoxToHeapSlot;
                if (varKind == Impl::VarKind::Union) {
                    // D039 Phase 2: if RHS is already a box (e.g. dict[str, Any]
                    // read via dragon_dict_get_box), forward it directly -
                    // don't compute a tag for the box-value-as-expression and
                    // don't wrap a box inside a box. Same forwarding pattern
                    // CallExpr.cpp uses for box-typed params.
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
                            // Incref new payload using the box's own tag.
                            // Borrow-return contract from dict_get_box (Phase 2):
                            // the dict still owns the +1, so a longer-lived
                            // alloca must incref to gain its own reference. An
                            // OWNED box temporary (box_binop / box_subscript)
                            // already carries that +1 - take ownership of it
                            // instead of double-counting (else it leaks).
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
                // Release the OWNED box temporary's +1 (captured above), but ONLY
                // when the store treated the RHS as borrowed (rhsBorrowed) and so
                // took its OWN independent ref (incref) - then the box's +1 is
                // surplus. This is exactly the box_subscript-on-Any case: the
                // container still owns the payload, box_subscript increfed a second
                // ref, and the slot increfed a third; drop the box's. A box_binop
                // result (`s: str = a + "y"`) is a FRESH payload with no other
                // owner and rhsBorrowed=false, so the store ADOPTS the box's +1 -
                // releasing it there double-freed (A/B-proven UAF in
                // test_box_arithmetic). Tag-gated: no-op for a non-heap payload.
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

                // Track TypedDict class name
                if (!typedDictClassName.empty()) {
                    impl_->varTypedDictClass[name->name] = typedDictClassName;
                }

                // An EXPLICIT concrete annotation is authoritative: `x: str`,
                // `x: int`, `x: list[T]`, ... is that type, full stop. The
                // RHS-class-detection fallbacks below (which guess a class
                // from the value expression) must NOT override it. Skipping
                // them also stops a stale, program-wide `varClassNames` entry
                // for the RHS variable name from bleeding a bogus class onto a
                // differently-typed target: `const name: str = r` where `r` is
                // an Any/Union loop var whose name collides with some class
                // instance elsewhere resolved to `<Class>`, and `name` was
                // stamped ClassInstance -> generic dragon_decref on a string at
                // scope exit (heap corruption). Only Union / ClassInstance /
                // Other / Unknown targets can legitimately acquire a class here.
                bool targetMayBeClass =
                    varKind == Impl::VarKind::ClassInstance ||
                    varKind == Impl::VarKind::Union ||
                    varKind == Impl::VarKind::Other;
                if (!annotClassName.empty()) {
                    impl_->varClassNames[name->name] = annotClassName;
                    // GC Phase 3: varKind already set to ClassInstance via typeExprToKind
                } else if (targetMayBeClass) {
                    if (auto* callVal = dynamic_cast<CallExpr*>(node.value.get())) {
                        if (auto* calleeName = dynamic_cast<NameExpr*>(callVal->callee.get())) {
                            if (impl_->classNames.count(calleeName->name)) {
                                impl_->varClassNames[name->name] = calleeName->name;
                                // GC Phase 3: set ClassInstance kind for scope-exit
                                // decref - but NEVER on a box slot. `p: Any =
                                // Dog(...)` allocates a 16-byte {tag, payload} box;
                                // its kind must stay Union so cleanup extracts the
                                // payload. Overriding to ClassInstance made cleanup
                                // load the first 8 bytes (the TAG, 7) as a pointer
                                // and dragon_decref(7) SEGV'd at scope exit.
                                if (impl_->options.gcMode == GCMode::RC &&
                                    alloca->getAllocatedType() != impl_->boxType)
                                    impl_->setVar(name->name, alloca, Impl::VarKind::ClassInstance);
                            }
                        }
                    }
                }
                // Fallback: detect class instances from complex expressions.
                // Gated on targetMayBeClass for the same reason as above.
                if (targetMayBeClass && !impl_->varClassNames.count(name->name)) {
                    auto cls = impl_->resolveExprClassName(node.value.get());
                    if (!cls.empty()) {
                        impl_->varClassNames[name->name] = cls;
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
                    // GC Phase 3: varKind already set to ClassInstance via typeExprToKind
                }
            }
        }
    }

    // B Phase 1: if the RHS lowered to a stack construction (CallExpr fork set
    // lastWasStackInstance), mark the bound local so scope cleanup skips its
    // decref - the storage is reclaimed when the frame unwinds. Cleared
    // unconditionally so the transient flag never leaks into a later statement.
    if (impl_->lastWasStackInstance) {
        impl_->lastWasStackInstance = false;
        if (auto* nm = dynamic_cast<NameExpr*>(node.target.get()))
            impl_->markStackAllocated(nm->name);
    }
}

} // namespace dragon
