// TODO: except-as targets probably need their own scope depth
#include "dragon/Sema.h"
#include <functional>
#include <set>

namespace dragon {

//===----------------------------------------------------------------------===//
// Scope Implementation
//===----------------------------------------------------------------------===//

Scope::Scope(Kind kind, Scope* parent) : kind_(kind), parent_(parent) {}
Scope::~Scope() = default;

bool Scope::define(const Symbol& symbol) {
    auto it = symbols_.find(symbol.name);
    if (it != symbols_.end()) {
        // A user declaration shadows an injected builtin (which lives in the
        // outer namespace, not this scope) - replace the builtin entry.
        if (it->second.isBuiltin && !symbol.isBuiltin) {
            it->second = symbol;
            return true;
        }
        return false;
    }
    symbols_[symbol.name] = symbol;
    return true;
}

Symbol* Scope::lookup(const std::string& name) {
    auto it = symbols_.find(name);
    if (it != symbols_.end()) return &it->second;
    if (parent_) return parent_->lookup(name);
    return nullptr;
}

Symbol* Scope::lookupLocal(const std::string& name) {
    auto it = symbols_.find(name);
    return it != symbols_.end() ? &it->second : nullptr;
}

Scope* Scope::enclosingFunction() {
    for (Scope* s = this; s; s = s->parent_) {
        if (s->kind_ == Kind::Function) return s;
    }
    return nullptr;
}

Scope* Scope::enclosingClass() {
    for (Scope* s = this; s; s = s->parent_) {
        if (s->kind_ == Kind::Class) return s;
    }
    return nullptr;
}

namespace {
struct NameResolution {
    Symbol* sym = nullptr;          // the binding, or null if unresolved
    Scope* owner = nullptr;         // the scope that owns the binding
    bool crossedFunction = false;   // reaching it passed out of the current function
};

// Resolve `n` from `from` up the scope chain, recording whether the search
// crossed a function boundary before finding the binding. Used to enforce that
// an assignment may not rebind a variable owned by an ENCLOSING function
// without `nonlocal` - those markers are defined inside the current function,
// so they are found before the search crosses out.
NameResolution resolveAcross(Scope* from, const std::string& n) {
    NameResolution r;
    for (Scope* p = from; p; p = p->parent()) {
        if (Symbol* local = p->lookupLocal(n)) { r.sym = local; r.owner = p; break; }
        if (p->kind() == Scope::Kind::Function) r.crossedFunction = true;
    }
    return r;
}

// What a bare/compound assignment to `r`'s binding requires when reaching it
// crossed out of the current function. `nonlocal` for an enclosing function's
// var, `global` for a module-level one; `Ok` when no boundary was crossed (a
// local or same-function block) or the owner is a class scope.
enum class OuterRebind { Ok, NeedNonlocal, NeedGlobal };
OuterRebind classifyRebind(const NameResolution& r) {
    if (!r.sym || !r.crossedFunction) return OuterRebind::Ok;
    switch (r.owner->kind()) {
        case Scope::Kind::Function: return OuterRebind::NeedNonlocal;
        case Scope::Kind::Module:   return OuterRebind::NeedGlobal;
        default:                    return OuterRebind::Ok;
    }
}

// The diagnostic for a rejected cross-scope rebind. `nonlocal`/`global` are
// found inside the current function (markers defined there), so a declared
// opt-in resolves before the walk crosses out and never reaches here.
std::string rebindErrorMsg(OuterRebind kind, const std::string& n) {
    if (kind == OuterRebind::NeedNonlocal)
        return "'" + n + "' is owned by an enclosing function; add 'nonlocal " +
               n + "' to rebind it, or declare a new local with '" + n +
               ": <type> = ...'";
    return "'" + n + "' is a module global; add 'global " + n + "' to assign it "
           "inside a function, or declare a new local with '" + n +
           ": <type> = ...'";
}
} // namespace

//===----------------------------------------------------------------------===//
// Sema Implementation
//===----------------------------------------------------------------------===//

struct Sema::Impl {
    std::vector<SemaDiagnostic> diagnostics;
    std::vector<std::unique_ptr<Scope>> scopes;
    Scope* currentScope = nullptr;
    bool isInLoop = false;
    bool isInFunction = false;

