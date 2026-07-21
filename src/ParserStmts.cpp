/// Dragon Parser - statement & declaration parsing
/// Split from Parser.cpp (file-size policy): pure code motion. Shared
/// pimpl state + literal/docstring helpers live in ParserImpl.h.
#include "dragon/Parser.h"
#include "ParserImpl.h"
#include <cctype>
#include <charconv>
#include <stdexcept>
#include <string>

namespace dragon {

std::unique_ptr<Stmt> Parser::statement() {
    ParserRecursionGuard guard(impl_->recursionDepth);
    if (impl_->recursionDepth > Impl::kMaxRecursionDepth) {
        // Deeply nested compound statements (if/if/if/...) would otherwise
        // blow the C stack. Return PassStmt placeholder and synchronize.
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
                    // Wire @staticmethod / @classmethod / @property / @<name>.setter on class methods
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
                                        // Python convention names getter and setter identically.
                                        // Mangle the setter so it occupies a distinct vtable slot.
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

    // Contextual keyword: "thread" is only a keyword at statement start
    if (check(TokenType::IDENTIFIER) && current().lexeme() == "thread")
        return threadStatement();

    // Contextual keyword: "enum" is only a keyword at statement start in
    // .dr mode. Lowered at parse time to a ClassDecl with static int fields.
    if (impl_->options.isDragonFile &&
        check(TokenType::IDENTIFIER) && current().lexeme() == "enum" &&
        peek().type() == TokenType::IDENTIFIER) {
        return enumDeclaration();
    }

    // Contextual keyword: `own f: T` sole-owner field marker (docs/001-memory).
    // Two consecutive identifiers cannot start any other statement (`own = 5`,
    // `own(x)`, `own.f` all differ at the second token), so one-token
    // lookahead disambiguates and code using `own` as a name keeps compiling.
    if (impl_->options.isDragonFile &&
        check(TokenType::IDENTIFIER) && current().lexeme() == "own" &&
        peekNext().type() == TokenType::IDENTIFIER) {
        return ownDeclaration();
    }

    // Contextual keyword: `defer f(...)` scope-exit call (the other half of
    // the fire coin). Two consecutive identifiers cannot start any other
    // statement, so one-token lookahead disambiguates and code using `defer`
    // as a name (the http router's defer method) keeps compiling.
    if (impl_->options.isDragonFile &&
        check(TokenType::IDENTIFIER) && current().lexeme() == "defer" &&
        peekNext().type() == TokenType::IDENTIFIER) {
        return deferStatement();
    }

    // Dragon-specific keywords (.dr mode only)
    if (impl_->options.isDragonFile && check(TokenType::EXTERN)) {
        return externDeclaration();
    }
    if (impl_->options.isDragonFile && check(TokenType::CONST)) {
        return constDeclaration();
    }
    if (impl_->options.isDragonFile && check(TokenType::STATIC)) {
        return staticDeclaration();
    }

    // Note: self() constructor syntax removed - use def() instead (parsed in functionDeclaration)

    // Soft keyword: match at statement position
    if (check(TokenType::IDENTIFIER) && peek().lexeme() == "match") {
        return matchStatement();
    }

    // Soft keyword: type alias at statement position (PEP 695)
    if (check(TokenType::IDENTIFIER) && peek().lexeme() == "type") {
        advance(); // skip 'type'
        auto stmt = std::make_unique<TypeAliasStmt>();
        stmt->setLocation(previous().location());
        stmt->name = std::string(consume(TokenType::IDENTIFIER, "Expect type alias name").lexeme());
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

// Assignment RHS that may be a MOVE: `self._f = own x` transfers the
// binding's +1 into an own field (docs/002 2.4 row 3 / 2.8). Same contextual
// shape as the call-argument form; anything else parses as a normal
// expression (`x = own + 1` still works for a variable named own).
std::unique_ptr<Expr> Parser::maybeMoveRhs() {
    if (impl_->options.isDragonFile && check(TokenType::IDENTIFIER) &&
        current().lexeme() == "own" &&
        peekNext().type() == TokenType::IDENTIFIER) {
        advance();  // 'own'
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
    // `mine = dub base` - a fresh, independent, priced copy (docs/002 2.7).
    if (impl_->options.isDragonFile && check(TokenType::IDENTIFIER) &&
        current().lexeme() == "dub" &&
        peekNext().type() == TokenType::IDENTIFIER) {
        advance();  // 'dub'
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
    // Statement location = its first token (the LHS target / expression).
    // Captured before any tokens are consumed and stamped on every node built
    // below, so diagnostics on assignments never fall back to 0:0.
    SourceLocation stmtLoc = peek().location();
    // Check for starred expression at start: *name, ... = ...
    std::unique_ptr<Expr> expr;
    if (check(TokenType::STAR) && !check(TokenType::STAR_EQUAL)) {
        // Peek: if STAR followed by IDENTIFIER followed by COMMA or EQUAL, it's starred unpacking
        size_t saved = impl_->current;
        advance(); // consume STAR
        if (check(TokenType::IDENTIFIER)) {
            auto name = std::make_unique<NameExpr>();
            name->name = std::string(advance().lexeme());
            auto starred = std::make_unique<StarredExpr>();
            starred->value = std::move(name);
            expr = std::move(starred);
        } else {
            // Not a starred target, rewind
            impl_->current = saved;
            expr = expression();
        }
    } else {
        expr = expression();
    }
    if (!expr) return nullptr;

    // Check for comma after first expression - could be tuple unpacking target
    // e.g. a, b = 1, 2 or a, *rest = [1, 2, 3]
    if (check(TokenType::COMMA) && !check(TokenType::NEWLINE)) {
        // Look ahead: is there an = sign after the comma-separated list?
        // We speculatively collect comma-separated targets
        auto tuple = std::make_unique<TupleExpr>();
        tuple->elements.push_back(std::move(expr));
        while (match(TokenType::COMMA)) {
            if (check(TokenType::EQUAL) || check(TokenType::NEWLINE) ||
                check(TokenType::RIGHT_PAREN) || isAtEnd()) break;
            // Handle *name in target
            if (check(TokenType::STAR)) {
                advance(); // consume STAR
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
            // Parse RHS - also support comma-separated as tuple.
            // After `=` the parser is committed to a RHS expression, so a
            // NEWLINE between the `=` and the RHS is purely cosmetic
            // continuation (Python multi-line assignment).
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
        // No = found - it's just a tuple expression statement
        // (rare but valid: `a, b` as a bare expression)
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
        // Parse RHS - also support comma-separated as tuple.
        // After `=` the parser is committed to a RHS expression, so a
        // NEWLINE between the `=` and the RHS is purely cosmetic
        // continuation (Python multi-line assignment).
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
            // Same rationale as above: `=` commits to a RHS, so allow a
            // line break between the `=` and the start of the expression.
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
        // The aug-assign operator commits to a RHS just like `=` does.
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

// defer <call>: schedule a direct call for scope exit. The operand must BE a
// call - a function, method, or bound-closure call. A deferred call's return
// value is discarded, so deferring anything value-shaped (a bare name,
// arithmetic over calls) is rejected here, not later.
std::unique_ptr<Stmt> Parser::deferStatement() {
    Token kw = advance();  // the 'defer' identifier
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
    stmt->setLocation(previous().location());  // the 'del' keyword
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
    // Python parity: `from X import (a, b, c,)` may span multiple lines and
    // tolerate a trailing comma. The lexer already suppresses NEWLINE inside
    // `()` so we only need to (a) accept the optional `(`, (b) allow the
    // trailing comma before `)`, and (c) consume the matching `)`.
    bool parenthesized = match(TokenType::LEFT_PAREN);
    do {
        // Trailing comma support: `from os import (a, b,)` exits the loop
        // here once we see the closing paren after a comma.
        if (parenthesized && check(TokenType::RIGHT_PAREN)) break;
        ImportStmt::Alias alias;
        alias.name = std::string(consume(TokenType::IDENTIFIER, "Expect name").lexeme());
        if (match(TokenType::AS)) alias.asName = std::string(consume(TokenType::IDENTIFIER, "Expect alias").lexeme());
        stmt->names.push_back(std::move(alias));
    } while (match(TokenType::COMMA));
    if (parenthesized) consume(TokenType::RIGHT_PAREN, "Expect ')' after import list");
    return stmt;
}

//===----------------------------------------------------------------------===//
// Compound Statements
//===----------------------------------------------------------------------===//

std::unique_ptr<Stmt> Parser::ifStatement() {
    consume(TokenType::IF, "Expect 'if'");
    auto stmt = std::make_unique<IfStmt>();
    stmt->condition = expression();
    stmt->thenBody = parseBlock();
    // Accept both `elif cond { ... }` and the C/Java/JS-style `else if cond { ... }`.
    // The latter is valid Python too (after dedent rules), and rejecting it just
    // because the lexer doesn't emit ELIF for it is a footgun for users coming
    // from non-Python languages.
    //
    // Newline tolerance: in brace-block mode the lexer emits NEWLINE between
    // `}` and a following `else`/`elif` on the next line. We skip those here
    // so multi-line `if {} \n else if {} \n else {}` works the same as the
    // same-line `} else if {} else {}` form. Pythonic style alternates these
    // freely; rejecting one would force users to pick one based on layout.
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
            advance(); // ELSE
            advance(); // IF
            auto cond = expression();
            auto body = parseBlock();
            stmt->elifClauses.emplace_back(std::move(cond), std::move(body));
            continue;
        }
        // Not an elif/else-if continuation - rewind so newlines don't get
        // eaten when the if has no else clause (the block parser depends on
        // them as statement separators).
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
    // Parse target as a primary expression (name, tuple) -- not full expression
    // to avoid consuming 'in' as a comparison operator
    auto target = primary();
    // Check for comma: for a, b in ... (tuple unpacking)
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
    // `for x in dub names` - snapshot iteration (docs/002 2.7/E17): the loop
    // walks a priced copy evaluated once, so mutating the original inside
    // the body is well-defined.
    if (impl_->options.isDragonFile && check(TokenType::IDENTIFIER) &&
        current().lexeme() == "dub" &&
        peekNext().type() == TokenType::IDENTIFIER) {
        advance();  // 'dub'
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
        // Check for except* (PEP 654 exception groups)
        if (match(TokenType::STAR)) {
            handler.isStar = true;
        }
        if (match(TokenType::LEFT_PAREN)) {
            if (check(TokenType::IDENTIFIER)) {
                std::string first = std::string(advance().lexeme());
                if (match(TokenType::COLON)) {
                    // (name: Type) - Dragon typed-binding form.
                    handler.name = first;
                    handler.type = parseType();
                } else {
                    // (Type) or (T1, T2, ...) - match any of the listed types.
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
            // `except (A, B) as e`
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
        stmt->items.push_back(std::move(item));
    } while (match(TokenType::COMMA));
    stmt->body = parseBlock();
    return stmt;
}

std::unique_ptr<Stmt> Parser::threadStatement() {
    // "thread" is a contextual keyword - consumed as IDENTIFIER, not THREAD token
    consume(TokenType::IDENTIFIER, "Expect 'thread'");
    auto stmt = std::make_unique<ThreadStmt>();
    stmt->setLocation(previous().location());
    stmt->body = parseBlock();
    return stmt;
}

//===----------------------------------------------------------------------===//
// Dragon-specific: const, static, self() constructor
//===----------------------------------------------------------------------===//

std::unique_ptr<Stmt> Parser::constDeclaration() {
    consume(TokenType::CONST, "Expect 'const'");
    auto loc = previous().location();
    // One or more `name: type` pairs. A single pair is the ordinary const
    // declaration; two or more is a const tuple-unpack
    // (`const q: int, r: int = divmod(7, 2)`), every target annotated.
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

    // Multi-target: lower onto the existing tuple-unpack AssignStmt path with
    // a synthesized tuple[...] annotation, so Sema/TypeChecker/CodeGen reuse
    // the one unpack implementation.
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
    // `own f: T [= value]` - sole-owner field marker (docs/001-memory.md).
    // The `own` identifier itself was matched contextually by statement().
    auto loc = current().location();
    advance();  // consume 'own'
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

    // static const name: type = value
    if (check(TokenType::CONST)) {
        auto stmt = constDeclaration();
        if (auto* ann = dynamic_cast<AnnAssignStmt*>(stmt.get())) {
            ann->isStatic = true;
        } else {
            // constDeclaration returned the multi-target unpack form.
            error("'static const' declares a single name; unpack is not supported here");
        }
        return stmt;
    }

    // static def method() { ... }
    if (check(TokenType::DEF) || check(TokenType::ASYNC)) {
        auto decl = functionDeclaration();
        if (auto* func = dynamic_cast<FunctionDecl*>(decl.get())) {
            func->isStatic = true;
            func->hasImplicitSelf = false;
        }
        return decl;
    }

    // static name: type = value (static field)
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

    // Expect "C" string literal (lexeme includes quotes: "C")
    auto stripQuotes = [](const std::string& s) -> std::string {
        if (s.size() >= 2 && (s.front() == '"' || s.front() == '\''))
            return s.substr(1, s.size() - 2);
        return s;
    };
    if (!check(TokenType::STRING) || stripQuotes(peek().lexeme()) != "C") {
        error("Expect '\"C\"' after 'extern'");
        return nullptr;
    }
    advance(); // consume "C"

    // Form 1: extern "C" from "lib" { def ...; def ...; }
    if (check(TokenType::FROM)) {
        advance(); // consume 'from'
        if (!check(TokenType::STRING)) {
            error("Expect library name string after 'from'");
            return nullptr;
        }
        std::string libName = stripQuotes(std::string(advance().lexeme()));
        consume(TokenType::LEFT_BRACE, "Expect '{' after library name");
        // Parse multiple extern function signatures inside the block
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
        // Stash extra declarations for the module to pick up
        for (size_t i = 1; i < decls.size(); i++) {
            impl_->pendingStmts.push_back(std::move(decls[i]));
        }
        return std::move(decls[0]);
    }

    // Form 2: extern "C" def func(params) -> ret
    return parseExternFuncSig("");
}

std::unique_ptr<Stmt> Parser::parseExternFuncSig(const std::string& libHint) {
    consume(TokenType::DEF, "Expect 'def' in extern declaration");
    auto loc = previous().location();

    auto decl = std::make_unique<FunctionDecl>();
    decl->setLocation(loc);
    decl->isExtern = true;
    decl->externLib = libHint;
    // The C-symbol slot accepts either an IDENTIFIER or any Dragon keyword
    // whose lexeme is a valid C identifier (e.g. `raise`, `pass`, `for`).
    // When the token is a keyword, an `as ALIAS` clause is required after
    // the signature so Dragon code has a usable name to call it by.
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
        // Lexer classified this as a keyword (e.g. RAISE) but the lexeme is
        // a valid C identifier - accept it as the external symbol name.
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
    // `extern "C" def CSYM(...) [-> ret] as DRAGON_NAME` lets a C
    // symbol whose spelling collides with a Dragon keyword (`raise`, `for`,
    // `pass`, ...) be bound under a Dragon-side alias. The alias is the
    // Dragon-visible identifier; `decl->externSymbol` keeps the C symbol so
    // the linker still finds it. Alias is mandatory iff the C name is a
    // keyword (otherwise the declaration would introduce an unusable scope
    // entry). For a non-keyword C name the alias is allowed but optional
    // (sugar for `from CSYM import CSYM as DRAGON_NAME`-style renaming).
    if (match(TokenType::AS)) {
        std::string alias = std::string(consume(TokenType::IDENTIFIER, "Expect alias name").lexeme());
        decl->name = alias;
        decl->externSymbol = cSymbol;
    } else if (nameIsKeyword) {
        error("extern 'C' symbol '" + cSymbol + "' is a Dragon keyword; "
              "add `as <alias>` so Dragon code can call it");
        decl->name = cSymbol; // best-effort recovery
    } else {
        decl->name = cSymbol;
    }
    // No body for extern declarations
    return decl;
}

// selfConstructor() removed - def() syntax replaces self() (Decision 009 update)

std::unique_ptr<Stmt> Parser::matchStatement() {
    // Consume 'match' (it's an IDENTIFIER used as soft keyword)
    advance(); // skip 'match'
    auto stmt = std::make_unique<MatchStmt>();
    stmt->setLocation(previous().location());
    stmt->subject = expression();

    // In .dr mode: match subject { case ... { } case ... { } }
    // In .py mode: match subject:\n case ...:
    if (impl_->options.isDragonFile) {
        consume(TokenType::LEFT_BRACE, "Expect '{' after match subject");
        // Forward-progress invariant: every iteration must advance the cursor.
        // consume() records an error WITHOUT advancing on a missing brace, and
        // statement()/parsePattern() can bottom out on a bad token without
        // advancing either, so an unguarded loop would spin forever. The colon
        // case-body form (`case 0: ...`) is the trigger - same hazard parseBlock
        // guards against - so we synchronize and break out on a missing case
        // brace, and watch the cursor in both loops.
        while (!check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
            size_t caseLoopStart = impl_->current;
            while (match(TokenType::NEWLINE) || match(TokenType::SEMICOLON)) {}
            if (check(TokenType::RIGHT_BRACE)) break;
            // Expect 'case' (soft keyword)
            if (!check(TokenType::IDENTIFIER) || peek().lexeme() != "case") {
                error("Expect 'case' in match block");
                break;
            }
            advance(); // skip 'case'
            MatchStmt::MatchCase matchCase;
            matchCase.pattern = parsePattern();
            // Optional guard: if expr
            if (check(TokenType::IF)) {
                advance();
                matchCase.guard = expression();
            }
            // Case body must be brace-delimited (`case p { ... }`). The colon
            // form is unsupported: report it, synchronize past the bad token so
            // the loop makes progress, and stop parsing further cases.
            if (!check(TokenType::LEFT_BRACE)) {
                error("Expect '{' after case pattern");
                synchronize();
                break;
            }
            advance(); // consume '{'
            while (!check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
                while (match(TokenType::NEWLINE) || match(TokenType::SEMICOLON)) {}
                if (check(TokenType::RIGHT_BRACE)) break;
                auto s = statement();
                if (s) matchCase.body.push_back(std::move(s));
                else if (!isAtEnd()) advance(); // statement() may not advance on error
            }
            consume(TokenType::RIGHT_BRACE, "Expect '}' after case body");
            stmt->cases.push_back(std::move(matchCase));
            // Backstop: if a single case parse consumed nothing (pattern and
            // body both bottomed out without advancing), force progress so the
            // outer loop can never spin on the same token.
            if (impl_->current == caseLoopStart) {
                // Invariant: a parsed case consumes at least `case`. No progress
                // means a grammar gap - surface it and stop, rather than silently
                // skipping a token (which would hide the real defect).
                error("malformed case in match block");
                break;
            }
        }
        consume(TokenType::RIGHT_BRACE, "Expect '}' after match block");
    } else {
        // .py mode: colon + indented block of cases
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
            advance(); // skip 'case'
            MatchStmt::MatchCase matchCase;
            matchCase.pattern = parsePattern();
            // Optional guard: if expr
            if (check(TokenType::IF)) {
                advance();
                matchCase.guard = expression();
            }
            // Case body: colon + indented block
            matchCase.body = parseBlock();
            stmt->cases.push_back(std::move(matchCase));
            // Backstop: guarantee forward progress even if pattern/body parsing
            // bottomed out on a bad token without advancing (see .dr branch).
            if (impl_->current == caseLoopStart) {
                // See the .dr branch: no progress means a grammar gap; surface it
                // and stop, rather than silently skipping a token.
                error("malformed case in match block");
                break;
            }
        }
        match(TokenType::DEDENT);
    }
    return stmt;
}

MatchPattern Parser::parsePattern(bool allowCommaOr) {
    // Same recursion cap as expression()/statement(): nested sequence patterns
    // (`case [[[...]]]:`) recurse parsePattern->parsePrimaryPattern->parsePattern
    // with no other bound, so a deeply nested pattern would otherwise overflow
    // the native stack and SIGSEGV the compiler.
    ParserRecursionGuard guard(impl_->recursionDepth);
    if (impl_->recursionDepth > Impl::kMaxRecursionDepth) {
        error(peek(), "pattern nesting too deep");
        synchronize();
        MatchPattern tooDeep;
        tooDeep.kind = MatchPattern::Kind::Wildcard;
        return tooDeep;
    }
    // Class pattern: `TypeName(...)` / `pkg.Class(...)`. Currently a type test
    // (`case int()`, `case Point()`); positional/keyword field destructuring is
    // parsed but rejected downstream as not-yet-supported (clean error, not a
    // miscompile). `className` is the (possibly dotted) type name already lexed.
    auto parseClassPattern = [&](const std::string& className) -> MatchPattern {
        advance();  // consume '('
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
                    advance();             // consume '='
                    parsePattern(false);   // discard the value pattern
                } else {
                    p.subPatterns.push_back(std::move(sub));
                }
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RIGHT_PAREN, "Expect ')' after class pattern");
        return p;
    };

    // Or pattern: p1 | p2 | p3 (or p1, p2, p3 in .dr mode)
    // Parse primary pattern first, then check for | or comma
    auto parsePrimaryPattern = [&]() -> MatchPattern {
        // Wildcard: _
        if (check(TokenType::IDENTIFIER) && peek().lexeme() == "_") {
            advance();
            MatchPattern p;
            p.kind = MatchPattern::Kind::Wildcard;
            return p;
        }
        // Sequence pattern: [p1, p2, ...]
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
        // Sequence pattern: (p1, p2, ...)
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
        // Literal patterns: numbers, strings, True, False, None
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
        // Negative number literal: -42
        if (check(TokenType::MINUS)) {
            auto unary = expression();
            MatchPattern p;
            p.kind = MatchPattern::Kind::Literal;
            p.literal = std::move(unary);
            return p;
        }
        // Capture, Value (dotted name like Color.RED), or Class (`Name(...)`)
        if (check(TokenType::IDENTIFIER)) {
            std::string name = std::string(advance().lexeme());
            // Dotted name: a value pattern (Color.RED) or a dotted class
            // pattern (pkg.Class(...)).
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
                // pkg.Class(...) - a class pattern with a dotted name.
                if (check(TokenType::LEFT_PAREN)) return parseClassPattern(full);
                // Color.RED - a value pattern.
                MatchPattern p;
                p.kind = MatchPattern::Kind::Value;
                p.literal = std::move(cur);
                return p;
            }
            // Class pattern: Name(...)
            if (check(TokenType::LEFT_PAREN)) return parseClassPattern(name);
            // Capture pattern: name
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

    // Check for Or pattern: pattern | pattern (both modes)
    // In .dr mode, comma also serves as OR separator at top level: case 1, 2, 3
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

    // def() in .dr class body = anonymous constructor (becomes __init__)
    if (impl_->inClassBody && impl_->options.isDragonFile &&
        check(TokenType::LEFT_PAREN)) {
        decl->name = "__init__";
        decl->isConstructor = true;
        decl->hasImplicitSelf = true;
        decl->isMethod = true;
    } else {
        decl->name = std::string(consume(TokenType::IDENTIFIER, "Expect function name").lexeme());
    }
    // D044 - optional PEP 695 type-parameter list `def f[T](...)`. Parsed
    // between the name and the parameter list (`[` here is unambiguous).
    decl->typeParams = parseTypeParams();
    consume(TokenType::LEFT_PAREN, "Expect '('");
    decl->params = parseParameters();
    consume(TokenType::RIGHT_PAREN, "Expect ')'");

    // Extract positional-only (/) and keyword-only (bare *) separators
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

    // Mark methods and enforce implicit self in .dr mode
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

    // Clear inClassBody while parsing the function body so nested `def` inside
    // a method body isn't misclassified as a method itself. inClassBody tracks
    // direct class membership, not transitive enclosure.
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
    // D044 - optional PEP 695 type-parameter list `class Foo[T]`. Parsed before
    // the base list; a `[` directly after the class name is unambiguous.
    decl->typeParams = parseTypeParams();
    if (match(TokenType::LEFT_PAREN)) {
        if (!check(TokenType::RIGHT_PAREN)) {
            do { decl->bases.push_back(expression()); } while (match(TokenType::COMMA));
        }
        consume(TokenType::RIGHT_PAREN, "Expect ')'");
    }
    bool savedInClass = impl_->inClassBody;
    impl_->inClassBody = true;
    decl->body = parseBlock();
    impl_->inClassBody = savedInClass;
    decl->docstring = extractDocstring(decl->body);

    // Assign constructor indices to __init__ overloads
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

/// Parse an enum declaration:
///
///  enum Color { RED, GREEN, BLUE }
///  enum Status { OK = 200, NOT_FOUND = 404, ERROR = 500 }
///  enum Mixed { A, B = 10, C, D } // C = 11, D = 12 (resume from explicit)
///
/// Lowered at parse time to a `class Name { static M0: int = V0 ... }` so the
/// existing class infrastructure does all the heavy lifting (static field
/// access, name resolution, codegen as a global int constant). Members get
/// auto-numbered from 0; explicit values reset the running counter.
std::unique_ptr<Stmt> Parser::enumDeclaration() {
    Token enumTok = current();
    advance(); // consume the contextual "enum" identifier
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
            // Only int literal initializers supported in v0.0.1.
            // Negative values via UnaryExpr(-, IntegerLiteral).
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

        // Build: static <member>: int = <memberValue>
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

        // Comma optional between members; newline always allowed.
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

//===----------------------------------------------------------------------===//
// Type Annotation Parsing
//===----------------------------------------------------------------------===//

std::unique_ptr<TypeExpr> Parser::parseType() {
    // Same recursion cap as expression()/statement(): `list[list[list[...]]]`
    // (or Callable nesting) recurses parseType->parseGenericType->parseType
    // with no other bound, so an attacker-supplied deeply nested annotation
    // would otherwise overflow the native stack and SIGSEGV the compiler
    // (which is exposed to user input via the web playground).
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
    if (match(TokenType::NONE)) {
        auto t = std::make_unique<NamedTypeExpr>();
        t->name = "None";
        t->setLocation(previous().location());
        return t;
    }
    if (match(TokenType::IDENTIFIER)) {
        auto t = std::make_unique<NamedTypeExpr>();
        t->setLocation(previous().location());  // start of the (possibly dotted) name
        t->name = std::string(previous().lexeme());
        // Accept dotted type names like `ipaddress.IPv4Address` so a user
        // can write `a: ipaddress.IPv4Address = ...` after `import ipaddress`.
        // The full dotted path is stored in NamedTypeExpr.name; the
        // TypeChecker walks dots through ModuleType chains in resolveType.
        while (check(TokenType::DOT) && peekNext().type() == TokenType::IDENTIFIER) {
            advance();              // consume DOT
            advance();              // consume IDENTIFIER
            t->name += ".";
            t->name += std::string(previous().lexeme());
        }
        if (check(TokenType::LEFT_BRACKET)) return parseGenericType(std::move(t));
        return t;
    }
    return nullptr;
}

std::unique_ptr<TypeExpr> Parser::parseGenericType(std::unique_ptr<TypeExpr> base) {
    // Capture the type's start (the base name, e.g. `Shelter` in `Shelter[int]`)
    // BEFORE moving `base`, so every node built here carries a real source
    // location. Without it, a type error raised against the instantiation (arity
    // mismatch, bound violation, unknown-generic) would report at 0:0.
    SourceLocation startLoc = base ? base->location() : peek().location();
    consume(TokenType::LEFT_BRACKET, "Expect '['");
    if (check(TokenType::LEFT_BRACKET)) {
        // Callable[[params], return]
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

    // Normalize Union[A, B, ...] and Optional[T] into the pipe-form
    // UnionTypeExpr so every downstream stage (Sema, TypeChecker, CodeGen)
    // treats them identically to `A | B` - one union representation, no
    // per-form special-casing.
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
    // `class Foo[]` / `def f[]()` is meaningless - an empty bracket pair is a
    // user error, not a non-generic decl. Reject it rather than silently
    // treating the decl as non-generic.
    if (check(TokenType::RIGHT_BRACKET)) {
        error("Expect at least one type parameter inside '[]'");
        advance();  // consume ']' to keep parsing
        return params;
    }
    do {
        TypeParam tp;
        tp.name = std::string(consume(TokenType::IDENTIFIER,
                                      "Expect type-parameter name").lexeme());
        // Bounds - bounded type parameter `T: Bound`. The bound is any type
        // expression (a class, typically). It is stored on the TypeParam, then
        // (a) consulted when the generic body accesses members/operators on a
        // `T`-typed value and (b) enforced at each instantiation: the concrete
        // type argument must satisfy the bound.
        if (match(TokenType::COLON)) {
            tp.bound = parseType();
        }
        params.push_back(std::move(tp));
    } while (match(TokenType::COMMA));
    consume(TokenType::RIGHT_BRACKET, "Expect ']' after type parameters");
    return params;
}

//===----------------------------------------------------------------------===//
// Block Parsing
//===----------------------------------------------------------------------===//

std::vector<std::unique_ptr<Stmt>> Parser::parseBlock() {
    std::vector<std::unique_ptr<Stmt>> stmts;
    // Forward-progress invariant: statement() may return nullptr after an
    // error (e.g. primary() bottoms out on an unrecognized token without
    // advancing). Without an explicit advance() in that case, this loop
    // would spin on the same bad token forever, allocating diagnostics
    // until the process OOMs. parseModule applies the same guard at top
    // level; parseBlock missed it.
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
        // Colon may already have been consumed (e.g., by functionDeclaration)
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
        // Positional-only separator: /
        if (match(TokenType::SLASH)) {
            Parameter sep;
            sep.name = "/";
            params.push_back(std::move(sep));
            continue;
        }
        Parameter param;
        if (match(TokenType::STAR)) {
            if (check(TokenType::COMMA) || check(TokenType::RIGHT_PAREN)) {
                // Bare * - keyword-only separator (no name)
                param.isVarArg = true;
                param.name = "";
            } else {
                // *args
                param.isVarArg = true;
                if (check(TokenType::IDENTIFIER)) param.name = std::string(advance().lexeme());
            }
        } else if (match(TokenType::POWER)) {
            param.isKwArg = true;
            param.name = std::string(consume(TokenType::IDENTIFIER, "Expect parameter name").lexeme());
        } else {
            // `own p: T` - the callee owns the parameter (docs/002 2.8).
            // Contextual: two consecutive identifiers cannot otherwise start
            // a parameter, so a parameter NAMED own keeps compiling.
            if (check(TokenType::IDENTIFIER) && current().lexeme() == "own" &&
                peekNext().type() == TokenType::IDENTIFIER) {
                advance();
                param.isOwn = true;
            }
            param.name = std::string(consume(TokenType::IDENTIFIER, "Expect parameter name").lexeme());
        }
        if (match(TokenType::COLON)) param.type = parseType();
        if (match(TokenType::EQUAL)) param.defaultValue = expression();
        // Typing is not optional for variadics. A scalar parameter can have its
        // type inferred, but an unannotated *args/**kwargs has no element type
        // to monomorphize on - the call site would silently erase every element
        // to i64 (floats to bit-patterns, pointers to raw ints). Require the
        // annotation: the element type drives the representation. Use
        // `*args: Any` (box list) for heterogeneous arguments, or a concrete
        // element type like `*args: int` / `*args: str`. The bare `*` keyword-
        // only separator (no name) is exempt.
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

//===----------------------------------------------------------------------===//
// Error Handling
//===----------------------------------------------------------------------===//

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

} // namespace dragon
