#include "../CodeGenImpl.h"

namespace dragon {

llvm::Value* CodeGen::emitCompFilterTruth(llvm::Value* cond) {
    if (cond->getType() == impl_->i64Type)
        return impl_->builder->CreateICmpNE(
            cond, llvm::ConstantInt::get(impl_->i64Type, 0));
    if (cond->getType() == impl_->f64Type)
        return impl_->builder->CreateFCmpONE(
            cond, llvm::ConstantFP::get(impl_->f64Type, 0.0));
    return cond;
}

void CodeGen::emitCompRangeArgs(CallExpr* call, llvm::Value*& start,
                                llvm::Value*& end, llvm::Value*& step) {
    start = llvm::ConstantInt::get(impl_->i64Type, 0);
    end = nullptr;
    step = llvm::ConstantInt::get(impl_->i64Type, 1);
    if (call->args.size() == 1) {
        call->args[0]->accept(*this);
        end = impl_->lastValue;
    } else if (call->args.size() >= 2) {
        call->args[0]->accept(*this);
        start = impl_->lastValue;
        call->args[1]->accept(*this);
        end = impl_->lastValue;
        if (call->args.size() >= 3) {
            call->args[2]->accept(*this);
            step = impl_->lastValue;
        }
    } else {
        end = llvm::ConstantInt::get(impl_->i64Type, 0);
    }
}

void CodeGen::emitCompAppendBody(Expr* element, Expr* condition,
                                 llvm::AllocaInst* listAlloca, int64_t elemTag,
                                 const std::string& bbPrefix) {
    auto* func = impl_->currentFunction;
    bool isF64 = (elemTag == 2);
    bool isPtr = (elemTag == 1 || elemTag == 5 || elemTag == 6 || elemTag == 7);
    const char* appendFn = isF64 ? "dragon_list_append_f64"
                          : isPtr ? "dragon_list_append_ptr"
                                  : "dragon_list_append";

    auto evalCoerceAppend = [&]() {
        element->accept(*this);
        llvm::Value* elemVal = impl_->lastValue;

        if (elemTag == TAG_STR && elemVal->getType()->isPointerTy()) {
            elemVal = impl_->ensureHeapString(elemVal, element);
        }

        // dragon_list_append_ptr ADOPTS the reference: a BORROWED element (loop var, field, xs[i]) must be
        // incref'd or the source and result both decref the same +1 (double free); an owned temp already carries its +1.
        if (isPtr && impl_->options.gcMode == GCMode::RC &&
            elemVal->getType()->isPointerTy() &&
            Impl::isBorrowedHeapExpr(element)) {
            impl_->builder->CreateCall(
                impl_->runtimeFuncs[elemTag == 1 ? "dragon_incref_str"
                                                 : "dragon_incref"],
                {elemVal});
        }

        if (isF64) {
            if (elemVal->getType() == impl_->i64Type)
                elemVal = impl_->builder->CreateSIToFP(elemVal, impl_->f64Type);
            else if (elemVal->getType() == impl_->i1Type)
                elemVal = impl_->builder->CreateUIToFP(elemVal, impl_->f64Type);
        } else if (isPtr) {
            if (!elemVal->getType()->isPointerTy())
                elemVal = impl_->builder->CreateIntToPtr(elemVal, impl_->i8PtrType);
        } else {
            if (elemVal->getType() == impl_->i1Type) {
                elemVal = impl_->builder->CreateZExt(elemVal, impl_->i64Type);
            } else if (elemVal->getType() == impl_->f64Type) {
                elemVal = impl_->builder->CreateBitCast(elemVal, impl_->i64Type);
            } else if (elemVal->getType()->isPointerTy()) {
                elemVal = impl_->builder->CreatePtrToInt(elemVal, impl_->i64Type);
            }
        }
        llvm::Value* curList = impl_->builder->CreateLoad(impl_->i8PtrType, listAlloca);
        impl_->builder->CreateCall(impl_->runtimeFuncs[appendFn], {curList, elemVal});
    };

    if (condition) {
        condition->accept(*this);
        llvm::Value* filterCond = emitCompFilterTruth(impl_->lastValue);
        auto* appendBB = llvm::BasicBlock::Create(*impl_->context, bbPrefix + "append", func);
        auto* skipBB = llvm::BasicBlock::Create(*impl_->context, bbPrefix + "skip", func);
        impl_->builder->CreateCondBr(filterCond, appendBB, skipBB);

        impl_->builder->SetInsertPoint(appendBB);
        evalCoerceAppend();
        impl_->builder->CreateBr(skipBB);

        impl_->builder->SetInsertPoint(skipBB);
    } else {
        evalCoerceAppend();
    }
}

void CodeGen::bindCompElemVar(Expr* iterable, const std::string& varName,
                              llvm::Value* collVal, llvm::Value* curIdx) {
    Type::Kind elemKind = impl_->getIterableElementKind(iterable);
    Impl::VarKind loopKind = Impl::typeKindToVarKind(elemKind);
    auto* elemAlloca = impl_->bindListElemTyped(
        impl_->currentFunction, collVal, curIdx, varName, loopKind);
    impl_->setVar(varName, elemAlloca, loopKind);
    if (Impl::isHeapKind(loopKind))
        impl_->scopes.back().borrowed.insert(varName);
}

void CodeGen::emitCompExtraClauses(std::vector<CompClause>& clauses, size_t clauseIdx,
                                   const std::function<void()>& innermost) {
    if (clauseIdx >= clauses.size()) {
        innermost();
        return;
    }
    auto* func = impl_->currentFunction;
    auto& clause = clauses[clauseIdx];
    std::string ecVarName = clause.varNames.empty() ? "__ec" : clause.varNames[0];

    auto* ecCallExpr = dynamic_cast<CallExpr*>(clause.iterable.get());
    auto* ecCalleeName = ecCallExpr ? dynamic_cast<NameExpr*>(ecCallExpr->callee.get()) : nullptr;
    bool ecIsRange = ecCalleeName && ecCalleeName->name == "range";

    auto emitFilteredRecurse = [&]() {
        if (clause.condition) {
            clause.condition->accept(*this);
            llvm::Value* ecFilter = emitCompFilterTruth(impl_->lastValue);
            auto* ecPassBB = llvm::BasicBlock::Create(*impl_->context, "ecpass", func);
            auto* ecSkipBB = llvm::BasicBlock::Create(*impl_->context, "ecskip", func);
            impl_->builder->CreateCondBr(ecFilter, ecPassBB, ecSkipBB);
            impl_->builder->SetInsertPoint(ecPassBB);
            emitCompExtraClauses(clauses, clauseIdx + 1, innermost);
            if (!impl_->builder->GetInsertBlock()->getTerminator())
                impl_->builder->CreateBr(ecSkipBB);
            impl_->builder->SetInsertPoint(ecSkipBB);
        } else {
            emitCompExtraClauses(clauses, clauseIdx + 1, innermost);
        }
    };

    if (ecIsRange) {
        llvm::Value* ecStart;
        llvm::Value* ecEnd;
        llvm::Value* ecStep;
        emitCompRangeArgs(ecCallExpr, ecStart, ecEnd, ecStep);
        auto* ecVar = impl_->createEntryAlloca(func, ecVarName, impl_->i64Type);
        impl_->builder->CreateStore(ecStart, ecVar);
        auto* ecCondBB = llvm::BasicBlock::Create(*impl_->context, "eccond", func);
        auto* ecBodyBB = llvm::BasicBlock::Create(*impl_->context, "ecbody", func);
        auto* ecIncBB = llvm::BasicBlock::Create(*impl_->context, "ecinc", func);
        auto* ecEndBB = llvm::BasicBlock::Create(*impl_->context, "ecend", func);
        impl_->builder->CreateBr(ecCondBB);
        impl_->builder->SetInsertPoint(ecCondBB);
        llvm::Value* ecCur = impl_->builder->CreateLoad(impl_->i64Type, ecVar, "eci");
        llvm::Value* ecCmp = impl_->builder->CreateICmpSLT(ecCur, ecEnd, "eccmp");
        impl_->builder->CreateCondBr(ecCmp, ecBodyBB, ecEndBB);
        impl_->builder->SetInsertPoint(ecBodyBB);
        impl_->pushScope();
        impl_->setVar(ecVarName, ecVar, Impl::VarKind::Int);

        emitFilteredRecurse();

        impl_->emitScopeCleanup();
        impl_->popScope();
        if (!impl_->builder->GetInsertBlock()->getTerminator())
            impl_->builder->CreateBr(ecIncBB);
        impl_->builder->SetInsertPoint(ecIncBB);
        ecCur = impl_->builder->CreateLoad(impl_->i64Type, ecVar, "eci");
        llvm::Value* ecNext = impl_->builder->CreateAdd(ecCur, ecStep, "ecinc");
        impl_->builder->CreateStore(ecNext, ecVar);
        impl_->builder->CreateBr(ecCondBB);
        impl_->builder->SetInsertPoint(ecEndBB);
    } else {
        clause.iterable->accept(*this);
        llvm::Value* ecColl = impl_->lastValue;
        bool ecFromDict = impl_->isBareDictIterable(clause.iterable.get());
        bool ecOwnedIterTemp = !ecFromDict &&
            !Impl::isBorrowedHeapExpr(clause.iterable.get()) &&
            clause.iterable->type &&
            (clause.iterable->type->kind() == Type::Kind::List ||
             clause.iterable->type->kind() == Type::Kind::Set ||
             clause.iterable->type->kind() == Type::Kind::Tuple);
        if (ecFromDict)
            ecColl = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_keys"], {ecColl}, "compdictkeys");
        llvm::Value* ecLen = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_list_len"], {ecColl}, "eclen");
        auto* ecIdx = impl_->createEntryAlloca(func, "__ecidx", impl_->i64Type);
        impl_->builder->CreateStore(llvm::ConstantInt::get(impl_->i64Type, 0), ecIdx);
        auto* ecCondBB = llvm::BasicBlock::Create(*impl_->context, "eccond", func);
        auto* ecBodyBB = llvm::BasicBlock::Create(*impl_->context, "ecbody", func);
        auto* ecIncBB = llvm::BasicBlock::Create(*impl_->context, "ecinc", func);
        auto* ecEndBB = llvm::BasicBlock::Create(*impl_->context, "ecend", func);
        impl_->builder->CreateBr(ecCondBB);
        impl_->builder->SetInsertPoint(ecCondBB);
        llvm::Value* ecCurIdx = impl_->builder->CreateLoad(impl_->i64Type, ecIdx, "ecidx");
        llvm::Value* ecCmp = impl_->builder->CreateICmpSLT(ecCurIdx, ecLen, "eccmp");
        impl_->builder->CreateCondBr(ecCmp, ecBodyBB, ecEndBB);
        impl_->builder->SetInsertPoint(ecBodyBB);
        impl_->pushScope();
        bindCompElemVar(clause.iterable.get(), ecVarName, ecColl, ecCurIdx);

        emitFilteredRecurse();

        impl_->emitScopeCleanup();
        impl_->popScope();
        if (!impl_->builder->GetInsertBlock()->getTerminator())
            impl_->builder->CreateBr(ecIncBB);
        impl_->builder->SetInsertPoint(ecIncBB);
        ecCurIdx = impl_->builder->CreateLoad(impl_->i64Type, ecIdx, "ecidx");
        llvm::Value* ecNextIdx = impl_->builder->CreateAdd(
            ecCurIdx, llvm::ConstantInt::get(impl_->i64Type, 1), "ecinc");
        impl_->builder->CreateStore(ecNextIdx, ecIdx);
        impl_->builder->CreateBr(ecCondBB);
        impl_->builder->SetInsertPoint(ecEndBB);
        if (ecFromDict || ecOwnedIterTemp)
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {ecColl});
    }
}

