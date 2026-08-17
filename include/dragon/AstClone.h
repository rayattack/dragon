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

}

#endif

