# Decision 023: Dragon Script (.drs) - Data + Scripting Format

> **Status:** Implemented (all 7 phases landed).

Dragon Script (`.drs`) is human-readable data with a bit of scripting - between JSON and a real language. Enough to parameterize configs, not enough to write an app in. I implemented it as **pure `.dr` stdlib** (`stdlib/drs.dr`), dogfooding our own parser patterns (same shape as `json.dr`). Teh format is intentionally not Turing-complete.

JSON is too terse (no comments, no variables). YAML is ambiguous and footgun-prone. I still have PTSD from the Norway problem. TOML struggles with deep nesting. HCL/Jsonnet/CUE add power but are whole ecosystems. More fundamentally, **all existing formats are passive** - flat snapshots the client computed locally. `.drs` is a **programmable payload**: variables, conditionals, list generation travel with the data; the receiver evaluates in its own context.

### DRS vs Existing Formats

| Format | What travels over the wire | Receiver's job |
|--------|---------------------------|----------------|
| JSON | Flat data, no logic | Read values, run its own logic |
| YAML | Flat data, anchors are just copy-paste | Read values, run its own logic |
| TOML | Flat data, even less expressive | Read values, run its own logic |
| **DRS** | **Data + variables + conditionals + generation** | **Evaluate, get resolved data** |

### Example: Programmable Deploy Payload

A client sends this to a deployment server:

```drs
# Client controls these two variables - everything else derives from them
env = 'staging'
region = 'us-east'

{
 action = 'deploy'
 replicas: int = ${env == 'production' ? 5 : 1}
 image = 'myapp:${env == 'production' ? 'stable' : 'latest'}'
 canary: bool = ${env == 'production' ? true : false}

 resources = {
 cpu = ${env == 'production' ? '4cores' : '512m'}
 memory = ${env == 'production' ? '8Gi' : '1Gi'}
 }

 regions = for r in ['us-east', 'eu-west', 'ap-south'] if r == region {
 { name = ${r}, primary = true }
 }
}
```

The client changes **two variables** and the entire payload reshapes itself - replicas, image tags, resource limits, region selection. With JSON the client would compute all of that locally and send a flat blob. With `.drs`, the **intent travels with the data**.

The server just evaluates:

```dragon
from drs import loads

def handle_deploy(body: str) -> dict {
 config: dict = drs.loads(body)
 # config is fully resolved - replicas, regions, resources
 # all computed from the variables the client chose
 return config
}
```

---

## Format Specification

### Design Rule: No Double Quotes

**Double quotes (`"`) are forbidden in `.drs` files.** This is a deliberate design choice, not a limitation:

1. **HTML embedding** - `.drs` payloads can live inside HTML `"..."` attributes without escaping:```html
 <form data-config="{ name = 'my-app', port = 8080 }">
 <button onclick="sendDrs('{ action = `submit`, id = 42 }')">
 ```

2. **String transport** - `.drs` can be sent as a double-quoted string in any protocol (HTTP headers, JSON string values, CLI args) without escaping hell.

