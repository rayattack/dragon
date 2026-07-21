/// Dragon CodeGen - Lambda, Function Declaration, TypeAlias
#include "../CodeGenImpl.h"

namespace dragon {

// the per-closure-site env GC hook. One emitter, two call sites
// (LambdaExpr + nested def), so the DEALLOC/TRAVERSE/CLEAR logic can't drift.
// Signature matches DragonEnv.gc_fn: void(env: ptr, op: i32, visit: ptr, arg: ptr).
//  op 0 DEALLOC - decref each heap capture (the former dealloc_fn behavior).
//  op 1 TRAVERSE - visit each cycle-capable capture so the cycle collector
//  subtracts the env's internal ref to it. The visit fns
//  (subtract/reachable) only dereference a TRACKED child, so a
//  bare-fn-ptr Closure capture is a safe hash-miss - no gate.
//  op 2 CLEAR - decref + NULL each heap capture slot (break the cycle; the
//  later env dealloc then sees emptied slots, double-frees 0).
llvm::Function* CodeGen::Impl::emitEnvGcFn(
        const std::string& baseName, llvm::StructType* envStructType,
        const std::vector<EnvCaptureDesc>& caps) {
    auto* i32Ty = llvm::Type::getInt32Ty(*context);
    auto* gcFnType = llvm::FunctionType::get(
        voidType, {i8PtrType, i32Ty, i8PtrType, i8PtrType}, false);
    auto* gcFn = llvm::Function::Create(
        gcFnType, llvm::Function::InternalLinkage, baseName + "__env_gc",
        module.get());

    auto* prevFunc = currentFunction;
    auto* prevBlock = builder->GetInsertBlock();
    currentFunction = gcFn;

    auto* entry = llvm::BasicBlock::Create(*context, "entry", gcFn);
    builder->SetInsertPoint(entry);

    auto argIt = gcFn->arg_begin();
    llvm::Value* envPtr   = &*argIt++; envPtr->setName("env");
    llvm::Value* opArg    = &*argIt++; opArg->setName("op");
    llvm::Value* visitArg = &*argIt++; visitArg->setName("visit");
    llvm::Value* dataArg  = &*argIt;   dataArg->setName("arg");

    llvm::Value* envTyped = builder->CreateBitCast(
        envPtr, llvm::PointerType::getUnqual(*context), "env.typed");

    auto* deallocBB  = llvm::BasicBlock::Create(*context, "op.dealloc", gcFn);
    auto* traverseBB = llvm::BasicBlock::Create(*context, "op.traverse", gcFn);
    auto* clearBB    = llvm::BasicBlock::Create(*context, "op.clear", gcFn);
    auto* markBB     = llvm::BasicBlock::Create(*context, "op.markshared", gcFn); 
    auto* retBB      = llvm::BasicBlock::Create(*context, "op.ret", gcFn);

    auto* sw = builder->CreateSwitch(opArg, retBB, 4);
    sw->addCase(llvm::ConstantInt::get(i32Ty, 0 /*DEALLOC*/),  deallocBB);
    sw->addCase(llvm::ConstantInt::get(i32Ty, 1 /*TRAVERSE*/), traverseBB);
    sw->addCase(llvm::ConstantInt::get(i32Ty, 2 /*CLEAR*/),    clearBB);
    sw->addCase(llvm::ConstantInt::get(i32Ty, 3 /*MARK_SHARED*/), markBB);

    auto* visitFnPtrType = llvm::FunctionType::get(
        voidType, {i8PtrType, i8PtrType}, false);

    auto loadCapPtr = [&](size_t i) {
        auto* fieldPtr = builder->CreateStructGEP(
            envStructType, envTyped, (unsigned)(i + 1));
        auto* ptr = builder->CreateLoad(i8PtrType, fieldPtr);
        return std::make_pair(fieldPtr, ptr);
    };
    // The exact per-kind decref the old dealloc_fn used (cell/str/closure/
    // generic) - preserved so DEALLOC behavior is byte-identical, and reused by
    // CLEAR. Closure goes through the tag-gated _callable (bare-fn-ptr safe).
    auto emitCapDecref = [&](const EnvCaptureDesc& c, llvm::Value* ptr) {
        if (c.isCellRelay)
            builder->CreateCall(runtimeFuncs["dragon_decref"], {ptr});
        else if (c.kind == VarKind::Str)
            builder->CreateCall(runtimeFuncs["dragon_decref_str"], {ptr});
        else if (c.kind == VarKind::Closure)
            builder->CreateCall(runtimeFuncs["dragon_decref_callable"], {ptr});
        else
            builder->CreateCall(runtimeFuncs["dragon_decref"], {ptr});
    };

    // DEALLOC
    builder->SetInsertPoint(deallocBB);
    for (size_t i = 0; i < caps.size(); i++) {
        if (caps[i].isCellRelay || isHeapKind(caps[i].kind)) {
            auto pr = loadCapPtr(i);
            emitCapDecref(caps[i], pr.second);
        }
    }
    builder->CreateRetVoid();

    // TRAVERSE
    builder->SetInsertPoint(traverseBB);
    for (size_t i = 0; i < caps.size(); i++) {
        if (!envCaptureIsCyclic(caps[i].kind, caps[i].isCellRelay)) continue;
        auto pr = loadCapPtr(i);
        auto* notNull = builder->CreateICmpNE(
            pr.second, llvm::ConstantPointerNull::get(
                           llvm::PointerType::getUnqual(*context)));
        auto* visitBB = llvm::BasicBlock::Create(*context, "cap.visit", gcFn);
        auto* contBB  = llvm::BasicBlock::Create(*context, "cap.cont", gcFn);
        builder->CreateCondBr(notNull, visitBB, contBB);
        builder->SetInsertPoint(visitBB);
        builder->CreateCall(visitFnPtrType, visitArg, {pr.second, dataArg});
        builder->CreateBr(contBB);
        builder->SetInsertPoint(contBB);
    }
    builder->CreateRetVoid();

    // CLEAR
    builder->SetInsertPoint(clearBB);
    for (size_t i = 0; i < caps.size(); i++) {
        if (caps[i].isCellRelay || isHeapKind(caps[i].kind)) {
            auto pr = loadCapPtr(i);
            emitCapDecref(caps[i], pr.second);
            builder->CreateStore(
                llvm::Constant::getNullValue(
                    envStructType->getElementType((unsigned)(i + 1))),
                pr.first);
        }
    }
    builder->CreateRetVoid();

    // Propagate the SHARED flag into every heap capture - str included
    // (TRAVERSE skips them: they cannot close a cycle, but a capture read out
    // of shared closure increfs, so it must be marked or refcount tears cross-thread)
    builder->SetInsertPoint(markBB);
    for (size_t i = 0; i < caps.size(); i++) {
        if (!(caps[i].isCellRelay || isHeapKind(caps[i].kind))) continue;
        auto pr = loadCapPtr(i);
        if (caps[i].isCellRelay) {
            builder->CreateCall(runtimeFuncs["dragon_mark_shared_cell"],
                                {dataArg, pr.second});
        } else if (caps[i].kind == VarKind::Str) {
            builder->CreateCall(runtimeFuncs["dragon_mark_shared_str"],
                                {pr.second});
        } else if (caps[i].kind == VarKind::Closure) {
            // Tag-gated: a Callable capture may be a bare fn pointer.
            builder->CreateCall(runtimeFuncs["dragon_mark_shared_callable"],
                                {dataArg, pr.second});
        } else {
            builder->CreateCall(runtimeFuncs["dragon_mark_shared_worklist_push"],
                                {dataArg, pr.second});
        }
    }
    builder->CreateRetVoid();

    builder->SetInsertPoint(retBB);
    builder->CreateRetVoid();

    currentFunction = prevFunc;
    if (prevBlock) builder->SetInsertPoint(prevBlock);
    return gcFn;
}

void CodeGen::visit(LambdaExpr& node) {
    // Lambda expression: generate a new internal function and return its pointer.
    // D027: If capturedVars is non-empty, create a closure (env + wrapper).

    bool hasCaptures = !node.capturedVars.empty();

    // 1. Generate unique function name
    std::string lambdaName = "__dragon_lambda_" + std::to_string(impl_->lambdaCounter++);

    // 2. Determine return type. An expression-body lambda always returns a
    // value (int default); a block lambda is a procedure only when it has no
    // value-returning return (then void, so a bare `return` lowers cleanly).
    llvm::Type* retType = impl_->typeExprToLLVM(node.returnType.get());
    if (!node.returnType) {
        retType = (node.body || impl_->bodyReturnsValue(node.bodyStmts))
            ? impl_->i64Type : impl_->voidType;
    }

    // 3. Build parameter types (user-visible params)
    std::vector<llvm::Type*> userParamTypes;
    for (auto& p : node.params) {
        userParamTypes.push_back(impl_->typeExprToLLVM(p.type.get()));
    }

    // 4. Build full param types: user params + optional trailing i8* env
    std::vector<llvm::Type*> paramTypes = userParamTypes;
    if (hasCaptures) {
        paramTypes.push_back(impl_->i8PtrType);  // env pointer
    }

    auto* funcType = llvm::FunctionType::get(retType, paramTypes, false);
    auto* lambdaFunc = llvm::Function::Create(
        funcType, llvm::Function::InternalLinkage, lambdaName, impl_->module.get());

    // D027: Gather capture info BEFORE switching to lambda function context.
    // We need to load values from the current scope while we're still in the caller.
    // D027.1: a capture flagged `nonlocal` by the lambda body is relayed as
    // a cell pointer; reads/writes route through dragon_cell_get/set.
    std::unordered_set<std::string> innerCellRelayed(
        node.mutatedCapturedVars.begin(), node.mutatedCapturedVars.end());
    struct CaptureInfo {
        std::string name;
        llvm::Value* value;
        Impl::VarKind kind;
        std::string className;  // for ClassInstance captures
        bool isCellRelay = false;
    };
    std::vector<CaptureInfo> captures;
    if (hasCaptures) {
        for (auto& capName : node.capturedVars) {
            CaptureInfo ci;
            ci.name = capName;
            ci.kind = impl_->lookupVarKind(capName);
            ci.isCellRelay = innerCellRelayed.count(capName) > 0;
            // Capture class name for field access inside lambda
            auto cnIt = impl_->varClassNames.find(capName);
            if (cnIt != impl_->varClassNames.end())
                ci.className = cnIt->second;
            // Load current value from enclosing scope
            auto* alloca = impl_->lookupVar(capName);
            if (alloca) {
                ci.value = impl_->builder->CreateLoad(
                    alloca->getAllocatedType(), alloca, capName + ".cap");
            } else {
                auto* gv = impl_->lookupModuleGlobal(capName);
                if (gv) {
                    ci.value = impl_->builder->CreateLoad(
                        gv->getValueType(), gv, capName + ".cap");
                } else {
                    ci.value = llvm::ConstantInt::get(impl_->i64Type, 0);
                }
            }
            captures.push_back(ci);
        }
    }

    // 5. Save current codegen state
    auto* prevFunc = impl_->currentFunction;
    auto* prevBlock = impl_->builder->GetInsertBlock();
    // Isolate lambda's scope chain so emitAllScopeCleanup in ReturnStmt
    // doesn't walk into the enclosing function's scopes.
    auto savedScopes = std::move(impl_->scopes);
    impl_->scopes.clear();
    // D027.1: lambda's own cell-promoted locals (from any nonlocal in its
    // own nested defs) are computed independently. Save/restore the parent's.
    auto savedCellPromoted = std::move(impl_->cellPromotedLocals);
    impl_->cellPromotedLocals.clear();
    {
        std::unordered_set<std::string> nestedMutated;
        if (node.body) impl_->collectNestedMutatedCaptures(node.body.get(), nestedMutated);
        for (auto& bs : node.bodyStmts) impl_->collectNestedMutatedCaptures(bs.get(), nestedMutated);
        std::unordered_set<std::string> ownRelay(
            node.mutatedCapturedVars.begin(), node.mutatedCapturedVars.end());
        for (const auto& n : nestedMutated) {
            if (ownRelay.count(n)) continue;
            impl_->cellPromotedLocals.insert(n);
        }
    }

    // 6. Set up new function
    impl_->currentFunction = lambdaFunc;
    auto* entry = llvm::BasicBlock::Create(*impl_->context, "entry", lambdaFunc);
    impl_->builder->SetInsertPoint(entry);

    // 7. Push scope, create allocas for user params
    impl_->pushScope();
    size_t idx = 0;
    for (auto& arg : lambdaFunc->args()) {
        if (idx >= node.params.size()) break;  // skip env param
        std::string paramName = node.params[idx].name;
        arg.setName(paramName);
        auto* alloca = impl_->createEntryAlloca(lambdaFunc, paramName, funcType->getParamType(idx));
        impl_->builder->CreateStore(&arg, alloca);
        auto paramKind = impl_->typeExprToKind(node.params[idx].type.get());
        impl_->setVar(paramName, alloca, paramKind);
        impl_->trackPtrParam(paramName, node.params[idx].type.get());
        if (paramKind == Impl::VarKind::ClassInstance) {
            impl_->bindClassVar(paramName, node.params[idx].type.get());
        }
        if (Impl::isHeapKind(paramKind))
            impl_->scopes.back().borrowed.insert(paramName);
        idx++;
    }

    // D030: Build per-lambda env struct type with native field types.
    //  { [24 x i8]; <native capture types...> }
    //  Field 0 mirrors sizeof(DragonEnv) = 16 (header) + 8 (dealloc_fn) = 24 bytes.
    //  Subsequent fields are the captures themselves, in order, at native LLVM types.
    // Computed once and used at: (a) lambda body unpack, (b) capture site populate,
    // (c) dealloc fn for heap-typed captures.
    auto kindToCaptureLLVM = [&](Impl::VarKind k) -> llvm::Type* {
        switch (k) {
            case Impl::VarKind::Float: return impl_->f64Type;
            case Impl::VarKind::Bool:  return impl_->i1Type;
            case Impl::VarKind::Str:
            case Impl::VarKind::StrLiteral:
            case Impl::VarKind::List:
            case Impl::VarKind::Dict:
            case Impl::VarKind::Tuple:
            case Impl::VarKind::Set:
            case Impl::VarKind::File:
            case Impl::VarKind::ClassInstance:
            case Impl::VarKind::Generator:
            case Impl::VarKind::Closure:
                return impl_->i8PtrType;
            default:
                return impl_->i64Type;  // Int / Other / unknown
        }
    };

    llvm::StructType* envStructType = nullptr;
    if (hasCaptures) {
        std::vector<llvm::Type*> envFields;
        envFields.push_back(llvm::ArrayType::get(
            llvm::Type::getInt8Ty(*impl_->context), 24));  // header + dealloc_fn
        for (auto& cap : captures) {
            envFields.push_back(cap.isCellRelay
                ? impl_->i8PtrType : kindToCaptureLLVM(cap.kind));
        }
        envStructType = llvm::StructType::create(
            *impl_->context, envFields, lambdaName + ".env");
    }

    // D030: Load captures from env into local variables via typed GEPs (no i64 round-trip).
    if (hasCaptures) {
        llvm::Value* envArg = &*(lambdaFunc->arg_end() - 1);
        envArg->setName("__env");
        // Cast i8* env to the per-lambda env struct pointer
        llvm::Value* envTyped = impl_->builder->CreateBitCast(
            envArg, llvm::PointerType::getUnqual(*impl_->context), "__env.typed");

        for (size_t i = 0; i < captures.size(); i++) {
            auto& cap = captures[i];
            llvm::Type* fieldType = envStructType->getElementType((unsigned)(i + 1));

            // GEP env, 0, i+1 -> ptr to capture field
            auto* fieldPtr = impl_->builder->CreateStructGEP(
                envStructType, envTyped, (unsigned)(i + 1), cap.name + ".env.ptr");
            // Load the field at its native type - no bitcast/IntToPtr/ICmpNE round-trip
            auto* typedVal = impl_->builder->CreateLoad(
                fieldType, fieldPtr, cap.name + ".env");

            auto* alloca = impl_->createEntryAlloca(lambdaFunc, cap.name, fieldType);
            impl_->builder->CreateStore(typedVal, alloca);
            impl_->setVar(cap.name, alloca, cap.kind);
            // Restore class name mapping for ClassInstance captures
            if (!cap.className.empty())
                impl_->varClassNames[cap.name] = cap.className;
            // Captures are borrowed from the env - don't decref at scope exit
            impl_->scopes.back().borrowed.insert(cap.name);
            // D027.1: cell-relayed captures use cell ops for read/write.
            if (cap.isCellRelay) {
                impl_->markCellBacked(cap.name);
            }
        }
    }

    // 8. Generate body
    if (node.body) {
        node.body->accept(*this);
        llvm::Value* bodyVal = impl_->lastValue;
        if (bodyVal->getType() != retType) {
            if (retType == impl_->f64Type && bodyVal->getType() == impl_->i64Type)
                bodyVal = impl_->builder->CreateSIToFP(bodyVal, impl_->f64Type);
            else if (retType == impl_->i64Type && bodyVal->getType() == impl_->i1Type)
                bodyVal = impl_->builder->CreateZExt(bodyVal, impl_->i64Type);
            else if (retType == impl_->f64Type && bodyVal->getType() == impl_->i1Type)
                bodyVal = impl_->builder->CreateUIToFP(bodyVal, impl_->f64Type);
        }
        if (impl_->options.gcMode == GCMode::RC) {
            if (auto* nameExpr = dynamic_cast<NameExpr*>(node.body.get())) {
                auto kind = impl_->lookupVarKind(nameExpr->name);
                // emitIncrefByKind dispatches Str/Closure/generic - the
                // Closure case is tag-gated (a returned bare-fn Callable
                // must not have a refcount written into its code bytes).
                impl_->emitIncrefByKind(bodyVal, kind);
            }
        }
        impl_->emitScopeCleanup();
        impl_->builder->CreateRet(bodyVal);
    } else if (!node.bodyStmts.empty()) {
        for (auto& stmt : node.bodyStmts) {
            stmt->accept(*this);
        }
        if (!impl_->builder->GetInsertBlock()->getTerminator()) {
            impl_->emitScopeCleanup();
            if (retType == impl_->voidType) {
                impl_->builder->CreateRetVoid();
            } else {
                impl_->builder->CreateRet(llvm::Constant::getNullValue(retType));
            }
        }
    } else {
        impl_->emitScopeCleanup();
        if (retType == impl_->voidType) {
            impl_->builder->CreateRetVoid();
        } else {
            impl_->builder->CreateRet(llvm::Constant::getNullValue(retType));
        }
    }

    // 9. Pop scope and restore state
    impl_->popScope();
    impl_->scopes = std::move(savedScopes);
    impl_->cellPromotedLocals = std::move(savedCellPromoted);
    impl_->currentFunction = prevFunc;
    if (prevBlock) impl_->builder->SetInsertPoint(prevBlock);

    // 10. Result
    if (hasCaptures) {
        // emit the multi-op env GC hook (DEALLOC/TRAVERSE/CLEAR)
        // via the shared emitter, and gc-track the env iff it captures a
        // cycle-capable object (so instance/list -> closure -> env cycles are
        // collectable; scalar/str-only envs stay untracked - #1).
        std::vector<Impl::EnvCaptureDesc> capDescs;
        capDescs.reserve(captures.size());
        bool envTrackable = false;
        for (auto& cap : captures) {
            capDescs.push_back({cap.kind, cap.isCellRelay});
            if (Impl::envCaptureIsCyclic(cap.kind, cap.isCellRelay))
                envTrackable = true;
        }
        auto* gcFn = impl_->emitEnvGcFn(lambdaName, envStructType, capDescs);

        // D030: Allocate env at exact native size, then populate via typed GEPs.
        const auto& dl = impl_->module->getDataLayout();
        uint64_t envSize = dl.getTypeAllocSize(envStructType);

        auto* envVal = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_env_alloc"],
            {llvm::ConstantInt::get(impl_->i64Type, (int64_t)envSize),
             impl_->builder->CreateBitCast(gcFn, impl_->i8PtrType),
             llvm::ConstantInt::get(llvm::Type::getInt32Ty(*impl_->context),
                                    envTrackable ? 1 : 0)},
            "closure.env");

        // envVal is i8* - cast to the typed pointer for GEPs
        llvm::Value* envTyped = impl_->builder->CreateBitCast(
            envVal, llvm::PointerType::getUnqual(*impl_->context), "closure.env.typed");

        // Populate captures with native-typed stores. Coerce only when
        // the loaded value's type doesn't match the field type - those are
        // legitimate widening (e.g. if the source was an alloca of a
        // different type than the capture field).
        for (size_t i = 0; i < captures.size(); i++) {
            auto& cap = captures[i];
            llvm::Type* fieldType = envStructType->getElementType((unsigned)(i + 1));
            llvm::Value* storeVal = cap.value;

            // Reconcile loaded value type with field type. The capture's
            // .value was loaded from the enclosing alloca at its native
            // type, which usually matches kindToCaptureLLVM. The cases that
            // need adjustment are int->float captures (rare) or capturing
            // a literal whose type is narrower.
            if (storeVal->getType() != fieldType) {
                if (fieldType == impl_->f64Type && storeVal->getType() == impl_->i64Type)
                    storeVal = impl_->builder->CreateSIToFP(storeVal, fieldType);
                else if (fieldType == impl_->i64Type && storeVal->getType() == impl_->i1Type)
                    storeVal = impl_->builder->CreateZExt(storeVal, fieldType);
                else if (fieldType == impl_->i8PtrType && storeVal->getType()->isIntegerTy())
                    storeVal = impl_->builder->CreateIntToPtr(storeVal, fieldType);
                else if (fieldType->isIntegerTy() && storeVal->getType()->isPointerTy())
                    storeVal = impl_->builder->CreatePtrToInt(storeVal, fieldType);
                // Otherwise: assume bitcast-compatible (rare; LLVM will assert if not)
                else
                    storeVal = impl_->builder->CreateBitCast(storeVal, fieldType);
            }

            auto* fieldPtr = impl_->builder->CreateStructGEP(
                envStructType, envTyped, (unsigned)(i + 1), cap.name + ".env.slot");
            impl_->builder->CreateStore(storeVal, fieldPtr);

            // Incref heap captures - env now holds a reference. Cell-relayed
            // captures travel as TAG_CELL pointers, so the env's incref hits
            // the cell itself (plain dragon_incref); _str would mis-navigate
            // through the cell's bytes treating them as a string payload.
            if (impl_->options.gcMode == GCMode::RC) {
                if (cap.isCellRelay) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_incref"], {storeVal});
                } else if (Impl::isHeapKind(cap.kind)) {
                    if (cap.kind == Impl::VarKind::Str) {
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_incref_str"], {storeVal});
                    } else if (cap.kind == Impl::VarKind::Closure) {
                        // a captured Callable may hold a BARE fn pointer
                        // (a decorator's `fn` param wrapping a plain def).
                        // The generic incref would WRITE a refcount into the
                        // function's code bytes (SIGSEGV - the chained-
                        // decorator crash). The tag-gated variant increfs a
                        // real DragonClosure and no-ops on a bare fn ptr.
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_incref_callable"], {storeVal});
                    } else {
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_incref"], {storeVal});
                    }
                }
            }
        }

