/// Dragon CodeGen - Exception Handling (Try, With, Match, Raise)
#include "../CodeGenImpl.h"

namespace dragon {

void CodeGen::visit(AssertStmt& node) {
    node.test->accept(*this);
    llvm::Value* cond = impl_->lastValue;
    if (cond->getType() == impl_->i64Type) {
        // Already int
    } else if (cond->getType() == impl_->i1Type) {
        cond = impl_->builder->CreateZExt(cond, impl_->i64Type);
    }

    if (node.msg) {
        node.msg->accept(*this);
        llvm::Value* msgVal = impl_->lastValue;
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_assert"], {cond, msgVal});
        // Passing-assert path: an owned +1 message temp (concat / f-string)
        // must be released here or every passing assert leaks it. On the
        // failing path the raise longjmps past this decref and the slot's
        // protective dup keeps the message alive; the temp itself leaks
        // once per CAUGHT failing assert - rare and bounded.
        if (impl_->options.gcMode == GCMode::RC &&
            impl_->isOwnedStrResult(msgVal))
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_decref_str"], {msgVal});
    } else {
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_assert_no_msg"], {cond});
    }
}

// Control flow / misc stubs
void CodeGen::visit(TryStmt& node) {
    auto* func = impl_->currentFunction;
    int excId = impl_->excCounter++;
    std::string prefix = "try" + std::to_string(excId);

    // Create basic blocks
    auto* tryBodyBB  = llvm::BasicBlock::Create(*impl_->context, prefix + ".body", func);
    auto* dispatchBB = llvm::BasicBlock::Create(*impl_->context, prefix + ".dispatch", func);

    // Analyze handlers
    struct HandlerInfo {
        llvm::BasicBlock* checkBB = nullptr;
        llvm::BasicBlock* bodyBB  = nullptr;
        int64_t typeCode = 0; // 0 = catch-all
        std::vector<int64_t> altCodes; // `except (A, B, ...)` extra type codes
    };
    std::vector<HandlerInfo> handlerInfos;
    bool hasCatchAll = false;

    for (size_t i = 0; i < node.handlers.size(); ++i) {
        HandlerInfo hi;
        hi.bodyBB = llvm::BasicBlock::Create(*impl_->context,
            prefix + ".handler." + std::to_string(i), func);

        auto& handler = node.handlers[i];
        if (handler.type) {
            hi.typeCode = 10; // default to Exception
            if (auto* named = dynamic_cast<NamedTypeExpr*>(handler.type.get())) {
                hi.typeCode = impl_->excTypeCode(named->name);
            }
            for (const auto& alt : handler.altTypeNames)
                hi.altCodes.push_back(impl_->excTypeCode(alt));
            hi.checkBB = llvm::BasicBlock::Create(*impl_->context,
                prefix + ".handler.check." + std::to_string(i), func);
        } else {
            hi.typeCode = 0;
            hasCatchAll = true;
        }
        handlerInfos.push_back(hi);
    }

    // Unmatched block for re-raise (only if no catch-all)
    llvm::BasicBlock* unmatchedBB = nullptr;
    if (!hasCatchAll && !node.handlers.empty()) {
        unmatchedBB = llvm::BasicBlock::Create(*impl_->context,
            prefix + ".unmatched", func);
    }

    // Else and finally blocks
    llvm::BasicBlock* elseBB = nullptr;
    if (!node.elseBody.empty()) {
        elseBB = llvm::BasicBlock::Create(*impl_->context, prefix + ".else", func);
    }

    llvm::BasicBlock* finallyBB = nullptr;
    if (!node.finallyBody.empty()) {
        finallyBB = llvm::BasicBlock::Create(*impl_->context, prefix + ".finally", func);
    }

    auto* endBB = llvm::BasicBlock::Create(*impl_->context, prefix + ".end", func);

    // A try whose exception goes unhandled (no handler at all, or no handler
    // matched) must still run `finally` and THEN re-raise - never silently
    // swallow it. The unhandled paths record the in-flight exception into these
    // slots, route through `finally`, and re-raise at reraiseCheckBB if the
    // flag is set. (setjmp-safe: these are memory allocas, not registers.)
    auto* reraiseFlag = impl_->createEntryAlloca(func, prefix + ".rr.flag", impl_->i1Type);
    auto* savedType = impl_->createEntryAlloca(func, prefix + ".rr.type", impl_->i64Type);
    auto* savedObj  = impl_->createEntryAlloca(func, prefix + ".rr.obj", impl_->i8PtrType);
    auto* savedMsg  = impl_->createEntryAlloca(func, prefix + ".rr.msg", impl_->i8PtrType);
    impl_->builder->CreateStore(llvm::ConstantInt::getFalse(*impl_->context), reraiseFlag);

    auto* reraiseCheckBB = llvm::BasicBlock::Create(*impl_->context, prefix + ".rrcheck", func);
    auto* doReraiseBB = llvm::BasicBlock::Create(*impl_->context, prefix + ".reraise", func);

    // Determine merge points. `finally` (when present) flows into
    // reraiseCheckBB; with no finally, unhandled paths jump straight there.
    llvm::BasicBlock* afterHandlerBB = finallyBB ? finallyBB : reraiseCheckBB;
    llvm::BasicBlock* afterTryBodyBB = elseBB ? elseBB : afterHandlerBB;

    // === Push frame + setjmp + branch ===
    auto* jmpbufPtr = impl_->builder->CreateCall(
        impl_->runtimeFuncs["dragon_exc_push_frame"], {}, "jmpbuf");
    auto* setjmpResult = impl_->builder->CreateCall(
        impl_->runtimeFuncs["setjmp"], {jmpbufPtr}, "setjmp.result");
    auto* isNormal = impl_->builder->CreateICmpEQ(
        setjmpResult,
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(*impl_->context), 0),
        "is.normal");
    impl_->builder->CreateCondBr(isNormal, tryBodyBB, dispatchBB);

    // === Push exit-cleanup stack so return/break/continue can inline finally ===
    if (!node.finallyBody.empty()) {
        Impl::ExitCleanup ec;
        ec.isWith = false;
        ec.func = impl_->currentFunction;
        ec.scopeDepth = impl_->scopes.size();
        for (auto& s : node.finallyBody) ec.finallyBody.push_back(s.get());
        impl_->exitCleanupStack.push_back(std::move(ec));
    }

    // === Try body ===
    impl_->builder->SetInsertPoint(tryBodyBB);
    // Frame is live for the duration of the body so a return/break/continue
    // inside it pops it (the normal-exit pop below is bypassed once they set a
    // terminator). Keyed by function for free nested-function isolation.
    impl_->tryFrameFuncs.push_back(func);
    // The try body is its own lexical scope (block-scoping). This is load-bearing
    // for the unwind cleanup: its owned heap locals are freed EITHER by codegen
    // (normal completion, below) OR by dragon_exc_cleanup_unwind (longjmp arrival
    // at dispatch) - never both. If they lived in the enclosing function scope
    // instead, the function-return cleanup would re-decref what the unwind already
    // freed on the caught path (double-free).
    impl_->pushScope();
    for (auto& stmt : node.tryBody) stmt->accept(*this);
    bool tryTerminated = impl_->builder->GetInsertBlock()->getTerminator() != nullptr;
    if (!tryTerminated) {
        // Normal completion: codegen decrefs the try-body locals and rewinds the
        // cleanup stack to this try's depth.
        impl_->emitScopeCleanup();
    }
    impl_->popScope();
    impl_->tryFrameFuncs.pop_back();
    if (!tryTerminated) {
        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_pop_frame"], {});
        impl_->builder->CreateBr(afterTryBodyBB);
    }

