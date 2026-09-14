#include "../CodeGenImpl.h"

namespace dragon {

namespace {

bool isPtrElemKind(Type::Kind k) {
    switch (k) {
        case Type::Kind::Str:
        case Type::Kind::Bytes:
        case Type::Kind::ByteArray:
        case Type::Kind::List:
        case Type::Kind::Dict:
        case Type::Kind::Tuple:
        case Type::Kind::Set:
        case Type::Kind::Instance:
        case Type::Kind::Function:
            return true;
        default:
            return false;
    }
}

bool isPrimElemKind(Type::Kind k) {
    switch (k) {
        case Type::Kind::Int:
        case Type::Kind::Float:
        case Type::Kind::Bool:
            return true;
        default:
            return false;
    }
}

}

void CodeGen::visit(AssignStmt& node) {
    impl_->setStatementDebugLoc(node);
    if (node.targets.empty()) return;

    impl_->lastClosureCallableType = nullptr;

    if (tryEmitStrSelfAppend(node)) return;

    impl_->lastValueIsType = false;
    impl_->lastClosureCallableType = nullptr;
    node.value->accept(*this);
    llvm::Value* val = impl_->lastValue;

    bool firstTargetDone = false;
    bool rhsNonNeg = impl_->isExprDefinitelyNonNeg(node.value.get());
    for (auto& target : node.targets) {
        if (auto* name = dynamic_cast<NameExpr*>(target.get())) {
            if (rhsNonNeg) impl_->knownNonNeg.insert(name->name);
            else           impl_->knownNonNeg.erase(name->name);
        }
        if (firstTargetDone && impl_->options.gcMode == GCMode::RC &&
            val->getType()->isPointerTy()) {
            auto tag = impl_->inferPtrValueTag(node.value.get());
            if (tag == TAG_STR) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_incref_str"], {val});
            } else if (tag == 5 || tag == 6 || tag == 7) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_incref"], {val});
            }
        }
        firstTargetDone = true;

        if (auto* sub = dynamic_cast<SubscriptExpr*>(target.get())) {
            if (tryEmitSetitemOverloadStore(*sub, val)) continue;
            if (sub->object->type &&
                sub->object->type->kind() == Type::Kind::ByteArray) {
                sub->object->accept(*this);
                llvm::Value* ba = impl_->lastValue;
                sub->index->accept(*this);
                llvm::Value* idx = impl_->lastValue;
                if (idx->getType() == impl_->i1Type)
                    idx = impl_->builder->CreateZExt(idx, impl_->i64Type);
                llvm::Value* byteVal = val;
                if (byteVal->getType() == impl_->i1Type)
                    byteVal = impl_->builder->CreateZExt(byteVal, impl_->i64Type);
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_bytearray_set"],
                    {impl_->toI8Ptr(ba), idx, byteVal});
                continue;
            }
            if (tryEmitDictSubscriptStore(*sub, node, val)) continue;
            tryEmitListSubscriptStore(*sub, node, val);
            continue;
        }
        if (auto* attr = dynamic_cast<AttributeExpr*>(target.get())) {
            if (impl_->isDragonFile && tryEmitDictAttrStore(*attr, node, val))
                continue;
            if (tryEmitStaticFieldStore(*attr, node, val)) continue;
            if (tryEmitPropertySetterStore(*attr, node, val)) continue;
            tryEmitInstanceFieldStore(*attr, node, val);
            continue;
        }
        if (auto* tupleTarget = dynamic_cast<TupleExpr*>(target.get())) {
            emitTupleUnpackAssign(*tupleTarget, node, val);
            continue;
        }
        if (auto* name = dynamic_cast<NameExpr*>(target.get())) {
            emitNameAssign(*name, node, val);
        }
    }
}

bool CodeGen::tryEmitStrSelfAppend(AssignStmt& node) {
    if (node.targets.size() != 1) return false;
    auto* tName = dynamic_cast<NameExpr*>(node.targets[0].get());
    if (!tName) return false;
    auto* bin = dynamic_cast<BinaryExpr*>(node.value.get());
    if (!bin || bin->op.type() != TokenType::PLUS) return false;
    auto* binLeft = dynamic_cast<NameExpr*>(bin->left.get());
    if (!binLeft || binLeft->name != tName->name) return false;
    if (impl_->isCellBacked(tName->name)) return false;
    auto vk = impl_->lookupVarKind(tName->name);
    if (vk != Impl::VarKind::Str && vk != Impl::VarKind::StrLiteral) return false;
    auto slot = impl_->resolveNameSlot(tName->name);
    if (!slot.ptr || slot.type != impl_->i8PtrType) return false;

    llvm::Value* cur = impl_->builder->CreateLoad(slot.type, slot.ptr, tName->name);
    bin->right->accept(*this);
    impl_->emitStrAppendInplace(slot.ptr, cur, impl_->lastValue, tName->name);
    return true;
}

bool CodeGen::tryEmitSetitemOverloadStore(SubscriptExpr& sub, llvm::Value* val) {
    std::string setClassName = impl_->resolveExprClassName(sub.object.get());
    if (setClassName.empty() || !impl_->hasDunder(setClassName, "__setitem__"))
        return false;
    sub.object->accept(*this);
    llvm::Value* obj = impl_->lastValue;
    sub.index->accept(*this);
    llvm::Value* idx = impl_->lastValue;
    impl_->callDunder(setClassName, "__setitem__", obj, {idx, val});
    return true;
}

bool CodeGen::tryEmitDictSubscriptStore(SubscriptExpr& sub, AssignStmt& node,
                                        llvm::Value* val) {
    bool isDict = false;
    if (auto* objName = dynamic_cast<NameExpr*>(sub.object.get())) {
        isDict = impl_->lookupVarKind(objName->name) == Impl::VarKind::Dict;
    } else if (auto* objAttr = dynamic_cast<AttributeExpr*>(sub.object.get())) {
        std::string cls = impl_->resolveAttrTargetClass(objAttr);
        if (!cls.empty() &&
            impl_->lookupFieldKind(cls, objAttr->attribute) == Impl::VarKind::Dict)
            isDict = true;
    }
    if (!isDict && sub.object->type &&
        sub.object->type->kind() == Type::Kind::Dict)
        isDict = true;
    if (!isDict) return false;

    Type::Kind dictKk = impl_->resolveDictKeyKind(sub.object.get());
    bool intKeyed = dictKk == Type::Kind::Int || dictKk == Type::Kind::Float;

    sub.object->accept(*this);
    llvm::Value* dict = impl_->lastValue;
    sub.index->accept(*this);
    llvm::Value* key = impl_->lastValue;

    if (intKeyed) {
        if (dictKk == Type::Kind::Float)
            key = impl_->emitFloatDictKeyBits(key);
        if (key->getType() == impl_->i1Type)
            key = impl_->builder->CreateZExt(key, impl_->i64Type);
        else if (key->getType()->isPointerTy())
            key = impl_->builder->CreatePtrToInt(key, impl_->i64Type);
        else if (key->getType() != impl_->i64Type)
            key = impl_->builder->CreateZExtOrTrunc(key, impl_->i64Type);

        if (val->getType() == impl_->f64Type) {
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_int_set_f64"], {dict, key, val});
            return true;
        }
        if (val->getType()->isPointerTy()) {
            int64_t tag = impl_->inferPtrValueTag(node.value.get());
            llvm::Value* pval = val;
            if (tag == 1) pval = impl_->ensureHeapString(pval, node.value.get());
            if (impl_->options.gcMode == GCMode::RC &&
                (tag == 1 || tag == 5 || tag == 6 || tag == 7) &&
                Impl::isBorrowedHeapExpr(node.value.get())) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs[tag == 1 ? "dragon_incref_str"
                                                 : "dragon_incref"], {pval});
            }
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_int_set_ptr"],
                {dict, key, pval, llvm::ConstantInt::get(impl_->i64Type, tag)});
            return true;
        }
        auto prim = impl_->taggedPrimStore(val);
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_dict_int_set_tagged"],
            {dict, key, prim.first, prim.second});
        return true;
    }

    if (impl_->options.gcMode == GCMode::RC && key &&
        key->getType()->isPointerTy()) {
        Expr* keyExpr = sub.index.get();
        if (impl_->dictKeyUsesObjEngine(sub.object.get())) {
            impl_->emitRetainDictObjKey(key, keyExpr);
        } else {
            bool keyIsLiteral =
                dynamic_cast<StringLiteral*>(keyExpr) ||
                (dynamic_cast<NameExpr*>(keyExpr) &&
                 impl_->lookupVarKind(
                     static_cast<NameExpr*>(keyExpr)->name)
                     == Impl::VarKind::StrLiteral);
            if (keyIsLiteral) {
                key = impl_->ensureHeapString(key, keyExpr);
            } else if (Impl::isBorrowedHeapExpr(keyExpr)) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_incref_str"], {key});
            }
        }
    }

    if (val->getType() == impl_->boxType) {
        auto* tagV = impl_->boxTag(val, "set.tag");
        auto* payloadV = impl_->boxPayloadI64(val, "set.payload");
        if (impl_->options.gcMode == GCMode::RC &&
            !impl_->isOwnedBoxResult(val))
            impl_->emitUnionIncref(payloadV, tagV);
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_dict_set_tagged"],
            {dict, key, payloadV, tagV});
        return true;
    }
    if (val->getType() == impl_->f64Type) {
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_dict_set_str_f64"], {dict, key, val});
        return true;
    }
    if (val->getType()->isPointerTy()) {
        int64_t tag = impl_->inferPtrValueTag(node.value.get());
        llvm::Value* pval = val;
        if (tag == TAG_STR)
            pval = impl_->ensureHeapString(pval, node.value.get());
        impl_->increfBorrowedContainerValue(pval, node.value.get(), tag);
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_dict_set_str_ptr"],
            {dict, key, pval, llvm::ConstantInt::get(impl_->i64Type, tag)});
        return true;
    }
    auto prim = impl_->taggedPrimStore(val);
    impl_->builder->CreateCall(
        impl_->runtimeFuncs["dragon_dict_set_tagged"],
        {dict, key, prim.first, prim.second});
    return true;
}