        // Create closure: dragon_closure_create(fn_ptr, env). Returns ptr.
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_closure_create"],
            {impl_->builder->CreateBitCast(lambdaFunc, impl_->i8PtrType), envVal},
            "closure");

        // Record user-facing callable type for indirect call dispatch
        impl_->lastClosureCallableType = llvm::FunctionType::get(
            retType, userParamTypes, false);
    } else {
        // Non-capturing: result is bare function pointer (unchanged behavior)
        impl_->lastValue = lambdaFunc;
        impl_->lastClosureCallableType = nullptr;
    }
}

//===---------------------------------------------------------------------===//
// D027.1: Recursive collection of nested-function `mutatedCapturedVars`.
//
// For a given function body, we want to know which names get nonlocal-mutated
// somewhere inside its nested defs/lambdas (potentially many levels deep).
// The intersection with the function's own locals tells us which locals to
// cell-promote at this function's level. Names not bound here will be
// resolved by an even-further-out function that owns them.
//===---------------------------------------------------------------------===//

void CodeGen::Impl::collectNestedMutatedCaptures(
    const std::vector<std::unique_ptr<Stmt>>& body,
    std::unordered_set<std::string>& out)
{
    for (auto& s : body) collectNestedMutatedCaptures(s.get(), out);
}

