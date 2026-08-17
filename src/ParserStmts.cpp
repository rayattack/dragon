#include "dragon/Parser.h"
#include "ParserImpl.h"
#include <cctype>
#include <charconv>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace dragon {

std::unique_ptr<Stmt> Parser::statement() {
    ParserRecursionGuard guard(impl_->recursionDepth);
    if (impl_->recursionDepth > Impl::kMaxRecursionDepth) {
        error(peek(), "statement nesting too deep");
        auto stub = std::make_unique<PassStmt>();
        stub->setLocation(peek().location());
        synchronize();
        return stub;
    }
    while (match(TokenType::NEWLINE)) {}

    if (check(TokenType::AT)) {
        auto decorators = parseDecorators();
        if (check(TokenType::DEF) || check(TokenType::ASYNC)) {
            auto decl = functionDeclaration();
            if (decl) {
                if (auto* func = dynamic_cast<FunctionDecl*>(decl.get())) {
                    func->decorators = std::move(decorators);
                    if (func->isMethod) {
                        for (auto& dec : func->decorators) {
                            if (auto* n = dynamic_cast<NameExpr*>(dec.get())) {
                                if (n->name == "staticmethod") {
                                    func->isStatic = true;
                                    func->hasImplicitSelf = false;
                                } else if (n->name == "classmethod") {
                                    func->isClassMethod = true;
                                    func->isStatic = true;
                                    func->hasImplicitSelf = false;
                                } else if (n->name == "property") {
                                    func->isProperty = true;
                                }
                            } else if (auto* a = dynamic_cast<AttributeExpr*>(dec.get())) {
                                if (a->attribute == "setter") {
                                    if (auto* base = dynamic_cast<NameExpr*>(a->object.get())) {
                                        func->propertySetterFor = base->name;
                                        func->name = base->name + "__setter";
                                    }
                                }
                            }
                        }
                    }
                }
            }
            return decl;
        }
        if (check(TokenType::CLASS)) {
            auto decl = classDeclaration();
            if (decl) {
                if (auto* cls = dynamic_cast<ClassDecl*>(decl.get()))
                    cls->decorators = std::move(decorators);
            }
            return decl;
        }
        error("Decorators can only be applied to functions or classes");
        return nullptr;
    }

    if (check(TokenType::DEF) || check(TokenType::ASYNC)) return functionDeclaration();
    if (check(TokenType::CLASS)) return classDeclaration();
    if (check(TokenType::IF)) return ifStatement();
    if (check(TokenType::WHILE)) return whileStatement();
    if (check(TokenType::FOR)) return forStatement();
    if (check(TokenType::TRY)) return tryStatement();
    if (check(TokenType::WITH)) return withStatement();

    if (check(TokenType::IDENTIFIER) && current().lexeme() == "thread")
        return threadStatement();

    if (impl_->options.isDragonFile &&
        check(TokenType::IDENTIFIER) && current().lexeme() == "enum" &&
        peek().type() == TokenType::IDENTIFIER) {
        return enumDeclaration();
    }

    if (impl_->options.isDragonFile &&
        check(TokenType::IDENTIFIER) && current().lexeme() == "own" &&
        peekNext().type() == TokenType::IDENTIFIER) {
        return ownDeclaration();
    }

    if (impl_->options.isDragonFile &&
        check(TokenType::IDENTIFIER) && current().lexeme() == "defer" &&
        peekNext().type() == TokenType::IDENTIFIER) {
        return deferStatement();
    }

    if (impl_->options.isDragonFile && check(TokenType::EXTERN)) {
        return externDeclaration();
    }
    if (impl_->options.isDragonFile && check(TokenType::CONST)) {
        return constDeclaration();
    }
    if (impl_->options.isDragonFile && check(TokenType::STATIC)) {
        return staticDeclaration();
    }

    if (check(TokenType::IDENTIFIER) && peek().lexeme() == "match") {
        return matchStatement();
    }

    if (check(TokenType::IDENTIFIER) && peek().lexeme() == "type") {
        advance();
        auto typeLoc = previous().location();
        std::string typeName = std::string(
            consume(TokenType::IDENTIFIER, "Expect type name").lexeme());
        if (impl_->options.isDragonFile &&
            (check(TokenType::LEFT_BRACE) || check(TokenType::LEFT_PAREN))) {
            auto contract = contractDeclaration(std::move(typeName));
            contract->setLocation(typeLoc);
            return contract;
        }
        auto stmt = std::make_unique<TypeAliasStmt>();
        stmt->setLocation(typeLoc);
        stmt->name = std::move(typeName);
        consume(TokenType::EQUAL, "Expect '=' after type alias name");
        stmt->value = parseType();
        return stmt;
    }

    return simpleStatement();
}

std::unique_ptr<Stmt> Parser::simpleStatement() {
    if (match(TokenType::RETURN)) return returnStatement();
    if (match(TokenType::RAISE)) return raiseStatement();
    if (match(TokenType::BREAK)) return breakStatement();
    if (match(TokenType::CONTINUE)) return continueStatement();
    if (match(TokenType::PASS)) return passStatement();
    if (match(TokenType::ASSERT)) return assertStatement();
    if (match(TokenType::GLOBAL)) return globalStatement();
    if (match(TokenType::NONLOCAL)) return nonlocalStatement();
    if (match(TokenType::DEL)) return deleteStatement();
    if (match(TokenType::IMPORT)) return importStatement();
    if (match(TokenType::FROM)) return fromImportStatement();
    return expressionStatement();
}

std::unique_ptr<Stmt> Parser::compoundStatement() { return statement(); }

std::unique_ptr<Expr> Parser::maybeMoveRhs() {
    if (impl_->options.isDragonFile && check(TokenType::IDENTIFIER) &&
        current().lexeme() == "own" &&
        peekNext().type() == TokenType::IDENTIFIER) {
        advance();
        auto moved = std::make_unique<NameExpr>();
        moved->name = std::string(
            consume(TokenType::IDENTIFIER,
                    "Expect binding name after 'own'").lexeme());
        moved->setLocation(previous().location());
        moved->isMoveMarked = true;
        if (check(TokenType::DOT) || check(TokenType::LEFT_BRACKET))
            error("own moves a BINDING; a field or element cannot be moved "
                  "(its container owns it) - bind it first or dub it");
        return moved;
    }
    if (impl_->options.isDragonFile && check(TokenType::IDENTIFIER) &&
        current().lexeme() == "dub" &&
        peekNext().type() == TokenType::IDENTIFIER) {
        advance();
        auto dubbed = std::make_unique<NameExpr>();
        dubbed->name = std::string(
            consume(TokenType::IDENTIFIER,
                    "Expect binding name after 'dub'").lexeme());
        dubbed->setLocation(previous().location());
        dubbed->isDubMarked = true;
        return dubbed;
    }
    return expression();
}

