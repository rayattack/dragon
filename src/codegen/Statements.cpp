#include "../CodeGenImpl.h"

namespace dragon {

void CodeGen::visit(StarredExpr& node) {
    impl_->addError(
        "internal error: starred expression reached codegen outside a call "
        "or unpacking context",
        node.location());
    impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
}

void CodeGen::visit(ExprStmt& node) {
    node.expr->accept(*this);

    if (impl_->lastValue && impl_->options.gcMode == GCMode::RC &&
        dynamic_cast<AwaitExpr*>(node.expr.get()) && node.expr->type) {
        int64_t tag = Impl::taskResultReleaseTag(node.expr->type->kind());
        if (tag != TAG_INT) {
            llvm::Value* v = impl_->lastValue;
            if (!v->getType()->isPointerTy())
                v = impl_->builder->CreateIntToPtr(v, impl_->i8PtrType);
            if (tag == TAG_TASK_HANDLE)
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_vthread_detach"], {v});
            else if (tag == TAG_STR)
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref_str"], {v});
            else if (tag == TAG_CALLABLE)
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref_callable"], {v});
            else
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref"], {v});
        }
        return;
    }

    if (impl_->lastValue && dynamic_cast<FireExpr*>(node.expr.get())) {
        llvm::Value* vt = impl_->lastValue;
        if (!vt->getType()->isPointerTy())
            vt = impl_->builder->CreateIntToPtr(vt, impl_->i8PtrType);
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_vthread_detach"], {vt});
        return;
    }

    if (!impl_->templateBlockBufferStack.empty() && impl_->lastValue) {
        if (dynamic_cast<TemplateExpr*>(node.expr.get()) ||
            dynamic_cast<TemplateFileExpr*>(node.expr.get())) {
            llvm::Value* buf = impl_->templateBlockBufferStack.back();
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_append_ptr"],
                {buf, impl_->lastValue});
            return;
        }
    }

    if (impl_->options.gcMode != GCMode::RC || !impl_->lastValue) {
        return;
    }

    if (llvm::isa<llvm::PHINode>(impl_->lastValue)) {
        auto* ty = impl_->lastValue->getType();
        if (ty == impl_->boxType) {
            if (impl_->isOwnedBoxResult(impl_->lastValue)) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_box_decref"], {impl_->lastValue});
            }
        } else if (ty->isPointerTy() && impl_->isOwnedStrResult(impl_->lastValue)) {
            int64_t tag = impl_->inferPtrValueTag(node.expr.get());
            if (tag == 1) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref_str"], {impl_->lastValue});
            } else if (tag == 5 || tag == 6 || tag == 7) {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_decref"], {impl_->lastValue});
            }
        }
        return;
    }

    if (!llvm::isa<llvm::CallInst>(impl_->lastValue)) {
        return;
    }

    auto* callInst = llvm::cast<llvm::CallInst>(impl_->lastValue);
    auto* retTy = callInst->getType();

    if (retTy == impl_->boxType) {
        if (impl_->isOwnedBoxResult(impl_->lastValue)) {
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_box_decref"], {impl_->lastValue});
        }
        return;
    }

    if (retTy->isPointerTy()) {
        int64_t tag = impl_->inferPtrValueTag(node.expr.get());
        if (tag == TAG_STR) {
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_decref_str"], {impl_->lastValue});
        } else if (tag == TAG_LIST || tag == TAG_DICT || tag == TAG_BYTES) {
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_decref"], {impl_->lastValue});
        }
        return;
    }

    if (!retTy->isIntegerTy(64)) return;

    auto* callee = callInst->getCalledFunction();
    if (!callee) return;
    llvm::StringRef fname = callee->getName();
    static const std::unordered_set<std::string> popFns = {
        "dragon_list_pop",
        "dragon_dict_pop",
        "dragon_dict_pop_default",
        "dragon_set_pop",
        "dragon_deque_pop",
        "dragon_deque_popleft",
    };
    if (!popFns.count(fname.str())) return;

    auto* call = dynamic_cast<CallExpr*>(node.expr.get());
    if (!call) return;
    auto* attr = dynamic_cast<AttributeExpr*>(call->callee.get());
    if (!attr || !attr->object) return;

    int64_t elemTag = 0;
    Expr* container = attr->object.get();
    if (container->type) {
        if (auto* lt = dynamic_cast<ListType*>(container->type.get())) {
            if (lt->elementType)
                elemTag = Impl::typeKindToElemTag(lt->elementType->kind());
        } else if (auto* dt = dynamic_cast<DictType*>(container->type.get())) {
            if (dt->valueType)
                elemTag = Impl::typeKindToElemTag(dt->valueType->kind());
        }
    }

    if (elemTag != 1 && elemTag != 5 && elemTag != 6 && elemTag != 7) return;

    auto* ptr = impl_->builder->CreateIntToPtr(impl_->lastValue, impl_->i8PtrType,
                                                "pop.discard.ptr");
    if (elemTag == 1) {
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_decref_str"], {ptr});
    } else {
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_decref"], {ptr});
    }
}