void CodeGen::emitCompLoopNest(
    Expr* iterable, const std::vector<std::string>& varNames,
    std::vector<CompClause>& extraClauses, const std::string& bbPrefix,
    const std::string& idxAllocaName, const std::function<void()>& innermost,
    const std::function<void(llvm::Value*, llvm::Value*)>& bindElemVars) {
    if (varNames.empty()) return;
    auto* func = impl_->currentFunction;

    auto* callExpr = dynamic_cast<CallExpr*>(iterable);
    auto* calleeName = callExpr ? dynamic_cast<NameExpr*>(callExpr->callee.get()) : nullptr;
    bool isRange = calleeName && calleeName->name == "range";

    if (isRange) {
        llvm::Value* startVal;
        llvm::Value* endVal;
        llvm::Value* stepVal;
        emitCompRangeArgs(callExpr, startVal, endVal, stepVal);

        auto* loopVar = impl_->createEntryAlloca(func, varNames[0], impl_->i64Type);
        impl_->builder->CreateStore(startVal, loopVar);

        auto* condBB = llvm::BasicBlock::Create(*impl_->context, bbPrefix + "cond", func);
        auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, bbPrefix + "body", func);
        auto* incBB = llvm::BasicBlock::Create(*impl_->context, bbPrefix + "inc", func);
        auto* endBB = llvm::BasicBlock::Create(*impl_->context, bbPrefix + "end", func);

        impl_->builder->CreateBr(condBB);

        impl_->builder->SetInsertPoint(condBB);
        llvm::Value* current = impl_->builder->CreateLoad(impl_->i64Type, loopVar, "i");
        llvm::Value* cond = impl_->builder->CreateICmpSLT(current, endVal, "cmp");
        impl_->builder->CreateCondBr(cond, bodyBB, endBB);

        impl_->builder->SetInsertPoint(bodyBB);
        impl_->pushScope();
        impl_->setVar(varNames[0], loopVar, Impl::VarKind::Int);

        emitCompExtraClauses(extraClauses, 0, innermost);

        impl_->emitScopeCleanup();
        impl_->popScope();
        if (!impl_->builder->GetInsertBlock()->getTerminator())
            impl_->builder->CreateBr(incBB);

        impl_->builder->SetInsertPoint(incBB);
        current = impl_->builder->CreateLoad(impl_->i64Type, loopVar, "i");
        llvm::Value* next = impl_->builder->CreateAdd(current, stepVal, "inc");
        impl_->builder->CreateStore(next, loopVar);
        impl_->builder->CreateBr(condBB);

        impl_->builder->SetInsertPoint(endBB);
    } else {
        iterable->accept(*this);
        llvm::Value* collVal = impl_->lastValue;
        bool collFromDict = impl_->isBareDictIterable(iterable);
        bool ownedIterTemp = !collFromDict && iterable &&
            !Impl::isBorrowedHeapExpr(iterable) && iterable->type &&
            (iterable->type->kind() == Type::Kind::List ||
             iterable->type->kind() == Type::Kind::Set ||
             iterable->type->kind() == Type::Kind::Tuple);
        if (collFromDict)
            collVal = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_keys"], {collVal}, "compdictkeys");
        llvm::Value* collLen = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_list_len"], {collVal}, "colllen");

        auto* idxAlloca = impl_->createEntryAlloca(func, idxAllocaName, impl_->i64Type);
        impl_->builder->CreateStore(llvm::ConstantInt::get(impl_->i64Type, 0), idxAlloca);

        auto* condBB = llvm::BasicBlock::Create(*impl_->context, bbPrefix + "cond", func);
        auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, bbPrefix + "body", func);
        auto* incBB = llvm::BasicBlock::Create(*impl_->context, bbPrefix + "inc", func);
        auto* endBB = llvm::BasicBlock::Create(*impl_->context, bbPrefix + "end", func);

        impl_->builder->CreateBr(condBB);

        impl_->builder->SetInsertPoint(condBB);
        llvm::Value* curIdx = impl_->builder->CreateLoad(impl_->i64Type, idxAlloca, "idx");
        llvm::Value* cond = impl_->builder->CreateICmpSLT(curIdx, collLen, "cmp");
        impl_->builder->CreateCondBr(cond, bodyBB, endBB);

        impl_->builder->SetInsertPoint(bodyBB);
        impl_->pushScope();

        bindElemVars(collVal, curIdx);

        emitCompExtraClauses(extraClauses, 0, innermost);

        impl_->emitScopeCleanup();
        impl_->popScope();
        if (!impl_->builder->GetInsertBlock()->getTerminator())
            impl_->builder->CreateBr(incBB);

        impl_->builder->SetInsertPoint(incBB);
        curIdx = impl_->builder->CreateLoad(impl_->i64Type, idxAlloca, "idx");
        llvm::Value* nextIdx = impl_->builder->CreateAdd(
            curIdx, llvm::ConstantInt::get(impl_->i64Type, 1), "inc");
        impl_->builder->CreateStore(nextIdx, idxAlloca);
        impl_->builder->CreateBr(condBB);

        impl_->builder->SetInsertPoint(endBB);
        if (collFromDict || ownedIterTemp)
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {collVal});
    }
}