std::unique_ptr<Stmt> Parser::expressionStatement() {
    SourceLocation stmtLoc = peek().location();
    std::unique_ptr<Expr> expr;
    if (check(TokenType::STAR) && !check(TokenType::STAR_EQUAL)) {
        size_t saved = impl_->current;
        advance();
        if (check(TokenType::IDENTIFIER)) {
            auto name = std::make_unique<NameExpr>();
            name->name = std::string(advance().lexeme());
            auto starred = std::make_unique<StarredExpr>();
            starred->value = std::move(name);
            expr = std::move(starred);
        } else {
            impl_->current = saved;
            expr = expression();
        }
    } else {
        expr = expression();
    }
    if (!expr) return nullptr;

    if (check(TokenType::COMMA) && !check(TokenType::NEWLINE)) {
        auto tuple = std::make_unique<TupleExpr>();
        tuple->elements.push_back(std::move(expr));
        while (match(TokenType::COMMA)) {
            if (check(TokenType::EQUAL) || check(TokenType::NEWLINE) ||
                check(TokenType::RIGHT_PAREN) || isAtEnd()) break;
            if (check(TokenType::STAR)) {
                advance();
                auto inner = primary();
                auto starred = std::make_unique<StarredExpr>();
                starred->value = std::move(inner);
                tuple->elements.push_back(std::move(starred));
            } else {
                tuple->elements.push_back(expression());
            }
        }
        if (match(TokenType::EQUAL)) {
            auto stmt = std::make_unique<AssignStmt>();
            stmt->setLocation(stmtLoc);
            stmt->targets.push_back(std::move(tuple));
            skipNewlines();
            auto rhsFirst = expression();
            if (check(TokenType::COMMA)) {
                auto rhsTuple = std::make_unique<TupleExpr>();
                rhsTuple->elements.push_back(std::move(rhsFirst));
                while (match(TokenType::COMMA)) {
                    if (check(TokenType::NEWLINE) || isAtEnd()) break;
                    rhsTuple->elements.push_back(expression());
                }
                stmt->value = std::move(rhsTuple);
            } else {
                stmt->value = std::move(rhsFirst);
            }
            return stmt;
        }
        if (tuple->elements.size() == 1) {
            auto exprStmt = std::make_unique<ExprStmt>();
            exprStmt->setLocation(stmtLoc);
            exprStmt->expr = std::move(tuple->elements[0]);
            return exprStmt;
        }
        auto exprStmt = std::make_unique<ExprStmt>();
        exprStmt->setLocation(stmtLoc);
        exprStmt->expr = std::move(tuple);
        return exprStmt;
    }

    if (match(TokenType::EQUAL)) {
        auto stmt = std::make_unique<AssignStmt>();
        stmt->setLocation(stmtLoc);
        stmt->targets.push_back(std::move(expr));
        skipNewlines();
        auto rhsFirst = maybeMoveRhs();
        if (check(TokenType::COMMA)) {
            auto rhsTuple = std::make_unique<TupleExpr>();
            rhsTuple->elements.push_back(std::move(rhsFirst));
            while (match(TokenType::COMMA)) {
                if (check(TokenType::NEWLINE) || isAtEnd()) break;
                rhsTuple->elements.push_back(expression());
            }
            stmt->value = std::move(rhsTuple);
        } else {
            stmt->value = std::move(rhsFirst);
        }
        return stmt;
    }

    if (match(TokenType::COLON)) {
        auto stmt = std::make_unique<AnnAssignStmt>();
        stmt->setLocation(stmtLoc);
        stmt->target = std::move(expr);
        stmt->annotation = parseType();
        if (match(TokenType::EQUAL)) {
            skipNewlines();
            stmt->value = maybeMoveRhs();
        }
        return stmt;
    }

    if (check(TokenType::PLUS_EQUAL) || check(TokenType::MINUS_EQUAL) ||
        check(TokenType::STAR_EQUAL) || check(TokenType::SLASH_EQUAL) ||
        check(TokenType::DOUBLE_SLASH_EQUAL) || check(TokenType::PERCENT_EQUAL) ||
        check(TokenType::POWER_EQUAL) || check(TokenType::AT_EQUAL) ||
        check(TokenType::AMPERSAND_EQUAL) || check(TokenType::PIPE_EQUAL) ||
        check(TokenType::CARET_EQUAL) || check(TokenType::LEFT_SHIFT_EQUAL) ||
        check(TokenType::RIGHT_SHIFT_EQUAL)) {
        auto op = advance();
        auto stmt = std::make_unique<AugAssignStmt>();
        stmt->setLocation(stmtLoc);
        stmt->target = std::move(expr);
        stmt->op = op;
        skipNewlines();
        stmt->value = expression();
        return stmt;
    }

    auto stmt = std::make_unique<ExprStmt>();
    stmt->setLocation(stmtLoc);
    stmt->expr = std::move(expr);
    return stmt;
}

std::unique_ptr<Stmt> Parser::assignmentStatement() { return nullptr; }

std::unique_ptr<Stmt> Parser::returnStatement() {
    auto stmt = std::make_unique<ReturnStmt>();
    stmt->setLocation(previous().location());
    if (!check(TokenType::NEWLINE) && !check(TokenType::RIGHT_BRACE) &&
        !check(TokenType::DEDENT) && !isAtEnd()) {
        stmt->value = expression();
    }
    return stmt;
}

std::unique_ptr<Stmt> Parser::raiseStatement() {
    auto stmt = std::make_unique<RaiseStmt>();
    if (!check(TokenType::NEWLINE) && !check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
        stmt->exception = expression();
        if (match(TokenType::FROM)) stmt->cause = expression();
    }
    return stmt;
}

std::unique_ptr<Stmt> Parser::breakStatement() { return std::make_unique<BreakStmt>(); }
std::unique_ptr<Stmt> Parser::continueStatement() { return std::make_unique<ContinueStmt>(); }
std::unique_ptr<Stmt> Parser::passStatement() { return std::make_unique<PassStmt>(); }

std::unique_ptr<Stmt> Parser::assertStatement() {
    auto stmt = std::make_unique<AssertStmt>();
    stmt->test = expression();
    if (match(TokenType::COMMA)) stmt->msg = expression();
    return stmt;
}

std::unique_ptr<Stmt> Parser::globalStatement() {
    auto stmt = std::make_unique<GlobalStmt>();
    do {
        stmt->names.push_back(std::string(consume(TokenType::IDENTIFIER, "Expect variable name").lexeme()));
    } while (match(TokenType::COMMA));
    return stmt;
}

std::unique_ptr<Stmt> Parser::nonlocalStatement() {
    auto stmt = std::make_unique<NonlocalStmt>();
    do {
        stmt->names.push_back(std::string(consume(TokenType::IDENTIFIER, "Expect variable name").lexeme()));
    } while (match(TokenType::COMMA));
    return stmt;
}

std::unique_ptr<Stmt> Parser::deferStatement() {
    Token kw = advance();
    auto stmt = std::make_unique<DeferStmt>();
    stmt->setLocation(kw.location());
    auto operand = expression();
    if (!dynamic_cast<CallExpr*>(operand.get())) {
        error(kw, "'defer' requires a direct call: a function call, method "
                  "call, or bound-closure call");
    }
    stmt->call = std::move(operand);
    return stmt;
}

std::unique_ptr<Stmt> Parser::deleteStatement() {
    auto stmt = std::make_unique<DeleteStmt>();
    stmt->setLocation(previous().location());
    do {
        stmt->targets.push_back(expression());
    } while (match(TokenType::COMMA));
    return stmt;
}

std::unique_ptr<Stmt> Parser::importStatement() {
    auto stmt = std::make_unique<ImportStmt>();
    do {
        ImportStmt::Alias alias;
        alias.name = std::string(consume(TokenType::IDENTIFIER, "Expect module name").lexeme());
        while (match(TokenType::DOT)) {
            alias.name += ".";
            alias.name += std::string(consume(TokenType::IDENTIFIER, "Expect module name").lexeme());
        }
        if (match(TokenType::AS)) {
            alias.asName = std::string(consume(TokenType::IDENTIFIER, "Expect alias").lexeme());
        }
        stmt->names.push_back(std::move(alias));
    } while (match(TokenType::COMMA));
    return stmt;
}

std::unique_ptr<Stmt> Parser::fromImportStatement() {
    auto stmt = std::make_unique<FromImportStmt>();
    while (match(TokenType::DOT)) stmt->level++;
    if (check(TokenType::IDENTIFIER)) {
        stmt->module = std::string(advance().lexeme());
        while (match(TokenType::DOT)) {
            stmt->module += ".";
            stmt->module += std::string(consume(TokenType::IDENTIFIER, "Expect module name").lexeme());
        }
    }
    consume(TokenType::IMPORT, "Expect 'import'");
    if (match(TokenType::STAR)) return stmt;
    bool parenthesized = match(TokenType::LEFT_PAREN);
    do {
        if (parenthesized && check(TokenType::RIGHT_PAREN)) break;
        ImportStmt::Alias alias;
        alias.name = std::string(consume(TokenType::IDENTIFIER, "Expect name").lexeme());
        if (match(TokenType::AS)) alias.asName = std::string(consume(TokenType::IDENTIFIER, "Expect alias").lexeme());
        stmt->names.push_back(std::move(alias));
    } while (match(TokenType::COMMA));
    if (parenthesized) consume(TokenType::RIGHT_PAREN, "Expect ')' after import list");
    return stmt;
}

