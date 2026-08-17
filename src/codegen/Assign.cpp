#include "../CodeGenImpl.h"

namespace dragon {

void CodeGen::visit(AssignStmt& node) {
    if (node.targets.empty()) return;

    impl_->lastClosureCallableType = nullptr;

    if (node.targets.size() == 1) {
        if (auto* tName = dynamic_cast<NameExpr*>(node.targets[0].get())) {
            if (auto* bin = dynamic_cast<BinaryExpr*>(node.value.get())) {
                auto* binLeft = dynamic_cast<NameExpr*>(bin->left.get());
                if (bin->op.type() == TokenType::PLUS && binLeft &&
                    binLeft->name == tName->name &&
                    !impl_->isCellBacked(tName->name)) {
                    auto vk = impl_->lookupVarKind(tName->name);
                    if (vk == Impl::VarKind::Str || vk == Impl::VarKind::StrLiteral) {
                        llvm::Value* storeTarget = nullptr;
                        llvm::Type* loadType = nullptr;
                        if (auto* alloca = impl_->lookupVar(tName->name)) {
                            storeTarget = alloca;
                            loadType = alloca->getAllocatedType();
                        } else if (auto* gv = impl_->lookupModuleGlobal(tName->name)) {
                            if (impl_->shouldUseModuleGlobal(tName->name)) {
                                storeTarget = gv;
                                loadType = gv->getValueType();
                            }
                        }
                        if (storeTarget && loadType == impl_->i8PtrType) {
                            llvm::Value* cur = impl_->builder->CreateLoad(
                                loadType, storeTarget, tName->name);
                            bin->right->accept(*this);
                            impl_->emitStrAppendInplace(
                                storeTarget, cur, impl_->lastValue, tName->name);
                            return;
                        }
                    }
                }
            }
        }
    }

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
            std::string setClassName = impl_->resolveExprClassName(sub->object.get());
            if (!setClassName.empty() && impl_->hasDunder(setClassName, "__setitem__")) {
                sub->object->accept(*this);
                llvm::Value* obj = impl_->lastValue;
                sub->index->accept(*this);
                llvm::Value* idx = impl_->lastValue;
                impl_->callDunder(setClassName, "__setitem__", obj, {idx, val});
                continue;
            }
        }

