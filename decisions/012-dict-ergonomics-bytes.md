# Decision 012: Dict Ergonomics & Bytes Type

Done. Parts 1-3 + value tagging + hash table shipped; Part 4 (`bytes`) landed later.

Three dict ergonomics tweaks plus a full `bytes` type for both modes: bare-key dict literals (`{name: 'Jon'}` without quoting keys), dot-access on dicts (`data.name` instead of `data["name"]`), computed keys `{(expr): value}`, and proper `bytes` (`b'hello'`, `b"\x00\xff"`) with method support.

`.dr` should feel nicer than Python here. JS, Lua, Ruby, Go all let you write `{name: 'Jon'}` without quoting keys - requiring `{"name": "Jon"}` when the key is obviously a string literal is ceremony that adds nothing. Same for reads: `data["name"]` is noisy when the compiler knows it's `dict[str, Any]`; `data.name` reads fine and matches JS/TS.

On bytes: Dragon had strings (`str` = `i8*`, null-terminated) but `b''` only half-worked - prefix lexed, parsed, type-checked (`Type::Kind::Bytes` exists), then CodeGen silently treated bytes as regular strings. No length tracking, no null-byte support, no bytes methods. Real `bytes` needed for binary file I/O, network protocols (HTTP server), crypto hashing, image/audio, any binary interop.

## Part 1: Bare-Key Dict Literals (`.dr` mode only)

### Current behavior

```dragon
// Today: name is parsed as a NameExpr (variable lookup)
const key: str = "name"
const data: dict = {key: 'Jon'} // uses VALUE of variable key → {"name": "Jon"}
```

### New behavior in `.dr` mode

```dragon
// Bare identifier keys → string literals (JS-style)
const data: dict = {name: 'Jon', age: 10, house: 'Stark'}
// Equivalent to: {"name": "Jon", "age": 10, "house": "Stark"}

// Quoted keys still work (required for special characters)
const headers: dict = {"content-type": "text/html", "x-request-id": "abc"}

// Computed keys - parentheses mean "evaluate this expression"
const field: str = "age"
const data: dict = {name: 'Jon', (field): 10}
// → {"name": "Jon", "age": 10}

// Expression keys
const data: dict = {(get_key): value, (f"prefix_{i}"): i}
```

### Disambiguation rules

| Syntax | `.dr` meaning | `.py` meaning |
|--------|--------------|--------------|
| `{name: 'Jon'}` | `{"name": "Jon"}` - bare key is string | `{name_var: "Jon"}` - variable lookup |
| `{"name": 'Jon'}` | `{"name": "Jon"}` - explicit string | `{"name": "Jon"}` - explicit string |
| `{(name): 'Jon'}` | `{name_var: "Jon"}` - evaluated | N/A (valid but unusual) |
| `{(f"{x}_key"): 1}` | Computed f-string key | N/A |

**`.py` mode is unchanged.** Python semantics preserved - bare identifiers are variable
lookups, as always.

### Implementation - COMPLETE

**Parser change** (~40 lines in `Parser::parseDict`):

`parseDictKey` lambda in `.dr` mode:
1. If current token is `IDENTIFIER` and next token is `COLON` (not `:=` walrus):
 - `makeBareKey(name)` creates a `StringLiteral` with the identifier's lexeme as value
 - Advances past the identifier
2. If current token is `LEFT_PAREN`:
 - Consume `(`
 - Parse `expression` (evaluated at runtime)
 - Consume `)`
3. Otherwise: fall through to `expression` (handles quoted strings, numbers, etc.)

First-entry detection uses lookahead and backtracking for computed keys `(expr)` vs
set/tuple ambiguity. `makeBareKey` lambda creates a default `StringLiteral` and sets
`.value` manually (StringLiteral has no string constructor).

**No AST changes.** `DictExpr` already stores `vector<pair<Expr, Expr>>` - the key
`Expr` is just a `StringLiteral` instead of a `NameExpr`. CodeGen sees no difference.

**6 parser tests added:** BareKeyDict, BareKeyDictMixedWithQuoted,
BareKeyDictSingleEntry, ComputedKeyDict, PyModeDictNotBareKey,
BareKeyDictWithNumericValue.

### Edge cases