std::unique_ptr<Stmt> Parser::ifStatement() {
    consume(TokenType::IF, "Expect 'if'");
    auto stmt = std::make_unique<IfStmt>();
    stmt->condition = expression();
    stmt->thenBody = parseBlock();
    auto skipNewlines = [&]() {
        while (match(TokenType::NEWLINE) || match(TokenType::SEMICOLON)) {}
    };
    while (true) {
        size_t saved = impl_->current;
        skipNewlines();
        if (match(TokenType::ELIF)) {
            auto cond = expression();
            auto body = parseBlock();
            stmt->elifClauses.emplace_back(std::move(cond), std::move(body));
            continue;
        }
        if (check(TokenType::ELSE) && peekNext().type() == TokenType::IF) {
            advance();
            advance();
            auto cond = expression();
            auto body = parseBlock();
            stmt->elifClauses.emplace_back(std::move(cond), std::move(body));
            continue;
        }
        impl_->current = saved;
        break;
    }
    size_t savedBeforeElse = impl_->current;
    while (match(TokenType::NEWLINE) || match(TokenType::SEMICOLON)) {}
    if (match(TokenType::ELSE)) {
        stmt->elseBody = parseBlock();
    } else {
        impl_->current = savedBeforeElse;
    }
    return stmt;
}

std::unique_ptr<Stmt> Parser::whileStatement() {
    consume(TokenType::WHILE, "Expect 'while'");
    auto stmt = std::make_unique<WhileStmt>();
    stmt->condition = expression();
    stmt->body = parseBlock();
    if (match(TokenType::ELSE)) stmt->elseBody = parseBlock();
    return stmt;
}

std::unique_ptr<Stmt> Parser::forStatement() {
    consume(TokenType::FOR, "Expect 'for'");
    auto stmt = std::make_unique<ForStmt>();
    auto target = primary();
    if (check(TokenType::COMMA)) {
        auto tuple = std::make_unique<TupleExpr>();
        tuple->elements.push_back(std::move(target));
        while (match(TokenType::COMMA)) {
            if (check(TokenType::IN)) break;
            tuple->elements.push_back(primary());
        }
        stmt->target = std::move(tuple);
    } else {
        stmt->target = std::move(target);
    }
    consume(TokenType::IN, "Expect 'in'");
    if (impl_->options.isDragonFile && check(TokenType::IDENTIFIER) &&
        current().lexeme() == "dub" &&
        peekNext().type() == TokenType::IDENTIFIER) {
        advance();
        auto dubbed = std::make_unique<NameExpr>();
        dubbed->name = std::string(
            consume(TokenType::IDENTIFIER,
                    "Expect binding name after 'dub'").lexeme());
        dubbed->setLocation(previous().location());
        dubbed->isDubMarked = true;
        stmt->iterable = std::move(dubbed);
    } else {
        stmt->iterable = expression();
    }
    stmt->body = parseBlock();
    if (match(TokenType::ELSE)) stmt->elseBody = parseBlock();
    return stmt;
}

std::unique_ptr<Stmt> Parser::tryStatement() {
    consume(TokenType::TRY, "Expect 'try'");
    auto stmt = std::make_unique<TryStmt>();
    stmt->tryBody = parseBlock();

    while (match(TokenType::EXCEPT) || match(TokenType::CATCH)) {
        TryStmt::ExceptHandler handler;
        if (match(TokenType::STAR)) {
            handler.isStar = true;
        }
        if (match(TokenType::LEFT_PAREN)) {
            if (check(TokenType::IDENTIFIER)) {
                std::string first = std::string(advance().lexeme());
                if (match(TokenType::COLON)) {
                    handler.name = first;
                    handler.type = parseType();
                } else {
                    auto t = std::make_unique<NamedTypeExpr>();
                    t->name = first;
                    handler.type = std::move(t);
                    while (match(TokenType::COMMA)) {
                        if (check(TokenType::IDENTIFIER))
                            handler.altTypeNames.push_back(std::string(advance().lexeme()));
                    }
                }
            }
            consume(TokenType::RIGHT_PAREN, "Expect ')'");
            if (match(TokenType::AS)) {
                handler.name = std::string(consume(TokenType::IDENTIFIER, "Expect name").lexeme());
            }
        } else if (check(TokenType::IDENTIFIER)) {
            auto t = std::make_unique<NamedTypeExpr>();
            t->name = std::string(advance().lexeme());
            handler.type = std::move(t);
            if (match(TokenType::AS)) {
                handler.name = std::string(consume(TokenType::IDENTIFIER, "Expect name").lexeme());
            }
        }
        handler.body = parseBlock();
        stmt->handlers.push_back(std::move(handler));
    }

    if (match(TokenType::ELSE)) stmt->elseBody = parseBlock();
    if (match(TokenType::FINALLY)) stmt->finallyBody = parseBlock();
    return stmt;
}

std::unique_ptr<Stmt> Parser::withStatement() {
    consume(TokenType::WITH, "Expect 'with'");
    auto stmt = std::make_unique<WithStmt>();
    do {
        WithStmt::WithItem item;
        item.contextExpr = expression();
        if (match(TokenType::AS)) item.optionalVars = expression();
        if (!item.optionalVars) {
            if (auto* cast = dynamic_cast<AsCastExpr*>(item.contextExpr.get())) {
                if (cast->contracts.size() == 1 && !cast->fromBracedSet) {
                    auto bind = std::make_unique<NameExpr>();
                    bind->name = cast->contracts[0];
                    bind->setLocation(cast->location());
                    item.optionalVars = std::move(bind);
                    item.contextExpr = std::move(cast->operand);
                }
            }
        }
        stmt->items.push_back(std::move(item));
    } while (match(TokenType::COMMA));
    stmt->body = parseBlock();
    return stmt;
}

std::unique_ptr<Stmt> Parser::threadStatement() {
    consume(TokenType::IDENTIFIER, "Expect 'thread'");
    auto stmt = std::make_unique<ThreadStmt>();
    stmt->setLocation(previous().location());
    stmt->body = parseBlock();
    return stmt;
}

std::unique_ptr<Stmt> Parser::constDeclaration() {
    consume(TokenType::CONST, "Expect 'const'");
    auto loc = previous().location();
    std::vector<std::unique_ptr<NameExpr>> names;
    std::vector<std::unique_ptr<TypeExpr>> annotations;
    do {
        auto name = std::make_unique<NameExpr>();
        name->name = std::string(consume(TokenType::IDENTIFIER, "Expect variable name after 'const'").lexeme());
        name->setLocation(previous().location());
        consume(TokenType::COLON, "Expect ':' after const variable name");
        annotations.push_back(parseType());
        names.push_back(std::move(name));
    } while (match(TokenType::COMMA));
    consume(TokenType::EQUAL, "const declaration must have an initializer");
    auto value = expression();

    if (names.size() == 1) {
        auto stmt = std::make_unique<AnnAssignStmt>();
        stmt->setLocation(loc);
        stmt->target = std::move(names[0]);
        stmt->annotation = std::move(annotations[0]);
        stmt->value = std::move(value);
        stmt->isConst = true;
        return stmt;
    }

    auto tup = std::make_unique<TupleExpr>();
    tup->setLocation(loc);
    for (auto& n : names) tup->elements.push_back(std::move(n));
    auto tupAnn = std::make_unique<TupleTypeExpr>();
    tupAnn->setLocation(loc);
    for (auto& a : annotations) tupAnn->elementTypes.push_back(std::move(a));

    auto stmt = std::make_unique<AssignStmt>();
    stmt->setLocation(loc);
    stmt->targets.push_back(std::move(tup));
    stmt->typeAnnotation = std::move(tupAnn);
    stmt->value = std::move(value);
    stmt->isConst = true;
    return stmt;
}

std::unique_ptr<Stmt> Parser::ownDeclaration() {
    auto loc = current().location();
    advance();
    auto name = std::make_unique<NameExpr>();
    name->name = std::string(
        consume(TokenType::IDENTIFIER, "Expect field name after 'own'").lexeme());
    name->setLocation(previous().location());
    consume(TokenType::COLON, "Expect ':' after own field name");
    auto annotation = parseType();
    std::unique_ptr<Expr> value;
    if (match(TokenType::EQUAL)) {
        value = expression();
    }
    auto stmt = std::make_unique<AnnAssignStmt>();
    stmt->setLocation(loc);
    stmt->target = std::move(name);
    stmt->annotation = std::move(annotation);
    stmt->value = std::move(value);
    stmt->isOwn = true;
    return stmt;
}

