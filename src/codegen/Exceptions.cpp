#include "../CodeGenImpl.h"

namespace dragon {

void CodeGen::visit(AssertStmt& node) {
    node.test->accept(*this);
    llvm::Value* cond = impl_->lastValue;
    if (cond->getType() == impl_->i64Type) {
    } else if (cond->getType() == impl_->i1Type) {
        cond = impl_->builder->CreateZExt(cond, impl_->i64Type);
    }

    if (node.msg) {
        node.msg->accept(*this);
        llvm::Value* msgVal = impl_->lastValue;
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_assert"], {cond, msgVal});
        if (impl_->options.gcMode == GCMode::RC &&
            impl_->isOwnedStrResult(msgVal))
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_decref_str"], {msgVal});
    } else {
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_assert_no_msg"], {cond});
    }
}

void CodeGen::visit(TryStmt& node) {
    auto* func = impl_->currentFunction;
    int excId = impl_->excCounter++;
    std::string prefix = "try" + std::to_string(excId);

    auto* tryBodyBB  = llvm::BasicBlock::Create(*impl_->context, prefix + ".body", func);
    auto* dispatchBB = llvm::BasicBlock::Create(*impl_->context, prefix + ".dispatch", func);

    struct HandlerInfo {
        llvm::BasicBlock* checkBB = nullptr;
        llvm::BasicBlock* bodyBB  = nullptr;
        int64_t typeCode = 0;
        std::vector<int64_t> altCodes;
    };
    std::vector<HandlerInfo> handlerInfos;
    bool hasCatchAll = false;

    for (size_t i = 0; i < node.handlers.size(); ++i) {
        HandlerInfo hi;
        hi.bodyBB = llvm::BasicBlock::Create(*impl_->context,
            prefix + ".handler." + std::to_string(i), func);

        auto& handler = node.handlers[i];
        if (handler.type) {
            hi.typeCode = 10;
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

    llvm::BasicBlock* unmatchedBB = nullptr;
    if (!hasCatchAll && !node.handlers.empty()) {
        unmatchedBB = llvm::BasicBlock::Create(*impl_->context,
            prefix + ".unmatched", func);
    }

    llvm::BasicBlock* elseBB = nullptr;
    if (!node.elseBody.empty()) {
        elseBB = llvm::BasicBlock::Create(*impl_->context, prefix + ".else", func);
    }

    llvm::BasicBlock* finallyBB = nullptr;
    if (!node.finallyBody.empty()) {
        finallyBB = llvm::BasicBlock::Create(*impl_->context, prefix + ".finally", func);
    }

    auto* endBB = llvm::BasicBlock::Create(*impl_->context, prefix + ".end", func);

    auto* reraiseFlag = impl_->createEntryAlloca(func, prefix + ".rr.flag", impl_->i1Type);
    auto* savedType = impl_->createEntryAlloca(func, prefix + ".rr.type", impl_->i64Type);
    auto* savedObj  = impl_->createEntryAlloca(func, prefix + ".rr.obj", impl_->i8PtrType);
    auto* savedMsg  = impl_->createEntryAlloca(func, prefix + ".rr.msg", impl_->i8PtrType);
    impl_->builder->CreateStore(llvm::ConstantInt::getFalse(*impl_->context), reraiseFlag);

    auto* reraiseCheckBB = llvm::BasicBlock::Create(*impl_->context, prefix + ".rrcheck", func);
    auto* doReraiseBB = llvm::BasicBlock::Create(*impl_->context, prefix + ".reraise", func);

    llvm::BasicBlock* afterHandlerBB = finallyBB ? finallyBB : reraiseCheckBB;
    llvm::BasicBlock* afterTryBodyBB = elseBB ? elseBB : afterHandlerBB;

    auto* jmpbufPtr = impl_->builder->CreateCall(
        impl_->runtimeFuncs["dragon_exc_push_frame"], {}, "jmpbuf");
    auto* setjmpResult = impl_->builder->CreateCall(
        impl_->runtimeFuncs["setjmp"], {jmpbufPtr}, "setjmp.result");
    auto* isNormal = impl_->builder->CreateICmpEQ(
        setjmpResult,
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(*impl_->context), 0),
        "is.normal");
    impl_->builder->CreateCondBr(isNormal, tryBodyBB, dispatchBB);

    if (!node.finallyBody.empty()) {
        Impl::ExitCleanup ec;
        ec.isWith = false;
        ec.func = impl_->currentFunction;
        ec.scopeDepth = impl_->scopes.size();
        for (auto& s : node.finallyBody) ec.finallyBody.push_back(s.get());
        impl_->exitCleanupStack.push_back(std::move(ec));
    }

    impl_->builder->SetInsertPoint(tryBodyBB);
    impl_->tryFrameFuncs.push_back(func);
    // The try body is its own lexical scope: its owned heap locals are freed EITHER by codegen (normal completion)
    // OR by dragon_exc_cleanup_unwind (longjmp), never both - in the enclosing scope they'd double-free on the caught path.
    impl_->pushScope();
    for (auto& stmt : node.tryBody) stmt->accept(*this);
    bool tryTerminated = impl_->builder->GetInsertBlock()->getTerminator() != nullptr;
    if (!tryTerminated) {
        impl_->emitScopeCleanup();
    }
    impl_->popScope();
    impl_->tryFrameFuncs.pop_back();
    if (!tryTerminated) {
        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_pop_frame"], {});
        impl_->builder->CreateBr(afterTryBodyBB);
    }

    impl_->builder->SetInsertPoint(dispatchBB);
    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_cleanup_unwind"], {});
    impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_pop_frame"], {});
    auto* excType = impl_->builder->CreateCall(
        impl_->runtimeFuncs["dragon_exc_get_type"], {}, "exc.type");

    if (!handlerInfos.empty()) {
        if (handlerInfos[0].checkBB) {
            impl_->builder->CreateBr(handlerInfos[0].checkBB);
        } else {
            impl_->builder->CreateBr(handlerInfos[0].bodyBB);
        }
    } else {
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

    for (size_t i = 0; i < handlerInfos.size(); ++i) {
        auto& hi = handlerInfos[i];
        auto& handler = node.handlers[i];

        if (hi.checkBB) {
            impl_->builder->SetInsertPoint(hi.checkBB);
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

        impl_->builder->SetInsertPoint(hi.bodyBB);
        impl_->pushScope();

        if (!handler.name.empty()) {
            bool boundInstance = false;
            if (auto* named = dynamic_cast<NamedTypeExpr*>(handler.type.get())) {
                if (impl_->userExcCodesBySym.count(impl_->classSym(named->name)) > 0 &&
                    impl_->classNames.count(named->name)) {
                    auto* obj = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_exc_bind_obj"], {}, "exc.obj");
                    auto* alloca = impl_->createEntryAlloca(
                        func, handler.name, impl_->i8PtrType);
                    impl_->builder->CreateStore(obj, alloca);
                    impl_->setVar(handler.name, alloca, Impl::VarKind::ClassInstance);
                    impl_->varClassNames[handler.name] = named->name;
                    impl_->emitCleanupPush(handler.name, obj, Impl::DCLEAN_OBJ);
                    boundInstance = true;
                }
            }
            if (!boundInstance) {
                auto* msg = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_exc_bind_msg"], {}, "exc.msg");
                auto* alloca = impl_->createEntryAlloca(
                    func, handler.name, impl_->i8PtrType);
                impl_->builder->CreateStore(msg, alloca);
                impl_->setVar(handler.name, alloca, Impl::VarKind::Str);
                impl_->emitCleanupPush(handler.name, msg, Impl::DCLEAN_STR);
            }
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

    if (unmatchedBB) {
        impl_->builder->SetInsertPoint(unmatchedBB);
        auto* reType = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_exc_get_type"], {}, "reraise.type");
        auto* reObj = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_exc_retain_obj"],
            {impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_exc_get_obj"], {}, "reraise.obj.raw")},
            "reraise.obj");
        auto* reMsg = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_exc_bind_msg"], {}, "reraise.msg");
        impl_->builder->CreateStore(reType, savedType);
        impl_->builder->CreateStore(reObj, savedObj);
        impl_->builder->CreateStore(reMsg, savedMsg);
        impl_->builder->CreateStore(llvm::ConstantInt::getTrue(*impl_->context), reraiseFlag);
        impl_->builder->CreateBr(afterHandlerBB);
    }

    if (elseBB) {
        impl_->builder->SetInsertPoint(elseBB);
        for (auto& stmt : node.elseBody) stmt->accept(*this);
        if (!impl_->builder->GetInsertBlock()->getTerminator())
            impl_->builder->CreateBr(afterHandlerBB);
    }

    if (finallyBB) {
        impl_->builder->SetInsertPoint(finallyBB);
        for (auto& stmt : node.finallyBody) stmt->accept(*this);
        if (!impl_->builder->GetInsertBlock()->getTerminator())
            impl_->builder->CreateBr(reraiseCheckBB);
    }

    if (!node.finallyBody.empty()) {
        impl_->exitCleanupStack.pop_back();
    }

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
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_raise_exc_obj_consume"], {t, o, m});
        impl_->builder->CreateUnreachable();
    }

    impl_->builder->SetInsertPoint(endBB);
}

void CodeGen::visit(WithStmt& node) {
    struct CtxInfo {
        llvm::Value* val;
        bool isClassCtx;
        bool isLock;
        std::string className;
        llvm::Value* enterResult = nullptr;
        bool isLockTemp = false;
        bool subjectOwned = true;
                                   // decref `val` - doing so was an A/B-proven UAF (test_d045_privacy / test_rc_with_subject.dr).
        llvm::Function* exitFn = nullptr;
    };
    std::vector<CtxInfo> contextHandles;

    for (auto& item : node.items) {
        std::string ctxClassName = impl_->resolveExprClassName(item.contextExpr.get());
        if (ctxClassName.empty() && item.contextExpr->type) {
            if (auto inst = std::dynamic_pointer_cast<InstanceType>(item.contextExpr->type))
                if (inst->classType && impl_->classNames.count(inst->classType->name))
                    ctxClassName = inst->classType->name;
        }
        const ClassType* ctxCT = nullptr;
        if (item.contextExpr->type)
            if (auto inst = std::dynamic_pointer_cast<InstanceType>(item.contextExpr->type))
                if (inst->classType && inst->classType->name == ctxClassName)
                    ctxCT = inst->classType.get();
        llvm::Function* enterFn =
            ctxCT ? impl_->methodFromClassType(ctxCT, "__enter__") : nullptr;
        llvm::Function* exitFn =
            ctxCT ? impl_->methodFromClassType(ctxCT, "__exit__") : nullptr;

        bool isLockCtx = false;
        bool isLockTemp = false;
        if (impl_->isLockExpr(item.contextExpr.get())) {
            isLockCtx = true;
        } else if (auto* ce = dynamic_cast<CallExpr*>(item.contextExpr.get())) {
            if (auto* cn = dynamic_cast<NameExpr*>(ce->callee.get()))
                if (cn->name == "Lock") { isLockCtx = true; isLockTemp = true; }
        }

        item.contextExpr->accept(*this);
        llvm::Value* ctxVal = impl_->lastValue;
        llvm::Value* enterResultV = nullptr;
        bool subjectOwned = !Impl::isBorrowedHeapExpr(item.contextExpr.get());

        bool isClassCtx = !isLockCtx && !ctxClassName.empty() &&
            (ctxCT ? (enterFn != nullptr && exitFn != nullptr)
                   : (impl_->hasDunder(ctxClassName, "__enter__") &&
                      impl_->hasDunder(ctxClassName, "__exit__"))) &&
            (ctxVal->getType() == impl_->i8PtrType || ctxVal->getType()->isPointerTy());

        if (isLockCtx) {
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
            enterResultV = enterFn
                ? impl_->emitDunderCall(enterFn, "__enter__", ctxVal)
                : impl_->callDunder(ctxClassName, "__enter__", ctxVal);
            if (!enterResultV) {
                impl_->addError("internal error: cannot resolve __enter__ on class '" +
                                ctxClassName + "' (two classes may share the name)",
                                node.location());
                enterResultV = ctxVal;
            }

            if (item.optionalVars) {
                if (auto* nameExpr = dynamic_cast<NameExpr*>(item.optionalVars.get())) {
                    auto* alloca = impl_->createEntryAlloca(
                        impl_->currentFunction, nameExpr->name, enterResultV->getType());
                    impl_->builder->CreateStore(enterResultV, alloca);
                    impl_->setVar(nameExpr->name, alloca);
                    impl_->varClassNames[nameExpr->name] = ctxClassName;
                    if (ctxCT)
                        impl_->varClassOwningModule[nameExpr->name] = ctxCT->definingModule;
                }
            }
        } else {
            if (item.optionalVars) {
                if (auto* nameExpr = dynamic_cast<NameExpr*>(item.optionalVars.get())) {
                    auto* alloca = impl_->createEntryAlloca(
                        impl_->currentFunction, nameExpr->name, ctxVal->getType());
                    impl_->builder->CreateStore(ctxVal, alloca);
                    impl_->setVar(nameExpr->name, alloca, Impl::VarKind::Other);
                }
            }
        }
        contextHandles.push_back({ctxVal, isClassCtx, isLockCtx, ctxClassName, enterResultV, isLockTemp, subjectOwned, exitFn});
    }

    bool needsExcSafe = false;
    for (auto& ci : contextHandles) {
        if (ci.isClassCtx || ci.isLock) { needsExcSafe = true; break; }
    }

    if (needsExcSafe) {
        auto* func = impl_->currentFunction;
        auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, "with.body", func);
        auto* excBB = llvm::BasicBlock::Create(*impl_->context, "with.exc", func);
        auto* cleanupBB = llvm::BasicBlock::Create(*impl_->context, "with.cleanup", func);
        auto* endBB = llvm::BasicBlock::Create(*impl_->context, "with.end", func);

        auto* jmpbufPtr = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_exc_push_frame"], {}, "jmpbuf");
        auto* setjmpResult = impl_->builder->CreateCall(
            impl_->runtimeFuncs["setjmp"], {jmpbufPtr}, "setjmp.result");
        auto* isNormal = impl_->builder->CreateICmpEQ(
            setjmpResult,
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*impl_->context), 0),
            "is.normal");
        impl_->builder->CreateCondBr(isNormal, bodyBB, excBB);

        impl_->builder->SetInsertPoint(bodyBB);
        impl_->tryFrameFuncs.push_back(func);
        {
            Impl::ExitCleanup ec;
            ec.isWith = true;
            ec.func = func;
            ec.scopeDepth = impl_->scopes.size();
            for (auto& ci : contextHandles)
                ec.withItems.push_back({ci.isClassCtx, ci.isLock, ci.className, ci.val, ci.enterResult, ci.exitFn, ci.isLockTemp, ci.subjectOwned});
            impl_->exitCleanupStack.push_back(std::move(ec));
        }
        impl_->pushScope();
        for (auto& stmt : node.body) stmt->accept(*this);
        if (!impl_->builder->GetInsertBlock()->getTerminator())
            impl_->emitScopeCleanup();
        impl_->popScope();
        impl_->exitCleanupStack.pop_back();
        impl_->tryFrameFuncs.pop_back();
        if (!impl_->builder->GetInsertBlock()->getTerminator()) {
            impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_pop_frame"], {});
            impl_->builder->CreateBr(cleanupBB);
        }

        impl_->builder->SetInsertPoint(excBB);
        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_cleanup_unwind"], {});
        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_exc_pop_frame"], {});
        for (auto& ci : contextHandles) {
            if (ci.isClassCtx) {
                if (ci.exitFn) impl_->emitDunderCall(ci.exitFn, "__exit__", ci.val);
                else impl_->callDunder(ci.className, "__exit__", ci.val);
                if (impl_->options.gcMode == GCMode::RC) {
                    if (ci.subjectOwned)
                        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {ci.val});
                    if (ci.enterResult && ci.enterResult->getType()->isPointerTy())
                        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {ci.enterResult});
                }
            } else if (ci.isLock) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_lock_release"], {ci.val});
                if (ci.isLockTemp)
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_lock_destroy"], {ci.val});
            }
        }
        {
            auto* reType = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_exc_get_type"], {}, "reraise.type");
            auto* reObj = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_exc_retain_obj"],
                {impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_exc_get_obj"], {},
                    "reraise.obj.raw")},
                "reraise.obj");
            auto* reMsg = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_exc_get_msg"], {}, "reraise.msg");
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_raise_exc_obj"], {reType, reObj, reMsg});
        }
        impl_->builder->CreateUnreachable();

        impl_->builder->SetInsertPoint(cleanupBB);
        for (auto& ci : contextHandles) {
            if (ci.isClassCtx) {
                if (ci.exitFn) impl_->emitDunderCall(ci.exitFn, "__exit__", ci.val);
                else impl_->callDunder(ci.className, "__exit__", ci.val);
                if (impl_->options.gcMode == GCMode::RC) {
                    if (ci.subjectOwned)
                        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {ci.val});
                    if (ci.enterResult && ci.enterResult->getType()->isPointerTy())
                        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_decref"], {ci.enterResult});
                }
            } else if (ci.isLock) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_lock_release"], {ci.val});
                if (ci.isLockTemp)
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_lock_destroy"], {ci.val});
            }
        }
        impl_->builder->CreateBr(endBB);

        impl_->builder->SetInsertPoint(endBB);
    } else {
        impl_->pushScope();
        for (auto& stmt : node.body) stmt->accept(*this);
        if (!impl_->builder->GetInsertBlock()->getTerminator())
            impl_->emitScopeCleanup();
        impl_->popScope();
    }
}

