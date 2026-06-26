# 003 -- Dragon Parser: Recursive Descent with Precedence Climbing

> **Source files:** `include/dragon/Parser.h`, `src/Parser.cpp` (expressions + plumbing), `src/ParserStmts.cpp` (statements + declarations), `src/ParserImpl.h` (shared pimpl + helpers)
> **Last Updated:** 2026-06-22
> **Test suite:** ParserTests (196 tests)

---

## 1. Overview

Dragon's parser is a **hand-written recursive descent parser** that transforms a token stream (produced by the Lexer) into an Abstract Syntax Tree. It handles both syntax modes of Dragon:

- **`.dr` files** -- curly-brace-delimited blocks; the parser itself rejects a missing annotation
- **`.py` files** -- indentation-based blocks; the parser *permits* an omitted annotation, but the separate `TypeHintEnforcer` pass rejects it, so `.py` is just as statically typed (only the enforcement site differs)

The parser uses **operator precedence climbing** for expression parsing: each precedence level is implemented as a separate function that delegates to the next-higher-precedence function, forming a call chain from loosest to tightest binding.

The parser is defined in `include/dragon/Parser.h`. The implementation is split across two translation units to stay under the file-size policy: `src/Parser.cpp` (~1233 lines: expression parsing plus token-management plumbing) and `src/ParserStmts.cpp` (statement and declaration parsing). The shared pimpl struct and the small literal/docstring helpers live in `src/ParserImpl.h` so both translation units reference one copy of the state. It follows the **pimpl idiom**: all mutable state lives inside `struct Parser::Impl`, and the `Parser` class holds a `std::unique_ptr<Impl>`.

---

## 2. The Impl Struct

All parser state is encapsulated in a private implementation struct:

```cpp
// src/ParserImpl.h
struct Parser::Impl {
    std::vector<Token> tokens;       // The complete token stream from the lexer
    ParserOptions options;           // Configuration (isDragonFile, requireTypes, filename)
    size_t current = 0;              // Index of the current token being examined
    bool inClassBody = false;        // True while parsing a class body
    std::vector<ParserDiagnostic> diagnostics;  // Accumulated error/warning messages
    std::vector<std::unique_ptr<Stmt>> pendingStmts;  // Extra stmts from multi-decl constructs
    int recursionDepth = 0;          // Live nesting depth for the recursion guard
    static constexpr int kMaxRecursionDepth = 500;    // Cap that aborts runaway nesting
};
```

### Field details

| Field | Type | Purpose |
|-------|------|---------|
| `tokens` | `std::vector<Token>` | The full token stream, moved in from the constructor. Includes the trailing `END_OF_FILE` token. |
| `options` | `ParserOptions` | Controls syntax mode, type requirement enforcement, and the filename used in error messages. |
| `current` | `size_t` | A cursor index into `tokens`. Starts at 0 and advances as the parser consumes tokens. |
| `inClassBody` | `bool` | True while a class body is being parsed, so method declarations can be flagged. |
| `diagnostics` | `std::vector<ParserDiagnostic>` | Every error and warning encountered during parsing is pushed here. Checked via `hasErrors()`. |
| `pendingStmts` | `std::vector<std::unique_ptr<Stmt>>` | Overflow slot for multi-declaration constructs (e.g. `extern "C" from "lib" { ... }`). `parseModule()` drains it after each statement. |
| `recursionDepth` / `kMaxRecursionDepth` | `int` / `static constexpr int` | The compiler is exposed to user input via dragonlang.org, so `expression()` and `statement()` bump `recursionDepth` through a `ParserRecursionGuard` (RAII, defined in `ParserImpl.h`) and bail with a diagnostic once it exceeds `kMaxRecursionDepth` (500), preventing a `(((...)))`-style stack-overflow attack. |

The `ParserRecursionGuard` increments `recursionDepth` on entry and decrements it on scope exit, so even an error-recovery early return cannot leak depth. It is declared `inline` at namespace scope in `ParserImpl.h` so both translation units share one type rather than two ODR-distinct copies.

Note that the only contextual flag the Impl struct carries is `inClassBody` (used to mark method declarations). The parser does not track `inLoop`/`inFunction` and does not validate break/continue placement or return-outside-function. That validation is deferred to the semantic analysis pass (Sema).

---

## 3. The ParserOptions Struct

```cpp
// include/dragon/Parser.h, line 21
struct ParserOptions {
    bool isDragonFile = true;    // true = .dr (curly braces), false = .py (indentation)
    bool requireTypes = true;    // true = type annotations mandatory, false = optional
    std::string filename = "<stdin>";  // Used in error messages and SourceLocation
};
```

These options are set by the Driver based on file extension. A `.dr` file sets `isDragonFile = true` and `requireTypes = true`. A `.py` file sets both to `false`. The `filename` field propagates into every `SourceLocation` on every AST node produced by this parse.

---

## 4. Token Consumption Methods

The parser interacts with the token stream through a set of accessor and consumption methods. These are the fundamental building blocks that all parsing functions use.

### `advance()` -- Consume and return

```cpp
Token Parser::advance() {
    if (!isAtEnd()) impl_->current++;
    return previous();
}
```

Moves the cursor forward by one and returns the **previously current** token (the one just consumed). If already at `END_OF_FILE`, the cursor does not advance.

### `current()` and `peek()` -- Look at the current token

```cpp
Token Parser::current() const { return impl_->tokens[impl_->current]; }
Token Parser::peek() const    { return impl_->tokens[impl_->current]; }
```

