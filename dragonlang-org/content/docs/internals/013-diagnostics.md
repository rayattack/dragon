# Dragon Diagnostic and Error Reporting System

> **Version:** 0.2.0
> **Files:** `include/dragon/DiagnosticFormatter.h`, `src/DiagnosticFormatter.cpp`, and diagnostic structs in each compiler stage header
> **Last updated:** 2026-02-21

---

## 1. Overview

Dragon uses a multi-layered diagnostic system. Each stage of the compilation pipeline defines its own diagnostic struct to report warnings and errors. These diagnostics are collected during processing and then formatted for display by the `DiagnosticFormatter` class, which supports two output modes: Dragon-branded ("Dragon theme") and classic compiler-style ("plain mode").

The flow is:

```
Source code
  -> Lexer         (produces LexerDiagnostic)
  -> Parser        (produces ParserDiagnostic)
  -> TypeHintEnforcer (.py files only; produces EnforcerDiagnostic)
  -> Sema          (produces SemaDiagnostic)
  -> TypeChecker   (produces TypeDiagnostic)
  -> CodeGen             (produces CodeGenDiagnostic)
  -> Driver collects all diagnostics
  -> DiagnosticFormatter formats for display
  -> Output to stderr
```

---

## 2. Diagnostic Types Across the Pipeline

Every compiler stage defines a diagnostic struct with a consistent shape. All diagnostic structs share the same three fields:

| Field | Type | Description |
|-------|------|-------------|
| `level` | `Level` enum | Severity: `Warning` or `Error` |
| `location` | `SourceLocation` | File, line, and column where the issue was detected |
| `message` | `std::string` | Human-readable description of the problem |

### 2.1 LexerDiagnostic

**Defined in:** `include/dragon/Lexer.h`

```cpp
struct LexerDiagnostic {
    enum class Level { Warning, Error };
    Level level;
    SourceLocation location;
    std::string message;
};
```

**When produced:** During tokenization. Examples:
- Unterminated string literal
- Invalid character in source
- Unexpected token at start of line (indentation errors in `.py` mode)

**Access:** `lexer.diagnostics()` returns a `std::vector<LexerDiagnostic>`.

### 2.2 ParserDiagnostic

**Defined in:** `include/dragon/Parser.h`

```cpp
struct ParserDiagnostic {
    enum class Level { Warning, Error };
    Level level;
    SourceLocation location;
    std::string message;
};
```

**When produced:** During parsing. Examples:
- Expected token not found (e.g., missing closing brace)
- Invalid syntax in expression
- Unexpected token in statement position
- Missing type annotation in `.dr` mode (when `requireTypes` is set)

**Access:** `parser.diagnostics()` returns a `std::vector<ParserDiagnostic>`. The convenience function `parser.hasErrors()` checks if any Error-level diagnostics exist.

### 2.3 SemaDiagnostic

**Defined in:** `include/dragon/Sema.h`

```cpp
struct SemaDiagnostic {
    enum class Level { Warning, Error };
    Level level;
    SourceLocation location;
    std::string message;
};
```

**When produced:** During semantic analysis (name resolution and scope checking). Examples:
- Undeclared variable usage
- Duplicate variable declaration in same scope
- `break`/`continue` outside of loop
- `return` outside of function

**Access:** `sema.diagnostics()` returns a `std::vector<SemaDiagnostic>`.

### 2.4 TypeDiagnostic

**Defined in:** `include/dragon/TypeChecker.h`

```cpp
struct TypeDiagnostic {
    enum class Level { Warning, Error };
    Level level;
    SourceLocation location;
    std::string message;
};
```

**When produced:** During type checking. Examples:
- Type mismatch in assignment (`int` assigned to `str` variable)
- Invalid operand types for binary operator
- Wrong number of arguments in function call
- Return type does not match function signature
- Unknown type name in annotation

**Access:** `typeChecker.diagnostics()` returns a `std::vector<TypeDiagnostic>`.

### 2.5 EnforcerDiagnostic

**Defined in:** `include/dragon/TypeHintEnforcer.h`

```cpp
struct EnforcerDiagnostic {
    enum class Level { Error, Warning };
    Level level = Level::Error;
    SourceLocation location;
    std::string message;
};
```

**When produced:** During type hint enforcement on `.py` files. This is a separate pass from the TypeChecker -- it checks for the **presence** of type annotations, not their correctness. Examples:
- Function parameter missing type annotation
- Function missing return type annotation
- Module-level variable missing type annotation