bool CodeGen::tryEmitListSubscriptStore(SubscriptExpr& sub, AssignStmt& node,
                                        llvm::Value* val) {
    bool isList = false;
    if (auto* objName = dynamic_cast<NameExpr*>(sub.object.get())) {
        isList = impl_->lookupVarKind(objName->name) == Impl::VarKind::List;
    } else if (auto* objAttr = dynamic_cast<AttributeExpr*>(sub.object.get())) {
        std::string cls = impl_->resolveAttrTargetClass(objAttr);
        if (!cls.empty() &&
            impl_->lookupFieldKind(cls, objAttr->attribute) == Impl::VarKind::List)
            isList = true;
    }
    if (!isList && sub.object->type &&
        sub.object->type->kind() == Type::Kind::List)
        isList = true;
    if (!isList) return false;

    sub.object->accept(*this);
    llvm::Value* list = impl_->lastValue;
    sub.index->accept(*this);
    llvm::Value* idx = impl_->lastValue;
    if (idx->getType() == impl_->i1Type) {
        idx = impl_->builder->CreateZExt(idx, impl_->i64Type);
    }

    // Element kind: varListElemKinds for a bare var, AST type for a
    // class field - else a list[instance] field store skips RC (UAF).
    Type::Kind setElemKind = Type::Kind::Int;
    if (auto* objName = dynamic_cast<NameExpr*>(sub.object.get())) {
        auto it = impl_->varListElemKinds.find(objName->name);
        if (it != impl_->varListElemKinds.end()) setElemKind = it->second;
    }
    if (setElemKind == Type::Kind::Int) {
        Type::Kind astElemKind =
            impl_->getIterableElementKind(sub.object.get());
        if (astElemKind != Type::Kind::Int)
            setElemKind = astElemKind;
    }

    if (Impl::isBoxedKind(impl_->getIterableElementKind(sub.object.get()))) {
        auto tp = impl_->boxArgTagPayload(node.value.get(), val, true);
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_list_box_set"],
            {list, idx, tp.first, tp.second});
        return true;
    }

    if (isPtrElemKind(setElemKind)) {
        llvm::Value* pval = val;
        if (setElemKind == Type::Kind::Str && pval->getType()->isPointerTy())
            pval = impl_->ensureHeapString(pval, node.value.get());
        if (!pval->getType()->isPointerTy())
            pval = impl_->builder->CreateIntToPtr(pval, impl_->i8PtrType);
        if (impl_->options.gcMode == GCMode::RC &&
            Impl::isBorrowedHeapExpr(node.value.get())) {
            const char* increfFn =
                setElemKind == Type::Kind::Str      ? "dragon_incref_str" :
                setElemKind == Type::Kind::Function ? "dragon_incref_callable"
                                                    : "dragon_incref";
            impl_->builder->CreateCall(impl_->runtimeFuncs[increfFn], {pval});
        }
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_list_set_ptr"], {list, idx, pval});
        return true;
    }

    llvm::Value* storeVal = val;
    if (storeVal->getType() == impl_->i1Type) {
        storeVal = impl_->builder->CreateZExt(storeVal, impl_->i64Type);
    } else if (storeVal->getType() == impl_->f64Type) {
        storeVal = impl_->builder->CreateBitCast(storeVal, impl_->i64Type);
    } else if (storeVal->getType()->isPointerTy()) {
        storeVal = impl_->builder->CreatePtrToInt(storeVal, impl_->i64Type);
    }

    if (!isPrimElemKind(setElemKind)) {
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_list_set"], {list, idx, storeVal});
        return true;
    }
    impl_->emitListPrimInlineStore(sub, setElemKind, list, idx, storeVal, val);
    return true;
}

void CodeGen::Impl::emitListPrimInlineStore(SubscriptExpr& sub,
                                            Type::Kind elemKind,
                                            llvm::Value* list, llvm::Value* idx,
                                            llvm::Value* storeVal,
                                            llvm::Value* rawVal) {
    auto* tbaaHdrTag = llvm::MDNode::get(*context,
        {tbaaListHeader, tbaaListHeader,
         llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Type, 0))});
    auto* tbaaDataTag = llvm::MDNode::get(*context,
        {tbaaListData, tbaaListData,
         llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Type, 0))});

    auto* dataGEP = builder->CreateGEP(i64Type, list,
        llvm::ConstantInt::get(i64Type, 2), "lset.data.gep");
    auto* dataRaw = builder->CreateLoad(i64Type, dataGEP, "lset.data.raw");
    llvm::cast<llvm::Instruction>(dataRaw)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaHdrTag);
    auto* dataPtr = builder->CreateIntToPtr(dataRaw, i8PtrType, "lset.data");
    auto* sizeGEP = builder->CreateGEP(i64Type, list,
        llvm::ConstantInt::get(i64Type, 3), "lset.size.gep");
    auto* sizeLoad = builder->CreateLoad(i64Type, sizeGEP, "lset.size");
    llvm::cast<llvm::Instruction>(sizeLoad)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaHdrTag);
    auto* size = sizeLoad;

    llvm::Value* finalIdx;
    if (isExprDefinitelyNonNeg(sub.index.get())) {
        finalIdx = idx;
    } else {
        auto* isNeg = builder->CreateICmpSLT(idx,
            llvm::ConstantInt::get(i64Type, 0), "idx.neg");
        auto* adjIdx = builder->CreateAdd(idx, size, "idx.adj");
        finalIdx = builder->CreateSelect(isNeg, adjIdx, idx, "idx.final");
    }

    auto* inBounds = builder->CreateICmpULT(finalIdx, size, "idx.ok");
    auto* func = currentFunction;
    auto* okBB = llvm::BasicBlock::Create(*context, "lset.ok", func);
    auto* oobBB = llvm::BasicBlock::Create(*context, "lset.oob", func);
    builder->CreateCondBr(inBounds, okBB, oobBB);

    builder->SetInsertPoint(oobBB);
    builder->CreateCall(runtimeFuncs["dragon_list_set"], {list, idx, storeVal});
    builder->CreateUnreachable();

    builder->SetInsertPoint(okBB);
    auto* i8Ty = llvm::Type::getInt8Ty(*context);
    llvm::Type* strideTy;
    llvm::Value* elemVal;
    switch (elemKind) {
        case Type::Kind::Bool:
            strideTy = i8Ty;
            elemVal = storeVal;
            if (elemVal->getType() != i8Ty)
                elemVal = builder->CreateTrunc(elemVal, i8Ty, "lset.elem.trunc");
            break;
        case Type::Kind::Float:
            strideTy = f64Type;
            elemVal = rawVal;
            if (elemVal->getType() == i64Type)
                elemVal = builder->CreateSIToFP(elemVal, f64Type);
            else if (elemVal->getType() == i1Type)
                elemVal = builder->CreateUIToFP(elemVal, f64Type);
            break;
        default:
            strideTy = i64Type;
            elemVal = storeVal;
            break;
    }
    auto* elemGEP = builder->CreateGEP(strideTy, dataPtr,
        finalIdx, "lset.elem.gep");
    auto* elemStore = builder->CreateStore(elemVal, elemGEP);
    elemStore->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaDataTag);
}

