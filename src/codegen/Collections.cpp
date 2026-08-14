/// Dragon CodeGen - Collection Literals (List, Tuple, Dict, Set)
#include "../CodeGenImpl.h"

namespace dragon {

// Allocates the monomorphized list variant for an element tag (shared by list literals
// and *args packing): float->ListF64, str/list/dict/bytes->ListPtr, Any->ListBox, other tag->tagged, else->i64.
llvm::Value* CodeGen::Impl::emitNewTypedList(int64_t elemTag, bool isAny,
                                             llvm::Value* capVal) {
    bool isF64 = (elemTag == 2);
    // elemTag 10 = TAG_CLOSURE (list[Callable]) - a ptr-storage variant.
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

// Appends an already-evaluated `val` to a list built by emitNewTypedList, matching the
// storage variant. `elemExpr` drives tag inference and the borrow/incref + ensureHeapString discipline.
void CodeGen::Impl::emitTypedListAppend(llvm::Value* list, llvm::Value* val,
                                        Expr* elemExpr, int64_t elemTag,
                                        bool isAny, CodeGen& cg) {
    bool isF64 = (elemTag == 2);
    bool isPtr = (elemTag == 1 || elemTag == 5 || elemTag == 6 || elemTag == 7 ||
                  elemTag == 10);
    if (isAny) {
        // list[Any]/box list stores (tag, payload-as-i64); forwards an existing box or
        // infers the tag. Model B: the list owns one ref, so a borrowed source is increfed (shared with append/insert via boxArgTagPayload).
        auto tp = boxArgTagPayload(elemExpr, val, /*takesOwnership=*/true);
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
                // tag-gated: a borrowed Callable may be a bare fn ptr (no header);
                // _callable no-ops on it, increfs a real closure.
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
    // Create a new list with initial capacity = number of elements
    int64_t cap = node.elements.empty() ? 8 : (int64_t)node.elements.size();
    llvm::Value* capVal = llvm::ConstantInt::get(impl_->i64Type, cap);
    int64_t elemTag = impl_->getListElemTag(&node);

    // D039 Phase 4: detect list[Any] from the typechecker's element type; when the
    // declared elem kind is Any, use DragonListBox so each element keeps its own tag.
    bool isAny = false;
    if (node.type) {
        if (auto* lt = dynamic_cast<ListType*>(node.type.get())) {
            if (lt->elementType && lt->elementType->kind() == Type::Kind::Any)
                isAny = true;
        }
    }

    // D030/D039: pick the monomorphized list variant for this element tag.
    llvm::Value* list = impl_->emitNewTypedList(elemTag, isAny, capVal);

    // Append each element via the matching typed op.
    for (auto& elem : node.elements) {
        // [*other_list, ...] -> dragon_list_extend(list, other_list)
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
    // Tuple layout (runtime): { int64_t* data, int64_t length }; all element values are
    // stored as i64 (pointers via ptrtoint, floats via bitcast).
    int64_t count = node.elements.size();
    llvm::Value* countVal = llvm::ConstantInt::get(impl_->i64Type, count);
    llvm::Value* tuple = impl_->builder->CreateCall(
        impl_->runtimeFuncs["dragon_tuple_new"], {countVal}, "tuple");

    // Phase 5: check TupleType for per-element tags
    TupleType* tupleType = node.type ? dynamic_cast<TupleType*>(node.type.get()) : nullptr;

    for (int64_t i = 0; i < count; i++) {
        node.elements[i]->accept(*this);
        llvm::Value* val = impl_->lastValue;
        // Any/Union element is a {tag, payload} box; dragon_tuple_set wants an i64 slot,
        // so extract payload+tag and route through the tagged setter (passing the box raw is an LLVM verify failure). RC: incref a borrowed source's payload; an owned box temp already owns its +1.
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
        // Phase 5: determine per-element tag early so we can promote string literals
        int64_t elemTag = 0;
        if (tupleType && i < (int64_t)tupleType->elementTypes.size() &&
            tupleType->elementTypes[i]) {
            Type::Kind slotKind = tupleType->elementTypes[i]->kind();
            if (slotKind == Type::Kind::Any || slotKind == Type::Kind::Union) {
                // Any/Union slot holding a concrete value (box case handled above): tag
                // comes from the element's own static type, since typeKindToElemTag(Any) is TAG_INT and would store a heap pointer untagged (leak + wrong tag on read-back).
                if (node.elements[i]->type)
                    elemTag =
                        Impl::typeKindToElemTag(node.elements[i]->type->kind());
            } else {
                elemTag = Impl::typeKindToElemTag(slotKind);
            }
        }
        // Promote string literals to heap DragonStrings when stored with TAG_STR
        if (elemTag == TAG_STR && wasPtr) { // TAG_STR
            val = impl_->ensureHeapString(val, node.elements[i].get());
            wasPtr = val->getType()->isPointerTy();
        }
        // Model B: tuple_set takes ownership of one ref. Borrowed sources need an incref
        // first or scope cleanup drops the local to 0 while the tuple still holds the pointer (UAF on result[i]). Fresh sources already own their +1.
        if (impl_->options.gcMode == GCMode::RC && wasPtr && elemTag != 0 &&
            Impl::isBorrowedHeapExpr(node.elements[i].get())) {
            if (elemTag == TAG_STR) // TAG_STR
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_incref_str"], {val});
            else
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_incref"], {val});
        }
        // Convert all values to i64 for storage
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
    // Dict literal: dragon_dict_new + dragon_dict_set per entry. D030 Phase 3.G: int-keyed
    // literals (from the first key's type) route to dragon_dict_int_* so int keys cross at i64, not the str-keyed path.
    int64_t cap = std::max((int64_t)node.entries.size(), (int64_t)4);
    llvm::Value* capVal = llvm::ConstantInt::get(impl_->i64Type, cap);
    llvm::Value* dict = impl_->builder->CreateCall(
        impl_->runtimeFuncs["dragon_dict_new"], {capVal}, "dict");

    bool intKeys = false;
    for (auto& entry : node.entries) {
        if (!entry.first) continue;
        if (entry.first->type && entry.first->type->kind() == Type::Kind::Int) {
            intKeys = true;
        } else if (dynamic_cast<IntegerLiteral*>(entry.first.get())) {
            intKeys = true;
        }
        break;
    }

    for (auto& entry : node.entries) {
        // {**other_dict, ...} -> dragon_dict_update(dict, other_dict)
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
            // Coerce the key to i64 (bool widens; ptr would be a type bug at this point).
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

        // D030 Phase 3.F: dispatch by native LLVM type to the matching typed
        // str-keyed dict-set op so values don't funnel through i64 at the call site.
        if (val->getType() == impl_->f64Type) {
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_set_str_f64"], {dict, key, val});
            continue;
        }
        if (val->getType()->isPointerTy()) {
            int64_t tag = impl_->inferPtrValueTag(entry.second.get());
            llvm::Value* pval = val;
            if (tag == 1) pval = impl_->ensureHeapString(pval, entry.second.get());
            // Model B: dict_set_str_ptr takes ownership of one ref. Borrowed sources need
            // an incref so the new dict's reference outlives the source's owning scope.
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
                impl_->runtimeFuncs["dragon_dict_set_str_ptr"],
                {dict, key, pval, llvm::ConstantInt::get(impl_->i64Type, tag)});
            continue;
        }
        // Any/Union value is a {tag, payload} box; dragon_dict_set_tagged wants both
        // extracted (passing the box raw where i64 is expected is an LLVM verify failure). RC: incref a borrowed source's payload; an owned box temp already owns its +1.
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
        // Int / Bool value: legacy i64-tagged path.
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
    // Set layout (runtime): { int64_t* buckets, uint8_t* states, capacity, count }; open
    // addressing with linear probing, values stored as i64.

    // Determine element tag from first element (if any)
    int64_t elemTag = TAG_INT; // TAG_INT default
    if (!node.elements.empty()) {
        auto* first = node.elements[0].get();
        if (first->type) {
            elemTag = impl_->typeKindToElemTag(first->type->kind());
        }
        if (elemTag == 0) {
            // Fallback: check AST node type
            if (dynamic_cast<StringLiteral*>(first))
                elemTag = TAG_STR; // TAG_STR
            else if (dynamic_cast<ListExpr*>(first) || dynamic_cast<ListCompExpr*>(first))
                elemTag = TAG_LIST; // TAG_LIST
            else if (dynamic_cast<DictExpr*>(first) || dynamic_cast<DictCompExpr*>(first))
                elemTag = TAG_DICT; // TAG_DICT
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
        // dragon_set_add increfs (doesn't consume), so an owned element temp's +1 must be
        // released after the add or the literal leaks one ref per build (mirrors set.add in CallMethods.cpp). Borrowed elements have no droppable +1.
        llvm::Value* ownedElem = nullptr;
        Impl::VarKind elemDk = Impl::VarKind::Other;
        // Convert to i64 for storage
        if (val->getType() == impl_->f64Type) {
            val = impl_->builder->CreateBitCast(val, impl_->i64Type);
        } else if (val->getType() == impl_->i1Type) {
            val = impl_->builder->CreateZExt(val, impl_->i64Type);
        } else if (val->getType()->isPointerTy()) {
            // Promote string literals to heap strings for proper RC management
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
} // namespace dragon
