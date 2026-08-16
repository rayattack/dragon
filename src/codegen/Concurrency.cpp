/// Dragon CodeGen - Concurrency (fire, await, yield, thread)
#include "../CodeGenImpl.h"

namespace dragon {

void CodeGen::visit(FireExpr& node) {
    // D030: fire spawns a green thread via a per-callsite trampoline typed to the
    // target's exact signature - no i64 funneling, no generic Fn0..Fn8 dispatch.
    impl_->needsPthread = true;

    // Resolve the target function: (a) fire { block } synthesizes an internal nullary
    // fn; (b) fire fn(args) resolves the callee and evaluates user args.
    llvm::Function* targetFn = nullptr;
    std::vector<llvm::Value*> userArgs;
    std::vector<Impl::VarKind> argKinds;
    std::string siteName;

    if (!node.bodyStmts.empty()) {
        // Block form `fire { ... }`: lower the body to a fn taking captures as params,
        // then marshal them through the same spawn path as `fire fn(args)` (atomic-incref in, trampoline decref out; no cross-function alloca reference).

        // Reassigning an enclosing local from the vthread would be a data race (no
        // thread-safe cell exists yet). Reject explicitly rather than miscompile.
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

        // Snapshot each capture's value + kind in the enclosing frame before switching
        // into the fire fn. A name that doesn't resolve to a local (e.g. a module global) is skipped.
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
        // Save+clear the scope chain so capture names resolve to the fire fn's own
        // param allocas, not the parent frame's.
        auto savedScopes = std::move(impl_->scopes);
        impl_->scopes.clear();
        impl_->currentFunction = fireFn;
        auto* entry = llvm::BasicBlock::Create(*impl_->context, "entry", fireFn);
        impl_->builder->SetInsertPoint(entry);
        impl_->pushScope();

        // Materialize each capture as a local param. Heap captures are borrowed (spawn
        // site increfs, trampoline decrefs); the body must not decref at scope cleanup or it double-frees.
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

        // Marshal captures as spawn args (mirrors the call form); atomic-incref heap
        // captures so they survive until the trampoline decrefs them post-call.
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
            // Resolve like a normal call: import alias, same-module mangled name, user
            // symbol, then bare name. Bare-name-only lookup failed for `fire` inside an imported module (e.g. stdlib ui rpc bridge).
            const std::string aliasSym = impl_->lookupImportedAlias(nameExpr->name);
            if (!aliasSym.empty())
                targetFn = impl_->module->getFunction(aliasSym);
            if (!targetFn)
                targetFn = impl_->module->getFunction(
                    Impl::mangleFunc(impl_->currentModuleName, nameExpr->name));
            if (!targetFn)
                targetFn = impl_->module->getFunction(
                    Impl::userFuncName(nameExpr->name));
            if (!targetFn)
                targetFn = impl_->module->getFunction(nameExpr->name);
            // funcParamKinds/paramIsOwn are keyed by the RESOLVED symbol.
            calleeName = targetFn ? targetFn->getName().str() : nameExpr->name;
        } else if (auto* attrExpr =
                   dynamic_cast<AttributeExpr*>(callExpr->callee.get())) {
            // Resolve (owningModule, className) like CallMethods.cpp's instance method
            // dispatch, so `fire self.method(...)`/`fire obj.method(...)` reach the module-mangled symbol, not the old bare className.
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
                        owningModule =
                            impl_->resolveClassOwningModule(className);
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

        // docs/002 2.8/2.9: `fire consume(own o)` moves the caller's +1; neutralize the
        // kind so neither the fire-site incref nor the trampoline decref touch it - keeping the pair double-freed it (A/B-proven, spawn-lend probe).
        for (size_t i = 0; i < userArgs.size() && i < argKinds.size(); i++)
            if (impl_->paramIsOwn(calleeName, (unsigned)i))
                argKinds[i] = Impl::VarKind::Other;

        // Atomic-incref heap args at the call site so they survive until the trampoline
        // decrefs them post-call.
        for (size_t i = 0; i < userArgs.size() && i < argKinds.size(); i++)
            impl_->emitAtomicIncref(userArgs[i], argKinds[i]);

        siteName = calleeName + "_" + std::to_string(impl_->lambdaCounter++);
    }

    // Build the per-callsite typed args struct from the target's LLVM signature.
    auto* targetTy = targetFn->getFunctionType();
    std::vector<llvm::Type*> argTypes;
    for (unsigned i = 0; i < targetTy->getNumParams(); i++)
        argTypes.push_back(targetTy->getParamType(i));
    auto* argsStructType =
        impl_->makeSpawnArgsStructType(argTypes, "fire.args." + siteName);

    // Build the trampoline (off the current insert point).
    int64_t resultTag = 0;
    if (auto* resultCall = dynamic_cast<CallExpr*>(node.operand.get()))
        if (resultCall->type)
            resultTag = Impl::taskResultReleaseTag(resultCall->type->kind());
    auto* tramp = impl_->buildFireTrampoline(
        targetFn, argsStructType, argKinds, siteName, resultTag);

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
    // `fire f(own x)`: the args struct now carries the moved +1; null the caller's slot
    // so its scope exit sees nothing (docs/002 2.8).
    if (auto* movedCall = dynamic_cast<CallExpr*>(node.operand.get()))
        impl_->emitMoveOutSlots(*movedCall);
}

// ADR 054: a conformance cast is proof, not conversion - the operand's pointer flows
// through unchanged. isBorrowedHeapExpr/resolveExprVarKind see through the node to the operand.
void CodeGen::visit(AsCastExpr& node) {
    node.operand->accept(*this);
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

    // Join the vthread, get the result (i64), then reinterpret at the task's native type
    // T (D030): bitcast for float, inttoptr for str/list/instance, truncate for bool.
    auto* rawResult = impl_->builder->CreateCall(
        impl_->runtimeFuncs["dragon_vthread_join"], {handle}, "await.result");
    impl_->lastValue = impl_->taskResultFromI64(rawResult, node.type.get());

    // `await t` consumes the task (runtime frees the vthread); null the binding's slot so
    // scope-exit detach doesn't double-free and a later is_alive/await reads NULL instead of the freed struct (closed a use-after-await UAF). Unbound temps have no slot to blank.
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
        impl_->addError(
            "internal error: yield reached codegen outside a generator "
            "function; the front end should have rejected this",
            node.location());
        impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
        return;
    }

