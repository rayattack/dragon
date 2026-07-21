/// Dragon CodeGen - Miscellaneous Statements
#include "../CodeGenImpl.h"

namespace dragon {

void CodeGen::visit(StarredExpr&) {
    impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
}

//===----------------------------------------------------------------------===//
// Visitor: Statements
//===----------------------------------------------------------------------===//

void CodeGen::visit(ExprStmt& node) {
    node.expr->accept(*this);

    // A discarded `fire fn()` / `fire { ... }` (fire-and-forget): its Task handle
    // is dropped right here, so detach the vthread - it then frees itself on
    // completion instead of leaking the struct + its ~2MB coroutine stack (#2).
    // `t: Task[T] = fire ...` (binding) and `await fire ...` are NOT
    // ExprStmt(FireExpr), so they keep the handle for join. Not RC-gated: the
    // vthread refcount is independent of the object GC mode.
    if (impl_->lastValue && dynamic_cast<FireExpr*>(node.expr.get())) {
        llvm::Value* vt = impl_->lastValue;
        if (!vt->getType()->isPointerTy())
            vt = impl_->builder->CreateIntToPtr(vt, impl_->i8PtrType);
        impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_vthread_detach"], {vt});
        return;
    }

    // D017 Phase 4.B - freestanding TemplateExpr / TemplateFileExpr at
    // statement position inside a `!{ ... }` block-interp appends its
    // rendered string to the active block buffer instead of being treated
    // as a discarded value. Both `:{ ... }` aliases and explicit
    // `template { ... }` fragments work this way (spec §"Using `template {}`
    // instead (identical behavior)"). The buffer takes ownership of one
    // reference via dragon_list_append_ptr, so we skip the discarded-value
    // decref path below.
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

    // A discarded value-callee (closure) result is a PHI of owned calls - see
    // emitCallableValueCall, which merges the closure/bare/env call results -
    // not a CallInst, so the CallInst-keyed paths below would miss it and leak
    // it every call (e.g. a bare `render()` statement where `render` is a
    // `Callable[[], str]`). Release it here, gated on isOwnedStrResult (which
    // sees through the value-callee PHI) so a PHI from a borrowed source - a
    // conditional `a if c else b` over locals, say - is left untouched. The
    // element variant (str vs list/dict/instance) comes from the expr's
    // resolved type via inferPtrValueTag, exactly like the CallInst ptr path.
    if (llvm::isa<llvm::PHINode>(impl_->lastValue)) {
        auto* ty = impl_->lastValue->getType();
        if (ty == impl_->boxType) {
            // value-callee returning Any: release the box's +1 on its
            // refcounted payload (no-op by tag for non-heap payloads).
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

    // Decref discarded heap-typed results to prevent leaks (D020 Phase 4a).
    // Path A: pointer-typed return - decref by inferred tag.
    // Path B (Tier1 1.6): i64-typed return from pop-family runtime funcs -
    // these return a tagged value (heap pointer encoded as i64) that the
    // caller now owns. When the result is discarded, leak unless we
    // IntToPtr and decref by the container's element tag.
    auto* callInst = llvm::cast<llvm::CallInst>(impl_->lastValue);
    auto* retTy = callInst->getType();

    // Path C: discarded OWNED box result (`anyA + anyB` / `anyVal[i]` as a bare
    // statement). The box owns +1 on a refcounted payload; release it by tag.
    // Borrowed box reads are not owned (isOwnedBoxResult rejects them).
    if (retTy == impl_->boxType) {
        if (impl_->isOwnedBoxResult(impl_->lastValue)) {
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_box_decref"], {impl_->lastValue});
        }
        return;
    }

    if (retTy->isPointerTy()) {
        int64_t tag = impl_->inferPtrValueTag(node.expr.get());
        if (tag == 1) { // TAG_STR
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_decref_str"], {impl_->lastValue});
        } else if (tag == 5 || tag == 6 || tag == 7) { // TAG_LIST/DICT/BYTES
            impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_decref"], {impl_->lastValue});
        }
        return;
    }

    if (!retTy->isIntegerTy(64)) return;

    // Path B: discarded i64 return from a pop-family call.
    // Allowlist of runtime functions that return owning i64 heap values.
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

    // Find the container expression in the AST so we can extract elem_tag.
    auto* call = dynamic_cast<CallExpr*>(node.expr.get());
    if (!call) return;
    auto* attr = dynamic_cast<AttributeExpr*>(call->callee.get());
    if (!attr || !attr->object) return;

    int64_t elemTag = 0;
    Expr* container = attr->object.get();
    if (container->type) {
        if (auto* lt = dynamic_cast<ListType*>(container->type.get())) {
            // list[T] / set[T] (Set is represented as ListType in TypeChecker)
            if (lt->elementType)
                elemTag = Impl::typeKindToElemTag(lt->elementType->kind());
        } else if (auto* dt = dynamic_cast<DictType*>(container->type.get())) {
            // dict.pop returns the value
            if (dt->valueType)
                elemTag = Impl::typeKindToElemTag(dt->valueType->kind());
        }
    }

    // Primitive elem (Int=0, Float=2, Bool=3, None=4) - no decref needed.
    // Heap tags: Str=1, List=5, Dict=6, Bytes=7.
    if (elemTag != 1 && elemTag != 5 && elemTag != 6 && elemTag != 7) return;

    // Convert i64 -> ptr and decref using the proper variant.
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
    // Detect isinstance() narrowing for union-typed variables.
    // Returns (varName, narrowedKind) if condition is isinstance(x, T) where x is Union.
    // Also handles `x != none` for `T | None` unions: narrows x to the non-None
    // member inside the then-branch.
    auto detectNarrowing = [this](Expr* cond) -> std::pair<std::string, Impl::VarKind> {
        // Pattern: `union_var != none` (or symmetric) -> narrow to the
        // single non-None member of a 2-member union.
        if (auto* bin = dynamic_cast<BinaryExpr*>(cond)) {
            auto op = bin->op.type();
            // `x != None` / `x is not None` -> THEN sees the non-None member.
            // `x == None` / `x is None` -> ELSE sees it; THEN is None, so we
            //  return the Other sentinel which skips THEN narrowing while
            //  computeElseKind() still narrows the ELSE branch.
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
                        // The non-`Other` member is the narrowed kind. (`None`
                        // maps to VarKind::Other via typeExprToKind.)
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
        // Map type name to VarKind
        Impl::VarKind nk = Impl::VarKind::Other;
        if (typeName->name == "int")        nk = Impl::VarKind::Int;
        else if (typeName->name == "float") nk = Impl::VarKind::Float;
        else if (typeName->name == "bool")  nk = Impl::VarKind::Bool;
        else if (typeName->name == "str")   nk = Impl::VarKind::Str;
        else if (typeName->name == "bytes") nk = Impl::VarKind::List;  // D030 §5: bytes/list share generic-heap dispatch
        else if (typeName->name == "list") {
            // Narrowing a DECLARED union to its concrete list member keeps
            // the native-list rebind - the member type fixes the layout. A
            // bare Any gives no layout guarantee (a tag-5 payload may be a
            // monomorphized DragonList or a DragonListBox), so KEEP THE BOX
            // binding: len / subscript / iteration on it runtime-dispatch on
            // the payload header instead of walking the wrong stride. An Any
            // param also registers a unionMemberKinds entry, so the gate is
            // "the declared members INCLUDE a list", not mere presence.
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

    // Helper: compute the "else" narrowed kind for a 2-member union
    auto computeElseKind = [this](const std::string& varName, Impl::VarKind matchedKind) -> Impl::VarKind {
        auto membIt = impl_->unionMemberKinds.find(varName);
        if (membIt == impl_->unionMemberKinds.end()) return Impl::VarKind::Union;
        auto& members = membIt->second;
        if (members.size() == 2) {
            // Return the other member
            return (members[0] == matchedKind) ? members[1] : members[0];
        }
        return Impl::VarKind::Union;  // 3+ members: stay union in else
    };

    // The class an `isinstance(x, SomeClass)` condition narrows to, or "" if the
    // condition is not a class-arm isinstance. detectNarrowing returns only the
    // VarKind (ClassInstance) - the NAME is needed so the narrowed `x.method()`
    // dispatches directly to `SomeClass`'s method instead of falling to the
    // "class unknown" vtable path, which guesses the callee from an arbitrary
    // same-named method (wrong dispatch / wrong-arity IR when classes collide).
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

    // Record a narrowed class name (+owning module) for the branch body and
    // return a restorer. varClassNames/varClassOwningModule are program-wide
    // and NOT scope-managed (unlike the VarKind set via setVar), so the entry
    // must be restored when the narrowed branch ends or it would leak onto the
    // union var after the block.
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

    // Determine elif/else blocks
    std::vector<llvm::BasicBlock*> elifBlocks;
    for (size_t i = 0; i < node.elifClauses.size(); ++i) {
        elifBlocks.push_back(
            llvm::BasicBlock::Create(*impl_->context, "elif", func));
    }
    llvm::BasicBlock* elseBB = nullptr;
    if (!node.elseBody.empty()) {
        elseBB = llvm::BasicBlock::Create(*impl_->context, "else", func);
    }

    // First branch
    llvm::BasicBlock* nextBB = elifBlocks.empty()
        ? (elseBB ? elseBB : mergeBB) : elifBlocks[0];
    impl_->builder->CreateCondBr(cond, thenBB, nextBB);

    // Then body - with narrowing if isinstance detected.
    // D030 Phase 4: extract the box's payload at the narrowed native type
    // and shadow the union local with a typed alloca for the narrowed scope.
    // Inside `if isinstance(x, int):`, x reads as native i64 (not a box).
    impl_->builder->SetInsertPoint(thenBB);
    impl_->pushScope();  // each branch is its own block scope
    // narrowKind == Other is the "no THEN narrowing" sentinel (e.g. `x is None`,
    // where only the ELSE branch narrows). Skip extraction in that case.
    if (!narrowVar.empty() && narrowKind != Impl::VarKind::Other) {
        // Resolve the storage: local alloca first, then module global. Without
        // the global fallback, narrowing for module-level union vars (the common
        // case for `const r: T | None = ...`) silently fails.
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
            // Mark the narrowed binding as borrowed - payload lives in the box,
            // we don't own a separate refcount on it.
            impl_->scopes.back().borrowed.insert(narrowVar);
        } else if (localAlloca) {
            impl_->setVar(narrowVar, localAlloca, narrowKind);
        }
    }
    // Record the narrowed class name so `x.method()` in the then-body dispatches
    // directly to the isinstance'd class (restored after the branch).
    auto restoreThenNarrow = applyClassNarrow(
        narrowVar, narrowKind == Impl::VarKind::ClassInstance
                       ? narrowClassName(node.condition.get()) : std::string());
    for (auto& stmt : node.thenBody) stmt->accept(*this);
    impl_->emitScopeCleanup();
    impl_->popScope();
    restoreThenNarrow();
    // Did the then-branch unconditionally terminate (return/raise/...)? If so it
    // does NOT fall through to the merge, which enables fall-through narrowing
    // below (the Python early-return idiom).
    bool thenTerminated = impl_->builder->GetInsertBlock()->getTerminator() != nullptr;
    if (!thenTerminated)
        impl_->builder->CreateBr(mergeBB);

    // Elif bodies - each can have its own narrowing
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
        impl_->pushScope();  // each elif branch is its own block scope
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

    // Else body - narrow to remaining type for 2-member unions
    if (elseBB) {
        impl_->builder->SetInsertPoint(elseBB);
        impl_->pushScope();  // else branch is its own block scope
        if (!narrowVar.empty()) {
            auto elseKind = computeElseKind(narrowVar, narrowKind);
            if (elseKind != Impl::VarKind::Union) {
                // Resolve storage: local alloca first, then module global -
                // mirroring the then-branch. Without the global fallback,
                // else-narrowing for module-level union vars silently failed
                // and `v` stayed a box (e.g. v.upper() called on the box ->
                // LLVM verify error).
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

    // Fall-through narrowing: when the then-branch unconditionally terminates and
    // there is no elif/else, the merge is reached ONLY via the condition being
    // false - so `x` is narrowed to the else-type for the rest of THIS (enclosing)
    // scope. This is the Python early-return idiom:
    //  if isinstance(x, int) { return ... } # x is str from here on
    //  return f"{x.upper()}"
    // Mirrors the else-branch payload extraction, but binds into the enclosing
    // scope (the then/else scopes were already popped).
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
    // `while ... else` (Python): the else block runs IFF the loop completed
    // normally (condition became false) and was NOT exited via `break`. The
    // natural-completion edge from the condition targets elseBB; `break`
    // keeps targeting endBB (breakBlock below stays endBB) so it bypasses
    // else. With no else clause behavior is unchanged - completion goes
    // straight to endBB.
    llvm::BasicBlock* elseBB = node.elseBody.empty()
        ? endBB
        : llvm::BasicBlock::Create(*impl_->context, "whileelse", func);

    impl_->loopStack.push({endBB, condBB, impl_->scopes.size(), impl_->tryFrameFuncs.size(), impl_->exitCleanupStack.size()});
    impl_->builder->CreateBr(condBB);

    // Condition - false falls through to the else block (or endBB when absent).
    impl_->builder->SetInsertPoint(condBB);
    node.condition->accept(*this);
    llvm::Value* cond = impl_->toBool(impl_->lastValue, node.condition.get());
    impl_->builder->CreateCondBr(cond, bodyBB, elseBB);

    // Body - its own block scope; heap locals are cleaned each iteration,
    // before the back-edge to the condition.
    impl_->builder->SetInsertPoint(bodyBB);
    impl_->pushScope();
    for (auto& stmt : node.body) stmt->accept(*this);
    impl_->emitScopeCleanup();
    impl_->popScope();
    if (!impl_->builder->GetInsertBlock()->getTerminator())
        impl_->builder->CreateBr(condBB);

    // Pop the loop context BEFORE emitting the else body - a `break` inside
    // the else would belong to an enclosing loop, not this one, matching
    // Python scoping (the else is outside the loop proper).
    impl_->loopStack.pop();

    // Else body - reached only on natural completion. Own block scope, then
    // fall through to endBB.
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
    // In generator body functions, return always emits RetVoid
    // (the trampoline marks the generator as exhausted after body returns)
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
        // `-> T | None` (Union / Any) lowers to a `{i64, i64}` box. When the
        // return expression is one of the union members (or `none`), widen by
        // tagging and packing into a box so the LLVM-level types line up.
        //
        // D039 Phase 8 fix: when the wrapped payload is a heap pointer
        // (string, list, dict, class, bytes), it's about to be scope-cleanup-
        // decref'd. Incref the NATIVE pointer here, before wrapping, so the
        // caller's box owns its own +1 ref. The downstream "return path
        // incref" only sees the box (struct value) and can't reach the
        // payload - so the incref must happen at the wrap site.
        bool retBoxWrapped = false;
        if (retType == impl_->boxType && retVal->getType() != impl_->boxType) {
            auto* tag = impl_->emitTagForExpr(node.value.get(), *this);
            // Incref the heap payload (if any) BEFORE wrapping, while it's
            // still in its native ptr form. The wrap discards type info that
            // the incref dispatch needs.
            // Gated on BORROWED payloads only (`return name` / `return
            // self.f` / `return d[k]` - scope cleanup or the container owns
            // the +1). An OWNED temp (str() / concat / slice / literal /
            // call result) already carries the +1 the caller's box takes;
            // increffing it too leaks one object per call (
            // `def f() -> Any { return str(42) }`). IR-level predicates, not
            // AST: a ternary of locals merges in a PHI, which the predicates
            // conservatively treat as a borrow - incref'd, never dangling.
            if (impl_->options.gcMode == GCMode::RC &&
                retVal->getType()->isPointerTy()) {
                if (auto* tagConst = llvm::dyn_cast<llvm::ConstantInt>(tag)) {
                    int64_t t = tagConst->getSExtValue();
                    if (t == 1) {  // TAG_STR
                        if (!impl_->isOwnedStrResult(retVal))
                            impl_->builder->CreateCall(
                                impl_->runtimeFuncs["dragon_incref_str"], {retVal});
                    } else if (t == 5 || t == 6 || t == 7) {
                        // TAG_LIST / TAG_DICT / TAG_BYTES/CLASS - heap-tagged
                        if (!impl_->isOwnedPtrResult(retVal))
                            impl_->builder->CreateCall(
                                impl_->runtimeFuncs["dragon_incref"], {retVal});
                    }
                }
            }
            retVal = impl_->makeBox(tag, retVal);
            retBoxWrapped = true;
        } else if (retVal->getType() == impl_->boxType && retType != impl_->boxType) {
            // D039: returning an Any (box) from a function with a CONCRETE
            // return type (e.g. `return d[k]` on a dict[str, Any] from a
            // `-> str`). coerceArg can't unbox a {tag, payload} box, so do it
            // here, mirroring the unbox-on-assign path (Assign.cpp). A BORROW
            // box payload (dragon_dict_get_box contract) is incref'd by runtime
            // tag so the caller receives an owning ref; an OWNED box temporary
            // (box_binop / box_subscript) already carries that +1, so taking its
            // payload as the return value must NOT incref again (else it leaks).
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
            retBoxWrapped = true;  // RC handled here - skip the generic incref below
        } else if (retType == impl_->boxType && retVal->getType() == impl_->boxType) {
            // Returning an already-boxed value from an Any/Union function. Make
            // the returned box OWNED so callers can take ownership uniformly
            // (isOwnedBoxResult treats any box-returning call as owned). An owned
            // producer (box_binop / box_subscript / another owned call) already
            // carries the +1; a BORROW - a box local about to be scope-cleanup-
            // decref'd, or a dict/list element (dragon_dict_get_box) - must be
            // incref'd here, else the payload is freed out from under the caller
            // (use-after-free) or the caller's take-ownership double-frees it.
            if (impl_->options.gcMode == GCMode::RC &&
                !impl_->isOwnedBoxResult(retVal)) {
                auto* tag = impl_->boxTag(retVal, "ret.tag");
                auto* payloadI64 = impl_->boxPayloadI64(retVal, "ret.payload");
                impl_->emitUnionIncref(payloadI64, tag);
            }
            retBoxWrapped = true;  // RC handled here - skip the generic incref below
        } else if (retVal->getType() != retType)
            retVal = impl_->coerceArg(retVal, retType);
        // GC: if returning a local heap-kinded variable, incref it first so
        // scope cleanup doesn't free it. Only incref when we KNOW the value
        // has a DragonObjectHeader (isHeapKind). StrLiteral and other
        // ptr-typed values without headers must NOT be incref'd.
        // Phase 2: Str uses dragon_incref_str (data ptr -> header).
        // Phase 3: ClassInstance uses dragon_incref (pointer IS the header).
        // D039 Phase 8: skip when we already incref'd at the box-wrap site -
        // retVal there is the {tag, payload} struct, not a heap pointer, and
        // the incref already protected the payload's lifetime.
        if (impl_->options.gcMode == GCMode::RC && !retBoxWrapped) {
            // Incref borrowed heap values before scope cleanup.
            // NameExpr: local variable about to be decref'd by scope cleanup.
            // AttributeExpr: field borrowed from an object.
            auto increfIfHeap = [&](Impl::VarKind kind) {
                if (Impl::isHeapKind(kind)) {
                    impl_->emitIncrefByKind(retVal, kind);
                }
            };
            if (auto* nameExpr = dynamic_cast<NameExpr*>(node.value.get())) {
                increfIfHeap(impl_->lookupVarKind(nameExpr->name));
            } else if (auto* attrExpr = dynamic_cast<AttributeExpr*>(node.value.get())) {
                // Field access returns a borrowed reference - incref to keep alive.
                // Look up the field's VarKind from classFieldKinds for precise dispatch.
                if (auto* objName = dynamic_cast<NameExpr*>(attrExpr->object.get())) {
                    auto objKind = impl_->lookupVarKind(objName->name);
                    if (objKind == Impl::VarKind::ClassInstance) {
                        // Determine the class name for field kind lookup
                        std::string className;
                        if (objName->name == "self" && !impl_->currentClassName.empty()) {
                            className = impl_->currentClassName;
                        } else {
                            auto it = impl_->varClassNames.find(objName->name);
                            if (it != impl_->varClassNames.end()) className = it->second;
                        }
                        // Look up the specific field's VarKind (walk inheritance chain)
                        Impl::VarKind fieldKind = Impl::VarKind::Other;
                        for (std::string cls = className; !cls.empty(); ) {
                            auto fkIt = impl_->classFieldKinds.find(cls);
                            if (fkIt != impl_->classFieldKinds.end()) {
                                auto fieldIt = fkIt->second.find(attrExpr->attribute);
                                if (fieldIt != fkIt->second.end()) {
                                    fieldKind = fieldIt->second;
                                    break;
                                }
                            }
                            auto parentIt = impl_->classParentNames.find(cls);
                            if (parentIt != impl_->classParentNames.end())
                                cls = parentIt->second;
                            else
                                break;
                        }
                        if (Impl::isHeapKind(fieldKind)) {
                            impl_->emitIncrefByKind(retVal, fieldKind);
                        }
                    }
                }
            } else if (auto* subExpr = dynamic_cast<SubscriptExpr*>(node.value.get())) {
                // Returning a subscript value (`return d[k]`, `return lst[i]`,
                // `return _MIME_TYPES[ext]`) yields a *borrowed* reference into
                // the container. The caller is expected to receive an owning
                // ref (calling-convention contract), so we must incref before
                // returning. Without this, the caller can store the value into
                // another container that "steals" the +1 - when the caller's
                // container is freed it decrefs the value, drops the source
                // container's reference too, and the source ends up holding a
                // dangling pointer (use-after-free on the next read).
                //
                // EXCEPT a SLICE (`return s[a:b]`, `return xs[a:b]`): that lowers
                // to dragon_{str,list,bytes}_slice, which returns a FRESH +1
                // object the caller already owns. Increffing it makes +2 and
                // leaks one object per call (e.g. os.path.basename's
                // `return path[idx+1:]` leaked its result every call). Mirror
                // isBorrowedHeapExpr's slice carve-out: only an element read
                // borrows; a slice is owned.
                //
                // AND EXCEPT a STRING element read (`return s[i]`): strings are
                // immutable, so dragon_str_index mallocs a fresh 1-char string -
                // owned exactly like a slice, with no container holding a
                // reference. isBorrowedHeapExpr has carried this carve-out for
                // a while; this return path predated it, so every
                // `def peek() -> str { return self.src[i] }` came back +2 and
                // leaked one string per call (the GraphQL parser hot loop found
                // it). list/dict element returns keep the incref: those really
                // do borrow the container's reference.
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
    // Create dead block for any subsequent code in the enclosing block
    // (e.g., dragon_exc_pop_frame after a return inside a try body)
    auto* deadBB = llvm::BasicBlock::Create(
        *impl_->context, "ret.dead", impl_->currentFunction);
    impl_->builder->SetInsertPoint(deadBB);
}

void CodeGen::visit(BreakStmt&) {
    if (!impl_->loopStack.empty()) {
        // Pop only the try/with frames opened inside the loop body (those above
        // the count recorded at loop entry) - the jump lands at breakBlock,
        // still inside any try enclosing the loop.
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
    // No-op
}

// defer <call> (defer.md): arguments and the method receiver evaluate HERE
// into an i64 snapshot array; only the call runs at scope exit. The entry
// appended to the current Scope is emitted by emitScopeCleanupFor on every
// normal exit edge (LIFO, before the RC decref pass), and the runtime
// cleanup-stack entries registered below make the same thunk run during a
// longjmp unwind. Zero heap either way: the snapshot array is an entry
// alloca and the unwind entries live on the preallocated cleanup stack.
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
    if (impl_->scopes.empty()) return;  // module-level: Sema already refused
    if (!call->kwArgs.empty()) {
        impl_->addError("defer does not support keyword arguments yet; "
                        "pass them positionally", node.location());
        return;
    }

    // Resolve the callee to a direct function, mirroring fire's call form.
    llvm::Function* targetFn = nullptr;
    std::string calleeName;
    bool isMethodCall = false;
    int deferVtIdx = -1;         // >= 0: dispatch through the vtable (D026)
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
                    auto cmIt = impl_->classOwningModule.find(className);
                    if (cmIt != impl_->classOwningModule.end())
                        owningModule = cmIt->second;
                }
            }
        }
        // Fall back to the receiver's static type so nested bases
        // (`defer a.b.close()`) resolve instead of silently missing.
        if (className.empty() && attrExpr->object->type) {
            if (auto* inst = dynamic_cast<InstanceType*>(
                    attrExpr->object->type.get())) {
                if (inst->classType) className = inst->classType->name;
            }
            if (!className.empty()) {
                auto cmIt = impl_->classOwningModule.find(className);
                if (cmIt != impl_->classOwningModule.end())
                    owningModule = cmIt->second;
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
                // D026 parity: a deferred method must dispatch exactly like
                // the direct call would. The receiver's STATIC class resolved
                // the symbol, but the runtime value may be a subclass; when
                // any subclass overrides this method, the thunk goes through
                // the receiver's vtable, or `defer conn.close()` on a
                // ConnReader would silently run the base no-op.
                if (impl_->methodIsOverridden(className, attrExpr->attribute)) {
                    auto idxIt =
                        impl_->classMethodVtableIndices.find(className);
                    if (idxIt != impl_->classMethodVtableIndices.end()) {
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

    // Evaluate every argument NOW (snapshot rule) and classify what the
    // snapshot slot owns. drainKind != Other means the slot holds a +1
    // released after the deferred call; own-moved values ride into the
    // callee and drain nothing.
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
        if (nm && nm->isMoveMarked) continue;  // own: callee adopts the +1
        Impl::VarKind tk = e->type
            ? Impl::typeKindToVarKind(e->type->kind())
            : Impl::VarKind::Other;
        if (tk == Impl::VarKind::Union) {
            impl_->addError(
                "defer: an argument of Any/union type is not supported yet; "
                "annotate the concrete type", node.location());
            return;
        }
        if (nm && nm->isDubMarked) {  // dub: the eval minted an owned copy
            drainKinds[i] = tk;
            continue;
        }
        Impl::VarKind owned = impl_->ownedTempDrainKind(e, vals[i]);
        if (owned != Impl::VarKind::Other) {
            drainKinds[i] = owned;  // owned temp: the slot adopts it
            continue;
        }
        if (Impl::isHeapKind(tk)) {
            // Borrowed heap value. A rodata literal has no header to count -
            // dup it to a refcounted copy; anything else takes a +1 so a
            // rebind of the source name cannot strand the snapshot.
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

    // Fill omitted trailing args from the callee's defaults, evaluated here
    // (snapshot semantics hold for defaults too). Fresh heap defaults are
    // owned temps the matching slot adopts.
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

    // Coerce to the callee's exact param types and snapshot as i64s.
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

    // `defer f(own x)`: the snapshot now carries the moved +1 - null the
    // binding's slot and its unwind-stack entry so neither path frees what
    // the deferred callee will adopt.
    impl_->emitMoveOutSlots(*call);

    // Register the unwind entries, gated on a live exception frame exactly
    // like every other cleanup push (no frame live here means no handler
    // exists that an unwind crossing this scope could reach). One entry per
    // snapshot value with its drain kind, then the thunk entry on top; the
    // scope's normal-exit rewind pops them unexecuted.
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

    // An owned +1 message temp (concat / str() / f-string) transfers its
    // reference into the exc slot via the consume variant - the raise
    // longjmps, so the caller can never release it. Borrowed reads and
    // literals go through the plain raise (the slot dups mortal heap msgs).
    auto emitRaise = [&](llvm::Value* typeVal, llvm::Value* msgVal) {
        bool owned = impl_->options.gcMode == GCMode::RC &&
                     impl_->isOwnedStrResult(msgVal);
        // A constant ptr message is a rodata C literal (CreateGlobalString);
        // heap DragonStrings always arrive via loads/calls. Literals go
        // through the cstr raise so the runtime wraps them in a heap
        // DragonString - the exc_msg slot must never hold a raw literal
        // (its header probe would read 24 bytes before the rodata).
        bool literal = !owned && llvm::isa<llvm::Constant>(msgVal);
        auto* raiseFn = impl_->runtimeFuncs[owned   ? "dragon_raise_exc_consume"
                                            : literal ? "dragon_raise_exc_cstr"
                                                      : "dragon_raise_exc"];
        impl_->builder->CreateCall(raiseFn, {typeVal, msgVal});
        impl_->builder->CreateUnreachable();
        // Create dead block for any subsequent code (e.g., print after raise in try body)
        auto* deadBB = llvm::BasicBlock::Create(*impl_->context, "raise.dead", func);
        impl_->builder->SetInsertPoint(deadBB);
    };

    // Re-raise the in-flight exception preserving its ORIGINAL type + message
    // + instance. The runtime keeps all three in TLS (or the vthread struct),
    // still valid while a handler body runs. Used for bare `raise` and
    // `raise <handler-var>` - the handler variable only carries one of msg/
    // instance, so the full state must come from the runtime slots, not from
    // the variable. Always route through dragon_raise_exc_obj so a typed-
    // field exception keeps its instance through nested handlers (obj=NULL
    // for built-in raises makes this equivalent to dragon_raise_exc).
    auto emitReraiseCurrent = [&]() {
        auto* t = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_exc_get_type"], {}, "reraise.type");
        // The obj-raise consumes a +1; this re-raise borrows the slot's own
        // pointer - retain first, the same-pointer fold nets it out.
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
        // Check for pattern: raise ValueError("msg")
        if (auto* call = dynamic_cast<CallExpr*>(node.exception.get())) {
            if (auto* name = dynamic_cast<NameExpr*>(call->callee.get())) {
                int64_t typeCode = impl_->excTypeCode(name->name);
                auto* typeVal = llvm::ConstantInt::get(impl_->i64Type, typeCode);

                // User-defined exception class with typed-field ctor: construct
                // the instance via its __init__ and route through the obj-aware
                // raise. Without this, `raise HTTPError(404, "url")` would
                // lower the int 404 straight into `dragon_raise_exc(i64, ptr
                // msg)` - type mismatch, LLVM verifier failure - and the user
                // ctor would never run, so e.code / e.url / e.reason would
                // not survive unwinding. Built-in exceptions (ValueError,
                // OSError, ...) keep the message-only path: they have no user-
                // typed ctor and their first arg is, by convention, the msg.
                bool isUserExc = impl_->userExcCodes.count(name->name) > 0;
                auto* initFn = isUserExc
                    ? impl_->module->getFunction(
                          impl_->classSymPrefix(name->name) + "___init__")
                    : nullptr;
                // Arity mismatch (e.g. `raise EmptyError()` against a ctor
                // declared `def(msg: str)`) falls through to the message-only
                // path: invoking the ctor with the wrong arity produces an
                // LLVM verifier failure. Historical behavior silently treated
                // such raises as built-in-style (type + message-or-classname);
                // preserve that. The user-class type code still routes to a
                // matching `except UserClass as e` handler - `e` just binds
                // to the message string rather than an instance.
                bool arityMatches = false;
                if (isUserExc && initFn) {
                    // initFn signature: (self, args...). User args = numParams - 1.
                    // Exact match always works; a short call works when every
                    // missing user arg has a default. Defaults for the ctor are
                    // stored on the matching `<clsSym>_new` symbol (ImplInit.cpp
                    // line 1451), NOT on the `___init__` symbol, and the vector
                    // is keyed by user-arg position (no self slot). visit(CallExpr)
                    // calls fillDefaultArgs for the ctor before invoking it.
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
                    // Derive a runtime `msg` for the obj-raise. Two reasons
                    // we don't just punt to the class name:
                    //  1. `print(e)` for an exception instance routes through
                    //  the runtime msg slot (see emitPrintArg) so single-
                    //  string-arg ctors stay Python-shaped (`AppError("x")`
                    //  -> str(e) = "x").
                    //  2. Uncaught raises still print a useful message at the
                    //  process-exit fallback in dragon_raise_exc_obj.
                    // We snapshot the first arg ONLY when it's a pure-eval
                    // expression (StringLiteral or NameExpr), so that visiting
                    // the CallExpr below re-evaluates it without observable
                    // side effects. Anything else (CallExpr arg, f-string,
                    // arithmetic) falls back to the class name to keep raise
                    // sites side-effect-faithful.
                    llvm::Value* msgVal = nullptr;
                    if (!call->args.empty()) {
                        Expr* a0 = call->args[0].get();
                        bool pure = false;
                        if (auto* sl = dynamic_cast<StringLiteral*>(a0))
                            pure = !sl->isBytes && !sl->isFString;
                        else if (dynamic_cast<NameExpr*>(a0))
                            pure = true;
                        if (pure) {
                            a0->accept(*this);
                            llvm::Value* v = impl_->lastValue;
                            // A NameExpr can resolve to non-string scalars
                            // (HTTPError(code, url) - first arg is int);
                            // only adopt as msg when the resulting value is a
                            // ptr-shaped string.
                            if (v && v->getType()->isPointerTy())
                                msgVal = v;
                        }
                    }
                    if (!msgVal)
                        msgVal = impl_->builder->CreateGlobalString(name->name);
                    // Evaluate the call expression as a normal constructor:
                    // visit(CallExpr) handles malloc + gc_track + __init__ call
                    // for any user class. The result is the instance pointer
                    // with refcount=1, which the obj-aware raise treats as an
                    // owned ref handed off to the matched handler's binding.
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

        // `raise e` where `e` is an enclosing handler's bound exception variable:
        // re-raise the in-flight exception with its ORIGINAL type. Without this
        // the type would be lost (the var holds only the message), so an
        // enclosing `except <OriginalType>` could not match it.
        if (auto* nameRef = dynamic_cast<NameExpr*>(node.exception.get())) {
            for (auto& v : impl_->handlerExcVars) {
                if (v == nameRef->name) {
                    emitReraiseCurrent();
                    return;
                }
            }
            // `raise <var>` where var holds a user-exception instance built
            // earlier (`saved: HTTPError = HTTPError(...); raise saved`).
            // Route through the obj-aware raise so the handler's `except
            // <UserClass> as e` binding gets the real instance (typed-field
            // access works) AND the correct type code routes through the
            // parent-class catch-range (so `except URLError` matches an
            // HTTPError instance). The static class is the source of truth
            // for the type code; if the instance is polymorphic (e.g. a
            // subclass stored under a parent annotation), the parent code's
            // [lo,hi] range still covers the runtime subclass.
            auto cnIt = impl_->varClassNames.find(nameRef->name);
            if (cnIt != impl_->varClassNames.end() &&
                impl_->userExcCodes.count(cnIt->second) > 0) {
                int64_t typeCode = impl_->excTypeCode(cnIt->second);
                auto* typeVal = llvm::ConstantInt::get(impl_->i64Type, typeCode);
                node.exception->accept(*this);
                llvm::Value* inst = impl_->lastValue;
                if (!inst->getType()->isPointerTy())
                    inst = impl_->builder->CreateIntToPtr(inst, impl_->i8PtrType);
                else if (inst->getType() != impl_->i8PtrType)
                    inst = impl_->builder->CreateBitCast(inst, impl_->i8PtrType);
                auto* msgVal = impl_->builder->CreateGlobalString(cnIt->second);
                // The instance is BORROWED from the local's slot; the raise
                // transfers a +1 into the owning exc_obj slot, so retain
                // first. The local's own ref is then correctly freed by the
                // unwind / scope cleanup.
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

        // raise with some other non-call expression (rare: raising a value).
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

    // Bare `raise`: re-raise the current in-flight exception (Python semantics),
    // not a hardcoded SystemExit.
    emitReraiseCurrent();
}
void CodeGen::visit(GlobalStmt& node) {
    // .py mode: register these names so AssignStmt/NameExpr use the module global
    for (auto& name : node.names) {
        impl_->globalDeclaredVars.insert(name);
    }
}
void CodeGen::visit(NonlocalStmt& node) {
    // .py mode: register these names as nonlocal (proxy via globals for now)
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
            // Decref the current value if it's a heap-allocated type
            if (impl_->options.gcMode == GCMode::RC) {
                auto kind = impl_->lookupVarKind(nameExpr->name);
                if (Impl::isHeapKind(kind)) {
                    auto* val = impl_->builder->CreateLoad(
                        alloca->getAllocatedType(), alloca, nameExpr->name + ".del");
                    // docs/002 ADR: OwnershipCheck PROVED this binding is the
                    // value's sole owner, so debug builds (-O0) assert
                    // rc == 1 right here - the executable form of the proof.
                    // A disagreement (codegen refcount bug, or a callee that
                    // retained a borrow) aborts naming this exact line.
                    // Release builds: the plain decref below, identical to
                    // scope exit, just earlier.
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
                            // A Callable slot may hold a bare fn ptr; the box
                            // variant's closure arm handles the header gate.
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
                // docs/002: del of a PROVEN Lock local destroys the OS
                // primitive through the same releaser the own-field dealloc
                // uses. A Lock is a raw pthread mutex (no refcount header),
                // so the rc==1 assert does not apply; compilation implies
                // the ownership proof (an escaped/borrowed Lock refuses).
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
            }
            // Store null/zero to the slot to prevent double-free on scope cleanup
            if (alloca->getAllocatedType() == impl_->boxType) {
                // Union slot: the decref above drained the payload by tag;
                // zero the whole {tag, payload} box so scope cleanup's own
                // Union drain sees tag 0 and no-ops instead of double-freeing.
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
        // del d[key] for dicts. Dispatches str- vs int-keyed exactly like the
        // subscript-store path in Assign.cpp. del lst[i] is routed to
        // dragon_list_delitem below; del x.attr remains unsupported (hard
        // error - never a silent no-op).
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
                // del lst[i] - list element deletion. The three monomorphized
                // list variants (I64/F64/Ptr) share layout, so a single
                // decref-aware runtime entry covers them. A non-list, non-dict
                // subscript (e.g. del x.attr) is a hard error, NOT a no-op:
                // silently dropping it once turned scheduler.run() into an
                // unbounded loop that exhausted RAM.
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
                // list[Any] is a DragonListBox (16B/elem) - it must NOT go
                // through dragon_list_delitem (8B shift scrambles the boxes).
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

            bool intKeyed = impl_->dictKeyIsInt(sub->object.get());
            sub->object->accept(*this);
            llvm::Value* dict = impl_->lastValue;
            sub->index->accept(*this);
            llvm::Value* key = impl_->lastValue;

            if (intKeyed) {
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
                // Release an owned str key TEMP used only to address the entry
                // (`del d[str(i)]`, `del d[a + b]`). The dict drops its OWN key
                // ref internally; this temp is a separate +1 the caller still
                // owns. Borrowed keys (a bare name/literal) are not owned, so
                // isOwnedStrResult leaves them alone. Pre-existing leak, surfaced
                // by the dict-delete ASan probe.
                if (impl_->options.gcMode == GCMode::RC &&
                    impl_->isOwnedStrResult(key)) {
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_decref_str"], {key});
                }
            }
        }
        // del adict.key - dict dot-access delete (.dr str-keyed dicts), the
        // delete mirror of `adict.key` reads. Equivalent to del adict["key"].
        // Resolves the same dict receivers as the read path in Attributes.cpp:
        // a Dict-kind NameExpr, a class field of Dict kind (self._d.k), or a
        // statically Dict-typed receiver. A genuine class instance has fixed
        // layout, so deleting one of its attributes is meaningless -> hard
        // error (never a silent no-op - that's what OOM'd sched.run()).
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
                            auto fkIt = impl_->classFieldKinds.find(cls);
                            if (fkIt != impl_->classFieldKinds.end()) {
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
                // dict dot-access is str-keyed by definition; the attribute
                // name is the key. dragon_dict_del raises KeyError on miss,
                // exactly like del adict["key"].
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
        // Any other target (del expr(), del literal) is unsupported - fail
        // loudly, never a silent no-op (that's what OOM'd sched.run()).
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
void CodeGen::visit(ImportStmt& node) {
    auto& registry = StdlibRegistry::instance();
    std::set<std::string> dummy;
    for (auto& alias : node.names) {
        // Skip StdlibRegistry for modules resolved as .dr/.py files
        if (impl_->fileResolvedModules.count(alias.name)) continue;
        registry.resolveImport(alias.name, alias.asName,
                               impl_->symbolAliases, dummy);
        impl_->importedModules.insert(alias.name);
    }
}

void CodeGen::visit(FromImportStmt& node) {
    // For file-resolved modules (`from os import listdir` etc.), the
    // imported names enter the importing module's scope under their bare
    // form, but the LLVM symbol they resolve to is per-module-mangled
    // (`os__listdir`). Record the alias so CallExpr's same-module lookup
    // can find the right symbol - without this the call falls through to
    // the bare name, which doesn't exist in mangled stdlib modules and
    // produces "Unknown function" diagnostics.
    if (impl_->fileResolvedModules.count(node.module)) {
        // Scoped to the IMPORTING module (currentModuleName) so a `from os
        // import listdir` in stdlib/tarfile.dr stays inside tarfile and
        // two modules importing the same bare name from different sources
        // don't clobber.
        auto& bucket = impl_->importedFuncAliasesByModule[impl_->currentModuleName];
        for (auto& alias : node.names) {
            const std::string& localName =
                alias.asName.empty() ? alias.name : alias.asName;
            std::string mangled = Impl::mangleFunc(node.module, alias.name);
            // `extern "C"` functions (e.g. `math.sqrt`) keep their bare C
            // symbol name rather than the per-module mangle. The non-aliased
            // import only resolved by coincidence (entry-module mangle of a
            // bare name == the bare name); an `as` rename broke that, so map
            // the alias to whichever symbol actually exists. All modules are
            // forward-declared before any body is emitted, so this probe is
            // valid here.
            if (impl_->module && !impl_->module->getFunction(mangled)) {
                std::string bare = Impl::userFuncName(alias.name);
                if (impl_->module->getFunction(bare)) mangled = bare;
            }
            bucket[localName] = mangled;

            // If the imported name is a CLASS defined by node.module, pin the
            // local name to that module for class resolution too. The
            // importedClassAliasesByModule map is consulted first by
            // resolveClassOwningModule but was never populated (only its func
            // twin was), so `from drs import Schema` fell through to the global
            // last-write-wins classOwningModule map. That let an unrelated
            // `from json import loads` in the same file make `Schema` resolve to
            // json.Schema whenever json.dr also declares a class Schema - a
            // silent miscompile (json__Schema_new(null) + no `field` method)
            // even though the developer imported Schema from exactly one place.
            // Probe the mangled ctor symbol (all classes are forward-declared
            // before any body is emitted, so this is valid here). Populated in
            // import order, so two explicit imports of the same local name are
            // last-write-wins - alias the second one to disambiguate.
            std::string classMangled = Impl::mangleClass(node.module, alias.name);
            if (impl_->module &&
                (impl_->module->getFunction(classMangled + "_new") ||
                 impl_->module->getFunction(classMangled + "_new_0") ||
                 impl_->module->getFunction(classMangled + "___init__") ||
                 impl_->module->getFunction(classMangled + "___init___0"))) {
                impl_->importedClassAliasesByModule[impl_->currentModuleName]
                    [localName] = node.module;
            }
        }
        return;
    }
    auto& registry = StdlibRegistry::instance();
    std::set<std::string> dummy;
    if (node.names.empty()) {
        registry.resolveFromImportStar(node.module,
                                       impl_->symbolAliases, dummy);
    } else {
        for (auto& alias : node.names) {
            registry.resolveFromImport(node.module, alias.name, alias.asName,
                                       impl_->symbolAliases, dummy);
        }
    }
}

//===----------------------------------------------------------------------===//
// Visitor: Declarations
//===----------------------------------------------------------------------===//

} // namespace dragon