bool CodeGen::tryEmitDictAttrStore(AttributeExpr& attr, AssignStmt& node,
                                   llvm::Value* val) {
    auto* objName = dynamic_cast<NameExpr*>(attr.object.get());
    if (!objName) return false;
    if (impl_->lookupVarKind(objName->name) != Impl::VarKind::Dict) return false;

    attr.object->accept(*this);
    llvm::Value* dict = impl_->lastValue;
    auto* keyStr = impl_->builder->CreateGlobalString(attr.attribute);
    llvm::Value* storeVal = val;
    int64_t tag = 0;
    if (storeVal->getType() == impl_->i1Type) {
        tag = 3;
        storeVal = impl_->builder->CreateZExt(storeVal, impl_->i64Type);
    } else if (storeVal->getType() == impl_->f64Type) {
        tag = 2;
        storeVal = impl_->builder->CreateBitCast(storeVal, impl_->i64Type);
    } else if (storeVal->getType()->isPointerTy()) {
        tag = impl_->inferPtrValueTag(node.value.get());
        if (tag == TAG_STR) storeVal = impl_->ensureHeapString(storeVal, node.value.get());
        impl_->increfBorrowedContainerValue(storeVal, node.value.get(), tag);
        storeVal = impl_->builder->CreatePtrToInt(storeVal, impl_->i64Type);
    }
    llvm::Value* tagVal = llvm::ConstantInt::get(impl_->i64Type, tag);
    impl_->builder->CreateCall(
        impl_->runtimeFuncs["dragon_dict_set_tagged"], {dict, keyStr, storeVal, tagVal});
    return true;
}

bool CodeGen::tryEmitStaticFieldStore(AttributeExpr& attr, AssignStmt& node,
                                      llvm::Value* val) {
    auto* objName = dynamic_cast<NameExpr*>(attr.object.get());
    if (!objName) return false;
    auto sfIt = impl_->staticFieldGlobalsBySym.find(impl_->classSym(objName->name));
    if (sfIt == impl_->staticFieldGlobalsBySym.end()) return false;
    auto gvIt = sfIt->second.find(attr.attribute);
    if (gvIt == sfIt->second.end()) return false;

    llvm::GlobalVariable* gv = gvIt->second;
    llvm::Type* fieldType = gv->getValueType();
    llvm::Value* storeVal = impl_->coerceAssignedFieldValue(val, fieldType);
    Impl::VarKind fieldKind = impl_->lookupFieldKind(objName->name, attr.attribute);
    Impl::VarKind newKind = impl_->fieldStoreNewKind(node.value.get(), fieldKind);
    bool rhsBorrowed = Impl::isBorrowedHeapExpr(node.value.get());
    impl_->storeWithRCOverwrite(
        gv, fieldType, storeVal, fieldKind, newKind, rhsBorrowed,
        objName->name + "." + attr.attribute);
    return true;
}

bool CodeGen::tryEmitPropertySetterStore(AttributeExpr& attr, AssignStmt& node,
                                         llvm::Value* val) {
    std::string className = impl_->resolveAttrTargetClass(&attr);
    if (className.empty()) return false;

    std::string setterClass;
    std::string setterMethodName;
    for (std::string cur = className; !cur.empty(); ) {
        auto sit = impl_->classPropertySettersBySym.find(impl_->classSym(cur));
        if (sit != impl_->classPropertySettersBySym.end()) {
            auto mit = sit->second.find(attr.attribute);
            if (mit != sit->second.end()) {
                setterClass = cur;
                setterMethodName = mit->second;
                break;
            }
        }
        auto pp = impl_->classParentNamesBySym.find(impl_->classSym(cur));
        if (pp == impl_->classParentNamesBySym.end()) break;
        cur = pp->second;
    }
    if (setterClass.empty()) return false;

    std::string setterFuncName =
        impl_->classSymPrefix(setterClass) + "_" + setterMethodName;
    auto* setterFn = impl_->module->getFunction(setterFuncName);
    if (!setterFn) return false;

    attr.object->accept(*this);
    llvm::Value* obj = impl_->lastValue;
    if (!obj->getType()->isPointerTy())
        obj = impl_->builder->CreateIntToPtr(obj, impl_->i8PtrType);
    std::vector<std::pair<llvm::Value*, Impl::VarKind>> setterTemps;
    Impl::VarKind rdk = impl_->ownedTempDrainKind(attr.object.get(), obj);
    if (rdk != Impl::VarKind::Other)
        setterTemps.emplace_back(obj, rdk);
    if (node.targets.size() == 1) {
        Impl::VarKind vdk = impl_->ownedTempDrainKind(node.value.get(), val);
        if (vdk != Impl::VarKind::Other)
            setterTemps.emplace_back(val, vdk);
    }
    auto setterBases = impl_->pushArgTempCleanups(setterTemps);
    auto* fty = setterFn->getFunctionType();
    llvm::Value* coerced = val;
    if (fty->getNumParams() >= 2)
        coerced = impl_->coerceArg(coerced, fty->getParamType(1));
    impl_->builder->CreateCall(setterFn, {obj, coerced});
    impl_->popArgTempCleanups(setterBases);
    impl_->drainBorrowTemps(setterTemps);
    return true;
}

