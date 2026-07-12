#include "dragon/OwnershipCheck.h"
#include "dragon/TypeChecker.h"  // Type::Kind - the heap-typed binding gate

#include <unordered_map>
#include <unordered_set>

namespace dragon {

//===----------------------------------------------------------------------===//
// del / own / dub ownership analysis (docs/001-memory.md, ADR docs/002).
//
// A structured forward dataflow, modeled on DefiniteAssignment: each tracked
// heap-typed local gets an integer id; a Flow carries the per-id ownership
// state at a program point. `del x` compiles only when x is Owned with no
// recorded escape/alias/capture fact - the compiler-proven sole owner. The
// pass is refusal-conservative: anything unprovable refuses with a diagnostic
// naming the cause; it never changes what the program does at runtime
//===----------------------------------------------------------------------===//

namespace {

enum class St : uint8_t {
    Untracked,  // scalar / unknown type: no ownership, del only poisons
    Owned,      // sole owner, no facts
    OwnedFact,  // sole owner, but escaped/aliased/captured (del refuses)
    Borrowed,   // names a value owned elsewhere
    Dead,       // deleted (moved arrives with `own` transfer)
    CondDead,   // dead on one branch of a join only
};

struct BindState {
    St st = St::Untracked;
    // OwnedFact: what happened and where.
    SourceLocation factLoc;
    std::string factDesc;
    bool factIsCapture = false;
    // Borrowed: who owns it (may be empty for field/element reads).
    std::string borrowOwner;
    // Dead / CondDead: where the kill happened (and, for CondDead, a live branch).
    SourceLocation killLoc;
    SourceLocation condLiveLoc;
    // Dead: HOW it died - del (killWasDel) or a move into killDesc.
    bool killWasDel = true;
    std::string killDesc;
    // Dead with a LENT flavor (E12's joined-Task borrow door): the binding
    // crossed `fire` as a plain borrow into the named bound Task, and it
    // REVIVES to Owned at `await t` / `t.join()` - the machine's only
    // backwards transition. Runtime-untouched (no slot nulling, no cleanup
    // elision - the spawn-site incref machinery covers the window); the
    // checker only blocks reads until the happens-before edge.
    std::string lentTask;  // non-empty = lent flavor
};

struct VarSlot {
    int id;
    std::string name;
    SourceLocation declLoc;
};

struct Flow {
    std::unordered_map<int, BindState> states;
    bool terminated = false;
};

} // namespace

struct OwnershipCheck::Impl {
    std::vector<OwnDiagnostic> diags;

    std::vector<std::unordered_map<std::string, VarSlot>> scopes;
    std::vector<std::vector<Flow>> loopBreaks;
    std::unordered_set<std::string> globalNames;  // `global` / `nonlocal` names
    std::unordered_set<int> reported;             // ids already diagnosed (no cascades)
    int nextId = 0;
    bool atModuleLevel = false;

    // own fields (docs/001-memory.md): class name -> its own-marked fields.
    // Collected in a prepass over the module's ClassDecls; consulted at every
    // field store (E8: a borrow cannot be stored into a sole-owner field).
    std::unordered_map<std::string, std::unordered_set<std::string>> classOwnFields;
    std::string currentClassName;  // enclosing class while analyzing a method
    std::unordered_map<int, std::string> slotNames;  // id -> name (join diags)
    std::unordered_set<std::string> e15Reported;     // "cls.field" once-only
    // ADR 2.9 door 4: classes with an own Lock field declare themselves
    // internally locked - their instances may cross a spawn as borrows.
    std::unordered_set<std::string> lockGuardedClasses;

    //==------------------------------------------------------------------=//
    // Scope helpers
    void pushFrame() { scopes.emplace_back(); }
    void popFrame() { scopes.pop_back(); }