These are synonymous. Both return the token at the current cursor position without advancing. The parser predominantly uses `peek()`.

### `peekNext()` -- Look one token ahead

```cpp
Token Parser::peekNext() const {
    if (impl_->current + 1 >= impl_->tokens.size()) return impl_->tokens.back();
    return impl_->tokens[impl_->current + 1];
}
```

Returns the token one position beyond `current`. Used for two-token lookahead, for example when detecting keyword arguments (`IDENTIFIER` followed by `EQUAL`) in function calls.

### `previous()` -- The last consumed token

```cpp
Token Parser::previous() const { return impl_->tokens[impl_->current - 1]; }
```

Returns the token immediately before the cursor. Called after `advance()` or `match()` to get the token that was just consumed.

### `check(type)` -- Non-consuming type test

```cpp
bool Parser::check(TokenType type) const {
    if (isAtEnd()) return false;
    return peek().type() == type;
}
```

Returns true if the current token is of the given type, without consuming it. Returns false at end-of-file regardless of the type argument.

### `match(type)` -- Conditional consume

```cpp
bool Parser::match(TokenType type) {
    if (check(type)) { advance(); return true; }
    return false;
}
```

If the current token matches the type, consumes it and returns true. Otherwise returns false and leaves the cursor unchanged. There is also a variadic template overload:

```cpp
template<typename... Types>
bool match(Types... types) { return (match(types) || ...); }
```

This uses a C++17 fold expression to try each type in order, short-circuiting on the first match.

### `consume(type, message)` -- Required consume

```cpp
Token Parser::consume(TokenType type, const std::string& message) {
    if (check(type)) return advance();
    error(message);
    return Token();  // Default-constructed token (type = END_OF_FILE, empty lexeme)
}
```

Like `match()` but mandatory. If the expected token is not present, an error diagnostic is emitted and a default-constructed `Token` is returned. The parser continues (does not throw).

### `isAtEnd()` -- End-of-file check

```cpp
bool Parser::isAtEnd() const {
    return peek().type() == TokenType::END_OF_FILE;
}
```

Returns true when the cursor has reached the sentinel `END_OF_FILE` token.

---

## 5. Expression Parsing with Precedence Climbing

Expression parsing is the core of the parser. Dragon uses the same operator precedence as Python, implemented as a chain of mutually recursive functions. Each function handles one precedence level and delegates to the next tighter level.

### The Complete Precedence Chain (lowest to highest)

```
expression()       -- entry point, delegates to assignment()
  assignment()     -- delegates to ternary() [assignment handled in expressionStatement()]
    ternary()      -- a if cond else b
      orExpr()     -- left-associative: x or y
        andExpr()  -- left-associative: x and y
          notExpr()       -- prefix: not x (recursive for chaining: not not x)
            comparison()  -- left-associative: <, >, <=, >=, ==, !=, in, is
              bitwiseOr() -- left-associative: x | y
                bitwiseXor()  -- left-associative: x ^ y
                  bitwiseAnd()  -- left-associative: x & y
                    shift()     -- left-associative: x << y, x >> y
                      term()    -- left-associative: x + y, x - y
                        factor()  -- left-associative: x * y, x / y, x % y, x // y
                          unary()   -- prefix: -x, +x, ~x (recursive)
                            power()   -- RIGHT-associative: x ** y
                              fireExpr()   -- prefix: fire fn() / fire { block }
                                awaitExpr()  -- prefix: await x
                                  call()     -- postfix: f(), obj.attr, arr[i]
                                    primary()  -- literals, names, parens, containers
```

### Precedence Level Details

#### Level 1: `expression()` and `assignment()`

```cpp
std::unique_ptr<Expr> Parser::expression() {
    ParserRecursionGuard guard(impl_->recursionDepth);
    if (impl_->recursionDepth > Impl::kMaxRecursionDepth) {
        // Cap fired: emit a diagnostic, return a benign placeholder, and
        // synchronize past the over-nested expression.
        error(peek(), "expression nesting too deep");
        auto stub = std::make_unique<IntegerLiteral>();
        stub->setLocation(peek().location());
        stub->value = 0;
        synchronize();
        return stub;
    }
    return assignment();
}
std::unique_ptr<Expr> Parser::assignment() { return ternary(); }
```

`expression()` is the public entry point. Before delegating, it bumps `recursionDepth` through a `ParserRecursionGuard` and bails with an `"expression nesting too deep"` diagnostic once the depth exceeds `kMaxRecursionDepth` (500), returning a `0` placeholder so callers that do not uniformly null-check keep working. `assignment()` just delegates to `ternary()`. Actual assignment (`=`, `:`, `+=`, etc.) is handled at the statement level in `expressionStatement()`, not as an expression. This is a deliberate design choice: in Dragon, assignment is a statement, not an expression (unlike C where `a = b` is an expression).

#### Level 2: `ternary()` -- Conditional expressions

```cpp
std::unique_ptr<Expr> Parser::ternary() {
    auto expr = orExpr();
    if (match(TokenType::IF)) {
        auto node = std::make_unique<IfExpr>();
        node->thenExpr = std::move(expr);     // The value-if-true was parsed FIRST
        node->condition = orExpr();            // Condition parsed SECOND
        consume(TokenType::ELSE, "Expect 'else' in ternary expression");
        node->elseExpr = ternary();            // Right-recursive for chaining
        return node;
    }
    return expr;
}
```

