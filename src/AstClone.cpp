// Deep-clone utilities for the Decision 044 generics monomorphization engine.
// See include/dragon/AstClone.h for the contract. Every Expr/Stmt/TypeExpr node
// type is handled explicitly; a missing case returns nullptr (caught loudly in
// testing rather than silently dropping a subtree). New AST nodes must be added
// here - that coupling is the price of a faithful structural clone.

#include "dragon/AstClone.h"

#include <cassert>
#include <cstdio>
#include <typeinfo>

namespace dragon {

namespace {
// A missing clone case is an internal compiler error, not a silent drop: the
// monomorphizer consumes a null clone with `if (!cloned) continue;`, so a new
// AST node added without an AstClone case would silently omit a stamped
// statement / null a sub-expression and miscompile the generic instantiation.
// Fire loudly on every build (stderr) and abort under assertions so tests
// catch it.
[[maybe_unused]] std::nullptr_t cloneMissingCase(const char* kind,
                                                 const ASTNode& node) {
    std::fprintf(stderr,
        "internal compiler error: AstClone has no case for %s node '%s' "
        "(generic monomorphization would silently drop it); add a clone case\n",
        kind, typeid(node).name());
    assert(false && "AstClone: missing node case (see stderr)");
    return nullptr;
}
}  // namespace

//===----------------------------------------------------------------------===//
// Small helpers
//===----------------------------------------------------------------------===//

namespace {

template <typename T>
void setLoc(T& dst, const ASTNode& src) { dst->setLocation(src.location()); }

std::vector<std::unique_ptr<TypeExpr>> cloneTypeVec(
    const std::vector<std::unique_ptr<TypeExpr>>& v, const TypeSubst& subst) {
    std::vector<std::unique_ptr<TypeExpr>> out;
    out.reserve(v.size());
    for (auto& e : v) out.push_back(cloneTypeExpr(e.get(), subst));
    return out;
}

std::vector<std::unique_ptr<Expr>> cloneExprVec(
    const std::vector<std::unique_ptr<Expr>>& v, const TypeSubst& subst) {
    std::vector<std::unique_ptr<Expr>> out;
    out.reserve(v.size());
    for (auto& e : v) out.push_back(cloneExpr(e.get(), subst));
    return out;
}

Parameter cloneParam(const Parameter& p, const TypeSubst& subst) {
    Parameter q;
    q.name = p.name;
    q.type = cloneTypeExpr(p.type.get(), subst);
    q.defaultValue = cloneExpr(p.defaultValue.get(), subst);
    q.isVarArg = p.isVarArg;
    q.isKwArg = p.isKwArg;
    q.isOwn = p.isOwn;
    return q;
}

CompClause cloneClause(const CompClause& c, const TypeSubst& subst) {
    CompClause q;
    q.varNames = c.varNames;
    q.iterable = cloneExpr(c.iterable.get(), subst);
    q.condition = cloneExpr(c.condition.get(), subst);
    return q;
}

std::vector<CompClause> cloneClauses(const std::vector<CompClause>& v,
                                     const TypeSubst& subst) {
    std::vector<CompClause> out;
    out.reserve(v.size());
    for (auto& c : v) out.push_back(cloneClause(c, subst));
    return out;
}

// Render a TypeExpr as the equivalent value-position expression, so a
// type-parameter used as a call-site/construction type argument (`Inner[T]`,
// `g[T](...)` - where the `[...]` is parsed as a value subscript, not a type
// annotation) substitutes correctly: `T` -> `int` becomes the value `int`,
// `T` -> `list[int]` becomes the value subscript `list[int]`, `T` -> a stamped
// class becomes its name. Used by cloneExpr's NameExpr case.
std::unique_ptr<Expr> typeExprToValueExpr(const TypeExpr* t) {
    if (!t) return nullptr;
    if (auto* n = dynamic_cast<const NamedTypeExpr*>(t)) {
        // A dotted type name ("mod.Class") becomes an attribute chain.
        auto dot = n->name.find('.');
        if (dot == std::string::npos) {
            auto e = std::make_unique<NameExpr>();
            e->name = n->name;
            return e;
        }
        std::unique_ptr<Expr> cur;
        size_t start = 0;
        while (true) {
            auto next = n->name.find('.', start);
            std::string seg = n->name.substr(start, next == std::string::npos
                                                         ? std::string::npos
                                                         : next - start);
            if (!cur) {
                auto nm = std::make_unique<NameExpr>(); nm->name = seg; cur = std::move(nm);
            } else {
                auto at = std::make_unique<AttributeExpr>();
                at->object = std::move(cur); at->attribute = seg; cur = std::move(at);
            }
            if (next == std::string::npos) break;
            start = next + 1;
        }
        return cur;
    }
    if (auto* g = dynamic_cast<const GenericTypeExpr*>(t)) {
        auto sub = std::make_unique<SubscriptExpr>();
        sub->object = typeExprToValueExpr(g->base.get());
        if (g->typeArgs.size() == 1) {
            sub->index = typeExprToValueExpr(g->typeArgs[0].get());
        } else {
            auto tup = std::make_unique<TupleExpr>();
            for (auto& a : g->typeArgs) tup->elements.push_back(typeExprToValueExpr(a.get()));
            sub->index = std::move(tup);
        }
        return sub;
    }
    // A union (`int | str`) used as a value-position type argument becomes a
    // left-folded `|` BinaryExpr chain - exprToTypeExpr flattens it back to a
    // UnionTypeExpr, so it round-trips. (Optional was normalized to a union with
    // None at parse time, so it is covered here too.)
    if (auto* u = dynamic_cast<const UnionTypeExpr*>(t)) {
        std::unique_ptr<Expr> acc;
        for (auto& mem : u->types) {
            auto v = typeExprToValueExpr(mem.get());
            if (!v) return nullptr;  // a member isn't value-denotable
            if (!acc) { acc = std::move(v); continue; }
            auto bin = std::make_unique<BinaryExpr>();
            bin->left = std::move(acc);
            bin->op = Token(TokenType::PIPE, "|", SourceLocation{});
            bin->right = std::move(v);
            acc = std::move(bin);
        }
        return acc;  // null only if the union was empty (never produced)
    }
    // Callables and bare tuple-type-exprs are not value-position type args.
    return nullptr;
}

MatchPattern clonePattern(const MatchPattern& p, const TypeSubst& subst) {
    MatchPattern q;
    q.kind = p.kind;
    q.literal = cloneExpr(p.literal.get(), subst);
    q.name = p.name;
    for (auto& sp : p.subPatterns) q.subPatterns.push_back(clonePattern(sp, subst));
    q.guard = cloneExpr(p.guard.get(), subst);
    return q;
}

}  // namespace

//===----------------------------------------------------------------------===//
// Type expressions
//===----------------------------------------------------------------------===//

std::unique_ptr<TypeExpr> cloneTypeExpr(const TypeExpr* t, const TypeSubst& subst) {
    if (!t) return nullptr;
    if (auto* n = dynamic_cast<const NamedTypeExpr*>(t)) {
        // The substitution pivot: a bare type-parameter name is replaced by the
        // concrete type argument's TypeExpr (cloned, so the request's template is
        // not aliased into the stamped decl). Dotted names ("mod.Class") never
        // match a type-param key.
        auto it = subst.find(n->name);
        if (it != subst.end()) return cloneTypeExpr(it->second, {});
        auto r = std::make_unique<NamedTypeExpr>();
        r->name = n->name;
        setLoc(r, *t);
        return r;
    }
    if (auto* g = dynamic_cast<const GenericTypeExpr*>(t)) {
        auto r = std::make_unique<GenericTypeExpr>();
        r->base = cloneTypeExpr(g->base.get(), subst);
        r->typeArgs = cloneTypeVec(g->typeArgs, subst);
        setLoc(r, *t);
        return r;
    }
    if (auto* o = dynamic_cast<const OptionalTypeExpr*>(t)) {
        auto r = std::make_unique<OptionalTypeExpr>();
        r->inner = cloneTypeExpr(o->inner.get(), subst);
        setLoc(r, *t);
        return r;
    }
    if (auto* u = dynamic_cast<const UnionTypeExpr*>(t)) {
        auto r = std::make_unique<UnionTypeExpr>();
        r->types = cloneTypeVec(u->types, subst);
        setLoc(r, *t);
        return r;
    }
    if (auto* c = dynamic_cast<const CallableTypeExpr*>(t)) {
        auto r = std::make_unique<CallableTypeExpr>();
        r->paramTypes = cloneTypeVec(c->paramTypes, subst);
        r->returnType = cloneTypeExpr(c->returnType.get(), subst);
        setLoc(r, *t);
        return r;
    }
    if (auto* tt = dynamic_cast<const TupleTypeExpr*>(t)) {
        auto r = std::make_unique<TupleTypeExpr>();
        r->elementTypes = cloneTypeVec(tt->elementTypes, subst);
        setLoc(r, *t);
        return r;
    }
    return nullptr;
}

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

std::unique_ptr<Expr> cloneExpr(const Expr* e, const TypeSubst& subst) {
    if (!e) return nullptr;

    if (auto* n = dynamic_cast<const IntegerLiteral*>(e)) {
        auto r = std::make_unique<IntegerLiteral>(); r->value = n->value; setLoc(r, *e); return r;
    }
    if (auto* n = dynamic_cast<const FloatLiteral*>(e)) {
        auto r = std::make_unique<FloatLiteral>(); r->value = n->value; setLoc(r, *e); return r;
    }
    if (auto* n = dynamic_cast<const StringLiteral*>(e)) {
        auto r = std::make_unique<StringLiteral>();
        r->value = n->value;
        r->isRaw = n->isRaw;
        r->isFString = n->isFString;
        r->isBytes = n->isBytes;
        for (auto& part : n->fstringParts) {
            FStringPart fp;
            fp.kind = part.kind;
            fp.literal = part.literal;
            fp.expr = cloneExpr(part.expr.get(), subst);
            fp.formatSpec = part.formatSpec;
            fp.conversion = part.conversion;
            r->fstringParts.push_back(std::move(fp));
        }
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const BooleanLiteral*>(e)) {
        auto r = std::make_unique<BooleanLiteral>(); r->value = n->value; setLoc(r, *e); return r;
    }
    if (dynamic_cast<const NoneLiteral*>(e)) {
        auto r = std::make_unique<NoneLiteral>(); setLoc(r, *e); return r;
    }
    if (auto* n = dynamic_cast<const NameExpr*>(e)) {
        // A type parameter used in VALUE position (the `T` in a `Inner[T]` /
        // `g[T](...)` type-argument subscript) is substituted to the value-syntax
        // form of its concrete type. In a generic body a bare `T` is always a
        // type reference (never a value variable), so this rewrite is safe.
        auto it = subst.find(n->name);
        if (it != subst.end()) {
            if (auto v = typeExprToValueExpr(it->second)) { v->setLocation(e->location()); return v; }
        }
        auto r = std::make_unique<NameExpr>(); r->name = n->name;
        r->isMoveMarked = n->isMoveMarked;
        r->isDubMarked = n->isDubMarked;
        setLoc(r, *e); return r;
    }
    if (auto* n = dynamic_cast<const BinaryExpr*>(e)) {
        auto r = std::make_unique<BinaryExpr>();
        r->left = cloneExpr(n->left.get(), subst);
        r->op = n->op;
        r->right = cloneExpr(n->right.get(), subst);
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const ChainedCompExpr*>(e)) {
        auto r = std::make_unique<ChainedCompExpr>();
        r->operands = cloneExprVec(n->operands, subst);
        r->operators = n->operators;
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const WalrusExpr*>(e)) {
        auto r = std::make_unique<WalrusExpr>();
        r->name = n->name;
        r->value = cloneExpr(n->value.get(), subst);
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const UnaryExpr*>(e)) {
        auto r = std::make_unique<UnaryExpr>();
        r->op = n->op;
        r->operand = cloneExpr(n->operand.get(), subst);
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const CallExpr*>(e)) {
        auto r = std::make_unique<CallExpr>();
        r->resolvedMethodOverload = n->resolvedMethodOverload;
        r->callee = cloneExpr(n->callee.get(), subst);
        r->args = cloneExprVec(n->args, subst);
        for (auto& [k, v] : n->kwArgs)
            r->kwArgs.emplace_back(k, cloneExpr(v.get(), subst));
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const AttributeExpr*>(e)) {
        auto r = std::make_unique<AttributeExpr>();
        r->object = cloneExpr(n->object.get(), subst);
        r->attribute = n->attribute;
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const SubscriptExpr*>(e)) {
        auto r = std::make_unique<SubscriptExpr>();
        r->object = cloneExpr(n->object.get(), subst);
        r->index = cloneExpr(n->index.get(), subst);
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const SliceExpr*>(e)) {
        auto r = std::make_unique<SliceExpr>();
        r->lower = cloneExpr(n->lower.get(), subst);
        r->upper = cloneExpr(n->upper.get(), subst);
        r->step = cloneExpr(n->step.get(), subst);
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const ListExpr*>(e)) {
        auto r = std::make_unique<ListExpr>();
        r->elements = cloneExprVec(n->elements, subst);
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const TupleExpr*>(e)) {
        auto r = std::make_unique<TupleExpr>();
        r->elements = cloneExprVec(n->elements, subst);
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const DictExpr*>(e)) {
        auto r = std::make_unique<DictExpr>();
        for (auto& [k, v] : n->entries)
            r->entries.emplace_back(cloneExpr(k.get(), subst), cloneExpr(v.get(), subst));
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const SetExpr*>(e)) {
        auto r = std::make_unique<SetExpr>();
        r->elements = cloneExprVec(n->elements, subst);
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const ListCompExpr*>(e)) {
        auto r = std::make_unique<ListCompExpr>();
        r->element = cloneExpr(n->element.get(), subst);
        r->varName = n->varName;
        r->iterable = cloneExpr(n->iterable.get(), subst);
        r->condition = cloneExpr(n->condition.get(), subst);
        r->extraClauses = cloneClauses(n->extraClauses, subst);
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const DictCompExpr*>(e)) {
        auto r = std::make_unique<DictCompExpr>();
        r->key = cloneExpr(n->key.get(), subst);
        r->value = cloneExpr(n->value.get(), subst);
        r->varNames = n->varNames;
        r->iterable = cloneExpr(n->iterable.get(), subst);
        r->condition = cloneExpr(n->condition.get(), subst);
        r->extraClauses = cloneClauses(n->extraClauses, subst);
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const SetCompExpr*>(e)) {
        auto r = std::make_unique<SetCompExpr>();
        r->element = cloneExpr(n->element.get(), subst);
        r->varName = n->varName;
        r->iterable = cloneExpr(n->iterable.get(), subst);
        r->condition = cloneExpr(n->condition.get(), subst);
        r->extraClauses = cloneClauses(n->extraClauses, subst);
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const GeneratorExpr*>(e)) {
        auto r = std::make_unique<GeneratorExpr>();
        r->element = cloneExpr(n->element.get(), subst);
        r->varName = n->varName;
        r->iterable = cloneExpr(n->iterable.get(), subst);
        r->condition = cloneExpr(n->condition.get(), subst);
        r->extraClauses = cloneClauses(n->extraClauses, subst);
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const LambdaExpr*>(e)) {
        auto r = std::make_unique<LambdaExpr>();
        for (auto& p : n->params) {
            LambdaExpr::Parameter q;
            q.name = p.name;
            q.type = cloneTypeExpr(p.type.get(), subst);
            q.defaultValue = cloneExpr(p.defaultValue.get(), subst);
            r->params.push_back(std::move(q));
        }
        r->returnType = cloneTypeExpr(n->returnType.get(), subst);
        r->body = cloneExpr(n->body.get(), subst);
        r->bodyStmts = cloneBody(n->bodyStmts, subst);
        r->capturedVars = n->capturedVars;
        r->mutatedCapturedVars = n->mutatedCapturedVars;
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const IfExpr*>(e)) {
        auto r = std::make_unique<IfExpr>();
        r->condition = cloneExpr(n->condition.get(), subst);
        r->thenExpr = cloneExpr(n->thenExpr.get(), subst);
        r->elseExpr = cloneExpr(n->elseExpr.get(), subst);
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const AwaitExpr*>(e)) {
        auto r = std::make_unique<AwaitExpr>();
        r->operand = cloneExpr(n->operand.get(), subst);
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const FireExpr*>(e)) {
        auto r = std::make_unique<FireExpr>();
        r->operand = cloneExpr(n->operand.get(), subst);
        r->bodyStmts = cloneBody(n->bodyStmts, subst);
        r->capturedVars = n->capturedVars;
        r->mutatedCapturedVars = n->mutatedCapturedVars;
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const YieldExpr*>(e)) {
        auto r = std::make_unique<YieldExpr>();
        r->value = cloneExpr(n->value.get(), subst);
        r->isYieldFrom = n->isYieldFrom;
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const StarredExpr*>(e)) {
        auto r = std::make_unique<StarredExpr>();
        r->value = cloneExpr(n->value.get(), subst);
        r->isDoubleStar = n->isDoubleStar;
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const TemplateExpr*>(e)) {
        auto r = std::make_unique<TemplateExpr>();
        r->body = n->body;
        r->contentType = n->contentType;
        r->isContentAlias = n->isContentAlias;
        // Deep-clone the pre-parsed interpolations so a template inside a
        // generic body keeps its typed AST after monomorphization (mirrors the
        // fstringParts clone above); without this the clone would carry `body`
        // but no parts and lower to empty output.
        for (auto& part : n->templateParts) {
            TemplatePart tp;
            tp.kind = part.kind;
            tp.literal = part.literal;
            tp.expr = cloneExpr(part.expr.get(), subst);
            for (auto& s : part.blockStmts)
                tp.blockStmts.push_back(cloneStmt(s.get(), subst));
            tp.filterName = part.filterName;
            tp.isSpread = part.isSpread;
            tp.exprText = part.exprText;
            tp.bangPos = part.bangPos;
            tp.parseFailed = part.parseFailed;
            r->templateParts.push_back(std::move(tp));
        }
        setLoc(r, *e);
        return r;
    }
    if (auto* n = dynamic_cast<const TemplateFileExpr*>(e)) {
        auto r = std::make_unique<TemplateFileExpr>();
        r->filePath = n->filePath;
        r->contentType = n->contentType;
        setLoc(r, *e);
        return r;
    }
    return cloneMissingCase("Expr", *e);
}

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

std::vector<std::unique_ptr<Stmt>> cloneBody(
    const std::vector<std::unique_ptr<Stmt>>& body, const TypeSubst& subst) {
    std::vector<std::unique_ptr<Stmt>> out;
    out.reserve(body.size());
    for (auto& s : body) out.push_back(cloneStmt(s.get(), subst));
    return out;
}

std::unique_ptr<Stmt> cloneStmt(const Stmt* s, const TypeSubst& subst) {
    if (!s) return nullptr;

    if (auto* n = dynamic_cast<const ExprStmt*>(s)) {
        auto r = std::make_unique<ExprStmt>();
        r->expr = cloneExpr(n->expr.get(), subst);
        setLoc(r, *s);
        return r;
    }
    if (auto* n = dynamic_cast<const AssignStmt*>(s)) {
        auto r = std::make_unique<AssignStmt>();
        r->targets = cloneExprVec(n->targets, subst);
        r->value = cloneExpr(n->value.get(), subst);
        r->typeAnnotation = cloneTypeExpr(n->typeAnnotation.get(), subst);
        setLoc(r, *s);
        return r;
    }
    if (auto* n = dynamic_cast<const AugAssignStmt*>(s)) {
        auto r = std::make_unique<AugAssignStmt>();
        r->target = cloneExpr(n->target.get(), subst);
        r->op = n->op;
        r->value = cloneExpr(n->value.get(), subst);
        setLoc(r, *s);
        return r;
    }
    if (auto* n = dynamic_cast<const AnnAssignStmt*>(s)) {
        auto r = std::make_unique<AnnAssignStmt>();
        r->target = cloneExpr(n->target.get(), subst);
        r->annotation = cloneTypeExpr(n->annotation.get(), subst);
        r->value = cloneExpr(n->value.get(), subst);
        r->isConst = n->isConst;
        r->isStatic = n->isStatic;
        r->isOwn = n->isOwn;
        setLoc(r, *s);
        return r;
    }
    if (auto* n = dynamic_cast<const IfStmt*>(s)) {
        auto r = std::make_unique<IfStmt>();
        r->condition = cloneExpr(n->condition.get(), subst);
        r->thenBody = cloneBody(n->thenBody, subst);
        for (auto& [cond, body] : n->elifClauses)
            r->elifClauses.emplace_back(cloneExpr(cond.get(), subst),
                                        cloneBody(body, subst));
        r->elseBody = cloneBody(n->elseBody, subst);
        setLoc(r, *s);
        return r;
    }
    if (auto* n = dynamic_cast<const WhileStmt*>(s)) {
        auto r = std::make_unique<WhileStmt>();
        r->condition = cloneExpr(n->condition.get(), subst);
        r->body = cloneBody(n->body, subst);
        r->elseBody = cloneBody(n->elseBody, subst);
        setLoc(r, *s);
        return r;
    }
    if (auto* n = dynamic_cast<const ForStmt*>(s)) {
        auto r = std::make_unique<ForStmt>();
        r->target = cloneExpr(n->target.get(), subst);
        r->iterable = cloneExpr(n->iterable.get(), subst);
        r->body = cloneBody(n->body, subst);
        r->elseBody = cloneBody(n->elseBody, subst);
        setLoc(r, *s);
        return r;
    }
    if (auto* n = dynamic_cast<const TryStmt*>(s)) {
        auto r = std::make_unique<TryStmt>();
        r->tryBody = cloneBody(n->tryBody, subst);
        for (auto& h : n->handlers) {
            TryStmt::ExceptHandler q;
            q.type = cloneTypeExpr(h.type.get(), subst);
            q.altTypeNames = h.altTypeNames;
            q.name = h.name;
            q.body = cloneBody(h.body, subst);
            q.isStar = h.isStar;
            r->handlers.push_back(std::move(q));
        }
        r->elseBody = cloneBody(n->elseBody, subst);
        r->finallyBody = cloneBody(n->finallyBody, subst);
        setLoc(r, *s);
        return r;
    }
    if (auto* n = dynamic_cast<const WithStmt*>(s)) {
        auto r = std::make_unique<WithStmt>();
        for (auto& it : n->items) {
            WithStmt::WithItem q;
            q.contextExpr = cloneExpr(it.contextExpr.get(), subst);
            q.optionalVars = cloneExpr(it.optionalVars.get(), subst);
            r->items.push_back(std::move(q));
        }
        r->body = cloneBody(n->body, subst);
        setLoc(r, *s);
        return r;
    }
    if (auto* n = dynamic_cast<const ThreadStmt*>(s)) {
        auto r = std::make_unique<ThreadStmt>();
        r->body = cloneBody(n->body, subst);
        r->capturedVars = n->capturedVars;
        r->mutatedCapturedVars = n->mutatedCapturedVars;
        setLoc(r, *s);
        return r;
    }
    if (auto* n = dynamic_cast<const DeferStmt*>(s)) {
        auto r = std::make_unique<DeferStmt>();
        r->call = cloneExpr(n->call.get(), subst);
        setLoc(r, *s);
        return r;
    }
    if (auto* n = dynamic_cast<const MatchStmt*>(s)) {
        auto r = std::make_unique<MatchStmt>();
        r->subject = cloneExpr(n->subject.get(), subst);
        for (auto& c : n->cases) {
            MatchStmt::MatchCase q;
            q.pattern = clonePattern(c.pattern, subst);
            q.guard = cloneExpr(c.guard.get(), subst);
            q.body = cloneBody(c.body, subst);
            r->cases.push_back(std::move(q));
        }
        setLoc(r, *s);
        return r;
    }
    if (auto* n = dynamic_cast<const ReturnStmt*>(s)) {
        auto r = std::make_unique<ReturnStmt>();
        r->value = cloneExpr(n->value.get(), subst);
        setLoc(r, *s);
        return r;
    }
    if (auto* n = dynamic_cast<const RaiseStmt*>(s)) {
        auto r = std::make_unique<RaiseStmt>();
        r->exception = cloneExpr(n->exception.get(), subst);
        r->cause = cloneExpr(n->cause.get(), subst);
        setLoc(r, *s);
        return r;
    }
    if (dynamic_cast<const BreakStmt*>(s)) {
        auto r = std::make_unique<BreakStmt>(); setLoc(r, *s); return r;
    }
    if (dynamic_cast<const ContinueStmt*>(s)) {
        auto r = std::make_unique<ContinueStmt>(); setLoc(r, *s); return r;
    }
    if (dynamic_cast<const PassStmt*>(s)) {
        auto r = std::make_unique<PassStmt>(); setLoc(r, *s); return r;
    }
    if (auto* n = dynamic_cast<const AssertStmt*>(s)) {
        auto r = std::make_unique<AssertStmt>();
        r->test = cloneExpr(n->test.get(), subst);
        r->msg = cloneExpr(n->msg.get(), subst);
        setLoc(r, *s);
        return r;
    }
    if (auto* n = dynamic_cast<const GlobalStmt*>(s)) {
        auto r = std::make_unique<GlobalStmt>(); r->names = n->names; setLoc(r, *s); return r;
    }
    if (auto* n = dynamic_cast<const NonlocalStmt*>(s)) {
        auto r = std::make_unique<NonlocalStmt>(); r->names = n->names; setLoc(r, *s); return r;
    }
    if (auto* n = dynamic_cast<const DeleteStmt*>(s)) {
        auto r = std::make_unique<DeleteStmt>();
        r->targets = cloneExprVec(n->targets, subst);
        r->provenUnique = n->provenUnique;
        setLoc(r, *s);
        return r;
    }
    if (auto* n = dynamic_cast<const ImportStmt*>(s)) {
        auto r = std::make_unique<ImportStmt>(); r->names = n->names; setLoc(r, *s); return r;
    }
    if (auto* n = dynamic_cast<const FromImportStmt*>(s)) {
        auto r = std::make_unique<FromImportStmt>();
        r->module = n->module;
        r->level = n->level;
        r->names = n->names;
        setLoc(r, *s);
        return r;
    }
    if (auto* n = dynamic_cast<const FunctionDecl*>(s)) {
        auto r = std::make_unique<FunctionDecl>();
        r->name = n->name;
        // typeParams intentionally copied verbatim; the monomorphizer clears
        // them on a stamped instantiation (a concrete decl is not a template).
        for (auto& tp : n->typeParams) {
            TypeParam q;
            q.name = tp.name;
            q.bound = cloneTypeExpr(tp.bound.get(), subst);
            r->typeParams.push_back(std::move(q));
        }
        for (auto& p : n->params) r->params.push_back(cloneParam(p, subst));
        r->returnType = cloneTypeExpr(n->returnType.get(), subst);
        r->body = cloneBody(n->body, subst);
        r->decorators = cloneExprVec(n->decorators, subst);
        r->isAsync = n->isAsync;
        r->isMethod = n->isMethod;
        r->hasImplicitSelf = n->hasImplicitSelf;
        r->isStatic = n->isStatic;
        r->isClassMethod = n->isClassMethod;
        r->isConstructor = n->isConstructor;
        r->isExtern = n->isExtern;
        r->externLib = n->externLib;
        r->externSymbol = n->externSymbol;
        r->isProperty = n->isProperty;
        r->propertySetterFor = n->propertySetterFor;
        r->constructorIndex = n->constructorIndex;
        r->methodOverloadIndex = n->methodOverloadIndex;
        r->methodOverloadCount = n->methodOverloadCount;
        r->posOnlyEnd = n->posOnlyEnd;
        r->kwOnlyStart = n->kwOnlyStart;
        r->capturedVars = n->capturedVars;
        r->mutatedCapturedVars = n->mutatedCapturedVars;
        r->docstring = n->docstring;
        setLoc(r, *s);
        return r;
    }
    if (auto* n = dynamic_cast<const ClassDecl*>(s)) {
        auto r = std::make_unique<ClassDecl>();
        r->name = n->name;
        for (auto& tp : n->typeParams) {
            TypeParam q;
            q.name = tp.name;
            q.bound = cloneTypeExpr(tp.bound.get(), subst);
            r->typeParams.push_back(std::move(q));
        }
        r->bases = cloneExprVec(n->bases, subst);
        for (auto& [k, v] : n->keywords)
            r->keywords.emplace_back(k, cloneExpr(v.get(), subst));
        r->body = cloneBody(n->body, subst);
        r->decorators = cloneExprVec(n->decorators, subst);
        r->docstring = n->docstring;
        setLoc(r, *s);
        return r;
    }
    if (auto* n = dynamic_cast<const TypeAliasStmt*>(s)) {
        auto r = std::make_unique<TypeAliasStmt>();
        r->name = n->name;
        r->value = cloneTypeExpr(n->value.get(), subst);
        setLoc(r, *s);
        return r;
    }
    return cloneMissingCase("Stmt", *s);
}

}  // namespace dragon