    // D027: Closure capture tracking.
    // Stack of capture contexts - one per nested lambda being analyzed.
    struct CaptureContext {
        Scope* lambdaScope;  // the lambda's own Function scope
        std::set<std::string> capturedNames;
        // Subset of capturedNames declared `nonlocal` somewhere in this
        // function's body - they need heap-cell promotion at the binding
        // scope and cell-pointer storage in the env (instead of a value
        // copy) so writes propagate back to the owner.
        std::set<std::string> nonlocalDeclaredNames;
    };
    std::vector<CaptureContext> captureStack;
};

Sema::Sema() : impl_(std::make_unique<Impl>()) {
    // Create module scope and define builtins
    pushScope(Scope::Kind::Module);
    defineBuiltins();
}

Sema::~Sema() = default;

bool Sema::analyze(Module& module) {
    module.accept(*this);
    popScope();
    return !hasErrors();
}

const std::vector<SemaDiagnostic>& Sema::diagnostics() const {
    return impl_->diagnostics;
}

bool Sema::hasErrors() const {
    for (const auto& d : impl_->diagnostics) {
        if (d.level == SemaDiagnostic::Level::Error) return true;
    }
    return false;
}

//===----------------------------------------------------------------------===//
// Type Expression Visitors
//===----------------------------------------------------------------------===//

void Sema::visit(NamedTypeExpr&) {}
void Sema::visit(GenericTypeExpr& node) {
    if (node.base) node.base->accept(*this);
    for (auto& arg : node.typeArgs) arg->accept(*this);
}
void Sema::visit(OptionalTypeExpr& node) {
    if (node.inner) node.inner->accept(*this);
}
void Sema::visit(UnionTypeExpr& node) {
    for (auto& t : node.types) t->accept(*this);
}
void Sema::visit(CallableTypeExpr& node) {
    for (auto& p : node.paramTypes) p->accept(*this);
    if (node.returnType) node.returnType->accept(*this);
}
void Sema::visit(TupleTypeExpr& node) {
    for (auto& e : node.elementTypes) e->accept(*this);
}

//===----------------------------------------------------------------------===//
// Expression Visitors
//===----------------------------------------------------------------------===//

void Sema::visit(IntegerLiteral&) {}
void Sema::visit(FloatLiteral&) {}
void Sema::visit(StringLiteral& node) {
    // F-string interpolations are real expressions in the enclosing scope -
    // visit them so name resolution + capture analysis fire (e.g. base in
    // `f"{base}/x"` becomes a closure capture of the surrounding nested def).
    for (auto& part : node.fstringParts) {
        if (part.kind == FStringPart::Kind::Expression && part.expr) {
            part.expr->accept(*this);
        }
    }
}
void Sema::visit(TemplateExpr&) {}
void Sema::visit(TemplateFileExpr&) {}
void Sema::visit(BooleanLiteral&) {}
void Sema::visit(NoneLiteral&) {}

void Sema::visit(NameExpr& node) {
    Symbol* sym = currentScope()->lookup(node.name);
    if (!sym) {
        error(node.location(), "undefined name '" + node.name + "'");
        return;
    }

    // D027: Check if this name is captured from an enclosing scope.
    // For each lambda on the capture stack, if the name is NOT defined
    // within that lambda's scope (or below), but IS defined in an outer
    // non-module scope, it's a capture for that lambda.
    if (!impl_->captureStack.empty() &&
        sym->kind != Symbol::Kind::Function &&
        sym->kind != Symbol::Kind::Class &&
        sym->kind != Symbol::Kind::Module &&
        sym->kind != Symbol::Kind::TypeAlias) {
        for (auto& ctx : impl_->captureStack) {
            // Check if name is defined within the lambda's own scope tree
            bool definedInLambda = false;
            for (Scope* s = impl_->currentScope; s && s != ctx.lambdaScope->parent(); s = s->parent()) {
                if (s->lookupLocal(node.name)) {
                    definedInLambda = true;
                    break;
                }
            }
            if (!definedInLambda) {
                // Verify it's defined in a non-module scope above the lambda
                for (Scope* s = ctx.lambdaScope->parent(); s; s = s->parent()) {
                    if (s->lookupLocal(node.name)) {
                        if (s->kind() != Scope::Kind::Module) {
                            ctx.capturedNames.insert(node.name);
                        }
                        break;
                    }
                }
            }
        }
    }
}

void Sema::visit(BinaryExpr& node) {
    node.left->accept(*this);
    node.right->accept(*this);
}

void Sema::visit(ChainedCompExpr& node) {
    for (auto& op : node.operands) op->accept(*this);
}

void Sema::visit(WalrusExpr& node) {
    node.value->accept(*this);
    // Define the variable in current scope
    Symbol sym;
    sym.name = node.name;
    sym.kind = Symbol::Kind::Variable;
    sym.isInitialized = true;
    currentScope()->define(sym);
}

void Sema::visit(UnaryExpr& node) {
    node.operand->accept(*this);
}

void Sema::visit(CallExpr& node) {
    node.callee->accept(*this);
    for (auto& arg : node.args) arg->accept(*this);
    for (auto& [name, val] : node.kwArgs) val->accept(*this);
}

void Sema::visit(AttributeExpr& node) {
    node.object->accept(*this);
}

void Sema::visit(SubscriptExpr& node) {
    node.object->accept(*this);
    node.index->accept(*this);
}

void Sema::visit(SliceExpr& node) {
    if (node.lower) node.lower->accept(*this);
    if (node.upper) node.upper->accept(*this);
    if (node.step) node.step->accept(*this);
}

void Sema::visit(ListExpr& node) {
    for (auto& e : node.elements) e->accept(*this);
}

void Sema::visit(TupleExpr& node) {
    for (auto& e : node.elements) e->accept(*this);
}

void Sema::visit(DictExpr& node) {
    for (auto& [key, val] : node.entries) {
        if (key) key->accept(*this);  // null key = **spread entry
        if (val) val->accept(*this);
    }
}

void Sema::visit(SetExpr& node) {
    for (auto& e : node.elements) e->accept(*this);
}

void Sema::visit(ListCompExpr& node) {
    pushScope(Scope::Kind::Block);
    Symbol varSym;
    varSym.name = node.varName;
    varSym.kind = Symbol::Kind::Variable;
    varSym.isInitialized = true;
    currentScope()->define(varSym);
    node.iterable->accept(*this);
    if (node.condition) node.condition->accept(*this);
    for (auto& clause : node.extraClauses) {
        for (auto& name : clause.varNames) {
            Symbol s;
            s.name = name;
            s.kind = Symbol::Kind::Variable;
            s.isInitialized = true;
            currentScope()->define(s);
        }
        clause.iterable->accept(*this);
        if (clause.condition) clause.condition->accept(*this);
    }
    node.element->accept(*this);
    popScope();
}

void Sema::visit(DictCompExpr& node) {
    pushScope(Scope::Kind::Block);
    for (auto& name : node.varNames) {
        Symbol varSym;
        varSym.name = name;
        varSym.kind = Symbol::Kind::Variable;
        varSym.isInitialized = true;
        currentScope()->define(varSym);
    }
    node.iterable->accept(*this);
    if (node.condition) node.condition->accept(*this);
    for (auto& clause : node.extraClauses) {
        for (auto& name : clause.varNames) {
            Symbol s;
            s.name = name;
            s.kind = Symbol::Kind::Variable;
            s.isInitialized = true;
            currentScope()->define(s);
        }
        clause.iterable->accept(*this);
        if (clause.condition) clause.condition->accept(*this);
    }
    node.key->accept(*this);
    node.value->accept(*this);
    popScope();
}

void Sema::visit(SetCompExpr& node) {
    pushScope(Scope::Kind::Block);
    Symbol varSym;
    varSym.name = node.varName;
    varSym.kind = Symbol::Kind::Variable;
    varSym.isInitialized = true;
    currentScope()->define(varSym);
    node.iterable->accept(*this);
    node.element->accept(*this);
    if (node.condition) node.condition->accept(*this);
    for (auto& clause : node.extraClauses) {
        for (auto& name : clause.varNames) {
            Symbol s;
            s.name = name;
            s.kind = Symbol::Kind::Variable;
            s.isInitialized = true;
            currentScope()->define(s);
        }
        clause.iterable->accept(*this);
        if (clause.condition) clause.condition->accept(*this);
    }
    popScope();
}

void Sema::visit(GeneratorExpr& node) {
    pushScope(Scope::Kind::Block);
    Symbol varSym;
    varSym.name = node.varName;
    varSym.kind = Symbol::Kind::Variable;
    varSym.isInitialized = true;
    currentScope()->define(varSym);
    node.iterable->accept(*this);
    node.element->accept(*this);
    if (node.condition) node.condition->accept(*this);
    for (auto& clause : node.extraClauses) {
        for (auto& name : clause.varNames) {
            Symbol s;
            s.name = name;
            s.kind = Symbol::Kind::Variable;
            s.isInitialized = true;
            currentScope()->define(s);
        }
        clause.iterable->accept(*this);
        if (clause.condition) clause.condition->accept(*this);
    }
    popScope();
}

void Sema::visit(LambdaExpr& node) {
    pushScope(Scope::Kind::Function);
    for (auto& p : node.params) {
        Symbol paramSym;
        paramSym.name = p.name;
        paramSym.kind = Symbol::Kind::Parameter;
        paramSym.isInitialized = true;
        currentScope()->define(paramSym);
        if (p.type) p.type->accept(*this);
        if (p.defaultValue) p.defaultValue->accept(*this);
    }
    if (node.returnType) node.returnType->accept(*this);

    // D027: Push capture context before visiting body
    Impl::CaptureContext capCtx;
    capCtx.lambdaScope = impl_->currentScope;
    impl_->captureStack.push_back(std::move(capCtx));

    bool prevInFunction = impl_->isInFunction;
    impl_->isInFunction = true;
    if (node.body) node.body->accept(*this);
    for (auto& s : node.bodyStmts) s->accept(*this);
    impl_->isInFunction = prevInFunction;

    // D027: Store captured variables and pop context
    auto& ctx = impl_->captureStack.back();
    node.capturedVars.assign(ctx.capturedNames.begin(), ctx.capturedNames.end());
    node.mutatedCapturedVars.assign(
        ctx.nonlocalDeclaredNames.begin(), ctx.nonlocalDeclaredNames.end());
    impl_->captureStack.pop_back();

    popScope();
}

void Sema::visit(IfExpr& node) {
    node.condition->accept(*this);
    node.thenExpr->accept(*this);
    node.elseExpr->accept(*this);
}

void Sema::visit(AwaitExpr& node) {
    node.operand->accept(*this);
}

void Sema::visit(FireExpr& node) {
    if (node.operand) {
        node.operand->accept(*this);
        return;
    }
    // Block form `fire { ... }` runs on a vthread, so it captures enclosing
    // locals like a lambda - push a Function scope + capture context so codegen
    // knows exactly which locals to marshal as spawn args (without this the body
    // referenced the parent frame's allocas directly -> invalid cross-function
    // IR). nonlocal-mutated captures are rejected in codegen (the cells aren't
    // thread-safe yet - that would be a data race).
    pushScope(Scope::Kind::Function);
    Impl::CaptureContext capCtx;
    capCtx.lambdaScope = impl_->currentScope;
    impl_->captureStack.push_back(std::move(capCtx));
    bool prevInFunction = impl_->isInFunction;
    impl_->isInFunction = true;
    for (auto& s : node.bodyStmts) s->accept(*this);
    impl_->isInFunction = prevInFunction;
    auto& ctx = impl_->captureStack.back();
    node.capturedVars.assign(ctx.capturedNames.begin(), ctx.capturedNames.end());
    node.mutatedCapturedVars.assign(
        ctx.nonlocalDeclaredNames.begin(), ctx.nonlocalDeclaredNames.end());
    impl_->captureStack.pop_back();
    popScope();
}

void Sema::visit(YieldExpr& node) {
    if (!impl_->isInFunction) {
        error(node.location(), "'yield' outside function");
    }
    if (node.value) node.value->accept(*this);
}

void Sema::visit(StarredExpr& node) {
    node.value->accept(*this);
}

//===----------------------------------------------------------------------===//
// Statement Visitors
//===----------------------------------------------------------------------===//

void Sema::visit(ExprStmt& node) {
    node.expr->accept(*this);
}

void Sema::visit(AssignStmt& node) {
    // Visit the value first
    node.value->accept(*this);
    if (node.typeAnnotation) node.typeAnnotation->accept(*this);

    // Define or update targets. Single NameExpr is the common case; a single
    // TupleExpr target is tuple-unpacking (`a, b = pair`); multiple targets
    // is chained assignment (`a = b = c`).
    // A name is declared exactly once per scope via `x: T = ...` (AnnAssignStmt)
    // or an implicit binding form (tuple-unpack, for/with/except target, walrus).
    // A bare `x = v` is a *reassignment*: the name must already resolve in scope;
    // it never introduces a new (possibly shadowing) binding. `allowImplicitDecl`
    // is set for forms with no annotation slot (tuple-unpack), which may declare.
    auto defineLocal = [&](const std::string& n, SourceLocation loc, bool allowImplicitDecl) {
        // A bare `=` may freely reassign a binding owned by THIS function - its
        // params, locals, or any enclosing block of the same function. But a
        // binding owned by an ENCLOSING FUNCTION needs `nonlocal`, and a MODULE
        // global needs `global`. Without the marker we reject it - otherwise
        // codegen would silently spin up a throwaway local (Python's forgot-the-
        // keyword footgun) and the outer variable would never change. Reads are
        // unaffected (this path only runs for assignment targets), and the rule
        // is identical in `.dr` and `.py` - Sema runs once for both.
        NameResolution r = resolveAcross(currentScope(), n);
        if (r.sym) {
            if (r.sym->isConst) {
                error(loc, "cannot reassign const variable '" + n + "'");
                return;
            }
            OuterRebind ob = classifyRebind(r);
            if (!allowImplicitDecl && ob != OuterRebind::Ok) {
                error(loc, rebindErrorMsg(ob, n));
                return;
            }
            r.sym->isInitialized = true;  // reassignment of a visible binding
            return;
        }
        if (!allowImplicitDecl) {
            error(loc, "'" + n + "' is not declared; introduce it with '" + n +
                       ": <type> = ...' (bare '=' only reassigns an existing variable)");
            return;
        }
        Symbol sym;
        sym.name = n;
        sym.kind = Symbol::Kind::Variable;
        sym.declaration = loc;
        sym.isInitialized = true;
        currentScope()->define(sym);
    };
    // A bare single-name assignment is strict unless the statement itself carries
    // an annotation (the vestigial AssignStmt::typeAnnotation declaration form).
    const bool singleNameMayDeclare = (node.typeAnnotation != nullptr);
    for (auto& target : node.targets) {
        if (auto* name = dynamic_cast<NameExpr*>(target.get())) {
            defineLocal(name->name, name->location(), singleNameMayDeclare);
        } else if (auto* tup = dynamic_cast<TupleExpr*>(target.get())) {
            // Tuple unpacking has no per-element annotation slot, so each name
            // may implicitly declare (with an inferred type). The const form
            // (`const a: T, b: U = expr`) is a pure declaration: every name is
            // fresh (redeclaration is an error, same rule as a single const)
            // and binds const.
            std::function<void(Expr*)> visitTarget = [&](Expr* t) {
                if (auto* n = dynamic_cast<NameExpr*>(t)) {
                    if (node.isConst) {
                        Symbol* prior = currentScope()->lookupLocal(n->name);
                        if (prior && !prior->isBuiltin) {
                            const char* what = prior->kind == Symbol::Kind::Parameter
                                                   ? "parameter" : "variable";
                            error(n->location(), std::string("redeclaration of ") + what +
                                  " '" + n->name + "'; it is already declared in this scope");
                            return;
                        }
                        Symbol sym;
                        sym.name = n->name;
                        sym.kind = Symbol::Kind::Variable;
                        sym.declaration = n->location();
                        sym.isInitialized = true;
                        sym.isConst = true;
                        currentScope()->define(sym);
                        return;
                    }
                    defineLocal(n->name, n->location(), /*allowImplicitDecl=*/true);
                } else if (auto* nestedTup = dynamic_cast<TupleExpr*>(t)) {
                    for (auto& e : nestedTup->elements) visitTarget(e.get());
                } else if (auto* starred = dynamic_cast<StarredExpr*>(t)) {
                    // `*rest` / `*init` in a tuple-unpack target declares the
                    // inner name (bound to a list of the remaining elements).
                    // Reuse the NameExpr path (incl. const handling); without
                    // this the starred name was never declared, so a valid
                    // `first, *rest = [...]` failed with "undefined name 'rest'".
                    visitTarget(starred->value.get());
                } else {
                    if (!isValidAssignmentTarget(t)) {
                        error(t->location(), "invalid assignment target");
                    }
                    t->accept(*this);
                }
            };
            for (auto& e : tup->elements) visitTarget(e.get());
        } else {
            if (!isValidAssignmentTarget(target.get())) {
                error(target->location(), "invalid assignment target");
            }
            target->accept(*this);
        }
    }
}

void Sema::visit(AugAssignStmt& node) {
    node.target->accept(*this);
    node.value->accept(*this);
    // `n += 1` is a read-modify-rebind of `n`, so it obeys the same rule as a
    // bare `=`: it needs `nonlocal` to rebind an enclosing function's variable
    // and `global` to rebind a module global (else codegen silently mutates a
    // throwaway local).
    if (auto* name = dynamic_cast<NameExpr*>(node.target.get())) {
        NameResolution r = resolveAcross(currentScope(), name->name);
        if (r.sym && r.sym->isConst) {
            error(node.target->location(), "cannot reassign const variable '" + name->name + "'");
        } else if (OuterRebind ob = classifyRebind(r); ob != OuterRebind::Ok) {
            error(node.target->location(), rebindErrorMsg(ob, name->name));
        }
    }
}

void Sema::visit(AnnAssignStmt& node) {
    if (node.annotation) node.annotation->accept(*this);
    if (node.value) node.value->accept(*this);

    // Define the variable. A name may be declared only once per scope: a second
    // `x: T = ...` for a name already bound in THIS scope (a prior declaration or
    // a parameter) is a redeclaration - reassign with bare `x = ...` instead.
    if (auto* name = dynamic_cast<NameExpr*>(node.target.get())) {
        Symbol* prior = currentScope()->lookupLocal(name->name);
        // Shadowing an injected builtin (outer namespace) is a fresh declaration,
        // not a redeclaration - only a genuine same-scope user binding is an error.
        // A module-level const we hoisted in the pre-pass is THIS statement's own
        // forward declaration; clear the marker and treat this visit as the real
        // declaration rather than a redeclaration of itself.
        if (prior && prior->isModuleForwardDecl) {
            prior->isModuleForwardDecl = false;
        } else if (prior && !prior->isBuiltin) {
            const char* what = prior->kind == Symbol::Kind::Parameter
                                   ? "parameter" : "variable";
            error(name->location(), std::string("redeclaration of ") + what + " '" +
                  name->name + "'; it is already declared in this scope (reassign with '" +
                  name->name + " = ...')");
            return;
        }
        Symbol sym;
        sym.name = name->name;
        sym.kind = Symbol::Kind::Variable;
        sym.declaration = name->location();
        sym.isInitialized = (node.value != nullptr);
        sym.isConst = node.isConst;
        sym.isStatic = node.isStatic;
        currentScope()->define(sym);
    }
}

void Sema::visit(IfStmt& node) {
    // Each block body is its own lexical scope (D: block scoping). The
    // condition is evaluated in the enclosing scope, so a walrus there
    // (`if (x := f()) > 0`) binds outside the branch, matching Python.
    node.condition->accept(*this);
    pushScope(Scope::Kind::Block);
    for (auto& s : node.thenBody) s->accept(*this);
    popScope();
    for (auto& [cond, body] : node.elifClauses) {
        cond->accept(*this);
        pushScope(Scope::Kind::Block);
        for (auto& s : body) s->accept(*this);
        popScope();
    }
    pushScope(Scope::Kind::Block);
    for (auto& s : node.elseBody) s->accept(*this);
    popScope();
}

void Sema::visit(WhileStmt& node) {
    node.condition->accept(*this);
    bool prevInLoop = impl_->isInLoop;
    impl_->isInLoop = true;
    pushScope(Scope::Kind::Block);
    for (auto& s : node.body) s->accept(*this);
    popScope();
    impl_->isInLoop = prevInLoop;
    pushScope(Scope::Kind::Block);
    for (auto& s : node.elseBody) s->accept(*this);
    popScope();
}

void Sema::visit(ForStmt& node) {
    node.iterable->accept(*this);

    // The loop target and body live in the loop's block scope, so the target
    // is not visible after the loop. Single-name form (`for x in xs`) and
    // tuple-unpacking form (`for k, v in d.items()`) both bind here.
    pushScope(Scope::Kind::Block);
    auto defineTargetName = [&](const std::string& n, SourceLocation loc) {
        Symbol sym;
        sym.name = n;
        sym.kind = Symbol::Kind::Variable;
        sym.declaration = loc;
        sym.isInitialized = true;
        currentScope()->define(sym);
    };
    if (auto* name = dynamic_cast<NameExpr*>(node.target.get())) {
        defineTargetName(name->name, name->location());
    } else if (auto* tup = dynamic_cast<TupleExpr*>(node.target.get())) {
        for (auto& elem : tup->elements) {
            if (auto* en = dynamic_cast<NameExpr*>(elem.get()))
                defineTargetName(en->name, en->location());
        }
    }

    bool prevInLoop = impl_->isInLoop;
    impl_->isInLoop = true;
    for (auto& s : node.body) s->accept(*this);
    impl_->isInLoop = prevInLoop;
    popScope();
    pushScope(Scope::Kind::Block);
    for (auto& s : node.elseBody) s->accept(*this);
    popScope();
}

void Sema::visit(TryStmt& node) {
    pushScope(Scope::Kind::Block);
    for (auto& s : node.tryBody) s->accept(*this);
    popScope();
    for (auto& handler : node.handlers) {
        pushScope(Scope::Kind::Block);
        if (handler.type) handler.type->accept(*this);
        if (!handler.name.empty()) {
            Symbol sym;
            sym.name = handler.name;
            sym.kind = Symbol::Kind::Variable;
            sym.isInitialized = true;
            currentScope()->define(sym);
        }
        for (auto& s : handler.body) s->accept(*this);
        popScope();
    }
    pushScope(Scope::Kind::Block);
    for (auto& s : node.elseBody) s->accept(*this);
    popScope();
    pushScope(Scope::Kind::Block);
    for (auto& s : node.finallyBody) s->accept(*this);
    popScope();
}

void Sema::visit(WithStmt& node) {
    // The context-manager `as` bindings and the body share the with-block scope.
    pushScope(Scope::Kind::Block);
    for (auto& item : node.items) {
        item.contextExpr->accept(*this);
        if (item.optionalVars) {
            if (auto* name = dynamic_cast<NameExpr*>(item.optionalVars.get())) {
                Symbol sym;
                sym.name = name->name;
                sym.kind = Symbol::Kind::Variable;
                sym.isInitialized = true;
                currentScope()->define(sym);
            }
        }
    }
    for (auto& s : node.body) s->accept(*this);
    popScope();
}

void Sema::visit(ThreadStmt& node) {
    // thread { ... } runs the body on a scoped OS thread, capturing enclosing
    // locals (read-only). Mirror the fire-block capture analysis so codegen
    // marshals the captures as thread args (else the body references the parent
    // frame's allocas -> invalid cross-function IR). The thread auto-joins at
    // scope exit, so captures are alive for its whole lifetime; nonlocal-mutated
    // captures are still rejected in codegen (cross-thread write = data race).
    pushScope(Scope::Kind::Function);
    Impl::CaptureContext capCtx;
    capCtx.lambdaScope = impl_->currentScope;
    impl_->captureStack.push_back(std::move(capCtx));
    bool prevInFunction = impl_->isInFunction;
    impl_->isInFunction = true;
    for (auto& s : node.body) s->accept(*this);
    impl_->isInFunction = prevInFunction;
    auto& ctx = impl_->captureStack.back();
    node.capturedVars.assign(ctx.capturedNames.begin(), ctx.capturedNames.end());
    node.mutatedCapturedVars.assign(
        ctx.nonlocalDeclaredNames.begin(), ctx.nonlocalDeclaredNames.end());
    impl_->captureStack.pop_back();
    popScope();
}

void Sema::visit(MatchStmt& node) {
    node.subject->accept(*this);
    // Recursively traverse pattern tree to define captures and visit literals
    std::function<void(MatchPattern&)> defineCaptures = [&](MatchPattern& pat) {
        if (pat.kind == MatchPattern::Kind::Capture) {
            Symbol sym;
            sym.name = pat.name;
            sym.kind = Symbol::Kind::Variable;
            sym.isInitialized = true;
            currentScope()->define(sym);
        }
        if (pat.literal) pat.literal->accept(*this);
        if (pat.guard) pat.guard->accept(*this);
        for (auto& sub : pat.subPatterns) defineCaptures(sub);
    };
    for (auto& c : node.cases) {
        pushScope(Scope::Kind::Block);
        defineCaptures(c.pattern);
        if (c.guard) c.guard->accept(*this);
        for (auto& s : c.body) s->accept(*this);
        popScope();
    }
}

void Sema::visit(ReturnStmt& node) {
    if (!impl_->isInFunction) {
        error(node.location(), "'return' outside function");
    }
    if (node.value) node.value->accept(*this);
}

void Sema::visit(DeferStmt& node) {
    // No scope ends before process exit at module level, so a module-level
    // defer could never run; an explicit shutdown-hook mechanism, if ever
    // wanted, is its own ADR.
    if (!impl_->isInFunction) {
        error(node.location(), "'defer' outside function");
    }
    if (node.call) node.call->accept(*this);
}

void Sema::visit(RaiseStmt& node) {
    if (node.exception) node.exception->accept(*this);
    if (node.cause) node.cause->accept(*this);
}

void Sema::visit(BreakStmt& node) {
    if (!impl_->isInLoop) {
        error(node.location(), "'break' outside loop");
    }
}

void Sema::visit(ContinueStmt& node) {
    if (!impl_->isInLoop) {
        error(node.location(), "'continue' outside loop");
    }
}

void Sema::visit(PassStmt&) {}

void Sema::visit(AssertStmt& node) {
    node.test->accept(*this);
    if (node.msg) node.msg->accept(*this);
}

void Sema::visit(GlobalStmt& node) {
    for (auto& name : node.names) {
        Symbol sym;
        sym.name = name;
        sym.kind = Symbol::Kind::Variable;
        sym.isGlobal = true;
        sym.isInitialized = true;
        currentScope()->define(sym);
    }
}

void Sema::visit(NonlocalStmt& node) {
    for (auto& name : node.names) {
        // Find the TRUE binding scope (not a nested nonlocal marker). We
        // walk up looking for a non-nonlocal Variable/Parameter binding
        // inside an enclosing function scope. Module-scope bindings dont
        // qualify (Python: nonlocal can't reference globals - use `global`).
        Scope* enclosing = currentScope()->parent();
        Scope* bindingScope = nullptr;
        while (enclosing) {
            Symbol* sym = enclosing->lookupLocal(name);
            if (sym && !sym->isNonlocal) {
                if (enclosing->kind() != Scope::Kind::Module) {
                    bindingScope = enclosing;
                }
                break;
            }
            if (enclosing->kind() == Scope::Kind::Module) break;
            enclosing = enclosing->parent();
        }
        if (!bindingScope) {
            error(node.location(), "no binding for nonlocal '" + name + "' found");
        } else {
            // Mark every CaptureContext on the stack whose owning function
            // sits between this `nonlocal` declaration and the binding scope
            // - each such context must capture the cell pointer (not the
            // value) and forward it to nested closures unchanged.
            //
            // ctx.lambdaScope's parent chain passing through bindingScope
            // means the binding is reachable from ctx via scope chain, so
            // ctx must capture it.
            for (auto& ctx : impl_->captureStack) {
                bool reachableViaScopeChain = false;
                for (Scope* p = ctx.lambdaScope->parent(); p; p = p->parent()) {
                    if (p == bindingScope) { reachableViaScopeChain = true; break; }
                    if (p->kind() == Scope::Kind::Module) break;
                }
                if (reachableViaScopeChain) {
                    ctx.capturedNames.insert(name);
                    ctx.nonlocalDeclaredNames.insert(name);
                }
            }
        }
        // Define the nonlocal marker in the current scope so subsequent
        // AssignStmt's defineLocal path finds it via lookupLocal and skips
        // the shadow-define branch (writes route to the existing binding,
        // not a fresh local). Also so NameExpr capture analysis treats
        // reads as already-captured (the ctx.capturedNames insert above
        // covered them up-front).
        Symbol sym;
        sym.name = name;
        sym.kind = Symbol::Kind::Variable;
        sym.isNonlocal = true;
        sym.isInitialized = true;
        currentScope()->define(sym);
    }
}

void Sema::visit(DeleteStmt& node) {
    for (auto& target : node.targets) {
        if (!isValidAssignmentTarget(target.get())) {
            error(target->location(), "cannot delete this expression");
        }
        target->accept(*this);
    }
}

void Sema::visit(ImportStmt& node) {
    for (auto& alias : node.names) {
        std::string defName = alias.asName.empty() ? alias.name : alias.asName;
        auto dotPos = defName.find('.');
        if (dotPos != std::string::npos) {
            defName = defName.substr(0, dotPos);
        }
        Symbol sym;
        sym.name = defName;
        sym.kind = Symbol::Kind::Module;
        sym.isInitialized = true;
        currentScope()->define(sym);
    }
}

void Sema::visit(FromImportStmt& node) {
    for (auto& alias : node.names) {
        std::string defName = alias.asName.empty() ? alias.name : alias.asName;
        Symbol sym;
        sym.name = defName;
        sym.kind = Symbol::Kind::Variable;
        sym.isInitialized = true;
        currentScope()->define(sym);
    }
}

//===----------------------------------------------------------------------===//
// Declaration Visitors
//===----------------------------------------------------------------------===//

void Sema::visit(FunctionDecl& node) {
    // Define function in current scope
    Symbol funcSym;
    funcSym.name = node.name;
    funcSym.kind = Symbol::Kind::Function;
    funcSym.declaration = node.location();
    funcSym.isInitialized = true;
    currentScope()->define(funcSym);

    // Visit decorators in outer scope
    for (auto& dec : node.decorators) dec->accept(*this);

    // Enter function scope
    pushScope(Scope::Kind::Function);

    // D044 - bind type parameters (`def f[T]()`) as type symbols so the body
    // can name them in value position (e.g. an explicit construction `Inner[T]`
    // or call `g[T](...)`) without a spurious "undefined name 'T'".
    for (auto& tp : node.typeParams) {
        Symbol tpSym;
        tpSym.name = tp.name;
        tpSym.kind = Symbol::Kind::Class;  // a type name
        tpSym.declaration = node.location();
        tpSym.isInitialized = true;
        currentScope()->define(tpSym);
    }

    bool prevInFunction = impl_->isInFunction;

    // Nested-def capture tracking: when this `def` is lexically inside another
    // function body, references to enclosing locals must become closure
    // captures (Python semantics). Reuses the same machinery LambdaExpr uses.
    bool isNested = prevInFunction && !node.isMethod;
    if (isNested) {
        Impl::CaptureContext capCtx;
        capCtx.lambdaScope = impl_->currentScope;
        impl_->captureStack.push_back(std::move(capCtx));
    }

    impl_->isInFunction = true;

    // For implicit-self methods, define 'self' in scope
    if (node.isMethod && node.hasImplicitSelf) {
        Symbol selfSym;
        selfSym.name = "self";
        selfSym.kind = Symbol::Kind::Parameter;
        selfSym.declaration = node.location();
        selfSym.isInitialized = true;
        currentScope()->define(selfSym);
    }

    // Define parameters
    for (auto& p : node.params) {
        Symbol paramSym;
        paramSym.name = p.name;
        paramSym.kind = Symbol::Kind::Parameter;
        paramSym.declaration = node.location();
        paramSym.isInitialized = true;
        currentScope()->define(paramSym);
        if (p.type) p.type->accept(*this);
        if (p.defaultValue) p.defaultValue->accept(*this);
    }
    if (node.returnType) node.returnType->accept(*this);

    // Visit body
    for (auto& s : node.body) s->accept(*this);

    if (isNested) {
        auto& ctx = impl_->captureStack.back();
        node.capturedVars.assign(ctx.capturedNames.begin(), ctx.capturedNames.end());
        node.mutatedCapturedVars.assign(
            ctx.nonlocalDeclaredNames.begin(), ctx.nonlocalDeclaredNames.end());
        impl_->captureStack.pop_back();
    }

    impl_->isInFunction = prevInFunction;
    popScope();
}

void Sema::visit(ClassDecl& node) {
    // Define class in current scope
    Symbol classSym;
    classSym.name = node.name;
    classSym.kind = Symbol::Kind::Class;
    classSym.declaration = node.location();
    classSym.isInitialized = true;
    currentScope()->define(classSym);

    // Visit decorators and bases in outer scope
    for (auto& dec : node.decorators) dec->accept(*this);
    for (auto& base : node.bases) base->accept(*this);
    for (auto& [name, val] : node.keywords) val->accept(*this);

    // Enter class scope
    pushScope(Scope::Kind::Class);
    // D044 - bind type parameters (`class Foo[T]`) as type symbols, visible to
    // every method body (e.g. an in-body construction `Inner[T](...)`).
    for (auto& tp : node.typeParams) {
        Symbol tpSym;
        tpSym.name = tp.name;
        tpSym.kind = Symbol::Kind::Class;  // a type name
        tpSym.declaration = node.location();
        tpSym.isInitialized = true;
        currentScope()->define(tpSym);
    }
    for (auto& s : node.body) s->accept(*this);
    popScope();
}

void Sema::visit(TypeAliasStmt& node) {
    // Define type alias name in current scope
    Symbol sym;
    sym.name = node.name;
    sym.kind = Symbol::Kind::Variable;
    sym.declaration = node.location();
    sym.isInitialized = true;
    currentScope()->define(sym);
    if (node.value) node.value->accept(*this);
}

void Sema::visit(Module& node) {
    // Python-parity: top-level `def`s and `class`es resolve by name at
    // call time, not by source order. Pre-register all top-level function
    // and class symbols so mutual recursion (and forward references in
    // general) work like Python's:
    //
    //  def foo(): bar() # OK - bar is looked up when foo runs
    //  def bar(): foo()
    //
    // Without this pre-pass, foo's body would see `bar` as undefined
    // because Sema visits statements in source order.
    for (auto& s : node.body) {
        if (auto* fn = dynamic_cast<FunctionDecl*>(s.get())) {
            Symbol funcSym;
            funcSym.name = fn->name;
            funcSym.kind = Symbol::Kind::Function;
            funcSym.declaration = fn->location();
            funcSym.isInitialized = true;
            currentScope()->define(funcSym);
        } else if (auto* cls = dynamic_cast<ClassDecl*>(s.get())) {
            Symbol clsSym;
            clsSym.name = cls->name;
            clsSym.kind = Symbol::Kind::Class;
            clsSym.declaration = cls->location();
            clsSym.isInitialized = true;
            currentScope()->define(clsSym);
        } else if (auto* ann = dynamic_cast<AnnAssignStmt*>(s.get())) {
            // Hoist module-level typed globals (const / static / plain) so a
            // forward reference to a const defined later in the file resolves,
            // matching codegen's order-independent global resolution and
            // Python module semantics. Without this, `if n > _MAX` used above
            // its `const _MAX: int = ...` definition is flagged undefined -
            // a false positive that breaks the stdlib build now that
            // dependency Sema errors surface. Marked so the
            // second-pass visit recognizes its own hoist and does not report a
            // redeclaration.
            if (auto* name = dynamic_cast<NameExpr*>(ann->target.get())) {
                Symbol sym;
                sym.name = name->name;
                sym.kind = Symbol::Kind::Variable;
                sym.declaration = name->location();
                sym.isInitialized = (ann->value != nullptr);
                sym.isConst = ann->isConst;
                sym.isStatic = ann->isStatic;
                sym.isModuleForwardDecl = true;
                currentScope()->define(sym);
            }
        }
    }
    // Second pass: visit each statement normally. Function/Class visitors
    // attempt to define again - the duplicate-define is a no-op (returns
    // false from Scope::define but otherwise harmless; the visitor's body
    // analysis still runs).
    for (auto& s : node.body) s->accept(*this);
}

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

void Sema::pushScope(Scope::Kind kind) {
    auto scope = std::make_unique<Scope>(kind, impl_->currentScope);
    impl_->currentScope = scope.get();
    impl_->scopes.push_back(std::move(scope));
}

void Sema::popScope() {
    impl_->currentScope = impl_->currentScope->parent();
}

Scope* Sema::currentScope() {
    return impl_->currentScope;
}

void Sema::defineBuiltins() {
    auto defineBuiltin = [this](const std::string& name, Symbol::Kind kind = Symbol::Kind::Function) {
        Symbol sym;
        sym.name = name;
        sym.kind = kind;
        sym.isInitialized = true;
        sym.isBuiltin = true;
        currentScope()->define(sym);
    };

    // Built-in functions
    defineBuiltin("print");
    defineBuiltin("len");
    defineBuiltin("range");
    defineBuiltin("input");
    defineBuiltin("int", Symbol::Kind::Class);
    defineBuiltin("float", Symbol::Kind::Class);
    defineBuiltin("str", Symbol::Kind::Class);
    defineBuiltin("bool", Symbol::Kind::Class);
    defineBuiltin("bytes", Symbol::Kind::Class);
    defineBuiltin("list", Symbol::Kind::Class);
    defineBuiltin("dict", Symbol::Kind::Class);
    defineBuiltin("set", Symbol::Kind::Class);
    defineBuiltin("tuple", Symbol::Kind::Class);
    // `deque` is a global builtin container (like list/set/dict - no import
    // required); codegen routes its ctor/methods through the __Deque path.
    // `from collections import deque` (Python parity) also resolves it.
    defineBuiltin("deque", Symbol::Kind::Class);
    defineBuiltin("type", Symbol::Kind::Class);
    defineBuiltin("object", Symbol::Kind::Class);
    // D044 - `Any` is a type name; allow it in value position as an explicit
    // generic type argument (`Box[Any]`). The TypeChecker resolves it to AnyType.
    defineBuiltin("Any", Symbol::Kind::Class);
    // NOTE: `Lock` is NOT a global builtin - it is import-gated, matching Python
    // (`from threading import Lock`). It enters scope via FromImportStmt, which
    // defines imported names unconditionally; bare `Lock()` is undefined here.
    defineBuiltin("abs");
    defineBuiltin("min");
    defineBuiltin("max");
    defineBuiltin("sum");
    // Numeric builtins whose CodeGen handlers live in CallBuiltins.cpp
    // (round/pow/divmod) and TypeChecker recognizes them - register the
    // names so Sema doesn't reject the call before CodeGen runs.
    defineBuiltin("pow");
    defineBuiltin("round");
    defineBuiltin("divmod");
    defineBuiltin("sorted");
    defineBuiltin("reversed");
    defineBuiltin("enumerate");
    defineBuiltin("zip");
    defineBuiltin("map");
    defineBuiltin("filter");
    defineBuiltin("any");
    defineBuiltin("all");
    defineBuiltin("isinstance");
    defineBuiltin("issubclass");
    defineBuiltin("hasattr");
    defineBuiltin("getattr");
    // setattr/delattr deliberately absent: dynamic attribute injection is a
    // non-goal (D021). Declared attributes only; runtime field creation by
    // string name is barred by commandment #3.
    defineBuiltin("dir");
    // Internal intrinsic: does the currently-handled exception match the
    // given exception-type code? Backs unittest.assertRaises. Underscore
    // prefix signals "internal" - not part of the user-facing surface.
    defineBuiltin("__exc_matches");
    defineBuiltin("id");
    defineBuiltin("hash");
    defineBuiltin("repr");
    defineBuiltin("chr");
    defineBuiltin("ord");
    // float<->bits reinterpret intrinsics (the user-reachable bitcast that
    // struct.pack/unpack + binary wire codecs need; see struct.dr).
    defineBuiltin("__float_bits");
    defineBuiltin("__float_from_bits");
    defineBuiltin("__float32_bits");
    defineBuiltin("__float32_from_bits");
    defineBuiltin("hex");
    defineBuiltin("oct");
    defineBuiltin("bin");
    defineBuiltin("super");
    defineBuiltin("property", Symbol::Kind::Class);
    defineBuiltin("staticmethod", Symbol::Kind::Class);
    defineBuiltin("classmethod", Symbol::Kind::Class);
    // 6.18: @dataclass / NamedTuple are compile-time recognizers in CodeGen.
    // Register the names so decorator/base resolution doesn't flag them.
    defineBuiltin("dataclass", Symbol::Kind::Class);
    defineBuiltin("NamedTuple", Symbol::Kind::Class);
    // TypedDict is a compile-time recognizer: TypeChecker tracks it via
    // isTypedDict and CodeGen lowers it (currently dict-backed). Register the
    // name here so base-class resolution doesn't flag `class C(TypedDict)` as
    // an undefined name (mirrors NamedTuple/dataclass above).
    defineBuiltin("TypedDict", Symbol::Kind::Class);
    // Built-in exceptions - kept in sync with CodeGenImpl::excTypeCode and
    // CodeGenImpl::isBuiltinExcName. Codes are wired in the codegen layer; here
    // we just register the names so Sema doesn't flag them as undefined.
    defineBuiltin("BaseException", Symbol::Kind::Class);
    defineBuiltin("SystemExit", Symbol::Kind::Class);
    defineBuiltin("KeyboardInterrupt", Symbol::Kind::Class);
    defineBuiltin("GeneratorExit", Symbol::Kind::Class);
    defineBuiltin("Exception", Symbol::Kind::Class);
    defineBuiltin("StopIteration", Symbol::Kind::Class);
    defineBuiltin("StopAsyncIteration", Symbol::Kind::Class);
    defineBuiltin("ArithmeticError", Symbol::Kind::Class);
    defineBuiltin("FloatingPointError", Symbol::Kind::Class);
    defineBuiltin("OverflowError", Symbol::Kind::Class);
    defineBuiltin("ZeroDivisionError", Symbol::Kind::Class);
    defineBuiltin("AssertionError", Symbol::Kind::Class);
    defineBuiltin("AttributeError", Symbol::Kind::Class);
    defineBuiltin("BufferError", Symbol::Kind::Class);
    defineBuiltin("EOFError", Symbol::Kind::Class);
    defineBuiltin("ImportError", Symbol::Kind::Class);
    defineBuiltin("ModuleNotFoundError", Symbol::Kind::Class);
    defineBuiltin("LookupError", Symbol::Kind::Class);
    defineBuiltin("IndexError", Symbol::Kind::Class);
    defineBuiltin("KeyError", Symbol::Kind::Class);
    defineBuiltin("MemoryError", Symbol::Kind::Class);
    defineBuiltin("NameError", Symbol::Kind::Class);
    defineBuiltin("UnboundLocalError", Symbol::Kind::Class);
    defineBuiltin("OSError", Symbol::Kind::Class);
    defineBuiltin("IOError", Symbol::Kind::Class);  // Py3 alias of OSError
    defineBuiltin("FileNotFoundError", Symbol::Kind::Class);
    defineBuiltin("FileExistsError", Symbol::Kind::Class);
    defineBuiltin("IsADirectoryError", Symbol::Kind::Class);
    defineBuiltin("NotADirectoryError", Symbol::Kind::Class);
    defineBuiltin("PermissionError", Symbol::Kind::Class);
    defineBuiltin("TimeoutError", Symbol::Kind::Class);
    defineBuiltin("ConnectionError", Symbol::Kind::Class);
    defineBuiltin("BrokenPipeError", Symbol::Kind::Class);
    defineBuiltin("ConnectionAbortedError", Symbol::Kind::Class);
    defineBuiltin("ConnectionRefusedError", Symbol::Kind::Class);
    defineBuiltin("ConnectionResetError", Symbol::Kind::Class);
    defineBuiltin("RuntimeError", Symbol::Kind::Class);
    defineBuiltin("NotImplementedError", Symbol::Kind::Class);
    defineBuiltin("RecursionError", Symbol::Kind::Class);
    defineBuiltin("SyntaxError", Symbol::Kind::Class);
    defineBuiltin("TypeError", Symbol::Kind::Class);
    defineBuiltin("ValueError", Symbol::Kind::Class);
    defineBuiltin("UnicodeError", Symbol::Kind::Class);
    defineBuiltin("UnicodeDecodeError", Symbol::Kind::Class);
    defineBuiltin("UnicodeEncodeError", Symbol::Kind::Class);
    defineBuiltin("UnicodeTranslateError", Symbol::Kind::Class);
    defineBuiltin("Warning", Symbol::Kind::Class);
    defineBuiltin("DeprecationWarning", Symbol::Kind::Class);
    defineBuiltin("FutureWarning", Symbol::Kind::Class);
    defineBuiltin("ResourceWarning", Symbol::Kind::Class);
    defineBuiltin("RuntimeWarning", Symbol::Kind::Class);
    defineBuiltin("UserWarning", Symbol::Kind::Class);

    // Built-in constants
    defineBuiltin("True", Symbol::Kind::Variable);
    defineBuiltin("False", Symbol::Kind::Variable);
    defineBuiltin("None", Symbol::Kind::Variable);
    defineBuiltin("__name__", Symbol::Kind::Variable);
    defineBuiltin("__file__", Symbol::Kind::Variable);
    defineBuiltin("__doc__", Symbol::Kind::Variable);
}

void Sema::resolveImport(const std::string&) {
    // Import resolution is a no-op for now
}

bool Sema::isValidAssignmentTarget(Expr* expr) {
    if (dynamic_cast<NameExpr*>(expr)) return true;
    if (dynamic_cast<AttributeExpr*>(expr)) return true;
    if (dynamic_cast<SubscriptExpr*>(expr)) return true;
    if (auto* tuple = dynamic_cast<TupleExpr*>(expr)) {
        for (auto& e : tuple->elements) {
            if (!isValidAssignmentTarget(e.get())) return false;
        }
        return true;
    }
    if (auto* list = dynamic_cast<ListExpr*>(expr)) {
        for (auto& e : list->elements) {
            if (!isValidAssignmentTarget(e.get())) return false;
        }
        return true;
    }
    if (auto* starred = dynamic_cast<StarredExpr*>(expr)) {
        return isValidAssignmentTarget(starred->value.get());
    }
    return false;
}

void Sema::error(const SourceLocation& loc, const std::string& message) {
    impl_->diagnostics.push_back({SemaDiagnostic::Level::Error, loc, message});
}

void Sema::warning(const SourceLocation& loc, const std::string& message) {
    impl_->diagnostics.push_back({SemaDiagnostic::Level::Warning, loc, message});
}

} // namespace dragon