std::unique_ptr<Stmt> Parser::staticDeclaration() {
    consume(TokenType::STATIC, "Expect 'static'");
    auto loc = previous().location();

    if (check(TokenType::CONST)) {
        auto stmt = constDeclaration();
        if (auto* ann = dynamic_cast<AnnAssignStmt*>(stmt.get())) {
            ann->isStatic = true;
        } else {
            error("'static const' declares a single name; unpack is not supported here");
        }
        return stmt;
    }

    if (check(TokenType::DEF) || check(TokenType::ASYNC)) {
        auto decl = functionDeclaration();
        if (auto* func = dynamic_cast<FunctionDecl*>(decl.get())) {
            func->isStatic = true;
            func->hasImplicitSelf = false;
        }
        return decl;
    }

    if (check(TokenType::IDENTIFIER)) {
        auto name = std::make_unique<NameExpr>();
        name->name = std::string(consume(TokenType::IDENTIFIER, "Expect field name after 'static'").lexeme());
        name->setLocation(previous().location());
        consume(TokenType::COLON, "Expect ':' after static field name");
        auto annotation = parseType();
        std::unique_ptr<Expr> value;
        if (match(TokenType::EQUAL)) {
            value = expression();
        }
        auto stmt = std::make_unique<AnnAssignStmt>();
        stmt->setLocation(loc);
        stmt->target = std::move(name);
        stmt->annotation = std::move(annotation);
        stmt->value = std::move(value);
        stmt->isStatic = true;
        return stmt;
    }

    error("Expect field or method declaration after 'static'");
    return nullptr;
}

std::unique_ptr<Stmt> Parser::externDeclaration() {
    consume(TokenType::EXTERN, "Expect 'extern'");
    auto loc = previous().location();

    auto stripQuotes = [](const std::string& s) -> std::string {
        if (s.size() >= 2 && (s.front() == '"' || s.front() == '\''))
            return s.substr(1, s.size() - 2);
        return s;
    };
    if (!check(TokenType::STRING)) {
        error("Expect a language string after 'extern' (\"C\", \"python\", \"golang\", \"rust\")");
        return nullptr;
    }
    const std::string lang = stripQuotes(std::string(peek().lexeme()));
    if (lang == "go") {
        error("unknown extern language \"go\" - Dragon spells it \"golang\"");
        return nullptr;
    }
    if (lang != "C" && lang != "python" && lang != "golang" && lang != "rust") {
        error("unknown extern language \"" + lang +
              "\" (supported: \"C\", \"python\", \"golang\", \"rust\")");
        return nullptr;
    }
    advance();
    if (lang != "C") return parseProcessExternDef(lang);

    if (check(TokenType::FROM)) {
        advance();
        if (!check(TokenType::STRING)) {
            error("Expect library name string after 'from'");
            return nullptr;
        }
        std::string libName = stripQuotes(std::string(advance().lexeme()));
        consume(TokenType::LEFT_BRACE, "Expect '{' after library name");
        std::vector<std::unique_ptr<Stmt>> decls;
        while (!check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
            while (match(TokenType::NEWLINE) || match(TokenType::SEMICOLON)) {}
            if (check(TokenType::RIGHT_BRACE)) break;
            decls.push_back(parseExternFuncSig(libName));
            while (match(TokenType::NEWLINE) || match(TokenType::SEMICOLON)) {}
        }
        consume(TokenType::RIGHT_BRACE, "Expect '}' after extern block");
        if (decls.empty()) {
            error("Empty extern block");
            return nullptr;
        }
        for (size_t i = 1; i < decls.size(); i++) {
            impl_->pendingStmts.push_back(std::move(decls[i]));
        }
        return std::move(decls[0]);
    }

    return parseExternFuncSig("");
}

std::unique_ptr<Stmt> Parser::parseExternFuncSig(const std::string& libHint) {
    consume(TokenType::DEF, "Expect 'def' in extern declaration");
    auto loc = previous().location();

    auto decl = std::make_unique<FunctionDecl>();
    decl->setLocation(loc);
    decl->isExtern = true;
    decl->externLib = libHint;
    auto isCIdentLike = [](const std::string& s) -> bool {
        if (s.empty()) return false;
        if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_'))
            return false;
        for (char c : s) {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
                return false;
        }
        return true;
    };
    std::string cSymbol;
    bool nameIsKeyword = false;
    if (check(TokenType::IDENTIFIER)) {
        cSymbol = std::string(advance().lexeme());
    } else if (isCIdentLike(std::string(peek().lexeme()))) {
        nameIsKeyword = true;
        cSymbol = std::string(advance().lexeme());
    } else {
        error("Expect function name");
        return decl;
    }
    consume(TokenType::LEFT_PAREN, "Expect '(' after function name");
    decl->params = parseParameters();
    consume(TokenType::RIGHT_PAREN, "Expect ')' after parameters");

    if (match(TokenType::ARROW)) {
        decl->returnType = parseType();
    }
    if (match(TokenType::AS)) {
        std::string alias = std::string(consume(TokenType::IDENTIFIER, "Expect alias name").lexeme());
        decl->name = alias;
        decl->externSymbol = cSymbol;
    } else if (nameIsKeyword) {
        error("extern 'C' symbol '" + cSymbol + "' is a Dragon keyword; "
              "add `as <alias>` so Dragon code can call it");
        decl->name = cSymbol;
    } else {
        decl->name = cSymbol;
    }
    return decl;
}

namespace {

std::unique_ptr<NameExpr> ffiName(const std::string& n, SourceLocation loc) {
    auto e = std::make_unique<NameExpr>(); e->name = n; e->setLocation(loc); return e;
}
std::unique_ptr<Expr> ffiStr(const std::string& s, SourceLocation loc) {
    auto e = std::make_unique<StringLiteral>(); e->value = s; e->setLocation(loc); return e;
}
std::unique_ptr<Stmt> ffiCallStmt(const std::string& recv, const std::string& method,
                                  std::vector<std::unique_ptr<Expr>> args, SourceLocation loc) {
    auto attr = std::make_unique<AttributeExpr>();
    attr->object = ffiName(recv, loc);
    attr->attribute = method;
    attr->setLocation(loc);
    auto call = std::make_unique<CallExpr>();
    call->callee = std::move(attr);
    for (auto& a : args) call->args.push_back(std::move(a));
    call->setLocation(loc);
    auto s = std::make_unique<ExprStmt>();
    s->expr = std::move(call);
    s->setLocation(loc);
    return s;
}
std::unique_ptr<Expr> ffiTypeExprToExpr(const TypeExpr* t) {
    if (auto* nt = dynamic_cast<const NamedTypeExpr*>(t)) return ffiName(nt->name, t->location());
    if (auto* gt = dynamic_cast<const GenericTypeExpr*>(t)) {
        auto base = ffiTypeExprToExpr(gt->base.get());
        if (!base || gt->typeArgs.empty()) return nullptr;
        auto sub = std::make_unique<SubscriptExpr>();
        sub->object = std::move(base);
        if (gt->typeArgs.size() == 1) {
            sub->index = ffiTypeExprToExpr(gt->typeArgs[0].get());
            if (!sub->index) return nullptr;
        } else {
            auto tup = std::make_unique<TupleExpr>();
            for (auto& a : gt->typeArgs) {
                auto ae = ffiTypeExprToExpr(a.get());
                if (!ae) return nullptr;
                tup->elements.push_back(std::move(ae));
            }
            tup->setLocation(t->location());
            sub->index = std::move(tup);
        }
        sub->setLocation(t->location());
        return sub;
    }
    return nullptr;
}

}