| Case | Behavior |
|------|----------|
| `{class: 'value'}` | Works - keywords are valid bare keys (parser context-switches) |
| `{True: 1}` | `{True: 1}` - boolean literal, not string. Only IDENTIFIER → string |
| `{42: 'value'}` | `{42: "value"}` - number literal, not string. Only IDENTIFIER → string |
| `{name}` | Set literal containing variable `name` (no colon = set, not dict) |
| `{name: v for ...}` | Dict comprehension - bare key is string in `.dr` mode |

## Part 2: Dot-Access on Dicts (`.dr` mode only)

### Syntax

```dragon
const data: dict[str, Any] = {name: 'Jon', age: 10}

// Read - compiles to dragon_dict_get(data, "name")
const name: str = data.name

// Write - compiles to dragon_dict_set(data, "age", 11)
data.age = 11

// Nested - compiles to dragon_dict_get(dragon_dict_get(data, "address"), "city")
const city: str = data.address.city

// Dict methods still work (called with parens)
const keys: list = data.keys
const val: Any = data.get("name", "default")
```

### How the compiler resolves `data.X`

`data.X` is an `AttributeExpr`. The compiler resolves it based on the **type of `data`**:

```
visit(AttributeExpr):
 1. Is object a class instance? → GEP to struct field (existing path)
 2. Is object a dict?
 a. Is attribute being CALLED (CallExpr parent)?
 → Check dict method names (keys, values, items, get, pop, ...)
 → If match: dispatch to runtime dict method (existing path)
 b. Not being called?
 → Emit dragon_dict_get(obj, "attribute_name")
 3. Otherwise → existing behavior
```

For assignment (`data.name = value`):

```
visit(AssignStmt):
 If target is AttributeExpr and object is dict:
 → Emit dragon_dict_set(obj, "attribute_name", value)
```

### Dict method names (reserved, not treated as key access)

These identifiers, when used with ``, dispatch to dict methods instead of key access:

`keys`, `values`, `items`, `get`, `pop`, `popitem`, `clear`, `update`, `copy`,
`setdefault`, `__dict__`, `__len__`, `__contains__`, `__getitem__`, `__setitem__`

Without ``, they are key access: `data.keys` → `dragon_dict_get(data, "keys")`.
With ``, they are method calls: `data.keys` → `dragon_dict_keys(data)`.

### `.py` mode

**No dot-access on dicts.** `data.name` on a dict in `.py` mode is a type error.
Use `data["name"]` or `data.get("name")`. Python tax.

### Implementation - COMPLETE

| Component | Change | LOC |
|-----------|--------|-----|
| CodeGen `visit(AttributeExpr&)` | Before class GEP path, check if object is dict via `lookupVarKind` → emit `dragon_dict_get(obj, "attr")` | ~15 |
| CodeGen `visit(AssignStmt&)` | Before static field path, check if target is `AttributeExpr` on dict → emit `dragon_dict_set_tagged(obj, "attr", val, tag)` | ~20 |
| TypeChecker `typeNames` | Registered bare `dict`→`DictType(str,Any)`, `list`→`ListType(Any)`, `tuple`→`TupleType(Any)`, `set`→`ListType(Any)` as default generic types | ~10 |
| CodeGen `typeExprToKind` | Added bare `dict`/`Dict`/`list`/`tuple`/`set` → correct VarKind for NamedTypeExpr | ~5 |
| CodeGen `typeExprToLLVM` | Added bare `dict`/`Dict`/`tuple`/`Tuple`/`set`/`Set` → `i8PtrType` for NamedTypeExpr | ~4 |

**13 E2E + IR tests added:** BareKeyDictBasic, BareKeyDictMixed, ComputedKeyDict,
DictDotAccessRead, DictDotAccessReadBareKey, DictDotAccessWrite,
DictDotAccessWriteNew, DictMethodsStillWork, BareKeyDictSingleEntry,
DictDotAccessReadWriteCombined, BareKeyDictIR, DictDotAccessReadIR,
DictDotAccessWriteIR.

## Part 3: String Quotes (Documentation)

Both `'single'` and `"double"` quotes are **already fully supported** in both modes.
This section documents the behavior for the record.