void CodeGen::Impl::collectNestedMutatedCaptures(Stmt* s,
                                                 std::unordered_set<std::string>& out)
{
    if (!s) return;
    if (auto* fd = dynamic_cast<FunctionDecl*>(s)) {
        // The nested fn's own mutatedCapturedVars (its `nonlocal` decls)
        // surface as required cell-backings at this scanner's level. We
        // also recurse into its body to pick up grandchildren whose
        // nonlocal targets bind even further out - those names appear
        // here too because the intermediate nested fn relays them.
        for (const auto& n : fd->mutatedCapturedVars) out.insert(n);
        collectNestedMutatedCaptures(fd->body, out);
        return;
    }
    if (auto* es = dynamic_cast<ExprStmt*>(s)) {
        collectNestedMutatedCaptures(es->expr.get(), out);
        return;
    }
    if (auto* as = dynamic_cast<AssignStmt*>(s)) {
        for (auto& t : as->targets) collectNestedMutatedCaptures(t.get(), out);
        collectNestedMutatedCaptures(as->value.get(), out);
        return;
    }
    if (auto* an = dynamic_cast<AnnAssignStmt*>(s)) {
        if (an->value) collectNestedMutatedCaptures(an->value.get(), out);
        return;
    }
    if (auto* aa = dynamic_cast<AugAssignStmt*>(s)) {
        if (aa->target) collectNestedMutatedCaptures(aa->target.get(), out);
        collectNestedMutatedCaptures(aa->value.get(), out);
        return;
    }
    if (auto* ifs = dynamic_cast<IfStmt*>(s)) {
        if (ifs->condition) collectNestedMutatedCaptures(ifs->condition.get(), out);
        collectNestedMutatedCaptures(ifs->thenBody, out);
        for (auto& [cond, body] : ifs->elifClauses) {
            if (cond) collectNestedMutatedCaptures(cond.get(), out);
            collectNestedMutatedCaptures(body, out);
        }
        collectNestedMutatedCaptures(ifs->elseBody, out);
        return;
    }
    if (auto* w = dynamic_cast<WhileStmt*>(s)) {
        if (w->condition) collectNestedMutatedCaptures(w->condition.get(), out);
        collectNestedMutatedCaptures(w->body, out);
        collectNestedMutatedCaptures(w->elseBody, out);
        return;
    }
    if (auto* f = dynamic_cast<ForStmt*>(s)) {
        if (f->iterable) collectNestedMutatedCaptures(f->iterable.get(), out);
        collectNestedMutatedCaptures(f->body, out);
        collectNestedMutatedCaptures(f->elseBody, out);
        return;
    }
    if (auto* t = dynamic_cast<TryStmt*>(s)) {
        collectNestedMutatedCaptures(t->tryBody, out);
        for (auto& h : t->handlers) collectNestedMutatedCaptures(h.body, out);
        collectNestedMutatedCaptures(t->elseBody, out);
        collectNestedMutatedCaptures(t->finallyBody, out);
        return;
    }
    if (auto* ws = dynamic_cast<WithStmt*>(s)) {
        for (auto& it : ws->items) {
            if (it.contextExpr) collectNestedMutatedCaptures(it.contextExpr.get(), out);
        }
        collectNestedMutatedCaptures(ws->body, out);
        return;
    }
    if (auto* r = dynamic_cast<ReturnStmt*>(s)) {
        if (r->value) collectNestedMutatedCaptures(r->value.get(), out);
        return;
    }
    if (auto* th = dynamic_cast<ThreadStmt*>(s)) {
        // ThreadStmt blocks are themselves capture sites - its own
        // mutatedCapturedVars bubble up here so the owning function
        // cell-promotes the appropriate locals.
        for (const auto& n : th->mutatedCapturedVars) out.insert(n);
        collectNestedMutatedCaptures(th->body, out);
        return;
    }
}

void CodeGen::Impl::collectNestedMutatedCaptures(Expr* e,
                                                 std::unordered_set<std::string>& out)
{
    if (!e) return;
    if (auto* le = dynamic_cast<LambdaExpr*>(e)) {
        for (const auto& n : le->mutatedCapturedVars) out.insert(n);
        if (le->body) collectNestedMutatedCaptures(le->body.get(), out);
        for (auto& bs : le->bodyStmts) collectNestedMutatedCaptures(bs.get(), out);
        return;
    }
    if (auto* fe = dynamic_cast<FireExpr*>(e)) {
        for (const auto& n : fe->mutatedCapturedVars) out.insert(n);
        // FireExpr can wrap either a CallExpr (fire foo()) via `operand`,
        // or a body block (`fire { ... }`) via `bodyStmts`. Recurse into
        // both so anything deeper still surfaces.
        if (fe->operand) collectNestedMutatedCaptures(fe->operand.get(), out);
        for (auto& s : fe->bodyStmts) collectNestedMutatedCaptures(s.get(), out);
        return;
    }
    if (auto* ce = dynamic_cast<CallExpr*>(e)) {
        if (ce->callee) collectNestedMutatedCaptures(ce->callee.get(), out);
        for (auto& a : ce->args) collectNestedMutatedCaptures(a.get(), out);
        for (auto& [_, a] : ce->kwArgs) collectNestedMutatedCaptures(a.get(), out);
        return;
    }
    if (auto* be = dynamic_cast<BinaryExpr*>(e)) {
        collectNestedMutatedCaptures(be->left.get(), out);
        collectNestedMutatedCaptures(be->right.get(), out);
        return;
    }
    if (auto* ue = dynamic_cast<UnaryExpr*>(e)) {
        collectNestedMutatedCaptures(ue->operand.get(), out);
        return;
    }
    if (auto* ie = dynamic_cast<IfExpr*>(e)) {
        collectNestedMutatedCaptures(ie->condition.get(), out);
        collectNestedMutatedCaptures(ie->thenExpr.get(), out);
        collectNestedMutatedCaptures(ie->elseExpr.get(), out);
        return;
    }
    if (auto* sub = dynamic_cast<SubscriptExpr*>(e)) {
        collectNestedMutatedCaptures(sub->object.get(), out);
        collectNestedMutatedCaptures(sub->index.get(), out);
        return;
    }
    if (auto* attr = dynamic_cast<AttributeExpr*>(e)) {
        collectNestedMutatedCaptures(attr->object.get(), out);
        return;
    }
    // Leaf expressions and ones that can't host a nested fn/lambda body
    // don't matter for this scan.
}

