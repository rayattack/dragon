#ifndef DRAGON_AST_CLONE_H
#define DRAGON_AST_CLONE_H

#include "dragon/AST.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dragon {

using TypeSubst = std::unordered_map<std::string, const TypeExpr*>;

std::unique_ptr<TypeExpr> cloneTypeExpr(const TypeExpr* t, const TypeSubst& subst = {});
std::unique_ptr<Expr> cloneExpr(const Expr* e, const TypeSubst& subst = {});
std::unique_ptr<Stmt> cloneStmt(const Stmt* s, const TypeSubst& subst = {});

std::vector<std::unique_ptr<Stmt>> cloneBody(
    const std::vector<std::unique_ptr<Stmt>>& body, const TypeSubst& subst = {});

// Erase `type` aliases from the surface syntax before any semantic pass runs:
// every annotation naming an alias (declared in the module, or imported from an
// earlier module via `from m import a [as b]`) is rewritten to the alias's
// definition. Aliases stay declared (the checker still resolves and exports
// them) but no later pass ever needs to know they existed. Modules must be in
// dependency order, entry module last.
void canonicalizeTypeAliases(const std::vector<Module*>& modulesInDepOrder);

// True when the type expression mentions `name` anywhere (any nesting depth).
bool typeExprMentionsName(const TypeExpr* t, const std::string& name);

}

#endif