    // === Dispatch block ===
    impl_->builder->SetInsertPoint(dispatchBB);
    // Free the owned heap locals the longjmp skipped over (the try body's locals
    // declared after this frame's setjmp). Runs BEFORE pop_frame so it reads this
    // frame's saved cleanup depth. See DragonCleanupStack.
    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_cleanup_unwind"], {});
    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_pop_frame"], {});
    auto* excType = impl_->builder->CreateCall(
        impl_->runtimeFuncs["dragon_exc_get_type"], {}, "exc.type");

    if (!handlerInfos.empty()) {
        if (handlerInfos[0].checkBB) {
            impl_->builder->CreateBr(handlerInfos[0].checkBB);
        } else {
            // First handler is catch-all
            impl_->builder->CreateBr(handlerInfos[0].bodyBB);
        }
    } else {
        // No handlers (try with only finally): record the in-flight exception,
        // run finally, then re-raise - do NOT swallow it.
        // Retain BOTH the saved instance and message (+1 each): if the finally
        // body raises and catches internally, the slot overwrite releases the
        // old values - without these holds the deferred re-raise would use
        // freed pointers. The consume re-raise transfers both holds back.
        auto* curObj = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_exc_retain_obj"],
            {impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_exc_get_obj"], {}, "rr.obj.raw")},
            "rr.obj");
        auto* curMsg = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_exc_bind_msg"], {}, "rr.msg");
        impl_->builder->CreateStore(excType, savedType);
        impl_->builder->CreateStore(curObj, savedObj);
        impl_->builder->CreateStore(curMsg, savedMsg);
        impl_->builder->CreateStore(llvm::ConstantInt::getTrue(*impl_->context), reraiseFlag);
        impl_->builder->CreateBr(afterHandlerBB);
    }

    // === Handler check + body blocks ===
    for (size_t i = 0; i < handlerInfos.size(); ++i) {
        auto& hi = handlerInfos[i];
        auto& handler = node.handlers[i];

        // Emit check block (typed handlers only)
        if (hi.checkBB) {
            impl_->builder->SetInsertPoint(hi.checkBB);
            // Use runtime dragon_exc_matches for both built-in and user-defined.
            // For `except (A, B, ...)` the handler matches if ANY listed type
            // matches, so OR the per-type results together.
            auto matchCode = [&](int64_t code, const std::string& tag) -> llvm::Value* {
                auto* r = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_exc_matches"],
                    {excType, llvm::ConstantInt::get(impl_->i64Type, code)}, tag);
                return impl_->builder->CreateICmpNE(
                    r, llvm::ConstantInt::get(impl_->i64Type, 0), tag + ".b");
            };
            llvm::Value* cmp = matchCode(hi.typeCode, "exc.match." + std::to_string(i));
            for (size_t a = 0; a < hi.altCodes.size(); ++a) {
                cmp = impl_->builder->CreateOr(
                    cmp,
                    matchCode(hi.altCodes[a],
                              "exc.match." + std::to_string(i) + "." + std::to_string(a)),
                    "exc.any." + std::to_string(i) + "." + std::to_string(a));
            }

            // Find next block if this handler doesn't match
            llvm::BasicBlock* nextBB = nullptr;
            for (size_t j = i + 1; j < handlerInfos.size(); ++j) {
                if (handlerInfos[j].checkBB) {
                    nextBB = handlerInfos[j].checkBB;
                } else {
                    nextBB = handlerInfos[j].bodyBB;
                }
                break;
            }
            if (!nextBB) {
                nextBB = unmatchedBB ? unmatchedBB : afterHandlerBB;
            }

            impl_->builder->CreateCondBr(cmp, hi.bodyBB, nextBB);
        }

        // Emit handler body
        impl_->builder->SetInsertPoint(hi.bodyBB);
        impl_->pushScope();

        // If handler has a name, bind exception to it. Two shapes:
        //  1. Typed-field path - `raise UserExc(args)` constructed an
        //  instance and routed through dragon_raise_exc_obj. The handler
        //  type is the same user class (or an ancestor); bind `e` to
        //  that instance so `e.code` / `e.reason` / `e.url` work.
        //  2. Message-only path - built-in raise: bind `e` to the message
        //  string (historical behavior, used by 100% of stdlib `except
        //  OSError as e: print(e)` sites).
        // The instance pointer dominates the message: if a handler types
        // its binding to a user class, we want the typed instance even if
        // the runtime's msg slot was set as a fallback.
        if (!handler.name.empty()) {
            bool boundInstance = false;
            if (auto* named = dynamic_cast<NamedTypeExpr*>(handler.type.get())) {
                // Bind as instance when the handler types its binding to a
                // user-defined exception class. Built-in handler types
                // (Exception, ValueError, ...) keep the message-string
                // binding - they have no struct shape for `e.x` access.
                if (impl_->userExcCodes.count(named->name) > 0 &&
                    impl_->classNames.count(named->name)) {
                    // dragon_exc_bind_obj returns the in-flight instance with
                    // its OWN +1 (the slot keeps its ref; the next raise's
                    // overwrite releases it). The binding's scope cleanup
                    // drops this +1 on the normal path; the unwind cleanup
                    // entry drops it when a nested raise longjmps past the
                    // handler.
                    auto* obj = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_exc_bind_obj"], {}, "exc.obj");
                    auto* alloca = impl_->createEntryAlloca(
                        func, handler.name, impl_->i8PtrType);
                    impl_->builder->CreateStore(obj, alloca);
                    // ClassInstance binding routes attribute / method access
                    // to the class struct + emitted methods. varClassNames
                    // pins the concrete class for resolveExprClassName so
                    // `e.code` etc. lower via the right struct GEP.
                    impl_->setVar(handler.name, alloca, Impl::VarKind::ClassInstance);
                    impl_->varClassNames[handler.name] = named->name;
                    impl_->emitCleanupPush(handler.name, obj, Impl::DCLEAN_OBJ);
                    boundInstance = true;
                }
            }
            if (!boundInstance) {
                // dragon_exc_bind_msg returns the in-flight message with its
                // OWN +1 (mortal heap only; literals/immortals no-op), so a
                // nested raise inside this handler - which overwrites and
                // releases the slot - cannot leave `e` dangling. Bind as Str:
                // the handler's scope cleanup (and the unwind cleanup entry)
                // drop the +1; dragon_decref_str is literal-safe.
                auto* msg = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_exc_bind_msg"], {}, "exc.msg");
                auto* alloca = impl_->createEntryAlloca(
                    func, handler.name, impl_->i8PtrType);
                impl_->builder->CreateStore(msg, alloca);
                impl_->setVar(handler.name, alloca, Impl::VarKind::Str);
                impl_->emitCleanupPush(handler.name, msg, Impl::DCLEAN_STR);
            }
            // Track the bound name so `raise <name>` inside this body is
            // recognized as a re-raise of the in-flight exception (RaiseStmt).
            impl_->handlerExcVars.push_back(handler.name);
        }

        for (auto& stmt : handler.body) stmt->accept(*this);