namespace {
// --- D027 closure-return analysis (escaping-closure dispatch fix) ----------
// A `-> Callable[...]` function may return either a closure (a capturing nested
// def / capturing lambda: a heap DragonClosure with an env) or a bare function
// pointer. The call-site dispatch differs, so the receiving var must be marked
// VarKind::Closure iff the function returns a closure. Detected SOUNDLY at decl
// time: only when EVERY value-return is provably a closure. Anything we can't
// prove stays bare-fn - so a bare-fn-returning function is never mis-marked
// (which would crash the closure dispatch by reading an env off a code ptr).

void collectCapturingDefs(const std::vector<std::unique_ptr<Stmt>>& body,
                          std::unordered_set<std::string>& out);

// Names of nested defs declared in `s` (NOT inside deeper nested fns/lambdas)
// that capture an enclosing var -> they materialize as DragonClosures.
void collectCapturingDefsStmt(Stmt* s, std::unordered_set<std::string>& out) {
    if (!s) return;
    if (auto* fd = dynamic_cast<FunctionDecl*>(s)) {
        if (!fd->capturedVars.empty()) out.insert(fd->name);
        return;  // a nested def's own inner defs are ITS closures, not ours
    }
    if (auto* ifs = dynamic_cast<IfStmt*>(s)) {
        collectCapturingDefs(ifs->thenBody, out);
        for (auto& cl : ifs->elifClauses) collectCapturingDefs(cl.second, out);
        collectCapturingDefs(ifs->elseBody, out);
    } else if (auto* w = dynamic_cast<WhileStmt*>(s)) {
        collectCapturingDefs(w->body, out);
        collectCapturingDefs(w->elseBody, out);
    } else if (auto* f = dynamic_cast<ForStmt*>(s)) {
        collectCapturingDefs(f->body, out);
        collectCapturingDefs(f->elseBody, out);
    } else if (auto* t = dynamic_cast<TryStmt*>(s)) {
        collectCapturingDefs(t->tryBody, out);
        for (auto& h : t->handlers) collectCapturingDefs(h.body, out);
        collectCapturingDefs(t->elseBody, out);
        collectCapturingDefs(t->finallyBody, out);
    } else if (auto* ws = dynamic_cast<WithStmt*>(s)) {
        collectCapturingDefs(ws->body, out);
    }
}
void collectCapturingDefs(const std::vector<std::unique_ptr<Stmt>>& body,
                          std::unordered_set<std::string>& out) {
    for (auto& s : body) collectCapturingDefsStmt(s.get(), out);
}

bool everyReturnClosure(const std::vector<std::unique_ptr<Stmt>>& body,
                        const std::unordered_set<std::string>& capDefs,
                        bool& sawReturn);

// False on the first value-return that is NOT provably a closure (incl. a bare
// `return`), so any path that could yield a bare fn vetoes the whole function.
bool everyReturnClosureStmt(Stmt* s,
                            const std::unordered_set<std::string>& capDefs,
                            bool& sawReturn) {
    if (!s) return true;
    if (dynamic_cast<FunctionDecl*>(s)) return true;  // nested def's returns are its own
    if (auto* r = dynamic_cast<ReturnStmt*>(s)) {
        if (!r->value) return false;                  // bare return -> not all-closure
        sawReturn = true;
        if (auto* le = dynamic_cast<LambdaExpr*>(r->value.get()))
            return !le->capturedVars.empty();
        if (auto* ne = dynamic_cast<NameExpr*>(r->value.get()))
            return capDefs.count(ne->name) > 0;
        return false;                                 // unknown returned expr -> conservative
    }
    if (auto* ifs = dynamic_cast<IfStmt*>(s)) {
        if (!everyReturnClosure(ifs->thenBody, capDefs, sawReturn)) return false;
        for (auto& cl : ifs->elifClauses)
            if (!everyReturnClosure(cl.second, capDefs, sawReturn)) return false;
        return everyReturnClosure(ifs->elseBody, capDefs, sawReturn);
    }
    if (auto* w = dynamic_cast<WhileStmt*>(s))
        return everyReturnClosure(w->body, capDefs, sawReturn)
            && everyReturnClosure(w->elseBody, capDefs, sawReturn);
    if (auto* f = dynamic_cast<ForStmt*>(s))
        return everyReturnClosure(f->body, capDefs, sawReturn)
            && everyReturnClosure(f->elseBody, capDefs, sawReturn);
    if (auto* t = dynamic_cast<TryStmt*>(s)) {
        if (!everyReturnClosure(t->tryBody, capDefs, sawReturn)) return false;
        for (auto& h : t->handlers)
            if (!everyReturnClosure(h.body, capDefs, sawReturn)) return false;
        if (!everyReturnClosure(t->elseBody, capDefs, sawReturn)) return false;
        return everyReturnClosure(t->finallyBody, capDefs, sawReturn);
    }
    if (auto* ws = dynamic_cast<WithStmt*>(s))
        return everyReturnClosure(ws->body, capDefs, sawReturn);
    return true;
}
bool everyReturnClosure(const std::vector<std::unique_ptr<Stmt>>& body,
                        const std::unordered_set<std::string>& capDefs,
                        bool& sawReturn) {
    for (auto& s : body)
        if (!everyReturnClosureStmt(s.get(), capDefs, sawReturn)) return false;
    return true;
}
} // namespace

bool CodeGen::Impl::functionReturnsClosure(FunctionDecl& node) {
    if (!dynamic_cast<CallableTypeExpr*>(node.returnType.get())) return false;
    std::unordered_set<std::string> capDefs;
    collectCapturingDefs(node.body, capDefs);
    bool sawReturn = false;
    return everyReturnClosure(node.body, capDefs, sawReturn) && sawReturn;
}

// Emit a generator function (shared by free functions and methods). Builds the
// inner body fn `<site>__gen_body(ptr gen, [ptr self,] params...) -> void`, then
// fills `wrapper` to build the typed args struct + per-callsite trampoline +
// decref fn, create the generator object, and return it. For a method,
// `hasSelf` threads `self` (typed by `selfClass`) as the first captured arg -
// modelled as the leading heap (ClassInstance) arg, so the wrapper increfs it
// and the destroy decref fn drops it (the generator owns a reference to self
// while alive). `userParamStart` skips an explicit self/cls in node.params.
void CodeGen::emitGeneratorFn(FunctionDecl& node, llvm::Function* wrapper,
                              const std::string& siteName, bool hasSelf,
                              const std::string& selfClass, size_t userParamStart) {
    // --- 1. Inner body function ---
    std::vector<llvm::Type*> bodyParamTypes;
    bodyParamTypes.push_back(impl_->i8PtrType);                 // gen
    if (hasSelf) bodyParamTypes.push_back(impl_->i8PtrType);    // self
    for (size_t i = userParamStart; i < node.params.size(); ++i)
        bodyParamTypes.push_back(impl_->typeExprToLLVM(node.params[i].type.get()));
    auto* bodyFuncType = llvm::FunctionType::get(impl_->voidType, bodyParamTypes, false);
    auto* bodyFunc = llvm::Function::Create(
        bodyFuncType, llvm::Function::InternalLinkage, siteName + "__gen_body",
        impl_->module.get());

    auto* prevFunc = impl_->currentFunction;
    auto* prevBlock = impl_->builder->GetInsertBlock();
    auto* prevGenPtr = impl_->generatorPtr;
    std::string prevClassName = impl_->currentClassName;
    auto savedGlobalDecls = impl_->globalDeclaredVars;
    auto savedNonlocalDecls = impl_->nonlocalDeclaredVars;
    impl_->globalDeclaredVars.clear();
    impl_->nonlocalDeclaredVars.clear();
    auto savedScopes = std::move(impl_->scopes);
    impl_->scopes.clear();

    impl_->builder->SetInsertPoint(
        llvm::BasicBlock::Create(*impl_->context, "entry", bodyFunc));
    impl_->currentFunction = bodyFunc;
    if (hasSelf) impl_->currentClassName = selfClass;
    impl_->pushScope();

    auto argIt = bodyFunc->arg_begin();
    argIt->setName("__gen");
    auto* genAlloca = impl_->createEntryAlloca(bodyFunc, "__gen", impl_->i8PtrType);
    impl_->builder->CreateStore(&*argIt, genAlloca);
    impl_->generatorPtr = genAlloca;
    ++argIt;
    if (hasSelf) {
        argIt->setName("self");
        auto* selfAlloca = impl_->createEntryAlloca(bodyFunc, "self", impl_->i8PtrType);
        impl_->builder->CreateStore(&*argIt, selfAlloca);
        impl_->setVar("self", selfAlloca, Impl::VarKind::ClassInstance);
        impl_->scopes.back().borrowed.insert("self");  // generator owns the ref; body borrows
        ++argIt;
    }
    for (size_t pi = userParamStart; argIt != bodyFunc->arg_end(); ++argIt, ++pi) {
        std::string paramName = node.params[pi].name;
        argIt->setName(paramName);
        auto* alloca = impl_->createEntryAlloca(bodyFunc, paramName, argIt->getType());
        impl_->builder->CreateStore(&*argIt, alloca);
        auto paramKind = impl_->typeExprToKind(node.params[pi].type.get());
        impl_->setVar(paramName, alloca, paramKind);
        impl_->trackPtrParam(paramName, node.params[pi].type.get());
        if (Impl::isHeapKind(paramKind))
            impl_->scopes.back().borrowed.insert(paramName);
    }

    for (auto& stmt : node.body) stmt->accept(*this);

    if (!impl_->builder->GetInsertBlock()->getTerminator()) {
        impl_->emitScopeCleanup();
        impl_->builder->CreateRetVoid();  // trampoline marks the generator exhausted
    }
    impl_->popScope();
    impl_->generatorPtr = prevGenPtr;
    impl_->scopes = std::move(savedScopes);
    impl_->currentFunction = prevFunc;
    impl_->currentClassName = prevClassName;
    impl_->globalDeclaredVars = savedGlobalDecls;
    impl_->nonlocalDeclaredVars = savedNonlocalDecls;

    // --- 2. Wrapper body: create + return the generator object ---
    // The wrapper's LLVM args ARE the captured args, in order: [self?, params...].
    impl_->builder->SetInsertPoint(
        llvm::BasicBlock::Create(*impl_->context, "entry", wrapper));
    unsigned nwrap = (unsigned)wrapper->arg_size();
    std::vector<Impl::VarKind> argKinds;
    std::vector<llvm::Type*> argTypes;
    if (hasSelf) {
        argKinds.push_back(Impl::VarKind::ClassInstance);
        argTypes.push_back(impl_->i8PtrType);
    }
    for (size_t i = userParamStart; i < node.params.size(); ++i) {
        argKinds.push_back(impl_->typeExprToKind(node.params[i].type.get()));
        argTypes.push_back(impl_->typeExprToLLVM(node.params[i].type.get()));
    }
    auto* argsStructType = impl_->makeSpawnArgsStructType(argTypes, "gen.args." + siteName);
    auto* tramp = impl_->buildGeneratorTrampoline(bodyFunc, argsStructType, siteName);
    auto* decrefFn = impl_->buildGeneratorDecrefFn(argsStructType, argKinds, siteName);
    impl_->builder->SetInsertPoint(&wrapper->getEntryBlock());

    // Incref heap captured args (incl self) so they survive past the wrapper's
    // return; the matching decref runs at generator destroy via decrefFn.
    for (unsigned i = 0; i < nwrap; ++i)
        if (Impl::isHeapKind(argKinds[i]) && argKinds[i] != Impl::VarKind::Union)
            impl_->emitIncrefByKind(wrapper->getArg(i), argKinds[i]);

    std::vector<llvm::Value*> userArgs;
    for (unsigned i = 0; i < nwrap; ++i) userArgs.push_back(wrapper->getArg(i));
    auto* argsAlloca = impl_->createEntryAlloca(wrapper, "gen.args", argsStructType);
    impl_->populateSpawnArgs(argsAlloca, argsStructType, userArgs);

    const auto& dl = impl_->module->getDataLayout();
    uint64_t argsSize = dl.getTypeAllocSize(argsStructType);
    auto* argsAsI8 = impl_->builder->CreateBitCast(argsAlloca, impl_->i8PtrType);
    auto* trampAsI8 = impl_->builder->CreateBitCast(tramp, impl_->i8PtrType);
    llvm::Value* decrefAsI8 = decrefFn
        ? impl_->builder->CreateBitCast(decrefFn, impl_->i8PtrType)
        : llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(impl_->i8PtrType));
    auto* genObj = impl_->builder->CreateCall(
        impl_->runtimeFuncs["dragon_generator_create_typed"],
        {trampAsI8, argsAsI8,
         llvm::ConstantInt::get(impl_->i64Type, (int64_t)argsSize), decrefAsI8},
        "gen.obj");
    impl_->builder->CreateRet(genObj);

    impl_->currentFunction = prevFunc;
    if (prevBlock) impl_->builder->SetInsertPoint(prevBlock);
}

