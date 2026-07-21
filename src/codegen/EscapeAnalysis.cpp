/// Dragon CodeGen - Escape Analysis (B Phase 1)
///
/// Decides which class-instance constructions can be stack-allocated instead
/// of heap-allocated + refcounted. A construction `v: T = T(args)` is a
/// candidate when the bound local `v` provably does not escape its declaring
/// block: every use of `v` is a plain field access `v.field` (read or scalar
/// store), and `v` is never returned, stored elsewhere, passed to a callee,
/// used as a method receiver / subscript / operator operand, captured by a
/// nested function / lambda / fire / thread, rebound, or boxed.
///
/// The analysis is deliberately CONSERVATIVE - it defaults to "escapes" for
/// anything it does not explicitly recognize as safe, because a false negative
/// (missing an escape) would leave a dangling pointer into a freed stack frame.
/// The remaining gates (the class is scalar-only, has a single trivial
/// non-self-escaping constructor) are applied at the CallExpr construction fork
/// where authoritative class metadata is available.
#include "../CodeGenImpl.h"
#include <functional>

namespace dragon {

namespace {

/// Exhaustive "does NameExpr(name) appear anywhere in this subtree" probe,
/// built on DefaultASTVisitor (which recurses into every child). Used as the
/// sound fallback for capture contexts and node kinds the structural walk
/// below does not special-case.
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

/// Task-detach escape walk (Task-detach tail). A Task local `target` must NOT be
/// detached at scope exit only if it TRANSFERS out: returned, stored, passed as
/// an argument, captured by a closure/fire/thread/nested-def, or rebound. It is
/// NOT a transfer to CONSUME it in-scope - `await t` / `t.join()` - or READ it -
/// `t.is_alive()`. Consume/read are in-scope uses, and the codegen blanks the
/// binding's slot to NULL right after a consume (await/join), so the scope-exit
/// detach sees an empty slot and does nothing on a consumed path - "a scope
/// frees what it (still) holds." (Earlier this excluded await/join because a join
/// FREES the struct and a same-path detach would UAF; the slot-nulling removes
/// that hazard, so a conditionally-awaited or polled Task is now safely
/// detachable on the path where it was abandoned.) Built on DefaultASTVisitor
/// (recurses into every child); the consume/read carve-out applies ONLY at the
/// live scope's top level - a capture context (lambda / fire / generator /
/// thread / nested def) takes the VARIABLE, so ANY mention inside it (even
/// t.is_alive()) is a transfer. DELIBERATELY separate from exprEscapes/
/// stmtEscapes so it can never weaken the stack-alloc analysis. Conservative:
/// any unrecognized mention is a transfer.
class TaskTransferVisitor : public DefaultASTVisitor {
public:
    explicit TaskTransferVisitor(std::string n) : target(std::move(n)) {}
    bool transferred = false;
    std::string target;

    bool isTarget(Expr* e) {
        auto* n = dynamic_cast<NameExpr*>(e);
        return n && n->name == target;
    }
    // Any mention of `target` anywhere in `node` (capture bodies: no carve-out).
    bool mentions(Expr& node) {
        NameMentionVisitor m(target); node.accept(m); return m.found;
    }
    bool mentions(Stmt& node) {
        NameMentionVisitor m(target); node.accept(m); return m.found;
    }

    void visit(NameExpr& n) override { if (n.name == target) transferred = true; }

    void visit(WalrusExpr& w) override {
        if (w.name == target) { transferred = true; return; }  // rebind -> transfer
        if (w.value) w.value->accept(*this);
    }

    void visit(AwaitExpr& a) override {
        // `await t` CONSUMES t in-scope (joins it; codegen then NULLs t's slot,
        // so the scope-exit detach is a no-op on this path). Not a transfer -
        // do not count the bare `t` operand. `await other` recurses normally.
        if (isTarget(a.operand.get())) return;
        if (a.operand) a.operand->accept(*this);
    }