void CodeGen::visit(IfStmt& node) {
    auto detectNarrowing = [this](Expr* cond) -> std::pair<std::string, Impl::VarKind> {
        if (auto* bin = dynamic_cast<BinaryExpr*>(cond)) {
            auto op = bin->op.type();
            bool isNe = (op == TokenType::NOT_EQUAL || op == TokenType::IS_NOT);
            bool isEq = (op == TokenType::EQUAL_EQUAL || op == TokenType::IS);
            if (isNe || isEq) {
                auto* lhsName = dynamic_cast<NameExpr*>(bin->left.get());
                auto* rhsName = dynamic_cast<NameExpr*>(bin->right.get());
                bool lhsIsNone = dynamic_cast<NoneLiteral*>(bin->left.get()) != nullptr;
                bool rhsIsNone = dynamic_cast<NoneLiteral*>(bin->right.get()) != nullptr;
                NameExpr* unionName = nullptr;
                if (lhsName && rhsIsNone) unionName = lhsName;
                else if (rhsName && lhsIsNone) unionName = rhsName;
                if (unionName &&
                    impl_->lookupVarKind(unionName->name) == Impl::VarKind::Union) {
                    auto membIt = impl_->unionMemberKinds.find(unionName->name);
                    if (membIt != impl_->unionMemberKinds.end() &&
                        membIt->second.size() == 2) {
                        Impl::VarKind nk = (membIt->second[0] == Impl::VarKind::Other)
                            ? membIt->second[1] : membIt->second[0];
                        if (nk != Impl::VarKind::Other)
                            return {unionName->name, isNe ? nk : Impl::VarKind::Other};
                    }
                }
            }
        }
        auto* call = dynamic_cast<CallExpr*>(cond);
        if (!call) return {"", Impl::VarKind::Other};
        auto* callee = dynamic_cast<NameExpr*>(call->callee.get());
        if (!callee || callee->name != "isinstance" || call->args.size() != 2)
            return {"", Impl::VarKind::Other};
        auto* argName = dynamic_cast<NameExpr*>(call->args[0].get());
        if (!argName || impl_->lookupVarKind(argName->name) != Impl::VarKind::Union)
            return {"", Impl::VarKind::Other};
        auto* typeName = dynamic_cast<NameExpr*>(call->args[1].get());
        if (!typeName) return {"", Impl::VarKind::Other};
        Impl::VarKind nk = Impl::VarKind::Other;
        if (typeName->name == "int")        nk = Impl::VarKind::Int;
        else if (typeName->name == "float") nk = Impl::VarKind::Float;
        else if (typeName->name == "bool")  nk = Impl::VarKind::Bool;
        else if (typeName->name == "str")   nk = Impl::VarKind::Str;
        else if (typeName->name == "bytes") nk = Impl::VarKind::List;
        else if (typeName->name == "list") {
            auto membIt = impl_->unionMemberKinds.find(argName->name);
            bool declaredListMember =
                membIt != impl_->unionMemberKinds.end() &&
                std::find(membIt->second.begin(), membIt->second.end(),
                          Impl::VarKind::List) != membIt->second.end();
            if (!declaredListMember)
                return {"", Impl::VarKind::Other};
            nk = Impl::VarKind::List;
        }
        else if (typeName->name == "dict")  nk = Impl::VarKind::Dict;
        else if (typeName->name == "tuple") nk = Impl::VarKind::Tuple;
        else if (typeName->name == "set")   nk = Impl::VarKind::Set;
        else if (impl_->classNames.count(typeName->name)) nk = Impl::VarKind::ClassInstance;
        if (nk == Impl::VarKind::Other) return {"", Impl::VarKind::Other};
        return {argName->name, nk};
    };

    auto computeElseKind = [this](const std::string& varName, Impl::VarKind matchedKind) -> Impl::VarKind {
        auto membIt = impl_->unionMemberKinds.find(varName);
        if (membIt == impl_->unionMemberKinds.end()) return Impl::VarKind::Union;
        auto& members = membIt->second;
        if (members.size() == 2) {
            return (members[0] == matchedKind) ? members[1] : members[0];
        }
        return Impl::VarKind::Union;
    };

    auto narrowClassName = [this](Expr* cond) -> std::string {
        auto* call = dynamic_cast<CallExpr*>(cond);
        if (!call) return "";
        auto* callee = dynamic_cast<NameExpr*>(call->callee.get());
        if (!callee || callee->name != "isinstance" || call->args.size() != 2)
            return "";
        auto* typeName = dynamic_cast<NameExpr*>(call->args[1].get());
        if (!typeName) return "";
        return impl_->classNames.count(typeName->name) ? typeName->name : "";
    };

    auto applyClassNarrow = [this](const std::string& var,
                                   const std::string& cls) -> std::function<void()> {
        if (var.empty() || cls.empty()) return []{};
        bool hadCN = impl_->varClassNames.count(var) > 0;
        std::string oldCN = hadCN ? impl_->varClassNames[var] : std::string();
        bool hadOM = impl_->varClassOwningModule.count(var) > 0;
        std::string oldOM = hadOM ? impl_->varClassOwningModule[var] : std::string();
        impl_->varClassNames[var] = cls;
        impl_->varClassOwningModule[var] = impl_->resolveClassOwningModule(cls);
        return [this, var, hadCN, oldCN, hadOM, oldOM] {
            if (hadCN) impl_->varClassNames[var] = oldCN;
            else impl_->varClassNames.erase(var);
            if (hadOM) impl_->varClassOwningModule[var] = oldOM;
            else impl_->varClassOwningModule.erase(var);
        };
    };

    node.condition->accept(*this);
    llvm::Value* cond = impl_->toBool(impl_->lastValue, node.condition.get());

    auto [narrowVar, narrowKind] = detectNarrowing(node.condition.get());

    auto* func = impl_->currentFunction;
    auto* thenBB = llvm::BasicBlock::Create(*impl_->context, "then", func);
    auto* mergeBB = llvm::BasicBlock::Create(*impl_->context, "ifend", func);

    std::vector<llvm::BasicBlock*> elifBlocks;
    for (size_t i = 0; i < node.elifClauses.size(); ++i) {
        elifBlocks.push_back(
            llvm::BasicBlock::Create(*impl_->context, "elif", func));
    }
    llvm::BasicBlock* elseBB = nullptr;
    if (!node.elseBody.empty()) {
        elseBB = llvm::BasicBlock::Create(*impl_->context, "else", func);
    }

    llvm::BasicBlock* nextBB = elifBlocks.empty()
        ? (elseBB ? elseBB : mergeBB) : elifBlocks[0];
    impl_->builder->CreateCondBr(cond, thenBB, nextBB);

    impl_->builder->SetInsertPoint(thenBB);
    impl_->pushScope();
    if (!narrowVar.empty() && narrowKind != Impl::VarKind::Other) {
        auto* localAlloca = impl_->lookupVar(narrowVar);
        llvm::Value* slotPtr = localAlloca;
        bool slotIsBox = (localAlloca && localAlloca->getAllocatedType() == impl_->boxType);
        if (!slotPtr) {
            if (auto* gv = impl_->lookupModuleGlobal(narrowVar)) {
                slotPtr = gv;
                slotIsBox = (gv->getValueType() == impl_->boxType);
            }
        }
        if (slotPtr && slotIsBox) {
            auto* box = impl_->builder->CreateLoad(
                impl_->boxType, slotPtr, narrowVar + ".box.narrow");
            llvm::Value* payload = impl_->boxPayloadAsKind(box, narrowKind);
            auto* narrowedAlloca = impl_->createEntryAlloca(
                func, narrowVar + ".narrowed", payload->getType());
            impl_->builder->CreateStore(payload, narrowedAlloca);
            impl_->setVar(narrowVar, narrowedAlloca, narrowKind);
            impl_->scopes.back().borrowed.insert(narrowVar);
        } else if (localAlloca) {
            impl_->setVar(narrowVar, localAlloca, narrowKind);
        }
    }
    auto restoreThenNarrow = applyClassNarrow(
        narrowVar, narrowKind == Impl::VarKind::ClassInstance
                       ? narrowClassName(node.condition.get()) : std::string());
    for (auto& stmt : node.thenBody) stmt->accept(*this);
    impl_->emitScopeCleanup();
    impl_->popScope();
    restoreThenNarrow();
    bool thenTerminated = impl_->builder->GetInsertBlock()->getTerminator() != nullptr;
    if (!thenTerminated)
        impl_->builder->CreateBr(mergeBB);

    for (size_t i = 0; i < node.elifClauses.size(); ++i) {
        impl_->builder->SetInsertPoint(elifBlocks[i]);
        node.elifClauses[i].first->accept(*this);
        llvm::Value* elifCond = impl_->toBool(impl_->lastValue, node.elifClauses[i].first.get());

        auto [elifNarrowVar, elifNarrowKind] = detectNarrowing(node.elifClauses[i].first.get());

        auto* elifThenBB = llvm::BasicBlock::Create(*impl_->context, "elifthen", func);
        llvm::BasicBlock* elifNextBB = (i + 1 < elifBlocks.size())
            ? elifBlocks[i + 1] : (elseBB ? elseBB : mergeBB);
        impl_->builder->CreateCondBr(elifCond, elifThenBB, elifNextBB);

        impl_->builder->SetInsertPoint(elifThenBB);
        impl_->pushScope();
        if (!elifNarrowVar.empty() && elifNarrowKind != Impl::VarKind::Other) {
            auto* existingAlloca = impl_->lookupVar(elifNarrowVar);
            if (existingAlloca && existingAlloca->getAllocatedType() == impl_->boxType) {
                auto* box = impl_->builder->CreateLoad(
                    impl_->boxType, existingAlloca, elifNarrowVar + ".box.narrow");
                llvm::Value* payload = impl_->boxPayloadAsKind(box, elifNarrowKind);
                auto* narrowedAlloca = impl_->createEntryAlloca(
                    func, elifNarrowVar + ".narrowed", payload->getType());
                impl_->builder->CreateStore(payload, narrowedAlloca);
                impl_->setVar(elifNarrowVar, narrowedAlloca, elifNarrowKind);
                impl_->scopes.back().borrowed.insert(elifNarrowVar);
            } else if (existingAlloca) {
                impl_->setVar(elifNarrowVar, existingAlloca, elifNarrowKind);
            }
        }
        auto restoreElifNarrow = applyClassNarrow(
            elifNarrowVar, elifNarrowKind == Impl::VarKind::ClassInstance
                               ? narrowClassName(node.elifClauses[i].first.get())
                               : std::string());
        for (auto& stmt : node.elifClauses[i].second) stmt->accept(*this);
        impl_->emitScopeCleanup();
        impl_->popScope();
        restoreElifNarrow();
        if (!impl_->builder->GetInsertBlock()->getTerminator())
            impl_->builder->CreateBr(mergeBB);
    }

    if (elseBB) {
        impl_->builder->SetInsertPoint(elseBB);
        impl_->pushScope();
        if (!narrowVar.empty()) {
            auto elseKind = computeElseKind(narrowVar, narrowKind);
            if (elseKind != Impl::VarKind::Union) {
                auto* localAlloca = impl_->lookupVar(narrowVar);
                llvm::Value* slotPtr = localAlloca;
                bool slotIsBox = (localAlloca && localAlloca->getAllocatedType() == impl_->boxType);
                if (!slotPtr) {
                    if (auto* gv = impl_->lookupModuleGlobal(narrowVar)) {
                        slotPtr = gv;
                        slotIsBox = (gv->getValueType() == impl_->boxType);
                    }
                }
                if (slotPtr && slotIsBox) {
                    auto* box = impl_->builder->CreateLoad(
                        impl_->boxType, slotPtr, narrowVar + ".box.narrow.else");
                    llvm::Value* payload = impl_->boxPayloadAsKind(box, elseKind);
                    auto* narrowedAlloca = impl_->createEntryAlloca(
                        func, narrowVar + ".narrowed.else", payload->getType());
                    impl_->builder->CreateStore(payload, narrowedAlloca);
                    impl_->setVar(narrowVar, narrowedAlloca, elseKind);
                    impl_->scopes.back().borrowed.insert(narrowVar);
                } else if (localAlloca) {
                    impl_->setVar(narrowVar, localAlloca, elseKind);
                }
            }
        }
        for (auto& stmt : node.elseBody) stmt->accept(*this);
        impl_->emitScopeCleanup();
        impl_->popScope();
        if (!impl_->builder->GetInsertBlock()->getTerminator())
            impl_->builder->CreateBr(mergeBB);
    }

    impl_->builder->SetInsertPoint(mergeBB);

    if (!narrowVar.empty() && stmtsAlwaysTerminate(node.thenBody) &&
        node.elifClauses.empty() && node.elseBody.empty()) {
        auto elseKind = computeElseKind(narrowVar, narrowKind);
        if (elseKind != Impl::VarKind::Union) {
            auto* localAlloca = impl_->lookupVar(narrowVar);
            llvm::Value* slotPtr = localAlloca;
            bool slotIsBox = (localAlloca && localAlloca->getAllocatedType() == impl_->boxType);
            if (!slotPtr) {
                if (auto* gv = impl_->lookupModuleGlobal(narrowVar)) {
                    slotPtr = gv;
                    slotIsBox = (gv->getValueType() == impl_->boxType);
                }
            }
            if (slotPtr && slotIsBox) {
                auto* box = impl_->builder->CreateLoad(
                    impl_->boxType, slotPtr, narrowVar + ".box.narrow.fall");
                llvm::Value* payload = impl_->boxPayloadAsKind(box, elseKind);
                auto* narrowedAlloca = impl_->createEntryAlloca(
                    func, narrowVar + ".narrowed.fall", payload->getType());
                impl_->builder->CreateStore(payload, narrowedAlloca);
                impl_->setVar(narrowVar, narrowedAlloca, elseKind);
                impl_->scopes.back().borrowed.insert(narrowVar);
            } else if (localAlloca) {
                impl_->setVar(narrowVar, localAlloca, elseKind);
            }
        }
    }
}

