/// Dragon CodeGen - Lambda, Function Declaration, TypeAlias
#include "../CodeGenImpl.h"

#include <optional>

namespace dragon {

// Shared per-closure-site env GC hook (Lambda + nested def) so DEALLOC/TRAVERSE/CLEAR match.
// TRAVERSE's bare-fn-ptr Closure capture is a safe hash-miss; CLEAR nulls slots so a later dealloc frees 0.
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
    // Per-kind decref (cell/str/closure/generic), reused by DEALLOC and CLEAR.
    // Closure goes through tag-gated _callable (bare-fn-ptr safe).
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

    // Propagate SHARED into every heap capture, str included: a capture read
    // out of a shared closure increfs, so unmarked refcounts tear cross-thread.
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
    // D027: capturedVars non-empty makes a closure (env + wrapper); otherwise a bare fn ptr.
    // Per-variable metadata dies with the lambda body (stale-type family).
    Impl::VarMetaScope _varMeta(*impl_);

    bool hasCaptures = !node.capturedVars.empty();

    std::string lambdaName = "__dragon_lambda_" + std::to_string(impl_->lambdaCounter++);

    // Expression-body lambdas default to int; a block lambda with no value-return is void.
    llvm::Type* retType = impl_->typeExprToLLVM(node.returnType.get());
    if (!node.returnType) {
        retType = (node.body || impl_->bodyReturnsValue(node.bodyStmts))
            ? impl_->i64Type : impl_->voidType;
    }

    std::vector<llvm::Type*> userParamTypes;
    for (auto& p : node.params) {
        userParamTypes.push_back(impl_->typeExprToLLVM(p.type.get()));
    }

    std::vector<llvm::Type*> paramTypes = userParamTypes;
    if (hasCaptures) {
        paramTypes.push_back(impl_->i8PtrType);  // env pointer
    }

    auto* funcType = llvm::FunctionType::get(retType, paramTypes, false);
    auto* lambdaFunc = llvm::Function::Create(
        funcType, llvm::Function::InternalLinkage, lambdaName, impl_->module.get());

    // D027: Gather capture info BEFORE switching context (load values from the
    // caller scope). D027.1: `nonlocal` captures relay as cell ptrs (cell_get/set).
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

    impl_->currentFunction = lambdaFunc;
    auto* entry = llvm::BasicBlock::Create(*impl_->context, "entry", lambdaFunc);
    impl_->builder->SetInsertPoint(entry);

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

    // D030: Per-lambda env struct { [24 x i8] header+dealloc_fn; native capture
    // fields... }. Used at body unpack, capture populate, and dealloc fn.
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
        llvm::Value* envTyped = impl_->builder->CreateBitCast(
            envArg, llvm::PointerType::getUnqual(*impl_->context), "__env.typed");

        for (size_t i = 0; i < captures.size(); i++) {
            auto& cap = captures[i];
            llvm::Type* fieldType = envStructType->getElementType((unsigned)(i + 1));

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
                // emitIncrefByKind dispatches Str/Closure/generic; Closure is
                // tag-gated (a returned bare-fn Callable has no refcount slot).
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

    impl_->popScope();
    impl_->scopes = std::move(savedScopes);
    impl_->cellPromotedLocals = std::move(savedCellPromoted);
    impl_->currentFunction = prevFunc;
    if (prevBlock) impl_->builder->SetInsertPoint(prevBlock);

    if (hasCaptures) {
        // Emit the shared multi-op env GC hook, gc-track the env iff it captures
        // a cycle-capable object (scalar/str-only envs stay untracked - #1).
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

        llvm::Value* envTyped = impl_->builder->CreateBitCast(
            envVal, llvm::PointerType::getUnqual(*impl_->context), "closure.env.typed");

        // Populate captures with native-typed stores; coerce only when the
        // loaded value's type doesn't match the field type (legitimate widening).
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
                // Otherwise: assume bitcast-compatible (rare; LLVM will assert if not)
                else
                    storeVal = impl_->builder->CreateBitCast(storeVal, fieldType);
            }

            auto* fieldPtr = impl_->builder->CreateStructGEP(
                envStructType, envTyped, (unsigned)(i + 1), cap.name + ".env.slot");
            impl_->builder->CreateStore(storeVal, fieldPtr);

            // Incref heap captures - env now holds a ref. Cell-relayed travel as
            // TAG_CELL, so plain dragon_incref (not _str, which misreads the cell).
            if (impl_->options.gcMode == GCMode::RC) {
                if (cap.isCellRelay) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_incref"], {storeVal});
                } else if (Impl::isHeapKind(cap.kind)) {
                    if (cap.kind == Impl::VarKind::Str) {
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_incref_str"], {storeVal});
                    } else if (cap.kind == Impl::VarKind::Closure) {
                        // A captured Callable may hold a BARE fn ptr; generic incref
                        // would write into code bytes (SIGSEGV). Tag-gated variant is safe.
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_incref_callable"], {storeVal});
                    } else {
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_incref"], {storeVal});
                    }
                }
            }
        }

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