        if (!handler.name.empty()) {
            impl_->handlerExcVars.pop_back();
        }
        impl_->emitScopeCleanup();
        impl_->popScope();
        if (!impl_->builder->GetInsertBlock()->getTerminator())
            impl_->builder->CreateBr(afterHandlerBB);
    }

    // === Unmatched block (re-raise) ===
    // Preserve the original instance pointer too so a typed-field exception
    // raised inside this try block keeps its instance for an outer handler.
    if (unmatchedBB) {
        impl_->builder->SetInsertPoint(unmatchedBB);
        auto* reType = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_exc_get_type"], {}, "reraise.type");
        // Retained (+1) saves - see the no-handler path above.
        auto* reObj = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_exc_retain_obj"],
            {impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_exc_get_obj"], {}, "reraise.obj.raw")},
            "reraise.obj");
        auto* reMsg = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_exc_bind_msg"], {}, "reraise.msg");
        // Record + route through finally (if any) before re-raising, so a
        // non-matching handler doesn't skip the finally block.
        impl_->builder->CreateStore(reType, savedType);
        impl_->builder->CreateStore(reObj, savedObj);
        impl_->builder->CreateStore(reMsg, savedMsg);
        impl_->builder->CreateStore(llvm::ConstantInt::getTrue(*impl_->context), reraiseFlag);
        impl_->builder->CreateBr(afterHandlerBB);
    }

    // === Else block ===
    if (elseBB) {
        impl_->builder->SetInsertPoint(elseBB);
        for (auto& stmt : node.elseBody) stmt->accept(*this);
        if (!impl_->builder->GetInsertBlock()->getTerminator())
            impl_->builder->CreateBr(afterHandlerBB);
    }

    // === Finally block ===
    if (finallyBB) {
        impl_->builder->SetInsertPoint(finallyBB);
        for (auto& stmt : node.finallyBody) stmt->accept(*this);
        if (!impl_->builder->GetInsertBlock()->getTerminator())
            impl_->builder->CreateBr(reraiseCheckBB);
    }

    // === Pop exit-cleanup stack ===
    if (!node.finallyBody.empty()) {
        impl_->exitCleanupStack.pop_back();
    }

    // === Re-raise check ===
    // Reached after `finally` (or directly, when there is none). If the
    // exception went unhandled, re-raise it now that finally has run.
    impl_->builder->SetInsertPoint(reraiseCheckBB);
    {
        auto* flag = impl_->builder->CreateLoad(impl_->i1Type, reraiseFlag, "rr.load");
        impl_->builder->CreateCondBr(flag, doReraiseBB, endBB);
    }
    impl_->builder->SetInsertPoint(doReraiseBB);
    {
        auto* t = impl_->builder->CreateLoad(impl_->i64Type, savedType, "rr.t");
        auto* o = impl_->builder->CreateLoad(impl_->i8PtrType, savedObj, "rr.o");
        auto* m = impl_->builder->CreateLoad(impl_->i8PtrType, savedMsg, "rr.m");
        // The save retained the message (+1, dragon_exc_bind_msg); the
        // consume raise transfers that hold into the slot.
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_raise_exc_obj_consume"], {t, o, m});
        impl_->builder->CreateUnreachable();
    }

    // === Continue at end ===
    impl_->builder->SetInsertPoint(endBB);
}