void CodeGen::visit(WhileStmt& node) {
    auto* func = impl_->currentFunction;
    auto* condBB = llvm::BasicBlock::Create(*impl_->context, "whilecond", func);
    auto* bodyBB = llvm::BasicBlock::Create(*impl_->context, "whilebody", func);
    auto* endBB = llvm::BasicBlock::Create(*impl_->context, "whileend", func);
    llvm::BasicBlock* elseBB = node.elseBody.empty()
        ? endBB
        : llvm::BasicBlock::Create(*impl_->context, "whileelse", func);

    impl_->loopStack.push({endBB, condBB, impl_->scopes.size(), impl_->tryFrameFuncs.size(), impl_->exitCleanupStack.size()});
    impl_->builder->CreateBr(condBB);

    impl_->builder->SetInsertPoint(condBB);
    node.condition->accept(*this);
    llvm::Value* cond = impl_->toBool(impl_->lastValue, node.condition.get());
    impl_->builder->CreateCondBr(cond, bodyBB, elseBB);

    impl_->builder->SetInsertPoint(bodyBB);
    impl_->pushScope();
    for (auto& stmt : node.body) stmt->accept(*this);
    impl_->emitScopeCleanup();
    impl_->popScope();
    if (!impl_->builder->GetInsertBlock()->getTerminator())
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
}