Python's ternary has the unusual form `value_if_true if condition else value_if_false`. The "then" expression is parsed first (as an `orExpr`), then the `if` keyword is detected, the condition is parsed, and finally the `else` branch. Note that `elseExpr` delegates to `ternary()` recursively, making the ternary operator **right-associative**: `a if c1 else b if c2 else d` parses as `a if c1 else (b if c2 else d)`.

#### Levels 3-4: `orExpr()` and `andExpr()` -- Logical operators

Both use the standard left-associative binary loop pattern:

```cpp
std::unique_ptr<Expr> Parser::orExpr() {
    auto expr = andExpr();
    while (match(TokenType::OR)) {
        auto bin = std::make_unique<BinaryExpr>();
        bin->left = std::move(expr);
        bin->op = previous();
        bin->right = andExpr();
        expr = std::move(bin);
    }
    return expr;
}
```

The `while` loop handles chaining: `a or b or c` becomes `((a or b) or c)`.

#### Level 5: `notExpr()` -- Logical negation

```cpp
std::unique_ptr<Expr> Parser::notExpr() {
    if (match(TokenType::NOT)) {
        auto un = std::make_unique<UnaryExpr>();
        un->op = previous();
        un->operand = notExpr();  // Recursive: not not x
        return un;
    }
    return comparison();
}
```

`not` is a prefix unary operator. It recurses into itself to handle `not not x`, which produces `UnaryExpr(not, UnaryExpr(not, x))`.

#### Level 6: `comparison()` -- Comparison operators and chained comparisons

`comparison()` parses the operand with `bitwiseOr()`, then uses a local `tryConsumeCompOp` lambda to recognize a comparison operator at the cursor. The recognized operators are `<`, `>`, `<=`, `>=`, `==`, `!=`, `in`, `is`, plus the two-word forms `not in` and `is not`. The two-word forms are detected with one-token lookahead (`peekNext()`) and synthesized into a single `Token` of type `NOT_IN` / `IS_NOT`, carrying the location of the first word and the lexeme `"not in"` / `"is not"` so AST printing and diagnostics render them faithfully. (A bare prefix `not` returns false from the lambda, leaving it to `notExpr()`.)

If no operator is found, the bare operand is returned. Otherwise the parser collects operands and operators into two parallel vectors via a loop. The shape of the result depends on how many operators were seen:

- **Exactly one operator**: a plain `BinaryExpr` (left operand, op, right operand).
- **Two or more operators**: a dedicated `ChainedCompExpr` carrying the `operands` and `operators` vectors.

```cpp
if (operators.size() == 1) {
    auto bin = std::make_unique<BinaryExpr>();
    bin->left = std::move(operands[0]);
    bin->op = operators[0];
    bin->right = std::move(operands[1]);
    return bin;
}
auto chain = std::make_unique<ChainedCompExpr>();
chain->operands = std::move(operands);
chain->operators = std::move(operators);
return chain;
```

This means Python's chained comparisons (`a < b < c`) *are* lowered here -- not into a nested `BinaryExpr` tree, but into a single `ChainedCompExpr` so each operand is evaluated once and the chain has proper short-circuit semantics. The `in` and `is` keywords are comparison operators in Python/Dragon, not separate constructs.

#### Levels 7-10: Bitwise operators

```
bitwiseOr()  -- x | y  (PIPE token)
bitwiseXor() -- x ^ y  (CARET token)
bitwiseAnd() -- x & y  (AMPERSAND token)
shift()      -- x << y, x >> y  (LEFT_SHIFT, RIGHT_SHIFT tokens)
```

All follow the same left-associative while-loop pattern as `orExpr()`.

#### Levels 11-12: Arithmetic operators

```
term()   -- x + y, x - y  (PLUS, MINUS)
factor() -- x * y, x / y, x % y, x // y  (STAR, SLASH, PERCENT, DOUBLE_SLASH)
```

Same left-associative pattern. `DOUBLE_SLASH` is Python's floor division operator `//`.

#### Level 13: `unary()` -- Unary prefix operators

```cpp
std::unique_ptr<Expr> Parser::unary() {
    if (match(TokenType::MINUS) || match(TokenType::PLUS) || match(TokenType::TILDE)) {
        auto un = std::make_unique<UnaryExpr>();
        un->op = previous();
        un->operand = unary();  // Recursive for --x (parsed as -(-(x)))
        return un;
    }
    return power();
}
```

Handles `-x`, `+x`, `~x`. Recursive to handle stacking: `--x` is `-(-(x))`.

#### Level 14: `power()` -- Exponentiation (RIGHT-ASSOCIATIVE)

```cpp
std::unique_ptr<Expr> Parser::power() {
    auto expr = fireExpr();
    if (match(TokenType::POWER)) {          // Note: 'if', not 'while'
        auto bin = std::make_unique<BinaryExpr>();
        bin->left = std::move(expr);
        bin->op = previous();
        bin->right = unary();               // Delegates to unary(), NOT power()
        return bin;
    }
    return expr;
}
```

This is the only **right-associative** binary operator in the hierarchy. The use of `if` instead of `while` means `2 ** 3 ** 4` is parsed as `2 ** (3 ** 4)`. Note that the right operand calls `unary()`, not `power()` -- this matches Python's semantics where `-2 ** 3` is `-(2 ** 3)`, not `(-2) ** 3`. The base operand delegates to `fireExpr()`, the next level down.

#### Level 15: `fireExpr()` -- Fire prefix (green-thread spawn)

