/// Dragon CodeGen - Method Call Dispatch
/// Handles: obj.method(args) for Thread, Lock, SyncList, SyncDict,
///  string/bytes/list/dict/file methods, super().method(),
///  static methods, instance methods, vtable dispatch, stdlib modules.
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

    // Spread INTO a variadic method (`recv.m(*xs)` / `recv.m(**d)`) is a
    // separate capability - free functions have it via emitVarArgCall's spread
    // arm, but packing it here would need the exception-safe pack-cleanup dance
    // before its dup-key guards raise. Until that lands, diagnose rather than
    // miscompile, so no leak or bad IR escapes.
    if (callHasSpread(node)) {
        addError("call-site spread (`*`/`**`) into a variadic method is not yet "
                 "supported", node.location());
        poison();
        return false;
    }

    // self (if any) already occupies args[0..selfOffset). For a static variadic
    // method selfOffset is 0 and this reduces to the free-function shape.
    const size_t selfOffset = args.size();
    const size_t numParams = methodFuncType->getNumParams();

    // 1. Regular positional args (those before `*args`). Drain owned heap temps
    // via the shared classifier so a `def m(a: str, *xs)` regular arg can't leak
    // (the callee borrows; the packed list/dict below drain through argTemps).
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
    // 1b. Pad regular slots the caller omitted (a default param BEFORE `*args`)
    // with null placeholders; fillDefaultArgs fills them at the tail. Without
    // this the `*args` list would land in the first omitted slot and its own
    // slot would be null - an LLVM "Operand is null" verify crash.
    while (args.size() < selfOffset + vaInfo.numRegularParams) {
        args.push_back(nullptr);
        llvmIdx++;
    }

    // 2. Pack surplus positionals into the `*args` list, monomorphized by the
    // declared element tag (native ints/floats/ptrs; boxed only for Any).
    // emitTypedListAppend owns each element's borrow/incref discipline, so the
    // list decref at the tail releases the element refs - no per-element drain.
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

    // 2b. Bind keyword args that NAME a regular param into that positional slot
    // (so `def m(x: int, **kw)` accepts `m(x=5, y=6)`). funcParamNames includes
    // "self" at index 0 for an instance method, hence the selfOffset shift when
    // deciding whether the matched name is a regular param.
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

    // 3. Pack the remaining keyword args into the `**kwargs` dict, tagging each
    // value by its LLVM type and increfing a BORROWED heap string the dict-set
    // adopts (mirrors emitVarArgCall step 3 / the dict-literal value path). The
    // dict is a call-site-owned temp released via argTemps at the tail.
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
        // `*args`-only method: a keyword that bound to no regular param is
        // unexpected. Guard so codegen never silently drops it.
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