void CodeGen::visit(ReturnStmt& node) {
    if (impl_->generatorPtr) {
        impl_->emitExcFramePops(impl_->currentFnTryFrames());
        impl_->emitEarlyExitCleanups(*this, 0,
                                     impl_->currentFnExitCleanupBase());
        impl_->builder->CreateRetVoid();
        auto* deadBB = llvm::BasicBlock::Create(
            *impl_->context, "ret.dead", impl_->currentFunction);
        impl_->builder->SetInsertPoint(deadBB);
        return;
    }

    if (node.value) {
        node.value->accept(*this);
        llvm::Value* retVal = impl_->lastValue;
        llvm::Type* retType = impl_->currentFunction->getReturnType();
        bool retBoxWrapped = false;
        if (retType == impl_->boxType && retVal->getType() != impl_->boxType) {
            auto* tag = impl_->emitTagForExpr(node.value.get(), *this);
            if (impl_->options.gcMode == GCMode::RC &&
                retVal->getType()->isPointerTy()) {
                if (auto* tagConst = llvm::dyn_cast<llvm::ConstantInt>(tag)) {
                    int64_t t = tagConst->getSExtValue();
                    if (t == TAG_STR) {
                        if (!impl_->isOwnedStrResult(retVal))
                            impl_->builder->CreateCall(
                                impl_->runtimeFuncs["dragon_incref_str"], {retVal});
                    } else if (t == 5 || t == 6 || t == 7) {
                        if (!impl_->isOwnedPtrResult(retVal))
                            impl_->builder->CreateCall(
                                impl_->runtimeFuncs["dragon_incref"], {retVal});
                    }
                }
            }
            retVal = impl_->makeBox(tag, retVal);
            retBoxWrapped = true;
        } else if (retVal->getType() == impl_->boxType && retType != impl_->boxType) {
            if (impl_->options.gcMode == GCMode::RC &&
                !impl_->isOwnedBoxResult(retVal)) {
                auto* tag = impl_->boxTag(retVal, "ret.tag");
                auto* payloadI64 = impl_->boxPayloadI64(retVal, "ret.payload");
                impl_->emitUnionIncref(payloadI64, tag);
            }
            retVal = impl_->boxPayloadAsKind(
                retVal, Impl::typeKindToVarKind(
                            retType == impl_->f64Type ? Type::Kind::Float :
                            retType == impl_->i1Type ? Type::Kind::Bool :
                            retType->isPointerTy() ? Type::Kind::Str :
                            Type::Kind::Int));
            if (retVal->getType() != retType)
                retVal = impl_->coerceArg(retVal, retType);
            retBoxWrapped = true;
        } else if (retType == impl_->boxType && retVal->getType() == impl_->boxType) {
            // Returning an already-boxed value from an Any/Union function: make it OWNED
            // so callers take ownership uniformly. Incref a BORROW here, else use-after-free / double-free.
            if (impl_->options.gcMode == GCMode::RC &&
                !impl_->isOwnedBoxResult(retVal)) {
                auto* tag = impl_->boxTag(retVal, "ret.tag");
                auto* payloadI64 = impl_->boxPayloadI64(retVal, "ret.payload");
                impl_->emitUnionIncref(payloadI64, tag);
            }
            retBoxWrapped = true;
        } else if (retVal->getType() != retType)
            retVal = impl_->coerceArg(retVal, retType);
        if (impl_->options.gcMode == GCMode::RC && !retBoxWrapped) {
            auto increfIfHeap = [&](Impl::VarKind kind) {
                if (Impl::isHeapKind(kind)) {
                    impl_->emitIncrefByKind(retVal, kind);
                }
            };
            Expr* retSrc = node.value.get();
            while (auto* castExpr = dynamic_cast<AsCastExpr*>(retSrc))
                retSrc = castExpr->operand.get();
            if (auto* nameExpr = dynamic_cast<NameExpr*>(retSrc)) {
                increfIfHeap(impl_->lookupVarKind(nameExpr->name));
            } else if (auto* attrExpr = dynamic_cast<AttributeExpr*>(retSrc)) {
                if (auto* objName = dynamic_cast<NameExpr*>(attrExpr->object.get())) {
                    auto objKind = impl_->lookupVarKind(objName->name);
                    if (objKind == Impl::VarKind::ClassInstance) {
                        std::string className;
                        if (objName->name == "self" && !impl_->currentClassName.empty()) {
                            className = impl_->currentClassName;
                        } else {
                            auto it = impl_->varClassNames.find(objName->name);
                            if (it != impl_->varClassNames.end()) className = it->second;
                        }
                        Impl::VarKind fieldKind = Impl::VarKind::Other;
                        for (std::string cls = className; !cls.empty(); ) {
                            auto fkIt = impl_->classFieldKindsBySym.find(impl_->classSym(cls));
                            if (fkIt != impl_->classFieldKindsBySym.end()) {
                                auto fieldIt = fkIt->second.find(attrExpr->attribute);
                                if (fieldIt != fkIt->second.end()) {
                                    fieldKind = fieldIt->second;
                                    break;
                                }
                            }
                            auto parentIt = impl_->classParentNamesBySym.find(impl_->classSym(cls));
                            if (parentIt != impl_->classParentNamesBySym.end())
                                cls = parentIt->second;
                            else
                                break;
                        }
                        if (Impl::isHeapKind(fieldKind)) {
                            impl_->emitIncrefByKind(retVal, fieldKind);
                        }
                    }
                }
            } else if (auto* subExpr = dynamic_cast<SubscriptExpr*>(retSrc)) {
                bool ownedStrElem = subExpr->object && subExpr->object->type &&
                    subExpr->object->type->kind() == Type::Kind::Str;
                if (dynamic_cast<SliceExpr*>(subExpr->index.get()) == nullptr &&
                    !ownedStrElem) {
                    Impl::VarKind kind = Impl::VarKind::Other;
                    if (node.value->type)
                        kind = Impl::typeKindToVarKind(node.value->type->kind());
                    if (Impl::isHeapKind(kind))
                        impl_->emitIncrefByKind(retVal, kind);
                }
            }
        }
        impl_->emitExcFramePops(impl_->currentFnTryFrames());
        impl_->emitEarlyExitCleanups(*this, 0,
                                     impl_->currentFnExitCleanupBase());
        impl_->builder->CreateRet(retVal);
    } else {
        impl_->emitExcFramePops(impl_->currentFnTryFrames());
        impl_->emitEarlyExitCleanups(*this, 0,
                                     impl_->currentFnExitCleanupBase());
        impl_->builder->CreateRetVoid();
    }
    auto* deadBB = llvm::BasicBlock::Create(
        *impl_->context, "ret.dead", impl_->currentFunction);
    impl_->builder->SetInsertPoint(deadBB);
}

void CodeGen::visit(BreakStmt&) {
    if (!impl_->loopStack.empty()) {
        impl_->emitExcFramePops(impl_->tryFrameFuncs.size() - impl_->loopStack.top().tryFrameDepth);
        impl_->emitEarlyExitCleanups(*this, impl_->loopStack.top().scopeDepth,
                                     impl_->loopStack.top().exitCleanupDepth);
        impl_->builder->CreateBr(impl_->loopStack.top().breakBlock);
    }
}

void CodeGen::visit(ContinueStmt&) {
    if (!impl_->loopStack.empty()) {
        impl_->emitExcFramePops(impl_->tryFrameFuncs.size() - impl_->loopStack.top().tryFrameDepth);
        impl_->emitEarlyExitCleanups(*this, impl_->loopStack.top().scopeDepth,
                                     impl_->loopStack.top().exitCleanupDepth);
        impl_->builder->CreateBr(impl_->loopStack.top().continueBlock);
    }
}