```cpp
std::unique_ptr<Expr> Parser::fireExpr() {
    if (match(TokenType::FIRE)) {
        auto fe = std::make_unique<FireExpr>();
        fe->setLocation(previous().location());
        if (check(TokenType::LEFT_BRACE)) {
            fe->bodyStmts = parseBlock();   // fire { block } form
        } else {
            fe->operand = expression();     // fire fn(args) form
        }
        return fe;
    }
    return awaitExpr();
}
```

Parses the two spawn forms of the concurrency model: `fire fn(args)` (whose operand is a full `expression()`) and `fire { block }` (an inline block parsed with `parseBlock()`, run as a green thread). Both produce a `FireExpr`. If the cursor is not on `FIRE`, it delegates to `awaitExpr()`.

#### Level 16: `awaitExpr()` -- Await prefix

```cpp
std::unique_ptr<Expr> Parser::awaitExpr() {
    if (match(TokenType::AWAIT)) {
        auto aw = std::make_unique<AwaitExpr>();
        aw->operand = unary();
        return aw;
    }
    return call();
}
```

Parses `await expr`. Delegates to `unary()` for its operand.

#### Level 17: `call()` -- Postfix operations (calls, attributes, subscripts)

```cpp
std::unique_ptr<Expr> Parser::call() {
    auto expr = primary();
    if (!expr) return nullptr;
    while (true) {
        if (match(TokenType::LEFT_PAREN))    { expr = finishCall(std::move(expr)); }
        else if (match(TokenType::DOT))      { /* AttributeExpr */ }
        else if (match(TokenType::LEFT_BRACKET)) { /* SubscriptExpr or SliceExpr */ }
        else break;
    }
    return expr;
}
```

This is the postfix chaining level. After parsing a `primary()` expression, it loops to handle any sequence of:

- **Function calls**: `f(a, b, key=val)` -- delegates to `finishCall()`
- **Attribute access**: `obj.attr` -- produces `AttributeExpr`
- **Subscript / slice**: `arr[i]`, `arr[1:5]`, `arr[::2]` -- produces `SubscriptExpr` wrapping either the index expression or a `SliceExpr`

The `while(true)` loop allows arbitrary chaining: `obj.method(x).attr[0](y)` is parsed left-to-right into a nested chain of postfix operations.

**Subscript vs. Slice detection**: When `LEFT_BRACKET` is matched, the parser checks for a `COLON` token. If a colon appears (either immediately or after the first expression), it parses a `SliceExpr` with optional `lower`, `upper`, and `step` fields. Otherwise, it parses a regular subscript index.

Note that the standalone `subscript()` and `attribute()` methods declared in the header are stubs that simply delegate. The actual postfix logic is in `call()`.

---

## 6. Primary Expression Parsing

`primary()` handles the "atoms" of the expression grammar -- the tightest-binding constructs that serve as the base for all operators.

### Dispatch order in `primary()`

The method uses a sequence of `if (match(...))` checks:

| Token | AST Node | Notes |
|-------|----------|-------|
| `INTEGER` | `IntegerLiteral` | Parses hex (`0x`), binary (`0b`), octal (`0o`), decimal. Strips `_` separators. Uses `std::stoll`. |
| `FLOAT` | `FloatLiteral` | Strips `_` separators. Uses `std::stod`. |
| `STRING` | `StringLiteral` | Detects `f`, `r`, `b` prefixes. Strips triple-quote or single-quote delimiters. |
| `TRUE` | `BooleanLiteral(true)` | |
| `FALSE` | `BooleanLiteral(false)` | |
| `NONE` | `NoneLiteral` | |
| `IDENTIFIER` | `NameExpr` | Variable references. |
| `LEFT_PAREN` | Parenthesized expr or `TupleExpr` | Empty parens `()` produce an empty `TupleExpr`. A single expression without trailing comma is a grouping (returns the inner expression). A trailing comma triggers tuple parsing. |
| `LEFT_BRACKET` | `ListExpr` or `ListCompExpr` | Delegates to `parseList()`. |
| `LEFT_BRACE` | `DictExpr`, `SetExpr`, or `DictCompExpr` | Delegates to `parseDict()`. |
| `LAMBDA` | `LambdaExpr` | Delegates to `parseLambda()`. |
| `YIELD` | `YieldExpr` | Delegates to `parseYield()`. |

If none of these match, an error is emitted and `nullptr` is returned.

### String literal parsing details

String prefix detection scans characters before the opening quote: `f`/`F` sets `isFString`, `r`/`R` sets `isRaw`, `b`/`B` sets `isBytes`. Triple-quoted strings (`"""..."""` or `'''...'''`) have their delimiters stripped (3 chars from each end). Single-quoted strings strip 1 char from each end.

### Parenthesized expressions vs. tuples

The parser uses a look-ahead strategy:

1. If `LEFT_PAREN` followed immediately by `RIGHT_PAREN` -- empty tuple: `TupleExpr{}`
2. Parse one expression
3. If followed by `COMMA` -- this is a tuple. Parse remaining elements separated by commas.
4. If followed by `RIGHT_PAREN` without comma -- this is a grouping. Return the inner expression directly (no tuple wrapper).

This matches Python semantics where `(x)` is just `x` but `(x,)` is a one-element tuple.

---

## 7. Literal and Container Parsers

### `parseList()` -- List literals and comprehensions

After `LEFT_BRACKET` has been consumed by `primary()`:

1. If immediately `RIGHT_BRACKET`: return empty `ListExpr`
2. Parse the first expression
3. If the next token is `FOR`: this is a **list comprehension**. Parse `for VAR in ITERABLE [if CONDITION]` and return `ListCompExpr`. The variable name is extracted from the first expression using `primary()` and `dynamic_cast<NameExpr*>`. The iterable is parsed with `orExpr()` (not `expression()`) to prevent the `if` filter from being consumed as part of a ternary expression.
4. Otherwise: parse remaining comma-separated elements and return `ListExpr`

### `parseDict()` -- Dict literals, set literals, and dict comprehensions

After `LEFT_BRACE` has been consumed:

1. If immediately `RIGHT_BRACE`: return empty `DictExpr`
2. Parse the first expression
3. If followed by `COLON`: this is a dict (or dict comprehension)
   - Parse the value with `orExpr()`
   - If `FOR` follows: parse a **dict comprehension** with optional tuple unpacking (`for k, v in ...`) and optional `if` filter. Returns `DictCompExpr`.
   - Otherwise: parse remaining `key: value` pairs. Returns `DictExpr`.
4. If NOT followed by `COLON`: this is a **set literal**. Parse remaining comma-separated elements. Returns `SetExpr`.

The ambiguity between dict and set is resolved by the presence or absence of the colon after the first expression.

### `parseLambda()` -- Lambda expressions

Dragon supports two lambda forms:

**Python-style** (no parentheses around params):
```
lambda x, y: x + y
```
Parameters are parsed as `IDENTIFIER` tokens with optional `= default_value`. The body is a single expression after the colon.

**Dragon-style** (parenthesized params with types):
```
lambda (x: int, y: int) : int { return x + y }
```
Parameters are parsed with optional `: type` annotations and `= default` values. If `LEFT_BRACE` follows, the body is a block (`bodyStmts`). Otherwise, it is a single expression (`body`).

The parser distinguishes between the two forms by checking for `LEFT_PAREN` after `LAMBDA`.

### `parseYield()` -- Yield expressions

Handles both `yield value` and `yield from iterable`. If `FROM` follows `YIELD`, it sets `isYieldFrom = true` and parses the iterable. Otherwise, if the next token is not `NEWLINE`, `RIGHT_PAREN`, or `END_OF_FILE`, it parses the yielded value.

---

## 8. Statement Parsing

### `statement()` -- Main dispatch

`statement()`, `simpleStatement()`, `matchStatement()`, `enumDeclaration()`, and the rest of the statement/declaration parsers live in `src/ParserStmts.cpp` (split out from `src/Parser.cpp` under the file-size policy). The `statement()` function is the top-level entry point for parsing a single statement. Like `expression()`, it opens with a `ParserRecursionGuard` and bails with a `"statement nesting too deep"` diagnostic (returning a `PassStmt` placeholder) once `recursionDepth` exceeds `kMaxRecursionDepth`, so deeply nested `if`/`if`/`if` chains cannot blow the C stack. It then skips any `NEWLINE` tokens and dispatches based on the current token.

```
statement()
  |-- Recursion-depth guard (PassStmt + diagnostic if too deep)
  |-- Skip NEWLINEs
  |-- If AT (@): parseDecorators() then functionDeclaration() or classDeclaration()
  |-- If DEF or ASYNC: functionDeclaration()
  |-- If CLASS: classDeclaration()
  |-- If IF: ifStatement()
  |-- If WHILE: whileStatement()
  |-- If FOR: forStatement()
  |-- If TRY: tryStatement()
  |-- If WITH: withStatement()
  |-- If contextual "thread" (IDENTIFIER at stmt start): threadStatement()
  |-- If contextual "enum" (.dr only, IDENTIFIER + IDENTIFIER): enumDeclaration()
  |-- If EXTERN (.dr only): externDeclaration()
  |-- If CONST (.dr only): constDeclaration()
  |-- If STATIC (.dr only): staticDeclaration()
  |-- If soft "match" (IDENTIFIER): matchStatement()
  |-- If soft "type" (IDENTIFIER, PEP 695): TypeAliasStmt
  |-- Otherwise: simpleStatement()
```

The `thread`, `enum`, `match`, and `type` dispatches are **contextual / soft keywords**: they are ordinary `IDENTIFIER` tokens that the parser only treats as keywords in statement-leading position (`enum`, `extern`, `const`, and `static` are gated to `.dr` mode). This keeps them usable as ordinary names elsewhere. Constructors use `def()` syntax and are parsed in `functionDeclaration()` -- the old `self()` constructor form has been removed.

### `simpleStatement()` -- Non-compound statements

Dispatches to specific statement parsers based on keyword tokens:

| Token | Parser | AST Node |
|-------|--------|----------|
| `RETURN` | `returnStatement()` | `ReturnStmt` |
| `RAISE` | `raiseStatement()` | `RaiseStmt` |
| `BREAK` | `breakStatement()` | `BreakStmt` |
| `CONTINUE` | `continueStatement()` | `ContinueStmt` |
| `PASS` | `passStatement()` | `PassStmt` |
| `ASSERT` | `assertStatement()` | `AssertStmt` |
| `GLOBAL` | `globalStatement()` | `GlobalStmt` |
| `NONLOCAL` | `nonlocalStatement()` | `NonlocalStmt` |
| `DEL` | `deleteStatement()` | `DeleteStmt` |
| `IMPORT` | `importStatement()` | `ImportStmt` |
| `FROM` | `fromImportStatement()` | `FromImportStmt` |
| (anything else) | `expressionStatement()` | `ExprStmt`, `AssignStmt`, `AnnAssignStmt`, or `AugAssignStmt` |

