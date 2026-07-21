# Decision 017: Template Blocks

> **Status:** Approved. phases 1-4 shipped; see log below.

I broke production HTML once with a Jinja typo that only showed up at runtime. Never again if I can help it. Dragon gets compile-time `template { ... }` blocks with `!{expr}` interpolation (the `!{` sigil so JSON/CSS braces don't fight us), pipe filters for injection blocking, and `template("file.html")` includes. Implementation log tracks what actually landed:

## Implementation log

| Sub-phase | Status | Summary |
|---|---|---|
| Phase 1 | shipped | `template { ... }` block + `!{expr}` interpolation. Lexer brace-depth scanner, `TEMPLATE` token, `TemplateExpr` AST, CodeGen second-pass with native-type stringify (int/float/bool/str all go through their native LLVM types - no i64 funnel). |
| Phase 2 | shipped | Pipe filters: `\| html`, `\| sql`, `\| url`, `\| raw`, and user-defined `\| my_fn` (any in-scope `(str) -> str` function). Runtime escapes in `lib/Runtime/runtime_platform.cpp`. |
| Phase 3 | shipped | `template("file.html")` compile-time file include. Same IR as inline. |
| Phase 4.A | | **Typed-template hardening.** `template[X]` was already wired through Lexer/Parser/TypeChecker/CodeGen/stdlib, but lacked Template-protocol enforcement and parent-walk method resolution. This sub-phase: (1) `ClassType::parentClass` is now populated during `visit(ClassDecl)` from `node.bases` - one source of truth for inheritance, no parallel map; (2) `TypeChecker::resolveTemplateContentType` walks the parent chain and rejects `template[X]` unless `X` extends `Template`; (3) dispatch hook - `template[X]` where `X` extends `StructTemplate` produces a clear "reserved for structured templates" error rather than silently falling through to string mode; (4) CodeGen `_escape` lookup goes through `resolveMethodFunction` (parent-walk), so `class MyHTML(HTML)` inherits HTML's escape; (5) CodeGen `_new`/`_validate` use `classSymPrefix` so stdlib-imported types resolve correctly under mangled names; (6) test coverage: 14 typed-template CodeGen tests + 6 TypeChecker protocol tests. 1541 total tests green. |
| Phase 4.B | | **Block interpolation + `:{}` content alias + context inheritance.** (1) New `TEMPLATE_CONTENT_OPEN` token + `LexerOptions::inTemplateInterpolation` flag; lexer recognizes `:{` only when the body of a `!{}` is being re-lexed by CodeGen - outside that context it's a syntax error (lexer falls through to plain COLON, parser rejects). (2) `TemplateExpr::isContentAlias` flag distinguishes `:{}` fragments from explicit `template { ... }` so we know when to inherit content type vs return raw str. (3) Parser produces a `TemplateExpr` (empty `contentType`, `isContentAlias=true`) from each `TEMPLATE_CONTENT_OPEN` - content type is resolved at CodeGen time via the stack. (4) `CodeGenImpl::templateContextStack` push on entry to every `visit(TemplateExpr)`, pop on exit; `:{}` reads the top to inherit escape behavior. (5) `!{}` second-pass now calls `parseModule` on the body - single `ExprStmt` → expression mode (existing fast path); anything else → block mode. (6) Block mode: allocate runtime `DragonListPtr*` (TAG_STR=1), push onto `templateBlockBufferStack`, visit each parsed statement, pop, emit `dragon_str_join_ptr("", buffer)` - that's the `!{}`'s value. (7) `visit(ExprStmt)` appends rendered TemplateExpr/TemplateFileExpr fragments to top-of-buffer-stack instead of treating as discarded. (8) Removed the f-string-style format-spec stripper from template codegen - it was eating colons in `:{...}` aliases and `x: int =` declarations; templates never documented format-spec support. |
| Phase 4.C | | **`!{*expr}` spread + `\| join` / `\| join(sep)` filter.** (1) New runtime `dragon_str_join_ptr(sep, list)` - walks `DragonListPtr` directly, zero per-element tag decode (§monomorphized containers). (2) Spread `!{*xs}` desugars at CodeGen time: leading `*` after whitespace strips, sets implicit `join` filter with empty separator. (3) `\| join` filter path: bypasses stringify (the expression's value IS the list pointer), parses optional `(sep_expr)` arg by lex+parse+visit inline, emits one `dragon_str_join_ptr` call. (4) Conflict detection: explicit `\| join(...)` combined with `*` spread produces a compile error. |
| Final | | 1556 total tests green (15 new for 4.B/4.C: nested loops, typed-template+block context inheritance, spread, all join variants, empty-loop edge case, top-level `:{}` syntax-error rejection). Real-binary smoke test with stdlib import confirms `template[HTML] { ... !{ for u in users { :{<li>!{u}</li>} } } ... }` end-to-end. |

Traditional engines (Go `text/template`, Jinja2, Handlebars) parse at runtime, crash on typos in prod, and use `{{ }}` that collides with JSON/JS/CSS. I'm making templates a compiler primitive instead - zero-cost at runtime, variables checked at compile time, `!{expr | html}` / `!{expr | sql}` filters built in.

## Design Alternatives Considered

### `template("string !{code}")`
Function-call syntax that looks like runtime but needs compiler magic. Conflates compile-time
and runtime when used as `template(open('file'))`.

### `t"..."` string prefix
Consistent with `f"..."` / `r"..."` / `b"..."` pattern but still requires string quotes and
triple-quote ceremony for multiline. Templates are document-oriented, not string-oriented.

### `template { ... }` block (Chosen)
Keyword block like `thread { ... }`. Everything inside is raw text. `!{expr}` breaks out
to Dragon expressions. No quotes, no escaping quotes, content stands on its own. The `template`
keyword unifies inline and file-based templates:
- `template { ... }` - inline block
- `template("file.html")` - compile-time file include

## Syntax

```python
# Inline template block
name: str = "World"
html: str = template {
<html>
 <body>
 <h1>Hello !{name}</h1>
 <p>You are !{age} years old</p>
 </body>
</html>
}

# Expression interpolation
total: str = template {Total: !{price * quantity}}

# Escaped literal !{ via doubling
code: str = template {Use !!{ for literal bang-brace}

# JSON (braces are balanced, depth tracking handles it)
json: str = template {
{"name": "!{name}", "age": !{age}}
}

# Pipe filters for escaping (Phase 2)
safe: str = template {<div>!{user_input | html}</div>}
query: str = template {SELECT * FROM users WHERE name = '!{name | sql}'}

# User-defined filter function (Phase 2)
def shout(s: str) -> str:
 return s.upper + "!!!"

loud: str = template {!{name | shout}}

# Compile-time file template (Phase 3)
page: str = template("templates/page.html")
```

### Closing-brace rule
The lexer tracks `{` / `}` depth inside the template body (starting at 1 after the opening `{`).
The template closes when depth returns to 0. This works for all balanced-brace formats (HTML,
JSON, CSS, JS, SQL, YAML). For rare unbalanced `}` in prose, escape as `!!}`.

## Implementation Phases

### Phase 1: Basic `template { }` with `!{expr}`

**Lexer** (`src/Lexer.cpp`) - new token type + template body scanning: - Add `TEMPLATE` token type to `Token.h` - In `scanIdentifier`: after scanning an identifier, if lexeme == "template" and next char is `{`, switch to template body scanning mode - Template body scanner: advance past `{`, track brace depth (start=1), accumulate raw text. When depth returns to 0, create a TEMPLATE token whose lexeme is the body text (without outer braces). Handle `!{` as normal text (not special at lex time - CodeGen does the second-pass). This is analogous to how triple-quoted strings scan across multiple lines.

**Token** (`include/dragon/Token.h`):
- Add `TEMPLATE` to TokenType enum

**AST** (`include/dragon/AST.h`):
- Add new node:
 ```cpp
 class TemplateExpr : public Expr {
 public:
 std::string body; // raw template text
 void accept(ASTVisitor& visitor) override;
 };
 ```

**AST visitors** (`include/dragon/AST.h`, `src/AST.cpp`): - Add `virtual void visit(TemplateExpr& node)` to ASTVisitor - Implement in DefaultASTVisitor (no-op traversal) and ASTPrinter

**Parser** (`src/Parser.cpp`): - In `primary`: when `match(TokenType::TEMPLATE)`, create TemplateExpr with body = token lexeme

**TypeChecker** (`src/TypeChecker.cpp`):
- Add `visit(TemplateExpr& node)`: set `node.type = strType`

**Sema** (`src/Sema.cpp`):
- Add `visit(TemplateExpr& node)`: no-op (or delegate to default)

**CodeGen** (`src/CodeGen.cpp`) - main work:
- Add `visit(TemplateExpr& node)`:
 - Copy the f-string second-pass pattern (lines 1822-1929) with these changes:
 - Scan `node.body` for `!{` instead of `{`
 - `!!{` produces literal `!{` (like `{{` produces `{` in f-strings)
 - `!!}` produces literal `}` (for unbalanced brace edge case)
 - Same inline Lexer+Parser+visit for expressions inside `!{...}`
 - Same type-based string conversion (int_to_str, float_to_str, bool_to_str, __str__ dunder)
 - Same dragon_str_concat chaining
- Declare `visit(TemplateExpr&)` in CodeGen.h

**Tests**: LexerTest, ParserTest, CodeGenTest (IR + E2E)

### Phase 2: Pipe Filters (`!{expr | filter}`)

Add pipe-based filtering inside `!{...}` interpolations. The pipe `|` appears after the
expression, followed by a filter name. Built-in filters: `html`, `sql`, `url`. User-defined
filters: any in-scope function with signature `(str) -> str`.

```python
# Built-in filters
template {<div>!{user_input | html}</div>}
template {WHERE name = '!{name | sql}'}
template {?q=!{query | url}}

# User-defined filter
def shout(s: str) -> str:
 return s.upper + "!!!"
template {!{name | shout}}
```

**CodeGen** (`src/CodeGen.cpp`): - Extend Phase 1 `!{...}` scanner: after extracting the expression text, check for trailing `| filter_name` (strip whitespace around `|`). Split into expr + filter name. - After string conversion, apply filter: - `html` → call `dragon_template_escape_html(strVal)` - `sql` → call `dragon_template_escape_sql(strVal)`
 - `url` → call `dragon_template_escape_url(strVal)` - Otherwise → look up `filter_name` as an LLVM function in the module (same as NameExpr resolution), call it with `(strVal)`, expect `i8*` return - Compile error if filter name is not a built-in and not found in module

**Runtime** (`lib/Runtime/runtime.cpp`): - `dragon_template_escape_html(s)` - `&`→`&amp;` `<`→`&lt;` `>`→`&gt;` `"`→`&quot;` `'`→`&#x27;`
- `dragon_template_escape_sql(s)` - `'`→`''`
- `dragon_template_escape_url(s)` - percent-encoding (unreserved chars pass through)

**CodeGen** `declareRuntimeFunctions`:
- Declare the three escape functions (`i8* → i8*`)

### Phase 3: Compile-Time File Templates (`template("file.html")`)

`template("path")` is a compiler intrinsic - the compiler reads the file at compile time,
parses `!{expr}` segments, resolves expressions against the calling scope, and generates
the same zero-cost concat chain IR. No runtime template parsing.

```python
name: str = "World"
html: str = template("templates/page.html")
# Compiler reads page.html, finds !{name}, compiles against calling scope
```

**How it works:** 1. Lexer sees `template` followed by `(` (not `{`) - emits IDENTIFIER("template") 2. Parser detects contextual keyword `template` + `(`, parses string literal arg, creates `TemplateFileExpr` AST node with `filePath` field 3. CodeGen reads file at compile time (path relative to source file), treats content as template body, runs same `!{` scanner + inline Lexer+Parser+visit 4. Compile error if: path is not a string literal, file doesn't exist, `!{expr}` references undefined variables

**Files to modify:** - `include/dragon/AST.h` - Add `TemplateFileExpr` node - `src/AST.cpp` - Visitor + ASTPrinter - `src/Parser.cpp` - Detect `template(` contextual keyword, parse string literal arg - `src/TypeChecker.cpp` - visit(TemplateFileExpr&) sets type to str - `src/CodeGen.cpp` - visit(TemplateFileExpr&) reads file, runs template scanner on content

### Phase 4: Typed Templates (`template[X]`) - PENDING

Typed templates make `template[X]` a generic where `X` is a content type - an UPPERCASE class
extending `Template` that provides auto-escaping, validation, and type safety. This eliminates
injection vulnerabilities by default and enables user-defined DSLs as first-class template targets.

#### Convention

- `UPPERCASE` = template content type (extends `Template`)
- `PascalCase` = regular class
- `snake_case` = variable/function

Template content types are uppercase because they represent content formats (HTML, SQL, CSS),
which are conventionally written in caps. Custom DSLs follow the same convention.

#### The Template Protocol

```python
class Template {
 _inner: str

 @staticmethod
 def escape(s: str) -> str {
 return s # no-op default - subclasses override
 }

 @staticmethod
 def validate(content: str) -> None {
 pass # optional - subclasses override to check final output
 }

 def to_str(self) -> str {
 return self._inner
 }

 def __str__(self) -> str {
 return self._inner
 }
}
```

| Method | Required | Purpose |
|--------|----------|---------|
| `escape(str) -> str` | yes | Auto-applied to every `!{expr}` interpolation |
| `validate(str) -> None` | no | Called on final output, raise on invalid content |

#### Syntax

```python
# Typed - auto-escaping, type-safe return
page: HTML = template[HTML] {
 <h1>!{user_input}</h1> # auto-escaped via HTML.escape
 <div>!{trusted | raw}</div> # explicit opt-out
}

query: SQL = template[SQL] {
 SELECT * FROM users WHERE name = '!{name}' # auto-escaped via SQL.escape
}

# Untyped - raw text, no escaping (existing behavior, unchanged)
raw: str = template {Hello !{name}}
```

#### Built-in Content Types (stdlib)

Built-in types are pre-defined subclasses of `Template` shipped in `stdlib/template.dr`. They have no special compiler status - developers can replace or extend them.

```python
class HTML(Template) {
 @staticmethod
 def escape(s: str) -> str {
 # & → &amp; < → &lt; > → &gt; " → &quot; ' → &#x27;
 ...
 }
}

class SQL(Template) {
 @staticmethod
 def escape(s: str) -> str {
 # ' → ''
 ...
 }
}

class CSS(Template) {
 @staticmethod
 def escape(s: str) -> str {
 # escape \ and non-alphanumeric in identifiers
 ...
 }
}

class JSON(Template) {
 @staticmethod
 def escape(s: str) -> str {
 # \ → \\ " → \" control chars → \uXXXX
 ...
 }
}

class URL(Template) {
 @staticmethod
 def escape(s: str) -> str {
 # percent-encoding of reserved characters
 ...
 }
}

class XML(Template) {
 @staticmethod
 def escape(s: str) -> str {
 # same as HTML + additional XML entities
 ...
 }
}
```

#### User-Defined Content Types

Any UPPERCASE class extending `Template` works with `template[X]`:

```python
class SHELL(Template) {
 @staticmethod
 def escape(s: str) -> str {
 return s.replace("'", "'\\''")
 }
}

cmd: SHELL = template[SHELL] {ls -la '/home/!{username}/docs'}

class GQL(Template) {
 @staticmethod
 def escape(s: str) -> str {
 return s.replace("\\", "\\\\").replace('"', '\\"')
 }

 @staticmethod
 def validate(content: str) -> None {
 if not content.strip.startswith(("query", "mutation", "subscription")) {
 raise ValueError("Invalid GraphQL")
 }
 }
}

q: GQL = template[GQL] {
 query {
 user(name: "!{name}") { email }
 }
}

class MD(Template) {
 @staticmethod
 def escape(s: str) -> str {
 for ch in ["*", "_", "`", "[", "]", "#"] {
 s = s.replace(ch, "\\" + ch)
 }
 return s
 }
}