void CodeGen::visit(PassStmt&) {
}

void CodeGen::visit(DeferStmt& node) {
    auto* call = dynamic_cast<CallExpr*>(node.call.get());
    if (!call) {
        impl_->addError("defer requires a direct call", node.location());
        return;
    }
    if (impl_->options.gcMode != GCMode::RC) {
        impl_->addError("defer requires the RC memory mode", node.location());
        return;
    }
    if (impl_->scopes.empty()) return;
    if (!call->kwArgs.empty()) {
        impl_->addError("defer does not support keyword arguments yet; "
                        "pass them positionally", node.location());
        return;
    }

    llvm::Function* targetFn = nullptr;
    std::string calleeName;
    bool isMethodCall = false;
    int deferVtIdx = -1;
    llvm::Value* selfVal = nullptr;
    Expr* selfExpr = nullptr;
    if (auto* nameExpr = dynamic_cast<NameExpr*>(call->callee.get())) {
        targetFn = impl_->module->getFunction(nameExpr->name);
        calleeName = nameExpr->name;
    } else if (auto* attrExpr =
               dynamic_cast<AttributeExpr*>(call->callee.get())) {
        std::string className;
        std::string owningModule;
        if (auto* objName = dynamic_cast<NameExpr*>(attrExpr->object.get())) {
            if (objName->name == "self" && !impl_->currentClassName.empty()) {
                className = impl_->currentClassName;
                owningModule = impl_->currentModuleName;
            } else {
                auto vit = impl_->varClassNames.find(objName->name);
                if (vit != impl_->varClassNames.end()) className = vit->second;
                auto vmIt = impl_->varClassOwningModule.find(objName->name);
                if (vmIt != impl_->varClassOwningModule.end()) {
                    owningModule = vmIt->second;
                } else if (!className.empty()) {
                    owningModule = impl_->resolveClassOwningModule(className);
                }
            }
        }
        if (className.empty() && attrExpr->object->type) {
            if (auto* inst = dynamic_cast<InstanceType*>(
                    attrExpr->object->type.get())) {
                if (inst->classType) {
                    className = inst->classType->name;
                    owningModule = inst->classType->definingModule;
                }
            }
        }
        if (!className.empty()) {
            std::string methodFuncName;
            targetFn = impl_->resolveMethodFunction(
                owningModule, className, attrExpr->attribute, &methodFuncName);
            if (targetFn) {
                calleeName = methodFuncName;
                isMethodCall = true;
                attrExpr->object->accept(*this);
                selfVal = impl_->lastValue;
                selfExpr = attrExpr->object.get();
                if (impl_->methodIsOverridden(className, attrExpr->attribute)) {
                    auto idxIt =
                        impl_->classMethodVtableIndicesBySym.find(impl_->classSym(className));
                    if (idxIt != impl_->classMethodVtableIndicesBySym.end()) {
                        auto mIt = idxIt->second.find(attrExpr->attribute);
                        if (mIt != idxIt->second.end())
                            deferVtIdx = (int)mIt->second;
                    }
                }
            }
        }
    }
    if (!targetFn) {
        impl_->addError(
            "defer: cannot resolve the callee to a direct function or "
            "method; a computed or closure callee is not supported yet",
            node.location());
        return;
    }

    std::vector<llvm::Value*> vals;
    std::vector<Expr*> exprs;
    if (isMethodCall) {
        vals.push_back(selfVal);
        exprs.push_back(selfExpr);
    }
    for (auto& a : call->args) {
        a->accept(*this);
        vals.push_back(impl_->lastValue);
        exprs.push_back(a.get());
    }

    std::vector<Impl::VarKind> drainKinds(vals.size(), Impl::VarKind::Other);
    for (size_t i = 0; i < vals.size(); ++i) {
        Expr* e = exprs[i];
        if (!vals[i]) {
            impl_->addError("defer: could not evaluate argument",
                            node.location());
            return;
        }
        auto* nm = dynamic_cast<NameExpr*>(e);
        if (nm && nm->isMoveMarked) continue;
        Impl::VarKind tk = e->type
            ? Impl::typeKindToVarKind(e->type->kind())
            : Impl::VarKind::Other;
        if (tk == Impl::VarKind::Union) {
            impl_->addError(
                "defer: an argument of Any/union type is not supported yet; "
                "annotate the concrete type", node.location());
            return;
        }
        if (nm && nm->isDubMarked) {
            drainKinds[i] = tk;
            continue;
        }
        Impl::VarKind owned = impl_->ownedTempDrainKind(e, vals[i]);
        if (owned != Impl::VarKind::Other) {
            drainKinds[i] = owned;
            continue;
        }
        if (Impl::isHeapKind(tk)) {
            llvm::Value* heapified = impl_->ensureHeapString(vals[i], e);
            if (heapified != vals[i]) {
                vals[i] = heapified;
                drainKinds[i] = Impl::VarKind::Str;
            } else {
                impl_->emitIncrefByKind(vals[i], tk);
                drainKinds[i] = tk;
            }
        }
    }

    auto* targetTy = targetFn->getFunctionType();
    if (vals.size() < targetTy->getNumParams()) {
        std::vector<std::pair<llvm::Value*, Impl::VarKind>> defaultTemps;
        impl_->fillDefaultArgs(calleeName, targetFn, vals, *this,
                               &defaultTemps);
        drainKinds.resize(vals.size(), Impl::VarKind::Other);
        for (auto& [dv, dk] : defaultTemps)
            for (size_t i = 0; i < vals.size(); ++i)
                if (vals[i] == dv) { drainKinds[i] = dk; break; }
    }
    if (vals.size() != targetTy->getNumParams()) {
        impl_->addError("defer: argument count does not match '" +
                        calleeName + "'", node.location());
        return;
    }

    unsigned argc = (unsigned)vals.size();
    std::vector<llvm::Value*> valsI64(argc);
    for (unsigned i = 0; i < argc; ++i) {
        auto* pty = targetTy->getParamType(i);
        if (!pty->isPointerTy() && !pty->isDoubleTy() && !pty->isIntegerTy()) {
            impl_->addError(
                "defer: parameter " + std::to_string(i + 1) + " of '" +
                calleeName + "' has a type defer cannot snapshot yet",
                node.location());
            return;
        }
        valsI64[i] = impl_->cleanupValToI64(impl_->coerceArg(vals[i], pty));
    }
    auto* func = impl_->currentFunction;
    auto* arrayTy = llvm::ArrayType::get(impl_->i64Type, std::max(argc, 1u));
    std::string siteName =
        calleeName + "_" + std::to_string(impl_->lambdaCounter++);
    auto* argSlots = impl_->createEntryAlloca(func, "defer.args." + siteName,
                                              arrayTy);
    for (unsigned i = 0; i < argc; ++i) {
        auto* slotPtr = impl_->builder->CreateConstInBoundsGEP2_64(
            arrayTy, argSlots, 0, i, "defer.snap.p");
        impl_->builder->CreateStore(valsI64[i], slotPtr);
    }

    auto* thunk = impl_->buildDeferThunk(targetFn, siteName, deferVtIdx);

    impl_->emitMoveOutSlots(*call);

    {
        auto* i32Ty = llvm::Type::getInt32Ty(*impl_->context);
        auto& scope = impl_->scopes.back();
        if (!scope.cleanupBaseAlloca)
            scope.cleanupBaseAlloca =
                impl_->createEntryAllocaI32(func, "clbase", -1);
        auto* baseAlloca = scope.cleanupBaseAlloca;
        auto* doBB =
            llvm::BasicBlock::Create(*impl_->context, "defer.clpush.do", func);
        auto* contBB = llvm::BasicBlock::Create(*impl_->context,
                                                "defer.clpush.cont", func);
        impl_->builder->CreateCondBr(impl_->emitActiveFramesNonZero(), doBB,
                                     contBB);
        impl_->builder->SetInsertPoint(doBB);
        auto* pushFn = impl_->runtimeFuncs["dragon_cleanup_push"];
        llvm::Value* firstSlot = nullptr;
        for (unsigned i = 0; i < argc; ++i) {
            auto* slot = impl_->builder->CreateCall(
                pushFn,
                {valsI64[i],
                 llvm::ConstantInt::get(
                     i32Ty, impl_->cleanupKindFor(drainKinds[i])),
                 llvm::ConstantInt::get(i32Ty, 0)},
                "defer.cl.arg");
            if (!firstSlot) firstSlot = slot;
        }
        auto* thunkI64 = impl_->builder->CreatePtrToInt(thunk, impl_->i64Type,
                                                        "defer.thunk.i64");
        auto* callSlot = impl_->builder->CreateCall(
            pushFn,
            {thunkI64,
             llvm::ConstantInt::get(i32Ty, Impl::DCLEAN_DEFER_CALL),
             llvm::ConstantInt::get(i32Ty, argc)},
            "defer.cl.call");
        if (!firstSlot) firstSlot = callSlot;
        auto* curBase =
            impl_->builder->CreateLoad(i32Ty, baseAlloca, "clbase.cur");
        auto* isFirst = impl_->builder->CreateICmpEQ(
            curBase, llvm::ConstantInt::get(i32Ty, -1), "clbase.first");
        impl_->builder->CreateStore(
            impl_->builder->CreateSelect(isFirst, firstSlot, curBase,
                                         "clbase.new"),
            baseAlloca);
        impl_->builder->CreateBr(contBB);
        impl_->builder->SetInsertPoint(contBB);
    }

    impl_->scopes.back().deferred.push_back(
        {thunk, argSlots, argc, std::move(drainKinds)});
}