bool CodeGen::emitMethodCall(CallExpr& node, AttributeExpr& attr) {
    // ADR 010: when the TypeChecker resolved this call to a specific method
    // overload, dispatch to that overload's per-index symbol (`name__ovN`).
    // resolvedMethodOverload is -1 for every non-overloaded call, so the bare
    // name (and all existing behavior) is unchanged on the common path; the
    // resolution itself was compile-time, so this only redirects a direct call.
    std::string method = attr.attribute;
    if (node.resolvedMethodOverload >= 0)
        method += "__ov" + std::to_string(node.resolvedMethodOverload);

    // C9-B: method-call spread (`obj.m(*xs)`) is wired ONLY at the class-
    // instance dispatch below (the path that builds args via
    // expandSpreadCallArgs). Any other method kind (builtins on str/list/dict,
    // module functions, Thread/Task handles) does not support it. The early
    // builtin/module paths are type-gated on a str/list/dict/... receiver, so a
    // user-class-instance receiver never reaches them - but to be certain a
    // spread call can't be silently grabbed (and mishandled) by one of them,
    // bail up-front unless the receiver is a NameExpr-bound user-class instance
    // (or `self`). That leaves the instance dispatch as the sole consumer of a
    // method spread; everything else returns false so visit(CallExpr) emits the
    // "spread not supported" diagnostic. Non-spread calls are unaffected.
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
            // Non-Name receiver (`self.field.m(*xs)`, `make().m(*xs)`,
            // `arr[0].m(*xs)`) - handled by the second instance-dispatch block
            // when its class resolves statically.
            std::string cn = impl_->resolveExprClassName(attr.object.get());
            instanceRecv = !cn.empty() && impl_->classNames.count(cn) > 0;
        }
        if (!instanceRecv) return false;
    }

    // Thread handle .join() method - joins vthread and returns result
    // Thread handle .is_alive() method - returns 1 if running, 0 if done
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
                    // Reinterpret the i64 result slot at the task's native T
                    // (D030): join() on Task[T] has CallExpr type T.
                    impl_->lastValue = impl_->taskResultFromI64(raw, node.type.get());
                    // `t.join()` CONSUMES t (same as `await t`): the runtime freed
                    // the vthread, so blank a LOCAL binding's slot (free(p);p=NULL)
                    // so the scope-exit detach won't double-free and a later
                    // is_alive()/join reads NULL, not freed memory.
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

    // Lock method dispatch (Python threading.Lock shape): acquire(blocking=...),
    // release(), destroy(). isLockExpr covers a tagged local/global receiver
    // AND a Lock-typed instance field (`self._lock.acquire()`) - the old
    // NameExpr-only check let field receivers fall through to the generic
    // instance path, which silently DROPPED the calls (the "lock" never
    // locked; found by the concurrent-mutation detector).
    if (impl_->isLockExpr(attr.object.get())) {
        llvm::Value* handle = nullptr;
        if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
            llvm::Value* handlePtr = impl_->lookupVar(objName->name);
            if (!handlePtr) handlePtr = impl_->lookupModuleGlobal(objName->name);
            if (handlePtr)
                handle = impl_->builder->CreateLoad(impl_->i8PtrType, handlePtr, "lock.handle");
        } else {
            // Field receiver: evaluate the attribute expression - the field
            // slot holds the dragon_lock_new() pointer.
            attr.object->accept(*this);
            handle = impl_->lastValue;
            if (handle && !handle->getType()->isPointerTy())
                handle = impl_->builder->CreateIntToPtr(handle, impl_->i8PtrType, "lock.handle");
        }
        {
            if (handle) {
                if (method == "acquire") {
                    // Python shape: acquire(blocking=True, timeout=-1) -> bool.
                    // `blocking`/`timeout` come from positional args[0]/[1] or
                    // kwargs. acquire() returns True once held; with
                    // blocking=False or a timeout it returns whether it got it.
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
                    // Own the borrowed key (#20a): dragon_syncdict_setdefault
                    // delegates to dragon_dict_setdefault, which adopts on insert
                    // and releases on present (under the syncdict wrlock).
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

    // Check if the object is a list variable
    bool isList = false;
    if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
        isList = impl_->lookupVarKind(objName->name) == Impl::VarKind::List;
    }

    // Check if the object is a dict variable
    bool isDict = false;
    if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
        isDict = impl_->lookupVarKind(objName->name) == Impl::VarKind::Dict;
    }

    // Check if the object is a set variable
    bool isSet = false;
    if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
        isSet = impl_->lookupVarKind(objName->name) == Impl::VarKind::Set;
    }
    if (!isSet) {
        if (dynamic_cast<SetExpr*>(attr.object.get())) isSet = true;
    }

    // Check if the object is a class field (self.field or instance.field)
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
                auto fkIt = impl_->classFieldKinds.find(className);
                if (fkIt != impl_->classFieldKinds.end()) {
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

    // Check if the object is a string variable or literal
    bool isStr = false;
    if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
        auto sk = impl_->lookupVarKind(objName->name);
        isStr = (sk == Impl::VarKind::Str || sk == Impl::VarKind::StrLiteral);
    }
    if (!isStr) {
        if (auto* sl = dynamic_cast<StringLiteral*>(attr.object.get()))
            isStr = !sl->isBytes;
    }

    // Check if the object is a bytes variable or literal. D030 §5:
    // bytes-ness is identified solely by the static type / AST shape.
    // VarKind::Bytes was deleted; bytes-typed slots carry VarKind::List
    // (generic-heap) at the VarKind layer.
    bool isBytes = attr.object && attr.object->type &&
                   attr.object->type->kind() == Type::Kind::Bytes;
    if (!isBytes) {
        if (auto* sl = dynamic_cast<StringLiteral*>(attr.object.get()))
            isBytes = sl->isBytes;
    }

    // Final fallback: typechecker-propagated type. Handles chained subscripts
    // (paths[0].startswith(...), all_parts[i].append(...)), dict values as
    // receivers, and any other expression whose VarKind heuristic misses but
    // whose static type is unambiguous.
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

    // --- Deque method dispatch ---
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
                    // Element tag: prefer the receiver's static deque[T]
                    // element type; fall back to the value's LLVM type. The
                    // runtime stores it (drives RC, `in` equality, repr).
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
                    // The deque took its own ref; release an OWNED heap arg
                    // temp (f-string, concat, method result) or it leaks one
                    // ref per append. Borrowed args (named vars) untouched.
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
                    // A heap-ptr element type (str / list / dict / set / tuple /
                    // bytes / instance) pops via the _ptr variant so the OWNED
                    // transfer is a recognized ptr result - drained when passed
                    // to a borrow callee, adopted when bound. Scalars (int /
                    // float / bool) and Callable elements keep the i64 form.
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

    // --- String method dispatch ---
    if (isStr) {
        attr.object->accept(*this);
        llvm::Value* obj = impl_->lastValue;
        // An owned heap-str receiver - a slice (s[a:b]), concat, str(), or
        // call result - is consumed by the method and must be released, or it
        // leaks once per call (e.g. the JSON decoder's `buf[a:b].decode()`).
        // Every str method below returns a FRESH, non-aliasing result, so
        // dropping the receiver after the call is always balanced. Borrowed
        // receivers (named locals, fields) are skipped by isOwnedStrResult.
        bool ownedStrRecv =
            impl_->options.gcMode == GCMode::RC && impl_->isOwnedStrResult(obj);
        // Owned heap temporaries materialized for a borrow-arg slot (s.replace(a+b, c),
        // s.split(x+y), s.join(make_list()), ...). Every str method borrows its args
        // and returns a FRESH result, so these are released at the common tail below -
        // one decref per owned temp, never for a borrowed Name/field. (#3 class A.)
        std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
        bool strHandled = [&]() -> bool {

        // strip/lstrip/rstrip(chars) - Python char-set trim (NOT prefix/suffix).
        // Must precede the no-arg block below, which would otherwise swallow
        // these and silently drop the argument.
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

        // No-arg boolean predicates. Runtime returns int64_t (0/1); D030
        // says values flow at native types, so we convert to i1 here so that
        // downstream consumers (print, if, &&) see a real bool, not an int.
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

        // Python-parity str.find / rfind / count with optional start[, end].
        //  find(sub) -> dragon_str_<m>(s, sub)
        //  find(sub, start) -> dragon_str_<m>_se(s, sub, start, -1)
        //  find(sub, start, end) -> dragon_str_<m>_se(s, sub, start, end)
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

        // .index() and .rindex() - map to dragon_str_index_of / dragon_str_rindex
        if ((method == "index" || method == "rindex") && node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* arg = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            std::string rtName = (method == "index") ? "dragon_str_index_of" : "dragon_str_rindex";
            auto* fn = impl_->getOrDeclareRuntime(rtName,
                llvm::FunctionType::get(impl_->i64Type, {impl_->i8PtrType, impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, arg}, method);
            return true;
        }

        // replace(old, new[, count]) - 2 string args + optional int count, returning string
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

        // zfill(width) - int arg returning string
        if (method == "zfill" && node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* width = impl_->lastValue;
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_zfill",
                llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType, impl_->i64Type}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, width}, "zfill");
            return true;
        }

        // expandtabs(tabsize) - int arg returning string
        if (method == "expandtabs") {
            llvm::Value* tabsize = llvm::ConstantInt::get(impl_->i64Type, 8);
            if (node.args.size() >= 1) { node.args[0]->accept(*this); tabsize = impl_->lastValue; }
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_expandtabs",
                llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType, impl_->i64Type}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, tabsize}, "expandtabs");
            return true;
        }

        // center/ljust/rjust(width[, fillchar]) - int + optional char arg
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

        // split(sep[, maxsplit]) / rsplit(sep[, maxsplit]) - return list (ptr).
        // maxsplit defaults to -1 (unlimited); rsplit splits from the right.
        // Previously split dropped maxsplit and rsplit had no dispatch at all.
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

        // join(list) - returns string
        if (method == "join" && node.args.size() >= 1) {
            node.args[0]->accept(*this);
            llvm::Value* list = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            auto* fn = impl_->getOrDeclareRuntime("dragon_str_join",
                llvm::FunctionType::get(impl_->i8PtrType, {impl_->i8PtrType, impl_->i8PtrType}, false));
            impl_->lastValue = impl_->builder->CreateCall(fn, {obj, list}, "join");
            return true;
        }

        // splitlines() - returns list
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

        // encode(encoding="utf-8", errors="strict") - returns bytes.
        // The encoding/errors args are honored by dragon_str_encode_ex
        // (UTF-8/ASCII only, strict/replace), never silently discarded.
        // obj is captured above, so accepting the arg
        // exprs after it is safe.
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

    // --- Bytes method dispatch ---
    if (isBytes) {
        attr.object->accept(*this);
        llvm::Value* obj = impl_->lastValue;
        // Owned heap-bytes receiver (slice b[a:b], etc.) - same consume-and-
        // release contract as the str block above. All bytes methods return
        // fresh non-aliasing results. dragon_decref drops the DragonBytes.
        bool ownedBytesRecv =
            impl_->options.gcMode == GCMode::RC && impl_->isOwnedPtrResult(obj);
        // Owned heap temporaries for borrow-arg slots (b.replace(x+y, z), ...);
        // every bytes method borrows its args - released at the common tail. (#3.)
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

        // decode(encoding="utf-8", errors="strict") - returns str (i8*).
        // The encoding/errors args are honored by dragon_bytes_decode_ex,
        // never silently discarded. Default errors="strict" matches Python:
        // invalid input raises UnicodeDecodeError, not a silent
        // Latin-1 reinterpretation. obj is captured above.
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

        // hex() - returns str
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

        // 1-arg(bytes) methods returning int64 (find/rfind/count return positions)
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
            // fromhex borrows the hex string and returns fresh bytes; release an
            // owned-temp arg (bytes.fromhex(a + b)) after the call. (#3 class A.)
            Impl::VarKind dk = impl_->ownedTempDrainKind(node.args[0].get(), hexStr);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_fromhex"], {hexStr}, "fromhex");
            impl_->emitDecrefByKind(hexStr, dk);
            return true;
        }
        // 6.17: dict.fromkeys(iterable[, value]) - classmethod-style.
        // Default value is None (i64=0, tag=TAG_NONE=4).
        if (objName->name == "dict" && method == "fromkeys" &&
            (node.args.size() == 1 || node.args.size() == 2)) {
            node.args[0]->accept(*this);
            llvm::Value* keysList = impl_->lastValue;
            llvm::Value* val;
            llvm::Value* tag;
            if (node.args.size() == 2) {
                node.args[1]->accept(*this);
                llvm::Value* raw = impl_->lastValue;
                // D030 §5: prefer the typechecker's static type for tag
                // derivation - bytes-typed values must round-trip with
                // TAG_BYTES even when their slot's VarKind has collapsed
                // into the generic-heap cohort.
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

    // --- List method dispatch ---
    if (isList) {
        attr.object->accept(*this);
        llvm::Value* obj = impl_->lastValue;
        // An owned list RECEIVER temp (make().count(1), slice-then-method) is
        // consumed by the call and leaks once per evaluation without a drain -
        // the str path does the same (see
        // ownedStrRecv above). Drained at the tail, gated on an allow-list of
        // methods that cannot hand out a borrow into the receiver
        // (void / scalar / transfer / fresh results only).
        bool ownedListRecv = impl_->options.gcMode == GCMode::RC &&
                             !Impl::isBorrowedHeapExpr(attr.object.get()) &&
                             impl_->isOwnedPtrResult(obj);
        static const std::set<std::string> kListRecvDrainOk = {
            "append", "insert", "extend", "remove", "clear", "sort",
            "reverse", "count", "index", "pop", "copy"};
        // Owned heap temporaries materialized for a BORROW-method arg slot
        // (xs.remove(a+b), xs.index(x+y), xs.extend(make())). The transfer
        // methods append/insert deliberately do NOT track - they adopt the +1
        // (Model B). Tracked temps are released once at the common tail. (#3.)
        std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
        bool listHandled = [&]() -> bool {

        if (method == "append" && node.args.size() == 1) {
            // Track element class name when appending a class instance.
            // Detect both constructor calls: list.append(Route(...))
            // and variables with known class type: list.append(item)
            std::string appendedClassName;
            if (auto* argCall = dynamic_cast<CallExpr*>(node.args[0].get())) {
                if (auto* argFn = dynamic_cast<NameExpr*>(argCall->callee.get())) {
                    if (impl_->classNames.count(argFn->name))
                        appendedClassName = argFn->name;
                }
            } else if (auto* argName = dynamic_cast<NameExpr*>(node.args[0].get())) {
                // varClassNames is keyed by bare name and is NOT cleared between
                // functions, so a name used as a class instance in one function
                // (e.g. an `entry` in a stdlib module) leaves a stale entry. If a
                // later function appends a STRING of the same name, that stale
                // entry would mark `out` as list[Instance] and emit a generic
                // dragon_incref on a string pointer - whose refcount header lives
                // at a different offset than dragon_decref_str uses - so the incref
                // misses, the iterable temp's destroy frees the string, and the
                // list is left holding a dangling pointer (use-after-free). A
                // string is never a class instance, so skipping the lookup for a
                // str-bound arg is always correct and leaves genuine instance/Task
                // element tracking untouched.
                Impl::VarKind ak = impl_->lookupVarKind(argName->name);
                if (ak != Impl::VarKind::Str && ak != Impl::VarKind::StrLiteral) {
                    auto vit = impl_->varClassNames.find(argName->name);
                    if (vit != impl_->varClassNames.end())
                        appendedClassName = vit->second;
                }
            }
            if (!appendedClassName.empty()) {
                // Track the element class for later attribute access - but
                // NEVER clobber a declared list[Any] receiver: overwriting its
                // elem kind with Instance re-routed this append (and every
                // later element op) from the box-list runtime to
                // dragon_list_append_ptr on a DragonListBox - an ASan
                // heap-buffer-overflow (`l: list[Any] = []; l.append(Thing())`).
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
                            auto ckIt = impl_->classFieldListElemKinds.find(ownerClass);
                            bool fieldIsAny = false;
                            if (ckIt != impl_->classFieldListElemKinds.end()) {
                                auto fIt = ckIt->second.find(listAttr->attribute);
                                fieldIsAny = fIt != ckIt->second.end() &&
                                             fIt->second == Type::Kind::Any;
                            }
                            if (!fieldIsAny) {
                                impl_->classFieldListElemKinds[ownerClass][listAttr->attribute] = Type::Kind::Instance;
                                impl_->classFieldListElemClassName[ownerClass][listAttr->attribute] = appendedClassName;
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
            // D030 Phase 3.C: dispatch append by element kind to the matching
            // typed runtime op so the value never funnels through i64.
            // D039 Phase 4: list[Any] uses dragon_list_box_append.
            Type::Kind appendElemKind = impl_->getIterableElementKind(attr.object.get());
            if (appendElemKind == Type::Kind::Any) {
                // dragon_list_box_append takes ownership of one reference on a
                // refcounted payload (Model B) - boxArgTagPayload increfs a
                // borrowed source so the list's reference outlives it.
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
                // ADR 046: a bare fn used as a value (`llvm::Function`) stored into
                // a list[Callable] must be wrapped into a DragonClosure(fn, null)
                // so every element is a uniform refcounted closure - the dispatch
                // unwraps env==null -> bare call, and the list owns one reference.
                // The fresh wrapper carries refcount 1, so it skips the
                // borrowed-incref below (append_ptr takes that reference).
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
                // Model B (CPython convention, documented at runtime_list.cpp:455):
                // dragon_list_append_ptr takes ownership of one reference.
                // Borrowed sources (a heap-typed local, class field, or
                // container subscript) need an incref before crossing in -
                // otherwise scope cleanup at the source's owning scope would
                // decref the value back to 0 while the list still holds it.
                if (impl_->options.gcMode == GCMode::RC && !freshWrappedClosure &&
                    Impl::isBorrowedHeapExpr(node.args[0].get())) {
                    if (appendElemKind == Type::Kind::Str)
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_incref_str"], {val});
                    else if (appendElemKind == Type::Kind::Function)
                        // Tag-gated: a Callable slot may hold a real DragonClosure
                        // or a bare fn ptr - incref only the former (no-op on a
                        // headerless code pointer), never dragon_incref on .text.
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
            // D030 Phase 3.C note: dragon_list_insert is still polymorphic
            // (i64-funneled) - typed insert ops can be added in a follow-up if
            // insert becomes a hot path. For now we coerce to i64 as before.
            // dragon_list_insert BORROWS its value (increfs internally, unlike
            // append's adopt), so an owned temp arg (`xs.insert(0, "a" + s)`)
            // keeps its construction +1 forever without the same drain its
            // sibling handlers (remove/extend/index/count) route through.
            // Leak-freedom is pinned under the ASan build.
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
            // list[float] -> DragonListF64: pop must return native f64. The
            // generic i64 pop returns the raw 8 bytes, and the caller's
            // numeric coercion would SIToFP them - converting the f64 BIT
            // PATTERN as an integer (garbage). The typed pop returns double so
            // the value stays unboxed end-to-end (commandment #1).
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
            // list.sort() / list.sort(reverse=...). reverse= selects descending;
            // without it the cheaper in-place ascending sort.
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

    // --- Dict method dispatch ---
    if (isDict) {
        attr.object->accept(*this);
        llvm::Value* obj = impl_->lastValue;

        // Int-keyed dicts store keys as native i64 in the pointer slot. The
        // generic dragon_dict_get/_has_key/_get_default take `const char*` keys
        // (str-keyed dicts); calling them with an i64 key is an LLVM signature
        // mismatch. Route to the dragon_dict_int_* family, which takes i64 keys.
        bool intKeyed = impl_->dictKeyIsInt(attr.object.get());

        // dragon_dict_get* return the value as a raw i64. Re-cast it to the
        // dict's native value type (D030) so consumers that key off the LLVM
        // type - f-strings, print - treat a str value as a string, not a
        // pointer-as-int. (A typed binding already coerces via its target; a
        // bare interpolation has no target, which is why f"{d.get(k)}" printed
        // a raw pointer int.)
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
        // A heap-OBJECT value type (list/dict/set/tuple/bytes/instance) - reads
        // must own the result (incref) or the binding frees the dict's value
        // (#19). str has its own *_str getters; int/float/bool/Any are not here.
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

        // Owned dict RECEIVER temp drain (mirror of ownedListRecv above).
        // Conservative allow-list: get/setdefault/keys/values/items are
        // excluded until their result-ownership contracts are pinned (a
        // borrowed str value or a mixed-tag values() borrow-list must not
        // outlive a drained receiver).
        bool ownedDictRecv = impl_->options.gcMode == GCMode::RC &&
                             !Impl::isBorrowedHeapExpr(attr.object.get()) &&
                             impl_->isOwnedPtrResult(obj);
        static const std::set<std::string> kDictRecvDrainOk = {
            "pop", "popitem", "clear", "update"};
        // Owned heap temporaries for a borrow-method KEY / other-dict arg
        // (d.get(a+b), d.pop(x+y), d.update(make())). Lookups borrow the key, so
        // an owned temp is released at the tail below. setdefault and the
        // default-VALUE args are intentionally NOT tracked here - their key is
        // transfer-adopted and the default is return-aliased; both are handled
        // via the runtime ownership contract, not a caller drain. (#3 class A.)
        std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
        bool dictHandled = [&]() -> bool {

        // .get(key) -> dragon_dict_get(dict, key)
        if (method == "get" && node.args.size() == 1) {
            node.args[0]->accept(*this);
            llvm::Value* key = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            // Heap-VALUED dict: own the returned value (the getter increfs it) so
            // the binding's scope-decref balances - a bare borrow would free the
            // dict's stored value -> UAF (#19). str-keyed key is a ptr, int-keyed
            // an i64; the matching getter's signature takes each.
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

        // .get(key, default) -> dragon_dict_get_default(dict, key, default)
        if (method == "get" && node.args.size() == 2) {
            node.args[0]->accept(*this);
            llvm::Value* key = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            node.args[1]->accept(*this);
            llvm::Value* defVal = impl_->lastValue;
            // Str-keyed, str-VALUED dict: route to the owned-str getter so a str
            // local bound to the result is refcount-balanced. The generic getter
            // returns a BORROWED value which, decref'd at scope exit, double-frees
            // the dict's stored string. defVal is already an i8* here.
            Type::Kind getVk = Type::Kind::Unknown;
            if (attr.object->type && attr.object->type->kind() == Type::Kind::Dict)
                getVk = static_cast<DictType&>(*attr.object->type).valueType->kind();
            if (!intKeyed && getVk == Type::Kind::Str) {
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_get_str_default"],
                    {obj, key, defVal}, "dictgetstrdef");
                return true;
            }
            // Heap-VALUED (list/dict/set/tuple/bytes/instance): own the result via
            // the incref-on-return getter so the binding's decref balances (#19).
            // The default temp is released right after - balanced whether the key
            // was present (default unused) or absent (default returned: the
            // getter's incref + this drain net to the binding's single +1).
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

        // .keys() -> dragon_dict_keys(dict)
        if (method == "keys" && node.args.empty()) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_keys"], {obj}, "dictkeys");
            return true;
        }

        // .has_key(key) -> dragon_dict_has_key(dict, key)
        if (method == "has_key" && node.args.size() == 1) {
            node.args[0]->accept(*this);
            llvm::Value* key = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs[intKeyed ? "dragon_dict_int_has_key"
                                             : "dragon_dict_has_key"],
                {obj, key}, "haskey");
            return true;
        }

        // .values() -> dragon_dict_values(dict)
        // D039 Phase 9: when the dict's value type is Any, route to
        // dragon_dict_values_box so the resulting list iterates with box
        // semantics (preserves per-entry tags for isinstance / print / etc.).
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

        // .items() -> dragon_dict_items(dict)
        if (method == "items" && node.args.empty()) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_items"], {obj}, "dictitems");
            return true;
        }

        // 6.17: .popitem() -> dragon_dict_popitem(dict) - returns DragonTuple*
        // (LIFO order; raises KeyError on empty dict).
        if (method == "popitem" && node.args.empty()) {
            llvm::Value* tupleI64 = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_popitem"], {obj}, "dictpopitem");
            impl_->lastValue = impl_->builder->CreateIntToPtr(
                tupleI64, impl_->i8PtrType, "popitem_ptr");
            return true;
        }

        // .pop(key) -> dragon_dict_pop(dict, key)
        if (method == "pop" && node.args.size() == 1) {
            node.args[0]->accept(*this);
            llvm::Value* key = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_pop"], {obj, key}, "dictpop");
            return true;
        }

        // .pop(key, default) -> dragon_dict_pop_default(dict, key, default)
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

        // .clear() -> dragon_dict_clear(dict)
        if (method == "clear" && node.args.empty()) {
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_dict_clear"], {obj});
            impl_->lastValue = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*impl_->context));
            return true;
        }

        // .update(other_dict) -> dragon_dict_update(dict, other_dict)
        if (method == "update" && node.args.size() == 1) {
            node.args[0]->accept(*this);
            llvm::Value* other = impl_->trackBorrowTemp(node.args[0].get(), impl_->lastValue, argTemps);
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_dict_update"], {obj, other});
            impl_->lastValue = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*impl_->context));
            return true;
        }

        // .setdefault(key, default) -> dragon_dict_setdefault(dict, key, default)
        if (method == "setdefault" && node.args.size() == 2) {
            node.args[0]->accept(*this);
            llvm::Value* key = impl_->lastValue;
            node.args[1]->accept(*this);
            llvm::Value* defVal = impl_->lastValue;
            // Heap-VALUED dict: own the result via the incref-on-return setdefault
            // (the setdefault sibling of the owned-get fix). The
            // key-absent branch ALSO stores the default, tagged correctly, so the
            // runtime increfs twice (dict's retained copy + binding) - matched by
            // the SAME owned-temp drain the get-default path uses. A bare borrow
            // would free the dict's stored value -> UAF.
            if (isHeapValueKind(dictValueKind())) {
                // Own the borrowed heap key so the dict's stored key can't dangle
                // (the runtime releases it on the present branch). See #20a.
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
            // Scalar-valued str-keyed dict: no value UAF (scalars aren't heap),
            // but the KEY still dangles on insert - own the borrowed key (#20a).
            // The shared dragon_dict_setdefault releases it on its present branch.
            impl_->increfBorrowedSetdefaultKey(node.args[0].get(), key);
            if (defVal->getType() == impl_->i1Type) defVal = impl_->builder->CreateZExt(defVal, impl_->i64Type);
            else if (defVal->getType() == impl_->f64Type) defVal = impl_->builder->CreateBitCast(defVal, impl_->i64Type);
            else if (defVal->getType()->isPointerTy()) defVal = impl_->builder->CreatePtrToInt(defVal, impl_->i64Type);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_setdefault"],
                {obj, key, defVal}, "dictsetdef");
            return true;
        }

        // .copy() -> dragon_dict_copy(dict)
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

    // --- Set method dispatch (4.4: real hash-table-backed set) ---
    if (isSet) {
        attr.object->accept(*this);
        llvm::Value* obj = impl_->lastValue;
        // Owned set RECEIVER temp drain (mirror of ownedListRecv above). Set
        // binary ops return fresh sets whose elements are increfed on add, so
        // none can borrow from the receiver.
        bool ownedSetRecv = impl_->options.gcMode == GCMode::RC &&
                            !Impl::isBorrowedHeapExpr(attr.object.get()) &&
                            impl_->isOwnedPtrResult(obj);
        static const std::set<std::string> kSetRecvDrainOk = {
            "add", "remove", "discard", "clear", "union", "intersection",
            "difference", "symmetric_difference", "issubset", "issuperset",
            "isdisjoint"};
        // Owned heap temporaries for borrow-method args - the lookup value of
        // remove/discard (via argToI64) and the other-set arg of the binary ops
        // (via setArg). set.add is excluded: it runs its own owned-str decref
        // (the set increfs to own its element). Released at the common tail. (#3.)
        std::vector<std::pair<llvm::Value*, Impl::VarKind>> argTemps;
        bool setHandled = [&]() -> bool {

        // Helper: encode an arg expr into i64 for set storage / lookup.
        // Promotes string-literal args to heap strings so RC ownership is
        // correct (the set incref's whatever it stores). For lookups
        // (contains / discard / remove), content hashing makes the heap-vs-
        // literal distinction irrelevant - but we go through the same path
        // for code simplicity.
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
                // dragon_set_add INCREFS to take its own reference, so an
                // owned +1 temp (concat / str() / the dup ensureHeapString
                // just made) must be released after the call or every add
                // leaks one string. Borrowed reads (s.add(name)) have no +1
                // to drop and are skipped by isOwnedStrResult. Gated on the
                // arg's static str type: a non-str owned pointer would need
                // dragon_decref, not the string-header walk.
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
            // An empty set() is allocated untagged (TAG_INT -> raw i64 hashing),
            // which mis-hashes and never decrefs string elements. Teach the set
            // its element type from the first added value's static type; the
            // runtime adopts it only while the set is still empty (free, no
            // rehash). Set literals are already tagged, so this is a no-op there.
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

    // --- super parent-method dispatch ---
    // Two mode-exclusive spellings dispatch a call to the parent class's method:
    //  .dr keyword form: `super.method(args)` (attr.object is NameExpr "super")
    //  .py Python form: `super().method(args)` (attr.object is a CallExpr of "super")
    // The `super` surface tracks the file mode, like braces-vs-indent: .dr rejects
    // the Python spelling and .py rejects the keyword spelling, so neither leaks
    // into the other. `super().__init__(...)` / `super(args)` (ctor delegation) are
    // handled in CallExpr.cpp; here we only see parent *method* calls.
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

            // Resolve the parent's owning module so a cross-module parent's method
            // picks the right `<ParentMod>__Parent_<method>` symbol.
            auto parentIt = impl_->classParentNames.find(impl_->currentClassName);
            if (parentIt != impl_->classParentNames.end()) {
                auto pmIt = impl_->classOwningModule.find(parentIt->second);
                std::string parentMod = pmIt != impl_->classOwningModule.end()
                                            ? pmIt->second
                                            : impl_->currentModuleName;
                std::string parentMethodName =
                    Impl::mangleClass(parentMod, parentIt->second) + "_" + method;
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

    // --- Static method dispatch: ClassName.method(args) ---
    // Check if the object is a class name directly (not an instance variable)
    if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
        if (impl_->classNames.count(objName->name)) {
            std::string methodFuncName =
                impl_->classSymPrefix(objName->name) + "_" + method;
            if (impl_->staticMethods.count(methodFuncName)) {
                auto* methodFunc = impl_->module->getFunction(methodFuncName);
                if (methodFunc) {
                    // Static method: do NOT pass self
                    std::vector<llvm::Value*> args;
                    // Owned heap-temp args to release after the call (#3 class A).
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

    // --- Static method via a module-qualified class: mod.Class.method(args) ---
    // The receiver `mod.Class` is an AttributeExpr whose object (`mod`) is a
    // Module. The same-NameExpr block above only catches a bare `Class.method`;
    // without this, `mod.Class.staticmethod()` silently compiled to nothing.
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
                    // Owned heap-temp args to release after the call (#3 class A).
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

    // --- Class instance method dispatch ---
    if (auto* objName = dynamic_cast<NameExpr*>(attr.object.get())) {
        std::string className;
        std::string owningModule;
        if (objName->name == "self" && !impl_->currentClassName.empty()) {
            className = impl_->currentClassName;
            owningModule = impl_->currentModuleName;
        } else {
            auto vit = impl_->varClassNames.find(objName->name);
            if (vit != impl_->varClassNames.end()) className = vit->second;
            // Per-instance owning module - populated in Assign.cpp when the
            // RHS is a class instantiation. Falls back to the class's
            // forwardDeclareClasses-recorded owner if the var was assigned
            // through a path that didn't carry module info (e.g. function
            // return value).
            auto vmIt = impl_->varClassOwningModule.find(objName->name);
            if (vmIt != impl_->varClassOwningModule.end()) {
                owningModule = vmIt->second;
            } else if (!className.empty()) {
                // Route through the alias-aware resolver, not the raw
                // last-write-wins classOwningModule map: a var assigned through
                // a path that didn't record its owning module (e.g. a function
                // return value) must still honor `from X import Class` scoping,
                // or method dispatch picks a same-named class from another
                // co-compiled module.
                owningModule = impl_->resolveClassOwningModule(className);
            }
            // AUTHORITATIVE OVERRIDE. varClassNames/varClassOwningModule are
            // program-wide, keyed by bare variable name only, and never cleared:
            // a `v: Val` local in one module and a `v: FileVFS` local in another
            // co-compiled module share the one `v` entry, so a stale (class,
            // module) pair leaks across functions. When it is the MODULE that
            // goes stale, resolveMethodFunction mangles the (correct) class under
            // the wrong module, fails to find its own method, and walks UP to the
            // parent - silently dispatching FileVFS.size() to the base VFS.size().
            // The type checker already proved this receiver's exact type, so trust
            // it: pin (class, module) from the resolved InstanceType, but only when
            // that pin actually resolves the method, so this can only correct a
            // misdispatch, never introduce one.
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
            // MRO lookup via the shared resolver - walks the inheritance
            // chain and mangles each level's symbol per its owning module
            // so two same-named classes from different modules don't collide.
            std::string methodFuncName;
            auto* methodFunc = impl_->resolveMethodFunction(
                owningModule, className, method, &methodFuncName);
            // Self-correct a STALE owning module. varClassOwningModule is
            // program-wide and never cleared, and not every binding site
            // refreshes it (params/globals now do via bindClassVar; loop vars
            // and some assignment fallbacks still set only varClassNames). So a
            // stored owner can be left over from an earlier same-named binding
            // in another function. className is always fresh, so when the stored
            // owner fails to resolve the method, re-resolve the module from the
            // (canonical, alias-aware) className before giving up.
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
                // A variadic method (`*args`/`**kwargs`) packs surplus
                // positionals into a list and surplus keywords into a dict
                // (self already at args[0]), exactly like a variadic free
                // function; the packs drain through argTemps at the tail. This
                // takes precedence over the fixed-arity spread path. Non-variadic
                // spread calls expand through the shared routine; plain calls
                // keep the original positional + kwarg loops verbatim.
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
                    // An own param ADOPTS the arg's +1 (moved binding or
                    // fresh temp): the callee releases it; a caller drain
                    // here would double-free (A/B-proven fresh-temp probe).
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
                        // A MONOMORPHIZED GENERIC method (e.g. assertEqual[T])
                        // records its T param as a non-heap kind (T is erased ->
                        // Other), so argTempDecrefKind bails at !isHeapKind and an
                        // OWNED box result passed to it leaked one object per call
                        // (`self.assertEqual(m[k], v)`). Drain ONLY a provably-
                        // OWNED BOX: isOwnedBoxResult has an explicit borrowed-
                        // returner denylist (dict_get_box / dict_int_get_box /
                        // list_box_get => false), so a borrowed dict[str,Any]/
                        // list[Any] element is never double-freed.
                        if (arg->getType() == impl_->boxType) {
                            if (impl_->isOwnedBoxResult(arg))
                                argTemps.emplace_back(arg, Impl::VarKind::Union);
                        } else {
                            // NATIVE owned temps (`assertEqual(a + b + [5], ...)`,
                            // `assertEqual(mklist(), ...)`) leaked one object per
                            // call through the same erased-T gap. ownedTempDrainKind
                            // gates on the EXPRESSION first (borrowed Name/Attribute/
                            // element reads return Other), then the static type,
                            // then value provenance - so the borrowed int-keyed
                            // dict[int,str] `d[k]` getter that double-freed a bare
                            // isOwnedStrResult cut (A/B-proven UAF in
                            // test_augassign_targets + test_builtin_quickwins) is
                            // rejected at the expression gate before isOwnedStrResult
                            // is ever consulted.
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
                // D040: bind call-site keyword arguments past `self` to their
                // named parameter slots. Without this, kwargs to instance
                // methods were silently dropped and fillDefaultArgs filled the
                // slots with defaults - a silent miscompile (self.greet("Ada",
                // greeting="Hello") used the default greeting). funcParamNames
                // for a method INCLUDES "self" at index 0 (ImplInit.cpp), so a
                // std::find over it yields the correct LLVM param index
                // directly - do NOT add paramOffset.
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
                // Fill missing args with default values. Route the sink so a
                // synthesized heap default (`= [10, 20]`) is drained after the
                // call like every other owned arg temp - the free-function path
                // (CallExpr) passes it too; without the sink, methods leak one
                // default per omitting call.
                impl_->fillDefaultArgs(methodFuncName, methodFunc, args, *this,
                                       &argTemps);

                // D026 virtual dispatch. Devirtualize to a direct call (C-speed)
                // unless a subclass overrides this method - then the receiver,
                // though statically typed as `className`, may be a subclass at
                // runtime, so dispatch through its vtable (one indirect load +
                // call, the same path the D025 dynamic case uses). The
                // whole-program override check keeps the direct call for every
                // leaf / non-overridden site.
                llvm::Value* callee = methodFunc;
                if (!isStaticCall && impl_->methodIsOverridden(className, method)) {
                    auto idxIt = impl_->classMethodVtableIndices.find(className);
                    if (idxIt != impl_->classMethodVtableIndices.end()) {
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

                // Exception-safe temps: unwind frees on raise, pop+decref on
                // normal return (mirrors the free-function call path).
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

            // No method found - check if it's a callable field (ptr type)
            // e.g., route.handler(req, res, ctx) where handler is a ptr field.
            //
            // Bug A: the field can hold either a bare LLVM function pointer
            // (assigned a non-capturing lambda or top-level def) or a
            // DragonClosure* (assigned a capturing lambda). The two shapes
            // need different ABI: the closure's fn_ptr expects a trailing
            // i8* env, the bare pointer doesn't. Discriminate at runtime by
            // reading the DragonObjectHeader.type_tag (offset 8) - if it's
            // DRAGON_TAG_CLOSURE (10), unwrap fn_ptr+env from the closure
            // struct and call with env appended; otherwise call the value
            // as a bare function pointer.
            {
                auto fieldIt = impl_->classFieldIndices.find(className);
                auto fieldTypeIt = impl_->classFieldTypes.find(className);
                if (fieldIt != impl_->classFieldIndices.end() &&
                    fieldTypeIt != impl_->classFieldTypes.end()) {
                    auto idxIt = fieldIt->second.find(method);
                    if (idxIt != fieldIt->second.end()) {
                        // Load the object pointer
                        attr.object->accept(*this);
                        llvm::Value* objPtr = impl_->lastValue;
                        if (!objPtr->getType()->isPointerTy())
                            objPtr = impl_->builder->CreateIntToPtr(objPtr, impl_->i8PtrType);
                        // GEP to the field
                        auto structIt = impl_->classStructTypes.find(className);
                        auto* gep = impl_->builder->CreateStructGEP(
                            structIt->second, objPtr, idxIt->second, method + "_ptr");
                        auto* fieldType = fieldTypeIt->second[method];
                        llvm::Value* fnPtr = impl_->builder->CreateLoad(
                            fieldType, gep, method + "_val");
                        if (!fnPtr->getType()->isPointerTy())
                            fnPtr = impl_->builder->CreateIntToPtr(fnPtr, impl_->i8PtrType);

                        // Recover the user-facing FunctionType when the field
                        // was declared as `Callable[[A,B,...], R]`. Without
                        // it we fall back to a synthetic all-i64 signature
                        // (legacy path - works on x86-64 only because GP
                        // regs alias ptr/i64 args).
                        auto cfIt = impl_->classFieldCallableType.find(className);
                        llvm::FunctionType* userFnType = nullptr;
                        if (cfIt != impl_->classFieldCallableType.end()) {
                            auto fIt = cfIt->second.find(method);
                            if (fIt != cfIt->second.end()) userFnType = fIt->second;
                        }

                        // Evaluate user args once, and coerce to the recovered
                        // signature when known; else keep the legacy i64 path.
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

                        // Runtime tag check: load DragonObjectHeader.type_tag
                        // (the byte at offset 8 of every heap-allocated GC
                        // header). For a bare LLVM fn pointer this reads a
                        // byte from the function's prologue in .text - safe
                        // (r-x mapping) and statistically ~never equal to
                        // DRAGON_TAG_CLOSURE (10), so the bare path stays.
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

                        // -- Closure path: unwrap DragonClosure { hdr, fn_ptr, env } --
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

                        // -- Bare path: legacy fn pointer call --
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

                        // -- Merge --
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
            // Decision 026: Vtable dynamic dispatch - className unknown
            // (e.g., obj = cls(); obj.speak() where cls is a first-class class value)
            auto vk = impl_->lookupVarKind(objName->name);
            if (vk == Impl::VarKind::ClassInstance) {
                // Last-resort dispatch for a class instance whose concrete class
                // the compiler failed to record (a className-tracking gap - real
                // code has a known class: classes are not first-class values, and
                // isinstance narrowing now records the narrowed class). With no
                // class we cannot know which same-named method is the target, so
                // we must not GUESS a signature: an arity mismatch would emit a
                // malformed indirect call (LLVM "Incorrect number of arguments").
                // Accept a vtable candidate ONLY when its signature matches this
                // call's arity; otherwise fall through to the clean "cannot
                // resolve method" diagnostic (CallExpr.cpp). All classes in a
                // hierarchy share a method's vtable index, so an arity match is
                // the safe common denominator.
                const size_t wantParams = node.args.size() + 1;  // self + args
                int methodIndex = -1;
                llvm::FunctionType* methodFuncType = nullptr;
                for (auto& [cls, methodMap] : impl_->classMethodVtableIndices) {
                    auto it = methodMap.find(method);
                    if (it == methodMap.end()) continue;
                    // MRO walk via the shared resolver - vtable's stored
                    // function pointer must match a real mangled symbol
                    // for the class or one of its parents.
                    auto cmIt = impl_->classOwningModule.find(cls);
                    std::string clsMod = cmIt != impl_->classOwningModule.end()
                                             ? cmIt->second
                                             : impl_->currentModuleName;
                    auto* func = impl_->resolveMethodFunction(clsMod, cls, method);
                    if (func && func->getFunctionType()->getNumParams() == wantParams) {
                        methodIndex = (int)it->second;
                        methodFuncType = func->getFunctionType();
                        break;
                    }
                }

                if (methodIndex >= 0 && methodFuncType) {
                    // Load object pointer
                    attr.object->accept(*this);
                    llvm::Value* objPtr = impl_->lastValue;
                    if (!objPtr->getType()->isPointerTy())
                        objPtr = impl_->builder->CreateIntToPtr(objPtr, impl_->i8PtrType);

                    // Load vtable pointer from struct offset 2 using a minimal header struct type
                    auto* headerStructType = llvm::StructType::get(*impl_->context,
                        {impl_->i64Type, impl_->i64Type, impl_->i8PtrType});
                    auto* vtableSlot = impl_->builder->CreateStructGEP(
                        headerStructType, objPtr, 2, "vt_slot");
                    auto* vtablePtr = impl_->builder->CreateLoad(
                        impl_->i8PtrType, vtableSlot, "vtable");

                    // GEP into vtable array to get method function pointer
                    auto* vtableArrayType = llvm::ArrayType::get(impl_->i8PtrType, 0);
                    auto* methodSlot = impl_->builder->CreateGEP(
                        vtableArrayType, vtablePtr,
                        {impl_->builder->getInt64(0), impl_->builder->getInt64(methodIndex)},
                        "method_slot");
                    auto* methodPtr = impl_->builder->CreateLoad(
                        impl_->i8PtrType, methodSlot, "method_ptr");

                    // Build argument list: self + user args
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

                    // Indirect call through vtable
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

    // --- Class instance method dispatch on a non-Name receiver ---
    // Handles `make_box(42).show()`, `(a + b).render()`, `arr[0].id()` - anything
    // whose class identity can be resolved statically via `resolveExprClassName`.
    // Without this, chained method calls on temporaries silently fall through
    // and produce `i64 0`, masking the real call entirely.
    if (!dynamic_cast<NameExpr*>(attr.object.get())) {
        std::string className = impl_->resolveExprClassName(attr.object.get());
        if (!className.empty() && impl_->classNames.count(className)) {
            // Owning module via the alias-aware resolver so a chained call on a
            // temporary (`s.field(...).optional(...)`) honors `from X import
            // Class` scoping. Without per-expression module tracking the base
            // is the importing module's alias (or the same-module probe); it
            // falls back to last-write-wins only when neither applies.
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
                // Variadic method on a non-Name receiver - pack `*args`/`**kwargs`
                // (self already pushed), same as the NameExpr-receiver block
                // above. Precedes the fixed-arity spread path.
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
                    // An own param ADOPTS the arg's +1 (moved binding or
                    // fresh temp): the callee releases it; a caller drain
                    // here would double-free (A/B-proven fresh-temp probe).
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
                        // Monomorphized generic method (T erased -> non-heap kind):
                        // drain a provably-owned BOX (isOwnedBoxResult has a
                        // borrowed-returner denylist), or a NATIVE owned temp via
                        // ownedTempDrainKind (expression-gated first, so borrowed
                        // getters never reach the isOwnedStrResult false-positive -
                        // see the NameExpr-receiver path above).
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
                // D040: bind call-site keyword arguments past `self` to their
                // named parameter slots (same as the NameExpr-receiver path
                // above). funcParamNames INCLUDES "self" at index 0, so the
                // std::find index is already the LLVM param position - no
                // paramOffset added.
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
                // Fill omitted trailing parameters with their defaults - same as
                // the NameExpr-receiver path above. Without this, a call on a
                // field/temporary receiver (`self.ctx.wrap_socket(s, False, h)`
                // omitting do_handshake_on_connect=True) passed too few args and
                // LLVM rejected the call.
                // Sink wired for the same reason as the NameExpr-receiver path
                // above: method heap defaults must drain.
                impl_->fillDefaultArgs(methodFuncName, methodFunc, args, *this,
                                       &argTemps);

                // D026 virtual dispatch on a non-Name receiver (temporaries,
                // `make(...).speak()`, `arr[0].speak()`): same devirtualize-
                // unless-overridden rule as the NameExpr path above.
                llvm::Value* callee = methodFunc;
                if (!isStaticCall && impl_->methodIsOverridden(className, method)) {
                    auto idxIt = impl_->classMethodVtableIndices.find(className);
                    if (idxIt != impl_->classMethodVtableIndices.end()) {
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

    // --- Stdlib module method dispatch (e.g., math.sqrt) ---
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
