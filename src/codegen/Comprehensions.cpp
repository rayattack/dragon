#include "../CodeGenImpl.h"

namespace dragon {

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

    auto* callExpr = dynamic_cast<CallExpr*>(node.iterable.get());
    auto* calleeName = callExpr ? dynamic_cast<NameExpr*>(callExpr->callee.get()) : nullptr;
    bool isRange = calleeName && calleeName->name == "range";

    auto emitInnermostBody = [&]() {
        bool isF64 = (elemTag == 2);
        bool isPtr = (elemTag == 1 || elemTag == 5 || elemTag == 6 || elemTag == 7);
        const char* appendFn = isF64 ? "dragon_list_append_f64"
                              : isPtr ? "dragon_list_append_ptr"
                                      : "dragon_list_append";

        auto evalCoerceAppend = [&]() {
            node.element->accept(*this);
            llvm::Value* elemVal = impl_->lastValue;

            if (elemTag == TAG_STR && elemVal->getType()->isPointerTy()) {
                elemVal = impl_->ensureHeapString(elemVal, node.element.get());
            }

            // dragon_list_append_ptr ADOPTS the reference: a BORROWED element (loop var, field, xs[i]) must be
            // incref'd or the source and result both decref the same +1 (double free); an owned temp already carries its +1.
            if (isPtr && impl_->options.gcMode == GCMode::RC &&
                elemVal->getType()->isPointerTy() &&
                Impl::isBorrowedHeapExpr(node.element.get())) {
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

        if (node.condition) {
            node.condition->accept(*this);
            llvm::Value* filterCond = impl_->lastValue;
            if (filterCond->getType() == impl_->i64Type) {
                filterCond = impl_->builder->CreateICmpNE(
                    filterCond, llvm::ConstantInt::get(impl_->i64Type, 0));
            } else if (filterCond->getType() == impl_->f64Type) {
                filterCond = impl_->builder->CreateFCmpONE(
                    filterCond, llvm::ConstantFP::get(impl_->f64Type, 0.0));
            }
            auto* appendBB = llvm::BasicBlock::Create(*impl_->context, "compappend", func);
            auto* skipBB = llvm::BasicBlock::Create(*impl_->context, "compskip", func);
            impl_->builder->CreateCondBr(filterCond, appendBB, skipBB);

            impl_->builder->SetInsertPoint(appendBB);
            evalCoerceAppend();
            impl_->builder->CreateBr(skipBB);

            impl_->builder->SetInsertPoint(skipBB);
        } else {
            evalCoerceAppend();
        }
    };

    std::function<void(size_t)> emitExtraClauses = [&](size_t clauseIdx) {
        if (clauseIdx >= node.extraClauses.size()) {
            emitInnermostBody();
            return;
        }
        auto& clause = node.extraClauses[clauseIdx];
        std::string ecVarName = clause.varNames.empty() ? "__ec" : clause.varNames[0];

        auto* ecCallExpr = dynamic_cast<CallExpr*>(clause.iterable.get());
        auto* ecCalleeName = ecCallExpr ? dynamic_cast<NameExpr*>(ecCallExpr->callee.get()) : nullptr;
        bool ecIsRange = ecCalleeName && ecCalleeName->name == "range";

        if (ecIsRange) {
            llvm::Value* ecStart = llvm::ConstantInt::get(impl_->i64Type, 0);
            llvm::Value* ecEnd = nullptr;
            llvm::Value* ecStep = llvm::ConstantInt::get(impl_->i64Type, 1);
            if (ecCallExpr->args.size() == 1) {
                ecCallExpr->args[0]->accept(*this);
                ecEnd = impl_->lastValue;
            } else if (ecCallExpr->args.size() >= 2) {
                ecCallExpr->args[0]->accept(*this);
                ecStart = impl_->lastValue;
                ecCallExpr->args[1]->accept(*this);
                ecEnd = impl_->lastValue;
                if (ecCallExpr->args.size() >= 3) {
                    ecCallExpr->args[2]->accept(*this);
                    ecStep = impl_->lastValue;
                }
            } else {
                ecEnd = llvm::ConstantInt::get(impl_->i64Type, 0);
            }
            auto* ecVar = impl_->createEntryAlloca(func, ecVarName, impl_->i64Type);
            impl_->builder->CreateStore(ecStart, ecVar);
            auto* ecCond = llvm::BasicBlock::Create(*impl_->context, "eccond", func);
            auto* ecBody = llvm::BasicBlock::Create(*impl_->context, "ecbody", func);
            auto* ecInc = llvm::BasicBlock::Create(*impl_->context, "ecinc", func);
            auto* ecEndBB = llvm::BasicBlock::Create(*impl_->context, "ecend", func);
            impl_->builder->CreateBr(ecCond);
            impl_->builder->SetInsertPoint(ecCond);
            llvm::Value* ecCur = impl_->builder->CreateLoad(impl_->i64Type, ecVar, "eci");
            llvm::Value* ecCmp = impl_->builder->CreateICmpSLT(ecCur, ecEnd, "eccmp");
            impl_->builder->CreateCondBr(ecCmp, ecBody, ecEndBB);
            impl_->builder->SetInsertPoint(ecBody);
            impl_->pushScope();
            impl_->setVar(ecVarName, ecVar, Impl::VarKind::Int);

            if (clause.condition) {
                clause.condition->accept(*this);
                llvm::Value* ecFilter = impl_->lastValue;
                if (ecFilter->getType() == impl_->i64Type) {
                    ecFilter = impl_->builder->CreateICmpNE(
                        ecFilter, llvm::ConstantInt::get(impl_->i64Type, 0));
                } else if (ecFilter->getType() == impl_->f64Type) {
                    ecFilter = impl_->builder->CreateFCmpONE(
                        ecFilter, llvm::ConstantFP::get(impl_->f64Type, 0.0));
                }
                auto* ecPassBB = llvm::BasicBlock::Create(*impl_->context, "ecpass", func);
                auto* ecSkipBB = llvm::BasicBlock::Create(*impl_->context, "ecskip", func);
                impl_->builder->CreateCondBr(ecFilter, ecPassBB, ecSkipBB);
                impl_->builder->SetInsertPoint(ecPassBB);
                emitExtraClauses(clauseIdx + 1);
                if (!impl_->builder->GetInsertBlock()->getTerminator())
                    impl_->builder->CreateBr(ecSkipBB);
                impl_->builder->SetInsertPoint(ecSkipBB);
            } else {
                emitExtraClauses(clauseIdx + 1);
            }

            impl_->emitScopeCleanup();
            impl_->emitScopeCleanup();
        impl_->popScope();
            if (!impl_->builder->GetInsertBlock()->getTerminator())
                impl_->builder->CreateBr(ecInc);
            impl_->builder->SetInsertPoint(ecInc);
            ecCur = impl_->builder->CreateLoad(impl_->i64Type, ecVar, "eci");
            llvm::Value* ecNext = impl_->builder->CreateAdd(ecCur, ecStep, "ecinc");
            impl_->builder->CreateStore(ecNext, ecVar);
            impl_->builder->CreateBr(ecCond);
            impl_->builder->SetInsertPoint(ecEndBB);
        } else {
            clause.iterable->accept(*this);
            llvm::Value* ecColl = impl_->lastValue;
            bool ecFromDict = impl_->isBareDictIterable(clause.iterable.get());
            if (ecFromDict)
                ecColl = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_keys"], {ecColl}, "compdictkeys");
            llvm::Value* ecLen = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_len"], {ecColl}, "eclen");
            auto* ecIdx = impl_->createEntryAlloca(func, "__ecidx", impl_->i64Type);
            impl_->builder->CreateStore(llvm::ConstantInt::get(impl_->i64Type, 0), ecIdx);
            auto* ecCond = llvm::BasicBlock::Create(*impl_->context, "eccond", func);
            auto* ecBody = llvm::BasicBlock::Create(*impl_->context, "ecbody", func);
            auto* ecInc = llvm::BasicBlock::Create(*impl_->context, "ecinc", func);
            auto* ecEndBB = llvm::BasicBlock::Create(*impl_->context, "ecend", func);
            impl_->builder->CreateBr(ecCond);
            impl_->builder->SetInsertPoint(ecCond);
            llvm::Value* ecCurIdx = impl_->builder->CreateLoad(impl_->i64Type, ecIdx, "ecidx");
            llvm::Value* ecCmp = impl_->builder->CreateICmpSLT(ecCurIdx, ecLen, "eccmp");
            impl_->builder->CreateCondBr(ecCmp, ecBody, ecEndBB);
            impl_->builder->SetInsertPoint(ecBody);
            impl_->pushScope();
            Type::Kind ecElemKind = impl_->getIterableElementKind(clause.iterable.get());
            Impl::VarKind ecLoopKind = Impl::typeKindToVarKind(ecElemKind);
            auto* ecVar = impl_->bindListElemTyped(
                func, ecColl, ecCurIdx, ecVarName, ecLoopKind);
            impl_->setVar(ecVarName, ecVar, ecLoopKind);
            if (Impl::isHeapKind(ecLoopKind))
                impl_->scopes.back().borrowed.insert(ecVarName);

            if (clause.condition) {
                clause.condition->accept(*this);
                llvm::Value* ecFilter = impl_->lastValue;
                if (ecFilter->getType() == impl_->i64Type) {
                    ecFilter = impl_->builder->CreateICmpNE(
                        ecFilter, llvm::ConstantInt::get(impl_->i64Type, 0));
                } else if (ecFilter->getType() == impl_->f64Type) {
                    ecFilter = impl_->builder->CreateFCmpONE(
                        ecFilter, llvm::ConstantFP::get(impl_->f64Type, 0.0));
                }
                auto* ecPassBB = llvm::BasicBlock::Create(*impl_->context, "ecpass", func);
                auto* ecSkipBB = llvm::BasicBlock::Create(*impl_->context, "ecskip", func);
                impl_->builder->CreateCondBr(ecFilter, ecPassBB, ecSkipBB);
                impl_->builder->SetInsertPoint(ecPassBB);
                emitExtraClauses(clauseIdx + 1);
                if (!impl_->builder->GetInsertBlock()->getTerminator())
                    impl_->builder->CreateBr(ecSkipBB);
                impl_->builder->SetInsertPoint(ecSkipBB);
            } else {
                emitExtraClauses(clauseIdx + 1);
            }

            impl_->emitScopeCleanup();
            impl_->emitScopeCleanup();
        impl_->popScope();
            if (!impl_->builder->GetInsertBlock()->getTerminator())
                impl_->builder->CreateBr(ecInc);
            impl_->builder->SetInsertPoint(ecInc);
            ecCurIdx = impl_->builder->CreateLoad(impl_->i64Type, ecIdx, "ecidx");
            llvm::Value* ecNextIdx = impl_->builder->CreateAdd(
                ecCurIdx, llvm::ConstantInt::get(impl_->i64Type, 1), "ecinc");
            impl_->builder->CreateStore(ecNextIdx, ecIdx);
            impl_->builder->CreateBr(ecCond);
            impl_->builder->SetInsertPoint(ecEndBB);
            if (ecFromDict)
                impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {ecColl});
        }
    };

    if (isRange) {
        llvm::Value* startVal = llvm::ConstantInt::get(impl_->i64Type, 0);
        llvm::Value* endVal = nullptr;
        llvm::Value* stepVal = llvm::ConstantInt::get(impl_->i64Type, 1);

        if (callExpr->args.size() == 1) {
            callExpr->args[0]->accept(*this);
            endVal = impl_->lastValue;
        } else if (callExpr->args.size() >= 2) {
            callExpr->args[0]->accept(*this);
            startVal = impl_->lastValue;
            callExpr->args[1]->accept(*this);
            endVal = impl_->lastValue;
            if (callExpr->args.size() >= 3) {
                callExpr->args[2]->accept(*this);
                stepVal = impl_->lastValue;
            }
        } else {
            endVal = llvm::ConstantInt::get(impl_->i64Type, 0);
        }

        auto* loopVar = impl_->createEntryAlloca(func, node.varName, impl_->i64Type);
        impl_->builder->CreateStore(startVal, loopVar);

        auto* condBB = llvm::BasicBlock::Create(*impl_->context, "compcond", func);
        auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, "compbody", func);
        auto* incBB = llvm::BasicBlock::Create(*impl_->context, "compinc", func);
        auto* endBB = llvm::BasicBlock::Create(*impl_->context, "compend", func);

        impl_->builder->CreateBr(condBB);

        impl_->builder->SetInsertPoint(condBB);
        llvm::Value* current = impl_->builder->CreateLoad(impl_->i64Type, loopVar, "i");
        llvm::Value* cond = impl_->builder->CreateICmpSLT(current, endVal, "cmp");
        impl_->builder->CreateCondBr(cond, bodyBB, endBB);

        impl_->builder->SetInsertPoint(bodyBB);
        impl_->pushScope();
        impl_->setVar(node.varName, loopVar, Impl::VarKind::Int);

        emitExtraClauses(0);

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
        node.iterable->accept(*this);
        llvm::Value* collVal = impl_->lastValue;
        bool collFromDict = impl_->isBareDictIterable(node.iterable.get());
        bool ownedIterTemp = !collFromDict && node.iterable &&
            !Impl::isBorrowedHeapExpr(node.iterable.get()) && node.iterable->type &&
            (node.iterable->type->kind() == Type::Kind::List ||
             node.iterable->type->kind() == Type::Kind::Set ||
             node.iterable->type->kind() == Type::Kind::Tuple);
        if (collFromDict)
            collVal = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_keys"], {collVal}, "compdictkeys");
        llvm::Value* collLen = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_list_len"], {collVal}, "colllen");

        auto* idxAlloca = impl_->createEntryAlloca(func, "__compidx", impl_->i64Type);
        impl_->builder->CreateStore(llvm::ConstantInt::get(impl_->i64Type, 0), idxAlloca);

        auto* condBB = llvm::BasicBlock::Create(*impl_->context, "compcond", func);
        auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, "compbody", func);
        auto* incBB = llvm::BasicBlock::Create(*impl_->context, "compinc", func);
        auto* endBB = llvm::BasicBlock::Create(*impl_->context, "compend", func);

