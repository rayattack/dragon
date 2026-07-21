/// Dragon CodeGen - Assignment (Assign, AugAssign, AnnAssign)
#include "../CodeGenImpl.h"

namespace dragon {

void CodeGen::visit(AssignStmt& node) {
    if (node.targets.empty()) return;

    // A capturing-lambda literal sets lastClosureCallableType (Functions.cpp),
    // consumed below to mark the assigned var VarKind::Closure. Reset it at
    // entry so it reflects ONLY this statement's RHS: a lambda consumed earlier
    // as a CALL ARG (e.g. `assertRaises(E, lambda ... {})`) leaves the flag set,
    // and the next assignment emitted - even in another function - would then
    // mis-mark its var a closure (e.g. `x: int = 42` increfing 42 as a heap
    // pointer -> SIGSEGV). The flag is consumed only by the assign visitors, so
    // clearing it here is sufficient and complete.
    impl_->lastClosureCallableType = nullptr;

    // In-place string append fast path for `s = s + x` (self-reassign).
    // The accumulator idiom `s = s + "..."` would otherwise lower to a fresh
    // dragon_str_concat each iteration -> O(n²). Recognize the exact shape
    // `T = T + rhs` (single NameExpr target; BinaryExpr(+) RHS whose left is the
    // same name) for a non-cell-backed Str slot, and route to the amortized
    // in-place append. Must run BEFORE node.value is visited - once the
    // BinaryExpr path runs it has already emitted the concat. All static checks
    // and slot resolution happen first, so a non-match falls through to the
    // normal path below without having evaluated (and thus without
    // double-evaluating) any operand.
    if (node.targets.size() == 1) {
        if (auto* tName = dynamic_cast<NameExpr*>(node.targets[0].get())) {
            if (auto* bin = dynamic_cast<BinaryExpr*>(node.value.get())) {
                auto* binLeft = dynamic_cast<NameExpr*>(bin->left.get());
                if (bin->op.type() == TokenType::PLUS && binLeft &&
                    binLeft->name == tName->name &&
                    !impl_->isCellBacked(tName->name)) {
                    auto vk = impl_->lookupVarKind(tName->name);
                    if (vk == Impl::VarKind::Str || vk == Impl::VarKind::StrLiteral) {
                        // Resolve the slot: local alloca, else a usable module
                        // global - mirroring the += path (AugAssignStmt).
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
                        // Commit only when the slot is a string pointer. The
                        // type system guarantees `str + rhs` ⇒ rhs is str, so
                        // evaluating only the right operand is sound.
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

    // Generate the value
    node.value->accept(*this);
    llvm::Value* val = impl_->lastValue;

    bool firstTargetDone = false;
    // 6.12(B): track non-negativity of plain-name targets so subsequent
    // subscripts can skip the negative-index correction.
    bool rhsNonNeg = impl_->isExprDefinitelyNonNeg(node.value.get());
    for (auto& target : node.targets) {
        if (auto* name = dynamic_cast<NameExpr*>(target.get())) {
            if (rhsNonNeg) impl_->knownNonNeg.insert(name->name);
            else           impl_->knownNonNeg.erase(name->name);
        }
        // Multiple assignment (a = b = expr): each additional target needs its
        // own reference. Incref the value for targets beyond the first.
        if (firstTargetDone && impl_->options.gcMode == GCMode::RC &&
            val->getType()->isPointerTy()) {
            // Determine incref variant based on RHS expression type
            auto tag = impl_->inferPtrValueTag(node.value.get());
            if (tag == 1) { // TAG_STR
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_incref_str"], {val});
            } else if (tag == 5 || tag == 6 || tag == 7) { // list/dict/bytes
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_incref"], {val});
            }
        }
        firstTargetDone = true;

        // __setitem__ dunder dispatch for class instances
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

        // Dict subscript assignment: d["key"] = value
        // Also: obj.field["key"] = value where field is a dict on obj's class.
        // Without the AttributeExpr branch the assignment silently no-ops -
        // every dict-field-mutation-through-method (e.g. Response.html()'s
        // `self.headers["content-type"] = "text/html"`) would lose the write.
        auto isDictFieldOf = [this](const std::string& className,
                                    const std::string& fieldName) -> bool {
            auto fkIt = impl_->classFieldKinds.find(className);
            if (fkIt == impl_->classFieldKinds.end()) return false;
            auto fkIt2 = fkIt->second.find(fieldName);
            if (fkIt2 == fkIt->second.end()) return false;
            return fkIt2->second == Impl::VarKind::Dict;
        };
        auto isListFieldOf = [this](const std::string& className,
                                    const std::string& fieldName) -> bool {
            auto fkIt = impl_->classFieldKinds.find(className);
            if (fkIt == impl_->classFieldKinds.end()) return false;
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
            // Nested base (`self.pager.pending[k] = v`): resolve the base
            // expression's static class so container-field stores through a
            // chain land exactly like single-level ones (they silently
            // no-oped before - caught by test_nested_attr_store.dr).
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
            if (isDict) {
                // D030 Phase 3.G: int-keyed dicts route through the
                // dragon_dict_int_* family. Resolve key kind from the dict
                // expression's tracked annotation BEFORE evaluating index so
                // we can branch the entire write path consistently.
                bool intKeyed = impl_->dictKeyIsInt(sub->object.get());

                sub->object->accept(*this);
                llvm::Value* dict = impl_->lastValue;
                sub->index->accept(*this);
                llvm::Value* key = impl_->lastValue;

                if (intKeyed) {
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

                // The runtime stores the key pointer directly (no copy or
                // refcount inside dragon_dict_set_tagged). When the key
                // expression is a string LITERAL it lives forever - safe to
                // store as-is. When the key is a borrowed heap str
                // (NameExpr / AttributeExpr / SubscriptExpr - e.g. `kv[0]`
                // unpacked from a tuple parameter inside an @x.setter body)
                // the source frees the str at scope exit, leaving the dict
                // pointing at recycled memory. Promote literals to heap and
                // incref borrowed heap keys so the dict owns a real ref.
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

                // D039 Phase 2 (write side): if the RHS is a box (e.g. from
                // a dict[str, Any] read, list[Any] subscript, Any local, or
                // function return -> Any), decompose into (payload, tag) and
                // store via dragon_dict_set_tagged. The dict stores the
                // payload by tag - same as if the source were a native value.
                // Refcount: the box's payload is a borrow from whatever
                // produced the box; the dict takes ownership of one ref, so
                // we incref the refcounted payloads before storing.
                if (val->getType() == impl_->boxType) {
                    auto* tagV = impl_->boxTag(val, "set.tag");
                    auto* payloadV = impl_->boxPayloadI64(val, "set.payload");
                    // An OWNED box temporary (box_binop / box_subscript) already
                    // carries the +1 the dict takes - don't double-count it.
                    if (impl_->options.gcMode == GCMode::RC &&
                        !impl_->isOwnedBoxResult(val))
                        impl_->emitUnionIncref(payloadV, tagV);
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_dict_set_tagged"],
                        {dict, key, payloadV, tagV});
                    continue;
                }

                // D030 Phase 3.F: dispatch by the value's native LLVM type so
                // f64 / ptr values cross the runtime boundary without bitcast.
                if (val->getType() == impl_->f64Type) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_dict_set_str_f64"], {dict, key, val});
                    continue;
                }
                if (val->getType()->isPointerTy()) {
                    int64_t tag = impl_->inferPtrValueTag(node.value.get());
                    llvm::Value* pval = val;
                    if (tag == 1) pval = impl_->ensureHeapString(pval, node.value.get());
                    // Model B: dict_set_str_ptr takes ownership of one ref.
                    // Borrowed sources need an incref so the dict's reference
                    // outlives the source's owning scope.
                    if (impl_->options.gcMode == GCMode::RC &&
                        (tag == 1 || tag == 5 || tag == 6 || tag == 7 || tag == 10) &&
                        Impl::isBorrowedHeapExpr(node.value.get())) {
                        if (tag == 1)
                            impl_->builder->CreateCall(
                                impl_->runtimeFuncs["dragon_incref_str"], {pval});
                        else if (tag == 10)
                            // borrowed Callable dict value - tag-gated incref
                            // (bare fn ptr safe). dict destroy decrefs via tag 10.
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
                // Int / Bool: legacy i64 path with per-entry tag.
                llvm::Value* storeVal = val;
                int64_t tag = 0;
                if (storeVal->getType() == impl_->i1Type) {
                    tag = 3; // bool
                    storeVal = impl_->builder->CreateZExt(storeVal, impl_->i64Type);
                }
                llvm::Value* tagVal = llvm::ConstantInt::get(impl_->i64Type, tag);
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_set_tagged"], {dict, key, storeVal, tagVal});
                continue;
            }
            // List subscript assignment: lst[i] = value
            // Also: obj.field[i] = value where field is a list on obj's class.
            bool isList = false;
            if (auto* objName = dynamic_cast<NameExpr*>(sub->object.get())) {
                isList = impl_->lookupVarKind(objName->name) == Impl::VarKind::List;
            } else if (auto* objAttr = dynamic_cast<AttributeExpr*>(sub->object.get())) {
                std::string cls = resolveAttrClass(objAttr);
                if (!cls.empty() && isListFieldOf(cls, objAttr->attribute))
                    isList = true;
            }
            if (isList) {
                sub->object->accept(*this);
                llvm::Value* list = impl_->lastValue;
                sub->index->accept(*this);
                llvm::Value* idx = impl_->lastValue;
                if (idx->getType() == impl_->i1Type) {
                    idx = impl_->builder->CreateZExt(idx, impl_->i64Type);
                }

                // Resolve element kind once (shared across the typed dispatch
                // and the legacy primitive-inline path). A bare-variable target
                // resolves via varListElemKinds; a class-field target
                // (`self._cookies[i] = c`, an AttributeExpr object) has no entry
                // there, so fall back to the AST list type via
                // getIterableElementKind - otherwise a `list[<instance>]` field
                // store wrongly takes the i64 path (no RC: the old element is
                // never decref'd and the new one never incref'd, so arg-temp
                // cleanup frees a still-listed instance -> UAF).
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

                // D039 Phase 4 (write side): list[Any] is a DragonListBox with
                // 16-byte {tag,payload} elements - route to dragon_list_box_set,
                // NOT dragon_list_set (which assumes 8-byte DragonList storage
                // and would scramble the boxes). Detection mirrors the read path
                // in Attributes.cpp (varListElemKinds, then the AST list type).
                // boxArgTagPayload boxes the RHS and increfs a borrowed source so
                // the list's owned ref outlives it (Model B; box_set decrefs the
                // overwritten element).
                if (impl_->getIterableElementKind(sub->object.get()) == Type::Kind::Any) {
                    auto tp = impl_->boxArgTagPayload(node.value.get(), val,
                                                      /*takesOwnership=*/true);
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_list_box_set"],
                        {list, idx, tp.first, tp.second});
                    continue;
                }

                // D030 Phase 3.C: list[<heap>] uses dragon_list_set_ptr which
                // takes a native pointer and handles refcount on overwrite.
                bool isPtrElem = (setElemKind == Type::Kind::Str      ||
                                  setElemKind == Type::Kind::Bytes    ||
                                  setElemKind == Type::Kind::List     ||
                                  setElemKind == Type::Kind::Dict     ||
                                  setElemKind == Type::Kind::Tuple    ||
                                  setElemKind == Type::Kind::Set      ||
                                  setElemKind == Type::Kind::Instance ||
                                  setElemKind == Type::Kind::Function);  //
                                  // list[Callable] element is a refcounted closure
                                  // ptr - route the overwrite through set_ptr so the
                                  // old closure is decref'd and the new one's
                                  // ownership is handled (else it stores raw i64 with
                                  // no RC and the overwritten closure leaks).
                if (isPtrElem) {
                    llvm::Value* pval = val;
                    if (setElemKind == Type::Kind::Str && pval->getType()->isPointerTy())
                        pval = impl_->ensureHeapString(pval, node.value.get());
                    if (!pval->getType()->isPointerTy())
                        pval = impl_->builder->CreateIntToPtr(pval, impl_->i8PtrType);
                    // Model B: dragon_list_set_ptr decrefs the old element and
                    // takes ownership of the new one. Borrowed sources need an
                    // incref so the list's reference outlives the source's
                    // owning scope.
                    if (impl_->options.gcMode == GCMode::RC &&
                        Impl::isBorrowedHeapExpr(node.value.get())) {
                        if (setElemKind == Type::Kind::Str)
                            impl_->builder->CreateCall(
                                impl_->runtimeFuncs["dragon_incref_str"], {pval});
                        else if (setElemKind == Type::Kind::Function)
                            // tag-gated - element may be a bare fn ptr.
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

                // Check if elements are primitive (no decref needed on overwrite).
                // After the heap dispatch above, only Int/Float/Bool remain primitive.
                bool primElems = (setElemKind == Type::Kind::Int   ||
                                  setElemKind == Type::Kind::Float ||
                                  setElemKind == Type::Kind::Bool);

                if (primElems) {
                    // Inline list set for primitive elements (no GC needed)
                    // TBAA tags: header fields vs array data (enables LICM)
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

                    // 6.12(B) Negative-index elision (see Attributes.cpp).
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

                    // OOB: dragon_list_set prints error and exits
                    impl_->builder->SetInsertPoint(oobBB);
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_list_set"], {list, idx, storeVal});
                    impl_->builder->CreateUnreachable();

                    impl_->builder->SetInsertPoint(okBB);
                    // D030 Phase 3.C: pick GEP/store type matching the list
                    // variant - same dispatch as the read path in Attributes.cpp.
                    //  Bool -> i8 (DragonList 1-byte packing, D028)
                    //  Float -> f64 (DragonListF64 native double*)
                    //  else -> i64 (DragonList int*)
                    bool isBoolElem  = (setElemKind == Type::Kind::Bool);
                    bool isFloatElem = (setElemKind == Type::Kind::Float);
                    auto* i8Ty = llvm::Type::getInt8Ty(*impl_->context);
                    llvm::Type* strideTy;
                    llvm::Value* elemVal;
                    if (isBoolElem) {
                        strideTy = i8Ty;
                        elemVal = storeVal;  // i64 from earlier coercion
                        if (elemVal->getType() != i8Ty)
                            elemVal = impl_->builder->CreateTrunc(elemVal, i8Ty, "lset.elem.trunc");
                    } else if (isFloatElem) {
                        strideTy = impl_->f64Type;
                        // Use the original native value, not the i64-bitcast.
                        // Promote int / bool literals to f64 if needed.
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
                    // Heap-element lists: use runtime call for correct GC
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_list_set"], {list, idx, storeVal});
                }
                continue;
            }
        }
        // Dict dot-access write (.dr mode): data.name = val -> dragon_dict_set_tagged(data, "name", val, tag)
        if (impl_->isDragonFile) {
            if (auto* attrTarget = dynamic_cast<AttributeExpr*>(target.get())) {
                if (auto* objName = dynamic_cast<NameExpr*>(attrTarget->object.get())) {
                    if (impl_->lookupVarKind(objName->name) == Impl::VarKind::Dict) {
                        attrTarget->object->accept(*this);
                        llvm::Value* dict = impl_->lastValue;
                        auto* keyStr = impl_->builder->CreateGlobalString(attrTarget->attribute);
                        llvm::Value* storeVal = val;
                        int64_t tag = 0; // default: int
                        if (storeVal->getType() == impl_->i1Type) {
                            tag = 3; // bool
                            storeVal = impl_->builder->CreateZExt(storeVal, impl_->i64Type);
                        } else if (storeVal->getType() == impl_->f64Type) {
                            tag = 2; // float
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
        // Static field assignment: ClassName.field = val
        if (auto* attrTarget = dynamic_cast<AttributeExpr*>(target.get())) {
            if (auto* objName = dynamic_cast<NameExpr*>(attrTarget->object.get())) {
                auto sfIt = impl_->staticFieldGlobals.find(objName->name);
                if (sfIt != impl_->staticFieldGlobals.end()) {
                    auto gvIt = sfIt->second.find(attrTarget->attribute);
                    if (gvIt != sfIt->second.end()) {
                        llvm::GlobalVariable* gv = gvIt->second;
                        llvm::Value* storeVal = val;
                        llvm::Type* fieldType = gv->getValueType();
                        // Type coercion to match the global's type
                        if (storeVal->getType() != fieldType) {
                            if (fieldType == impl_->f64Type && storeVal->getType() == impl_->i64Type)
                                storeVal = impl_->builder->CreateSIToFP(storeVal, impl_->f64Type);
                            else if (fieldType == impl_->i64Type && storeVal->getType() == impl_->i1Type)
                                storeVal = impl_->builder->CreateZExt(storeVal, impl_->i64Type);
                            else if (fieldType == impl_->i64Type && storeVal->getType() == impl_->f64Type)
                                storeVal = impl_->builder->CreateFPToSI(storeVal, impl_->i64Type);
                        }
                        // RC overwrite: decref old static field value before storing new
                        {
                            Impl::VarKind fieldKind = Impl::VarKind::Other;
                            auto fkIt = impl_->classFieldKinds.find(objName->name);
                            if (fkIt != impl_->classFieldKinds.end()) {
                                auto fkIt2 = fkIt->second.find(attrTarget->attribute);
                                if (fkIt2 != fkIt->second.end()) fieldKind = fkIt2->second;
                            }
                            // Infer newKind from RHS
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
        // 4.1 @property: setter dispatch on instance.x = val
        // Walk inheritance so subclasses inherit parent setters. Falls through to
        // field write when no setter is registered for this attribute.
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
                    // Nested base (`a.b.prop = v`): resolve the base
                    // expression's static class so chained targets dispatch
                    // setters exactly like single-level ones.
                    className = impl_->resolveExprClassName(attrTarget->object.get());
                }
                if (!className.empty()) {
                    std::string setterClass;
                    std::string setterMethodName;
                    for (std::string cur = className; !cur.empty(); ) {
                        auto sit = impl_->classPropertySetters.find(cur);
                        if (sit != impl_->classPropertySetters.end()) {
                            auto mit = sit->second.find(attrTarget->attribute);
                            if (mit != sit->second.end()) {
                                setterClass = cur;
                                setterMethodName = mit->second;
                                break;
                            }
                        }
                        auto pp = impl_->classParentNames.find(cur);
                        if (pp == impl_->classParentNames.end()) break;
                        cur = pp->second;
                    }
                    if (!setterClass.empty()) {
                        // Mangle with the owning module (same fix as the getter
                        // in Attributes.cpp): a cross-module setter symbol is
                        // `<mod>__<Class>_<setter>`, not the bare form.
                        std::string setterFuncName =
                            impl_->classSymPrefix(setterClass) + "_" + setterMethodName;
                        auto* setterFn = impl_->module->getFunction(setterFuncName);
                        if (setterFn) {
                            attrTarget->object->accept(*this);
                            llvm::Value* obj = impl_->lastValue;
                            if (!obj->getType()->isPointerTy())
                                obj = impl_->builder->CreateIntToPtr(obj, impl_->i8PtrType);
                            auto* fty = setterFn->getFunctionType();
                            llvm::Value* coerced = val;
                            if (fty->getNumParams() >= 2)
                                coerced = impl_->coerceArg(coerced, fty->getParamType(1));
                            impl_->builder->CreateCall(setterFn, {obj, coerced});
                            continue;
                        }
                    }
                }
            }
        }

        // Class field assignment: self.x = val or instance.x = val.
        // The base may itself be an attribute chain (`self.pager.log_tail = v`,
        // `o.mid.inner.a = v`): resolve its static class and store through the
        // same GEP path. Without the nested-base case the assignment silently
        // no-ops - it compiles, runs, and changes nothing (caught by
        // test_nested_attr_store.dr).
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
                    auto structIt = impl_->classStructTypes.find(className);
                    auto fieldIt = impl_->classFieldIndices.find(className);
                    if (structIt != impl_->classStructTypes.end() && fieldIt != impl_->classFieldIndices.end()) {
                        auto idxIt = fieldIt->second.find(attrTarget->attribute);
                        if (idxIt != fieldIt->second.end()) {
                            // Load object pointer
                            attrTarget->object->accept(*this);
                            llvm::Value* objPtr = impl_->lastValue;
                            // GEP to field
                            auto* gep = impl_->builder->CreateStructGEP(
                                structIt->second, objPtr, idxIt->second,
                                attrTarget->attribute + "_ptr");
                            // Coerce value type to match field type
                            auto* fieldType = impl_->classFieldTypes[className][attrTarget->attribute];
                            llvm::Value* storeVal = val;
                            if (storeVal->getType() != fieldType) {
                                if (fieldType == impl_->f64Type && storeVal->getType() == impl_->i64Type)
                                    storeVal = impl_->builder->CreateSIToFP(storeVal, impl_->f64Type);
                                else if (fieldType == impl_->i64Type && storeVal->getType() == impl_->i1Type)
                                    storeVal = impl_->builder->CreateZExt(storeVal, impl_->i64Type);
                                else if (fieldType == impl_->i64Type && storeVal->getType() == impl_->f64Type)
                                    storeVal = impl_->builder->CreateFPToSI(storeVal, impl_->i64Type);
                            }
                            // Bug A: Callable[[...], R] field - value at runtime is
                            // either a bare LLVM fn pointer (no header, no RC) or a
                            // DragonClosure* (header at offset 0, type_tag = 10).
                            // The class field owns a reference, so a capturing closure
                            // outlives the local that produced it - incref new + decref
                            // old via tag-aware runtime helpers (no-op when the value
                            // is a bare fn pointer).
                            {
                                auto cfIt = impl_->classFieldCallableType.find(className);
                                if (cfIt != impl_->classFieldCallableType.end() &&
                                    cfIt->second.count(attrTarget->attribute)) {
                                    llvm::Value* newPtr = storeVal;
                                    if (newPtr->getType()->isIntegerTy())
                                        newPtr = impl_->builder->CreateIntToPtr(
                                            newPtr, impl_->i8PtrType);
                                    else if (newPtr->getType() != impl_->i8PtrType &&
                                             newPtr->getType()->isPointerTy())
                                        newPtr = impl_->builder->CreateBitCast(
                                            newPtr, impl_->i8PtrType);
                                    // incref the NEW closure only when it is a
                                    // BORROW (a param/field/subscript read the field
                                    // must outlive). An OWNED temporary (a closure
                                    // literal or a closure-returning call) already
                                    // carries the +1 the field takes - incref'ing it
                                    // too would leak. (dragon_incref_callable is
                                    // tag-gated, so it also no-ops on a bare fn ptr.)
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
                                    // D018 field write barrier (Callable form)
                                    impl_->emitFieldSharedBarrier(
                                        objPtr, newPtr, Impl::VarKind::Closure);
                                    impl_->builder->CreateStore(storeVal, gep);
                                    continue;
                                }
                            }
                            // RC overwrite: decref old instance field value before storing new
                            {
                                Impl::VarKind fieldKind = Impl::VarKind::Other;
                                auto fkIt = impl_->classFieldKinds.find(className);
                                if (fkIt != impl_->classFieldKinds.end()) {
                                    auto fkIt2 = fkIt->second.find(attrTarget->attribute);
                                    if (fkIt2 != fkIt->second.end()) fieldKind = fkIt2->second;
                                }
                                // Infer newKind from RHS
                                Impl::VarKind newKind = Impl::VarKind::Other;
                                if (auto* sl = dynamic_cast<StringLiteral*>(node.value.get()))
                                    newKind = (sl->isBytes ? Impl::VarKind::List : Impl::VarKind::StrLiteral);
                                else if (fieldKind != Impl::VarKind::Other)
                                    newKind = fieldKind;
                                bool rhsBorrowed = Impl::isBorrowedHeapExpr(node.value.get());
                                // D039 box -> native unbox for a TYPED field, the
                                // mirror of the local-slot path above. Without it
                                // `self.name = d["name"]` (d: dict[str, Any])
                                // stored the 16-byte {tag, payload} box over the
                                // 8-byte field slot: the write spilled into the
                                // NEXT field, an int field read back tag bits, and
                                // a str field held a non-pointer the shared-mark
                                // walk / dealloc then dereferenced (SEGV,
                                // test_any_field_store.dr). Extract the payload at
                                // the field's natural type; ownership follows the
                                // local-slot protocol exactly.
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
                                    // A BORROWED box's payload is still owned by
                                    // its container, so the field must take its
                                    // own reference; an OWNED box's +1 transfers
                                    // to the field (surplus released below when
                                    // the store increfs independently).
                                    if (impl_->options.gcMode == GCMode::RC &&
                                        !ownedBoxUnboxed && fieldType->isPointerTy())
                                        rhsBorrowed = true;
                                }
                                // An Any/Union field slot is a {tag, payload} box
                                // VALUE. A NATIVE RHS (`k.v = "d" + s`, `k.v = 5`)
                                // must be boxed here: the raw store wrote the
                                // native value over the box's TAG half - reads saw
                                // pointer bits as the tag (isinstance silently
                                // false), the stored heap value leaked (nothing
                                // could ever read or release it), and dealloc
                                // tag-dispatched on garbage (test_rc_any_field.dr).
                                // boxArgTagPayload settles ownership Model-B style
                                // (increfs a borrowed source; an owned temp's +1
                                // transfers to the box), so the slot store must
                                // not incref again.
                                if (fieldKind == Impl::VarKind::Union &&
                                    storeVal->getType() != impl_->boxType) {
                                    auto tp = impl_->boxArgTagPayload(
                                        node.value.get(), storeVal,
                                        /*takesOwnership=*/true);
                                    storeVal = impl_->makeBox(tp.first, tp.second);
                                    rhsBorrowed = false;
                                }
                                // A heap value stored into a SHARED instance's field becomes
                                // globally reachable - mark it (inline-gated on the object's SHARED bit).
                                impl_->emitFieldSharedBarrier(objPtr, storeVal, fieldKind);
                                impl_->storeWithRCOverwrite(
                                    gep, fieldType, storeVal, fieldKind, newKind, rhsBorrowed,
                                    className + "." + attrTarget->attribute);
                                // `self._f = own x`: the field adopted the +1
                                // (rhsBorrowed=false via isBorrowedHeapExpr's
                                // move exception); null the source so scope
                                // exit sees nothing (docs/002 2.4 row 3).
                                impl_->emitMoveOutIfMarked(node.value.get());
                                // Release the OWNED box temporary's +1 when the
                                // store took its own independent ref (same rule
                                // as the local-slot path: adopt when
                                // rhsBorrowed=false, release the surplus when
                                // the store increfed). Tag-gated: no-op for a
                                // non-heap payload.
                                if (ownedBoxUnboxed && rhsBorrowed)
                                    impl_->emitUnionDecref(ownedBoxPayload, ownedBoxTag);
                            }
                            continue;
                        }
                    }
                }
            }
        }
        // Tuple unpacking: a, b = (1, 2) or a, b = func()
        if (auto* tupleTarget = dynamic_cast<TupleExpr*>(target.get())) {
            // val is a pointer to a DragonTuple or DragonList - extract elements
            int64_t numTargets = tupleTarget->elements.size();

            // DragonTuple and DragonList are DIFFERENT structs: their data/
            // length fields happen to align, but DragonList::elem_size (which
            // dragon_list_get's loader reads) lies PAST a DragonTuple's
            // allocation - an out-of-bounds read that can silently misload
            // every element if the stray byte reads 1. So the get function
            // MUST match the RHS's actual runtime shape. Detect tuples by
            // literal, by tracked VarKind, and - crucially - by the
            // typechecker-propagated static type, which covers call results
            // (`q, r = divmod(a, b)`, `h, s, v = rgb_to_hsv(...)`,
            // `a, b, c = s.rpartition("-")`).
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

            // Find starred target index (if any) for starred unpacking
            int64_t starIdx = -1;
            for (int64_t i = 0; i < numTargets; i++) {
                if (dynamic_cast<StarredExpr*>(tupleTarget->elements[i].get())) {
                    starIdx = i;
                    break;
                }
            }

            if (starIdx >= 0) {
                // Starred unpacking: first, *rest, last = iterable
                // Get total length of iterable
                // DragonTuple::length aliases DragonList::size (offset 24),
                // so dragon_list_len reads correctly for both shapes; the
                // per-element gets below must still dispatch by shape.
                llvm::Value* totalLen = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_list_len"], {val}, "totallen");

                // Pre-star elements
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
                            oldKind, Impl::VarKind::Other, /*newIsBorrowed=*/true,
                            nameTarget->name);
                    }
                }

                // Post-star elements count
                int64_t postStarCount = numTargets - starIdx - 1;

                // Starred element: collect middle into a new list
                auto* starredExpr = dynamic_cast<StarredExpr*>(tupleTarget->elements[starIdx].get());
                if (auto* starName = dynamic_cast<NameExpr*>(starredExpr->value.get())) {
                    // rest_len = total_len - pre_star - post_star
                    llvm::Value* preCount = llvm::ConstantInt::get(impl_->i64Type, starIdx);
                    llvm::Value* postCount = llvm::ConstantInt::get(impl_->i64Type, postStarCount);
                    llvm::Value* restLen = impl_->builder->CreateSub(totalLen, preCount);
                    restLen = impl_->builder->CreateSub(restLen, postCount, "restlen");

                    // Create new list for starred target
                    llvm::Value* capVal = llvm::ConstantInt::get(impl_->i64Type, 8);
                    llvm::Value* restList = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_list_new"], {capVal}, "restlist");

                    // Loop: for j in range(rest_len): rest.append(source[starIdx + j])
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

                // Post-star elements
                for (int64_t i = 0; i < postStarCount; i++) {
                    // idx = total_len - post_star_count + i
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
                            oldKind, Impl::VarKind::Other, /*newIsBorrowed=*/true,
                            nameTarget->name);
                    }
                }
            } else {
                // Simple tuple unpacking (no starred): a, b, c = (1, 2, 3).
                // Shape detection hoisted above (covers literals, tracked
                // VarKind, and static-typed call results).
                std::string getFuncName = elemGetFn;

                // Per-element static type from the RHS tuple/list, so a float
                // element is bitcast back to f64 (not left as raw bits), a str/
                // container is reconstituted as a ptr, etc. - mirroring the
                // tuple-subscript coercion in Attributes.cpp. Without this, the
                // i64 from dragon_tuple_get was stored verbatim and `a, b =
                // f()` on a tuple[float,...] printed the float's bit pattern.
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

                    // Coerce the i64 storage value to the element's native type.
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
                        // MODULE-LEVEL unpack (`const a: T, b: U = f()` at the
                        // top level): the targets are module globals - the type
                        // checker registers them in module scope and accepts
                        // cross-function references, so lowering them as
                        // main-frame allocas (the old behavior) made codegen
                        // reject the very programs the checker passed
                        // ("Undefined variable: a" from another function).
                        // Mirror the AnnAssign module-global path: a real
                        // GlobalVariable + kind tracking + the D018 marking
                        // (globals are reachable from every vthread by name).
                        bool unpackModuleLevel =
                            (impl_->currentFunction == impl_->mainFunction) &&
                            (impl_->scopes.size() <= impl_->moduleBodyScopeDepth);
                        if (unpackModuleLevel) {
                            auto* gv = impl_->lookupModuleGlobal(nameTarget->name);
                            Impl::VarKind oldKind = gv
                                ? impl_->lookupVarKind(nameTarget->name)
                                : Impl::VarKind::Other;
                            if (!gv) {
                                gv = new llvm::GlobalVariable(
                                    *impl_->module, slotTy, /*isConstant=*/false,
                                    llvm::GlobalValue::InternalLinkage,
                                    llvm::Constant::getNullValue(slotTy),
                                    "global." + nameTarget->name);
                                impl_->moduleGlobals[nameTarget->name] = gv;
                            }
                            impl_->moduleGlobalKinds[nameTarget->name] = newKind;
                            bool gborrowed = (slotTy == impl_->i8PtrType);
                            impl_->storeWithRCOverwrite(
                                gv, slotTy, elem, oldKind, newKind,
                                /*newIsBorrowed=*/gborrowed, nameTarget->name);
                            if (!elemClassName.empty()) {
                                impl_->varClassNames[nameTarget->name] = elemClassName;
                                impl_->varClassOwningModule[nameTarget->name] =
                                    impl_->resolveClassOwningModule(elemClassName);
                                if (impl_->options.gcMode == GCMode::RC)
                                    impl_->moduleGlobalKinds[nameTarget->name] =
                                        Impl::VarKind::ClassInstance;
                            }
                            // D018 completion, same rules as the AnnAssign
                            // hook: const str -> immortal (never reassigned,
                            // immutable leaf); everything else heap -> SHARED.
                            if (impl_->options.gcMode == GCMode::RC &&
                                slotTy == impl_->i8PtrType) {
                                Impl::VarKind sk =
                                    impl_->moduleGlobalKinds[nameTarget->name];
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
                            // Heap elements come borrowed from the tuple/list get.
                            bool borrowed = (slotTy == impl_->i8PtrType);
                            impl_->storeWithRCOverwrite(
                                alloca, slotTy, elem,
                                oldKind, newKind, /*newIsBorrowed=*/borrowed,
                                nameTarget->name);
                            if (!elemClassName.empty())
                                impl_->varClassNames[nameTarget->name] = elemClassName;
                        }
                    }
                }
                // Each heap element above was increfed into its slot (scalars
                // hold no ref), so the RHS container's own +1 refs are now
                // redundant. If the RHS is an OWNED temp (a call result -
                // divmod(...), s.partition(...) - or a tuple/list literal),
                // nobody else frees it: the struct + buffer + its element refs
                // leak (`q, r = divmod(...)`). Drop it here.
                // A BORROWED source (Name / attribute / element read) is left to
                // its owner. dragon_decref dispatches on the header, freeing both
                // DragonTuple and DragonList shapes and decref'ing each element
                // (heap: -1 leaves the slot's +1; scalar: no-op). Scoped to this
                // simple-unpack branch ONLY - the starred path stores its
                // elements borrowed (non-refcounted), so freeing the source there
                // would dangle them.
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
            // D027.1: cell-backed (nonlocal-mutable) write - route through
            // dragon_cell_set so the mutation lands in the same backing slot
            // every reader and the owner observe. Bypasses the storeWithRC /
            // shadow-define path entirely; cell ops carry their own RC.
            if (impl_->isCellBacked(name->name)) {
                auto* alloca = impl_->lookupVar(name->name);
                Impl::VarKind cellKind = impl_->lookupVarKind(name->name);
                llvm::Value* coerced = val;
                // Mirror the small coercions storeWithRCOverwrite applies for
                // bool/float/ptr boundary cases - a `.find(...) >= 0` (i1) into
                // a Bool cell, a Float cell from an int literal, etc.
                if (cellKind == Impl::VarKind::Bool && coerced->getType() == impl_->i64Type)
                    coerced = impl_->builder->CreateICmpNE(
                        coerced, llvm::ConstantInt::get(impl_->i64Type, 0), "tobool");
                else if (cellKind == Impl::VarKind::Int && coerced->getType() == impl_->i1Type)
                    coerced = impl_->builder->CreateZExt(coerced, impl_->i64Type, "boolext");
                else if (cellKind == Impl::VarKind::Float && coerced->getType() == impl_->i64Type)
                    coerced = impl_->builder->CreateSIToFP(coerced, impl_->f64Type, "i2f");
                // Ownedness matters: `acc = acc + s` hands the cell an OWNED
                // fresh concat (+1 already) - incref'ing it too leaked one
                // string per nonlocal mutation. A borrowed RHS (name/field/
                // element read) still gets the cell's own incref.
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
                    // Decision 025: bare class name -> VarKind::Type
                    if (impl_->classNames.count(rhsName->name))
                        return Impl::VarKind::Type;
                    return impl_->lookupVarKind(rhsName->name);
                }
                // D025: call to a function whose return type is `type`
                if (auto* callExpr = dynamic_cast<CallExpr*>(node.value.get())) {
                    if (auto* calleeName = dynamic_cast<NameExpr*>(callExpr->callee.get())) {
                        if (impl_->funcReturnsType.count(calleeName->name))
                            return Impl::VarKind::Type;
                    }
                }
                // D025: subscript of a dict[K, type] yields a class descriptor
                if (auto* sub = dynamic_cast<SubscriptExpr*>(node.value.get())) {
                    if (auto* objName = dynamic_cast<NameExpr*>(sub->object.get())) {
                        if (impl_->varDictValueIsType.count(objName->name))
                            return Impl::VarKind::Type;
                    }
                }
                // a Callable-typed RHS (a closure-returning call, a popped
                // list[Callable] element, a Callable field/param read) is a
                // refcounted closure. Resolve to Closure from the static type
                // BEFORE the llvm-type checks so it isn't misread as Int (pop
                // returns its element as i64) or Str (a closure-call result is a
                // bare ptr). This keeps a reassigned closure local VarKind::Closure
                // so scope cleanup still decrefs its final value.
                if (node.value && node.value->type &&
                    node.value->type->kind() == Type::Kind::Function)
                    return Impl::VarKind::Closure;
                if (rhsVal->getType() == impl_->i64Type) return Impl::VarKind::Int;
                if (rhsVal->getType() == impl_->f64Type) return Impl::VarKind::Float;
                if (rhsVal->getType() == impl_->i1Type) return Impl::VarKind::Bool;
                if (rhsVal->getType() == impl_->i8PtrType &&
                    !dynamic_cast<NoneLiteral*>(node.value.get())) {
                    // Check if RHS is a call to a generator function or TypedDict
                    if (auto* callExpr = dynamic_cast<CallExpr*>(node.value.get())) {
                        if (auto* calleeName = dynamic_cast<NameExpr*>(callExpr->callee.get())) {
                            // generatorFunctions is keyed by the LLVM symbol
                            // (post-mangling); resolveCalleeSymbol applies the
                            // alias / current-module / fallback chain.
                            if (impl_->generatorFunctions.count(
                                    impl_->resolveCalleeSymbol(calleeName->name)))
                                return Impl::VarKind::Generator;
                            if (impl_->typedDictClasses.count(calleeName->name))
                                return Impl::VarKind::Dict;
                            if (impl_->classNames.count(calleeName->name))
                                return Impl::VarKind::ClassInstance;
                            // D025: function returning `type` -> caller gets a class descriptor
                            if (impl_->funcReturnsType.count(calleeName->name))
                                return Impl::VarKind::Type;
                            // D025: function returning `ptr` - receiving var is a fn pointer.
                            // Record so the indirect-call site has a callable signal.
                            if (impl_->funcReturnsPtr.count(calleeName->name)) {
                                for (auto& t : node.targets) {
                                    if (auto* tgtName = dynamic_cast<NameExpr*>(t.get()))
                                        impl_->varIsPtrCallable.insert(tgtName->name);
                                }
                            }
                            // D026: dynamic construction via first-class class value
                            if (impl_->lookupVarKind(calleeName->name) == Impl::VarKind::Type)
                                return Impl::VarKind::ClassInstance;
                        }
                    }
                    if (dynamic_cast<FireExpr*>(node.value.get()))
                        return Impl::VarKind::ClassInstance;
                    // The RHS's resolved static type is authoritative for
                    // ptr-shaped heap values. A bytes/list/dict/tuple/set/
                    // instance expression (e.g. a `b + bytes(...)` concat, or a
                    // function returning `bytes`) lowers to a ptr but must NOT
                    // default to Str: a Str misclassification routes a later
                    // `x = x + ...` reassignment through the str-only in-place
                    // append fast path, which navigates the non-string buffer
                    // as a DragonString header and corrupts it (and scope
                    // cleanup would decref_str a non-string). Trust the type
                    // checker's kind for these heap kinds; str / None / scalar
                    // cases still fall through to the historical Str default.
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
                                // A raw `ptr` (fopen/malloc/extern-C result) is
                                // unmanaged: never refcounted, never decref'd.
                                // Without this it fell through to the Str default
                                // below, so reassigning a ptr local (`h = fopen`)
                                // marked it a string and scope cleanup ran
                                // dragon_decref_str over a FILE*/raw buffer.
                                return Impl::VarKind::Other;
                            default: break;
                        }
                    }
                    return Impl::VarKind::Str;
                }
                return Impl::VarKind::Other;
            };
            if (!alloca) {
                // Check module globals: .dr mode uses scope-chain (AssignStmt = update existing),
                // .py mode uses explicit `global x` declarations
                auto* gv = impl_->lookupModuleGlobal(name->name);
                if (gv && impl_->shouldUseModuleGlobal(name->name)) {
                    // Store directly to the global variable
                    llvm::Type* gvType = gv->getValueType();
                    Impl::VarKind oldKind = impl_->lookupVarKind(name->name);
                    // Union global (16-byte box slot): wrap the value in a box
                    // with its runtime tag. Without this a bare 8-byte store
                    // landed in the tag field and left the payload stale (the
                    // `<box tag=... payload=5>` after `x = "hello"` bug).
                    if (oldKind == Impl::VarKind::Union && gvType == impl_->boxType) {
                        if (val->getType() == impl_->boxType) {
                            if (impl_->options.gcMode == GCMode::RC) {
                                auto* oldBox = impl_->builder->CreateLoad(impl_->boxType, gv, "old.box");
                                impl_->emitUnionDecref(impl_->boxPayloadI64(oldBox, "op"),
                                                       impl_->boxTag(oldBox, "ot"));
                                // Owned box temporary already owns its +1.
                                if (!impl_->isOwnedBoxResult(val))
                                    impl_->emitUnionIncref(impl_->boxPayloadI64(val, "np"),
                                                           impl_->boxTag(val, "nt"));
                            }
                            impl_->builder->CreateStore(val, gv);
                            // D018 completion: the payload is now reachable from
                            // every vthread via the global - mark it SHARED so
                            // its refcount ops route to the atomic path.
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
                            // D018 completion: same as the box-valued branch.
                            if (impl_->options.gcMode == GCMode::RC)
                                impl_->builder->CreateCall(
                                    impl_->runtimeFuncs["dragon_mark_shared_boxed"],
                                    {newTag, impl_->nativeToPayloadI64(val)});
                        }
                        continue;
                    }
                    // D039: box -> native unbox for a typed global REASSIGNMENT
                    // (`s = anyGlobal` where s is a native-typed global) - the
                    // same gap the AnnAssign path had. Extract the payload at the
                    // slot's type so a 16-byte box isn't stored into the 8-byte
                    // native slot (which read back the tag as the value and
                    // overran the global).
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
                        impl_->moduleGlobalKinds[name->name] = newKind;
                    // const reassignment is compile error, so mark shred never immortal
                    impl_->emitMarkSharedGlobal(val, newKind);
                    continue;
                }
                // Module-level new var in main(): create GlobalVariable (no local alloca)
                // so functions can access it via `global` (.py) or scope-chain (.dr)
                if (impl_->currentFunction == impl_->mainFunction) {
                    auto* gv = new llvm::GlobalVariable(
                        *impl_->module, val->getType(), /*isConstant=*/false,
                        llvm::GlobalValue::InternalLinkage,
                        llvm::Constant::getNullValue(val->getType()),
                        "global." + name->name);
                    impl_->moduleGlobals[name->name] = gv;
                    // Infer VarKind
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
                    // Decision 025: bare class name -> VarKind::Type
                    else if (auto* rhsNE = dynamic_cast<NameExpr*>(node.value.get())) {
                        if (impl_->classNames.count(rhsNE->name))
                            vk = Impl::VarKind::Type;
                        else if (val->getType() == impl_->i64Type) vk = Impl::VarKind::Int;
                        else if (val->getType() == impl_->f64Type) vk = Impl::VarKind::Float;
                        else if (val->getType() == impl_->i1Type) vk = Impl::VarKind::Bool;
                    }
                    // D025: call to a function returning `type` (LLVM ret i64)
                    else if (auto* callVal2 = dynamic_cast<CallExpr*>(node.value.get())) {
                        if (auto* calleeNm = dynamic_cast<NameExpr*>(callVal2->callee.get())) {
                            if (impl_->funcReturnsType.count(calleeNm->name))
                                vk = Impl::VarKind::Type;
                        }
                    }
                    // D025: subscript of dict[K, type] yields a class descriptor
                    else if (auto* sub2 = dynamic_cast<SubscriptExpr*>(node.value.get())) {
                        if (auto* objName2 = dynamic_cast<NameExpr*>(sub2->object.get())) {
                            if (impl_->varDictValueIsType.count(objName2->name))
                                vk = Impl::VarKind::Type;
                        }
                    }
                    if (vk != Impl::VarKind::Other) { /* set above */ }
                    else if (val->getType() == impl_->i64Type) vk = Impl::VarKind::Int;
                    else if (val->getType() == impl_->f64Type) vk = Impl::VarKind::Float;
                    else if (val->getType() == impl_->i1Type) vk = Impl::VarKind::Bool;
                    else if (val->getType() == impl_->i8PtrType && vk == Impl::VarKind::Other) {
                        // Check if RHS is a call to a generator, TypedDict, class
                        // constructor, a function returning a class descriptor, or
                        // a function returning a `ptr` (callable function pointer).
                        if (auto* callVal = dynamic_cast<CallExpr*>(node.value.get())) {
                            if (auto* calleeName = dynamic_cast<NameExpr*>(callVal->callee.get())) {
                                if (impl_->generatorFunctions.count(
                                        impl_->resolveCalleeSymbol(calleeName->name)))
                                    vk = Impl::VarKind::Generator;
                                else if (impl_->typedDictClasses.count(calleeName->name))
                                    vk = Impl::VarKind::Dict;
                                else if (impl_->funcReturnsType.count(calleeName->name))
                                    vk = Impl::VarKind::Type;  // D025: -> type return
                                else if (impl_->funcReturnsPtr.count(calleeName->name))
                                    impl_->varIsPtrCallable.insert(name->name);
                            }
                        }
                        if (vk == Impl::VarKind::Other)
                            vk = Impl::VarKind::Str;  // ptr from runtime call -> dynamic string
                    }
                    impl_->moduleGlobalKinds[name->name] = vk;
                    impl_->storeWithRCOverwrite(
                        gv, gv->getValueType(), val, Impl::VarKind::Other, vk, rhsBorrowed, name->name);
                    // D027: closure from capturing lambda (module global)
                    if (impl_->lastClosureCallableType) {
                        impl_->callableTypes[name->name] = impl_->lastClosureCallableType;
                        impl_->moduleGlobalKinds[name->name] = Impl::VarKind::Closure;
                        impl_->lastClosureCallableType = nullptr;
                    }
                    // D025 Phase 4: type() returned a class descriptor (module global)
                    if (impl_->lastValueIsType) {
                        impl_->moduleGlobalKinds[name->name] = Impl::VarKind::Type;
                        impl_->lastValueIsType = false;
                    }
                    // `X = ...` at module top level) is reachable from every
                    // vthread by name. Mark the stored graph SHARED; runs
                    // after the Closure/Type kind fix-ups above so callables
                    // route through the tag-gated path and class descriptors
                    // are skipped. A literal-kind global is RC-inert on reads,
                    // but promote it to immortal anyway (no-op for rodata
                    // strings, permanent for heap-materialized literals) so no
                    // residual refcount traffic can race.
                    {
                        Impl::VarKind sharedKind = impl_->moduleGlobalKinds[name->name];
                        if (sharedKind == Impl::VarKind::StrLiteral &&
                            val->getType()->isPointerTy())
                            impl_->builder->CreateCall(
                                impl_->runtimeFuncs["dragon_str_make_immortal"], {val});
                        else
                            impl_->emitMarkSharedGlobal(val, sharedKind);
                    }
                    // ADR 025 removal: class-as-value aliasing is dropped.
                    // Binding a class name to a variable is rejected, so nothing
                    // populates typeVarClassName (the map stays empty; the
                    // construction/isinstance sites that read it always error).
                    // Track callable types for first-class function support (module globals).
                    // Runs regardless of class-alias check above - `f = inc` and
                    // `Application = Router` are mutually exclusive in practice but
                    // the class-alias block must not swallow the NameExpr branch.
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
                    // Track class instances and TypedDict
                    if (auto* callVal = dynamic_cast<CallExpr*>(node.value.get())) {
                        if (auto* calleeName = dynamic_cast<NameExpr*>(callVal->callee.get())) {
                            if (impl_->typedDictClasses.count(calleeName->name)) {
                                impl_->varTypedDictClass[name->name] = calleeName->name;
                            } else if (impl_->classNames.count(calleeName->name)) {
                                impl_->varClassNames[name->name] = calleeName->name;
                                impl_->varClassOwningModule[name->name] =
                                    impl_->resolveClassOwningModule(calleeName->name);
                                // GC Phase 3: mark module global as ClassInstance
                                if (impl_->options.gcMode == GCMode::RC)
                                    impl_->moduleGlobalKinds[name->name] = Impl::VarKind::ClassInstance;
                            } else if (impl_->lookupVarKind(calleeName->name) == Impl::VarKind::Type ||
                                       impl_->moduleGlobalKinds.count(calleeName->name) &&
                                       impl_->moduleGlobalKinds[calleeName->name] == Impl::VarKind::Type) {
                                // A VarKind::Type callee is not a constructible
                                // class value (ADR 025 removal): the construction
                                // itself already errored in CallExpr. Tag the
                                // result ClassInstance so downstream cleanup is
                                // well-formed; there is no alias to resolve.
                                if (impl_->options.gcMode == GCMode::RC)
                                    impl_->moduleGlobalKinds[name->name] = Impl::VarKind::ClassInstance;
                            }
                            if (calleeName->name == "Lock")
                                impl_->varClassNames[name->name] = "__Lock";
                            if (calleeName->name == "SyncList")
                                impl_->varClassNames[name->name] = "__SyncList";
                            if (calleeName->name == "SyncDict")
                                impl_->varClassNames[name->name] = "__SyncDict";
                            if (calleeName->name == "deque") {
                                impl_->varClassNames[name->name] = "__Deque";
                                impl_->moduleGlobalKinds[name->name] = Impl::VarKind::Deque;
                            }
                        }
                    }
                    // Track fire expressions as __Thread handles
                    if (dynamic_cast<FireExpr*>(node.value.get())) {
                        impl_->varClassNames[name->name] = "__Thread";
                    }
                    // Fallback: detect class instances from complex expressions (e.g., a + b)
                    if (!impl_->varClassNames.count(name->name)) {
                        auto cls = impl_->resolveExprClassName(node.value.get());
                        if (!cls.empty()) {
                            impl_->varClassNames[name->name] = cls;
                            // Owning module: explicit `mod.ClassName(args)` carries
                            // it on the AttributeExpr's ModuleType; everything else
                            // falls back to resolveClassOwningModule (alias ->
                            // same-module probe -> global owner map).
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
                            // GC Phase 3: mark module global as ClassInstance
                            if (impl_->options.gcMode == GCMode::RC && impl_->classNames.count(cls))
                                impl_->moduleGlobalKinds[name->name] = Impl::VarKind::ClassInstance;
                        }
                    }
                    continue;
                }
                // Function-level new variable - create alloca
                alloca = impl_->createEntryAlloca(
                    impl_->currentFunction, name->name, val->getType());
                impl_->setVar(name->name, alloca);
            }
            // Type coercion if needed
            llvm::Type* allocType = alloca->getAllocatedType();
            // D039: box -> native unboxing for typed slots. When the RHS is a
            // {tag, payload} box but the alloca is an unboxed native type
            // (f64/i64/i1/ptr), extract the payload at the slot's natural
            // type. Relies on the user having narrowed via isinstance before
            // this assignment - the bit reinterpretation is correct IFF the
            // box's runtime tag matches the slot's type. Without this, a
            // direct CreateStore of a 16-byte box into an 8-byte slot
            // corrupts the adjacent stack alloca; subsequent scope cleanup
            // then dispatches on garbage tag bits and decrefs random
            // pointers.
            bool didUnboxToNative = false;
            // An OWNED box result (dragon_box_subscript on an `Any` value) unboxed
            // into a native slot carries a +1 on its payload that the store below
            // neither adopts nor releases - it increfs for the slot's own ref, or
            // (a fresh Other-kind slot) does nothing - so the box's +1 is orphaned
            // (one leaked payload per `x: str = anyVal[k]`). Capture the payload +
            // tag from the box BEFORE it is unboxed, and release that +1 after the
            // store. Gated on isOwnedBoxResult: a BORROWED element read
            // (dict_get_box / dict_int_get_box / list_box_get, e.g. the hot-path
            // `const s: str = typedDict[k]` over a parsed dict[str,Any]) carries NO
            // +1, so it is left alone - releasing it would double-free.
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
            // A BORROWED box unboxed into an owned heap-pointer slot must take
            // its OWN reference: the container the box read from still holds the
            // +1, so treating the slot as owned without an incref double-frees
            // when that container is destroyed (mirror of AugAnnAssign Phase 7a -
            // the UAF a `s = doc[k] if k in doc else ""` ternary planted, freed
            // via the slot then re-read by dragon_dict_destroy). The syntactic
            // isBorrowedHeapExpr(expr) is false for a ternary source, so key the
            // incref off the box's real ownership (ownedBoxUnboxed) instead.
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
            // Mixed owned/literal slot: a string LITERAL stored into a slot that alreadly
            // holds an woned heap string must not downgrade the slot's clearup kind to
            // StrLiteral. Scope cleanup keys off the single compile-time varKind
            // (emitScopeCleanupFor), so a downgrade makes it skip the decref, leaking the
            // owned value that reaches cleanup on a branch where the literal store did not
            // run (e.g. `p = dsn[7:]; if p == "" { p = ":memory:" }`). Keep the slot
            // Str: dragon_decref_str is a safe no-op on the literal branch (it skips non-heap strings).
            // Gated on !isBorrowedSlot: a BORROWED slot conditionally overwritten
            // by a literal must stay StrLiteral (skip cleanup) - decref'ing the
            // caller's borrowed value on the not-taken branch would be a UAF.
            if (newKind == Impl::VarKind::StrLiteral &&
                oldKind == Impl::VarKind::Str &&
                !impl_->isBorrowedSlot(name->name)) {
                newKind = Impl::VarKind::Str;
            }
            // When the RHS box was unboxed into a native HEAP slot (str/list/dict),
            // `val` is now a bare `inttoptr` payload - NOT a tagged box. But
            // inferIncomingKind read the SOURCE variable's kind: for `ver = anyVar`
            // that is Union. storeWithRCOverwrite then emits a Union incref, which
            // is tag-dispatched and cannot act on a bare pointer, so the string is
            // never retained; when the box's owner (e.g. the dict it came from) is
            // destroyed, the slot dangles -> use-after-free. (A non-heap Other kind
            // skips the incref + cleanup entirely - the same hole.) Re-derive
            // newKind from the slot's real heap kind so the correct dragon_incref_*
            // fires: oldKind distinguishes str/list/dict (never incref_str on a
            // list ptr), and a slot last holding a literal reads back as StrLiteral
            // (still a string slot).
            if (didUnboxToNative && allocType->isPointerTy() &&
                (newKind == Impl::VarKind::Union || !Impl::isHeapKind(newKind))) {
                if (oldKind == Impl::VarKind::StrLiteral)
                    newKind = Impl::VarKind::Str;
                else if (Impl::isHeapKind(oldKind) && oldKind != Impl::VarKind::Union)
                    newKind = oldKind;
            }

            // D030 Phase 4: Union-typed target - build a box from (newTag, val),
            // decref old box's payload (conditional), incref new payload
            // (conditional), store the new box. Replaces the old companion
            // tag-alloca update + storeWithRCOverwrite pair.
            if (oldKind == Impl::VarKind::Union) {
                newKind = Impl::VarKind::Union;
                // D039 Phase 2: RHS already a box - forward without re-wrapping.
                if (val->getType() == impl_->boxType) {
                    if (impl_->options.gcMode == GCMode::RC) {
                        auto* oldBox = impl_->builder->CreateLoad(
                            impl_->boxType, alloca, "old.box");
                        auto* oldTag = impl_->boxTag(oldBox, "old.tag");
                        auto* oldPayload = impl_->boxPayloadI64(oldBox, "old.payload");
                        impl_->emitUnionDecref(oldPayload, oldTag);
                        auto* newTag = impl_->boxTag(val, "new.tag");
                        auto* newPayload = impl_->boxPayloadI64(val, "new.payload");
                        // Owned box temporary already carries the +1 the slot
                        // needs - take ownership instead of double-counting.
                        if (!impl_->isOwnedBoxResult(val))
                            impl_->emitUnionIncref(newPayload, newTag);
                    }
                    impl_->builder->CreateStore(val, alloca);
                    // Refresh the unwind cleanup snapshot (union reassignment).
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
                    // Refresh the unwind cleanup snapshot (union reassignment).
                    if (impl_->options.gcMode == GCMode::RC) {
                        auto* clPayload = impl_->boxPayloadI64(boxVal, "cl.payload");
                        impl_->emitCleanupUpdate(name->name, clPayload, newTag);
                    }
                }
            } else {
                impl_->storeWithRCOverwrite(
                    alloca, allocType, val, oldKind, newKind, rhsBorrowed, name->name);
            }
            // Release the OWNED box temporary's +1 (captured above), but ONLY when
            // the store treated the RHS as borrowed (rhsBorrowed) and took its own
            // independent ref - then the box's +1 is surplus (the box_subscript-on-
            // Any case: container owns the payload, box_subscript increfed, the
            // slot increfed again; drop the box's). A box_binop / Any-returning
            // call result is a FRESH payload with no other owner (rhsBorrowed=
            // false, store ADOPTS the box's +1); releasing it would double-free.
            // Tag-gated: no-op for a non-heap payload.
            if (ownedBoxUnboxed && rhsBorrowed)
                impl_->emitUnionDecref(ownedBoxPayload, ownedBoxTag);

            // D027: If RHS was a closure (from capturing lambda), track it
            if (impl_->lastClosureCallableType) {
                impl_->callableTypes[name->name] = impl_->lastClosureCallableType;
                impl_->setVar(name->name, alloca, Impl::VarKind::Closure);
                impl_->lastClosureCallableType = nullptr;
            }
            // D025 Phase 4: type() returned a class descriptor
            else if (impl_->lastValueIsType) {
                impl_->setVar(name->name, alloca, Impl::VarKind::Type);
                impl_->lastValueIsType = false;
            }
            // ADR 025 removal: class-as-value aliasing (cls = ClassName) is
            // dropped - no typeVarClassName tracking here.
            // Update VarKind to match the new value's type.
            // newKind was already inferred via inferIncomingKind above.
            // Always update when newKind is known, to prevent stale VarKind
            // after reassignment (e.g., x:int=1 then x = some_str_func()).
            else if (oldKind == Impl::VarKind::Union) {
                // Keep Union kind - don't override with concrete kind
            } else if (didUnboxToNative) {
                // The RHS box was unboxed at the slot's native type; keep
                // the existing concrete varKind so cleanup uses the right
                // shape. Promoting to Union here would mismatch the alloca.
            } else if (newKind != Impl::VarKind::Other) {
                // Don't promote a non-box alloca's varKind to Union - the
                // alloca shape is fixed at declaration and varKind must
                // match (Union cleanup loads a 16-byte box, which would
                // overrun an 8-byte slot and dispatch decref on stack
                // garbage). Same constraint for any kind whose natural
                // storage shape exceeds the alloca's allocated bytes.
                if (newKind == Impl::VarKind::Union &&
                    alloca->getAllocatedType() != impl_->boxType) {
                    // Keep the existing varKind - the value has been (or
                    // will be at the store below) coerced to the alloca's
                    // shape via storeWithRCOverwrite.
                } else {
                    impl_->setVar(name->name, alloca, newKind);
                }
            } else if (auto* rhsName = dynamic_cast<NameExpr*>(node.value.get())) {
                // Propagate VarKind from the source variable (preserves Str vs StrLiteral)
                auto rhsKind = impl_->lookupVarKind(rhsName->name);
                if (rhsKind != Impl::VarKind::Other &&
                    !(rhsKind == Impl::VarKind::Union &&
                      alloca->getAllocatedType() != impl_->boxType))
                    impl_->setVar(name->name, alloca, rhsKind);
            }
            // Track callable types for first-class function support.
            // If the RHS is a lambda (llvm::Function*), record its FunctionType.
            // If the RHS is a named function reference, record that function's type.
            // Runs unconditionally (not chained off the else-ifs above) so
            // it isn't swallowed by class-alias / lastValueIsType / etc.
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

            // Track class name for instances assigned from constructor calls
            if (auto* callVal = dynamic_cast<CallExpr*>(node.value.get())) {
                if (auto* calleeName = dynamic_cast<NameExpr*>(callVal->callee.get())) {
                    if (impl_->typedDictClasses.count(calleeName->name)) {
                        impl_->varTypedDictClass[name->name] = calleeName->name;
                    } else if (impl_->classNames.count(calleeName->name)) {
                        impl_->varClassNames[name->name] = calleeName->name;
                        impl_->varClassOwningModule[name->name] =
                            impl_->resolveClassOwningModule(calleeName->name);
                        // GC Phase 3: mark as ClassInstance for scope-exit decref
                        if (impl_->options.gcMode == GCMode::RC)
                            impl_->setVar(name->name, alloca, Impl::VarKind::ClassInstance);
                    } else if (impl_->lookupVarKind(calleeName->name) == Impl::VarKind::Type) {
                        // A VarKind::Type callee is not a constructible class value
                        // (ADR 025 removal): construction already errored in
                        // CallExpr. Tag ClassInstance so cleanup is well-formed;
                        // there is no alias to resolve.
                        if (impl_->options.gcMode == GCMode::RC)
                            impl_->setVar(name->name, alloca, Impl::VarKind::ClassInstance);
                    }
                    // Track Lock() calls
                    if (calleeName->name == "Lock") {
                        impl_->varClassNames[name->name] = "__Lock";
                        // docs/002 2.10: bare Lock LOCAL - the scope owns it;
                        // arm the null-gated destroy at scope exit (module
                        // globals are handled on the module path above and
                        // are NOT armed - they live for the process).
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
            // Track fire expressions as __Thread handles
            if (dynamic_cast<FireExpr*>(node.value.get())) {
                impl_->varClassNames[name->name] = "__Thread";
            }
            // Fallback: detect class instances from complex expressions (e.g., a + b)
            if (!impl_->varClassNames.count(name->name)) {
                auto cls = impl_->resolveExprClassName(node.value.get());
                if (!cls.empty()) {
                    impl_->varClassNames[name->name] = cls;
                    // Owning module: explicit `mod.ClassName(args)` carries it
                    // on the AttributeExpr's ModuleType; everything else falls
                    // back to resolveClassOwningModule.
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
                    // GC Phase 3: mark as ClassInstance for scope-exit decref
                    if (impl_->options.gcMode == GCMode::RC && impl_->classNames.count(cls))
                        impl_->setVar(name->name, alloca, Impl::VarKind::ClassInstance);
                }
            }
        }
    }
}


} // namespace dragon