    void visit(CallExpr& c) override {
        // `t.join()` CONSUMES (joins + codegen NULLs the slot) and `t.is_alive()`
        // READS - both are in-scope, neither is a transfer.
        if (auto* attr = dynamic_cast<AttributeExpr*>(c.callee.get()))
            if (isTarget(attr->object.get()) && c.args.empty() && c.kwArgs.empty() &&
                (attr->attribute == "join" || attr->attribute == "is_alive"))
                return;  // allowed - do NOT descend into the `t` receiver
        if (c.callee) c.callee->accept(*this);
        for (auto& a : c.args) if (a) a->accept(*this);
        for (auto& kv : c.kwArgs) if (kv.second) kv.second->accept(*this);
    }

    // Capture contexts: the closure / fire / generator / thread / nested def
    // takes the VARIABLE, so any mention of `t` inside escapes the handle (even
    // a `t.is_alive()` read becomes a captured use that outlives this scope).
    // No carve-out here - scan the whole body for a bare mention.
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

    // `v.field` - a field access. Reading a (scalar) field copies the value
    // out; the instance pointer does not flow anywhere. SAFE: return false
    // WITHOUT descending into the bare `v` (which would otherwise count as an
    // escape). A deeper object (e.g. `other.field`) is recursed normally.
    if (auto* attr = dynamic_cast<AttributeExpr*>(e)) {
        if (auto* ne = dynamic_cast<NameExpr*>(attr->object.get()))
            if (ne->name == name) return false;
        return exprEscapes(attr->object.get(), name);
    }

    // Calls. `v.method(...)` lets the receiver escape (the method body may
    // retain `self`); `v(...)` calls the instance. Otherwise recurse into the
    // callee + every argument so a bare `v` passed as an arg is caught.
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

    // `v[i]` - __getitem__, a method call on the instance.
    if (auto* sub = dynamic_cast<SubscriptExpr*>(e)) {
        if (auto* ne = dynamic_cast<NameExpr*>(sub->object.get()))
            if (ne->name == name) return true;
        return exprEscapes(sub->object.get(), name) ||
               exprEscapes(sub->index.get(), name);
    }

    // Bare occurrence of `v` anywhere not whitelisted above -> escape.
    if (auto* ne = dynamic_cast<NameExpr*>(e))
        return ne->name == name;

    // Structural compound expressions: recurse with exprEscapes so that
    // `v.x + v.y`, `[v.x]`, `(v.x, v.y)`, `{k: v.x}` etc. stay SAFE (only the
    // scalar field values flow), while a bare `v` in any of them escapes.
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
    // Walrus binding to `v` rebinds the name -> disqualify.
    if (auto* we = dynamic_cast<WalrusExpr*>(e)) {
        if (we->name == name) return true;
        return exprEscapes(we->value.get(), name);
    }

