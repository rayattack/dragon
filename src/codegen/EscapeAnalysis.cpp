/// Escape analysis (B Phase 1): finds `v: T = T(args)` locals that never escape their
/// block, so they can stack-allocate instead of heap+refcount. Defaults to "escapes" when unsure.
#include "../CodeGenImpl.h"
#include <functional>

namespace dragon {

namespace {

/// Finds any NameExpr(name) mention in a subtree. Fallback for capture contexts and
/// node kinds the structural walk below doesn't special-case.
class NameMentionVisitor : public DefaultASTVisitor {
public:
    explicit NameMentionVisitor(std::string n) : target(std::move(n)) {}
    bool found = false;
    std::string target;
    void visit(NameExpr& node) override { if (node.name == target) found = true; }
    void visit(WalrusExpr& node) override {
        if (node.name == target) found = true;
        if (node.value) node.value->accept(*this);
    }
};

/// Task-detach transfer walk. True only on a real transfer (return/store/pass/capture/rebind);
/// await/join (consume, nulls the slot) and is_alive (read) are not transfers, so detach after them is safe. Kept separate from exprEscapes/stmtEscapes so it never weakens stack-alloc.
class TaskTransferVisitor : public DefaultASTVisitor {
public:
    explicit TaskTransferVisitor(std::string n) : target(std::move(n)) {}
    bool transferred = false;
    std::string target;

    bool isTarget(Expr* e) {
        auto* n = dynamic_cast<NameExpr*>(e);
        return n && n->name == target;
    }
    // Any mention of `target` in `node` (capture bodies: no carve-out).
    bool mentions(Expr& node) {
        NameMentionVisitor m(target); node.accept(m); return m.found;
    }
    bool mentions(Stmt& node) {
        NameMentionVisitor m(target); node.accept(m); return m.found;
    }

    void visit(NameExpr& n) override { if (n.name == target) transferred = true; }

    void visit(WalrusExpr& w) override {
        if (w.name == target) { transferred = true; return; }  // rebind: transfer
        if (w.value) w.value->accept(*this);
    }

    void visit(AwaitExpr& a) override {
        // `await t` consumes t (slot nulled after); not a transfer, don't count the
        // bare operand. `await other` recurses normally.
        if (isTarget(a.operand.get())) return;
        if (a.operand) a.operand->accept(*this);
    }

    void visit(CallExpr& c) override {
        // `t.join()` consumes (slot nulled after); `t.is_alive()` reads. Neither is a transfer.
        if (auto* attr = dynamic_cast<AttributeExpr*>(c.callee.get()))
            if (isTarget(attr->object.get()) && c.args.empty() && c.kwArgs.empty() &&
                (attr->attribute == "join" || attr->attribute == "is_alive"))
                return;  // do not descend into the `t` receiver
        if (c.callee) c.callee->accept(*this);
        for (auto& a : c.args) if (a) a->accept(*this);
        for (auto& kv : c.kwArgs) if (kv.second) kv.second->accept(*this);
    }