bool CodeGen::tryEmitInstanceFieldStore(AttributeExpr& attr, AssignStmt& node,
                                        llvm::Value* val) {
    std::string className = impl_->resolveAttrTargetClass(&attr);
    if (className.empty()) return false;
    auto structIt = impl_->classStructTypesBySym.find(impl_->classSym(className));
    if (structIt == impl_->classStructTypesBySym.end()) return false;
    auto fieldIt = impl_->classFieldIndicesBySym.find(impl_->classSym(className));
    if (fieldIt == impl_->classFieldIndicesBySym.end()) return false;
    auto idxIt = fieldIt->second.find(attr.attribute);
    if (idxIt == fieldIt->second.end()) return false;

    attr.object->accept(*this);
    llvm::Value* objPtr = impl_->lastValue;
    auto* gep = impl_->builder->CreateStructGEP(
        structIt->second, objPtr, idxIt->second,
        attr.attribute + "_ptr");
    auto* fieldType = impl_->classFieldTypesBySym[impl_->classSym(className)][attr.attribute];
    llvm::Value* storeVal = impl_->coerceAssignedFieldValue(val, fieldType);

    auto cfIt = impl_->classFieldCallableTypeBySym.find(impl_->classSym(className));
    if (cfIt != impl_->classFieldCallableTypeBySym.end() &&
        cfIt->second.count(attr.attribute)) {
        auto toI8Ptr = [&](llvm::Value* p) -> llvm::Value* {
            if (p->getType()->isIntegerTy())
                return impl_->builder->CreateIntToPtr(p, impl_->i8PtrType);
            if (p->getType() != impl_->i8PtrType && p->getType()->isPointerTy())
                return impl_->builder->CreateBitCast(p, impl_->i8PtrType);
            return p;
        };
        llvm::Value* newPtr = toI8Ptr(storeVal);
        if (Impl::isBorrowedHeapExpr(node.value.get()))
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_incref_callable"], {newPtr});
        auto* oldVal = impl_->builder->CreateLoad(
            fieldType, gep, attr.attribute + ".old");
        llvm::Value* oldPtr = toI8Ptr(oldVal);
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_decref_callable"], {oldPtr});
        impl_->emitFieldSharedBarrier(objPtr, newPtr, Impl::VarKind::Closure);
        impl_->builder->CreateStore(storeVal, gep);
        return true;
    }

    Impl::VarKind fieldKind = impl_->lookupFieldKind(className, attr.attribute);
    Impl::VarKind newKind = impl_->fieldStoreNewKind(node.value.get(), fieldKind);
    bool rhsBorrowed = Impl::isBorrowedHeapExpr(node.value.get());
    bool ownedBoxUnboxed = false;
    llvm::Value* ownedBoxPayload = nullptr;
    llvm::Value* ownedBoxTag = nullptr;
    if (fieldKind != Impl::VarKind::Union &&
        storeVal->getType() == impl_->boxType &&
        fieldType != impl_->boxType) {
        if (impl_->options.gcMode == GCMode::RC &&
            impl_->isOwnedBoxResult(storeVal)) {
            ownedBoxUnboxed = true;
            ownedBoxPayload = impl_->boxPayloadI64(storeVal, "ownbox.pay");
            ownedBoxTag = impl_->boxTag(storeVal, "ownbox.tag");
        }
        storeVal = impl_->boxPayloadAsKind(
            storeVal, Impl::typeKindToVarKind(
                fieldType == impl_->f64Type ? Type::Kind::Float :
                fieldType == impl_->i1Type ? Type::Kind::Bool :
                fieldType->isPointerTy() ? Type::Kind::Str :
                Type::Kind::Int));
        if (impl_->options.gcMode == GCMode::RC &&
            !ownedBoxUnboxed && fieldType->isPointerTy())
            rhsBorrowed = true;
    }
    if (fieldKind == Impl::VarKind::Union &&
        storeVal->getType() != impl_->boxType) {
        auto tp = impl_->boxArgTagPayload(node.value.get(), storeVal, true);
        storeVal = impl_->makeBox(tp.first, tp.second);
        rhsBorrowed = false;
    }
    impl_->emitFieldSharedBarrier(objPtr, storeVal, fieldKind);
    impl_->storeWithRCOverwrite(
        gep, fieldType, storeVal, fieldKind, newKind, rhsBorrowed,
        className + "." + attr.attribute);
    impl_->emitMoveOutIfMarked(node.value.get());
    if (ownedBoxUnboxed && rhsBorrowed)
        impl_->emitUnionDecref(ownedBoxPayload, ownedBoxTag);
    return true;
}

void CodeGen::emitTupleUnpackAssign(TupleExpr& tupleTarget, AssignStmt& node,
                                    llvm::Value* val) {
    int64_t numTargets = tupleTarget.elements.size();

    bool rhsIsTuple = dynamic_cast<TupleExpr*>(node.value.get()) != nullptr;
    if (!rhsIsTuple) {
        if (auto* rhsName = dynamic_cast<NameExpr*>(node.value.get())) {
            rhsIsTuple = impl_->lookupVarKind(rhsName->name) == Impl::VarKind::Tuple;
        }
    }
    if (!rhsIsTuple && node.value->type &&
        node.value->type->kind() == Type::Kind::Tuple)
        rhsIsTuple = true;
    const std::string elemGetFn =
        rhsIsTuple ? "dragon_tuple_get" : "dragon_list_get";

    int64_t starIdx = -1;
    for (int64_t i = 0; i < numTargets; i++) {
        if (dynamic_cast<StarredExpr*>(tupleTarget.elements[i].get())) {
            starIdx = i;
            break;
        }
    }

    if (starIdx >= 0) {
        llvm::Value* totalLen = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_list_len"], {val}, "totallen");

        for (int64_t i = 0; i < starIdx; i++) {
            llvm::Value* idx = llvm::ConstantInt::get(impl_->i64Type, i);
            llvm::Value* elem = impl_->builder->CreateCall(
                impl_->runtimeFuncs[elemGetFn], {val, idx}, "elem");
            if (auto* nameTarget = dynamic_cast<NameExpr*>(tupleTarget.elements[i].get()))
                impl_->storeUnpackedI64Elem(nameTarget, elem);
        }

        int64_t postStarCount = numTargets - starIdx - 1;

        auto* starredExpr = dynamic_cast<StarredExpr*>(tupleTarget.elements[starIdx].get());
        if (auto* starName = dynamic_cast<NameExpr*>(starredExpr->value.get())) {
            llvm::Value* preCount = llvm::ConstantInt::get(impl_->i64Type, starIdx);
            llvm::Value* postCount = llvm::ConstantInt::get(impl_->i64Type, postStarCount);
            llvm::Value* restLen = impl_->builder->CreateSub(totalLen, preCount);
            restLen = impl_->builder->CreateSub(restLen, postCount, "restlen");

            llvm::Value* capVal = llvm::ConstantInt::get(impl_->i64Type, 8);
            llvm::Value* restList = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_new"], {capVal}, "restlist");

            auto* func = impl_->currentFunction;
            auto* loopBB = llvm::BasicBlock::Create(*impl_->context, "star.loop", func);
            auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, "star.body", func);
            auto* endBB = llvm::BasicBlock::Create(*impl_->context, "star.end", func);

            auto* jAlloca = impl_->createEntryAlloca(func, "star.j", impl_->i64Type);
            impl_->builder->CreateStore(llvm::ConstantInt::get(impl_->i64Type, 0), jAlloca);
            impl_->builder->CreateBr(loopBB);

            impl_->builder->SetInsertPoint(loopBB);
            llvm::Value* j = impl_->builder->CreateLoad(impl_->i64Type, jAlloca, "j");
            llvm::Value* cond = impl_->builder->CreateICmpSLT(j, restLen, "cmp");
            impl_->builder->CreateCondBr(cond, bodyBB, endBB);

            impl_->builder->SetInsertPoint(bodyBB);
            llvm::Value* srcIdx = impl_->builder->CreateAdd(
                llvm::ConstantInt::get(impl_->i64Type, starIdx), j, "srcidx");
            llvm::Value* elem = impl_->builder->CreateCall(
                impl_->runtimeFuncs[elemGetFn], {val, srcIdx}, "starelem");
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_append"], {restList, elem});
            llvm::Value* jNext = impl_->builder->CreateAdd(
                j, llvm::ConstantInt::get(impl_->i64Type, 1), "jnext");
            impl_->builder->CreateStore(jNext, jAlloca);
            impl_->builder->CreateBr(loopBB);

            impl_->builder->SetInsertPoint(endBB);

            auto* alloca = impl_->lookupVar(starName->name);
            if (!alloca) {
                alloca = impl_->createEntryAlloca(
                    func, starName->name, restList->getType());
                impl_->setVar(starName->name, alloca, Impl::VarKind::List);
            }
            impl_->builder->CreateStore(restList, alloca);
        }

        for (int64_t i = 0; i < postStarCount; i++) {
            llvm::Value* offset = llvm::ConstantInt::get(impl_->i64Type, postStarCount - i);
            llvm::Value* idx = impl_->builder->CreateSub(totalLen, offset, "postidx");
            llvm::Value* elem = impl_->builder->CreateCall(
                impl_->runtimeFuncs[elemGetFn], {val, idx}, "postelem");
            auto& elemExpr = tupleTarget.elements[starIdx + 1 + i];
            if (auto* nameTarget = dynamic_cast<NameExpr*>(elemExpr.get()))
                impl_->storeUnpackedI64Elem(nameTarget, elem);
        }
        return;
    }

    auto rhsElemType = [&](int64_t i) -> std::shared_ptr<Type> {
        if (!node.value->type) return nullptr;
        if (auto* tt = dynamic_cast<TupleType*>(node.value->type.get())) {
            if (i < (int64_t)tt->elementTypes.size()) return tt->elementTypes[i];
        } else if (auto* lt = dynamic_cast<ListType*>(node.value->type.get())) {
            return lt->elementType;
        }
        return nullptr;
    };

    for (int64_t i = 0; i < numTargets; i++) {
        llvm::Value* idx = llvm::ConstantInt::get(impl_->i64Type, i);
        llvm::Value* elem = impl_->builder->CreateCall(
            impl_->runtimeFuncs[elemGetFn], {val, idx}, "unpack");

        llvm::Type* slotTy = impl_->i64Type;
        Impl::VarKind newKind = Impl::VarKind::Other;
        std::string elemClassName;
        auto et = rhsElemType(i);
        Type::Kind ek = et ? et->kind() : Type::Kind::Int;
        switch (ek) {
            case Type::Kind::Float:
                elem = impl_->builder->CreateBitCast(elem, impl_->f64Type, "unpack.f64");
                slotTy = impl_->f64Type; newKind = Impl::VarKind::Float;
                break;
            case Type::Kind::Bool:
                elem = impl_->builder->CreateICmpNE(
                    elem, llvm::ConstantInt::get(impl_->i64Type, 0), "unpack.bool");
                slotTy = impl_->i1Type; newKind = Impl::VarKind::Bool;
                break;
            case Type::Kind::Str:
            case Type::Kind::Bytes:
            case Type::Kind::ByteArray:
            case Type::Kind::List:
            case Type::Kind::Dict:
            case Type::Kind::Set:
            case Type::Kind::Tuple:
            case Type::Kind::Instance:
            case Type::Kind::Ptr:
                elem = impl_->builder->CreateIntToPtr(elem, impl_->i8PtrType, "unpack.ptr");
                slotTy = impl_->i8PtrType;
                newKind = Impl::typeKindToVarKind(ek);
                if (auto* it = dynamic_cast<InstanceType*>(et.get());
                    it && it->classType && ek == Type::Kind::Instance)
                    elemClassName = it->classType->name;
                break;
            case Type::Kind::Int:
            default:
                newKind = Impl::VarKind::Int;
                break;
        }

        if (auto* nameTarget = dynamic_cast<NameExpr*>(tupleTarget.elements[i].get()))
            impl_->storeUnpackedElemToName(nameTarget, elem, slotTy, newKind,
                                           elemClassName, node.isConst);
    }
    if (impl_->options.gcMode == GCMode::RC &&
        val->getType()->isPointerTy() &&
        !Impl::isBorrowedHeapExpr(node.value.get()) &&
        impl_->isOwnedPtrResult(val)) {
        impl_->emitDecrefByKind(val, Impl::VarKind::List);
    }
}