void CodeGen::visit(RaiseStmt& node) {
    auto* func = impl_->currentFunction;

    auto emitRaise = [&](llvm::Value* typeVal, llvm::Value* msgVal) {
        bool owned = impl_->options.gcMode == GCMode::RC &&
                     impl_->isOwnedStrResult(msgVal);
        bool literal = !owned && llvm::isa<llvm::Constant>(msgVal);
        auto* raiseFn = impl_->runtimeFuncs[owned   ? "dragon_raise_exc_consume"
                                            : literal ? "dragon_raise_exc_cstr"
                                                      : "dragon_raise_exc"];
        impl_->builder->CreateCall(raiseFn, {typeVal, msgVal});
        impl_->builder->CreateUnreachable();
        auto* deadBB = llvm::BasicBlock::Create(*impl_->context, "raise.dead", func);
        impl_->builder->SetInsertPoint(deadBB);
    };

    auto emitReraiseCurrent = [&]() {
        auto* t = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_exc_get_type"], {}, "reraise.type");
        auto* o = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_exc_retain_obj"],
            {impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_exc_get_obj"], {},
                "reraise.obj.raw")},
            "reraise.obj");
        auto* m = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_exc_get_msg"], {}, "reraise.msg");
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_raise_exc_obj"], {t, o, m});
        impl_->builder->CreateUnreachable();
        auto* deadBB = llvm::BasicBlock::Create(*impl_->context, "raise.dead", func);
        impl_->builder->SetInsertPoint(deadBB);
    };

    if (node.exception) {
        if (auto* call = dynamic_cast<CallExpr*>(node.exception.get())) {
            if (auto* name = dynamic_cast<NameExpr*>(call->callee.get())) {
                int64_t typeCode = impl_->excTypeCode(name->name);
                auto* typeVal = llvm::ConstantInt::get(impl_->i64Type, typeCode);

                bool isUserExc = impl_->userExcCodesBySym.count(impl_->classSym(name->name)) > 0;
                auto* initFn = isUserExc
                    ? impl_->module->getFunction(
                          impl_->classSymPrefix(name->name) + "___init__")
                    : nullptr;
                bool arityMatches = false;
                if (isUserExc && initFn) {
                    unsigned expected = initFn->getFunctionType()->getNumParams();
                    if (expected >= 1 && call->args.size() == expected - 1) {
                        arityMatches = true;
                    } else if (expected >= 1 && call->args.size() < expected - 1) {
                        std::string newSym =
                            impl_->classSymPrefix(name->name) + "_new";
                        auto defIt = impl_->funcParamDefaults.find(newSym);
                        if (defIt != impl_->funcParamDefaults.end()) {
                            bool allHaveDefaults = true;
                            size_t userArgs = (size_t)expected - 1;
                            for (size_t i = call->args.size(); i < userArgs; ++i) {
                                if (i >= defIt->second.size() ||
                                    defIt->second[i] == nullptr) {
                                    allHaveDefaults = false;
                                    break;
                                }
                            }
                            if (allHaveDefaults) arityMatches = true;
                        }
                    }
                }
                if (isUserExc && initFn && arityMatches) {
                    llvm::Value* msgVal = nullptr;
                    if (!call->args.empty()) {
                        Expr* a0 = call->args[0].get();
                        bool pure = false;
                        if (auto* sl = dynamic_cast<StringLiteral*>(a0))
                            pure = !sl->isBytes && !sl->isFString;
                        else if (dynamic_cast<NameExpr*>(a0))
                            pure = true;
                        // Adopt a0 as msg only when it is actually a `str`; a ptr-shaped
                        // non-string first arg (list/dict/bytes) raised as a string UAFs.
                        const bool isStr = a0->type && a0->type->kind() == Type::Kind::Str;
                        if (pure && isStr) {
                            a0->accept(*this);
                            llvm::Value* v = impl_->lastValue;
                            if (v && v->getType()->isPointerTy())
                                msgVal = v;
                        }
                    }
                    if (!msgVal)
                        msgVal = impl_->builder->CreateGlobalString(name->name);
                    node.exception->accept(*this);
                    llvm::Value* inst = impl_->lastValue;
                    if (!inst->getType()->isPointerTy())
                        inst = impl_->builder->CreateIntToPtr(inst, impl_->i8PtrType);
                    else if (inst->getType() != impl_->i8PtrType)
                        inst = impl_->builder->CreateBitCast(inst, impl_->i8PtrType);
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_raise_exc_obj"],
                        {typeVal, inst, msgVal});
                    impl_->builder->CreateUnreachable();
                    auto* deadBB = llvm::BasicBlock::Create(
                        *impl_->context, "raise.dead", func);
                    impl_->builder->SetInsertPoint(deadBB);
                    return;
                }

                llvm::Value* msgVal = nullptr;
                if (!call->args.empty()) {
                    call->args[0]->accept(*this);
                    msgVal = impl_->lastValue;
                } else {
                    msgVal = impl_->builder->CreateGlobalString(name->name);
                }

                emitRaise(typeVal, msgVal);
                return;
            }
        }

        if (auto* nameRef = dynamic_cast<NameExpr*>(node.exception.get())) {
            for (auto& v : impl_->handlerExcVars) {
                if (v == nameRef->name) {
                    emitReraiseCurrent();
                    return;
                }
            }
            auto cnIt = impl_->varClassNames.find(nameRef->name);
            if (cnIt != impl_->varClassNames.end() &&
                impl_->userExcCodesBySym.count(impl_->classSym(cnIt->second)) > 0) {
                int64_t typeCode = impl_->excTypeCode(cnIt->second);
                auto* typeVal = llvm::ConstantInt::get(impl_->i64Type, typeCode);
                node.exception->accept(*this);
                llvm::Value* inst = impl_->lastValue;
                if (!inst->getType()->isPointerTy())
                    inst = impl_->builder->CreateIntToPtr(inst, impl_->i8PtrType);
                else if (inst->getType() != impl_->i8PtrType)
                    inst = impl_->builder->CreateBitCast(inst, impl_->i8PtrType);
                auto* msgVal = impl_->builder->CreateGlobalString(cnIt->second);
                inst = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_exc_retain_obj"], {inst},
                    "raise.obj.retained");
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_raise_exc_obj"],
                    {typeVal, inst, msgVal});
                impl_->builder->CreateUnreachable();
                auto* deadBB = llvm::BasicBlock::Create(
                    *impl_->context, "raise.dead", func);
                impl_->builder->SetInsertPoint(deadBB);
                return;
            }
        }

        node.exception->accept(*this);
        llvm::Value* excVal = impl_->lastValue;
        auto* typeVal = llvm::ConstantInt::get(impl_->i64Type, 1);
        llvm::Value* msgVal = excVal;
        if (msgVal->getType() != impl_->i8PtrType) {
            msgVal = impl_->builder->CreateGlobalString("Exception");
        }
        emitRaise(typeVal, msgVal);
        return;
    }

    emitReraiseCurrent();
}
void CodeGen::visit(GlobalStmt& node) {
    for (auto& name : node.names) {
        impl_->globalDeclaredVars.insert(name);
    }
}
void CodeGen::visit(NonlocalStmt& node) {
    for (auto& name : node.names) {
        impl_->nonlocalDeclaredVars.insert(name);
    }
}
void CodeGen::visit(DeleteStmt& node) {
    for (size_t ti = 0; ti < node.targets.size(); ++ti) {
        auto& target = node.targets[ti];
        if (auto* nameExpr = dynamic_cast<NameExpr*>(target.get())) {
            auto* alloca = impl_->lookupVar(nameExpr->name);
            if (!alloca) continue;
            if (impl_->options.gcMode == GCMode::RC) {
                auto kind = impl_->lookupVarKind(nameExpr->name);
                if (Impl::isHeapKind(kind)) {
                    auto* val = impl_->builder->CreateLoad(
                        alloca->getAllocatedType(), alloca, nameExpr->name + ".del");
                    bool proven = ti < node.provenUnique.size() &&
                                  node.provenUnique[ti];
                    if (proven && impl_->options.optimizationLevel == 0) {
                        auto* file = impl_->builder->CreateGlobalString(
                            impl_->currentModuleName.empty()
                                ? "main" : impl_->currentModuleName);
                        auto* line = llvm::ConstantInt::get(
                            impl_->i64Type, nameExpr->location().line);
                        if (kind == Impl::VarKind::Union &&
                            val->getType() == impl_->boxType) {
                            impl_->builder->CreateCall(
                                impl_->runtimeFuncs["dragon_del_assert_unique_box"],
                                {impl_->boxTag(val, "del.tag"),
                                 impl_->boxPayloadI64(val, "del.payload"),
                                 file, line});
                        } else if (kind == Impl::VarKind::Closure) {
                            auto* asI64 = impl_->builder->CreatePtrToInt(
                                val, impl_->i64Type, "del.clos.i64");
                            impl_->builder->CreateCall(
                                impl_->runtimeFuncs["dragon_del_assert_unique_box"],
                                {llvm::ConstantInt::get(impl_->i64Type, 10),
                                 asI64, file, line});
                        } else if (val->getType()->isPointerTy()) {
                            auto* cls = llvm::ConstantInt::get(
                                impl_->i64Type,
                                kind == Impl::VarKind::Str ? 1 : 0);
                            impl_->builder->CreateCall(
                                impl_->runtimeFuncs["dragon_del_assert_unique"],
                                {impl_->toI8Ptr(val), cls, file, line});
                        }
                    }
                    impl_->emitDecrefByKind(val, kind);
                }
                else if (nameExpr->type &&
                         nameExpr->type->kind() == Type::Kind::Lock) {
                    auto* val = impl_->builder->CreateLoad(
                        alloca->getAllocatedType(), alloca,
                        nameExpr->name + ".del.lock");
                    llvm::Value* p = val;
                    if (p->getType()->isIntegerTy())
                        p = impl_->builder->CreateIntToPtr(p, impl_->i8PtrType);
                    if (p->getType()->isPointerTy())
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_lock_destroy"], {p});
                }
                else if (nameExpr->type &&
                         nameExpr->type->kind() == Type::Kind::Task) {
                    auto* val = impl_->builder->CreateLoad(
                        alloca->getAllocatedType(), alloca,
                        nameExpr->name + ".del.task");
                    llvm::Value* p = val;
                    if (p->getType()->isIntegerTy())
                        p = impl_->builder->CreateIntToPtr(p, impl_->i8PtrType);
                    if (p->getType()->isPointerTy())
                        impl_->builder->CreateCall(
                            impl_->runtimeFuncs["dragon_vthread_detach"], {p});
                }
            }
            // Store null/zero to the slot to prevent double-free on scope cleanup
            if (alloca->getAllocatedType() == impl_->boxType) {
                // Union slot: zero the whole {tag, payload} box so scope cleanup's Union
                // drain sees tag 0 and no-ops instead of double-freeing the already-drained payload.
                impl_->builder->CreateStore(
                    llvm::Constant::getNullValue(impl_->boxType), alloca);
            } else if (alloca->getAllocatedType()->isPointerTy()) {
                impl_->builder->CreateStore(
                    llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(alloca->getAllocatedType())),
                    alloca);
            } else {
                impl_->builder->CreateStore(
                    llvm::ConstantInt::get(alloca->getAllocatedType(), 0),
                    alloca);
            }
        }
        else if (auto* sub = dynamic_cast<SubscriptExpr*>(target.get())) {
            bool isDict = false;
            if (auto* objName = dynamic_cast<NameExpr*>(sub->object.get())) {
                isDict = impl_->lookupVarKind(objName->name) == Impl::VarKind::Dict;
            }
            if (!isDict && sub->object->type &&
                sub->object->type->kind() == Type::Kind::Dict) {
                isDict = true;
            }
            if (!isDict) {
                bool isList = false;
                if (auto* objName = dynamic_cast<NameExpr*>(sub->object.get())) {
                    isList = impl_->lookupVarKind(objName->name) ==
                             Impl::VarKind::List;
                }
                if (!isList && sub->object->type &&
                    sub->object->type->kind() == Type::Kind::List) {
                    isList = true;
                }
                if (!isList) {
                    SourceLocation loc = target->location();
                    if (loc.line == 0) loc = node.location();
                    impl_->addError(
                        "unsupported 'del' target: only 'del name', "
                        "'del list[i]', and 'del dict[key]' are supported",
                        loc);
                    continue;
                }
                bool isBox = impl_->getIterableElementKind(sub->object.get()) ==
                             Type::Kind::Any;
                sub->object->accept(*this);
                llvm::Value* lst = impl_->lastValue;
                sub->index->accept(*this);
                llvm::Value* idx = impl_->lastValue;
                if (idx->getType() == impl_->i1Type)
                    idx = impl_->builder->CreateZExt(idx, impl_->i64Type);
                else if (idx->getType()->isPointerTy())
                    idx = impl_->builder->CreatePtrToInt(idx, impl_->i64Type);
                else if (idx->getType() != impl_->i64Type)
                    idx = impl_->builder->CreateZExtOrTrunc(idx, impl_->i64Type);
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs[isBox ? "dragon_list_box_delitem"
                                              : "dragon_list_delitem"],
                    {lst, idx});
                continue;
            }

            Type::Kind dictKk = impl_->resolveDictKeyKind(sub->object.get());
            bool intKeyed =
                dictKk == Type::Kind::Int || dictKk == Type::Kind::Float;
            sub->object->accept(*this);
            llvm::Value* dict = impl_->lastValue;
            sub->index->accept(*this);
            llvm::Value* key = impl_->lastValue;

            if (intKeyed) {
                if (dictKk == Type::Kind::Float)
                    key = impl_->emitFloatDictKeyBits(key);
                if (key->getType() == impl_->i1Type)
                    key = impl_->builder->CreateZExt(key, impl_->i64Type);
                else if (key->getType()->isPointerTy())
                    key = impl_->builder->CreatePtrToInt(key, impl_->i64Type);
                else if (key->getType() != impl_->i64Type)
                    key = impl_->builder->CreateZExtOrTrunc(key, impl_->i64Type);
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_int_del"], {dict, key});
            } else {
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_del"], {dict, key});
                if (impl_->options.gcMode == GCMode::RC &&
                    impl_->isOwnedStrResult(key)) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_decref_str"], {key});
                }
            }
        }
        else if (auto* attr = dynamic_cast<AttributeExpr*>(target.get())) {
            bool isDictObj = false;
            if (impl_->isDragonFile) {
                if (auto* objName = dynamic_cast<NameExpr*>(attr->object.get())) {
                    isDictObj = impl_->lookupVarKind(objName->name) ==
                                Impl::VarKind::Dict;
                }
                if (!isDictObj) {
                    if (auto* objAttr =
                            dynamic_cast<AttributeExpr*>(attr->object.get())) {
                        std::string cls;
                        if (auto* aon =
                                dynamic_cast<NameExpr*>(objAttr->object.get())) {
                            if (aon->name == "self" &&
                                !impl_->currentClassName.empty())
                                cls = impl_->currentClassName;
                            else {
                                auto vit = impl_->varClassNames.find(aon->name);
                                if (vit != impl_->varClassNames.end())
                                    cls = vit->second;
                            }
                        }
                        if (cls.empty())
                            cls = impl_->resolveExprClassName(
                                objAttr->object.get());
                        if (!cls.empty()) {
                            auto fkIt = impl_->classFieldKindsBySym.find(impl_->classSym(cls));
                            if (fkIt != impl_->classFieldKindsBySym.end()) {
                                auto f2 = fkIt->second.find(objAttr->attribute);
                                if (f2 != fkIt->second.end() &&
                                    f2->second == Impl::VarKind::Dict)
                                    isDictObj = true;
                            }
                        }
                    }
                }
                if (!isDictObj && attr->object->type &&
                    attr->object->type->kind() == Type::Kind::Dict) {
                    isDictObj = true;
                }
            }
            if (isDictObj) {
                attr->object->accept(*this);
                llvm::Value* dict = impl_->lastValue;
                auto* keyStr =
                    impl_->builder->CreateGlobalString(attr->attribute);
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_del"], {dict, keyStr});
            } else {
                SourceLocation loc = target->location();
                if (loc.line == 0) loc = node.location();
                impl_->addError(
                    "cannot 'del' an attribute of a class instance (objects "
                    "have fixed layout); supported: 'del name', "
                    "'del list[i]', 'del dict[key]', and 'del dict.key'",
                    loc);
            }
        }
        else {
            SourceLocation loc = target->location();
            if (loc.line == 0) loc = node.location();
            impl_->addError(
                "unsupported 'del' target: only 'del name', 'del list[i]', "
                "'del dict[key]', and 'del dict.key' are supported",
                loc);
        }
    }
}
void CodeGen::visit(ImportStmt&) {}