### `expressionStatement()` -- The assignment/expression ambiguity

This is one of the most important functions. It parses an expression, then checks what follows to determine the actual statement type:

1. Parse a full `expression()`
2. If `EQUAL` follows: this is an **assignment** (`AssignStmt`). The expression becomes the target, and another expression is parsed as the value.
3. If `COLON` follows: this is an **annotated assignment** (`AnnAssignStmt`). Parse the type annotation, then optionally `= value`.
4. If an augmented assignment operator follows (`+=`, `-=`, `*=`, `/=`, `//=`, `%=`, `**=`, `@=`, `&=`, `|=`, `^=`, `<<=`, `>>=`): parse as `AugAssignStmt`.
5. Otherwise: wrap in `ExprStmt`.

This design avoids separate assignment and expression parsing paths. The parser speculatively parses an expression and then decides retroactively whether it was actually an assignment target.

---

## 9. Compound Statement Parsing

### `ifStatement()`

```
if CONDITION BLOCK
[elif CONDITION BLOCK]*
[else BLOCK]
```

Produces an `IfStmt` with:
- `condition` -- the first condition expression
- `thenBody` -- the first block
- `elifClauses` -- vector of `(condition, body)` pairs
- `elseBody` -- optional else block

### `whileStatement()`

```
while CONDITION BLOCK
[else BLOCK]
```

