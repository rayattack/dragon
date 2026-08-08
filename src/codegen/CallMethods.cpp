/// Method-call dispatch: obj.method(args) for builtin receivers (str/bytes/
/// list/dict/set, Thread/Lock/Sync*), super, static/instance/vtable, modules.
#include "../CodeGenImpl.h"

namespace dragon {

bool CodeGen::Impl::packVarArgMethodArgs(
        CodeGen& cg, CallExpr& node, const std::string& methodFuncName,
        llvm::FunctionType* methodFuncType,
        std::vector<llvm::Value*>& args,
        std::vector<std::pair<llvm::Value*, VarKind>>& argTemps,
        const std::string& dispName) {
    auto vaIt = funcVarArgInfo.find(methodFuncName);
    if (vaIt == funcVarArgInfo.end()) return true;  // not variadic (caller gates)
    const VarArgInfo& vaInfo = vaIt->second;

    auto poison = [&]() {
        lastValue = llvm::ConstantPointerNull::get(
            llvm::PointerType::getUnqual(*context));
    };

    // Spread into a variadic method is not wired yet: diagnose rather than
    // miscompile (packing here would leak or emit bad IR on the dup-key raise).
    if (callHasSpread(node)) {
        addError("call-site spread (`*`/`**`) into a variadic method is not yet "
                 "supported", node.location());
        poison();
        return false;
    }

    // self (if any) already occupies args[0..selfOffset); 0 for a static method.
    const size_t selfOffset = args.size();
    const size_t numParams = methodFuncType->getNumParams();

    // 1. Regular positionals (before `*args`): the callee borrows, so owned
    // heap temps must drain via the shared classifier or a regular arg leaks.
    size_t llvmIdx = selfOffset;
    for (size_t i = 0; i < vaInfo.numRegularParams && i < node.args.size(); ++i) {
        node.args[i]->accept(cg);
        llvm::Value* arg = lastValue;
        collectArgTemp(methodFuncName, node.args[i].get(), arg,
                       (unsigned)llvmIdx, argTemps);
        if (llvmIdx < numParams)
            arg = coerceArgFromExpr(node.args[i].get(), arg,
                                    methodFuncType->getParamType((unsigned)llvmIdx));
        args.push_back(arg);
        llvmIdx++;
    }
    // 1b. Pad omitted regular slots (defaults before `*args`) with nulls for
    // fillDefaultArgs, else the `*args` list lands in the wrong slot (bad IR).
    while (args.size() < selfOffset + vaInfo.numRegularParams) {
        args.push_back(nullptr);
        llvmIdx++;
    }

    // 2. Pack surplus positionals into the `*args` list, monomorphized by
    // element tag; emitTypedListAppend owns each element's refs (tail decref).
    if (vaInfo.hasVarArg) {
        size_t extra = (node.args.size() > vaInfo.numRegularParams)
            ? node.args.size() - vaInfo.numRegularParams : 0;
        auto* cap = llvm::ConstantInt::get(i64Type, (int64_t)extra);
        llvm::Value* argsList =
            emitNewTypedList(vaInfo.varArgElemTag, vaInfo.varArgElemIsAny, cap);
        for (size_t i = vaInfo.numRegularParams; i < node.args.size(); ++i) {
            node.args[i]->accept(cg);
            emitTypedListAppend(argsList, lastValue, node.args[i].get(),
                                vaInfo.varArgElemTag, vaInfo.varArgElemIsAny, cg);
        }
        args.push_back(argsList);
        argTemps.emplace_back(argsList, VarKind::List);
        llvmIdx++;
    }

    // 2b. Bind keywords naming a regular param into that slot; funcParamNames
    // includes "self" at index 0, hence the selfOffset shift.
    std::vector<bool> kwConsumed(node.kwArgs.size(), false);
    if (!node.kwArgs.empty()) {
        auto pnIt = funcParamNames.find(methodFuncName);
        if (pnIt != funcParamNames.end()) {
            const auto& paramNames = pnIt->second;
            for (size_t ki = 0; ki < node.kwArgs.size(); ++ki) {
                const std::string& kwName = node.kwArgs[ki].first;
                if (kwName.empty()) continue;  // no `**` spread here (guarded)
                auto nameIt =
                    std::find(paramNames.begin(), paramNames.end(), kwName);
                if (nameIt == paramNames.end()) continue;  // unknown -> **kwargs
                size_t idx = (size_t)std::distance(paramNames.begin(), nameIt);
                if (idx < selfOffset) continue;                    // "self"
                if (idx - selfOffset >= vaInfo.numRegularParams)   // *args/**kwargs name
                    continue;                                      // -> **kwargs
                if (idx < args.size() && args[idx] != nullptr) {
                    addError(dispName + " got multiple values for argument '" +
                             kwName + "'", node.location());
                    poison();
                    return false;
                }
                node.kwArgs[ki].second->accept(cg);
                llvm::Value* arg = lastValue;
                collectArgTemp(methodFuncName, node.kwArgs[ki].second.get(), arg,
                               (unsigned)idx, argTemps);
                args[idx] = coerceArgFromExpr(
                    node.kwArgs[ki].second.get(), arg,
                    methodFuncType->getParamType((unsigned)idx));
                kwConsumed[ki] = true;
            }
        }
    }

    // 3. Pack remaining keywords into the `**kwargs` dict: tag by LLVM type,
    // incref a borrowed heap value the dict-set adopts; dict drains via argTemps.
    if (vaInfo.hasKwArg) {
        auto* cap = llvm::ConstantInt::get(i64Type, (int64_t)node.kwArgs.size());
        llvm::Value* kwargsDict =
            builder->CreateCall(runtimeFuncs["dragon_dict_new"], {cap}, "kwargs");
        for (size_t ki = 0; ki < node.kwArgs.size(); ++ki) {
            if (kwConsumed[ki]) continue;
            const std::string& kwName = node.kwArgs[ki].first;
            if (kwName.empty()) continue;
            node.kwArgs[ki].second->accept(cg);
            llvm::Value* val = lastValue;
            int64_t tag = 0;  // TAG_INT
            if (val->getType() == i1Type) {
                tag = 3;  // TAG_BOOL
                val = builder->CreateZExt(val, i64Type);
            } else if (val->getType() == f64Type) {
                tag = 2;  // TAG_FLOAT
                val = builder->CreateBitCast(val, i64Type);
            } else if (val->getType()->isPointerTy()) {
                tag = 1;  // TAG_STR (default for heap pointers)
                if (options.gcMode == GCMode::RC &&
                    isBorrowedHeapExpr(node.kwArgs[ki].second.get()))
                    builder->CreateCall(runtimeFuncs["dragon_incref_str"], {val});
                val = builder->CreatePtrToInt(val, i64Type);
            }
            auto* keyStr = builder->CreateGlobalString(kwName);
            builder->CreateCall(runtimeFuncs["dragon_dict_set_tagged"],
                {kwargsDict, keyStr, val,
                 llvm::ConstantInt::get(i64Type, tag)});
        }
        args.push_back(kwargsDict);
        argTemps.emplace_back(kwargsDict, VarKind::Dict);
        llvmIdx++;
    } else {
        // `*args`-only method: an unbound keyword is an error, never dropped.
        for (size_t ki = 0; ki < node.kwArgs.size(); ++ki) {
            if (!kwConsumed[ki] && !node.kwArgs[ki].first.empty()) {
                addError(dispName + " got an unexpected keyword argument '" +
                         node.kwArgs[ki].first + "'", node.location());
                poison();
                return false;
            }
        }
    }
    return true;
}

bool CodeGen::Impl::emitContractMethodCall(CodeGen& cg, CallExpr& node,
                                           AttributeExpr& attr) {
    auto ct = std::static_pointer_cast<ContractType>(attr.object->type);
    const std::string& method = attr.attribute;

    auto poison = [&]() {
        lastValue = llvm::ConstantPointerNull::get(
            llvm::PointerType::getUnqual(*context));
    };

    auto ownerIt = ct->methodOwner.find(method);
    if (ownerIt == ct->methodOwner.end()) {
        addError("contract '" + ct->display + "' declares no method '" +
                 method + "'", node.location());
        poison();
        return true;
    }
    const ContractDecl* owner = ownerIt->second;
    auto slotIt = contractMethodSlots.find({owner, method});
    if (slotIt == contractMethodSlots.end()) {
        addError("internal: no colored slot for contract method '" +
                 owner->name + "." + method + "'", node.location());
        poison();
        return true;
    }
    const FunctionDecl* sig = nullptr;
    for (auto& m : owner->methods)
        if (m->name == method) { sig = m.get(); break; }
    if (!sig) {
        addError("internal: contract '" + owner->name + "' lost signature '" +
                 method + "'", node.location());
        poison();
        return true;
    }
    if (!node.kwArgs.empty() || callHasSpread(node)) {
        addError("keyword and spread arguments are not yet supported on a "
                 "contract-typed call; pass positionally", node.location());
        poison();
        return true;
    }

    // The contract signature IS the ABI: self ptr + declared params.
    std::vector<llvm::Type*> paramTys;
    paramTys.push_back(i8PtrType);
    for (auto& p : sig->params) paramTys.push_back(typeExprToLLVM(p.type.get()));
    llvm::Type* retTy = sig->returnType ? typeExprToLLVM(sig->returnType.get())
                                        : voidType;
    auto* fnType = llvm::FunctionType::get(retTy, paramTys, false);

    // Receiver: an ordinary instance pointer (the cast added nothing). An
    // OWNED receiver temp (`pick().amazing_method()`) must drain after the
    // call - a bare local stays borrowed and is skipped by the classifier.
    attr.object->accept(cg);
    llvm::Value* self = lastValue;
    if (!self->getType()->isPointerTy())
        self = builder->CreateIntToPtr(self, i8PtrType);

    std::vector<llvm::Value*> args;
    args.push_back(self);
    std::vector<std::pair<llvm::Value*, VarKind>> argTemps;
    auto recvDk = ownedTempDrainKind(attr.object.get(), self);
    if (recvDk != VarKind::Other) argTemps.emplace_back(self, recvDk);
    for (size_t i = 0; i < node.args.size() && i + 1 < paramTys.size(); ++i) {
        node.args[i]->accept(cg);
        llvm::Value* arg = lastValue;
        auto dk = ownedTempDrainKind(node.args[i].get(), arg);
        if (dk != VarKind::Other) argTemps.emplace_back(arg, dk);
        args.push_back(coerceArgFromExpr(node.args[i].get(), arg, paramTys[i + 1]));
    }

    // Load the vtable from the instance header (rc, tag, vtable) and call
    // through the colored slot - the same shape as D026 virtual dispatch.
    auto* headerTy = llvm::StructType::get(*context,
        {i64Type, i64Type, i8PtrType});
    auto* vtSlot = builder->CreateStructGEP(headerTy, self, 2, "vt_slot");
    auto* vtPtr = builder->CreateLoad(i8PtrType, vtSlot, "vtable");
    auto* vtArrTy = llvm::ArrayType::get(i8PtrType, 0);
    auto* mSlot = builder->CreateGEP(vtArrTy, vtPtr,
        {builder->getInt64(0), builder->getInt64((int64_t)slotIt->second)},
        "contract_slot");
    llvm::Value* callee = builder->CreateLoad(i8PtrType, mSlot, "contract_fn");

    auto argTempBases = pushArgTempCleanups(argTemps);
    if (fnType->getReturnType()->isVoidTy()) {
        builder->CreateCall(fnType, callee, args);
        poison();
    } else {
        lastValue = normalizeIntC(
            builder->CreateCall(fnType, callee, args, "ccall"));
    }
    popArgTempCleanups(argTempBases);
    for (auto& [v, k] : argTemps) emitDecrefByKind(v, k);
    return true;
}

bool CodeGen::emitMethodCall(CallExpr& node, AttributeExpr& attr) {
    // D010: a TypeChecker-resolved overload dispatches to its per-index symbol
    // (`name__ovN`); resolvedMethodOverload is -1 for non-overloaded calls.
    std::string method = attr.attribute;
    if (node.resolvedMethodOverload >= 0)
        method += "__ov" + std::to_string(node.resolvedMethodOverload);

    // Spread (`obj.m(*xs)`) is wired only at class-instance dispatch: bail
    // unless the receiver is a user-class instance, so no earlier path grabs it.
    if (callHasSpread(node)) {
        auto* objName = dynamic_cast<NameExpr*>(attr.object.get());
        bool instanceRecv = false;
        if (objName) {
            if (objName->name == "self" && !impl_->currentClassName.empty()) {
                instanceRecv = true;
            } else {
                auto it = impl_->varClassNames.find(objName->name);
                instanceRecv = it != impl_->varClassNames.end() &&
                               impl_->classNames.count(it->second) > 0;
            }
        } else {
            // Non-Name receiver (`self.field.m(*xs)`, `make().m(*xs)`): handled
            // by the non-Name instance-dispatch block if its class is static.
            std::string cn = impl_->resolveExprClassName(attr.object.get());
            instanceRecv = !cn.empty() && impl_->classNames.count(cn) > 0;
        }
        if (!instanceRecv) return false;
    }

    // ADR 054 - contract-typed receiver: the concrete class is unknown by
    // design, so dispatch through the globally colored vtable slot. The
    // contract's own signature is the ABI (conformance enforced exact match
    // against every conforming class, so all implementations agree).
    if (attr.object && attr.object->type &&
        attr.object->type->kind() == Type::Kind::Contract) {
        return impl_->emitContractMethodCall(*this, node, attr);
    }

    // Thread handle dispatch: join() / is_alive()
    if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
        auto tIt = impl_->varClassNames.find(objName->name);
        if (tIt != impl_->varClassNames.end() && tIt->second == "__Thread") {
            if (method == "join") {
                llvm::Value* localSlot = impl_->lookupVar(objName->name);
                llvm::Value* handlePtr = localSlot;
                if (!handlePtr) handlePtr = impl_->lookupModuleGlobal(objName->name);
                if (handlePtr) {
                    auto* handle = impl_->builder->CreateLoad(impl_->i8PtrType, handlePtr, "vthread.handle");
                    auto* raw = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_vthread_join"], {handle}, "vthread.join");
                    // D030: reinterpret the i64 result slot at the task's native T.
                    impl_->lastValue = impl_->taskResultFromI64(raw, node.type.get());
                    // join() CONSUMES t (the runtime freed the vthread): null a
                    // LOCAL slot so detach / later calls never touch freed memory.
                    if (localSlot && impl_->options.gcMode == GCMode::RC)
                        impl_->builder->CreateStore(
                            llvm::ConstantPointerNull::get(
                                llvm::cast<llvm::PointerType>(impl_->i8PtrType)),
                            localSlot);
                    return true;
                }
            }
            if (method == "is_alive") {
                llvm::Value* handlePtr = impl_->lookupVar(objName->name);
                if (!handlePtr) handlePtr = impl_->lookupModuleGlobal(objName->name);
                if (handlePtr) {
                    auto* handle = impl_->builder->CreateLoad(impl_->i8PtrType, handlePtr, "vthread.handle");
                    auto* raw = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_vthread_is_alive"], {handle}, "vthread.alive");
                    // i64 -> i1 (bool), the declared return of is_alive().
                    impl_->lastValue = impl_->builder->CreateICmpNE(
                        raw, llvm::ConstantInt::get(impl_->i64Type, 0), "vthread.alive.b");
                    return true;
                }
            }
        }
    }

