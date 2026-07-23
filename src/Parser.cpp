#include "dragon/Parser.h"
// FIXME: fold this back into ParserStmts once pendingStmts is sorted
#include "ParserImpl.h"
#include <cctype>
#include <charconv>
#include <stdexcept>
#include <string>

namespace dragon {

//===----------------------------------------------------------------------===//
// Parser Implementation (Parser::Impl + literal/docstring helpers now live in
// ParserImpl.h, shared with ParserStmts.cpp)
//===----------------------------------------------------------------------===//

Parser::Parser(std::vector<Token> tokens, ParserOptions options)
    : impl_(std::make_unique<Impl>()) {
    impl_->tokens = std::move(tokens);
    impl_->options = options;
}

Parser::~Parser() = default;

// parseIntLiteralChecked / parseFloatLiteralChecked / extractDocstring moved to
// ParserImpl.h (inline, shared with ParserStmts.cpp).

std::unique_ptr<Module> Parser::parseModule() {
    auto module = std::make_unique<Module>();
    module->filename = impl_->options.filename;
    module->isDragonFile = impl_->options.isDragonFile;

    while (!isAtEnd()) {
        if (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON)) { advance(); continue; }
        auto stmt = parseStatement();
        if (stmt) {
            module->body.push_back(std::move(stmt));
        } else {
            if (!isAtEnd()) advance();
        }
        // Drain any extra stmts from multi-decl constructs (extern "C" from "lib" { })
        for (auto& pending : impl_->pendingStmts) {
            module->body.push_back(std::move(pending));
        }
        impl_->pendingStmts.clear();
    }

    module->docstring = extractDocstring(module->body);
    return module;
}

std::unique_ptr<Expr> Parser::parseExpression() {
    return expression();
}

std::unique_ptr<Stmt> Parser::parseStatement() {
    return statement();
}

std::vector<TemplatePart> Parser::parseTemplateBody(
        const std::string& body, const SourceLocation& loc, bool isDragonFile) {
    std::vector<TemplatePart> out;
    const std::string& val = body;
    size_t i = 0;
    while (i < val.size()) {
        if (val[i] == '!' && i + 1 < val.size() && val[i+1] == '!' &&
            i + 2 < val.size() && val[i+2] == '{') {
            // Escaped !!{ -> literal !{
            TemplatePart p;
            p.kind = TemplatePart::Kind::Literal;
            p.literal = "!{";
            out.push_back(std::move(p));
            i += 3;
        } else if (val[i] == '!' && i + 1 < val.size() && val[i+1] == '!' &&
                   i + 2 < val.size() && val[i+2] == '}') {
            // Escaped !!} -> literal }
            TemplatePart p;
            p.kind = TemplatePart::Kind::Literal;
            p.literal = "}";
            out.push_back(std::move(p));
            i += 3;
        } else if (val[i] == '!' && i + 1 < val.size() && val[i+1] == '{') {
            // Template interpolation: !{expr}
            const size_t bangPos = i;
            size_t start = i + 2;
            int depth = 1;
            size_t j = start;
            while (j < val.size() && depth > 0) {
                if (val[j] == '{') depth++;
                else if (val[j] == '}') depth--;
                if (depth > 0) j++;
            }
            std::string exprText = val.substr(start, j - start);
            i = j + 1;

            // Pipe filter: rightmost top-level '|' (outside any (), [], {}).
            // Braces matter for block interpolations whose inner `!{e | raw}`
            // pipes live at brace-depth > 0.
            std::string filterName;
            std::string parseText = exprText;
            {
                int parenDepth = 0;
                int lastPipe = -1;
                for (size_t fi = 0; fi < parseText.size(); fi++) {
                    if (parseText[fi] == '(' || parseText[fi] == '[' ||
                        parseText[fi] == '{') parenDepth++;
                    else if (parseText[fi] == ')' || parseText[fi] == ']' ||
                             parseText[fi] == '}') parenDepth--;
                    else if (parseText[fi] == '|' && parenDepth == 0) {
                        lastPipe = (int)fi;
                    }
                }
                if (lastPipe >= 0) {
                    std::string rawFilter = parseText.substr(lastPipe + 1);
                    size_t fs = rawFilter.find_first_not_of(" \t\n\r");
                    size_t fe = rawFilter.find_last_not_of(" \t\n\r");
                    if (fs != std::string::npos && fe != std::string::npos) {
                        filterName = rawFilter.substr(fs, fe - fs + 1);
                    }
                    parseText = parseText.substr(0, lastPipe);
                    size_t te = parseText.find_last_not_of(" \t\n\r");
                    if (te != std::string::npos) {
                        parseText = parseText.substr(0, te + 1);
                    }
                }
            }

            // `!{*expr}` spread sugar - record the marker; CodeGen defaults it
            // to `| join` (empty sep) and rejects combining it with a filter.
            bool isSpread = false;
            {
                size_t ws = parseText.find_first_not_of(" \t\n\r");
                if (ws != std::string::npos && parseText[ws] == '*') {
                    isSpread = true;
                    parseText = parseText.substr(ws + 1);
                }
            }

            // Sub-lex + sub-parse the interpolation body. inTemplateInterpolation
            // makes `:{` a TEMPLATE_CONTENT_OPEN so block-mode statements can
            // hold `:{ ... }` content fragments (D017 Phase 4.B). Nested content
            // aliases created below recurse through primary() -> parseTemplateBody.
            LexerOptions fLexOpts;
            fLexOpts.filename = "<template>";
            fLexOpts.inTemplateInterpolation = true;
            Lexer fLexer(parseText, fLexOpts);
            auto fTokens = fLexer.tokenize();
            ParserOptions fOpts;
            fOpts.isDragonFile = isDragonFile;
            Parser fParser(std::move(fTokens), fOpts);
            auto fModule = fParser.parseModule();

            TemplatePart p;
            p.kind = TemplatePart::Kind::Interpolation;
            p.bangPos = bangPos;
            p.exprText = std::move(exprText);
            p.filterName = std::move(filterName);
            p.isSpread = isSpread;

            // Expression mode = exactly one ExprStmt; anything else is a block
            // interpolation (`for`/`if` + `:{}` fragments). parseModule handles
            // both and yields an AST the later stages walk uniformly.
            if (fModule && !fParser.hasErrors()) {
                if (fModule->body.size() == 1) {
                    if (auto* es = dynamic_cast<ExprStmt*>(fModule->body[0].get())) {
                        p.expr = std::move(es->expr);
                    } else {
                        p.kind = TemplatePart::Kind::Block;
                        p.blockStmts = std::move(fModule->body);
                    }
                } else if (!fModule->body.empty()) {
                    p.kind = TemplatePart::Kind::Block;
                    p.blockStmts = std::move(fModule->body);
                } else {
                    p.parseFailed = true;
                }
            } else {
                p.parseFailed = true;
            }
            if (p.expr) p.expr->setLocation(loc);
            out.push_back(std::move(p));
        } else {
            // Literal text run - stored raw (template text is literal by design;
            // escapes are NOT processed, matching the original CodeGen scan).
            size_t start = i;
            while (i < val.size()) {
                if (val[i] == '!' && i + 1 < val.size() &&
                    (val[i+1] == '{' || val[i+1] == '!')) break;
                i++;
            }
            std::string text = val.substr(start, i - start);
            if (!text.empty()) {
                TemplatePart p;
                p.kind = TemplatePart::Kind::Literal;
                p.literal = std::move(text);
                out.push_back(std::move(p));
            }
        }
    }
    return out;
}