void CodeGen::Impl::storeUnpackedI64Elem(NameExpr* nameTarget, llvm::Value* elem) {
    if (tryNarrowShadowWriteThrough(nameTarget->name, elem, true,
                                    VarKind::Int)) return;
    auto* alloca = lookupVar(nameTarget->name);
    bool hadSlot = (alloca != nullptr);
    if (!alloca) {
        alloca = createEntryAlloca(currentFunction, nameTarget->name, i64Type);
        setVar(nameTarget->name, alloca);
    }
    VarKind oldKind = hadSlot ? lookupVarKind(nameTarget->name) : VarKind::Other;
    storeWithRCOverwrite(alloca, i64Type, elem, oldKind, VarKind::Other, true,
                         nameTarget->name);
}

void CodeGen::Impl::storeUnpackedElemToName(NameExpr* nameTarget, llvm::Value* elem,
                                            llvm::Type* slotTy, VarKind newKind,
                                            const std::string& elemClassName,
                                            bool isConst) {
    bool unpackModuleLevel = (currentFunction == mainFunction) &&
                             (scopes.size() <= moduleBodyScopeDepth);
    if (unpackModuleLevel) {
        std::string gKey = globalKeyOrOwn(nameTarget->name);
        auto* gv = lookupModuleGlobal(nameTarget->name);
        VarKind oldKind = gv ? lookupVarKind(nameTarget->name) : VarKind::Other;
        if (!gv) {
            gv = new llvm::GlobalVariable(
                *module, slotTy, false,
                llvm::GlobalValue::InternalLinkage,
                llvm::Constant::getNullValue(slotTy),
                "global." + gKey);
            moduleGlobals[gKey] = gv;
        }
        moduleGlobalKinds[gKey] = newKind;
        bool gborrowed = (slotTy == i8PtrType);
        storeWithRCOverwrite(gv, slotTy, elem, oldKind, newKind,
                             gborrowed, nameTarget->name);
        if (!elemClassName.empty()) {
            varClassNames[nameTarget->name] = elemClassName;
            varClassOwningModule[nameTarget->name] =
                resolveClassOwningModule(elemClassName);
            moduleGlobalClassNames[gKey] = {elemClassName,
                resolveClassOwningModule(elemClassName)};
            if (options.gcMode == GCMode::RC)
                moduleGlobalKinds[gKey] = VarKind::ClassInstance;
        }
        if (options.gcMode == GCMode::RC && slotTy == i8PtrType) {
            VarKind sk = moduleGlobalKinds[gKey];
            if (sk == VarKind::Str) {
                builder->CreateCall(
                    runtimeFuncs[isConst ? "dragon_str_make_immortal"
                                         : "dragon_mark_shared_str"],
                    {elem});
            } else {
                emitMarkSharedGlobal(elem, sk);
            }
        }
        return;
    }
    if (tryNarrowShadowWriteThrough(nameTarget->name, elem, true, newKind)) {
        if (!elemClassName.empty())
            varClassNames[nameTarget->name] = elemClassName;
        return;
    }
    auto* alloca = lookupVar(nameTarget->name);
    bool hadSlot = (alloca != nullptr);
    if (!alloca) {
        alloca = createEntryAlloca(currentFunction, nameTarget->name, slotTy);
        setVar(nameTarget->name, alloca, newKind);
    }
    VarKind oldKind = hadSlot ? lookupVarKind(nameTarget->name) : VarKind::Other;
    bool borrowed = (slotTy == i8PtrType);
    storeWithRCOverwrite(alloca, slotTy, elem, oldKind, newKind, borrowed,
                         nameTarget->name);
    if (!elemClassName.empty())
        varClassNames[nameTarget->name] = elemClassName;
}

void CodeGen::emitNameAssign(NameExpr& name, AssignStmt& node, llvm::Value* val) {
    bool rhsBorrowed = (val->getType() == impl_->boxType)
        ? !impl_->isOwnedBoxResult(val)
        : Impl::isBorrowedHeapExpr(node.value.get());
    if (impl_->tryNarrowShadowWriteThrough(
            name.name, val, rhsBorrowed,
            impl_->inferAssignedVarKind(node, val))) return;
    if (impl_->isCellBacked(name.name)) {
        auto* alloca = impl_->lookupVar(name.name);
        Impl::VarKind cellKind = impl_->lookupVarKind(name.name);
        llvm::Value* coerced = val;
        if (cellKind == Impl::VarKind::Bool && coerced->getType() == impl_->i64Type)
            coerced = impl_->builder->CreateICmpNE(
                coerced, llvm::ConstantInt::get(impl_->i64Type, 0), "tobool");
        else if (cellKind == Impl::VarKind::Int && coerced->getType() == impl_->i1Type)
            coerced = impl_->builder->CreateZExt(coerced, impl_->i64Type, "boolext");
        else if (cellKind == Impl::VarKind::Float && coerced->getType() == impl_->i64Type)
            coerced = impl_->builder->CreateSIToFP(coerced, impl_->f64Type, "i2f");
        impl_->emitCellWrite(alloca, cellKind, coerced, name.name,
                             Impl::isBorrowedHeapExpr(node.value.get()));
        return;
    }
    auto* alloca = impl_->lookupVar(name.name);
    bool hadExistingSlot = (alloca != nullptr);
    if (!alloca) {
        if (tryEmitExistingGlobalStore(name, node, val)) return;
        if (impl_->currentFunction == impl_->mainFunction) {
            emitNewModuleGlobalStore(name, node, val);
            return;
        }
        alloca = impl_->createEntryAlloca(
            impl_->currentFunction, name.name, val->getType());
        impl_->setVar(name.name, alloca);
    }
    emitLocalSlotStore(name, node, val, alloca, hadExistingSlot);
}

