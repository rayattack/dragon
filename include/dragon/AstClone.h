#ifndef DRAGON_AST_CLONE_H
#define DRAGON_AST_CLONE_H

#include "dragon/AST.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dragon {

// Deep-clone utilities (Decision 044 - generics monomorphization engine).
//
// The monomorphizer stamps a concrete copy of each generic class/function per
// distinct type argument. To do that it must deep-copy the decl's AST and, in
// the same pass, substitute every reference to a type parameter (`T`) with the
// concrete type argument's TypeExpr. These free functions provide both: a
// faithful structural clone of any Expr/Stmt/TypeExpr, with an optional
// substitution map applied to every embedded type annotation.
//
// `TypeSubst` maps a type-parameter name (e.g. "T") to the concrete TypeExpr to
// splice in (e.g. a NamedTypeExpr "int", or a GenericTypeExpr `list[int]`). When
// `cloneTypeExpr` encounters a bare NamedTypeExpr whose name is a key, it clones
// the substitution target instead of the original - so `list[T]` becomes
// `list[int]`, `T` becomes `int`, etc., throughout params/returns/fields/locals.
//
// Resolved-type pointers (`Expr::type`) are intentionally NOT copied: a stamped
// decl is re-type-checked, which repopulates them at the concrete type. Copying
// them would carry a `TypeVarType` into a context that must be concrete.
using TypeSubst = std::unordered_map<std::string, const TypeExpr*>;

std::unique_ptr<TypeExpr> cloneTypeExpr(const TypeExpr* t, const TypeSubst& subst = {});
std::unique_ptr<Expr> cloneExpr(const Expr* e, const TypeSubst& subst = {});
std::unique_ptr<Stmt> cloneStmt(const Stmt* s, const TypeSubst& subst = {});

// Clone a statement body (vector of owned Stmts), threading the substitution.
std::vector<std::unique_ptr<Stmt>> cloneBody(
    const std::vector<std::unique_ptr<Stmt>>& body, const TypeSubst& subst = {});

}  // namespace dragon

#endif  // DRAGON_AST_CLONE_H
