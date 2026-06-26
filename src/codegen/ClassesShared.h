/// Dragon CodeGen - shared class-emission AST builders
///
/// cloneTypeExpr / makeSelfAssign are used by BOTH the class declaration
/// emitter (Classes.cpp) and the @dataclass / NamedTuple / Enum synthesis
/// (ClassSynthesis.cpp). `inline` so both TUs can include this without an
/// ODR clash. Pure code motion from Classes.cpp - no behavior change.
#ifndef DRAGON_CODEGEN_CLASSES_SHARED_H
#define DRAGON_CODEGEN_CLASSES_SHARED_H

#include "../CodeGenImpl.h"

namespace dragon {

// Deep-clone a TypeExpr (used by @dataclass / NamedTuple synthesis to copy
// class-body field annotations into synthesized __init__ parameter types).
inline std::unique_ptr<TypeExpr> cloneTypeExpr(TypeExpr* t) {
    if (!t) return nullptr;
    if (auto* n = dynamic_cast<NamedTypeExpr*>(t)) {
        auto r = std::make_unique<NamedTypeExpr>();
        r->name = n->name;
        r->setLocation(n->location());
        return r;
    }
    if (auto* g = dynamic_cast<GenericTypeExpr*>(t)) {
        auto r = std::make_unique<GenericTypeExpr>();
        r->base = cloneTypeExpr(g->base.get());
        for (auto& a : g->typeArgs) r->typeArgs.push_back(cloneTypeExpr(a.get()));
        r->setLocation(g->location());
        return r;
    }
    if (auto* o = dynamic_cast<OptionalTypeExpr*>(t)) {
        auto r = std::make_unique<OptionalTypeExpr>();
        r->inner = cloneTypeExpr(o->inner.get());
        r->setLocation(o->location());
        return r;
    }
    if (auto* u = dynamic_cast<UnionTypeExpr*>(t)) {
        auto r = std::make_unique<UnionTypeExpr>();
        for (auto& tt : u->types) r->types.push_back(cloneTypeExpr(tt.get()));
        r->setLocation(u->location());
        return r;
    }
    if (auto* c = dynamic_cast<CallableTypeExpr*>(t)) {
        auto r = std::make_unique<CallableTypeExpr>();
        for (auto& pt : c->paramTypes) r->paramTypes.push_back(cloneTypeExpr(pt.get()));
        r->returnType = cloneTypeExpr(c->returnType.get());
        r->setLocation(c->location());
        return r;
    }
    if (auto* tt = dynamic_cast<TupleTypeExpr*>(t)) {
        auto r = std::make_unique<TupleTypeExpr>();
        for (auto& et : tt->elementTypes) r->elementTypes.push_back(cloneTypeExpr(et.get()));
        r->setLocation(tt->location());
        return r;
    }
    return nullptr;
}

// Helper: build an AssignStmt for `self.<field> = <field>`.
inline std::unique_ptr<Stmt> makeSelfAssign(const std::string& field, SourceLocation loc) {
    auto attrTarget = std::make_unique<AttributeExpr>();
    auto selfName = std::make_unique<NameExpr>();
    selfName->name = "self";
    selfName->setLocation(loc);
    attrTarget->object = std::move(selfName);
    attrTarget->attribute = field;
    attrTarget->setLocation(loc);

    auto rhs = std::make_unique<NameExpr>();
    rhs->name = field;
    rhs->setLocation(loc);

    auto assign = std::make_unique<AssignStmt>();
    assign->targets.push_back(std::move(attrTarget));
    assign->value = std::move(rhs);
    assign->setLocation(loc);
    return assign;
}

}  // namespace dragon

#endif  // DRAGON_CODEGEN_CLASSES_SHARED_H