        impl_->builder->CreateBr(condBB);

        impl_->builder->SetInsertPoint(condBB);
        llvm::Value* curIdx = impl_->builder->CreateLoad(impl_->i64Type, idxAlloca, "idx");
        llvm::Value* cond = impl_->builder->CreateICmpSLT(curIdx, collLen, "cmp");
        impl_->builder->CreateCondBr(cond, bodyBB, endBB);

        impl_->builder->SetInsertPoint(bodyBB);
        impl_->pushScope();

        Type::Kind elemKind = impl_->getIterableElementKind(node.iterable.get());
        Impl::VarKind loopKind = Impl::typeKindToVarKind(elemKind);
        auto* elemAlloca = impl_->bindListElemTyped(
            func, collVal, curIdx, node.varName, loopKind);
        impl_->setVar(node.varName, elemAlloca, loopKind);
        if (Impl::isHeapKind(loopKind)) impl_->scopes.back().borrowed.insert(node.varName);

        emitExtraClauses(0);

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

    impl_->lastValue = impl_->builder->CreateLoad(impl_->i8PtrType, listAlloca);
}
void CodeGen::visit(DictCompExpr& node) {
    auto* func = impl_->currentFunction;

    llvm::Value* capVal = llvm::ConstantInt::get(impl_->i64Type, 8);
    llvm::Value* dict = impl_->builder->CreateCall(
        impl_->runtimeFuncs["dragon_dict_new"], {capVal}, "compdict");
    auto* dictAlloca = impl_->createEntryAlloca(func, "__compdict", impl_->i8PtrType);
    impl_->builder->CreateStore(dict, dictAlloca);

    auto* callExpr = dynamic_cast<CallExpr*>(node.iterable.get());
    auto* calleeName = callExpr ? dynamic_cast<NameExpr*>(callExpr->callee.get()) : nullptr;
    bool isRange = calleeName && calleeName->name == "range";

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
        if (valVal->getType() == impl_->i1Type) {
            compTag = 3;
            valVal = impl_->builder->CreateZExt(valVal, impl_->i64Type);
        } else if (valVal->getType() == impl_->f64Type) {
            compTag = 2;
            valVal = impl_->builder->CreateBitCast(valVal, impl_->i64Type);
        } else if (valVal->getType()->isPointerTy()) {
            compTag = 1;
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
            if (impl_->options.gcMode == GCMode::RC && compTag == 1 &&
                Impl::isBorrowedHeapExpr(node.value.get())) {
                auto* vp = impl_->builder->CreateIntToPtr(
                    v, impl_->i8PtrType, "dcomp.v");
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_incref_str"], {vp});
            }
            llvm::Value* curDict = impl_->builder->CreateLoad(impl_->i8PtrType, dictAlloca);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs[dictSetFn], {curDict, k, v, compTagVal});
        };

        if (node.condition) {
            node.condition->accept(*this);
            llvm::Value* filterCond = impl_->lastValue;
            if (filterCond->getType() == impl_->i64Type) {
                filterCond = impl_->builder->CreateICmpNE(
                    filterCond, llvm::ConstantInt::get(impl_->i64Type, 0));
            } else if (filterCond->getType() == impl_->f64Type) {
                filterCond = impl_->builder->CreateFCmpONE(
                    filterCond, llvm::ConstantFP::get(impl_->f64Type, 0.0));
            }
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

    std::function<void(size_t)> emitExtraClauses = [&](size_t clauseIdx) {
        if (clauseIdx >= node.extraClauses.size()) {
            emitInnermostBody();
            return;
        }
        auto& clause = node.extraClauses[clauseIdx];
        std::string ecVarName = clause.varNames.empty() ? "__ec" : clause.varNames[0];

        auto* ecCallExpr = dynamic_cast<CallExpr*>(clause.iterable.get());
        auto* ecCalleeName = ecCallExpr ? dynamic_cast<NameExpr*>(ecCallExpr->callee.get()) : nullptr;
        bool ecIsRange = ecCalleeName && ecCalleeName->name == "range";

        if (ecIsRange) {
            llvm::Value* ecStart = llvm::ConstantInt::get(impl_->i64Type, 0);
            llvm::Value* ecEnd = nullptr;
            llvm::Value* ecStep = llvm::ConstantInt::get(impl_->i64Type, 1);
            if (ecCallExpr->args.size() == 1) {
                ecCallExpr->args[0]->accept(*this);
                ecEnd = impl_->lastValue;
            } else if (ecCallExpr->args.size() >= 2) {
                ecCallExpr->args[0]->accept(*this);
                ecStart = impl_->lastValue;
                ecCallExpr->args[1]->accept(*this);
                ecEnd = impl_->lastValue;
                if (ecCallExpr->args.size() >= 3) {
                    ecCallExpr->args[2]->accept(*this);
                    ecStep = impl_->lastValue;
                }
            } else {
                ecEnd = llvm::ConstantInt::get(impl_->i64Type, 0);
            }
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
            if (clause.condition) {
                clause.condition->accept(*this);
                llvm::Value* ecFilter = impl_->lastValue;
                if (ecFilter->getType() == impl_->i64Type)
                    ecFilter = impl_->builder->CreateICmpNE(ecFilter, llvm::ConstantInt::get(impl_->i64Type, 0));
                else if (ecFilter->getType() == impl_->f64Type)
                    ecFilter = impl_->builder->CreateFCmpONE(ecFilter, llvm::ConstantFP::get(impl_->f64Type, 0.0));
                auto* ecPassBB = llvm::BasicBlock::Create(*impl_->context, "ecpass", func);
                auto* ecSkipBB = llvm::BasicBlock::Create(*impl_->context, "ecskip", func);
                impl_->builder->CreateCondBr(ecFilter, ecPassBB, ecSkipBB);
                impl_->builder->SetInsertPoint(ecPassBB);
                emitExtraClauses(clauseIdx + 1);
                if (!impl_->builder->GetInsertBlock()->getTerminator())
                    impl_->builder->CreateBr(ecSkipBB);
                impl_->builder->SetInsertPoint(ecSkipBB);
            } else {
                emitExtraClauses(clauseIdx + 1);
            }
            impl_->emitScopeCleanup();
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
            Type::Kind ecElemKind = impl_->getIterableElementKind(clause.iterable.get());
            Impl::VarKind ecLoopKind = Impl::typeKindToVarKind(ecElemKind);
            auto* ecVar = impl_->bindListElemTyped(
                func, ecColl, ecCurIdx, ecVarName, ecLoopKind);
            impl_->setVar(ecVarName, ecVar, ecLoopKind);
            if (Impl::isHeapKind(ecLoopKind))
                impl_->scopes.back().borrowed.insert(ecVarName);
            if (clause.condition) {
                clause.condition->accept(*this);
                llvm::Value* ecFilter = impl_->lastValue;
                if (ecFilter->getType() == impl_->i64Type)
                    ecFilter = impl_->builder->CreateICmpNE(ecFilter, llvm::ConstantInt::get(impl_->i64Type, 0));
                else if (ecFilter->getType() == impl_->f64Type)
                    ecFilter = impl_->builder->CreateFCmpONE(ecFilter, llvm::ConstantFP::get(impl_->f64Type, 0.0));
                auto* ecPassBB = llvm::BasicBlock::Create(*impl_->context, "ecpass", func);
                auto* ecSkipBB = llvm::BasicBlock::Create(*impl_->context, "ecskip", func);
                impl_->builder->CreateCondBr(ecFilter, ecPassBB, ecSkipBB);
                impl_->builder->SetInsertPoint(ecPassBB);
                emitExtraClauses(clauseIdx + 1);
                if (!impl_->builder->GetInsertBlock()->getTerminator())
                    impl_->builder->CreateBr(ecSkipBB);
                impl_->builder->SetInsertPoint(ecSkipBB);
            } else {
                emitExtraClauses(clauseIdx + 1);
            }
            impl_->emitScopeCleanup();
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
            if (ecFromDict)
                impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {ecColl});
        }
    };

    if (isRange && !node.varNames.empty()) {
        llvm::Value* startVal = llvm::ConstantInt::get(impl_->i64Type, 0);
        llvm::Value* endVal = nullptr;
        llvm::Value* stepVal = llvm::ConstantInt::get(impl_->i64Type, 1);

        if (callExpr->args.size() == 1) {
            callExpr->args[0]->accept(*this);
            endVal = impl_->lastValue;
        } else if (callExpr->args.size() >= 2) {
            callExpr->args[0]->accept(*this);
            startVal = impl_->lastValue;
            callExpr->args[1]->accept(*this);
            endVal = impl_->lastValue;
            if (callExpr->args.size() >= 3) {
                callExpr->args[2]->accept(*this);
                stepVal = impl_->lastValue;
            }
        } else {
            endVal = llvm::ConstantInt::get(impl_->i64Type, 0);
        }

        std::string loopVarName = node.varNames[0];
        auto* loopVar = impl_->createEntryAlloca(func, loopVarName, impl_->i64Type);
        impl_->builder->CreateStore(startVal, loopVar);

        auto* condBB = llvm::BasicBlock::Create(*impl_->context, "dcompcond", func);
        auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, "dcompbody", func);
        auto* incBB = llvm::BasicBlock::Create(*impl_->context, "dcompinc", func);
        auto* endBB = llvm::BasicBlock::Create(*impl_->context, "dcompend", func);

        impl_->builder->CreateBr(condBB);

        impl_->builder->SetInsertPoint(condBB);
        llvm::Value* current = impl_->builder->CreateLoad(impl_->i64Type, loopVar, "i");
        llvm::Value* cond = impl_->builder->CreateICmpSLT(current, endVal, "cmp");
        impl_->builder->CreateCondBr(cond, bodyBB, endBB);

        impl_->builder->SetInsertPoint(bodyBB);
        impl_->pushScope();
        impl_->setVar(loopVarName, loopVar, Impl::VarKind::Int);

        emitExtraClauses(0);

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
    } else if (!node.varNames.empty()) {
        node.iterable->accept(*this);
        llvm::Value* collVal = impl_->lastValue;
        bool collFromDict = impl_->isBareDictIterable(node.iterable.get());
        bool ownedIterTemp = !collFromDict && node.iterable &&
            !Impl::isBorrowedHeapExpr(node.iterable.get()) && node.iterable->type &&
            (node.iterable->type->kind() == Type::Kind::List ||
             node.iterable->type->kind() == Type::Kind::Set ||
             node.iterable->type->kind() == Type::Kind::Tuple);
        if (collFromDict)
            collVal = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_keys"], {collVal}, "compdictkeys");
        llvm::Value* collLen = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_list_len"], {collVal}, "colllen");

        auto* idxAlloca = impl_->createEntryAlloca(func, "__dcompidx", impl_->i64Type);
        impl_->builder->CreateStore(llvm::ConstantInt::get(impl_->i64Type, 0), idxAlloca);

        auto* condBB = llvm::BasicBlock::Create(*impl_->context, "dcompcond", func);
        auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, "dcompbody", func);
        auto* incBB = llvm::BasicBlock::Create(*impl_->context, "dcompinc", func);
        auto* endBB = llvm::BasicBlock::Create(*impl_->context, "dcompend", func);

        impl_->builder->CreateBr(condBB);

        impl_->builder->SetInsertPoint(condBB);
        llvm::Value* curIdx = impl_->builder->CreateLoad(impl_->i64Type, idxAlloca, "idx");
        llvm::Value* cond = impl_->builder->CreateICmpSLT(curIdx, collLen, "cmp");
        impl_->builder->CreateCondBr(cond, bodyBB, endBB);

        impl_->builder->SetInsertPoint(bodyBB);
        impl_->pushScope();

        if (node.varNames.size() == 1) {
            Type::Kind elemKind = impl_->getIterableElementKind(node.iterable.get());
            Impl::VarKind loopKind = Impl::typeKindToVarKind(elemKind);
            auto* elemAlloca = impl_->bindListElemTyped(
                func, collVal, curIdx, node.varNames[0], loopKind);
            impl_->setVar(node.varNames[0], elemAlloca, loopKind);
            if (Impl::isHeapKind(loopKind))
                impl_->scopes.back().borrowed.insert(node.varNames[0]);
        } else {
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
        }

        emitExtraClauses(0);

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

    impl_->lastValue = impl_->builder->CreateLoad(impl_->i8PtrType, dictAlloca);
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

    auto* callExpr = dynamic_cast<CallExpr*>(node.iterable.get());
    auto* calleeName = callExpr ? dynamic_cast<NameExpr*>(callExpr->callee.get()) : nullptr;
    bool isRange = calleeName && calleeName->name == "range";

    auto emitInnermostBody = [&]() {
        node.element->accept(*this);
        llvm::Value* elemVal = impl_->lastValue;

        if (elemVal->getType() == impl_->i1Type) {
            elemVal = impl_->builder->CreateZExt(elemVal, impl_->i64Type);
        } else if (elemVal->getType() == impl_->f64Type) {
            elemVal = impl_->builder->CreateBitCast(elemVal, impl_->i64Type);
        } else if (elemVal->getType()->isPointerTy()) {
            elemVal = impl_->builder->CreatePtrToInt(elemVal, impl_->i64Type);
        }

        if (node.condition) {
            node.condition->accept(*this);
            llvm::Value* filterCond = impl_->lastValue;
            if (filterCond->getType() == impl_->i64Type) {
                filterCond = impl_->builder->CreateICmpNE(
                    filterCond, llvm::ConstantInt::get(impl_->i64Type, 0));
            } else if (filterCond->getType() == impl_->f64Type) {
                filterCond = impl_->builder->CreateFCmpONE(
                    filterCond, llvm::ConstantFP::get(impl_->f64Type, 0.0));
            }
            auto* addBB = llvm::BasicBlock::Create(*impl_->context, "scompadd", func);
            auto* skipBB = llvm::BasicBlock::Create(*impl_->context, "scompskip", func);
            impl_->builder->CreateCondBr(filterCond, addBB, skipBB);

            impl_->builder->SetInsertPoint(addBB);
            llvm::Value* curSet = impl_->builder->CreateLoad(impl_->i8PtrType, setAlloca);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_set_add"], {curSet, elemVal});
            impl_->builder->CreateBr(skipBB);

            impl_->builder->SetInsertPoint(skipBB);
        } else {
            llvm::Value* curSet = impl_->builder->CreateLoad(impl_->i8PtrType, setAlloca);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_set_add"], {curSet, elemVal});
        }
    };

    std::function<void(size_t)> emitExtraClauses = [&](size_t clauseIdx) {
        if (clauseIdx >= node.extraClauses.size()) {
            emitInnermostBody();
            return;
        }
        auto& clause = node.extraClauses[clauseIdx];
        std::string ecVarName = clause.varNames.empty() ? "__ec" : clause.varNames[0];

        auto* ecCallExpr = dynamic_cast<CallExpr*>(clause.iterable.get());
        auto* ecCalleeName = ecCallExpr ? dynamic_cast<NameExpr*>(ecCallExpr->callee.get()) : nullptr;
        bool ecIsRange = ecCalleeName && ecCalleeName->name == "range";

        if (ecIsRange) {
            llvm::Value* ecStart = llvm::ConstantInt::get(impl_->i64Type, 0);
            llvm::Value* ecEnd = nullptr;
            llvm::Value* ecStep = llvm::ConstantInt::get(impl_->i64Type, 1);
            if (ecCallExpr->args.size() == 1) {
                ecCallExpr->args[0]->accept(*this);
                ecEnd = impl_->lastValue;
            } else if (ecCallExpr->args.size() >= 2) {
                ecCallExpr->args[0]->accept(*this);
                ecStart = impl_->lastValue;
                ecCallExpr->args[1]->accept(*this);
                ecEnd = impl_->lastValue;
                if (ecCallExpr->args.size() >= 3) {
                    ecCallExpr->args[2]->accept(*this);
                    ecStep = impl_->lastValue;
                }
            } else {
                ecEnd = llvm::ConstantInt::get(impl_->i64Type, 0);
            }
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
            if (clause.condition) {
                clause.condition->accept(*this);
                llvm::Value* ecFilter = impl_->lastValue;
                if (ecFilter->getType() == impl_->i64Type)
                    ecFilter = impl_->builder->CreateICmpNE(ecFilter, llvm::ConstantInt::get(impl_->i64Type, 0));
                else if (ecFilter->getType() == impl_->f64Type)
                    ecFilter = impl_->builder->CreateFCmpONE(ecFilter, llvm::ConstantFP::get(impl_->f64Type, 0.0));
                auto* ecPassBB = llvm::BasicBlock::Create(*impl_->context, "ecpass", func);
                auto* ecSkipBB = llvm::BasicBlock::Create(*impl_->context, "ecskip", func);
                impl_->builder->CreateCondBr(ecFilter, ecPassBB, ecSkipBB);
                impl_->builder->SetInsertPoint(ecPassBB);
                emitExtraClauses(clauseIdx + 1);
                if (!impl_->builder->GetInsertBlock()->getTerminator())
                    impl_->builder->CreateBr(ecSkipBB);
                impl_->builder->SetInsertPoint(ecSkipBB);
            } else {
                emitExtraClauses(clauseIdx + 1);
            }
            impl_->emitScopeCleanup();
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
            Type::Kind ecElemKind = impl_->getIterableElementKind(clause.iterable.get());
            Impl::VarKind ecLoopKind = Impl::typeKindToVarKind(ecElemKind);
            auto* ecVar = impl_->bindListElemTyped(
                func, ecColl, ecCurIdx, ecVarName, ecLoopKind);
            impl_->setVar(ecVarName, ecVar, ecLoopKind);
            if (Impl::isHeapKind(ecLoopKind))
                impl_->scopes.back().borrowed.insert(ecVarName);
            if (clause.condition) {
                clause.condition->accept(*this);
                llvm::Value* ecFilter = impl_->lastValue;
                if (ecFilter->getType() == impl_->i64Type)
                    ecFilter = impl_->builder->CreateICmpNE(ecFilter, llvm::ConstantInt::get(impl_->i64Type, 0));
                else if (ecFilter->getType() == impl_->f64Type)
                    ecFilter = impl_->builder->CreateFCmpONE(ecFilter, llvm::ConstantFP::get(impl_->f64Type, 0.0));
                auto* ecPassBB = llvm::BasicBlock::Create(*impl_->context, "ecpass", func);
                auto* ecSkipBB = llvm::BasicBlock::Create(*impl_->context, "ecskip", func);
                impl_->builder->CreateCondBr(ecFilter, ecPassBB, ecSkipBB);
                impl_->builder->SetInsertPoint(ecPassBB);
                emitExtraClauses(clauseIdx + 1);
                if (!impl_->builder->GetInsertBlock()->getTerminator())
                    impl_->builder->CreateBr(ecSkipBB);
                impl_->builder->SetInsertPoint(ecSkipBB);
            } else {
                emitExtraClauses(clauseIdx + 1);
            }
            impl_->emitScopeCleanup();
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
            if (ecFromDict)
                impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {ecColl});
        }
    };

    if (isRange) {
        llvm::Value* startVal = llvm::ConstantInt::get(impl_->i64Type, 0);
        llvm::Value* endVal = nullptr;
        llvm::Value* stepVal = llvm::ConstantInt::get(impl_->i64Type, 1);

        if (callExpr->args.size() == 1) {
            callExpr->args[0]->accept(*this);
            endVal = impl_->lastValue;
        } else if (callExpr->args.size() >= 2) {
            callExpr->args[0]->accept(*this);
            startVal = impl_->lastValue;
            callExpr->args[1]->accept(*this);
            endVal = impl_->lastValue;
            if (callExpr->args.size() >= 3) {
                callExpr->args[2]->accept(*this);
                stepVal = impl_->lastValue;
            }
        } else {
            endVal = llvm::ConstantInt::get(impl_->i64Type, 0);
        }

        auto* loopVar = impl_->createEntryAlloca(func, node.varName, impl_->i64Type);
        impl_->builder->CreateStore(startVal, loopVar);

        auto* condBB = llvm::BasicBlock::Create(*impl_->context, "scompcond", func);
        auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, "scompbody", func);
        auto* incBB = llvm::BasicBlock::Create(*impl_->context, "scompinc", func);
        auto* endBB = llvm::BasicBlock::Create(*impl_->context, "scompend", func);

        impl_->builder->CreateBr(condBB);

        impl_->builder->SetInsertPoint(condBB);
        llvm::Value* current = impl_->builder->CreateLoad(impl_->i64Type, loopVar, "i");
        llvm::Value* cond = impl_->builder->CreateICmpSLT(current, endVal, "cmp");
        impl_->builder->CreateCondBr(cond, bodyBB, endBB);

        impl_->builder->SetInsertPoint(bodyBB);
        impl_->pushScope();
        impl_->setVar(node.varName, loopVar, Impl::VarKind::Int);

        emitExtraClauses(0);

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
        node.iterable->accept(*this);
        llvm::Value* collVal = impl_->lastValue;
        bool collFromDict = impl_->isBareDictIterable(node.iterable.get());
        bool ownedIterTemp = !collFromDict && node.iterable &&
            !Impl::isBorrowedHeapExpr(node.iterable.get()) && node.iterable->type &&
            (node.iterable->type->kind() == Type::Kind::List ||
             node.iterable->type->kind() == Type::Kind::Set ||
             node.iterable->type->kind() == Type::Kind::Tuple);
        if (collFromDict)
            collVal = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_keys"], {collVal}, "compdictkeys");
        llvm::Value* collLen = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_list_len"], {collVal}, "colllen");

        auto* idxAlloca = impl_->createEntryAlloca(func, "__scompidx", impl_->i64Type);
        impl_->builder->CreateStore(llvm::ConstantInt::get(impl_->i64Type, 0), idxAlloca);

        auto* condBB = llvm::BasicBlock::Create(*impl_->context, "scompcond", func);
        auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, "scompbody", func);
        auto* incBB = llvm::BasicBlock::Create(*impl_->context, "scompinc", func);
        auto* endBB = llvm::BasicBlock::Create(*impl_->context, "scompend", func);

        impl_->builder->CreateBr(condBB);

        impl_->builder->SetInsertPoint(condBB);
        llvm::Value* curIdx = impl_->builder->CreateLoad(impl_->i64Type, idxAlloca, "idx");
        llvm::Value* cond = impl_->builder->CreateICmpSLT(curIdx, collLen, "cmp");
        impl_->builder->CreateCondBr(cond, bodyBB, endBB);

        impl_->builder->SetInsertPoint(bodyBB);
        impl_->pushScope();

        Type::Kind elemKind = impl_->getIterableElementKind(node.iterable.get());
        Impl::VarKind loopKind = Impl::typeKindToVarKind(elemKind);
        auto* elemAlloca = impl_->bindListElemTyped(
            func, collVal, curIdx, node.varName, loopKind);
        impl_->setVar(node.varName, elemAlloca, loopKind);
        if (Impl::isHeapKind(loopKind))
            impl_->scopes.back().borrowed.insert(node.varName);

        emitExtraClauses(0);

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

    impl_->lastValue = impl_->builder->CreateLoad(impl_->i8PtrType, setAlloca);
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

    auto* callExpr = dynamic_cast<CallExpr*>(node.iterable.get());
    auto* calleeName = callExpr ? dynamic_cast<NameExpr*>(callExpr->callee.get()) : nullptr;
    bool isRange = calleeName && calleeName->name == "range";

    auto emitInnermostBody = [&]() {
        node.element->accept(*this);
        llvm::Value* elemVal = impl_->lastValue;

        if (elemTag == TAG_STR && elemVal->getType()->isPointerTy()) {
            elemVal = impl_->ensureHeapString(elemVal, node.element.get());
        }

        if (elemVal->getType() == impl_->i1Type) {
            elemVal = impl_->builder->CreateZExt(elemVal, impl_->i64Type);
        } else if (elemVal->getType() == impl_->f64Type) {
            elemVal = impl_->builder->CreateBitCast(elemVal, impl_->i64Type);
        } else if (elemVal->getType()->isPointerTy()) {
            elemVal = impl_->builder->CreatePtrToInt(elemVal, impl_->i64Type);
        }

        if (node.condition) {
            node.condition->accept(*this);
            llvm::Value* filterCond = impl_->lastValue;
            if (filterCond->getType() == impl_->i64Type) {
                filterCond = impl_->builder->CreateICmpNE(
                    filterCond, llvm::ConstantInt::get(impl_->i64Type, 0));
            } else if (filterCond->getType() == impl_->f64Type) {
                filterCond = impl_->builder->CreateFCmpONE(
                    filterCond, llvm::ConstantFP::get(impl_->f64Type, 0.0));
            }
            auto* appendBB = llvm::BasicBlock::Create(*impl_->context, "genappend", func);
            auto* skipBB = llvm::BasicBlock::Create(*impl_->context, "genskip", func);
            impl_->builder->CreateCondBr(filterCond, appendBB, skipBB);

            impl_->builder->SetInsertPoint(appendBB);
            llvm::Value* curList = impl_->builder->CreateLoad(impl_->i8PtrType, listAlloca);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_append"], {curList, elemVal});
            impl_->builder->CreateBr(skipBB);

            impl_->builder->SetInsertPoint(skipBB);
        } else {
            llvm::Value* curList = impl_->builder->CreateLoad(impl_->i8PtrType, listAlloca);
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_append"], {curList, elemVal});
        }
    };

    std::function<void(size_t)> emitExtraClauses = [&](size_t clauseIdx) {
        if (clauseIdx >= node.extraClauses.size()) {
            emitInnermostBody();
            return;
        }
        auto& clause = node.extraClauses[clauseIdx];
        std::string ecVarName = clause.varNames.empty() ? "__ec" : clause.varNames[0];

        auto* ecCallExpr = dynamic_cast<CallExpr*>(clause.iterable.get());
        auto* ecCalleeName = ecCallExpr ? dynamic_cast<NameExpr*>(ecCallExpr->callee.get()) : nullptr;
        bool ecIsRange = ecCalleeName && ecCalleeName->name == "range";

        if (ecIsRange) {
            llvm::Value* ecStart = llvm::ConstantInt::get(impl_->i64Type, 0);
            llvm::Value* ecEnd = nullptr;
            llvm::Value* ecStep = llvm::ConstantInt::get(impl_->i64Type, 1);
            if (ecCallExpr->args.size() == 1) {
                ecCallExpr->args[0]->accept(*this);
                ecEnd = impl_->lastValue;
            } else if (ecCallExpr->args.size() >= 2) {
                ecCallExpr->args[0]->accept(*this);
                ecStart = impl_->lastValue;
                ecCallExpr->args[1]->accept(*this);
                ecEnd = impl_->lastValue;
                if (ecCallExpr->args.size() >= 3) {
                    ecCallExpr->args[2]->accept(*this);
                    ecStep = impl_->lastValue;
                }
            } else {
                ecEnd = llvm::ConstantInt::get(impl_->i64Type, 0);
            }
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
            if (clause.condition) {
                clause.condition->accept(*this);
                llvm::Value* ecFilter = impl_->lastValue;
                if (ecFilter->getType() == impl_->i64Type)
                    ecFilter = impl_->builder->CreateICmpNE(ecFilter, llvm::ConstantInt::get(impl_->i64Type, 0));
                else if (ecFilter->getType() == impl_->f64Type)
                    ecFilter = impl_->builder->CreateFCmpONE(ecFilter, llvm::ConstantFP::get(impl_->f64Type, 0.0));
                auto* ecPassBB = llvm::BasicBlock::Create(*impl_->context, "ecpass", func);
                auto* ecSkipBB = llvm::BasicBlock::Create(*impl_->context, "ecskip", func);
                impl_->builder->CreateCondBr(ecFilter, ecPassBB, ecSkipBB);
                impl_->builder->SetInsertPoint(ecPassBB);
                emitExtraClauses(clauseIdx + 1);
                if (!impl_->builder->GetInsertBlock()->getTerminator())
                    impl_->builder->CreateBr(ecSkipBB);
                impl_->builder->SetInsertPoint(ecSkipBB);
            } else {
                emitExtraClauses(clauseIdx + 1);
            }
            impl_->emitScopeCleanup();
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
            Type::Kind ecElemKind = impl_->getIterableElementKind(clause.iterable.get());
            Impl::VarKind ecLoopKind = Impl::typeKindToVarKind(ecElemKind);
            auto* ecVar = impl_->bindListElemTyped(
                func, ecColl, ecCurIdx, ecVarName, ecLoopKind);
            impl_->setVar(ecVarName, ecVar, ecLoopKind);
            if (Impl::isHeapKind(ecLoopKind))
                impl_->scopes.back().borrowed.insert(ecVarName);
            if (clause.condition) {
                clause.condition->accept(*this);
                llvm::Value* ecFilter = impl_->lastValue;
                if (ecFilter->getType() == impl_->i64Type)
                    ecFilter = impl_->builder->CreateICmpNE(ecFilter, llvm::ConstantInt::get(impl_->i64Type, 0));
                else if (ecFilter->getType() == impl_->f64Type)
                    ecFilter = impl_->builder->CreateFCmpONE(ecFilter, llvm::ConstantFP::get(impl_->f64Type, 0.0));
                auto* ecPassBB = llvm::BasicBlock::Create(*impl_->context, "ecpass", func);
                auto* ecSkipBB = llvm::BasicBlock::Create(*impl_->context, "ecskip", func);
                impl_->builder->CreateCondBr(ecFilter, ecPassBB, ecSkipBB);
                impl_->builder->SetInsertPoint(ecPassBB);
                emitExtraClauses(clauseIdx + 1);
                if (!impl_->builder->GetInsertBlock()->getTerminator())
                    impl_->builder->CreateBr(ecSkipBB);
                impl_->builder->SetInsertPoint(ecSkipBB);
            } else {
                emitExtraClauses(clauseIdx + 1);
            }
            impl_->emitScopeCleanup();
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
            if (ecFromDict)
                impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {ecColl});
        }
    };

    if (isRange) {
        llvm::Value* startVal = llvm::ConstantInt::get(impl_->i64Type, 0);
        llvm::Value* endVal = nullptr;
        llvm::Value* stepVal = llvm::ConstantInt::get(impl_->i64Type, 1);

        if (callExpr->args.size() == 1) {
            callExpr->args[0]->accept(*this);
            endVal = impl_->lastValue;
        } else if (callExpr->args.size() >= 2) {
            callExpr->args[0]->accept(*this);
            startVal = impl_->lastValue;
            callExpr->args[1]->accept(*this);
            endVal = impl_->lastValue;
            if (callExpr->args.size() >= 3) {
                callExpr->args[2]->accept(*this);
                stepVal = impl_->lastValue;
            }
        } else {
            endVal = llvm::ConstantInt::get(impl_->i64Type, 0);
        }

        auto* loopVar = impl_->createEntryAlloca(func, node.varName, impl_->i64Type);
        impl_->builder->CreateStore(startVal, loopVar);

        auto* condBB = llvm::BasicBlock::Create(*impl_->context, "gencond", func);
        auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, "genbody", func);
        auto* incBB = llvm::BasicBlock::Create(*impl_->context, "geninc", func);
        auto* endBB = llvm::BasicBlock::Create(*impl_->context, "genend", func);

        impl_->builder->CreateBr(condBB);

        impl_->builder->SetInsertPoint(condBB);
        llvm::Value* current = impl_->builder->CreateLoad(impl_->i64Type, loopVar, "i");
        llvm::Value* cond = impl_->builder->CreateICmpSLT(current, endVal, "cmp");
        impl_->builder->CreateCondBr(cond, bodyBB, endBB);

        impl_->builder->SetInsertPoint(bodyBB);
        impl_->pushScope();
        impl_->setVar(node.varName, loopVar, Impl::VarKind::Int);

        emitExtraClauses(0);

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
        node.iterable->accept(*this);
        llvm::Value* collVal = impl_->lastValue;
        bool collFromDict = impl_->isBareDictIterable(node.iterable.get());
        bool ownedIterTemp = !collFromDict && node.iterable &&
            !Impl::isBorrowedHeapExpr(node.iterable.get()) && node.iterable->type &&
            (node.iterable->type->kind() == Type::Kind::List ||
             node.iterable->type->kind() == Type::Kind::Set ||
             node.iterable->type->kind() == Type::Kind::Tuple);
        if (collFromDict)
            collVal = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_keys"], {collVal}, "compdictkeys");
        llvm::Value* collLen = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_list_len"], {collVal}, "colllen");

        auto* idxAlloca = impl_->createEntryAlloca(func, "__genidx", impl_->i64Type);
        impl_->builder->CreateStore(llvm::ConstantInt::get(impl_->i64Type, 0), idxAlloca);

        auto* condBB = llvm::BasicBlock::Create(*impl_->context, "gencond", func);
        auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, "genbody", func);
        auto* incBB = llvm::BasicBlock::Create(*impl_->context, "geninc", func);
        auto* endBB = llvm::BasicBlock::Create(*impl_->context, "genend", func);

        impl_->builder->CreateBr(condBB);

        impl_->builder->SetInsertPoint(condBB);
        llvm::Value* curIdx = impl_->builder->CreateLoad(impl_->i64Type, idxAlloca, "idx");
        llvm::Value* cond = impl_->builder->CreateICmpSLT(curIdx, collLen, "cmp");
        impl_->builder->CreateCondBr(cond, bodyBB, endBB);

        impl_->builder->SetInsertPoint(bodyBB);
        impl_->pushScope();

        Type::Kind elemKind = impl_->getIterableElementKind(node.iterable.get());
        Impl::VarKind loopKind = Impl::typeKindToVarKind(elemKind);
        auto* elemAlloca = impl_->bindListElemTyped(
            func, collVal, curIdx, node.varName, loopKind);
        impl_->setVar(node.varName, elemAlloca, loopKind);
        if (Impl::isHeapKind(loopKind))
            impl_->scopes.back().borrowed.insert(node.varName);

        emitExtraClauses(0);

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

    impl_->lastValue = impl_->builder->CreateLoad(impl_->i8PtrType, listAlloca);
}
}