void CodeGen::visit(MatchStmt& node) {
    auto* func = impl_->currentFunction;

    node.subject->accept(*this);
    llvm::Value* subjectVal = impl_->lastValue;
    llvm::Type* subjectTy = subjectVal->getType();

    auto* subjectAlloca = impl_->createEntryAlloca(func, "match.subject", subjectTy);
    impl_->builder->CreateStore(subjectVal, subjectAlloca);

    std::shared_ptr<Type> subjectStaticType =
        node.subject ? node.subject->type : nullptr;
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

    auto* subjectNameExpr = dynamic_cast<NameExpr*>(node.subject.get());
    std::string subjectName = subjectNameExpr ? subjectNameExpr->name : "";
    auto scalarNarrowKind = [&](const std::string& tn) -> Impl::VarKind {
        if (tn == "int")   return Impl::VarKind::Int;
        if (tn == "float") return Impl::VarKind::Float;
        if (tn == "bool")  return Impl::VarKind::Bool;
        if (tn == "str")   return Impl::VarKind::Str;
        return Impl::VarKind::Other;
    };

    auto* endBB = llvm::BasicBlock::Create(*impl_->context, "match.end", func);

    std::function<llvm::Value*(llvm::Value*, llvm::Type*, const MatchPattern&)>
    emitPatternMatch = [&](llvm::Value* val, llvm::Type* valTy,
                           const MatchPattern& pat) -> llvm::Value* {
        using Kind = MatchPattern::Kind;

        switch (pat.kind) {
        case Kind::Wildcard:
            return llvm::ConstantInt::get(impl_->i1Type, 1);

        case Kind::Capture: {
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

        case Kind::Literal: {
            if (!pat.literal) return llvm::ConstantInt::get(impl_->i1Type, 0);

            pat.literal->accept(*this);
            llvm::Value* litVal = impl_->lastValue;

            if (valTy == impl_->boxType) {
                auto* boxTagV = impl_->boxTag(val, "lit.tag");
                if (dynamic_cast<NoneLiteral*>(pat.literal.get()))
                    return impl_->builder->CreateICmpEQ(
                        boxTagV, llvm::ConstantInt::get(impl_->i64Type, 4),
                        "lit.none");
                int64_t litTag = TAG_INT;
                Impl::VarKind payKind = Impl::VarKind::Int;
                if (litVal->getType() == impl_->i8PtrType) {
                    litTag = TAG_STR; payKind = Impl::VarKind::Str;
                } else if (litVal->getType() == impl_->f64Type) {
                    litTag = TAG_FLOAT; payKind = Impl::VarKind::Float;
                } else if (litVal->getType() == impl_->i1Type) {
                    litTag = TAG_BOOL; payKind = Impl::VarKind::Bool;
                }
                auto* tagEq = impl_->builder->CreateICmpEQ(
                    boxTagV, llvm::ConstantInt::get(impl_->i64Type, litTag),
                    "lit.tageq");
                llvm::Value* payload = impl_->boxPayloadAsKind(val, payKind);
                if (litTag == 1) {
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

            if (dynamic_cast<NoneLiteral*>(pat.literal.get())) {
                return impl_->builder->CreateIsNull(val, "match.none");
            }

            if (valTy == impl_->i8PtrType && litVal->getType() == impl_->i8PtrType) {
                auto* eq = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_str_eq"], {val, litVal}, "match.streq");
                return impl_->builder->CreateICmpNE(
                    eq, llvm::ConstantInt::get(impl_->i64Type, 0), "match.streq.bool");
            }

            if (valTy == impl_->i1Type && litVal->getType() == impl_->i1Type) {
                return impl_->builder->CreateICmpEQ(val, litVal, "match.booleq");
            }

            if (valTy == impl_->f64Type && litVal->getType() == impl_->f64Type) {
                return impl_->builder->CreateFCmpOEQ(val, litVal, "match.floateq");
            }

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

            return llvm::ConstantInt::get(impl_->i1Type, 0);
        }

        case Kind::Value: {
            if (!pat.literal) return llvm::ConstantInt::get(impl_->i1Type, 0);

            pat.literal->accept(*this);
            llvm::Value* patVal = impl_->lastValue;

            if (valTy == impl_->i8PtrType && patVal->getType() == impl_->i8PtrType) {
                auto* eq = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_str_eq"], {val, patVal}, "match.valeq");
                return impl_->builder->CreateICmpNE(
                    eq, llvm::ConstantInt::get(impl_->i64Type, 0), "match.valeq.bool");
            }

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

        case Kind::Sequence: {
            auto* tupleLen = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_tuple_len"], {val}, "match.tuplen");
            auto* expectedLen = llvm::ConstantInt::get(
                impl_->i64Type, static_cast<int64_t>(pat.subPatterns.size()));
            auto* lenOk = impl_->builder->CreateICmpEQ(
                tupleLen, expectedLen, "match.lencheck");

            auto* elemCheckBB = llvm::BasicBlock::Create(
                *impl_->context, "match.seq.elem", func);
            auto* seqFailBB = llvm::BasicBlock::Create(
                *impl_->context, "match.seq.fail", func);
            auto* seqDoneBB = llvm::BasicBlock::Create(
                *impl_->context, "match.seq.done", func);

            impl_->builder->CreateCondBr(lenOk, elemCheckBB, seqFailBB);

            impl_->builder->SetInsertPoint(elemCheckBB);
            llvm::Value* allMatch = llvm::ConstantInt::get(impl_->i1Type, 1);
            for (size_t i = 0; i < pat.subPatterns.size(); ++i) {
                auto* idx = llvm::ConstantInt::get(impl_->i64Type, static_cast<int64_t>(i));
                auto* elem = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_tuple_get"], {val, idx}, "match.elem");
                llvm::Value* elemMatch = emitPatternMatch(
                    elem, impl_->i64Type, pat.subPatterns[i]);
                allMatch = impl_->builder->CreateAnd(allMatch, elemMatch, "match.seq.and");
            }
            impl_->builder->CreateBr(seqDoneBB);
            auto* elemEndBB = impl_->builder->GetInsertBlock();

            impl_->builder->SetInsertPoint(seqFailBB);
            impl_->builder->CreateBr(seqDoneBB);

            impl_->builder->SetInsertPoint(seqDoneBB);
            auto* phi = impl_->builder->CreatePHI(impl_->i1Type, 2, "match.seq.phi");
            phi->addIncoming(allMatch, elemEndBB);
            phi->addIncoming(llvm::ConstantInt::get(impl_->i1Type, 0), seqFailBB);
            return phi;
        }

        case Kind::Or: {
            if (pat.subPatterns.empty())
                return llvm::ConstantInt::get(impl_->i1Type, 0);

            auto* orDoneBB = llvm::BasicBlock::Create(
                *impl_->context, "match.or.done", func);

            std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> incoming;

            for (size_t i = 0; i < pat.subPatterns.size(); ++i) {
                llvm::Value* subMatch = emitPatternMatch(
                    val, valTy, pat.subPatterns[i]);
                auto* currentBB = impl_->builder->GetInsertBlock();

                if (i + 1 < pat.subPatterns.size()) {
                    auto* nextBB = llvm::BasicBlock::Create(
                        *impl_->context, "match.or.next", func);
                    impl_->builder->CreateCondBr(subMatch, orDoneBB, nextBB);
                    incoming.push_back({llvm::ConstantInt::get(impl_->i1Type, 1), currentBB});
                    impl_->builder->SetInsertPoint(nextBB);
                } else {
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

        case Kind::Class: {
            auto tagFor = [&](const std::string& tn) -> int64_t {
                if (tn == "int")   return TAG_INT;
                if (tn == "str")   return TAG_STR;
                if (tn == "float") return TAG_FLOAT;
                if (tn == "bool")  return TAG_BOOL;
                if (tn == "list")  return TAG_LIST;
                if (tn == "dict")  return TAG_DICT;
                if (tn == "bytes") return TAG_BYTES;
                if (impl_->classNames.count(tn)) return 7;
                return -1;
            };

            bool wantDestructure =
                !pat.subPatterns.empty() && impl_->classNames.count(pat.name) > 0;
            llvm::Value* classTest = nullptr;
            llvm::Value* instPtr = nullptr;

            if (valTy == impl_->boxType) {
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
                if (impl_->classNames.count(pat.name)) {
                    std::string cur = subjectClassName;
                    bool inChain = false;
                    while (!cur.empty()) {
                        if (cur == pat.name) { inChain = true; break; }
                        auto pit = impl_->classParentNamesBySym.find(impl_->classSym(cur));
                        if (pit == impl_->classParentNamesBySym.end()) break;
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

            if (!wantDestructure || !instPtr)
                return classTest;

            std::vector<std::string> order;
            {
                std::vector<std::string> chain;
                std::string cur = pat.name;
                while (!cur.empty()) {
                    chain.push_back(cur);
                    auto pit = impl_->classParentNamesBySym.find(impl_->classSym(cur));
                    cur = (pit != impl_->classParentNamesBySym.end()) ? pit->second : "";
                }
                std::set<std::string> seen;
                for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
                    auto fo = impl_->classFieldOrderBySym.find(impl_->classSym(*rit));
                    if (fo == impl_->classFieldOrderBySym.end()) continue;
                    for (auto& f : fo->second)
                        if (seen.insert(f).second) order.push_back(f);
                }
            }

            auto* clsFieldsBB = llvm::BasicBlock::Create(*impl_->context, "match.cls.fields", func);
            auto* clsFailBB = llvm::BasicBlock::Create(*impl_->context, "match.cls.fail", func);
            auto* clsDoneBB = llvm::BasicBlock::Create(*impl_->context, "match.cls.done", func);
            impl_->builder->CreateCondBr(classTest, clsFieldsBB, clsFailBB);

            impl_->builder->SetInsertPoint(clsFieldsBB);
            auto* structTy = impl_->classStructTypesBySym.count(impl_->classSym(pat.name))
                ? impl_->classStructTypesBySym[impl_->classSym(pat.name)] : nullptr;
            llvm::Value* allMatch = llvm::ConstantInt::get(impl_->i1Type, 1);
            for (size_t si = 0; si < pat.subPatterns.size() && si < order.size(); ++si) {
                auto idxIt = impl_->classFieldIndicesBySym[impl_->classSym(pat.name)].find(order[si]);
                if (!structTy || idxIt == impl_->classFieldIndicesBySym[impl_->classSym(pat.name)].end()) continue;
                auto* fTy = impl_->classFieldTypesBySym[impl_->classSym(pat.name)][order[si]];
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

        return llvm::ConstantInt::get(impl_->i1Type, 0);
    };

    size_t numCases = node.cases.size();
    for (size_t i = 0; i < numCases; ++i) {
        auto& arm = node.cases[i];

        auto* testBB = llvm::BasicBlock::Create(
            *impl_->context, "match.case" + std::to_string(i) + ".test", func);
        auto* bodyBB = llvm::BasicBlock::Create(
            *impl_->context, "match.case" + std::to_string(i) + ".body", func);

        impl_->builder->CreateBr(testBB);
        impl_->builder->SetInsertPoint(testBB);

        impl_->pushScope();

        llvm::Value* subject = impl_->builder->CreateLoad(subjectTy, subjectAlloca, "match.subj");

        llvm::Value* matched = emitPatternMatch(subject, subjectTy, arm.pattern);

        llvm::BasicBlock* fallthroughBB = nullptr;

        if (arm.guard) {
            auto* guardBB = llvm::BasicBlock::Create(
                *impl_->context, "match.case" + std::to_string(i) + ".guard", func);
            auto* guardFailBB = llvm::BasicBlock::Create(
                *impl_->context, "match.case" + std::to_string(i) + ".gfail", func);

            impl_->builder->CreateCondBr(matched, guardBB, guardFailBB);

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

            fallthroughBB = guardFailBB;
        } else {
            auto* fallBB = llvm::BasicBlock::Create(
                *impl_->context, "match.case" + std::to_string(i) + ".fall", func);
            impl_->builder->CreateCondBr(matched, bodyBB, fallBB);
            fallthroughBB = fallBB;
        }

        impl_->builder->SetInsertPoint(bodyBB);
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
            impl_->emitScopeCleanup();
            impl_->builder->CreateBr(endBB);
        }

        impl_->builder->SetInsertPoint(fallthroughBB);
        if (i + 1 >= numCases) {
            impl_->builder->CreateBr(endBB);
        }

        impl_->popScope();
    }

    if (numCases == 0) {
        impl_->builder->CreateBr(endBB);
    }

    impl_->builder->SetInsertPoint(endBB);
}

}
