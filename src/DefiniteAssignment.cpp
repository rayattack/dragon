#include "dragon/DefiniteAssignment.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace dragon {

//===----------------------------------------------------------------------===//
// Implementation
//
// A structured forward "must" dataflow. Each tracked no-initializer local gets
// a unique integer id; a `Flow` carries the set of ids that are *definitely
// assigned* at a program point plus whether control has already left the path
// (return/raise/break/continue). Joins intersect the assigned sets of the
// surviving (non-terminated) predecessors, so a variable is only "assigned"
// when assigned on every path.
//===----------------------------------------------------------------------===//

namespace {

/// Dataflow fact at a program point.
struct Flow {
    std::unordered_set<int> assigned;  // ids known definitely-assigned here
    bool terminated = false;           // path already left (return/raise/break/continue)
};

/// A name binding in a lexical scope frame.
struct VarSlot {
    int id;
    bool tracked;  // true only for no-initializer locals we must guard
    SourceLocation declLoc;
    bool isModuleConst = false;  // a module-level const/global (eager, source-ordered init)
};

} // namespace

struct DefiniteAssignment::Impl {
    std::vector<DADiagnostic> diags;

    // Lexical scope stack for the callable currently being analyzed. Each frame
    // maps a name to its slot. Reads resolve innermost-first.
    std::vector<std::unordered_map<std::string, VarSlot>> scopes;

    // Per-loop collector of break-edge states (one vector per enclosing loop).
    std::vector<std::vector<Flow>> loopBreaks;

    int nextId = 0;

    // Ids already reported as used-before-assignment, to avoid cascading
    // duplicate errors for the same variable.
    std::unordered_set<int> reported;

    // --- constructor field-init context -----------------------------------
    // Active only while analyzing a constructor body. Each own-declared field
    // with no class-body default gets a slot id; `self.field = ...` marks it
    // assigned, and the constructor must leave every slot assigned on all exits.
    bool inConstructor = false;
    std::unordered_map<std::string, int> ctorFieldSlot;  // field name -> slot id
    std::unordered_set<int> ctorAllFieldIds;             // every field slot id
    std::vector<Flow>* returnCollector = nullptr;        // flow at each `return`

    // --- module-initialization order context -------------------------------
    // Module level consts/globals init eagerly, in source order. A binding
    // whose initaializer reads another module binding's value before that binding
    // is initialized cpatures a still zeroed slot: a null deref (heap type) or a slient
    // 0 (scalar). Let's model the module top-level as a definite init flow - every
    // module binding is a tracked slot, assigned when its initializer runs; a read of
    // a still-unassgned one directly or through a function/ctor called during ini
    // is a ompile time error. Function/method bodies analyze with a cleared scope
    // stack (analyzeCallable), so ordinary forward FUNCTION stay legal: a callee
    // reads a constant only when actually CALLED, and for init time-time calls we
    // should expect consultation of module-const read-set (below).
    bool inModuleInit = false;               // true only while walking the module top level
    std::string currentInitConst;            // binding whose initializer is running (diagnostics)
    std::unordered_map<std::string, SourceLocation> moduleConsts;  // name -> decl loc
    std::unordered_map<std::string, FunctionDecl*> moduleFuncs;    // top-level fn name -> decl
    std::unordered_map<std::string, ClassDecl*> moduleClasses;     // class name -> decl (for ctor)
    std::unordered_map<const FunctionDecl*, std::unordered_set<std::string>> readSetMemo;

    // --- scope helpers -----------------------------------------------------
    void pushFrame() { scopes.emplace_back(); }
    void popFrame() { scopes.pop_back(); }