**Note:** The `Level` enum order is reversed (`Error, Warning`) compared to other diagnostic types (`Warning, Error`), and it has a default value of `Level::Error`. This is the only diagnostic type with a default level.

**Access:** `enforcer.diagnostics()` returns a `std::vector<EnforcerDiagnostic>`.

### 2.6 CodeGenDiagnostic

**Defined in:** `include/dragon/CodeGen.h`

```cpp
struct CodeGenDiagnostic {
    enum class Level { Warning, Error };
    Level level;
    SourceLocation location;
    std::string message;
};
```

**When produced:** During LLVM IR generation. Examples:
- Unsupported AST node type (feature not yet implemented in CodeGen)
- Failed LLVM module verification
- Internal code generation errors

**Access:** `codegen.diagnostics()` returns a `std::vector<CodeGenDiagnostic>`.

---

## 3. The DiagnosticFormatter Class

### Location

- **Header:** `include/dragon/DiagnosticFormatter.h`
- **Implementation:** `src/DiagnosticFormatter.cpp`

### Purpose

`DiagnosticFormatter` is a centralized formatting facility that replaces duplicated error-printing loops throughout the Driver. It takes raw diagnostic data (filename, line, column, level, message) and produces formatted strings in one of two styles.

### Construction

```cpp
DiagnosticFormatter formatter;                    // Default: Dragon theme, suggestions on, color off
DiagnosticFormatter formatter(customStyle);        // Custom style
```

---

## 4. DiagnosticStyle

```cpp
struct DiagnosticStyle {
    bool useDragonTheme = true;   // Dragon-branded errors vs plain
    bool showSuggestions = true;   // Show suggestion hints
    bool colorOutput = false;     // ANSI color codes (default off for test stability)
};
```

| Field | Default | Description |
|-------|---------|-------------|
| `useDragonTheme` | `true` | When true, uses Dragon-branded format ("DRAGON SCALE ERROR: ..."). When false, uses classic compiler format ("file:line:col: level: message"). |
| `showSuggestions` | `true` | When true, appends a "Suggestion: ..." line below the error if a suggestion string is provided. |
| `colorOutput` | `false` | When true, wraps level labels and suggestion keywords in ANSI escape codes. Default is off to ensure test output stability (ANSI codes would interfere with string comparisons in tests). |

---

## 5. Output Formats

### 5.1 Dragon Theme Format

When `useDragonTheme` is true, errors are formatted as:

```
DRAGON SCALE ERROR: <message> at [<filename>:<line>:<column>]
  Suggestion: <suggestion text>
```

Warnings use a similar format:

```
DRAGON SCALE WARNING: <message> at [<filename>:<line>:<column>]
  Suggestion: <suggestion text>
```

For custom level strings (neither "error" nor "warning"), the formatter falls back to:

```
DRAGON SCALE <level>: <message> at [<filename>:<line>:<column>]
```

The level string is uppercased for "error" and "warning" (producing "DRAGON SCALE ERROR" and "DRAGON SCALE WARNING") but passed through as-is for other levels.

**Example output:**

```
DRAGON SCALE ERROR: undeclared variable 'x' at [main.dr:10:5]
  Suggestion: did you mean to declare 'x: int'?
```

### 5.2 Plain Mode Format

When `useDragonTheme` is false, errors follow the classic GCC/Clang-style format:

```
<filename>:<line>:<column>: <level>: <message>
  Suggestion: <suggestion text>
```

**Example output:**

```
main.py:5:3: error: syntax error near 'def'
  Suggestion: check for missing colon at end of line
```

### 5.3 Suggestion Line

The suggestion line is only emitted when both conditions are met:
1. `style_.showSuggestions` is `true`
2. The `suggestion` parameter is non-empty

If either condition is false, no suggestion line appears.

---

## 6. ANSI Color Support

### Color Codes

The formatter defines three ANSI escape sequences in an anonymous namespace:

```cpp
const char* kRed   = "\033[1;31m";   // Bold red
const char* kCyan  = "\033[36m";     // Cyan (not bold)
const char* kReset = "\033[0m";      // Reset all attributes
```

### The `colorize()` Helper

```cpp
std::string colorize(const std::string& text, const char* color, bool useColor);
```

When `useColor` is `true`, wraps the text in the given color code and a reset code:

```
\033[1;31mDRAGON SCALE ERROR\033[0m
```