void CodeGen::visit(FromImportStmt& node) {
    if (impl_->fileResolvedModules.count(node.module)) {
        auto& bucket = impl_->importedFuncAliasesByModule[impl_->currentModuleName];
        for (auto& alias : node.names) {
            const std::string& localName =
                alias.asName.empty() ? alias.name : alias.asName;
            std::string mangled = Impl::mangleFunc(node.module, alias.name);
            if (impl_->module && !impl_->module->getFunction(mangled)) {
                std::string bare = Impl::userFuncName(alias.name);
                if (impl_->module->getFunction(bare)) mangled = bare;
            }
            bucket[localName] = mangled;

            std::string classMangled = Impl::mangleClass(node.module, alias.name);
            if (impl_->module &&
                (impl_->module->getFunction(classMangled + "_new") ||
                 impl_->module->getFunction(classMangled + "_new_0") ||
                 impl_->module->getFunction(classMangled + "___init__") ||
                 impl_->module->getFunction(classMangled + "___init___0"))) {
                impl_->importedClassAliasesByModule[impl_->currentModuleName]
                    [localName] = node.module;
            }

            std::string globalKey = Impl::mangleGlobal(node.module, alias.name);
            if (impl_->moduleGlobals.count(globalKey)) {
                impl_->importedGlobalAliasesByModule[impl_->currentModuleName]
                    [localName] = globalKey;
            }
        }
        return;
    }
}

}