| Syntax | Type | Both modes |
|--------|------|-----------|
| `'hello'` | `str` | Yes |
| `"hello"` | `str` | Yes |
| `'''multi\nline'''` | `str` | Yes |
| `"""multi\nline"""` | `str` | Yes |
| `f'hello {name}'` | `str` (f-string) | Yes |
| `f"hello {name}"` | `str` (f-string) | Yes |
| `r'no\nescape'` | `str` (raw) | Yes |
| `r"no\nescape"` | `str` (raw) | Yes |
| `b'bytes'` | `bytes` | Lexes/parses, no codegen (this decision fixes it) |
| `b"bytes"` | `bytes` | Lexes/parses, no codegen (this decision fixes it) |
| `rb'raw bytes'` | `bytes` (raw) | Lexes/parses, no codegen |
| `br"raw bytes"` | `bytes` (raw) | Lexes/parses, no codegen |

Single and double quotes are interchangeable everywhere. Convention: use `'single'`
for short strings, `"double"` when the string contains apostrophes, `"""triple"""`
for docstrings and multi-line. This is not enforced.

## Part 3a: Dict Value Tagging (done)

### Problem

Dict values are stored as uniform `int64_t`. String values are pointers cast to i64
via `PtrToInt`. When printing a dict value retrieved by subscript (`d["name"]`) or
dot-access (`d.name`), the CodeGen sees `i64` return type from `dragon_dict_get` and
dispatches to `dragon_print_int` - printing the raw pointer number instead of the
string content.

Withou runtime type information on dict values, there is no way to correctly print,
compare, or cast values retrieved from a `dict[str, Any]`.

### Solution: Per-entry type tags

Each dict entry stores an `int8_t` tag alongside its `int64_t` value:

```
Tag 0 = int (value is literal i64)
Tag 1 = str (value is char* cast to i64)
Tag 2 = float (value is double bitcast to i64)
Tag 3 = bool (value is 0 or 1)
Tag 4 = None
Tag 5 = list (value is DragonList* cast to i64)
Tag 6 = dict (value is DragonDict* cast to i64)
```

### Runtime additions

| Function | Signature | Description |
|----------|-----------|-------------|
| `dragon_dict_set_tagged` | `(DragonDict*, const char*, i64, i64) → void` | Store value with type tag |
| `dragon_dict_get_tag` | `(DragonDict*, const char*) → i64` | Retrieve tag for a key |
| `dragon_print_tagged` | `(i64, i64) → void` | Print value based on tag (str→`%s`, float→`%g`, etc.) |

`dragon_dict_set` is preserved as a convenience wrapper that calls
`dragon_dict_set_tagged` with `TAG_INT`.

### CodeGen changes

| Path | Change |
|------|--------|
| `visit(DictExpr&)` | Determines tag from LLVM type of each value before i64 conversion; calls `dragon_dict_set_tagged` |
| `visit(DictCompExpr&)` | Same tagging logic for comprehension bodies |
| `visit(AssignStmt&)` - dict subscript write | Tags value before `dragon_dict_set_tagged` |
| `visit(AssignStmt&)` - dict dot-access write | Tags value before `dragon_dict_set_tagged` |
| `print` dispatch | Detects `SubscriptExpr` on dict or `AttributeExpr` on dict → calls `dragon_dict_get_tag` + `dragon_print_tagged` instead of default type-based dispatch |

Tag determination is compile-time based on LLVM IR type:
- `i1` → `TAG_BOOL` (3)
- `f64` → `TAG_FLOAT` (2)
- `ptr` (pointer) → `TAG_STR` (1)
- `i64` → `TAG_INT` (0)

### Impact on print_dict

`dragon_print_dict` now prints values correctly based on tags:
- `TAG_STR` → `printf("'%s'", (char*)value)`
- `TAG_FLOAT` → `printf("%g", bitcast_to_double(value))`
- `TAG_BOOL` → `printf("True"/"False")`
- `TAG_NONE` → `printf("None")`
- default → `printf("%ld", value)`

### Performance

Tags add 1 byte per dict entry. `dragon_print_tagged` adds zero lookup overhead - the
tag is retrieved in the same probe as the value via `dragon_dict_get_tag`, which shares
the same hash table slot.

### Future: Runtime type checking

The tag system enables optional runtime type checking for dict access:

```dragon
x: int = d.age // could emit: get value, check tag == TAG_INT, throw TypeError if not
```

A single combined function `dragon_dict_get_checked(d, key, expected_tag)` would do
one hash probe and one branch - zero overhead vs the current unchecked path.

This is deferred. Currently tags are used for print dispatch only.

## Part 3b: Hash Table Dict (done)