    VarSlot* resolve(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    // Declare a name in the innermost frame. `tracked` no-init locals start
    // unassigned; everything else is declared already-assigned by the caller.
    int declare(const std::string& name, bool tracked, SourceLocation loc) {
        int id = nextId++;
        scopes.back()[name] = VarSlot{id, tracked, loc};
        return id;
    }

    // Declare a name that is immediately assigned (param, init'd decl, loop /
    // with / except target, import, walrus, ...).
    void declareAssigned(const std::string& name, SourceLocation loc, Flow& flow) {
        int id = declare(name, /*tracked=*/false, loc);
        flow.assigned.insert(id);
    }

    // Mark an existing name assigned (a plain `x = ...`). If the name isn't a
    // local of this callable it's a global/outer binding - record it as an
    // assigned untracked local so later reads in this callable don't warn.
    void markAssigned(const std::string& name, SourceLocation loc, Flow& flow) {
        if (VarSlot* s = resolve(name)) {
            flow.assigned.insert(s->id);
        } else {
            declareAssigned(name, loc, flow);
        }
    }

    void error(SourceLocation loc, const std::string& msg) {
        diags.push_back(DADiagnostic{loc, msg});
    }

    // --- merge -------------------------------------------------------------
    // Join several branch outcomes: a variable is assigned afterwards only if
    // assigned on every surviving branch. Terminated branches don't constrain.
    Flow merge(const std::vector<Flow>& branches) {
        std::vector<const Flow*> live;
        for (const auto& b : branches)
            if (!b.terminated) live.push_back(&b);

        Flow out;
        if (live.empty()) {
            out.terminated = true;  // every path left
            return out;
        }
        out.assigned = live[0]->assigned;
        for (size_t i = 1; i < live.size(); ++i) {
            std::unordered_set<int> next;
            for (int id : out.assigned)
                if (live[i]->assigned.count(id)) next.insert(id);
            out.assigned = std::move(next);
        }
        return out;
    }

    // --- traversal ---------------------------------------------------------

    // Analyze a function/method/module/lambda body in isolation: a fresh scope
    // stack and loop context, with parameters pre-declared as assigned.
    void analyzeCallable(const std::vector<Parameter>& params,
                         const std::vector<std::unique_ptr<Stmt>>& body) {
        auto savedScopes = std::move(scopes);
        auto savedLoops = std::move(loopBreaks);
        bool savedInCtor = inConstructor;
        auto* savedRet = returnCollector;
        bool savedInModuleInit = inModuleInit;
        std::string savedInitConst = std::move(currentInitConst);
        scopes.clear();
        loopBreaks.clear();
        inConstructor = false;       // a nested def is not the constructor
        returnCollector = nullptr;   // its returns are its own
        inModuleInit = false;        // its body runs at call time, not module load
        currentInitConst.clear();

        pushFrame();
        Flow flow;
        for (const auto& p : params)
            declareAssigned(p.name, SourceLocation{}, flow);
        analyzeBlock(body, std::move(flow));
        popFrame();

        scopes = std::move(savedScopes);
        loopBreaks = std::move(savedLoops);
        inConstructor = savedInCtor;
        returnCollector = savedRet;
        inModuleInit = savedInModuleInit;
        currentInitConst = std::move(savedInitConst);
    }

    // Analyze a constructor body: in addition to the usual local-read checks,
    // verify every own-declared non-defaulted field is assigned on all paths.
    void analyzeConstructor(
        const std::string& className,
        const std::vector<std::pair<std::string, SourceLocation>>& fields,
        FunctionDecl* ctor) {
        auto savedScopes = std::move(scopes);
        auto savedLoops = std::move(loopBreaks);
        bool savedInCtor = inConstructor;
        auto savedSlots = std::move(ctorFieldSlot);
        auto savedAll = std::move(ctorAllFieldIds);
        auto* savedRet = returnCollector;
        bool savedInModuleInit = inModuleInit;
        std::string savedInitConst = std::move(currentInitConst);
        scopes.clear();
        loopBreaks.clear();
        ctorFieldSlot.clear();
        ctorAllFieldIds.clear();
        inConstructor = true;
        inModuleInit = false;        // cos ctor body runs at call time, not module load
        currentInitConst.clear();
        std::vector<Flow> returnStates;
        returnCollector = &returnStates;

        pushFrame();
        Flow flow;
        declareAssigned("self", SourceLocation{}, flow);  // implicit in .dr
        for (const auto& p : ctor->params)
            declareAssigned(p.name, SourceLocation{}, flow);
        for (const auto& f : fields) {
            int id = nextId++;
            ctorFieldSlot[f.first] = id;
            ctorAllFieldIds.insert(id);
        }

        Flow fall = analyzeBlock(ctor->body, std::move(flow));
        std::vector<Flow> exits = std::move(returnStates);
        if (!fall.terminated) exits.push_back(fall);
        Flow exit = merge(exits);

        for (const auto& f : fields) {
            int id = ctorFieldSlot[f.first];
            if (!exit.assigned.count(id)) {
                error(ctor->location(),
                      "constructor of '" + className +
                          "' may return without assigning field '" + f.first +
                          "'; assign it in the constructor or give it a "
                          "default ('" + f.first + ": T = ...')");
            }
        }

        popFrame();
        scopes = std::move(savedScopes);
        loopBreaks = std::move(savedLoops);
        inConstructor = savedInCtor;
        ctorFieldSlot = std::move(savedSlots);
        ctorAllFieldIds = std::move(savedAll);
        returnCollector = savedRet;
        inModuleInit = savedInModuleInit;
        currentInitConst = std::move(savedInitConst);
    }

    // Process a block as its own lexical scope; returns the outgoing flow.
    Flow analyzeBlock(const std::vector<std::unique_ptr<Stmt>>& body, Flow flow) {
        pushFrame();
        for (const auto& s : body) {
            flow = analyzeStmt(s.get(), std::move(flow));
        }
        popFrame();
        return flow;
    }

    Flow analyzeStmt(Stmt* s, Flow flow);
    void checkExpr(Expr* e, Flow& flow);

    // --- module-init order analysis ---------------------------------------
    // Resolve a call's callee to a THIS-MOD function or constructor, or null
    // for imported/method/indirect callees (which cannot read this module's priate consts).
    // computeReadSet returns the set of module consts a
    // callable's body reads, unioned transitively over init-time calls; memoized.
    static FunctionDecl* classCtor(ClassDecl* cd);
    FunctionDecl* resolveModuleCallable(Expr* callee);
    const std::unordered_set<std::string>& computeReadSet(
        FunctionDecl* f, std::unordered_set<const FunctionDecl*>& visiting);
    void collectBoundNames(const std::vector<std::unique_ptr<Stmt>>& body,
                           std::unordered_set<std::string>& out);
    void collectBoundStmt(Stmt* s, std::unordered_set<std::string>& out);
    void scanReads(const std::vector<std::unique_ptr<Stmt>>& body,
                   const std::unordered_set<std::string>& bound,
                   std::unordered_set<std::string>& constReads,
                   std::vector<FunctionDecl*>& callees);
    void scanReadsStmt(Stmt* s, const std::unordered_set<std::string>& bound,
                       std::unordered_set<std::string>& constReads,
                       std::vector<FunctionDecl*>& callees);
    void scanReadsExpr(Expr* e, const std::unordered_set<std::string>& bound,
                       std::unordered_set<std::string>& constReads,
                       std::vector<FunctionDecl*>& callees);

    // Mark the variables written by an assignment target (handles plain names,
    // tuple/list unpacking, and starred targets; container/attribute targets
    // only read their object).
    void assignTarget(Expr* target, Flow& flow) {
        if (auto* n = dynamic_cast<NameExpr*>(target)) {
            markAssigned(n->name, n->location(), flow);
        } else if (auto* t = dynamic_cast<TupleExpr*>(target)) {
            for (auto& el : t->elements) assignTarget(el.get(), flow);
        } else if (auto* l = dynamic_cast<ListExpr*>(target)) {
            for (auto& el : l->elements) assignTarget(el.get(), flow);
        } else if (auto* st = dynamic_cast<StarredExpr*>(target)) {
            assignTarget(st->value.get(), flow);
        } else if (auto* sub = dynamic_cast<SubscriptExpr*>(target)) {
            checkExpr(sub->object.get(), flow);
            checkExpr(sub->index.get(), flow);
        } else if (auto* at = dynamic_cast<AttributeExpr*>(target)) {
            checkExpr(at->object.get(), flow);
            // `self.field = ...` inside a constructor assigns that field slot.
            if (inConstructor) {
                if (auto* obj = dynamic_cast<NameExpr*>(at->object.get())) {
                    if (obj->name == "self") {
                        auto it = ctorFieldSlot.find(at->attribute);
                        if (it != ctorFieldSlot.end())
                            flow.assigned.insert(it->second);
                    }
                }
            }
        }
    }

    // Bind names introduced by a match pattern (captures / sequence elements).
    void bindPattern(const MatchPattern& p, Flow& flow) {
        switch (p.kind) {
            case MatchPattern::Kind::Capture:
                if (!p.name.empty() && p.name != "_")
                    declareAssigned(p.name, SourceLocation{}, flow);
                break;
            case MatchPattern::Kind::Sequence:
            case MatchPattern::Kind::Or:
                for (const auto& sub : p.subPatterns) bindPattern(sub, flow);
                break;
            default:
                break;
        }
    }

    // Collect field names assigned via `self.<field> = ...` anywhere in a body
    // (descending into nested blocks). Used to detect the deferred-init pattern:
    // a field a non-constructor method assigns is intentionally initialized
    // later, so the constructor isn't required to assign it.
    void collectAssignedSelfFields(const std::vector<std::unique_ptr<Stmt>>& body,
                                   std::unordered_set<std::string>& out) {
        for (auto& s : body) collectStmtSelfFields(s.get(), out);
    }
    void collectStmtSelfFields(Stmt* s, std::unordered_set<std::string>& out) {
        auto tgt = [&](Expr* t) {
            if (auto* at = dynamic_cast<AttributeExpr*>(t))
                if (auto* o = dynamic_cast<NameExpr*>(at->object.get()))
                    if (o->name == "self") out.insert(at->attribute);
        };
        if (auto* as = dynamic_cast<AssignStmt*>(s)) {
            for (auto& t : as->targets) tgt(t.get());
        } else if (auto* an = dynamic_cast<AnnAssignStmt*>(s)) {
            tgt(an->target.get());
        } else if (auto* aug = dynamic_cast<AugAssignStmt*>(s)) {
            tgt(aug->target.get());
        } else if (auto* iff = dynamic_cast<IfStmt*>(s)) {
            collectAssignedSelfFields(iff->thenBody, out);
            for (auto& el : iff->elifClauses) collectAssignedSelfFields(el.second, out);
            collectAssignedSelfFields(iff->elseBody, out);
        } else if (auto* wh = dynamic_cast<WhileStmt*>(s)) {
            collectAssignedSelfFields(wh->body, out);
            collectAssignedSelfFields(wh->elseBody, out);
        } else if (auto* fo = dynamic_cast<ForStmt*>(s)) {
            collectAssignedSelfFields(fo->body, out);
            collectAssignedSelfFields(fo->elseBody, out);
        } else if (auto* tr = dynamic_cast<TryStmt*>(s)) {
            collectAssignedSelfFields(tr->tryBody, out);
            for (auto& h : tr->handlers) collectAssignedSelfFields(h.body, out);
            collectAssignedSelfFields(tr->elseBody, out);
            collectAssignedSelfFields(tr->finallyBody, out);
        } else if (auto* w = dynamic_cast<WithStmt*>(s)) {
            collectAssignedSelfFields(w->body, out);
        } else if (auto* m = dynamic_cast<MatchStmt*>(s)) {
            for (auto& c : m->cases) collectAssignedSelfFields(c.body, out);
        }
    }

    static bool patternIsIrrefutable(const MatchPattern& p) {
        return (p.kind == MatchPattern::Kind::Wildcard ||
                p.kind == MatchPattern::Kind::Capture) &&
               p.guard == nullptr;
    }

    static bool isConstTrue(Expr* e) {
        if (auto* b = dynamic_cast<BooleanLiteral*>(e)) return b->value;
        if (auto* i = dynamic_cast<IntegerLiteral*>(e)) return i->value != 0;
        return false;
    }
};

//===----------------------------------------------------------------------===//
// Expression reads
//===----------------------------------------------------------------------===//

void DefiniteAssignment::Impl::checkExpr(Expr* e, Flow& flow) {
    if (!e) return;

    if (auto* n = dynamic_cast<NameExpr*>(e)) {
        VarSlot* s = resolve(n->name);
        if (s && s->tracked && !flow.assigned.count(s->id) &&
            !reported.count(s->id)) {
            reported.insert(s->id);
            if (s->isModuleConst) {
                std::string who = currentInitConst.empty()
                    ? std::string("module-level initialization")
                    : ("'" + currentInitConst + "'");
                error(n->location(),
                      who + " reads '" + n->name +
                          "' during module initialization, but '" + n->name +
                          "' is initialized later (line " +
                          std::to_string(s->declLoc.line) + "); move '" + n->name +
                          "' above it");
            } else {
                error(n->location(),
                      "variable '" + n->name +
                          "' may be read before it is assigned a value");
            }
            // Suppress cascading reports for this variable downstream.
            flow.assigned.insert(s->id);
        }
        return;
    }
    if (auto* w = dynamic_cast<WalrusExpr*>(e)) {
        checkExpr(w->value.get(), flow);
        declareAssigned(w->name, w->location(), flow);  // n := v binds n
        return;
    }
    if (auto* b = dynamic_cast<BinaryExpr*>(e)) {
        checkExpr(b->left.get(), flow);
        checkExpr(b->right.get(), flow);
        return;
    }
    if (auto* c = dynamic_cast<ChainedCompExpr*>(e)) {
        for (auto& o : c->operands) checkExpr(o.get(), flow);
        return;
    }
    if (auto* u = dynamic_cast<UnaryExpr*>(e)) {
        checkExpr(u->operand.get(), flow);
        return;
    }
    if (auto* call = dynamic_cast<CallExpr*>(e)) {
        checkExpr(call->callee.get(), flow);
        for (auto& a : call->args) checkExpr(a.get(), flow);
        for (auto& kw : call->kwArgs) checkExpr(kw.second.get(), flow);
        // Module-init order: a call evaluated during eager module initialization
        // runs the callee's body NOW. If that body reads a module const not yet
        // initialized (directly or transitively through further init-time calls),
        // that is a forward read - reject it. Only calls to this-module callables
        // are inspected; imported/method/indirect callees cannot reach our consts.
        if (inModuleInit) {
            if (FunctionDecl* target = resolveModuleCallable(call->callee.get())) {
                std::unordered_set<const FunctionDecl*> visiting;
                const auto& rs = computeReadSet(target, visiting);
                for (const auto& cname : rs) {
                    VarSlot* cs = resolve(cname);
                    if (cs && cs->isModuleConst && !flow.assigned.count(cs->id) &&
                        !reported.count(cs->id)) {
                        reported.insert(cs->id);
                        std::string who = currentInitConst.empty()
                            ? std::string("module-level initialization")
                            : ("'" + currentInitConst + "'");
                        error(call->location(),
                              who + " reads '" + cname +
                                  "' during module initialization through this call, but '" +
                                  cname + "' is initialized later (line " +
                                  std::to_string(cs->declLoc.line) + "); move '" + cname +
                                  "' above it, or move the call out of module initialization");
                    }
                }
            }
        }
        // Constructor leniency: once `self` escapes to a helper - `self.setup()`
        // or `register(self)` - that callee may assign any field, so we can no
        // longer prove a field is unassigned. Treat all field slots as assigned.
        if (inConstructor && !ctorAllFieldIds.empty()) {
            bool selfEscapes = false;
            if (auto* ce = dynamic_cast<AttributeExpr*>(call->callee.get()))
                if (auto* o = dynamic_cast<NameExpr*>(ce->object.get()))
                    if (o->name == "self") selfEscapes = true;
            auto isSelf = [](Expr* x) {
                auto* n = dynamic_cast<NameExpr*>(x);
                return n && n->name == "self";
            };
            for (auto& a : call->args)
                if (isSelf(a.get())) selfEscapes = true;
            for (auto& kw : call->kwArgs)
                if (isSelf(kw.second.get())) selfEscapes = true;
            if (selfEscapes)
                for (int id : ctorAllFieldIds) flow.assigned.insert(id);
        }
        return;
    }
    if (auto* at = dynamic_cast<AttributeExpr*>(e)) {
        checkExpr(at->object.get(), flow);
        return;
    }
    if (auto* sub = dynamic_cast<SubscriptExpr*>(e)) {
        checkExpr(sub->object.get(), flow);
        checkExpr(sub->index.get(), flow);
        return;
    }
    if (auto* sl = dynamic_cast<SliceExpr*>(e)) {
        checkExpr(sl->lower.get(), flow);
        checkExpr(sl->upper.get(), flow);
        checkExpr(sl->step.get(), flow);
        return;
    }
    if (auto* l = dynamic_cast<ListExpr*>(e)) {
        for (auto& el : l->elements) checkExpr(el.get(), flow);
        return;
    }
    if (auto* t = dynamic_cast<TupleExpr*>(e)) {
        for (auto& el : t->elements) checkExpr(el.get(), flow);
        return;
    }
    if (auto* st = dynamic_cast<SetExpr*>(e)) {
        for (auto& el : st->elements) checkExpr(el.get(), flow);
        return;
    }
    if (auto* d = dynamic_cast<DictExpr*>(e)) {
        for (auto& kv : d->entries) {
            checkExpr(kv.first.get(), flow);
            checkExpr(kv.second.get(), flow);
        }
        return;
    }
    if (auto* str = dynamic_cast<StringLiteral*>(e)) {
        if (str->isFString)
            for (auto& part : str->fstringParts)
                if (part.kind == FStringPart::Kind::Expression)
                    checkExpr(part.expr.get(), flow);
        return;
    }
    if (auto* tern = dynamic_cast<IfExpr*>(e)) {
        checkExpr(tern->condition.get(), flow);
        // Only one branch runs; check both for reads but don't let a branch's
        // walrus escape (conditional binding can't be relied on afterwards).
        Flow t = flow;
        checkExpr(tern->thenExpr.get(), t);
        Flow f = flow;
        checkExpr(tern->elseExpr.get(), f);
        return;
    }
    if (auto* aw = dynamic_cast<AwaitExpr*>(e)) {
        checkExpr(aw->operand.get(), flow);
        return;
    }
    if (auto* y = dynamic_cast<YieldExpr*>(e)) {
        checkExpr(y->value.get(), flow);
        return;
    }
    if (auto* star = dynamic_cast<StarredExpr*>(e)) {
        checkExpr(star->value.get(), flow);
        return;
    }
    // Comprehensions: the *first* iterable is evaluated in the enclosing scope;
    // the bound variable and remaining clauses live in the comprehension scope.
    auto comp = [&](Expr* element, Expr* element2, const std::string& varName,
                    const std::vector<std::string>& varNames, Expr* iterable,
                    Expr* condition, const std::vector<CompClause>& extra) {
        checkExpr(iterable, flow);
        pushFrame();
        Flow inner = flow;
        if (!varName.empty()) declareAssigned(varName, SourceLocation{}, inner);
        for (const auto& vn : varNames) declareAssigned(vn, SourceLocation{}, inner);
        for (const auto& cl : extra) {
            checkExpr(cl.iterable.get(), inner);
            for (const auto& vn : cl.varNames)
                declareAssigned(vn, SourceLocation{}, inner);
            checkExpr(cl.condition.get(), inner);
        }
        checkExpr(condition, inner);
        checkExpr(element, inner);
        checkExpr(element2, inner);
        popFrame();
    };
    if (auto* lc = dynamic_cast<ListCompExpr*>(e)) {
        comp(lc->element.get(), nullptr, lc->varName, {}, lc->iterable.get(),
             lc->condition.get(), lc->extraClauses);
        return;
    }
    if (auto* sc = dynamic_cast<SetCompExpr*>(e)) {
        comp(sc->element.get(), nullptr, sc->varName, {}, sc->iterable.get(),
             sc->condition.get(), sc->extraClauses);
        return;
    }
    if (auto* ge = dynamic_cast<GeneratorExpr*>(e)) {
        comp(ge->element.get(), nullptr, ge->varName, {}, ge->iterable.get(),
             ge->condition.get(), ge->extraClauses);
        return;
    }
    if (auto* dc = dynamic_cast<DictCompExpr*>(e)) {
        comp(dc->key.get(), dc->value.get(), "", dc->varNames, dc->iterable.get(),
             dc->condition.get(), dc->extraClauses);
        return;
    }
    // Nested callables: analyze their bodies independently (captures are treated
    // as assigned because cross-procedure flow isn't modeled).
    if (auto* lam = dynamic_cast<LambdaExpr*>(e)) {
        std::vector<Parameter> params;
        for (auto& p : lam->params) {
            Parameter q;
            q.name = p.name;
            params.push_back(std::move(q));
        }
        if (lam->body) {
            // Single-expression lambda: its body runs at call time, not module load.
            auto saved = std::move(scopes);
            auto savedLoops = std::move(loopBreaks);
            bool savedInModuleInit = inModuleInit;
            scopes.clear();
            loopBreaks.clear();
            inModuleInit = false;
            pushFrame();
            Flow lf;
            for (const auto& p : params) declareAssigned(p.name, SourceLocation{}, lf);
            checkExpr(lam->body.get(), lf);
            popFrame();
            scopes = std::move(saved);
            loopBreaks = std::move(savedLoops);
            inModuleInit = savedInModuleInit;
        } else {
            analyzeCallable(params, lam->bodyStmts);
        }
        return;
    }
    if (auto* fire = dynamic_cast<FireExpr*>(e)) {
        if (fire->operand) {
            checkExpr(fire->operand.get(), flow);
        } else {
            analyzeCallable({}, fire->bodyStmts);
        }
        return;
    }
    // Literals / templates / type-less leaves: nothing to read.
}

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

Flow DefiniteAssignment::Impl::analyzeStmt(Stmt* s, Flow flow) {
    if (!s || flow.terminated) {
        // Still descend into declarations even on a dead path so nested
        // callables get analyzed; but for straight statements there's nothing
        // observable once terminated.
        if (!s) return flow;
    }

    if (auto* es = dynamic_cast<ExprStmt*>(s)) {
        checkExpr(es->expr.get(), flow);
        return flow;
    }
    if (auto* ds = dynamic_cast<DeferStmt*>(s)) {
        // Arguments and receiver evaluate AT the defer statement, so every
        // name it references must be definitely assigned here, not at exit.
        checkExpr(ds->call.get(), flow);
        return flow;
    }
    if (auto* an = dynamic_cast<AnnAssignStmt*>(s)) {
        // `self.x: T = v` / `obj.x: T` - attribute target, not a local.
        if (auto* nm = dynamic_cast<NameExpr*>(an->target.get())) {
            if (an->value) {
                // A module-level binding was pre-declared as a tracked slot; set
                // the diagnostic context, check its initializer (forward reads of
                // not-yet-initialized consts fire here), then mark the existing
                // slot assigned instead of shadowing it with a fresh one.
                VarSlot* modSlot =
                    inModuleInit ? resolve(nm->name) : nullptr;
                if (modSlot && !modSlot->isModuleConst) modSlot = nullptr;
                std::string savedInitConst;
                if (modSlot) {
                    savedInitConst = std::move(currentInitConst);
                    currentInitConst = nm->name;
                }
                checkExpr(an->value.get(), flow);
                if (modSlot) {
                    flow.assigned.insert(modSlot->id);
                    currentInitConst = std::move(savedInitConst);
                } else {
                    declareAssigned(nm->name, nm->location(), flow);
                }
            } else {
                // No initializer: a tracked local that must be assigned before use.
                declare(nm->name, /*tracked=*/true, nm->location());
            }
        } else {
            if (an->value) checkExpr(an->value.get(), flow);
            assignTarget(an->target.get(), flow);
        }
        return flow;
    }
    if (auto* as = dynamic_cast<AssignStmt*>(s)) {
        checkExpr(as->value.get(), flow);
        for (auto& t : as->targets) assignTarget(t.get(), flow);
        return flow;
    }
    if (auto* aug = dynamic_cast<AugAssignStmt*>(s)) {
        // `x += v` reads x then writes it.
        checkExpr(aug->target.get(), flow);
        checkExpr(aug->value.get(), flow);
        if (auto* nm = dynamic_cast<NameExpr*>(aug->target.get()))
            markAssigned(nm->name, nm->location(), flow);
        return flow;
    }
    if (auto* iff = dynamic_cast<IfStmt*>(s)) {
        Flow cond = flow;  // threads walrus effects through the chain of tests
        checkExpr(iff->condition.get(), cond);
        std::vector<Flow> outs;
        outs.push_back(analyzeBlock(iff->thenBody, cond));
        for (auto& el : iff->elifClauses) {
            checkExpr(el.first.get(), cond);
            outs.push_back(analyzeBlock(el.second, cond));
        }
        if (!iff->elseBody.empty())
            outs.push_back(analyzeBlock(iff->elseBody, cond));
        else
            outs.push_back(cond);  // implicit empty else: falls through unchanged
        return merge(outs);
    }
    if (auto* wh = dynamic_cast<WhileStmt*>(s)) {
        checkExpr(wh->condition.get(), flow);
        loopBreaks.emplace_back();
        analyzeBlock(wh->body, flow);  // body may run; assignments don't escape
        std::vector<Flow> breaks = std::move(loopBreaks.back());
        loopBreaks.pop_back();

        std::vector<Flow> outs = std::move(breaks);
        if (isConstTrue(wh->condition.get())) {
            // `while True { ... }` - only reachable afterward via break.
            return merge(outs);
        }
        // Otherwise the loop may run zero times / exit when the condition fails:
        // that path carries the else clause (if any), else the incoming flow.
        if (!wh->elseBody.empty())
            outs.push_back(analyzeBlock(wh->elseBody, flow));
        else
            outs.push_back(flow);
        return merge(outs);
    }
    if (auto* fo = dynamic_cast<ForStmt*>(s)) {
        checkExpr(fo->iterable.get(), flow);
        loopBreaks.emplace_back();
        // Loop target is scoped to the body and assigned within it.
        pushFrame();
        Flow body = flow;
        assignTarget(fo->target.get(), body);
        for (auto& st : fo->body) body = analyzeStmt(st.get(), std::move(body));
        popFrame();
        std::vector<Flow> breaks = std::move(loopBreaks.back());
        loopBreaks.pop_back();

        // A for-loop always has a zero-iteration path, so body assignments never
        // survive; the post-state is the incoming flow (or else clause) joined
        // with any break edges.
        std::vector<Flow> outs = std::move(breaks);
        if (!fo->elseBody.empty())
            outs.push_back(analyzeBlock(fo->elseBody, flow));
        else
            outs.push_back(flow);
        return merge(outs);
    }
    if (auto* tr = dynamic_cast<TryStmt*>(s)) {
        // try completed normally -> optional else.
        Flow tryFlow = analyzeBlock(tr->tryBody, flow);
        std::vector<Flow> outs;
        if (!tr->elseBody.empty())
            outs.push_back(analyzeBlock(tr->elseBody, tryFlow));
        else
            outs.push_back(tryFlow);
        // Each handler may fire after an exception anywhere in try, so it starts
        // from the pre-try state (plus its bound exception name).
        for (auto& h : tr->handlers) {
            Flow hf = flow;
            pushFrame();
            if (!h.name.empty()) declareAssigned(h.name, SourceLocation{}, hf);
            for (auto& st : h.body) hf = analyzeStmt(st.get(), std::move(hf));
            popFrame();
            outs.push_back(hf);
        }
        Flow post = merge(outs);
        // finally always runs; its reads are checked against the minimal
        // pre-try state, and its assignments are unconditionally added.
        if (!tr->finallyBody.empty()) {
            Flow fin = analyzeBlock(tr->finallyBody, flow);
            for (int id : fin.assigned) post.assigned.insert(id);
            post.terminated = post.terminated || fin.terminated;
        }
        return post;
    }
    if (auto* w = dynamic_cast<WithStmt*>(s)) {
        for (auto& item : w->items) {
            checkExpr(item.contextExpr.get(), flow);
            if (item.optionalVars)
                assignTarget(item.optionalVars.get(), flow);
        }
        return analyzeBlock(w->body, flow);
    }
    if (auto* th = dynamic_cast<ThreadStmt*>(s)) {
        analyzeCallable({}, th->body);  // independent concurrent block
        return flow;
    }
    if (auto* m = dynamic_cast<MatchStmt*>(s)) {
        checkExpr(m->subject.get(), flow);
        std::vector<Flow> outs;
        bool hasCatchAll = false;
        for (auto& c : m->cases) {
            Flow cf = flow;
            pushFrame();
            bindPattern(c.pattern, cf);
            checkExpr(c.pattern.guard.get(), cf);
            checkExpr(c.guard.get(), cf);
            for (auto& st : c.body) cf = analyzeStmt(st.get(), std::move(cf));
            popFrame();
            outs.push_back(cf);
            if (patternIsIrrefutable(c.pattern) && c.guard == nullptr)
                hasCatchAll = true;
        }
        if (!hasCatchAll) outs.push_back(flow);  // no case matched
        return merge(outs);
    }
    if (auto* ret = dynamic_cast<ReturnStmt*>(s)) {
        checkExpr(ret->value.get(), flow);
        // A `return` is a constructor exit: record the field-init state here.
        if (returnCollector) returnCollector->push_back(flow);
        flow.terminated = true;
        return flow;
    }
    if (auto* rz = dynamic_cast<RaiseStmt*>(s)) {
        checkExpr(rz->exception.get(), flow);
        checkExpr(rz->cause.get(), flow);
        flow.terminated = true;
        return flow;
    }
    if (dynamic_cast<BreakStmt*>(s)) {
        if (!loopBreaks.empty()) loopBreaks.back().push_back(flow);
        flow.terminated = true;
        return flow;
    }
    if (dynamic_cast<ContinueStmt*>(s)) {
        flow.terminated = true;
        return flow;
    }
    if (dynamic_cast<PassStmt*>(s)) {
        return flow;
    }
    if (auto* asrt = dynamic_cast<AssertStmt*>(s)) {
        checkExpr(asrt->test.get(), flow);
        checkExpr(asrt->msg.get(), flow);
        return flow;
    }
    if (auto* g = dynamic_cast<GlobalStmt*>(s)) {
        for (auto& n : g->names) declareAssigned(n, SourceLocation{}, flow);
        return flow;
    }
    if (auto* nl = dynamic_cast<NonlocalStmt*>(s)) {
        for (auto& n : nl->names) declareAssigned(n, SourceLocation{}, flow);
        return flow;
    }
    if (auto* del = dynamic_cast<DeleteStmt*>(s)) {
        for (auto& t : del->targets) {
            if (auto* nm = dynamic_cast<NameExpr*>(t.get())) {
                if (VarSlot* slot = resolve(nm->name))
                    flow.assigned.erase(slot->id);
            } else {
                checkExpr(t.get(), flow);
            }
        }
        return flow;
    }
    if (auto* imp = dynamic_cast<ImportStmt*>(s)) {
        for (auto& a : imp->names) {
            const std::string& bound = a.asName.empty() ? a.name : a.asName;
            // `import a.b.c` binds the top-level name `a` when unaliased.
            std::string top = bound;
            if (a.asName.empty()) {
                auto dot = top.find('.');
                if (dot != std::string::npos) top = top.substr(0, dot);
            }
            declareAssigned(top, SourceLocation{}, flow);
        }
        return flow;
    }
    if (auto* fi = dynamic_cast<FromImportStmt*>(s)) {
        for (auto& a : fi->names) {
            const std::string& bound = a.asName.empty() ? a.name : a.asName;
            declareAssigned(bound, SourceLocation{}, flow);
        }
        return flow;
    }
    if (auto* fn = dynamic_cast<FunctionDecl*>(s)) {
        if (!fn->name.empty()) declareAssigned(fn->name, fn->location(), flow);
        if (!fn->isExtern) analyzeCallable(fn->params, fn->body);
        return flow;
    }
    if (auto* cd = dynamic_cast<ClassDecl*>(s)) {
        if (!cd->name.empty()) declareAssigned(cd->name, cd->location(), flow);

        // A decorator may rewrite the class (e.g. @dataclass synthesizes field
        // assignment), so don't enforce constructor field-init on decorated
        // classes. Generic class templates are stamped per-instantiation and
        // re-analyzed there, so skip the abstract template too.
        bool enforceFields = cd->decorators.empty() && cd->typeParams.empty();

        // Deferred-init detection: a field assigned by a NON-constructor method
        // is intentionally initialized later (the `listen_tls()` / `connect()`
        // pattern - Swift would model it as an Optional). Such fields are exempt
        // from the constructor requirement; fields the constructor alone owns
        // still get full per-path checking.
        std::unordered_set<std::string> deferredFields;
        if (enforceFields) {
            for (auto& member : cd->body) {
                auto* method = dynamic_cast<FunctionDecl*>(member.get());
                if (!method) continue;
                bool isCtor = method->isConstructor || method->name == "__init__";
                if (!isCtor) collectAssignedSelfFields(method->body, deferredFields);
            }
        }

        // Own-declared fields with no class-body default and not deferred: these
        // are exactly the slots a constructor must fill (the parent ctor handles
        // inherited ones).
        std::vector<std::pair<std::string, SourceLocation>> requiredFields;
        if (enforceFields) {
            for (auto& member : cd->body) {
                auto* fld = dynamic_cast<AnnAssignStmt*>(member.get());
                if (!fld || fld->isStatic || fld->value) continue;
                if (auto* nm = dynamic_cast<NameExpr*>(fld->target.get()))
                    if (!deferredFields.count(nm->name))
                        requiredFields.push_back({nm->name, fld->location()});
            }
        }

        // Analyze methods independently; route explicit constructors through the
        // field-init check when the class has required fields.
        for (auto& member : cd->body) {
            if (auto* method = dynamic_cast<FunctionDecl*>(member.get())) {
                if (method->isExtern) continue;
                bool isCtor = method->isConstructor || method->name == "__init__";
                if (isCtor && !requiredFields.empty())
                    analyzeConstructor(cd->name, requiredFields, method);
                else
                    analyzeCallable(method->params, method->body);
            }
        }
        return flow;
    }
    if (auto* ta = dynamic_cast<TypeAliasStmt*>(s)) {
        declareAssigned(ta->name, ta->location(), flow);
        return flow;
    }
    return flow;
}

//===----------------------------------------------------------------------===//
// Module-initialization order: interprocedural read-set
//
// A module const's initializer may reach another const not only in its own RHS
// but THROUGH a function/ctor it calls during init (deps.dr's
// `const _READY = _ensure_data()`, where `_ensure_data` reads `DATA_DIR`). These
// helpers compute, for a this-module callable i.e. the set of module consts its body
// reads - unioned transitively over further this-module calls, memoized. The
// module-init walk consults that set at each init-time call site and errors on a
// read of a not-yet-initialized const.
//
// Conservative by construction: unresolved callees (imported, method, or called
// through a value) and reads inside nested lambdas/comprehensions contribute
// nothing TODO: - a false negative never a false positive that
// would reject correct code. Mutual recursion among init-time callees is
// under-approximated for the same reason. Local/param names that shadow a const
// are excluded so a parameter named like a const is never mistaken for it.
//===----------------------------------------------------------------------===//

FunctionDecl* DefiniteAssignment::Impl::classCtor(ClassDecl* cd) {
    if (!cd) return nullptr;
    for (auto& m : cd->body)
        if (auto* fn = dynamic_cast<FunctionDecl*>(m.get()))
            if (fn->isConstructor || fn->name == "__init__") return fn;
    return nullptr;
}

FunctionDecl* DefiniteAssignment::Impl::resolveModuleCallable(Expr* callee) {
    auto* n = dynamic_cast<NameExpr*>(callee);
    if (!n) return nullptr;  // method/attribute/indirect callee: Stage 2 (logged)
    if (auto it = moduleFuncs.find(n->name); it != moduleFuncs.end()) return it->second;
    if (auto it = moduleClasses.find(n->name); it != moduleClasses.end())
        return classCtor(it->second);
    return nullptr;          // imported / builtin: cannot read this module's consts
}

// Names bound as a local by an assignment/for/with target expression.
static void da_targetNames(Expr* t, std::unordered_set<std::string>& out) {
    if (auto* n = dynamic_cast<NameExpr*>(t)) out.insert(n->name);
    else if (auto* tup = dynamic_cast<TupleExpr*>(t)) {
        for (auto& e : tup->elements) da_targetNames(e.get(), out);
    } else if (auto* lst = dynamic_cast<ListExpr*>(t)) {
        for (auto& e : lst->elements) da_targetNames(e.get(), out);
    } else if (auto* st = dynamic_cast<StarredExpr*>(t)) {
        da_targetNames(st->value.get(), out);
    }
}

void DefiniteAssignment::Impl::collectBoundNames(
    const std::vector<std::unique_ptr<Stmt>>& body,
    std::unordered_set<std::string>& out) {
    for (auto& s : body) collectBoundStmt(s.get(), out);
}

void DefiniteAssignment::Impl::collectBoundStmt(Stmt* s,
                                                std::unordered_set<std::string>& out) {
    if (!s) return;
    if (auto* as = dynamic_cast<AssignStmt*>(s)) {
        for (auto& t : as->targets) da_targetNames(t.get(), out);
    } else if (auto* an = dynamic_cast<AnnAssignStmt*>(s)) {
        da_targetNames(an->target.get(), out);
    } else if (auto* aug = dynamic_cast<AugAssignStmt*>(s)) {
        da_targetNames(aug->target.get(), out);
    } else if (auto* fo = dynamic_cast<ForStmt*>(s)) {
        da_targetNames(fo->target.get(), out);
        collectBoundNames(fo->body, out);
        collectBoundNames(fo->elseBody, out);
    } else if (auto* iff = dynamic_cast<IfStmt*>(s)) {
        collectBoundNames(iff->thenBody, out);
        for (auto& el : iff->elifClauses) collectBoundNames(el.second, out);
        collectBoundNames(iff->elseBody, out);
    } else if (auto* wh = dynamic_cast<WhileStmt*>(s)) {
        collectBoundNames(wh->body, out);
        collectBoundNames(wh->elseBody, out);
    } else if (auto* tr = dynamic_cast<TryStmt*>(s)) {
        collectBoundNames(tr->tryBody, out);
        for (auto& h : tr->handlers) {
            if (!h.name.empty()) out.insert(h.name);
            collectBoundNames(h.body, out);
        }
        collectBoundNames(tr->elseBody, out);
        collectBoundNames(tr->finallyBody, out);
    } else if (auto* w = dynamic_cast<WithStmt*>(s)) {
        for (auto& item : w->items)
            if (item.optionalVars) da_targetNames(item.optionalVars.get(), out);
        collectBoundNames(w->body, out);
    } else if (auto* m = dynamic_cast<MatchStmt*>(s)) {
        for (auto& c : m->cases) collectBoundNames(c.body, out);
    } else if (auto* g = dynamic_cast<GlobalStmt*>(s)) {
        for (auto& nm : g->names) out.insert(nm);
    } else if (auto* nl = dynamic_cast<NonlocalStmt*>(s)) {
        for (auto& nm : nl->names) out.insert(nm);
    } else if (auto* imp = dynamic_cast<ImportStmt*>(s)) {
        for (auto& a : imp->names) {
            std::string b = a.asName.empty() ? a.name : a.asName;
            if (a.asName.empty()) {
                auto dot = b.find('.');
                if (dot != std::string::npos) b = b.substr(0, dot);
            }
            out.insert(b);
        }
    } else if (auto* fi = dynamic_cast<FromImportStmt*>(s)) {
        for (auto& a : fi->names) out.insert(a.asName.empty() ? a.name : a.asName);
    } else if (auto* fn = dynamic_cast<FunctionDecl*>(s)) {
        if (!fn->name.empty()) out.insert(fn->name);
    } else if (auto* cd = dynamic_cast<ClassDecl*>(s)) {
        if (!cd->name.empty()) out.insert(cd->name);
    }
}

void DefiniteAssignment::Impl::scanReads(
    const std::vector<std::unique_ptr<Stmt>>& body,
    const std::unordered_set<std::string>& bound,
    std::unordered_set<std::string>& constReads,
    std::vector<FunctionDecl*>& callees) {
    for (auto& s : body) scanReadsStmt(s.get(), bound, constReads, callees);
}

void DefiniteAssignment::Impl::scanReadsStmt(
    Stmt* s, const std::unordered_set<std::string>& bound,
    std::unordered_set<std::string>& constReads,
    std::vector<FunctionDecl*>& callees) {
    if (!s) return;
    auto E = [&](Expr* e) { scanReadsExpr(e, bound, constReads, callees); };
    auto B = [&](const std::vector<std::unique_ptr<Stmt>>& b) {
        scanReads(b, bound, constReads, callees);
    };
    if (auto* es = dynamic_cast<ExprStmt*>(s)) E(es->expr.get());
    else if (auto* as = dynamic_cast<AssignStmt*>(s)) E(as->value.get());
    else if (auto* an = dynamic_cast<AnnAssignStmt*>(s)) { if (an->value) E(an->value.get()); }
    else if (auto* aug = dynamic_cast<AugAssignStmt*>(s)) { E(aug->target.get()); E(aug->value.get()); }
    else if (auto* ret = dynamic_cast<ReturnStmt*>(s)) E(ret->value.get());
    else if (auto* iff = dynamic_cast<IfStmt*>(s)) {
        E(iff->condition.get()); B(iff->thenBody);
        for (auto& el : iff->elifClauses) { E(el.first.get()); B(el.second); }
        B(iff->elseBody);
    } else if (auto* wh = dynamic_cast<WhileStmt*>(s)) {
        E(wh->condition.get()); B(wh->body); B(wh->elseBody);
    } else if (auto* fo = dynamic_cast<ForStmt*>(s)) {
        E(fo->iterable.get()); B(fo->body); B(fo->elseBody);
    } else if (auto* tr = dynamic_cast<TryStmt*>(s)) {
        B(tr->tryBody);
        for (auto& h : tr->handlers) B(h.body);
        B(tr->elseBody); B(tr->finallyBody);
    } else if (auto* w = dynamic_cast<WithStmt*>(s)) {
        for (auto& item : w->items) E(item.contextExpr.get());
        B(w->body);
    } else if (auto* m = dynamic_cast<MatchStmt*>(s)) {
        E(m->subject.get());
        for (auto& c : m->cases) { E(c.guard.get()); B(c.body); }
    } else if (auto* rz = dynamic_cast<RaiseStmt*>(s)) {
        E(rz->exception.get()); E(rz->cause.get());
    } else if (auto* asrt = dynamic_cast<AssertStmt*>(s)) {
        E(asrt->test.get()); E(asrt->msg.get());
    } else if (auto* ds = dynamic_cast<DeferStmt*>(s)) {
        E(ds->call.get());
    } else if (auto* del = dynamic_cast<DeleteStmt*>(s)) {
        for (auto& t : del->targets) E(t.get());
    }
    // Nested FunctionDecl/ClassDecl are separate callables, not run by this call.
}

void DefiniteAssignment::Impl::scanReadsExpr(
    Expr* e, const std::unordered_set<std::string>& bound,
    std::unordered_set<std::string>& constReads,
    std::vector<FunctionDecl*>& callees) {
    if (!e) return;
    auto R = [&](Expr* x) { scanReadsExpr(x, bound, constReads, callees); };
    if (auto* n = dynamic_cast<NameExpr*>(e)) {
        if (!bound.count(n->name) && moduleConsts.count(n->name))
            constReads.insert(n->name);
        return;
    }
    if (auto* call = dynamic_cast<CallExpr*>(e)) {
        if (FunctionDecl* t = resolveModuleCallable(call->callee.get()))
            callees.push_back(t);
        R(call->callee.get());
        for (auto& a : call->args) R(a.get());
        for (auto& kw : call->kwArgs) R(kw.second.get());
        return;
    }
    if (auto* b = dynamic_cast<BinaryExpr*>(e)) { R(b->left.get()); R(b->right.get()); return; }
    if (auto* u = dynamic_cast<UnaryExpr*>(e)) { R(u->operand.get()); return; }
    if (auto* c = dynamic_cast<ChainedCompExpr*>(e)) { for (auto& o : c->operands) R(o.get()); return; }
    if (auto* at = dynamic_cast<AttributeExpr*>(e)) { R(at->object.get()); return; }
    if (auto* sub = dynamic_cast<SubscriptExpr*>(e)) { R(sub->object.get()); R(sub->index.get()); return; }
    if (auto* sl = dynamic_cast<SliceExpr*>(e)) { R(sl->lower.get()); R(sl->upper.get()); R(sl->step.get()); return; }
    if (auto* l = dynamic_cast<ListExpr*>(e)) { for (auto& el : l->elements) R(el.get()); return; }
    if (auto* t = dynamic_cast<TupleExpr*>(e)) { for (auto& el : t->elements) R(el.get()); return; }
    if (auto* st = dynamic_cast<SetExpr*>(e)) { for (auto& el : st->elements) R(el.get()); return; }
    if (auto* d = dynamic_cast<DictExpr*>(e)) {
        for (auto& kv : d->entries) { R(kv.first.get()); R(kv.second.get()); }
        return;
    }
    if (auto* str = dynamic_cast<StringLiteral*>(e)) {
        if (str->isFString)
            for (auto& part : str->fstringParts)
                if (part.kind == FStringPart::Kind::Expression) R(part.expr.get());
        return;
    }
    if (auto* tern = dynamic_cast<IfExpr*>(e)) {
        R(tern->condition.get()); R(tern->thenExpr.get()); R(tern->elseExpr.get());
        return;
    }
    if (auto* aw = dynamic_cast<AwaitExpr*>(e)) { R(aw->operand.get()); return; }
    if (auto* y = dynamic_cast<YieldExpr*>(e)) { R(y->value.get()); return; }
    if (auto* star = dynamic_cast<StarredExpr*>(e)) { R(star->value.get()); return; }
    if (auto* wl = dynamic_cast<WalrusExpr*>(e)) { R(wl->value.get()); return; }
    // Lambdas / fire / comprehensions / templates / literals: not descended (Stage 2).
}

const std::unordered_set<std::string>& DefiniteAssignment::Impl::computeReadSet(
    FunctionDecl* f, std::unordered_set<const FunctionDecl*>& visiting) {
    static const std::unordered_set<std::string> kEmpty;
    if (!f) return kEmpty;
    if (auto it = readSetMemo.find(f); it != readSetMemo.end()) return it->second;
    if (visiting.count(f)) return kEmpty;  // recursion cycle: under-approx (Stage 2)
    visiting.insert(f);

    std::unordered_set<std::string> bound;
    for (const auto& p : f->params) bound.insert(p.name);
    collectBoundNames(f->body, bound);

    std::unordered_set<std::string> reads;
    std::vector<FunctionDecl*> callees;
    scanReads(f->body, bound, reads, callees);
    for (FunctionDecl* g : callees) {
        const auto& sub = computeReadSet(g, visiting);
        reads.insert(sub.begin(), sub.end());
    }

    visiting.erase(f);
    auto ins = readSetMemo.emplace(f, std::move(reads));
    return ins.first->second;
}

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

DefiniteAssignment::DefiniteAssignment() : impl_(std::make_unique<Impl>()) {}
DefiniteAssignment::~DefiniteAssignment() = default;

bool DefiniteAssignment::analyze(Module& module) {
    impl_->diags.clear();
    impl_->reported.clear();
    impl_->moduleConsts.clear();
    impl_->moduleFuncs.clear();
    impl_->moduleClasses.clear();
    impl_->readSetMemo.clear();
    impl_->currentInitConst.clear();

    // COllect the inputs to the module-init order check - every module-lelevel
    // value binding (eagaer, source-ordered init), plus top-level function and
    // classes (call targets whose bodies may read a const during init).
    for (auto& s : module.body) {
        Stmt* st = s.get();
        if (auto* an = dynamic_cast<AnnAssignStmt*>(st)) {
            if (an->value)
                if (auto* nm = dynamic_cast<NameExpr*>(an->target.get()))
                    impl_->moduleConsts.emplace(nm->name, nm->location());
        } else if (auto* as = dynamic_cast<AssignStmt*>(st)) {
            for (auto& t : as->targets)
                if (auto* nm = dynamic_cast<NameExpr*>(t.get()))
                    impl_->moduleConsts.emplace(nm->name, nm->location());
        } else if (auto* fn = dynamic_cast<FunctionDecl*>(st)) {
            if (!fn->name.empty()) impl_->moduleFuncs.emplace(fn->name, fn);
        } else if (auto* cd = dynamic_cast<ClassDecl*>(st)) {
            if (!cd->name.empty()) impl_->moduleClasses.emplace(cd->name, cd);
        }
    }

    // The module top-level body is its own flow scope (executed top to bottom).
    impl_->scopes.clear();
    impl_->loopBreaks.clear();
    impl_->pushFrame();
    // Pre-declare every module binding as a tracked slot - a read before its
    // initializer runs is then caught by the same use-before-assignment guard
    // that protects no-initializer locals.
    for (auto& kv : impl_->moduleConsts) {
        impl_->declare(kv.first, /*tracked=*/true, kv.second);
        impl_->scopes.back()[kv.first].isModuleConst = true;
    }
    impl_->inModuleInit = true;
    Flow flow;
    for (auto& s : module.body)
        flow = impl_->analyzeStmt(s.get(), std::move(flow));
    impl_->inModuleInit = false;
    impl_->popFrame();

    return impl_->diags.empty();
}

const std::vector<DADiagnostic>& DefiniteAssignment::diagnostics() const {
    return impl_->diags;
}

bool DefiniteAssignment::hasErrors() const {
    return !impl_->diags.empty();
}

} // namespace dragon