bool CodeGen::tryEmitExistingGlobalStore(NameExpr& name, AssignStmt& node,
                                         llvm::Value* val) {
    auto* gv = impl_->lookupModuleGlobal(name.name);
    if (!gv || !impl_->shouldUseModuleGlobal(name.name)) return false;

    llvm::Type* gvType = gv->getValueType();
    Impl::VarKind oldKind = impl_->lookupVarKind(name.name);
    bool rhsBorrowed = Impl::isBorrowedHeapExpr(node.value.get());
    if (oldKind == Impl::VarKind::Union && gvType == impl_->boxType) {
        if (val->getType() == impl_->boxType) {
            if (impl_->options.gcMode == GCMode::RC) {
                auto* oldBox = impl_->builder->CreateLoad(impl_->boxType, gv, "old.box");
                impl_->emitUnionDecref(impl_->boxPayloadI64(oldBox, "op"),
                                       impl_->boxTag(oldBox, "ot"));
                if (!impl_->isOwnedBoxResult(val))
                    impl_->emitUnionIncref(impl_->boxPayloadI64(val, "np"),
                                           impl_->boxTag(val, "nt"));
            }
            impl_->builder->CreateStore(val, gv);
            if (impl_->options.gcMode == GCMode::RC)
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_mark_shared_boxed"],
                    {impl_->boxTag(val, "shr.tag"),
                     impl_->boxPayloadI64(val, "shr.pay")});
        } else {
            auto* newTag = impl_->emitTagForExpr(node.value.get(), *this);
            if (impl_->options.gcMode == GCMode::RC) {
                auto* oldBox = impl_->builder->CreateLoad(impl_->boxType, gv, "old.box");
                impl_->emitUnionDecref(impl_->boxPayloadI64(oldBox, "op"),
                                       impl_->boxTag(oldBox, "ot"));
                if (rhsBorrowed)
                    impl_->emitUnionIncref(impl_->nativeToPayloadI64(val), newTag);
            }
            impl_->builder->CreateStore(impl_->makeBox(newTag, val), gv);
            if (impl_->options.gcMode == GCMode::RC)
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_mark_shared_boxed"],
                    {newTag, impl_->nativeToPayloadI64(val)});
        }
        return true;
    }
    if (val->getType() == impl_->boxType && gvType != impl_->boxType) {
        val = impl_->boxPayloadAsKind(
            val, Impl::typeKindToVarKind(
                     gvType == impl_->f64Type ? Type::Kind::Float :
                     gvType == impl_->i1Type ? Type::Kind::Bool :
                     gvType->isPointerTy() ? Type::Kind::Str :
                     Type::Kind::Int));
    }
    if (val->getType() != gvType) {
        if (gvType == impl_->f64Type && val->getType() == impl_->i64Type)
            val = impl_->builder->CreateSIToFP(val, impl_->f64Type);
        else if (gvType == impl_->i64Type && val->getType() == impl_->i1Type)
            val = impl_->builder->CreateZExt(val, impl_->i64Type);
    }
    Impl::VarKind newKind = impl_->inferAssignedVarKind(node, val);
    impl_->storeWithRCOverwrite(
        gv, gvType, val, oldKind, newKind, rhsBorrowed, name.name);
    if (newKind != Impl::VarKind::Other)
        impl_->moduleGlobalKinds[impl_->globalKeyOrOwn(name.name)] = newKind;
    impl_->emitMarkSharedGlobal(val, newKind);
    return true;
}

void CodeGen::emitNewModuleGlobalStore(NameExpr& name, AssignStmt& node,
                                       llvm::Value* val) {
    bool rhsBorrowed = Impl::isBorrowedHeapExpr(node.value.get());
    std::string gKey = impl_->globalKeyOrOwn(name.name);
    auto* gv = new llvm::GlobalVariable(
        *impl_->module, val->getType(), false,
        llvm::GlobalValue::InternalLinkage,
        llvm::Constant::getNullValue(val->getType()),
        "global." + gKey);
    impl_->moduleGlobals[gKey] = gv;
    Impl::VarKind vk = Impl::VarKind::Other;
    if (dynamic_cast<ListExpr*>(node.value.get()) || dynamic_cast<ListCompExpr*>(node.value.get()))
        vk = Impl::VarKind::List;
    else if (dynamic_cast<DictExpr*>(node.value.get()) || dynamic_cast<DictCompExpr*>(node.value.get()))
        vk = Impl::VarKind::Dict;
    else if (dynamic_cast<TupleExpr*>(node.value.get()))
        vk = Impl::VarKind::Tuple;
    else if (dynamic_cast<SetExpr*>(node.value.get()))
        vk = Impl::VarKind::Set;
    else if (auto* sl = dynamic_cast<StringLiteral*>(node.value.get()))
        vk = (sl->isBytes ? Impl::VarKind::Bytes : Impl::VarKind::StrLiteral);
    else if (auto* rhsNE = dynamic_cast<NameExpr*>(node.value.get())) {
        if (impl_->classNames.count(rhsNE->name))
            vk = Impl::VarKind::Type;
        else if (val->getType() == impl_->i64Type) vk = Impl::VarKind::Int;
        else if (val->getType() == impl_->f64Type) vk = Impl::VarKind::Float;
        else if (val->getType() == impl_->i1Type) vk = Impl::VarKind::Bool;
    }
    else if (auto* callVal2 = dynamic_cast<CallExpr*>(node.value.get())) {
        if (auto* calleeNm = dynamic_cast<NameExpr*>(callVal2->callee.get())) {
            if (impl_->funcReturnsType.count(impl_->resolveCalleeSymbol(calleeNm->name)))
                vk = Impl::VarKind::Type;
        }
    }
    else if (auto* sub2 = dynamic_cast<SubscriptExpr*>(node.value.get())) {
        if (auto* objName2 = dynamic_cast<NameExpr*>(sub2->object.get())) {
            if (impl_->varDictValueIsType.count(objName2->name))
                vk = Impl::VarKind::Type;
        }
    }
    if (vk == Impl::VarKind::Other) {
        if (val->getType() == impl_->i64Type) vk = Impl::VarKind::Int;
        else if (val->getType() == impl_->f64Type) vk = Impl::VarKind::Float;
        else if (val->getType() == impl_->i1Type) vk = Impl::VarKind::Bool;
        else if (val->getType() == impl_->i8PtrType) {
            if (auto* callVal = dynamic_cast<CallExpr*>(node.value.get())) {
                if (auto* calleeName = dynamic_cast<NameExpr*>(callVal->callee.get())) {
                    if (impl_->generatorFunctions.count(
                            impl_->resolveCalleeSymbol(calleeName->name)))
                        vk = Impl::VarKind::Generator;
                    else if (impl_->typedDictClassesBySym.count(impl_->classSym(calleeName->name)))
                        vk = Impl::VarKind::Dict;
                    else if (impl_->funcReturnsType.count(impl_->resolveCalleeSymbol(calleeName->name)))
                        vk = Impl::VarKind::Type;
                    else if (impl_->funcReturnsPtr.count(impl_->resolveCalleeSymbol(calleeName->name)))
                        impl_->varIsPtrCallable.insert(name.name);
                }
            }
            if (vk == Impl::VarKind::Other)
                vk = Impl::VarKind::Str;
        }
    }
    impl_->moduleGlobalKinds[gKey] = vk;
    impl_->storeWithRCOverwrite(
        gv, gv->getValueType(), val, Impl::VarKind::Other, vk, rhsBorrowed, name.name);
    if (auto* boundClosureType = impl_->takeCallableTypeFor(val)) {
        impl_->callableTypes[name.name] = boundClosureType;
        impl_->moduleGlobalKinds[gKey] = Impl::VarKind::Closure;
    }
    if (impl_->lastValueIsType) {
        impl_->moduleGlobalKinds[gKey] = Impl::VarKind::Type;
        impl_->lastValueIsType = false;
    }
    Impl::VarKind sharedKind = impl_->moduleGlobalKinds[gKey];
    if (sharedKind == Impl::VarKind::StrLiteral &&
        val->getType()->isPointerTy())
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_str_make_immortal"], {val});
    else
        impl_->emitMarkSharedGlobal(val, sharedKind);
    impl_->recordAssignedCallableType(name.name, val, node.value.get());
    if (auto* callVal = dynamic_cast<CallExpr*>(node.value.get())) {
        if (auto* calleeName = dynamic_cast<NameExpr*>(callVal->callee.get())) {
            if (impl_->typedDictClassesBySym.count(impl_->classSym(calleeName->name))) {
                impl_->varTypedDictClass[name.name] = calleeName->name;
            } else if (impl_->classNames.count(calleeName->name)) {
                std::string ownMod =
                    impl_->resolveClassOwningModule(calleeName->name);
                impl_->varClassNames[name.name] = calleeName->name;
                impl_->varClassOwningModule[name.name] = ownMod;
                impl_->moduleGlobalClassNames[gKey] = {calleeName->name, ownMod};
                if (impl_->options.gcMode == GCMode::RC)
                    impl_->moduleGlobalKinds[gKey] = Impl::VarKind::ClassInstance;
            } else if (impl_->lookupVarKind(calleeName->name) == Impl::VarKind::Type) {
                if (impl_->options.gcMode == GCMode::RC)
                    impl_->moduleGlobalKinds[gKey] = Impl::VarKind::ClassInstance;
            }
            if (calleeName->name == "Lock")
                impl_->varClassNames[name.name] = "__Lock";
            if (calleeName->name == "SyncList")
                impl_->varClassNames[name.name] = "__SyncList";
            if (calleeName->name == "SyncDict")
                impl_->varClassNames[name.name] = "__SyncDict";
            if (calleeName->name == "deque") {
                impl_->varClassNames[name.name] = "__Deque";
                impl_->moduleGlobalClassNames[gKey] = {"__Deque", ""};
                impl_->moduleGlobalKinds[gKey] = Impl::VarKind::Deque;
            }
        }
    }
    if (dynamic_cast<FireExpr*>(node.value.get())) {
        impl_->varClassNames[name.name] = "__Thread";
    }
    if (auto cls = impl_->recordVarClassFromValue(name.name, node.value.get());
        !cls.empty()) {
        impl_->moduleGlobalClassNames[gKey] =
            {cls, impl_->varClassOwningModule[name.name]};
        if (impl_->options.gcMode == GCMode::RC && impl_->classNames.count(cls))
            impl_->moduleGlobalKinds[gKey] = Impl::VarKind::ClassInstance;
    }
}