        auto isDictFieldOf = [this](const std::string& className,
                                    const std::string& fieldName) -> bool {
            auto fkIt = impl_->classFieldKindsBySym.find(impl_->classSym(className));
            if (fkIt == impl_->classFieldKindsBySym.end()) return false;
            auto fkIt2 = fkIt->second.find(fieldName);
            if (fkIt2 == fkIt->second.end()) return false;
            return fkIt2->second == Impl::VarKind::Dict;
        };
        auto isListFieldOf = [this](const std::string& className,
                                    const std::string& fieldName) -> bool {
            auto fkIt = impl_->classFieldKindsBySym.find(impl_->classSym(className));
            if (fkIt == impl_->classFieldKindsBySym.end()) return false;
            auto fkIt2 = fkIt->second.find(fieldName);
            if (fkIt2 == fkIt->second.end()) return false;
            return fkIt2->second == Impl::VarKind::List;
        };
        auto resolveAttrClass = [this](AttributeExpr* attrExpr) -> std::string {
            if (auto* attrObjName = dynamic_cast<NameExpr*>(attrExpr->object.get())) {
                if (attrObjName->name == "self" && !impl_->currentClassName.empty())
                    return impl_->currentClassName;
                auto vit = impl_->varClassNames.find(attrObjName->name);
                if (vit != impl_->varClassNames.end()) return vit->second;
                return "";
            }
            return impl_->resolveExprClassName(attrExpr->object.get());
        };
        if (auto* sub = dynamic_cast<SubscriptExpr*>(target.get())) {
            bool isDict = false;
            if (auto* objName = dynamic_cast<NameExpr*>(sub->object.get())) {
                isDict = impl_->lookupVarKind(objName->name) == Impl::VarKind::Dict;
            } else if (auto* objAttr = dynamic_cast<AttributeExpr*>(sub->object.get())) {
                std::string cls = resolveAttrClass(objAttr);
                if (!cls.empty() && isDictFieldOf(cls, objAttr->attribute))
                    isDict = true;
            }
            if (!isDict && sub->object->type &&
                sub->object->type->kind() == Type::Kind::Dict)
                isDict = true;
            if (isDict) {
                Type::Kind dictKk = impl_->resolveDictKeyKind(sub->object.get());
                bool intKeyed =
                    dictKk == Type::Kind::Int || dictKk == Type::Kind::Float;

                sub->object->accept(*this);
                llvm::Value* dict = impl_->lastValue;
                sub->index->accept(*this);
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
                        continue;
                    }
                    if (val->getType()->isPointerTy()) {
                        int64_t tag = impl_->inferPtrValueTag(node.value.get());
                        llvm::Value* pval = val;
                        if (tag == 1) pval = impl_->ensureHeapString(pval, node.value.get());
                        if (impl_->options.gcMode == GCMode::RC &&
                            (tag == 1 || tag == 5 || tag == 6 || tag == 7) &&
                            Impl::isBorrowedHeapExpr(node.value.get())) {
                            if (tag == 1)
                                impl_->builder->CreateCall(
                                    impl_->runtimeFuncs["dragon_incref_str"], {pval});
                            else
                                impl_->builder->CreateCall(
                                    impl_->runtimeFuncs["dragon_incref"], {pval});
                        }
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_dict_int_set_ptr"],
                            {dict, key, pval, llvm::ConstantInt::get(impl_->i64Type, tag)});
                        continue;
                    }
                    llvm::Value* storeVal = val;
                    int64_t tag = 0;
                    if (storeVal->getType() == impl_->i1Type) {
                        tag = 3;
                        storeVal = impl_->builder->CreateZExt(storeVal, impl_->i64Type);
                    }
                    llvm::Value* tagVal = llvm::ConstantInt::get(impl_->i64Type, tag);
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_dict_int_set_tagged"], {dict, key, storeVal, tagVal});
                    continue;
                }

                if (impl_->options.gcMode == GCMode::RC && key &&
                    key->getType()->isPointerTy()) {
                    Expr* keyExpr = sub->index.get();
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

                if (val->getType() == impl_->boxType) {
                    auto* tagV = impl_->boxTag(val, "set.tag");
                    auto* payloadV = impl_->boxPayloadI64(val, "set.payload");
                    if (impl_->options.gcMode == GCMode::RC &&
                        !impl_->isOwnedBoxResult(val))
                        impl_->emitUnionIncref(payloadV, tagV);
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_dict_set_tagged"],
                        {dict, key, payloadV, tagV});
                    continue;
                }

                if (val->getType() == impl_->f64Type) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_dict_set_str_f64"], {dict, key, val});
                    continue;
                }
                if (val->getType()->isPointerTy()) {
                    int64_t tag = impl_->inferPtrValueTag(node.value.get());
                    llvm::Value* pval = val;
                    if (tag == 1) pval = impl_->ensureHeapString(pval, node.value.get());
                    if (impl_->options.gcMode == GCMode::RC &&
                        (tag == 1 || tag == 5 || tag == 6 || tag == 7 || tag == 10) &&
                        Impl::isBorrowedHeapExpr(node.value.get())) {
                        if (tag == 1)
                            impl_->builder->CreateCall(
                                impl_->runtimeFuncs["dragon_incref_str"], {pval});
                        else if (tag == 10)
                            impl_->builder->CreateCall(
                                impl_->runtimeFuncs["dragon_incref_callable"], {pval});
                        else
                            impl_->builder->CreateCall(
                                impl_->runtimeFuncs["dragon_incref"], {pval});
                    }
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_dict_set_str_ptr"],
                        {dict, key, pval, llvm::ConstantInt::get(impl_->i64Type, tag)});
                    continue;
                }
                llvm::Value* storeVal = val;
                int64_t tag = 0;
                if (storeVal->getType() == impl_->i1Type) {
                    tag = 3;
                    storeVal = impl_->builder->CreateZExt(storeVal, impl_->i64Type);
                }
                llvm::Value* tagVal = llvm::ConstantInt::get(impl_->i64Type, tag);
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_set_tagged"], {dict, key, storeVal, tagVal});
                continue;
            }
            bool isList = false;
            if (auto* objName = dynamic_cast<NameExpr*>(sub->object.get())) {
                isList = impl_->lookupVarKind(objName->name) == Impl::VarKind::List;
            } else if (auto* objAttr = dynamic_cast<AttributeExpr*>(sub->object.get())) {
                std::string cls = resolveAttrClass(objAttr);
                if (!cls.empty() && isListFieldOf(cls, objAttr->attribute))
                    isList = true;
            }
            if (!isList && sub->object->type &&
                sub->object->type->kind() == Type::Kind::List)
                isList = true;
            if (isList) {
                sub->object->accept(*this);
                llvm::Value* list = impl_->lastValue;
                sub->index->accept(*this);
                llvm::Value* idx = impl_->lastValue;
                if (idx->getType() == impl_->i1Type) {
                    idx = impl_->builder->CreateZExt(idx, impl_->i64Type);
                }

                // Element kind: varListElemKinds for a bare var, AST type for a
                // class field - else a list[instance] field store skips RC (UAF).
                Type::Kind setElemKind = Type::Kind::Int;
                if (auto* objName = dynamic_cast<NameExpr*>(sub->object.get())) {
                    auto it = impl_->varListElemKinds.find(objName->name);
                    if (it != impl_->varListElemKinds.end()) setElemKind = it->second;
                }
                if (setElemKind == Type::Kind::Int) {
                    Type::Kind astElemKind =
                        impl_->getIterableElementKind(sub->object.get());
                    if (astElemKind != Type::Kind::Int)
                        setElemKind = astElemKind;
                }

                if (impl_->getIterableElementKind(sub->object.get()) == Type::Kind::Any) {
                    auto tp = impl_->boxArgTagPayload(node.value.get(), val,
                                                      true);
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_list_box_set"],
                        {list, idx, tp.first, tp.second});
                    continue;
                }

                bool isPtrElem = (setElemKind == Type::Kind::Str      ||
                                  setElemKind == Type::Kind::Bytes    ||
                                  setElemKind == Type::Kind::List     ||
                                  setElemKind == Type::Kind::Dict     ||
                                  setElemKind == Type::Kind::Tuple    ||
                                  setElemKind == Type::Kind::Set      ||
                                  setElemKind == Type::Kind::Instance ||
                                  setElemKind == Type::Kind::Function);
                if (isPtrElem) {
                    llvm::Value* pval = val;
                    if (setElemKind == Type::Kind::Str && pval->getType()->isPointerTy())
                        pval = impl_->ensureHeapString(pval, node.value.get());
                    if (!pval->getType()->isPointerTy())
                        pval = impl_->builder->CreateIntToPtr(pval, impl_->i8PtrType);
                    if (impl_->options.gcMode == GCMode::RC &&
                        Impl::isBorrowedHeapExpr(node.value.get())) {
                        if (setElemKind == Type::Kind::Str)
                            impl_->builder->CreateCall(
                                impl_->runtimeFuncs["dragon_incref_str"], {pval});
                        else if (setElemKind == Type::Kind::Function)
                            impl_->builder->CreateCall(
                                impl_->runtimeFuncs["dragon_incref_callable"], {pval});
                        else
                            impl_->builder->CreateCall(
                                impl_->runtimeFuncs["dragon_incref"], {pval});
                    }
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_list_set_ptr"], {list, idx, pval});
                    continue;
                }

                llvm::Value* storeVal = val;
                if (storeVal->getType() == impl_->i1Type) {
                    storeVal = impl_->builder->CreateZExt(storeVal, impl_->i64Type);
                } else if (storeVal->getType() == impl_->f64Type) {
                    storeVal = impl_->builder->CreateBitCast(storeVal, impl_->i64Type);
                } else if (storeVal->getType()->isPointerTy()) {
                    storeVal = impl_->builder->CreatePtrToInt(storeVal, impl_->i64Type);
                }

                bool primElems = (setElemKind == Type::Kind::Int   ||
                                  setElemKind == Type::Kind::Float ||
                                  setElemKind == Type::Kind::Bool);

                if (primElems) {
                    auto* tbaaHdrTag = llvm::MDNode::get(*impl_->context,
                        {impl_->tbaaListHeader, impl_->tbaaListHeader,
                         llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(impl_->i64Type, 0))});
                    auto* tbaaDataTag = llvm::MDNode::get(*impl_->context,
                        {impl_->tbaaListData, impl_->tbaaListData,
                         llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(impl_->i64Type, 0))});

                    auto* dataGEP = impl_->builder->CreateGEP(impl_->i64Type, list,
                        llvm::ConstantInt::get(impl_->i64Type, 2), "lset.data.gep");
                    auto* dataRaw = impl_->builder->CreateLoad(impl_->i64Type, dataGEP, "lset.data.raw");
                    llvm::cast<llvm::Instruction>(dataRaw)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaHdrTag);
                    auto* dataPtr = impl_->builder->CreateIntToPtr(dataRaw, impl_->i8PtrType, "lset.data");
                    auto* sizeGEP = impl_->builder->CreateGEP(impl_->i64Type, list,
                        llvm::ConstantInt::get(impl_->i64Type, 3), "lset.size.gep");
                    auto* sizeLoad = impl_->builder->CreateLoad(impl_->i64Type, sizeGEP, "lset.size");
                    llvm::cast<llvm::Instruction>(sizeLoad)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaHdrTag);
                    auto* size = sizeLoad;

                    llvm::Value* finalIdx;
                    if (impl_->isExprDefinitelyNonNeg(sub->index.get())) {
                        finalIdx = idx;
                    } else {
                        auto* isNeg = impl_->builder->CreateICmpSLT(idx,
                            llvm::ConstantInt::get(impl_->i64Type, 0), "idx.neg");
                        auto* adjIdx = impl_->builder->CreateAdd(idx, size, "idx.adj");
                        finalIdx = impl_->builder->CreateSelect(isNeg, adjIdx, idx, "idx.final");
                    }

                    auto* inBounds = impl_->builder->CreateICmpULT(finalIdx, size, "idx.ok");
                    auto* func = impl_->currentFunction;
                    auto* okBB = llvm::BasicBlock::Create(*impl_->context, "lset.ok", func);
                    auto* oobBB = llvm::BasicBlock::Create(*impl_->context, "lset.oob", func);
                    impl_->builder->CreateCondBr(inBounds, okBB, oobBB);

                    impl_->builder->SetInsertPoint(oobBB);
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_list_set"], {list, idx, storeVal});
                    impl_->builder->CreateUnreachable();

                    impl_->builder->SetInsertPoint(okBB);
                    bool isBoolElem  = (setElemKind == Type::Kind::Bool);
                    bool isFloatElem = (setElemKind == Type::Kind::Float);
                    auto* i8Ty = llvm::Type::getInt8Ty(*impl_->context);
                    llvm::Type* strideTy;
                    llvm::Value* elemVal;
                    if (isBoolElem) {
                        strideTy = i8Ty;
                        elemVal = storeVal;
                        if (elemVal->getType() != i8Ty)
                            elemVal = impl_->builder->CreateTrunc(elemVal, i8Ty, "lset.elem.trunc");
                    } else if (isFloatElem) {
                        strideTy = impl_->f64Type;
                        elemVal = val;
                        if (elemVal->getType() == impl_->i64Type)
                            elemVal = impl_->builder->CreateSIToFP(elemVal, impl_->f64Type);
                        else if (elemVal->getType() == impl_->i1Type)
                            elemVal = impl_->builder->CreateUIToFP(elemVal, impl_->f64Type);
                    } else {
                        strideTy = impl_->i64Type;
                        elemVal = storeVal;
                    }
                    auto* elemGEP = impl_->builder->CreateGEP(strideTy, dataPtr,
                        finalIdx, "lset.elem.gep");
                    auto* elemStore = impl_->builder->CreateStore(elemVal, elemGEP);
                    elemStore->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaDataTag);
                } else {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_list_set"], {list, idx, storeVal});
                }
                continue;
            }
        }
        if (impl_->isDragonFile) {
            if (auto* attrTarget = dynamic_cast<AttributeExpr*>(target.get())) {
                if (auto* objName = dynamic_cast<NameExpr*>(attrTarget->object.get())) {
                    if (impl_->lookupVarKind(objName->name) == Impl::VarKind::Dict) {
                        attrTarget->object->accept(*this);
                        llvm::Value* dict = impl_->lastValue;
                        auto* keyStr = impl_->builder->CreateGlobalString(attrTarget->attribute);
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
                            if (tag == 1) storeVal = impl_->ensureHeapString(storeVal, node.value.get());
                            storeVal = impl_->builder->CreatePtrToInt(storeVal, impl_->i64Type);
                        }
                        llvm::Value* tagVal = llvm::ConstantInt::get(impl_->i64Type, tag);
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_dict_set_tagged"], {dict, keyStr, storeVal, tagVal});
                        continue;
                    }
                }
            }
        }
        if (auto* attrTarget = dynamic_cast<AttributeExpr*>(target.get())) {
            if (auto* objName = dynamic_cast<NameExpr*>(attrTarget->object.get())) {
                auto sfIt = impl_->staticFieldGlobalsBySym.find(impl_->classSym(objName->name));
                if (sfIt != impl_->staticFieldGlobalsBySym.end()) {
                    auto gvIt = sfIt->second.find(attrTarget->attribute);
                    if (gvIt != sfIt->second.end()) {
                        llvm::GlobalVariable* gv = gvIt->second;
                        llvm::Value* storeVal = val;
                        llvm::Type* fieldType = gv->getValueType();
                        if (storeVal->getType() != fieldType) {
                            if (fieldType == impl_->f64Type && storeVal->getType() == impl_->i64Type)
                                storeVal = impl_->builder->CreateSIToFP(storeVal, impl_->f64Type);
                            else if (fieldType == impl_->i64Type && storeVal->getType() == impl_->i1Type)
                                storeVal = impl_->builder->CreateZExt(storeVal, impl_->i64Type);
                            else if (fieldType == impl_->i64Type && storeVal->getType() == impl_->f64Type)
                                storeVal = impl_->builder->CreateFPToSI(storeVal, impl_->i64Type);
                        }
                        {
                            Impl::VarKind fieldKind = Impl::VarKind::Other;
                            auto fkIt = impl_->classFieldKindsBySym.find(impl_->classSym(objName->name));
                            if (fkIt != impl_->classFieldKindsBySym.end()) {
                                auto fkIt2 = fkIt->second.find(attrTarget->attribute);
                                if (fkIt2 != fkIt->second.end()) fieldKind = fkIt2->second;
                            }
                            Impl::VarKind newKind = Impl::VarKind::Other;
                            if (auto* sl = dynamic_cast<StringLiteral*>(node.value.get()))
                                newKind = (sl->isBytes ? Impl::VarKind::List : Impl::VarKind::StrLiteral);
                            else if (fieldKind != Impl::VarKind::Other)
                                newKind = fieldKind;
                            bool rhsBorrowed = Impl::isBorrowedHeapExpr(node.value.get());
                            impl_->storeWithRCOverwrite(
                                gv, fieldType, storeVal, fieldKind, newKind, rhsBorrowed,
                                objName->name + "." + attrTarget->attribute);
                        }
                        continue;
                    }
                }
            }
        }
        if (auto* attrTarget = dynamic_cast<AttributeExpr*>(target.get())) {
            {
                std::string className;
                if (auto* objName = dynamic_cast<NameExpr*>(attrTarget->object.get())) {
                    if (objName->name == "self" && !impl_->currentClassName.empty()) {
                        className = impl_->currentClassName;
                    } else {
                        auto vit = impl_->varClassNames.find(objName->name);
                        if (vit != impl_->varClassNames.end()) className = vit->second;
                    }
                } else {
                    className = impl_->resolveExprClassName(attrTarget->object.get());
                }
                if (!className.empty()) {
                    std::string setterClass;
                    std::string setterMethodName;
                    for (std::string cur = className; !cur.empty(); ) {
                        auto sit = impl_->classPropertySettersBySym.find(impl_->classSym(cur));
                        if (sit != impl_->classPropertySettersBySym.end()) {
                            auto mit = sit->second.find(attrTarget->attribute);
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
                    if (!setterClass.empty()) {
                        std::string setterFuncName =
                            impl_->classSymPrefix(setterClass) + "_" + setterMethodName;
                        auto* setterFn = impl_->module->getFunction(setterFuncName);
                        if (setterFn) {
                            attrTarget->object->accept(*this);
                            llvm::Value* obj = impl_->lastValue;
                            if (!obj->getType()->isPointerTy())
                                obj = impl_->builder->CreateIntToPtr(obj, impl_->i8PtrType);
                            std::vector<std::pair<llvm::Value*, Impl::VarKind>>
                                setterTemps;
                            Impl::VarKind rdk = impl_->ownedTempDrainKind(
                                attrTarget->object.get(), obj);
                            if (rdk != Impl::VarKind::Other)
                                setterTemps.emplace_back(obj, rdk);
                            if (node.targets.size() == 1) {
                                Impl::VarKind vdk = impl_->ownedTempDrainKind(
                                    node.value.get(), val);
                                if (vdk != Impl::VarKind::Other)
                                    setterTemps.emplace_back(val, vdk);
                            }
                            auto setterBases =
                                impl_->pushArgTempCleanups(setterTemps);
                            auto* fty = setterFn->getFunctionType();
                            llvm::Value* coerced = val;
                            if (fty->getNumParams() >= 2)
                                coerced = impl_->coerceArg(coerced, fty->getParamType(1));
                            impl_->builder->CreateCall(setterFn, {obj, coerced});
                            impl_->popArgTempCleanups(setterBases);
                            impl_->drainBorrowTemps(setterTemps);
                            continue;
                        }
                    }
                }
            }
        }

        if (auto* attrTarget = dynamic_cast<AttributeExpr*>(target.get())) {
            {
                std::string className;
                if (auto* objName = dynamic_cast<NameExpr*>(attrTarget->object.get())) {
                    if (objName->name == "self" && !impl_->currentClassName.empty()) {
                        className = impl_->currentClassName;
                    } else {
                        auto vit = impl_->varClassNames.find(objName->name);
                        if (vit != impl_->varClassNames.end()) className = vit->second;
                    }
                } else {
                    className = impl_->resolveExprClassName(attrTarget->object.get());
                }

                if (!className.empty()) {
                    auto structIt = impl_->classStructTypesBySym.find(impl_->classSym(className));
                    auto fieldIt = impl_->classFieldIndicesBySym.find(impl_->classSym(className));
                    if (structIt != impl_->classStructTypesBySym.end() && fieldIt != impl_->classFieldIndicesBySym.end()) {
                        auto idxIt = fieldIt->second.find(attrTarget->attribute);
                        if (idxIt != fieldIt->second.end()) {
                            attrTarget->object->accept(*this);
                            llvm::Value* objPtr = impl_->lastValue;
                            auto* gep = impl_->builder->CreateStructGEP(
                                structIt->second, objPtr, idxIt->second,
                                attrTarget->attribute + "_ptr");
                            auto* fieldType = impl_->classFieldTypesBySym[impl_->classSym(className)][attrTarget->attribute];
                            llvm::Value* storeVal = val;
                            if (storeVal->getType() != fieldType) {
                                if (fieldType == impl_->f64Type && storeVal->getType() == impl_->i64Type)
                                    storeVal = impl_->builder->CreateSIToFP(storeVal, impl_->f64Type);
                                else if (fieldType == impl_->i64Type && storeVal->getType() == impl_->i1Type)
                                    storeVal = impl_->builder->CreateZExt(storeVal, impl_->i64Type);
                                else if (fieldType == impl_->i64Type && storeVal->getType() == impl_->f64Type)
                                    storeVal = impl_->builder->CreateFPToSI(storeVal, impl_->i64Type);
                            }
                            {
                                auto cfIt = impl_->classFieldCallableTypeBySym.find(impl_->classSym(className));
                                if (cfIt != impl_->classFieldCallableTypeBySym.end() &&
                                    cfIt->second.count(attrTarget->attribute)) {
                                    llvm::Value* newPtr = storeVal;
                                    if (newPtr->getType()->isIntegerTy())
                                        newPtr = impl_->builder->CreateIntToPtr(
                                            newPtr, impl_->i8PtrType);
                                    else if (newPtr->getType() != impl_->i8PtrType &&
                                             newPtr->getType()->isPointerTy())
                                        newPtr = impl_->builder->CreateBitCast(
                                            newPtr, impl_->i8PtrType);
                                    if (Impl::isBorrowedHeapExpr(node.value.get()))
                                        impl_->builder->CreateCall(
                                            impl_->runtimeFuncs["dragon_incref_callable"],
                                            {newPtr});
                                    auto* oldVal = impl_->builder->CreateLoad(
                                        fieldType, gep,
                                        attrTarget->attribute + ".old");
                                    llvm::Value* oldPtr = oldVal;
                                    if (oldPtr->getType()->isIntegerTy())
                                        oldPtr = impl_->builder->CreateIntToPtr(
                                            oldPtr, impl_->i8PtrType);
                                    else if (oldPtr->getType() != impl_->i8PtrType &&
                                             oldPtr->getType()->isPointerTy())
                                        oldPtr = impl_->builder->CreateBitCast(
                                            oldPtr, impl_->i8PtrType);
                                    impl_->builder->CreateCall(
                                        impl_->runtimeFuncs["dragon_decref_callable"],
                                        {oldPtr});
                                    impl_->emitFieldSharedBarrier(
                                        objPtr, newPtr, Impl::VarKind::Closure);
                                    impl_->builder->CreateStore(storeVal, gep);
                                    continue;
                                }
                            }
                            {
                                Impl::VarKind fieldKind = Impl::VarKind::Other;
                                auto fkIt = impl_->classFieldKindsBySym.find(impl_->classSym(className));
                                if (fkIt != impl_->classFieldKindsBySym.end()) {
                                    auto fkIt2 = fkIt->second.find(attrTarget->attribute);
                                    if (fkIt2 != fkIt->second.end()) fieldKind = fkIt2->second;
                                }
                                Impl::VarKind newKind = Impl::VarKind::Other;
                                if (auto* sl = dynamic_cast<StringLiteral*>(node.value.get()))
                                    newKind = (sl->isBytes ? Impl::VarKind::List : Impl::VarKind::StrLiteral);
                                else if (fieldKind != Impl::VarKind::Other)
                                    newKind = fieldKind;
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
                                        ownedBoxPayload =
                                            impl_->boxPayloadI64(storeVal, "ownbox.pay");
                                        ownedBoxTag =
                                            impl_->boxTag(storeVal, "ownbox.tag");
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
                                    auto tp = impl_->boxArgTagPayload(
                                        node.value.get(), storeVal,
                                        true);
                                    storeVal = impl_->makeBox(tp.first, tp.second);
                                    rhsBorrowed = false;
                                }
                                impl_->emitFieldSharedBarrier(objPtr, storeVal, fieldKind);
                                impl_->storeWithRCOverwrite(
                                    gep, fieldType, storeVal, fieldKind, newKind, rhsBorrowed,
                                    className + "." + attrTarget->attribute);
                                impl_->emitMoveOutIfMarked(node.value.get());
                                if (ownedBoxUnboxed && rhsBorrowed)
                                    impl_->emitUnionDecref(ownedBoxPayload, ownedBoxTag);
                            }
                            continue;
                        }
                    }
                }
            }
        }
        if (auto* tupleTarget = dynamic_cast<TupleExpr*>(target.get())) {
            int64_t numTargets = tupleTarget->elements.size();

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
                if (dynamic_cast<StarredExpr*>(tupleTarget->elements[i].get())) {
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
                    if (auto* nameTarget = dynamic_cast<NameExpr*>(tupleTarget->elements[i].get())) {
                        auto* alloca = impl_->lookupVar(nameTarget->name);
                        bool hadSlot = (alloca != nullptr);
                        if (!alloca) {
                            alloca = impl_->createEntryAlloca(
                                impl_->currentFunction, nameTarget->name, impl_->i64Type);
                            impl_->setVar(nameTarget->name, alloca);
                        }
                        Impl::VarKind oldKind = hadSlot
                            ? impl_->lookupVarKind(nameTarget->name)
                            : Impl::VarKind::Other;
                        impl_->storeWithRCOverwrite(
                            alloca, impl_->i64Type, elem,
                            oldKind, Impl::VarKind::Other, true,
                            nameTarget->name);
                    }
                }

                int64_t postStarCount = numTargets - starIdx - 1;

                auto* starredExpr = dynamic_cast<StarredExpr*>(tupleTarget->elements[starIdx].get());
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
                    auto& elemExpr = tupleTarget->elements[starIdx + 1 + i];
                    if (auto* nameTarget = dynamic_cast<NameExpr*>(elemExpr.get())) {
                        auto* alloca = impl_->lookupVar(nameTarget->name);
                        bool hadSlot = (alloca != nullptr);
                        if (!alloca) {
                            alloca = impl_->createEntryAlloca(
                                impl_->currentFunction, nameTarget->name, impl_->i64Type);
                            impl_->setVar(nameTarget->name, alloca);
                        }
                        Impl::VarKind oldKind = hadSlot
                            ? impl_->lookupVarKind(nameTarget->name)
                            : Impl::VarKind::Other;
                        impl_->storeWithRCOverwrite(
                            alloca, impl_->i64Type, elem,
                            oldKind, Impl::VarKind::Other, true,
                            nameTarget->name);
                    }
                }
            } else {
                std::string getFuncName = elemGetFn;

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
                        impl_->runtimeFuncs[getFuncName], {val, idx}, "unpack");

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
                        case Type::Kind::List:
                        case Type::Kind::Dict:
                        case Type::Kind::Set:
                        case Type::Kind::Tuple:
                        case Type::Kind::Instance:
                        case Type::Kind::Ptr:
                            elem = impl_->builder->CreateIntToPtr(elem, impl_->i8PtrType, "unpack.ptr");
                            slotTy = impl_->i8PtrType;
                            newKind = Impl::typeKindToVarKind(ek);
                            if (ek == Type::Kind::Instance)
                                if (auto* it = dynamic_cast<InstanceType*>(et.get()))
                                    if (it->classType) elemClassName = it->classType->name;
                            break;
                        case Type::Kind::Int:
                        default:
                            newKind = Impl::VarKind::Int;
                            break;
                    }

                    if (auto* nameTarget = dynamic_cast<NameExpr*>(tupleTarget->elements[i].get())) {
                        bool unpackModuleLevel =
                            (impl_->currentFunction == impl_->mainFunction) &&
                            (impl_->scopes.size() <= impl_->moduleBodyScopeDepth);
                        if (unpackModuleLevel) {
                            std::string gKey = impl_->globalKeyOrOwn(nameTarget->name);
                            auto* gv = impl_->lookupModuleGlobal(nameTarget->name);
                            Impl::VarKind oldKind = gv
                                ? impl_->lookupVarKind(nameTarget->name)
                                : Impl::VarKind::Other;
                            if (!gv) {
                                gv = new llvm::GlobalVariable(
                                    *impl_->module, slotTy, false,
                                    llvm::GlobalValue::InternalLinkage,
                                    llvm::Constant::getNullValue(slotTy),
                                    "global." + gKey);
                                impl_->moduleGlobals[gKey] = gv;
                            }
                            impl_->moduleGlobalKinds[gKey] = newKind;
                            bool gborrowed = (slotTy == impl_->i8PtrType);
                            impl_->storeWithRCOverwrite(
                                gv, slotTy, elem, oldKind, newKind,
                                gborrowed, nameTarget->name);
                            if (!elemClassName.empty()) {
                                impl_->varClassNames[nameTarget->name] = elemClassName;
                                impl_->varClassOwningModule[nameTarget->name] =
                                    impl_->resolveClassOwningModule(elemClassName);
                                impl_->moduleGlobalClassNames[gKey] = {elemClassName,
                                    impl_->resolveClassOwningModule(elemClassName)};
                                if (impl_->options.gcMode == GCMode::RC)
                                    impl_->moduleGlobalKinds[gKey] =
                                        Impl::VarKind::ClassInstance;
                            }
                            if (impl_->options.gcMode == GCMode::RC &&
                                slotTy == impl_->i8PtrType) {
                                Impl::VarKind sk =
                                    impl_->moduleGlobalKinds[gKey];
                                if (sk == Impl::VarKind::Str) {
                                    impl_->builder->CreateCall(
                                        impl_->runtimeFuncs[node.isConst
                                            ? "dragon_str_make_immortal"
                                            : "dragon_mark_shared_str"],
                                        {elem});
                                } else {
                                    impl_->emitMarkSharedGlobal(elem, sk);
                                }
                            }
                        } else {
                            auto* alloca = impl_->lookupVar(nameTarget->name);
                            bool hadSlot = (alloca != nullptr);
                            if (!alloca) {
                                alloca = impl_->createEntryAlloca(
                                    impl_->currentFunction, nameTarget->name, slotTy);
                                impl_->setVar(nameTarget->name, alloca, newKind);
                            }
                            Impl::VarKind oldKind = hadSlot
                                ? impl_->lookupVarKind(nameTarget->name)
                                : Impl::VarKind::Other;
                            bool borrowed = (slotTy == impl_->i8PtrType);
                            impl_->storeWithRCOverwrite(
                                alloca, slotTy, elem,
                                oldKind, newKind, borrowed,
                                nameTarget->name);
                            if (!elemClassName.empty())
                                impl_->varClassNames[nameTarget->name] = elemClassName;
                        }
                    }
                }
                if (impl_->options.gcMode == GCMode::RC &&
                    val->getType()->isPointerTy() &&
                    !Impl::isBorrowedHeapExpr(node.value.get()) &&
                    impl_->isOwnedPtrResult(val)) {
                    impl_->emitDecrefByKind(val, Impl::VarKind::List);
                }
            }
            continue;
        }
        if (auto* name = dynamic_cast<NameExpr*>(target.get())) {
            if (impl_->isCellBacked(name->name)) {
                auto* alloca = impl_->lookupVar(name->name);
                Impl::VarKind cellKind = impl_->lookupVarKind(name->name);
                llvm::Value* coerced = val;
                if (cellKind == Impl::VarKind::Bool && coerced->getType() == impl_->i64Type)
                    coerced = impl_->builder->CreateICmpNE(
                        coerced, llvm::ConstantInt::get(impl_->i64Type, 0), "tobool");
                else if (cellKind == Impl::VarKind::Int && coerced->getType() == impl_->i1Type)
                    coerced = impl_->builder->CreateZExt(coerced, impl_->i64Type, "boolext");
                else if (cellKind == Impl::VarKind::Float && coerced->getType() == impl_->i64Type)
                    coerced = impl_->builder->CreateSIToFP(coerced, impl_->f64Type, "i2f");
                impl_->emitCellWrite(alloca, cellKind, coerced, name->name,
                                     Impl::isBorrowedHeapExpr(node.value.get()));
                continue;
            }
            auto* alloca = impl_->lookupVar(name->name);
            bool hadExistingSlot = (alloca != nullptr);
            bool rhsBorrowed = Impl::isBorrowedHeapExpr(node.value.get());
            auto inferIncomingKind = [&](llvm::Value* rhsVal) -> Impl::VarKind {
                if (dynamic_cast<ListExpr*>(node.value.get()) || dynamic_cast<ListCompExpr*>(node.value.get()))
                    return Impl::VarKind::List;
                if (dynamic_cast<DictExpr*>(node.value.get()) || dynamic_cast<DictCompExpr*>(node.value.get()))
                    return Impl::VarKind::Dict;
                if (dynamic_cast<TupleExpr*>(node.value.get()))
                    return Impl::VarKind::Tuple;
                if (dynamic_cast<SetExpr*>(node.value.get()) || dynamic_cast<SetCompExpr*>(node.value.get()))
                    return Impl::VarKind::Set;
                if (auto* sl = dynamic_cast<StringLiteral*>(node.value.get()))
                    return (sl->isBytes ? Impl::VarKind::List : Impl::VarKind::StrLiteral);
                if (auto* rhsName = dynamic_cast<NameExpr*>(node.value.get())) {
                    if (impl_->classNames.count(rhsName->name))
                        return Impl::VarKind::Type;
                    return impl_->lookupVarKind(rhsName->name);
                }
                if (auto* callExpr = dynamic_cast<CallExpr*>(node.value.get())) {
                    if (auto* calleeName = dynamic_cast<NameExpr*>(callExpr->callee.get())) {
                        if (impl_->funcReturnsType.count(impl_->resolveCalleeSymbol(calleeName->name)))
                            return Impl::VarKind::Type;
                    }
                }
                if (auto* sub = dynamic_cast<SubscriptExpr*>(node.value.get())) {
                    if (auto* objName = dynamic_cast<NameExpr*>(sub->object.get())) {
                        if (impl_->varDictValueIsType.count(objName->name))
                            return Impl::VarKind::Type;
                    }
                }
                if (node.value && node.value->type &&
                    node.value->type->kind() == Type::Kind::Function)
                    return Impl::VarKind::Closure;
                if (rhsVal->getType() == impl_->i64Type) return Impl::VarKind::Int;
                if (rhsVal->getType() == impl_->f64Type) return Impl::VarKind::Float;
                if (rhsVal->getType() == impl_->i1Type) return Impl::VarKind::Bool;
                if (rhsVal->getType() == impl_->i8PtrType &&
                    !dynamic_cast<NoneLiteral*>(node.value.get())) {
                    if (auto* callExpr = dynamic_cast<CallExpr*>(node.value.get())) {
                        if (auto* calleeName = dynamic_cast<NameExpr*>(callExpr->callee.get())) {
                            if (impl_->generatorFunctions.count(
                                    impl_->resolveCalleeSymbol(calleeName->name)))
                                return Impl::VarKind::Generator;
                            if (impl_->typedDictClassesBySym.count(impl_->classSym(calleeName->name)))
                                return Impl::VarKind::Dict;
                            if (impl_->classNames.count(calleeName->name))
                                return Impl::VarKind::ClassInstance;
                            if (impl_->funcReturnsType.count(impl_->resolveCalleeSymbol(calleeName->name)))
                                return Impl::VarKind::Type;
                            if (impl_->funcReturnsPtr.count(impl_->resolveCalleeSymbol(calleeName->name))) {
                                for (auto& t : node.targets) {
                                    if (auto* tgtName = dynamic_cast<NameExpr*>(t.get()))
                                        impl_->varIsPtrCallable.insert(tgtName->name);
                                }
                            }
                            if (impl_->lookupVarKind(calleeName->name) == Impl::VarKind::Type)
                                return Impl::VarKind::ClassInstance;
                        }
                    }
                    if (dynamic_cast<FireExpr*>(node.value.get()))
                        return Impl::VarKind::ClassInstance;
                    if (node.value->type) {
                        switch (node.value->type->kind()) {
                            case Type::Kind::Bytes:
                            case Type::Kind::List:
                            case Type::Kind::Dict:
                            case Type::Kind::Tuple:
                            case Type::Kind::Set:
                            case Type::Kind::Instance:
                                return Impl::typeKindToVarKind(node.value->type->kind());
                            case Type::Kind::Ptr:
                                return Impl::VarKind::Other;
                            default: break;
                        }
                    }
                    return Impl::VarKind::Str;
                }
                return Impl::VarKind::Other;
            };
            if (!alloca) {
                auto* gv = impl_->lookupModuleGlobal(name->name);
                if (gv && impl_->shouldUseModuleGlobal(name->name)) {
                    llvm::Type* gvType = gv->getValueType();
                    Impl::VarKind oldKind = impl_->lookupVarKind(name->name);
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
                        continue;
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
                    Impl::VarKind newKind = inferIncomingKind(val);
                    impl_->storeWithRCOverwrite(
                        gv, gvType, val, oldKind, newKind, rhsBorrowed, name->name);
                    if (newKind != Impl::VarKind::Other)
                        impl_->moduleGlobalKinds[impl_->globalKeyOrOwn(name->name)] = newKind;
                    impl_->emitMarkSharedGlobal(val, newKind);
                    continue;
                }
                if (impl_->currentFunction == impl_->mainFunction) {
                    std::string gKey = impl_->globalKeyOrOwn(name->name);
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
                        vk = (sl->isBytes ? Impl::VarKind::List : Impl::VarKind::StrLiteral);
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
                    if (vk != Impl::VarKind::Other) { }
                    else if (val->getType() == impl_->i64Type) vk = Impl::VarKind::Int;
                    else if (val->getType() == impl_->f64Type) vk = Impl::VarKind::Float;
                    else if (val->getType() == impl_->i1Type) vk = Impl::VarKind::Bool;
                    else if (val->getType() == impl_->i8PtrType && vk == Impl::VarKind::Other) {
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
                                    impl_->varIsPtrCallable.insert(name->name);
                            }
                        }
                        if (vk == Impl::VarKind::Other)
                            vk = Impl::VarKind::Str;
                    }
                    impl_->moduleGlobalKinds[gKey] = vk;
                    impl_->storeWithRCOverwrite(
                        gv, gv->getValueType(), val, Impl::VarKind::Other, vk, rhsBorrowed, name->name);
                    if (impl_->lastClosureCallableType) {
                        impl_->callableTypes[name->name] = impl_->lastClosureCallableType;
                        impl_->moduleGlobalKinds[gKey] = Impl::VarKind::Closure;
                        impl_->lastClosureCallableType = nullptr;
                    }
                    if (impl_->lastValueIsType) {
                        impl_->moduleGlobalKinds[gKey] = Impl::VarKind::Type;
                        impl_->lastValueIsType = false;
                    }
                    {
                        Impl::VarKind sharedKind = impl_->moduleGlobalKinds[gKey];
                        if (sharedKind == Impl::VarKind::StrLiteral &&
                            val->getType()->isPointerTy())
                            impl_->builder->CreateCall(
                                impl_->runtimeFuncs["dragon_str_make_immortal"], {val});
                        else
                            impl_->emitMarkSharedGlobal(val, sharedKind);
                    }
                    if (auto* lambdaFn = llvm::dyn_cast<llvm::Function>(val)) {
                        impl_->callableTypes[name->name] = lambdaFn->getFunctionType();
                    } else if (auto* rhsNameExpr = dynamic_cast<NameExpr*>(node.value.get())) {
                        if (!impl_->classNames.count(rhsNameExpr->name)) {
                            llvm::Function* refFunc = nullptr;
                            std::string aliasSym = impl_->lookupImportedAlias(rhsNameExpr->name);
                            if (!aliasSym.empty())
                                refFunc = impl_->module->getFunction(aliasSym);
                            if (!refFunc)
                                refFunc = impl_->module->getFunction(
                                    Impl::mangleFunc(impl_->currentModuleName, rhsNameExpr->name));
                            if (!refFunc)
                                refFunc = impl_->module->getFunction(
                                    Impl::userFuncName(rhsNameExpr->name));
                            if (!refFunc) refFunc = impl_->module->getFunction(rhsNameExpr->name);
                            if (refFunc)
                                impl_->callableTypes[name->name] = refFunc->getFunctionType();
                            auto ctIt = impl_->callableTypes.find(rhsNameExpr->name);
                            if (ctIt != impl_->callableTypes.end())
                                impl_->callableTypes[name->name] = ctIt->second;
                        }
                    }
                    if (auto* callVal = dynamic_cast<CallExpr*>(node.value.get())) {
                        if (auto* calleeName = dynamic_cast<NameExpr*>(callVal->callee.get())) {
                            if (impl_->typedDictClassesBySym.count(impl_->classSym(calleeName->name))) {
                                impl_->varTypedDictClass[name->name] = calleeName->name;
                            } else if (impl_->classNames.count(calleeName->name)) {
                                std::string ownMod =
                                    impl_->resolveClassOwningModule(calleeName->name);
                                impl_->varClassNames[name->name] = calleeName->name;
                                impl_->varClassOwningModule[name->name] = ownMod;
                                impl_->moduleGlobalClassNames[gKey] = {calleeName->name, ownMod};
                                if (impl_->options.gcMode == GCMode::RC)
                                    impl_->moduleGlobalKinds[gKey] = Impl::VarKind::ClassInstance;
                            } else if (impl_->lookupVarKind(calleeName->name) == Impl::VarKind::Type) {
                                if (impl_->options.gcMode == GCMode::RC)
                                    impl_->moduleGlobalKinds[gKey] = Impl::VarKind::ClassInstance;
                            }
                            if (calleeName->name == "Lock")
                                impl_->varClassNames[name->name] = "__Lock";
                            if (calleeName->name == "SyncList")
                                impl_->varClassNames[name->name] = "__SyncList";
                            if (calleeName->name == "SyncDict")
                                impl_->varClassNames[name->name] = "__SyncDict";
                            if (calleeName->name == "deque") {
                                impl_->varClassNames[name->name] = "__Deque";
                                impl_->moduleGlobalClassNames[gKey] = {"__Deque", ""};
                                impl_->moduleGlobalKinds[gKey] = Impl::VarKind::Deque;
                            }
                        }
                    }
                    if (dynamic_cast<FireExpr*>(node.value.get())) {
                        impl_->varClassNames[name->name] = "__Thread";
                    }
                    if (auto cls = impl_->recordVarClassFromValue(name->name, node.value.get());
                        !cls.empty()) {
                        impl_->moduleGlobalClassNames[gKey] =
                            {cls, impl_->varClassOwningModule[name->name]};
                        if (impl_->options.gcMode == GCMode::RC && impl_->classNames.count(cls))
                            impl_->moduleGlobalKinds[gKey] = Impl::VarKind::ClassInstance;
                    }
                    continue;
                }
                alloca = impl_->createEntryAlloca(
                    impl_->currentFunction, name->name, val->getType());
                impl_->setVar(name->name, alloca);
            }
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
                ? impl_->lookupVarKind(name->name)
                : Impl::VarKind::Other;
            Impl::VarKind newKind = inferIncomingKind(val);
            // A literal store must not downgrade an owned Str slot to StrLiteral
            // (cleanup would skip the decref); a BORROWED slot stays StrLiteral (UAF).
            if (newKind == Impl::VarKind::StrLiteral &&
                oldKind == Impl::VarKind::Str &&
                !impl_->isBorrowedSlot(name->name)) {
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
                if (val->getType() == impl_->boxType) {
                    if (impl_->options.gcMode == GCMode::RC) {
                        auto* oldBox = impl_->builder->CreateLoad(
                            impl_->boxType, alloca, "old.box");
                        auto* oldTag = impl_->boxTag(oldBox, "old.tag");
                        auto* oldPayload = impl_->boxPayloadI64(oldBox, "old.payload");
                        impl_->emitUnionDecref(oldPayload, oldTag);
                        auto* newTag = impl_->boxTag(val, "new.tag");
                        auto* newPayload = impl_->boxPayloadI64(val, "new.payload");
                        if (!impl_->isOwnedBoxResult(val))
                            impl_->emitUnionIncref(newPayload, newTag);
                    }
                    impl_->builder->CreateStore(val, alloca);
                    if (impl_->options.gcMode == GCMode::RC) {
                        auto* clTag = impl_->boxTag(val, "cl.tag");
                        auto* clPayload = impl_->boxPayloadI64(val, "cl.payload");
                        impl_->emitCleanupUpdate(name->name, clPayload, clTag);
                    }
                } else {
                    auto* newTag = impl_->emitTagForExpr(node.value.get(), *this);
                    if (impl_->options.gcMode == GCMode::RC) {
                        auto* oldBox = impl_->builder->CreateLoad(
                            impl_->boxType, alloca, "old.box");
                        auto* oldTag = impl_->boxTag(oldBox, "old.tag");
                        auto* oldPayload = impl_->boxPayloadI64(oldBox, "old.payload");
                        impl_->emitUnionDecref(oldPayload, oldTag);
                        if (rhsBorrowed) {
                            auto* newPayloadI64 = impl_->nativeToPayloadI64(val);
                            impl_->emitUnionIncref(newPayloadI64, newTag);
                        }
                    }
                    llvm::Value* boxVal = impl_->makeBox(newTag, val);
                    impl_->builder->CreateStore(boxVal, alloca);
                    if (impl_->options.gcMode == GCMode::RC) {
                        auto* clPayload = impl_->boxPayloadI64(boxVal, "cl.payload");
                        impl_->emitCleanupUpdate(name->name, clPayload, newTag);
                    }
                }
            } else {
                impl_->storeWithRCOverwrite(
                    alloca, allocType, val, oldKind, newKind, rhsBorrowed, name->name);
            }
            // Release the owned box's +1 only when the store took its own ref
            // (surplus); a fresh payload's +1 was adopted - releasing double-frees.
            if (ownedBoxUnboxed && rhsBorrowed)
                impl_->emitUnionDecref(ownedBoxPayload, ownedBoxTag);

            if (impl_->lastClosureCallableType) {
                impl_->callableTypes[name->name] = impl_->lastClosureCallableType;
                impl_->setVar(name->name, alloca, Impl::VarKind::Closure);
                impl_->lastClosureCallableType = nullptr;
            }
            else if (impl_->lastValueIsType) {
                impl_->setVar(name->name, alloca, Impl::VarKind::Type);
                impl_->lastValueIsType = false;
            }
            else if (oldKind == Impl::VarKind::Union) {
            } else if (didUnboxToNative) {
            } else if (newKind != Impl::VarKind::Other) {
                if (newKind == Impl::VarKind::Union &&
                    alloca->getAllocatedType() != impl_->boxType) {
                } else {
                    impl_->setVar(name->name, alloca, newKind);
                }
            } else if (auto* rhsName = dynamic_cast<NameExpr*>(node.value.get())) {
                auto rhsKind = impl_->lookupVarKind(rhsName->name);
                if (rhsKind != Impl::VarKind::Other &&
                    !(rhsKind == Impl::VarKind::Union &&
                      alloca->getAllocatedType() != impl_->boxType))
                    impl_->setVar(name->name, alloca, rhsKind);
            }
            if (auto* lambdaFn = llvm::dyn_cast<llvm::Function>(val)) {
                impl_->callableTypes[name->name] = lambdaFn->getFunctionType();
            } else if (auto* rhsName = dynamic_cast<NameExpr*>(node.value.get())) {
                if (!impl_->classNames.count(rhsName->name)) {
                    llvm::Function* refFunc = nullptr;
                    std::string aliasSym = impl_->lookupImportedAlias(rhsName->name);
                    if (!aliasSym.empty())
                        refFunc = impl_->module->getFunction(aliasSym);
                    if (!refFunc)
                        refFunc = impl_->module->getFunction(
                            Impl::mangleFunc(impl_->currentModuleName, rhsName->name));
                    if (!refFunc)
                        refFunc = impl_->module->getFunction(
                            Impl::userFuncName(rhsName->name));
                    if (!refFunc) refFunc = impl_->module->getFunction(rhsName->name);
                    if (refFunc) {
                        impl_->callableTypes[name->name] = refFunc->getFunctionType();
                    }
                    auto ctIt = impl_->callableTypes.find(rhsName->name);
                    if (ctIt != impl_->callableTypes.end()) {
                        impl_->callableTypes[name->name] = ctIt->second;
                    }
                }
            }

            if (auto* callVal = dynamic_cast<CallExpr*>(node.value.get())) {
                if (auto* calleeName = dynamic_cast<NameExpr*>(callVal->callee.get())) {
                    if (impl_->typedDictClassesBySym.count(impl_->classSym(calleeName->name))) {
                        impl_->varTypedDictClass[name->name] = calleeName->name;
                    } else if (impl_->classNames.count(calleeName->name)) {
                        impl_->varClassNames[name->name] = calleeName->name;
                        impl_->varClassOwningModule[name->name] =
                            impl_->resolveClassOwningModule(calleeName->name);
                        if (impl_->options.gcMode == GCMode::RC)
                            impl_->setVar(name->name, alloca, Impl::VarKind::ClassInstance);
                    } else if (impl_->lookupVarKind(calleeName->name) == Impl::VarKind::Type) {
                        if (impl_->options.gcMode == GCMode::RC)
                            impl_->setVar(name->name, alloca, Impl::VarKind::ClassInstance);
                    }
                    if (calleeName->name == "Lock") {
                        impl_->varClassNames[name->name] = "__Lock";
                        if (!impl_->scopes.empty())
                            impl_->scopes.back().lockDestroyOnExit.insert(
                                name->name);
                    }
                    if (calleeName->name == "SyncList") {
                        impl_->varClassNames[name->name] = "__SyncList";
                    }
                    if (calleeName->name == "SyncDict") {
                        impl_->varClassNames[name->name] = "__SyncDict";
                    }
                    if (calleeName->name == "deque") {
                        impl_->varClassNames[name->name] = "__Deque";
                        impl_->setVar(name->name, alloca, Impl::VarKind::Deque);
                    }
                }
            }
            if (dynamic_cast<FireExpr*>(node.value.get())) {
                impl_->varClassNames[name->name] = "__Thread";
            }
            if (auto cls = impl_->recordVarClassFromValue(name->name, node.value.get());
                !cls.empty()) {
                if (impl_->options.gcMode == GCMode::RC && impl_->classNames.count(cls))
                    impl_->setVar(name->name, alloca, Impl::VarKind::ClassInstance);
            }
        }
    }
}


}