When `useColor` is `false`, returns the text unchanged.

### Where Colors Are Applied

| Element | Color | Mode |
|---------|-------|------|
| Level label in Dragon theme ("DRAGON SCALE ERROR") | Bold Red (`kRed`) | Dragon theme only |
| Level label in plain mode ("error", "warning") | Bold Red (`kRed`) | Plain mode only |
| "Suggestion" keyword | Cyan (`kCyan`) | Both modes |

The diagnostic message itself is never colorized -- only the structural labels receive color treatment.

---

## 7. Specialized Formatters

### 7.1 `formatMissingType()`

Produces a diagnostic for a missing PEP-484 type annotation. This is used by the TypeHintEnforcer when processing `.py` files.

**Signature:**

```cpp
std::string formatMissingType(const std::string& filename,
                              int line, int column,
                              const std::string& symbolName,
                              const std::string& context = "parameter") const;
```

**Dragon theme output:**

```
DRAGON SCALE ERROR: Missing type hint at [app.py:12:5]
  parameter 'data' requires a type annotation
  Suggestion: "To breathe fire, the Dragon needs to know this type. Add ': int', ': str', etc."
```

The `context` parameter controls the kind label (e.g., "parameter", "return type", "variable"). The `symbolName` is the identifier that lacks a type annotation.

**Plain mode output:**

```
app.py:12:5: error: missing type annotation for parameter 'data'
```

In plain mode, no suggestion is shown (the Dragon-themed suggestion text is only emitted in Dragon theme mode). The suggestion is baked into the Dragon theme branch and always uses the phrase "To breathe fire, the Dragon needs to know this type."

### 7.2 `formatUntypedImport()`

Produces the "Borders must be secured" error for importing an untyped `.py` file into a Dragon context.

**Signature:**

```cpp
std::string formatUntypedImport(const std::string& importedFile) const;
```

**Output (always the same regardless of theme setting):**

```
Borders must be secured: utils.py must be strictly typed to be imported into a Dragon context.
```

This diagnostic is **not style-sensitive** -- it always produces the same output regardless of `useDragonTheme`, `showSuggestions`, or `colorOutput` settings. This is a deliberate design choice: the "borders must be secured" message represents a core Dragon philosophy about type safety at module boundaries, and its wording is always Dragon-branded.

---

## 8. How Diagnostics Flow Through the System

### 8.1 Collection Phase

Each compiler stage collects diagnostics internally during processing:

```cpp
// Lexer
Lexer lexer(source, opts);
auto tokens = lexer.tokenize();
auto lexDiags = lexer.diagnostics();       // LexerDiagnostic[]

// Parser
Parser parser(tokens, opts);
auto module = parser.parseModule();
auto parseDiags = parser.diagnostics();    // ParserDiagnostic[]
bool hasParseErrors = parser.hasErrors();

// Sema
Sema sema;
sema.analyze(*module);
auto semaDiags = sema.diagnostics();       // SemaDiagnostic[]

// TypeChecker
TypeChecker tc;
tc.check(*module);
auto typeDiags = tc.diagnostics();         // TypeDiagnostic[]

// TypeHintEnforcer (.py files only)
TypeHintEnforcer enforcer;
bool typesOk = enforcer.enforce(*module);
auto enfDiags = enforcer.diagnostics();    // EnforcerDiagnostic[]
```

### 8.2 Checking for Errors

The Driver checks for errors after each stage using `hasErrors()` methods:

```cpp
if (parser.hasErrors()) {
    // Format and print parser diagnostics, then bail out
}
if (sema.hasErrors()) {
    // Format and print sema diagnostics, then bail out
}
// etc.
```

The pipeline is sequential and fails fast: if the Lexer produces errors, the Parser is not invoked. If the Parser produces errors, Sema is not invoked. This prevents cascading errors from corrupted ASTs.

### 8.3 Formatting Phase

The Driver creates a `DiagnosticFormatter` and calls `format()` for each diagnostic:

```cpp
DiagnosticFormatter formatter(style);

for (auto& diag : parser.diagnostics()) {
    std::string formatted = formatter.format(
        diag.location.filename,
        diag.location.line,
        diag.location.column,
        diag.level == ParserDiagnostic::Level::Error ? "error" : "warning",
        diag.message
    );
    std::cerr << formatted;
}
```

### 8.4 Exit Code

The Driver returns a non-zero exit code if any Error-level diagnostics were produced:

| Exit Code | Meaning |
|-----------|---------|
| 0 | Success (no errors, warnings are allowed) |
| 1 | Compilation failed (one or more errors) |

---

## 9. Diagnostic Output Examples

### 9.1 Type Error (Dragon Theme, Color Off)

```
DRAGON SCALE ERROR: type mismatch: cannot assign 'str' to variable of type 'int' at [main.dr:5:1]
```

### 9.2 Type Error (Dragon Theme, Color On)

```
\033[1;31mDRAGON SCALE ERROR\033[0m: type mismatch: cannot assign 'str' to variable of type 'int' at [main.dr:5:1]
```

### 9.3 Type Error (Plain Mode)

```
main.dr:5:1: error: type mismatch: cannot assign 'str' to variable of type 'int'
```

### 9.4 Missing Type Hint (Dragon Theme)

```
DRAGON SCALE ERROR: Missing type hint at [utils.py:12:5]
  parameter 'data' requires a type annotation
  Suggestion: "To breathe fire, the Dragon needs to know this type. Add ': int', ': str', etc."
```

### 9.5 Untyped Import

```
Borders must be secured: utils.py must be strictly typed to be imported into a Dragon context.
```

### 9.6 Parser Error with Suggestion (Dragon Theme)

```
DRAGON SCALE ERROR: expected '{' after 'if' condition at [main.dr:3:15]
  Suggestion: Dragon uses curly braces for blocks, not colons
```

---

## 10. Design Considerations

### 10.1 Color Default is Off

The `colorOutput` field defaults to `false`. This is intentional for two reasons:
1. **Test stability:** Test assertions compare formatted strings character-by-character. ANSI escape codes would make every test fragile and platform-dependent.
2. **Piping:** When stderr is piped to a file or another process, ANSI codes produce garbage. Defaulting to off ensures clean output in non-interactive contexts.

The Driver can enable color output based on terminal detection or a `--color` flag.

### 10.2 Theme is Not Just Cosmetic

The Dragon theme serves a deliberate purpose: it makes Dragon feel like its own language with its own identity, rather than a Python clone that happens to have a different file extension. Error messages like "To breathe fire, the Dragon needs to know this type" and "Borders must be secured" reinforce the language's philosophy (strict typing, safe module boundaries) through language rather than just technical error codes.

### 10.3 Suggestion System

Suggestions are optional metadata attached to diagnostics. They are not generated automatically -- each diagnostic site must explicitly provide a suggestion string. Currently, suggestions are only used in:
- The `format()` method (general diagnostics, where the caller provides the suggestion)
- The `formatMissingType()` method (hardcoded suggestion about type annotations)

Future work could add an automatic suggestion engine that infers fixes from common error patterns (e.g., "did you mean 'strlen'?" for a misspelled function name).

---

## 11. Test Coverage

The `DiagnosticFormatter` has 17 dedicated tests in `test/DiagnosticTest.cpp`:

| Test | What It Validates |
|------|-------------------|
| `DragonThemeError` | "DRAGON SCALE ERROR" appears with location |
| `DragonThemeWarning` | "DRAGON SCALE WARNING" appears |
| `DragonThemeCustomLevel` | Custom level strings are passed through |
| `DragonThemeWithSuggestion` | Suggestion line appears when provided |
| `DragonThemeNoSuggestionWhenEmpty` | No suggestion line when suggestion is empty |
| `PlainModeError` | "file:line:col: error: message" format |
| `PlainModeWarning` | "file:line:col: warning: message" format |
| `PlainModeWithSuggestion` | Suggestion line works in plain mode |
| `SuggestionsDisabled` | No suggestion when `showSuggestions` is false |
| `ColorOutputContainsAnsiCodes` | ANSI escape codes present when color is on |
| `NoColorOutputNoAnsiCodes` | No ANSI codes when color is off |
| `MissingTypeDragonTheme` | "Missing type hint" format with "breathe fire" |
| `MissingTypePlainMode` | Plain format for missing type |
| `MissingTypeReturnType` | Return type context works |
| `UntypedImportMessage` | "Borders must be secured" message |
| `StyleAccessor` | `style()` getter returns correct values |
| `DefaultStyleIsDragonThemed` | Default construction uses Dragon theme |

---

## Previous Document

[012 - Dragon Standard Library Mappings](012-stdlib.md)

## Next Document

[014 - Dragon Test Infrastructure](014-testing.md)