void CodeGen::emitLocalSlotStore(NameExpr& name, AssignStmt& node,
                                 llvm::Value* val, llvm::AllocaInst* alloca,
                                 bool hadExistingSlot) {
    bool rhsBorrowed = Impl::isBorrowedHeapExpr(node.value.get());
    llvm::Type* allocType = alloca->getAllocatedType();
    bool didUnboxToNative = false;
    bool ownedBoxUnboxed = false;
    llvm::Value* ownedBoxPayload = nullptr;
    llvm::Value* ownedBoxTag = nullptr;
    if (val->getType() == impl_->boxType && allocType != impl_->boxType) {
        if (impl_->options.gcMode == GCMode::RC &&
            impl_->isOwnedBoxResult(val)) {
            ownedBoxUnboxed = true;
            ownedBoxPayload = impl_->boxPayloadI64(val, "ownbox.pay");
            ownedBoxTag = impl_->boxTag(val, "ownbox.tag");
        }
        val = impl_->boxPayloadAsKind(
            val, Impl::typeKindToVarKind(
                     allocType == impl_->f64Type ? Type::Kind::Float :
                     allocType == impl_->i1Type ? Type::Kind::Bool :
                     allocType->isPointerTy() ? Type::Kind::Str :
                     Type::Kind::Int));
        didUnboxToNative = true;
    }
    if (impl_->options.gcMode == GCMode::RC && didUnboxToNative &&
        !ownedBoxUnboxed && allocType->isPointerTy())
        rhsBorrowed = true;
    if (val->getType() != allocType) {
        if (allocType == impl_->f64Type && val->getType() == impl_->i64Type)
            val = impl_->builder->CreateSIToFP(val, impl_->f64Type);
        else if (allocType == impl_->i64Type && val->getType() == impl_->i1Type)
            val = impl_->builder->CreateZExt(val, impl_->i64Type);
    }
    Impl::VarKind oldKind = hadExistingSlot
        ? impl_->lookupVarKind(name.name)
        : Impl::VarKind::Other;
    impl_->takeOwnershipOfBorrowedSlot(name.name, alloca, oldKind);
    Impl::VarKind newKind = impl_->inferAssignedVarKind(node, val);
    // A literal store must not downgrade an owned Str slot to StrLiteral
    // (cleanup would skip the decref); a BORROWED slot stays StrLiteral (UAF).
    if (newKind == Impl::VarKind::StrLiteral &&
        oldKind == Impl::VarKind::Str &&
        !impl_->isBorrowedSlot(name.name)) {
        newKind = Impl::VarKind::Str;
    }
    // Unboxed-to-heap-slot RHS: re-derive newKind from the slot's real
    // heap kind, else a Union/Other kind skips the incref (UAF).
    if (didUnboxToNative && allocType->isPointerTy() &&
        (newKind == Impl::VarKind::Union || !Impl::isHeapKind(newKind))) {
        if (oldKind == Impl::VarKind::StrLiteral)
            newKind = Impl::VarKind::Str;
        else if (Impl::isHeapKind(oldKind) && oldKind != Impl::VarKind::Union)
            newKind = oldKind;
    }

    if (oldKind == Impl::VarKind::Union) {
        newKind = Impl::VarKind::Union;
        const bool valueIsBox = (val->getType() == impl_->boxType);
        const bool slotOwnsItsValue = impl_->options.gcMode == GCMode::RC &&
                                      !impl_->isBorrowedSlot(name.name);
        llvm::Value* newTag = valueIsBox
            ? impl_->boxTag(val, "new.tag")
            : impl_->emitTagForExpr(node.value.get(), *this);
        const bool retainNew = valueIsBox ? !impl_->isOwnedBoxResult(val)
                                          : rhsBorrowed;
        if (slotOwnsItsValue) {
            auto* oldBox = impl_->builder->CreateLoad(
                impl_->boxType, alloca, "old.box");
            impl_->emitUnionDecref(impl_->boxPayloadI64(oldBox, "old.payload"),
                                   impl_->boxTag(oldBox, "old.tag"));
        }
        if (slotOwnsItsValue && retainNew)
            impl_->emitUnionIncref(
                valueIsBox ? impl_->boxPayloadI64(val, "new.payload")
                           : impl_->nativeToPayloadI64(val),
                newTag);
        llvm::Value* boxVal = valueIsBox ? val : impl_->makeBox(newTag, val);
        impl_->builder->CreateStore(boxVal, alloca);
        if (slotOwnsItsValue)
            impl_->emitCleanupUpdate(
                name.name, impl_->boxPayloadI64(boxVal, "cl.payload"), newTag);
    } else {
        impl_->storeWithRCOverwrite(
            alloca, allocType, val, oldKind, newKind, rhsBorrowed, name.name);
    }
    // Release the owned box's +1 only when the store took its own ref
    // (surplus); a fresh payload's +1 was adopted - releasing double-frees.
    if (ownedBoxUnboxed && rhsBorrowed)
        impl_->emitUnionDecref(ownedBoxPayload, ownedBoxTag);

    if (auto* boundClosureType = impl_->takeCallableTypeFor(val)) {
        impl_->callableTypes[name.name] = boundClosureType;
        impl_->setVar(name.name, alloca, Impl::VarKind::Closure);
    }
    else if (impl_->lastValueIsType) {
        impl_->setVar(name.name, alloca, Impl::VarKind::Type);
        impl_->lastValueIsType = false;
    }
    else if (oldKind == Impl::VarKind::Union) {
    } else if (didUnboxToNative) {
    } else if (newKind != Impl::VarKind::Other) {
        if (newKind == Impl::VarKind::Union &&
            alloca->getAllocatedType() != impl_->boxType) {
        } else {
            impl_->setVar(name.name, alloca, newKind);
        }
    } else if (auto* rhsName = dynamic_cast<NameExpr*>(node.value.get())) {
        auto rhsKind = impl_->lookupVarKind(rhsName->name);
        if (rhsKind != Impl::VarKind::Other &&
            !(rhsKind == Impl::VarKind::Union &&
              alloca->getAllocatedType() != impl_->boxType))
            impl_->setVar(name.name, alloca, rhsKind);
    }
    impl_->recordAssignedCallableType(name.name, val, node.value.get());

    if (auto* callVal = dynamic_cast<CallExpr*>(node.value.get())) {
        if (auto* calleeName = dynamic_cast<NameExpr*>(callVal->callee.get())) {
            if (impl_->typedDictClassesBySym.count(impl_->classSym(calleeName->name))) {
                impl_->varTypedDictClass[name.name] = calleeName->name;
            } else if (impl_->classNames.count(calleeName->name)) {
                impl_->varClassNames[name.name] = calleeName->name;
                impl_->varClassOwningModule[name.name] =
                    impl_->resolveClassOwningModule(calleeName->name);
                if (impl_->options.gcMode == GCMode::RC)
                    impl_->setVar(name.name, alloca, Impl::VarKind::ClassInstance);
            } else if (impl_->lookupVarKind(calleeName->name) == Impl::VarKind::Type) {
                if (impl_->options.gcMode == GCMode::RC)
                    impl_->setVar(name.name, alloca, Impl::VarKind::ClassInstance);
            }
            if (calleeName->name == "Lock") {
                impl_->varClassNames[name.name] = "__Lock";
                if (!impl_->scopes.empty())
                    impl_->scopes.back().lockDestroyOnExit.insert(name.name);
            }
            if (calleeName->name == "SyncList") {
                impl_->varClassNames[name.name] = "__SyncList";
            }
            if (calleeName->name == "SyncDict") {
                impl_->varClassNames[name.name] = "__SyncDict";
            }
            if (calleeName->name == "deque") {
                impl_->varClassNames[name.name] = "__Deque";
                impl_->setVar(name.name, alloca, Impl::VarKind::Deque);
            }
        }
    }
    if (dynamic_cast<FireExpr*>(node.value.get())) {
        impl_->varClassNames[name.name] = "__Thread";
    }
    if (auto cls = impl_->recordVarClassFromValue(name.name, node.value.get());
        !cls.empty()) {
        if (impl_->options.gcMode == GCMode::RC && impl_->classNames.count(cls))
            impl_->setVar(name.name, alloca, Impl::VarKind::ClassInstance);
    }
}