void CodeGen::Impl::preregisterDecoratedFunction(FunctionDecl& node) {
    if (node.isExtern || node.isMethod || !node.typeParams.empty()) return;
    if (node.decorators.empty()) return;
    // Same user-decorator filter as the application block in visit(FunctionDecl):
    // built-in flags (staticmethod/classmethod/property, @x.setter) are not
    // runtime wrappers and don't produce an indirect-dispatch global.
    bool hasUser = false;
    for (auto& dec : node.decorators) {
        if (auto* n = dynamic_cast<NameExpr*>(dec.get())) {
            if (n->name == "staticmethod" || n->name == "classmethod" ||
                n->name == "property")
                continue;
        }
        if (auto* a = dynamic_cast<AttributeExpr*>(dec.get())) {
            if (a->attribute == "setter" &&
                dynamic_cast<NameExpr*>(a->object.get()) != nullptr)
                continue;
        }
        hasUser = true;
        break;
    }
    if (!hasUser) return;
    if (decoratedFunctions.count(node.name)) return;  // already registered
    auto* func = module->getFunction(mangleFunc(currentModuleName, node.name));
    if (!func) return;
    auto* gv = new llvm::GlobalVariable(
        *module, i8PtrType, false, llvm::GlobalVariable::InternalLinkage,
        llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context)),
        "__decorated_" + node.name);
    decoratedFunctions[node.name] = gv;
    callableTypes[node.name] = func->getFunctionType();
}