Produces `WhileStmt` with optional `elseBody` (Python's while-else construct).

### `forStatement()`

```
for TARGET in ITERABLE BLOCK
[else BLOCK]
```

Produces `ForStmt`. The target is parsed with `primary()`, **not** `expression()`. This is a critical design decision: if `expression()` were used, parsing `for x in items` would parse `x in items` as a comparison expression (since `in` is a comparison operator at precedence level 6). Using `primary()` limits the target to simple names, preventing the `in` keyword from being consumed by the comparison level.

### `tryStatement()`

```
try BLOCK
(except|catch) [TYPE [as NAME]] BLOCK
[else BLOCK]
[finally BLOCK]
```

Produces `TryStmt`. Handlers accept both Python's `except` and Dragon's `catch` keyword (both tokens are consumed by the same `match()` call).

**Handler syntax for `catch`/`except`:**
- Parenthesized form (Dragon style): `catch (e: ValueError) { ... }`. If the token after `(` is `IDENTIFIER` followed by `COLON`, it is parsed as `name: type`. Otherwise, the identifier is treated as a type name.
- Non-parenthesized form (Python style): `except ValueError as e:`. The identifier is parsed as a type, and `as NAME` optionally follows.

### `withStatement()`

```
with EXPR [as VAR] [, EXPR [as VAR]]* BLOCK
```

Produces `WithStmt` with a vector of `WithItem` structs, each containing a context expression and an optional target variable.

---

## 10. Block Parsing

The `parseBlock()` function is the key to Dragon's bilingual syntax. It handles both brace-delimited and indentation-based blocks depending on `options.isDragonFile`.

### Dragon mode (`isDragonFile = true`)

```cpp
consume(TokenType::LEFT_BRACE, "Expect '{' before block");
while (!check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
    while (match(TokenType::NEWLINE) || match(TokenType::SEMICOLON)) {}
    if (check(TokenType::RIGHT_BRACE)) break;
    auto stmt = statement();
    if (stmt) stmts.push_back(std::move(stmt));
}
consume(TokenType::RIGHT_BRACE, "Expect '}' after block");
```

Consumes `{`, parses statements until `}`, consumes `}`. Newlines and semicolons between statements are skipped.

### Python mode (`isDragonFile = false`)

```cpp
match(TokenType::COLON);      // Consume optional colon (may already be consumed)
match(TokenType::NEWLINE);    // Consume the newline after the colon
if (match(TokenType::INDENT)) {
    while (!check(TokenType::DEDENT) && !isAtEnd()) {
        while (match(TokenType::NEWLINE) || match(TokenType::SEMICOLON)) {}
        if (check(TokenType::DEDENT)) break;
        auto stmt = statement();
        if (stmt) stmts.push_back(std::move(stmt));
    }
    match(TokenType::DEDENT);
} else {
    auto stmt = simpleStatement();
    if (stmt) stmts.push_back(std::move(stmt));
}
```

Key points:
- The colon is consumed with `match()` (not `consume()`) because some callers (like `functionDeclaration()`) may have already consumed it.
- `INDENT` and `DEDENT` are synthetic tokens emitted by the Lexer when it detects indentation changes. They bracket an indented block.
- If no `INDENT` token is present after the colon and newline, the block is a **single-line body** (e.g., `if x: pass`), parsed with `simpleStatement()`.
- The `DEDENT` is consumed with `match()` rather than `consume()` for resilience -- if dedent is missing due to an error, parsing continues.

---

## 11. Function Declaration Parsing

```cpp
std::unique_ptr<Stmt> Parser::functionDeclaration() {
    auto decl = std::make_unique<FunctionDecl>();
    decl->isAsync = match(TokenType::ASYNC);
    consume(TokenType::DEF, "Expect 'def'");
    decl->name = consume(TokenType::IDENTIFIER, "Expect function name").lexeme();
    consume(TokenType::LEFT_PAREN, "Expect '('");
    decl->params = parseParameters();
    consume(TokenType::RIGHT_PAREN, "Expect ')'");
    // Return type annotation
    if (match(TokenType::COLON)) {
        if (!check(LEFT_BRACE) && !check(NEWLINE) && !check(INDENT))
            decl->returnType = parseType();
    } else if (match(TokenType::ARROW)) {
        decl->returnType = parseType();
    }
    decl->body = parseBlock();
    return decl;
}
```

### Return type annotation handling

The return type is specified using the arrow syntax in both modes:
- **Dragon style**: `def foo() -> int { ... }` -- an `ARROW` (`->`) followed by the return type, followed by the block
- **Python style**: `def foo() -> int:` -- an `ARROW` (`->`) followed by the return type, then a colon for the block

This is unambiguous: `->` always introduces a return type annotation. No lookahead or disambiguation is needed.

### Parameter parsing: `parseParameters()`

```
(name: type = default, *args: type, **kwargs: type)
```

Parameters are parsed in a comma-separated loop. Each parameter can be:
- **Regular**: `name [: type] [= default]`
- **Variadic positional**: `*name [: type]` -- sets `isVarArg = true`. The name is optional (bare `*` is a separator-only marker).
- **Variadic keyword**: `**name [: type]` -- sets `isKwArg = true`. Detected by matching the `POWER` token (`**`).

Type annotations (`: type`) and default values (`= expr`) are both optional.

---

## 12. Class Declaration Parsing

```cpp
std::unique_ptr<Stmt> Parser::classDeclaration() {
    consume(TokenType::CLASS, "Expect 'class'");
    auto decl = std::make_unique<ClassDecl>();
    decl->name = consume(TokenType::IDENTIFIER, "Expect class name").lexeme();
    if (match(TokenType::LEFT_PAREN)) {
        if (!check(TokenType::RIGHT_PAREN)) {
            do { decl->bases.push_back(expression()); } while (match(TokenType::COMMA));
        }
        consume(TokenType::RIGHT_PAREN, "Expect ')'");
    }
    decl->body = parseBlock();
    return decl;
}
```

Base classes are parsed as a comma-separated list of expressions inside optional parentheses. The class body is a block. Note that `keywords` (like `metaclass=Meta`) are declared in the `ClassDecl` AST node but the parser does not yet extract them from the base class list -- they would currently be parsed as regular base class expressions.

### Decorator parsing

```cpp
std::vector<std::unique_ptr<Expr>> Parser::parseDecorators() {
    std::vector<std::unique_ptr<Expr>> decorators;
    while (match(TokenType::AT)) {
        decorators.push_back(expression());
        match(TokenType::NEWLINE);
    }
    return decorators;
}
```

Decorators are parsed as `@expression` followed by an optional newline. They are collected into a vector and attached to the subsequent `FunctionDecl` or `ClassDecl` node. The decorator expression can be any expression, including dotted names (`@module.decorator`) and calls (`@decorator(args)`).

---

## 13. Type Annotation Parsing

Type annotations appear after colons in variable declarations, parameter lists, and return types.

### `parseType()` -- Entry point

```cpp
std::unique_ptr<TypeExpr> Parser::parseType() { return parseUnionType(); }
```

### `parseUnionType()` -- Union types with `|`

```cpp
std::unique_ptr<TypeExpr> Parser::parseUnionType() {
    auto type = parsePrimaryType();
    if (!type) return nullptr;
    if (check(TokenType::PIPE)) {
        auto u = std::make_unique<UnionTypeExpr>();
        u->types.push_back(std::move(type));
        while (match(TokenType::PIPE)) {
            auto next = parsePrimaryType();
            if (next) u->types.push_back(std::move(next));
        }
        return u;
    }
    return type;
}
```

Parses `int | str | None` as a `UnionTypeExpr` containing three types. If no `PIPE` follows the first type, returns the primary type directly.

### `parsePrimaryType()` -- Simple and generic types

```cpp
std::unique_ptr<TypeExpr> Parser::parsePrimaryType() {
    if (match(TokenType::NONE)) {
        auto t = std::make_unique<NamedTypeExpr>();
        t->name = "None";
        return t;
    }
    if (match(TokenType::IDENTIFIER)) {
        auto t = std::make_unique<NamedTypeExpr>();
        t->name = previous().lexeme();
        if (check(TokenType::LEFT_BRACKET)) return parseGenericType(std::move(t));
        return t;
    }
    return nullptr;
}
```

Handles:
- `None` -- produces `NamedTypeExpr("None")`
- Simple names: `int`, `str`, `MyClass` -- produces `NamedTypeExpr`
- Generic types: if `[` follows the name, delegates to `parseGenericType()`

### `parseGenericType()` -- Parameterized types

```cpp
std::unique_ptr<TypeExpr> Parser::parseGenericType(std::unique_ptr<TypeExpr> base) {
    consume(TokenType::LEFT_BRACKET, "Expect '['");
    if (check(TokenType::LEFT_BRACKET)) {
        // Callable[[int, str], bool]
        advance();
        auto callable = std::make_unique<CallableTypeExpr>();
        // Parse param types inside inner []
        // Parse return type after comma
        // ...
        return callable;
    }
    auto generic = std::make_unique<GenericTypeExpr>();
    generic->base = std::move(base);
    // Parse comma-separated type arguments
    // ...
    consume(TokenType::RIGHT_BRACKET, "Expect ']'");
    return generic;
}
```

Handles two cases:
1. **`Callable[[param_types], return_type]`** -- detected by a second `LEFT_BRACKET` immediately after the first. Produces `CallableTypeExpr`.
2. **Regular generics** (`list[int]`, `dict[str, int]`, `tuple[int, str, bool]`) -- produces `GenericTypeExpr` with the base type and a vector of type arguments.

---

## 14. Error Handling and Recovery

### Error reporting

```cpp
void Parser::error(const std::string& message) { error(peek(), message); }

void Parser::error(const Token& token, const std::string& message) {
    impl_->diagnostics.push_back({
        ParserDiagnostic::Level::Error,
        token.location(),
        message
    });
}
```

Errors are accumulated in the diagnostics vector. Each diagnostic includes the severity level, the source location of the offending token, and a human-readable message. The parser does **not** throw exceptions -- it continues parsing after errors.

### `ParserDiagnostic`

```cpp
struct ParserDiagnostic {
    enum class Level { Warning, Error };
    Level level;
    SourceLocation location;
    std::string message;
};
```

### `synchronize()` -- Panic mode recovery

```cpp
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
```

After an error, `synchronize()` skips tokens until it finds a likely statement boundary:
- A `NEWLINE` token (the previous token was a newline -- we are at the start of a new line)
- A statement-starting keyword: `class`, `def`, `for`, `if`, `while`, `return`

This prevents a single syntax error from cascading into many spurious errors.

### `isAtStatementBoundary()`

```cpp
bool Parser::isAtStatementBoundary() const {
    return check(TokenType::NEWLINE) || check(TokenType::END_OF_FILE);
}
```

Returns true when the cursor is at a natural statement boundary.

### `isAtBlockEnd()`

```cpp
bool Parser::isAtBlockEnd() const {
    return check(TokenType::RIGHT_BRACE) || check(TokenType::DEDENT);
}
```

Returns true when the cursor is at the end of a block (either `}` in Dragon mode or `DEDENT` in Python mode).

---

## 15. Module Parsing

```cpp
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
            if (!isAtEnd()) advance();  // Skip problematic token to avoid infinite loop
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
```

The top-level loop parses statements until `END_OF_FILE`. Blank lines (`NEWLINE`) and stray semicolons are skipped. If a statement parse fails (returns `nullptr`), the parser advances one token to avoid an infinite loop. After each statement the loop drains `impl_->pendingStmts`, the overflow slot a multi-declaration construct (such as `extern "C" from "lib" { ... }`) uses to emit more than one top-level statement. Finally, a leading bare string literal in the body is captured as the module docstring via `extractDocstring()`.

---

## 16. Design Decisions and Rationale

### Why `primary()` for for-loop targets

The for-loop parser uses `primary()` to parse the loop target variable:

```cpp
stmt->target = primary();  // NOT expression()
consume(TokenType::IN, "Expect 'in'");
```

If `expression()` were used, the parser would parse `for x in items` as `for (x in items)` -- because `in` is a comparison operator at precedence level 6. The comparison level would consume `in` as a binary operator, and the parser would never see the `IN` token it needs. Using `primary()` restricts the target to simple names (or other atomic expressions), ensuring `in` is not consumed prematurely.

### Why `orExpr()` in comprehensions instead of `expression()`

In list comprehensions and dict comprehensions, the iterable and condition are parsed with `orExpr()`:

```cpp
comp->iterable = orExpr();        // Not expression()
if (match(TokenType::IF)) {
    comp->condition = orExpr();   // Not expression()
}
```

This prevents the `if` keyword in the filter clause from being parsed as part of a ternary expression (`x if cond else y`). By stopping at the `or` level, the `if` keyword remains available for the comprehension filter.

### Python `except` vs Dragon `catch`

The try-statement handler parsing accepts both keywords:

```cpp
while (match(TokenType::EXCEPT) || match(TokenType::CATCH)) { ... }
```

In `.dr` files, `catch` is idiomatic. In `.py` files, `except` is used. Both produce the same AST (`TryStmt::ExceptHandler`). Dragon's `catch` uses C++/Java-style parenthesized syntax: `catch (e: ValueError) { ... }`, while Python's `except` uses `except ValueError as e:`.

### Assignment is a statement, not an expression

Unlike C/C++ where `a = b` is an expression with a value, Dragon (like Python) treats assignment as a statement. The `assignment()` function in the precedence chain does nothing -- it just delegates to `ternary()`. Actual assignment detection happens in `expressionStatement()` after the full expression is parsed. This prevents assignment in expression positions and avoids the classic `if (x = 5)` bug.

### Return type syntax uniformity

Both Dragon mode and Python mode use `->` for return type annotations: `def foo() -> int { ... }` (Dragon) and `def foo() -> int:` (Python). This eliminates the colon ambiguity that previously existed when `:` was used for both return types and block introducers.

---

## 17. Public API Summary

```cpp
class Parser {
public:
    Parser(std::vector<Token> tokens, ParserOptions options = {});
    ~Parser();

    std::unique_ptr<Module> parseModule();        // Parse a complete file
    std::unique_ptr<Expr> parseExpression();       // Parse a single expression (REPL)
    std::unique_ptr<Stmt> parseStatement();        // Parse a single statement

    const std::vector<ParserDiagnostic>& diagnostics() const;
    bool hasErrors() const;
};
```

Copy semantics are deleted. The parser takes ownership of the token vector via move. The three parse methods (`parseModule`, `parseExpression`, `parseStatement`) are the public entry points. After parsing, `diagnostics()` returns all accumulated messages and `hasErrors()` indicates whether any errors were found.

---

## Previous Document

[002 - Dragon Token System](002-tokens.md)

## Next Document

[004 - Dragon AST](004-ast.md)
