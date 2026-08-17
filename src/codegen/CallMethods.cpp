#include "../CodeGenImpl.h"

namespace dragon {

bool CodeGen::Impl::packVarArgMethodArgs(
        CodeGen& cg, CallExpr& node, const std::string& methodFuncName,
        llvm::FunctionType* methodFuncType,
        std::vector<llvm::Value*>& args,
        std::vector<std::pair<llvm::Value*, VarKind>>& argTemps,
        const std::string& dispName) {
    auto vaIt = funcVarArgInfo.find(methodFuncName);
    if (vaIt == funcVarArgInfo.end()) return true;
    const VarArgInfo& vaInfo = vaIt->second;

    auto poison = [&]() {
        lastValue = llvm::ConstantPointerNull::get(
            llvm::PointerType::getUnqual(*context));
    };

    if (callHasSpread(node)) {
        addError("call-site spread (`*`/`**`) into a variadic method is not yet "
                 "supported", node.location());
        poison();
        return false;
    }

    const size_t selfOffset = args.size();
    const size_t numParams = methodFuncType->getNumParams();

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
    while (args.size() < selfOffset + vaInfo.numRegularParams) {
        args.push_back(nullptr);
        llvmIdx++;
    }

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

    std::vector<bool> kwConsumed(node.kwArgs.size(), false);
    if (!node.kwArgs.empty()) {
        auto pnIt = funcParamNames.find(methodFuncName);
        if (pnIt != funcParamNames.end()) {
            const auto& paramNames = pnIt->second;
            for (size_t ki = 0; ki < node.kwArgs.size(); ++ki) {
                const std::string& kwName = node.kwArgs[ki].first;
                if (kwName.empty()) continue;
                auto nameIt =
                    std::find(paramNames.begin(), paramNames.end(), kwName);
                if (nameIt == paramNames.end()) continue;
                size_t idx = (size_t)std::distance(paramNames.begin(), nameIt);
                if (idx < selfOffset) continue;
                if (idx - selfOffset >= vaInfo.numRegularParams)
                    continue;
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
            int64_t tag = TAG_INT;
            if (val->getType() == i1Type) {
                tag = TAG_BOOL;
                val = builder->CreateZExt(val, i64Type);
            } else if (val->getType() == f64Type) {
                tag = TAG_FLOAT;
                val = builder->CreateBitCast(val, i64Type);
            } else if (val->getType()->isPointerTy()) {
                tag = TAG_STR;
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

    std::vector<llvm::Type*> paramTys;
    paramTys.push_back(i8PtrType);
    for (auto& p : sig->params) paramTys.push_back(typeExprToLLVM(p.type.get()));
    llvm::Type* retTy = sig->returnType ? typeExprToLLVM(sig->returnType.get())
                                        : voidType;
    auto* fnType = llvm::FunctionType::get(retTy, paramTys, false);

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
    drainBorrowTemps(argTemps);
    return true;
}

bool CodeGen::emitMethodCall(CallExpr& node, AttributeExpr& attr) {
    std::string method = attr.attribute;
    if (node.resolvedMethodOverload >= 0)
        method += "__ov" + std::to_string(node.resolvedMethodOverload);

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
            std::string cn = impl_->resolveExprClassName(attr.object.get());
            instanceRecv = !cn.empty() && impl_->classNames.count(cn) > 0;
        }
        if (!instanceRecv) return false;
    }

    if (attr.object && attr.object->type &&
        attr.object->type->kind() == Type::Kind::Contract) {
        return impl_->emitContractMethodCall(*this, node, attr);
    }

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
                    impl_->lastValue = impl_->taskResultFromI64(raw, node.type.get());
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
                    impl_->lastValue = impl_->builder->CreateICmpNE(
                        raw, llvm::ConstantInt::get(impl_->i64Type, 0), "vthread.alive.b");
                    return true;
                }
            }
        }
    }

    if (impl_->isLockExpr(attr.object.get())) {
        llvm::Value* handle = nullptr;
        if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
            llvm::Value* handlePtr = impl_->lookupVar(objName->name);
            if (!handlePtr) handlePtr = impl_->lookupModuleGlobal(objName->name);
            if (handlePtr)
                handle = impl_->builder->CreateLoad(impl_->i8PtrType, handlePtr, "lock.handle");
        } else {
            attr.object->accept(*this);
            handle = impl_->lastValue;
            if (handle && !handle->getType()->isPointerTy())
                handle = impl_->builder->CreateIntToPtr(handle, impl_->i8PtrType, "lock.handle");
        }
        {
            if (handle) {
                if (method == "acquire") {
                    Expr* blockingExpr = nullptr;
                    Expr* timeoutExpr = nullptr;
                    if (node.args.size() >= 1) blockingExpr = node.args[0].get();
                    if (node.args.size() >= 2) timeoutExpr = node.args[1].get();
                    for (auto& kw : node.kwArgs) {
                        if (kw.first == "blocking") blockingExpr = kw.second.get();
                        else if (kw.first == "timeout") timeoutExpr = kw.second.get();
                    }

                    if (!blockingExpr && !timeoutExpr) {
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_lock_acquire"], {handle});
                        impl_->lastValue =
                            llvm::ConstantInt::get(impl_->i1Type, 1);
                        return true;
                    }
                    llvm::Value* blk = llvm::ConstantInt::get(impl_->i64Type, 1);
                    if (blockingExpr) {
                        blockingExpr->accept(*this);
                        blk = impl_->lastValue;
                        if (blk->getType() == impl_->i1Type)
                            blk = impl_->builder->CreateZExt(blk, impl_->i64Type);
                        else if (blk->getType()->isPointerTy())
                            blk = impl_->builder->CreatePtrToInt(blk, impl_->i64Type);
                    }
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

    bool isBytes = attr.object && attr.object->type &&
                   attr.object->type->kind() == Type::Kind::Bytes;
    if (!isBytes) {
        if (auto* sl = dynamic_cast<StringLiteral*>(attr.object.get()))
            isBytes = sl->isBytes;
    }

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
                    int64_t elemTag = 0;
                    if (auto* lt = dynamic_cast<ListType*>(attr.object->type.get())) {
                        if (lt->elementType) {
                            int64_t t = impl_->typeKindToTag(lt->elementType->kind());
                            if (t > 0) elemTag = t;
                        }
                    }
                    if (val->getType() == impl_->i1Type) {
                        if (elemTag == TAG_INT) elemTag = TAG_BOOL;
                        val = impl_->builder->CreateZExt(val, impl_->i64Type);
                    } else if (val->getType() == impl_->f64Type) {
                        if (elemTag == TAG_INT) elemTag = TAG_FLOAT;
                        val = impl_->builder->CreateBitCast(val, impl_->i64Type);
                    } else if (val->getType()->isPointerTy()) {
                        if (elemTag == TAG_INT) elemTag = TAG_STR;
                        val = impl_->builder->CreatePtrToInt(val, impl_->i64Type);
                    }
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs[method == "append"
                                                ? "dragon_deque_append"
                                                : "dragon_deque_appendleft"],
                        {handle, val,
                         llvm::ConstantInt::get(impl_->i64Type, elemTag)});
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

    if (isStr) {
        attr.object->accept(*this);
        llvm::Value* obj = impl_->lastValue;
        bool ownedStrRecv =
            impl_->options.gcMode == GCMode::RC && impl_->isOwnedStrResult(obj);
        std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
        std::vector<llvm::Value*> argTempBases;
        if (ownedStrRecv)
            argTempBases.push_back(
                impl_->emitCleanupPushTemp(obj, Impl::DCLEAN_STR));
        bool strHandled = [&]() -> bool {

        if ((method == "strip" || method == "lstrip" || method == "rstrip") &&
            node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* chars = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_" + method + "_chars",
                llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType, impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, chars}, method);
            return true;
        }

        if (method == "upper" || method == "lower" || method == "strip" ||
            method == "lstrip" || method == "rstrip" || method == "title" ||
            method == "capitalize" || method == "swapcase" || method == "casefold") {
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_" + method,
                llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj}, method);
            return true;
        }

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

        if ((method == "removeprefix" || method == "removesuffix") && node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* arg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_" + method,
                llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType, impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, arg}, method);
            return true;
        }

        if ((method == "startswith" || method == "endswith" || method == "contains") &&
            node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* arg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_" + method,
                llvm::FunctionType::get(impl_->i64Type, {impl_->i8PtrType, impl_->i8PtrType}, false));
            llvm::Value* call = impl_->builder->CreateCall(fn, {obj, arg}, method);
            impl_->lastValue = impl_->builder->CreateICmpNE(
                call, llvm::ConstantInt::get(impl_->i64Type, 0), method + ".b");
            return true;
        }

        if ((method == "find" || method == "rfind" || method == "count") &&
            node.args.size() >= 1 && node.args.size() <= 3) {
            node.args[0]->accept(*this);
            llvm::Value* sub = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
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

        if ((method == "index" || method == "rindex") && node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* arg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
            std::string rtName = (method == "index") ? "dragon_str_index_of" : "dragon_str_rindex";
            auto* fn = impl_->getOrDeclareRuntime(rtName,
                llvm::FunctionType::get(impl_->i64Type, {impl_->i8PtrType, impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, arg}, method);
            return true;
        }

        if (method == "replace" && node.args.size() >= 2) {
            node.args[0]->accept(*this);
            llvm::Value* old_s = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
            node.args[1]->accept(*this);
            llvm::Value* new_s = impl_->trackBorrowTempGuarded(node.args[1].get(), impl_->lastValue, argTemps, argTempBases);
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

        if (method == "zfill" && node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* width = impl_->lastValue;
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_zfill",
                llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType, impl_->i64Type}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, width}, "zfill");
            return true;
        }

        if (method == "expandtabs") {
            llvm::Value* tabsize = llvm::ConstantInt::get(impl_->i64Type, 8);
            if (node.args.size() >= 1) { node.args[0]->accept(*this); tabsize = impl_->lastValue; }
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_expandtabs",
                llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType, impl_->i64Type}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, tabsize}, "expandtabs");
            return true;
        }

        if (method == "center" || method == "ljust" || method == "rjust") {
            if (node.args.size() >= 1) {
                node.args[0]->accept(*this);
                llvm::Value* width = impl_->lastValue;
                llvm::Value* fill = llvm::ConstantInt::get(
                    llvm::Type::getInt8Ty(*impl_->context), ' ');
                if (node.args.size() >= 2) {
                    node.args[1]->accept(*this);
                    llvm::Value* fillStr = impl_->trackBorrowTempGuarded(node.args[1].get(), impl_->lastValue, argTemps, argTempBases);
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

        if (method == "split" || method == "rsplit") {
            llvm::Value* sep = llvm::ConstantPointerNull::get(
                llvm::PointerType::getUnqual(*impl_->context));
            if (node.args.size() >= 1) { node.args[0]->accept(*this); sep = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases); }
            llvm::Value* maxsplit = llvm::ConstantInt::get(impl_->i64Type, -1);
            if (node.args.size() >= 2) { node.args[1]->accept(*this); maxsplit = impl_->lastValue; }
            const char* rt = (method == "split") ? "dragon_str_split_max" : "dragon_str_rsplit";
            auto* fn = impl_->getOrDeclareRuntime(rt,
                llvm::FunctionType::get(impl_->i8PtrType,
                    {impl_->i8PtrType, impl_->i8PtrType, impl_->i64Type}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, sep, maxsplit}, method);
            return true;
        }

        if (method == "join" && node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* list = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_join",
                llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType, impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, list}, "join");
            return true;
        }

        if (method == "splitlines") {
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_splitlines",
                llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj}, "splitlines");
            return true;
        }

        if ((method == "partition" || method == "rpartition") && node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* sep = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_" + method,
                llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType, impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, sep}, method);
            return true;
        }

        if (method == "encode") {
            llvm::Value* enc = impl_->builder->CreateGlobalString("utf-8");
            llvm::Value* err = impl_->builder->CreateGlobalString("strict");
            if (node.args.size() >= 1) { node.args[0]->accept(*this); enc = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases); }
            if (node.args.size() >= 2) { node.args[1]->accept(*this); err = impl_->trackBorrowTempGuarded(node.args[1].get(), impl_->lastValue, argTemps, argTempBases); }
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_encode_ex",
                llvm::FunctionType::get(impl_->i8PtrType,
                    {impl_->i8PtrType, impl_->i8PtrType, impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, enc, err}, "encode");
            return true;
        }
        return false;
        }();
        impl_->popArgTempCleanups(argTempBases);
        if (strHandled) {
            impl_->drainBorrowTemps(argTemps);
            impl_->emitMoveOutSlots(node);
            if (ownedStrRecv)
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref_str"], {obj});
            return true;
        }
    }

    if (isBytes) {
        attr.object->accept(*this);
        llvm::Value* obj = impl_->lastValue;
        bool ownedBytesRecv =
            impl_->options.gcMode == GCMode::RC && impl_->isOwnedPtrResult(obj);
        std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
        std::vector<llvm::Value*> argTempBases;
        if (ownedBytesRecv)
            argTempBases.push_back(
                impl_->emitCleanupPushTemp(obj, Impl::DCLEAN_OBJ));
        bool bytesHandled = [&]() -> bool {

        if (method == "upper" || method == "lower" || method == "strip" ||
            method == "lstrip" || method == "rstrip") {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_" + method], {obj}, method);
            return true;
        }

        if (method == "isdigit" || method == "isalpha" || method == "isalnum" ||
            method == "isspace") {
            llvm::Value* call = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_" + method], {obj}, method);
            impl_->lastValue = impl_->builder->CreateICmpNE(
                call, llvm::ConstantInt::get(impl_->i64Type, 0), method + ".b");
            return true;
        }

        if (method == "decode") {
            llvm::Value* enc = impl_->builder->CreateGlobalString("utf-8");
            llvm::Value* err = impl_->builder->CreateGlobalString("strict");
            if (node.args.size() >= 1) { node.args[0]->accept(*this); enc = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases); }
            if (node.args.size() >= 2) { node.args[1]->accept(*this); err = impl_->trackBorrowTempGuarded(node.args[1].get(), impl_->lastValue, argTemps, argTempBases); }
            auto* fn = impl_->getOrDeclareRuntime("dragon_bytes_decode_ex",
                llvm::FunctionType::get(impl_->i8PtrType,
                    {impl_->i8PtrType, impl_->i8PtrType, impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, enc, err}, "decode");
            return true;
        }

        if (method == "hex") {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_hex"], {obj}, "hex");
            return true;
        }

        if ((method == "startswith" || method == "endswith") &&
            node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* arg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
            llvm::Value* call = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_" + method], {obj, arg}, method);
            impl_->lastValue = impl_->builder->CreateICmpNE(
                call, llvm::ConstantInt::get(impl_->i64Type, 0), method + ".b");
            return true;
        }

        if ((method == "find" || method == "rfind" || method == "count") &&
            node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* arg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_" + method], {obj, arg}, method);
            return true;
        }

        if ((method == "index" || method == "rindex") && node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* arg = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
            std::string rtName = (method == "index") ? "dragon_bytes_index_of" : "dragon_bytes_rindex";
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs[rtName], {obj, arg}, method);
            return true;
        }

        if (method == "replace" && node.args.size() >= 2) {
            node.args[0]->accept(*this);
            llvm::Value* old_b = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
            node.args[1]->accept(*this);
            llvm::Value* new_b = impl_->trackBorrowTempGuarded(node.args[1].get(), impl_->lastValue, argTemps, argTempBases);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_replace"], {obj, old_b, new_b}, "replace");
            return true;
        }

        if (method == "split") {
            llvm::Value* sep = llvm::ConstantPointerNull::get(
                llvm::PointerType::getUnqual(*impl_->context));
            if (node.args.size() >= 1) { node.args[0]->accept(*this); sep = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases); }
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_split"], {obj, sep}, "split");
            return true;
        }

        if (method == "join" && node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* list = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_join"], {obj, list}, "join");
            return true;
        }
        return false;
        }();
        impl_->popArgTempCleanups(argTempBases);
        if (bytesHandled) {
            impl_->drainBorrowTemps(argTemps);
            impl_->emitMoveOutSlots(node);
            if (ownedBytesRecv)
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref"], {obj});
            return true;
        }
    }

    if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
        if (objName->name == "bytes" && method == "fromhex" && node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* hexStr = impl_->lastValue;
            std::vector<std::pair<llvm::Value*, Impl::VarKind>> hexTemps;
            Impl::VarKind dk = impl_->ownedTempDrainKind(node.args[0].get(), hexStr);
            if (dk != Impl::VarKind::Other) hexTemps.emplace_back(hexStr, dk);
            auto hexBases = impl_->pushArgTempCleanups(hexTemps);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_fromhex"], {hexStr}, "fromhex");
            impl_->popArgTempCleanups(hexBases);
            impl_->drainBorrowTemps(hexTemps);
            return true;
        }
        if (objName->name == "dict" && method == "fromkeys" &&
            (node.args.size() == 1 || node.args.size() == 2)) {
            node.args[0]->accept(*this);
            llvm::Value* keysList = impl_->lastValue;
            llvm::Value* val;
            llvm::Value* tag;
            if (node.args.size() == 2) {
                node.args[1]->accept(*this);
                llvm::Value* raw = impl_->lastValue;
                int64_t tagVal = -1;
                if (node.args[1] && node.args[1]->type)
                    tagVal = Impl::typeKindToTag(node.args[1]->type->kind());
                if (tagVal < 0) {
                    Impl::VarKind vk = Impl::VarKind::Other;
                    if (auto* nm = dynamic_cast<NameExpr*>(node.args[1].get()))
                        vk = impl_->lookupVarKind(nm->name);
                    else if (auto* sl = dynamic_cast<StringLiteral*>(node.args[1].get()))
                        tagVal = sl->isBytes ? TAG_BYTES : TAG_STR;
                    else if (dynamic_cast<IntegerLiteral*>(node.args[1].get()))
                        vk = Impl::VarKind::Int;
                    else if (dynamic_cast<FloatLiteral*>(node.args[1].get()))
                        vk = Impl::VarKind::Float;
                    else if (dynamic_cast<BooleanLiteral*>(node.args[1].get()))
                        vk = Impl::VarKind::Bool;
                    if (tagVal < 0) tagVal = Impl::varKindToTag(vk);
                }
                if (tagVal < TAG_INT) tagVal = TAG_INT;
                if (raw->getType() == impl_->i1Type)
                    raw = impl_->builder->CreateZExt(raw, impl_->i64Type);
                else if (raw->getType() == impl_->f64Type)
                    raw = impl_->builder->CreateBitCast(raw, impl_->i64Type);
                else if (raw->getType()->isPointerTy())
                    raw = impl_->builder->CreatePtrToInt(raw, impl_->i64Type);
                val = raw;
                tag = llvm::ConstantInt::get(impl_->i64Type, tagVal);
            } else {
                val = llvm::ConstantInt::get(impl_->i64Type, 0);
                tag = llvm::ConstantInt::get(impl_->i64Type, TAG_NONE);
            }
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_fromkeys"],
                {keysList, val, tag}, "fromkeys");
            return true;
        }
    }

    if (isList) {
        attr.object->accept(*this);
        llvm::Value* obj = impl_->lastValue;
        bool ownedListRecv = impl_->options.gcMode == GCMode::RC &&
                             !Impl::isBorrowedHeapExpr(attr.object.get()) &&
                             impl_->isOwnedPtrResult(obj);
        static const std::set<std::string> kListRecvDrainOk = {
            "append", "insert", "extend", "remove", "clear", "sort",
            "reverse", "count", "index", "pop", "copy"};
        std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
        std::vector<llvm::Value*> argTempBases;
        if (ownedListRecv && kListRecvDrainOk.count(method))
            argTempBases.push_back(
                impl_->emitCleanupPushTemp(obj, Impl::DCLEAN_OBJ));
        bool listHandled = [&]() -> bool {

        if (method == "append" && node.args.size() == 1) {
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
            Type::Kind appendElemKind = impl_->getIterableElementKind(attr.object.get());
            if (appendElemKind == Type::Kind::Any) {
                auto tp = impl_->boxArgTagPayload(node.args[0].get(),
                                                  val, true);
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
                       appendElemKind == Type::Kind::Function ||
                       appendElemKind == Type::Kind::Instance) {
                if (appendElemKind == Type::Kind::Str && val->getType()->isPointerTy())
                    val = impl_->ensureHeapString(val, node.args[0].get());
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
                if (impl_->options.gcMode == GCMode::RC && !freshWrappedClosure &&
                    Impl::isBorrowedHeapExpr(node.args[0].get())) {
                    if (appendElemKind == Type::Kind::Str)
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_incref_str"], {val});
                    else if (appendElemKind == Type::Kind::Function)
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_incref_callable"], {val});
                    else
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_incref"], {val});
                }
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_list_append_ptr"], {obj, val});
            } else {
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
            if (impl_->getIterableElementKind(attr.object.get()) ==
                Type::Kind::Any) {
                if (idx->getType() == impl_->i1Type)
                    idx = impl_->builder->CreateZExt(idx, impl_->i64Type);
                else if (idx->getType()->isPointerTy())
                    idx = impl_->builder->CreatePtrToInt(idx, impl_->i64Type);
                else if (idx->getType() != impl_->i64Type)
                    idx = impl_->builder->CreateZExtOrTrunc(idx, impl_->i64Type);
                auto tp = impl_->boxArgTagPayload(node.args[1].get(),
                                                  val, true);
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_list_box_insert"],
                    {obj, idx, tp.first, tp.second});
                impl_->lastValue = llvm::ConstantPointerNull::get(
                    llvm::PointerType::getUnqual(*impl_->context));
                return true;
            }
            val = impl_->trackBorrowTempGuarded(node.args[1].get(), val, argTemps, argTempBases);
            if (val->getType() == impl_->f64Type) val = impl_->builder->CreateBitCast(val, impl_->i64Type);
            else if (val->getType() == impl_->i1Type) val = impl_->builder->CreateZExt(val, impl_->i64Type);
            else if (val->getType()->isPointerTy()) val = impl_->builder->CreatePtrToInt(val, impl_->i64Type);
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_list_insert"], {obj, idx, val});
            impl_->lastValue = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*impl_->context));
            return true;
        }
        if (method == "remove" && node.args.size() == 1) {
            node.args[0]->accept(*this);
            llvm::Value* val = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
            if (impl_->getIterableElementKind(attr.object.get()) ==
                Type::Kind::Any) {
                auto tp = impl_->boxArgTagPayload(node.args[0].get(),
                                                  val, false);
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
            if (popElemKind == Type::Kind::Float) {
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_list_pop_f64"], {obj, idx},
                    "listpop.f64");
                return true;
            }
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
            llvm::Value* other = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_list_extend"], {obj, other});
            impl_->lastValue = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*impl_->context));
            return true;
        }
        if (method == "index" && node.args.size() == 1) {
            node.args[0]->accept(*this);
            llvm::Value* val = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
            if (val->getType() == impl_->f64Type) val = impl_->builder->CreateBitCast(val, impl_->i64Type);
            else if (val->getType() == impl_->i1Type) val = impl_->builder->CreateZExt(val, impl_->i64Type);
            else if (val->getType()->isPointerTy()) val = impl_->builder->CreatePtrToInt(val, impl_->i64Type);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_index"], {obj, val}, "listindex");
            return true;
        }
        if (method == "count" && node.args.size() == 1) {
            node.args[0]->accept(*this);
            llvm::Value* val = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
            if (val->getType() == impl_->f64Type) val = impl_->builder->CreateBitCast(val, impl_->i64Type);
            else if (val->getType() == impl_->i1Type) val = impl_->builder->CreateZExt(val, impl_->i64Type);
            else if (val->getType()->isPointerTy()) val = impl_->builder->CreatePtrToInt(val, impl_->i64Type);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_count"], {obj, val}, "listcount");
            return true;
        }
        if (method == "sort" && node.args.empty()) {
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
        return false;
        }();
        impl_->popArgTempCleanups(argTempBases);
        if (listHandled) {
            impl_->drainBorrowTemps(argTemps);
            impl_->emitMoveOutSlots(node);
            if (ownedListRecv && kListRecvDrainOk.count(method))
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref"], {obj});
            return true;
        }
    }

    if (isDict) {
        attr.object->accept(*this);
        llvm::Value* obj = impl_->lastValue;

        Type::Kind dictKk = impl_->resolveDictKeyKind(attr.object.get());
        bool intKeyed = dictKk == Type::Kind::Int || dictKk == Type::Kind::Float;
        auto normDictKey = [&](llvm::Value* k) {
            return dictKk == Type::Kind::Float ? impl_->emitFloatDictKeyBits(k)
                                               : k;
        };

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
                    return raw;
            }
        };
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

        bool ownedDictRecv = impl_->options.gcMode == GCMode::RC &&
                             !Impl::isBorrowedHeapExpr(attr.object.get()) &&
                             impl_->isOwnedPtrResult(obj);
        static const std::set<std::string> kDictRecvDrainOk = {
            "pop", "popitem", "clear", "update"};
        std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
        std::vector<llvm::Value*> argTempBases;
        if (ownedDictRecv && kDictRecvDrainOk.count(method))
            argTempBases.push_back(
                impl_->emitCleanupPushTemp(obj, Impl::DCLEAN_OBJ));
        bool dictHandled = [&]() -> bool {

        if (method == "get" && node.args.size() == 1) {
            node.args[0]->accept(*this);
            llvm::Value* key = normDictKey(impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases));
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

        if (method == "get" && node.args.size() == 2) {
            node.args[0]->accept(*this);
            llvm::Value* key = normDictKey(impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases));
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

        if (method == "keys" && node.args.empty()) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_keys"], {obj}, "dictkeys");
            return true;
        }

        if (method == "has_key" && node.args.size() == 1) {
            node.args[0]->accept(*this);
            llvm::Value* key = normDictKey(impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases));
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs[intKeyed ? "dragon_dict_int_has_key"
                                             : "dragon_dict_has_key"],
                {obj, key}, "haskey");
            return true;
        }

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

        if (method == "items" && node.args.empty()) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_items"], {obj}, "dictitems");
            return true;
        }

        if (method == "popitem" && node.args.empty()) {
            llvm::Value* tupleI64 = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_popitem"], {obj}, "dictpopitem");
            impl_->lastValue = impl_->builder->CreateIntToPtr(
                tupleI64, impl_->i8PtrType, "popitem_ptr");
            return true;
        }

        if (method == "pop" && node.args.size() == 1) {
            node.args[0]->accept(*this);
            llvm::Value* key = normDictKey(impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases));
            llvm::Value* popped = impl_->builder->CreateCall(
                impl_->runtimeFuncs[intKeyed ? "dragon_dict_int_pop"
                                             : "dragon_dict_pop"],
                {obj, key}, "dictpop");
            impl_->lastValue = intKeyed ? coerceDictValue(popped) : popped;
            return true;
        }

        if (method == "pop" && node.args.size() == 2) {
            node.args[0]->accept(*this);
            llvm::Value* key = normDictKey(impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases));
            node.args[1]->accept(*this);
            llvm::Value* defVal = impl_->lastValue;
            if (defVal->getType() == impl_->i1Type) defVal = impl_->builder->CreateZExt(defVal, impl_->i64Type);
            else if (defVal->getType() == impl_->f64Type) defVal = impl_->builder->CreateBitCast(defVal, impl_->i64Type);
            else if (defVal->getType()->isPointerTy()) defVal = impl_->builder->CreatePtrToInt(defVal, impl_->i64Type);
            llvm::Value* popped = impl_->builder->CreateCall(
                impl_->runtimeFuncs[intKeyed ? "dragon_dict_int_pop_default"
                                             : "dragon_dict_pop_default"],
                {obj, key, defVal}, "dictpopdef");
            impl_->lastValue = intKeyed ? coerceDictValue(popped) : popped;
            return true;
        }

        if (method == "clear" && node.args.empty()) {
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_dict_clear"], {obj});
            impl_->lastValue = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*impl_->context));
            return true;
        }

        if (method == "update" && node.args.size() == 1) {
            node.args[0]->accept(*this);
            llvm::Value* other = impl_->trackBorrowTempGuarded(node.args[0].get(), impl_->lastValue, argTemps, argTempBases);
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_dict_update"], {obj, other});
            impl_->lastValue = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*impl_->context));
            return true;
        }

        if (method == "setdefault" && node.args.size() == 2) {
            node.args[0]->accept(*this);
            llvm::Value* key = normDictKey(impl_->lastValue);
            node.args[1]->accept(*this);
            llvm::Value* defVal = impl_->lastValue;
            // Heap-valued setdefault: own the result via the incref-on-return
            // variant (absent branch increfs dict copy + binding); borrow = UAF.
            if (isHeapValueKind(dictValueKind())) {
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
                impl_->runtimeFuncs[intKeyed ? "dragon_dict_int_setdefault"
                                             : "dragon_dict_setdefault"],
                {obj, key, defVal}, "dictsetdef");
            return true;
        }

        if (method == "copy" && node.args.empty()) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_copy"], {obj}, "dictcopy");
            return true;
        }
        return false;
        }();
        impl_->popArgTempCleanups(argTempBases);
        if (dictHandled) {
            impl_->drainBorrowTemps(argTemps);
            impl_->emitMoveOutSlots(node);
            if (ownedDictRecv && kDictRecvDrainOk.count(method))
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref"], {obj});
            return true;
        }
    }

    if (isSet) {
        attr.object->accept(*this);
        llvm::Value* obj = impl_->lastValue;
        bool ownedSetRecv = impl_->options.gcMode == GCMode::RC &&
                            !Impl::isBorrowedHeapExpr(attr.object.get()) &&
                            impl_->isOwnedPtrResult(obj);
        static const std::set<std::string> kSetRecvDrainOk = {
            "add", "remove", "discard", "clear", "union", "intersection",
            "difference", "symmetric_difference", "issubset", "issuperset",
            "isdisjoint"};
        std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
        std::vector<llvm::Value*> argTempBases;
        if (ownedSetRecv && kSetRecvDrainOk.count(method))
            argTempBases.push_back(
                impl_->emitCleanupPushTemp(obj, Impl::DCLEAN_OBJ));
        bool setHandled = [&]() -> bool {

        auto argToI64 = [&](size_t i) -> llvm::Value* {
            node.args[i]->accept(*this);
            llvm::Value* v = impl_->lastValue;
            impl_->trackBorrowTempGuarded(node.args[i].get(), v, argTemps, argTempBases);
            if (v->getType() == impl_->i1Type)
                v = impl_->builder->CreateZExt(v, impl_->i64Type);
            else if (v->getType() == impl_->f64Type)
                v = impl_->builder->CreateBitCast(v, impl_->i64Type);
            else if (v->getType()->isPointerTy())
                v = impl_->builder->CreatePtrToInt(v, impl_->i64Type);
            return v;
        };

        if (method == "add" && node.args.size() == 1) {
            llvm::Value* v;
            llvm::Value* ownedStrArg = nullptr;
            node.args[0]->accept(*this);
            v = impl_->lastValue;
            if (v->getType()->isPointerTy()) {
                v = impl_->ensureHeapString(v, node.args[0].get());
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
            {
                int64_t addTag = 0;
                if (node.args[0]->type)
                    addTag = impl_->typeKindToElemTag(node.args[0]->type->kind());
                if (addTag == 0 && dynamic_cast<StringLiteral*>(node.args[0].get()))
                    addTag = TAG_STR;
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
        auto setArg = [&]() -> llvm::Value* {
            node.args[0]->accept(*this);
            llvm::Value* v = impl_->lastValue;
            impl_->trackBorrowTempGuarded(node.args[0].get(), v, argTemps, argTempBases);
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
        return false;
        }();
        impl_->popArgTempCleanups(argTempBases);
        if (setHandled) {
            impl_->drainBorrowTemps(argTemps);
            impl_->emitMoveOutSlots(node);
            if (ownedSetRecv && kSetRecvDrainOk.count(method))
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref"], {obj});
            return true;
        }
    }

    {
        bool bareSuper = false;
        bool calledSuper = false;
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

    if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
        if (impl_->classNames.count(objName->name)) {
            std::string methodFuncName =
                impl_->classSymPrefix(objName->name) + "_" + method;
            if (impl_->staticMethods.count(methodFuncName)) {
                auto* methodFunc = impl_->module->getFunction(methodFuncName);
                if (methodFunc) {
                    std::vector<llvm::Value*> args;
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
                    auto argTempBases = impl_->pushArgTempCleanups(argTemps);
                    if (methodFunc->getReturnType()->isVoidTy()) {
                        impl_->builder->CreateCall(methodFunc, args);
                        impl_->lastValue = llvm::ConstantPointerNull::get(
                            llvm::PointerType::getUnqual(*impl_->context));
                    } else {
                        impl_->lastValue = impl_->normalizeIntC(
                            impl_->builder->CreateCall(methodFunc, args, "smcall"));
                    }
                    impl_->popArgTempCleanups(argTempBases);
                    impl_->drainBorrowTemps(argTemps);
                    impl_->emitMoveOutSlots(node);
                    return true;
                }
            }
        }
    }

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
                    auto argTempBases = impl_->pushArgTempCleanups(argTemps);
                    if (methodFunc->getReturnType()->isVoidTy()) {
                        impl_->builder->CreateCall(methodFunc, args);
                        impl_->lastValue = llvm::ConstantPointerNull::get(
                            llvm::PointerType::getUnqual(*impl_->context));
                    } else {
                        impl_->lastValue = impl_->normalizeIntC(
                            impl_->builder->CreateCall(methodFunc, args, "smcall"));
                    }
                    impl_->popArgTempCleanups(argTempBases);
                    impl_->drainBorrowTemps(argTemps);
                    impl_->emitMoveOutSlots(node);
                    return true;
                }
            }
        }
    }

    if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
        std::string className;
        std::string owningModule;
        if (objName->name == "self" && !impl_->currentClassName.empty()) {
            className = impl_->currentClassName;
            owningModule = impl_->currentModuleName;
        } else if (const auto* gb = impl_->globalClassBindingFor(objName->name)) {
            className = gb->className;
            owningModule = gb->owningModule;
        } else {
            auto vit = impl_->varClassNames.find(objName->name);
            if (vit != impl_->varClassNames.end()) className = vit->second;
            auto vmIt = impl_->varClassOwningModule.find(objName->name);
            if (vmIt != impl_->varClassOwningModule.end()) {
                owningModule = vmIt->second;
            } else if (!className.empty()) {
                owningModule = impl_->resolveClassOwningModule(className);
            }
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
            std::string methodFuncName;
            auto* methodFunc = impl_->resolveMethodFunction(
                owningModule, className, method, &methodFuncName);
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
                    attr.object->accept(*this);
                    llvm::Value* obj = impl_->lastValue;
                    if (!obj->getType()->isPointerTy())
                        obj = impl_->builder->CreateIntToPtr(obj, impl_->i8PtrType);
                    args.push_back(obj);
                }

                std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
                auto mpkIt = impl_->funcParamKinds.find(methodFuncName);
                unsigned paramOffset = isStaticCall ? 0 : 1;
                if (impl_->funcVarArgInfo.count(methodFuncName)) {
                    if (!impl_->packVarArgMethodArgs(
                            *this, node, methodFuncName, methodFuncType, args,
                            argTemps, "method '" + method + "'"))
                        return true;
                } else if (callHasSpread(node)) {
                    if (!impl_->expandSpreadCallArgs(
                            *this, methodFunc, node, args, argTemps,
                            "method '" + method + "'"))
                        return true;
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
                }
                impl_->fillDefaultArgs(methodFuncName, methodFunc, args, *this,
                                       &argTemps);

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
                impl_->drainBorrowTemps(argTemps);
                impl_->emitMoveOutSlots(node);
                return true;
            }

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

                        auto cfIt = impl_->classFieldCallableTypeBySym.find(impl_->classSym(className));
                        llvm::FunctionType* userFnType = nullptr;
                        if (cfIt != impl_->classFieldCallableTypeBySym.end()) {
                            auto fIt = cfIt->second.find(method);
                            if (fIt != cfIt->second.end()) userFnType = fIt->second;
                        }

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

                        std::vector<llvm::Type*> closureArgTypes(bareArgTypes);
                        closureArgTypes.push_back(impl_->i8PtrType);
                        auto* closureFnType = llvm::FunctionType::get(
                            retTy, closureArgTypes, false);

                        auto* i8Ty = llvm::Type::getInt8Ty(*impl_->context);
                        auto* tagAddr = impl_->builder->CreateGEP(
                            i8Ty, fnPtr,
                            llvm::ConstantInt::get(impl_->i64Type, 8),
                            method + "_tag_addr");
                        auto* tagByte = impl_->builder->CreateLoad(
                            i8Ty, tagAddr, method + "_tag");
                        auto* isClosure = impl_->builder->CreateICmpEQ(
                            tagByte,
                            llvm::ConstantInt::get(i8Ty, 10),
                            method + "_is_closure");

                        auto* fn = impl_->builder->GetInsertBlock()->getParent();
                        auto* closureBB = llvm::BasicBlock::Create(
                            *impl_->context, method + ".closure", fn);
                        auto* bareBB = llvm::BasicBlock::Create(
                            *impl_->context, method + ".bare", fn);
                        auto* mergeBB = llvm::BasicBlock::Create(
                            *impl_->context, method + ".cont", fn);
                        impl_->builder->CreateCondBr(isClosure, closureBB, bareBB);

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
            auto vk = impl_->lookupVarKind(objName->name);
            if (vk == Impl::VarKind::ClassInstance) {
                const size_t wantParams = node.args.size() + 1;
                int methodIndex = -1;
                llvm::FunctionType* methodFuncType = nullptr;
                for (auto& [cls, methodMap] : impl_->classMethodVtableIndicesBySym) {
                    auto it = methodMap.find(method);
                    if (it == methodMap.end()) continue;
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
                    args.push_back(objPtr);
                    for (size_t i = 0; i < node.args.size(); ++i) {
                        node.args[i]->accept(*this);
                        llvm::Value* arg = impl_->lastValue;
                        unsigned paramIdx = (unsigned)(i + 1);
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

    if (!dynamic_cast<NameExpr*>(attr.object.get())) {
        std::string className = impl_->resolveExprClassName(attr.object.get());
        if (!className.empty() && impl_->classNames.count(className)) {
            std::string owningModule =
                impl_->resolveClassOwningModule(className);
            std::string methodFuncName;
            auto* methodFunc = impl_->resolveMethodFunction(
                owningModule, className, method, &methodFuncName);
            if (methodFunc) {
                bool isStaticCall = impl_->staticMethods.count(methodFuncName) > 0;
                auto methodFuncType = methodFunc->getFunctionType();
                std::vector<llvm::Value*> args;
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
                if (impl_->funcVarArgInfo.count(methodFuncName)) {
                    if (!impl_->packVarArgMethodArgs(
                            *this, node, methodFuncName, methodFuncType, args,
                            argTemps, "method '" + method + "'"))
                        return true;
                } else if (callHasSpread(node)) {
                    if (!impl_->expandSpreadCallArgs(
                            *this, methodFunc, node, args, argTemps,
                            "method '" + method + "'"))
                        return true;
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
                }
                impl_->fillDefaultArgs(methodFuncName, methodFunc, args, *this,
                                       &argTemps);

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
                impl_->drainBorrowTemps(argTemps);
                impl_->emitMoveOutSlots(node);
                return true;
            }
        }
    }

    return false;
}

}