### Problem

The original `DragonDict` used linear scan (`O(n)` per lookup):

```c
// Old: O(n) - fine for <10 keys, unacceptable for 100+
for (int64_t i = 0; i < d->size; i++) {
 if (strcmp(d->keys[i], key) == 0) return d->values[i];
}
```

Python's dict is `O(1)` via hash table. Dragon targeting Python developers must match
this expectation.

### Solution: CPython compact dict design

Two data structures:

1. **Dense entries array** - stores `{hash, key, value, tag}` in insertion order.
 Preserves Python 3.7+ insertion-order iteration guarantee.
2. **Sparse index table** - maps `hash → entry_index` for O(1) lookup.
 Uses open addressing with linear probing.

```c
struct DictEntry {
 uint64_t hash; // FNV-1a, precomputed
 const char* key;
 int64_t value;
 int8_t tag;
};

struct DragonDict {
 DictEntry* entries; // dense, insertion order
 int64_t* indices; // sparse hash index → entry index (1=empty, -2=tombstone)
 int64_t size; // live entries
 int64_t capacity; // entries array capacity
 int64_t index_size; // index table size (always power of 2)
};
```

### Hash function

FNV-1a (64-bit) on the key string. Fast, good distribution, no external dependency:

```c
static uint64_t dict_hash(const char* key) {
 uint64_t h = 14695981039346656037ULL;
 for (const char* p = key; *p; p++) {
 h ^= (uint64_t)(unsigned char)*p;
 h *= 1099511628211ULL;
 }
 return h | 1; // ensure non-zero (0 reserved as sentinel)
}
```

### Probe strategy

Linear probing with bitmask (`slot = hash & (index_size - 1)`). Load factor ~66%
(grow when `size * 3 >= index_size * 2`). Index table size is always 2x entries
capacity and a power of 2.

### Lookup path

```c
int64_t dragon_dict_get(DragonDict* d, const char* key) {
 uint64_t h = dict_hash(key);
 int64_t slot = dict_probe(d, key, h); // O(1) amortized
 int64_t idx = d->indices[slot];
 if (idx >= 0) return d->entries[idx].value;
 // KeyError...
}
```

`dict_probe` scans the index table: compares hash first (fast reject), then strcmp
for confirmation. Skips tombstone slots (`-2`), stops at empty slots (`-1`).

### Deletion

`dragon_dict_pop` marks the index slot as tombstone (`-2`), shifts the dense entries
array to preserve insertion order, then rebuilds the index. This is `O(n)` but pop is
rarely used in hot paths. Clear sets `size = 0` and resets all index slots to empty.

### Resize

When entries array is full or index load exceeds 66%, both arrays double in size.
Index is rebuilt from scratch (rehash all entries). Amortized `O(1)` per insertion.

### Performance characteristics

| Operation | Old (linear) | New (hash table) |
|-----------|-------------|-----------------|
| Lookup | O(n) - ~10-200ns | O(1) - ~30-40ns |
| Insert | O(n) check + O(1) append | O(1) amortized |
| Iterate | O(n) dense walk | O(n) dense walk (same) |
| Delete | O(n) scan + shift | O(n) shift + rebuild |
| Memory | 17 bytes/entry | ~33 bytes/entry + index overhead |

### Insertion order guarantee

Iteration walks the dense entries array from index 0 to `size-1`. This is insertion
order. `dragon_dict_keys`, `dragon_dict_values`, `dragon_dict_items` all
produce lists in insertion order, matching Python 3.7+ behavior.

### Changes

**One file only:** `lib/Runtime/runtime.cpp`. Zero compiler changes. All
`dragon_dict_*` function signatures are unchanged - the hash table is an internal
implementation detail behind the same API.

### Tests

All 855 existing tests pass unchanged. The hash table is a drop-in replacement.

## Part 4: Bytes Type

### Current pipeline state