std::unique_ptr<Stmt> Parser::parseProcessExternDef(const std::string& lang) {
    consume(TokenType::DEF, "Expect 'def' in extern declaration");
    auto loc = previous().location();

    auto decl = std::make_unique<FunctionDecl>();
    decl->setLocation(loc);
    decl->externLang = lang;
    if (!check(TokenType::IDENTIFIER)) {
        error("Expect function name");
        return decl;
    }
    decl->name = std::string(advance().lexeme());
    consume(TokenType::LEFT_PAREN, "Expect '(' after function name");
    decl->params = parseParameters();
    consume(TokenType::RIGHT_PAREN, "Expect ')' after parameters");
    if (!match(TokenType::ARROW)) {
        error("a process extern needs a return type - the child's output decodes into it");
        return decl;
    }
    decl->returnType = parseType();
    if (!match(TokenType::FROM)) {
        error(std::string("Expect `from \"<path>\"` naming the foreign ") +
              (lang == "python" ? "script" : "binary"));
        return decl;
    }
    if (!check(TokenType::STRING)) {
        error("Expect a path string after 'from'");
        return decl;
    }
    {
        std::string raw = std::string(advance().lexeme());
        if (raw.size() >= 2 && (raw.front() == '"' || raw.front() == '\''))
            raw = raw.substr(1, raw.size() - 2);
        decl->externPath = raw;
    }

    for (auto& p : decl->params) {
        if (p.isVarArg || p.isKwArg) {
            error("process externs take a fixed parameter list");
            return decl;
        }
    }
    auto isBytesType = [](const TypeExpr* t) -> bool {
        auto* nt = dynamic_cast<const NamedTypeExpr*>(t);
        return nt && nt->name == "bytes";
    };

    std::string resolvedPath = decl->externPath;
    {
        std::filesystem::path p(decl->externPath);
        if (p.is_relative())
            p = std::filesystem::path(impl_->options.filename).parent_path() / p;
        std::error_code ec;
        auto abs = std::filesystem::absolute(p, ec);
        if (!ec) resolvedPath = abs.lexically_normal().string();
    }

    auto freshName = [&](std::string seed) -> std::string {
        for (bool clash = true; clash;) {
            clash = false;
            for (auto& p : decl->params)
                if (p.name == seed) { seed += "_"; clash = true; break; }
        }
        return seed;
    };
    std::string wn = freshName("_w");
    std::string bn = freshName("_blobs");
    if (bn == wn) bn += "b";
    {
        auto ctorCall = std::make_unique<CallExpr>();
        ctorCall->callee = ffiName("JsonWriter", loc);
        ctorCall->setLocation(loc);
        auto annot = std::make_unique<NamedTypeExpr>();
        annot->name = "JsonWriter";
        annot->setLocation(loc);
        auto d = std::make_unique<AnnAssignStmt>();
        d->target = ffiName(wn, loc);
        d->annotation = std::move(annot);
        d->value = std::move(ctorCall);
        d->setLocation(loc);
        decl->body.push_back(std::move(d));
    }
    {
        std::vector<std::unique_ptr<Expr>> none;
        decl->body.push_back(ffiCallStmt(wn, "begin_object", std::move(none), loc));
    }
    std::vector<std::string> blobParams;
    for (auto& p : decl->params) {
        {
            std::vector<std::unique_ptr<Expr>> kargs;
            kargs.push_back(ffiStr(p.name, loc));
            decl->body.push_back(ffiCallStmt(wn, "key", std::move(kargs), loc));
        }
        if (isBytesType(p.type.get())) {
            std::vector<std::unique_ptr<Expr>> none0;
            decl->body.push_back(ffiCallStmt(wn, "begin_object", std::move(none0), loc));
            std::vector<std::unique_ptr<Expr>> bkey;
            bkey.push_back(ffiStr("$blob", loc));
            decl->body.push_back(ffiCallStmt(wn, "key", std::move(bkey), loc));
            auto idx = std::make_unique<IntegerLiteral>();
            idx->value = (int64_t)blobParams.size();
            idx->setLocation(loc);
            std::vector<std::unique_ptr<Expr>> ival;
            ival.push_back(std::move(idx));
            decl->body.push_back(ffiCallStmt(wn, "write_int", std::move(ival), loc));
            std::vector<std::unique_ptr<Expr>> none1;
            decl->body.push_back(ffiCallStmt(wn, "end_object", std::move(none1), loc));
            blobParams.push_back(p.name);
            continue;
        }
        auto typeArgE = ffiTypeExprToExpr(p.type.get());
        if (!typeArgE) {
            error("process extern param '" + p.name + "' has a type the process "
                  "lane cannot carry (scalars, lists, classes, bytes)");
            return decl;
        }
        auto encSub = std::make_unique<SubscriptExpr>();
        encSub->object = ffiName("encode", loc);
        encSub->index = std::move(typeArgE);
        encSub->setLocation(loc);
        auto encCall = std::make_unique<CallExpr>();
        encCall->callee = std::move(encSub);
        encCall->args.push_back(ffiName(p.name, loc));
        encCall->setLocation(loc);
        std::vector<std::unique_ptr<Expr>> wargs;
        wargs.push_back(std::move(encCall));
        decl->body.push_back(ffiCallStmt(wn, "write_raw", std::move(wargs), loc));
    }
    {
        std::vector<std::unique_ptr<Expr>> none;
        decl->body.push_back(ffiCallStmt(wn, "end_object", std::move(none), loc));
    }
    {
        auto lb = std::make_unique<ListExpr>();
        for (auto& n : blobParams) lb->elements.push_back(ffiName(n, loc));
        lb->setLocation(loc);
        auto lt = std::make_unique<GenericTypeExpr>();
        auto base = std::make_unique<NamedTypeExpr>();
        base->name = "list";
        base->setLocation(loc);
        auto elem = std::make_unique<NamedTypeExpr>();
        elem->name = "bytes";
        elem->setLocation(loc);
        lt->base = std::move(base);
        lt->typeArgs.push_back(std::move(elem));
        lt->setLocation(loc);
        auto d = std::make_unique<AnnAssignStmt>();
        d->target = ffiName(bn, loc);
        d->annotation = std::move(lt);
        d->value = std::move(lb);
        d->setLocation(loc);
        decl->body.push_back(std::move(d));
    }

    auto argvList = std::make_unique<ListExpr>();
    if (lang == "python") {
        auto py = std::make_unique<CallExpr>();
        py->callee = ffiName("python3", loc);
        py->setLocation(loc);
        argvList->elements.push_back(std::move(py));
    }
    argvList->elements.push_back(ffiStr(resolvedPath, loc));
    argvList->setLocation(loc);

    auto mkFinish = [&]() -> std::unique_ptr<Expr> {
        auto attr = std::make_unique<AttributeExpr>();
        attr->object = ffiName(wn, loc);
        attr->attribute = "finish";
        attr->setLocation(loc);
        auto fin = std::make_unique<CallExpr>();
        fin->callee = std::move(attr);
        fin->setLocation(loc);
        return fin;
    };
    auto scCall = std::make_unique<CallExpr>();
    if (isBytesType(decl->returnType.get())) {
        scCall->callee = ffiName("sidecar_call_bytes", loc);
    } else {
        auto retTypeE = ffiTypeExprToExpr(decl->returnType.get());
        if (!retTypeE) {
            error("process extern return type is not carryable (a class, list, scalar, or bytes)");
            return decl;
        }
        auto scSub = std::make_unique<SubscriptExpr>();
        scSub->object = ffiName("sidecar_call", loc);
        scSub->index = std::move(retTypeE);
        scSub->setLocation(loc);
        scCall->callee = std::move(scSub);
    }
    scCall->args.push_back(std::move(argvList));
    scCall->args.push_back(mkFinish());
    scCall->args.push_back(ffiName(bn, loc));
    scCall->setLocation(loc);
    auto ret = std::make_unique<ReturnStmt>();
    ret->value = std::move(scCall);
    ret->setLocation(loc);
    decl->body.push_back(std::move(ret));

    if (!impl_->ffiProcessImportsInjected) {
        impl_->ffiProcessImportsInjected = true;
        auto mkFrom = [&](const std::string& mod,
                          std::vector<std::string> names) -> std::unique_ptr<Stmt> {
            auto fi = std::make_unique<FromImportStmt>();
            fi->module = mod;
            for (auto& n : names) fi->names.push_back({n, ""});
            fi->setLocation(loc);
            return fi;
        };
        auto first = mkFrom("ffi", {"sidecar_call", "sidecar_call_bytes", "python3"});
        impl_->pendingStmts.push_back(mkFrom("json", {"encode", "JsonWriter"}));
        impl_->pendingStmts.push_back(std::move(decl));
        return first;
    }
    return decl;
}

