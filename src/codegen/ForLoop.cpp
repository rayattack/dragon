#include "../CodeGenImpl.h"

namespace dragon {

void CodeGen::visit(ForStmt& node) {
    auto* func = impl_->currentFunction;
    auto* targetName = dynamic_cast<NameExpr*>(node.target.get());
    auto* tupleTarget = dynamic_cast<TupleExpr*>(node.target.get());
    if (!targetName && !tupleTarget) return;

    if (auto* enumName = dynamic_cast<NameExpr*>(node.iterable.get())) {
        if (impl_->enumKindBySym.count(impl_->classSym(enumName->name))) {
            auto attr = std::make_unique<AttributeExpr>();
            auto obj = std::make_unique<NameExpr>();
            obj->name = enumName->name;
            obj->setLocation(node.iterable->location());
            attr->object = std::move(obj);
            attr->attribute = "__members__";
            attr->setLocation(node.iterable->location());
            attr->type = std::make_shared<ListType>(
                std::make_shared<InstanceType>(std::make_shared<ClassType>(enumName->name)));
            node.iterable = std::move(attr);
        }
    }

    auto* callExpr = dynamic_cast<CallExpr*>(node.iterable.get());
    auto* calleeName = callExpr ? dynamic_cast<NameExpr*>(callExpr->callee.get()) : nullptr;
    bool isRange = calleeName && calleeName->name == "range";

    if (isRange && targetName) {
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

        auto* loopVar = impl_->createEntryAlloca(func, targetName->name, impl_->i64Type);
        impl_->setVar(targetName->name, loopVar);
        impl_->builder->CreateStore(startVal, loopVar);

        auto* condBB = llvm::BasicBlock::Create(*impl_->context, "forcond", func);
        auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, "forbody", func);
        auto* incBB = llvm::BasicBlock::Create(*impl_->context, "forinc", func);
        auto* endBB = llvm::BasicBlock::Create(*impl_->context, "forend", func);
        llvm::BasicBlock* elseBB = node.elseBody.empty()
            ? endBB
            : llvm::BasicBlock::Create(*impl_->context, "forelse", func);

        impl_->loopStack.push({endBB, incBB, impl_->scopes.size(), impl_->tryFrameFuncs.size(), impl_->exitCleanupStack.size()});
        impl_->builder->CreateBr(condBB);

        impl_->builder->SetInsertPoint(condBB);
        llvm::Value* current = impl_->builder->CreateLoad(impl_->i64Type, loopVar, "i");
        llvm::Value* cond = impl_->builder->CreateICmpSLT(current, endVal, "cmp");
        impl_->builder->CreateCondBr(cond, bodyBB, elseBB);

        impl_->builder->SetInsertPoint(bodyBB);
        impl_->pushScope();
        for (auto& stmt : node.body) stmt->accept(*this);
        impl_->emitScopeCleanup();
        impl_->popScope();
        if (!impl_->builder->GetInsertBlock()->getTerminator())
            impl_->builder->CreateBr(incBB);

        impl_->builder->SetInsertPoint(incBB);
        current = impl_->builder->CreateLoad(impl_->i64Type, loopVar, "i");
        llvm::Value* next = impl_->builder->CreateAdd(current, stepVal, "inc");
        impl_->builder->CreateStore(next, loopVar);
        impl_->builder->CreateBr(condBB);

        impl_->loopStack.pop();
        if (elseBB != endBB) {
            impl_->builder->SetInsertPoint(elseBB);
            impl_->pushScope();
            for (auto& stmt : node.elseBody) stmt->accept(*this);
            impl_->emitScopeCleanup();
            impl_->popScope();
            if (!impl_->builder->GetInsertBlock()->getTerminator())
                impl_->builder->CreateBr(endBB);
        }
        impl_->builder->SetInsertPoint(endBB);
        return;
    }