3. **Simplicity** - one string delimiter to learn (`'`), one raw delimiter (`` ` ``). No cognitive overhead choosing between `"` and `'`.

The two string types serve distinct purposes:
- **`'...'`** - standard strings with interpolation and escapes
- **`` `...` ``** - raw strings for embedding quotes, SQL, regex, code, or any
 content that needs to nest `'` characters

### File Structure

A `.drs` file has two zones:

```
┌─────────────────────────┐
│ HEADER │ Declarations: =, if/else, include
│ (reusable bindings) │ No payload data
├─────────────────────────┤
│ { } │ ROOT PAYLOAD
│ (the actual data) │ One required root block
└─────────────────────────┘
```

### Complete Example

```drs
# Header zone - declarations and includes
include 'base.drs'

env = 'production'
base = '/opt/dragon'

logging = {
 log_level = 'info'
 log_format = 'json'
}

if env == 'production' {
 replicas = 3
 debug = false
} else {
 replicas = 1
 debug = true
}

ports = for i in range(3) { 8080 + i }
# → [8080, 8081, 8082]

allowed = for p in ports if p != 8081 { p }
# → [8080, 8082]

# Root payload - exactly one, just bare braces
{
 name = 'my-app'
 version: float = 1.2
 debug: bool = ${debug}
 replicas: int = ${replicas}

 server = {
 host = '0.0.0.0'
 port: int = ${env == 'production' ? 443 : 8080}
 workers: int = ${cpu_count * 2}
 }

 instances = for i in range(replicas) {
 {
 id = ${i}
 port = ${8080 + i}
 name = 'worker-${i}'
 }
 }

 logging = {
 use logging
 log_level = ${env == 'production' ? 'warn' : 'debug'}
 }

 query = `SELECT * FROM users WHERE name = 'admin'`

 message = '''
 Deploying ${name} v${version}
 to ${env} with ${replicas} replicas
 '''

 tags: list[str] = ['api', ${env}]
}
```

### Syntax Rules

#### Values

| Type | Syntax | Example |
|------|--------|---------|
| Integer | Bare number | `42`, `-7` |
| Float | Decimal point | `3.14`, `-0.5` |
| Boolean | `true` / `false` | `debug = false` |
| None | `None` | `value = None` |
| String | `'...'` | `'hello'` |
| Raw string | `` `...` `` | `` `no\nescape` `` |
| Multi-line string | `'''...'''` | Triple single-quoted |
| Multi-line raw | ` ```...```` | Triple-backtick | | Object | `{ }` | `server = { ... }` | | List | `[ ]` | `[1, 2, 3]` |

**Forbidden:** `"` (double quote) anywhere in a `.drs` file. Parser error.

#### String Types

| Syntax | Interpolation `${}` | Escape sequences | Use case |
|--------|---------------------|------------------|----------|
| `'...'` | Yes | Yes (`\n \t \\ \'`) | General purpose strings |
| `'''...'''` | Yes | Yes | Multi-line strings |
| `` `...` `` | No | No | Raw: SQL, regex, nesting `'` quotes |
| ` ```...``` ` | No | No | Multi-line raw: code blocks, templates |

**`'` strings** are the standard - interpolation with `${}`, escape sequences with `\`.

**Backtick strings** are raw - no interpolation, no escape processing. Use them when you need to embed single quotes or any content verbatim:

```drs
# Backticks for content containing single quotes
greeting = `It's a beautiful day`
query = `SELECT * FROM users WHERE name = 'admin'`
pattern = `\d+\.\d+`

# Multi-line raw for code/templates
script = ```
 if [ '$HOME' != '' ]; then
 echo 'Hello, world!'
 fi
```
```

#### Assignments

All assignments use `=`. Type annotations are optional.

```drs
name = 'Dragon' # untyped
port: int = 8080 # typed
ratio: float = 0.75 # typed
server = { host = '0.0.0.0' } # nested object
```

#### Comments

```drs
# Line comment
/* Block comment */
```

Both allowed in header and payload.

#### Expressions `${}`

Expressions appear inside `${}` in string interpolation and as values.

| Expression | Example | Notes |
|-----------|---------|-------|
| References | `${base}` | Header bindings or parent scope |
| Ternary | `${x == 'a' ? 1 : 2}` | Single-level only |
| Arithmetic | `${cores * 2}` | `+`, `-`, `*`, `/`, `%` |
| Comparison | `${count > 5}` | `==`, `!=`, `>`, `<`, `>=`, `<=` |
| String concat | `${base + '/logs'}` | `+` on strings |
| Boolean logic | `${debug and verbose}` | `and`, `or`, `not` |

**NOT allowed in `${}`:** - Nested ternaries - Function calls
- Assignment - Indexing / attribute access

#### Conditionals (Header and Payload)

```drs
if env == 'production' {
 debug = false
} else {
 debug = true
}
```

No `elif` - keep it simple. Nest `if/else` if needed.

#### For Expressions (List Generation)

```drs
# Generate list
ports = for i in range(3) { 8080 + i }
# → [8080, 8081, 8082]

# Filter
allowed = for p in ports if p != 8081 { p }
# → [8080, 8082]

# Generate list of objects
instances = for i in range(3) {
 { id = ${i}, port = ${8080 + i} }
}
```

`for` is an expression that produces a list, not a statement with side effects.

#### `use` (Compose Reusable Blocks)

```drs
logging = {
 log_level = 'info'
 log_format = 'json'
}

{
 app_logging = {
 use logging # pulls in all key-value pairs
 log_level = 'warn' # override specific keys
 }
}
```

`use` copies key-value pairs from a header binding. Later keys override earlier ones. Only allowed inside payload `{}` blocks.

#### `include` (File Composition)

```drs
include 'base.drs'
include 'secrets.drs'
```

Only allowed in the header. Included files' header bindings become available. Circular includes are an error.

### Structural Rules

| Rule | Rationale |
|------|-----------|
| **No `"` (double quotes)** | HTML/transport embedding without escaping |
| One root `{}` block, required | Single source of truth |
| Header: `=`, `if/else`, `for`, `include` | Declarations and computation |
| Payload: `=`, `if/else`, `for`, `use`, `${}` | Data with composition |
| No `include` inside payload | No surprise file loads in nested data |
| No `use` in header | `use` is for composing into payload |
| No function definitions | Not a programming language |
| No classes, imports, loops with side effects | Declarative only |
| No mutation / reassignment | Bindings are immutable once set |
| Trailing commas allowed | In lists and everywhere else |
| No implicit type coercion | `'8080'` is always a string, `8080` is always an int |

---

## API Design

### `stdlib/drs.dr` - Public API

```dragon
from drs import load, loads, dumps

# Parse a .drs file from disk
config: dict = drs.load('config.drs')

# Parse a .drs string
config: dict = drs.loads(source_string)

# Emit a dict as .drs text
text: str = drs.dumps(data)

# Parse with variables injected (e.g., env vars)
config: dict = drs.load('config.drs', vars={'cpu_count': 8, 'env': 'staging'})
```

### Return Type

`drs.load` / `drs.loads` returns a `dict` (Dragon's native dict type). Nested `{}` blocks become nested dicts. Lists become lists. Primitives map directly.

---

## Implementation Plan

All phases implemented as pure `.dr` stdlib code. No C++ or LLVM changes needed.

### Phase 0: Tokenizer (`stdlib/drs.dr`)

Hand-written tokenizer (same pattern as `json.dr`, `csv.dr`).

Token types: - `LBRACE`, `RBRACE`, `LBRACKET`, `RBRACKET` - `EQUALS`, `COLON`, `COMMA` - `STRING` (single-quoted), `RAW_STRING` (backtick), `INT`, `FLOAT`, `BOOL`, `NONE` - `IDENT` (bare keys, references) - `DOLLAR_BRACE` (`${`) - `IF`, `ELSE`, `FOR`, `IN`, `RANGE`, `USE`, `INCLUDE` - `AND`, `OR`, `NOT` - `QUESTION`, `TERNARY_COLON` (for ternary `?:`) - `COMPARISON` (`==`, `!=`, `<`, `>`, `<=`, `>=`) - `ARITH` (`+`, `-`, `*`, `/`, `%`) - `NEWLINE`, `EOF` - `COMMENT` (discarded) - `DOUBLE_QUOTE` → immediate parse error with clear message

### Phase 1: Parser - Data Only

Recursive descent parser handling the core data subset (no expressions, no control flow):

- Bare key-value pairs: `key = value`
- Typed key-value pairs: `key: type = value`
- Nested objects: `key = { ... }`
- Lists: `[1, 2, 3]`
- Single-quoted and backtick strings
- Comments (discarded)
- Root `{}` payload extraction

Output: nested `dict` structure.

**Test:** Parse a static `.drs` file with nested objects and lists, verify dict output matches expected.

### Phase 2: Header Bindings + `use`

- Parse header `=` bindings into a symbol table (dict)
- Resolve `use` statements by copying bindings into payload dicts
- Override semantics: later keys win
- `include` support: read + parse included files, merge their header bindings

**Test:** Header bindings referenced via `use` in payload. Include files. Override behavior.

### Phase 3: Expression Evaluator

Mini expression evaluator for `${}` blocks:

- Variable references (lookup in symbol table)
- Arithmetic: `+`, `-`, `*`, `/`, `%`
- Comparison: `==`, `!=`, `<`, `>`, `<=`, `>=`
- Boolean: `and`, `or`, `not`
- Ternary: `cond ? true_val : false_val`
- String concatenation: `+`
- String interpolation within `'` strings

Operator precedence (low to high):
1. Ternary `? :`
2. `or`
3. `and`
4. `not`
5. Comparison `== != < > <= >=`
6. Addition `+ -`
7. Multiplication `* / %`

**Test:** Various expressions, ternaries, variable resolution, type errors.

### Phase 4: Control Flow

- `if/else` in header and payload (evaluates condition, selects branch)
- `for i in range(n) { expr }` → produces list
- `for x in list if cond { expr }` → filter + map

`for` evaluates to a list. `if/else` evaluates to a set of bindings.

**Test:** Conditional configs, generated port lists, filtered lists.

### Phase 5: `dumps` - Emitter

Convert a Dragon `dict` back to `.drs` text:

- Pretty-print with indentation
- Infer types from values
- Emit nested objects, lists, strings (uses `'` exclusively)
- No header/expressions in output (pure data)

**Test:** Round-trip: `drs.loads(drs.dumps(data)) == data`.

### Phase 6: Error Reporting

- Line/column numbers in all parse errors
- Clear messages: `'Expected = after key name at line 5, column 12'`
- **`"` detection:** `'Double quotes are not allowed in .drs files. Use single quotes or backticks instead (line 3, column 8)'`
- Circular include detection
- Undefined variable references
- Type mismatch in expressions

---

## What Dragon Features Are Needed

### Already Available (No Changes)

| Feature | Used For | Reference |
|---------|----------|-----------|
| String char indexing `s[i]` | Tokenizer | json.dr, csv.dr |
| String slicing `s[i:j]` | Token extraction | json.dr |
| `.split`, `.strip`, `.startswith` | Line parsing | configparser.dr |
| `dict` with string keys | Symbol table + output | json.dr |
| `list[str]`, `list[dict]` | Token stream, results | json.dr |
| File I/O (`io.open`) | `load` reads files | io.dr |
| `len`, string methods | General parsing | All stdlib |
| `for` loops, `while` loops | Iteration | All stdlib |
| `if/else`, ternary | Control flow | All stdlib |
| `try/except` | Error handling | json.dr |
| f-strings | Error messages | All stdlib |
| Classes | Token, Parser, Expr types | - |
| `isinstance` / `type` | Expression node dispatch | - |
| Cross-module imports | `from drs import load` | - |

### Potentially Needed Enhancements

| Feature | Why | Workaround | Priority |
|---------|-----|------------|----------|
| `dict.get(key, default)` | Safe key lookup with fallback | `if key in dict` + `dict[key]` | Nice-to-have |
| `dict.update(other_dict)` | `use` semantics (merge dicts) | Manual key-by-key copy loop | Nice-to-have |
| `dict.keys` → `list[str]` | Iterate header bindings | Already exists in runtime | Verify |
| `str.find(sub)` → `int` | Tokenizer substring search | `in` + manual scan | Verify |
| `int` / `float` conversion | Parse number tokens | `dragon_str_to_int` via FFI | Verify |
| Enum or constants | Token types | Integer constants | Workaround |

None of these are blockers. The json.dr parser already handles equivalent complexity.

---

## What `.drs` buys the project

### Positive

- Dragon gets a native, powerful config format that feels like Dragon
- `.drs` can replace JSON/YAML/TOML in Dragon projects
- **HTML-safe by design** - embeds in `"..."` attributes without escaping
- Implementation dogfoods Dragon's class system, string handling, and dict operations
- No C++ or LLVM changes required - pure stdlib addition
- Pattern established for future format parsers (e.g., XML, MessagePack)

### Negative

- Another format to learn (mitigated: syntax is Dragon-native)
- Expression evaluator adds complexity vs. pure-data formats
- Must carefully limit features to prevent scope creep toward Turing-completeness
- No `"` means copy-pasting from JSON requires converting quotes

### Neutral

- `.drs` files are Dragon-specific; cross-language interop uses JSON/YAML export
- `dumps` emits pure data (no expressions) - lossy round-trip for computed configs

---

## File Extension

`.drs` - Dragon Script

MIME type: `text/x-dragon-script` (future registration)

---

## Open Questions

1. Should `for` support nested iteration (`for i in range(n) for j in range(m)`)?
 **Current answer:** No. Single-level only.
2. Should there be a schema validation layer?
 **Current answer:** Deferred. Type annotations are documentation for now.
3. Should `dumps` preserve comments?
 **Current answer:** No. Comments are discarded on parse (same as JSON).