std::unique_ptr<Stmt> Parser::matchStatement() {
    advance();
    auto stmt = std::make_unique<MatchStmt>();
    stmt->setLocation(previous().location());
    stmt->subject = expression();

    if (impl_->options.isDragonFile) {
        consume(TokenType::LEFT_BRACE, "Expect '{' after match subject");
        while (!check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
            size_t caseLoopStart = impl_->current;
            while (match(TokenType::NEWLINE) || match(TokenType::SEMICOLON)) {}
            if (check(TokenType::RIGHT_BRACE)) break;
            if (!check(TokenType::IDENTIFIER) || peek().lexeme() != "case") {
                error("Expect 'case' in match block");
                break;
            }
            advance();
            MatchStmt::MatchCase matchCase;
            matchCase.pattern = parsePattern();
            if (check(TokenType::IF)) {
                advance();
                matchCase.guard = expression();
            }
            if (!check(TokenType::LEFT_BRACE)) {
                error("Expect '{' after case pattern");
                synchronize();
                break;
            }
            advance();
            while (!check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
                while (match(TokenType::NEWLINE) || match(TokenType::SEMICOLON)) {}
                if (check(TokenType::RIGHT_BRACE)) break;
                auto s = statement();
                if (s) matchCase.body.push_back(std::move(s));
                else if (!isAtEnd()) advance();
            }
            consume(TokenType::RIGHT_BRACE, "Expect '}' after case body");
            stmt->cases.push_back(std::move(matchCase));
            if (impl_->current == caseLoopStart) {
                error("malformed case in match block");
                break;
            }
        }
        consume(TokenType::RIGHT_BRACE, "Expect '}' after match block");
    } else {
        consume(TokenType::COLON, "Expect ':' after match subject");
        match(TokenType::NEWLINE);
        consume(TokenType::INDENT, "Expect indented block after match");
        while (!check(TokenType::DEDENT) && !isAtEnd()) {
            size_t caseLoopStart = impl_->current;
            while (match(TokenType::NEWLINE) || match(TokenType::SEMICOLON)) {}
            if (check(TokenType::DEDENT)) break;
            if (!check(TokenType::IDENTIFIER) || peek().lexeme() != "case") {
                error("Expect 'case' in match block");
                break;
            }
            advance();
            MatchStmt::MatchCase matchCase;
            matchCase.pattern = parsePattern();
            if (check(TokenType::IF)) {
                advance();
                matchCase.guard = expression();
            }
            matchCase.body = parseBlock();
            stmt->cases.push_back(std::move(matchCase));
            if (impl_->current == caseLoopStart) {
                error("malformed case in match block");
                break;
            }
        }
        match(TokenType::DEDENT);
    }
    return stmt;
}