void CodeGen::visit(ListCompExpr& node) {
    auto* func = impl_->currentFunction;

    int64_t elemTag = 0;
    if (node.element && node.element->type) {
        elemTag = impl_->typeKindToElemTag(node.element->type->kind());
    }

    llvm::Value* capVal = llvm::ConstantInt::get(impl_->i64Type, 8);
    llvm::Value* list;
    if (elemTag != 0) {
        llvm::Value* tagVal = llvm::ConstantInt::get(impl_->i64Type, elemTag);
        list = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_list_new_tagged"], {capVal, tagVal}, "complist");
    } else {
        list = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_list_new"], {capVal}, "complist");
    }
    auto* listAlloca = impl_->createEntryAlloca(func, "__complist", impl_->i8PtrType);
    impl_->builder->CreateStore(list, listAlloca);

    auto* compCleanupBase = impl_->emitCleanupPushTemp(list, Impl::DCLEAN_OBJ);
    emitCompLoopNest(
        node.iterable.get(), {node.varName}, node.extraClauses, "comp",
        "__compidx",
        [&]() {
            emitCompAppendBody(node.element.get(), node.condition.get(),
                               listAlloca, elemTag, "comp");
        },
        [&](llvm::Value* collVal, llvm::Value* curIdx) {
            bindCompElemVar(node.iterable.get(), node.varName, collVal, curIdx);
        });

    impl_->emitCleanupPopTemp(compCleanupBase);
    impl_->lastValue = list;
}

