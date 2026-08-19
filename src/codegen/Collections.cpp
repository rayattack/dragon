#include "../CodeGenImpl.h"

namespace dragon {

llvm::Value* CodeGen::Impl::emitNewTypedList(int64_t elemTag, bool isAny,
                                             llvm::Value* capVal) {
    bool isF64 = (elemTag == 2);
    bool isPtr = (elemTag == 1 || elemTag == 5 || elemTag == 6 || elemTag == 7 ||
                  elemTag == 10);
    if (isAny)
        return builder->CreateCall(
            runtimeFuncs["dragon_list_box_new"], {capVal}, "list");
    if (isF64)
        return builder->CreateCall(
            runtimeFuncs["dragon_list_new_f64"], {capVal}, "list");
    if (isPtr) {
        auto* tagVal = llvm::ConstantInt::get(i64Type, elemTag);
        return builder->CreateCall(
            runtimeFuncs["dragon_list_new_ptr"], {capVal, tagVal}, "list");
    }
    if (elemTag != 0) {
        auto* tagVal = llvm::ConstantInt::get(i64Type, elemTag);
        return builder->CreateCall(
            runtimeFuncs["dragon_list_new_tagged"], {capVal, tagVal}, "list");
    }
    return builder->CreateCall(runtimeFuncs["dragon_list_new"], {capVal}, "list");
}

void CodeGen::Impl::emitTypedListAppend(llvm::Value* list, llvm::Value* val,
                                        Expr* elemExpr, int64_t elemTag,
                                        bool isAny, CodeGen& cg) {
    bool isF64 = (elemTag == 2);
    bool isPtr = (elemTag == 1 || elemTag == 5 || elemTag == 6 || elemTag == 7 ||
                  elemTag == 10);
    if (isAny) {
        auto tp = boxArgTagPayload(elemExpr, val, true);
        builder->CreateCall(
            runtimeFuncs["dragon_list_box_append"], {list, tp.first, tp.second});
    } else if (isF64) {
        if (val->getType() == i64Type)
            val = builder->CreateSIToFP(val, f64Type);
        else if (val->getType() == i1Type)
            val = builder->CreateUIToFP(val, f64Type);
        builder->CreateCall(runtimeFuncs["dragon_list_append_f64"], {list, val});
    } else if (isPtr) {
        if (elemTag == 1 && val->getType()->isPointerTy())
            val = ensureHeapString(val, elemExpr);
        if (!val->getType()->isPointerTy())
            val = builder->CreateIntToPtr(val, i8PtrType);
        if (options.gcMode == GCMode::RC && Impl::isBorrowedHeapExpr(elemExpr)) {
            if (elemTag == 1)
                builder->CreateCall(runtimeFuncs["dragon_incref_str"], {val});
            else if (elemTag == 10)
                builder->CreateCall(runtimeFuncs["dragon_incref_callable"], {val});
            else
                builder->CreateCall(runtimeFuncs["dragon_incref"], {val});
        }
        builder->CreateCall(runtimeFuncs["dragon_list_append_ptr"], {list, val});
    } else {
        if (val->getType() == f64Type)
            val = builder->CreateBitCast(val, i64Type);
        else if (val->getType() == i1Type)
            val = builder->CreateZExt(val, i64Type);
        else if (val->getType()->isPointerTy())
            val = builder->CreatePtrToInt(val, i64Type);
        builder->CreateCall(runtimeFuncs["dragon_list_append"], {list, val});
    }
}

void CodeGen::visit(ListExpr& node) {
    int64_t cap = node.elements.empty() ? 8 : (int64_t)node.elements.size();
    llvm::Value* capVal = llvm::ConstantInt::get(impl_->i64Type, cap);
    int64_t elemTag = impl_->getListElemTag(&node);

    bool isAny = false;
    if (node.type) {
        if (auto* lt = dynamic_cast<ListType*>(node.type.get())) {
            if (lt->elementType && lt->elementType->kind() == Type::Kind::Any)
                isAny = true;
        }
    }

    llvm::Value* list = impl_->emitNewTypedList(elemTag, isAny, capVal);

    for (auto& elem : node.elements) {
        if (dynamic_cast<StarredExpr*>(elem.get())) {
            auto* starred = static_cast<StarredExpr*>(elem.get());
            starred->value->accept(*this);
            llvm::Value* otherList = impl_->lastValue;
            if (!otherList->getType()->isPointerTy())
                otherList = impl_->builder->CreateIntToPtr(otherList, impl_->i8PtrType);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_extend"], {list, otherList});
            continue;
        }
        elem->accept(*this);
        impl_->emitTypedListAppend(list, impl_->lastValue, elem.get(),
                                   elemTag, isAny, *this);
    }

    impl_->lastValue = list;
}
void CodeGen::visit(TupleExpr& node) {
    int64_t count = node.elements.size();
    llvm::Value* countVal = llvm::ConstantInt::get(impl_->i64Type, count);
    llvm::Value* tuple = impl_->builder->CreateCall(
        impl_->runtimeFuncs["dragon_tuple_new"], {countVal}, "tuple");

    TupleType* tupleType = node.type ? dynamic_cast<TupleType*>(node.type.get()) : nullptr;

    for (int64_t i = 0; i < count; i++) {
        node.elements[i]->accept(*this);
        llvm::Value* val = impl_->lastValue;
        if (val->getType() == impl_->boxType) {
            llvm::Value* btag = impl_->boxTag(val, "tv.tag");
            llvm::Value* bpayload = impl_->boxPayloadI64(val, "tv.payload");
            if (impl_->options.gcMode == GCMode::RC &&
                Impl::isBorrowedHeapExpr(node.elements[i].get())) {
                impl_->emitUnionIncref(bpayload, btag);
            }
            llvm::Value* bidx = llvm::ConstantInt::get(impl_->i64Type, i);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_tuple_set_tagged"],
                {tuple, bidx, bpayload, btag});
            continue;
        }
        bool wasPtr = val->getType()->isPointerTy();
        int64_t elemTag = 0;
        if (tupleType && i < (int64_t)tupleType->elementTypes.size() &&
            tupleType->elementTypes[i]) {
            Type::Kind slotKind = tupleType->elementTypes[i]->kind();
            if (slotKind == Type::Kind::Any || slotKind == Type::Kind::Union) {
                if (node.elements[i]->type)
                    elemTag =
                        Impl::typeKindToElemTag(node.elements[i]->type->kind());
            } else {
                elemTag = Impl::typeKindToElemTag(slotKind);
            }
        }
        if (elemTag == TAG_STR && wasPtr) {
            val = impl_->ensureHeapString(val, node.elements[i].get());
            wasPtr = val->getType()->isPointerTy();
        }
        // Model B: tuple_set takes ownership of one ref. Borrowed sources need an incref
        // first or scope cleanup drops the local to 0 while the tuple still holds the pointer (UAF on result[i]). Fresh sources already own their +1.
        if (impl_->options.gcMode == GCMode::RC && wasPtr && elemTag != 0 &&
            Impl::isBorrowedHeapExpr(node.elements[i].get())) {
            if (elemTag == TAG_STR)
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_incref_str"], {val});
            else
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_incref"], {val});
        }
        if (val->getType() == impl_->f64Type) {
            val = impl_->builder->CreateBitCast(val, impl_->i64Type);
        } else if (val->getType() == impl_->i1Type) {
            val = impl_->builder->CreateZExt(val, impl_->i64Type);
        } else if (wasPtr) {
            val = impl_->builder->CreatePtrToInt(val, impl_->i64Type);
        }
        llvm::Value* idxVal = llvm::ConstantInt::get(impl_->i64Type, i);
        if (elemTag != 0) {
            llvm::Value* tagVal = llvm::ConstantInt::get(impl_->i64Type, elemTag);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_tuple_set_tagged"], {tuple, idxVal, val, tagVal});
        } else {
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_tuple_set"], {tuple, idxVal, val});
        }
    }

