# Dragon Token System: Comprehensive Reference

> **Last Updated:** 2026-06-22

---

## Table of Contents

1. [Overview](#1-overview)
2. [The Token Class](#2-the-token-class)
3. [The TokenType Enum](#3-the-tokentype-enum)
4. [Source Location](#4-source-location)
5. [Keywords](#5-keywords)
6. [Operator Tokens](#6-operator-tokens)
7. [Special Tokens](#7-special-tokens)
8. [Token Categories for Parser Reference](#8-token-categories-for-parser-reference)

---

## 1. Overview

The token system is defined across two files:

- **Header**: `include/dragon/Token.h` -- declares `TokenType`, `SourceLocation`, `Token`,
  and the free functions `isKeyword()` and `keywordType()`.
- **Implementation**: `src/Token.cpp` -- defines `Token` methods, the `tokenTypeName()`
  switch, and the keyword lookup map.

A token is the atomic unit of the Dragon lexer's output. Each token carries three pieces
of information:

1. **Type** (`TokenType`): What kind of token it is (integer literal, `+` operator, `if`
   keyword, etc.).
2. **Lexeme** (`std::string`): The exact source text that was matched.
3. **Location** (`SourceLocation`): Where in the source the token was found.

---

## 2. The Token Class

Defined in `include/dragon/Token.h`, lines 146-174.

### Fields

```cpp
class Token {
private:
    TokenType type_;          // The token's classification
    std::string lexeme_;      // The matched source text (owned copy)
    SourceLocation location_; // File, line, column, and byte offset
};
```

All three fields are private. Access is through const accessor methods.

### Constructors

```cpp
Token();
Token(TokenType type, std::string lexeme, SourceLocation location);
```

**Default constructor** (`Token.cpp` line 10): Creates an `ERROR` token with an empty
lexeme and default-constructed location. This exists so that tokens can be stored in
vectors and other containers that require default construction.

```cpp
Token::Token() : type_(TokenType::ERROR), lexeme_(""), location_{} {}
```

**Parameterized constructor** (`Token.cpp` line 12): Moves the lexeme and location into
the token.

```cpp
Token::Token(TokenType type, std::string lexeme, SourceLocation location)
    : type_(type), lexeme_(std::move(lexeme)), location_(std::move(location)) {}
```

### Accessor Methods

```cpp
TokenType type() const;                  // Returns type_
const std::string& lexeme() const;       // Returns lexeme_ by const reference
const SourceLocation& location() const;  // Returns location_ by const reference
```

All defined at `Token.cpp` lines 15-17. These are simple getters with no logic.

### Type Checking Methods

```cpp
bool is(TokenType t) const;
```

Returns `true` if the token's type matches `t`. Defined at `Token.cpp` line 19 as
`return type_ == t;`.

```cpp
template<typename... Types>
bool isOneOf(Types... types) const {
    return (is(types) || ...);
}
```

A variadic template using C++17 fold expressions. Returns `true` if the token's type
matches any of the provided types. Defined inline in the header. Example
usage:

```cpp
if (tok.isOneOf(TokenType::PLUS, TokenType::MINUS, TokenType::STAR)) { ... }
```

### Debugging Methods

```cpp
std::string toString() const;
```

Returns a human-readable representation of the token. Format: `"TYPENAME"` for tokens
with empty lexemes or EOF, `"TYPENAME(lexeme)"` for all others. Defined at `Token.cpp`
lines 21-29:

```cpp
std::string Token::toString() const {
    std::string result = tokenTypeName(type_);
    if (!lexeme_.empty() && type_ != TokenType::END_OF_FILE) {
        result += "(";
        result += lexeme_;
        result += ")";
    }
    return result;
}
```

Examples:
- Integer 42: `"INTEGER(42)"`
- Plus operator: `"PLUS(+)"`
- End of file: `"EOF"`
- Newline: `"NEWLINE(\n)"`

```cpp
static const char* tokenTypeName(TokenType type);
```

A static method that maps every `TokenType` variant to a string name. Implemented as
a switch statement covering all cases (`Token.cpp` lines 31-157). Returns `"UNKNOWN"` as
a fallback for any unhandled value, though this should never occur.

### Free Functions

```cpp
bool isKeyword(std::string_view name);
```

Returns `true` if the given name is a keyword in the Dragon language. Performs a lookup
in the static keyword map (`isKeyword()` in `Token.cpp`).

```cpp
TokenType keywordType(std::string_view name);
```

Returns the `TokenType` for a keyword, or `TokenType::IDENTIFIER` if the name is not a
keyword. This is the primary function called by `Lexer::scanIdentifier()` to classify
identifiers. Defined as `keywordType()` in `Token.cpp`.

---

## 3. The TokenType Enum

The `TokenType` enum class is defined in `include/dragon/Token.h`, lines 10-135. It
contains **103 distinct values** organized into the following categories.

### Complete Listing

#### Literals (7 values)

| TokenType | Description | Example Lexemes | tokenTypeName |
|-----------|-------------|-----------------|---------------|
| `INTEGER` | Integer literal (decimal, hex, octal, binary) | `42`, `0xFF`, `0b1010`, `0o77`, `1_000` | `"INTEGER"` |
| `FLOAT` | Floating-point literal | `3.14`, `1e10`, `1.5e-3` | `"FLOAT"` |
| `STRING` | String literal (all prefix variants) | `"hello"`, `'world'`, `f"hi {x}"`, `r"\n"` | `"STRING"` |
| `BYTES` | Byte string literal | `b"data"` | `"BYTES"` |
| `TRUE` | Boolean true literal | `True` | `"TRUE"` |
| `FALSE` | Boolean false literal | `False` | `"FALSE"` |
| `NONE` | None literal | `None` | `"NONE"` |

Note: `TRUE`, `FALSE`, and `NONE` are treated as keywords in the keyword map (they are
looked up in `keywordType()`), but they are classified under "Literals" in the enum
because they represent literal values. In practice, `BYTES` is declared but the lexer
currently emits `STRING` for `b"..."` strings -- the `BYTES` type exists for future
differentiation.

#### Identifiers (1 value)

| TokenType | Description | Example Lexemes | tokenTypeName |
|-----------|-------------|-----------------|---------------|
| `IDENTIFIER` | User-defined name | `foo`, `_bar`, `MyClass`, `x123` | `"IDENTIFIER"` |

#### Python Keywords (32 values)

| TokenType | Keyword | tokenTypeName |
|-----------|---------|---------------|
| `AND` | `and` | `"AND"` |
| `AS` | `as` | `"AS"` |
| `ASSERT` | `assert` | `"ASSERT"` |
| `ASYNC` | `async` | `"ASYNC"` |
| `AWAIT` | `await` | `"AWAIT"` |
| `BREAK` | `break` | `"BREAK"` |
| `CLASS` | `class` | `"CLASS"` |
| `CONTINUE` | `continue` | `"CONTINUE"` |
| `DEF` | `def` | `"DEF"` |
| `DEL` | `del` | `"DEL"` |
| `ELIF` | `elif` | `"ELIF"` |
| `ELSE` | `else` | `"ELSE"` |
| `EXCEPT` | `except` | `"EXCEPT"` |
| `FINALLY` | `finally` | `"FINALLY"` |
| `FOR` | `for` | `"FOR"` |
| `FROM` | `from` | `"FROM"` |
| `GLOBAL` | `global` | `"GLOBAL"` |
| `IF` | `if` | `"IF"` |
| `IMPORT` | `import` | `"IMPORT"` |
| `IN` | `in` | `"IN"` |
| `IS` | `is` | `"IS"` |
| `LAMBDA` | `lambda` | `"LAMBDA"` |
| `NONLOCAL` | `nonlocal` | `"NONLOCAL"` |
| `NOT` | `not` | `"NOT"` |
| `OR` | `or` | `"OR"` |
| `PASS` | `pass` | `"PASS"` |
| `RAISE` | `raise` | `"RAISE"` |
| `RETURN` | `return` | `"RETURN"` |
| `TRY` | `try` | `"TRY"` |
| `WHILE` | `while` | `"WHILE"` |
| `WITH` | `with` | `"WITH"` |
| `YIELD` | `yield` | `"YIELD"` |

#### Dragon-Specific Keywords (7 values)

| TokenType | Keyword | tokenTypeName | Notes |
|-----------|---------|---------------|-------|
| `FIRE` | `fire` | `"FIRE"` | Spawn a green-thread task (`fire fn()` / `fire { ... }`) |
| `CATCH` | `catch` | `"CATCH"` | Alternative to `except` for brace syntax |
| `CONST` | `const` | `"CONST"` | Immutable binding (Dragon extension) |
| `STATIC` | `static` | `"STATIC"` | Static fields/methods (Dragon extension) |
| `EXTERN` | `extern` | `"EXTERN"` | C FFI: `extern "C"` declarations |
| `THREAD` | `thread` | `"THREAD"` | Scoped OS thread `thread { block }` (contextual; see note) |
| `ENUM` | `enum` | `"ENUM"` | Int-backed enum `enum Name { A, B, C }` (contextual; see note) |

Note: `THREAD` and `ENUM` are declared as `TokenType` values and have `tokenTypeName`
entries, but the lexer never emits them. `thread` and `enum` are contextual keywords the
parser recognizes by lexeme at statement start, so the lexer classifies them as
`IDENTIFIER`. The names `match`, `case`, and `type` are soft keywords (also matched by
lexeme in the parser) and have **no** dedicated `TokenType` at all.

#### Arithmetic Operators (7 values)

| TokenType | Symbol | tokenTypeName |
|-----------|--------|---------------|
| `PLUS` | `+` | `"PLUS"` |
| `MINUS` | `-` | `"MINUS"` |
| `STAR` | `*` | `"STAR"` |
| `SLASH` | `/` | `"SLASH"` |
| `DOUBLE_SLASH` | `//` | `"DOUBLE_SLASH"` |
| `PERCENT` | `%` | `"PERCENT"` |
| `POWER` | `**` | `"POWER"` |

#### Decorator / Matrix Operator (1 value)

| TokenType | Symbol | tokenTypeName |
|-----------|--------|---------------|
| `AT` | `@` | `"AT"` |

#### Bitwise Operators (6 values)

| TokenType | Symbol | tokenTypeName |
|-----------|--------|---------------|
| `AMPERSAND` | `&` | `"AMPERSAND"` |
| `PIPE` | `\|` | `"PIPE"` |
| `CARET` | `^` | `"CARET"` |
| `TILDE` | `~` | `"TILDE"` |
| `LEFT_SHIFT` | `<<` | `"LEFT_SHIFT"` |
| `RIGHT_SHIFT` | `>>` | `"RIGHT_SHIFT"` |

#### Comparison Operators (8 values)

| TokenType | Symbol | tokenTypeName |
|-----------|--------|---------------|
| `LESS` | `<` | `"LESS"` |
| `GREATER` | `>` | `"GREATER"` |
| `LESS_EQUAL` | `<=` | `"LESS_EQUAL"` |
| `GREATER_EQUAL` | `>=` | `"GREATER_EQUAL"` |
| `EQUAL_EQUAL` | `==` | `"EQUAL_EQUAL"` |
| `NOT_EQUAL` | `!=` | `"NOT_EQUAL"` |
| `NOT_IN` | `not in` | `"NOT_IN"` |
| `IS_NOT` | `is not` | `"IS_NOT"` |

`NOT_IN` and `IS_NOT` are never produced by the lexer. The parser synthesizes them in
`comparison()` from a `NOT` followed by `IN` (and `IS` followed by `NOT`, respectively).

#### Assignment Operators (15 values)

| TokenType | Symbol | tokenTypeName |
|-----------|--------|---------------|
| `EQUAL` | `=` | `"EQUAL"` |
| `PLUS_EQUAL` | `+=` | `"PLUS_EQUAL"` |
| `MINUS_EQUAL` | `-=` | `"MINUS_EQUAL"` |
| `STAR_EQUAL` | `*=` | `"STAR_EQUAL"` |
| `SLASH_EQUAL` | `/=` | `"SLASH_EQUAL"` |
| `DOUBLE_SLASH_EQUAL` | `//=` | `"DOUBLE_SLASH_EQUAL"` |
| `PERCENT_EQUAL` | `%=` | `"PERCENT_EQUAL"` |
| `POWER_EQUAL` | `**=` | `"POWER_EQUAL"` |
| `AT_EQUAL` | `@=` | `"AT_EQUAL"` |
| `AMPERSAND_EQUAL` | `&=` | `"AMPERSAND_EQUAL"` |
| `PIPE_EQUAL` | `\|=` | `"PIPE_EQUAL"` |
| `CARET_EQUAL` | `^=` | `"CARET_EQUAL"` |
| `LEFT_SHIFT_EQUAL` | `<<=` | `"LEFT_SHIFT_EQUAL"` |
| `RIGHT_SHIFT_EQUAL` | `>>=` | `"RIGHT_SHIFT_EQUAL"` |
| `WALRUS` | `:=` | `"WALRUS"` |

#### Delimiters (12 values)

| TokenType | Symbol | tokenTypeName |
|-----------|--------|---------------|
| `LEFT_PAREN` | `(` | `"LEFT_PAREN"` |
| `RIGHT_PAREN` | `)` | `"RIGHT_PAREN"` |
| `LEFT_BRACKET` | `[` | `"LEFT_BRACKET"` |
| `RIGHT_BRACKET` | `]` | `"RIGHT_BRACKET"` |
| `LEFT_BRACE` | `{` | `"LEFT_BRACE"` |
| `RIGHT_BRACE` | `}` | `"RIGHT_BRACE"` |
| `COMMA` | `,` | `"COMMA"` |
| `COLON` | `:` | `"COLON"` |
| `SEMICOLON` | `;` | `"SEMICOLON"` |
| `DOT` | `.` | `"DOT"` |
| `ARROW` | `->` | `"ARROW"` |
| `ELLIPSIS` | `...` | `"ELLIPSIS"` |

#### Indentation Tokens (3 values)

| TokenType | tokenTypeName | Notes |
|-----------|---------------|-------|
| `INDENT` | `"INDENT"` | Emitted in Python mode when indentation increases |
| `DEDENT` | `"DEDENT"` | Emitted in Python mode when indentation decreases |
| `NEWLINE` | `"NEWLINE"` | Statement separator in both modes |

#### Template Tokens (2 values)

| TokenType | tokenTypeName | Notes |
|-----------|---------------|-------|
| `TEMPLATE` | `"TEMPLATE"` | A `template { ... }` / `template[X] { ... }` body captured whole by the lexer |
| `TEMPLATE_CONTENT_OPEN` | `"TEMPLATE_CONTENT_OPEN"` | The `:{ ... }` content alias inside a `!{}` block-interpolation |

#### Special Tokens (2 values)

| TokenType | tokenTypeName | Notes |
|-----------|---------------|-------|
| `END_OF_FILE` | `"EOF"` | Marks the end of the token stream |
| `ERROR` | `"ERROR"` | Represents a lexer error |

### Summary Count

| Category | Count |
|----------|-------|
| Literals | 7 |
| Identifiers | 1 |
| Python Keywords | 32 |
| Dragon Keywords | 7 |
| Arithmetic Operators | 7 |
| Decorator/Matrix | 1 |
| Bitwise Operators | 6 |
| Comparison Operators | 8 |
| Assignment Operators | 15 |
| Delimiters | 12 |
| Indentation | 3 |
| Template | 2 |
| Special | 2 |
| **Total** | **103** |

---

## 4. Source Location

### The SourceLocation Struct

Defined in `include/dragon/Token.h`, lines 138-143:

```cpp
struct SourceLocation {
    std::string filename;   // Source file name (owned string)
    size_t line = 0;        // Line number (1-based during lexing, 0 = unknown)
    size_t column = 0;      // Column number (1-based during lexing, 0 = unknown)
    size_t offset = 0;      // Byte offset from start of source
};
```

### Field Details

**`filename`** (`std::string`): The name of the source file. Stored as an owned
`std::string`, not a `string_view` or pointer. This is a deliberate design decision
for lifetime safety -- `SourceLocation` values are copied into AST nodes, diagnostics,
error messages, and debug info. If `filename` were a `string_view`, the original string
would need to outlive all downstream consumers.

Common values:
- `"<stdin>"` -- the `LexerOptions` default
- `"<test>"` -- set by the test harness (`test/TestHelpers.h` line 17)
- `"path/to/file.dr"` or `"path/to/file.py"` -- set by the `Driver` for real files

The per-token cost of storing the filename is mitigated by `std::string`'s small-string
optimization (SSO). On most implementations, strings up to ~22 characters fit in the
inline buffer without heap allocation.

**`line`** (`size_t`, default `0`): The line number where the token starts. The lexer
uses 1-based numbering (`Impl::line` starts at `1`). A value of `0` indicates an unknown
or synthetic location.

**`column`** (`size_t`, default `0`): The column number where the token starts. Also
1-based. The lexer tracks columns precisely, including tabs (each tab increments the
column by 1 in `advance()`, though tab-width expansion happens separately in
`handleIndentation()`).

**`offset`** (`size_t`, default `0`): The byte offset of the token's first character
from the beginning of the source text. This is captured from `Impl::start` when the
token is created. Useful for direct source text access and for span computation (the
span of a token is `[offset, offset + lexeme.size())`).

### Construction in the Lexer

The lexer creates `SourceLocation` values in `makeToken()`:

```cpp
SourceLocation loc{impl_->options.filename, impl_->line,
                   impl_->startColumn, impl_->start};
```

Note that `startColumn` (captured at the beginning of the token scan) is used rather
than the current `column`, which has already advanced past the token.

---

## 5. Keywords

### Complete Keyword Table

The lexer's keyword map has 43 entries. These are stored in a static
`std::unordered_map<std::string_view, TokenType>` in `src/Token.cpp`.

| # | Keyword | TokenType | Category |
|---|---------|-----------|----------|
| 1 | `and` | `AND` | Logical operator |
| 2 | `as` | `AS` | Alias/context manager |
| 3 | `assert` | `ASSERT` | Debug statement |
| 4 | `async` | `ASYNC` | Async definition |
| 5 | `await` | `AWAIT` | Async expression |
| 6 | `break` | `BREAK` | Loop control |
| 7 | `class` | `CLASS` | Class definition |
| 8 | `continue` | `CONTINUE` | Loop control |
| 9 | `def` | `DEF` | Function definition |
| 10 | `del` | `DEL` | Deletion statement |
| 11 | `elif` | `ELIF` | Conditional branch |
| 12 | `else` | `ELSE` | Conditional/loop else |
| 13 | `except` | `EXCEPT` | Exception handling |
| 14 | `finally` | `FINALLY` | Exception cleanup |
| 15 | `for` | `FOR` | For loop |
| 16 | `from` | `FROM` | Import source |
| 17 | `global` | `GLOBAL` | Scope declaration |
| 18 | `if` | `IF` | Conditional |
| 19 | `import` | `IMPORT` | Module import |
| 20 | `in` | `IN` | Membership/iteration |
| 21 | `is` | `IS` | Identity test |
| 22 | `lambda` | `LAMBDA` | Anonymous function |
| 23 | `nonlocal` | `NONLOCAL` | Scope declaration |
| 24 | `not` | `NOT` | Logical negation |
| 25 | `or` | `OR` | Logical operator |
| 26 | `pass` | `PASS` | Null statement |
| 27 | `raise` | `RAISE` | Exception raising |
| 28 | `return` | `RETURN` | Function return |
| 29 | `try` | `TRY` | Exception handling |
| 30 | `while` | `WHILE` | While loop |
| 31 | `with` | `WITH` | Context manager |
| 32 | `yield` | `YIELD` | Generator yield |
| 33 | `fire` | `FIRE` | Spawn a green-thread task (Dragon) |
| 34 | `True` | `TRUE` | Boolean literal |
| 35 | `False` | `FALSE` | Boolean literal |
| 36 | `None` | `NONE` | None literal |
| 37 | `true` | `TRUE` | Boolean literal (`.dr`-mode alias) |
| 38 | `false` | `FALSE` | Boolean literal (`.dr`-mode alias) |
| 39 | `none` | `NONE` | None literal (`.dr`-mode alias) |
| 40 | `catch` | `CATCH` | Dragon extension |
| 41 | `const` | `CONST` | Dragon extension |
| 42 | `static` | `STATIC` | Dragon extension |
| 43 | `extern` | `EXTERN` | Dragon extension (C FFI) |

`match`, `case`, and `type` are **not** in this map. They are soft keywords the parser
matches by lexeme, so the lexer emits them as `IDENTIFIER`. Likewise `thread` and `enum`
are contextual keywords resolved at statement start in the parser, also lexed as
`IDENTIFIER` (their `THREAD`/`ENUM` `TokenType` values exist but are never produced by the
lexer).

### How Keywords Are Distinguished from Identifiers

The keyword recognition happens during identifier scanning in `Lexer::scanIdentifier()`.
The process is:

1. The lexer reads all alphanumeric characters and underscores into a span.
2. The span text is extracted as a `std::string_view`.
3. `keywordType(text)` is called, which does a hash-table lookup in the keyword map.
4. If found, the corresponding `TokenType` (e.g., `IF`, `DEF`, `CLASS`) is returned.
5. If not found, `TokenType::IDENTIFIER` is returned.

This means:
- **Keywords are reserved** -- `if`, `for`, etc. cannot be used as variable names.
- **Keyword matching is exact** -- `If` is an identifier (not `IF`), `TRUE` is an
  identifier (not `True`). The keyword map is case-sensitive.
- **Identifier prefixes of keywords are identifiers** -- `iffy` is `IDENTIFIER`, not
  `IF` followed by `fy`. This works naturally because `scanIdentifier()` consumes the
  entire alphanumeric run before doing the lookup.

### Case Sensitivity

Most keywords use their exact Python casing:
- Lowercase: `and`, `as`, `assert`, ..., `yield`, `fire`, `catch`, `const`, `static`, `extern`
- Title case: `True`, `False`, `None`

The boolean/`None` literals are the one exception: in addition to the title-case
`True`/`False`/`None`, the map also accepts the lowercase aliases `true`/`false`/`none`
for `.dr` mode. Outside those six entries the map is case-sensitive, so `If`, `RETURN`,
and `Catch` are all identifiers.

### Dragon Extensions

Dragon's keyword map adds the following entries beyond Python's standard set:

**`fire`** - Spawns a green-thread task. `fire fn()` and `fire { block }` both yield a
`Task[T]` handle (no binding = fire-and-forget):

```dragon
t: Task[int] = fire work()
```

**`catch`** - Alternative to `except` in brace-mode exception handling, following the C++/Java/JavaScript convention:

```dragon
try {
    risky_operation()
} catch ValueError as e {
    handle_error(e)
}
```

Both `except` and `catch` are recognized as keywords in both modes, but `catch` is the idiomatic choice in `.dr` files.

**`const`** - Immutable binding declaration. Sema-enforced:

```dragon
const MAX_SIZE: int = 100
```

**`static`** - Static field/method declaration:

```dragon
class Counter {
    static count: int = 0
    static def get_count() -> int {
        return Counter.count
    }
}
```

**`extern`** - Marks a C FFI declaration (`extern "C"`).

Two more Dragon keywords - `thread` (scoped OS thread) and `enum` (int-backed enum) - are
**not** in the lexer's keyword map. They are contextual: the parser recognizes them by
lexeme at statement start, and the lexer emits them as `IDENTIFIER` everywhere else (so
`thread` and `enum` remain usable as ordinary names). The borrowed Python soft keywords
`match`, `case`, and `type` work the same way and have no dedicated `TokenType`.

---

## 6. Operator Tokens

### Single-Character Operators

These operators are recognized from a single character with no multi-character extension:

| Symbol | TokenType | Description |
|--------|-----------|-------------|
| `~` | `TILDE` | Bitwise NOT |

### Multi-Character Operators (Disambiguation)

Many operators have multiple forms that share a starting character. The lexer uses
`match()` to attempt consuming additional characters, resolving the longest-match token.

#### From `+`

| Token | Symbol | Lexer Logic |
|-------|--------|-------------|
| `PLUS` | `+` | Default |
| `PLUS_EQUAL` | `+=` | `match('=')` |

#### From `-`

| Token | Symbol | Lexer Logic |
|-------|--------|-------------|
| `MINUS` | `-` | Default |
| `MINUS_EQUAL` | `-=` | `match('=')` first |
| `ARROW` | `->` | `match('>')` second |

#### From `*`

| Token | Symbol | Lexer Logic |
|-------|--------|-------------|
| `STAR` | `*` | Default |
| `POWER` | `**` | `match('*')` |
| `STAR_EQUAL` | `*=` | `match('=')` |
| `POWER_EQUAL` | `**=` | `match('*')` then `match('=')` |

#### From `/`

| Token | Symbol | Lexer Logic |
|-------|--------|-------------|
| `SLASH` | `/` | Default |
| `DOUBLE_SLASH` | `//` | `match('/')` |
| `SLASH_EQUAL` | `/=` | `match('=')` |
| `DOUBLE_SLASH_EQUAL` | `//=` | `match('/')` then `match('=')` |

#### From `%`

| Token | Symbol | Lexer Logic |
|-------|--------|-------------|
| `PERCENT` | `%` | Default |
| `PERCENT_EQUAL` | `%=` | `match('=')` |

#### From `@`

| Token | Symbol | Lexer Logic |
|-------|--------|-------------|
| `AT` | `@` | Default |
| `AT_EQUAL` | `@=` | `match('=')` |

#### From `&`

| Token | Symbol | Lexer Logic |
|-------|--------|-------------|
| `AMPERSAND` | `&` | Default |
| `AMPERSAND_EQUAL` | `&=` | `match('=')` |

#### From `|`

| Token | Symbol | Lexer Logic |
|-------|--------|-------------|
| `PIPE` | `\|` | Default |
| `PIPE_EQUAL` | `\|=` | `match('=')` |

#### From `^`

| Token | Symbol | Lexer Logic |
|-------|--------|-------------|
| `CARET` | `^` | Default |
| `CARET_EQUAL` | `^=` | `match('=')` |

#### From `=`

| Token | Symbol | Lexer Logic |
|-------|--------|-------------|
| `EQUAL` | `=` | Default |
| `EQUAL_EQUAL` | `==` | `match('=')` |

#### From `!`

| Token | Symbol | Lexer Logic |
|-------|--------|-------------|
| `NOT_EQUAL` | `!=` | `match('=')` succeeds |
| (error) | `!` | `match('=')` fails: `"Use 'not' for boolean negation."` |

#### From `<`

| Token | Symbol | Lexer Logic |
|-------|--------|-------------|
| `LESS` | `<` | Default |
| `LESS_EQUAL` | `<=` | `match('=')` |
| `LEFT_SHIFT` | `<<` | `match('<')` |
| `LEFT_SHIFT_EQUAL` | `<<=` | `match('<')` then `match('=')` |

#### From `>`

| Token | Symbol | Lexer Logic |
|-------|--------|-------------|
| `GREATER` | `>` | Default |
| `GREATER_EQUAL` | `>=` | `match('=')` |
| `RIGHT_SHIFT` | `>>` | `match('>')` |
| `RIGHT_SHIFT_EQUAL` | `>>=` | `match('>')` then `match('=')` |

#### From `:`

| Token | Symbol | Lexer Logic |
|-------|--------|-------------|
| `COLON` | `:` | Default |
| `WALRUS` | `:=` | `match('=')` |

#### From `.`

| Token | Symbol | Lexer Logic |
|-------|--------|-------------|
| `DOT` | `.` | Default |
| `ELLIPSIS` | `...` | `peekChar() == '.'` and `peekNext() == '.'` (two-char lookahead) |

---

## 7. Special Tokens

### NEWLINE

**Purpose**: Statement separator in both brace mode and indentation mode.

**Lexeme**: The literal `\n` character.

**Emission rules**:
- Emitted when the lexer encounters `\n` and `nestingDepth == 0`.
- Suppressed (skipped) when `nestingDepth > 0` (inside `()`, `[]`, or `{}` in Python
  mode).
- In brace mode, newlines between `{` and `}` are still emitted because `{}` does not
  affect `nestingDepth`.

**Parser usage**: The parser treats `NEWLINE` as a statement terminator, similar to `;`
in C-family languages. Multiple consecutive newlines may be emitted; the parser skips
extras.

### INDENT

**Purpose**: Marks the beginning of a new indented block in Python mode.

**Lexeme**: Empty string (`""`).

**Emission**: Only in Python mode (`useBraceBlocks == false`). Emitted when the leading
whitespace of a new line exceeds the current indentation level.

**Lifetime**: One `INDENT` token is emitted per indentation level increase. The parser
consumes it where a `LEFT_BRACE` would appear in brace mode.

### DEDENT

**Purpose**: Marks the end of an indented block in Python mode.

**Lexeme**: Empty string (`""`).

**Emission**: Only in Python mode. Emitted when the leading whitespace of a new line is
less than the current indentation level. Multiple `DEDENT` tokens may be emitted in
sequence when dedenting multiple levels at once. Remaining `DEDENT` tokens are also
emitted at `END_OF_FILE` to close all open blocks.

**Balancing**: The lexer guarantees that the total number of `DEDENT` tokens emitted
equals the total number of `INDENT` tokens emitted (plus any needed at EOF to balance).

### END_OF_FILE

**Purpose**: Marks the end of the token stream.

**Lexeme**: Empty string (from `makeToken(TokenType::END_OF_FILE)`).

**tokenTypeName**: Returns `"EOF"`.

**Guarantees**: Always the last token in the stream returned by `tokenize()`. The parser
should stop consuming when it sees this token.

### ERROR

**Purpose**: Represents a lexical error.

**Lexeme**: The source text from `start` to `current` at the point of the error.

**Side effect**: When an `ERROR` token is created, a diagnostic is also added to the
diagnostics vector via `errorToken()`.

**Default constructor**: The `Token()` default constructor creates an `ERROR` token,
which serves as a safe sentinel value.

---

## 8. Token Categories for Parser Reference

The Dragon parser uses a recursive descent strategy with the following precedence levels,
from lowest to highest. This section maps each level to the tokens that participate in
it, based on the parser implementation in `src/Parser.cpp` and `include/dragon/Parser.h`.

### Expression Precedence Table (Lowest to Highest)

| # | Level | Parser Method | Associativity | Token Types |
|---|-------|--------------|---------------|-------------|
| 1 | Assignment | `assignment()` | Right | `EQUAL`, `PLUS_EQUAL`, `MINUS_EQUAL`, `STAR_EQUAL`, `SLASH_EQUAL`, `DOUBLE_SLASH_EQUAL`, `PERCENT_EQUAL`, `POWER_EQUAL`, `AT_EQUAL`, `AMPERSAND_EQUAL`, `PIPE_EQUAL`, `CARET_EQUAL`, `LEFT_SHIFT_EQUAL`, `RIGHT_SHIFT_EQUAL` |
| 2 | Ternary | `ternary()` | Right | `IF` ... `ELSE` (conditional expression: `a if cond else b`) |
| 3 | Logical OR | `orExpr()` | Left | `OR` |
| 4 | Logical AND | `andExpr()` | Left | `AND` |
| 5 | Logical NOT | `notExpr()` | Prefix (right) | `NOT` |
| 6 | Comparison | `comparison()` | Left (chained) | `LESS`, `GREATER`, `LESS_EQUAL`, `GREATER_EQUAL`, `EQUAL_EQUAL`, `NOT_EQUAL`, `IN`, `IS` |
| 7 | Bitwise OR | `bitwiseOr()` | Left | `PIPE` |
| 8 | Bitwise XOR | `bitwiseXor()` | Left | `CARET` |
| 9 | Bitwise AND | `bitwiseAnd()` | Left | `AMPERSAND` |
| 10 | Shift | `shift()` | Left | `LEFT_SHIFT`, `RIGHT_SHIFT` |
| 11 | Addition | `term()` | Left | `PLUS`, `MINUS` |
| 12 | Multiplication | `factor()` | Left | `STAR`, `SLASH`, `PERCENT`, `DOUBLE_SLASH` |
| 13 | Unary | `unary()` | Prefix (right) | `MINUS`, `PLUS`, `TILDE` |
| 14 | Exponentiation | `power()` | Right | `POWER` |
| 15 | Await | `awaitExpr()` | Prefix | `AWAIT` |
| 16 | Call/Attr/Subscript | `call()` | Left (postfix) | `LEFT_PAREN`, `DOT`, `LEFT_BRACKET` |
| 17 | Primary | `primary()` | N/A | Literals and atoms (see below) |

### Tokens That Start Expressions (Primary)

The `primary()` parser method (in `src/Parser.cpp`) accepts the following token
types as the start of an expression:

| TokenType | What It Starts |
|-----------|---------------|
| `INTEGER` | Integer literal |
| `FLOAT` | Float literal |
| `STRING` | String literal |
| `TRUE` | Boolean `True` |
| `FALSE` | Boolean `False` |
| `NONE` | None literal |
| `IDENTIFIER` | Variable reference or name |
| `LEFT_PAREN` | Parenthesized expression or tuple |
| `LEFT_BRACKET` | List literal or list comprehension |
| `LEFT_BRACE` | Dict or set literal |
| `LAMBDA` | Lambda expression (handled before `primary()`) |
| `YIELD` | Yield expression (handled before `primary()`) |
| `NOT` | Logical not (handled at `notExpr()` level) |
| `MINUS`, `PLUS`, `TILDE` | Unary operators (handled at `unary()` level) |
| `AWAIT` | Await expression (handled at `awaitExpr()` level) |
| `ELLIPSIS` | Ellipsis literal `...` |

### Tokens That Start Statements

The parser's statement-level dispatch recognizes these tokens as statement starters:

| TokenType | Statement Type |
|-----------|---------------|
| `DEF` | Function definition |
| `CLASS` | Class definition |
| `IF` | If/elif/else statement |
| `WHILE` | While loop |
| `FOR` | For loop |
| `TRY` | Try/except/finally |
| `WITH` | With (context manager) statement |
| `RETURN` | Return statement |
| `BREAK` | Break statement |
| `CONTINUE` | Continue statement |
| `PASS` | Pass (no-op) statement |
| `IMPORT` | Import statement |
| `FROM` | From-import statement |
| `RAISE` | Raise statement |
| `ASSERT` | Assert statement |
| `DEL` | Delete statement |
| `GLOBAL` | Global declaration |
| `NONLOCAL` | Nonlocal declaration |
| `ASYNC` | Async def/for/with |
| `YIELD` | Yield statement |
| `AT` | Decorator (before `DEF` or `CLASS`) |

Any token not in this list that starts a line is treated as the beginning of an
expression statement (the expression is parsed and the result is discarded).

### Binary Operator Tokens by Precedence

For quick reference, here are all binary operator tokens grouped by their precedence
level (lowest binding first):

```
Level 3  (Logical OR):    or
Level 4  (Logical AND):   and
Level 6  (Comparison):    <  >  <=  >=  ==  !=  in  is
Level 7  (Bitwise OR):    |
Level 8  (Bitwise XOR):   ^
Level 9  (Bitwise AND):   &
Level 10 (Shift):         <<  >>
Level 11 (Additive):      +  -
Level 12 (Multiplicative): *  /  %  //
Level 14 (Power):         **
```

### Augmented Assignment Tokens

All 14 augmented assignment operators use the same right-hand side parsing (a full
expression). They are listed here for completeness:

```
+=   -=   *=   /=   //=   %=   **=   @=   &=   |=   ^=   <<=   >>=
```

The walrus operator (`:=`) is syntactically distinct -- it is a named expression, not an
assignment statement.

---

## Source File References

| Component | File | Lines |
|-----------|------|-------|
| `TokenType` enum | `include/dragon/Token.h` | 10-135 |
| `SourceLocation` struct | `include/dragon/Token.h` | 138-143 |
| `Token` class declaration | `include/dragon/Token.h` | 146-174 |
| `isKeyword()` declaration | `include/dragon/Token.h` | 177 |
| `keywordType()` declaration | `include/dragon/Token.h` | 180 |
| `Token` constructors | `src/Token.cpp` | 10-13 |
| `Token` accessors | `src/Token.cpp` | 15-17 |
| `Token::toString()` | `src/Token.cpp` | 21-29 |
| `Token::tokenTypeName()` | `src/Token.cpp` | 31-157 |
| Keyword map | `src/Token.cpp` | 163-210 |
| `isKeyword()` | `src/Token.cpp` | 212-214 |
| `keywordType()` | `src/Token.cpp` | 216-222 |
| Lexer test suite | `test/LexerTest.cpp` | - |
| Test helpers (lex, lexErrors) | `test/TestHelpers.h` | 14-53 |

---

## Previous Document

[001 - Dragon Lexer](001-lexer.md)

## Next Document

[003 - Dragon Parser](003-parser.md)
