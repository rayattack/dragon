# Dragon Lexer: Comprehensive Reference

> **Last Updated:** 2026-06-22

---

## Table of Contents

1. [Overview](#1-overview)
2. [Dual-Mode Lexing](#2-dual-mode-lexing)
3. [The Impl Struct](#3-the-impl-struct)
4. [Token Scanning (scanToken)](#4-token-scanning-scantoken)
5. [Indentation Handling](#5-indentation-handling)
6. [NEWLINE Token Suppression](#6-newline-token-suppression)
7. [String Literal Scanning](#7-string-literal-scanning)
8. [Number Scanning](#8-number-scanning)
9. [Identifier and Keyword Scanning](#9-identifier-and-keyword-scanning)
10. [Whitespace and Comment Handling](#10-whitespace-and-comment-handling)
11. [Error Handling](#11-error-handling)
12. [LexerOptions and LexerDiagnostic](#12-lexeroptions-and-lexerdiagnostic)
13. [Edge Cases and Design Decisions](#13-edge-cases-and-design-decisions)
14. [Public API Reference](#14-public-api-reference)

---

## 1. Overview

The Dragon lexer (`include/dragon/Lexer.h`, `src/Lexer.cpp`) is the first stage of the
Dragon compiler pipeline. It transforms raw source text into a flat sequence of `Token`
objects that the parser consumes. The lexer is responsible for:

- Recognizing all literal forms (integers, floats, strings, booleans, None)
- Identifying keywords and user-defined identifiers
- Producing operator and delimiter tokens (single and multi-character)
- Emitting `NEWLINE` tokens as statement separators
- Synthesizing `INDENT` and `DEDENT` tokens in Python indentation mode
- Suppressing `NEWLINE` tokens inside parenthesized and bracketed expressions
- Skipping whitespace and `#`-style line comments
- Reporting lexical errors as diagnostics

The lexer supports two distinct syntactic modes: **brace mode** for `.dr` files and
**indentation mode** for `.py` files. This is controlled by the `LexerOptions.useBraceBlocks`
flag.

### Position in the Pipeline

```
Source text --> [Lexer] --> Token stream --> [Parser] --> AST --> [Sema] --> [TypeChecker] --> [CodeGen (LLVM)]
```

The lexer is instantiated with a `std::string_view` of the source and a `LexerOptions`
struct. It exposes two consumption models:

- **Batch**: `tokenize()` returns a `std::vector<Token>` containing every token through
  the final `END_OF_FILE`.
- **Streaming**: `nextToken()` returns one token at a time; `peek()` and `peekAhead(n)`
  allow lookahead without consumption.

In practice, the Dragon parser uses the batch model -- `tokenize()` is called once and
the resulting vector is handed to the `Parser` constructor.

---

## 2. Dual-Mode Lexing

Dragon has two file formats that share the same token set but differ in how blocks are
delimited:

| Aspect | `.dr` (Brace Mode) | `.py` (Indentation Mode) |
|--------|-------------------|------------------------|
| `LexerOptions.useBraceBlocks` | `true` (default) | `false` |
| Block delimiters | `{` and `}` | `INDENT` and `DEDENT` |
| NEWLINE emission | Yes, at nesting depth 0 | Yes, at nesting depth 0 |
| Curly braces affect nesting | **No** | Yes |
| INDENT/DEDENT tokens | Never emitted | Emitted based on indentStack |

### Brace Mode (.dr)

In brace mode, `{` and `}` are emitted as `LEFT_BRACE` and `RIGHT_BRACE` tokens, and
they do **not** increment `nestingDepth`. This means newlines between `{` and `}` are
still significant -- they serve as statement separators, similar to Go. Only `()` and `[]`
suppress newlines in brace mode.

### Indentation Mode (.py)

In indentation mode, the lexer tracks leading whitespace at the start of each line. When
the indentation level increases, an `INDENT` token is emitted before the first token on
that line. When it decreases, one or more `DEDENT` tokens are emitted. Curly braces `{}`
in indentation mode **do** increment `nestingDepth` (for dict/set literals inside
expressions), suppressing newlines within them.

Both modes emit `NEWLINE` tokens as statement terminators when `nestingDepth == 0`.

---

## 3. The Impl Struct

The lexer uses the pimpl idiom. All mutable state lives inside `Lexer::Impl`, defined at
the top of `src/Lexer.cpp`:

```cpp
struct Lexer::Impl {
    std::string_view source;       // The full source text (non-owning view)
    LexerOptions options;          // Configuration (mode, tab width, filename)

    size_t start = 0;             // Start position of the current token being scanned
    size_t current = 0;           // Current read position in source
    size_t line = 1;              // Current line number (1-based)
    size_t column = 1;            // Current column number (1-based)
    size_t startColumn = 1;       // Column at the start of the current token

    std::vector<Token> peekedTokens;     // Lookahead buffer for peek()/peekAhead()
    std::vector<LexerDiagnostic> diagnostics;  // Accumulated error/warning messages
    std::stack<int> indentStack;         // Stack of indentation levels (Python mode)
    int pendingDedents = 0;              // Number of DEDENT tokens waiting to be emitted
    bool pendingIndent = false;          // Whether an INDENT token is waiting to be emitted
    bool atLineStart = true;             // Whether we're at the beginning of a line
    int nestingDepth = 0;                // Depth of () and [] nesting (and {} in Python mode)
};
```

### Field-by-Field Description

**`source`** (`std::string_view`): A non-owning view of the entire source text. The
caller must ensure the underlying string outlives the `Lexer`. Set in the constructor.

**`options`** (`LexerOptions`): Configuration copied into `Impl` at construction time.
Controls brace vs. indentation mode, tab width, and filename for diagnostics.

**`start`** (`size_t`, default `0`): The byte offset where the current token began. Set
to `current` at the beginning of each `scanToken()` call (in `nextToken()`). Used
by `makeToken()` to extract the lexeme as `source.substr(start, current - start)`.

**`current`** (`size_t`, default `0`): The byte offset of the next character to be read.
Incremented by `advance()`, `match()`, and other scanning methods.

**`line`** (`size_t`, default `1`): The current line number, 1-based. Incremented by
`advance()` whenever it consumes a `\n` character.

**`column`** (`size_t`, default `1`): The current column number, 1-based. Incremented by
`advance()` for every non-newline character; reset to `1` after a newline.

**`startColumn`** (`size_t`, default `1`): The column number where the current token
started. Captured alongside `start` and used in `SourceLocation` for tokens.

**`peekedTokens`** (`std::vector<Token>`): A buffer for tokens that have been produced by
`nextToken()` but returned to the stream because `peek()` or `peekAhead()` was called.
Consumed FIFO -- `nextToken()` drains from the front before scanning new tokens.

**`diagnostics`** (`std::vector<LexerDiagnostic>`): Accumulates all errors and warnings
encountered during lexing. Each diagnostic records a severity level, source location, and
human-readable message.

**`indentStack`** (`std::stack<int>`): A stack of integer indentation levels, used only
in Python mode. Initialized with a single `0` entry representing the base indentation
(in the constructor). Pushed/popped by `handleIndentation()` when indentation
changes.

**`pendingDedents`** (`int`, default `0`): When the indentation level decreases by
multiple levels at once (e.g., from 8 spaces back to 0), multiple DEDENT tokens must be
emitted. This counter tracks how many remain. Decremented by `nextToken()` each time a
DEDENT is returned.

**`pendingIndent`** (`bool`, default `false`): Set to `true` by `handleIndentation()` when
the indentation increases. `nextToken()` checks this flag and emits an `INDENT` token
before scanning the next real token.

**`atLineStart`** (`bool`, default `true`): Indicates that the lexer is positioned at
the beginning of a new line. Set to `true` when a `NEWLINE` token is emitted (in `scanToken()`).
In Python mode, when `atLineStart` is true, `nextToken()` calls `handleIndentation()`
before doing anything else. Set to `false` by `handleIndentation()` after
processing the line's leading whitespace.

**`nestingDepth`** (`int`, default `0`): Tracks how deeply nested we are inside `()`
and `[]` pairs (and `{}` in Python mode). When this is greater than zero, newline
characters are suppressed -- they become whitespace instead of `NEWLINE` tokens. See
Section 6 for the full rules.

### Initialization

The constructor (`Lexer::Lexer`) creates the `Impl` via `std::make_unique`,
copies in the source and options, and pushes `0` onto the `indentStack` to establish
the base indentation level.

---

## 4. Token Scanning (scanToken)

The `scanToken()` method is the heart of the lexer. It is called by
`nextToken()` after all pending INDENT/DEDENT tokens have been drained and whitespace has
been skipped. It consumes the next character via `advance()` and dispatches based on its
value.

### Dispatch Order

1. **Alphabetic or underscore** (`std::isalpha(c) || c == '_'`): Could be an identifier,
   a keyword, or a string with a prefix (`f`, `r`, `b`, `F`, `R`, `B`).
   - **String prefix check**: If the character is one of `f/F/r/R/b/B`,
     the lexer peeks ahead. If the next character is a quote (`"` or `'`), it consumes
     the quote and delegates to `scanString()`. For double prefixes (`rb`, `br`, `Rb`,
     `bR`, etc.), it checks two characters ahead.
   - **Otherwise**: Falls through to `scanIdentifier()`.

2. **Digit** (`std::isdigit(c)`): Delegates to `scanNumber()`.

3. **Quote** (`"` or `'`): Delegates to `scanString(c)`.

4. **Switch on character**: Handles all operators, delimiters, and newlines:

### Operator and Delimiter Recognition

The switch statement in `scanToken()` handles every non-alphanumeric token. For each starting
character, it uses `match()` to attempt consuming follow-up characters for multi-character
tokens, falling back to the single-character form:

| Character | Possible Tokens | Logic |
|-----------|----------------|-------|
| `(` | `LEFT_PAREN` | Increments `nestingDepth` |
| `)` | `RIGHT_PAREN` | Decrements `nestingDepth` (if > 0) |
| `[` | `LEFT_BRACKET` | Increments `nestingDepth` |
| `]` | `RIGHT_BRACKET` | Decrements `nestingDepth` (if > 0) |
| `{` | `LEFT_BRACE` | Increments `nestingDepth` only in Python mode |
| `}` | `RIGHT_BRACE` | Decrements `nestingDepth` only in Python mode (if > 0) |
| `,` | `COMMA` | |
| `;` | `SEMICOLON` | |
| `:` | `COLON`, `WALRUS` (`:=`), `TEMPLATE_CONTENT_OPEN` (`:{` inside `!{}` interp) | `match('=')` for walrus; `:{` scans a content body only when `options.inTemplateInterpolation` is set |
| `.` | `DOT`, `ELLIPSIS` (`...`) | Peek two chars ahead for `...` |
| `+` | `PLUS`, `PLUS_EQUAL` (`+=`) | `match('=')` |
| `-` | `MINUS`, `MINUS_EQUAL` (`-=`), `ARROW` (`->`) | `match('=')` then `match('>')` |
| `*` | `STAR`, `POWER` (`**`), `STAR_EQUAL` (`*=`), `POWER_EQUAL` (`**=`) | Nested `match` calls |
| `/` | `SLASH`, `DOUBLE_SLASH` (`//`), `SLASH_EQUAL` (`/=`), `DOUBLE_SLASH_EQUAL` (`//=`) | Nested `match` calls |
| `%` | `PERCENT`, `PERCENT_EQUAL` (`%=`) | `match('=')` |
| `@` | `AT`, `AT_EQUAL` (`@=`) | `match('=')` |
| `&` | `AMPERSAND`, `AMPERSAND_EQUAL` (`&=`) | `match('=')` |
| `\|` | `PIPE`, `PIPE_EQUAL` (`\|=`) | `match('=')` |
| `^` | `CARET`, `CARET_EQUAL` (`^=`) | `match('=')` |
| `~` | `TILDE` | No compound forms |
| `=` | `EQUAL`, `EQUAL_EQUAL` (`==`) | `match('=')` |
| `!` | `NOT_EQUAL` (`!=`) | `match('=')` required; bare `!` is an error |
| `<` | `LESS`, `LESS_EQUAL` (`<=`), `LEFT_SHIFT` (`<<`), `LEFT_SHIFT_EQUAL` (`<<=`) | Nested `match` |
| `>` | `GREATER`, `GREATER_EQUAL` (`>=`), `RIGHT_SHIFT` (`>>`), `RIGHT_SHIFT_EQUAL` (`>>=`) | Nested `match` |
| `\n` | `NEWLINE` or skip | See Section 6 |

### The match() Helper

`match(char expected)` is the key primitive for multi-character token
recognition. It peeks at the current character; if it equals `expected`, it advances past
it and returns `true`. Otherwise it returns `false` without consuming anything. This
allows clean chaining:

```cpp
case '*':
    if (match('*')) {
        if (match('=')) return makeToken(TokenType::POWER_EQUAL);  // **=
        return makeToken(TokenType::POWER);                         // **
    }
    if (match('=')) return makeToken(TokenType::STAR_EQUAL);        // *=
    return makeToken(TokenType::STAR);                               // *
```

### Newline Handling in scanToken

When the lexer encounters `\n` (the `case '\n'` arm of `scanToken()`), the behavior depends on `nestingDepth`:

- **If `nestingDepth == 0`**: Sets `atLineStart = true` and returns a `NEWLINE` token.
- **If `nestingDepth > 0`**: The newline is inside a parenthesized/bracketed expression.
  The lexer resets `start` and `startColumn`, calls `skipWhitespaceAndComments()`, and
  recursively calls `scanToken()` to return the next meaningful token. The newline is
  effectively invisible.

### Default Case

Any character not matched by the above dispatch produces an error token:
```cpp
return errorToken(std::string("Unexpected character '") + c + "'");
```

---

## 5. Indentation Handling

Indentation handling is active only in Python mode (`useBraceBlocks == false`). The
mechanism closely follows the Python language specification.

### The indentStack

The `indentStack` is a `std::stack<int>` initialized with a single `0` entry. Each entry
represents an indentation level (measured in columns) that has introduced a new block.
The stack is monotonically increasing from bottom to top.

Example progression:
```
Initial:      [0]
After 4-space indent:  [0, 4]
After 8-space indent:  [0, 4, 8]
Dedent to 4:  [0, 4]
Dedent to 0:  [0]
```

### handleIndentation()

Called from `nextToken()` when `atLineStart` is true and we are in Python mode. This
method:

1. **Counts leading whitespace**:
   - Each space counts as 1.
   - Each tab counts as `options.tabWidth` (default 4).
   - Advances past all leading spaces and tabs.

2. **Skips blank lines and comment-only lines**: If the line is empty, ends
   at EOF, or starts with `#`, no indentation change is recorded. This prevents blank
   lines from generating spurious DEDENT tokens.

3. **Compares to current level** (`currentIndent()` returns `indentStack.top()`):
   - **Indent increased** (`indent > currentLevel`): Pushes the new level onto
     `indentStack` and sets `pendingIndent = true`.
   - **Indent decreased** (`indent < currentLevel`): Pops levels from `indentStack`
     and increments `pendingDedents` for each pop, until `currentIndent()` matches
     `indent`.
   - **Indent unchanged**: No tokens emitted.

4. **Indentation mismatch error**: After popping, if `indent` does not
   exactly match `currentIndent()`, a diagnostic error is emitted:
   `"Indentation does not match any outer level"`.

5. **Clears `atLineStart`**: Sets `atLineStart = false` so that subsequent
   tokens on this line do not re-trigger indentation processing.

### INDENT/DEDENT Emission in nextToken()

The `nextToken()` method checks for pending indentation tokens before doing anything
else:

1. **Pending DEDENTs**: If `pendingDedents > 0`, decrement and return a
   `DEDENT` token.
2. **Pending INDENT**: If `pendingIndent` is true, clear it and return an
   `INDENT` token.
3. **At line start in Python mode**: Call `handleIndentation()`, then check
   again for pending INDENT/DEDENT.

### EOF Dedent Emission

At end of file (the EOF branch of `nextToken()`), if in Python mode and the `indentStack` has more than one
entry, the lexer emits all remaining DEDENTs to properly close every open block:

```cpp
if (!impl_->options.useBraceBlocks && impl_->indentStack.size() > 1) {
    impl_->indentStack.pop();
    impl_->pendingDedents = static_cast<int>(impl_->indentStack.size()) - 1;
    while (impl_->indentStack.size() > 1) impl_->indentStack.pop();
    return makeToken(TokenType::DEDENT, "");
}
```

This ensures the parser always sees a balanced number of INDENT and DEDENT tokens.

### Tab Handling

Tabs are converted to spaces using `options.tabWidth` (default 4). Each tab character
adds `tabWidth` columns to the indentation measurement. This is a simple additive model,
not the "tab stops" model used by CPython. The difference:

- Dragon: `\t` always adds 4 columns (configurable).
- CPython: `\t` rounds up to the next multiple of 8.

This is a deliberate simplification. Users who mix tabs and spaces may see different
behavior compared to CPython.

---

## 6. NEWLINE Token Suppression

One of the lexer's most important behaviors is suppressing `NEWLINE` tokens inside
grouped expressions. This allows multi-line expressions like:

```python
result = (
    a + b +
    c + d
)

items = [
    1, 2,
    3, 4
]
```

### The nestingDepth Mechanism

The `nestingDepth` counter tracks how deeply nested the lexer is inside grouping
constructs. When `nestingDepth > 0`, newline characters are treated as whitespace
instead of producing `NEWLINE` tokens.

**What increments nestingDepth:**

| Token | Brace Mode | Python Mode |
|-------|-----------|-------------|
| `(` | +1 | +1 |
| `[` | +1 | +1 |
| `{` | **no change** | +1 |

**What decrements nestingDepth (if > 0):**

| Token | Brace Mode | Python Mode |
|-------|-----------|-------------|
| `)` | -1 | -1 |
| `]` | -1 | -1 |
| `}` | **no change** | -1 |

### The Critical Brace Mode Detail

In brace mode (`.dr` files), curly braces `{` and `}` do **not** affect `nestingDepth`.
This is the critical design decision encoded in the `case '{'` / `case '}'` arms of `scanToken()`:

```cpp
case '{':
    // In brace mode, don't count {} for nesting (only () and [] suppress newlines)
    if (!impl_->options.useBraceBlocks) impl_->nestingDepth++;
    return makeToken(TokenType::LEFT_BRACE);
case '}':
    if (!impl_->options.useBraceBlocks && impl_->nestingDepth > 0) impl_->nestingDepth--;
    return makeToken(TokenType::RIGHT_BRACE);
```

The reasoning: in brace mode, `{` and `}` delimit statement blocks (function bodies,
if/else bodies, etc.). Newlines inside these blocks must remain significant as statement
separators. If `{}` suppressed newlines, the parser could not tell where one statement
ends and the next begins.

In Python mode, `{}` is used for dict/set literals (blocks are indentation-based), so
newlines inside `{}` should be suppressed just like inside `()` and `[]`.

### Suppression Points

Newline suppression happens in two places:

1. **`scanToken()`**: When `\n` is encountered as the scanned character. If
   `nestingDepth > 0`, instead of returning `NEWLINE`, the lexer skips whitespace and
   recursively calls `scanToken()`.

2. **`skipWhitespaceAndComments()`**: In brace mode with `nestingDepth > 0`,
   newlines encountered during whitespace skipping are silently consumed. At depth 0,
   the method returns, letting `scanToken()` handle the newline.

### Guard Against Negative Depth

The decrement is guarded: `if (impl_->nestingDepth > 0)`. An unmatched `)`, `]`, or `}`
will not make `nestingDepth` go negative. This is purely defensive -- the parser catches
mismatched delimiters at a higher level.

---

## 7. String Literal Scanning

String scanning is handled by `scanString(char quote)`. The `quote` parameter
is the opening quote character (`"` or `'`). By the time `scanString` is called, the
opening quote has already been consumed by the caller.

### String Prefixes

String prefixes are detected in `scanToken()` before `scanString()` is called (in the
identifier-or-prefix branch). The following prefixes are recognized:

| Prefix | Meaning | Case-insensitive |
|--------|---------|-----------------|
| `f` / `F` | f-string (format string) | Yes |
| `r` / `R` | Raw string | Yes |
| `b` / `B` | Byte string | Yes |
| `rb` / `rB` / `Rb` / `RB` | Raw byte string | Yes |
| `br` / `bR` / `Br` / `BR` | Raw byte string | Yes |

The prefix detection works by checking:
1. Is the first character one of `f/F/r/R/b/B`?
2. Is the next character a quote (`"` or `'`)?
3. For double prefixes: Is the next character the second prefix letter, and is the
   character after that a quote?

**Important**: The lexer recognizes prefixes but does **not** distinguish token types by
prefix. All prefixed strings produce `TokenType::STRING`. The prefix is included in the
token's lexeme (e.g., the lexeme for `f"hello"` is `f"hello"`), allowing later stages
to interpret the prefix.

The one exception is `b"..."` strings, which could theoretically produce `TokenType::BYTES`,
but in practice the current lexer emits `STRING` for all prefixed strings because the
prefix-detection code delegates to `scanString()` which always returns `STRING`.

### Triple-Quoted Strings

After consuming the first quote, `scanString()` checks whether the next two characters
are also the same quote:

```cpp
bool isTriple = false;
if (peekChar() == quote && peekNext() == quote) {
    advance(); // second quote
    advance(); // third quote
    isTriple = true;
}
```

Triple-quoted strings allow embedded newlines. The closing delimiter is three consecutive
matching quotes. A single quote character inside a triple-quoted string is allowed and
does not terminate the string.

### Escape Sequences

Inside the string body, the lexer recognizes escape sequences by looking for `\`:

```cpp
if (c == '\\') {
    advance();           // consume the backslash
    if (!isAtEnd()) advance();  // consume the escaped character
    continue;
}
```

The lexer does **not** interpret escape sequences -- it simply skips two characters
(backslash + whatever follows). The actual interpretation of escape sequences (`\n`,
`\t`, `\\`, `\'`, `\"`, `\0`, `\a`, `\b`, `\f`, `\r`, `\v`, hex escapes, etc.) is
deferred to later compiler stages or runtime. The lexer's job is only to correctly
identify the boundaries of the string.

This approach means all of the following escape sequences pass through the lexer
correctly:
- `\n` (newline), `\t` (tab), `\\` (literal backslash)
- `\'` (single quote), `\"` (double quote)
- `\0` (null), `\a` (bell), `\b` (backspace)
- `\f` (form feed), `\r` (carriage return), `\v` (vertical tab)
- `\xNN` (hex escape), `\uNNNN` (unicode), `\UNNNNNNNN` (unicode)
- `\ooo` (octal escape)

### Unterminated String Errors

Two error conditions are detected:

1. **Newline in non-triple string**: If `\n` is encountered and `isTriple` is
   false, the error `"Unterminated string literal"` is reported.

2. **End of file**: If the source ends before the closing quote(s), the same
   error is reported.

Both return an `ERROR` token via `errorToken()`.

---

## 8. Number Scanning

Number scanning is handled by `scanNumber()`. The first digit has already been
consumed by `scanToken()` before `scanNumber()` is called.

### Number Formats

The lexer recognizes the following numeric literal forms:

#### Hexadecimal: `0x` or `0X` prefix

```
0x1F   0xFF   0x1_0000   0XDEAD_BEEF
```

After consuming the `x`/`X`, the lexer reads hex digits (`0-9`, `a-f`, `A-F`) and
underscores. At least one hex digit must follow the prefix, or the error
`"Invalid hexadecimal literal"` is reported. Returns `TokenType::INTEGER`.

#### Binary: `0b` or `0B` prefix

```
0b1010   0B1111_0000
```

After consuming the `b`/`B`, the lexer reads binary digits (`0`, `1`) and underscores.
At least one binary digit must follow, or `"Invalid binary literal"` is reported. Returns
`TokenType::INTEGER`.

#### Octal: `0o` or `0O` prefix

```
0o77   0O755   0o1_777
```

After consuming the `o`/`O`, the lexer reads octal digits (`0-7`) and underscores. At
least one octal digit must follow, or `"Invalid octal literal"` is reported. Returns
`TokenType::INTEGER`.

#### Decimal Integer

```
42   1_000_000   0
```

The lexer consumes digits and underscores. Underscore separators are allowed anywhere
within the digit sequence (following Python's convention). Returns `TokenType::INTEGER`
unless a decimal point or exponent follows.

#### Floating-Point

```
3.14   1.5e-3   2.5E+10   1e10   1_000.5
```

A decimal number becomes a float if it has:
- A decimal point followed by at least one digit: `peekChar() == '.'` and
  `std::isdigit(peekNext())`. This two-character lookahead prevents `1.method()` from
  being mislexed -- the `.` must be followed by a digit.
- A scientific notation exponent: `e` or `E`, optionally followed by `+` or
  `-`, then digits. If the exponent prefix is present but no digits follow, the error
  `"Invalid numeric literal: expected digits after exponent"` is reported.

Underscores are allowed in the fractional and exponent parts. Returns `TokenType::FLOAT`.

---

## 9. Identifier and Keyword Scanning

### scanIdentifier()

After the first character (alpha or `_`) has been consumed by `scanToken()`, the lexer
reads all subsequent alphanumeric characters and underscores:

```cpp
while (!isAtEnd() && (std::isalnum(peekChar()) || peekChar() == '_')) {
    advance();
}
```

The identifier text is extracted as a `std::string_view` from `start` to `current`, then
passed to `keywordType()` to determine if it's a keyword:

```cpp
std::string_view text = impl_->source.substr(impl_->start, impl_->current - impl_->start);
TokenType type = keywordType(text);
return makeToken(type);
```

### The `template` Contextual Form

Before the keyword lookup, `scanIdentifier()` special-cases the identifier `template`. If
`template` is immediately followed (after spaces/tabs, not a newline) by `{`, or by
`[ContentType]` then `{` or `(`, the lexer captures the brace-balanced body and emits a
single `TEMPLATE` token (via `scanTemplateBody()`) rather than the `template` identifier
plus its body tokens. A typed file template `template[X]("file.html")` emits a `TEMPLATE`
token carrying just the content type. If no `{`/`[` follows, the lexer rewinds and
`template` is lexed as an ordinary `IDENTIFIER`. This is the only contextual handling
inside the lexer itself.

### Keyword Lookup

The `keywordType()` function (defined in `src/Token.cpp`) performs a lookup in
a static `std::unordered_map<std::string_view, TokenType>`. If the identifier matches a
keyword, the corresponding `TokenType` is returned. Otherwise, `TokenType::IDENTIFIER` is
returned.

This means the keywords in the map are **reserved** -- you cannot use `if`, `for`, `class`,
etc. as variable names.

The keyword map has 43 entries: 32 Python keywords, the `fire` concurrency keyword, the 3
title-case literals (`True`, `False`, `None`), their 3 lowercase `.dr`-mode aliases
(`true`, `false`, `none`), and 4 Dragon extensions (`catch`, `const`, `static`, `extern`).
See `002-tokens.md` for the complete list.

A handful of names that look like keywords are deliberately **not** in this map. `match`,
`case`, and `type` are soft keywords matched by lexeme in the parser, so the lexer always
emits them as `IDENTIFIER`. `thread` and `enum` are contextual keywords resolved at
statement start in the parser (also lexed as `IDENTIFIER`); the `THREAD`/`ENUM` enum
constants exist but the lexer never produces them.

### Identifier vs. String Prefix Ambiguity

Characters `f`, `r`, `b` (and uppercase variants) could be either the start of an
identifier or a string prefix. The lexer resolves this by checking whether a quote
follows immediately. `f"hello"` is an f-string; `foo` is an identifier. This check
happens in `scanToken()` before falling through to `scanIdentifier()`.

---

## 10. Whitespace and Comment Handling

### skipWhitespaceAndComments()

Called at the beginning of each `nextToken()` cycle (after indentation handling), this
method skips:

- **Spaces** (`' '`): Always skipped.
- **Tabs** (`'\t'`): Always skipped.
- **Carriage returns** (`'\r'`): Always skipped.
- **Newlines** (`'\n'`): Conditionally skipped based on mode and nesting depth.
- **Comments** (`'#'`): Everything from `#` to end of line is skipped.

#### Newline Treatment in skipWhitespace

The newline handling in `skipWhitespaceAndComments()` is mode-dependent:

**Brace mode**:
- If `nestingDepth > 0`: Newlines are whitespace. `advance()` past them.
- If `nestingDepth == 0`: Return, letting `scanToken()` handle the `\n` as a `NEWLINE`
  token.

**Python mode**:
- Always returns when a newline is encountered, regardless of `nestingDepth`. The
  newline's significance is determined by `scanToken()`.

#### Comment Handling

Line comments begin with `#` and extend to the end of the line:

```cpp
case '#':
    while (!isAtEnd() && peekChar() != '\n')
        advance();
    break;
```

The `\n` itself is not consumed, so it remains available for newline processing in the
next iteration of the while loop. Dragon does not support multi-line comments (`/* */`).

---

## 11. Error Handling

### Error Reporting Mechanism

The lexer does not throw exceptions. Instead, errors are collected in the `diagnostics`
vector and error tokens (`TokenType::ERROR`) are returned to the caller. This allows the
lexer to recover and continue scanning, producing as many diagnostics as possible in a
single pass.

### errorToken()

Creates an `ERROR` token and records a diagnostic:

```cpp
Token Lexer::errorToken(const std::string& message) {
    addDiagnostic(LexerDiagnostic::Level::Error, message);
    return makeToken(TokenType::ERROR);
}
```

### addDiagnostic()

Records a diagnostic with the current source location:

```cpp
void Lexer::addDiagnostic(LexerDiagnostic::Level level, const std::string& message) {
    impl_->diagnostics.push_back({
        level,
        {impl_->options.filename, impl_->line, impl_->column, impl_->current},
        message
    });
}
```

### Error Conditions

| Error | Message | Trigger |
|-------|---------|---------|
| Unterminated string | `"Unterminated string literal"` | EOF or newline before closing quote |
| Invalid hex literal | `"Invalid hexadecimal literal"` | `0x` with no hex digits |
| Invalid binary literal | `"Invalid binary literal"` | `0b` with no binary digits |
| Invalid octal literal | `"Invalid octal literal"` | `0o` with no octal digits |
| Invalid exponent | `"Invalid numeric literal: expected digits after exponent"` | `e`/`E` with no digits |
| Bare `!` | `"Unexpected character '!'. Use 'not' for boolean negation."` | `!` not followed by `=` |
| Indentation mismatch | `"Indentation does not match any outer level"` | Dedent to non-existent level |
| Unexpected character | `"Unexpected character 'X'"` | Any unrecognized character |

### Querying Diagnostics

After lexing, diagnostics can be retrieved via:

- `diagnostics()`: Returns a `const std::vector<LexerDiagnostic>&`.
- `hasErrors()`: Returns `true` if any diagnostic has level `Error` (iterates the
  diagnostics vector, checking each level).

---

## 12. LexerOptions and LexerDiagnostic

### LexerOptions (defined in `include/dragon/Lexer.h`, line 21)

```cpp
struct LexerOptions {
    bool useBraceBlocks = true;          // true = .dr mode, false = .py mode
    int tabWidth = 4;                    // Number of spaces per tab for indentation
    std::string filename = "<stdin>";    // Filename for diagnostic locations
    bool inTemplateInterpolation = false; // CodeGen-set; enables the :{ ... } alias
};
```

**`useBraceBlocks`**: The primary mode switch. Defaults to `true` (Dragon/brace mode).
Set to `false` for Python/indentation mode. Affects:
- Whether `INDENT`/`DEDENT` tokens are emitted
- Whether `{}` increments `nestingDepth`
- Newline handling in `skipWhitespaceAndComments()`

**`tabWidth`**: Used only in Python mode during indentation counting. Each `\t`
contributes `tabWidth` columns. Default is 4.

**`filename`**: Embedded into every `SourceLocation` produced by the lexer. Used for
diagnostic messages. Default is `"<stdin>"`. The test harness sets this to `"<test>"`.

**`inTemplateInterpolation`**: Set by CodeGen when it re-lexes the body of a `!{ ... }`
template block-interpolation. When true, `:{` is recognized as `TEMPLATE_CONTENT_OPEN`
(the terse content-mode alias). Outside that context `:` and `{` keep their normal
meanings, so `:{` is not special. Defaults to `false`.

### LexerDiagnostic (defined in `include/dragon/Lexer.h`, line 13)

```cpp
struct LexerDiagnostic {
    enum class Level { Warning, Error };
    Level level;
    SourceLocation location;
    std::string message;
};
```

**`level`**: Either `Warning` or `Error`. Currently the lexer only emits `Error`-level
diagnostics -- no warnings are generated.

**`location`**: A `SourceLocation` capturing where the error occurred (filename, line,
column, byte offset).

**`message`**: A human-readable description of the problem.

---

## 13. Edge Cases and Design Decisions

### Why Token Stores std::string, Not string_view

The `Token` class stores its lexeme as `std::string` (owned copy) rather than
`std::string_view` (non-owning reference). This is a deliberate choice:

1. **Lifetime safety**: Tokens outlive the lexer and may outlive the source string.
   The parser stores tokens in a vector; the AST stores token values. If tokens held
   `string_view`s, any reallocation or deallocation of the source would create dangling
   references.

2. **Simplicity**: Owning strings eliminate an entire class of use-after-free bugs.
   The cost is additional memory allocation and copying, but tokens are small and the
   overhead is negligible compared to later compilation stages.

### Why SourceLocation Stores std::string for Filename

The `SourceLocation` struct stores `filename` as `std::string` rather than
`std::string_view` or `const char*`. This means every token carries a full copy of the
filename string. The reasons:

1. **Ownership clarity**: `SourceLocation` values are copied into AST nodes, diagnostics,
   and error messages. If `filename` were a `string_view`, the original string would need
   to outlive all of these.

2. **Safety over efficiency**: In a compiler with multiple source files, filenames may
   come from different sources (command line arguments, import resolution). Owning the
   string avoids tracking provenance.

The per-token overhead is mitigated by `std::string`'s small-string optimization (SSO):
filenames like `"<test>"` or `"main.dr"` fit in the inline buffer on most implementations.

### NEWLINE Emission in Both Modes

Both brace mode and indentation mode emit `NEWLINE` tokens. This is unusual -- many
brace-delimited languages treat newlines as pure whitespace. Dragon takes this approach
because:

1. **Statement separation**: In Dragon's brace mode, newlines serve as implicit semicolons
   (similar to Go). The parser uses `NEWLINE` to determine where one statement ends and
   the next begins, without requiring explicit `;`.

2. **Unified parser**: The parser can handle both modes with the same statement-parsing
   logic. The difference is only in block delimiters (`{`/`}` vs. `INDENT`/`DEDENT`),
   not in statement termination.

### The Ellipsis Detection

The `...` (ellipsis) token requires looking two characters ahead from the first `.`:

```cpp
case '.':
    if (peekChar() == '.' && peekNext() == '.') {
        advance(); // second .
        advance(); // third .
        return makeToken(TokenType::ELLIPSIS);
    }
    return makeToken(TokenType::DOT);
```

This means `..` (two dots) is not a valid token -- it would be lexed as `DOT DOT`. Only
exactly three dots produce `ELLIPSIS`.

### Float vs. Method Call Ambiguity

The float scanner requires a digit after the decimal point:

```cpp
if (!isAtEnd() && peekChar() == '.' && !isAtEnd(1) && std::isdigit(peekNext()))
```

This prevents `1.method()` from being mislexed as `1.` (float) followed by `method()`.
Instead, `1` is an integer, `.` is a dot, and `method` is an identifier. However, this
also means that `1.` (trailing dot) is not a valid float literal -- Dragon requires at
least one digit after the decimal point.

### Bare ! is an Error

Unlike C-family languages, `!` is not a valid unary operator in Dragon (or Python).
Boolean negation uses the `not` keyword. The lexer provides a helpful error message:

```
"Unexpected character '!'. Use 'not' for boolean negation."
```

However, `!=` is a valid comparison operator and is correctly recognized.

### peekAhead() Token Buffer Management

The `peekAhead(int n)` method needs to buffer multiple tokens without
consuming them from the normal stream. It does this by:

1. Saving the current `peekedTokens` buffer.
2. Clearing it temporarily.
3. Calling `nextToken()` to produce a fresh token.
4. Restoring the saved buffer and appending the new token.

This is a somewhat unusual design necessitated by the fact that `nextToken()` checks
`peekedTokens` first. Without the save/restore, `nextToken()` would return a previously
peeked token instead of scanning a new one.

### The advance() Side Effects

`advance()` does more than just increment `current` -- it also updates `line`
and `column`:

```cpp
char Lexer::advance() {
    char c = impl_->source[impl_->current++];
    if (c == '\n') {
        impl_->line++;
        impl_->column = 1;
    } else {
        impl_->column++;
    }
    return c;
}
```

This means line/column tracking is always accurate, even inside string literals that
span multiple lines. However, `match()` only increments `column`, not `line`,
because `match()` is never used to consume newline characters.

---

## 14. Public API Reference

### Constructor

```cpp
explicit Lexer(std::string_view source, LexerOptions options = {});
```

Creates a lexer for the given source text. The `source` must remain valid for the
lifetime of the `Lexer`. Copy semantics are disabled.

### Tokenization

```cpp
std::vector<Token> tokenize();
```

Scans the entire source and returns all tokens, ending with `END_OF_FILE`. This is the
primary API used by the parser.

```cpp
Token nextToken();
```

Returns the next token, advancing the lexer's position. Handles pending INDENT/DEDENT
tokens, indentation processing, and whitespace skipping before delegating to
`scanToken()`.

```cpp
Token peek();
```

Returns the next token without consuming it. Buffers the token internally; subsequent
calls to `peek()` return the same token until `nextToken()` is called.

```cpp
Token peekAhead(int n);
```

Returns the token `n` positions ahead (0-indexed: `peekAhead(0)` is equivalent to
`peek()`). Buffers all intermediate tokens.

### State Queries

```cpp
bool isAtEnd() const;
```

Returns `true` if the read position has reached or passed the end of the source text.

```cpp
const std::vector<LexerDiagnostic>& diagnostics() const;
```

Returns all diagnostics (errors and warnings) accumulated during lexing.

```cpp
bool hasErrors() const;
```

Returns `true` if any diagnostic has level `Error`.

---

## Source File References

| Component | Header | Implementation |
|-----------|--------|---------------|
| Lexer class | `include/dragon/Lexer.h` | `src/Lexer.cpp` |
| Token class | `include/dragon/Token.h` | `src/Token.cpp` |
| Keyword map | -- | `src/Token.cpp` (the static `keywords` map) |
| Test suite | -- | `test/LexerTest.cpp` |
| Test helpers | `test/TestHelpers.h` | -- |

---

## Previous Document

[000 - Dragon Language: Project Overview and Architecture](000-dragon-lang.md)

## Next Document

[002 - Dragon Token System](002-tokens.md)