    // Capture contexts take the VARIABLE, so any mention inside (even is_alive) escapes
    // the handle. No carve-out; scan the whole body.
    void visit(LambdaExpr& n) override    { if (mentions(n)) transferred = true; }
    void visit(FireExpr& n) override      { if (mentions(n)) transferred = true; }
    void visit(GeneratorExpr& n) override { if (mentions(n)) transferred = true; }
    void visit(ThreadStmt& n) override    { if (mentions(n)) transferred = true; }
    void visit(FunctionDecl& n) override  { if (mentions(n)) transferred = true; }
};

} // namespace

bool CodeGen::Impl::nodeMentionsName(Expr* e, const std::string& name) {
    if (!e) return false;
    NameMentionVisitor v(name);
    e->accept(v);
    return v.found;
}

bool CodeGen::Impl::nodeMentionsName(Stmt* s, const std::string& name) {
    if (!s) return false;
    NameMentionVisitor v(name);
    s->accept(v);
    return v.found;
}

bool CodeGen::Impl::taskLocalTransferEscapes(Stmt* s, const std::string& name) {
    if (!s) return false;
    TaskTransferVisitor v(name);
    s->accept(v);
    return v.transferred;
}

bool CodeGen::Impl::exprEscapes(Expr* e, const std::string& name) {
    if (!e) return false;

    // `v.field` reads a scalar field (copies out, pointer doesn't flow): safe, don't descend
    // into bare `v`. A deeper object (`other.field`) recurses normally.
    if (auto* attr = dynamic_cast<AttributeExpr*>(e)) {
        if (auto* ne = dynamic_cast<NameExpr*>(attr->object.get()))
            if (ne->name == name) return false;
        return exprEscapes(attr->object.get(), name);
    }

    // `v.method(...)` escapes (method may retain self); otherwise recurse into callee and
    // args so a bare `v` passed as an arg is caught.
    if (auto* call = dynamic_cast<CallExpr*>(e)) {
        if (auto* cae = dynamic_cast<AttributeExpr*>(call->callee.get()))
            if (auto* ne = dynamic_cast<NameExpr*>(cae->object.get()))
                if (ne->name == name) return true;  // v.method(...)
        if (exprEscapes(call->callee.get(), name)) return true;
        for (auto& a : call->args)
            if (exprEscapes(a.get(), name)) return true;
        for (auto& kv : call->kwArgs)
            if (exprEscapes(kv.second.get(), name)) return true;
        return false;
    }

    // `v[i]` is __getitem__, a method call on the instance.
    if (auto* sub = dynamic_cast<SubscriptExpr*>(e)) {
        if (auto* ne = dynamic_cast<NameExpr*>(sub->object.get()))
            if (ne->name == name) return true;
        return exprEscapes(sub->object.get(), name) ||
               exprEscapes(sub->index.get(), name);
    }

    // Bare occurrence of `v` anywhere not whitelisted above -> escape.
    if (auto* ne = dynamic_cast<NameExpr*>(e))
        return ne->name == name;

    // Compound expressions (`v.x + v.y`, `[v.x]`, `{k: v.x}`) recurse and stay safe since
    // only scalar field values flow; a bare `v` inside still escapes.
    if (auto* be = dynamic_cast<BinaryExpr*>(e))
        return exprEscapes(be->left.get(), name) || exprEscapes(be->right.get(), name);
    if (auto* ue = dynamic_cast<UnaryExpr*>(e))
        return exprEscapes(ue->operand.get(), name);
    if (auto* ie = dynamic_cast<IfExpr*>(e))
        return exprEscapes(ie->condition.get(), name) ||
               exprEscapes(ie->thenExpr.get(), name) ||
               exprEscapes(ie->elseExpr.get(), name);
    if (auto* cc = dynamic_cast<ChainedCompExpr*>(e)) {
        for (auto& op : cc->operands)
            if (exprEscapes(op.get(), name)) return true;
        return false;
    }
    if (auto* sl = dynamic_cast<SliceExpr*>(e))
        return exprEscapes(sl->lower.get(), name) ||
               exprEscapes(sl->upper.get(), name) ||
               exprEscapes(sl->step.get(), name);
    if (auto* st = dynamic_cast<StarredExpr*>(e))
        return exprEscapes(st->value.get(), name);
    if (auto* le = dynamic_cast<ListExpr*>(e)) {
        for (auto& el : le->elements)
            if (exprEscapes(el.get(), name)) return true;
        return false;
    }
    if (auto* te = dynamic_cast<TupleExpr*>(e)) {
        for (auto& el : te->elements)
            if (exprEscapes(el.get(), name)) return true;
        return false;
    }
    if (auto* se = dynamic_cast<SetExpr*>(e)) {
        for (auto& el : se->elements)
            if (exprEscapes(el.get(), name)) return true;
        return false;
    }
    if (auto* de = dynamic_cast<DictExpr*>(e)) {
        for (auto& kv : de->entries) {
            if (exprEscapes(kv.first.get(), name)) return true;
            if (exprEscapes(kv.second.get(), name)) return true;
        }
        return false;
    }
    // Walrus binding to `v` rebinds the name: disqualify.
    if (auto* we = dynamic_cast<WalrusExpr*>(e)) {
        if (we->name == name) return true;
        return exprEscapes(we->value.get(), name);
    }

    // Capture/opaque contexts (lambda, fire, comprehensions, await/yield, templates) and
    // any unrecognized kind: closures capture the VARIABLE, so any mention of `v` escapes.
    return nodeMentionsName(e, name);
}

bool CodeGen::Impl::stmtEscapes(Stmt* s, const std::string& name) {
    if (!s) return false;

    if (auto* es = dynamic_cast<ExprStmt*>(s))
        return exprEscapes(es->expr.get(), name);

    if (auto* as = dynamic_cast<AssignStmt*>(s)) {
        // `v = ...` and `other[v] = ...` escape via the target walk; `v.field = ...` is a
        // safe scalar store. Also checks the RHS.
        for (auto& t : as->targets)
            if (exprEscapes(t.get(), name)) return true;
        return exprEscapes(as->value.get(), name);
    }
    if (auto* an = dynamic_cast<AnnAssignStmt*>(s)) {
        // `v: T = ...` rebinds/shadows v: escape.
        if (an->target && exprEscapes(an->target.get(), name)) return true;
        return an->value ? exprEscapes(an->value.get(), name) : false;
    }
    if (auto* aa = dynamic_cast<AugAssignStmt*>(s)) {
        if (aa->target && exprEscapes(aa->target.get(), name)) return true;
        return exprEscapes(aa->value.get(), name);
    }
    if (auto* r = dynamic_cast<ReturnStmt*>(s))
        return r->value ? exprEscapes(r->value.get(), name) : false;
    if (auto* rs = dynamic_cast<RaiseStmt*>(s))
        return exprEscapes(rs->exception.get(), name) ||
               exprEscapes(rs->cause.get(), name);

    if (auto* ifs = dynamic_cast<IfStmt*>(s)) {
        if (exprEscapes(ifs->condition.get(), name)) return true;
        for (auto& st : ifs->thenBody) if (stmtEscapes(st.get(), name)) return true;
        for (auto& [cond, body] : ifs->elifClauses) {
            if (exprEscapes(cond.get(), name)) return true;
            for (auto& st : body) if (stmtEscapes(st.get(), name)) return true;
        }
        for (auto& st : ifs->elseBody) if (stmtEscapes(st.get(), name)) return true;
        return false;
    }
    if (auto* w = dynamic_cast<WhileStmt*>(s)) {
        if (exprEscapes(w->condition.get(), name)) return true;
        for (auto& st : w->body) if (stmtEscapes(st.get(), name)) return true;
        for (auto& st : w->elseBody) if (stmtEscapes(st.get(), name)) return true;
        return false;
    }
    if (auto* f = dynamic_cast<ForStmt*>(s)) {
        // A for-target binding `v` shadows it: escape.
        if (exprEscapes(f->target.get(), name)) return true;
        if (exprEscapes(f->iterable.get(), name)) return true;
        for (auto& st : f->body) if (stmtEscapes(st.get(), name)) return true;
        for (auto& st : f->elseBody) if (stmtEscapes(st.get(), name)) return true;
        return false;
    }
    if (auto* t = dynamic_cast<TryStmt*>(s)) {
        for (auto& st : t->tryBody) if (stmtEscapes(st.get(), name)) return true;
        for (auto& h : t->handlers) {
            if (h.name == name) return true;  // `except E as v` rebinds v
            for (auto& st : h.body) if (stmtEscapes(st.get(), name)) return true;
        }
        for (auto& st : t->elseBody) if (stmtEscapes(st.get(), name)) return true;
        for (auto& st : t->finallyBody) if (stmtEscapes(st.get(), name)) return true;
        return false;
    }
    if (auto* ws = dynamic_cast<WithStmt*>(s)) {
        for (auto& it : ws->items) {
            if (exprEscapes(it.contextExpr.get(), name)) return true;
            if (it.optionalVars && exprEscapes(it.optionalVars.get(), name))
                return true;  // `with x as v` rebinds v
        }
        for (auto& st : ws->body) if (stmtEscapes(st.get(), name)) return true;
        return false;
    }
    if (auto* as2 = dynamic_cast<AssertStmt*>(s))
        return exprEscapes(as2->test.get(), name) ||
               (as2->msg ? exprEscapes(as2->msg.get(), name) : false);

    // A nested def/thread body naming `v` captures it: escape (lambda/fire caught at
    // expr level).
    if (auto* fd = dynamic_cast<FunctionDecl*>(s))
        return nodeMentionsName(fd, name);
    if (auto* th = dynamic_cast<ThreadStmt*>(s))
        return nodeMentionsName(th, name);

    // Rare/opaque kinds (match, delete, global/nonlocal, class) and anything else fall
    // back to the exhaustive mention probe: any appearance of v is an escape.
    if (dynamic_cast<BreakStmt*>(s) || dynamic_cast<ContinueStmt*>(s) ||
        dynamic_cast<PassStmt*>(s) || dynamic_cast<ImportStmt*>(s) ||
        dynamic_cast<FromImportStmt*>(s) || dynamic_cast<TypeAliasStmt*>(s) ||
        dynamic_cast<ContractDecl*>(s))
        return false;

    return nodeMentionsName(s, name);
}

// Recurses into nested blocks/function bodies so buried candidate declarations are found.
static void forEachNestedBlock(
    Stmt* s, const std::function<void(const std::vector<std::unique_ptr<Stmt>>&)>& fn);

void CodeGen::Impl::analyzeBlockForStackAlloc(
    const std::vector<std::unique_ptr<Stmt>>& stmts, bool isModuleTopLevel) {
    for (size_t i = 0; i < stmts.size(); ++i) {
        Stmt* s = stmts[i].get();
        if (!s) continue;

        // 1. Recurse into nested blocks first; they're real lexical scopes, never
        // module-level.
        forEachNestedBlock(s, [this](const std::vector<std::unique_ptr<Stmt>>& b) {
            analyzeBlockForStackAlloc(b, /*isModuleTopLevel=*/false);
        });

        // Module globals are visible program-wide, so the sibling scan below can't see
        // earlier-defined function uses. Never promote globals; only nested blocks are eligible.
        if (isModuleTopLevel) continue;

        // 2. Is this a candidate `v: T = ClassName(args)` declaration?
        auto* an = dynamic_cast<AnnAssignStmt*>(s);
        if (!an || !an->value) continue;
        auto* targetName = dynamic_cast<NameExpr*>(an->target.get());
        if (!targetName) continue;

        // 2b. `t: Task[...] = fire ...`: an unjoined, non-escaping Task leaks its handle
        // ref, so mark it for scope-exit detach. Detaching an escaped Task would UAF, so the gate is strict non-escape.
        if (dynamic_cast<FireExpr*>(an->value.get()) || an->valueIsFreshTask) {
            bool isTaskAnnot = false;
            if (auto* nt = dynamic_cast<NamedTypeExpr*>(an->annotation.get()))
                isTaskAnnot = (nt->name == "Task");
            else if (auto* gt = dynamic_cast<GenericTypeExpr*>(an->annotation.get()))
                if (auto* gb = dynamic_cast<NamedTypeExpr*>(gt->base.get()))
                    isTaskAnnot = (gb->name == "Task");
            if (isTaskAnnot) {
                // await/join (consume) and is_alive (read) are not escapes here, so a
                // conditionally-joined or polled Task stays detachable. Only a real transfer forbids detach.
                const std::string& tv = targetName->name;
                bool taskEscaped = false;
                for (size_t j = i + 1; j < stmts.size(); ++j)
                    if (taskLocalTransferEscapes(stmts[j].get(), tv)) { taskEscaped = true; break; }
                if (!taskEscaped) detachableTaskDecls.insert(an);
            }
            continue;  // a fire value is never a stack-alloc ctor candidate
        }

        auto* ctorCall = dynamic_cast<CallExpr*>(an->value.get());
        if (!ctorCall) continue;
        auto* calleeName = dynamic_cast<NameExpr*>(ctorCall->callee.get());
        if (!calleeName || !classNames.count(calleeName->name)) continue;

        // 3. Does `v` escape the rest of this block? It's only in scope in subsequent
        // siblings (Dragon block scoping), never earlier or outside the block.
        const std::string& v = targetName->name;
        bool escaped = false;
        for (size_t j = i + 1; j < stmts.size(); ++j) {
            if (stmtEscapes(stmts[j].get(), v)) { escaped = true; break; }
        }
        if (!escaped)
            stackAllocSites.insert(ctorCall);
    }
}

static void forEachNestedBlock(
    Stmt* s, const std::function<void(const std::vector<std::unique_ptr<Stmt>>&)>& fn) {
    if (auto* ifs = dynamic_cast<IfStmt*>(s)) {
        fn(ifs->thenBody);
        for (auto& [cond, body] : ifs->elifClauses) { (void)cond; fn(body); }
        fn(ifs->elseBody);
    } else if (auto* w = dynamic_cast<WhileStmt*>(s)) {
        fn(w->body); fn(w->elseBody);
    } else if (auto* f = dynamic_cast<ForStmt*>(s)) {
        fn(f->body); fn(f->elseBody);
    } else if (auto* t = dynamic_cast<TryStmt*>(s)) {
        fn(t->tryBody);
        for (auto& h : t->handlers) fn(h.body);
        fn(t->elseBody); fn(t->finallyBody);
    } else if (auto* ws = dynamic_cast<WithStmt*>(s)) {
        fn(ws->body);
    } else if (auto* th = dynamic_cast<ThreadStmt*>(s)) {
        fn(th->body);
    } else if (auto* fd = dynamic_cast<FunctionDecl*>(s)) {
        fn(fd->body);  // free function or method body - its own scope
    } else if (auto* cd = dynamic_cast<ClassDecl*>(s)) {
        fn(cd->body);  // recurse to reach method FunctionDecls
    } else if (auto* ms = dynamic_cast<MatchStmt*>(s)) {
        for (auto& c : ms->cases) fn(c.body);
    }
}

void CodeGen::Impl::computeStackAllocSites(Module& entryModule,
                                           const std::vector<Module*>& depModules) {
    if (options.gcMode != GCMode::RC) return;  // gc=none has no RC to avoid
    for (auto* dep : depModules)
        analyzeBlockForStackAlloc(dep->body, /*isModuleTopLevel=*/true);
    analyzeBlockForStackAlloc(entryModule.body, /*isModuleTopLevel=*/true);
}

} // namespace dragon