void CodeGen::visit(FunctionDecl& node) {
    // D044 - a generic template (`def f[T](...)`) has free type vars and is
    // never lowered; only its stamped monomorphic instantiations are emitted
    // (they carry empty typeParams). Skip the template.
    if (!node.typeParams.empty()) return;
    // Mirror forwardDeclareFunctions: extern decls keep the bare C symbol
    // name, Dragon defs go through per-module mangling so cross-module
    // collisions on names like `open` / `compress` resolve to distinct
    // LLVM symbols. An aliased extern stores its C symbol in
    // `externSymbol`; the LLVM lookup must match what forwardDeclare used.
    const std::string externLinkName =
        node.externSymbol.empty() ? node.name : node.externSymbol;
    const std::string llvmName = node.isExtern
        ? Impl::userFuncName(externLinkName)
        : Impl::mangleFunc(impl_->currentModuleName, node.name);
    auto* func = impl_->module->getFunction(llvmName);

    // Nested `def` detection. forwardDeclareFunctions only registers
    // top-level functions, so a nested def has no LLVM symbol (`func ==
    // nullptr`). It can also share a user name with a module-level def
    // (Python lets you shadow with a nested def - the local binding wins);
    // in that case `func` IS non-null, but we're emitting from inside
    // another user function's body (currentFunction != mainFunction). Both
    // paths funnel into the closure-style emit below. Methods are skipped
    // - they're handled by class codegen separately.
    bool nested = !node.isMethod &&
                  (!func ||
                   (impl_->currentFunction != nullptr &&
                    impl_->currentFunction != impl_->mainFunction));
    if (nested) {
        emitNestedFunctionDecl(node);
        return;
    }
    if (!func) return;

    // Extern "C" functions are declarations only - no body to emit
    if (node.isExtern) return;

    // Skip if already has a body (was already generated)
    if (!func->empty()) return;

    // Stash the function/method docstring so `.__doc__` lookups (lowered in
    // Attributes.cpp) can lazily emit the `.rodata` constant for it.
    //  - Top-level fns: keyed by LLVM mangled name so cross-module
    //  same-name fns (`os.open`, `gzip.open`) get distinct entries.
    //  - Methods: keyed by (className, methodName) since methods aren't
    //  first-class values - they're only reachable via the
    //  `MyClass.method.__doc__` / `inst.method.__doc__` AttrExpr chain.
    if (node.docstring) {
        if (node.isMethod && !impl_->currentClassName.empty())
            impl_->methodDocstrings[impl_->currentClassName][node.name] = *node.docstring;
        else if (!node.isMethod)
            impl_->functionDocstrings[llvmName] = *node.docstring;
    }

    // D025: track functions returning class descriptors (-> type) or function
    // pointers (-> ptr). Callers use these sets to set the receiving var's
    // VarKind / varIsPtrCallable so subsequent calls dispatch correctly.
    if (auto* retNamed = dynamic_cast<NamedTypeExpr*>(node.returnType.get())) {
        if (retNamed->name == "type")
            impl_->funcReturnsType.insert(node.name);
        else if (retNamed->name == "ptr")
            impl_->funcReturnsPtr.insert(node.name);
    }

    // D027: track functions returning a CLOSURE value so a call result like
    // `g = make_closure()` marks g VarKind::Closure (call site unpacks fn+env
    // instead of executing the env object as code - the escaping-closure
    // SIGSEGV). The authoritative population is the forward-declaration pre-pass
    // (ImplInit.cpp) so it precedes method-body emission; this is a harmless
    // idempotent backstop for any function not covered there.
    if (impl_->functionReturnsClosure(node))
        impl_->funcReturnsClosure.insert(node.name);

    //=== async def: create inner body function + wrapper that spawns vthread ===
    if (node.isAsync) {
        impl_->needsPthread = true;

        // 1. Create the inner body function: foo__async_body(params) -> original_rettype
        std::vector<llvm::Type*> bodyParamTypes;
        for (auto& p : node.params) {
            bodyParamTypes.push_back(impl_->typeExprToLLVM(p.type.get()));
        }
        llvm::Type* bodyRetType = impl_->typeExprToLLVM(node.returnType.get());
        // If return type is void, use i64 so the result can be passed through the vthread
        if (bodyRetType == impl_->voidType) bodyRetType = impl_->i64Type;

        auto* bodyFuncType = llvm::FunctionType::get(bodyRetType, bodyParamTypes, false);
        std::string bodyName = node.name + "__async_body";
        auto* bodyFunc = llvm::Function::Create(
            bodyFuncType, llvm::Function::InternalLinkage, bodyName, impl_->module.get());

        // Generate body function
        auto* prevFunc = impl_->currentFunction;
        auto* prevBlock = impl_->builder->GetInsertBlock();

        auto savedGlobalDecls = impl_->globalDeclaredVars;
        auto savedNonlocalDecls = impl_->nonlocalDeclaredVars;
        impl_->globalDeclaredVars.clear();
        impl_->nonlocalDeclaredVars.clear();
        auto savedScopes = std::move(impl_->scopes);
        impl_->scopes.clear();

        auto* bodyEntry = llvm::BasicBlock::Create(*impl_->context, "entry", bodyFunc);
        impl_->builder->SetInsertPoint(bodyEntry);
        impl_->currentFunction = bodyFunc;
        impl_->pushScope();

        size_t idx = 0;
        for (auto& arg : bodyFunc->args()) {
            std::string paramName = node.params[idx].name;
            arg.setName(paramName);
            auto* alloca = impl_->createEntryAlloca(bodyFunc, paramName, bodyFuncType->getParamType(idx));
            impl_->builder->CreateStore(&arg, alloca);
            auto paramKind = impl_->typeExprToKind(node.params[idx].type.get());
            impl_->setVar(paramName, alloca, paramKind);
            impl_->trackPtrParam(paramName, node.params[idx].type.get());
            // GC: async/fire body params are BORROWED, exactly like a normal
            // function's params (see the sync path below). The reference the
            // body relies on is the spawn-site atomic-incref, which the fire
            // trampoline atomic-decrefs post-call. If the body ALSO decref'd its
            // params (which it did while they were left un-borrowed), that extra
            // decref plus the trampoline's dropped the caller's object to rc 0 -
            // a UAF of `xs` after `await` for `fire worker(xs)`.
            if (Impl::isHeapKind(paramKind))
                impl_->scopes.back().borrowed.insert(paramName);
            idx++;
        }

        for (auto& stmt : node.body) {
            stmt->accept(*this);
        }

        if (!impl_->builder->GetInsertBlock()->getTerminator()) {
            impl_->emitScopeCleanup();
            impl_->builder->CreateRet(
                llvm::Constant::getNullValue(bodyRetType));
        }
        impl_->popScope();
        impl_->scopes = std::move(savedScopes);
        impl_->currentFunction = prevFunc;
        if (prevBlock) impl_->builder->SetInsertPoint(prevBlock);
        impl_->globalDeclaredVars = savedGlobalDecls;
        impl_->nonlocalDeclaredVars = savedNonlocalDecls;

        // 2. Generate wrapper: foo(params) -> ptr (spawns vthread via D030 typed
        //  spawn API, returns handle). Wrapper is a small native-typed
        //  forwarder; the per-callsite trampoline does the real unpack+call.
        auto* wrapEntry = llvm::BasicBlock::Create(*impl_->context, "entry", func);
        impl_->builder->SetInsertPoint(wrapEntry);

        int64_t nargs = (int64_t)node.params.size();

        // Atomic-incref heap params crossing the thread boundary so they
        // survive on the worker until the trampoline atomic-decrefs them.
        std::vector<Impl::VarKind> paramKinds;
        for (int64_t i = 0; i < nargs; i++) {
            auto pk = impl_->typeExprToKind(node.params[i].type.get());
            paramKinds.push_back(pk);
            impl_->emitAtomicIncref(func->getArg(i), pk);
        }

        // Build typed args struct from the body fn's signature.
        std::vector<llvm::Type*> argTypes;
        for (unsigned i = 0; i < bodyFuncType->getNumParams(); i++)
            argTypes.push_back(bodyFuncType->getParamType(i));
        auto* argsStructType = impl_->makeSpawnArgsStructType(
            argTypes, "async.args." + node.name);

        // Synthesize the per-callsite trampoline.
        auto* tramp = impl_->buildFireTrampoline(
            bodyFunc, argsStructType, paramKinds, node.name);

        // Re-enter the wrapper to populate args and emit the spawn call.
        impl_->builder->SetInsertPoint(wrapEntry);

        std::vector<llvm::Value*> userArgs;
        for (int64_t i = 0; i < nargs; i++) userArgs.push_back(func->getArg(i));

        auto* argsAlloca = impl_->createEntryAlloca(
            func, "async.args", argsStructType);
        impl_->populateSpawnArgs(argsAlloca, argsStructType, userArgs);

        const auto& dl = impl_->module->getDataLayout();
        uint64_t argsSize = dl.getTypeAllocSize(argsStructType);

        auto* argsAsI8 = impl_->builder->CreateBitCast(argsAlloca, impl_->i8PtrType);
        auto* trampAsI8 = impl_->builder->CreateBitCast(tramp, impl_->i8PtrType);

        auto* handle = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_vthread_spawn_typed"],
            {trampAsI8, argsAsI8,
             llvm::ConstantInt::get(impl_->i64Type, (int64_t)argsSize)},
            "async.task");
        impl_->builder->CreateRet(handle);

        // Restore insert point back to caller (e.g. main)
        impl_->currentFunction = prevFunc;
        if (prevBlock) impl_->builder->SetInsertPoint(prevBlock);

        return;
    }

    //=== Generator function: create body function + wrapper that returns generator ===
    if (Impl::containsYield(node.body)) {
        impl_->generatorFunctions.insert(node.name);
        // Record the yielded value's VarKind so for-in consumers can type
        // their loop variable correctly (heap yields round-trip as ptr,
        // not raw i64).
        impl_->generatorYieldKinds[node.name] = impl_->inferYieldKind(node.body);
        // Free-function generator: no self, all params are user params.
        emitGeneratorFn(node, func, node.name,
                        /*hasSelf=*/false, /*selfClass=*/"", /*userParamStart=*/0);
        return;
    }

    //=== Normal (non-async, non-generator) function codegen ===
    auto* prevFunc = impl_->currentFunction;
    auto* prevBlock = impl_->builder->GetInsertBlock();

    // Save per-function global/nonlocal state (.py mode)
    auto savedGlobalDecls = impl_->globalDeclaredVars;
    auto savedNonlocalDecls = impl_->nonlocalDeclaredVars;
    impl_->globalDeclaredVars.clear();
    impl_->nonlocalDeclaredVars.clear();

    // Save and replace scope stack - functions get a clean scope so they
    // don't accidentally reference allocas from the enclosing function.
    // Module globals are accessed via GlobalVariable, not via the scope chain.
    auto savedScopes = std::move(impl_->scopes);
    impl_->scopes.clear();
    auto savedUnionMembers = std::move(impl_->unionMemberKinds);
    impl_->unionMemberKinds.clear();
    // 6.12(B): non-neg tracking is per-function (names refer to local
    // allocas, not the enclosing function's `i`).
    auto savedNonNeg = std::move(impl_->knownNonNeg);
    impl_->knownNonNeg.clear();

    // D027.1: each function recomputes which of its OWN locals must be
    // cell-promoted. We collect every name marked `nonlocal` somewhere
    // inside our nested defs/lambdas and subtract our own mutated-captures
    // (which are relayed through us, owned by an even-further-out function).
    auto savedCellPromoted = std::move(impl_->cellPromotedLocals);
    impl_->cellPromotedLocals.clear();
    {
        std::unordered_set<std::string> nestedMutated;
        impl_->collectNestedMutatedCaptures(node.body, nestedMutated);
        std::unordered_set<std::string> ownRelay(
            node.mutatedCapturedVars.begin(), node.mutatedCapturedVars.end());
        for (const auto& n : nestedMutated) {
            if (ownRelay.count(n)) continue;  // forwarded, not owned
            impl_->cellPromotedLocals.insert(n);
        }
    }

    auto* entry = llvm::BasicBlock::Create(*impl_->context, "entry", func);
    impl_->builder->SetInsertPoint(entry);
    impl_->currentFunction = func;
    impl_->pushScope();

    // Create allocas for parameters.
    // D030 Phase 4: union params are a single {i64, i64} box arg - no
    // hidden trailing tag, no funcUnionTagMask consultation.
    auto funcType = func->getFunctionType();
    size_t astIdx = 0;
    size_t llvmIdx = 0;
    for (auto& arg : func->args()) {
        // Skip bare * separator (no name, no LLVM param)
        while (astIdx < node.params.size() && node.params[astIdx].isVarArg && node.params[astIdx].name.empty())
            astIdx++;
        if (astIdx >= node.params.size()) break;

        // *args or **kwargs param: bind as list/dict
        if (node.params[astIdx].isVarArg) {
            std::string paramName = node.params[astIdx].name;
            arg.setName(paramName);
            auto* alloca = impl_->createEntryAlloca(func, paramName, impl_->i8PtrType);
            impl_->builder->CreateStore(&arg, alloca);
            impl_->setVar(paramName, alloca, Impl::VarKind::List);
            impl_->scopes.back().borrowed.insert(paramName);
            // The `*args: T` annotation is the per-element type, so register the
            // list element kind exactly like a `list[T]` param. This is what
            // makes `for a in args` and `args[i]` read native T, matching the
            // monomorphized list the call site packed (visit(CallExpr)). Without
            // it the loop var would default to int and print raw addresses.
            if (TypeExpr* elemTy = node.params[astIdx].type.get()) {
                Impl::VarKind ek = impl_->typeExprToKind(elemTy);
                impl_->varListElemKinds[paramName] = Impl::elemVarKindToTypeKind(ek);
                if (ek == Impl::VarKind::Type)
                    impl_->varListElemIsType.insert(paramName);
                if (auto* nt = dynamic_cast<NamedTypeExpr*>(elemTy)) {
                    if (impl_->classNames.count(nt->name) ||
                        impl_->classFieldKinds.count(nt->name))
                        impl_->varListElemClassName[paramName] = nt->name;
                }
            }
            astIdx++;
            llvmIdx++;
            continue;
        }
        if (node.params[astIdx].isKwArg) {
            std::string paramName = node.params[astIdx].name;
            arg.setName(paramName);
            auto* alloca = impl_->createEntryAlloca(func, paramName, impl_->i8PtrType);
            impl_->builder->CreateStore(&arg, alloca);
            impl_->setVar(paramName, alloca, Impl::VarKind::Dict);
            impl_->scopes.back().borrowed.insert(paramName);
            // Register the dict key/value kinds like a `dict[K,V]` param
            // (trackPtrParam). **kwargs keys are always str; the `**kwargs: T`
            // annotation's `type` is the per-VALUE type. Without this the
            // subscript dispatch keeps checkTag = -1 and falls to the untyped
            // dragon_dict_get (i64), misrouting e.g. an inlined `options[key]`
            // through dragon_int_to_str -> raw pointer.
            impl_->varDictKeyKinds[paramName] = Type::Kind::Str;
            impl_->varDictValueKinds[paramName] = Impl::elemVarKindToTypeKind(
                impl_->typeExprToKind(node.params[astIdx].type.get()));
            astIdx++;
            llvmIdx++;
            continue;
        }

        // Normal param
        std::string paramName = node.params[astIdx].name;
        arg.setName(paramName);
        auto* alloca = impl_->createEntryAlloca(func, paramName, funcType->getParamType(llvmIdx));
        impl_->builder->CreateStore(&arg, alloca);
        auto paramKind = impl_->typeExprToKind(node.params[astIdx].type.get());
        impl_->setVar(paramName, alloca, paramKind);
        impl_->trackPtrParam(paramName, node.params[astIdx].type.get());
        // Track union member kinds for union-typed params
        if (paramKind == Impl::VarKind::Union) {
            impl_->unionMemberKinds[paramName] =
                impl_->typeExprToUnionMembers(node.params[astIdx].type.get());
        }
        // Track class name for class-typed parameters (enables field access)
        if (paramKind == Impl::VarKind::ClassInstance) {
            impl_->bindClassVar(paramName, node.params[astIdx].type.get());
        }
        // GC: mark params as borrowed - caller owns the reference. An `own`
        // param is the exception (docs/002 2.8): the caller MOVED its +1 in,
        // so this callee owns it and scope exit releases it (unless the body
        // consumed it onward, which nulls the slot).
        if (Impl::isHeapKind(paramKind) && !node.params[astIdx].isOwn)
            impl_->scopes.back().borrowed.insert(paramName);
        // An own Lock param: the callee owns the mutex - arm the null-gated
        // scope-exit destroy exactly like a bare Lock local.
        if (node.params[astIdx].isOwn && node.params[astIdx].type) {
            if (auto* nt = dynamic_cast<NamedTypeExpr*>(
                    node.params[astIdx].type.get()))
                if (nt->name == "Lock")
                    impl_->scopes.back().lockDestroyOnExit.insert(paramName);
        }
        astIdx++;
        llvmIdx++;
    }

    // Generate body
    for (auto& stmt : node.body) {
        stmt->accept(*this);
    }

    // GC: emit scope cleanup before implicit return
    if (!impl_->builder->GetInsertBlock()->getTerminator()) {
        impl_->emitScopeCleanup();
        if (func->getReturnType() == impl_->voidType) {
            impl_->builder->CreateRetVoid();
        } else {
            impl_->builder->CreateRet(
                llvm::Constant::getNullValue(func->getReturnType()));
        }
    }

    impl_->popScope();

    // Restore enclosing scope stack, function, and insert point
    impl_->scopes = std::move(savedScopes);
    impl_->unionMemberKinds = std::move(savedUnionMembers);
    impl_->knownNonNeg = std::move(savedNonNeg);
    impl_->cellPromotedLocals = std::move(savedCellPromoted);
    impl_->currentFunction = prevFunc;
    if (prevBlock) impl_->builder->SetInsertPoint(prevBlock);

    // Restore per-function global/nonlocal state
    impl_->globalDeclaredVars = savedGlobalDecls;
    impl_->nonlocalDeclaredVars = savedNonlocalDecls;

    // D024: Apply user-defined decorators (bottom-up, skip staticmethod/classmethod)
    if (!node.decorators.empty()) {
        // Collect user-defined decorators (skip built-in flags already handled by parser)
        std::vector<Expr*> userDecorators;
        for (auto& dec : node.decorators) {
            if (auto* n = dynamic_cast<NameExpr*>(dec.get())) {
                if (n->name == "staticmethod" || n->name == "classmethod" ||
                    n->name == "property")
                    continue;
            }
            // @<name>.setter is handled at attribute-access site, not as a runtime wrapper
            if (auto* a = dynamic_cast<AttributeExpr*>(dec.get())) {
                if (a->attribute == "setter" &&
                    dynamic_cast<NameExpr*>(a->object.get()) != nullptr)
                    continue;
            }
            userDecorators.push_back(dec.get());
        }

        if (!userDecorators.empty()) {
            // Start with the function pointer as i8*
            llvm::Value* current = impl_->builder->CreateBitCast(func, impl_->i8PtrType);

            // Apply decorators bottom-up (last decorator in source is applied first)
            for (int i = (int)userDecorators.size() - 1; i >= 0; i--) {
                auto* decExpr = userDecorators[i];
                // Resolve decorator to a callable
                llvm::Function* decFn = nullptr;
                if (auto* nameExpr = dynamic_cast<NameExpr*>(decExpr)) {
                    decFn = impl_->module->getFunction(nameExpr->name);
                }
                if (decFn) {
                    // Call decorator(fn_ptr) -> returns new fn_ptr
                    llvm::Value* arg = current;
                    // Coerce arg to match decorator's first param type
                    if (decFn->getFunctionType()->getNumParams() > 0) {
                        auto* paramType = decFn->getFunctionType()->getParamType(0);
                        arg = impl_->coerceArg(arg, paramType);
                    }
                    llvm::Value* result = impl_->builder->CreateCall(decFn, {arg}, "decorated");
                    // Coerce result back to i8* for chaining
                    if (result->getType() != impl_->i8PtrType) {
                        if (result->getType() == impl_->i64Type)
                            result = impl_->builder->CreateIntToPtr(result, impl_->i8PtrType);
                        else if (result->getType()->isPointerTy())
                            result = impl_->builder->CreateBitCast(result, impl_->i8PtrType);
                    }
                    current = result;
                } else {
                    // Decorator is not a bare NameExpr - evaluate the expression.
                    // The result may be a DragonClosure* rather than a bare code
                    // pointer: a decorator FACTORY (`@register("x")`) whose inner
                    // decorator captures the factory argument returns a capturing
                    // closure, and calling that directly as a function pointer
                    // jumps into the closure struct's bytes (SIGSEGV). Route
                    // through the shared closure-vs-bare runtime discrimination -
                    // the same path a normal Callable-value call uses. A
                    // non-capturing decorator (bare fn ptr / null-env closure)
                    // takes the bare branch at the cost of one tag-byte check.
                    decExpr->accept(*this);
                    llvm::Value* decVal = impl_->lastValue;
                    if (!decVal->getType()->isPointerTy())
                        decVal = impl_->builder->CreateIntToPtr(decVal, impl_->i8PtrType);
                    if (current->getType() != impl_->i8PtrType)
                        current = impl_->builder->CreateBitCast(current, impl_->i8PtrType);
                    // Decorator ABI signature: (i8* fn) -> i8* fn. ownedClosure
                    // frees the transient factory-produced closure after one use.
                    auto* decoFnType = llvm::FunctionType::get(
                        impl_->i8PtrType, {impl_->i8PtrType}, false);
                    emitCallableValueCall(decVal, decoFnType, {current},
                                          /*ownedClosure=*/true, "decapply");
                    current = impl_->lastValue;
                    if (current->getType() != impl_->i8PtrType) {
                        if (current->getType() == impl_->i64Type)
                            current = impl_->builder->CreateIntToPtr(
                                current, impl_->i8PtrType);
                        else if (current->getType()->isPointerTy())
                            current = impl_->builder->CreateBitCast(
                                current, impl_->i8PtrType);
                    }
                }
            }

            // Store decorated result in a module global. Reuse the global the
            // decorator pre-pass already created (so a method body emitted
            // earlier loads the SAME symbol this stores into); create it here
            // only when this function wasn't pre-registered.
            llvm::GlobalVariable* gv = nullptr;
            auto preIt = impl_->decoratedFunctions.find(node.name);
            if (preIt != impl_->decoratedFunctions.end()) {
                gv = preIt->second;
            } else {
                gv = new llvm::GlobalVariable(
                    *impl_->module, impl_->i8PtrType, false,
                    llvm::GlobalVariable::InternalLinkage,
                    llvm::ConstantPointerNull::get(
                        llvm::PointerType::getUnqual(*impl_->context)),
                    "__decorated_" + node.name);
                impl_->decoratedFunctions[node.name] = gv;
            }
            impl_->builder->CreateStore(current, gv);
            // Track callable type for indirect dispatch
            impl_->callableTypes[node.name] = func->getFunctionType();
        }
    }
}