    {
        bool isGenerator = false;
        Impl::VarKind yieldKind = Impl::VarKind::Int;
        if (auto* callExpr = dynamic_cast<CallExpr*>(node.iterable.get())) {
            if (auto* callee = dynamic_cast<NameExpr*>(callExpr->callee.get())) {
                std::string sym = impl_->resolveCalleeSymbol(callee->name);
                if (impl_->generatorFunctions.count(sym)) {
                    isGenerator = true;
                    auto it = impl_->generatorYieldKinds.find(sym);
                    if (it != impl_->generatorYieldKinds.end()) yieldKind = it->second;
                }
            } else if (auto* attr = dynamic_cast<AttributeExpr*>(callExpr->callee.get())) {
                std::string cn = impl_->resolveExprClassName(attr->object.get());
                if (cn.empty()) {
                    if (auto* objName = dynamic_cast<NameExpr*>(attr->object.get()))
                        if (impl_->classNames.count(objName->name)) cn = objName->name;
                }
                if (!cn.empty()) {
                    std::string sym;
                    impl_->resolveMethodFunction(
                        impl_->resolveClassOwningModule(cn), cn, attr->attribute, &sym);
                    if (!sym.empty() && impl_->generatorFunctions.count(sym)) {
                        isGenerator = true;
                        auto it = impl_->generatorYieldKinds.find(sym);
                        if (it != impl_->generatorYieldKinds.end()) yieldKind = it->second;
                    }
                }
            }
        }
        if (!isGenerator) {
            if (auto* nameExpr = dynamic_cast<NameExpr*>(node.iterable.get())) {
                if (impl_->lookupVarKind(nameExpr->name) == Impl::VarKind::Generator) {
                    isGenerator = true;
                    auto kit = impl_->varGenYieldKinds.find(nameExpr->name);
                    if (kit != impl_->varGenYieldKinds.end()) yieldKind = kit->second;
                }
            }
        }
        auto* targetName = dynamic_cast<NameExpr*>(node.target.get());
        if (isGenerator && targetName) {
            auto* func = impl_->currentFunction;

            node.iterable->accept(*this);
            llvm::Value* genObj = impl_->lastValue;
            if (!genObj->getType()->isPointerTy())
                genObj = impl_->builder->CreateIntToPtr(genObj, impl_->i8PtrType);

            bool yieldIsHeap = Impl::isHeapKind(yieldKind);
            auto* loopVar = impl_->createEntryAlloca(func, targetName->name,
                yieldIsHeap ? impl_->i8PtrType : impl_->i64Type);
            impl_->setVar(targetName->name, loopVar, yieldKind);
            if (yieldIsHeap) impl_->scopes.back().borrowed.insert(targetName->name);

            auto* genAlloca = impl_->createEntryAlloca(func, "__gen_iter", impl_->i8PtrType);
            impl_->builder->CreateStore(genObj, genAlloca);
            llvm::Value* genCleanupBase =
                impl_->emitCleanupPushTemp(genObj, Impl::DCLEAN_OBJ);

            auto* condBB = llvm::BasicBlock::Create(*impl_->context, "gen.cond", func);
            auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, "gen.body", func);
            auto* endBB = llvm::BasicBlock::Create(*impl_->context, "gen.end", func);
            llvm::BasicBlock* elseBB = node.elseBody.empty()
                ? endBB
                : llvm::BasicBlock::Create(*impl_->context, "gen.else", func);

            impl_->loopStack.push({endBB, condBB, impl_->scopes.size(), impl_->tryFrameFuncs.size(), impl_->exitCleanupStack.size()});
            impl_->builder->CreateBr(condBB);

            impl_->builder->SetInsertPoint(condBB);
            auto* genPtr = impl_->builder->CreateLoad(impl_->i8PtrType, genAlloca, "gen.ptr");

            auto* jmpbufPtr = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_exc_push_frame"], {}, "jmpbuf");
            auto* setjmpResult = impl_->builder->CreateCall(
                impl_->runtimeFuncs["setjmp"], {jmpbufPtr}, "setjmp.result");
            auto* isNormal = impl_->builder->CreateICmpEQ(
                setjmpResult,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*impl_->context), 0),
                "is.normal");

            auto* nextBB = llvm::BasicBlock::Create(*impl_->context, "gen.next", func);
            auto* excBB = llvm::BasicBlock::Create(*impl_->context, "gen.exc", func);
            impl_->builder->CreateCondBr(isNormal, nextBB, excBB);

            impl_->builder->SetInsertPoint(nextBB);
            auto* nextVal = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_generator_next"], {genPtr}, "gen.val");
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_pop_frame"], {});
            if (yieldIsHeap) {
                llvm::Value* asPtr = impl_->builder->CreateIntToPtr(
                    nextVal, impl_->i8PtrType, "gen.val.ptr");
                impl_->builder->CreateStore(asPtr, loopVar);
            } else {
                impl_->builder->CreateStore(nextVal, loopVar);
            }
            impl_->builder->CreateBr(bodyBB);

            impl_->builder->SetInsertPoint(excBB);
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_cleanup_unwind"], {});
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_pop_frame"], {});
            auto* excType = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_exc_get_type"], {}, "exc.type");
            auto* isStopIter = impl_->builder->CreateICmpEQ(
                excType,
                llvm::ConstantInt::get(impl_->i64Type, 11),
                "is.stopiter");
            auto* reraiseBB = llvm::BasicBlock::Create(*impl_->context, "gen.reraise", func);
            impl_->builder->CreateCondBr(isStopIter, elseBB, reraiseBB);

            // Re-raise: no emitAllScopeCleanup() here - it double-decref'd a still-live
            // local (e.g. generator's `self`, held by the coroutine) -> use-after-free.
            impl_->builder->SetInsertPoint(reraiseBB);
            {
                auto* reType = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_exc_get_type"], {}, "reraise.type");
                auto* reMsg = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_exc_get_msg"], {}, "reraise.msg");
                auto* reObj = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_exc_retain_obj"],
                    {impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_exc_get_obj"], {},
                        "reraise.obj.raw")},
                    "reraise.obj");
                {
                    auto* gAb = impl_->builder->CreateLoad(
                        impl_->i8PtrType, genAlloca, "gen.abandon");
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_generator_abandon"], {gAb});
                }
                // This re-raise bypasses endBB's decref, so decref the generator here
                // (else it leaks); mirror endBB exactly (decref then pop cleanup-stack) to avoid a double-free.
                if (impl_->options.gcMode == GCMode::RC) {
                    auto* g = impl_->builder->CreateLoad(
                        impl_->i8PtrType, genAlloca, "gen.reraise.cleanup");
                    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {g});
                    impl_->emitCleanupPopTemp(genCleanupBase);
                }
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_raise_exc_obj"], {reType, reObj, reMsg});
            }
            impl_->builder->CreateUnreachable();

            impl_->builder->SetInsertPoint(bodyBB);
            impl_->pushScope();
            for (auto& stmt : node.body) stmt->accept(*this);
            impl_->emitScopeCleanup();
            impl_->popScope();
            if (!impl_->builder->GetInsertBlock()->getTerminator())
                impl_->builder->CreateBr(condBB);

            impl_->loopStack.pop();

            if (elseBB != endBB) {
                impl_->builder->SetInsertPoint(elseBB);
                impl_->pushScope();
                for (auto& stmt : node.elseBody) stmt->accept(*this);
                impl_->emitScopeCleanup();
                impl_->popScope();
                if (!impl_->builder->GetInsertBlock()->getTerminator())
                    impl_->builder->CreateBr(endBB);
            }

            impl_->builder->SetInsertPoint(endBB);

            if (impl_->options.gcMode == GCMode::RC) {
                auto* finalGen = impl_->builder->CreateLoad(impl_->i8PtrType, genAlloca, "gen.cleanup");
                impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {finalGen});
                impl_->emitCleanupPopTemp(genCleanupBase);
            }
            return;
        }
    }

    {
        std::string iterClassName = impl_->resolveExprClassName(node.iterable.get());
        std::string iterDisplayName = iterClassName;
        if (node.iterable->type) {
            if (auto* inst =
                    dynamic_cast<InstanceType*>(node.iterable->type.get())) {
                if (inst->classType) {
                    iterDisplayName = inst->classType->name;
                    iterClassName = Impl::mangleClass(
                        inst->classType->definingModule, inst->classType->name);
                }
            }
        }
        auto* targetName = dynamic_cast<NameExpr*>(node.target.get());
        if (!iterClassName.empty() && targetName &&
            impl_->hasDunder(iterClassName, "__iter__")) {
            auto* func = impl_->currentFunction;

            node.iterable->accept(*this);
            std::vector<std::pair<llvm::Value*, Impl::VarKind>> iterableTemps;
            std::vector<llvm::Value*> iterableTempBases;
            llvm::Value* iterable = impl_->trackBorrowTempGuarded(
                node.iterable.get(), impl_->lastValue, iterableTemps,
                iterableTempBases);
            auto* iterator = impl_->callDunder(iterClassName, "__iter__", iterable);
            impl_->popArgTempCleanups(iterableTempBases);
            impl_->drainBorrowTemps(iterableTemps);

            llvm::Type* loopVarType = impl_->i8PtrType;
            Impl::VarKind loopVarKind = Impl::VarKind::Other;
            std::string loopVarClassName;
            std::string loopVarClassModule;
            std::string nextOwner = iterClassName;
            std::string nextOwnerDisplay = iterDisplayName;
            {
                std::string iterDef =
                    impl_->findDunderClass(iterClassName, "__iter__");
                if (!iterDef.empty()) {
                    auto rIt =
                        impl_->methodReturnClassNames.find(iterDef + "___iter__");
                    if (rIt != impl_->methodReturnClassNames.end()) {
                        auto fdmIt =
                            impl_->funcDefiningModule.find(iterDef + "___iter__");
                        nextOwnerDisplay = rIt->second;
                        nextOwner = Impl::mangleClass(
                            impl_->resolveClassOwningModuleFrom(
                                fdmIt != impl_->funcDefiningModule.end()
                                    ? fdmIt->second
                                    : impl_->currentModuleName,
                                rIt->second),
                            rIt->second);
                    }
                }
            }
            {
                std::string nextClass = impl_->findDunderClass(nextOwner, "__next__");
                if (nextClass.empty()) {
                    impl_->addError(
                        "cannot iterate: __iter__ returns '" + nextOwnerDisplay +
                        "', which has no __next__ method",
                        node.location());
                    return;
                }
                std::string methKey = nextClass + "___next__";
                auto* nextFn = impl_->module->getFunction(methKey);
                if (nextFn) loopVarType = nextFn->getReturnType();
                auto rkIt = impl_->methodReturnKinds.find(methKey);
                if (rkIt != impl_->methodReturnKinds.end())
                    loopVarKind = Impl::typeKindToVarKind(rkIt->second);
                auto rcIt = impl_->methodReturnClassNames.find(methKey);
                if (rcIt != impl_->methodReturnClassNames.end()) {
                    loopVarClassName = rcIt->second;
                    auto fdmIt = impl_->funcDefiningModule.find(methKey);
                    loopVarClassModule = impl_->resolveClassOwningModuleFrom(
                        fdmIt != impl_->funcDefiningModule.end()
                            ? fdmIt->second
                            : impl_->currentModuleName,
                        loopVarClassName);
                }
            }
            auto* loopVar = impl_->createEntryAlloca(func, targetName->name, loopVarType);
            impl_->emitNullSlot(loopVar);
            impl_->setVar(targetName->name, loopVar, loopVarKind);
            if (!loopVarClassName.empty()) {
                impl_->varClassNames[targetName->name] = loopVarClassName;
                impl_->varClassOwningModule[targetName->name] = loopVarClassModule;
            }
            // __next__ returns OWNED (+1); a prior borrowed-mark on the loop var (copied
            // from the generator-yield convention) leaked one element per iter of `for line in open(p)` (A/B-proven).

            std::string iterObjName = "__iter_obj." + std::to_string(impl_->forIterCounter++);
            auto* iterAlloca = impl_->createEntryAlloca(func, iterObjName, impl_->i8PtrType);
            impl_->builder->CreateStore(iterator, iterAlloca);
            impl_->setVar(iterObjName, iterAlloca, Impl::VarKind::ClassInstance);

            auto* condBB = llvm::BasicBlock::Create(*impl_->context, "iter.cond", func);
            auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, "iter.body", func);
            auto* endBB = llvm::BasicBlock::Create(*impl_->context, "iter.end", func);
            llvm::BasicBlock* elseBB = node.elseBody.empty()
                ? endBB
                : llvm::BasicBlock::Create(*impl_->context, "iter.else", func);

            impl_->loopStack.push({endBB, condBB, impl_->scopes.size(), impl_->tryFrameFuncs.size(), impl_->exitCleanupStack.size()});
            impl_->builder->CreateBr(condBB);

            impl_->builder->SetInsertPoint(condBB);
            auto* iterObj = impl_->builder->CreateLoad(impl_->i8PtrType, iterAlloca, "iter.obj");

            auto* jmpbufPtr = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_exc_push_frame"], {}, "jmpbuf");
            auto* setjmpResult = impl_->builder->CreateCall(
                impl_->runtimeFuncs["setjmp"], {jmpbufPtr}, "setjmp.result");
            auto* isNormal = impl_->builder->CreateICmpEQ(
                setjmpResult,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*impl_->context), 0),
                "is.normal");

            auto* nextBB = llvm::BasicBlock::Create(*impl_->context, "iter.next", func);
            auto* excBB = llvm::BasicBlock::Create(*impl_->context, "iter.exc", func);
            impl_->builder->CreateCondBr(isNormal, nextBB, excBB);

            impl_->builder->SetInsertPoint(nextBB);
            auto* nextVal = impl_->callDunder(nextOwner, "__next__", iterObj);
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_pop_frame"], {});
            // RC overwrite: the loop var owns each element (+1 method return); release
            // the previous one before storing next, else all but the last leaked (A/B-proven).
            if (impl_->options.gcMode == GCMode::RC &&
                Impl::isHeapKind(loopVarKind)) {
                auto* prevVal = impl_->builder->CreateLoad(
                    loopVarType, loopVar, "iter.prev");
                impl_->emitDecrefByKind(prevVal, loopVarKind);
            }
            impl_->builder->CreateStore(nextVal, loopVar);
            impl_->builder->CreateBr(bodyBB);

            impl_->builder->SetInsertPoint(excBB);
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_cleanup_unwind"], {});
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_pop_frame"], {});
            auto* excType = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_exc_get_type"], {}, "exc.type");
            auto* isStopIter = impl_->builder->CreateICmpEQ(
                excType,
                llvm::ConstantInt::get(impl_->i64Type, 11),
                "is.stopiter");
            auto* reraiseBB = llvm::BasicBlock::Create(*impl_->context, "iter.reraise", func);
            impl_->builder->CreateCondBr(isStopIter, elseBB, reraiseBB);

            impl_->builder->SetInsertPoint(reraiseBB);
            {
                auto* reType = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_exc_get_type"], {}, "reraise.type");
                auto* reMsg = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_exc_get_msg"], {}, "reraise.msg");
                auto* reObj = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_exc_retain_obj"],
                    {impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_exc_get_obj"], {},
                        "reraise.obj.raw")},
                    "reraise.obj");
                impl_->emitAllScopeCleanup();
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_raise_exc_obj"], {reType, reObj, reMsg});
            }
            impl_->builder->CreateUnreachable();

            impl_->builder->SetInsertPoint(bodyBB);
            impl_->pushScope();
            for (auto& stmt : node.body) stmt->accept(*this);
            impl_->emitScopeCleanup();
            impl_->popScope();
            if (!impl_->builder->GetInsertBlock()->getTerminator())
                impl_->builder->CreateBr(condBB);

            impl_->loopStack.pop();

            if (elseBB != endBB) {
                impl_->builder->SetInsertPoint(elseBB);
                impl_->pushScope();
                for (auto& stmt : node.elseBody) stmt->accept(*this);
                impl_->emitScopeCleanup();
                impl_->popScope();
                if (!impl_->builder->GetInsertBlock()->getTerminator())
                    impl_->builder->CreateBr(endBB);
            }

            impl_->builder->SetInsertPoint(endBB);
            return;
        }
    }

    {
        bool iterMayBeBox = false;
        if (node.iterable->type &&
            (node.iterable->type->kind() == Type::Kind::Any ||
             node.iterable->type->kind() == Type::Kind::Union))
            iterMayBeBox = true;
        if (auto* nm = dynamic_cast<NameExpr*>(node.iterable.get())) {
            iterMayBeBox = impl_->lookupVarKind(nm->name) == Impl::VarKind::Union;
        }
        bool anyElemList = false;
        if (node.iterable->type) {
            if (auto* lt = dynamic_cast<ListType*>(node.iterable->type.get())) {
                if (lt->elementType &&
                    lt->elementType->kind() == Type::Kind::Any)
                    anyElemList = true;
            }
        }
        if (auto* nm = dynamic_cast<NameExpr*>(node.iterable.get())) {
            auto it = impl_->varListElemKinds.find(nm->name);
            if (it != impl_->varListElemKinds.end() &&
                it->second == Type::Kind::Any)
                anyElemList = true;
            if (impl_->varListElemIsType.count(nm->name))
                anyElemList = false;
        } else if (auto* iterAttr =
                       dynamic_cast<AttributeExpr*>(node.iterable.get())) {
            std::string ownerClass;
            if (auto* objName = dynamic_cast<NameExpr*>(iterAttr->object.get())) {
                if (objName->name == "self" && !impl_->currentClassName.empty())
                    ownerClass = impl_->currentClassName;
                else {
                    auto vit = impl_->varClassNames.find(objName->name);
                    if (vit != impl_->varClassNames.end())
                        ownerClass = vit->second;
                }
            }
            if (!ownerClass.empty()) {
                auto cit = impl_->classFieldListElemKindsBySym.find(impl_->classSym(ownerClass));
                if (cit != impl_->classFieldListElemKindsBySym.end()) {
                    auto fit = cit->second.find(iterAttr->attribute);
                    if (fit != cit->second.end() &&
                        fit->second == Type::Kind::Any)
                        anyElemList = true;
                }
            }
        }
        if (anyElemList) iterMayBeBox = true;
        auto* boxTarget = dynamic_cast<NameExpr*>(node.target.get());
        if (iterMayBeBox && boxTarget) {
            node.iterable->accept(*this);
            llvm::Value* iterBox = impl_->lastValue;
            if (iterBox->getType() != impl_->boxType) {
                if (iterBox->getType()->isPointerTy())
                    iterBox = impl_->makeBox(
                        llvm::ConstantInt::get(impl_->i64Type, 5), iterBox);
                else
                    iterBox = impl_->makeBox(
                        llvm::ConstantInt::get(impl_->i64Type, 0), iterBox);
            }
            bool ownedIterable = impl_->options.gcMode == GCMode::RC &&
                                 impl_->isOwnedBoxResult(iterBox);

            auto* lenV = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_box_len"], {iterBox},
                "boxiter.len");
            auto* idxA = impl_->createEntryAlloca(
                func, "boxiter.i." + std::to_string(impl_->forIterCounter++),
                impl_->i64Type);
            impl_->builder->CreateStore(
                llvm::ConstantInt::get(impl_->i64Type, 0), idxA);
            auto* loopVar = impl_->createEntryAlloca(
                func, boxTarget->name, impl_->boxType);
            impl_->builder->CreateStore(
                llvm::Constant::getNullValue(impl_->boxType), loopVar);
            impl_->setVar(boxTarget->name, loopVar, Impl::VarKind::Union);

            auto* condBB = llvm::BasicBlock::Create(*impl_->context, "boxiter.cond", func);
            auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, "boxiter.body", func);
            auto* incrBB = llvm::BasicBlock::Create(*impl_->context, "boxiter.incr", func);
            auto* endBB  = llvm::BasicBlock::Create(*impl_->context, "boxiter.end", func);
            llvm::BasicBlock* elseBB = node.elseBody.empty()
                ? endBB
                : llvm::BasicBlock::Create(*impl_->context, "boxiter.else", func);

            impl_->loopStack.push({endBB, incrBB, impl_->scopes.size(),
                                   impl_->tryFrameFuncs.size(),
                                   impl_->exitCleanupStack.size()});
            impl_->builder->CreateBr(condBB);

            impl_->builder->SetInsertPoint(condBB);
            auto* iCur = impl_->builder->CreateLoad(impl_->i64Type, idxA, "boxiter.icur");
            auto* inRange = impl_->builder->CreateICmpSLT(iCur, lenV, "boxiter.cmp");
            impl_->builder->CreateCondBr(inRange, bodyBB, elseBB);

            impl_->builder->SetInsertPoint(bodyBB);
            auto* idxBox = impl_->makeBox(
                llvm::ConstantInt::get(impl_->i64Type, 0), iCur);
            auto* elemBox = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_box_subscript"],
                {iterBox, idxBox}, "boxiter.elem");
            if (impl_->options.gcMode == GCMode::RC) {
                auto* prev = impl_->builder->CreateLoad(
                    impl_->boxType, loopVar, "boxiter.prev");
                impl_->emitUnionDecref(
                    impl_->boxPayloadI64(prev, "boxiter.prev.pay"),
                    impl_->boxTag(prev, "boxiter.prev.tag"));
            }
            impl_->builder->CreateStore(elemBox, loopVar);
            impl_->pushScope();
            for (auto& stmt : node.body) stmt->accept(*this);
            impl_->emitScopeCleanup();
            impl_->popScope();
            if (!impl_->builder->GetInsertBlock()->getTerminator())
                impl_->builder->CreateBr(incrBB);

            impl_->builder->SetInsertPoint(incrBB);
            auto* iNext = impl_->builder->CreateAdd(
                impl_->builder->CreateLoad(impl_->i64Type, idxA, "boxiter.i2"),
                llvm::ConstantInt::get(impl_->i64Type, 1), "boxiter.inext");
            impl_->builder->CreateStore(iNext, idxA);
            impl_->builder->CreateBr(condBB);

            impl_->loopStack.pop();

            if (elseBB != endBB) {
                impl_->builder->SetInsertPoint(elseBB);
                impl_->pushScope();
                for (auto& stmt : node.elseBody) stmt->accept(*this);
                impl_->emitScopeCleanup();
                impl_->popScope();
                if (!impl_->builder->GetInsertBlock()->getTerminator())
                    impl_->builder->CreateBr(endBB);
            }

            impl_->builder->SetInsertPoint(endBB);
            // Release the LAST element (both break and natural exhaustion land here),
            // null the slot to prevent a double-free, and drop the iterable's own +1 if owned.
            if (impl_->options.gcMode == GCMode::RC) {
                auto* last = impl_->builder->CreateLoad(
                    impl_->boxType, loopVar, "boxiter.last");
                impl_->emitUnionDecref(
                    impl_->boxPayloadI64(last, "boxiter.last.pay"),
                    impl_->boxTag(last, "boxiter.last.tag"));
                impl_->builder->CreateStore(
                    llvm::Constant::getNullValue(impl_->boxType), loopVar);
                if (ownedIterable)
                    impl_->emitUnionDecref(
                        impl_->boxPayloadI64(iterBox, "boxiter.it.pay"),
                        impl_->boxTag(iterBox, "boxiter.it.tag"));
            }
            return;
        }
    }

    bool isStrIterable = false;
    bool isListIterable = false;
    bool isDictItemsIterable = false;
    bool isDictKeysIterable = false;

    auto resolveOwnerClass = [&](AttributeExpr* attr) -> std::string {
        if (auto* objName = dynamic_cast<NameExpr*>(attr->object.get())) {
            if (objName->name == "self" && !impl_->currentClassName.empty())
                return impl_->currentClassName;
            auto vit = impl_->varClassNames.find(objName->name);
            if (vit != impl_->varClassNames.end()) return vit->second;
            return {};
        }
        return impl_->resolveExprClassName(attr->object.get());
    };

    auto fieldVarKind = [&](const std::string& cls, const std::string& field) -> Impl::VarKind {
        auto cit = impl_->classFieldKindsBySym.find(impl_->classSym(cls));
        if (cit == impl_->classFieldKindsBySym.end()) return Impl::VarKind::Other;
        auto fit = cit->second.find(field);
        if (fit == cit->second.end()) return Impl::VarKind::Other;
        return fit->second;
    };

    if (auto* iterCall = dynamic_cast<CallExpr*>(node.iterable.get())) {
        if (auto* attr = dynamic_cast<AttributeExpr*>(iterCall->callee.get())) {
            bool objIsDict = false;
            if (auto* objName = dynamic_cast<NameExpr*>(attr->object.get())) {
                objIsDict = impl_->lookupVarKind(objName->name) == Impl::VarKind::Dict;
            } else if (auto* objAttr = dynamic_cast<AttributeExpr*>(attr->object.get())) {
                std::string owner = resolveOwnerClass(objAttr);
                if (!owner.empty())
                    objIsDict = fieldVarKind(owner, objAttr->attribute) == Impl::VarKind::Dict;
            }
            if (objIsDict) {
                if (attr->attribute == "items") isDictItemsIterable = true;
                else if (attr->attribute == "keys") isDictKeysIterable = true;
                else if (attr->attribute == "values") isListIterable = true;
                else isListIterable = true;
            }
        }
    }
    if (!isDictItemsIterable && !isDictKeysIterable && !isListIterable) {
        if (auto* iterName = dynamic_cast<NameExpr*>(node.iterable.get())) {
            auto kind = impl_->lookupVarKind(iterName->name);
            if (kind == Impl::VarKind::Str || kind == Impl::VarKind::StrLiteral) isStrIterable = true;
            else if (kind == Impl::VarKind::Dict) isDictKeysIterable = true;
            else if (kind == Impl::VarKind::List) isListIterable = true;
            else isListIterable = true;
        } else if (auto* iterAttr = dynamic_cast<AttributeExpr*>(node.iterable.get())) {
            std::string owner = resolveOwnerClass(iterAttr);
            Impl::VarKind kind = owner.empty()
                ? Impl::VarKind::Other
                : fieldVarKind(owner, iterAttr->attribute);
            if (kind == Impl::VarKind::Str || kind == Impl::VarKind::StrLiteral) isStrIterable = true;
            else if (kind == Impl::VarKind::Dict) isDictKeysIterable = true;
            else if (kind == Impl::VarKind::List) isListIterable = true;
            else if (node.iterable->type) {
                switch (node.iterable->type->kind()) {
                    case Type::Kind::Dict: isDictKeysIterable = true; break;
                    case Type::Kind::Str:  isStrIterable = true; break;
                    default:               isListIterable = true; break;
                }
            }
            else isListIterable = true;
        } else if (dynamic_cast<StringLiteral*>(node.iterable.get())) {
            isStrIterable = true;
        } else if (node.iterable->type) {
            switch (node.iterable->type->kind()) {
                case Type::Kind::Dict: isDictKeysIterable = true; break;
                case Type::Kind::Str:  isStrIterable = true; break;
                default:               isListIterable = true; break;
            }
        } else {
            isListIterable = true;
        }
    }

    bool dictKeysAreInt = false;
    bool dictKeysAreFloat = false;
    if (isDictKeysIterable || isDictItemsIterable) {
        Expr* dictExpr = node.iterable.get();
        if (auto* iterCall = dynamic_cast<CallExpr*>(dictExpr))
            if (auto* attr = dynamic_cast<AttributeExpr*>(iterCall->callee.get()))
                dictExpr = attr->object.get();
        Type::Kind kk = impl_->resolveDictKeyKind(dictExpr);
        dictKeysAreInt = kk == Type::Kind::Int || kk == Type::Kind::Float;
        dictKeysAreFloat = kk == Type::Kind::Float;
    }

    llvm::Value* iterableVal;
    if (isDictKeysIterable) {
        if (auto* iterCall = dynamic_cast<CallExpr*>(node.iterable.get())) {
            node.iterable->accept(*this);
            iterableVal = impl_->lastValue;
        } else {
            node.iterable->accept(*this);
            llvm::Value* dictVal = impl_->lastValue;
            iterableVal = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_keys"], {dictVal}, "dictkeys");
            Impl::VarKind rd =
                impl_->ownedTempDrainKind(node.iterable.get(), dictVal);
            if (rd != Impl::VarKind::Other)
                impl_->emitDecrefByKind(dictVal, rd);
        }
    } else if (isDictItemsIterable) {
        node.iterable->accept(*this);
        iterableVal = impl_->lastValue;
    } else {
        node.iterable->accept(*this);
        iterableVal = impl_->lastValue;
    }
    std::string iterName = "__iter." + std::to_string(impl_->forIterCounter++);
    auto* iterAlloca = impl_->createEntryAlloca(func, iterName, impl_->i8PtrType);
    impl_->builder->CreateStore(iterableVal, iterAlloca);
    bool ownedContainerIter =
        !isDictKeysIterable && !isDictItemsIterable &&
        node.iterable && !Impl::isBorrowedHeapExpr(node.iterable.get()) &&
        node.iterable->type &&
        (node.iterable->type->kind() == Type::Kind::List ||
         node.iterable->type->kind() == Type::Kind::Set ||
         node.iterable->type->kind() == Type::Kind::Tuple);
    if (isDictKeysIterable || isDictItemsIterable || ownedContainerIter) {
        impl_->setVar(iterName, iterAlloca, Impl::VarKind::List);
    }

    auto* idxVar = impl_->createEntryAlloca(func, "__i", impl_->i64Type);
    impl_->builder->CreateStore(llvm::ConstantInt::get(impl_->i64Type, 0), idxVar);

    auto* condBB = llvm::BasicBlock::Create(*impl_->context, "forcond", func);
    auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, "forbody", func);
    auto* incBB = llvm::BasicBlock::Create(*impl_->context, "forinc", func);
    auto* endBB = llvm::BasicBlock::Create(*impl_->context, "forend", func);
    llvm::BasicBlock* elseBB = node.elseBody.empty()
        ? endBB
        : llvm::BasicBlock::Create(*impl_->context, "forelse", func);

    impl_->loopStack.push({endBB, incBB, impl_->scopes.size(), impl_->tryFrameFuncs.size(), impl_->exitCleanupStack.size()});
    impl_->builder->CreateBr(condBB);

    impl_->builder->SetInsertPoint(condBB);
    llvm::Value* currentIdx = impl_->builder->CreateLoad(impl_->i64Type, idxVar, "__i");
    llvm::Value* iterLoaded = impl_->builder->CreateLoad(impl_->i8PtrType, iterAlloca, "__iter");
    llvm::Value* lenVal;
    if (isStrIterable) {
        lenVal = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_str_len"], {iterLoaded}, "len");
    } else {
        lenVal = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_list_len"], {iterLoaded}, "len");
    }
    llvm::Value* cond = impl_->builder->CreateICmpSLT(currentIdx, lenVal, "cmp");
    impl_->builder->CreateCondBr(cond, bodyBB, elseBB);

    impl_->builder->SetInsertPoint(bodyBB);
    impl_->pushScope();

    currentIdx = impl_->builder->CreateLoad(impl_->i64Type, idxVar, "__i");
    iterLoaded = impl_->builder->CreateLoad(impl_->i8PtrType, iterAlloca, "__iter");

    if (tupleTarget && isDictItemsIterable) {
        Impl::VarKind valVarKind = Impl::VarKind::Int;
        if (auto* methCall = dynamic_cast<CallExpr*>(node.iterable.get())) {
            if (auto* methAttr = dynamic_cast<AttributeExpr*>(methCall->callee.get())) {
                if (auto* dn = dynamic_cast<NameExpr*>(methAttr->object.get())) {
                    auto vit = impl_->varDictValueKinds.find(dn->name);
                    if (vit != impl_->varDictValueKinds.end()) {
                        Type::Kind k = vit->second;
                        if (k == Type::Kind::Str) valVarKind = Impl::VarKind::Str;
                        else if (k == Type::Kind::Float) valVarKind = Impl::VarKind::Float;
                        else if (k == Type::Kind::Bool) valVarKind = Impl::VarKind::Bool;
                        else if (k == Type::Kind::Bytes) valVarKind = Impl::VarKind::List;
                        else if (k == Type::Kind::List) valVarKind = Impl::VarKind::List;
                        else if (k == Type::Kind::Dict) valVarKind = Impl::VarKind::Dict;
                        else if (k == Type::Kind::Instance) valVarKind = Impl::VarKind::ClassInstance;
                    }
                }
            }
        }

        llvm::Value* elem = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_list_get"], {iterLoaded, currentIdx}, "elem");
        llvm::Value* tuplePtr = impl_->builder->CreateIntToPtr(elem, impl_->i8PtrType, "tupleptr");

        for (size_t i = 0; i < tupleTarget->elements.size(); i++) {
            if (auto* name = dynamic_cast<NameExpr*>(tupleTarget->elements[i].get())) {
                llvm::Value* idx = llvm::ConstantInt::get(impl_->i64Type, i);
                llvm::Value* val = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_tuple_get"], {tuplePtr, idx}, "unpack");
                if (i == 0) {
                    if (dictKeysAreFloat) {
                        auto* alloca = impl_->createEntryAlloca(func, name->name, impl_->f64Type);
                        impl_->builder->CreateStore(
                            impl_->builder->CreateBitCast(val, impl_->f64Type, "k.f"),
                            alloca);
                        impl_->setVar(name->name, alloca, Impl::VarKind::Float);
                    } else if (dictKeysAreInt) {
                        auto* alloca = impl_->createEntryAlloca(func, name->name, impl_->i64Type);
                        impl_->builder->CreateStore(val, alloca);
                        impl_->setVar(name->name, alloca, Impl::VarKind::Int);
                    } else {
                        llvm::Value* strPtr = impl_->builder->CreateIntToPtr(val, impl_->i8PtrType, "keystr");
                        auto* alloca = impl_->createEntryAlloca(func, name->name, impl_->i8PtrType);
                        impl_->builder->CreateStore(strPtr, alloca);
                        impl_->setVar(name->name, alloca, Impl::VarKind::Str);
                        impl_->scopes.back().borrowed.insert(name->name);
                    }
                } else {
                    llvm::Type* slotTy;
                    llvm::Value* slotVal;
                    if (valVarKind == Impl::VarKind::Float) {
                        slotTy = impl_->f64Type;
                        slotVal = impl_->builder->CreateBitCast(val, impl_->f64Type, "v.f");
                    } else if (valVarKind == Impl::VarKind::Bool) {
                        slotTy = impl_->i1Type;
                        slotVal = impl_->builder->CreateICmpNE(
                            val, llvm::ConstantInt::get(impl_->i64Type, 0), "v.b");
                    } else if (valVarKind == Impl::VarKind::Str ||
                               valVarKind == Impl::VarKind::List ||
                               valVarKind == Impl::VarKind::Dict ||
                               valVarKind == Impl::VarKind::Tuple ||
                               valVarKind == Impl::VarKind::Set ||
                               valVarKind == Impl::VarKind::ClassInstance) {
                        slotTy = impl_->i8PtrType;
                        slotVal = impl_->builder->CreateIntToPtr(val, impl_->i8PtrType, "v.p");
                    } else {
                        slotTy = impl_->i64Type;
                        slotVal = val;
                    }
                    auto* alloca = impl_->lookupVar(name->name);
                    if (!alloca || alloca->getAllocatedType() != slotTy)
                        alloca = impl_->createEntryAlloca(func, name->name, slotTy);
                    impl_->setVar(name->name, alloca, valVarKind);
                    // items() co-owns the value (dragon_dict_items increfs it); mark it
                    // BORROWED, else per-iter cleanup decref'd it and tuple+dict destroy double-freed it.
                    if (Impl::isHeapKind(valVarKind))
                        impl_->scopes.back().borrowed.insert(name->name);
                    impl_->builder->CreateStore(slotVal, alloca);
                }
            }
        }
    } else if (tupleTarget) {
        std::vector<Type::Kind> posKinds(tupleTarget->elements.size(), Type::Kind::Int);
        if (node.iterable->type && node.iterable->type->kind() == Type::Kind::List) {
            auto& lt = static_cast<ListType&>(*node.iterable->type);
            if (lt.elementType && lt.elementType->kind() == Type::Kind::Tuple) {
                auto& tt = static_cast<TupleType&>(*lt.elementType);
                for (size_t i = 0; i < posKinds.size() && i < tt.elementTypes.size(); i++) {
                    if (tt.elementTypes[i]) posKinds[i] = tt.elementTypes[i]->kind();
                }
            }
        }
        auto srcElemKind = [&](Expr* e) -> Type::Kind {
            if (auto* nm = dynamic_cast<NameExpr*>(e)) {
                auto it = impl_->varListElemKinds.find(nm->name);
                if (it != impl_->varListElemKinds.end()) return it->second;
            }
            if (e && e->type && e->type->kind() == Type::Kind::List)
                return static_cast<ListType&>(*e->type).elementType->kind();
            return Type::Kind::Int;
        };
        if (auto* itCall = dynamic_cast<CallExpr*>(node.iterable.get())) {
            if (auto* itCallee = dynamic_cast<NameExpr*>(itCall->callee.get())) {
                if (itCallee->name == "enumerate" && !itCall->args.empty() &&
                    posKinds.size() >= 2) {
                    posKinds[1] = srcElemKind(itCall->args[0].get());
                } else if (itCallee->name == "zip" && itCall->args.size() >= 2) {
                    if (posKinds.size() >= 1) posKinds[0] = srcElemKind(itCall->args[0].get());
                    if (posKinds.size() >= 2) posKinds[1] = srcElemKind(itCall->args[1].get());
                }
            }
        }
        llvm::Value* elem = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_list_get"], {iterLoaded, currentIdx}, "elem");
        llvm::Value* tuplePtr = impl_->builder->CreateIntToPtr(elem, impl_->i8PtrType, "tupleptr");
        for (size_t i = 0; i < tupleTarget->elements.size(); i++) {
            if (auto* name = dynamic_cast<NameExpr*>(tupleTarget->elements[i].get())) {
                llvm::Value* idx = llvm::ConstantInt::get(impl_->i64Type, i);
                llvm::Value* val = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_tuple_get"], {tuplePtr, idx}, "unpack");
                Type::Kind ek = posKinds[i];
                llvm::Type* slotTy = impl_->i64Type;
                llvm::Value* slotVal = val;
                Impl::VarKind vk = Impl::VarKind::Int;
                if (ek == Type::Kind::Float) {
                    slotTy = impl_->f64Type; vk = Impl::VarKind::Float;
                    slotVal = impl_->builder->CreateBitCast(val, impl_->f64Type, "u.f");
                } else if (ek == Type::Kind::Bool) {
                    slotTy = impl_->i1Type; vk = Impl::VarKind::Bool;
                    slotVal = impl_->builder->CreateICmpNE(
                        val, llvm::ConstantInt::get(impl_->i64Type, 0), "u.b");
                } else if (ek == Type::Kind::Str) {
                    slotTy = impl_->i8PtrType; vk = Impl::VarKind::Str;
                    slotVal = impl_->builder->CreateIntToPtr(val, impl_->i8PtrType, "u.s");
                } else if (ek == Type::Kind::List || ek == Type::Kind::Bytes) {
                    slotTy = impl_->i8PtrType; vk = Impl::VarKind::List;
                    slotVal = impl_->builder->CreateIntToPtr(val, impl_->i8PtrType, "u.l");
                } else if (ek == Type::Kind::Dict) {
                    slotTy = impl_->i8PtrType; vk = Impl::VarKind::Dict;
                    slotVal = impl_->builder->CreateIntToPtr(val, impl_->i8PtrType, "u.d");
                } else if (ek == Type::Kind::Set) {
                    slotTy = impl_->i8PtrType; vk = Impl::VarKind::Set;
                    slotVal = impl_->builder->CreateIntToPtr(val, impl_->i8PtrType, "u.set");
                } else if (ek == Type::Kind::Tuple) {
                    slotTy = impl_->i8PtrType; vk = Impl::VarKind::Tuple;
                    slotVal = impl_->builder->CreateIntToPtr(val, impl_->i8PtrType, "u.t");
                } else if (ek == Type::Kind::Instance) {
                    slotTy = impl_->i8PtrType; vk = Impl::VarKind::ClassInstance;
                    slotVal = impl_->builder->CreateIntToPtr(val, impl_->i8PtrType, "u.i");
                }
                auto* alloca = impl_->lookupVar(name->name);
                if (!alloca || alloca->getAllocatedType() != slotTy)
                    alloca = impl_->createEntryAlloca(func, name->name, slotTy);
                impl_->setVar(name->name, alloca, vk);
                // dragon_tuple_get returns a BORROW (the tuple co-owns the element);
                // without this mark, scope cleanup decref'd it and tuple destroy double-freed it (`for k, v in d.items()` UAF).
                if (Impl::isHeapKind(vk))
                    impl_->scopes.back().borrowed.insert(name->name);
                impl_->builder->CreateStore(slotVal, alloca);
            }
        }
    } else if (isDictKeysIterable) {
        llvm::Value* elem = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_list_get"], {iterLoaded, currentIdx}, "elem");
        if (dictKeysAreFloat) {
            auto* targetAlloca = impl_->createEntryAlloca(func, targetName->name, impl_->f64Type);
            impl_->builder->CreateStore(
                impl_->builder->CreateBitCast(elem, impl_->f64Type, "key.f"),
                targetAlloca);
            impl_->setVar(targetName->name, targetAlloca, Impl::VarKind::Float);
        } else if (dictKeysAreInt) {
            auto* targetAlloca = impl_->createEntryAlloca(func, targetName->name, impl_->i64Type);
            impl_->builder->CreateStore(elem, targetAlloca);
            impl_->setVar(targetName->name, targetAlloca, Impl::VarKind::Int);
        } else {
            llvm::Value* strPtr = impl_->builder->CreateIntToPtr(elem, impl_->i8PtrType, "keystr");
            auto* targetAlloca = impl_->createEntryAlloca(func, targetName->name, impl_->i8PtrType);
            impl_->builder->CreateStore(strPtr, targetAlloca);
            impl_->setVar(targetName->name, targetAlloca, Impl::VarKind::Str);
            impl_->scopes.back().borrowed.insert(targetName->name);
        }
    } else if (isStrIterable) {
        llvm::Value* elem = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_str_index"], {iterLoaded, currentIdx}, "ch");
        auto* targetAlloca = impl_->createEntryAlloca(func, targetName->name, impl_->i8PtrType);
        impl_->builder->CreateStore(elem, targetAlloca);
        impl_->setVar(targetName->name, targetAlloca, Impl::VarKind::Str);
    } else {
        Type::Kind elemTypeKind = Type::Kind::Int;
        if (auto* iterName = dynamic_cast<NameExpr*>(node.iterable.get())) {
            auto it = impl_->varListElemKinds.find(iterName->name);
            if (it != impl_->varListElemKinds.end()) elemTypeKind = it->second;
        } else if (auto* iterAttr = dynamic_cast<AttributeExpr*>(node.iterable.get())) {
            std::string className;
            if (auto* objName = dynamic_cast<NameExpr*>(iterAttr->object.get())) {
                if (objName->name == "self" && !impl_->currentClassName.empty()) {
                    className = impl_->currentClassName;
                } else {
                    auto vit = impl_->varClassNames.find(objName->name);
                    if (vit != impl_->varClassNames.end()) className = vit->second;
                }
            }
            if (!className.empty()) {
                auto cit = impl_->classFieldListElemKindsBySym.find(impl_->classSym(className));
                if (cit != impl_->classFieldListElemKindsBySym.end()) {
                    auto fit = cit->second.find(iterAttr->attribute);
                    if (fit != cit->second.end()) elemTypeKind = fit->second;
                }
            }
        } else if (auto* iterCall = dynamic_cast<CallExpr*>(node.iterable.get())) {
            if (auto* methAttr = dynamic_cast<AttributeExpr*>(iterCall->callee.get())) {
                if (methAttr->attribute == "values") {
                    if (auto* dn = dynamic_cast<NameExpr*>(methAttr->object.get())) {
                        auto vit = impl_->varDictValueKinds.find(dn->name);
                        if (vit != impl_->varDictValueKinds.end())
                            elemTypeKind = vit->second;
                    }
                }
            }
        }

        if (elemTypeKind == Type::Kind::Int) {
            if (auto* iterAttr = dynamic_cast<AttributeExpr*>(node.iterable.get())) {
                std::string ownerClass;
                if (auto* objName = dynamic_cast<NameExpr*>(iterAttr->object.get())) {
                    if (objName->name == "self" && !impl_->currentClassName.empty())
                        ownerClass = impl_->currentClassName;
                    else {
                        auto vit = impl_->varClassNames.find(objName->name);
                        if (vit != impl_->varClassNames.end()) ownerClass = vit->second;
                    }
                }
                if (!ownerClass.empty()) {
                    auto cit = impl_->classFieldListElemClassNameBySym.find(impl_->classSym(ownerClass));
                    if (cit != impl_->classFieldListElemClassNameBySym.end()) {
                        auto fit = cit->second.find(iterAttr->attribute);
                        if (fit != cit->second.end())
                            elemTypeKind = Type::Kind::Instance;
                    }
                }
            }
            if (auto* iterName = dynamic_cast<NameExpr*>(node.iterable.get())) {
                auto it = impl_->varListElemClassName.find(iterName->name);
                if (it != impl_->varListElemClassName.end())
                    elemTypeKind = Type::Kind::Instance;
            }
        }

        if (elemTypeKind == Type::Kind::Int && node.iterable->type) {
            if (auto* lt = dynamic_cast<ListType*>(node.iterable->type.get())) {
                if (lt->elementType) {
                    switch (lt->elementType->kind()) {
                        case Type::Kind::Str:
                        case Type::Kind::Bytes:
                        case Type::Kind::Float:
                        case Type::Kind::Bool:
                        case Type::Kind::List:
                        case Type::Kind::Dict:
                        case Type::Kind::Set:
                        case Type::Kind::Tuple:
                        case Type::Kind::Instance:
                            elemTypeKind = lt->elementType->kind();
                            break;
                        default:
                            break;
                    }
                }
            }
        }

        Impl::VarKind elemVarKind = Impl::typeKindToVarKind(elemTypeKind);
        if (auto* iterName = dynamic_cast<NameExpr*>(node.iterable.get())) {
            if (impl_->varListElemIsType.count(iterName->name))
                elemVarKind = Impl::VarKind::Type;
        }

        std::string elemClassName;
        if (elemTypeKind == Type::Kind::Instance) {
            if (auto* iterName = dynamic_cast<NameExpr*>(node.iterable.get())) {
                auto it = impl_->varListElemClassName.find(iterName->name);
                if (it != impl_->varListElemClassName.end()) elemClassName = it->second;
            } else if (auto* iterAttr = dynamic_cast<AttributeExpr*>(node.iterable.get())) {
                std::string ownerClass;
                if (auto* objName = dynamic_cast<NameExpr*>(iterAttr->object.get())) {
                    if (objName->name == "self" && !impl_->currentClassName.empty())
                        ownerClass = impl_->currentClassName;
                    else {
                        auto vit = impl_->varClassNames.find(objName->name);
                        if (vit != impl_->varClassNames.end()) ownerClass = vit->second;
                    }
                }
                if (!ownerClass.empty()) {
                    auto cit = impl_->classFieldListElemClassNameBySym.find(impl_->classSym(ownerClass));
                    if (cit != impl_->classFieldListElemClassNameBySym.end()) {
                        auto fit = cit->second.find(iterAttr->attribute);
                        if (fit != cit->second.end()) elemClassName = fit->second;
                    }
                }
            }
            if (elemClassName.empty() && node.iterable->type) {
                if (auto* lt = dynamic_cast<ListType*>(node.iterable->type.get())) {
                    if (auto* it2 = dynamic_cast<InstanceType*>(lt->elementType.get())) {
                        if (it2->classType) elemClassName = it2->classType->name;
                    }
                }
            }
        }

        llvm::AllocaInst* targetAlloca;
        if (elemVarKind == Impl::VarKind::Type) {
            targetAlloca = impl_->bindListElemTyped(
                func, iterLoaded, currentIdx, targetName->name, elemVarKind);
        } else {
            targetAlloca = impl_->bindListElemByTypeKind(
                func, iterLoaded, currentIdx, targetName->name, elemTypeKind);
        }
        impl_->setVar(targetName->name, targetAlloca, elemVarKind);
        if (elemTypeKind == Type::Kind::Instance && !elemClassName.empty())
            impl_->varClassNames[targetName->name] = elemClassName;
        if (elemTypeKind == Type::Kind::Dict) {
            std::string tdCls;
            if (auto* iterName = dynamic_cast<NameExpr*>(node.iterable.get())) {
                auto it = impl_->varListElemClassName.find(iterName->name);
                if (it != impl_->varListElemClassName.end()) tdCls = it->second;
            }
            if (tdCls.empty() && node.iterable->type) {
                if (auto* lt = dynamic_cast<ListType*>(node.iterable->type.get()))
                    if (auto* inst = dynamic_cast<InstanceType*>(lt->elementType.get()))
                        if (inst->classType && inst->classType->isTypedDict)
                            tdCls = inst->classType->name;
            }
            if (!tdCls.empty() && impl_->typedDictClassesBySym.count(impl_->classSym(tdCls)))
                impl_->varTypedDictClass[targetName->name] = tdCls;
        }
        if (Impl::isHeapTypeKind(elemTypeKind))
            impl_->scopes.back().borrowed.insert(targetName->name);

        llvm::FunctionType* elemCallable = nullptr;
        if (auto* iterName = dynamic_cast<NameExpr*>(node.iterable.get())) {
            auto it = impl_->varListElemCallableType.find(iterName->name);
            if (it != impl_->varListElemCallableType.end())
                elemCallable = it->second;
        } else if (auto* iterAttr = dynamic_cast<AttributeExpr*>(node.iterable.get())) {
            std::string ownerClass;
            if (auto* objName = dynamic_cast<NameExpr*>(iterAttr->object.get())) {
                if (objName->name == "self" && !impl_->currentClassName.empty())
                    ownerClass = impl_->currentClassName;
                else {
                    auto vit = impl_->varClassNames.find(objName->name);
                    if (vit != impl_->varClassNames.end()) ownerClass = vit->second;
                }
            }
            if (!ownerClass.empty()) {
                auto cit = impl_->classFieldListElemCallableTypeBySym.find(impl_->classSym(ownerClass));
                if (cit != impl_->classFieldListElemCallableTypeBySym.end()) {
                    auto fit = cit->second.find(iterAttr->attribute);
                    if (fit != cit->second.end()) elemCallable = fit->second;
                }
            }
        }
        if (elemCallable) {
            impl_->callableTypes[targetName->name] = elemCallable;
            impl_->varIsPtrCallable.insert(targetName->name);
        }
    }

    for (auto& stmt : node.body) stmt->accept(*this);
    impl_->emitScopeCleanup();
    impl_->popScope();
    if (!impl_->builder->GetInsertBlock()->getTerminator())
        impl_->builder->CreateBr(incBB);

    impl_->builder->SetInsertPoint(incBB);
    currentIdx = impl_->builder->CreateLoad(impl_->i64Type, idxVar, "__i");
    llvm::Value* nextIdx = impl_->builder->CreateAdd(
        currentIdx, llvm::ConstantInt::get(impl_->i64Type, 1), "inc");
    impl_->builder->CreateStore(nextIdx, idxVar);
    impl_->builder->CreateBr(condBB);

    impl_->loopStack.pop();

    if (elseBB != endBB) {
        impl_->builder->SetInsertPoint(elseBB);
        impl_->pushScope();
        for (auto& stmt : node.elseBody) stmt->accept(*this);
        impl_->emitScopeCleanup();
        impl_->popScope();
        if (!impl_->builder->GetInsertBlock()->getTerminator())
            impl_->builder->CreateBr(endBB);
    }

    impl_->builder->SetInsertPoint(endBB);

}

}