void CodeGen::visit(DictCompExpr& node) {
    auto* func = impl_->currentFunction;

    llvm::Value* capVal = llvm::ConstantInt::get(impl_->i64Type, 8);
    llvm::Value* dict = impl_->builder->CreateCall(
        impl_->runtimeFuncs["dragon_dict_new"], {capVal}, "compdict");
    auto* dictAlloca = impl_->createEntryAlloca(func, "__compdict", impl_->i8PtrType);
    impl_->builder->CreateStore(dict, dictAlloca);

    auto emitInnermostBody = [&]() {
        node.key->accept(*this);
        llvm::Value* keyVal = impl_->lastValue;
        bool keyIsPtr = keyVal->getType()->isPointerTy();
        if (!keyIsPtr) {
            if (keyVal->getType() == impl_->i1Type)
                keyVal = impl_->builder->CreateZExt(keyVal, impl_->i64Type);
            else if (keyVal->getType() == impl_->f64Type)
                keyVal = impl_->builder->CreateBitCast(keyVal, impl_->i64Type);
        }
        const std::string dictSetFn =
            keyIsPtr ? "dragon_dict_set_tagged" : "dragon_dict_int_set_tagged";

        node.value->accept(*this);
        llvm::Value* valVal = impl_->lastValue;

        int64_t compTag = 0;
        bool valIsPtr = false;
        if (valVal->getType() == impl_->i1Type) {
            compTag = 3;
            valVal = impl_->builder->CreateZExt(valVal, impl_->i64Type);
        } else if (valVal->getType() == impl_->f64Type) {
            compTag = 2;
            valVal = impl_->builder->CreateBitCast(valVal, impl_->i64Type);
        } else if (valVal->getType()->isPointerTy()) {
            compTag = impl_->inferPtrValueTag(node.value.get());
            valIsPtr = true;
            valVal = impl_->builder->CreatePtrToInt(valVal, impl_->i64Type);
        }
        llvm::Value* compTagVal = llvm::ConstantInt::get(impl_->i64Type, compTag);

        // dragon_dict_set_tagged expects one owned ref per key/value: a BORROWED key/value must be incref'd, a literal
        // key heap-promoted; without this the dict aliased the freed source list's ref (UAF). Emitted at the INSERT site.
        auto emitDictSet = [&]() {
            llvm::Value* k = keyVal;
            llvm::Value* v = valVal;
            if (impl_->options.gcMode == GCMode::RC && keyIsPtr) {
                Expr* keyExpr = node.key.get();
                bool keyIsLiteral =
                    dynamic_cast<StringLiteral*>(keyExpr) ||
                    (dynamic_cast<NameExpr*>(keyExpr) &&
                     impl_->lookupVarKind(
                         static_cast<NameExpr*>(keyExpr)->name)
                         == Impl::VarKind::StrLiteral);
                if (keyIsLiteral)
                    k = impl_->ensureHeapString(k, keyExpr);
                else if (Impl::isBorrowedHeapExpr(keyExpr))
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_incref_str"], {k});
            }
            if (impl_->options.gcMode == GCMode::RC && valIsPtr) {
                auto* vp = impl_->builder->CreateIntToPtr(
                    v, impl_->i8PtrType, "dcomp.v");
                impl_->increfBorrowedContainerValue(vp, node.value.get(), compTag);
            }
            llvm::Value* curDict = impl_->builder->CreateLoad(impl_->i8PtrType, dictAlloca);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs[dictSetFn], {curDict, k, v, compTagVal});
        };

        if (node.condition) {
            node.condition->accept(*this);
            llvm::Value* filterCond = emitCompFilterTruth(impl_->lastValue);
            auto* insertBB = llvm::BasicBlock::Create(*impl_->context, "dcompinsert", func);
            auto* skipBB = llvm::BasicBlock::Create(*impl_->context, "dcompskip", func);
            impl_->builder->CreateCondBr(filterCond, insertBB, skipBB);

            impl_->builder->SetInsertPoint(insertBB);
            emitDictSet();
            impl_->builder->CreateBr(skipBB);

            impl_->builder->SetInsertPoint(skipBB);
        } else {
            emitDictSet();
        }
    };

    auto* compCleanupBase = impl_->emitCleanupPushTemp(dict, Impl::DCLEAN_OBJ);
    emitCompLoopNest(
        node.iterable.get(), node.varNames, node.extraClauses, "dcomp",
        "__dcompidx", emitInnermostBody,
        [&](llvm::Value* collVal, llvm::Value* curIdx) {
            if (node.varNames.size() == 1) {
                bindCompElemVar(node.iterable.get(), node.varNames[0], collVal,
                                curIdx);
                return;
            }
            llvm::Value* rawElem = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_get"], {collVal, curIdx}, "rawelem");
            llvm::Value* tuplePtr = impl_->builder->CreateIntToPtr(rawElem, impl_->i8PtrType, "tupleptr");
            std::vector<Type::Kind> ekinds(node.varNames.size(), Type::Kind::Int);
            if (node.iterable && node.iterable->type &&
                node.iterable->type->kind() == Type::Kind::List) {
                auto& lt = static_cast<ListType&>(*node.iterable->type);
                if (lt.elementType && lt.elementType->kind() == Type::Kind::Tuple) {
                    auto& tt = static_cast<TupleType&>(*lt.elementType);
                    for (size_t i = 0; i < ekinds.size() && i < tt.elementTypes.size(); ++i)
                        if (tt.elementTypes[i]) ekinds[i] = tt.elementTypes[i]->kind();
                }
            }
            for (size_t vi = 0; vi < node.varNames.size(); ++vi) {
                llvm::Value* fieldIdx = llvm::ConstantInt::get(impl_->i64Type, vi);
                llvm::Value* fieldVal = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_tuple_get"], {tuplePtr, fieldIdx},
                    node.varNames[vi] + "_val");
                Type::Kind ek = ekinds[vi];
                llvm::Type* slotTy = impl_->i64Type;
                llvm::Value* slotVal = fieldVal;
                Impl::VarKind vk = Impl::VarKind::Int;
                if (ek == Type::Kind::Float) {
                    slotTy = impl_->f64Type; vk = Impl::VarKind::Float;
                    slotVal = impl_->builder->CreateBitCast(
                        fieldVal, impl_->f64Type, "dc.f");
                } else if (ek == Type::Kind::Bool) {
                    slotTy = impl_->i1Type; vk = Impl::VarKind::Bool;
                    slotVal = impl_->builder->CreateICmpNE(
                        fieldVal, llvm::ConstantInt::get(impl_->i64Type, 0), "dc.b");
                } else if (Impl::isHeapTypeKind(ek)) {
                    slotTy = impl_->i8PtrType;
                    vk = Impl::typeKindToVarKind(ek);
                    slotVal = impl_->builder->CreateIntToPtr(
                        fieldVal, impl_->i8PtrType, "dc.p");
                }
                auto* fieldAlloca = impl_->createEntryAlloca(func, node.varNames[vi], slotTy);
                impl_->builder->CreateStore(slotVal, fieldAlloca);
                impl_->setVar(node.varNames[vi], fieldAlloca, vk);
                if (Impl::isHeapKind(vk))
                    impl_->scopes.back().borrowed.insert(node.varNames[vi]);
            }
        });

    impl_->emitCleanupPopTemp(compCleanupBase);
    impl_->lastValue = dict;
}