    // Lock dispatch (threading.Lock shape). isLockExpr must cover Lock-typed
    // instance fields too, else `self._lock.acquire()` is silently dropped.
    if (impl_->isLockExpr(attr.object.get())) {
        llvm::Value* handle = nullptr;
        if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
            llvm::Value* handlePtr = impl_->lookupVar(objName->name);
            if (!handlePtr) handlePtr = impl_->lookupModuleGlobal(objName->name);
            if (handlePtr)
                handle = impl_->builder->CreateLoad(impl_->i8PtrType, handlePtr, "lock.handle");
        } else {
            // Field receiver: the field slot holds the dragon_lock_new() pointer.
            attr.object->accept(*this);
            handle = impl_->lastValue;
            if (handle && !handle->getType()->isPointerTy())
                handle = impl_->builder->CreateIntToPtr(handle, impl_->i8PtrType, "lock.handle");
        }
        {
            if (handle) {
                if (method == "acquire") {
                    // acquire(blocking=True, timeout=-1) -> bool: True once
                    // held; with blocking=False/timeout, whether it got it.
                    Expr* blockingExpr = nullptr;
                    Expr* timeoutExpr = nullptr;
                    if (node.args.size() >= 1) blockingExpr = node.args[0].get();
                    if (node.args.size() >= 2) timeoutExpr = node.args[1].get();
                    for (auto& kw : node.kwArgs) {
                        if (kw.first == "blocking") blockingExpr = kw.second.get();
                        else if (kw.first == "timeout") timeoutExpr = kw.second.get();
                    }

                    if (!blockingExpr && !timeoutExpr) {
                        // Fast path: plain blocking acquire(), no branch.
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_lock_acquire"], {handle});
                        impl_->lastValue =
                            llvm::ConstantInt::get(impl_->i1Type, 1);
                        return true;
                    }
                    // blocking flag -> i64 (default 1 = blocking).
                    llvm::Value* blk = llvm::ConstantInt::get(impl_->i64Type, 1);
                    if (blockingExpr) {
                        blockingExpr->accept(*this);
                        blk = impl_->lastValue;
                        if (blk->getType() == impl_->i1Type)
                            blk = impl_->builder->CreateZExt(blk, impl_->i64Type);
                        else if (blk->getType()->isPointerTy())
                            blk = impl_->builder->CreatePtrToInt(blk, impl_->i64Type);
                    }
                    // timeout seconds -> f64 (default -1.0 = wait forever).
                    llvm::Value* tmo =
                        llvm::ConstantFP::get(impl_->f64Type, -1.0);
                    if (timeoutExpr) {
                        timeoutExpr->accept(*this);
                        tmo = impl_->lastValue;
                        if (tmo->getType() == impl_->i64Type)
                            tmo = impl_->builder->CreateSIToFP(tmo, impl_->f64Type);
                        else if (tmo->getType() == impl_->i1Type)
                            tmo = impl_->builder->CreateSIToFP(
                                impl_->builder->CreateZExt(tmo, impl_->i64Type),
                                impl_->f64Type);
                    }
                    auto* raw = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_lock_acquire_ex"],
                        {handle, blk, tmo}, "lock.acq");
                    // i64 (1=held, 0=contended/timed-out) -> i1 (bool).
                    impl_->lastValue = impl_->builder->CreateICmpNE(
                        raw, llvm::ConstantInt::get(impl_->i64Type, 0), "lock.acq.b");
                    return true;
                }
                if (method == "release") {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_lock_release"], {handle});
                    impl_->lastValue = llvm::ConstantPointerNull::get(
                        llvm::PointerType::getUnqual(*impl_->context));
                    return true;
                }
                if (method == "destroy") {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_lock_destroy"], {handle});
                    impl_->lastValue = llvm::ConstantPointerNull::get(
                        llvm::PointerType::getUnqual(*impl_->context));
                    return true;
                }
            }
        }
    }

    // SyncList method dispatch
    if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
        auto slIt = impl_->varClassNames.find(objName->name);
        if (slIt != impl_->varClassNames.end() && slIt->second == "__SyncList") {
            llvm::Value* handlePtr = impl_->lookupVar(objName->name);
            if (!handlePtr) handlePtr = impl_->lookupModuleGlobal(objName->name);
            if (handlePtr) {
                auto* handle = impl_->builder->CreateLoad(impl_->i8PtrType, handlePtr, "synclist.handle");
                auto* nullVal = llvm::ConstantPointerNull::get(
                    llvm::PointerType::getUnqual(*impl_->context));
                auto coerceI64 = [&](llvm::Value* v) -> llvm::Value* {
                    if (v->getType() == impl_->f64Type)
                        return impl_->builder->CreateBitCast(v, impl_->i64Type);
                    if (v->getType() == impl_->i1Type)
                        return impl_->builder->CreateZExt(v, impl_->i64Type);
                    if (v->getType()->isPointerTy())
                        return impl_->builder->CreatePtrToInt(v, impl_->i64Type);
                    return v;
                };
                if (method == "append" && node.args.size() == 1) {
                    node.args[0]->accept(*this);
                    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_synclist_append"],
                        {handle, coerceI64(impl_->lastValue)});
                    impl_->lastValue = nullVal; return true;
                }
                if (method == "pop") {
                    llvm::Value* idx;
                    if (node.args.size() == 1) { node.args[0]->accept(*this); idx = impl_->lastValue; }
                    else idx = llvm::ConstantInt::get(impl_->i64Type, -1);
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_synclist_pop"], {handle, idx}, "slpop");
                    return true;
                }
                if (method == "get" && node.args.size() == 1) {
                    node.args[0]->accept(*this);
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_synclist_get"], {handle, impl_->lastValue}, "slget");
                    return true;
                }
                if (method == "set" && node.args.size() == 2) {
                    node.args[0]->accept(*this); auto idx = impl_->lastValue;
                    node.args[1]->accept(*this);
                    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_synclist_set"],
                        {handle, idx, coerceI64(impl_->lastValue)});
                    impl_->lastValue = nullVal; return true;
                }
                if (method == "len" && node.args.empty()) {
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_synclist_len"], {handle}, "sllen");
                    return true;
                }
                if (method == "clear" && node.args.empty()) {
                    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_synclist_clear"], {handle});
                    impl_->lastValue = nullVal; return true;
                }
                if (method == "extend" && node.args.size() == 1) {
                    node.args[0]->accept(*this);
                    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_synclist_extend"],
                        {handle, impl_->lastValue});
                    impl_->lastValue = nullVal; return true;
                }
                if (method == "remove" && node.args.size() == 1) {
                    node.args[0]->accept(*this);
                    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_synclist_remove"],
                        {handle, coerceI64(impl_->lastValue)});
                    impl_->lastValue = nullVal; return true;
                }
                if (method == "insert" && node.args.size() == 2) {
                    node.args[0]->accept(*this); auto idx = impl_->lastValue;
                    node.args[1]->accept(*this);
                    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_synclist_insert"],
                        {handle, idx, coerceI64(impl_->lastValue)});
                    impl_->lastValue = nullVal; return true;
                }
                if (method == "index" && node.args.size() == 1) {
                    node.args[0]->accept(*this);
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_synclist_index"],
                        {handle, coerceI64(impl_->lastValue)}, "slindex");
                    return true;
                }
                if (method == "count" && node.args.size() == 1) {
                    node.args[0]->accept(*this);
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_synclist_count"],
                        {handle, coerceI64(impl_->lastValue)}, "slcount");
                    return true;
                }
                if (method == "sort" && node.args.empty()) {
                    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_synclist_sort"], {handle});
                    impl_->lastValue = nullVal; return true;
                }
                if (method == "reverse" && node.args.empty()) {
                    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_synclist_reverse"], {handle});
                    impl_->lastValue = nullVal; return true;
                }
                if (method == "copy" && node.args.empty()) {
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_synclist_copy"], {handle}, "slcopy");
                    return true;
                }
                if (method == "destroy" && node.args.empty()) {
                    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_synclist_destroy"], {handle});
                    impl_->lastValue = nullVal; return true;
                }
            }
        }
    }

    // SyncDict method dispatch
    if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
        auto sdIt = impl_->varClassNames.find(objName->name);
        if (sdIt != impl_->varClassNames.end() && sdIt->second == "__SyncDict") {
            llvm::Value* handlePtr = impl_->lookupVar(objName->name);
            if (!handlePtr) handlePtr = impl_->lookupModuleGlobal(objName->name);
            if (handlePtr) {
                auto* handle = impl_->builder->CreateLoad(impl_->i8PtrType, handlePtr, "syncdict.handle");
                auto* nullVal = llvm::ConstantPointerNull::get(
                    llvm::PointerType::getUnqual(*impl_->context));
                auto coerceI64 = [&](llvm::Value* v) -> llvm::Value* {
                    if (v->getType() == impl_->f64Type)
                        return impl_->builder->CreateBitCast(v, impl_->i64Type);
                    if (v->getType() == impl_->i1Type)
                        return impl_->builder->CreateZExt(v, impl_->i64Type);
                    if (v->getType()->isPointerTy())
                        return impl_->builder->CreatePtrToInt(v, impl_->i64Type);
                    return v;
                };
                if (method == "set" && node.args.size() == 2) {
                    node.args[0]->accept(*this); auto key = impl_->lastValue;
                    node.args[1]->accept(*this);
                    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_syncdict_set"],
                        {handle, key, coerceI64(impl_->lastValue)});
                    impl_->lastValue = nullVal; return true;
                }
                if (method == "get" && node.args.size() == 1) {
                    node.args[0]->accept(*this);
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_syncdict_get"], {handle, impl_->lastValue}, "sdget");
                    return true;
                }
                if (method == "get" && node.args.size() == 2) {
                    node.args[0]->accept(*this); auto key = impl_->lastValue;
                    node.args[1]->accept(*this);
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_syncdict_get_default"],
                        {handle, key, coerceI64(impl_->lastValue)}, "sdgetdef");
                    return true;
                }
                if (method == "len" && node.args.empty()) {
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_syncdict_len"], {handle}, "sdlen");
                    return true;
                }
                if (method == "has_key" && node.args.size() == 1) {
                    node.args[0]->accept(*this);
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_syncdict_has_key"], {handle, impl_->lastValue}, "sdhaskey");
                    return true;
                }
                if (method == "keys" && node.args.empty()) {
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_syncdict_keys"], {handle}, "sdkeys");
                    return true;
                }
                if (method == "values" && node.args.empty()) {
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_syncdict_values"], {handle}, "sdvalues");
                    return true;
                }
                if (method == "items" && node.args.empty()) {
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_syncdict_items"], {handle}, "sditems");
                    return true;
                }
                if (method == "pop" && node.args.size() == 1) {
                    node.args[0]->accept(*this);
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_syncdict_pop"], {handle, impl_->lastValue}, "sdpop");
                    return true;
                }
                if (method == "pop" && node.args.size() == 2) {
                    node.args[0]->accept(*this); auto key = impl_->lastValue;
                    node.args[1]->accept(*this);
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_syncdict_pop_default"],
                        {handle, key, coerceI64(impl_->lastValue)}, "sdpopdef");
                    return true;
                }
                if (method == "clear" && node.args.empty()) {
                    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_syncdict_clear"], {handle});
                    impl_->lastValue = nullVal; return true;
                }
                if (method == "update" && node.args.size() == 1) {
                    node.args[0]->accept(*this);
                    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_syncdict_update"],
                        {handle, impl_->lastValue});
                    impl_->lastValue = nullVal; return true;
                }
                if (method == "setdefault" && node.args.size() == 2) {
                    node.args[0]->accept(*this); auto key = impl_->lastValue;
                    // Own the borrowed key: the runtime adopts it on insert and
                    // releases it on present (under the syncdict wrlock).
                    impl_->increfBorrowedSetdefaultKey(node.args[0].get(), key);
                    node.args[1]->accept(*this);
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_syncdict_setdefault"],
                        {handle, key, coerceI64(impl_->lastValue)}, "sdsetdef");
                    return true;
                }
                if (method == "copy" && node.args.empty()) {
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_syncdict_copy"], {handle}, "sdcopy");
                    return true;
                }
                if (method == "destroy" && node.args.empty()) {
                    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_syncdict_destroy"], {handle});
                    impl_->lastValue = nullVal; return true;
                }
            }
        }
    }

    bool isList = false;
    if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
        isList = impl_->lookupVarKind(objName->name) == Impl::VarKind::List;
    }

    bool isDict = false;
    if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
        isDict = impl_->lookupVarKind(objName->name) == Impl::VarKind::Dict;
    }

    bool isSet = false;
    if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
        isSet = impl_->lookupVarKind(objName->name) == Impl::VarKind::Set;
    }
    if (!isSet) {
        if (dynamic_cast<SetExpr*>(attr.object.get())) isSet = true;
    }

    // Class-field receiver (self.field / instance.field): use its recorded kind.
    if (!isList && !isDict && !isSet) {
        if (auto* innerAttr = dynamic_cast<AttributeExpr*>(attr.object.get())) {
            std::string className;
            if (auto* innerObj = dynamic_cast<NameExpr*>(innerAttr->object.get())) {
                if (innerObj->name == "self" && !impl_->currentClassName.empty()) {
                    className = impl_->currentClassName;
                } else {
                    auto vit = impl_->varClassNames.find(innerObj->name);
                    if (vit != impl_->varClassNames.end()) className = vit->second;
                }
            }
            if (!className.empty()) {
                auto fkIt = impl_->classFieldKindsBySym.find(impl_->classSym(className));
                if (fkIt != impl_->classFieldKindsBySym.end()) {
                    auto fkIt2 = fkIt->second.find(innerAttr->attribute);
                    if (fkIt2 != fkIt->second.end()) {
                        if (fkIt2->second == Impl::VarKind::List) isList = true;
                        else if (fkIt2->second == Impl::VarKind::Dict) isDict = true;
                        else if (fkIt2->second == Impl::VarKind::Set) isSet = true;
                    }
                }
            }
        }
    }

    bool isStr = false;
    if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
        auto sk = impl_->lookupVarKind(objName->name);
        isStr = (sk == Impl::VarKind::Str || sk == Impl::VarKind::StrLiteral);
    }
    if (!isStr) {
        if (auto* sl = dynamic_cast<StringLiteral*>(attr.object.get()))
            isStr = !sl->isBytes;
    }

    // D030 §5: bytes-ness comes solely from the static type / AST shape; there
    // is no VarKind::Bytes (bytes slots carry the generic-heap VarKind::List).
    bool isBytes = attr.object && attr.object->type &&
                   attr.object->type->kind() == Type::Kind::Bytes;
    if (!isBytes) {
        if (auto* sl = dynamic_cast<StringLiteral*>(attr.object.get()))
            isBytes = sl->isBytes;
    }

    // Final fallback: the typechecker-propagated static type, for receivers the
    // VarKind heuristics miss (chained subscripts, dict values, ...).
    if (!isList && !isDict && !isSet && !isStr && !isBytes && attr.object->type) {
        switch (attr.object->type->kind()) {
            case Type::Kind::List:  isList  = true; break;
            case Type::Kind::Dict:  isDict  = true; break;
            case Type::Kind::Set:   isSet   = true; break;
            case Type::Kind::Str:   isStr   = true; break;
            case Type::Kind::Bytes: isBytes = true; break;
            default: break;
        }
    }

    // Deque method dispatch
    if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
        auto dqIt = impl_->varClassNames.find(objName->name);
        if (dqIt != impl_->varClassNames.end() && dqIt->second == "__Deque") {
            llvm::Value* handlePtr = impl_->lookupVar(objName->name);
            if (!handlePtr) handlePtr = impl_->lookupModuleGlobal(objName->name);
            if (handlePtr) {
                auto* handle = impl_->builder->CreateLoad(impl_->i8PtrType, handlePtr, "deque.handle");
                if ((method == "append" || method == "appendleft") &&
                    node.args.size() == 1) {
                    node.args[0]->accept(*this);
                    llvm::Value* rawVal = impl_->lastValue;
                    llvm::Value* val = rawVal;
                    // Element tag: prefer the static deque[T] element type, else
                    // the value's LLVM type; the runtime stores it (RC, `in`, repr).
                    int64_t elemTag = 0;
                    if (auto* lt = dynamic_cast<ListType*>(attr.object->type.get())) {
                        if (lt->elementType) {
                            int64_t t = impl_->typeKindToTag(lt->elementType->kind());
                            if (t > 0) elemTag = t;
                        }
                    }
                    if (val->getType() == impl_->i1Type) {
                        if (elemTag == 0) elemTag = 3;  // TAG_BOOL
                        val = impl_->builder->CreateZExt(val, impl_->i64Type);
                    } else if (val->getType() == impl_->f64Type) {
                        if (elemTag == 0) elemTag = 2;  // TAG_FLOAT
                        val = impl_->builder->CreateBitCast(val, impl_->i64Type);
                    } else if (val->getType()->isPointerTy()) {
                        if (elemTag == 0) elemTag = 1;  // TAG_STR
                        val = impl_->builder->CreatePtrToInt(val, impl_->i64Type);
                    }
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs[method == "append"
                                                ? "dragon_deque_append"
                                                : "dragon_deque_appendleft"],
                        {handle, val,
                         llvm::ConstantInt::get(impl_->i64Type, elemTag)});
                    // The deque took its own ref: release an OWNED heap arg temp
                    // or it leaks one ref per append; borrowed args untouched.
                    if (rawVal->getType()->isPointerTy()) {
                        Impl::VarKind pk = elemTag == 1 ? Impl::VarKind::Str
                                                        : Impl::VarKind::List;
                        Impl::VarKind dk = impl_->argTempDecrefKind(
                            node.args[0].get(), pk, rawVal);
                        if (dk != Impl::VarKind::Other)
                            impl_->emitDecrefByKind(rawVal, dk);
                    }
                    impl_->lastValue = llvm::ConstantPointerNull::get(
                        llvm::PointerType::getUnqual(*impl_->context));
                    return true;
                }
                if ((method == "popleft" || method == "pop") &&
                    node.args.empty()) {
                    // Heap-ptr element types pop via the _ptr variant so the OWNED
                    // transfer is a recognized ptr result; scalars/Callable stay i64.
                    bool heapElem = false;
                    if (auto* lt = dynamic_cast<ListType*>(attr.object->type.get())) {
                        if (lt->elementType) {
                            switch (lt->elementType->kind()) {
                                case Type::Kind::Str:
                                case Type::Kind::List:
                                case Type::Kind::Dict:
                                case Type::Kind::Set:
                                case Type::Kind::Tuple:
                                case Type::Kind::Bytes:
                                case Type::Kind::Instance:
                                    heapElem = true;
                                    break;
                                default:
                                    break;
                            }
                        }
                    }
                    const char* fnName =
                        method == "popleft"
                            ? (heapElem ? "dragon_deque_popleft_ptr"
                                        : "dragon_deque_popleft")
                            : (heapElem ? "dragon_deque_pop_ptr"
                                        : "dragon_deque_pop");
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs[fnName], {handle}, method);
                    return true;
                }
            }
        }
    }

    // String method dispatch
    if (isStr) {
        attr.object->accept(*this);
        llvm::Value* obj = impl_->lastValue;
        // An owned heap-str receiver (slice/concat/call result) must be released
        // or it leaks per call; every str method returns a fresh result, so ok.
        bool ownedStrRecv =
            impl_->options.gcMode == GCMode::RC && impl_->isOwnedStrResult(obj);
        // Owned heap temps for borrow-arg slots: every str method borrows its
        // args, so these drain at the common tail, never for a borrowed Name/field.
        std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
        bool strHandled = [&]() -> bool {

        // strip/lstrip/rstrip(chars): char-set trim (NOT prefix/suffix). Must
        // precede the no-arg block, else it swallows these and drops the arg.
        if ((method == "strip" || method == "lstrip" || method == "rstrip") &&
            node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* chars = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_" + method + "_chars",
                llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType, impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, chars}, method);
            return true;
        }

        // No-arg methods returning string
        if (method == "upper" || method == "lower" || method == "strip" ||
            method == "lstrip" || method == "rstrip" || method == "title" ||
            method == "capitalize" || method == "swapcase" || method == "casefold") {
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_" + method,
                llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj}, method);
            return true;
        }

        // No-arg bool predicates: runtime returns i64 0/1, converted to i1 so
        // consumers see a native bool (D030).
        if (method == "isdigit" || method == "isalpha" || method == "isalnum" ||
            method == "isspace" || method == "isupper" || method == "islower" ||
            method == "istitle" || method == "isascii" || method == "isdecimal" ||
            method == "isnumeric" || method == "isprintable" || method == "isidentifier") {
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_" + method,
                llvm::FunctionType::get(impl_->i64Type, {impl_->i8PtrType}, false));
            llvm::Value* call = impl_->builder->CreateCall(fn, {obj}, method);
            impl_->lastValue = impl_->builder->CreateICmpNE(
                call, llvm::ConstantInt::get(impl_->i64Type, 0), method + ".b");
            return true;
        }

        // 1-arg(str) methods returning string
        if ((method == "removeprefix" || method == "removesuffix") && node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_" + method,
                llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType, impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, arg}, method);
            return true;
        }

        // 1-arg(str) bool predicates - convert runtime i64 to native i1.
        if ((method == "startswith" || method == "endswith" || method == "contains") &&
            node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_" + method,
                llvm::FunctionType::get(impl_->i64Type, {impl_->i8PtrType, impl_->i8PtrType}, false));
            llvm::Value* call = impl_->builder->CreateCall(fn, {obj, arg}, method);
            impl_->lastValue = impl_->builder->CreateICmpNE(
                call, llvm::ConstantInt::get(impl_->i64Type, 0), method + ".b");
            return true;
        }

        // find/rfind/count with optional start[, end]; the _se variant takes both.
        if ((method == "find" || method == "rfind" || method == "count") &&
            node.args.size() >= 1 && node.args.size() <= 3) {
            node.args[0]->accept(*this);
            llvm::Value* sub = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            if (node.args.size() == 1) {
                auto* fn = impl_->getOrDeclareRuntime("dragon_str_" + method,
                    llvm::FunctionType::get(impl_->i64Type,
                        {impl_->i8PtrType, impl_->i8PtrType}, false));
                impl_->lastValue = impl_->builder->CreateCall(fn, {obj, sub}, method);
                return true;
            }
            node.args[1]->accept(*this);
            llvm::Value* start = impl_->lastValue;
            llvm::Value* end = nullptr;
            if (node.args.size() == 3) {
                node.args[2]->accept(*this);
                end = impl_->lastValue;
            } else {
                end = llvm::ConstantInt::get(impl_->i64Type, -1);
            }
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_" + method + "_se",
                llvm::FunctionType::get(impl_->i64Type,
                    {impl_->i8PtrType, impl_->i8PtrType, impl_->i64Type, impl_->i64Type},
                    false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, sub, start, end}, method);
            return true;
        }

        // index/rindex
        if ((method == "index" || method == "rindex") && node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            std::string rtName = (method == "index") ? "dragon_str_index_of" : "dragon_str_rindex";
            auto* fn = impl_->getOrDeclareRuntime(rtName,
                llvm::FunctionType::get(impl_->i64Type, {impl_->i8PtrType, impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, arg}, method);
            return true;
        }

        // replace(old, new[, count])
        if (method == "replace" && node.args.size() >= 2) {
            node.args[0]->accept(*this);
            llvm::Value* old_s = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            node.args[1]->accept(*this);
            llvm::Value* new_s = impl_->trackBorrowTemp(node.args[1].get(), impl_->lastValue, argTemps);
            if (node.args.size() >= 3) {
                node.args[2]->accept(*this);
                llvm::Value* count = impl_->lastValue;
                auto* fn = impl_->getOrDeclareRuntime("dragon_str_replace_n",
                    llvm::FunctionType::get(impl_->i8PtrType,
                        {impl_->i8PtrType, impl_->i8PtrType, impl_->i8PtrType, impl_->i64Type}, false));
                impl_->lastValue = impl_->builder->CreateCall(fn, {obj, old_s, new_s, count}, "replace");
                return true;
            }
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_replace",
                llvm::FunctionType::get(impl_->i8PtrType,
                    {impl_->i8PtrType, impl_->i8PtrType, impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, old_s, new_s}, "replace");
            return true;
        }

        // zfill(width)
        if (method == "zfill" && node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* width = impl_->lastValue;
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_zfill",
                llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType, impl_->i64Type}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, width}, "zfill");
            return true;
        }

        // expandtabs(tabsize=8)
        if (method == "expandtabs") {
            llvm::Value* tabsize = llvm::ConstantInt::get(impl_->i64Type, 8);
            if (node.args.size() >= 1) { node.args[0]->accept(*this); tabsize = impl_->lastValue; }
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_expandtabs",
                llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType, impl_->i64Type}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, tabsize}, "expandtabs");
            return true;
        }

        // center/ljust/rjust(width[, fillchar])
        if (method == "center" || method == "ljust" || method == "rjust") {
            if (node.args.size() >= 1) {
                node.args[0]->accept(*this);
                llvm::Value* width = impl_->lastValue;
                // Fill char: default ' ' (32), or first char of second arg
                llvm::Value* fill = llvm::ConstantInt::get(
                    llvm::Type::getInt8Ty(*impl_->context), ' ');
                if (node.args.size() >= 2) {
                    node.args[1]->accept(*this);
                    llvm::Value* fillStr = impl_->trackBorrowTemp(node.args[1].get(), impl_->lastValue, argTemps);
                    fill = impl_->builder->CreateLoad(
                        llvm::Type::getInt8Ty(*impl_->context), fillStr, "fillch");
                }
                auto* fn = impl_->getOrDeclareRuntime("dragon_str_" + method,
                    llvm::FunctionType::get(impl_->i8PtrType,
                        {impl_->i8PtrType, impl_->i64Type, llvm::Type::getInt8Ty(*impl_->context)}, false));
                impl_->lastValue = impl_->builder->CreateCall(fn, {obj, width, fill}, method);
                return true;
            }
        }

        // split/rsplit(sep[, maxsplit]) return a list; maxsplit -1 = unlimited,
        // rsplit splits from the right.
        if (method == "split" || method == "rsplit") {
            llvm::Value* sep = llvm::ConstantPointerNull::get(
                llvm::PointerType::getUnqual(*impl_->context));
            if (node.args.size() >= 1) { node.args[0]->accept(*this); sep = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps); }
            llvm::Value* maxsplit = llvm::ConstantInt::get(impl_->i64Type, -1);
            if (node.args.size() >= 2) { node.args[1]->accept(*this); maxsplit = impl_->lastValue; }
            const char* rt = (method == "split") ? "dragon_str_split_max" : "dragon_str_rsplit";
            auto* fn = impl_->getOrDeclareRuntime(rt,
                llvm::FunctionType::get(impl_->i8PtrType,
                    {impl_->i8PtrType, impl_->i8PtrType, impl_->i64Type}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, sep, maxsplit}, method);
            return true;
        }

        // join(list)
        if (method == "join" && node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* list = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_join",
                llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType, impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, list}, "join");
            return true;
        }

        // splitlines()
        if (method == "splitlines") {
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_splitlines",
                llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj}, "splitlines");
            return true;
        }

        // partition/rpartition(sep) - returns a 3-tuple (DragonTuple*, an i8*);
        // the TupleType result from TypeChecker drives tuple repr / unpack.
        if ((method == "partition" || method == "rpartition") && node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* sep = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_" + method,
                llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType, impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, sep}, method);
            return true;
        }

        // encode(encoding="utf-8", errors="strict") -> bytes; args honored by
        // dragon_str_encode_ex (UTF-8/ASCII, strict/replace), never discarded.
        if (method == "encode") {
            llvm::Value* enc = impl_->builder->CreateGlobalString("utf-8");
            llvm::Value* err = impl_->builder->CreateGlobalString("strict");
            if (node.args.size() >= 1) { node.args[0]->accept(*this); enc = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps); }
            if (node.args.size() >= 2) { node.args[1]->accept(*this); err = impl_->trackBorrowTemp(node.args[1].get(), impl_->lastValue, argTemps); }
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_encode_ex",
                llvm::FunctionType::get(impl_->i8PtrType,
                    {impl_->i8PtrType, impl_->i8PtrType, impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, enc, err}, "encode");
            return true;
        }
        return false;  // not a str method - fall through to other dispatch
        }();
        if (strHandled) {
            for (auto& [v, k] : argTemps) impl_->emitDecrefByKind(v, k);
            impl_->emitMoveOutSlots(node);
            if (ownedStrRecv)
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref_str"], {obj});
            return true;
        }
    }

    // Bytes method dispatch
    if (isBytes) {
        attr.object->accept(*this);
        llvm::Value* obj = impl_->lastValue;
        // Owned heap-bytes receiver: same consume-and-release contract as the
        // str block; all bytes methods return fresh non-aliasing results.
        bool ownedBytesRecv =
            impl_->options.gcMode == GCMode::RC && impl_->isOwnedPtrResult(obj);
        // Owned heap temps for borrow-arg slots: every bytes method borrows its
        // args; released at the common tail.
        std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
        bool bytesHandled = [&]() -> bool {

        // No-arg methods returning bytes
        if (method == "upper" || method == "lower" || method == "strip" ||
            method == "lstrip" || method == "rstrip") {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_" + method], {obj}, method);
            return true;
        }

        // No-arg bool predicates - convert runtime i64 to native i1.
        if (method == "isdigit" || method == "isalpha" || method == "isalnum" ||
            method == "isspace") {
            llvm::Value* call = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_" + method], {obj}, method);
            impl_->lastValue = impl_->builder->CreateICmpNE(
                call, llvm::ConstantInt::get(impl_->i64Type, 0), method + ".b");
            return true;
        }

        // decode(encoding="utf-8", errors="strict") -> str; strict matches Python
        // (invalid input raises UnicodeDecodeError, no silent Latin-1 fallback).
        if (method == "decode") {
            llvm::Value* enc = impl_->builder->CreateGlobalString("utf-8");
            llvm::Value* err = impl_->builder->CreateGlobalString("strict");
            if (node.args.size() >= 1) { node.args[0]->accept(*this); enc = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps); }
            if (node.args.size() >= 2) { node.args[1]->accept(*this); err = impl_->trackBorrowTemp(node.args[1].get(), impl_->lastValue, argTemps); }
            auto* fn = impl_->getOrDeclareRuntime("dragon_bytes_decode_ex",
                llvm::FunctionType::get(impl_->i8PtrType,
                    {impl_->i8PtrType, impl_->i8PtrType, impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, enc, err}, "decode");
            return true;
        }

        // hex()
        if (method == "hex") {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_hex"], {obj}, "hex");
            return true;
        }

        // 1-arg(bytes) bool predicates - convert runtime i64 to native i1.
        if ((method == "startswith" || method == "endswith") &&
            node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            llvm::Value* call = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_" + method], {obj, arg}, method);
            impl_->lastValue = impl_->builder->CreateICmpNE(
                call, llvm::ConstantInt::get(impl_->i64Type, 0), method + ".b");
            return true;
        }

        // find/rfind/count return positions (i64)
        if ((method == "find" || method == "rfind" || method == "count") &&
            node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_" + method], {obj, arg}, method);
            return true;
        }

        // index/rindex - raises ValueError
        if ((method == "index" || method == "rindex") && node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            std::string rtName = (method == "index") ? "dragon_bytes_index_of" : "dragon_bytes_rindex";
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs[rtName], {obj, arg}, method);
            return true;
        }

        // replace(old, new)
        if (method == "replace" && node.args.size() >= 2) {
            node.args[0]->accept(*this);
            llvm::Value* old_b = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            node.args[1]->accept(*this);
            llvm::Value* new_b = impl_->trackBorrowTemp(node.args[1].get(), impl_->lastValue, argTemps);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_replace"], {obj, old_b, new_b}, "replace");
            return true;
        }

        // split(sep)
        if (method == "split") {
            llvm::Value* sep = llvm::ConstantPointerNull::get(
                llvm::PointerType::getUnqual(*impl_->context));
            if (node.args.size() >= 1) { node.args[0]->accept(*this); sep = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps); }
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_split"], {obj, sep}, "split");
            return true;
        }

        // join(list)
        if (method == "join" && node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* list = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_join"], {obj, list}, "join");
            return true;
        }
        return false;  // not a bytes method - fall through to other dispatch
        }();
        if (bytesHandled) {
            for (auto& [v, k] : argTemps) impl_->emitDecrefByKind(v, k);
            impl_->emitMoveOutSlots(node);
            if (ownedBytesRecv)
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref"], {obj});
            return true;
        }
    }

    // bytes.fromhex() - static constructor
    if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
        if (objName->name == "bytes" && method == "fromhex" && node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* hexStr = impl_->lastValue;
            // fromhex borrows the hex string and returns fresh bytes; release
            // an owned-temp arg after the call.
            Impl::VarKind dk = impl_->ownedTempDrainKind(node.args[0].get(), hexStr);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_fromhex"], {hexStr}, "fromhex");
            impl_->emitDecrefByKind(hexStr, dk);
            return true;
        }
        // dict.fromkeys(iterable[, value]); default value None (TAG_NONE).
        if (objName->name == "dict" && method == "fromkeys" &&
            (node.args.size() == 1 || node.args.size() == 2)) {
            node.args[0]->accept(*this);
            llvm::Value* keysList = impl_->lastValue;
            llvm::Value* val;
            llvm::Value* tag;
            if (node.args.size() == 2) {
                node.args[1]->accept(*this);
                llvm::Value* raw = impl_->lastValue;
                // D030 §5: prefer the static type for the tag - bytes values
                // must round-trip TAG_BYTES even when their VarKind is generic-heap.
                int64_t tagVal = -1;
                if (node.args[1] && node.args[1]->type)
                    tagVal = Impl::typeKindToTag(node.args[1]->type->kind());
                if (tagVal < 0) {
                    Impl::VarKind vk = Impl::VarKind::Other;
                    if (auto* nm = dynamic_cast<NameExpr*>(node.args[1].get()))
                        vk = impl_->lookupVarKind(nm->name);
                    else if (auto* sl = dynamic_cast<StringLiteral*>(node.args[1].get()))
                        tagVal = sl->isBytes ? 7 : 1;  // TAG_BYTES / TAG_STR
                    else if (dynamic_cast<IntegerLiteral*>(node.args[1].get()))
                        vk = Impl::VarKind::Int;
                    else if (dynamic_cast<FloatLiteral*>(node.args[1].get()))
                        vk = Impl::VarKind::Float;
                    else if (dynamic_cast<BooleanLiteral*>(node.args[1].get()))
                        vk = Impl::VarKind::Bool;
                    if (tagVal < 0) tagVal = Impl::varKindToTag(vk);
                }
                if (tagVal < 0) tagVal = 0;  // TAG_INT default
                // Coerce raw value to i64 for the runtime call.
                if (raw->getType() == impl_->i1Type)
                    raw = impl_->builder->CreateZExt(raw, impl_->i64Type);
                else if (raw->getType() == impl_->f64Type)
                    raw = impl_->builder->CreateBitCast(raw, impl_->i64Type);
                else if (raw->getType()->isPointerTy())
                    raw = impl_->builder->CreatePtrToInt(raw, impl_->i64Type);
                val = raw;
                tag = llvm::ConstantInt::get(impl_->i64Type, tagVal);
            } else {
                // No value arg -> None
                val = llvm::ConstantInt::get(impl_->i64Type, 0);
                tag = llvm::ConstantInt::get(impl_->i64Type, 4); // TAG_NONE
            }
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_fromkeys"],
                {keysList, val, tag}, "fromkeys");
            return true;
        }
    }

    // List method dispatch
    if (isList) {
        attr.object->accept(*this);
        llvm::Value* obj = impl_->lastValue;
        // An owned list RECEIVER temp leaks once per call without a drain; done
        // at the tail, allow-listed to methods that cannot borrow the receiver.
        bool ownedListRecv = impl_->options.gcMode == GCMode::RC &&
                             !Impl::isBorrowedHeapExpr(attr.object.get()) &&
                             impl_->isOwnedPtrResult(obj);
        static const std::set<std::string> kListRecvDrainOk = {
            "append", "insert", "extend", "remove", "clear", "sort",
            "reverse", "count", "index", "pop", "copy"};
        // Owned heap temps for borrow-method arg slots, released at the tail;
        // append/insert do NOT track - they adopt the +1 (Model B).
        std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
        bool listHandled = [&]() -> bool {

        if (method == "append" && node.args.size() == 1) {
            // Track the element class when appending a class instance
            // (ctor call or class-typed variable).
            std::string appendedClassName;
            if (auto* argCall = dynamic_cast<CallExpr*>(node.args[0].get())) {
                if (auto* argFn = dynamic_cast<NameExpr*>(argCall->callee.get())) {
                    if (impl_->classNames.count(argFn->name))
                        appendedClassName = argFn->name;
                }
            } else if (auto* argName = dynamic_cast<NameExpr*>(node.args[0].get())) {
                // varClassNames is bare-name-keyed and never cleared: a str-bound
                // arg must skip it, else a stale Instance entry mis-increfs (UAF).
                Impl::VarKind ak = impl_->lookupVarKind(argName->name);
                if (ak != Impl::VarKind::Str && ak != Impl::VarKind::StrLiteral) {
                    auto vit = impl_->varClassNames.find(argName->name);
                    if (vit != impl_->varClassNames.end())
                        appendedClassName = vit->second;
                }
            }
            if (!appendedClassName.empty()) {
                // NEVER clobber a declared list[Any] receiver's elem kind with
                // Instance: that re-routes box-list ops to append_ptr (overflow).
                if (auto* listAttr = dynamic_cast<AttributeExpr*>(attr.object.get())) {
                    if (auto* listObj = dynamic_cast<NameExpr*>(listAttr->object.get())) {
                        std::string ownerClass;
                        if (listObj->name == "self" && !impl_->currentClassName.empty())
                            ownerClass = impl_->currentClassName;
                        else {
                            auto vit = impl_->varClassNames.find(listObj->name);
                            if (vit != impl_->varClassNames.end()) ownerClass = vit->second;
                        }
                        if (!ownerClass.empty()) {
                            auto ckIt = impl_->classFieldListElemKindsBySym.find(impl_->classSym(ownerClass));
                            bool fieldIsAny = false;
                            if (ckIt != impl_->classFieldListElemKindsBySym.end()) {
                                auto fIt = ckIt->second.find(listAttr->attribute);
                                fieldIsAny = fIt != ckIt->second.end() &&
                                             fIt->second == Type::Kind::Any;
                            }
                            if (!fieldIsAny) {
                                impl_->classFieldListElemKindsBySym[impl_->classSym(ownerClass)][listAttr->attribute] = Type::Kind::Instance;
                                impl_->classFieldListElemClassNameBySym[impl_->classSym(ownerClass)][listAttr->attribute] = appendedClassName;
                            }
                        }
                    }
                } else if (auto* listName = dynamic_cast<NameExpr*>(attr.object.get())) {
                    auto ekIt = impl_->varListElemKinds.find(listName->name);
                    bool varIsAny = ekIt != impl_->varListElemKinds.end() &&
                                    ekIt->second == Type::Kind::Any;
                    if (!varIsAny) {
                        impl_->varListElemKinds[listName->name] = Type::Kind::Instance;
                        impl_->varListElemClassName[listName->name] = appendedClassName;
                    }
                }
            }
            node.args[0]->accept(*this);
            llvm::Value* val = impl_->lastValue;
            // D030: dispatch append by element kind so the value never funnels
            // through i64; D039: list[Any] uses dragon_list_box_append.
            Type::Kind appendElemKind = impl_->getIterableElementKind(attr.object.get());
            if (appendElemKind == Type::Kind::Any) {
                // dragon_list_box_append adopts one reference (Model B);
                // boxArgTagPayload increfs a borrowed source.
                auto tp = impl_->boxArgTagPayload(node.args[0].get(),
                                                  val, /*takesOwnership=*/true);
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_list_box_append"],
                    {obj, tp.first, tp.second});
                impl_->lastValue = llvm::ConstantPointerNull::get(
                    llvm::PointerType::getUnqual(*impl_->context));
                return true;
            }
            if (appendElemKind == Type::Kind::Float) {
                if (val->getType() == impl_->i64Type)
                    val = impl_->builder->CreateSIToFP(val, impl_->f64Type);
                else if (val->getType() == impl_->i1Type)
                    val = impl_->builder->CreateUIToFP(val, impl_->f64Type);
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_list_append_f64"], {obj, val});
            } else if (appendElemKind == Type::Kind::Str      ||
                       appendElemKind == Type::Kind::Bytes    ||
                       appendElemKind == Type::Kind::List     ||
                       appendElemKind == Type::Kind::Dict     ||
                       appendElemKind == Type::Kind::Tuple    ||
                       appendElemKind == Type::Kind::Set      ||
                       appendElemKind == Type::Kind::Function ||  // Callable = refcounted DragonClosure ptr
                       appendElemKind == Type::Kind::Instance) {
                if (appendElemKind == Type::Kind::Str && val->getType()->isPointerTy())
                    val = impl_->ensureHeapString(val, node.args[0].get());
                // D046: wrap a bare fn value in DragonClosure(fn, null) so every
                // element is refcounted; the fresh +1 skips the incref below.
                bool freshWrappedClosure = false;
                if (appendElemKind == Type::Kind::Function &&
                    llvm::isa<llvm::Function>(val)) {
                    auto* fnI8 = impl_->builder->CreateBitCast(val, impl_->i8PtrType);
                    auto* nullEnv = llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(impl_->i8PtrType));
                    val = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_closure_create"],
                        {fnI8, nullEnv}, "fn.wrap.append");
                    freshWrappedClosure = true;
                }
                if (!val->getType()->isPointerTy())
                    val = impl_->builder->CreateIntToPtr(val, impl_->i8PtrType);
                // Model B: append_ptr adopts one reference, so borrowed sources
                // need an incref, else scope cleanup frees the value in the list.
                if (impl_->options.gcMode == GCMode::RC && !freshWrappedClosure &&
                    Impl::isBorrowedHeapExpr(node.args[0].get())) {
                    if (appendElemKind == Type::Kind::Str)
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_incref_str"], {val});
                    else if (appendElemKind == Type::Kind::Function)
                        // A Callable slot may hold a DragonClosure or a bare fn
                        // ptr; the tag-gated incref no-ops on the latter (.text).
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_incref_callable"], {val});
                    else
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_incref"], {val});
                }
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_list_append_ptr"], {obj, val});
            } else {
                // Int / Bool / unknown - legacy i64 path.
                if (val->getType() == impl_->f64Type) {
                    val = impl_->builder->CreateBitCast(val, impl_->i64Type);
                } else if (val->getType() == impl_->i1Type) {
                    val = impl_->builder->CreateZExt(val, impl_->i64Type);
                } else if (val->getType()->isPointerTy()) {
                    val = impl_->builder->CreatePtrToInt(val, impl_->i64Type);
                }
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_list_append"], {obj, val});
            }
            impl_->lastValue = llvm::ConstantPointerNull::get(
                llvm::PointerType::getUnqual(*impl_->context));
            return true;
        }
        if (method == "insert" && node.args.size() == 2) {
            node.args[0]->accept(*this);
            llvm::Value* idx = impl_->lastValue;
            node.args[1]->accept(*this);
            llvm::Value* val = impl_->lastValue;
            // list[Any] -> 16-byte DragonListBox; route to the box-aware insert
            // (the i64 dragon_list_insert would shift 8-byte halves).
            if (impl_->getIterableElementKind(attr.object.get()) ==
                Type::Kind::Any) {
                if (idx->getType() == impl_->i1Type)
                    idx = impl_->builder->CreateZExt(idx, impl_->i64Type);
                else if (idx->getType()->isPointerTy())
                    idx = impl_->builder->CreatePtrToInt(idx, impl_->i64Type);
                else if (idx->getType() != impl_->i64Type)
                    idx = impl_->builder->CreateZExtOrTrunc(idx, impl_->i64Type);
                auto tp = impl_->boxArgTagPayload(node.args[1].get(),
                                                  val, /*takesOwnership=*/true);
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_list_box_insert"],
                    {obj, idx, tp.first, tp.second});
                impl_->lastValue = llvm::ConstantPointerNull::get(
                    llvm::PointerType::getUnqual(*impl_->context));
                return true;
            }
            // dragon_list_insert BORROWS its value (increfs internally, unlike
            // append's adopt), so an owned temp arg must drain or its +1 leaks.
            val = impl_->trackBorrowTemp(node.args[1].get(), val, argTemps);
            if (val->getType() == impl_->f64Type) val = impl_->builder->CreateBitCast(val, impl_->i64Type);
            else if (val->getType() == impl_->i1Type) val = impl_->builder->CreateZExt(val, impl_->i64Type);
            else if (val->getType()->isPointerTy()) val = impl_->builder->CreatePtrToInt(val, impl_->i64Type);
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_list_insert"], {obj, idx, val});
            impl_->lastValue = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*impl_->context));
            return true;
        }
        if (method == "remove" && node.args.size() == 1) {
            node.args[0]->accept(*this);
            llvm::Value* val = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            // list[Any] -> 16-byte DragonListBox; value-equality search via
            // dragon_box_eq (the i64 dragon_list_remove can't see the boxes).
            if (impl_->getIterableElementKind(attr.object.get()) ==
                Type::Kind::Any) {
                auto tp = impl_->boxArgTagPayload(node.args[0].get(),
                                                  val, /*takesOwnership=*/false);
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_list_box_remove"],
                    {obj, tp.first, tp.second});
                impl_->lastValue = llvm::ConstantPointerNull::get(
                    llvm::PointerType::getUnqual(*impl_->context));
                return true;
            }
            if (val->getType() == impl_->f64Type) val = impl_->builder->CreateBitCast(val, impl_->i64Type);
            else if (val->getType() == impl_->i1Type) val = impl_->builder->CreateZExt(val, impl_->i64Type);
            else if (val->getType()->isPointerTy()) val = impl_->builder->CreatePtrToInt(val, impl_->i64Type);
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_list_remove"], {obj, val});
            impl_->lastValue = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*impl_->context));
            return true;
        }
        if (method == "pop") {
            Type::Kind popElemKind =
                impl_->getIterableElementKind(attr.object.get());
            bool isBox = popElemKind == Type::Kind::Any;
            llvm::Value* idx;
            if (node.args.size() == 1) {
                node.args[0]->accept(*this);
                idx = impl_->lastValue;
            } else {
                idx = llvm::ConstantInt::get(impl_->i64Type, -1);
            }
            if (idx->getType() == impl_->i1Type)
                idx = impl_->builder->CreateZExt(idx, impl_->i64Type);
            else if (idx->getType()->isPointerTy())
                idx = impl_->builder->CreatePtrToInt(idx, impl_->i64Type);
            else if (idx->getType() != impl_->i64Type)
                idx = impl_->builder->CreateZExtOrTrunc(idx, impl_->i64Type);
            // list[float]: pop must return native f64 - the generic i64 pop's
            // raw bytes would be SIToFP'd (garbage from the f64 bit pattern).
            if (popElemKind == Type::Kind::Float) {
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_list_pop_f64"], {obj, idx},
                    "listpop.f64");
                return true;
            }
            // list[Any] -> 16-byte DragonListBox: dragon_list_box_pop returns a
            // {tag,payload} box (ownership transfers to the caller, no decref).
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs[isBox ? "dragon_list_box_pop"
                                          : "dragon_list_pop"],
                {obj, idx}, "listpop");
            return true;
        }
        if (method == "clear" && node.args.empty()) {
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_list_clear"], {obj});
            impl_->lastValue = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*impl_->context));
            return true;
        }
        if (method == "extend" && node.args.size() == 1) {
            node.args[0]->accept(*this);
            llvm::Value* other = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_list_extend"], {obj, other});
            impl_->lastValue = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*impl_->context));
            return true;
        }
        if (method == "index" && node.args.size() == 1) {
            node.args[0]->accept(*this);
            llvm::Value* val = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            if (val->getType() == impl_->f64Type) val = impl_->builder->CreateBitCast(val, impl_->i64Type);
            else if (val->getType() == impl_->i1Type) val = impl_->builder->CreateZExt(val, impl_->i64Type);
            else if (val->getType()->isPointerTy()) val = impl_->builder->CreatePtrToInt(val, impl_->i64Type);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_index"], {obj, val}, "listindex");
            return true;
        }
        if (method == "count" && node.args.size() == 1) {
            node.args[0]->accept(*this);
            llvm::Value* val = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            if (val->getType() == impl_->f64Type) val = impl_->builder->CreateBitCast(val, impl_->i64Type);
            else if (val->getType() == impl_->i1Type) val = impl_->builder->CreateZExt(val, impl_->i64Type);
            else if (val->getType()->isPointerTy()) val = impl_->builder->CreatePtrToInt(val, impl_->i64Type);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_count"], {obj, val}, "listcount");
            return true;
        }
        if (method == "sort" && node.args.empty()) {
            // sort(reverse=...) selects descending; without it the cheaper
            // in-place ascending sort.
            Expr* reverseArg = nullptr;
            for (auto& kw : node.kwArgs)
                if (kw.first == "reverse") reverseArg = kw.second.get();
            if (reverseArg) {
                reverseArg->accept(*this);
                llvm::Value* rev = impl_->lastValue;
                if (rev->getType() == impl_->i1Type)
                    rev = impl_->builder->CreateZExt(rev, impl_->i64Type);
                else if (rev->getType()->isPointerTy())
                    rev = impl_->builder->CreatePtrToInt(rev, impl_->i64Type);
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_list_sort_ex"], {obj, rev});
            } else {
                impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_list_sort"], {obj});
            }
            impl_->lastValue = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*impl_->context));
            return true;
        }
        if (method == "reverse" && node.args.empty()) {
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_list_reverse"], {obj});
            impl_->lastValue = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*impl_->context));
            return true;
        }
        if (method == "copy" && node.args.empty()) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_copy"], {obj}, "listcopy");
            return true;
        }
        return false;  // not a list method - fall through to other dispatch
        }();
        if (listHandled) {
            for (auto& [v, k] : argTemps) impl_->emitDecrefByKind(v, k);
            impl_->emitMoveOutSlots(node);
            if (ownedListRecv && kListRecvDrainOk.count(method))
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref"], {obj});
            return true;
        }
    }

    // Dict method dispatch
    if (isDict) {
        attr.object->accept(*this);
        llvm::Value* obj = impl_->lastValue;

        // Int-keyed dicts store native i64 keys: route to the dragon_dict_int_*
        // family, else the char*-keyed generics are a signature mismatch.
        bool intKeyed = impl_->dictKeyIsInt(attr.object.get());

        // dragon_dict_get* return raw i64: re-cast to the dict's native value
        // type (D030), else a target-less f"{d.get(k)}" prints a pointer int.
        auto coerceDictValue = [&](llvm::Value* raw) -> llvm::Value* {
            Type::Kind vk = Type::Kind::Unknown;
            if (attr.object->type && attr.object->type->kind() == Type::Kind::Dict)
                vk = static_cast<DictType&>(*attr.object->type).valueType->kind();
            switch (vk) {
                case Type::Kind::Str:   case Type::Kind::List:  case Type::Kind::Dict:
                case Type::Kind::Set:   case Type::Kind::Tuple: case Type::Kind::Bytes:
                case Type::Kind::Instance:
                    return impl_->builder->CreateIntToPtr(raw, impl_->i8PtrType, "dgv.ptr");
                case Type::Kind::Float:
                    return impl_->builder->CreateBitCast(raw, impl_->f64Type, "dgv.f64");
                default:
                    return raw;  // int/bool/any flow as i64
            }
        };
        // Heap-object value types: reads must own the result (incref) or the
        // binding's scope decref frees the dict's stored value.
        auto isHeapValueKind = [](Type::Kind k) {
            return k == Type::Kind::List || k == Type::Kind::Dict ||
                   k == Type::Kind::Set  || k == Type::Kind::Tuple ||
                   k == Type::Kind::Bytes || k == Type::Kind::Instance;
        };
        auto dictValueKind = [&]() -> Type::Kind {
            if (attr.object->type && attr.object->type->kind() == Type::Kind::Dict)
                return static_cast<DictType&>(*attr.object->type).valueType->kind();
            return Type::Kind::Unknown;
        };

        // Owned dict RECEIVER temp drain. Conservative allow-list: get/
        // setdefault/keys/values/items excluded - results may borrow the receiver.
        bool ownedDictRecv = impl_->options.gcMode == GCMode::RC &&
                             !Impl::isBorrowedHeapExpr(attr.object.get()) &&
                             impl_->isOwnedPtrResult(obj);
        static const std::set<std::string> kDictRecvDrainOk = {
            "pop", "popitem", "clear", "update"};
        // Owned heap temps for borrowed KEY / other-dict args, released at the
        // tail; setdefault keys and default-VALUE args are NOT tracked (adopted).
        std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
        bool dictHandled = [&]() -> bool {

        // .get(key)
        if (method == "get" && node.args.size() == 1) {
            node.args[0]->accept(*this);
            llvm::Value* key = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            // Heap-valued dict: own the returned value (the getter increfs) so
            // the binding's scope-decref balances - a bare borrow would UAF.
            if (isHeapValueKind(dictValueKind())) {
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs[intKeyed ? "dragon_dict_int_get_owned"
                                                 : "dragon_dict_get_ptr"],
                    {obj, key}, "dictget.owned");
                return true;
            }
            llvm::Value* raw = impl_->builder->CreateCall(
                impl_->runtimeFuncs[intKeyed ? "dragon_dict_int_get" : "dragon_dict_get"],
                {obj, key}, "dictget");
            impl_->lastValue = coerceDictValue(raw);
            return true;
        }

        // .get(key, default)
        if (method == "get" && node.args.size() == 2) {
            node.args[0]->accept(*this);
            llvm::Value* key = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            node.args[1]->accept(*this);
            llvm::Value* defVal = impl_->lastValue;
            // Str-keyed str-valued dict: route to the owned-str getter; the
            // generic getter's borrowed result would double-free at scope exit.
            Type::Kind getVk = Type::Kind::Unknown;
            if (attr.object->type && attr.object->type->kind() == Type::Kind::Dict)
                getVk = static_cast<DictType&>(*attr.object->type).valueType->kind();
            if (!intKeyed && getVk == Type::Kind::Str) {
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_get_str_default"],
                    {obj, key, defVal}, "dictgetstrdef");
                return true;
            }
            // Heap-valued: own the result via the incref-on-return getter; the
            // default temp drains right after, balanced present or absent.
            if (isHeapValueKind(getVk)) {
                Impl::VarKind ddk = impl_->ownedTempDrainKind(node.args[1].get(), defVal);
                llvm::Value* defPtr = defVal;
                if (!defPtr->getType()->isPointerTy())
                    defPtr = impl_->builder->CreateIntToPtr(defVal, impl_->i8PtrType);
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs[intKeyed ? "dragon_dict_int_get_owned_default"
                                                 : "dragon_dict_get_ptr_default"],
                    {obj, key, defPtr}, "dictgetdef.owned");
                if (ddk != Impl::VarKind::Other) impl_->emitDecrefByKind(defVal, ddk);
                return true;
            }
            // Convert default to i64 for storage
            if (defVal->getType() == impl_->i1Type) {
                defVal = impl_->builder->CreateZExt(defVal, impl_->i64Type);
            } else if (defVal->getType() == impl_->f64Type) {
                defVal = impl_->builder->CreateBitCast(defVal, impl_->i64Type);
            } else if (defVal->getType()->isPointerTy()) {
                defVal = impl_->builder->CreatePtrToInt(defVal, impl_->i64Type);
            }
            llvm::Value* raw = impl_->builder->CreateCall(
                impl_->runtimeFuncs[intKeyed ? "dragon_dict_int_get_default"
                                             : "dragon_dict_get_default"],
                {obj, key, defVal}, "dictgetdef");
            impl_->lastValue = coerceDictValue(raw);
            return true;
        }

        // .keys()
        if (method == "keys" && node.args.empty()) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_keys"], {obj}, "dictkeys");
            return true;
        }

        // .has_key(key)
        if (method == "has_key" && node.args.size() == 1) {
            node.args[0]->accept(*this);
            llvm::Value* key = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs[intKeyed ? "dragon_dict_int_has_key"
                                             : "dragon_dict_has_key"],
                {obj, key}, "haskey");
            return true;
        }

        // .values(); D039: an Any value type routes to dragon_dict_values_box
        // so the result list keeps per-entry tags (isinstance / print).
        if (method == "values" && node.args.empty()) {
            bool valueIsAny = false;
            if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
                auto vit = impl_->varDictValueKinds.find(objName->name);
                if (vit != impl_->varDictValueKinds.end() &&
                    vit->second == Type::Kind::Any)
                    valueIsAny = true;
            }
            if (!valueIsAny && attr.object->type &&
                attr.object->type->kind() == Type::Kind::Dict) {
                if (auto* dt = dynamic_cast<DictType*>(attr.object->type.get())) {
                    if (dt->valueType && dt->valueType->kind() == Type::Kind::Any)
                        valueIsAny = true;
                }
            }
            if (valueIsAny) {
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_values_box"], {obj},
                    "dictvalues.box");
            } else {
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_values"], {obj}, "dictvalues");
            }
            return true;
        }

        // .items()
        if (method == "items" && node.args.empty()) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_items"], {obj}, "dictitems");
            return true;
        }

        // .popitem() returns DragonTuple* (LIFO; raises KeyError on empty dict).
        if (method == "popitem" && node.args.empty()) {
            llvm::Value* tupleI64 = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_popitem"], {obj}, "dictpopitem");
            impl_->lastValue = impl_->builder->CreateIntToPtr(
                tupleI64, impl_->i8PtrType, "popitem_ptr");
            return true;
        }

        // .pop(key)
        if (method == "pop" && node.args.size() == 1) {
            node.args[0]->accept(*this);
            llvm::Value* key = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_pop"], {obj, key}, "dictpop");
            return true;
        }

        // .pop(key, default)
        if (method == "pop" && node.args.size() == 2) {
            node.args[0]->accept(*this);
            llvm::Value* key = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            node.args[1]->accept(*this);
            llvm::Value* defVal = impl_->lastValue;
            if (defVal->getType() == impl_->i1Type) defVal = impl_->builder->CreateZExt(defVal, impl_->i64Type);
            else if (defVal->getType() == impl_->f64Type) defVal = impl_->builder->CreateBitCast(defVal, impl_->i64Type);
            else if (defVal->getType()->isPointerTy()) defVal = impl_->builder->CreatePtrToInt(defVal, impl_->i64Type);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_pop_default"],
                {obj, key, defVal}, "dictpopdef");
            return true;
        }

        // .clear()
        if (method == "clear" && node.args.empty()) {
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_dict_clear"], {obj});
            impl_->lastValue = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*impl_->context));
            return true;
        }

        // .update(other_dict)
        if (method == "update" && node.args.size() == 1) {
            node.args[0]->accept(*this);
            llvm::Value* other = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_dict_update"], {obj, other});
            impl_->lastValue = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*impl_->context));
            return true;
        }

        // .setdefault(key, default)
        if (method == "setdefault" && node.args.size() == 2) {
            node.args[0]->accept(*this);
            llvm::Value* key = impl_->lastValue;
            node.args[1]->accept(*this);
            llvm::Value* defVal = impl_->lastValue;
            // Heap-valued setdefault: own the result via the incref-on-return
            // variant (absent branch increfs dict copy + binding); borrow = UAF.
            if (isHeapValueKind(dictValueKind())) {
                // Own the borrowed heap key so the dict's stored key can't
                // dangle (the runtime releases it on the present branch).
                impl_->increfBorrowedSetdefaultKey(node.args[0].get(), key);
                Impl::VarKind ddk = impl_->ownedTempDrainKind(node.args[1].get(), defVal);
                llvm::Value* defPtr = defVal;
                if (!defPtr->getType()->isPointerTy())
                    defPtr = impl_->builder->CreateIntToPtr(defVal, impl_->i8PtrType);
                int64_t tag = impl_->inferPtrValueTag(node.args[1].get());
                llvm::Value* tagV = llvm::ConstantInt::get(impl_->i64Type, tag);
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs[intKeyed ? "dragon_dict_int_setdefault_owned"
                                                 : "dragon_dict_setdefault_ptr"],
                    {obj, key, defPtr, tagV}, "dictsetdef.owned");
                if (ddk != Impl::VarKind::Other) impl_->emitDecrefByKind(defVal, ddk);
                return true;
            }
            // Scalar-valued dict: no value UAF, but the KEY still dangles on
            // insert - own the borrowed key; released on the present branch.
            impl_->increfBorrowedSetdefaultKey(node.args[0].get(), key);
            if (defVal->getType() == impl_->i1Type) defVal = impl_->builder->CreateZExt(defVal, impl_->i64Type);
            else if (defVal->getType() == impl_->f64Type) defVal = impl_->builder->CreateBitCast(defVal, impl_->i64Type);
            else if (defVal->getType()->isPointerTy()) defVal = impl_->builder->CreatePtrToInt(defVal, impl_->i64Type);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_setdefault"],
                {obj, key, defVal}, "dictsetdef");
            return true;
        }

        // .copy()
        if (method == "copy" && node.args.empty()) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_copy"], {obj}, "dictcopy");
            return true;
        }
        return false;  // not a dict method - fall through to other dispatch
        }();
        if (dictHandled) {
            for (auto& [v, k] : argTemps) impl_->emitDecrefByKind(v, k);
            impl_->emitMoveOutSlots(node);
            if (ownedDictRecv && kDictRecvDrainOk.count(method))
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref"], {obj});
            return true;
        }
    }

    // Set method dispatch (hash-table-backed)
    if (isSet) {
        attr.object->accept(*this);
        llvm::Value* obj = impl_->lastValue;
        // Owned set RECEIVER temp drain: set binary ops return fresh sets with
        // increfed elements, so none can borrow from the receiver.
        bool ownedSetRecv = impl_->options.gcMode == GCMode::RC &&
                            !Impl::isBorrowedHeapExpr(attr.object.get()) &&
                            impl_->isOwnedPtrResult(obj);
        static const std::set<std::string> kSetRecvDrainOk = {
            "add", "remove", "discard", "clear", "union", "intersection",
            "difference", "symmetric_difference", "issubset", "issuperset",
            "isdisjoint"};
        // Owned heap temps for borrow-method args, released at the tail;
        // set.add is excluded (it runs its own owned-str decref).
        std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
        bool setHandled = [&]() -> bool {

        // Encode an arg expr into i64 for set storage / lookup.
        auto argToI64 = [&](size_t i) -> llvm::Value* {
            node.args[i]->accept(*this);
            llvm::Value* v = impl_->lastValue;
            impl_->trackBorrowTemp(node.args[i].get(), v, argTemps);
            if (v->getType() == impl_->i1Type)
                v = impl_->builder->CreateZExt(v, impl_->i64Type);
            else if (v->getType() == impl_->f64Type)
                v = impl_->builder->CreateBitCast(v, impl_->i64Type);
            else if (v->getType()->isPointerTy())
                v = impl_->builder->CreatePtrToInt(v, impl_->i64Type);
            return v;
        };

        if (method == "add" && node.args.size() == 1) {
            // Heap-promote string-literal args so the set owns a refcounted copy.
            llvm::Value* v;
            llvm::Value* ownedStrArg = nullptr;
            node.args[0]->accept(*this);
            v = impl_->lastValue;
            if (v->getType()->isPointerTy()) {
                v = impl_->ensureHeapString(v, node.args[0].get());
                // dragon_set_add INCREFS, so an owned +1 str temp must drain or
                // every add leaks; str-gated (non-str would need dragon_decref).
                bool argIsStr =
                    (node.args[0]->type &&
                     node.args[0]->type->kind() == Type::Kind::Str) ||
                    dynamic_cast<StringLiteral*>(node.args[0].get());
                if (impl_->options.gcMode == GCMode::RC && argIsStr &&
                    impl_->isOwnedStrResult(v))
                    ownedStrArg = v;
                v = impl_->builder->CreatePtrToInt(v, impl_->i64Type);
            } else if (v->getType() == impl_->i1Type) {
                v = impl_->builder->CreateZExt(v, impl_->i64Type);
            } else if (v->getType() == impl_->f64Type) {
                v = impl_->builder->CreateBitCast(v, impl_->i64Type);
            }
            // An empty set() is untagged (raw i64 hashing, no str decref): adopt
            // the element tag from the first add; runtime accepts it only while empty.
            {
                int64_t addTag = 0;
                if (node.args[0]->type)
                    addTag = impl_->typeKindToElemTag(node.args[0]->type->kind());
                if (addTag == 0 && dynamic_cast<StringLiteral*>(node.args[0].get()))
                    addTag = 1; // TAG_STR
                if (addTag != 0) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_set_adopt_tag"],
                        {obj, llvm::ConstantInt::get(impl_->i64Type, addTag)});
                }
            }
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_set_add"], {obj, v});
            if (ownedStrArg)
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref_str"], {ownedStrArg});
            impl_->lastValue = llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(impl_->i8PtrType));
            return true;
        }
        if (method == "remove" && node.args.size() == 1) {
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_set_remove"], {obj, argToI64(0)});
            impl_->lastValue = llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(impl_->i8PtrType));
            return true;
        }
        if (method == "discard" && node.args.size() == 1) {
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_set_discard"], {obj, argToI64(0)});
            impl_->lastValue = llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(impl_->i8PtrType));
            return true;
        }
        if (method == "clear" && node.args.empty()) {
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_set_clear"], {obj});
            impl_->lastValue = llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(impl_->i8PtrType));
            return true;
        }
        if (method == "pop" && node.args.empty()) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_set_pop"], {obj}, "setpop");
            return true;
        }
        if (method == "copy" && node.args.empty()) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_set_copy"], {obj}, "setcopy");
            return true;
        }
        // Binary set ops - the second arg is itself a set pointer.
        auto setArg = [&]() -> llvm::Value* {
            node.args[0]->accept(*this);
            llvm::Value* v = impl_->lastValue;
            impl_->trackBorrowTemp(node.args[0].get(), v, argTemps);
            if (!v->getType()->isPointerTy())
                v = impl_->builder->CreateIntToPtr(v, impl_->i8PtrType);
            return v;
        };
        if (method == "union" && node.args.size() == 1) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_set_union"], {obj, setArg()}, "setunion");
            return true;
        }
        if (method == "intersection" && node.args.size() == 1) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_set_intersection"], {obj, setArg()}, "setinter");
            return true;
        }
        if (method == "difference" && node.args.size() == 1) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_set_difference"], {obj, setArg()}, "setdiff");
            return true;
        }
        if (method == "symmetric_difference" && node.args.size() == 1) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_set_symmetric_difference"], {obj, setArg()}, "setsymdiff");
            return true;
        }
        if (method == "update" && node.args.size() == 1) {
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_set_update"], {obj, setArg()});
            impl_->lastValue = llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(impl_->i8PtrType));
            return true;
        }
        if (method == "issubset" && node.args.size() == 1) {
            auto* r = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_set_issubset"], {obj, setArg()}, "issubset");
            impl_->lastValue = impl_->builder->CreateICmpNE(
                r, llvm::ConstantInt::get(impl_->i64Type, 0), "issubset.b");
            return true;
        }
        if (method == "issuperset" && node.args.size() == 1) {
            auto* r = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_set_issuperset"], {obj, setArg()}, "issuperset");
            impl_->lastValue = impl_->builder->CreateICmpNE(
                r, llvm::ConstantInt::get(impl_->i64Type, 0), "issuperset.b");
            return true;
        }
        if (method == "isdisjoint" && node.args.size() == 1) {
            auto* r = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_set_isdisjoint"], {obj, setArg()}, "isdisjoint");
            impl_->lastValue = impl_->builder->CreateICmpNE(
                r, llvm::ConstantInt::get(impl_->i64Type, 0), "isdisjoint.b");
            return true;
        }
        return false;  // not a set method - fall through to other dispatch
        }();
        if (setHandled) {
            for (auto& [v, k] : argTemps) impl_->emitDecrefByKind(v, k);
            impl_->emitMoveOutSlots(node);
            if (ownedSetRecv && kSetRecvDrainOk.count(method))
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref"], {obj});
            return true;
        }
    }

    // super parent-method dispatch: .dr spells it `super.m(...)`, .py
    // `super().m(...)`, mode-exclusive; ctor delegation lives in CallExpr.cpp.
    {
        bool bareSuper = false;    // super.method(...)
        bool calledSuper = false;  // super().method(...)
        if (auto* sn = dynamic_cast<NameExpr*>(attr.object.get())) {
            bareSuper = (sn->name == "super");
        } else if (auto* sc = dynamic_cast<CallExpr*>(attr.object.get())) {
            if (auto* scn = dynamic_cast<NameExpr*>(sc->callee.get()))
                calledSuper = (scn->name == "super");
        }

        if (bareSuper || calledSuper) {
            auto fail = [&](const std::string& msg) {
                impl_->addError(msg, node.location());
                impl_->lastValue = llvm::ConstantPointerNull::get(
                    llvm::PointerType::getUnqual(*impl_->context));
                return true;
            };
            if (impl_->currentClassName.empty())
                return fail("super is only valid inside a class method");

            // Enforce the mode-specific spelling.
            if (impl_->isDragonFile && calledSuper) {
                if (method == "__init__")
                    return fail("in .dr, delegate to the parent constructor as "
                        "`super(args)` - `super().__init__(...)` is .py-mode syntax");
                return fail("in .dr, call a parent method as `super." + method +
                    "(...)` - `super()." + method + "(...)` is .py-mode syntax");
            }
            if (impl_->isDragonFile && bareSuper && method == "__init__")
                return fail("in .dr, delegate to the parent constructor as "
                    "`super(args)` - `super.__init__(...)` is not valid");
            if (!impl_->isDragonFile && bareSuper)
                return fail("in .py mode, call a parent method as `super()." + method +
                    "(...)` - bare `super." + method + "` is .dr-mode syntax");

            // Parent entry IS its sym, so the method symbol is direct.
            auto parentIt = impl_->classParentNamesBySym.find(
                impl_->classSym(impl_->currentClassName));
            if (parentIt != impl_->classParentNamesBySym.end()) {
                std::string parentMethodName = parentIt->second + "_" + method;
                auto* parentMethod = impl_->module->getFunction(parentMethodName);
                if (parentMethod) {
                    auto* selfAlloca = impl_->lookupVar("self");
                    llvm::Value* selfVal = impl_->builder->CreateLoad(
                        impl_->i8PtrType, selfAlloca, "self");
                    std::vector<llvm::Value*> args = {selfVal};
                    auto parentMethodType = parentMethod->getFunctionType();
                    for (size_t i = 0; i < node.args.size(); ++i) {
                        node.args[i]->accept(*this);
                        llvm::Value* arg = impl_->lastValue;
                        unsigned paramIdx = (unsigned)(i + 1);
                        if (paramIdx < parentMethodType->getNumParams())
                            arg = impl_->coerceArgFromExpr(node.args[i].get(), arg, parentMethodType->getParamType(paramIdx));
                        args.push_back(arg);
                    }
                    if (parentMethod->getReturnType()->isVoidTy()) {
                        impl_->builder->CreateCall(parentMethod, args);
                        impl_->lastValue = llvm::ConstantPointerNull::get(
                            llvm::PointerType::getUnqual(*impl_->context));
                    } else {
                        impl_->lastValue = impl_->normalizeIntC(
                            impl_->builder->CreateCall(
                                parentMethod, args, "super_call"));
                    }
                    return true;
                }
                return fail("super." + method + "(...): parent class '" +
                    parentIt->second + "' has no method '" + method + "'");
            }
            return fail("super." + method + "(...): class '" +
                impl_->currentClassName + "' has no parent class");
        }
    }

    // Static method dispatch: ClassName.method(args)
    if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
        if (impl_->classNames.count(objName->name)) {
            std::string methodFuncName =
                impl_->classSymPrefix(objName->name) + "_" + method;
            if (impl_->staticMethods.count(methodFuncName)) {
                auto* methodFunc = impl_->module->getFunction(methodFuncName);
                if (methodFunc) {
                    // Static method: do NOT pass self
                    std::vector<llvm::Value*> args;
                    // Owned heap-temp args to release after the call.
                    std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
                    auto methodFuncType = methodFunc->getFunctionType();
                    for (size_t i = 0; i < node.args.size(); ++i) {
                        node.args[i]->accept(*this);
                        llvm::Value* arg = impl_->lastValue;
                        impl_->collectArgTemp(methodFuncName, node.args[i].get(),
                                              arg, (unsigned)i, argTemps);
                        unsigned paramIdx = (unsigned)i;
                        if (paramIdx < methodFuncType->getNumParams())
                            arg = impl_->coerceArgFromExpr(node.args[i].get(), arg, methodFuncType->getParamType(paramIdx));
                        args.push_back(arg);
                    }
                    if (methodFunc->getReturnType()->isVoidTy()) {
                        impl_->builder->CreateCall(methodFunc, args);
                        impl_->lastValue = llvm::ConstantPointerNull::get(
                            llvm::PointerType::getUnqual(*impl_->context));
                    } else {
                        impl_->lastValue = impl_->normalizeIntC(
                            impl_->builder->CreateCall(methodFunc, args, "smcall"));
                    }
                    for (auto& [v, k] : argTemps) impl_->emitDecrefByKind(v, k);
                    impl_->emitMoveOutSlots(node);
                    return true;
                }
            }
        }
    }

    // Static method via a module-qualified class: mod.Class.method(args).
    // Without this path, `mod.Class.staticmethod()` silently compiles to nothing.
    if (auto* objAttr = dynamic_cast<AttributeExpr*>(attr.object.get())) {
        if (objAttr->object && objAttr->object->type &&
            objAttr->object->type->kind() == Type::Kind::Module &&
            impl_->classNames.count(objAttr->attribute)) {
            const std::string& srcModule =
                static_cast<ModuleType&>(*objAttr->object->type).name;
            std::string methodFuncName =
                Impl::mangleClass(srcModule, objAttr->attribute) + "_" + method;
            if (impl_->staticMethods.count(methodFuncName)) {
                if (auto* methodFunc = impl_->module->getFunction(methodFuncName)) {
                    std::vector<llvm::Value*> args;
                    // Owned heap-temp args to release after the call.
                    std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
                    auto methodFuncType = methodFunc->getFunctionType();
                    for (size_t i = 0; i < node.args.size(); ++i) {
                        node.args[i]->accept(*this);
                        llvm::Value* arg = impl_->lastValue;
                        impl_->collectArgTemp(methodFuncName, node.args[i].get(),
                                              arg, (unsigned)i, argTemps);
                        unsigned paramIdx = (unsigned)i;
                        if (paramIdx < methodFuncType->getNumParams())
                            arg = impl_->coerceArgFromExpr(
                                node.args[i].get(), arg,
                                methodFuncType->getParamType(paramIdx));
                        args.push_back(arg);
                    }
                    if (methodFunc->getReturnType()->isVoidTy()) {
                        impl_->builder->CreateCall(methodFunc, args);
                        impl_->lastValue = llvm::ConstantPointerNull::get(
                            llvm::PointerType::getUnqual(*impl_->context));
                    } else {
                        impl_->lastValue = impl_->normalizeIntC(
                            impl_->builder->CreateCall(methodFunc, args, "smcall"));
                    }
                    for (auto& [v, k] : argTemps) impl_->emitDecrefByKind(v, k);
                    impl_->emitMoveOutSlots(node);
                    return true;
                }
            }
        }
    }

    // Class instance method dispatch
    if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
        std::string className;
        std::string owningModule;
        if (objName->name == "self" && !impl_->currentClassName.empty()) {
            className = impl_->currentClassName;
            owningModule = impl_->currentModuleName;
        } else if (const auto* gb = impl_->globalClassBindingFor(objName->name)) {
            // Unshadowed module global: the scoped binding is authoritative.
            className = gb->className;
            owningModule = gb->owningModule;
        } else {
            auto vit = impl_->varClassNames.find(objName->name);
            if (vit != impl_->varClassNames.end()) className = vit->second;
            // Per-instance owning module (recorded at instantiation in Assign.cpp);
            // falls back to the class's recorded owner when the var carried none.
            auto vmIt = impl_->varClassOwningModule.find(objName->name);
            if (vmIt != impl_->varClassOwningModule.end()) {
                owningModule = vmIt->second;
            } else if (!className.empty()) {
                // Alias-aware resolver, not the last-write-wins map, else dispatch
                // picks a same-named class from another co-compiled module.
                owningModule = impl_->resolveClassOwningModule(className);
            }
            // AUTHORITATIVE OVERRIDE: the bare-name maps can hold a stale (class,
            // module); pin both from the InstanceType when that pin resolves the method.
            if (attr.object->type &&
                attr.object->type->kind() == Type::Kind::Instance) {
                auto* inst = static_cast<InstanceType*>(attr.object->type.get());
                if (inst->classType && !inst->classType->name.empty() &&
                    impl_->classNames.count(inst->classType->name)) {
                    const std::string& tcClass = inst->classType->name;
                    std::string tcMod = inst->classType->definingModule.empty()
                        ? impl_->resolveClassOwningModule(tcClass)
                        : inst->classType->definingModule;
                    if (impl_->resolveMethodFunction(tcMod, tcClass, method,
                                                     nullptr)) {
                        className = tcClass;
                        owningModule = tcMod;
                    }
                }
            }
        }
        if (!className.empty()) {
            // MRO lookup: walk the chain, mangling each level per its owning
            // module so same-named classes from different modules don't collide.
            std::string methodFuncName;
            auto* methodFunc = impl_->resolveMethodFunction(
                owningModule, className, method, &methodFuncName);
            // Self-correct a stale owning module: when the stored owner fails to
            // resolve the method, re-resolve it from the (alias-aware) className.
            if (!methodFunc) {
                std::string fresh = impl_->resolveClassOwningModule(className);
                if (fresh != owningModule) {
                    methodFunc = impl_->resolveMethodFunction(
                        fresh, className, method, &methodFuncName);
                    if (methodFunc) owningModule = fresh;
                }
            }
            if (methodFunc) {
                bool isStaticCall = impl_->staticMethods.count(methodFuncName) > 0;
                std::vector<llvm::Value*> args;
                auto methodFuncType = methodFunc->getFunctionType();

                if (!isStaticCall) {
                    // Instance method: pass self as first arg
                    attr.object->accept(*this);
                    llvm::Value* obj = impl_->lastValue;
                    if (!obj->getType()->isPointerTy())
                        obj = impl_->builder->CreateIntToPtr(obj, impl_->i8PtrType);
                    args.push_back(obj);
                }

                // Owned heap-temporary args to release after the call (the
                // method borrows; self is pushed separately and never listed).
                std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
                auto mpkIt = impl_->funcParamKinds.find(methodFuncName);
                unsigned paramOffset = isStaticCall ? 0 : 1;
                // A variadic method packs surplus positionals/keywords (self at
                // args[0]); precedes the fixed-arity spread path; drains via argTemps.
                if (impl_->funcVarArgInfo.count(methodFuncName)) {
                    if (!impl_->packVarArgMethodArgs(
                            *this, node, methodFuncName, methodFuncType, args,
                            argTemps, "method '" + method + "'"))
                        return true;  // diagnosed; lastValue poisoned
                } else if (callHasSpread(node)) {
                    if (!impl_->expandSpreadCallArgs(
                            *this, methodFunc, node, args, argTemps,
                            "method '" + method + "'"))
                        return true;  // diagnosed; lastValue poisoned
                } else {
                for (size_t i = 0; i < node.args.size(); ++i) {
                    node.args[i]->accept(*this);
                    llvm::Value* arg = impl_->lastValue;
                    unsigned paramIdx = (unsigned)(i + paramOffset);
                    // An own param ADOPTS the arg's +1: the callee releases it; a
                    // caller drain would double-free (A/B-proven fresh-temp probe).
                    bool argDrained = impl_->paramIsOwn(methodFuncName, paramIdx);
                    if (!argDrained &&
                        mpkIt != impl_->funcParamKinds.end() &&
                        paramIdx < mpkIt->second.size()) {
                        Impl::VarKind dk = impl_->argTempDecrefKind(
                            node.args[i].get(), mpkIt->second[paramIdx], arg);
                        if (dk != Impl::VarKind::Other) {
                            argTemps.emplace_back(arg, dk);
                            argDrained = true;
                        }
                    }
                    if (!argDrained) {
                        // Monomorphized generic (T erased -> non-heap kind): drain ONLY
                        // a provably-owned box (isOwnedBoxResult denylists borrows).
                        if (arg->getType() == impl_->boxType) {
                            if (impl_->isOwnedBoxResult(arg))
                                argTemps.emplace_back(arg, Impl::VarKind::Union);
                        } else {
                            // ownedTempDrainKind gates on the expression first, so a borrowed
                            // read never double-frees (A/B-proven UAF, test_augassign_targets).
                            Impl::VarKind dk = impl_->ownedTempDrainKind(
                                node.args[i].get(), arg);
                            if (dk != Impl::VarKind::Other)
                                argTemps.emplace_back(arg, dk);
                        }
                    }
                    if (paramIdx < methodFuncType->getNumParams())
                        arg = impl_->coerceArgFromExpr(node.args[i].get(), arg, methodFuncType->getParamType(paramIdx));
                    args.push_back(arg);
                }
                // D040: bind keyword args to named param slots (else silently
                // dropped); funcParamNames includes "self" at 0, no paramOffset.
                if (!node.kwArgs.empty()) {
                    auto pnIt = impl_->funcParamNames.find(methodFuncName);
                    if (pnIt != impl_->funcParamNames.end()) {
                        const auto& paramNames = pnIt->second;
                        size_t numParams = methodFuncType->getNumParams();
                        if (args.size() < numParams)
                            args.resize(numParams, nullptr);
                        for (auto& [kwName, kwVal] : node.kwArgs) {
                            auto nameIt = std::find(paramNames.begin(),
                                                    paramNames.end(), kwName);
                            if (nameIt == paramNames.end()) {
                                impl_->addError(
                                    "method '" + method +
                                    "' got an unexpected keyword argument '" +
                                    kwName + "'",
                                    node.location());
                                return true;
                            }
                            size_t idx = (size_t)std::distance(
                                paramNames.begin(), nameIt);
                            if (idx >= numParams || args[idx] != nullptr) {
                                impl_->addError(
                                    "method '" + method +
                                    "' got multiple values for argument '" +
                                    kwName + "'",
                                    node.location());
                                return true;
                            }
                            kwVal->accept(*this);
                            llvm::Value* arg = impl_->lastValue;
                            if (mpkIt != impl_->funcParamKinds.end() &&
                                idx < mpkIt->second.size()) {
                                Impl::VarKind dk = impl_->argTempDecrefKind(
                                    kwVal.get(), mpkIt->second[idx], arg);
                                if (dk != Impl::VarKind::Other)
                                    argTemps.emplace_back(arg, dk);
                            }
                            args[idx] = impl_->coerceArgFromExpr(
                                kwVal.get(), arg,
                                methodFuncType->getParamType(idx));
                        }
                    }
                }
                }  // end non-spread arg build (else branch of callHasSpread)
                // Fill missing args with defaults; the argTemps sink drains a
                // synthesized heap default, else one leaks per omitting call.
                impl_->fillDefaultArgs(methodFuncName, methodFunc, args, *this,
                                       &argTemps);

                // D026 virtual dispatch: devirtualize to a direct call unless a
                // subclass overrides - then the receiver may be one, use its vtable.
                llvm::Value* callee = methodFunc;
                if (!isStaticCall && impl_->methodIsOverridden(className, method)) {
                    auto idxIt = impl_->classMethodVtableIndicesBySym.find(impl_->classSym(className));
                    if (idxIt != impl_->classMethodVtableIndicesBySym.end()) {
                        auto mIt = idxIt->second.find(method);
                        if (mIt != idxIt->second.end()) {
                            // self is args[0]; load vtable (struct offset 2),
                            // GEP the method's (hierarchy-stable) ordinal.
                            auto* headerTy = llvm::StructType::get(*impl_->context,
                                {impl_->i64Type, impl_->i64Type, impl_->i8PtrType});
                            auto* vtSlot = impl_->builder->CreateStructGEP(
                                headerTy, args[0], 2, "vt_slot");
                            auto* vtPtr = impl_->builder->CreateLoad(
                                impl_->i8PtrType, vtSlot, "vtable");
                            auto* vtArrTy = llvm::ArrayType::get(impl_->i8PtrType, 0);
                            auto* mSlot = impl_->builder->CreateGEP(vtArrTy, vtPtr,
                                {impl_->builder->getInt64(0),
                                 impl_->builder->getInt64((int64_t)mIt->second)},
                                "method_slot");
                            callee = impl_->builder->CreateLoad(
                                impl_->i8PtrType, mSlot, "method_ptr");
                        }
                    }
                }

                // Exception-safe temps: unwind frees on raise, pop+decref on return.
                auto argTempBases = impl_->pushArgTempCleanups(argTemps);
                if (methodFuncType->getReturnType()->isVoidTy()) {
                    impl_->builder->CreateCall(methodFuncType, callee, args);
                    impl_->lastValue = llvm::ConstantPointerNull::get(
                        llvm::PointerType::getUnqual(*impl_->context));
                } else {
                    impl_->lastValue = impl_->normalizeIntC(
                        impl_->builder->CreateCall(methodFuncType, callee, args, "mcall"));
                }
                impl_->popArgTempCleanups(argTempBases);
                for (auto& [v, k] : argTemps) impl_->emitDecrefByKind(v, k);
                impl_->emitMoveOutSlots(node);
                return true;
            }

            // No method found: try a callable field (route.handler(...)). It may hold
            // a bare fn ptr or a DragonClosure* (trailing i8* env ABI) - tag-checked.
            {
                auto fieldIt = impl_->classFieldIndicesBySym.find(impl_->classSym(className));
                auto fieldTypeIt = impl_->classFieldTypesBySym.find(impl_->classSym(className));
                if (fieldIt != impl_->classFieldIndicesBySym.end() &&
                    fieldTypeIt != impl_->classFieldTypesBySym.end()) {
                    auto idxIt = fieldIt->second.find(method);
                    if (idxIt != fieldIt->second.end()) {
                        attr.object->accept(*this);
                        llvm::Value* objPtr = impl_->lastValue;
                        if (!objPtr->getType()->isPointerTy())
                            objPtr = impl_->builder->CreateIntToPtr(objPtr, impl_->i8PtrType);
                        auto structIt = impl_->classStructTypesBySym.find(impl_->classSym(className));
                        auto* gep = impl_->builder->CreateStructGEP(
                            structIt->second, objPtr, idxIt->second, method + "_ptr");
                        auto* fieldType = fieldTypeIt->second[method];
                        llvm::Value* fnPtr = impl_->builder->CreateLoad(
                            fieldType, gep, method + "_val");
                        if (!fnPtr->getType()->isPointerTy())
                            fnPtr = impl_->builder->CreateIntToPtr(fnPtr, impl_->i8PtrType);

                        // Recover the declared Callable[[A,...], R] FunctionType;
                        // else the synthetic all-i64 fallback (x86-64 GP aliasing).
                        auto cfIt = impl_->classFieldCallableTypeBySym.find(impl_->classSym(className));
                        llvm::FunctionType* userFnType = nullptr;
                        if (cfIt != impl_->classFieldCallableTypeBySym.end()) {
                            auto fIt = cfIt->second.find(method);
                            if (fIt != cfIt->second.end()) userFnType = fIt->second;
                        }

                        // Evaluate args once; coerce to the recovered signature when known.
                        std::vector<llvm::Value*> userArgs;
                        std::vector<llvm::Type*> bareArgTypes;
                        for (size_t i = 0; i < node.args.size(); ++i) {
                            node.args[i]->accept(*this);
                            llvm::Value* arg = impl_->lastValue;
                            if (userFnType && i < userFnType->getNumParams()) {
                                arg = impl_->coerceArg(
                                    arg, userFnType->getParamType(i));
                                bareArgTypes.push_back(userFnType->getParamType(i));
                            } else {
                                if (arg->getType() == impl_->i1Type)
                                    arg = impl_->builder->CreateZExt(arg, impl_->i64Type);
                                else if (arg->getType() == impl_->f64Type)
                                    arg = impl_->builder->CreateBitCast(arg, impl_->i64Type);
                                else if (arg->getType()->isPointerTy())
                                    arg = impl_->builder->CreatePtrToInt(arg, impl_->i64Type);
                                bareArgTypes.push_back(impl_->i64Type);
                            }
                            userArgs.push_back(arg);
                        }

                        llvm::Type* retTy = userFnType
                            ? userFnType->getReturnType()
                            : impl_->i64Type;
                        auto* bareFnType = llvm::FunctionType::get(
                            retTy, bareArgTypes, false);

                        // Closure variant: same user params + trailing i8* env.
                        std::vector<llvm::Type*> closureArgTypes(bareArgTypes);
                        closureArgTypes.push_back(impl_->i8PtrType);
                        auto* closureFnType = llvm::FunctionType::get(
                            retTy, closureArgTypes, false);

                        // Tag check: read type_tag (offset 8). For a bare fn ptr this
                        // reads .text - safe (r-x) and ~never DRAGON_TAG_CLOSURE.
                        auto* i8Ty = llvm::Type::getInt8Ty(*impl_->context);
                        auto* tagAddr = impl_->builder->CreateGEP(
                            i8Ty, fnPtr,
                            llvm::ConstantInt::get(impl_->i64Type, 8),
                            method + "_tag_addr");
                        auto* tagByte = impl_->builder->CreateLoad(
                            i8Ty, tagAddr, method + "_tag");
                        auto* isClosure = impl_->builder->CreateICmpEQ(
                            tagByte,
                            llvm::ConstantInt::get(i8Ty, 10), // DRAGON_TAG_CLOSURE
                            method + "_is_closure");

                        auto* fn = impl_->builder->GetInsertBlock()->getParent();
                        auto* closureBB = llvm::BasicBlock::Create(
                            *impl_->context, method + ".closure", fn);
                        auto* bareBB = llvm::BasicBlock::Create(
                            *impl_->context, method + ".bare", fn);
                        auto* mergeBB = llvm::BasicBlock::Create(
                            *impl_->context, method + ".cont", fn);
                        impl_->builder->CreateCondBr(isClosure, closureBB, bareBB);

                        // Closure path: unwrap DragonClosure { hdr, fn_ptr, env }
                        impl_->builder->SetInsertPoint(closureBB);
                        auto* closureStructType = llvm::StructType::getTypeByName(
                            *impl_->context, "DragonClosure");
                        if (!closureStructType) {
                            closureStructType = llvm::StructType::create(
                                *impl_->context,
                                {llvm::ArrayType::get(i8Ty, 16),
                                 impl_->i8PtrType,
                                 impl_->i8PtrType},
                                "DragonClosure");
                        }
                        auto* fnPtrAddr = impl_->builder->CreateStructGEP(
                            closureStructType, fnPtr, 1, "closure.fn.ptr");
                        auto* closureFn = impl_->builder->CreateLoad(
                            impl_->i8PtrType, fnPtrAddr, "closure.fn");
                        auto* envAddr = impl_->builder->CreateStructGEP(
                            closureStructType, fnPtr, 2, "closure.env.ptr");
                        auto* envPtr = impl_->builder->CreateLoad(
                            impl_->i8PtrType, envAddr, "closure.env");
                        std::vector<llvm::Value*> closureArgs(userArgs);
                        closureArgs.push_back(envPtr);
                        llvm::Value* closureRet = nullptr;
                        if (closureFnType->getReturnType()->isVoidTy()) {
                            impl_->builder->CreateCall(
                                closureFnType, closureFn, closureArgs);
                        } else {
                            closureRet = impl_->builder->CreateCall(
                                closureFnType, closureFn, closureArgs, "ccall");
                        }
                        impl_->builder->CreateBr(mergeBB);
                        auto* closureEndBB = impl_->builder->GetInsertBlock();

                        // Bare path: legacy fn pointer call
                        impl_->builder->SetInsertPoint(bareBB);
                        llvm::Value* bareRet = nullptr;
                        if (bareFnType->getReturnType()->isVoidTy()) {
                            impl_->builder->CreateCall(
                                bareFnType, fnPtr, userArgs);
                        } else {
                            bareRet = impl_->builder->CreateCall(
                                bareFnType, fnPtr, userArgs, "fieldcall");
                        }
                        impl_->builder->CreateBr(mergeBB);
                        auto* bareEndBB = impl_->builder->GetInsertBlock();

                        // Merge
                        impl_->builder->SetInsertPoint(mergeBB);
                        if (retTy->isVoidTy()) {
                            impl_->lastValue = llvm::ConstantPointerNull::get(
                                llvm::PointerType::getUnqual(*impl_->context));
                        } else {
                            auto* phi = impl_->builder->CreatePHI(
                                retTy, 2, method + ".ret");
                            phi->addIncoming(closureRet, closureEndBB);
                            phi->addIncoming(bareRet, bareEndBB);
                            impl_->lastValue = impl_->normalizeIntC(phi);
                        }
                        return true;
                    }
                }
            }
        } else {
            // D026 vtable dynamic dispatch: className unknown (obj = cls(); obj.speak()).
            auto vk = impl_->lookupVarKind(objName->name);
            if (vk == Impl::VarKind::ClassInstance) {
                // No concrete class recorded: accept a vtable candidate ONLY on arity
                // match, else a wrong-arity indirect call would be malformed IR.
                const size_t wantParams = node.args.size() + 1;  // self + args
                int methodIndex = -1;
                llvm::FunctionType* methodFuncType = nullptr;
                for (auto& [cls, methodMap] : impl_->classMethodVtableIndicesBySym) {
                    auto it = methodMap.find(method);
                    if (it == methodMap.end()) continue;
                    // `cls` is a sym; mangleClass("", sym) inside the resolver
                    // is the identity, so the direct + chain lookups are exact.
                    auto* func = impl_->resolveMethodFunction("", cls, method);
                    if (func && func->getFunctionType()->getNumParams() == wantParams) {
                        methodIndex = (int)it->second;
                        methodFuncType = func->getFunctionType();
                        break;
                    }
                }

                if (methodIndex >= 0 && methodFuncType) {
                    attr.object->accept(*this);
                    llvm::Value* objPtr = impl_->lastValue;
                    if (!objPtr->getType()->isPointerTy())
                        objPtr = impl_->builder->CreateIntToPtr(objPtr, impl_->i8PtrType);

                    // Load vtable from header struct offset 2.
                    auto* headerStructType = llvm::StructType::get(*impl_->context,
                        {impl_->i64Type, impl_->i64Type, impl_->i8PtrType});
                    auto* vtableSlot = impl_->builder->CreateStructGEP(
                        headerStructType, objPtr, 2, "vt_slot");
                    auto* vtablePtr = impl_->builder->CreateLoad(
                        impl_->i8PtrType, vtableSlot, "vtable");

                    auto* vtableArrayType = llvm::ArrayType::get(impl_->i8PtrType, 0);
                    auto* methodSlot = impl_->builder->CreateGEP(
                        vtableArrayType, vtablePtr,
                        {impl_->builder->getInt64(0), impl_->builder->getInt64(methodIndex)},
                        "method_slot");
                    auto* methodPtr = impl_->builder->CreateLoad(
                        impl_->i8PtrType, methodSlot, "method_ptr");

                    std::vector<llvm::Value*> args;
                    args.push_back(objPtr); // self
                    for (size_t i = 0; i < node.args.size(); ++i) {
                        node.args[i]->accept(*this);
                        llvm::Value* arg = impl_->lastValue;
                        unsigned paramIdx = (unsigned)(i + 1); // +1 for self
                        if (paramIdx < methodFuncType->getNumParams())
                            arg = impl_->coerceArgFromExpr(node.args[i].get(), arg, methodFuncType->getParamType(paramIdx));
                        args.push_back(arg);
                    }

                    if (methodFuncType->getReturnType()->isVoidTy()) {
                        impl_->builder->CreateCall(methodFuncType, methodPtr, args);
                        impl_->lastValue = llvm::ConstantPointerNull::get(
                            llvm::PointerType::getUnqual(*impl_->context));
                    } else {
                        impl_->lastValue = impl_->normalizeIntC(
                            impl_->builder->CreateCall(methodFuncType, methodPtr, args, "vcall"));
                    }
                    return true;
                }
            }
        }
    }

    // Class instance dispatch on a non-Name receiver (make_box(42).show(),
    // arr[0].id()); without it, chained calls silently produce i64 0.
    if (!dynamic_cast<NameExpr*>(attr.object.get())) {
        std::string className = impl_->resolveExprClassName(attr.object.get());
        if (!className.empty() && impl_->classNames.count(className)) {
            // Alias-aware owning-module resolution so a chained call on a temporary
            // honors `from X import Class` scoping; last-write-wins only as fallback.
            std::string owningModule =
                impl_->resolveClassOwningModule(className);
            std::string methodFuncName;
            auto* methodFunc = impl_->resolveMethodFunction(
                owningModule, className, method, &methodFuncName);
            if (methodFunc) {
                bool isStaticCall = impl_->staticMethods.count(methodFuncName) > 0;
                auto methodFuncType = methodFunc->getFunctionType();
                std::vector<llvm::Value*> args;
                // Owned heap-temporary args (and an owned-temp receiver, e.g.
                // `make().speak()`) to release after the call.
                std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
                if (!isStaticCall) {
                    attr.object->accept(*this);
                    llvm::Value* obj = impl_->lastValue;
                    if (!obj->getType()->isPointerTy())
                        obj = impl_->builder->CreateIntToPtr(obj, impl_->i8PtrType);
                    if (impl_->options.gcMode == GCMode::RC &&
                        !Impl::isBorrowedHeapExpr(attr.object.get()))
                        argTemps.emplace_back(obj, Impl::VarKind::ClassInstance);
                    args.push_back(obj);
                }
                auto mpkIt = impl_->funcParamKinds.find(methodFuncName);
                unsigned paramOffset = isStaticCall ? 0 : 1;
                // Variadic method: pack `*args`/`**kwargs` (self already pushed);
                // precedes the fixed-arity spread path.
                if (impl_->funcVarArgInfo.count(methodFuncName)) {
                    if (!impl_->packVarArgMethodArgs(
                            *this, node, methodFuncName, methodFuncType, args,
                            argTemps, "method '" + method + "'"))
                        return true;  // diagnosed; lastValue poisoned
                } else if (callHasSpread(node)) {
                    if (!impl_->expandSpreadCallArgs(
                            *this, methodFunc, node, args, argTemps,
                            "method '" + method + "'"))
                        return true;  // diagnosed; lastValue poisoned
                } else {
                for (size_t i = 0; i < node.args.size(); ++i) {
                    node.args[i]->accept(*this);
                    llvm::Value* arg = impl_->lastValue;
                    unsigned paramIdx = (unsigned)(i + paramOffset);
                    // An own param ADOPTS the arg's +1: the callee releases it; a
                    // caller drain would double-free (A/B-proven fresh-temp probe).
                    bool argDrained = impl_->paramIsOwn(methodFuncName, paramIdx);
                    if (!argDrained &&
                        mpkIt != impl_->funcParamKinds.end() &&
                        paramIdx < mpkIt->second.size()) {
                        Impl::VarKind dk = impl_->argTempDecrefKind(
                            node.args[i].get(), mpkIt->second[paramIdx], arg);
                        if (dk != Impl::VarKind::Other) {
                            argTemps.emplace_back(arg, dk);
                            argDrained = true;
                        }
                    }
                    if (!argDrained) {
                        // Monomorphized generic (T erased): drain a provably-owned box
                        // or a native owned temp; both classifiers reject borrows first.
                        if (arg->getType() == impl_->boxType) {
                            if (impl_->isOwnedBoxResult(arg))
                                argTemps.emplace_back(arg, Impl::VarKind::Union);
                        } else {
                            Impl::VarKind dk = impl_->ownedTempDrainKind(
                                node.args[i].get(), arg);
                            if (dk != Impl::VarKind::Other)
                                argTemps.emplace_back(arg, dk);
                        }
                    }
                    if (paramIdx < methodFuncType->getNumParams())
                        arg = impl_->coerceArgFromExpr(node.args[i].get(), arg, methodFuncType->getParamType(paramIdx));
                    args.push_back(arg);
                }
                // D040: bind keyword args to named param slots; funcParamNames
                // includes "self" at 0, no paramOffset.
                if (!node.kwArgs.empty()) {
                    auto pnIt = impl_->funcParamNames.find(methodFuncName);
                    if (pnIt != impl_->funcParamNames.end()) {
                        const auto& paramNames = pnIt->second;
                        size_t numParams = methodFuncType->getNumParams();
                        if (args.size() < numParams)
                            args.resize(numParams, nullptr);
                        for (auto& [kwName, kwVal] : node.kwArgs) {
                            auto nameIt = std::find(paramNames.begin(),
                                                    paramNames.end(), kwName);
                            if (nameIt == paramNames.end()) {
                                impl_->addError(
                                    "method '" + method +
                                    "' got an unexpected keyword argument '" +
                                    kwName + "'",
                                    node.location());
                                return true;
                            }
                            size_t idx = (size_t)std::distance(
                                paramNames.begin(), nameIt);
                            if (idx >= numParams || args[idx] != nullptr) {
                                impl_->addError(
                                    "method '" + method +
                                    "' got multiple values for argument '" +
                                    kwName + "'",
                                    node.location());
                                return true;
                            }
                            kwVal->accept(*this);
                            llvm::Value* arg = impl_->lastValue;
                            if (mpkIt != impl_->funcParamKinds.end() &&
                                idx < mpkIt->second.size()) {
                                Impl::VarKind dk = impl_->argTempDecrefKind(
                                    kwVal.get(), mpkIt->second[idx], arg);
                                if (dk != Impl::VarKind::Other)
                                    argTemps.emplace_back(arg, dk);
                            }
                            args[idx] = impl_->coerceArgFromExpr(
                                kwVal.get(), arg,
                                methodFuncType->getParamType(idx));
                        }
                    }
                }
                }  // end non-spread arg build (else branch of callHasSpread)
                // Fill omitted params with defaults (too few args is malformed IR);
                // the argTemps sink drains synthesized heap defaults.
                impl_->fillDefaultArgs(methodFuncName, methodFunc, args, *this,
                                       &argTemps);

                // D026: same devirtualize-unless-overridden rule as the NameExpr path.
                llvm::Value* callee = methodFunc;
                if (!isStaticCall && impl_->methodIsOverridden(className, method)) {
                    auto idxIt = impl_->classMethodVtableIndicesBySym.find(impl_->classSym(className));
                    if (idxIt != impl_->classMethodVtableIndicesBySym.end()) {
                        auto mIt = idxIt->second.find(method);
                        if (mIt != idxIt->second.end()) {
                            auto* headerTy = llvm::StructType::get(*impl_->context,
                                {impl_->i64Type, impl_->i64Type, impl_->i8PtrType});
                            auto* vtSlot = impl_->builder->CreateStructGEP(
                                headerTy, args[0], 2, "vt_slot");
                            auto* vtPtr = impl_->builder->CreateLoad(
                                impl_->i8PtrType, vtSlot, "vtable");
                            auto* vtArrTy = llvm::ArrayType::get(impl_->i8PtrType, 0);
                            auto* mSlot = impl_->builder->CreateGEP(vtArrTy, vtPtr,
                                {impl_->builder->getInt64(0),
                                 impl_->builder->getInt64((int64_t)mIt->second)},
                                "method_slot");
                            callee = impl_->builder->CreateLoad(
                                impl_->i8PtrType, mSlot, "method_ptr");
                        }
                    }
                }

                // Exception-safe temps (see the NameExpr-receiver path above).
                auto argTempBases = impl_->pushArgTempCleanups(argTemps);
                if (methodFuncType->getReturnType()->isVoidTy()) {
                    impl_->builder->CreateCall(methodFuncType, callee, args);
                    impl_->lastValue = llvm::ConstantPointerNull::get(
                        llvm::PointerType::getUnqual(*impl_->context));
                } else {
                    impl_->lastValue = impl_->normalizeIntC(
                        impl_->builder->CreateCall(methodFuncType, callee, args, "mcall"));
                }
                impl_->popArgTempCleanups(argTempBases);
                for (auto& [v, k] : argTemps) impl_->emitDecrefByKind(v, k);
                impl_->emitMoveOutSlots(node);
                return true;
            }
        }
    }

    // Stdlib module method dispatch (e.g. math.sqrt)
    if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
        std::string qualName = objName->name + "." + method;
        auto aliasIt = impl_->symbolAliases.find(qualName);
        if (aliasIt != impl_->symbolAliases.end()) {
            const std::string& cName = aliasIt->second;
            // Math functions: double -> double
            if (node.args.size() == 1) {
                node.args[0]->accept(*this);
                llvm::Value* arg = impl_->lastValue;
                if (arg->getType() == impl_->i64Type)
                    arg = impl_->builder->CreateSIToFP(arg, impl_->f64Type);
                auto* fn = impl_->getOrDeclareRuntime(cName,
                    llvm::FunctionType::get(impl_->f64Type, {impl_->f64Type}, false));
                impl_->lastValue = impl_->builder->CreateCall(fn, {arg}, cName);
                return true;
            }
            // Two-arg math functions (e.g., pow)
            if (node.args.size() == 2) {
                node.args[0]->accept(*this);
                llvm::Value* arg1 = impl_->lastValue;
                node.args[1]->accept(*this);
                llvm::Value* arg2 = impl_->lastValue;
                if (arg1->getType() == impl_->i64Type)
                    arg1 = impl_->builder->CreateSIToFP(arg1, impl_->f64Type);
                if (arg2->getType() == impl_->i64Type)
                    arg2 = impl_->builder->CreateSIToFP(arg2, impl_->f64Type);
                auto* fn = impl_->getOrDeclareRuntime(cName,
                    llvm::FunctionType::get(impl_->f64Type, {impl_->f64Type, impl_->f64Type}, false));
                impl_->lastValue = impl_->builder->CreateCall(fn, {arg1, arg2}, cName);
                return true;
            }
            // Zero-arg (e.g., time.time()) - returns double
            if (node.args.empty()) {
                auto* fn = impl_->getOrDeclareRuntime(cName,
                    llvm::FunctionType::get(impl_->f64Type, {}, false));
                impl_->lastValue = impl_->builder->CreateCall(fn, {}, cName);
                return true;
            }
        }
    }

    return false;
}

} // namespace dragon
