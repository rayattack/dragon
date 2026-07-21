/// Dragon CodeGen - Concurrency (fire, await, yield, thread)
#include "../CodeGenImpl.h"

namespace dragon {

void CodeGen::visit(FireExpr& node) {
    // D030: fire spawns a green thread via a per-callsite trampoline that
    // knows the target's exact native signature - no i64 funneling, no
    // generic Fn0..Fn8 dispatch in the runtime.
    impl_->needsPthread = true;

    // Resolve the target function. Two shapes:
    //  (a) fire { block } -> synthesize an internal nullary fn, spawn it
    //  (b) fire fn(args) -> resolve callee, evaluate user args
    llvm::Function* targetFn = nullptr;
    std::vector<llvm::Value*> userArgs;
    std::vector<Impl::VarKind> argKinds;
    std::string siteName;

    if (!node.bodyStmts.empty()) {
        // Block form `fire { ... }`. The body captures enclosing locals (Sema
        // populated node.capturedVars); we lower the block to a function that
        // takes those captures AS PARAMETERS and marshal them through the SAME
        // spawn path as `fire fn(args)` - so the captured values are heap-copied
        // into the vthread's args buffer with proper atomic-incref / trampoline-
        // decref refcounting (no cross-function alloca reference, no data race).

        // Reassigning an enclosing local from the vthread needs a thread-safe
        // cell, which doesn't exist yet - that would be a data race. Reject it
        // explicitly rather than silently miscompile.
        if (!node.mutatedCapturedVars.empty()) {
            impl_->addError(
                "fire { ... } cannot reassign an enclosing variable '" +
                node.mutatedCapturedVars.front() +
                "' (capture-by-reference across threads is not supported yet); "
                "compute the value first and capture it read-only, or use a "
                "function call (`fire f(...)`)",
                node.location());
            impl_->lastValue = llvm::ConstantPointerNull::get(
                llvm::PointerType::getUnqual(*impl_->context));
            return;
        }

        // Snapshot each capture's value + kind in the ENCLOSING frame (before we
        // switch into the fire fn). A name that doesn't resolve to a local
        // (e.g. a module global) is not a frame capture - skip it.
        struct Cap { std::string name; llvm::Value* val; Impl::VarKind kind; llvm::Type* ty; };
        std::vector<Cap> caps;
        for (const auto& capName : node.capturedVars) {
            auto* a = impl_->lookupVar(capName);
            if (!a) continue;
            Cap c;
            c.name = capName;
            c.ty = a->getAllocatedType();
            c.val = impl_->builder->CreateLoad(c.ty, a, capName + ".cap");
            c.kind = impl_->lookupVarKind(capName);
            caps.push_back(std::move(c));
        }

        std::string fireFnName =
            "__dragon_fire_block_" + std::to_string(impl_->lambdaCounter++);
        std::vector<llvm::Type*> capParamTypes;
        for (auto& c : caps) capParamTypes.push_back(c.ty);
        auto* funcType = llvm::FunctionType::get(impl_->i64Type, capParamTypes, false);
        auto* fireFn = llvm::Function::Create(
            funcType, llvm::Function::InternalLinkage, fireFnName,
            impl_->module.get());

        auto* prevFunc = impl_->currentFunction;
        auto* prevBlock = impl_->builder->GetInsertBlock();
        // SAVE + CLEAR the scope chain so capture names resolve to the fire fn's
        // OWN param allocas, not the parent frame's (cross-function reference).
        auto savedScopes = std::move(impl_->scopes);
        impl_->scopes.clear();
        impl_->currentFunction = fireFn;
        auto* entry = llvm::BasicBlock::Create(*impl_->context, "entry", fireFn);
        impl_->builder->SetInsertPoint(entry);
        impl_->pushScope();

        // Materialize each capture as a local param. Heap captures are BORROWED
        // (the spawn site atomic-increfs and the trampoline atomic-decrefs them,
        // so the body must NOT decref at scope cleanup - avoids a double-free).
        unsigned ai = 0;
        for (auto& arg : fireFn->args()) {
            Cap& c = caps[ai++];
            arg.setName(c.name);
            auto* a = impl_->createEntryAlloca(fireFn, c.name, c.ty);
            impl_->builder->CreateStore(&arg, a);
            impl_->setVar(c.name, a, c.kind);
            if (Impl::isHeapKind(c.kind))
                impl_->scopes.back().borrowed.insert(c.name);
        }

        for (auto& stmt : node.bodyStmts) stmt->accept(*this);
        if (!impl_->builder->GetInsertBlock()->getTerminator()) {
            impl_->emitScopeCleanup();
            impl_->builder->CreateRet(llvm::ConstantInt::get(impl_->i64Type, 0));
        }
        impl_->popScope();
        impl_->scopes = std::move(savedScopes);

        impl_->currentFunction = prevFunc;
        if (prevBlock) impl_->builder->SetInsertPoint(prevBlock);

        targetFn = fireFn;
        siteName = fireFnName;

        // Marshal captures as the spawn args (mirrors the call form). Atomic-
        // incref heap captures so they survive on the worker until the
        // trampoline atomic-decrefs them post-call.
        for (auto& c : caps) { userArgs.push_back(c.val); argKinds.push_back(c.kind); }
        for (size_t i = 0; i < userArgs.size() && i < argKinds.size(); i++)
            impl_->emitAtomicIncref(userArgs[i], argKinds[i]);
    } else {
        auto* callExpr = dynamic_cast<CallExpr*>(node.operand.get());
        if (!callExpr) {
            impl_->addError("fire requires a function call or { block }",
                            node.location());
            impl_->lastValue = llvm::ConstantPointerNull::get(
                llvm::PointerType::getUnqual(*impl_->context));
            return;
        }

        bool isMethodCall = false;
        llvm::Value* selfVal = nullptr;
        std::string calleeName;
        if (auto* nameExpr = dynamic_cast<NameExpr*>(callExpr->callee.get())) {
            targetFn = impl_->module->getFunction(nameExpr->name);
            calleeName = nameExpr->name;
        } else if (auto* attrExpr =
                   dynamic_cast<AttributeExpr*>(callExpr->callee.get())) {
            // Resolve (owningModule, className) the same way CallMethods.cpp
            // does for instance method dispatch, so `fire self.method(...)`
            // and `fire obj.method(...)` reach the module-mangled symbol
            // (`<mod>__Class_method`) instead of the bare className that
            // pre-mangling codegen used to emit.
            std::string className;
            std::string owningModule;
            if (auto* objName =
                dynamic_cast<NameExpr*>(attrExpr->object.get())) {
                if (objName->name == "self" &&
                    !impl_->currentClassName.empty()) {
                    className = impl_->currentClassName;
                    owningModule = impl_->currentModuleName;
                } else {
                    auto vit = impl_->varClassNames.find(objName->name);
                    if (vit != impl_->varClassNames.end())
                        className = vit->second;
                    auto vmIt = impl_->varClassOwningModule.find(objName->name);
                    if (vmIt != impl_->varClassOwningModule.end()) {
                        owningModule = vmIt->second;
                    } else if (!className.empty()) {
                        auto cmIt = impl_->classOwningModule.find(className);
                        if (cmIt != impl_->classOwningModule.end())
                            owningModule = cmIt->second;
                    }
                }
            }
            if (!className.empty()) {
                std::string methodFuncName;
                targetFn = impl_->resolveMethodFunction(
                    owningModule, className, attrExpr->attribute,
                    &methodFuncName);
                if (targetFn) {
                    calleeName = methodFuncName;
                    isMethodCall = true;
                    attrExpr->object->accept(*this);
                    selfVal = impl_->lastValue;
                }
            }
        }

        if (!targetFn) {
            impl_->addError("fire: cannot resolve function", node.location());
            impl_->lastValue = llvm::ConstantPointerNull::get(
                llvm::PointerType::getUnqual(*impl_->context));
            return;
        }

        if (isMethodCall) userArgs.push_back(selfVal);
        for (auto& arg : callExpr->args) {
            arg->accept(*this);
            userArgs.push_back(impl_->lastValue);
        }

        auto kindsIt = impl_->funcParamKinds.find(calleeName);
        if (kindsIt != impl_->funcParamKinds.end()) argKinds = kindsIt->second;

        // docs/002 2.8/2.9: `fire consume(own o)` - the caller's +1 MOVES
        // with the value, and the callee's own-param scope exit releases it.
        // Neutralize the kind so BOTH halves of the borrow pair skip it (the
        // fire-site atomic incref here, the trampoline's post-call decref in
        // buildFireTrampoline) - keeping the pair double-freed the moved
        // object (A/B-proven by the spawn-lend probe).
        for (size_t i = 0; i < userArgs.size() && i < argKinds.size(); i++)
            if (impl_->paramIsOwn(calleeName, (unsigned)i))
                argKinds[i] = Impl::VarKind::Other;

        // Atomic-incref heap args at the call site so they survive on the
        // worker thread until the trampoline atomic-decrefs them post-call.
        for (size_t i = 0; i < userArgs.size() && i < argKinds.size(); i++)
            impl_->emitAtomicIncref(userArgs[i], argKinds[i]);

        siteName = calleeName + "_" + std::to_string(impl_->lambdaCounter++);
    }

    // Build the per-callsite typed args struct from the target's exact param
    // types. We use the function's LLVM signature as the source of truth.
    auto* targetTy = targetFn->getFunctionType();
    std::vector<llvm::Type*> argTypes;
    for (unsigned i = 0; i < targetTy->getNumParams(); i++)
        argTypes.push_back(targetTy->getParamType(i));
    auto* argsStructType =
        impl_->makeSpawnArgsStructType(argTypes, "fire.args." + siteName);

    // Build the trampoline (off the current insert point).
    auto* tramp = impl_->buildFireTrampoline(
        targetFn, argsStructType, argKinds, siteName);

    // Coerce user args to match the target's param types (e.g. i1 -> i1, i64 -> i64).
    for (size_t i = 0; i < userArgs.size() && i < argTypes.size(); i++)
        userArgs[i] = impl_->coerceArg(userArgs[i], argTypes[i]);

    // Allocate args struct on the caller's stack and populate.
    auto* func = impl_->builder->GetInsertBlock()->getParent();
    auto* argsAlloca = impl_->createEntryAlloca(func, "fire.args", argsStructType);
    impl_->populateSpawnArgs(argsAlloca, argsStructType, userArgs);

    // sizeof(args struct) for runtime memcpy.
    const auto& dl = impl_->module->getDataLayout();
    uint64_t argsSize = dl.getTypeAllocSize(argsStructType);

    auto* argsAsI8 = impl_->builder->CreateBitCast(argsAlloca, impl_->i8PtrType);
    auto* trampAsI8 = impl_->builder->CreateBitCast(tramp, impl_->i8PtrType);

    auto* spawn = impl_->runtimeFuncs["dragon_vthread_spawn_typed"];
    impl_->lastValue = impl_->builder->CreateCall(
        spawn,
        {trampAsI8, argsAsI8,
         llvm::ConstantInt::get(impl_->i64Type, (int64_t)argsSize)},
        "vthread");
    // `fire f(own x)`: the args struct now carries the moved +1 - null the
    // caller's slot so its scope exit sees nothing (docs/002 2.8).
    if (auto* movedCall = dynamic_cast<CallExpr*>(node.operand.get()))
        impl_->emitMoveOutSlots(*movedCall);
}

void CodeGen::visit(AwaitExpr& node) {
    // await expr -> evaluate expr (Task handle), join vthread, return result
    impl_->needsPthread = true;
    node.operand->accept(*this);
    llvm::Value* handle = impl_->lastValue;

    // Ensure the handle is a pointer (Task = ptr to DragonVThread)
    if (!handle->getType()->isPointerTy()) {
        handle = impl_->builder->CreateIntToPtr(handle, impl_->i8PtrType);
    }

    // Join the vthread and get the result (i64), then reinterpret it at the
    // task's native result type T (D030): bitcast for float, inttoptr for
    // str/list/instance, truncate for bool. node.type is T (await Task[T] -> T).
    auto* rawResult = impl_->builder->CreateCall(
        impl_->runtimeFuncs["dragon_vthread_join"], {handle}, "await.result");
    impl_->lastValue = impl_->taskResultFromI64(rawResult, node.type.get());

    // `await t` CONSUMES the task: the runtime just freed the vthread. Blank the
    // binding's slot (the `free(p); p = NULL` discipline) so (1) the scope-exit
    // detach sees an empty slot and does NOT double-free the consumed handle, and
    // (2) a later `t.is_alive()` / `await t` reads NULL (-> false / 0) instead of
    // the freed struct - closing the latent use-after-await UAF. Only a bound
    // NameExpr has a slot; `await fire f()` / `await get()` are owned temps the
    // join already reclaimed, with no slot to blank. Task-detach tail.
    if (impl_->options.gcMode == GCMode::RC) {
        if (auto* nm = dynamic_cast<NameExpr*>(node.operand.get())) {
            if (auto* slot = impl_->lookupVar(nm->name)) {
                impl_->builder->CreateStore(
                    llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(impl_->i8PtrType)),
                    slot);
            }
        }
    }
}
void CodeGen::visit(YieldExpr& node) {
    if (!impl_->generatorPtr) {
        // yield outside a generator function - shouldn't happen, but emit 0 as fallback
        impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
        return;
    }

    // Evaluate the yielded value (default to 0/None if no value)
    llvm::Value* yieldVal;
    if (node.value) {
        node.value->accept(*this);
        yieldVal = impl_->lastValue;
        // Coerce to i64 if needed
        if (yieldVal->getType()->isPointerTy()) {
            yieldVal = impl_->builder->CreatePtrToInt(yieldVal, impl_->i64Type);
        } else if (yieldVal->getType()->isDoubleTy()) {
            yieldVal = impl_->builder->CreateBitCast(yieldVal, impl_->i64Type);
        } else if (yieldVal->getType() == impl_->i1Type) {
            yieldVal = impl_->builder->CreateZExt(yieldVal, impl_->i64Type);
        }
    } else {
        yieldVal = llvm::ConstantInt::get(impl_->i64Type, 0);
    }

    // Load the generator pointer and call dragon_generator_yield
    auto* genPtr = impl_->builder->CreateLoad(impl_->i8PtrType, impl_->generatorPtr, "__gen.ptr");
    impl_->builder->CreateCall(
        impl_->runtimeFuncs["dragon_generator_yield"], {genPtr, yieldVal});

    // The yield expression itself evaluates to 0 (sent value not yet supported)
    impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
}
void CodeGen::visit(ThreadStmt& node) {
    // thread { block } - scoped OS thread with auto-join at scope exit.
    // The body captures enclosing locals (Sema populated node.capturedVars); we
    // lower it to a function taking those captures as i64 params (the
    // dragon_thread_fire/dragon_thread_entry ABI passes up to 8 i64 args) and
    // marshal them as the thread args - so the body never references the parent
    // frame's allocas (which would be invalid cross-function IR). The thread is
    // joined synchronously below, so captures stay alive for its lifetime and
    // are BORROWED (no incref/decref): the parent owns the single reference.
    impl_->needsPthread = true;

    // Cross-thread reassignment of an enclosing local needs a thread-safe cell
    // (data race otherwise) - reject explicitly rather than miscompile.
    if (!node.mutatedCapturedVars.empty()) {
        impl_->addError(
            "thread { ... } cannot reassign an enclosing variable '" +
            node.mutatedCapturedVars.front() +
            "' (cross-thread mutation is not supported yet); capture it "
            "read-only, or move the work into a function",
            node.location());
        return;
    }

    // Snapshot captures (value + native type + kind) in the enclosing frame.
    struct Cap { std::string name; llvm::Value* val; Impl::VarKind kind; llvm::Type* ty; };
    std::vector<Cap> caps;
    for (const auto& capName : node.capturedVars) {
        auto* a = impl_->lookupVar(capName);
        if (!a) continue;
        Cap c;
        c.name = capName;
        c.ty = a->getAllocatedType();
        c.val = impl_->builder->CreateLoad(c.ty, a, capName + ".cap");
        c.kind = impl_->lookupVarKind(capName);
        caps.push_back(std::move(c));
    }

    // Coerce between a value's native LLVM type and the i64 thread-arg ABI.
    auto toI64 = [&](llvm::Value* v) -> llvm::Value* {
        if (v->getType() == impl_->i64Type) return v;
        if (v->getType()->isPointerTy()) return impl_->builder->CreatePtrToInt(v, impl_->i64Type);
        if (v->getType() == impl_->f64Type) return impl_->builder->CreateBitCast(v, impl_->i64Type);
        if (v->getType() == impl_->i1Type) return impl_->builder->CreateZExt(v, impl_->i64Type);
        return impl_->builder->CreateZExtOrBitCast(v, impl_->i64Type);
    };
    auto fromI64 = [&](llvm::Value* v, llvm::Type* ty) -> llvm::Value* {
        if (ty == impl_->i64Type) return v;
        if (ty->isPointerTy()) return impl_->builder->CreateIntToPtr(v, ty);
        if (ty == impl_->f64Type) return impl_->builder->CreateBitCast(v, impl_->f64Type);
        if (ty == impl_->i1Type) return impl_->builder->CreateTrunc(v, impl_->i1Type);
        return impl_->builder->CreateTruncOrBitCast(v, ty);
    };

    std::string threadFnName = "__dragon_thread_" + std::to_string(impl_->lambdaCounter++);
    std::vector<llvm::Type*> ptys(caps.size(), impl_->i64Type);  // ABI: all args are i64
    auto* funcType = llvm::FunctionType::get(impl_->i64Type, ptys, false);
    auto* threadFn = llvm::Function::Create(
        funcType, llvm::Function::InternalLinkage, threadFnName, impl_->module.get());

    auto* prevFunc = impl_->currentFunction;
    auto* prevBlock = impl_->builder->GetInsertBlock();
    // SAVE + CLEAR scopes so capture names resolve to the thread fn's own param
    // allocas, not the parent frame's.
    auto savedScopes = std::move(impl_->scopes);
    impl_->scopes.clear();
    impl_->currentFunction = threadFn;
    auto* entry = llvm::BasicBlock::Create(*impl_->context, "entry", threadFn);
    impl_->builder->SetInsertPoint(entry);
    impl_->pushScope();

    unsigned ai = 0;
    for (auto& arg : threadFn->args()) {
        Cap& c = caps[ai++];
        arg.setName(c.name);
        auto* a = impl_->createEntryAlloca(threadFn, c.name, c.ty);
        impl_->builder->CreateStore(fromI64(&arg, c.ty), a);
        impl_->setVar(c.name, a, c.kind);
        if (Impl::isHeapKind(c.kind))
            impl_->scopes.back().borrowed.insert(c.name);  // borrowed: parent owns the ref
    }

    for (auto& stmt : node.body) stmt->accept(*this);
    if (!impl_->builder->GetInsertBlock()->getTerminator()) {
        impl_->emitScopeCleanup();
        impl_->builder->CreateRet(llvm::ConstantInt::get(impl_->i64Type, 0));
    }
    impl_->popScope();
    impl_->scopes = std::move(savedScopes);
    impl_->currentFunction = prevFunc;
    impl_->builder->SetInsertPoint(prevBlock);

    // Build the int64[] args buffer on the caller's stack (alive across the
    // synchronous join; dragon_thread_fire heap-copies it anyway).
    auto* argsPtrTy = llvm::PointerType::getUnqual(*impl_->context);
    llvm::Value* argsPtr = llvm::ConstantPointerNull::get(argsPtrTy);
    if (!caps.empty()) {
        auto* arrTy = llvm::ArrayType::get(impl_->i64Type, caps.size());
        auto* argsArr = impl_->createEntryAlloca(prevFunc, "thread.args", arrTy);
        for (size_t i = 0; i < caps.size(); i++) {
            auto* gep = impl_->builder->CreateInBoundsGEP(
                arrTy, argsArr,
                {llvm::ConstantInt::get(impl_->i64Type, 0),
                 llvm::ConstantInt::get(impl_->i64Type, (uint64_t)i)},
                "thread.arg");
            impl_->builder->CreateStore(toI64(caps[i].val), gep);
        }
        argsPtr = impl_->builder->CreateBitCast(argsArr, argsPtrTy);
    }

    auto* fnPtr = impl_->builder->CreateBitCast(threadFn, impl_->i8PtrType);
    auto* nargs = llvm::ConstantInt::get(impl_->i64Type, (int64_t)caps.size());
    auto* threadHandle = impl_->builder->CreateCall(
        impl_->runtimeFuncs["dragon_thread_fire"], {fnPtr, argsPtr, nargs}, "thread.scoped");
    impl_->builder->CreateCall(
        impl_->runtimeFuncs["dragon_thread_join"], {threadHandle}, "thread.join");
}

/// Lower match/case (PEP 634) to chained conditional branches.
///
/// Each case arm becomes a test-and-branch block:
///  match.caseN.test - evaluate pattern match condition
///  match.caseN.body - execute body if matched, then branch to match.end
/// A final match.end block is the merge point.
///
/// Pattern kinds:
///  Wildcard (_) - always true
///  Literal - compare subject with constant (int: ICmpEQ, str: dragon_str_eq, etc.)
///  Capture (x) - always true, binds subject value to a local variable
///  Sequence - check tuple length, then recursively match each element
///  Or (p1|p2) - short-circuit disjunction of sub-patterns
///  Value - evaluate dotted-name expression, compare with subject
} // namespace dragon