void CodeGen::visit(SetCompExpr& node) {
    auto* func = impl_->currentFunction;

    int64_t elemTag = 0;
    if (node.element && node.element->type) {
        elemTag = impl_->typeKindToElemTag(node.element->type->kind());
    }

    llvm::Value* set;
    if (elemTag != 0) {
        auto* tagVal = llvm::ConstantInt::get(impl_->i64Type, elemTag);
        set = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_set_new_tagged"], {tagVal}, "compset");
    } else {
        set = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_set_new"], {}, "compset");
    }
    auto* setAlloca = impl_->createEntryAlloca(func, "__compset", impl_->i8PtrType);
    impl_->builder->CreateStore(set, setAlloca);

    auto emitInnermostBody = [&]() {
        auto evalCoerceAdd = [&]() {
            node.element->accept(*this);
            llvm::Value* elemVal = impl_->lastValue;
            llvm::Value* elemPtr =
                elemVal->getType()->isPointerTy() ? elemVal : nullptr;

            if (elemVal->getType() == impl_->i1Type) {
                elemVal = impl_->builder->CreateZExt(elemVal, impl_->i64Type);
            } else if (elemVal->getType() == impl_->f64Type) {
                elemVal = impl_->builder->CreateBitCast(elemVal, impl_->i64Type);
            } else if (elemPtr) {
                elemVal = impl_->builder->CreatePtrToInt(elemVal, impl_->i64Type);
            }

            llvm::Value* curSet = impl_->builder->CreateLoad(impl_->i8PtrType, setAlloca);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_set_add"], {curSet, elemVal});

            if (elemPtr && elemTag != 0 && impl_->options.gcMode == GCMode::RC &&
                !Impl::isBorrowedHeapExpr(node.element.get())) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs[elemTag == 1 ? "dragon_decref_str"
                                                     : "dragon_decref"],
                    {elemPtr});
            }
        };

        if (node.condition) {
            node.condition->accept(*this);
            llvm::Value* filterCond = emitCompFilterTruth(impl_->lastValue);
            auto* addBB = llvm::BasicBlock::Create(*impl_->context, "scompadd", func);
            auto* skipBB = llvm::BasicBlock::Create(*impl_->context, "scompskip", func);
            impl_->builder->CreateCondBr(filterCond, addBB, skipBB);

            impl_->builder->SetInsertPoint(addBB);
            evalCoerceAdd();
            impl_->builder->CreateBr(skipBB);

            impl_->builder->SetInsertPoint(skipBB);
        } else {
            evalCoerceAdd();
        }
    };

    auto* compCleanupBase = impl_->emitCleanupPushTemp(set, Impl::DCLEAN_OBJ);
    emitCompLoopNest(
        node.iterable.get(), {node.varName}, node.extraClauses, "scomp",
        "__scompidx", emitInnermostBody,
        [&](llvm::Value* collVal, llvm::Value* curIdx) {
            bindCompElemVar(node.iterable.get(), node.varName, collVal, curIdx);
        });

    impl_->emitCleanupPopTemp(compCleanupBase);
    impl_->lastValue = set;
}