    // Capture / opaque contexts (lambda, fire, comprehensions, await/yield,
    // templates) and any unrecognized expression kind: a closure captures the
    // VARIABLE (not a field value), so ANY mention of `v` escapes. Sound
    // over-approximation via the exhaustive probe.
    return nodeMentionsName(e, name);
}

bool CodeGen::Impl::stmtEscapes(Stmt* s, const std::string& name) {
    if (!s) return false;

    if (auto* es = dynamic_cast<ExprStmt*>(s))
        return exprEscapes(es->expr.get(), name);

    if (auto* as = dynamic_cast<AssignStmt*>(s)) {
        // `v = ...` (NameExpr target) and `other[v] = ...` escape via the
        // target walk; `v.field = ...` is a safe scalar store (AttributeExpr
        // object == v -> exprEscapes returns false). Plus check the RHS.
        for (auto& t : as->targets)
            if (exprEscapes(t.get(), name)) return true;
        return exprEscapes(as->value.get(), name);
    }
    if (auto* an = dynamic_cast<AnnAssignStmt*>(s)) {
        // `v: T = ...` here is a rebind/shadow of v -> escape (target NameExpr(v)
        // makes exprEscapes return true).
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
        // A for-target binding `v` shadows it (exprEscapes(NameExpr(v)) -> true).
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

    // Capture sites: a nested def / thread body that names `v` captures the
    // variable -> escape. (Lambda/fire captures are caught at the expr level.)
    if (auto* fd = dynamic_cast<FunctionDecl*>(s))
        return nodeMentionsName(fd, name);
    if (auto* th = dynamic_cast<ThreadStmt*>(s))
        return nodeMentionsName(th, name);

    // Statements that can't reference a local without it showing up as a bare
    // name elsewhere, or rare/opaque kinds (match, delete, global/nonlocal,
    // class): fall back to the exhaustive mention probe - any appearance of v
    // is treated as an escape (sound over-approximation).
    if (dynamic_cast<BreakStmt*>(s) || dynamic_cast<ContinueStmt*>(s) ||
        dynamic_cast<PassStmt*>(s) || dynamic_cast<ImportStmt*>(s) ||
        dynamic_cast<FromImportStmt*>(s) || dynamic_cast<TypeAliasStmt*>(s))
        return false;

    return nodeMentionsName(s, name);
}

// Recurse into a statement's nested blocks / function bodies so candidate
// declarations buried deeper (loop bodies, branches, nested defs) are found.
static void forEachNestedBlock(
    Stmt* s, const std::function<void(const std::vector<std::unique_ptr<Stmt>>&)>& fn);

void CodeGen::Impl::analyzeBlockForStackAlloc(
    const std::vector<std::unique_ptr<Stmt>>& stmts, bool isModuleTopLevel) {
    for (size_t i = 0; i < stmts.size(); ++i) {
        Stmt* s = stmts[i].get();
        if (!s) continue;

        // 1. Recurse into nested blocks first (deeper candidates). Nested
        // blocks / function bodies are real lexical scopes, never module-level.
        forEachNestedBlock(s, [this](const std::vector<std::unique_ptr<Stmt>>& b) {
            analyzeBlockForStackAlloc(b, /*isModuleTopLevel=*/false);
        });

        // Module globals are visible program-wide; the subsequent-sibling scan
        // below cannot see uses in earlier-defined functions. Never promote
        // them - only their nested blocks (handled above) are eligible.
        if (isModuleTopLevel) continue;

        // 2. Is this a candidate `v: T = ClassName(args)` declaration?
        auto* an = dynamic_cast<AnnAssignStmt*>(s);
        if (!an || !an->value) continue;
        auto* targetName = dynamic_cast<NameExpr*>(an->target.get());
        if (!targetName) continue;

        // 2b. `t: Task[...] = fire ...` bound fire-and-forget (Task-detach tail).
        // A bound Task that is never joined/awaited and never escapes leaks its
        // handle ref - mark it for scope-exit dragon_vthread_detach. The SAME
        // conservative escape check flags any later use (join/await/is_alive are
        // method calls -> escape), so this fires ONLY for the pure declared-and-
        // unused case; that is exactly the leaking case (a joined task already
        // drops the ref, an escaped one transfers it). Detaching an escaped Task
        // would be a UAF (the receiver still needs the handle), so the gate is
        // strictly "does not escape the rest of this block".
        if (dynamic_cast<FireExpr*>(an->value.get())) {
            bool isTaskAnnot = false;
            if (auto* nt = dynamic_cast<NamedTypeExpr*>(an->annotation.get()))
                isTaskAnnot = (nt->name == "Task");
            else if (auto* gt = dynamic_cast<GenericTypeExpr*>(an->annotation.get()))
                if (auto* gb = dynamic_cast<NamedTypeExpr*>(gt->base.get()))
                    isTaskAnnot = (gb->name == "Task");
            if (isTaskAnnot) {
                // Refined transfer-escape walk: `await t` / `t.join()` (consume)
                // and `t.is_alive()` (read) are NOT escapes - detaching after
                // them is idempotent with join - so a conditionally-joined or
                // is_alive-polled Task is still detachable. Only a real transfer
                // (return / store / pass / capture / rebind) forbids detach.
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

        // 3. Does the bound local escape the rest of THIS block? `v` is in
        // scope only in subsequent siblings (and their nested blocks); it is
        // not visible earlier or outside this block (Dragon block scoping).
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

void CodeGen::Impl::computeStackAllocSites(Module& entryModule) {
    if (options.gcMode != GCMode::RC) return;  // gc=none has no RC to avoid
    analyzeBlockForStackAlloc(entryModule.body, /*isModuleTopLevel=*/true);
}

} // namespace dragon