    // Evaluate the yielded value (default to 0/None if no value)
    llvm::Value* yieldVal;
    int64_t yieldTag = 0;
    if (node.value) {
        node.value->accept(*this);
        yieldVal = impl_->lastValue;
        // Coerce to i64 if needed
        if (yieldVal->getType()->isPointerTy()) {
            // Heap yield: the slot owns a +1 (released on the next yield / at
            // destroy), so a borrowed source increfs; an owned temp transfers.
            if (node.value->type) {
                int64_t t = Impl::typeKindToTag(node.value->type->kind());
                if (t >= TAG_LIST || t == TAG_STR || t == TAG_CALLABLE)
                    yieldTag = t;
            }
            if (yieldTag != 0 && impl_->options.gcMode == GCMode::RC &&
                Impl::isBorrowedHeapExpr(node.value.get())) {
                if (yieldTag == TAG_STR)
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_incref_str"], {yieldVal});
                else if (yieldTag == TAG_CALLABLE)
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_incref_callable"], {yieldVal});
                else
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_incref"], {yieldVal});
            }
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
        impl_->runtimeFuncs["dragon_generator_yield"],
        {genPtr, yieldVal, llvm::ConstantInt::get(impl_->i64Type, yieldTag)});

    // The yield expression itself evaluates to 0 (sent value not yet supported)
    impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
}
void CodeGen::visit(ThreadStmt& node) {
    // thread { block }: scoped OS thread, auto-joined at scope exit. Lowers the body to
    // a fn taking captures as i64 params (ABI caps at 8 i64 args), so it never references the parent frame's allocas. Joined synchronously, so captures stay borrowed (no incref/decref).
    impl_->needsPthread = true;

    // Cross-thread reassignment of an enclosing local would be a data race. Reject
    // explicitly rather than miscompile.
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
    // Save+clear scopes so capture names resolve to the thread fn's own param allocas,
    // not the parent frame's.
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

} // namespace dragon
