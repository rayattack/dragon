#include "dragon/AstClone.h"

#include <cassert>
#include <cstdio>
#include <typeinfo>

namespace dragon {

namespace {
[[maybe_unused]] std::nullptr_t cloneMissingCase(const char* kind,
                                                 const ASTNode& node) {
    std::fprintf(stderr,
        "internal compiler error: AstClone has no case for %s node '%s' "
        "(generic monomorphization would silently drop it); add a clone case\n",
        kind, typeid(node).name());
    std::abort();
}
}

namespace {

template <typename T>
void setLoc(T& dst, const ASTNode& src) {
    dst->setLocation(src.location());
    if (auto* srcExpr = dynamic_cast<const Expr*>(&src)) {
        if (auto* dstExpr = dynamic_cast<Expr*>(dst.get())) dstExpr->type = srcExpr->type;
    }
}

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

std::unique_ptr<Expr> typeExprToValueExpr(const TypeExpr* t) {
    if (!t) return nullptr;
    if (auto* n = dynamic_cast<const NamedTypeExpr*>(t)) {
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
        if (!sub->object) return nullptr;
        // A type argument that has no value form (a union) does not stop the
        // whole expression being usable: in value position a generic is being
        // CONSTRUCTED (`T(**row)`), and construction depends on the base, not
        // on the element annotations. Fall back to the bare base rather than
        // emitting a null the visitors would dereference.
        if (g->typeArgs.size() == 1) {
            sub->index = typeExprToValueExpr(g->typeArgs[0].get());
            if (!sub->index) return typeExprToValueExpr(g->base.get());
        } else {
            auto tup = std::make_unique<TupleExpr>();
            for (auto& a : g->typeArgs) {
                auto v = typeExprToValueExpr(a.get());
                if (!v) return typeExprToValueExpr(g->base.get());
                tup->elements.push_back(std::move(v));
            }
            sub->index = std::move(tup);
        }
        return sub;
    }
    if (dynamic_cast<const UnionTypeExpr*>(t)) {
        // A union names a set of arms; it has no value form. Emitting one built
        // a `|` over the builtin conversion FUNCTIONS (`int | float`), which is
        // a bitwise-or on callables, not a type. Decline so the caller keeps
        // the type-parameter name instead of stamping nonsense.
        return nullptr;
    }
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

}

std::unique_ptr<TypeExpr> cloneTypeExpr(const TypeExpr* t, const TypeSubst& subst) {
    if (!t) return nullptr;
    if (auto* n = dynamic_cast<const NamedTypeExpr*>(t)) {
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
    if (auto* cs = dynamic_cast<const ContractSetTypeExpr*>(t)) {
        auto r = std::make_unique<ContractSetTypeExpr>();
        r->names = cs->names;
        setLoc(r, *t);
        return r;
    }
    cloneMissingCase("TypeExpr", *t);
    return nullptr;
}

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
        r->isDubMarked = n->isDubMarked;
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
    if (auto* n = dynamic_cast<const AsCastExpr*>(e)) {
        auto r = std::make_unique<AsCastExpr>();
        r->operand = cloneExpr(n->operand.get(), subst);
        r->contracts = n->contracts;
        r->fromBracedSet = n->fromBracedSet;
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
        r->valueIsFreshTask = n->valueIsFreshTask;
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
        for (auto& tp : n->typeParams) {
            TypeParam q;
            q.name = tp.name;
            q.bound = cloneTypeExpr(tp.bound.get(), subst);
            r->typeParams.push_back(std::move(q));
        }
        for (auto& p : n->params) r->params.push_back(cloneParam(p, subst));
        r->returnType = cloneTypeExpr(n->returnType.get(), subst);
        r->returnsOwn = n->returnsOwn;
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
        r->externLang = n->externLang;
        r->externPath = n->externPath;
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
        r->promises = n->promises;
        setLoc(r, *s);
        return r;
    }
    if (auto* n = dynamic_cast<const ContractDecl*>(s)) {
        auto r = std::make_unique<ContractDecl>();
        r->name = n->name;
        r->bases = n->bases;
        for (auto& m : n->methods) {
            auto cloned = cloneStmt(m.get(), subst);
            if (auto* fd = dynamic_cast<FunctionDecl*>(cloned.get())) {
                cloned.release();
                r->methods.emplace_back(fd);
            }
        }
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

bool typeExprMentionsName(const TypeExpr* t, const std::string& name) {
    if (!t) return false;
    if (auto* n = dynamic_cast<const NamedTypeExpr*>(t))
        return n->name == name;
    if (auto* g = dynamic_cast<const GenericTypeExpr*>(t)) {
        if (typeExprMentionsName(g->base.get(), name)) return true;
        for (auto& a : g->typeArgs)
            if (typeExprMentionsName(a.get(), name)) return true;
        return false;
    }
    if (auto* o = dynamic_cast<const OptionalTypeExpr*>(t))
        return typeExprMentionsName(o->inner.get(), name);
    if (auto* u = dynamic_cast<const UnionTypeExpr*>(t)) {
        for (auto& m : u->types)
            if (typeExprMentionsName(m.get(), name)) return true;
        return false;
    }
    if (auto* c = dynamic_cast<const CallableTypeExpr*>(t)) {
        for (auto& p : c->paramTypes)
            if (typeExprMentionsName(p.get(), name)) return true;
        return typeExprMentionsName(c->returnType.get(), name);
    }
    if (auto* tt = dynamic_cast<const TupleTypeExpr*>(t)) {
        for (auto& e : tt->elementTypes)
            if (typeExprMentionsName(e.get(), name)) return true;
        return false;
    }
    return false;
}

void canonicalizeTypeAliases(const std::vector<Module*>& modulesInDepOrder) {
    // module name -> that module's own top-level aliases, expanded
    std::unordered_map<std::string, TypeSubst> exportedAliases;
    // Owns every expanded definition; substituted copies are cloned into the
    // ASTs, so this only needs to live for the duration of the pass.
    std::vector<std::unique_ptr<TypeExpr>> owned;

    for (Module* m : modulesInDepOrder) {
        if (!m) continue;
        TypeSubst visible;
        TypeSubst own;
        // Several passes over the module: a use that appears ABOVE its `type`
        // declaration must still resolve (module-level names resolve before
        // codegen, so substitution cannot depend on source order), and each
        // extra round resolves one more level of alias-of-alias chaining.
        for (int round = 0; round < 4; ++round)
        for (auto& stmt : m->body) {
            if (auto* fi = dynamic_cast<FromImportStmt*>(stmt.get())) {
                auto srcIt = exportedAliases.find(fi->module);
                if (srcIt == exportedAliases.end()) continue;
                for (auto& al : fi->names) {
                    auto aIt = srcIt->second.find(al.name);
                    if (aIt == srcIt->second.end()) continue;
                    visible[al.asName.empty() ? al.name : al.asName] = aIt->second;
                }
            } else if (auto* ta = dynamic_cast<TypeAliasStmt*>(stmt.get())) {
                if (!ta->value) continue;
                if (typeExprMentionsName(ta->value.get(), ta->name)) {
                    // Recursive alias: textual substitution would never
                    // terminate. Uses keep (or are renamed back to) the
                    // canonical name; the checker ties the knot and codegen
                    // resolves the name through its alias table.
                    auto canonical = std::make_unique<NamedTypeExpr>();
                    canonical->name = ta->name;
                    own[ta->name] = canonical.get();
                    visible[ta->name] = canonical.get();
                    owned.push_back(std::move(canonical));
                    continue;
                }
                // Expand with what is visible so far, so alias-of-alias chains
                // resolve fully (declaration order matters, as in the checker).
                auto expanded = cloneTypeExpr(ta->value.get(), visible);
                own[ta->name] = expanded.get();
                visible[ta->name] = expanded.get();
                owned.push_back(std::move(expanded));
            }
        }
        if (!visible.empty()) {
            // A generic's type parameters are not aliases: an alias sharing a
            // name with any type parameter in this module is left alone rather
            // than risking substitution inside the generic's own scope.
            for (auto& stmt : m->body) {
                auto stripParams = [&](const std::vector<TypeParam>& tps) {
                    for (auto& tp : tps) visible.erase(tp.name);
                };
                if (auto* fd = dynamic_cast<FunctionDecl*>(stmt.get()))
                    stripParams(fd->typeParams);
                else if (auto* cd = dynamic_cast<ClassDecl*>(stmt.get())) {
                    stripParams(cd->typeParams);
                    for (auto& ms : cd->body)
                        if (auto* md = dynamic_cast<FunctionDecl*>(ms.get()))
                            stripParams(md->typeParams);
                }
            }
        }
        if (!visible.empty()) {
            for (auto& stmt : m->body) {
                auto replaced = cloneStmt(stmt.get(), visible);
                if (replaced) stmt = std::move(replaced);
            }
        }
        exportedAliases[m->moduleName] = std::move(own);
    }
}

}