CodeGen::Impl::VarKind CodeGen::Impl::inferAssignedVarKind(AssignStmt& node,
                                                           llvm::Value* rhsVal) {
    std::vector<std::string> boundNames;
    for (auto& t : node.targets)
        if (auto* tgtName = dynamic_cast<NameExpr*>(t.get()))
            boundNames.push_back(tgtName->name);
    return inferBoundVarKind(node.value.get(), rhsVal, boundNames);
}

CodeGen::Impl::VarKind CodeGen::Impl::inferBoundVarKind(
        Expr* value, llvm::Value* rhsVal,
        const std::vector<std::string>& boundNames) {
    if (dynamic_cast<ListExpr*>(value) || dynamic_cast<ListCompExpr*>(value))
        return VarKind::List;
    if (dynamic_cast<DictExpr*>(value) || dynamic_cast<DictCompExpr*>(value))
        return VarKind::Dict;
    if (dynamic_cast<TupleExpr*>(value))
        return VarKind::Tuple;
    if (dynamic_cast<SetExpr*>(value) || dynamic_cast<SetCompExpr*>(value))
        return VarKind::Set;
    if (auto* sl = dynamic_cast<StringLiteral*>(value))
        return sl->isBytes ? VarKind::Bytes : VarKind::StrLiteral;
    if (auto* rhsName = dynamic_cast<NameExpr*>(value)) {
        if (classNames.count(rhsName->name))
            return VarKind::Type;
        return lookupVarKind(rhsName->name);
    }
    if (auto* callExpr = dynamic_cast<CallExpr*>(value)) {
        if (auto* calleeName = dynamic_cast<NameExpr*>(callExpr->callee.get())) {
            if (funcReturnsType.count(resolveCalleeSymbol(calleeName->name)))
                return VarKind::Type;
        }
    }
    if (auto* sub = dynamic_cast<SubscriptExpr*>(value)) {
        if (auto* objName = dynamic_cast<NameExpr*>(sub->object.get())) {
            if (varDictValueIsType.count(objName->name))
                return VarKind::Type;
        }
    }
    if (value && value->type &&
        value->type->kind() == Type::Kind::Function)
        return VarKind::Closure;
    if (rhsVal->getType() == i64Type) return VarKind::Int;
    if (rhsVal->getType() == f64Type) return VarKind::Float;
    if (rhsVal->getType() == i1Type) return VarKind::Bool;
    if (rhsVal->getType() != i8PtrType || dynamic_cast<NoneLiteral*>(value))
        return VarKind::Other;
    auto* callExpr = dynamic_cast<CallExpr*>(value);
    auto* calleeName = callExpr
        ? dynamic_cast<NameExpr*>(callExpr->callee.get())
        : nullptr;
    if (calleeName) {
        std::string calleeSym = resolveCalleeSymbol(calleeName->name);
        if (generatorFunctions.count(calleeSym))
            return VarKind::Generator;
        if (typedDictClassesBySym.count(classSym(calleeName->name)))
            return VarKind::Dict;
        if (classNames.count(calleeName->name))
            return VarKind::ClassInstance;
        if (funcReturnsType.count(calleeSym))
            return VarKind::Type;
        if (funcReturnsPtr.count(calleeSym))
            for (auto& boundName : boundNames)
                varIsPtrCallable.insert(boundName);
        if (lookupVarKind(calleeName->name) == VarKind::Type)
            return VarKind::ClassInstance;
    }
    if (dynamic_cast<FireExpr*>(value))
        return VarKind::ClassInstance;
    if (!value->type)
        return VarKind::Str;
    switch (value->type->kind()) {
        case Type::Kind::Bytes:
        case Type::Kind::ByteArray:
        case Type::Kind::List:
        case Type::Kind::Deque:
        case Type::Kind::Dict:
        case Type::Kind::Tuple:
        case Type::Kind::Set:
        case Type::Kind::Instance:
            return typeKindToVarKind(value->type->kind());
        case Type::Kind::Ptr:
            return VarKind::Other;
        default:
            return VarKind::Str;
    }
}

void CodeGen::Impl::recordAssignedCallableType(const std::string& name,
                                               llvm::Value* val, Expr* rhs) {
    if (auto* lambdaFn = llvm::dyn_cast<llvm::Function>(val)) {
        callableTypes[name] = lambdaFn->getFunctionType();
        return;
    }
    auto* rhsName = dynamic_cast<NameExpr*>(rhs);
    if (!rhsName || classNames.count(rhsName->name)) return;
    llvm::Function* refFunc = nullptr;
    std::string aliasSym = lookupImportedAlias(rhsName->name);
    if (!aliasSym.empty())
        refFunc = module->getFunction(aliasSym);
    if (!refFunc)
        refFunc = module->getFunction(
            mangleFunc(currentModuleName, rhsName->name));
    if (!refFunc)
        refFunc = module->getFunction(userFuncName(rhsName->name));
    if (!refFunc) refFunc = module->getFunction(rhsName->name);
    if (refFunc)
        callableTypes[name] = refFunc->getFunctionType();
    auto ctIt = callableTypes.find(rhsName->name);
    if (ctIt != callableTypes.end())
        callableTypes[name] = ctIt->second;
}

}