void CodeGen::visit(WithStmt& node) {
    // Context handle tracking. `val` is the manager object (the ctor temp the
    // `with` owns - one ref). `enterResult` is what __enter__ returned and bound
    // to the `as` var; for the common `return self` it is the SAME object with a
    // SECOND ref (the return convention increfs). Both refs are released at
    // with-exit - releasing only `val` left the as-binding ref.
    struct CtxInfo {
        llvm::Value* val;
        bool isClassCtx;
        bool isLock;
        std::string className;
        llvm::Value* enterResult = nullptr;  // class CMs only; may == val
        bool isLockTemp = false;  // anonymous `with Lock()` - the with owns + frees it
        bool subjectOwned = true;  // false for a borrowed subject (bound local /
                                   // attribute): its slot owns the manager, so
                                   // with-exit must not decref `val` - doing so
                                   // over-released and the slot's scope-exit
                                   // decref read freed memory (A/B-proven UAF,
                                   // test_d045_privacy / test_rc_with_subject.dr).
    };
    std::vector<CtxInfo> contextHandles;

    for (auto& item : node.items) {
        // Check for class-based context manager (__enter__/__exit__)
        std::string ctxClassName = impl_->resolveExprClassName(item.contextExpr.get());
        // Fallback: a module-function call like `database.open(...)` returns a
        // class instance, but resolveExprClassName can't name it (it handles
        // `mod.Class(...)` and `func(...)`, not `mod.func(...)`). Use the expr's
        // inferred InstanceType so the `as` variable is class-tracked - without
        // it the with-bound var has no class, and generic method calls on it
        // (e.g. `db.all[T](...)`) can't resolve their receiver's class (the
        // non-generic methods happen to work, so the bug is silent).
        if (ctxClassName.empty() && item.contextExpr->type) {
            if (auto inst = std::dynamic_pointer_cast<InstanceType>(item.contextExpr->type))
                if (inst->classType && impl_->classNames.count(inst->classType->name))
                    ctxClassName = inst->classType->name;
        }

        // Intrinsic `Lock` context: `with lock { }` or `with Lock() { }`.
        // Lock has no class/dunders - it lowers to acquire on entry and
        // release on every exit (normal + exception), directly.
        bool isLockCtx = false;
        bool isLockTemp = false;  // `with Lock()` mints an anonymous lock the
                                  // with OWNS - it must be DESTROYED (not just
                                  // released) on exit or every use leaks the
                                  // pthread_mutex. A
                                  // named `with g:` lock is owned by its scope -
                                  // release only, never destroy here.
        if (impl_->isLockExpr(item.contextExpr.get())) {
            // Covers a tagged local/global (`with glock`) AND a Lock-typed
            // instance field (`with self._storage_lock`) - the NameExpr-only
            // check let field locks fall to the generic non-class context
            // path, which binds the value and SILENTLY SKIPS acquire/release
            // (the "critical section" ran unlocked; found by the concurrent-
            // mutation detector on Router._storage_lock).
            isLockCtx = true;
        } else if (auto* ce = dynamic_cast<CallExpr*>(item.contextExpr.get())) {
            if (auto* cn = dynamic_cast<NameExpr*>(ce->callee.get()))
                if (cn->name == "Lock") { isLockCtx = true; isLockTemp = true; }
        }

        item.contextExpr->accept(*this);
        llvm::Value* ctxVal = impl_->lastValue;
        llvm::Value* enterResultV = nullptr;  // __enter__ result (class CMs)
        // Ownership of the manager object follows the subject EXPRESSION:
        // `with Guard()` mints a ctor temp the with owns (+1 to drop at exit);
        // `with g` / `with self.guard` borrow the slot's reference.
        bool subjectOwned = !Impl::isBorrowedHeapExpr(item.contextExpr.get());

        bool isClassCtx = !isLockCtx && !ctxClassName.empty() &&
            impl_->hasDunder(ctxClassName, "__enter__") &&
            impl_->hasDunder(ctxClassName, "__exit__") &&
            (ctxVal->getType() == impl_->i8PtrType || ctxVal->getType()->isPointerTy());

        if (isLockCtx) {
            // Acquire on entry; `__enter__` on a Lock returns self.
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_lock_acquire"], {ctxVal});
            if (item.optionalVars) {
                if (auto* nameExpr = dynamic_cast<NameExpr*>(item.optionalVars.get())) {
                    auto* alloca = impl_->createEntryAlloca(
                        impl_->currentFunction, nameExpr->name, ctxVal->getType());
                    impl_->builder->CreateStore(ctxVal, alloca);
                    impl_->setVar(nameExpr->name, alloca);
                    impl_->varClassNames[nameExpr->name] = "__Lock";
                }
            }
        } else if (isClassCtx) {
            // Call __enter__() - result is bound to `as` variable
            enterResultV = impl_->callDunder(ctxClassName, "__enter__", ctxVal);

            if (item.optionalVars) {
                if (auto* nameExpr = dynamic_cast<NameExpr*>(item.optionalVars.get())) {
                    auto* alloca = impl_->createEntryAlloca(
                        impl_->currentFunction, nameExpr->name, enterResultV->getType());
                    impl_->builder->CreateStore(enterResultV, alloca);
                    impl_->setVar(nameExpr->name, alloca);
                    impl_->varClassNames[nameExpr->name] = ctxClassName;
                }
            }
        } else {
            // Non-class, non-lock context - bind ctxVal directly.
            if (item.optionalVars) {
                if (auto* nameExpr = dynamic_cast<NameExpr*>(item.optionalVars.get())) {
                    auto* alloca = impl_->createEntryAlloca(
                        impl_->currentFunction, nameExpr->name, ctxVal->getType());
                    impl_->builder->CreateStore(ctxVal, alloca);
                    impl_->setVar(nameExpr->name, alloca, Impl::VarKind::Other);
                }
            }
        }
        contextHandles.push_back({ctxVal, isClassCtx, isLockCtx, ctxClassName, enterResultV, isLockTemp, subjectOwned});
    }

    // Class context managers (__exit__) and locks (release) both need an
    // exception-safe exit, so both take the setjmp/longjmp-wrapped path.
    bool needsExcSafe = false;
    for (auto& ci : contextHandles) {
        if (ci.isClassCtx || ci.isLock) { needsExcSafe = true; break; }
    }

    if (needsExcSafe) {
        // Wrap body in setjmp/longjmp for exception-safe __exit__ calls
        auto* func = impl_->currentFunction;
        auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, "with.body", func);
        auto* excBB = llvm::BasicBlock::Create(*impl_->context, "with.exc", func);
        auto* cleanupBB = llvm::BasicBlock::Create(*impl_->context, "with.cleanup", func);
        auto* endBB = llvm::BasicBlock::Create(*impl_->context, "with.end", func);

        // Push exception frame
        auto* jmpbufPtr = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_exc_push_frame"], {}, "jmpbuf");
        auto* setjmpResult = impl_->builder->CreateCall(
            impl_->runtimeFuncs["setjmp"], {jmpbufPtr}, "setjmp.result");
        auto* isNormal = impl_->builder->CreateICmpEQ(
            setjmpResult,
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*impl_->context), 0),
            "is.normal");
        impl_->builder->CreateCondBr(isNormal, bodyBB, excBB);

        // Normal body. Frame is live for its duration so a return/break/
        // continue inside the `with` pops it (mirrors the try-body handling).
        impl_->builder->SetInsertPoint(bodyBB);
        impl_->tryFrameFuncs.push_back(func);
        // Register this with's __exit__/lock-release set so an early
        // return/break/continue inside the body replays it.
        {
            Impl::ExitCleanup ec;
            ec.isWith = true;
            ec.func = func;
            ec.scopeDepth = impl_->scopes.size();
            for (auto& ci : contextHandles)
                ec.withItems.push_back({ci.isClassCtx, ci.isLock, ci.className, ci.val, ci.enterResult, ci.isLockTemp, ci.subjectOwned});
            impl_->exitCleanupStack.push_back(std::move(ec));
        }
        // The with body is its own lexical scope, so a defer registered in it
        // runs at the body's exit - BEFORE __exit__ (defer.md section 4).
        impl_->pushScope();
        for (auto& stmt : node.body) stmt->accept(*this);
        if (!impl_->builder->GetInsertBlock()->getTerminator())
            impl_->emitScopeCleanup();
        impl_->popScope();
        impl_->exitCleanupStack.pop_back();
        impl_->tryFrameFuncs.pop_back();
        // Only pop the frame + fall through to cleanup if the body did not
        // already terminate. A `return` leaves a (no-terminator) dead block, so
        // this still runs harmlessly into it; a `break`/`continue` leaves the
        // body block terminated by its branch, so we must NOT append here (that
        // produced "terminator in the middle of a block" invalid IR) - the
        // early exit already replayed __exit__/release at the jump site.
        if (!impl_->builder->GetInsertBlock()->getTerminator()) {
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_pop_frame"], {});
            impl_->builder->CreateBr(cleanupBB);
        }

        // Exception path: free unwound body locals, pop frame, call __exit__ /
        // release locks, re-raise. The unwind (before pop_frame) rewinds the
        // cleanup stack to this frame's saved depth, so the chained re-raise's
        // outer unwind won't re-process the with-body's already-freed locals.
        impl_->builder->SetInsertPoint(excBB);
        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_cleanup_unwind"], {});
        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_pop_frame"], {});
        for (auto& ci : contextHandles) {
            if (ci.isClassCtx) {
                impl_->callDunder(ci.className, "__exit__", ci.val);
                if (impl_->options.gcMode == GCMode::RC) {   // release the CM object (#8)
                    if (ci.subjectOwned)  // borrowed subject: the slot owns it
                        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {ci.val});
                    if (ci.enterResult && ci.enterResult->getType()->isPointerTy())
                        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {ci.enterResult});
                }
            } else if (ci.isLock) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_lock_release"], {ci.val});
                if (ci.isLockTemp)  // anonymous `with Lock()` - free the mutex
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_lock_destroy"], {ci.val});
            }
        }
        {
            auto* reType = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_exc_get_type"], {}, "reraise.type");
            // Preserve the typed-field instance too: re-raising through the
            // msg-only entry point would NULL exc_obj and a downstream
            // `except UserExc as e` handler would lose its instance binding.
            // The obj-raise consumes a +1, and this re-raise borrows the
            // slot's own pointer - retain first; the same-pointer fold in
            // dragon_exc_obj_set folds it straight back (net no-op).
            auto* reObj = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_exc_retain_obj"],
                {impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_exc_get_obj"], {},
                    "reraise.obj.raw")},
                "reraise.obj");
            auto* reMsg = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_exc_get_msg"], {}, "reraise.msg");
            // msg == slot: dragon_exc_msg_set's self-store no-op keeps the
            // slot's existing ownership - plain (non-consume) is correct here.
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_raise_exc_obj"], {reType, reObj, reMsg});
        }
        impl_->builder->CreateUnreachable();

        // Normal cleanup: call __exit__ / release locks / close files, then
        // release the context-manager OBJECT itself. The CM is a
        // ctor temp the `with` owns; __exit__ runs first (it may still use the
        // object), then we drop the ctor's +1. Decref AFTER __exit__.
        impl_->builder->SetInsertPoint(cleanupBB);
        for (auto& ci : contextHandles) {
            if (ci.isClassCtx) {
                impl_->callDunder(ci.className, "__exit__", ci.val);
                if (impl_->options.gcMode == GCMode::RC) {
                    if (ci.subjectOwned)  // borrowed subject: the slot owns it
                        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {ci.val});
                    if (ci.enterResult && ci.enterResult->getType()->isPointerTy())
                        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {ci.enterResult});
                }
            } else if (ci.isLock) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_lock_release"], {ci.val});
                if (ci.isLockTemp)  // anonymous `with Lock()` - free the mutex
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_lock_destroy"], {ci.val});
            }
        }
        impl_->builder->CreateBr(endBB);

        impl_->builder->SetInsertPoint(endBB);
    } else {
        // No class context managers or locks - nothing to clean up on exit,
        // but the body is still its own lexical scope (defers, block locals).
        impl_->pushScope();
        for (auto& stmt : node.body) stmt->accept(*this);
        if (!impl_->builder->GetInsertBlock()->getTerminator())
            impl_->emitScopeCleanup();
        impl_->popScope();
    }
}