void CodeGen::visit(TypeAliasStmt& node) {
    // Type aliases are compile-time only - no code generation needed
}

/// Emit a nested `def` (one defined inside another function's body).
/// Lowers to the same shape as a capturing lambda: a top-level mangled
/// LLVM function that takes user params + an optional trailing i8* env,
/// plus a heap-allocated env populated from the enclosing scope. The
/// user-visible name is bound to either the bare fn-pointer (no captures)
/// or the closure object as a local in the enclosing scope, so calls in
/// that scope dispatch through the first-class-function path. While the
/// inner body is being emitted - with the enclosing scope chain saved
/// off - calls to the function's own name resolve through
/// nestedFunctionAliases to a direct LLVM call.
void CodeGen::emitNestedFunctionDecl(FunctionDecl& node) {
    bool hasCaptures = !node.capturedVars.empty();

    // 1. Mangled LLVM name - unique per nested def (siblings can't collide).
    std::string mangledName =
        "__dragon_nested_" + std::to_string(impl_->lambdaCounter++) + "__" + node.name;

    // 2. Determine return type and user param types.
    llvm::Type* retType = impl_->typeExprToLLVM(node.returnType.get());
    if (!node.returnType) retType = impl_->unannotatedReturnType(node.body);

    std::vector<llvm::Type*> userParamTypes;
    userParamTypes.reserve(node.params.size());
    for (auto& p : node.params) {
        userParamTypes.push_back(impl_->typeExprToLLVM(p.type.get()));
    }

    // 3. Full param types: user params + optional trailing i8* env.
    std::vector<llvm::Type*> paramTypes = userParamTypes;
    if (hasCaptures) paramTypes.push_back(impl_->i8PtrType);

    auto* funcType = llvm::FunctionType::get(retType, paramTypes, false);
    auto* userFnType = llvm::FunctionType::get(retType, userParamTypes, false);
    auto* nestedFunc = llvm::Function::Create(
        funcType, llvm::Function::InternalLinkage, mangledName, impl_->module.get());

    // 4. Gather capture info from the enclosing scope BEFORE switching contexts.
    // D027.1: a capture flagged `nonlocal` by the inner fn (`mutatedCapturedVars`)
    // must travel as a cell pointer - otherwise mutations inside the inner
    // wouldn't propagate back. The outer is required to have the name
    // cell-backed already (either it owns the cell-promoted local, or it
    // received the cell ptr from a further-out fn). At env-load time the
    // alloca's allocated type is i8* (cell ptr); we store that pointer
    // verbatim into the env, which is what dragon_cell_get/set on the inner
    // side will dereference.
    std::unordered_set<std::string> innerCellRelayed(
        node.mutatedCapturedVars.begin(), node.mutatedCapturedVars.end());
    struct CaptureInfo {
        std::string name;
        llvm::Value* value;
        Impl::VarKind kind;
        std::string className;
        bool isCellRelay = false;  // env field carries a cell ptr, not the value
    };
    std::vector<CaptureInfo> captures;
    if (hasCaptures) {
        for (auto& capName : node.capturedVars) {
            CaptureInfo ci;
            ci.name = capName;
            ci.kind = impl_->lookupVarKind(capName);
            ci.isCellRelay = innerCellRelayed.count(capName) > 0;
            auto cnIt = impl_->varClassNames.find(capName);
            if (cnIt != impl_->varClassNames.end())
                ci.className = cnIt->second;
            auto* alloca = impl_->lookupVar(capName);
            if (alloca) {
                // For cell-relayed captures: alloca holds the cell ptr (i8*),
                // which is exactly what we want to forward.
                ci.value = impl_->builder->CreateLoad(
                    alloca->getAllocatedType(), alloca, capName + ".cap");
            } else {
                auto* gv = impl_->lookupModuleGlobal(capName);
                if (gv) {
                    ci.value = impl_->builder->CreateLoad(
                        gv->getValueType(), gv, capName + ".cap");
                } else {
                    ci.value = llvm::ConstantInt::get(impl_->i64Type, 0);
                }
            }
            captures.push_back(ci);
        }
    }

    // 5. Save enclosing context.
    auto* prevFunc = impl_->currentFunction;
    auto* prevBlock = impl_->builder->GetInsertBlock();
    auto savedScopes = std::move(impl_->scopes);
    impl_->scopes.clear();
    auto savedGlobalDecls = impl_->globalDeclaredVars;
    auto savedNonlocalDecls = impl_->nonlocalDeclaredVars;
    impl_->globalDeclaredVars.clear();
    impl_->nonlocalDeclaredVars.clear();
    // D027.1: nested fn computes its own cell-promoted locals from its body.
    auto savedCellPromoted = std::move(impl_->cellPromotedLocals);
    impl_->cellPromotedLocals.clear();
    {
        std::unordered_set<std::string> nestedMutated;
        impl_->collectNestedMutatedCaptures(node.body, nestedMutated);
        std::unordered_set<std::string> ownRelay(
            node.mutatedCapturedVars.begin(), node.mutatedCapturedVars.end());
        for (const auto& n : nestedMutated) {
            if (ownRelay.count(n)) continue;
            impl_->cellPromotedLocals.insert(n);
        }
    }

    // 6. Set up nested function context.
    impl_->currentFunction = nestedFunc;
    auto* entry = llvm::BasicBlock::Create(*impl_->context, "entry", nestedFunc);
    impl_->builder->SetInsertPoint(entry);
    impl_->pushScope();

    // 7. Allocas for user params.
    size_t idx = 0;
    for (auto& arg : nestedFunc->args()) {
        if (idx >= node.params.size()) break;  // skip env param
        std::string paramName = node.params[idx].name;
        arg.setName(paramName);
        auto* alloca = impl_->createEntryAlloca(
            nestedFunc, paramName, funcType->getParamType(idx));
        impl_->builder->CreateStore(&arg, alloca);
        auto paramKind = impl_->typeExprToKind(node.params[idx].type.get());
        impl_->setVar(paramName, alloca, paramKind);
        impl_->trackPtrParam(paramName, node.params[idx].type.get());
        if (paramKind == Impl::VarKind::ClassInstance) {
            impl_->bindClassVar(paramName, node.params[idx].type.get());
        }
        if (Impl::isHeapKind(paramKind))
            impl_->scopes.back().borrowed.insert(paramName);
        idx++;
    }

    // 8. Build the env struct type (mirrors LambdaExpr's layout: 24-byte
    //  header + native-typed capture fields).
    auto kindToCaptureLLVM = [&](Impl::VarKind k) -> llvm::Type* {
        switch (k) {
            case Impl::VarKind::Float: return impl_->f64Type;
            case Impl::VarKind::Bool:  return impl_->i1Type;
            case Impl::VarKind::Str:
            case Impl::VarKind::StrLiteral:
            case Impl::VarKind::List:
            case Impl::VarKind::Dict:
            case Impl::VarKind::Tuple:
            case Impl::VarKind::Set:
            case Impl::VarKind::File:
            case Impl::VarKind::ClassInstance:
            case Impl::VarKind::Generator:
            case Impl::VarKind::Closure:
                return impl_->i8PtrType;
            default:
                return impl_->i64Type;
        }
    };

    llvm::StructType* envStructType = nullptr;
    if (hasCaptures) {
        std::vector<llvm::Type*> envFields;
        envFields.push_back(llvm::ArrayType::get(
            llvm::Type::getInt8Ty(*impl_->context), 24));
        for (auto& cap : captures) {
            // D027.1: cell-relayed captures always carry an i8* cell ptr
            // regardless of the source variable's native type.
            envFields.push_back(cap.isCellRelay
                ? impl_->i8PtrType : kindToCaptureLLVM(cap.kind));
        }
        envStructType = llvm::StructType::create(
            *impl_->context, envFields, mangledName + ".env");
    }

    // 9. Unpack captures from env at body entry.
    llvm::Value* envArgValue = nullptr;
    if (hasCaptures) {
        envArgValue = &*(nestedFunc->arg_end() - 1);
        envArgValue->setName("__env");
        llvm::Value* envTyped = impl_->builder->CreateBitCast(
            envArgValue, llvm::PointerType::getUnqual(*impl_->context), "__env.typed");

        for (size_t i = 0; i < captures.size(); i++) {
            auto& cap = captures[i];
            llvm::Type* fieldType = envStructType->getElementType((unsigned)(i + 1));
            auto* fieldPtr = impl_->builder->CreateStructGEP(
                envStructType, envTyped, (unsigned)(i + 1), cap.name + ".env.ptr");
            auto* typedVal = impl_->builder->CreateLoad(
                fieldType, fieldPtr, cap.name + ".env");
            auto* alloca = impl_->createEntryAlloca(nestedFunc, cap.name, fieldType);
            impl_->builder->CreateStore(typedVal, alloca);
            impl_->setVar(cap.name, alloca, cap.kind);
            if (!cap.className.empty())
                impl_->varClassNames[cap.name] = cap.className;
            // Captures are borrowed from the env - don't decref at scope exit.
            impl_->scopes.back().borrowed.insert(cap.name);
            // D027.1: cell-relayed captures - the alloca holds the cell ptr;
            // route reads/writes through dragon_cell_get/set so mutations
            // propagate back to the same backing slot the owner mutates.
            if (cap.isCellRelay) {
                impl_->markCellBacked(cap.name);
            }
        }
    }

    // 10. Install the alias so `<funcname>(...)` inside the body resolves
    //  to a direct LLVM call (with env auto-appended for capturing).
    Impl::NestedAliasInfo savedAlias;
    bool hadPriorAlias = false;
    {
        auto it = impl_->nestedFunctionAliases.find(node.name);
        if (it != impl_->nestedFunctionAliases.end()) {
            savedAlias = it->second;
            hadPriorAlias = true;
        }
        Impl::NestedAliasInfo info;
        info.fn = nestedFunc;
        info.userFnType = userFnType;
        info.envValue = envArgValue;
        impl_->nestedFunctionAliases[node.name] = info;
    }

    // 11. Emit body.
    for (auto& stmt : node.body) {
        stmt->accept(*this);
    }
    if (!impl_->builder->GetInsertBlock()->getTerminator()) {
        impl_->emitScopeCleanup();
        if (retType == impl_->voidType) {
            impl_->builder->CreateRetVoid();
        } else {
            impl_->builder->CreateRet(llvm::Constant::getNullValue(retType));
        }
    }

    // 12. Restore prior alias state for `node.name`. Sibling defs and the
    //  enclosing scope use the local-variable closure binding instead.
    if (hadPriorAlias) {
        impl_->nestedFunctionAliases[node.name] = savedAlias;
    } else {
        impl_->nestedFunctionAliases.erase(node.name);
    }

    // 13. Restore enclosing context.
    impl_->popScope();
    impl_->scopes = std::move(savedScopes);
    impl_->globalDeclaredVars = std::move(savedGlobalDecls);
    impl_->nonlocalDeclaredVars = std::move(savedNonlocalDecls);
    impl_->cellPromotedLocals = std::move(savedCellPromoted);
    impl_->currentFunction = prevFunc;
    if (prevBlock) impl_->builder->SetInsertPoint(prevBlock);

    // 14. Bind the name in the enclosing scope. For non-capturing nested
    //  defs the binding is the bare fn pointer; for capturing variants
    //  it's a freshly allocated closure object.
    llvm::Value* boundValue = nullptr;
    Impl::VarKind boundKind = Impl::VarKind::Other;
    bool isClosure = false;

    if (!hasCaptures) {
        boundValue = impl_->builder->CreateBitCast(nestedFunc, impl_->i8PtrType);
        boundKind = Impl::VarKind::Other;  // bare fn pointer - not a heap object
    } else {
        // multi-op env GC hook + gc-track gate (see LambdaExpr).
        // Shared emitter so the nested-def and lambda sites never drift.
        std::vector<Impl::EnvCaptureDesc> capDescs;
        capDescs.reserve(captures.size());
        bool envTrackable = false;
        for (auto& cap : captures) {
            capDescs.push_back({cap.kind, cap.isCellRelay});
            if (Impl::envCaptureIsCyclic(cap.kind, cap.isCellRelay))
                envTrackable = true;
        }
        auto* gcFn = impl_->emitEnvGcFn(mangledName, envStructType, capDescs);

        const auto& dl = impl_->module->getDataLayout();
        uint64_t envSize = dl.getTypeAllocSize(envStructType);

        auto* envVal = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_env_alloc"],
            {llvm::ConstantInt::get(impl_->i64Type, (int64_t)envSize),
             impl_->builder->CreateBitCast(gcFn, impl_->i8PtrType),
             llvm::ConstantInt::get(llvm::Type::getInt32Ty(*impl_->context),
                                    envTrackable ? 1 : 0)},
            "closure.env");

        llvm::Value* envTyped = impl_->builder->CreateBitCast(
            envVal, llvm::PointerType::getUnqual(*impl_->context), "closure.env.typed");

        for (size_t i = 0; i < captures.size(); i++) {
            auto& cap = captures[i];
            llvm::Type* fieldType = envStructType->getElementType((unsigned)(i + 1));
            llvm::Value* storeVal = cap.value;
            if (storeVal->getType() != fieldType) {
                if (fieldType == impl_->f64Type && storeVal->getType() == impl_->i64Type)
                    storeVal = impl_->builder->CreateSIToFP(storeVal, fieldType);
                else if (fieldType == impl_->i64Type && storeVal->getType() == impl_->i1Type)
                    storeVal = impl_->builder->CreateZExt(storeVal, fieldType);
                else if (fieldType == impl_->i8PtrType && storeVal->getType()->isIntegerTy())
                    storeVal = impl_->builder->CreateIntToPtr(storeVal, fieldType);
                else if (fieldType->isIntegerTy() && storeVal->getType()->isPointerTy())
                    storeVal = impl_->builder->CreatePtrToInt(storeVal, fieldType);
                else
                    storeVal = impl_->builder->CreateBitCast(storeVal, fieldType);
            }
            auto* fieldPtr = impl_->builder->CreateStructGEP(
                envStructType, envTyped, (unsigned)(i + 1), cap.name + ".env.slot");
            impl_->builder->CreateStore(storeVal, fieldPtr);
            if (impl_->options.gcMode == GCMode::RC) {
                if (cap.isCellRelay) {
                    // The env owns a refcount on the cell itself (TAG_CELL).
                    // Plain dragon_incref - `_str` would mis-navigate through
                    // the cell's bytes treating them as a string payload.
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_incref"], {storeVal});
                } else if (Impl::isHeapKind(cap.kind)) {
                    if (cap.kind == Impl::VarKind::Str) {
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_incref_str"], {storeVal});
                    } else if (cap.kind == Impl::VarKind::Closure) {
                        // tag-gated - a captured Callable may hold a
                        // BARE fn ptr; the generic incref would write a
                        // refcount into code bytes (SIGSEGV).
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_incref_callable"], {storeVal});
                    } else {
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_incref"], {storeVal});
                    }
                }
            }
        }

        boundValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_closure_create"],
            {impl_->builder->CreateBitCast(nestedFunc, impl_->i8PtrType), envVal},
            "closure");
        boundKind = Impl::VarKind::Closure;
        isClosure = true;
    }

    // 15. Allocate a local in the enclosing scope and store the bound value.
    //  callableTypes[name] gives the user-visible signature so calls
    //  through the local var dispatch with the right ABI.
    auto* localAlloca = impl_->createEntryAlloca(
        prevFunc, node.name, impl_->i8PtrType);
    impl_->builder->CreateStore(boundValue, localAlloca);
    impl_->setVar(node.name, localAlloca, boundKind);
    impl_->callableTypes[node.name] = userFnType;
    if (!isClosure) {
        impl_->varIsPtrCallable.insert(node.name);
    }
}

} // namespace dragon