doc: MD = template[MD] {
# Report for !{user_name}
Generated on !{date}
}

class RE(Template) {
 @staticmethod
 def escape(s: str) -> str {
 for ch in [".", "*", "+", "?", "(", ")", "[", "]", "{", "}", "\\", "^", "$", "|"] {
 s = s.replace(ch, "\\" + ch)
 }
 return s
 }
}

pattern: RE = template[RE] {^!{prefix}[0-9]+$}
```

#### Type Safety

`template[X]` returns type `X`, not `str`. This prevents cross-type mistakes:

```python
header: HTML = template[HTML] {<header>!{title}</header>}
query: SQL = template[SQL] {SELECT 1}

# Composition: HTML inside HTML → no double-escape
page: HTML = template[HTML] {
 !{header} # HTML in HTML → inserted as-is
 !{user_input} # str in HTML → auto-escaped
}

# Type errors caught at compile time
def send_response(body: HTML) -> None { ... }
send_response(page) # ok
send_response(query) # compile error: SQL is not HTML
send_response("raw") # compile error: str is not HTML
```

Interpolation dispatch: - `!{expr}` where expr is same type `X` → insert raw (no double-escape) - `!{expr}` where expr is `str` → apply `X.escape` - `!{expr}` where expr is other type → `__str__` then `X.escape` - `!{expr | raw}` → no escaping (explicit opt-out)

#### Compiler Resolution

```
template[X] { body }
 │
 ▼
 1. Is X a built-in? (HTML, SQL, CSS, JSON, URL, XML)
 → use stdlib escape, return built-in type

 2. Is X a class extending Template?
 → use X.escape as auto-filter
 → return type is X
 → if X.validate exists, call on final string

 3. Neither?
 → compile error: "X does not implement Template protocol"
