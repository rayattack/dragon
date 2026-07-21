/// Dragon CodeGen - For Loop
#include "../CodeGenImpl.h"

namespace dragon {

void CodeGen::visit(ForStmt& node) {
    auto* func = impl_->currentFunction;
    auto* targetName = dynamic_cast<NameExpr*>(node.target.get());
    auto* tupleTarget = dynamic_cast<TupleExpr*>(node.target.get());
    if (!targetName && !tupleTarget) return;

    // Class-based enum: `for c in Color` iterates the synthesized `__members__`
    // singleton list. Rewrite the iterable to `Color.__members__` and stamp it
    // with type list[Instance(Color)] so the list-iteration path below binds the
    // loop variable as a Color instance (sets varClassNames). __members__ was
    // created by synthesizeEnumMethods.
    if (auto* enumName = dynamic_cast<NameExpr*>(node.iterable.get())) {
        if (impl_->enumKind.count(enumName->name)) {
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

    // For-range optimization: for i in range(start, end, step) - only for simple targets
    auto* callExpr = dynamic_cast<CallExpr*>(node.iterable.get());
    auto* calleeName = callExpr ? dynamic_cast<NameExpr*>(callExpr->callee.get()) : nullptr;
    bool isRange = calleeName && calleeName->name == "range";

    if (isRange && targetName) {
        // --- Range-based for loop ---
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

        // Create loop variable
        auto* loopVar = impl_->createEntryAlloca(func, targetName->name, impl_->i64Type);
        impl_->setVar(targetName->name, loopVar);
        impl_->builder->CreateStore(startVal, loopVar);

        auto* condBB = llvm::BasicBlock::Create(*impl_->context, "forcond", func);
        auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, "forbody", func);
        auto* incBB = llvm::BasicBlock::Create(*impl_->context, "forinc", func);
        auto* endBB = llvm::BasicBlock::Create(*impl_->context, "forend", func);
        // `for ... else`: else runs on natural exhaustion (range condition
        // false), skipped on `break`. The exhausted edge targets elseBB;
        // `break` keeps targeting endBB so it bypasses else. Absent else ->
        // elseBB == endBB (unchanged flow).
        llvm::BasicBlock* elseBB = node.elseBody.empty()
            ? endBB
            : llvm::BasicBlock::Create(*impl_->context, "forelse", func);

        impl_->loopStack.push({endBB, incBB, impl_->scopes.size(), impl_->tryFrameFuncs.size(), impl_->exitCleanupStack.size()});
        impl_->builder->CreateBr(condBB);

        // Condition: i < end - false exhausts the loop and runs the else.
        impl_->builder->SetInsertPoint(condBB);
        llvm::Value* current = impl_->builder->CreateLoad(impl_->i64Type, loopVar, "i");
        llvm::Value* cond = impl_->builder->CreateICmpSLT(current, endVal, "cmp");
        impl_->builder->CreateCondBr(cond, bodyBB, elseBB);

        // Body
        impl_->builder->SetInsertPoint(bodyBB);
        impl_->pushScope();
        for (auto& stmt : node.body) stmt->accept(*this);
        impl_->emitScopeCleanup();
        impl_->popScope();
        if (!impl_->builder->GetInsertBlock()->getTerminator())
            impl_->builder->CreateBr(incBB);

        // Increment
        impl_->builder->SetInsertPoint(incBB);
        current = impl_->builder->CreateLoad(impl_->i64Type, loopVar, "i");
        llvm::Value* next = impl_->builder->CreateAdd(current, stepVal, "inc");
        impl_->builder->CreateStore(next, loopVar);
        impl_->builder->CreateBr(condBB);

        impl_->loopStack.pop();
        // Else body - only on natural exhaustion; falls through to endBB.
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

    // --- Generator iteration: for x in gen_func(...) ---
    {
        bool isGenerator = false;
        // Yielded value's VarKind - defaults to Int (matches the original
        // behavior and works for the common "yield i" case).
        Impl::VarKind yieldKind = Impl::VarKind::Int;
        // Check if iterable is a call to a known generator function.
        // Use resolveCalleeSymbol so the same-module / aliased / mangled
        // chain finds the correct key - generatorFunctions is keyed by the
        // LLVM symbol (post-mangling) since per-module mangling landed.
        if (auto* callExpr = dynamic_cast<CallExpr*>(node.iterable.get())) {
            if (auto* callee = dynamic_cast<NameExpr*>(callExpr->callee.get())) {
                std::string sym = impl_->resolveCalleeSymbol(callee->name);
                if (impl_->generatorFunctions.count(sym)) {
                    isGenerator = true;
                    auto it = impl_->generatorYieldKinds.find(sym);
                    if (it != impl_->generatorYieldKinds.end()) yieldKind = it->second;
                }
            } else if (auto* attr = dynamic_cast<AttributeExpr*>(callExpr->callee.get())) {
                // Method-generator call: `obj.gen_method(...)` / `self.gen_method(...)`.
                // Resolve the receiver's class + the method's mangled symbol and
                // check the generator registry (the method wrapper returns the
                // generator object, so the body iterates it like any generator).
                std::string cn = impl_->resolveExprClassName(attr->object.get());
                // Static call form `ClassName.gen_method(...)`: the receiver is a
                // class name, not an instance, so resolve the class directly.
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
        // Also check if iterable is a variable with VarKind::Generator
        if (!isGenerator) {
            if (auto* nameExpr = dynamic_cast<NameExpr*>(node.iterable.get())) {
                if (impl_->lookupVarKind(nameExpr->name) == Impl::VarKind::Generator) {
                    isGenerator = true;
                    // Yield kind for stored generators is recorded at the
                    // assignment site (g = gen() -> varGenYieldKinds[g]).
                    auto kit = impl_->varGenYieldKinds.find(nameExpr->name);
                    if (kit != impl_->varGenYieldKinds.end()) yieldKind = kit->second;
                }
            }
        }
        auto* targetName = dynamic_cast<NameExpr*>(node.target.get());
        if (isGenerator && targetName) {
            auto* func = impl_->currentFunction;

            // Evaluate the iterable (generator object)
            node.iterable->accept(*this);
            llvm::Value* genObj = impl_->lastValue;
            if (!genObj->getType()->isPointerTy())
                genObj = impl_->builder->CreateIntToPtr(genObj, impl_->i8PtrType);

            // Create loop variable. dragon_generator_next returns i64; for
            // heap-typed yields we IntToPtr-cast on each read so the loop
            // body sees the correct ptr type.
            bool yieldIsHeap = Impl::isHeapKind(yieldKind);
            auto* loopVar = impl_->createEntryAlloca(func, targetName->name,
                yieldIsHeap ? impl_->i8PtrType : impl_->i64Type);
            impl_->setVar(targetName->name, loopVar, yieldKind);
            // The yielded heap value is borrowed from the generator's
            // perspective today (no incref on yield, no decref on next).
            // Mark the loop var as borrowed so per-iter cleanup doesn't
            // free it.
            if (yieldIsHeap) impl_->scopes.back().borrowed.insert(targetName->name);

            auto* genAlloca = impl_->createEntryAlloca(func, "__gen_iter", impl_->i8PtrType);
            impl_->builder->CreateStore(genObj, genAlloca);
            // Register the generator temp for unwind cleanup: it is owned by the
            // for-loop (decref'd at endBB), but a raise that unwinds past this
            // frame (e.g. from the loop body) skips endBB and would leak the
            // generator + its coroutine stack. The cleanup stack frees it on that
            // path; emitCleanupPopTemp rewinds past it on the normal-exit decref.
            llvm::Value* genCleanupBase =
                impl_->emitCleanupPushTemp(genObj, Impl::DCLEAN_OBJ);

            auto* condBB = llvm::BasicBlock::Create(*impl_->context, "gen.cond", func);
            auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, "gen.body", func);
            auto* endBB = llvm::BasicBlock::Create(*impl_->context, "gen.end", func);
            // `for ... else`: else runs on natural exhaustion (StopIteration),
            // skipped on `break` (which targets endBB). Absent else ->
            // elseBB == endBB (unchanged flow).
            llvm::BasicBlock* elseBB = node.elseBody.empty()
                ? endBB
                : llvm::BasicBlock::Create(*impl_->context, "gen.else", func);

            impl_->loopStack.push({endBB, condBB, impl_->scopes.size(), impl_->tryFrameFuncs.size(), impl_->exitCleanupStack.size()});
            impl_->builder->CreateBr(condBB);

            // Condition: call dragon_generator_next(), catch StopIteration
            impl_->builder->SetInsertPoint(condBB);
            auto* genPtr = impl_->builder->CreateLoad(impl_->i8PtrType, genAlloca, "gen.ptr");

            // Push exception frame (setjmp)
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

            // Normal path: call dragon_generator_next()
            impl_->builder->SetInsertPoint(nextBB);
            auto* nextVal = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_generator_next"], {genPtr}, "gen.val");
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_pop_frame"], {});
            // For heap-typed yields, dragon_generator_next returns the value as
            // i64 (the runtime API is type-erased); cast back to ptr so the
            // loop variable's allocated type matches.
            if (yieldIsHeap) {
                llvm::Value* asPtr = impl_->builder->CreateIntToPtr(
                    nextVal, impl_->i8PtrType, "gen.val.ptr");
                impl_->builder->CreateStore(asPtr, loopVar);
            } else {
                impl_->builder->CreateStore(nextVal, loopVar);
            }
            impl_->builder->CreateBr(bodyBB);

            // Exception path: check if StopIteration (code 11)
            impl_->builder->SetInsertPoint(excBB);
            // Free consumer-side owned heap locals the longjmp skipped (no-op on
            // the StopIteration exhaustion path - each iteration already reset the
            // cleanup stack). Before pop_frame so it reads this frame's depth.
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_cleanup_unwind"], {});
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_pop_frame"], {});
            auto* excType = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_exc_get_type"], {}, "exc.type");
            auto* isStopIter = impl_->builder->CreateICmpEQ(
                excType,
                llvm::ConstantInt::get(impl_->i64Type, 11),
                "is.stopiter");
            auto* reraiseBB = llvm::BasicBlock::Create(*impl_->context, "gen.reraise", func);
            // StopIteration = natural exhaustion -> run the else (or endBB).
            impl_->builder->CreateCondBr(isStopIter, elseBB, reraiseBB);

            // Re-raise non-StopIteration exceptions. Do NOT explicitly decref
            // outer-scope locals here: re-raising longjmps to the nearest
            // handler, and the cleanup-stack unwind frees exactly the locals
            // registered between the raise point and that handler - whether it
            // is in THIS function (a `try` around the for-loop) or an outer one.
            // An explicit emitAllScopeCleanup() here wrongly assumed the
            // exception always leaves the function; when it is caught locally,
            // it decref'd a still-live local (e.g. the generator's receiver
            // `self`, also held by the coroutine), which scope-exit then
            // decref'd again - a use-after-free. This mirrors the plain
            // RaiseStmt path, which likewise relies on the cleanup stack.
            impl_->builder->SetInsertPoint(reraiseBB);
            {
                auto* reType = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_exc_get_type"], {}, "reraise.type");
                // The exc slot OWNS its message (dragon_exc_msg_set) - it is
                // never an alias of a scope local, so re-raising with msg ==
                // slot hits the self-store no-op, keeping the slot's ownership
                // intact. (The old dragon_exc_msg_preserve snapshot here would
                // now leak: the plain raise dups it AGAIN into the slot.)
                auto* reMsg = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_exc_get_msg"], {}, "reraise.msg");
                // Preserve the in-flight instance pointer too - see the matching
                // RaiseStmt reraise path. obj=NULL when only a message was raised.
                // Retained: the obj-raise consumes a +1; the same-pointer fold
                // in dragon_exc_obj_set nets it out.
                auto* reObj = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_exc_retain_obj"],
                    {impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_exc_get_obj"], {},
                        "reraise.obj.raw")},
                    "reraise.obj");
                // The generator body raised and longjmp'd out of mco_resume,
                // abandoning the coroutine mid-run (MCO_RUNNING) with minicoro's
                // running-coroutine pointer left dangling at it. Restore that
                // bookkeeping and mark the coroutine dead so it can be reclaimed
                // - must run here at the longjmp arrival, before any further
                // resume. (The body's heap locals were already freed by the
                // cleanup-stack unwind at excBB above.)
                {
                    auto* gAb = impl_->builder->CreateLoad(
                        impl_->i8PtrType, genAlloca, "gen.abandon");
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_generator_abandon"], {gAb});
                }
                // The generator object is normally decref'd at endBB, but this
                // re-raise bypasses endBB (it longjmps out), so decref it here -
                // otherwise the generator + its coroutine stack leak. Mirror
                // endBB EXACTLY: decref then pop its cleanup-stack entry, so the
                // outer handler's unwind does not also free the (now stale)
                // snapshot (double-free).
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

            // Body
            impl_->builder->SetInsertPoint(bodyBB);
            impl_->pushScope();
            for (auto& stmt : node.body) stmt->accept(*this);
            impl_->emitScopeCleanup();
            impl_->popScope();
            if (!impl_->builder->GetInsertBlock()->getTerminator())
                impl_->builder->CreateBr(condBB);

            impl_->loopStack.pop();

            // Else body - only on natural exhaustion (StopIteration edge).
            // Falls through to endBB, where the generator object is decref'd
            // on every exit path (break and natural alike).
            if (elseBB != endBB) {
                impl_->builder->SetInsertPoint(elseBB);
                impl_->pushScope();
                for (auto& stmt : node.elseBody) stmt->accept(*this);
                impl_->emitScopeCleanup();
                impl_->popScope();
                if (!impl_->builder->GetInsertBlock()->getTerminator())
                    impl_->builder->CreateBr(endBB);
            }

            // End
            impl_->builder->SetInsertPoint(endBB);

            // Cleanup: decref the generator object
            if (impl_->options.gcMode == GCMode::RC) {
                auto* finalGen = impl_->builder->CreateLoad(impl_->i8PtrType, genAlloca, "gen.cleanup");
                impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {finalGen});
                // Drop the generator's unwind-cleanup snapshot now that codegen
                // freed it (a later unwind must not re-free the stale pointer).
                impl_->emitCleanupPopTemp(genCleanupBase);
            }
            return;
        }
    }

    // --- __iter__/__next__ protocol for class instances ---
    {
        std::string iterClassName = impl_->resolveExprClassName(node.iterable.get());
        auto* targetName = dynamic_cast<NameExpr*>(node.target.get());
        if (!iterClassName.empty() && targetName &&
            impl_->hasDunder(iterClassName, "__iter__")) {
            auto* func = impl_->currentFunction;

            // Call __iter__() to get iterator
            node.iterable->accept(*this);
            llvm::Value* iterable = impl_->lastValue;
            auto* iterator = impl_->callDunder(iterClassName, "__iter__", iterable);

            // Determine loop variable type AND kind from __next__'s declared
            // return type. The LLVM type alone is ambiguous for `ptr` (str /
            // list / dict / bytes / instance all lower to ptr), so we also
            // consult methodReturnKinds - populated from the AST returnType
            // at class declaration time - to pick the right VarKind for the
            // bound value. Without the VarKind, method dispatch on `x` falls
            // through default int handling, so e.g. x.strip() on a str-typed
            // __next__ result returns 0 rather than the trimmed string.
            llvm::Type* loopVarType = impl_->i8PtrType; // default
            Impl::VarKind loopVarKind = Impl::VarKind::Other;
            std::string loopVarClassName;
            {
                std::string nextClass = impl_->findDunderClass(iterClassName, "__next__");
                if (!nextClass.empty()) {
                    // Cross-module classes are forward-declared with per-module
                    // mangling (`<mod>__<cls>_<method>`), and methodReturnKinds /
                    // methodReturnClassNames are keyed by that mangled symbol.
                    // Using the bare class name as the key only matched when
                    // the iterator class lived in the entry module - for an
                    // imported `for line in it` the lookups all missed and
                    // loopVarKind stayed `Other`, which dropped `line.strip()`
                    // into the default i8* method-dispatch path and miscompiled.
                    auto cmIt = impl_->classOwningModule.find(nextClass);
                    std::string defMod = cmIt != impl_->classOwningModule.end()
                                             ? cmIt->second
                                             : impl_->currentModuleName;
                    std::string methKey = Impl::mangleClass(defMod, nextClass) + "___next__";
                    auto* nextFn = impl_->module->getFunction(methKey);
                    if (nextFn) loopVarType = nextFn->getReturnType();
                    auto rkIt = impl_->methodReturnKinds.find(methKey);
                    if (rkIt != impl_->methodReturnKinds.end())
                        loopVarKind = Impl::typeKindToVarKind(rkIt->second);
                    // __next__ returning a class instance: also record the
                    // concrete class so attribute access on `x` resolves via
                    // the right struct (mirrors the field-from-method path).
                    auto rcIt = impl_->methodReturnClassNames.find(methKey);
                    if (rcIt != impl_->methodReturnClassNames.end())
                        loopVarClassName = rcIt->second;
                }
            }
            auto* loopVar = impl_->createEntryAlloca(func, targetName->name, loopVarType);
            // Zero-init: the first iteration's RC-overwrite (below) loads the
            // previous element before storing - a null previous no-ops.
            impl_->emitNullSlot(loopVar);
            impl_->setVar(targetName->name, loopVar, loopVarKind);
            if (!loopVarClassName.empty())
                impl_->varClassNames[targetName->name] = loopVarClassName;
            // The __next__ result is a real method CALL, and Dragon method
            // returns are OWNED (+1): ReturnStmt increfs a borrowed return
            // (a shared-field `return self.items[i]` included), so the loop
            // variable owns each element and the per-iteration scope cleanup
            // is exactly the right release. The previous borrowed-marking
            // (copied from the GENERATOR yield convention, which is a
            // different machinery) leaked one element per iteration - every
            // line of `for line in open(p)` (A/B-proven, both shapes:
            // fresh-returning and shared-field-returning __next__).

            // Store iterator and register for GC cleanup on all exit paths.
            // Unique scope name per loop so a sibling for-loop's setVar can't
            // clobber this binding and leak its iterator temp (see forIterCounter).
            std::string iterObjName = "__iter_obj." + std::to_string(impl_->forIterCounter++);
            auto* iterAlloca = impl_->createEntryAlloca(func, iterObjName, impl_->i8PtrType);
            impl_->builder->CreateStore(iterator, iterAlloca);
            impl_->setVar(iterObjName, iterAlloca, Impl::VarKind::ClassInstance);

            auto* condBB = llvm::BasicBlock::Create(*impl_->context, "iter.cond", func);
            auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, "iter.body", func);
            auto* endBB = llvm::BasicBlock::Create(*impl_->context, "iter.end", func);
            // `for ... else`: else runs on natural exhaustion (StopIteration),
            // skipped on `break` (which targets endBB). Absent else ->
            // elseBB == endBB (unchanged flow).
            llvm::BasicBlock* elseBB = node.elseBody.empty()
                ? endBB
                : llvm::BasicBlock::Create(*impl_->context, "iter.else", func);

            impl_->loopStack.push({endBB, condBB, impl_->scopes.size(), impl_->tryFrameFuncs.size(), impl_->exitCleanupStack.size()});
            impl_->builder->CreateBr(condBB);

            // Condition: try calling __next__(), catch StopIteration
            impl_->builder->SetInsertPoint(condBB);
            auto* iterObj = impl_->builder->CreateLoad(impl_->i8PtrType, iterAlloca, "iter.obj");

            // Push exception frame (setjmp)
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

            // Normal path: call __next__()
            impl_->builder->SetInsertPoint(nextBB);
            auto* nextVal = impl_->callDunder(iterClassName, "__next__", iterObj);
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_pop_frame"], {});
            // RC overwrite: the loop var OWNS each element (a method return
            // is +1), so release the previous iteration's element before
            // storing the next - a plain store orphaned every element but
            // the last (A/B-proven; the slot starts null, decref no-ops).
            if (impl_->options.gcMode == GCMode::RC &&
                Impl::isHeapKind(loopVarKind)) {
                auto* prevVal = impl_->builder->CreateLoad(
                    loopVarType, loopVar, "iter.prev");
                impl_->emitDecrefByKind(prevVal, loopVarKind);
            }
            impl_->builder->CreateStore(nextVal, loopVar);
            impl_->builder->CreateBr(bodyBB);

            // Exception path: check if StopIteration
            impl_->builder->SetInsertPoint(excBB);
            // Free consumer-side owned heap locals the longjmp skipped (no-op on
            // the StopIteration exhaustion path). Before pop_frame.
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_cleanup_unwind"], {});
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_pop_frame"], {});
            auto* excType = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_exc_get_type"], {}, "exc.type");
            auto* isStopIter = impl_->builder->CreateICmpEQ(
                excType,
                llvm::ConstantInt::get(impl_->i64Type, 11), // StopIteration = 11
                "is.stopiter");
            auto* reraiseBB = llvm::BasicBlock::Create(*impl_->context, "iter.reraise", func);
            // StopIteration = natural exhaustion -> run the else (or endBB).
            impl_->builder->CreateCondBr(isStopIter, elseBB, reraiseBB);

            impl_->builder->SetInsertPoint(reraiseBB);
            {
                auto* reType = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_exc_get_type"], {}, "reraise.type");
                // Slot owns its message; cleanup can't free it and the
                // re-raise's self-store no-op keeps it. See matching site above.
                auto* reMsg = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_exc_get_msg"], {}, "reraise.msg");
                // Retained slot re-raise (same-pointer fold nets it out).
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

            // Body
            impl_->builder->SetInsertPoint(bodyBB);
            impl_->pushScope();
            for (auto& stmt : node.body) stmt->accept(*this);
            impl_->emitScopeCleanup();
            impl_->popScope();
            if (!impl_->builder->GetInsertBlock()->getTerminator())
                impl_->builder->CreateBr(condBB);

            impl_->loopStack.pop();

            // Else body - only on natural exhaustion (StopIteration edge);
            // falls through to endBB.
            if (elseBB != endBB) {
                impl_->builder->SetInsertPoint(elseBB);
                impl_->pushScope();
                for (auto& stmt : node.elseBody) stmt->accept(*this);
                impl_->emitScopeCleanup();
                impl_->popScope();
                if (!impl_->builder->GetInsertBlock()->getTerminator())
                    impl_->builder->CreateBr(endBB);
            }

            // End
            impl_->builder->SetInsertPoint(endBB);
            return;
        }
    }

    // --- For-in on collection (list, string, or dict) ---
    // D039: `for x in <Any>` - a box-typed iterable. The payload may hold
    // EITHER list representation (the monomorphized DragonList family or a
    // DragonListBox), so the loop must not assume a layout: dragon_box_len
    // sizes it (raising the Python-shaped TypeError for non-sized values) and
    // dragon_box_subscript header-dispatches every element read, returning an
    // OWNED box. The loop var is itself an Any box - narrow it in the body
    // with isinstance. Previously an Any iterable fell into the native-list
    // default and walked the payload at the 8-byte stride: a list[str]
    // yielded raw pointers tagged int, a list[Any] yielded interleaved
    // tag/payload words.
    {
        bool iterMayBeBox = false;
        if (node.iterable->type &&
            (node.iterable->type->kind() == Type::Kind::Any ||
             node.iterable->type->kind() == Type::Kind::Union))
            iterMayBeBox = true;
        if (auto* nm = dynamic_cast<NameExpr*>(node.iterable.get())) {
            // A name's VarKind is the codegen truth: a Union-kind slot IS a
            // box; a name rebound by isinstance narrowing to a concrete kind
            // is NOT, even though its static type may still read Any.
            iterMayBeBox = impl_->lookupVarKind(nm->name) == Impl::VarKind::Union;
        }
        // A list[Any] iterable stores 16-byte boxes (DragonListBox). The
        // native-list loop walks elements at the 8-byte stride, which reads
        // interleaved tag/payload words - the exact failure this box path
        // exists to prevent for Any-typed iterables - so route every
        // Any-element list here as well. The coercion below wraps the raw
        // list pointer as a list-tagged box and dragon_box_subscript
        // dispatches either list representation. Element-kind sources, in
        // order: the checker's static type, a local's tracked elem kind, a
        // field's registered elem kind.
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
            // D025: a list[type] element is a class descriptor, not a value.
            // Its elem-kind entry reads Any (no Type::Kind counterpart), but
            // it must stay on the native path so constructing through the
            // loop var still dies with the "classes are not values" error.
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
                auto cit = impl_->classFieldListElemKinds.find(ownerClass);
                if (cit != impl_->classFieldListElemKinds.end()) {
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
            // Non-box lowerings of an Any-typed expression are not expected
            // (the type says box); coerce defensively rather than re-evaluate
            // the iterable through a different path. A scalar coerces to an
            // int box, and dragon_box_len then raises the honest TypeError.
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
            // The element box is OWNED (+1 on heap payloads): release the
            // previous iteration's element before overwriting (the slot
            // starts zeroed, so the first decref no-ops).
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
            // Release the LAST element (break and natural exhaustion both
            // land here), neutralize the slot so any later cleanup can't
            // double-free, and drop the iterable's own +1 if its expression
            // produced an owned box temporary.
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

    // Determine iterable type, including dict iteration patterns
    bool isStrIterable = false;
    bool isListIterable = false;
    bool isDictItemsIterable = false;  // for k, v in d.items()
    bool isDictKeysIterable = false;   // for k in d.keys() OR for k in d

    // Helper: resolve owner class name for an AttributeExpr's object.
    // Handles `self.<field>` (uses currentClassName), `obj.<field>`
    // (looks up varClassNames), and nested bases (`self.inner.<field>`,
    // `a.b.<field>`) via resolveExprClassName - the same resolution
    // resolveDictKeyKind uses, so the iteration shape and the key kind
    // are always read off the same owning class. Returns empty string if
    // unresolved.
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

    // Helper: look up the VarKind of a class field. Returns Other if unknown.
    auto fieldVarKind = [&](const std::string& cls, const std::string& field) -> Impl::VarKind {
        auto cit = impl_->classFieldKinds.find(cls);
        if (cit == impl_->classFieldKinds.end()) return Impl::VarKind::Other;
        auto fit = cit->second.find(field);
        if (fit == cit->second.end()) return Impl::VarKind::Other;
        return fit->second;
    };

    // Check for dict method calls: d.items(), d.keys(), d.values()
    // Recognises both `localDict.method()` and `self.field.method()` /
    // `obj.field.method()` so class-field dicts route to the typed runtime ops
    // (D030 alignment - the dispatch must consult the tracked field type
    // instead of falling through to the legacy list default).
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
    // If not a dict method call, check variable types
    if (!isDictItemsIterable && !isDictKeysIterable && !isListIterable) {
        if (auto* iterName = dynamic_cast<NameExpr*>(node.iterable.get())) {
            auto kind = impl_->lookupVarKind(iterName->name);
            if (kind == Impl::VarKind::Str || kind == Impl::VarKind::StrLiteral) isStrIterable = true;
            else if (kind == Impl::VarKind::Dict) isDictKeysIterable = true;
            else if (kind == Impl::VarKind::List) isListIterable = true;
            else isListIterable = true; // default to list for unknown
        } else if (auto* iterAttr = dynamic_cast<AttributeExpr*>(node.iterable.get())) {
            // `for x in self.<field>` / `for x in obj.<field>` - consult the
            // tracked class-field VarKind so dict-typed and str-typed fields
            // route to the correct runtime path. Without this, dict fields
            // fall through to the list default and the loop variable gets
            // mis-allocated as i64, crashing dragon_dict_get(ptr, ptr).
            std::string owner = resolveOwnerClass(iterAttr);
            Impl::VarKind kind = owner.empty()
                ? Impl::VarKind::Other
                : fieldVarKind(owner, iterAttr->attribute);
            if (kind == Impl::VarKind::Str || kind == Impl::VarKind::StrLiteral) isStrIterable = true;
            else if (kind == Impl::VarKind::Dict) isDictKeysIterable = true;
            else if (kind == Impl::VarKind::List) isListIterable = true;
            else if (node.iterable->type) {
                // Field kind untracked (e.g. the owner class was declared in
                // another module): the stamped static type still knows the
                // container shape. Without this fallback a dict field reached
                // through an unresolved owner iterated as a LIST and the loop
                // read the dict's pages as list slots (heap overflow).
                switch (node.iterable->type->kind()) {
                    case Type::Kind::Dict: isDictKeysIterable = true; break;
                    case Type::Kind::Str:  isStrIterable = true; break;
                    default:               isListIterable = true; break;
                }
            }
            else isListIterable = true; // unknown - list is the legacy default
        } else if (dynamic_cast<StringLiteral*>(node.iterable.get())) {
            isStrIterable = true;
        } else if (node.iterable->type) {
            // General-expression iterable (e.g. `for k in rows[0]`, a subscript,
            // or any call/index whose result is a container). VarKind tracking
            // only covers simple names and class fields, so fall back to the
            // expression's inferred static type to choose the iteration shape.
            // Without this a dict-valued expression defaulted to list iteration
            // and the loop walked the dict's bytes as i64 keys.
            switch (node.iterable->type->kind()) {
                case Type::Kind::Dict: isDictKeysIterable = true; break;
                case Type::Kind::Str:  isStrIterable = true; break;
                default:               isListIterable = true; break;  // list/set/tuple/bytes/unknown
            }
        } else {
            isListIterable = true; // default to list
        }
    }

    // Str-keyed dicts OWN their keys (heap DragonStrings), so a key loop var
    // must be a heap kind (for escape-incref) but borrowed (no per-iter decref).
    // Int-keyed dicts have i64 keys that must NEVER be treated as strings - keep
    // them on the legacy StrLiteral binding (no escape-incref). Resolve the dict
    // expr behind `for k in d` (the iterable itself) or `for k in d.keys()` /
    // `d.items()` (the method receiver).
    bool dictKeysAreInt = false;
    if (isDictKeysIterable || isDictItemsIterable) {
        Expr* dictExpr = node.iterable.get();
        if (auto* iterCall = dynamic_cast<CallExpr*>(dictExpr))
            if (auto* attr = dynamic_cast<AttributeExpr*>(iterCall->callee.get()))
                dictExpr = attr->object.get();
        dictKeysAreInt = impl_->dictKeyIsInt(dictExpr);
    }

    // Evaluate iterable, converting dicts to lists as needed
    llvm::Value* iterableVal;
    if (isDictKeysIterable) {
        // for k in d OR for k in d.keys() - get keys list from dict
        if (auto* iterCall = dynamic_cast<CallExpr*>(node.iterable.get())) {
            // d.keys() - evaluate the call (returns DragonList*)
            node.iterable->accept(*this);
            iterableVal = impl_->lastValue;
        } else {
            // for k in d - evaluate dict, then call dragon_dict_keys
            node.iterable->accept(*this);
            llvm::Value* dictVal = impl_->lastValue;
            iterableVal = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_keys"], {dictVal}, "dictkeys");
            // An OWNED dict temp (a `dub d` snapshot, a call result): the
            // keys list is independent (fresh list, retained keys), so the
            // temp is fully consumed here - release it or it leaks per loop.
            Impl::VarKind rd =
                impl_->ownedTempDrainKind(node.iterable.get(), dictVal);
            if (rd != Impl::VarKind::Other)
                impl_->emitDecrefByKind(dictVal, rd);
        }
    } else if (isDictItemsIterable) {
        // d.items() - evaluate the call (returns DragonList* of DragonTuple*)
        node.iterable->accept(*this);
        iterableVal = impl_->lastValue;
    } else {
        node.iterable->accept(*this);
        iterableVal = impl_->lastValue;
    }
    // Unique scope name per loop: if two container-iterating for-loops in one
    // scope both register the name "__iter", the second's
    // setVar clobbers the first in the scope map and cleanup leaks all but
    // the last iterable temp (a keys()/items()/comprehension list each).
    std::string iterName = "__iter." + std::to_string(impl_->forIterCounter++);
    auto* iterAlloca = impl_->createEntryAlloca(func, iterName, impl_->i8PtrType);
    impl_->builder->CreateStore(iterableVal, iterAlloca);
    // GC: register __iter so scope cleanup handles it on all exit paths
    // (normal exit, break, return). Dict/items paths create a new DragonList*
    // that must be decref'd. Regular list paths are borrowed (marked below).
    //
    // An OWNED iterable temp - a comprehension, map()/filter() (which desugar
    // to comprehensions), list()/sorted() results, a function returning a
    // fresh container, or an inline literal - is a +1 nobody else holds, so
    // iterating it without registering cleanup leaks the whole container each
    // pass (`for x in [c for c in xs]` / `for x in filter(f, xs)`). A borrowed
    // iterable (a NameExpr/AttributeExpr/element read) keeps its owner's
    // reference and must NOT be decref'd here. Gate on heap-container static
    // type so VarKind::List cleanup's dragon_decref is the right drop (str /
    // bytes use a different decref and stay borrowed - their temps are a
    // separate, smaller concern).
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

    // Create index variable __i
    auto* idxVar = impl_->createEntryAlloca(func, "__i", impl_->i64Type);
    impl_->builder->CreateStore(llvm::ConstantInt::get(impl_->i64Type, 0), idxVar);

    auto* condBB = llvm::BasicBlock::Create(*impl_->context, "forcond", func);
    auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, "forbody", func);
    auto* incBB = llvm::BasicBlock::Create(*impl_->context, "forinc", func);
    auto* endBB = llvm::BasicBlock::Create(*impl_->context, "forend", func);
    // `for ... else`: else runs on natural exhaustion (index reaches len),
    // skipped on `break` (which targets endBB). Absent else -> elseBB == endBB
    // (unchanged flow).
    llvm::BasicBlock* elseBB = node.elseBody.empty()
        ? endBB
        : llvm::BasicBlock::Create(*impl_->context, "forelse", func);

    impl_->loopStack.push({endBB, incBB, impl_->scopes.size(), impl_->tryFrameFuncs.size(), impl_->exitCleanupStack.size()});
    impl_->builder->CreateBr(condBB);

    // Condition: __i < len(iterable) - false exhausts the loop and runs else.
    // All dict paths produce a DragonList*, so use dragon_list_len
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

    // Body: get element, assign to target variable
    impl_->builder->SetInsertPoint(bodyBB);
    impl_->pushScope();

    currentIdx = impl_->builder->CreateLoad(impl_->i64Type, idxVar, "__i");
    iterLoaded = impl_->builder->CreateLoad(impl_->i8PtrType, iterAlloca, "__iter");

    if (tupleTarget && isDictItemsIterable) {
        // Dict items unpacking: for k, v in d.items()
        // Each list element is a DragonTuple* (stored as i64) with (key, value).
        // Track the dict's value kind so v gets the right VarKind for
        // print / dispatch / comparison.
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
                        else if (k == Type::Kind::Bytes) valVarKind = Impl::VarKind::List;  // D030 §5: bytes/list share generic-heap dispatch
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
                    if (dictKeysAreInt) {
                        // int-keyed dict: the items() tuple stores the key as a
                        // native i64 (untagged). Bind as VarKind::Int in an i64
                        // slot - IntToPtr'ing it into a ptr slot made `print(k)`
                        // deref the key value as a string pointer (-> SIGSEGV).
                        auto* alloca = impl_->createEntryAlloca(func, name->name, impl_->i64Type);
                        impl_->builder->CreateStore(val, alloca);
                        impl_->setVar(name->name, alloca, Impl::VarKind::Int);
                    } else {
                        // Key is an owned heap DragonString (the items() tuple
                        // holds a ref to it). Bind as VarKind::Str + BORROWED: no
                        // per-iteration decref of a key the tuple/dict still owns,
                        // but the heap kind lets escape paths (return/store the
                        // key) incref it. dragon_incref_str is a no-op for any
                        // bare-C-string key.
                        llvm::Value* strPtr = impl_->builder->CreateIntToPtr(val, impl_->i8PtrType, "keystr");
                        auto* alloca = impl_->createEntryAlloca(func, name->name, impl_->i8PtrType);
                        impl_->builder->CreateStore(strPtr, alloca);
                        impl_->setVar(name->name, alloca, Impl::VarKind::Str);
                        impl_->scopes.back().borrowed.insert(name->name);
                    }
                } else {
                    // D030 Phase 3.F: bind value slot at its native type so
                    // f64 / ptr values don't funnel through i64 in the body.
                    // Tuple slot returns i64; convert once to the native type.
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
                    // The items() tuple co-owns the value (dragon_dict_items
                    // increfs it) - the binding is a BORROW, exactly like the
                    // key above. Without this mark, per-iteration cleanup
                    // decref'd a heap value the tuple still holds, and the
                    // tuple destroy + dict destroy double-freed it.
                    if (Impl::isHeapKind(valVarKind))
                        impl_->scopes.back().borrowed.insert(name->name);
                    impl_->builder->CreateStore(slotVal, alloca);
                }
            }
        }
    } else if (tupleTarget) {
        // Generic tuple unpacking: for a, b in [(1,2), (3,4)] - plus the
        // element-typed forms enumerate(X) / zip(A,B). The tuple co-owns each
        // element by tag at runtime, but the unpack vars need the right static
        // VarKind + native slot type, else a str/float element funnels through
        // i64 (which made `for i, v in enumerate(list[str])` print pointers).
        std::vector<Type::Kind> posKinds(tupleTarget->elements.size(), Type::Kind::Int);
        // Plain `for a, b, ... in <list[tuple[T1,T2,...]]>`: pull each tuple
        // component's checked static type so the unpack vars get their real
        // native slot (str/ptr/float/...), instead of defaulting to i64 - which
        // made a str/ptr element funnel through i64 and print as a raw pointer
        // (and mis-type a dict key built from it). Best-effort: falls through to
        // the Int default + enumerate/zip handling below when the type is
        // unavailable. Placed first so the enumerate/zip block keeps precedence.
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
                    posKinds[1] = srcElemKind(itCall->args[0].get());  // index stays int
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
                // Coerce the i64 slot to the element's native type (mirrors the
                // dict-items unpack above).
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
                // dragon_tuple_get returns a BORROW - the tuple co-owns the
                // element (e.g. d.items() tuples incref'd their key/value).
                // Without the borrowed mark, per-iteration scope cleanup
                // decref'd a heap element the tuple still holds, so the later
                // tuple destroy double-freed it (`for k, v in d.items()` UAF).
                if (Impl::isHeapKind(vk))
                    impl_->scopes.back().borrowed.insert(name->name);
                impl_->builder->CreateStore(slotVal, alloca);
            }
        }
    } else if (isDictKeysIterable) {
        // Iterating over dict keys. dragon_dict_keys returns the keys list as
        // i64 elements: str-keyed -> DragonString pointer-as-i64; int-keyed ->
        // the raw i64 key. The two need different loop-var bindings.
        llvm::Value* elem = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_list_get"], {iterLoaded, currentIdx}, "elem");
        if (dictKeysAreInt) {
            // int-keyed dict: the key is a native i64 - bind it as VarKind::Int
            // in an i64 slot. The old path IntToPtr'd it into a ptr slot and
            // bound StrLiteral, so `print(k)` dereferenced the key value as a
            // string pointer (key 1 -> address 0x1 -> SIGSEGV).
            auto* targetAlloca = impl_->createEntryAlloca(func, targetName->name, impl_->i64Type);
            impl_->builder->CreateStore(elem, targetAlloca);
            impl_->setVar(targetName->name, targetAlloca, Impl::VarKind::Int);
        } else {
            // str-keyed dict: keys are owned heap DragonStrings (the dict holds
            // one ref per key). Bind the loop var as VarKind::Str (heap) but mark
            // it BORROWED so per-iteration scope cleanup does NOT decref a key the
            // dict still owns - while the heap kind still lets the
            // return/append/assign paths incref it when it ESCAPES the loop (e.g.
            // `for k in d: return k`). Without that escape-incref the returned key
            // would dangle once the dict frees its keys on destroy.
            // dragon_incref_str is a no-op for any non-heap literal key, so this
            // is safe even if a key is a bare C string.
            llvm::Value* strPtr = impl_->builder->CreateIntToPtr(elem, impl_->i8PtrType, "keystr");
            auto* targetAlloca = impl_->createEntryAlloca(func, targetName->name, impl_->i8PtrType);
            impl_->builder->CreateStore(strPtr, targetAlloca);
            impl_->setVar(targetName->name, targetAlloca, Impl::VarKind::Str);
            impl_->scopes.back().borrowed.insert(targetName->name);
        }
    } else if (isStrIterable) {
        // dragon_str_index returns const char*
        llvm::Value* elem = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_str_index"], {iterLoaded, currentIdx}, "ch");
        auto* targetAlloca = impl_->createEntryAlloca(func, targetName->name, impl_->i8PtrType);
        impl_->builder->CreateStore(elem, targetAlloca);
        impl_->setVar(targetName->name, targetAlloca, Impl::VarKind::Str);
    } else {
        // D030 §5: The loop-var alloca shape and the typed runtime-op
        // selection both come directly from the iterable's element
        // `Type::Kind`. The legacy `Type::Kind` -> `VarKind` fan-out switch
        // is collapsed to one boundary call (`typeKindToVarKind`) used
        // only where setVar / refcount tracking still consume VarKind -
        // they're the bookkeeping shapes that LLVM doesn't represent.
        Type::Kind elemTypeKind = Type::Kind::Int;
        if (auto* iterName = dynamic_cast<NameExpr*>(node.iterable.get())) {
            auto it = impl_->varListElemKinds.find(iterName->name);
            if (it != impl_->varListElemKinds.end()) elemTypeKind = it->second;
        } else if (auto* iterAttr = dynamic_cast<AttributeExpr*>(node.iterable.get())) {
            // self.field - look up element type from classFieldListElemKinds
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
                auto cit = impl_->classFieldListElemKinds.find(className);
                if (cit != impl_->classFieldListElemKinds.end()) {
                    auto fit = cit->second.find(iterAttr->attribute);
                    if (fit != cit->second.end()) elemTypeKind = fit->second;
                }
            }
        } else if (auto* iterCall = dynamic_cast<CallExpr*>(node.iterable.get())) {
            // D030 Phase 3.F: `for v in d.values()` - element kind is the
            // dict's tracked V kind. Without this the loop var defaults to
            // Int and f64 / ptr values get bashed through i64.
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

        // Class-instance heuristics - `list[Foo]` whose element kind tables
        // weren't populated with `Type::Kind::Instance` (entries created
        // before TypeChecker propagation). Fall back to the parallel
        // class-name maps so the alloca still gets sized as `ptr`.
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
                    auto cit = impl_->classFieldListElemClassName.find(ownerClass);
                    if (cit != impl_->classFieldListElemClassName.end()) {
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

        // Typechecker-propagated iterable type fallback - catches a list[T]
        // produced directly by an expression with no var/field entry, e.g.
        // `for part in s.split(";")` (method call -> list[str]) or a function
        // returning list[Foo]. Generalised from Instance-only to every concrete
        // element kind: without it the element defaulted to Int and a str/float/
        // ptr element was bashed through i64 (a `.strip()` on it then passed an
        // i64 where a ptr was required - LLVM verify failure).
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

        // Boundary mapping - setVar / refcount tracking still consume
        // VarKind. D025: `list[type]` iteration yields a class descriptor,
        // which is the one VarKind without a Type::Kind counterpart, so
        // it's an explicit one-line override after the boundary translation.
        Impl::VarKind elemVarKind = Impl::typeKindToVarKind(elemTypeKind);
        if (auto* iterName = dynamic_cast<NameExpr*>(node.iterable.get())) {
            if (impl_->varListElemIsType.count(iterName->name))
                elemVarKind = Impl::VarKind::Type;
        }

        // Resolve element class name for ClassInstance elements (for field access)
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
                    auto cit = impl_->classFieldListElemClassName.find(ownerClass);
                    if (cit != impl_->classFieldListElemClassName.end()) {
                        auto fit = cit->second.find(iterAttr->attribute);
                        if (fit != cit->second.end()) elemClassName = fit->second;
                    }
                }
            }
            // Fallback: pull the class name from the typechecker's InstanceType
            // when the heuristics above didn't catch the iterable shape.
            if (elemClassName.empty() && node.iterable->type) {
                if (auto* lt = dynamic_cast<ListType*>(node.iterable->type.get())) {
                    if (auto* it2 = dynamic_cast<InstanceType*>(lt->elementType.get())) {
                        if (it2->classType) elemClassName = it2->classType->name;
                    }
                }
            }
        }

        // D030 §5: bind via the Type::Kind-driven helper which picks the
        // typed runtime get and sizes the alloca from the static element
        // type itself. The list was allocated by visit(ListExpr) using the
        // matching variant (DragonListF64 / DragonListPtr / DragonList)
        // so the typed get returns the native LLVM type.
        // D025 escape: list[type] still yields a descriptor i64 - the one
        // shape that doesn't fall out of Type::Kind directly, so reuse the
        // VarKind binder for that single case.
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
        // C7: a `list[TypedDict]` loop variable binds as VarKind::Dict (a
        // TypedDict is dict-backed), but the field-tag dispatch in
        // visit(AttributeExpr) needs the TypedDict *schema* to emit a typed
        // dragon_dict_get_* (so `pl.name` yields a real str ptr, not a generic
        // i64). The Instance branch above records varClassNames for instances;
        // do the analogous wiring for the dict-backed TypedDict case so
        // str()/f-string of a field works (not just print, which masks it via
        // a runtime tag dispatch).
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
            if (!tdCls.empty() && impl_->typedDictClasses.count(tdCls))
                impl_->varTypedDictClass[targetName->name] = tdCls;
        }
        // Loop variable is a borrowed reference from the typed get (no
        // incref on extraction). Heap classification flows from the static
        // type - D030 §5: "the LLVM type IS the truth."
        if (Impl::isHeapTypeKind(elemTypeKind))
            impl_->scopes.back().borrowed.insert(targetName->name);

        // list[Callable[[...], R]]: register the loop var's callable signature
        // so f(args) lowers as a typed indirect call instead of erroring.
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
                auto cit = impl_->classFieldListElemCallableType.find(ownerClass);
                if (cit != impl_->classFieldListElemCallableType.end()) {
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

    // Increment: __i++
    impl_->builder->SetInsertPoint(incBB);
    currentIdx = impl_->builder->CreateLoad(impl_->i64Type, idxVar, "__i");
    llvm::Value* nextIdx = impl_->builder->CreateAdd(
        currentIdx, llvm::ConstantInt::get(impl_->i64Type, 1), "inc");
    impl_->builder->CreateStore(nextIdx, idxVar);
    impl_->builder->CreateBr(condBB);

    impl_->loopStack.pop();

    // Else body - only on natural exhaustion (index reached len); falls
    // through to endBB. `break` bypasses this by targeting endBB directly.
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

    // GC: dict iteration temp list cleanup handled by scope cleanup
    // via setVar("__iter", ..., VarKind::List) registered above.
}

} // namespace dragon