const std::vector<ParserDiagnostic>& Parser::diagnostics() const {
    return impl_->diagnostics;
}

bool Parser::hasErrors() const {
    for (const auto& diag : impl_->diagnostics) {
        if (diag.level == ParserDiagnostic::Level::Error) return true;
    }
    return false;
}

//===----------------------------------------------------------------------===//
// Token Management
//===----------------------------------------------------------------------===//

Token Parser::advance() {
    if (!isAtEnd()) impl_->current++;
    return previous();
}

Token Parser::current() const {
    return impl_->tokens[impl_->current];
}

Token Parser::previous() const {
    return impl_->tokens[impl_->current - 1];
}

Token Parser::peek() const {
    return impl_->tokens[impl_->current];
}

Token Parser::peekNext() const {
    if (impl_->current + 1 >= impl_->tokens.size()) {
        return impl_->tokens.back();
    }
    return impl_->tokens[impl_->current + 1];
}

bool Parser::check(TokenType type) const {
    if (isAtEnd()) return false;
    return peek().type() == type;
}

bool Parser::match(TokenType type) {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

Token Parser::consume(TokenType type, const std::string& message) {
    if (check(type)) return advance();
    error(message);
    return Token();
}

bool Parser::isAtEnd() const {
    return peek().type() == TokenType::END_OF_FILE;
}

void Parser::skipNewlines() {
    while (check(TokenType::NEWLINE)) advance();
}

//===----------------------------------------------------------------------===//
// Expression Parsing
//===----------------------------------------------------------------------===//

// RAII guard for the parser's recursion-depth counter. Increments on entry
// to a deep recursion sink (expression/statement) and decrements on scope
// exit, so even error-recovery early returns can't leak depth.

std::unique_ptr<Expr> Parser::expression() {
    ParserRecursionGuard guard(impl_->recursionDepth);
    if (impl_->recursionDepth > Impl::kMaxRecursionDepth) {
        // Cap fired: bail with a benign placeholder so callers (which don't
        // null-check uniformly) keep working. Synchronize to skip the rest
        // of the over-nested expression and avoid re-tripping immediately.
        error(peek(), "expression nesting too deep");
        auto stub = std::make_unique<IntegerLiteral>();
        stub->setLocation(peek().location());
        stub->value = 0;
        synchronize();
        return stub;
    }
    return assignment();
}

std::unique_ptr<Expr> Parser::assignment() {
    return ternary();
}

std::unique_ptr<Expr> Parser::ternary() {
    auto expr = orExpr();
    if (match(TokenType::IF)) {
        auto node = std::make_unique<IfExpr>();
        node->thenExpr = std::move(expr);
        node->condition = orExpr();
        consume(TokenType::ELSE, "Expect 'else' in ternary expression");
        node->elseExpr = ternary();
        return node;
    }
    return expr;
}

std::unique_ptr<Expr> Parser::orExpr() {
    auto expr = andExpr();
    while (match(TokenType::OR)) {
        auto bin = std::make_unique<BinaryExpr>();
        bin->left = std::move(expr);
        bin->op = previous();
        bin->setLocation(bin->op.location());
        // After a binary operator the parser is committed to a right operand,
        // so a NEWLINE between the operator and the next operand is purely
        // cosmetic line continuation (trailing-operator rule).
        skipNewlines();
        bin->right = andExpr();
        expr = std::move(bin);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::andExpr() {
    auto expr = notExpr();
    while (match(TokenType::AND)) {
        auto bin = std::make_unique<BinaryExpr>();
        bin->left = std::move(expr);
        bin->op = previous();
        bin->setLocation(bin->op.location());
        skipNewlines();
        bin->right = notExpr();
        expr = std::move(bin);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::notExpr() {
    if (match(TokenType::NOT)) {
        auto un = std::make_unique<UnaryExpr>();
        un->op = previous();
        un->setLocation(un->op.location());
        un->operand = notExpr();
        return un;
    }
    return comparison();
}

std::unique_ptr<Expr> Parser::comparison() {
    auto expr = bitwiseOr();

    // Recognize a comparison operator at the current cursor, including the
    // two-word forms `not in` and `is not`. On a match the cursor is advanced
    // past the operator and `out` is filled with a single Token whose type is
    // one of {LESS, GREATER, LESS_EQUAL, GREATER_EQUAL, EQUAL_EQUAL, NOT_EQUAL,
    // IN, IS, NOT_IN, IS_NOT}. The two-word forms synthesize a Token with the
    // location of the first word and lexeme "not in" / "is not", so AST
    // printing and diagnostics render them faithfully.
    auto tryConsumeCompOp = [&](Token& out) -> bool {
        TokenType t = peek().type();
        switch (t) {
            case TokenType::LESS:
            case TokenType::GREATER:
            case TokenType::LESS_EQUAL:
            case TokenType::GREATER_EQUAL:
            case TokenType::EQUAL_EQUAL:
            case TokenType::NOT_EQUAL:
                advance();
                out = previous();
                return true;
            case TokenType::IN:
                advance();
                out = previous();
                return true;
            case TokenType::IS:
                if (peekNext().type() == TokenType::NOT) {
                    Token first = advance();      // consume IS
                    advance();                    // consume NOT
                    out = Token(TokenType::IS_NOT, "is not", first.location());
                    return true;
                }
                advance();
                out = previous();
                return true;
            case TokenType::NOT:
                if (peekNext().type() == TokenType::IN) {
                    Token first = advance();      // consume NOT
                    advance();                    // consume IN
                    out = Token(TokenType::NOT_IN, "not in", first.location());
                    return true;
                }
                return false;                     // bare prefix `not` belongs to notExpr
            default:
                return false;
        }
    };

    Token firstOp;
    if (!tryConsumeCompOp(firstOp)) return expr;

    std::vector<std::unique_ptr<Expr>> operands;
    std::vector<Token> operators;
    operands.push_back(std::move(expr));
    operators.push_back(firstOp);
    skipNewlines();
    operands.push_back(bitwiseOr());

    Token nextOp;
    while (tryConsumeCompOp(nextOp)) {
        operators.push_back(nextOp);
        skipNewlines();
        operands.push_back(bitwiseOr());
    }

    if (operators.size() == 1) {
        auto bin = std::make_unique<BinaryExpr>();
        bin->left = std::move(operands[0]);
        bin->op = operators[0];
        bin->setLocation(bin->op.location());
        bin->right = std::move(operands[1]);
        return bin;
    }
    auto chain = std::make_unique<ChainedCompExpr>();
    chain->operands = std::move(operands);
    chain->operators = std::move(operators);
    return chain;
}

std::unique_ptr<Expr> Parser::bitwiseOr() {
    auto expr = bitwiseXor();
    while (match(TokenType::PIPE)) {
        auto bin = std::make_unique<BinaryExpr>();
        bin->left = std::move(expr);
        bin->op = previous();
        bin->setLocation(bin->op.location());
        skipNewlines();
        bin->right = bitwiseXor();
        expr = std::move(bin);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::bitwiseXor() {
    auto expr = bitwiseAnd();
    while (match(TokenType::CARET)) {
        auto bin = std::make_unique<BinaryExpr>();
        bin->left = std::move(expr);
        bin->op = previous();
        bin->setLocation(bin->op.location());
        skipNewlines();
        bin->right = bitwiseAnd();
        expr = std::move(bin);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::bitwiseAnd() {
    auto expr = shift();
    while (match(TokenType::AMPERSAND)) {
        auto bin = std::make_unique<BinaryExpr>();
        bin->left = std::move(expr);
        bin->op = previous();
        bin->setLocation(bin->op.location());
        skipNewlines();
        bin->right = shift();
        expr = std::move(bin);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::shift() {
    auto expr = term();
    while (match(TokenType::LEFT_SHIFT) || match(TokenType::RIGHT_SHIFT)) {
        auto bin = std::make_unique<BinaryExpr>();
        bin->left = std::move(expr);
        bin->op = previous();
        bin->setLocation(bin->op.location());
        skipNewlines();
        bin->right = term();
        expr = std::move(bin);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::term() {
    auto expr = factor();
    while (match(TokenType::PLUS) || match(TokenType::MINUS)) {
        auto bin = std::make_unique<BinaryExpr>();
        bin->left = std::move(expr);
        bin->op = previous();
        bin->setLocation(bin->op.location());
        // Trailing-operator continuation: allow `x +\n y` to span lines.
        skipNewlines();
        bin->right = factor();
        expr = std::move(bin);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::factor() {
    auto expr = unary();
    while (match(TokenType::STAR) || match(TokenType::SLASH) ||
           match(TokenType::PERCENT) || match(TokenType::DOUBLE_SLASH)) {
        auto bin = std::make_unique<BinaryExpr>();
        bin->left = std::move(expr);
        bin->op = previous();
        bin->setLocation(bin->op.location());
        skipNewlines();
        bin->right = unary();
        expr = std::move(bin);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::unary() {
    if (match(TokenType::MINUS) || match(TokenType::PLUS) || match(TokenType::TILDE)) {
        auto un = std::make_unique<UnaryExpr>();
        un->op = previous();
        un->setLocation(un->op.location());
        un->operand = unary();
        return un;
    }
    return power();
}

std::unique_ptr<Expr> Parser::power() {
    auto expr = fireExpr();
    if (match(TokenType::POWER)) {
        auto bin = std::make_unique<BinaryExpr>();
        bin->left = std::move(expr);
        bin->op = previous();
        bin->setLocation(bin->op.location());
        skipNewlines();
        bin->right = unary();
        return bin;
    }
    return expr;
}

std::unique_ptr<Expr> Parser::fireExpr() {
    if (match(TokenType::FIRE)) {
        auto fe = std::make_unique<FireExpr>();
        fe->setLocation(previous().location());
        if (check(TokenType::LEFT_BRACE)) {
            // fire { block } form - inline block as green thread
            fe->bodyStmts = parseBlock();
        } else {
            // fire fn(args) form
            fe->operand = expression();
        }
        return fe;
    }
    return awaitExpr();
}

std::unique_ptr<Expr> Parser::awaitExpr() {
    if (match(TokenType::AWAIT)) {
        auto aw = std::make_unique<AwaitExpr>();
        aw->operand = unary();
        return aw;
    }
    return call();
}

std::unique_ptr<Expr> Parser::call() {
    auto expr = primary();
    if (!expr) return nullptr;

    while (true) {
        if (match(TokenType::LEFT_PAREN)) {
            expr = finishCall(std::move(expr));
        } else if (match(TokenType::DOT)) {
            // Locate at the start of the postfix chain (the object), falling
            // back to the '.' token. Without this, `del obj.attr` diagnostics
            // (and any error on an attribute access) report at 0:0.
            SourceLocation loc = expr->location();
            if (loc.line == 0) loc = previous().location();
            auto attr = std::make_unique<AttributeExpr>();
            attr->setLocation(loc);
            attr->object = std::move(expr);
            attr->attribute = std::string(consume(TokenType::IDENTIFIER, "Expect attribute name after '.'").lexeme());
            expr = std::move(attr);
        } else if (match(TokenType::LEFT_BRACKET)) {
            // Capture now: `previous()` is the '[' token here, but parsing the
            // index/slice below advances past it. Prefer the object's start.
            SourceLocation subLoc = expr->location();
            if (subLoc.line == 0) subLoc = previous().location();
            // Check for slice syntax: obj[a:b], obj[a:b:c], obj[:b], obj[::c], etc.
            // A slice is detected by the presence of COLON before RIGHT_BRACKET
            std::unique_ptr<Expr> first;
            if (!check(TokenType::COLON) && !check(TokenType::RIGHT_BRACKET)) {
                first = expression();
            }
            if (check(TokenType::COLON)) {
                // This is a slice
                auto slice = std::make_unique<SliceExpr>();
                slice->lower = std::move(first);  // may be nullptr
                consume(TokenType::COLON, "Expect ':' in slice");
                // upper bound (optional)
                if (!check(TokenType::COLON) && !check(TokenType::RIGHT_BRACKET)) {
                    slice->upper = expression();
                }
                // step (optional, after second colon)
                if (match(TokenType::COLON)) {
                    if (!check(TokenType::RIGHT_BRACKET)) {
                        slice->step = expression();
                    }
                }
                consume(TokenType::RIGHT_BRACKET, "Expect ']' after slice");
                auto sub = std::make_unique<SubscriptExpr>();
                sub->setLocation(subLoc);
                sub->object = std::move(expr);
                sub->index = std::move(slice);
                expr = std::move(sub);
            } else {
                // Regular subscript. Comma-separated indices (`a[i, j]`) form a
                // tuple index - Python parity, and the form a multi-parameter
                // generic instantiation `pair[int, str](...)` lands on (the
                // TypeChecker reads the tuple's elements as type arguments when
                // the object is a generic callable).
                auto sub = std::make_unique<SubscriptExpr>();
                sub->setLocation(subLoc);
                sub->object = std::move(expr);
                if (check(TokenType::COMMA)) {
                    auto tup = std::make_unique<TupleExpr>();
                    tup->setLocation(subLoc);
                    tup->elements.push_back(std::move(first));
                    while (match(TokenType::COMMA)) {
                        if (check(TokenType::RIGHT_BRACKET)) break;  // trailing comma
                        tup->elements.push_back(expression());
                    }
                    sub->index = std::move(tup);
                } else {
                    sub->index = std::move(first);
                }
                consume(TokenType::RIGHT_BRACKET, "Expect ']' after subscript");
                expr = std::move(sub);
            }
        } else {
            break;
        }
    }
    return expr;
}

std::unique_ptr<Expr> Parser::finishCall(std::unique_ptr<Expr> callee) {
    auto callExpr = std::make_unique<CallExpr>();
    callExpr->callee = std::move(callee);
    // The call expression starts where its callee does; without this the
    // CallExpr defaulted to 0:0, so any diagnostic reported on the call node
    // itself (e.g. an unsolved generic type parameter) had no source location.
    callExpr->setLocation(callExpr->callee->location());

    if (!check(TokenType::RIGHT_PAREN)) {
        do {
            // Allow a trailing comma before the closing paren, e.g. f(a, b,).
            // The comma that got us here was the last token in the list.
            if (check(TokenType::RIGHT_PAREN)) break;
            // **expr - dict/kwargs spread (e.g. Customer(**row)). Represented as
            // a kwArg with an empty name (the spread sentinel), mirroring the
            // null-key DictExpr convention for `{**other}`.
            if (match(TokenType::POWER)) {
                callExpr->kwArgs.emplace_back("", expression());
            // *expr - positional spread (e.g. total(*xs)). Wrapped in StarredExpr.
            } else if (match(TokenType::STAR)) {
                auto starred = std::make_unique<StarredExpr>();
                starred->value = expression();
                callExpr->args.push_back(std::move(starred));
            } else if (check(TokenType::IDENTIFIER) && peekNext().type() == TokenType::EQUAL) {
                std::string name = std::string(peek().lexeme());
                advance();
                advance();
                callExpr->kwArgs.emplace_back(name, expression());
            } else if (check(TokenType::IDENTIFIER) && peek().lexeme() == "dub" &&
                       peekNext().type() == TokenType::IDENTIFIER) {
                // `f(dub x)` - pass a priced copy, keep yours (docs/002 2.7).
                advance();  // 'dub'
                auto dubbed = std::make_unique<NameExpr>();
                dubbed->name = std::string(
                    consume(TokenType::IDENTIFIER,
                            "Expect binding name after 'dub'").lexeme());
                dubbed->setLocation(previous().location());
                dubbed->isDubMarked = true;
                callExpr->args.push_back(std::move(dubbed));
            } else if (check(TokenType::IDENTIFIER) && peek().lexeme() == "own" &&
                       peekNext().type() == TokenType::IDENTIFIER) {
                // `f(own x)` - move argument (docs/002 2.8): the binding's +1
                // transfers to the callee and x is Moved afterwards. A move
                // is of a BINDING only; own field/element reads cannot move
                // (their owner is the container). Ordered after the kwarg
                // branch so `f(own=3)` stays a keyword argument.
                advance();  // 'own'
                auto moved = std::make_unique<NameExpr>();
                moved->name = std::string(
                    consume(TokenType::IDENTIFIER,
                            "Expect binding name after 'own'").lexeme());
                moved->setLocation(previous().location());
                moved->isMoveMarked = true;
                if (check(TokenType::DOT) || check(TokenType::LEFT_BRACKET))
                    error("own moves a BINDING; a field or element cannot be "
                          "moved (its container owns it) - bind it first or "
                          "dub it");
                callExpr->args.push_back(std::move(moved));
            } else {
                callExpr->args.push_back(expression());
            }
        } while (match(TokenType::COMMA));
    }
    consume(TokenType::RIGHT_PAREN, "Expect ')' after arguments");
    return callExpr;
}

std::unique_ptr<Expr> Parser::subscript() { return attribute(); }
std::unique_ptr<Expr> Parser::attribute() { return primary(); }

std::unique_ptr<Expr> Parser::primary() {
    if (match(TokenType::INTEGER)) {
        auto lit = std::make_unique<IntegerLiteral>();
        lit->setLocation(previous().location());
        std::string s(previous().lexeme());
        std::string clean;
        int base = 10;
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            for (size_t i = 2; i < s.size(); i++) if (s[i] != '_') clean += s[i];
            base = 16;
        } else if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
            for (size_t i = 2; i < s.size(); i++) if (s[i] != '_') clean += s[i];
            base = 2;
        } else if (s.size() > 2 && s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) {
            for (size_t i = 2; i < s.size(); i++) if (s[i] != '_') clean += s[i];
            base = 8;
        } else {
            for (char c : s) if (c != '_') clean += c;
        }
        int64_t parsed = 0;
        if (!parseIntLiteralChecked(clean, base, parsed)) {
            // Out-of-range or malformed integer: emit diagnostic, keep AST well-formed.
            error(previous(), "integer literal out of range");
            parsed = 0;
        }
        lit->value = parsed;
        return lit;
    }

    if (match(TokenType::FLOAT)) {
        auto lit = std::make_unique<FloatLiteral>();
        lit->setLocation(previous().location());
        std::string s(previous().lexeme());
        std::string clean;
        for (char c : s) if (c != '_') clean += c;
        double parsed = 0.0;
        if (!parseFloatLiteralChecked(clean, parsed)) {
            // Out-of-range or malformed float: emit diagnostic, keep AST well-formed.
            error(previous(), "float literal out of range");
            parsed = 0.0;
        }
        lit->value = parsed;
        return lit;
    }

    if (match(TokenType::TEMPLATE)) {
        auto loc = previous().location();
        std::string raw = previous().lexeme();

        // Decode content type: "TYPE\0body" = typed, "body" = untyped
        std::string contentType;
        std::string body;
        auto nullPos = raw.find('\0');
        if (nullPos != std::string::npos) {
            contentType = raw.substr(0, nullPos);
            body = raw.substr(nullPos + 1);
        } else {
            body = std::move(raw);
        }

        // template[X]("file.html") - typed file template
        // Body is empty because Lexer didn't scan a { block
        if (!contentType.empty() && body.empty() &&
            check(TokenType::LEFT_PAREN)) {
            advance(); // consume "("
            if (!check(TokenType::STRING)) {
                error("template() argument must be a string literal");
                return nullptr;
            }
            auto lexeme = current().lexeme();
            std::string filePath;
            if (lexeme.size() >= 2) {
                filePath = std::string(lexeme.substr(1, lexeme.size() - 2));
            }
            advance(); // consume string literal
            consume(TokenType::RIGHT_PAREN, "Expected ')' after template file path");
            auto expr = std::make_unique<TemplateFileExpr>();
            expr->setLocation(loc);
            expr->filePath = filePath;
            expr->contentType = std::move(contentType);
            return expr;
        }

        auto expr = std::make_unique<TemplateExpr>();
        expr->setLocation(loc);
        expr->body = std::move(body);
        expr->contentType = std::move(contentType);
        expr->templateParts = parseTemplateBody(
            expr->body, loc, /*isDragonFile=*/true);
        return expr;
    }

    // D017 Phase 4.B - `:{ ... }` content alias inside a `!{}` block-interp.
    // The Lexer only emits TEMPLATE_CONTENT_OPEN when re-lexing the body of
    // an !{} block (LexerOptions.inTemplateInterpolation). The body has
    // already been captured as the token's lexeme. ContentType is left empty
    // here - CodeGen inherits the enclosing template's type from its
    // context stack at emit time. `isContentAlias` distinguishes this from
    // an explicit untyped `template { ... }`, which matters at statement
    // position: a `:{}` ExprStmt appends to the block buffer; a regular
    // ExprStmt does not.
    if (match(TokenType::TEMPLATE_CONTENT_OPEN)) {
        auto loc = previous().location();
        auto expr = std::make_unique<TemplateExpr>();
        expr->setLocation(loc);
        expr->body = previous().lexeme();
        expr->isContentAlias = true;
        expr->templateParts = parseTemplateBody(
            expr->body, loc, /*isDragonFile=*/true);
        return expr;
    }

    // Contextual keyword: template("file.html") - untyped compile-time file template
    if (check(TokenType::IDENTIFIER) && current().lexeme() == "template" &&
        peekNext().type() == TokenType::LEFT_PAREN) {
        auto loc = current().location();
        advance(); // consume "template"
        advance(); // consume "("
        if (!check(TokenType::STRING)) {
            error("template() argument must be a string literal");
            return nullptr;
        }
        auto lexeme = current().lexeme();
        // Strip quotes from string literal
        std::string filePath;
        if (lexeme.size() >= 2) {
            filePath = std::string(lexeme.substr(1, lexeme.size() - 2));
        }
        advance(); // consume string literal
        consume(TokenType::RIGHT_PAREN, "Expected ')' after template file path");
        auto expr = std::make_unique<TemplateFileExpr>();
        expr->setLocation(loc);
        expr->filePath = filePath;
        return expr;
    }

    if (match(TokenType::STRING)) {
        auto lit = std::make_unique<StringLiteral>();
        lit->setLocation(previous().location());
        auto lexeme = previous().lexeme();
        size_t start = 0;
        while (start < lexeme.size() && (lexeme[start] == 'f' || lexeme[start] == 'r' ||
               lexeme[start] == 'b' || lexeme[start] == 'F' || lexeme[start] == 'R' ||
               lexeme[start] == 'B')) {
            if (lexeme[start] == 'f' || lexeme[start] == 'F') lit->isFString = true;
            if (lexeme[start] == 'r' || lexeme[start] == 'R') lit->isRaw = true;
            if (lexeme[start] == 'b' || lexeme[start] == 'B') lit->isBytes = true;
            start++;
        }
        if (start + 5 < lexeme.size() &&
            ((lexeme.substr(start, 3) == "\"\"\"") || (lexeme.substr(start, 3) == "'''"))) {
            lit->value = std::string(lexeme.substr(start + 3, lexeme.size() - start - 6));
        } else if (start < lexeme.size()) {
            lit->value = std::string(lexeme.substr(start + 1, lexeme.size() - start - 2));
        }
        // Parse f-string interpolations once, here, into a structured AST so
        // Sema (capture analysis), TypeChecker, and CodeGen all walk the same
        // tree - without each stage re-lexing the raw text. See Decision 030
        // (one source of truth) and the closure-capture fix.
        if (lit->isFString) {
            const std::string& v = lit->value;
            std::string buf;
            size_t i = 0;
            while (i < v.size()) {
                if (v[i] == '{' && i + 1 < v.size() && v[i + 1] == '{') {
                    buf.push_back('{');
                    i += 2;
                } else if (v[i] == '}' && i + 1 < v.size() && v[i + 1] == '}') {
                    buf.push_back('}');
                    i += 2;
                } else if (v[i] == '{') {
                    if (!buf.empty()) {
                        FStringPart litPart;
                        litPart.kind = FStringPart::Kind::Literal;
                        litPart.literal = std::move(buf);
                        lit->fstringParts.push_back(std::move(litPart));
                        buf.clear();
                    }
                    size_t exprStart = i + 1;
                    int depth = 1;
                    size_t j = exprStart;
                    while (j < v.size() && depth > 0) {
                        if (v[j] == '{') depth++;
                        else if (v[j] == '}') depth--;
                        if (depth > 0) j++;
                    }
                    std::string exprText = v.substr(exprStart, j - exprStart);
                    i = j + 1;

                    // Split out !conversion (only at top level) and :format_spec.
                    std::string conversionSpec;
                    std::string formatSpec;
                    {
                        int parenDepth = 0;
                        for (size_t fi = 0; fi < exprText.size(); fi++) {
                            char c = exprText[fi];
                            if (c == '(' || c == '[' || c == '{') parenDepth++;
                            else if (c == ')' || c == ']' || c == '}') parenDepth--;
                            else if (parenDepth == 0 && c == ':') {
                                formatSpec = exprText.substr(fi + 1);
                                exprText = exprText.substr(0, fi);
                                break;
                            }
                        }
                    }
                    if (exprText.size() >= 2 && exprText[exprText.size() - 2] == '!') {
                        char convChar = exprText.back();
                        if (convChar == 's' || convChar == 'r' || convChar == 'a') {
                            conversionSpec = std::string(1, convChar);
                            exprText.resize(exprText.size() - 2);
                        }
                    }

                    LexerOptions fLexOpts;
                    fLexOpts.filename = "<fstring>";
                    fLexOpts.useBraceBlocks = impl_->options.isDragonFile;
                    Lexer fLexer(exprText, fLexOpts);
                    auto fTokens = fLexer.tokenize();
                    ParserOptions fOpts;
                    fOpts.isDragonFile = impl_->options.isDragonFile;
                    fOpts.requireTypes = impl_->options.requireTypes;
                    fOpts.filename = "<fstring>";
                    Parser fParser(std::move(fTokens), fOpts);
                    auto fExpr = fParser.parseExpression();

                    FStringPart exprPart;
                    exprPart.kind = FStringPart::Kind::Expression;
                    if (fExpr && !fParser.hasErrors()) {
                        fExpr->setLocation(lit->location());
                        exprPart.expr = std::move(fExpr);
                    }
                    exprPart.formatSpec = std::move(formatSpec);
                    if (!conversionSpec.empty()) exprPart.conversion = conversionSpec[0];
                    lit->fstringParts.push_back(std::move(exprPart));
                } else {
                    buf.push_back(v[i]);
                    i++;
                }
            }
            if (!buf.empty()) {
                FStringPart litPart;
                litPart.kind = FStringPart::Kind::Literal;
                litPart.literal = std::move(buf);
                lit->fstringParts.push_back(std::move(litPart));
            }
        }
        return lit;
    }

    if (match(TokenType::TRUE)) {
        auto lit = std::make_unique<BooleanLiteral>();
        lit->setLocation(previous().location());
        lit->value = true;
        return lit;
    }
    if (match(TokenType::FALSE)) {
        auto lit = std::make_unique<BooleanLiteral>();
        lit->setLocation(previous().location());
        lit->value = false;
        return lit;
    }

    if (match(TokenType::NONE)) {
        auto lit = std::make_unique<NoneLiteral>();
        lit->setLocation(previous().location());
        return lit;
    }

    if (match(TokenType::IDENTIFIER)) {
        auto name = std::make_unique<NameExpr>();
        name->setLocation(previous().location());
        name->name = std::string(previous().lexeme());
        // Check for walrus operator: name := value
        if (match(TokenType::WALRUS)) {
            auto walrus = std::make_unique<WalrusExpr>();
            walrus->name = name->name;
            walrus->value = expression();
            return walrus;
        }
        return name;
    }

    if (match(TokenType::LEFT_PAREN)) {
        if (match(TokenType::RIGHT_PAREN)) {
            return std::make_unique<TupleExpr>();
        }
        auto expr = expression();
        // Check for generator expression: (expr for var in iterable)
        if (check(TokenType::FOR)) {
            auto gen = std::make_unique<GeneratorExpr>();
            gen->element = std::move(expr);
            consume(TokenType::FOR, "Expect 'for' in generator expression");
            auto target = primary();
            if (auto* name = dynamic_cast<NameExpr*>(target.get())) {
                gen->varName = name->name;
            }
            consume(TokenType::IN, "Expect 'in' in generator expression");
            gen->iterable = orExpr();
            if (match(TokenType::IF)) {
                gen->condition = orExpr();
            }
            // Parse extra clauses for nested comprehensions
            while (check(TokenType::FOR)) {
                CompClause clause;
                advance(); // consume 'for'
                auto extraTarget = primary();
                if (auto* name = dynamic_cast<NameExpr*>(extraTarget.get())) {
                    clause.varNames.push_back(name->name);
                }
                while (match(TokenType::COMMA)) {
                    auto next = primary();
                    if (auto* name = dynamic_cast<NameExpr*>(next.get())) {
                        clause.varNames.push_back(name->name);
                    }
                }
                consume(TokenType::IN, "Expect 'in' in comprehension clause");
                clause.iterable = orExpr();
                if (match(TokenType::IF)) {
                    clause.condition = orExpr();
                }
                gen->extraClauses.push_back(std::move(clause));
            }
            consume(TokenType::RIGHT_PAREN, "Expect ')' after generator expression");
            return gen;
        }
        if (match(TokenType::COMMA)) {
            auto tup = std::make_unique<TupleExpr>();
            if (expr) tup->elements.push_back(std::move(expr));
            while (!check(TokenType::RIGHT_PAREN) && !isAtEnd()) {
                tup->elements.push_back(expression());
                if (!match(TokenType::COMMA)) break;
            }
            consume(TokenType::RIGHT_PAREN, "Expect ')' after tuple");
            return tup;
        }
        consume(TokenType::RIGHT_PAREN, "Expect ')' after expression");
        return expr;
    }

    if (match(TokenType::LEFT_BRACKET)) {
        return parseList();
    }

    if (match(TokenType::LEFT_BRACE)) {
        return parseDict();
    }

    if (match(TokenType::LAMBDA)) {
        return parseLambda();
    }

    if (match(TokenType::YIELD)) {
        return parseYield();
    }

    error("Expected expression");
    return nullptr;
}

//===----------------------------------------------------------------------===//
// Literal Parsers
//===----------------------------------------------------------------------===//

std::unique_ptr<Expr> Parser::parseLambda() {
    auto lam = std::make_unique<LambdaExpr>();
    if (match(TokenType::LEFT_PAREN)) {
        if (!check(TokenType::RIGHT_PAREN)) {
            do {
                // Trailing comma before ')', e.g. lambda(x, y,) { ... }.
                if (check(TokenType::RIGHT_PAREN)) break;
                LambdaExpr::Parameter param;
                param.name = std::string(consume(TokenType::IDENTIFIER, "Expect parameter name").lexeme());
                if (match(TokenType::COLON)) param.type = parseType();
                if (match(TokenType::EQUAL)) param.defaultValue = expression();
                lam->params.push_back(std::move(param));
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RIGHT_PAREN, "Expect ')'");
        if (match(TokenType::ARROW)) lam->returnType = parseType();
        if (check(TokenType::LEFT_BRACE)) {
            lam->bodyStmts = parseBlock();
        } else {
            lam->body = expression();
        }
    } else {
        if (!check(TokenType::COLON)) {
            do {
                // Trailing comma before ':', e.g. lambda x, y,: x + y.
                if (check(TokenType::COLON)) break;
                LambdaExpr::Parameter param;
                param.name = std::string(consume(TokenType::IDENTIFIER, "Expect parameter name").lexeme());
                if (match(TokenType::EQUAL)) param.defaultValue = expression();
                lam->params.push_back(std::move(param));
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::COLON, "Expect ':' in lambda");
        lam->body = expression();
    }
    return lam;
}

std::unique_ptr<Expr> Parser::parseList() {
    if (match(TokenType::RIGHT_BRACKET)) {
        return std::make_unique<ListExpr>();
    }
    // Check for *expr spread at first element
    std::unique_ptr<Expr> first;
    if (match(TokenType::STAR)) {
        auto starred = std::make_unique<StarredExpr>();
        starred->value = expression();
        starred->isDoubleStar = false;
        first = std::move(starred);
    } else {
        first = expression();
    }
    if (check(TokenType::FOR)) {
        // List comprehension: [expr for var in iterable] or [expr for var in iterable if cond]
        auto comp = std::make_unique<ListCompExpr>();
        comp->element = std::move(first);
        consume(TokenType::FOR, "Expect 'for' in list comprehension");
        auto target = primary();
        if (auto* name = dynamic_cast<NameExpr*>(target.get())) {
            comp->varName = name->name;
        }
        consume(TokenType::IN, "Expect 'in' in list comprehension");
        // Use orExpr() to avoid consuming 'if' as part of a ternary expression
        comp->iterable = orExpr();
        if (match(TokenType::IF)) {
            comp->condition = orExpr();
        }
        // Parse extra clauses for nested comprehensions
        while (check(TokenType::FOR)) {
            CompClause clause;
            advance(); // consume 'for'
            auto extraTarget = primary();
            if (auto* name = dynamic_cast<NameExpr*>(extraTarget.get())) {
                clause.varNames.push_back(name->name);
            }
            while (match(TokenType::COMMA)) {
                auto next = primary();
                if (auto* name = dynamic_cast<NameExpr*>(next.get())) {
                    clause.varNames.push_back(name->name);
                }
            }
            consume(TokenType::IN, "Expect 'in' in comprehension clause");
            clause.iterable = orExpr();
            if (match(TokenType::IF)) {
                clause.condition = orExpr();
            }
            comp->extraClauses.push_back(std::move(clause));
        }
        consume(TokenType::RIGHT_BRACKET, "Expect ']' after list comprehension");
        return comp;
    }
    auto list = std::make_unique<ListExpr>();
    if (first) list->elements.push_back(std::move(first));
    while (match(TokenType::COMMA)) {
        if (check(TokenType::RIGHT_BRACKET)) break;
        if (match(TokenType::STAR)) {
            auto starred = std::make_unique<StarredExpr>();
            starred->value = expression();
            starred->isDoubleStar = false;
            list->elements.push_back(std::move(starred));
        } else {
            list->elements.push_back(expression());
        }
    }
    consume(TokenType::RIGHT_BRACKET, "Expect ']' after list");
    return list;
}

std::unique_ptr<Expr> Parser::parseDict() {
    // Newlines inside a { } collection literal are insignificant, but the lexer
    // deliberately does NOT suppress them here the way it does inside ( ) and
    // [ ] - it cannot tell a dict/set literal from a code block. So skip them at
    // every structural point, giving multi-line dict/set literals the implicit
    // line continuation developers expect.
    skipNewlines();
    if (match(TokenType::RIGHT_BRACE)) {
        return std::make_unique<DictExpr>();
    }

    // Helper: make a StringLiteral from a bare identifier
    auto makeBareKey = [](const std::string& name) -> std::unique_ptr<Expr> {
        auto lit = std::make_unique<StringLiteral>();
        lit->value = name;
        return lit;
    };

    // Helper: parse a dict key in .dr mode (bare-key + computed key support)
    auto parseDictKey = [&]() -> std::unique_ptr<Expr> {
        skipNewlines();
        if (impl_->options.isDragonFile) {
            // Bare key: identifier followed by colon -> string literal
            if (check(TokenType::IDENTIFIER) && peekNext().type() == TokenType::COLON) {
                std::string name = std::string(peek().lexeme());
                advance(); // consume identifier
                return makeBareKey(name);
            }
            // Computed key: (expr) -> evaluate expression
            if (match(TokenType::LEFT_PAREN)) {
                auto key = expression();
                consume(TokenType::RIGHT_PAREN, "Expect ')' after computed dict key");
                return key;
            }
        }
        return expression();
    };

    // **expr spread: {**other_dict, ...} -> merge into result dict
    if (match(TokenType::POWER)) {
        auto spreadVal = expression();
        auto dict = std::make_unique<DictExpr>();
        // null key = spread entry sentinel
        dict->entries.emplace_back(nullptr, std::move(spreadVal));
        skipNewlines();
        while (match(TokenType::COMMA)) {
            skipNewlines();
            if (check(TokenType::RIGHT_BRACE)) break;
            if (match(TokenType::POWER)) {
                auto sv = expression();
                dict->entries.emplace_back(nullptr, std::move(sv));
            } else {
                auto k = parseDictKey();
                consume(TokenType::COLON, "Expect ':' in dict");
                auto v = expression();
                dict->entries.emplace_back(std::move(k), std::move(v));
            }
            skipNewlines();
        }
        consume(TokenType::RIGHT_BRACE, "Expect '}' after dict");
        return dict;
    }

    // For the first entry, we need to handle both dict and set/set-comp.
    // In .dr mode, try bare-key/computed-key first if it looks like a dict.
    std::unique_ptr<Expr> first;
    bool firstWasBareKey = false;
    std::string firstBareName;
    if (impl_->options.isDragonFile &&
        check(TokenType::IDENTIFIER) && peekNext().type() == TokenType::COLON) {
        // Bare key for first entry
        std::string name = std::string(peek().lexeme());
        advance();
        first = makeBareKey(name);
        firstWasBareKey = true;
        firstBareName = name;
    } else if (impl_->options.isDragonFile && check(TokenType::LEFT_PAREN)) {
        // Could be computed key OR a parenthesized expression for a set.
        // Save position, try computed key, check if COLON follows.
        size_t savePos = impl_->current;
        advance(); // consume (
        auto tryKey = expression();
        if (match(TokenType::RIGHT_PAREN) && check(TokenType::COLON)) {
            // It's a computed dict key
            first = std::move(tryKey);
        } else {
            // Not a computed key - rewind and parse as normal expression
            impl_->current = savePos;
            first = expression();
        }
    } else {
        first = expression();
    }

    skipNewlines();
    if (match(TokenType::COLON)) {
        auto val = orExpr();
        skipNewlines();
        // Check for dict comprehension: {k: v for k in iterable}
        if (check(TokenType::FOR)) {
            auto comp = std::make_unique<DictCompExpr>();
            // In a comprehension the first "key" is the loop variable, not a
            // literal field name. If it was greedily parsed as a bare-key string
            // literal (the `.dr` `{name: v}` shorthand), rebuild it as a NameExpr
            // so `{k: ... for k in xs}` reads k's value, not the string "k".
            if (firstWasBareKey) {
                auto keyName = std::make_unique<NameExpr>();
                keyName->name = firstBareName;
                comp->key = std::move(keyName);
            } else {
                comp->key = std::move(first);
            }
            comp->value = std::move(val);
            consume(TokenType::FOR, "Expect 'for' in dict comprehension");
            auto target = primary();
            if (auto* name = dynamic_cast<NameExpr*>(target.get())) {
                comp->varNames.push_back(name->name);
            }
            // Check for tuple unpacking: for k, v in ...
            while (match(TokenType::COMMA)) {
                auto next = primary();
                if (auto* name = dynamic_cast<NameExpr*>(next.get())) {
                    comp->varNames.push_back(name->name);
                }
            }
            consume(TokenType::IN, "Expect 'in' in dict comprehension");
            comp->iterable = orExpr();
            if (match(TokenType::IF)) {
                comp->condition = orExpr();
            }
            // Parse extra clauses for nested comprehensions
            while (check(TokenType::FOR)) {
                CompClause clause;
                advance(); // consume 'for'
                auto extraTarget = primary();
                if (auto* name = dynamic_cast<NameExpr*>(extraTarget.get())) {
                    clause.varNames.push_back(name->name);
                }
                while (match(TokenType::COMMA)) {
                    auto next2 = primary();
                    if (auto* name = dynamic_cast<NameExpr*>(next2.get())) {
                        clause.varNames.push_back(name->name);
                    }
                }
                consume(TokenType::IN, "Expect 'in' in comprehension clause");
                clause.iterable = orExpr();
                if (match(TokenType::IF)) {
                    clause.condition = orExpr();
                }
                comp->extraClauses.push_back(std::move(clause));
            }
            skipNewlines();
            consume(TokenType::RIGHT_BRACE, "Expect '}' after dict comprehension");
            return comp;
        }
        auto dict = std::make_unique<DictExpr>();
        dict->entries.emplace_back(std::move(first), std::move(val));
        skipNewlines();
        while (match(TokenType::COMMA)) {
            skipNewlines();
            if (check(TokenType::RIGHT_BRACE)) break;
            if (match(TokenType::POWER)) {
                // **expr spread inside dict
                auto sv = expression();
                dict->entries.emplace_back(nullptr, std::move(sv));
            } else {
                auto k = parseDictKey();
                consume(TokenType::COLON, "Expect ':' in dict");
                auto v = expression();
                dict->entries.emplace_back(std::move(k), std::move(v));
            }
            skipNewlines();
        }
        consume(TokenType::RIGHT_BRACE, "Expect '}' after dict");
        return dict;
    }
    // Check for set comprehension: {expr for var in iterable}
    if (check(TokenType::FOR)) {
        auto comp = std::make_unique<SetCompExpr>();
        comp->element = std::move(first);
        consume(TokenType::FOR, "Expect 'for' in set comprehension");
        auto target = primary();
        if (auto* name = dynamic_cast<NameExpr*>(target.get())) {
            comp->varName = name->name;
        }
        consume(TokenType::IN, "Expect 'in' in set comprehension");
        comp->iterable = orExpr();
        if (match(TokenType::IF)) {
            comp->condition = orExpr();
        }
        // Parse extra clauses for nested comprehensions
        while (check(TokenType::FOR)) {
            CompClause clause;
            advance(); // consume 'for'
            auto extraTarget = primary();
            if (auto* name = dynamic_cast<NameExpr*>(extraTarget.get())) {
                clause.varNames.push_back(name->name);
            }
            while (match(TokenType::COMMA)) {
                auto next = primary();
                if (auto* name = dynamic_cast<NameExpr*>(next.get())) {
                    clause.varNames.push_back(name->name);
                }
            }
            consume(TokenType::IN, "Expect 'in' in comprehension clause");
            clause.iterable = orExpr();
            if (match(TokenType::IF)) {
                clause.condition = orExpr();
            }
            comp->extraClauses.push_back(std::move(clause));
        }
        skipNewlines();
        consume(TokenType::RIGHT_BRACE, "Expect '}' after set comprehension");
        return comp;
    }
    auto set = std::make_unique<SetExpr>();
    if (first) set->elements.push_back(std::move(first));
    skipNewlines();
    while (match(TokenType::COMMA)) {
        skipNewlines();
        if (check(TokenType::RIGHT_BRACE)) break;
        set->elements.push_back(expression());
        skipNewlines();
    }
    skipNewlines();
    consume(TokenType::RIGHT_BRACE, "Expect '}' after set");
    return set;
}

std::unique_ptr<Expr> Parser::parseTuple() { return nullptr; }
std::unique_ptr<Expr> Parser::parseSet() { return nullptr; }
std::unique_ptr<Expr> Parser::parseListComp() { return nullptr; }
std::unique_ptr<Expr> Parser::parseDictComp() { return nullptr; }

std::unique_ptr<Expr> Parser::parseYield() {
    auto yld = std::make_unique<YieldExpr>();
    if (match(TokenType::FROM)) {
        yld->isYieldFrom = true;
        yld->value = expression();
    } else if (!check(TokenType::NEWLINE) && !check(TokenType::RIGHT_PAREN) && !isAtEnd()) {
        yld->value = expression();
    }
    return yld;
}

//===----------------------------------------------------------------------===//
// Statement Parsing
//===----------------------------------------------------------------------===//


} // namespace dragon