```

#### Implementation

**Lexer** (`src/Lexer.cpp`): - When scanning `template` identifier: check for `[` after (in add addition to existing `{` and `(` checks). If `[`, lex normally - Parser handles the `[ident]` part.

**AST** (`include/dragon/AST.h`): - Add `std::string contentType` field to `TemplateExpr` and `TemplateFileExpr`
- Empty string = untyped (existing behavior), non-empty = typed template

**Parser** (`src/Parser.cpp`):
- In template parsing: after matching `template` contextual keyword, if next token is `[`,
 consume `[`, expect IDENTIFIER (the content type name), consume `]`, store in AST node.
 Then expect `{` or `(` as before.

**TypeChecker** (`src/TypeChecker.cpp`):
- `visit(TemplateExpr&)`: if `contentType` is non-empty, look up class by name, verify it
 extends `Template`, set `node.type` to that class type instead of `strType`

**CodeGen** (`src/CodeGen.cpp`):
- After each `!{expr}` string conversion, if template is typed:
 - Check expr type: if same content type, skip escape. If `| raw`, skip escape.
 - Otherwise, call `X.escape` on the string value (resolve as static method call)
- After full concat chain, if `X.validate` exists, emit call to `X.validate(result)`
- Wrap final string in content type instance (allocate `X`, set `_inner` field)

**Stdlib** (`stdlib/template.dr`):
- `Template` base class + `HTML`, `SQL`, `CSS`, `JSON`, `URL`, `XML` subclasses

**VS Code grammar** (`vscode-dragon/syntaxes/dragon.tmLanguage.json`):
- `template-block` rule with begin pattern matching `template(\[([A-Z]+)\])?\s*\{`
- Capture group 2 determines embedded language scope
- Map `HTML` → `meta.embedded.block.html`, `SQL` → `meta.embedded.block.sql`, etc.
- `embeddedLanguages` in `package.json` maps scopes to language IDs

**Tests**: LexerTest, ParserTest, TypeCheckerTest, CodeGenTest (IR + E2E)

#### Block Interpolation (`!{ statements }`) and the `:{}` Content Alias

`!{}` accepts full Dragon statements, not just expressions. This enables loops, conditionals,
and arbitrary logic inline within templates. Inside `!{}` blocks, the `:{}` alias drops back
into template content mode - it's the terse counterpart to `template {}`.

**Two forms, identical IR:**
- `template { content }` - explicit, self-documenting (12 chars)
- `:{ content }` - terse, mirrors `!{}` visually (2 chars)

Both inherit the enclosing `template[X]` context type. `:{}` is a contextual alias - only
valid inside `!{}` blocks within a template. Outside templates, `:{` is a syntax error.

**The pattern:** `!{}` drops into code. `:{}` drops back into content.

```
template[HTML] { ← content mode
 markup
 !{ ← code mode
 Dragon code
 :{ markup } ← content mode (inherited context)
 Dragon code
 } ← back to content mode
 markup
}
```

**Rules:** - Expression mode - `!{}` contains a single expression, stringify the result - Block mode - `!{}` contains statements; `:{}` / `template {}` fragments within are appended to an implicit buffer; the buffer is the interpolation result (empty string if no fragments)

**Detection: try expression first, fall back to block.**

1. Tokenize the `!{}` content (reused token stream) 2. Call `parseExpression` - if it succeeds AND consumes all tokens → expression mode 3. If it fails or tokens remain → reset cursor, call `parseBlock` → block mode

No heuristic, no content scanning. The parser decides. Failing `parseExpression` on a
statement keyword like `for` is a single token check - effectively free. 90%+ of `!{}` usage
is expressions, which succeed on the first try. The fallback only triggers for statements.

| Content | Mode | Why |
|---------|------|-----|
| `!{name}` | expression | `parseExpression` succeeds |
| `!{x + 1}` | expression | `parseExpression` succeeds |
| `!{"a" if b else "c"}` | expression | `parseExpression` succeeds |
| `!{func}` | expression | `parseExpression` succeeds |
| `!{for x in xs { :{ ... } }}` | block | `for` fails expression parse |
| `!{if flag { :{ ... } }}` | block | `if` fails expression parse |
| `!{for x in range(10) { print("hi") }}` | block | side effects only, produces `""` |
| `!{x: int = compute}` | block | declaration, produces `""` |

**Loops (single-line and multi-line - developer choice):**

```python
# Single-line
template[HTML] {
<ul>!{ for item in items { :{ <li>!{item}</li> } } }</ul>
}

# Multi-line
template[HTML] {
<ul>
 !{
 for item in items {
 :{ <li>!{item}</li> }
 }
 }
</ul>
}
```

**Conditionals (single-line and multi-line):**

```python
# Single-line
template[HTML] {
 !{ if logged_in { :{ <p>Welcome !{name}</p> } } else { :{ <p>Please log in</p> } } }
}

# Multi-line
template[HTML] {
 !{
 if logged_in {
 :{ <p>Welcome !{name}</p> }
 } else {
 :{ <p>Please log in</p> }
 }
 }
}
```

Note: for simple one-value conditionals, expression mode is still cleaner:
```python
template[HTML] {
 <p>!{"Welcome " + name if logged_in else "Please log in"}</p>
}
```

**Complex logic:**

```python
template[HTML] {
<table>
 !{
 for i, user in enumerate(users) {
 cls: str = "even" if i % 2 == 0 else "odd"
 :{
 <tr class="!{cls}">
 <td>!{user.name}</td>
 <td>!{user.email}</td>
 </tr>
 }
 }
 }
</table>
}
```

**Using `template {}` instead (identical behavior):**

```python
template[HTML] {
<ul>
 !{
 for item in items {
 template { <li>!{item}</li> }
 }
 }
</ul>
}
```

**How it works in CodeGen:** - CodeGen tokenizes `!{}` content, tries `parseExpression` first - If expression succeeds and consumes all tokens → stringify result (existing path) - If expression fails → reset token cursor, `parseBlock` → block mode - Block mode: CodeGen creates an implicit `std::vector<llvm::Value*> buffer` - Statements are parsed and emitted normally via `parseBlock` - `:{}` and `template {}` fragments are parsed as template content (with `!{}` interpolation support), converted to string values, and appended to the buffer - After the block, the buffer is concatenated via `dragon_str_concat` chain - The result becomes the interpolation value - Both single-line and multi-line work identically - no newline sensitivity

**Lexer/Parser for `:{}` alias:** - Lexer: when inside a template block interpolation context, `:{` is recognized as a content-mode opener (new token: `TEMPLATE_CONTENT_OPEN`) - Brace depth tracking applies as normal - content closes when depth returns to 0 - Parser: `TEMPLATE_CONTENT_OPEN` creates an implicit `TemplateExpr` node inheriting the enclosing content type

#### Context Inheritance

Inside a typed `template[X] { ... }`, both `!{}` blocks and `:{}` / `template {}` fragments
inherit the content type. The type is declared once at the top and propagates to any depth.

```python
# Type declared ONCE at the top - :{ } inherits HTML context everywhere
page: HTML = template[HTML] {
<html>
 <body>
 !{
 if show_nav {
 :{
 <nav>
 <ul>
 !{
 for link in nav_links {
 :{ <li><a href="!{link.url}">!{link.label}</a></li> }
 }
 }
 </ul>
 </nav>
 }
 }
 }
 <main>!{content}</main>
 </body>
</html>
}
```

The flow is bidirectional - freely alternate between markup and code and code at any depth:

```
template[HTML] { ← enter HTML context
 markup ← raw content
 !{ ← code mode (context: HTML)
 Dragon code
 :{ markup } ← content mode (context inherited)
 !{ ← deeper code mode (context still HTML)
 Dragon code
 :{ markup } ← content mode (context still HTML)
 }
 Dragon code
 }
 markup ← content mode
}
```

**When `template[X]` IS required:** - Top-level declaration: `page: HTML = template[HTML] { ... }` - Switching content type: inside `template[HTML]`, use `template[SQL]{...}` for a query - Standalone helper functions: `def footer -> HTML { return template[HTML]{...} }`

**When `:{}` or `template {}` is sufficient:** - Content fragments inside `!{}` blocks - they inherit the enclosing type - Nested `!{}` blocks at any depth - context propagates down

**Implementation:**
- CodeGen maintains a `templateContextStack` (push on entering `template[X]`, pop on exit)
- `:{}` and `template {}` inside `!{}` blocks read the current context type from stack top
- Auto-escaping for `!{expr}` inside `:{}` uses the inherited type
- Switching context explicitly (e.g. `template[SQL]{...}` inside an HTML template) pushes
 a new context onto the stack

#### List Spreading and the `join` Filter

For one-liner list rendering, Dragon provides spread and join as alternatives to block
interpolation:

**`!{*expr}` - spread operator (sugar for `| join` with empty separator):**

```python
template[HTML] {
<ul>!{*[template[HTML]{<li>!{item}</li>}.to_str for item in items]}</ul>
}
```

**`!{expr | join}` / `!{expr | join(sep)}` - join filter with optional separator:**

```python
template[HTML] {
<ul>
 !{[template[HTML]{<li>!{item}</li>}.to_str for item in items] | join("\n ")}
</ul>
}
```

| Syntax | Separator | When to use |
|--------|-----------|-------------|
| `!{*expr}` | `""` (none) | Quick one-liner concat |
| `!{expr \| join}` | `""` (none) | Explicit style, same as `*` |
| `!{expr \| join(sep)}` | custom | When separator matters |

Both `*` and `join` require the expression to be iterable - compile error otherwise. With block interpolation available, these are most useful for terse one-liners where a full `for` block would be overkill.

**Implementation:** - `*` detection: CodeGen checks for leading `*` in `!{}` text, strips it, parses remainder as expression, emits join loop with `""` separator
- `join` filter: CodeGen detects filter name `join`, checks optional `(sep)` arg (default `""`), emits `dragon_str_join(list, separator)` runtime call - Both lower to identical IR

#### Design Note: No Template Sub-Language

Dragon does NOT add template-specific directives (`!for`, `!if`, `!include`, `!macro`).
Block interpolation with `:{}` makes them unnecessary - Dragon IS the template language:

| Template concept | Dragon equivalent |
|------------------|-------------------|
| `{% include "x.html" %}` | `!{header}` - function call returning content type |
| `{% macro btn(x) %}` | `def btn(x: str) -> HTML { ... }` - function |
| `{% for x in xs %}` | `!{ for x in xs { :{ <li>!{x}</li> } } }` - block + `:{}` |
| `{% if cond %}` | `!{ if cond { :{ ... } } else { :{ ... } } }` - block + `:{}` |
| `{% extends "base" %}` | `def layout(body: HTML) -> HTML` - higher-order function |
| `{% block content %}` | `HTML` parameter to layout function |

Templates are declarative (markup + interpolation). Logic is Dragon (testable, debuggable,
typed). `!{}` enters code, `:{}` returns to content. One language, two sigils, no mini-language.

#### Estimated Cost

| Component | Lines | Notes |
|-----------|-------|-------|
| Lexer | ~25 | `[` detection after `template`, `:{` token in template context |
| AST | ~10 | `contentType` field + block interpolation flag |
| Parser | ~50 | `[IDENT]` consumption, block vs expression detection, `:{` as `TemplateExpr` |
| TypeChecker | ~30 | Class lookup, Template protocol check |
| CodeGen | ~220 | Escape dispatch, validate call, type wrapping, block interpolation with implicit buffer, context stack, `:{}` alias, `*` spread, `join` filter |
| Runtime | ~20 | `dragon_str_join(list, separator)` |
| stdlib/template.dr | ~100 | Base class + 6 built-in types |
| VS Code grammar | ~50 | Embedded language rules + `:{` highlighting |
| Tests | ~280 | Lex/parse/typecheck/codegen/E2E for typed templates, block interp, `:{}`alias, context inheritance, join/spread |
| **Total** | **~785** | |

## Cost Analysis

**Runtime cost: zero beyond manual string concatenation.** Both `template { }` and
`template("file.html")` compile down to the exact same IR - a chain of `dragon_str_concat`
calls with string literals and expression values. Identical to writing concatenation by hand.

For a template with N interpolations:
- N calls to `dragon_str_concat` (each: `strlen` + `malloc` + `memcpy`)
- N+1 string literal segments in `.rodata`
- Zero parsing, zero reflection, zero runtime template engine

**`template("file.html")` = same cost as inline.** The compiler reads the file once during
compilation (`std::ifstream::read`, microseconds), then the file is gone. The binary has no
reference to the file path, no file I/O code. At runtime it's just pre-compiled concat calls.
The file is essentially `#include` for templates.

**Compile-time cost: negligible.** Lexer brace-depth scan is O(template length). CodeGen
second-pass creates mini Lexer+Parser per `!{expr}` - identical to what f-strings already do.
Both dwarfed by LLVM optimization passes.

**Implementation cost: ~550-600 lines across 3 phases:**

| Phase | New code | Scope |
|---|---|---|
| Phase 1: `template { }` + `!{expr}` | ~300 lines | Token, Lexer, AST, Parser, TypeChecker, Sema, CodeGen, tests |
| Phase 2: `!{expr \| filter}` pipes | ~200 lines | CodeGen pipe parsing + 3 runtime escape functions + user-defined filter dispatch + tests |
| Phase 3: `template("file.html")` | ~120 lines | New AST node, Parser detection, CodeGen file read + tests |

**Binary size:** template string segments live in `.rodata`, same as any string literal. A 10KB
HTML template adds ~10KB to the binary. No hidden overhead. No runtime library shipped.

## Key Implementation Notes

- `template` is a contextual keyword - lexed as IDENTIFIER normally, only special when
 followed by `{` (block) or `(` (file). Can still be used as a variable name elsewhere.
- Brace depth tracking in lexer: `{` increments, `}` decrements, template closes at depth 0.
 `!{` increments depth like any `{`. Content braces must be balanced (always true for
 HTML/JSON/CSS/JS/SQL). Unbalanced `}` escaped as `!!}`.
- `!{...}` expression depth tracking reuses same `{ }` counting logic as f-strings
- Strings are `const char*` at runtime - template output is malloc'd string concat chains
- `template("path")` path resolved relative to source file being compiled (not CWD)
- `template("path")` with a non-literal arg is a compile error
- `template { }` uses braces in both `.dr` and `.py` mode (raw text delimiter, not code block)

## Verification

1. Build: `cmake --build build/`
2. Test: `cd build && ctest`
3. Manual E2E:
 ```python
 name: str = "World"
 x: int = 42
 print(template {Hello !{name}, x=!{x}})
 # → Hello World, x=42

 print(template {Literal: !!{not_interpolated}})
 # → Literal: !{not_interpolated}

 html: str = template {
 <html><body>
 <h1>!{name}</h1>
 <p>{"this is JSON, not interpolation"}</p>
 </body></html>
 }
 print(html)
 # → <html><body>\n <h1>World</h1>\n <p>{"this is JSON, not interpolation"}</p>\n</body></html>

 print(template("templates/greeting.html"))
 # → compiled from file, same perf as inline
 ```

## Comparison: Dragon vs Jinja2 vs Go vs Rust

### Full page example

Each example renders the same page: a conditional navigation bar with links, a title, and a list of items.

#### Jinja2 (Python)

```html
<html>
 <body>
 {% if show_nav %}
 <nav>
 <ul>
 {% for link in nav_links %}
 <li><a href="{{ link.url }}">{{ link.label }}</a></li>
 {% endfor %}
 </ul>
 </nav>
 {% endif %}
 <main>
 <h1>{{ title }}</h1>
 <ul>
 {% for item in items %}
 <li>{{ item }}</li>
 {% endfor %}
 </ul>
 </main>
 </body>
</html>
```

#### Go (`html/template`)

```html
<html>
 <body>
 {{if .ShowNav}}
 <nav>
 <ul>
 {{range .NavLinks}}
 <li><a href="{{.URL}}">{{.Label}}</a></li>
 {{end}}
 </ul>
 </nav>
 {{end}}
 <main>
 <h1>{{.Title}}</h1>
 <ul>
 {{range .Items}}
 <li>{{.}}</li>
 {{end}}
 </ul>
 </main>
 </body>
</html>
```

#### Rust (Askama - compile-time templates)

```html
<!-- templates/page.html -->
<html>
 <body>
 {% if show_nav %}
 <nav>
 <ul>
 {% for link in nav_links %}
 <li><a href="{{ link.url }}">{{ link.label }}</a></li>
 {% endfor %}
 </ul>
 </nav>
 {% endif %}
 <main>
 <h1>{{ title }}</h1>
 <ul>
 {% for item in items %}
 <li>{{ item }}</li>
 {% endfor %}
 </ul>
 </main>
 </body>
</html>
```

```rust
// Rust side - struct must mirror template variables
#[derive(Template)]
#[template(path = "page.html")]
struct PageTemplate<'a> {
 show_nav: bool,
 nav_links: &'a [NavLink],
 title: &'a str,
 items: &'a [String],
}
```

#### Dragon

```python
page: HTML = template[HTML] {
<html>
 <body>
 !{
 if show_nav {
 :{
 <nav>
 <ul>
 !{
 for link in nav_links {
 :{ <li><a href="!{link.url}">!{link.label}</a></li> }
 }
 }
 </ul>
 </nav>
 }
 }
 }
 <main>
 <h1>!{title}</h1>
 <ul>
 !{
 for item in items {
 :{ <li>!{item}</li> }
 }
 }
 </ul>
 </main>
 </body>
</html>
}
```

### Includes / Partials

```html
<!-- Jinja2 -->
{% include "header.html" %}
<main>{{ content }}</main>
{% include "footer.html" %}

<!-- Go -->
{{template "header" .}}
<main>{{.Content}}</main>
{{template "footer" .}}
```

```rust
// Rust (Askama) - partials are separate template structs
#[derive(Template)]
#[template(source = r#"{{ header.render.unwrap }}
<main>{{ content }}</main>
{{ footer.render.unwrap }}"#, ext = "html")]
struct Page { header: Header, content: String, footer: Footer }
```

```python
# Dragon - partials are just functions
def header -> HTML { return template[HTML]("partials/header.html") }
def footer -> HTML { return template[HTML]("partials/footer.html") }

page: HTML = template[HTML] {
!{header}
<main>!{content}</main>
!{footer}
}
```

### Layouts / Template Inheritance

```html
<!-- Jinja2 -->
<!-- base.html -->
<html>
 <head><title>{% block title %}{% endblock %}</title></head>
 <body>{% block content %}{% endblock %}</body>
</html>

<!-- page.html -->
{% extends "base.html" %}
{% block title %}Home{% endblock %}
{% block content %}<h1>Welcome</h1>{% endblock %}
```

```html
<!-- Go - no inheritance, use nested templates -->
{{define "base"}}<html>
 <head><title>{{template "title" .}}</title></head>
 <body>{{template "content" .}}</body>
</html>{{end}}
```

```rust
// Rust (Askama) - derive-based inheritance
#[derive(Template)]
#[template(path = "base.html")]
struct Base;

#[derive(Template)]
#[template(path = "page.html", parent = "base.html")]
struct Page { title: String }
```

```python
# Dragon - layouts are functions that take typed content
def layout(title: str, body: HTML) -> HTML {
 return template[HTML] {
 <html>
 <head><title>!{title}</title></head>
 <body>!{body}</body>
 </html>
 }
}

page: HTML = layout("Home", template[HTML] {
 <h1>Welcome</h1>
})
```

### Macros / Reusable Components

```html
<!-- Jinja2 -->
{% macro button(label, cls="primary") %}
<button class="btn btn-{{ cls }}">{{ label }}</button>
{% endmacro %}

{{ button("Save") }}
{{ button("Delete", cls="danger") }}
```

```html
<!-- Go - no macros, use nested templates with limited args -->
{{define "button"}}<button class="btn btn-{{.Cls}}">{{.Label}}</button>{{end}}
{{template "button" dict "Label" "Save" "Cls" "primary"}}
```

```rust
// Rust (Askama) - macros via separate template structs or functions
fn button(label: &str, cls: &str) -> String {
 format!(r#"<button class="btn btn-{}">{}</button>"#,
 askama::MarkupDisplay::new_unsafe(&cls),
 askama::MarkupDisplay::new_unsafe(&label))
}
```

```python
# Dragon - macros are just functions
def button(label: str, cls: str = "primary") -> HTML {
 return template[HTML] {
 <button class="btn btn-!{cls}">!{label}</button>
 }
}

form: HTML = template[HTML] {
!{button("Save")}
!{button("Delete", cls="danger")}
}
```

### Injection Prevention

```python
# Jinja2 - auto-escaping is opt-in, off by default in many configs
{{ user_input }} # may or may not be escaped
{{ user_input | e }} # explicit escape
{{ trusted_html | safe }} # explicit raw

# Go html/template - context-aware auto-escaping (best of the traditional engines)
{{.UserInput}} # auto-escaped based on context (HTML, JS, CSS, URL)

# Rust (Askama) - auto-escapes based on file extension (.html = escape)
{{ user_input }} # escaped because file is .html
{{ user_input | safe }} # explicit raw

# Dragon - typed auto-escaping, compile-time type safety
template[HTML] { !{user_input} } # auto-escaped via HTML.escape
template[HTML] { !{user_input | raw} } # explicit opt-out
template[SQL] { !{user_input} } # auto-escaped via SQL.escape - different function
```

Dragon is the only one where `HTML` can't be passed where `SQL` is expected - a **compile-time type error**, not a runtime mistake.

### Custom Content Types

```python
# Jinja2 - not possible (HTML-only engine, or manually configure escaping globally)
# Go - not possible (html/template is hardcoded for HTML contexts)
# Rust (Askama) - limited (escape mode per file extension, no user-defined types)

# Dragon - any DSL is a first-class template target
class SHELL(Template) {
 @staticmethod
 def escape(s: str) -> str {
 return s.replace("'", "'\\''")
 }
}

cmd: SHELL = template[SHELL] {ls -la '/home/!{username}/docs'}

class GQL(Template) {
 @staticmethod
 def escape(s: str) -> str {
 return s.replace("\\", "\\\\").replace('"', '\\"')
 }
}

query: GQL = template[GQL] {
 query { user(name: "!{name}") { email } }
}
```

### Feature Matrix

| Feature | Jinja2 | Go | Rust (Askama) | Dragon |
|---------|--------|-----|---------------|--------|
| **Basics** | | | | |
| Interpolation | `{{ x }}` | `{{.X}}` | `{{ x }}` | `!{x}` |
| Loops | `{% for %}` | `{{range}}` | `{% for %}` | `!{ for x { :{ } } }` |
| Conditionals | `{% if %}` | `{{if}}` | `{% if %}` | `!{ if x { :{ } } }` |
| Pipe filters | `{{ x \| f }}` | `{{x \| f}}` | `{{ x \| f }}` | `!{x \| f}` |
| **Composition** | | | | |
| Includes | `{% include %}` | `{{template}}` | N/A (structs) | `!{func}` |
| Macros | `{% macro %}` | N/A | N/A | Functions |
| Layout/inheritance | `{% extends %}` | Nested define | Parent derive | HOF |
| **Safety** | | | | |
| Auto-escaping | Opt-in | Context-aware | By file ext | By type (`template[X]`) |
| Cross-type safety | No | No | No | **Compile-time** |
| Custom escape types | No | No | No | **Yes** (`Template` protocol) |
| Brace collision | `{{ }}` vs JS | `{{ }}` vs JS | `{{ }}` vs JS | `!{ }` - no collision |
| **Performance** | | | | |
| Parse time | Runtime | Runtime | **Compile-time** | **Compile-time** |
| Runtime overhead | Interpreter | Reflection | **Zero** | **Zero** |
| **Architecture** | | | | |
| Template language | Jinja DSL | Go mini-DSL | Jinja-like DSL | **Dragon itself** |
| Separate files required | Yes (usually) | Yes (usually) | Yes (required) | No (inline or file) |
| Full host language power | No | No | No | **Yes** |

### What each can do that the others can't

**Jinja2:** - Runtime template loading from untrusted sources (sandboxed) - Swappable template engines at runtime - Non-programmer accessible (designers can edit templates)

**Go `html/template`:** - Context-aware escaping (knows HTML vs JS vs CSS vs URL position within a single template) - Sandboxed execution of untrusted templates

**Rust (Askama):**
- Compile-time template checking with zero runtime cost
- Type-checked variables (struct fields must match template references)
- Derives from file-based templates (clean separation of markup and code)

**Dragon:**
- Full language power inside templates (`for`, `if`, `while`, function calls, variable declarations)
- User-defined content types with custom escaping (`template[SHELL]`, `template[GQL]`, `template[MYPROTO]`)
- Typed template composition (`HTML` can't go where `SQL` is expected - compile error)
- Inline or file-based templates with identical performance
- Two-sigil mode switching (`!{}` = code, `:{}` = content) to any nesting depth
- Templates are values - pass them to functions, store in variables, return from functions

### The tradeoff in one sentence

| Language | Philosophy |
|----------|-----------|
| Jinja2 | Maximum readability and designer-friendliness; zero compile-time safety |
| Go | Pragmatic simplicity with good HTML auto-escaping; limited expressiveness |
| Rust | Compile-time safety via file-based templates; rigid separation of markup and code |
| Dragon | Full language power with typed injection prevention; templates are code, code is templates |