void CodeGen::visit(GeneratorExpr& node) {
    auto* func = impl_->currentFunction;

    int64_t elemTag = 0;
    if (node.element && node.element->type) {
        elemTag = impl_->typeKindToElemTag(node.element->type->kind());
    }

    llvm::Value* capVal = llvm::ConstantInt::get(impl_->i64Type, 8);
    llvm::Value* list;
    if (elemTag != 0) {
        llvm::Value* tagVal = llvm::ConstantInt::get(impl_->i64Type, elemTag);
        list = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_list_new_tagged"], {capVal, tagVal}, "genlist");
    } else {
        list = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_list_new"], {capVal}, "genlist");
    }
    auto* listAlloca = impl_->createEntryAlloca(func, "__genlist", impl_->i8PtrType);
    impl_->builder->CreateStore(list, listAlloca);

    auto* compCleanupBase = impl_->emitCleanupPushTemp(list, Impl::DCLEAN_OBJ);
    emitCompLoopNest(
        node.iterable.get(), {node.varName}, node.extraClauses, "gen",
        "__genidx",
        [&]() {
            emitCompAppendBody(node.element.get(), node.condition.get(),
                               listAlloca, elemTag, "gen");
        },
        [&](llvm::Value* collVal, llvm::Value* curIdx) {
            bindCompElemVar(node.iterable.get(), node.varName, collVal, curIdx);
        });

    impl_->emitCleanupPopTemp(compCleanupBase);
    impl_->lastValue = list;
}
}