MatchPattern Parser::parsePattern(bool allowCommaOr) {
    ParserRecursionGuard guard(impl_->recursionDepth);
    if (impl_->recursionDepth > Impl::kMaxRecursionDepth) {
        error(peek(), "pattern nesting too deep");
        synchronize();
        MatchPattern tooDeep;
        tooDeep.kind = MatchPattern::Kind::Wildcard;
        return tooDeep;
    }
    auto parseClassPattern = [&](const std::string& className) -> MatchPattern {
        advance();
        MatchPattern p;
        p.kind = MatchPattern::Kind::Class;
        p.name = className;
        if (!check(TokenType::RIGHT_PAREN)) {
            do {
                if (check(TokenType::RIGHT_PAREN)) break;
                MatchPattern sub = parsePattern(false);
                if (check(TokenType::EQUAL)) {
                    error("keyword class patterns (e.g. `Point(x=0)`) are not "
                          "yet supported; use `case TypeName()` or a guard");
                    advance();
                    parsePattern(false);
                } else {
                    p.subPatterns.push_back(std::move(sub));
                }
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RIGHT_PAREN, "Expect ')' after class pattern");
        return p;
    };

    auto parsePrimaryPattern = [&]() -> MatchPattern {
        if (check(TokenType::IDENTIFIER) && peek().lexeme() == "_") {
            advance();
            MatchPattern p;
            p.kind = MatchPattern::Kind::Wildcard;
            return p;
        }
        if (check(TokenType::LEFT_BRACKET)) {
            advance();
            MatchPattern p;
            p.kind = MatchPattern::Kind::Sequence;
            if (!check(TokenType::RIGHT_BRACKET)) {
                p.subPatterns.push_back(parsePattern(false));
                while (match(TokenType::COMMA)) {
                    if (check(TokenType::RIGHT_BRACKET)) break;
                    p.subPatterns.push_back(parsePattern(false));
                }
            }
            consume(TokenType::RIGHT_BRACKET, "Expect ']' after sequence pattern");
            return p;
        }
        if (check(TokenType::LEFT_PAREN)) {
            advance();
            MatchPattern p;
            p.kind = MatchPattern::Kind::Sequence;
            if (!check(TokenType::RIGHT_PAREN)) {
                p.subPatterns.push_back(parsePattern(false));
                while (match(TokenType::COMMA)) {
                    if (check(TokenType::RIGHT_PAREN)) break;
                    p.subPatterns.push_back(parsePattern(false));
                }
            }
            consume(TokenType::RIGHT_PAREN, "Expect ')' after sequence pattern");
            return p;
        }
        if (check(TokenType::INTEGER) || check(TokenType::FLOAT)) {
            auto lit = primary();
            MatchPattern p;
            p.kind = MatchPattern::Kind::Literal;
            p.literal = std::move(lit);
            return p;
        }
        if (check(TokenType::STRING)) {
            auto lit = primary();
            MatchPattern p;
            p.kind = MatchPattern::Kind::Literal;
            p.literal = std::move(lit);
            return p;
        }
        if (check(TokenType::TRUE) || check(TokenType::FALSE) || check(TokenType::NONE)) {
            auto lit = primary();
            MatchPattern p;
            p.kind = MatchPattern::Kind::Literal;
            p.literal = std::move(lit);
            return p;
        }
        if (check(TokenType::MINUS)) {
            auto unary = expression();
            MatchPattern p;
            p.kind = MatchPattern::Kind::Literal;
            p.literal = std::move(unary);
            return p;
        }
        if (check(TokenType::IDENTIFIER)) {
            std::string name = std::string(advance().lexeme());
            if (check(TokenType::DOT)) {
                auto nameExpr = std::make_unique<NameExpr>();
                nameExpr->name = name;
                nameExpr->setLocation(previous().location());
                std::unique_ptr<Expr> cur = std::move(nameExpr);
                std::string full = name;
                while (match(TokenType::DOT)) {
                    std::string attrName = std::string(
                        consume(TokenType::IDENTIFIER, "Expect attribute name").lexeme());
                    auto attr = std::make_unique<AttributeExpr>();
                    attr->object = std::move(cur);
                    attr->attribute = attrName;
                    attr->setLocation(previous().location());
                    cur = std::move(attr);
                    full += "." + attrName;
                }
                if (check(TokenType::LEFT_PAREN)) return parseClassPattern(full);
                MatchPattern p;
                p.kind = MatchPattern::Kind::Value;
                p.literal = std::move(cur);
                return p;
            }
            if (check(TokenType::LEFT_PAREN)) return parseClassPattern(name);
            MatchPattern p;
            p.kind = MatchPattern::Kind::Capture;
            p.name = name;
            return p;
        }
        error("Expect pattern");
        MatchPattern p;
        p.kind = MatchPattern::Kind::Wildcard;
        return p;
    };

    auto first = parsePrimaryPattern();

    auto isOrSep = [&]() -> bool {
        if (check(TokenType::PIPE)) return true;
        if (allowCommaOr && impl_->options.isDragonFile && check(TokenType::COMMA)) return true;
        return false;
    };
    if (isOrSep()) {
        MatchPattern orPat;
        orPat.kind = MatchPattern::Kind::Or;
        orPat.subPatterns.push_back(std::move(first));
        while (match(TokenType::PIPE) || (allowCommaOr && impl_->options.isDragonFile && match(TokenType::COMMA))) {
            orPat.subPatterns.push_back(parsePrimaryPattern());
        }
        return orPat;
    }

    return first;
}

std::unique_ptr<Stmt> Parser::functionDeclaration() {
    auto decl = std::make_unique<FunctionDecl>();
    decl->isAsync = match(TokenType::ASYNC);
    consume(TokenType::DEF, "Expect 'def'");
    decl->setLocation(previous().location());

    if (impl_->inClassBody && impl_->options.isDragonFile &&
        check(TokenType::LEFT_PAREN)) {
        decl->name = "__init__";
        decl->isConstructor = true;
        decl->hasImplicitSelf = true;
        decl->isMethod = true;
    } else {
        decl->name = std::string(consume(TokenType::IDENTIFIER, "Expect function name").lexeme());
    }
    decl->typeParams = parseTypeParams();
    consume(TokenType::LEFT_PAREN, "Expect '('");
    decl->params = parseParameters();
    consume(TokenType::RIGHT_PAREN, "Expect ')'");

    {
        std::vector<Parameter> cleaned;
        int realIdx = 0;
        for (auto& p : decl->params) {
            if (p.name == "/") {
                decl->posOnlyEnd = realIdx;
                continue;
            }
            if (p.isVarArg && p.name.empty()) {
                decl->kwOnlyStart = realIdx;
                continue;
            }
            cleaned.push_back(std::move(p));
            realIdx++;
        }
        decl->params = std::move(cleaned);
    }

    if (match(TokenType::ARROW)) {
        decl->returnType = parseType();
    }

    if (impl_->inClassBody) {
        decl->isMethod = true;
        if (impl_->options.isDragonFile) {
            decl->hasImplicitSelf = true;
            if (!decl->params.empty() && decl->params[0].name == "self") {
                std::string paramHint;
                for (size_t i = 1; i < decl->params.size(); ++i) {
                    if (!paramHint.empty()) paramHint += ", ";
                    paramHint += decl->params[i].name;
                    if (decl->params[i].type) paramHint += ": ...";
                }
                impl_->diagnostics.push_back({
                    ParserDiagnostic::Level::Error,
                    decl->location(),
                    "'self' is implicit in Dragon methods. Remove it from the parameter list.\n"
                    "  Write: def " + decl->name + "(" + paramHint + ") -> ...\n"
                    "  For explicit self, use a .py file instead."
                });
            }
        }
    }

    bool savedInClassForBody = impl_->inClassBody;
    impl_->inClassBody = false;
    decl->body = parseBlock();
    impl_->inClassBody = savedInClassForBody;
    decl->docstring = extractDocstring(decl->body);
    return decl;
}

std::unique_ptr<Stmt> Parser::classDeclaration() {
    consume(TokenType::CLASS, "Expect 'class'");
    auto decl = std::make_unique<ClassDecl>();
    decl->setLocation(previous().location());
    decl->name = std::string(consume(TokenType::IDENTIFIER, "Expect class name").lexeme());
    decl->typeParams = parseTypeParams();
    if (match(TokenType::LEFT_PAREN)) {
        if (!check(TokenType::RIGHT_PAREN)) {
            do { decl->bases.push_back(expression()); } while (match(TokenType::COMMA));
        }
        consume(TokenType::RIGHT_PAREN, "Expect ')'");
    }
    if (impl_->options.isDragonFile && match(TokenType::ARROW)) {
        do {
            decl->promises.push_back(std::string(consume(
                TokenType::IDENTIFIER,
                "Expect contract name after '->'").lexeme()));
        } while (match(TokenType::COMMA));
    }
    bool savedInClass = impl_->inClassBody;
    impl_->inClassBody = true;
    decl->body = parseBlock();
    impl_->inClassBody = savedInClass;
    decl->docstring = extractDocstring(decl->body);

    int ctorIdx = 0;
    for (auto& stmt : decl->body) {
        if (auto* fd = dynamic_cast<FunctionDecl*>(stmt.get())) {
            if (fd->name == "__init__") {
                fd->constructorIndex = ctorIdx++;
            }
        }
    }

    return decl;
}

std::unique_ptr<Stmt> Parser::contractDeclaration(std::string name) {
    auto decl = std::make_unique<ContractDecl>();
    decl->name = std::move(name);
    if (match(TokenType::LEFT_PAREN)) {
        if (!check(TokenType::RIGHT_PAREN)) {
            do {
                decl->bases.push_back(std::string(consume(
                    TokenType::IDENTIFIER,
                    "Expect contract name in composition list").lexeme()));
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RIGHT_PAREN, "Expect ')' after composition list");
    }
    consume(TokenType::LEFT_BRACE, "Expect '{' to open the contract body");
    while (!check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
        while (match(TokenType::NEWLINE) || match(TokenType::SEMICOLON)) {}
        if (check(TokenType::RIGHT_BRACE)) break;
        if (!check(TokenType::DEF)) {
            error(peek(), "a contract body contains only 'def' method "
                  "signatures - no fields, statements, or values");
            synchronize();
            continue;
        }
        advance();
        auto method = std::make_unique<FunctionDecl>();
        method->setLocation(previous().location());
        method->name = std::string(consume(
            TokenType::IDENTIFIER, "Expect method name after 'def'").lexeme());
        consume(TokenType::LEFT_PAREN, "Expect '(' after method name");
        method->params = parseParameters();
        consume(TokenType::RIGHT_PAREN, "Expect ')' after parameters");
        if (match(TokenType::ARROW)) method->returnType = parseType();
        method->isMethod = true;
        method->hasImplicitSelf = true;
        if (check(TokenType::LEFT_BRACE)) {
            error(peek(), "contract methods declare signatures only - the "
                  "implementation lives in the conforming class");
            synchronize();
        }
        for (auto& p : method->params) {
            if (p.defaultValue) {
                error("contract method parameters take no default values");
                p.defaultValue = nullptr;
            }
        }
        decl->methods.push_back(std::move(method));
    }
    consume(TokenType::RIGHT_BRACE, "Expect '}' to close the contract body");
    if (decl->methods.empty() && decl->bases.empty()) {
        error(previous(), "a contract must declare at least one method "
              "signature (an empty contract constrains nothing)");
    }
    return decl;
}

std::unique_ptr<Stmt> Parser::enumDeclaration() {
    Token enumTok = current();
    advance();
    auto decl = std::make_unique<ClassDecl>();
    decl->setLocation(enumTok.location());
    decl->name = std::string(consume(TokenType::IDENTIFIER, "Expect enum name").lexeme());

    consume(TokenType::LEFT_BRACE, "Expect '{' after enum name");
    while (match(TokenType::NEWLINE)) {}

    int64_t nextValue = 0;
    while (!check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
        Token memberTok = consume(TokenType::IDENTIFIER, "Expect enum member name");
        int64_t memberValue;
        if (match(TokenType::EQUAL)) {
            std::unique_ptr<Expr> v = expression();
            if (auto* lit = dynamic_cast<IntegerLiteral*>(v.get())) {
                memberValue = lit->value;
            } else if (auto* un = dynamic_cast<UnaryExpr*>(v.get())) {
                if (un->op.type() == TokenType::MINUS) {
                    if (auto* lit2 = dynamic_cast<IntegerLiteral*>(un->operand.get())) {
                        memberValue = -lit2->value;
                    } else {
                        error("Enum member values must be integer literals");
                        memberValue = nextValue;
                    }
                } else {
                    error("Enum member values must be integer literals");
                    memberValue = nextValue;
                }
            } else {
                error("Enum member values must be integer literals");
                memberValue = nextValue;
            }
            nextValue = memberValue + 1;
        } else {
            memberValue = nextValue++;
        }

        auto name = std::make_unique<NameExpr>();
        name->setLocation(memberTok.location());
        name->name = std::string(memberTok.lexeme());

        auto annotation = std::make_unique<NamedTypeExpr>();
        annotation->setLocation(memberTok.location());
        annotation->name = "int";

        auto value = std::make_unique<IntegerLiteral>();
        value->setLocation(memberTok.location());
        value->value = memberValue;

        auto field = std::make_unique<AnnAssignStmt>();
        field->setLocation(memberTok.location());
        field->target = std::move(name);
        field->annotation = std::move(annotation);
        field->value = std::move(value);
        field->isStatic = true;

        decl->body.push_back(std::move(field));

        match(TokenType::COMMA);
        while (match(TokenType::NEWLINE)) {}
    }
    consume(TokenType::RIGHT_BRACE, "Expect '}' to close enum");
    return decl;
}

std::vector<std::unique_ptr<Expr>> Parser::parseDecorators() {
    std::vector<std::unique_ptr<Expr>> decorators;
    while (match(TokenType::AT)) {
        decorators.push_back(expression());
        match(TokenType::NEWLINE);
    }
    return decorators;
}

std::unique_ptr<TypeExpr> Parser::parseType() {
    ParserRecursionGuard guard(impl_->recursionDepth);
    if (impl_->recursionDepth > Impl::kMaxRecursionDepth) {
        error(peek(), "type nesting too deep");
        synchronize();
        return nullptr;
    }
    return parseUnionType();
}

std::unique_ptr<TypeExpr> Parser::parseUnionType() {
    auto type = parsePrimaryType();
    if (!type) return nullptr;
    if (check(TokenType::PIPE)) {
        auto u = std::make_unique<UnionTypeExpr>();
        u->setLocation(type->location());
        u->types.push_back(std::move(type));
        while (match(TokenType::PIPE)) {
            auto next = parsePrimaryType();
            if (next) u->types.push_back(std::move(next));
        }
        return u;
    }
    return type;
}

std::unique_ptr<TypeExpr> Parser::parsePrimaryType() {
    if (impl_->options.isDragonFile && check(TokenType::LEFT_BRACE)) {
        advance();
        auto cs = std::make_unique<ContractSetTypeExpr>();
        cs->setLocation(previous().location());
        do {
            cs->names.push_back(std::string(consume(
                TokenType::IDENTIFIER,
                "Expect contract name in contract set").lexeme()));
        } while (match(TokenType::COMMA));
        consume(TokenType::RIGHT_BRACE, "Expect '}' after contract set");
        return cs;
    }
    if (match(TokenType::NONE)) {
        auto t = std::make_unique<NamedTypeExpr>();
        t->name = "None";
        t->setLocation(previous().location());
        return t;
    }
    if (match(TokenType::IDENTIFIER)) {
        auto t = std::make_unique<NamedTypeExpr>();
        t->setLocation(previous().location());
        t->name = std::string(previous().lexeme());
        while (check(TokenType::DOT) && peekNext().type() == TokenType::IDENTIFIER) {
            advance();
            advance();
            t->name += ".";
            t->name += std::string(previous().lexeme());
        }
        if (check(TokenType::LEFT_BRACKET)) return parseGenericType(std::move(t));
        return t;
    }
    return nullptr;
}

std::unique_ptr<TypeExpr> Parser::parseGenericType(std::unique_ptr<TypeExpr> base) {
    SourceLocation startLoc = base ? base->location() : peek().location();
    consume(TokenType::LEFT_BRACKET, "Expect '['");
    if (check(TokenType::LEFT_BRACKET)) {
        advance();
        auto callable = std::make_unique<CallableTypeExpr>();
        callable->setLocation(startLoc);
        if (!check(TokenType::RIGHT_BRACKET)) {
            do { callable->paramTypes.push_back(parseType()); } while (match(TokenType::COMMA));
        }
        consume(TokenType::RIGHT_BRACKET, "Expect ']'");
        consume(TokenType::COMMA, "Expect ','");
        callable->returnType = parseType();
        consume(TokenType::RIGHT_BRACKET, "Expect ']'");
        return callable;
    }
    auto generic = std::make_unique<GenericTypeExpr>();
    generic->setLocation(startLoc);
    generic->base = std::move(base);
    if (!check(TokenType::RIGHT_BRACKET)) {
        do {
            auto arg = parseType();
            if (arg) generic->typeArgs.push_back(std::move(arg));
        } while (match(TokenType::COMMA));
    }
    consume(TokenType::RIGHT_BRACKET, "Expect ']'");

    if (auto* gb = dynamic_cast<NamedTypeExpr*>(generic->base.get())) {
        if (gb->name == "Union" && !generic->typeArgs.empty()) {
            auto u = std::make_unique<UnionTypeExpr>();
            u->setLocation(startLoc);
            u->types = std::move(generic->typeArgs);
            return u;
        }
        if (gb->name == "Optional" && generic->typeArgs.size() == 1) {
            auto u = std::make_unique<UnionTypeExpr>();
            u->setLocation(startLoc);
            u->types.push_back(std::move(generic->typeArgs[0]));
            auto none = std::make_unique<NamedTypeExpr>();
            none->name = "None";
            u->types.push_back(std::move(none));
            return u;
        }
    }
    return generic;
}

std::vector<TypeParam> Parser::parseTypeParams() {
    std::vector<TypeParam> params;
    if (!match(TokenType::LEFT_BRACKET)) return params;
    if (check(TokenType::RIGHT_BRACKET)) {
        error("Expect at least one type parameter inside '[]'");
        advance();
        return params;
    }
    do {
        TypeParam tp;
        tp.name = std::string(consume(TokenType::IDENTIFIER,
                                      "Expect type-parameter name").lexeme());
        if (match(TokenType::COLON)) {
            tp.bound = parseType();
        }
        params.push_back(std::move(tp));
    } while (match(TokenType::COMMA));
    consume(TokenType::RIGHT_BRACKET, "Expect ']' after type parameters");
    return params;
}

std::vector<std::unique_ptr<Stmt>> Parser::parseBlock() {
    std::vector<std::unique_ptr<Stmt>> stmts;
    if (impl_->options.isDragonFile) {
        consume(TokenType::LEFT_BRACE, "Expect '{' before block");
        while (!check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
            while (match(TokenType::NEWLINE) || match(TokenType::SEMICOLON)) {}
            if (check(TokenType::RIGHT_BRACE)) break;
            auto stmt = statement();
            if (stmt) stmts.push_back(std::move(stmt));
            else if (!isAtEnd()) advance();
        }
        consume(TokenType::RIGHT_BRACE, "Expect '}' after block");
    } else {
        match(TokenType::COLON);
        match(TokenType::NEWLINE);
        if (match(TokenType::INDENT)) {
            while (!check(TokenType::DEDENT) && !isAtEnd()) {
                while (match(TokenType::NEWLINE) || match(TokenType::SEMICOLON)) {}
                if (check(TokenType::DEDENT)) break;
                auto stmt = statement();
                if (stmt) stmts.push_back(std::move(stmt));
                else if (!isAtEnd()) advance();
            }
            match(TokenType::DEDENT);
        } else {
            auto stmt = simpleStatement();
            if (stmt) stmts.push_back(std::move(stmt));
        }
    }
    return stmts;
}

std::vector<Parameter> Parser::parseParameters() {
    std::vector<Parameter> params;
    if (check(TokenType::RIGHT_PAREN)) return params;
    do {
        if (match(TokenType::SLASH)) {
            Parameter sep;
            sep.name = "/";
            params.push_back(std::move(sep));
            continue;
        }
        Parameter param;
        if (match(TokenType::STAR)) {
            if (check(TokenType::COMMA) || check(TokenType::RIGHT_PAREN)) {
                param.isVarArg = true;
                param.name = "";
            } else {
                param.isVarArg = true;
                if (check(TokenType::IDENTIFIER)) param.name = std::string(advance().lexeme());
            }
        } else if (match(TokenType::POWER)) {
            param.isKwArg = true;
            param.name = std::string(consume(TokenType::IDENTIFIER, "Expect parameter name").lexeme());
        } else {
            if (check(TokenType::IDENTIFIER) && current().lexeme() == "own" &&
                peekNext().type() == TokenType::IDENTIFIER) {
                advance();
                param.isOwn = true;
            }
            param.name = std::string(consume(TokenType::IDENTIFIER, "Expect parameter name").lexeme());
        }
        if (match(TokenType::COLON)) param.type = parseType();
        if (match(TokenType::EQUAL)) param.defaultValue = expression();
        if ((param.isVarArg || param.isKwArg) && !param.name.empty() && !param.type) {
            std::string sig = param.isKwArg ? "**" : "*";
            error("'" + sig + param.name + "' requires a type annotation: use '" +
                  sig + param.name + ": Any' for heterogeneous arguments, or a "
                  "concrete element type such as '" + sig + param.name + ": int'");
        }
        params.push_back(std::move(param));
    } while (match(TokenType::COMMA));
    return params;
}

void Parser::error(const std::string& message) {
    error(peek(), message);
}

void Parser::error(const Token& token, const std::string& message) {
    impl_->diagnostics.push_back({
        ParserDiagnostic::Level::Error,
        token.location(),
        message
    });
}

void Parser::synchronize() {
    advance();
    while (!isAtEnd()) {
        if (previous().type() == TokenType::NEWLINE) return;
        switch (peek().type()) {
            case TokenType::CLASS: case TokenType::DEF: case TokenType::FOR:
            case TokenType::IF: case TokenType::WHILE: case TokenType::RETURN:
                return;
            default: break;
        }
        advance();
    }
}

bool Parser::isAtStatementBoundary() const {
    return check(TokenType::NEWLINE) || check(TokenType::END_OF_FILE);
}

bool Parser::isAtBlockEnd() const {
    return check(TokenType::RIGHT_BRACE) || check(TokenType::DEDENT);
}

}