    impl_->lastValue = tuple;
}
void CodeGen::visit(DictExpr& node) {
    int64_t cap = std::max((int64_t)node.entries.size(), (int64_t)4);
    llvm::Value* capVal = llvm::ConstantInt::get(impl_->i64Type, cap);
    llvm::Value* dict = impl_->builder->CreateCall(
        impl_->runtimeFuncs["dragon_dict_new"], {capVal}, "dict");

    bool intKeys = false;
    bool floatKeys = false;
    if (auto* dt = dynamic_cast<DictType*>(node.type.get())) {
        if (dt->keyType && dt->keyType->kind() == Type::Kind::Int) intKeys = true;
        if (dt->keyType && dt->keyType->kind() == Type::Kind::Float) {
            intKeys = true;
            floatKeys = true;
        }
    }
    for (auto& entry : node.entries) {
        if (!entry.first) continue;
        if (entry.first->type && entry.first->type->kind() == Type::Kind::Int) {
            intKeys = true;
        } else if (entry.first->type &&
                   entry.first->type->kind() == Type::Kind::Float) {
            intKeys = true;
            floatKeys = true;
        } else if (dynamic_cast<IntegerLiteral*>(entry.first.get())) {
            intKeys = true;
        } else if (dynamic_cast<FloatLiteral*>(entry.first.get())) {
            intKeys = true;
            floatKeys = true;
        }
        break;
    }
    if (floatKeys)
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_dict_mark_float_keys"], {dict});

    for (auto& entry : node.entries) {
        if (!entry.first) {
            entry.second->accept(*this);
            llvm::Value* otherDict = impl_->lastValue;
            if (!otherDict->getType()->isPointerTy())
                otherDict = impl_->builder->CreateIntToPtr(otherDict, impl_->i8PtrType);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_update"], {dict, otherDict});
            continue;
        }

        entry.first->accept(*this);
        llvm::Value* key = impl_->lastValue;

        entry.second->accept(*this);
        llvm::Value* val = impl_->lastValue;

        if (intKeys) {
            if (floatKeys) key = impl_->emitFloatDictKeyBits(key);
            if (key->getType() == impl_->i1Type)
                key = impl_->builder->CreateZExt(key, impl_->i64Type);
            else if (key->getType() != impl_->i64Type && !key->getType()->isPointerTy())
                key = impl_->builder->CreateZExtOrTrunc(key, impl_->i64Type);

            if (val->getType() == impl_->f64Type) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_int_set_f64"], {dict, key, val});
                continue;
            }
            if (val->getType()->isPointerTy()) {
                int64_t tag = impl_->inferPtrValueTag(entry.second.get());
                llvm::Value* pval = val;
                if (tag == 1) pval = impl_->ensureHeapString(pval, entry.second.get());
                if (impl_->options.gcMode == GCMode::RC &&
                    (tag == 1 || tag == 5 || tag == 6 || tag == 7) &&
                    Impl::isBorrowedHeapExpr(entry.second.get())) {
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
            int64_t tag = 0;
            if (val->getType() == impl_->i1Type) {
                tag = 3;
                val = impl_->builder->CreateZExt(val, impl_->i64Type);
            }
            llvm::Value* tagVal = llvm::ConstantInt::get(impl_->i64Type, tag);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_int_set_tagged"], {dict, key, val, tagVal});
            continue;
        }

        if (val->getType() == impl_->f64Type) {
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_set_str_f64"], {dict, key, val});
            continue;
        }
        if (val->getType()->isPointerTy()) {
            int64_t tag = impl_->inferPtrValueTag(entry.second.get());
            llvm::Value* pval = val;
            if (tag == TAG_STR)
                pval = impl_->ensureHeapString(pval, entry.second.get());
            impl_->increfBorrowedContainerValue(pval, entry.second.get(), tag);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_set_str_ptr"],
                {dict, key, pval, llvm::ConstantInt::get(impl_->i64Type, tag)});
            continue;
        }
        if (val->getType() == impl_->boxType) {
            llvm::Value* btag = impl_->boxTag(val, "dv.tag");
            llvm::Value* bpayload = impl_->boxPayloadI64(val, "dv.payload");
            if (impl_->options.gcMode == GCMode::RC &&
                Impl::isBorrowedHeapExpr(entry.second.get())) {
                impl_->emitUnionIncref(bpayload, btag);
            }
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_set_tagged"],
                {dict, key, bpayload, btag});
            continue;
        }
        int64_t tag = 0;
        if (val->getType() == impl_->i1Type) {
            tag = 3;
            val = impl_->builder->CreateZExt(val, impl_->i64Type);
        }
        llvm::Value* tagVal = llvm::ConstantInt::get(impl_->i64Type, tag);
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_dict_set_tagged"], {dict, key, val, tagVal});
    }

    impl_->lastValue = dict;
}
void CodeGen::visit(SetExpr& node) {

    int64_t elemTag = TAG_INT;
    if (!node.elements.empty()) {
        auto* first = node.elements[0].get();
        if (first->type) {
            elemTag = impl_->typeKindToElemTag(first->type->kind());
        }
        if (elemTag == 0) {
            if (dynamic_cast<StringLiteral*>(first))
                elemTag = TAG_STR;
            else if (dynamic_cast<ListExpr*>(first) || dynamic_cast<ListCompExpr*>(first))
                elemTag = TAG_LIST;
            else if (dynamic_cast<DictExpr*>(first) || dynamic_cast<DictCompExpr*>(first))
                elemTag = TAG_DICT;
        }
    }

    llvm::Value* set;
    if (elemTag != 0) {
        auto* tagVal = llvm::ConstantInt::get(impl_->i64Type, elemTag);
        set = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_set_new_tagged"], {tagVal}, "set");
    } else {
        set = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_set_new"], {}, "set");
    }

    for (auto& elem : node.elements) {
        elem->accept(*this);
        llvm::Value* val = impl_->lastValue;
        llvm::Value* ownedElem = nullptr;
        Impl::VarKind elemDk = Impl::VarKind::Other;
        if (val->getType() == impl_->f64Type) {
            val = impl_->builder->CreateBitCast(val, impl_->i64Type);
        } else if (val->getType() == impl_->i1Type) {
            val = impl_->builder->CreateZExt(val, impl_->i64Type);
        } else if (val->getType()->isPointerTy()) {
            if (elemTag == 1) val = impl_->ensureHeapString(val, elem.get());
            elemDk = impl_->ownedTempDrainKind(elem.get(), val);
            if (elemDk != Impl::VarKind::Other) ownedElem = val;
            val = impl_->builder->CreatePtrToInt(val, impl_->i64Type);
        }
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_set_add"], {set, val});
        if (ownedElem) impl_->emitDecrefByKind(ownedElem, elemDk);
    }

    impl_->lastValue = set;
}
}