    VarSlot* resolve(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    int declare(const std::string& name, SourceLocation loc) {
        int id = nextId++;
        scopes.back()[name] = VarSlot{id, name, loc};
        slotNames[id] = name;
        return id;
    }

    void error(SourceLocation loc, const std::string& msg) {
        diags.push_back(OwnDiagnostic{loc, msg});
    }

    static std::string lineRef(const SourceLocation& loc) {
        return "line " + std::to_string(loc.line);
    }
    // "<desc> at line N", omitting the suffix when the location is unset.
    static std::string atLine(const std::string& desc, const SourceLocation& loc) {
        return loc.line > 0 ? desc + " at line " + std::to_string(loc.line) : desc;
    }

    //------------------------------------------------------------------//
    // Type gate: ownership applies to heap values - and to Lock, a raw OS
    // resource with no refcount whose only owners are `own` fields, `with`
    // temps, and proven locals (`del` releases; docs/002 release table).
    static bool typeIsHeap(const Type* t) {
        if (!t) return false;
        switch (t->kind()) {
            case Type::Kind::Str:
            case Type::Kind::Bytes:
            case Type::Kind::List:
            case Type::Kind::Dict:
            case Type::Kind::Set:
            case Type::Kind::Tuple:
            case Type::Kind::Instance:
            case Type::Kind::Function:
            case Type::Kind::Any:
            case Type::Kind::Union:
            case Type::Kind::Lock:
                return true;
            default:
                return false;
        }
    }

    // Resolve the class a field store/read targets ("" when unknown).
    std::string resolveFieldClass(AttributeExpr* at) {
        if (auto* obj = dynamic_cast<NameExpr*>(at->object.get()))
            if (obj->name == "self") return currentClassName;
        if (at->object && at->object->type &&
            at->object->type->kind() == Type::Kind::Instance) {
            auto& inst = static_cast<InstanceType&>(*at->object->type);
            if (inst.classType) return inst.classType->name;
        }
        return "";
    }

    // Does this attribute store target an `own` field?
    bool isOwnFieldStore(AttributeExpr* at) {
        std::string cls = resolveFieldClass(at);
        if (cls.empty()) return false;
        auto it = classOwnFields.find(cls);
        return it != classOwnFields.end() && it->second.count(at->attribute) != 0;
    }

    // ADR 2.10: raw resource types (the releaser-registry types). The registry
    // doubles as the classifier: registering a releaser IS what makes the
    // mandatory rules apply. Surface names for the v1 entries.
    static bool isRawResourceTypeName(const std::string& n) {
        return n == "Lock";
    }
    static bool isRawResourceAllocCallee(const std::string& n) {
        return n == "Lock" || n == "dragon_lock_new" || n == "dragon_tls_ctx_new";
    }

    // E16 (ADR 2.10): a raw resource nested inside any container/box type.
    // Syntactic walk so it works on unresolved annotations too.
    static bool typeExprNestsRawResource(TypeExpr* t, bool nested) {
        if (!t) return false;
        if (auto* n = dynamic_cast<NamedTypeExpr*>(t))
            return nested && isRawResourceTypeName(n->name);
        if (auto* g = dynamic_cast<GenericTypeExpr*>(t)) {
            for (auto& a : g->typeArgs)
                if (typeExprNestsRawResource(a.get(), true)) return true;
            return false;
        }
        if (auto* tt = dynamic_cast<TupleTypeExpr*>(t)) {
            for (auto& e : tt->elementTypes)
                if (typeExprNestsRawResource(e.get(), true)) return true;
            return false;
        }
        if (auto* o = dynamic_cast<OptionalTypeExpr*>(t))
            return typeExprNestsRawResource(o->inner.get(), true);
        if (auto* u = dynamic_cast<UnionTypeExpr*>(t)) {
            for (auto& m : u->types)
                if (typeExprNestsRawResource(m.get(), true)) return true;
            return false;
        }
        if (auto* c = dynamic_cast<CallableTypeExpr*>(t)) {
            for (auto& p : c->paramTypes)
                if (typeExprNestsRawResource(p.get(), true)) return true;
            return typeExprNestsRawResource(c->returnType.get(), true);
        }
        return false;
    }

    void checkE16(TypeExpr* annotation, const SourceLocation& loc) {
        if (typeExprNestsRawResource(annotation, /*nested=*/false))
            error(loc,
                  "a container cannot hold raw Lock values; wrap the "
                  "resource in a class with an own field");
    }

    // Prepass: collect own fields per class and validate the marker's shape.
    // Also enforces E15 (a raw-resource field MUST be own - a non-own Lock
    // has no owner to destroy it) and E16 on field annotations.
    void collectOwnFields(Module& module) {
        for (auto& s : module.body) {
            auto* cd = dynamic_cast<ClassDecl*>(s.get());
            if (!cd) continue;
            for (auto& member : cd->body) {
                auto* ann = dynamic_cast<AnnAssignStmt*>(member.get());
                if (!ann) continue;
                auto* tgt = dynamic_cast<NameExpr*>(ann->target.get());
                if (!tgt) continue;
                checkE16(ann->annotation.get(), ann->location());
                auto* named = dynamic_cast<NamedTypeExpr*>(ann->annotation.get());
                if (!ann->isOwn) {
                    // E15: a declared raw-resource field without own.
                    if (named && isRawResourceTypeName(named->name) &&
                        e15Reported.insert(cd->name + "." + tgt->name).second) {
                        error(ann->location(),
                              named->name + " is a resource type; a " +
                                  named->name + " field must be declared own "
                                  "(a non-own " + named->name +
                                  " has no owner to destroy it)");
                    }
                    continue;
                }
                if (named && (named->name == "int" || named->name == "float" ||
                              named->name == "bool")) {
                    error(ann->location(),
                          "own has no meaning for a scalar field ('" +
                              tgt->name + "': " + named->name +
                              " is copied, not owned)");
                    continue;
                }
                // An own Lock field declares the class internally locked:
                // its instances pass the spawn boundary as borrows (2.9
                // door 4 - the lock guards the shared state; own only says
                // the class's death destroys the mutex).
                if (named && named->name == "Lock")
                    lockGuardedClasses.insert(cd->name);
                classOwnFields[cd->name].insert(tgt->name);
            }
        }
    }

    //=------------------------------------------------------------------===//
    // RHS classification (ADR 2.1). Refusal-conservative: default is Borrowed
    BindState classifyRhs(Expr* v) {
        BindState b;
        if (!v || !typeIsHeap(v->type.get())) {
            b.st = St::Untracked;
            return b;
        }
        if (auto* n = dynamic_cast<NameExpr*>(v)) {
            // `dub x` mints a fresh independent owner (docs/002 2.7); a bare
            // name is a borrow of the named binding.
            if (n->isDubMarked) {
                b.st = St::Owned;
                return b;
            }
            b.st = St::Borrowed;
            b.borrowOwner = n->name;
            return b;
        }
        if (dynamic_cast<WalrusExpr*>(v) || dynamic_cast<AttributeExpr*>(v)) {
            b.st = St::Borrowed;
            return b;
        }
        if (auto* sub = dynamic_cast<SubscriptExpr*>(v)) {
            // A slice and a str element read return a FRESH value (see
            // isBorrowedHeapExpr); other element reads borrow the container's
            // reference.
            if (dynamic_cast<SliceExpr*>(sub->index.get()) ||
                (sub->object && sub->object->type &&
                 sub->object->type->kind() == Type::Kind::Str)) {
                b.st = St::Owned;
                return b;
            }
            b.st = St::Borrowed;
            return b;
        }
        if (auto* tern = dynamic_cast<IfExpr*>(v)) {
            BindState t = classifyRhs(tern->thenExpr.get());
            BindState f = classifyRhs(tern->elseExpr.get());
            b.st = (t.st == St::Owned && f.st == St::Owned) ? St::Owned
                                                            : St::Borrowed;
            return b;
        }
        // Fresh-owning shapes: literals, constructors, calls, operators,
        // comprehensions, f-strings, dub (later slice).
        if (dynamic_cast<CallExpr*>(v) || dynamic_cast<BinaryExpr*>(v) ||
            dynamic_cast<UnaryExpr*>(v) || dynamic_cast<ListExpr*>(v) ||
            dynamic_cast<DictExpr*>(v) || dynamic_cast<SetExpr*>(v) ||
            dynamic_cast<TupleExpr*>(v) || dynamic_cast<StringLiteral*>(v) ||
            dynamic_cast<ListCompExpr*>(v) || dynamic_cast<DictCompExpr*>(v) ||
            dynamic_cast<SetCompExpr*>(v) || dynamic_cast<AwaitExpr*>(v) ||
            dynamic_cast<LambdaExpr*>(v) || dynamic_cast<FireExpr*>(v)) {
            b.st = St::Owned;
            return b;
        }
        b.st = St::Borrowed;  // anything else: refuse-conservative
        return b;
    }

    //===------------------------------------------------------------------===//
    // State mutations
    BindState* stateOf(const std::string& name, Flow& flow) {
        VarSlot* s = resolve(name);
        if (!s) return nullptr;
        auto it = flow.states.find(s->id);
        return it == flow.states.end() ? nullptr : &it->second;
    }

    // A read of `name`. Dead/CondDead names are compile errors (E1/E2/E9).
    void checkRead(NameExpr* n, Flow& flow) {
        VarSlot* s = resolve(n->name);
        if (!s) return;
        auto it = flow.states.find(s->id);
        if (it == flow.states.end()) return;
        BindState& b = it->second;
        if (b.st == St::Dead && !reported.count(s->id)) {
            reported.insert(s->id);
            if (!b.lentTask.empty())
                error(n->location(),
                      "'" + n->name + "' is lent to " + b.killDesc +
                          " until it is awaited or joined - touching it here "
                          "races the thread");
            else
                error(n->location(),
                      b.killWasDel
                          ? "'" + n->name + "' was deleted at " +
                                lineRef(b.killLoc)
                          : "'" + n->name + "' was moved into " + b.killDesc);
            b = BindState{};
            b.st = St::Owned;  // heal
        } else if (b.st == St::CondDead && !reported.count(s->id)) {
            reported.insert(s->id);
            error(n->location(),
                  "'" + n->name + "' is " +
                      atLine(b.killWasDel ? "deleted on the branch"
                                          : "moved on the branch",
                             b.killLoc) +
                      " but not on every path; consume it on every path or "
                      "on none");
            b = BindState{};
            b.st = St::Owned;  // heal
        }
    }

    // Consume `name` (del or move): the shared precondition ladder (ADR 2.4).
    // Returns 0 = diagnosed, 1 = consumed (untracked binding: poison only),
    // 2 = consumed (PROVEN Owned sole owner - the rc==1 assert applies).
    int consumeBinding(NameExpr* n, Flow& flow, bool isDel,
                       const std::string& sinkDesc) {
        VarSlot* s = resolve(n->name);
        const char* verb = isDel ? "deleted" : "moved";
        if (!s || atModuleLevel || globalNames.count(n->name)) {
            error(n->location(),
                  std::string(isDel ? "del" : "a move") +
                      " of a module global or outer binding is not supported "
                      "(v1: locals only)");
            return 0;
        }
        auto it = flow.states.find(s->id);
        BindState b = it == flow.states.end() ? BindState{} : it->second;
        switch (b.st) {
            case St::Untracked: {
                // Untracked bindings (scalars, raw ptr handles) consume as
                // poison-only: no RC bookkeeping exists, but the name dies -
                // exactly what a raw TLS-handle move needs (SSLSocket(own
                // conn)). The rc==1 assert never applies (return 1, not 2).
                BindState d;
                d.st = St::Dead;
                d.killLoc = n->location();
                d.killWasDel = isDel;
                d.killDesc = sinkDesc;
                flow.states[s->id] = d;
                return 1;
            }
            case St::Owned: {
                BindState d;
                d.st = St::Dead;
                d.killLoc = n->location();
                d.killWasDel = isDel;
                d.killDesc = sinkDesc;
                flow.states[s->id] = d;
                return 2;
            }
            case St::OwnedFact:
                error(n->location(),
                      b.factIsCapture
                          ? "'" + n->name + "' is captured by " + b.factDesc
                          : "'" + n->name + "' escaped into " + b.factDesc);
                return 0;
            case St::Borrowed:
                error(n->location(),
                      "'" + n->name + "' is not the owner" +
                          (b.borrowOwner.empty()
                               ? std::string("")
                               : " (it borrows '" + b.borrowOwner + "')") +
                          "; only the sole owner can be " + verb);
                return 0;
            case St::Dead:
                error(n->location(),
                      b.killWasDel
                          ? "'" + n->name + "' was already deleted at " +
                                lineRef(b.killLoc)
                          : "'" + n->name + "' was already moved into " +
                                b.killDesc);
                return 0;
            case St::CondDead:
                error(n->location(),
                      "'" + n->name + "' is consumed on the branch at " +
                          lineRef(b.killLoc) +
                          " but not on every path; consume it on every path "
                          "or on none");
                return 0;
        }
        return 0;
    }

    // Record an escape/alias/capture fact on `name` if it is a tracked owner.
    void recordFact(const std::string& name, Flow& flow, SourceLocation loc,
                    const std::string& desc, bool isCapture = false) {
        BindState* b = stateOf(name, flow);
        if (!b) return;
        if (b->st == St::Owned) {
            b->st = St::OwnedFact;
            b->factLoc = loc;
            b->factDesc = desc;
            b->factIsCapture = isCapture;
        }
    }

    //===------------------------------------------------------------------===//
    // Sibling paths that DISAGREE on consumpotion are on error ar the JOIN, with
    // or without a later use - the non consuming branches' socpe exit release would
    // otherwise depend on which bran ran i.e. a runtime drop flag. `del` on the
    // non-consuming path is how obligation is discharged (the 1 mandatory del case)
    Flow merge(const std::vector<Flow>& branches) {
        std::vector<const Flow*> live;
        for (const auto& b : branches)
            if (!b.terminated) live.push_back(&b);
        Flow out;
        if (live.empty()) {
            out.terminated = true;
            return out;
        }
        // Union of ids across live branches; join states per id.
        for (const Flow* f : live)
            for (const auto& [id, st] : f->states)
                if (!out.states.count(id)) out.states[id] = st;
        for (auto& [id, st] : out.states) {
            bool anyDead = false, anyAlive = false;
            const BindState* deadSt = nullptr;
            const BindState* aliveSt = nullptr;
            for (const Flow* f : live) {
                auto it = f->states.find(id);
                if (it == f->states.end()) continue;
                if (it->second.st == St::Dead) {
                    anyDead = true;
                    deadSt = &it->second;
                } else {
                    anyAlive = true;
                    aliveSt = &it->second;
                }
            }
            if (anyDead && anyAlive) {
                if (!reported.count(id)) {
                    reported.insert(id);
                    auto nm = slotNames.count(id) ? slotNames[id] : "value";
                    if (!deadSt->lentTask.empty())
                        error(deadSt->killLoc,
                              "'" + nm + "' is lent to " + deadSt->killDesc +
                                  " on this branch but not awaited on every "
                                  "path; await/join it on every path");
                    else
                        error(deadSt->killLoc,
                              "'" + nm + "' is " +
                                  (deadSt->killWasDel ? "deleted" : "moved") +
                                  " on this branch but not on every path; "
                                  "consume it on every path (del it where "
                                  "there is nothing to move into) or on none");
                }
                st = *aliveSt;  // heal to the alive state, suppress cascades
                continue;
            }
            for (const Flow* f : live) {
                auto it = f->states.find(id);
                if (it == f->states.end()) continue;
                st = join(st, it->second);
            }
        }
        return out;
    }

    static BindState join(const BindState& a, const BindState& b) {
        if (a.st == b.st) {
            // Prefer the one carrying a fact (Owned + OwnedFact handled below;
            // equal states with different facts keep the first fact).
            return a;
        }
        auto rank = [](St s) {
            switch (s) {
                case St::Untracked: return 0;
                case St::Owned:     return 1;
                case St::OwnedFact: return 2;
                case St::Borrowed:  return 3;
                case St::Dead:      return 4;
                case St::CondDead:  return 5;
            }
            return 0;
        };
        const BindState& hi = rank(a.st) >= rank(b.st) ? a : b;
        const BindState& lo = rank(a.st) >= rank(b.st) ? b : a;
        // Dead on one side, alive on the other: conditionally dead (E9 on use).
        if (hi.st == St::Dead && lo.st != St::Dead) {
            BindState out = hi;
            out.st = St::CondDead;
            out.condLiveLoc = lo.st == St::CondDead ? lo.condLiveLoc
                                                    : SourceLocation{};
            return out;
        }
        if (hi.st == St::CondDead) return hi;
        // Owned + OwnedFact: the fact survives the join.
        if (hi.st == St::OwnedFact && lo.st == St::Owned) return hi;
        // Classification disagreement (Owned vs Borrowed, or Untracked mix):
        // refuse-conservative - Borrowed.
        BindState out = hi;
        if (hi.st == St::Borrowed || lo.st == St::Borrowed) {
            out.st = St::Borrowed;
            out.borrowOwner = "";
        }
        return out;
    }

    //===------------------------------------------------------------------===//
    // Expression walk (reads + literal/retaining-call escape facts)
    //===------------------------------------------------------------------===//

    // Container methods that RETAIN their argument (the container takes a
    // reference that outlives the call). A name passed here escapes. Missing
    // a retaining method here cannot free anything wrongly: the worst case is
    // a `del` that compiles and the debug rc==1 assert firing at that line.
    static bool isRetainingMethod(const std::string& m) {
        static const std::unordered_set<std::string> k = {
            "append", "appendleft", "insert", "add", "extend",
            "setdefault", "put", "push",
        };
        return k.count(m) != 0;
    }

    // E17 (docs/002 2.11): direct mutating operations on a binding - the
    // v1 checker's claim for mutation-during-iteration. Aliases and mutation
    // inside callees stay with the runtime concurrent-mutation detector.
    static bool isMutatingMethod(const std::string& m) {
        static const std::unordered_set<std::string> k = {
            "remove", "append", "appendleft", "insert", "pop", "popleft",
            "clear", "sort", "extend", "add", "discard", "update",
            "setdefault", "reverse",
        };
        return k.count(m) != 0;
    }

    // Does this statement tree DIRECTLY mutate the binding `name`?
    // (method calls on the name, subscript-assign, del name[k])
    bool bodyMutatesBinding(const std::vector<std::unique_ptr<Stmt>>& body,
                            const std::string& name, SourceLocation& where,
                            std::string& how) {
        for (auto& s : body) {
            if (stmtMutatesBinding(s.get(), name, where, how)) return true;
        }
        return false;
    }
    bool exprMutatesBinding(Expr* e, const std::string& name,
                            SourceLocation& where, std::string& how) {
        if (!e) return false;
        if (auto* call = dynamic_cast<CallExpr*>(e)) {
            if (auto* at = dynamic_cast<AttributeExpr*>(call->callee.get()))
                if (auto* obj = dynamic_cast<NameExpr*>(at->object.get()))
                    if (obj->name == name && isMutatingMethod(at->attribute)) {
                        where = call->location();
                        how = "'" + name + "." + at->attribute + "()'";
                        return true;
                    }
            for (auto& a : call->args)
                if (exprMutatesBinding(a.get(), name, where, how)) return true;
        }
        return false;
    }
    bool stmtMutatesBinding(Stmt* s, const std::string& name,
                            SourceLocation& where, std::string& how) {
        auto tgtHits = [&](Expr* t) {
            if (auto* sub = dynamic_cast<SubscriptExpr*>(t))
                if (auto* obj = dynamic_cast<NameExpr*>(sub->object.get()))
                    if (obj->name == name) {
                        where = t->location();
                        how = "a subscript store on '" + name + "'";
                        return true;
                    }
            return false;
        };
        if (auto* e = dynamic_cast<ExprStmt*>(s))
            return exprMutatesBinding(e->expr.get(), name, where, how);
        if (auto* as = dynamic_cast<AssignStmt*>(s)) {
            for (auto& t : as->targets)
                if (tgtHits(t.get())) return true;
            return exprMutatesBinding(as->value.get(), name, where, how);
        }
        if (auto* an = dynamic_cast<AnnAssignStmt*>(s))
            return tgtHits(an->target.get()) ||
                   exprMutatesBinding(an->value.get(), name, where, how);
        if (auto* aug = dynamic_cast<AugAssignStmt*>(s))
            return tgtHits(aug->target.get());
        if (auto* del = dynamic_cast<DeleteStmt*>(s)) {
            for (auto& t : del->targets)
                if (tgtHits(t.get())) return true;
            return false;
        }
        if (auto* iff = dynamic_cast<IfStmt*>(s)) {
            if (bodyMutatesBinding(iff->thenBody, name, where, how)) return true;
            for (auto& el : iff->elifClauses)
                if (bodyMutatesBinding(el.second, name, where, how)) return true;
            return bodyMutatesBinding(iff->elseBody, name, where, how);
        }
        if (auto* wh = dynamic_cast<WhileStmt*>(s))
            return bodyMutatesBinding(wh->body, name, where, how) ||
                   bodyMutatesBinding(wh->elseBody, name, where, how);
        if (auto* fo = dynamic_cast<ForStmt*>(s))
            return bodyMutatesBinding(fo->body, name, where, how) ||
                   bodyMutatesBinding(fo->elseBody, name, where, how);
        if (auto* tr = dynamic_cast<TryStmt*>(s)) {
            if (bodyMutatesBinding(tr->tryBody, name, where, how)) return true;
            for (auto& h : tr->handlers)
                if (bodyMutatesBinding(h.body, name, where, how)) return true;
            return bodyMutatesBinding(tr->elseBody, name, where, how) ||
                   bodyMutatesBinding(tr->finallyBody, name, where, how);
        }
        if (auto* w = dynamic_cast<WithStmt*>(s))
            return bodyMutatesBinding(w->body, name, where, how);
        if (auto* m = dynamic_cast<MatchStmt*>(s)) {
            for (auto& c : m->cases)
                if (bodyMutatesBinding(c.body, name, where, how)) return true;
            return false;
        }
        return false;
    }

    //===------------------------------------------------------------------===//
    // E12 - the spawn boundary (ADR 2.9) with the joined-Task borrow door.
    // Every heap value crossing `fire` must be moved (own), copied (dub),
    // fresh, internally locked, or - when the Task handle is BOUND - LENT
    // until the `await`/`join` revives it. A discarded handle has no join
    // that could ever end the lend, so plain borrows are a hard E12 there.
    //===------------------------------------------------------------------===//

    // Revive every binding lent to `taskName` (the only backwards transition).
    void reviveLends(const std::string& taskName, Flow& flow) {
        for (auto& [id, st] : flow.states) {
            if (st.st == St::Dead && st.lentTask == taskName) {
                st = BindState{};
                st.st = St::Owned;
                reported.erase(id);
            }
        }
    }

    // One expression crossing the spawn boundary.
    // taskName empty + !immediateAwait = discarded handle (hard E12).
    void checkSpawnCrossing(Expr* e, Flow& flow, const std::string& taskName,
                            bool immediateAwait, SourceLocation fireLoc) {
        if (!e) return;
        if (auto* mv = dynamic_cast<NameExpr*>(e); mv && mv->isMoveMarked) {
            consumeBinding(mv, flow, /*isDel=*/false,
                           atLine("a spawned task", fireLoc));
            return;
        }
        if (auto* db = dynamic_cast<NameExpr*>(e); db && db->isDubMarked) {
            checkRead(db, flow);  // a fresh copy crosses; the source stays
            return;
        }
        auto* nm = dynamic_cast<NameExpr*>(e);
        if (!nm) {
            checkExpr(e, flow);  // fresh temp / literal: implicit move
            return;
        }
        checkRead(nm, flow);
        if (!nm->type || !typeIsHeap(nm->type.get())) return;  // scalar / raw
        if (nm->type->kind() == Type::Kind::Lock) return;  // the lock IS a door
        // Internally-locked class instance (door 4).
        if (nm->type->kind() == Type::Kind::Instance) {
            auto& inst = static_cast<InstanceType&>(*nm->type);
            if (inst.classType && lockGuardedClasses.count(inst.classType->name))
                return;
        }
        if (nm->name == "self" && lockGuardedClasses.count(currentClassName))
            return;
        if (immediateAwait) return;  // `await fire f(o)`: the borrow window
                                     // IS the await - already synchronized
        VarSlot* s = resolve(nm->name);
        auto it = s ? flow.states.find(s->id) : flow.states.end();
        BindState b = it == flow.states.end() ? BindState{} : it->second;
        if (!taskName.empty() && b.st == St::Owned) {
            // LEND: reads blocked until the task's await/join revives it.
            BindState d;
            d.st = St::Dead;
            d.killLoc = nm->location();
            d.killWasDel = false;
            d.killDesc = "task '" + taskName + "'";
            d.lentTask = taskName;
            flow.states[s->id] = d;
            return;
        }
        if (b.st == St::Untracked) return;  // unknown type: runtime doors
        std::string why =
            taskName.empty()
                ? "the Task handle is discarded, so no await/join can ever "
                  "end the lend"
                : (b.st == St::OwnedFact
                       ? "'" + nm->name + "' escaped into " + b.factDesc +
                             " - another reference could race the thread"
                       : "'" + nm->name + "' is a borrow, not the sole owner");
        error(nm->location(),
              "'" + nm->name + "' crosses a thread boundary; move it (own " +
                  nm->name + "), copy it (dub " + nm->name +
                  "), or make it a locked type (" + why + ")");
    }

    void processFire(FireExpr* fire, Flow& flow, const std::string& taskName,
                     bool immediateAwait) {
        for (const auto& cap : fire->capturedVars)
            recordFact(cap, flow, fire->location(),
                       atLine("a green thread", fire->location()),
                       /*isCapture=*/true);
        if (!fire->bodyStmts.empty()) analyzeStmtsFresh(fire->bodyStmts);
        auto* call = dynamic_cast<CallExpr*>(fire->operand.get());
        if (!call) {
            checkExpr(fire->operand.get(), flow);
            return;
        }
        // The RECEIVER of a fired method call crosses too.
        if (auto* at = dynamic_cast<AttributeExpr*>(call->callee.get()))
            checkSpawnCrossing(at->object.get(), flow, taskName,
                               immediateAwait, fire->location());
        for (auto& a : call->args)
            checkSpawnCrossing(a.get(), flow, taskName, immediateAwait,
                               fire->location());
        for (auto& kw : call->kwArgs)
            checkSpawnCrossing(kw.second.get(), flow, taskName,
                               immediateAwait, fire->location());
    }

    // A scope is closing (return / callable end): a still-lent binding means
    // the runtime release would race the thread that borrowed it.
    void checkNoOutstandingLends(const Flow& flow, SourceLocation loc,
                                 const char* where) {
        for (const auto& [id, st] : flow.states) {
            if (st.st != St::Dead || st.lentTask.empty()) continue;
            if (reported.count(id)) continue;
            reported.insert(id);
            auto nm = slotNames.count(id) ? slotNames.at(id) : "a binding";
            error(loc.line ? loc : st.killLoc,
                  "'" + nm + "' is still lent to " + st.killDesc + " at " +
                      where + "; await or join the task first");
        }
    }

    void factForNameElements(Expr* e, Flow& flow, const std::string& desc) {
        if (auto* n = dynamic_cast<NameExpr*>(e)) {
            checkRead(n, flow);
            recordFact(n->name, flow, n->location(), desc);
            return;
        }
        checkExpr(e, flow);
    }

    void checkExpr(Expr* e, Flow& flow) {
        if (!e) return;
        if (auto* n = dynamic_cast<NameExpr*>(e)) {
            checkRead(n, flow);
            return;
        }
        if (auto* w = dynamic_cast<WalrusExpr*>(e)) {
            checkExpr(w->value.get(), flow);
            // Walrus targets adopt their value; the expression hands its
            // consumer a borrow of the slot (the A/B-proven walrus wall).
            int id = declare(w->name, w->location());
            BindState b;
            b.st = St::Borrowed;
            flow.states[id] = b;
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
            // A retaining container method escapes NameExpr args; every other
            // call argument is a plain borrow for the duration of the call
            // (the bible's `d = sha256(buf); del buf` must compile).
            bool retains = false;
            std::string mname;
            if (auto* at = dynamic_cast<AttributeExpr*>(call->callee.get())) {
                if (isRetainingMethod(at->attribute)) {
                    retains = true;
                    mname = at->attribute;
                }
            }
            std::string calleeDesc = "a call";
            if (auto* cn = dynamic_cast<NameExpr*>(call->callee.get()))
                calleeDesc = "'" + cn->name + "()'";
            else if (auto* ca = dynamic_cast<AttributeExpr*>(call->callee.get()))
                calleeDesc = "'" + ca->attribute + "()'";
            // `t.join()` is the same happens-before edge as `await t`.
            if (auto* jat = dynamic_cast<AttributeExpr*>(call->callee.get()))
                if (jat->attribute == "join")
                    if (auto* jt = dynamic_cast<NameExpr*>(jat->object.get()))
                        reviveLends(jt->name, flow);
            for (auto& a : call->args) {
                // `f(own x)` - the caller's +1 transfers (ADR 2.4/2.8): the
                // shared consuming ladder runs the same preconditions as del
                // and x is Moved afterwards. E13/E14 signature matching is
                // the TypeChecker's; this is the caller-side state change.
                if (auto* mv = dynamic_cast<NameExpr*>(a.get());
                    mv && mv->isMoveMarked) {
                    consumeBinding(mv, flow, /*isDel=*/false,
                                   atLine(calleeDesc, call->location()));
                    continue;
                }
                if (retains) {
                    factForNameElements(a.get(), flow,
                                        "'" + mname + "()' at " +
                                            lineRef(call->location()));
                } else {
                    checkExpr(a.get(), flow);
                }
            }
            for (auto& kw : call->kwArgs) checkExpr(kw.second.get(), flow);
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
        // Container literals RETAIN their elements: a name stored in one has
        // a second owner for as long as the literal lives, so it escapes.
        // (Conservative: a literal consumed transiently - print([x]) - also
        // records the fact; the refusal is a diagnostic, never a wrong free.)
        if (auto* l = dynamic_cast<ListExpr*>(e)) {
            for (auto& el : l->elements)
                factForNameElements(el.get(), flow,
                                    "a list literal at " + lineRef(l->location()));
            return;
        }
        if (auto* t = dynamic_cast<TupleExpr*>(e)) {
            for (auto& el : t->elements)
                factForNameElements(el.get(), flow,
                                    "a tuple literal at " + lineRef(t->location()));
            return;
        }
        if (auto* st = dynamic_cast<SetExpr*>(e)) {
            for (auto& el : st->elements)
                factForNameElements(el.get(), flow,
                                    "a set literal at " + lineRef(st->location()));
            return;
        }
        if (auto* d = dynamic_cast<DictExpr*>(e)) {
            for (auto& kv : d->entries) {
                factForNameElements(kv.first.get(), flow,
                                    "a dict literal at " + lineRef(d->location()));
                factForNameElements(kv.second.get(), flow,
                                    "a dict literal at " + lineRef(d->location()));
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
            Flow t = flow;
            checkExpr(tern->thenExpr.get(), t);
            Flow f = flow;
            checkExpr(tern->elseExpr.get(), f);
            return;
        }
        if (auto* lam = dynamic_cast<LambdaExpr*>(e)) {
            for (const auto& cap : lam->capturedVars)
                recordFact(cap, flow, lam->location(),
                           atLine("a closure", lam->location()),
                           /*isCapture=*/true);
            std::vector<Parameter> none;
            // Body reads of outer names are re-resolved in a fresh context
            // (they become Untracked there); the capture facts above carry
            // the ownership consequence.
            analyzeLambdaBody(lam);
            return;
        }
        if (auto* fire = dynamic_cast<FireExpr*>(e)) {
            // A FireExpr reaching the generic walker is not bound to a Task
            // name (bound fires are intercepted at the assignment handlers):
            // conservatively a discarded handle - plain borrows are E12.
            processFire(fire, flow, "", /*immediateAwait=*/false);
            return;
        }
        if (auto* aw = dynamic_cast<AwaitExpr*>(e)) {
            if (auto* fire = dynamic_cast<FireExpr*>(aw->operand.get())) {
                // `await fire f(o)`: the borrow window IS the await.
                processFire(fire, flow, "", /*immediateAwait=*/true);
                return;
            }
            if (auto* tn = dynamic_cast<NameExpr*>(aw->operand.get())) {
                checkRead(tn, flow);
                reviveLends(tn->name, flow);  // the happens-before edge
                return;
            }
            checkExpr(aw->operand.get(), flow);
            return;
        }
        if (auto* y = dynamic_cast<YieldExpr*>(e)) {
            checkExpr(y->value.get(), flow);
            return;
        }
        if (auto* st = dynamic_cast<StarredExpr*>(e)) {
            checkExpr(st->value.get(), flow);
            return;
        }
        if (auto* lc = dynamic_cast<ListCompExpr*>(e)) {
            checkExpr(lc->iterable.get(), flow);
            return;  // comprehension bodies run in their own scope
        }
        if (auto* dc = dynamic_cast<DictCompExpr*>(e)) {
            checkExpr(dc->iterable.get(), flow);
            return;
        }
        if (auto* sc = dynamic_cast<SetCompExpr*>(e)) {
            checkExpr(sc->iterable.get(), flow);
            return;
        }
        // Literals and anything else: no reads to track.
    }

    //===------------------------------------------------------------------===//
    // Statements
    //===------------------------------------------------------------------===//

    void bindTarget(Expr* target, Expr* value, Flow& flow) {
        // A move-marked RHS is only meaningful where something CONSUMES the
        // +1: an own field (below) or an own parameter (call sites). A local
        // or global target would just be an alias wearing a costume.
        auto* mvRhs = dynamic_cast<NameExpr*>(value);
        bool rhsIsMove = mvRhs && mvRhs->isMoveMarked;
        if (auto* n = dynamic_cast<NameExpr*>(target)) {
            if (rhsIsMove) {
                error(value->location(),
                      "a move needs a consuming destination (an own field or "
                      "an own parameter); '" + n->name +
                          " = own " + mvRhs->name + "' would only alias");
                return;
            }
            // Assignment to a `global`/`nonlocal` name stores into an outer
            // owner: the RHS name (if any) escapes.
            if (globalNames.count(n->name)) {
                if (auto* rn = dynamic_cast<NameExpr*>(value))
                    recordFact(rn->name, flow, n->location(),
                               "global '" + n->name + "' at " +
                                   lineRef(n->location()));
                return;
            }
            // `y = x`: y borrows x, and x gains an alias fact (one owner,
            // one name - Q2 signed off as a compile error on `del x`).
            if (auto* rn = dynamic_cast<NameExpr*>(value)) {
                recordFact(rn->name, flow, n->location(),
                           "alias '" + n->name + "' at " + lineRef(n->location()));
            }
            // Rebinding a Task name while something is lent to it would lose
            // the only handle that can ever end the lend (and a later await
            // of the NEW task would falsely revive the old lend).
            for (auto& [lid, lst] : flow.states) {
                if (lst.st == St::Dead && lst.lentTask == n->name &&
                    !reported.count(lid)) {
                    reported.insert(lid);
                    auto lname = slotNames.count(lid) ? slotNames[lid]
                                                      : "a binding";
                    error(n->location(),
                          "'" + n->name + "' is rebound while '" + lname +
                              "' is lent to it; await or join it first");
                }
            }
            BindState b = classifyRhs(value);
            VarSlot* s = resolve(n->name);
            int id = s ? s->id : declare(n->name, n->location());
            flow.states[id] = b;
            reported.erase(id);  // a rebind is a fresh generation
            return;
        }
        if (auto* t = dynamic_cast<TupleExpr*>(target)) {
            // Unpack: every element borrows from the unpacked value.
            for (auto& el : t->elements) {
                if (auto* n2 = dynamic_cast<NameExpr*>(el.get())) {
                    BindState b;
                    b.st = St::Borrowed;
                    VarSlot* s = resolve(n2->name);
                    int id = s ? s->id : declare(n2->name, n2->location());
                    flow.states[id] = b;
                } else {
                    checkExpr(el.get(), flow);
                }
            }
            return;
        }
        if (auto* sub = dynamic_cast<SubscriptExpr*>(target)) {
            // c[k] = x - x escapes into the container.
            checkExpr(sub->object.get(), flow);
            checkExpr(sub->index.get(), flow);
            if (auto* rn = dynamic_cast<NameExpr*>(value))
                recordFact(rn->name, flow, target->location(),
                           "a container store at " + lineRef(target->location()));
            return;
        }
        if (auto* at = dynamic_cast<AttributeExpr*>(target)) {
            checkExpr(at->object.get(), flow);
            // E15 (ADR 2.10): a raw resource stored into a NON-own field has
            // no owner to destroy it. Keyed on the allocator callee - the
            // registry doubles as the classifier - so undeclared (ctor-
            // assigned-only) fields are caught too.
            if (!isOwnFieldStore(at)) {
                if (auto* call = dynamic_cast<CallExpr*>(value)) {
                    auto* callee = dynamic_cast<NameExpr*>(call->callee.get());
                    if (callee && isRawResourceAllocCallee(callee->name)) {
                        std::string cls = resolveFieldClass(at);
                        if (e15Reported
                                .insert((cls.empty() ? "?" : cls) + "." +
                                        at->attribute)
                                .second)
                            error(target->location(),
                                  "Lock is a resource type; a Lock field must "
                                  "be declared own (a non-own Lock has no "
                                  "owner to destroy it)");
                    }
                }
            }
            // An OWN field takes SOLE ownership (docs/001-memory.md): only a
            // fresh value may be stored (its +1 transfers). A borrow - a named
            // binding, a field/element read - is E8. v1 note: moving a NAMED
            // Owned binding (`self._f = own x`) arrives with the own-transfer
            // slice; until then a name is refused with the same diagnostic.
            if (isOwnFieldStore(at)) {
                // `self._f = own x` (ADR 2.4 row 3): the named binding's +1
                // moves into the field through the shared consuming ladder
                // (Owned-clean required); the name is Moved afterwards.
                if (rhsIsMove) {
                    consumeBinding(mvRhs, flow, /*isDel=*/false,
                                   atLine("own field '" + at->attribute + "'",
                                          target->location()));
                    return;
                }
                if (value && typeIsHeap(value->type.get()) &&
                    classifyRhs(value).st != St::Owned) {
                    error(target->location(),
                          "field '" + at->attribute +
                              "' is an own field and takes sole ownership; "
                              "move it (own " +
                              (dynamic_cast<NameExpr*>(value)
                                   ? dynamic_cast<NameExpr*>(value)->name
                                   : std::string("x")) +
                              ") or store a fresh value - a borrow cannot "
                              "be stored");
                }
                return;
            }
            if (rhsIsMove) {
                error(value->location(),
                      "field '" + at->attribute + "' is not an own field; a "
                      "move needs a consuming destination");
                return;
            }
            // obj.f = x - x escapes into a plain field.
            if (auto* rn = dynamic_cast<NameExpr*>(value))
                recordFact(rn->name, flow, target->location(),
                           "field '" + at->attribute + "' at " +
                               lineRef(target->location()));
            return;
        }
        if (auto* st = dynamic_cast<StarredExpr*>(target)) {
            bindTarget(st->value.get(), value, flow);
            return;
        }
    }

    void handleDelete(DeleteStmt* del, Flow& flow) {
        del->provenUnique.assign(del->targets.size(), 0);
        for (size_t i = 0; i < del->targets.size(); ++i) {
            Expr* t = del->targets[i].get();
            if (dynamic_cast<AttributeExpr*>(t)) {
                error(t->location(),
                      "del of a field does not compile: a field's lifetime is "
                      "dynamic; own fields release when the owner dies");
                continue;
            }
            if (auto* sub = dynamic_cast<SubscriptExpr*>(t)) {
                // Container-element deletion (del d[k], del xs[i]) is the
                // existing statement semantics, not an ownership event.
                checkExpr(sub->object.get(), flow);
                checkExpr(sub->index.get(), flow);
                continue;
            }
            auto* n = dynamic_cast<NameExpr*>(t);
            if (!n) continue;
            // Shared consuming-transition ladder (ADR 2.4). provenUnique
            // marks only the PROVEN-Owned case: the -O0 rc==1 assert applies
            // to compiler-proven sole owners, never to untracked scalars.
            int r = consumeBinding(n, flow, /*isDel=*/true, "");
            if (r == 2) del->provenUnique[i] = 1;
        }
    }

    Flow analyzeBlock(const std::vector<std::unique_ptr<Stmt>>& body, Flow flow) {
        pushFrame();
        for (const auto& s : body) flow = analyzeStmt(s.get(), std::move(flow));
        popFrame();
        return flow;
    }

    // Loop rule (ADR 2.5): a binding that predates the loop must leave the
    // body in the state it entered with; killed-on-the-back-edge is E10.
    void checkBackEdge(const Flow& entry, Flow& exit, SourceLocation loopLoc) {
        for (const auto& [id, st] : entry.states) {
            if (st.st == St::Dead || st.st == St::CondDead) continue;
            auto it = exit.states.find(id);
            if (it == exit.states.end()) continue;
            if ((it->second.st == St::Dead || it->second.st == St::CondDead) &&
                !reported.count(id)) {
                reported.insert(id);
                error(it->second.killLoc.line ? it->second.killLoc : loopLoc,
                      "deleted on iteration 1; iteration 2 would use a dead "
                      "name - delete it after the loop instead");
                it->second = st;  // heal to entry state
            }
        }
    }

    Flow analyzeStmt(Stmt* s, Flow flow) {
        if (flow.terminated) return flow;

        if (auto* e = dynamic_cast<ExprStmt*>(s)) {
            checkExpr(e->expr.get(), flow);
            return flow;
        }
        if (auto* as = dynamic_cast<AssignStmt*>(s)) {
            // `t = fire f(o)`: a BOUND task - plain heap borrows LEND to `t`
            // instead of hard-E12ing (the joined-Task door). The target is
            // bound FIRST so the lend attaches to the fresh generation of
            // `t` (binding it is not "rebinding while lent").
            auto* fireVal = dynamic_cast<FireExpr*>(as->value.get());
            auto* soleName = as->targets.size() == 1
                                 ? dynamic_cast<NameExpr*>(as->targets[0].get())
                                 : nullptr;
            if (fireVal && soleName) {
                bindTarget(as->targets[0].get(), as->value.get(), flow);
                processFire(fireVal, flow, soleName->name, false);
                return flow;
            }
            checkExpr(as->value.get(), flow);
            if (as->targets.size() == 1) {
                bindTarget(as->targets[0].get(), as->value.get(), flow);
            } else {
                for (auto& t : as->targets)
                    bindTarget(t.get(), /*value=*/nullptr, flow);
            }
            return flow;
        }
        if (auto* an = dynamic_cast<AnnAssignStmt*>(s)) {
            // Class-body field declarations never reach here (the ClassDecl
            // branch only recurses into methods), so an isOwn AnnAssign in
            // statement position is misplaced by construction.
            if (an->isOwn)
                error(an->location(),
                      "own marks a class FIELD as sole owner; it has no "
                      "meaning on a local or module binding (del releases a "
                      "proven local early)");
            checkE16(an->annotation.get(), an->location());
            {
                auto* fireVal = dynamic_cast<FireExpr*>(an->value.get());
                auto* tn = dynamic_cast<NameExpr*>(an->target.get());
                if (fireVal && tn) {
                    // Bind the task name first, then lend (see AssignStmt).
                    bindTarget(an->target.get(), an->value.get(), flow);
                    processFire(fireVal, flow, tn->name, false);
                    return flow;
                }
                checkExpr(an->value.get(), flow);
            }
            bindTarget(an->target.get(), an->value.get(), flow);
            return flow;
        }
        if (auto* aug = dynamic_cast<AugAssignStmt*>(s)) {
            checkExpr(aug->value.get(), flow);
            // The target is read and rewritten; its classification is
            // type-dependent (str += rebinds, list += mutates in place), so
            // the state is left unchanged - refuse-conservative for del.
            checkExpr(aug->target.get(), flow);
            return flow;
        }
        if (auto* del = dynamic_cast<DeleteStmt*>(s)) {
            handleDelete(del, flow);
            return flow;
        }
        if (auto* iff = dynamic_cast<IfStmt*>(s)) {
            checkExpr(iff->condition.get(), flow);
            std::vector<Flow> branches;
            branches.push_back(analyzeBlock(iff->thenBody, flow));
            for (auto& el : iff->elifClauses) {
                Flow ef = flow;
                checkExpr(el.first.get(), ef);
                branches.push_back(analyzeBlock(el.second, std::move(ef)));
            }
            branches.push_back(analyzeBlock(iff->elseBody, flow));
            return merge(branches);
        }
        if (auto* wh = dynamic_cast<WhileStmt*>(s)) {
            checkExpr(wh->condition.get(), flow);
            Flow entry = flow;
            loopBreaks.emplace_back();
            Flow bodyOut = analyzeBlock(wh->body, flow);
            checkBackEdge(entry, bodyOut, wh->location());
            std::vector<Flow> outs = std::move(loopBreaks.back());
            loopBreaks.pop_back();
            outs.push_back(std::move(bodyOut));
            outs.push_back(entry);  // loop may run zero times
            Flow out = merge(outs);
            if (!wh->elseBody.empty()) out = analyzeBlock(wh->elseBody, std::move(out));
            return out;
        }
        if (auto* fo = dynamic_cast<ForStmt*>(s)) {
            checkExpr(fo->iterable.get(), flow);
            // E17 (docs/002 2.11, the one mandatory-dub site): iterating a
            // binding while the body DIRECTLY mutates it silently observes
            // its own mutations (remove() shifts the next element past the
            // cursor - reproduced live on this compiler). `for x in dub xs`
            // iterates a priced snapshot; collect-then-apply also compiles.
            if (auto* itn = dynamic_cast<NameExpr*>(fo->iterable.get());
                itn && !itn->isDubMarked) {
                SourceLocation where;
                std::string how;
                if (bodyMutatesBinding(fo->body, itn->name, where, how)) {
                    error(where,
                          how + " mutates '" + itn->name + "' while the loop "
                          "iterates it - the loop would observe its own "
                          "mutations; iterate a snapshot (for ... in dub " +
                          itn->name + ") or collect the changes and apply "
                          "them after the loop");
                }
            } else if (auto* dn = dynamic_cast<NameExpr*>(fo->iterable.get());
                       dn && dn->isDubMarked && dn->type) {
                auto k = dn->type->kind();
                // A snapshot of an immutable is the value itself: iterating
                // it cannot observe mutations, so the dub buys nothing.
                if (k == Type::Kind::Str || k == Type::Kind::Bytes ||
                    k == Type::Kind::Tuple) {
                    error(dn->location(),
                          "'" + dn->name + "' is immutable; iterating a "
                          "snapshot of it is meaningless - drop the dub");
                }
            }
            Flow entry = flow;
            loopBreaks.emplace_back();
            pushFrame();
            // Loop targets borrow the iterable's elements.
            bindTarget(fo->target.get(), /*value=*/nullptr, flow);
            Flow bodyOut = flow;
            for (const auto& st : fo->body)
                bodyOut = analyzeStmt(st.get(), std::move(bodyOut));
            popFrame();
            checkBackEdge(entry, bodyOut, fo->location());
            std::vector<Flow> outs = std::move(loopBreaks.back());
            loopBreaks.pop_back();
            outs.push_back(std::move(bodyOut));
            outs.push_back(entry);
            Flow out = merge(outs);
            if (!fo->elseBody.empty()) out = analyzeBlock(fo->elseBody, std::move(out));
            return out;
        }
        if (auto* tr = dynamic_cast<TryStmt*>(s)) {
            Flow tryOut = analyzeBlock(tr->tryBody, flow);
            // A handler can run after ANY prefix of the try body: its entry is
            // the conservative join of the entry and the try's fall-through.
            std::vector<Flow> handlerEntryParts;
            handlerEntryParts.push_back(flow);
            handlerEntryParts.push_back(tryOut);
            Flow handlerEntry = merge(handlerEntryParts);
            std::vector<Flow> outs;
            if (!tr->elseBody.empty() && !tryOut.terminated)
                outs.push_back(analyzeBlock(tr->elseBody, tryOut));
            else
                outs.push_back(tryOut);
            for (auto& h : tr->handlers) {
                Flow hf = handlerEntry;
                pushFrame();
                if (!h.name.empty()) {
                    BindState b;
                    b.st = St::Borrowed;  // the exception slot owns it
                    int id = declare(h.name, SourceLocation{});
                    hf.states[id] = b;
                }
                for (const auto& st : h.body)
                    hf = analyzeStmt(st.get(), std::move(hf));
                popFrame();
                outs.push_back(std::move(hf));
            }
            Flow out = merge(outs);
            if (!tr->finallyBody.empty())
                out = analyzeBlock(tr->finallyBody, std::move(out));
            return out;
        }
        if (auto* w = dynamic_cast<WithStmt*>(s)) {
            pushFrame();
            for (auto& item : w->items) {
                checkExpr(item.contextExpr.get(), flow);
                if (auto* n = dynamic_cast<NameExpr*>(item.optionalVars.get())) {
                    // The with statement holds its own release of the subject
                    // (__exit__/cleanup stack): the binding is a borrow, so a
                    // del inside the body refuses instead of double-freeing.
                    BindState b;
                    b.st = St::Borrowed;
                    b.borrowOwner = "the with statement";
                    int id = declare(n->name, n->location());
                    flow.states[id] = b;
                }
            }
            for (const auto& st : w->body) flow = analyzeStmt(st.get(), std::move(flow));
            popFrame();
            return flow;
        }
        if (auto* m = dynamic_cast<MatchStmt*>(s)) {
            checkExpr(m->subject.get(), flow);
            std::vector<Flow> branches;
            for (auto& c : m->cases) {
                Flow cf = flow;
                pushFrame();
                bindPattern(c.pattern, cf);
                if (c.guard) checkExpr(c.guard.get(), cf);
                for (const auto& st : c.body) cf = analyzeStmt(st.get(), std::move(cf));
                popFrame();
                branches.push_back(std::move(cf));
            }
            branches.push_back(flow);  // no case may match
            return merge(branches);
        }
        if (auto* r = dynamic_cast<ReturnStmt*>(s)) {
            checkExpr(r->value.get(), flow);
            // Returning while a binding is lent: the scope's release would
            // race the thread still borrowing it.
            checkNoOutstandingLends(flow, r->location(), "this return");
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
        if (auto* g = dynamic_cast<GlobalStmt*>(s)) {
            for (auto& n : g->names) globalNames.insert(n);
            return flow;
        }
        if (auto* nl = dynamic_cast<NonlocalStmt*>(s)) {
            for (auto& n : nl->names) globalNames.insert(n);
            return flow;
        }
        if (auto* asrt = dynamic_cast<AssertStmt*>(s)) {
            checkExpr(asrt->test.get(), flow);
            checkExpr(asrt->msg.get(), flow);
            return flow;
        }
        if (auto* th = dynamic_cast<ThreadStmt*>(s)) {
            for (const auto& cap : th->capturedVars)
                recordFact(cap, flow, th->location(),
                           "a thread block at " + lineRef(th->location()),
                           /*isCapture=*/true);
            return analyzeBlock(th->body, std::move(flow));
        }
        if (auto* fn = dynamic_cast<FunctionDecl*>(s)) {
            for (const auto& cap : fn->capturedVars)
                recordFact(cap, flow, fn->location(),
                           "nested function '" + fn->name + "' at " +
                               lineRef(fn->location()),
                           /*isCapture=*/true);
            for (const auto& p : fn->params)
                checkE16(p.type.get(), fn->location());
            checkE16(fn->returnType.get(), fn->location());
            analyzeCallable(fn->params, fn->body, fn->isMethod);
            return flow;
        }
        if (auto* cd = dynamic_cast<ClassDecl*>(s)) {
            std::string savedClass = currentClassName;
            currentClassName = cd->name;
            for (auto& member : cd->body)
                if (auto* mfn = dynamic_cast<FunctionDecl*>(member.get())) {
                    for (const auto& p : mfn->params)
                        checkE16(p.type.get(), mfn->location());
                    checkE16(mfn->returnType.get(), mfn->location());
                    analyzeCallable(mfn->params, mfn->body, /*isMethod=*/true);
                }
            currentClassName = savedClass;
            return flow;
        }
        // Import / Pass / everything else: no ownership consequence.
        return flow;
    }

    void bindPattern(const MatchPattern& p, Flow& flow) {
        switch (p.kind) {
            case MatchPattern::Kind::Capture:
                if (!p.name.empty() && p.name != "_") {
                    BindState b;
                    b.st = St::Borrowed;  // pattern captures borrow the subject
                    int id = declare(p.name, SourceLocation{});
                    flow.states[id] = b;
                }
                break;
            case MatchPattern::Kind::Sequence:
            case MatchPattern::Kind::Or:
                for (const auto& sub : p.subPatterns) bindPattern(sub, flow);
                break;
            default:
                break;
        }
    }

    void analyzeCallable(const std::vector<Parameter>& params,
                         const std::vector<std::unique_ptr<Stmt>>& body,
                         bool isMethod) {
        auto savedScopes = std::move(scopes);
        auto savedLoops = std::move(loopBreaks);
        auto savedGlobals = globalNames;
        bool savedModule = atModuleLevel;
        scopes.clear();
        loopBreaks.clear();
        atModuleLevel = false;

        pushFrame();
        Flow flow;
        if (isMethod) {
            BindState b;
            b.st = St::Borrowed;
            int id = declare("self", SourceLocation{});
            flow.states[id] = b;
        }
        for (const auto& p : params) {
            BindState b;
            // ADR 2.1: plain parameters borrow; `own p: T` arrives OWNED (the
            // caller moved its +1 in; the callee's scope exit releases it if
            // the body did not consume it).
            b.st = p.isOwn ? St::Owned : St::Borrowed;
            int id = declare(p.name, SourceLocation{});
            flow.states[id] = b;
        }
        Flow out = analyzeBlock(body, std::move(flow));
        // The callable is closing: every lend must have ended (the scope's
        // releases would otherwise race the borrowing thread).
        if (!out.terminated)
            checkNoOutstandingLends(out, SourceLocation{}, "the end of the function");
        popFrame();

        scopes = std::move(savedScopes);
        loopBreaks = std::move(savedLoops);
        globalNames = std::move(savedGlobals);
        atModuleLevel = savedModule;
    }

    void analyzeLambdaBody(LambdaExpr* lam) {
        auto savedScopes = std::move(scopes);
        auto savedLoops = std::move(loopBreaks);
        bool savedModule = atModuleLevel;
        scopes.clear();
        loopBreaks.clear();
        atModuleLevel = false;

        pushFrame();
        Flow flow;
        for (const auto& p : lam->params) {
            BindState b;
            b.st = St::Borrowed;
            int id = declare(p.name, SourceLocation{});
            flow.states[id] = b;
        }
        if (lam->body) checkExpr(lam->body.get(), flow);
        if (!lam->bodyStmts.empty()) analyzeBlock(lam->bodyStmts, std::move(flow));
        popFrame();

        scopes = std::move(savedScopes);
        loopBreaks = std::move(savedLoops);
        atModuleLevel = savedModule;
    }

    void analyzeStmtsFresh(const std::vector<std::unique_ptr<Stmt>>& body) {
        std::vector<Parameter> none;
        analyzeCallable(none, body, /*isMethod=*/false);
    }
};

//===----------------------------------------------------------------------===//
// Public interface
//===----------------------------------------------------------------------===//

OwnershipCheck::OwnershipCheck() : impl_(std::make_unique<Impl>()) {}
OwnershipCheck::~OwnershipCheck() = default;

bool OwnershipCheck::analyze(Module& module) {
    impl_->diags.clear();
    impl_->scopes.clear();
    impl_->loopBreaks.clear();
    impl_->globalNames.clear();
    impl_->reported.clear();
    impl_->nextId = 0;

    // Module top level: bindings here are globals; `del` on them refuses
    // (v1: locals only). Function/class bodies analyze as callables.
    impl_->classOwnFields.clear();
    impl_->currentClassName.clear();
    impl_->collectOwnFields(module);

    impl_->atModuleLevel = true;
    impl_->pushFrame();
    Flow flow;
    for (const auto& s : module.body)
        flow = impl_->analyzeStmt(s.get(), std::move(flow));
    impl_->popFrame();

    return impl_->diags.empty();
}

const std::vector<OwnDiagnostic>& OwnershipCheck::diagnostics() const {
    return impl_->diags;
}

bool OwnershipCheck::hasErrors() const { return !impl_->diags.empty(); }

} // namespace dragon