| Stage | Status | What exists |
|-------|--------|------------|
| Lexer | Complete | `b''`, `b""`, `rb''`, `br""` all recognized ([Lexer.cpp:219](src/Lexer.cpp#L219)) |
| Token | Complete | `TokenType::BYTES` exists ([Token.h:15](include/dragon/Token.h#L15)), though lexer emits `STRING` |
| AST | Complete | `StringLiteral::isBytes` flag ([AST.h:119](include/dragon/AST.h#L119)) |
| Parser | Complete | Sets `isBytes = true` from `b` prefix ([Parser.cpp:432](src/Parser.cpp#L432)) |
| TypeChecker | Complete | `Type::Kind::Bytes`, `bytesType` registered ([TypeChecker.cpp:502](src/TypeChecker.cpp#L502)) |
| CodeGen | **Missing** | `isBytes` never checked - bytes silently treated as `str` |
| Runtime | **Missing** | Zero bytes functions |

### LLVM representation

Strings are `i8*` (null-terminated C strings). Bytes are **length-prefixed** because
they can contain null bytes:

```
str: i8* → "hello\0" (null-terminated)
bytes: %DragonBytes* → {i64 len, i8* data} (length + raw data)
```

The `DragonBytes` runtime struct:

```c
typedef struct {
 int64_t len;
 uint8_t* data; // NOT null-terminated - may contain \x00
} DragonBytes;
```

### Bytes literals

```dragon
const a: bytes = b"hello"
const b: bytes = b'\x00\x01\x02'
const c: bytes = b"mixed\x00content"
const d: bytes = rb"no\escape" // raw bytes - backslash is literal

// Triple-quoted bytes
const e: bytes = b"""
binary
data
"""
```

### Bytes operations

**Indexing returns `int`** (Python convention):

```dragon
const data: bytes = b"hello"
const first: int = data[0] // 104 (ASCII 'h')
const last: int = data[-1] // 111 (ASCII 'o')
```

**Slicing returns `bytes`:**

```dragon
const data: bytes = b"hello world"
const sub: bytes = data[0:5] // b"hello"
const rev: bytes = data[::-1] // b"dlrow olleh"
```

**Operators:**

| Operator | Meaning | Example |
|----------|---------|---------|
| `+` | Concatenate | `b"hello" + b" world"` → `b"hello world"` |
| `*` | Repeat | `b"\x00" * 10` → 10 null bytes |
| `in` | Contains | `0x68 in b"hello"` → `True` |
| `==` / `!=` | Compare | `b"abc" == b"abc"` → `True` |
| `<` / `>` | Lexicographic | `b"abc" < b"abd"` → `True` |
| `len` | Length | `len(b"hello")` → `5` |

### Bytes methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `decode(encoding)` | `(str) -> str` | Decode to string. Default: `"utf-8"` |
| `hex` | ` -> str` | Hex representation: `b"\xff"` → `"ff"` |
| `fromhex(s)` | `static (str) -> bytes` | `bytes.fromhex("ff")` → `b"\xff"` |
| `find(sub)` | `(bytes) -> int` | First index of sub, or -1 |
| `rfind(sub)` | `(bytes) -> int` | Last index of sub, or -1 |
| `count(sub)` | `(bytes) -> int` | Count occurrences |
| `replace(old, new)` | `(bytes, bytes) -> bytes` | Replace all occurrences |
| `startswith(prefix)` | `(bytes) -> bool` | Prefix check |
| `endswith(suffix)` | `(bytes) -> bool` | Suffix check |
| `upper` | ` -> bytes` | ASCII uppercase |
| `lower` | ` -> bytes` | ASCII lowercase |
| `strip` | ` -> bytes` | Strip ASCII whitespace |
| `lstrip` | ` -> bytes` | Strip left |
| `rstrip` | ` -> bytes` | Strip right |
| `split(sep)` | `(bytes) -> list[bytes]` | Split by separator |
| `join(iterable)` | `(list[bytes]) -> bytes` | Join with separator |
| `index(sub)` | `(bytes) -> int` | Like find but raises ValueError |
| `rindex(sub)` | `(bytes) -> int` | Like rfind but raises ValueError |
| `isdigit` | ` -> bool` | All ASCII digits |
| `isalpha` | ` -> bool` | All ASCII letters |
| `isalnum` | ` -> bool` | All ASCII alphanumeric |
| `isspace` | ` -> bool` | All ASCII whitespace |

### Conversion between `str` and `bytes`

```dragon
// str → bytes
const s: str = "hello"
const b: bytes = s.encode // UTF-8 by default
const b2: bytes = s.encode("ascii")

// bytes → str
const b: bytes = b"hello"
const s: str = b.decode // UTF-8 by default
const s2: str = b.decode("ascii")
```

**Encoding support (Phase 1):** ASCII and UTF-8 only. Both are identity operations
on ASCII-only content. UTF-8 multi-byte validation is deferred.

### Implementation

| Component | Work | LOC |
|-----------|------|-----|
| **Runtime: `DragonBytes` struct** | `typedef struct { int64_t len; uint8_t* data; } DragonBytes` | ~20 |
| **Runtime: construction** | `dragon_bytes_new(data, len)`, `dragon_bytes_from_literal(str, len)` | ~30 |
| **Runtime: methods** | 22 methods listed above | ~350 |
| **Runtime: conversions** | `dragon_str_encode`, `dragon_bytes_decode` | ~50 |
| **CodeGen: literal emission** | Check `isBytes` in `visit(StringLiteral&)` - emit `{len, data}` struct | ~30 |
| **CodeGen: operators** | `+`, `*`, `in`, `==`, `<`, `[]`, `[:]` on bytes | ~100 |
| **CodeGen: method dispatch** | Bytes method calls in `visit(CallExpr&)` | ~80 |
| **CodeGen: str.encode** | New method on str type | ~15 |

**Total bytes: ~675 LOC**

## Implementation Plan

| Phase | Feature | LOC | Tests | Status |
|-------|---------|-----|-------|--------|
| 1 | Bare-key dict literals (`.dr` parser) | ~40 | 6 parser | **COMPLETE** |
| 2 | Computed keys `(expr)` in dict literals | (included in Phase 1) | (included) | **COMPLETE** |
| 3 | Dot-access reads + writes on dicts | ~35 | 13 (3 IR + 10 E2E) | **COMPLETE** |
| 3a | Dict value tagging (runtime + codegen) | ~120 | (existing tests validated) | **COMPLETE** |
| 3b | Hash table dict (runtime rewrite) | ~180 | (existing tests validated) | **COMPLETE** |
| 4 | `bytes` runtime struct + construction | ~50 | ~3 | Proposed |
| 5 | `bytes` CodeGen (literals, operators) | ~130 | ~10 | Proposed |
| 6 | `bytes` methods + str.encode/bytes.decode | ~430 | ~15 | Proposed |

**Dict ergonomics (Phases 1-3b): COMPLETE - ~375 LOC, 19 new tests, 855 total passing**

Phases 4-6 (bytes type) are independent of the dict work.

## Affected Components

### Dict ergonomics (COMPLETE)
- **Parser:** `parseDict` - `makeBareKey` lambda + `parseDictKey` lambda for bare/computed/quoted key disambiguation (`.dr` mode only)
- **TypeChecker:** `typeNames` - registered default generic types for bare `dict`, `list`, `tuple`, `set`
- **CodeGen `typeExprToKind`:** bare container names → correct VarKind for NamedTypeExpr
- **CodeGen `typeExprToLLVM`:** bare container names → `i8PtrType` for NamedTypeExpr
- **CodeGen `visit(AttributeExpr&)`:** dict dot-access read → `dragon_dict_get`
- **CodeGen `visit(AssignStmt&)`:** dict dot-access write → `dragon_dict_set_tagged`; dict subscript write → `dragon_dict_set_tagged`
- **CodeGen `visit(DictExpr&)`:** tagged set with compile-time type tags
- **CodeGen `visit(DictCompExpr&)`:** tagged set for comprehension values
- **CodeGen `print` dispatch:** detects dict subscript/dot-access → `dragon_dict_get_tag` + `dragon_print_tagged`
- **Runtime:** Hash table DragonDict (FNV-1a, open addressing, insertion-order dense array); `dragon_dict_set_tagged`, `dragon_dict_get_tag`, `dragon_print_tagged`
- **Sema:** No changes

### Bytes type
- **CodeGen:** `visit(StringLiteral&)` - bytes literal emission; `visit(CallExpr&)` - bytes method dispatch; `visit(BinaryExpr&)` - bytes operators
- **Runtime:** New `DragonBytes` struct + ~22 methods + encode/decode

## `.py` Mode

**No changes.** `.py` mode retains full Python semantics:
- Dict keys require quotes: `{"name": "Jon"}`
- Dict access via brackets: `data["name"]` or `data.get("name")`
- `b''` / `b""` bytes literals work identically (same CodeGen path)
- `str.encode` and `bytes.decode` work identically

The dict ergonomics (Parts 1-2) are `.dr`-mode-only. The bytes type (Part 4) is both
modes.