void CodeGen::visit(MatchStmt& node) {
    auto* func = impl_->currentFunction;

    // Evaluate the subject expression once and store it.
    node.subject->accept(*this);
    llvm::Value* subjectVal = impl_->lastValue;
    llvm::Type* subjectTy = subjectVal->getType();

    // Store subject in an alloca so we can reload it in each arm.
    auto* subjectAlloca = impl_->createEntryAlloca(func, "match.subject", subjectTy);
    impl_->builder->CreateStore(subjectVal, subjectAlloca);

    // For class/type-test patterns (`case TypeName()`): the subject's static
    // type drives ptr-shaped discrimination (str vs list vs class) and the
    // class-chain walk; a Union/Any subject uses the runtime box tag instead.
    std::shared_ptr<Type> subjectStaticType =
        node.subject ? node.subject->type : nullptr;
    // Resolve a class name from a (possibly union/Optional) static type - class
    // instance ptrs carry no runtime type tag, so `case ClassName()` leans on
    // the static type, including the non-None member of a `Class | None`.
    auto classNameOfType = [](Type* t) -> std::string {
        if (!t) return "";
        if (auto* inst = dynamic_cast<InstanceType*>(t))
            return inst->classType ? inst->classType->name : "";
        if (auto* ct = dynamic_cast<ClassType*>(t)) return ct->name;
        if (auto* ut = dynamic_cast<UnionType*>(t))
            for (auto& m : ut->types) {
                if (auto* inst = dynamic_cast<InstanceType*>(m.get()))
                    return inst->classType ? inst->classType->name : "";
                if (auto* ct = dynamic_cast<ClassType*>(m.get())) return ct->name;
            }
        return "";
    };
    std::string subjectClassName = impl_->resolveExprClassName(node.subject.get());
    if (subjectClassName.empty() && subjectStaticType)
        subjectClassName = classNameOfType(subjectStaticType.get());

    // #1 in-arm narrowing: when the subject is a bare name `v` and an arm is a
    // single scalar type-test `case T()`, shadow `v` with the unboxed native
    // payload inside that arm so the body uses it at its matched type (e.g.
    // `case int() { print(v + 1) }`) with one payload-extract of cost. Kept in
    // sync with the TypeChecker's narrowing predicate (scalars only here; class
    // narrowing rides with field destructuring). Speed: the arm body then emits
    // native i64/f64 ops instead of boxed ones - narrowing makes it faster.
    auto* subjectNameExpr = dynamic_cast<NameExpr*>(node.subject.get());
    std::string subjectName = subjectNameExpr ? subjectNameExpr->name : "";
    auto scalarNarrowKind = [&](const std::string& tn) -> Impl::VarKind {
        if (tn == "int")   return Impl::VarKind::Int;
        if (tn == "float") return Impl::VarKind::Float;
        if (tn == "bool")  return Impl::VarKind::Bool;
        if (tn == "str")   return Impl::VarKind::Str;
        return Impl::VarKind::Other;
    };

    // Create merge block.
    auto* endBB = llvm::BasicBlock::Create(*impl_->context, "match.end", func);

    // ---------------------------------------------------------------
    // Helper: recursively emit pattern-match test on |val| of type
    // |valTy|. Returns an i1 (true = match).
    // ---------------------------------------------------------------
    std::function<llvm::Value*(llvm::Value*, llvm::Type*, const MatchPattern&)>
    emitPatternMatch = [&](llvm::Value* val, llvm::Type* valTy,
                           const MatchPattern& pat) -> llvm::Value* {
        using Kind = MatchPattern::Kind;

        switch (pat.kind) {
        // -- Wildcard: always matches ------------------------------------
        case Kind::Wildcard:
            return llvm::ConstantInt::get(impl_->i1Type, 1);

        // -- Capture: always matches, bind variable ----------------------
        case Kind::Capture: {
            // Determine VarKind from the LLVM type of the captured value.
            // For match capture, use StrLiteral (safe - no decref) since the
            // captured string's provenance is unknown at this point.
            Impl::VarKind kind = Impl::VarKind::Other;
            if (valTy == impl_->i64Type)     kind = Impl::VarKind::Int;
            else if (valTy == impl_->f64Type) kind = Impl::VarKind::Float;
            else if (valTy == impl_->i1Type)  kind = Impl::VarKind::Bool;
            else if (valTy == impl_->i8PtrType) kind = Impl::VarKind::StrLiteral;

            auto* alloca = impl_->createEntryAlloca(func, pat.name, valTy);
            impl_->builder->CreateStore(val, alloca);
            impl_->setVar(pat.name, alloca, kind);
            return llvm::ConstantInt::get(impl_->i1Type, 1);
        }

        // -- Literal: compare subject with a constant --------------------
        case Kind::Literal: {
            if (!pat.literal) return llvm::ConstantInt::get(impl_->i1Type, 0);

            // Evaluate the literal expression.
            pat.literal->accept(*this);
            llvm::Value* litVal = impl_->lastValue;

            // Boxed subject (Union / Any): the literal matches iff the box tag
            // is the literal's type AND the unboxed payload equals it. Without
            // this the comparison runs against the {tag,payload} struct and
            // silently never matches (so `match v: int|str { case 0 {...} }`
            // would skip the `0` arm). int/float/bool payload compares are
            // arithmetic (safe even when the tag differs, since the result is
            // AND-ed with the tag check); the string compare derefs, so it is
            // guarded behind the tag check.
            if (valTy == impl_->boxType) {
                auto* boxTagV = impl_->boxTag(val, "lit.tag");
                if (dynamic_cast<NoneLiteral*>(pat.literal.get()))
                    return impl_->builder->CreateICmpEQ(
                        boxTagV, llvm::ConstantInt::get(impl_->i64Type, 4),
                        "lit.none");  // TAG_NONE
                int64_t litTag = 0;            // TAG_INT
                Impl::VarKind payKind = Impl::VarKind::Int;
                if (litVal->getType() == impl_->i8PtrType) {
                    litTag = 1; payKind = Impl::VarKind::Str;       // TAG_STR
                } else if (litVal->getType() == impl_->f64Type) {
                    litTag = 2; payKind = Impl::VarKind::Float;     // TAG_FLOAT
                } else if (litVal->getType() == impl_->i1Type) {
                    litTag = 3; payKind = Impl::VarKind::Bool;      // TAG_BOOL
                }
                auto* tagEq = impl_->builder->CreateICmpEQ(
                    boxTagV, llvm::ConstantInt::get(impl_->i64Type, litTag),
                    "lit.tageq");
                llvm::Value* payload = impl_->boxPayloadAsKind(val, payKind);
                if (litTag == 1) {
                    // Guard the string deref behind the tag check.
                    auto* entryBB = impl_->builder->GetInsertBlock();
                    auto* cmpBB = llvm::BasicBlock::Create(*impl_->context, "lit.str.cmp", func);
                    auto* doneBB = llvm::BasicBlock::Create(*impl_->context, "lit.str.done", func);
                    impl_->builder->CreateCondBr(tagEq, cmpBB, doneBB);
                    impl_->builder->SetInsertPoint(cmpBB);
                    auto* eq = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_str_eq"], {payload, litVal}, "lit.streq");
                    auto* streq = impl_->builder->CreateICmpNE(
                        eq, llvm::ConstantInt::get(impl_->i64Type, 0), "lit.streqb");
                    impl_->builder->CreateBr(doneBB);
                    auto* cmpEnd = impl_->builder->GetInsertBlock();
                    impl_->builder->SetInsertPoint(doneBB);
                    auto* phi = impl_->builder->CreatePHI(impl_->i1Type, 2, "lit.str.phi");
                    phi->addIncoming(llvm::ConstantInt::get(impl_->i1Type, 0), entryBB);
                    phi->addIncoming(streq, cmpEnd);
                    return phi;
                }
                llvm::Value* valEq;
                if (litTag == 2)
                    valEq = impl_->builder->CreateFCmpOEQ(payload, litVal, "lit.feq");
                else if (litTag == 3)
                    valEq = impl_->builder->CreateICmpEQ(payload, litVal, "lit.beq");
                else
                    valEq = impl_->builder->CreateICmpEQ(payload, litVal, "lit.ieq");
                return impl_->builder->CreateAnd(tagEq, valEq, "lit.match");
            }

            // None check: subject is a null pointer.
            if (dynamic_cast<NoneLiteral*>(pat.literal.get())) {
                return impl_->builder->CreateIsNull(val, "match.none");
            }

            // String comparison via runtime.
            if (valTy == impl_->i8PtrType && litVal->getType() == impl_->i8PtrType) {
                auto* eq = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_str_eq"], {val, litVal}, "match.streq");
                return impl_->builder->CreateICmpNE(
                    eq, llvm::ConstantInt::get(impl_->i64Type, 0), "match.streq.bool");
            }

            // Bool comparison (i1).
            if (valTy == impl_->i1Type && litVal->getType() == impl_->i1Type) {
                return impl_->builder->CreateICmpEQ(val, litVal, "match.booleq");
            }

            // Float comparison.
            if (valTy == impl_->f64Type && litVal->getType() == impl_->f64Type) {
                return impl_->builder->CreateFCmpOEQ(val, litVal, "match.floateq");
            }

            // Integer comparison (i64). Also widen bool->i64 or float->i64
            // if the subject/literal types don't line up.
            llvm::Value* lhs = val;
            llvm::Value* rhs = litVal;
            if (lhs->getType() == impl_->i1Type)
                lhs = impl_->builder->CreateZExt(lhs, impl_->i64Type);
            if (rhs->getType() == impl_->i1Type)
                rhs = impl_->builder->CreateZExt(rhs, impl_->i64Type);
            if (lhs->getType() == impl_->f64Type)
                lhs = impl_->builder->CreateFPToSI(lhs, impl_->i64Type);
            if (rhs->getType() == impl_->f64Type)
                rhs = impl_->builder->CreateFPToSI(rhs, impl_->i64Type);
            if (lhs->getType() == impl_->i64Type && rhs->getType() == impl_->i64Type)
                return impl_->builder->CreateICmpEQ(lhs, rhs, "match.inteq");

            // Fallback: no match (type mismatch at codegen time).
            return llvm::ConstantInt::get(impl_->i1Type, 0);
        }

        // -- Value: evaluate dotted-name expr, compare with subject ------
        case Kind::Value: {
            if (!pat.literal) return llvm::ConstantInt::get(impl_->i1Type, 0);

            pat.literal->accept(*this);
            llvm::Value* patVal = impl_->lastValue;

            // String comparison via runtime.
            if (valTy == impl_->i8PtrType && patVal->getType() == impl_->i8PtrType) {
                auto* eq = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_str_eq"], {val, patVal}, "match.valeq");
                return impl_->builder->CreateICmpNE(
                    eq, llvm::ConstantInt::get(impl_->i64Type, 0), "match.valeq.bool");
            }

            // Integer comparison.
            llvm::Value* lhs = val;
            llvm::Value* rhs = patVal;
            if (lhs->getType() == impl_->i1Type)
                lhs = impl_->builder->CreateZExt(lhs, impl_->i64Type);
            if (rhs->getType() == impl_->i1Type)
                rhs = impl_->builder->CreateZExt(rhs, impl_->i64Type);
            if (lhs->getType() == impl_->i64Type && rhs->getType() == impl_->i64Type)
                return impl_->builder->CreateICmpEQ(lhs, rhs, "match.valeq");

            return llvm::ConstantInt::get(impl_->i1Type, 0);
        }

        // -- Sequence: structural match against tuple elements -----------
        case Kind::Sequence: {
            // val must be a ptr (tuple). Check length first.
            auto* tupleLen = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_tuple_len"], {val}, "match.tuplen");
            auto* expectedLen = llvm::ConstantInt::get(
                impl_->i64Type, static_cast<int64_t>(pat.subPatterns.size()));
            auto* lenOk = impl_->builder->CreateICmpEQ(
                tupleLen, expectedLen, "match.lencheck");

            // Short-circuit: if length doesn't match, skip element checks.
            auto* elemCheckBB = llvm::BasicBlock::Create(
                *impl_->context, "match.seq.elem", func);
            auto* seqFailBB = llvm::BasicBlock::Create(
                *impl_->context, "match.seq.fail", func);
            auto* seqDoneBB = llvm::BasicBlock::Create(
                *impl_->context, "match.seq.done", func);

            impl_->builder->CreateCondBr(lenOk, elemCheckBB, seqFailBB);

            // Element matching block.
            impl_->builder->SetInsertPoint(elemCheckBB);
            llvm::Value* allMatch = llvm::ConstantInt::get(impl_->i1Type, 1);
            for (size_t i = 0; i < pat.subPatterns.size(); ++i) {
                auto* idx = llvm::ConstantInt::get(impl_->i64Type, static_cast<int64_t>(i));
                auto* elem = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_tuple_get"], {val, idx}, "match.elem");
                // dragon_tuple_get returns i64. For sub-pattern matching we
                // treat captured values as i64 (ints stored directly, ptrs via
                // inttoptr when needed later by user code).
                llvm::Value* elemMatch = emitPatternMatch(
                    elem, impl_->i64Type, pat.subPatterns[i]);
                allMatch = impl_->builder->CreateAnd(allMatch, elemMatch, "match.seq.and");
            }
            impl_->builder->CreateBr(seqDoneBB);
            // Save the block we ended up in (lambdas inside emitPatternMatch
            // may have created new blocks).
            auto* elemEndBB = impl_->builder->GetInsertBlock();

            // Fail block: length mismatch.
            impl_->builder->SetInsertPoint(seqFailBB);
            impl_->builder->CreateBr(seqDoneBB);

            // Merge with PHI.
            impl_->builder->SetInsertPoint(seqDoneBB);
            auto* phi = impl_->builder->CreatePHI(impl_->i1Type, 2, "match.seq.phi");
            phi->addIncoming(allMatch, elemEndBB);
            phi->addIncoming(llvm::ConstantInt::get(impl_->i1Type, 0), seqFailBB);
            return phi;
        }

        // -- Or: succeed if any alternative matches ----------------------
        case Kind::Or: {
            if (pat.subPatterns.empty())
                return llvm::ConstantInt::get(impl_->i1Type, 0);

            // Chain of short-circuit OR: try each sub-pattern.
            auto* orDoneBB = llvm::BasicBlock::Create(
                *impl_->context, "match.or.done", func);

            // We'll collect incoming edges for the final PHI.
            std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> incoming;

            for (size_t i = 0; i < pat.subPatterns.size(); ++i) {
                llvm::Value* subMatch = emitPatternMatch(
                    val, valTy, pat.subPatterns[i]);
                auto* currentBB = impl_->builder->GetInsertBlock();

                if (i + 1 < pat.subPatterns.size()) {
                    // Not the last alternative: branch to done on success,
                    // otherwise try next.
                    auto* nextBB = llvm::BasicBlock::Create(
                        *impl_->context, "match.or.next", func);
                    impl_->builder->CreateCondBr(subMatch, orDoneBB, nextBB);
                    incoming.push_back({llvm::ConstantInt::get(impl_->i1Type, 1), currentBB});
                    impl_->builder->SetInsertPoint(nextBB);
                } else {
                    // Last alternative: branch unconditionally to done.
                    impl_->builder->CreateBr(orDoneBB);
                    incoming.push_back({subMatch, currentBB});
                }
            }

            impl_->builder->SetInsertPoint(orDoneBB);
            auto* phi = impl_->builder->CreatePHI(
                impl_->i1Type, static_cast<unsigned>(incoming.size()), "match.or.phi");
            for (auto& [v, bb] : incoming) {
                phi->addIncoming(v, bb);
            }
            return phi;
        }

        // -- Class pattern: type test `case TypeName()`, plus positional field
        //  destructuring `case TypeName(p0, p1, ...)` (handled at the end).
        case Kind::Class: {
            auto tagFor = [&](const std::string& tn) -> int64_t {
                if (tn == "int")   return 0;   // TAG_INT
                if (tn == "str")   return 1;   // TAG_STR
                if (tn == "float") return 2;   // TAG_FLOAT
                if (tn == "bool")  return 3;   // TAG_BOOL
                if (tn == "list")  return 5;   // TAG_LIST
                if (tn == "dict")  return 6;   // TAG_DICT
                if (tn == "bytes") return 7;   // TAG_BYTES
                if (impl_->classNames.count(tn)) return 7;  // TAG_CLASS
                return -1;
            };

            // Compute the type test (classTest) and, for a class match that
            // we will destructure, the instance pointer to load fields from.
            bool wantDestructure =
                !pat.subPatterns.empty() && impl_->classNames.count(pat.name) > 0;
            llvm::Value* classTest = nullptr;
            llvm::Value* instPtr = nullptr;

            if (valTy == impl_->boxType) {
                // Union/Any subject (16-byte box): runtime tag check - mirrors
                // isinstance's box-tag path. The payload of a TAG_CLASS box is
                // the instance pointer (used for destructuring).
                int64_t tag = tagFor(pat.name);
                if (tag < 0) {
                    classTest = llvm::ConstantInt::get(impl_->i1Type, 0);
                } else {
                    auto* tagVal = impl_->boxTag(val, "match.tag");
                    classTest = impl_->builder->CreateICmpEQ(
                        tagVal, llvm::ConstantInt::get(impl_->i64Type, tag),
                        "match.isinst");
                    if (wantDestructure)
                        instPtr = impl_->boxPayloadAsKind(val, Impl::VarKind::ClassInstance);
                }
            } else if (valTy == impl_->i64Type) {
                classTest = llvm::ConstantInt::get(impl_->i1Type, pat.name == "int" ? 1 : 0);
            } else if (valTy == impl_->f64Type) {
                classTest = llvm::ConstantInt::get(impl_->i1Type, pat.name == "float" ? 1 : 0);
            } else if (valTy == impl_->i1Type) {
                classTest = llvm::ConstantInt::get(impl_->i1Type, pat.name == "bool" ? 1 : 0);
            } else if (valTy->isPointerTy()) {
                // A class test on a ptr subject: the dynamic class is recovered
                // from the static type (no runtime tag on a class ptr). If
                // pat.name is in the subject's class chain, the match is a
                // runtime non-null check (a null - None in a `Class | None`
                // Optional - is not an instance). The ptr itself is the instance.
                if (impl_->classNames.count(pat.name)) {
                    std::string cur = subjectClassName;
                    bool inChain = false;
                    while (!cur.empty()) {
                        if (cur == pat.name) { inChain = true; break; }
                        auto pit = impl_->classParentNames.find(cur);
                        if (pit == impl_->classParentNames.end()) break;
                        cur = pit->second;
                    }
                    if (!inChain) {
                        classTest = llvm::ConstantInt::get(impl_->i1Type, 0);
                    } else {
                        classTest = impl_->builder->CreateIsNotNull(val, "match.isinst");
                        if (wantDestructure) instPtr = val;
                    }
                } else {
                    Type::Kind k = subjectStaticType ? subjectStaticType->kind()
                                                     : Type::Kind::Unknown;
                    bool isMatch =
                        (pat.name == "str"   && k == Type::Kind::Str)   ||
                        (pat.name == "list"  && k == Type::Kind::List)  ||
                        (pat.name == "dict"  && k == Type::Kind::Dict)  ||
                        (pat.name == "tuple" && k == Type::Kind::Tuple) ||
                        (pat.name == "bytes" && k == Type::Kind::Bytes);
                    classTest = llvm::ConstantInt::get(impl_->i1Type, isMatch ? 1 : 0);
                }
            } else {
                classTest = llvm::ConstantInt::get(impl_->i1Type, 0);
            }

            // Plain type test (no field destructuring) - return the test.
            if (!wantDestructure || !instPtr)
                return classTest;

            // -- Positional field destructuring `case T(p0, p1, ...)` --------
            // Build the full field order (ancestors first, then own), the same
            // list the TypeChecker used to type the captures. Then guard the
            // field loads behind classTest (short-circuit: never read fields of
            // a non-matching object) and AND each sub-pattern's match.
            std::vector<std::string> order;
            {
                std::vector<std::string> chain;
                std::string cur = pat.name;
                while (!cur.empty()) {
                    chain.push_back(cur);
                    auto pit = impl_->classParentNames.find(cur);
                    cur = (pit != impl_->classParentNames.end()) ? pit->second : "";
                }
                std::set<std::string> seen;
                for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
                    auto fo = impl_->classFieldOrder.find(*rit);
                    if (fo == impl_->classFieldOrder.end()) continue;
                    for (auto& f : fo->second)
                        if (seen.insert(f).second) order.push_back(f);
                }
            }

            auto* clsFieldsBB = llvm::BasicBlock::Create(*impl_->context, "match.cls.fields", func);
            auto* clsFailBB = llvm::BasicBlock::Create(*impl_->context, "match.cls.fail", func);
            auto* clsDoneBB = llvm::BasicBlock::Create(*impl_->context, "match.cls.done", func);
            impl_->builder->CreateCondBr(classTest, clsFieldsBB, clsFailBB);

            impl_->builder->SetInsertPoint(clsFieldsBB);
            auto* structTy = impl_->classStructTypes.count(pat.name)
                ? impl_->classStructTypes[pat.name] : nullptr;
            llvm::Value* allMatch = llvm::ConstantInt::get(impl_->i1Type, 1);
            for (size_t si = 0; si < pat.subPatterns.size() && si < order.size(); ++si) {
                auto idxIt = impl_->classFieldIndices[pat.name].find(order[si]);
                if (!structTy || idxIt == impl_->classFieldIndices[pat.name].end()) continue;
                auto* fTy = impl_->classFieldTypes[pat.name][order[si]];
                auto* gep = impl_->builder->CreateStructGEP(
                    structTy, instPtr, idxIt->second, "match.fld." + order[si]);
                auto* fVal = impl_->builder->CreateLoad(fTy, gep, order[si]);
                auto* subMatch = emitPatternMatch(fVal, fTy, pat.subPatterns[si]);
                allMatch = impl_->builder->CreateAnd(allMatch, subMatch, "match.cls.and");
            }
            impl_->builder->CreateBr(clsDoneBB);
            auto* clsFieldsEndBB = impl_->builder->GetInsertBlock();

            impl_->builder->SetInsertPoint(clsFailBB);
            impl_->builder->CreateBr(clsDoneBB);

            impl_->builder->SetInsertPoint(clsDoneBB);
            auto* clsPhi = impl_->builder->CreatePHI(impl_->i1Type, 2, "match.cls.phi");
            clsPhi->addIncoming(allMatch, clsFieldsEndBB);
            clsPhi->addIncoming(llvm::ConstantInt::get(impl_->i1Type, 0), clsFailBB);
            return clsPhi;
        }
        }

        // Unreachable, but satisfy compiler.
        return llvm::ConstantInt::get(impl_->i1Type, 0);
    };

    // ---------------------------------------------------------------
    // Emit case arms as a chain of test-and-branch blocks.
    // ---------------------------------------------------------------
    size_t numCases = node.cases.size();
    for (size_t i = 0; i < numCases; ++i) {
        auto& arm = node.cases[i];

        auto* testBB = llvm::BasicBlock::Create(
            *impl_->context, "match.case" + std::to_string(i) + ".test", func);
        auto* bodyBB = llvm::BasicBlock::Create(
            *impl_->context, "match.case" + std::to_string(i) + ".body", func);

        // Branch from previous block (or entry) into this test.
        impl_->builder->CreateBr(testBB);
        impl_->builder->SetInsertPoint(testBB);

        // Push a scope so capture bindings are visible in the body and
        // guard, but don't leak to subsequent arms.
        impl_->pushScope();

        // Reload the subject from its alloca (IR is SSA - each block needs
        // its own load).
        llvm::Value* subject = impl_->builder->CreateLoad(subjectTy, subjectAlloca, "match.subj");

        // Emit the pattern match condition.
        llvm::Value* matched = emitPatternMatch(subject, subjectTy, arm.pattern);

        // Evaluate optional guard: case pattern if guard_expr.
        // The guard is only evaluated when the pattern matches.
        //
        // We track a "fallthroughBB": the block where control resumes
        // when this arm does NOT match. After emitting the body we
        // restore the insert point to fallthroughBB so the next
        // iteration's CreateBr(testBB) chains correctly.
        llvm::BasicBlock* fallthroughBB = nullptr;

        if (arm.guard) {
            // Create a block for guard evaluation and one for "guard failed".
            auto* guardBB = llvm::BasicBlock::Create(
                *impl_->context, "match.case" + std::to_string(i) + ".guard", func);
            auto* guardFailBB = llvm::BasicBlock::Create(
                *impl_->context, "match.case" + std::to_string(i) + ".gfail", func);

            impl_->builder->CreateCondBr(matched, guardBB, guardFailBB);

            // Guard evaluation block.
            impl_->builder->SetInsertPoint(guardBB);
            arm.guard->accept(*this);
            llvm::Value* guardVal = impl_->lastValue;
            if (guardVal->getType() == impl_->i64Type) {
                guardVal = impl_->builder->CreateICmpNE(
                    guardVal, llvm::ConstantInt::get(impl_->i64Type, 0));
            } else if (guardVal->getType() == impl_->f64Type) {
                guardVal = impl_->builder->CreateFCmpONE(
                    guardVal, llvm::ConstantFP::get(impl_->f64Type, 0.0));
            }
            impl_->builder->CreateCondBr(guardVal, bodyBB, guardFailBB);

            // guardFailBB is the fallthrough for the next case.
            fallthroughBB = guardFailBB;
        } else {
            // No guard: branch directly on pattern match.
            auto* fallBB = llvm::BasicBlock::Create(
                *impl_->context, "match.case" + std::to_string(i) + ".fall", func);
            impl_->builder->CreateCondBr(matched, bodyBB, fallBB);
            fallthroughBB = fallBB;
        }

        // Emit the case body.
        impl_->builder->SetInsertPoint(bodyBB);
        // #1 narrowing: extract the box payload at the matched native type and
        // shadow the bare-name subject for this arm's scope (mirrors the
        // isinstance narrowing in visit(IfStmt)). Borrowed - the payload lives
        // in the box, so scope cleanup must not decref it.
        if (!subjectName.empty() && subjectTy == impl_->boxType &&
            arm.pattern.kind == MatchPattern::Kind::Class &&
            arm.pattern.subPatterns.empty()) {
            Impl::VarKind nk = scalarNarrowKind(arm.pattern.name);
            if (nk != Impl::VarKind::Other) {
                auto* box = impl_->builder->CreateLoad(
                    impl_->boxType, subjectAlloca, "match.box.narrow");
                llvm::Value* payload = impl_->boxPayloadAsKind(box, nk);
                auto* na = impl_->createEntryAlloca(
                    func, subjectName + ".narrowed", payload->getType());
                impl_->builder->CreateStore(payload, na);
                impl_->setVar(subjectName, na, nk);
                impl_->scopes.back().borrowed.insert(subjectName);
            }
        }
        for (auto& stmt : arm.body) {
            stmt->accept(*this);
        }
        if (!impl_->builder->GetInsertBlock()->getTerminator()) {
            // Decref any heap-typed capture bindings the pattern bound into
            // this arm's scope before falling through to match.end.
            impl_->emitScopeCleanup();
            impl_->builder->CreateBr(endBB);
        }

        // Restore insert point to the fallthrough block. If this is
        // the last case, terminate it with a branch to match.end.
        // Otherwise, leave it unterminated so the next iteration can
        // chain it to the next case's test block.
        impl_->builder->SetInsertPoint(fallthroughBB);
        if (i + 1 >= numCases) {
            impl_->builder->CreateBr(endBB);
        }

        impl_->popScope();
    }

    // If there were no cases at all, we still need to branch to endBB.
    if (numCases == 0) {
        impl_->builder->CreateBr(endBB);
    }

    impl_->builder->SetInsertPoint(endBB);
}

} // namespace dragon