// D027.1: Recursively collect nested-fn `mutatedCapturedVars`. Names nonlocal-
// mutated in nested defs/lambdas, intersected with own locals, get cell-promoted.

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
        // The nested fn's own `nonlocal` decls surface as cell-backings here;
        // recurse to pick up grandchildren whose targets bind further out.
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
        // ThreadStmt blocks are capture sites: their mutatedCapturedVars bubble
        // up so the owning function cell-promotes the right locals.
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
        // FireExpr wraps either a CallExpr (via `operand`) or a body block
        // (via `bodyStmts`); recurse into both.
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
// D027 closure-return analysis: mark the receiving var VarKind::Closure only
// when EVERY value-return is provably a closure (mis-marking crashes dispatch).

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

// Builds the inner generator body fn, then fills `wrapper` with args struct + trampoline + decref fn returning the gen object.
// `hasSelf` threads self as a heap arg the gen owns (wrapper increfs, destroy decrefs); `userParamStart` skips self/cls.
void CodeGen::emitGeneratorFn(FunctionDecl& node, llvm::Function* wrapper,
                              const std::string& siteName, bool hasSelf,
                              const std::string& selfClass, size_t userParamStart) {
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

    // Wrapper body creates + returns the generator object; its LLVM args ARE the
    // captured args, in order: [self?, params...].
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
    // Same user-decorator filter as visit(FunctionDecl): built-in flags
    // (staticmethod/classmethod/property, @x.setter) produce no dispatch global.
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
    // D044 - a generic template (`def f[T](...)`) is never lowered; only its
    // stamped monomorphic instantiations (empty typeParams) are emitted.
    if (!node.typeParams.empty()) return;
    // Mirror forwardDeclareFunctions: externs keep the bare C symbol, Dragon
    // defs get per-module mangling; aliased externs use `externSymbol`.
    const std::string externLinkName =
        node.externSymbol.empty() ? node.name : node.externSymbol;
    const std::string llvmName = node.isExtern
        ? Impl::userFuncName(externLinkName)
        : Impl::mangleFunc(impl_->currentModuleName, node.name);
    auto* func = impl_->module->getFunction(llvmName);

    // Nested `def` detection: no LLVM symbol (func == nullptr), or shadows a
    // module-level def while emitting inside another fn body. Methods handled separately.
    bool nested = !node.isMethod &&
                  (!func ||
                   (impl_->currentFunction != nullptr &&
                    impl_->currentFunction != impl_->mainFunction));
    if (nested) {
        emitNestedFunctionDecl(node);
        return;
    }
    if (!func) return;
    // Per-variable metadata dies with this body (stale-type family). The nested
    // path scopes its own guard so the parent-scope binding it installs survives.
    Impl::VarMetaScope _varMeta(*impl_);

    // Extern "C" functions are declarations only - no body to emit
    if (node.isExtern) return;

    // Skip if already has a body (was already generated)
    if (!func->empty()) return;

    // D052 - stamped cross-module generic: symbol resolved above under the instantiation
    // module; swap to genericHomeModule for the body so bare calls/globals resolve there.
    const std::string _savedModForGeneric = impl_->currentModuleName;
    struct RestoreModuleName {
        std::string* slot; std::string saved; bool active;
        ~RestoreModuleName() { if (active) *slot = saved; }
    } _restoreModuleName{&impl_->currentModuleName, _savedModForGeneric,
                         !node.genericHomeModule.empty()};
    if (!node.genericHomeModule.empty())
        impl_->currentModuleName = node.genericHomeModule;

    // Stash the docstring so `.__doc__` (Attributes.cpp) can lazily emit .rodata.
    // Top-level keyed by mangled name (cross-module distinct); methods by (class, method).
    if (node.docstring) {
        if (node.isMethod && !impl_->currentClassName.empty())
            impl_->methodDocstringsBySym[impl_->classSym(impl_->currentClassName)][node.name] = *node.docstring;
        else if (!node.isMethod)
            impl_->functionDocstrings[llvmName] = *node.docstring;
    }

    // D025: track functions returning class descriptors (-> type) or fn ptrs
    // (-> ptr); callers set the receiving var's VarKind/varIsPtrCallable.
    if (auto* retNamed = dynamic_cast<NamedTypeExpr*>(node.returnType.get())) {
        if (retNamed->name == "type")
            impl_->funcReturnsType.insert(llvmName);
        else if (retNamed->name == "ptr")
            impl_->funcReturnsPtr.insert(llvmName);
    }

    // D027: track fns returning a CLOSURE so `g = make_closure()` marks g Closure
    // (call site unpacks fn+env, avoiding SIGSEGV). Idempotent backstop; see ImplInit.
    if (impl_->functionReturnsClosure(node))
        impl_->funcReturnsClosure.insert(llvmName);

    // async def: inner body fn + wrapper that spawns vthread
    if (node.isAsync) {
        impl_->needsPthread = true;

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
            // GC: async/fire body params are BORROWED like normal params (ref from
            // spawn-site atomic-incref); a body decref too would UAF `xs` after await.
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

        // D030: wrapper foo(params) -> ptr spawns a vthread via the typed spawn API;
        // wrapper forwards, the per-callsite trampoline unpacks and calls.
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

        auto* tramp = impl_->buildFireTrampoline(
            bodyFunc, argsStructType, paramKinds, node.name);

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

    // Generator function: body fn + wrapper that returns generator
    if (Impl::containsYield(node.body)) {
        impl_->generatorFunctions.insert(node.name);
        // Record the yielded value's VarKind so for-in consumers type their loop
        // var correctly (heap yields round-trip as ptr, not raw i64).
        impl_->generatorYieldKinds[node.name] = impl_->inferYieldKind(node.body);
        // Free-function generator: no self, all params are user params.
        emitGeneratorFn(node, func, node.name,
                        /*hasSelf=*/false, /*selfClass=*/"", /*userParamStart=*/0);
        return;
    }

    // Normal (non-async, non-generator) function codegen
    auto* prevFunc = impl_->currentFunction;
    auto* prevBlock = impl_->builder->GetInsertBlock();

    // Save per-function global/nonlocal state (.py mode)
    auto savedGlobalDecls = impl_->globalDeclaredVars;
    auto savedNonlocalDecls = impl_->nonlocalDeclaredVars;
    impl_->globalDeclaredVars.clear();
    impl_->nonlocalDeclaredVars.clear();

    // Save and replace scope stack - functions get a clean scope, not the
    // enclosing allocas. Module globals go through GlobalVariable.
    auto savedScopes = std::move(impl_->scopes);
    impl_->scopes.clear();
    auto savedUnionMembers = std::move(impl_->unionMemberKinds);
    impl_->unionMemberKinds.clear();
    // 6.12(B): non-neg tracking is per-function (names refer to local
    // allocas, not the enclosing function's `i`).
    auto savedNonNeg = std::move(impl_->knownNonNeg);
    impl_->knownNonNeg.clear();

    // D027.1: recompute which OWN locals to cell-promote: names marked `nonlocal`
    // in nested defs/lambdas, minus our own mutated-captures (relayed, not owned).
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

    // Create allocas for parameters. D030 Phase 4: union params are a single
    // {i64, i64} box arg - no hidden trailing tag, no funcUnionTagMask.
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
            // `*args: T` is the per-element type: register the list elem kind like
            // a `list[T]` param so `for a in args`/`args[i]` read native T, not int.
            if (TypeExpr* elemTy = node.params[astIdx].type.get()) {
                Impl::VarKind ek = impl_->typeExprToKind(elemTy);
                impl_->varListElemKinds[paramName] = Impl::elemVarKindToTypeKind(ek);
                if (ek == Impl::VarKind::Type)
                    impl_->varListElemIsType.insert(paramName);
                if (auto* nt = dynamic_cast<NamedTypeExpr*>(elemTy)) {
                    if (impl_->classNames.count(nt->name) ||
                        impl_->classFieldKindsBySym.count(impl_->classSym(nt->name)))
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
            // Register dict key/value kinds like `dict[K,V]`: **kwargs keys are str,
            // `**kwargs: T` is the value type. Else subscript falls to untyped dict_get.
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
        if (paramKind == Impl::VarKind::Union) {
            impl_->unionMemberKinds[paramName] =
                impl_->typeExprToUnionMembers(node.params[astIdx].type.get());
        }
        // Track class name for class-typed parameters (enables field access)
        if (paramKind == Impl::VarKind::ClassInstance) {
            impl_->bindClassVar(paramName, node.params[astIdx].type.get());
        }
        // GC: params are borrowed - caller owns the ref. An `own` param (docs/002
        // 2.8) moved its +1 in, so the callee owns it and scope exit releases it.
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
                    // Decorator not a bare NameExpr - evaluate it. A factory closure
                    // SIGSEGVs if called as a bare fn ptr; route via closure-vs-bare.
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

            // Store decorated result in a module global. Reuse the pre-pass global
            // (same symbol a method body loads); create here only if not pre-registered.
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

// ADR 054 - contracts are compile-time shapes: no code, no symbols. The
// registry work happened in collectContracts/assignContractSlots.
void CodeGen::visit(ContractDecl&) {}

void CodeGen::visit(ContractSetTypeExpr&) {}

void CodeGen::visit(TypeAliasStmt& node) {
    // Type aliases are compile-time only - no code generation needed
}

/// Emits a nested `def` like a capturing lambda: mangled fn (params + optional trailing
/// i8* env) plus heap env; binds to a bare fn ptr or closure via nestedFunctionAliases.
void CodeGen::emitNestedFunctionDecl(FunctionDecl& node) {
    bool hasCaptures = !node.capturedVars.empty();

    // Mangled LLVM name: unique per nested def so sibling defs can't collide.
    std::string mangledName =
        "__dragon_nested_" + std::to_string(impl_->lambdaCounter++) + "__" + node.name;

    llvm::Type* retType = impl_->typeExprToLLVM(node.returnType.get());
    if (!node.returnType) retType = impl_->unannotatedReturnType(node.body);

    std::vector<llvm::Type*> userParamTypes;
    userParamTypes.reserve(node.params.size());
    for (auto& p : node.params) {
        userParamTypes.push_back(impl_->typeExprToLLVM(p.type.get()));
    }

    std::vector<llvm::Type*> paramTypes = userParamTypes;
    if (hasCaptures) paramTypes.push_back(impl_->i8PtrType);

    auto* funcType = llvm::FunctionType::get(retType, paramTypes, false);
    auto* userFnType = llvm::FunctionType::get(retType, userParamTypes, false);
    auto* nestedFunc = llvm::Function::Create(
        funcType, llvm::Function::InternalLinkage, mangledName, impl_->module.get());

    // D027.1: gather capture info from the enclosing scope BEFORE switching context;
    // `nonlocal` captures travel as a cell ptr so mutations propagate (cell_get/set).
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

    // Save enclosing context; body metadata dies with the body (stale-type family),
    // released before the name-bind at the end so the parent-scope binding survives.
    std::optional<Impl::VarMetaScope> bodyMeta(*impl_);
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

    impl_->currentFunction = nestedFunc;
    auto* entry = llvm::BasicBlock::Create(*impl_->context, "entry", nestedFunc);
    impl_->builder->SetInsertPoint(entry);
    impl_->pushScope();

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

    // Env struct mirrors LambdaExpr's layout: 24-byte header + native-typed capture fields.
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
            // D027.1: cell-relayed captures - alloca holds the cell ptr; route
            // reads/writes via dragon_cell_get/set so mutations reach the owner's slot.
            if (cap.isCellRelay) {
                impl_->markCellBacked(cap.name);
            }
        }
    }

    // Install the alias so `<funcname>(...)` inside the body resolves to a direct LLVM
    // call, with env auto-appended for capturing defs.
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

    // Restore prior alias state for `node.name`; siblings and the enclosing scope use
    // the local-variable closure binding instead.
    if (hadPriorAlias) {
        impl_->nestedFunctionAliases[node.name] = savedAlias;
    } else {
        impl_->nestedFunctionAliases.erase(node.name);
    }

    impl_->popScope();
    impl_->scopes = std::move(savedScopes);
    impl_->globalDeclaredVars = std::move(savedGlobalDecls);
    impl_->nonlocalDeclaredVars = std::move(savedNonlocalDecls);
    impl_->cellPromotedLocals = std::move(savedCellPromoted);
    impl_->currentFunction = prevFunc;
    if (prevBlock) impl_->builder->SetInsertPoint(prevBlock);
    bodyMeta.reset();

    // Bind the name in the enclosing scope: a bare fn pointer for non-capturing defs,
    // a freshly allocated closure object for capturing ones.
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
                    // The env owns a refcount on the cell itself (TAG_CELL): plain
                    // dragon_incref, not `_str` (which would misread the cell's bytes).
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_incref"], {storeVal});
                } else if (Impl::isHeapKind(cap.kind)) {
                    if (cap.kind == Impl::VarKind::Str) {
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_incref_str"], {storeVal});
                    } else if (cap.kind == Impl::VarKind::Closure) {
                        // tag-gated - a captured Callable may hold a BARE fn ptr;
                        // generic incref would write a refcount into code bytes (SIGSEGV).
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

    // Allocate a local in the enclosing scope, store the bound value;
    // callableTypes[name] gives the user-visible signature for right-ABI dispatch.
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
