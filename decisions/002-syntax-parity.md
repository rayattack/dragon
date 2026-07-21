# Decision 002: Python Syntax & Semantics, Where They Compile

Still in progress - scope got corrected along the way. Dragon is *inspired by* Python, not a superset (see zen.md). The old "every valid Python 3.12 program runs" line was wrong and I should've narrowed it sooner. Parity now means: within the subset Dragon actually implements, syntax parses, builtins execute, methods behave. Depends on 001 and 008. Many gaps in the tables below have since closed (`match`/`case`, `enum`, tuple unpacking, generators, f-string format specs) - if a row looks stale, check git history before trusting me.

Dragon's pitch is "the snake that became a dragon" - typed, compiled, Python-ish. For the subset we support, I want the whole stack to work: parser, AST, sema, typechecker, codegen, runtime. Not just "parses and dies." CEmitter's gone now; one backend. Teh scope list below is still aspirational in places; I wrote some of this at 1am and occassionally marked things TODO that aren't TODO anymore.

What I'm aiming at across the stack: every Python 3.12 grammer production handled in the parser (in our subset), every construct with an AST node, everything type-checked, everything emitting working LLVM IR, builtins/methods with runtime implementations, and dual-mode support so `.dr` and `.py` both work where the feature applies.

On dynamic stuff - `eval`, `exec`, `getattr`, metaclasses. I'm not hand-waving them away as "incompatible with AOT." But after staring at perf numbers for too long, the approach refined:

**LLJIT stays in the compilation pipeline only, NOT in the runtime.** Using LLJIT for `eval`/`exec` is wrong:
- 20-100ms per function compilation latency (unacceptable for hot paths)
- Tens of MB binary overhead from embedding full LLVM
- Julia spent 10+ years fighting this exact problem (solved with native code caching, not faster JIT)

**Revised dynamic features strategy:**

| Feature | Approach | Overhead |
|---------|----------|----------|
| `eval("1+2")` with literal args | **Compile-time evaluation** (Nim approach) | Zero - optimized away |
| `eval(expr)` with dynamic args | **Bytecode interpreter** (CPython-style, ~50KB) | ~10x vs native - acceptable for dynamic use |
| `getattr`/`setattr` | **Inline caching** (90% of call sites are monomorphic) | ~5ns monomorphic, ~50ns polymorphic |
| `exec(code)` | **Bytecode interpreter** | Same as eval |
| async/await | **State machine transformation** (Rust approach, zero-cost) | Zero - no JIT, no runtime overhead |

**Three-tier dict system** (nobody else doing this in a Python-ish AOT compiler):

| Tier | Type | Access Time | When Used |
|------|------|-------------|-----------|
| 1. TypedDict→struct | `TypedDict("Person", name=str, age=int)` | **1ns** (compiled to `obj->field`) | Known keys at compile time |
| 2. Homogeneous | `dict[str, int]` | **30-50ns** (hash lookup) | Single value type |
| 3. Tagged union | `dict[str, Any]` | **40-70ns** (hash + tag check) | Mixed value types |

TypedDict→struct is the big win: `d["name"]` becomes a struct field load, no hash. **50-70x** faster than CPython dict lookup.

---

## What Works Today

### Actually working end-to-end

| Category | Features |
|----------|----------|
| **Literals** | int (dec/hex/bin/oct), float, str (single/double/triple/raw/bytes), bool, None, Ellipsis |
| **Operators** | All arithmetic (`+`,`-`,`*`,`/`,`//`,`%`,`**`), comparison (`<`,`>`,`<=`,`>=`,`==`,`!=`,`in`,`is`), bitwise (`&`,`\|`,`^`,`<<`,`>>`,`~`), logical (`and`,`or`,`not`), all 13 augmented assignments |
| **Control flow** | if/elif/else, while/else, for-range (1/2/3 arg), break, continue, pass |
| **Functions** | def, params, defaults, `*args`, `**kwargs`, return, recursion, forward declarations, async flag parsed |
| **Classes** | Single inheritance, `__init__`, methods, field access, implicit self (.dr) |
| **Strings** | 47 methods, concat, indexing, slicing, f-strings (simple variable interpolation only) |
| **Lists** | new, append, get, set, len, slice, print, negative indexing |
| **Dicts** | new, set, get, get_default, len, has_key, keys, print |
| **Exceptions** | try/except/else/finally, raise (with from), handler dispatch via setjmp/longjmp |
| **Imports** | import, from-import, relative imports, multi-file compilation, StdlibRegistry |
| **Types** | Annotations, unions (`int \| str`), optionals, generics (`list[int]`), callable, tuple types |
| **Both modes** | `.dr` (braces, mandatory types) and `.py` (indentation, optional types) |

### Parses but Doesn't Compile

| Feature | Parser Status | CodeGen Status |
|---------|--------------|----------------|
| Tuples (`(1, 2, 3)`) | Parsed + type-checked | Emits constant 0 (no runtime type) |
| Sets (`{1, 2, 3}`) | Parsed | Emits NULL pointer |
| yield / yield from | AST node exists | No codegen |
| async / await | Flags and AST exist | No codegen |
| Decorators | Parsed as expressions | Not applied in codegen |
| Walrus (`:=`) | Token exists | Never consumed by parser |

### Doesn't Parse

| Feature | Severity |
|---------|----------|
| match/case (PEP 634) | High |
| Generator expressions `(x for x in y)` | High |
| Set comprehensions `{x for x in y}` | High |
| Nested comprehensions (multiple `for` clauses) | High |
| Tuple unpacking in assignments (`a, b = 1, 2`) | High |
| Starred unpacking in targets (`a, *b = [1,2,3]`) | High |
| Chained comparisons (`a < b < c`) | High - parses as left-assoc binary, wrong semantics |
| F-string expressions (`f"{x + 1}"`) | High - only simple var names work |
| F-string format specs (`f"{x:.2f}"`) | Medium |
| Positional-only params (`/`) | Medium |
| Keyword-only params (bare `*`) | Medium |
| type alias statement (PEP 695) | Medium |
| except* (PEP 654) | Low |
| Soft keywords (match, case, type) | Blocked - needed for match/case and type alias |

### Missing Builtins

| Category | Missing |
|----------|---------|
| **Aggregates** | min, max, sum, any, all |
| **Iteration** | enumerate, zip, map, filter, sorted, reversed, iter, next |
| **Type introspection** | isinstance, type (as function), callable, repr, hash, id |
| **Numeric** | ord, chr, round, pow (3-arg), divmod, hex, oct, bin, format |
| **Constructors** | list, dict, set, tuple, frozenset, bytes, bytearray, complex, slice |
| **I/O** | open, file methods |
| **Class** | super, property, classmethod, staticmethod |
| **Attribute** | getattr, setattr, hasattr, delattr, dir, vars |
| **Dynamic** | eval, exec, compile |

### Missing Type Methods

| Type | Missing Methods |
|------|----------------|
| **list** | insert, remove, pop, clear, extend, index, count, sort, reverse, copy |
| **dict** | values, items, pop, popitem, clear, update, setdefault, copy |
| **set** | Entire type - add, remove, discard, pop, clear, union, intersection, difference, symmetric_difference, issubset, issuperset, isdisjoint, copy, update |
| **tuple** | count, index (no runtime representation exists) |
| **int** | bit_length, to_bytes, from_bytes |
| **float** | is_integer, as_integer_ratio |

---

## Implementation Phases

### Phase A: Core Data Types

**Goal:** Tuple and set runtime types. List/dict method completion. Without these, unpacking, comprehensions, match/case, and many builtins are impossible.

#### A.1 Tuple Runtime Type

**`lib/Runtime/runtime.cpp`** - new `DragonTuple` struct:

```c
typedef struct {
 int64_t* data;
 int64_t length;
} DragonTuple;

extern "C" {
 DragonTuple* dragon_tuple_new(int64_t count);
 int64_t dragon_tuple_get(DragonTuple* t, int64_t idx);
 void dragon_tuple_set(DragonTuple* t, int64_t idx, int64_t val);
 int64_t dragon_tuple_len(DragonTuple* t);
 void dragon_print_tuple(DragonTuple* t);
 int64_t dragon_tuple_count(DragonTuple* t, int64_t val);
 int64_t dragon_tuple_index(DragonTuple* t, int64_t val);
}
```

Storage: all elements stored as `int64_t` (same convention as lists - pointers bitcast to i64).

**`src/CodeGen.cpp`** - replace stub `visit(TupleExpr&)`:
1. Declare runtime functions in `declareRuntimeFunctions`
2. Call `dragon_tuple_new(N)` where N = element count
3. For each element: visit, then `dragon_tuple_set(tuple, i, val)`
4. Store tuple pointer as `lastValue`

**`src/CodeGen.cpp`** - print dispatch: add tuple case to `visit(CallExpr&)` print handler.

**Tests** (~10):
- `TupleCreate`: `t = (1, 2, 3)` - verify IR calls `dragon_tuple_new`
- `TupleIndex`: `t[0]`, `t[-1]` - verify access
- `TuplePrint`: `print((1, 2, 3))` → `(1, 2, 3)`
- `TupleLen`: `len((1, 2))` → `2`
- `TupleNested`: `((1, 2), (3, 4))`
- `TupleSingleElement`: `(42)` - trailing comma
- `TupleEmpty`: ``
- E2E: `TupleCreateAndPrint`, `TupleIndexAndPrint`

#### A.2 Set Runtime Type

**`lib/Runtime/runtime.cpp`** - new `DragonSet` struct (open-addressing hash set):

```c
typedef struct {
 int64_t* buckets; // hash table buckets (values)
 uint8_t* states; // 0=empty, 1=occupied, 2=deleted
 int64_t capacity;
 int64_t count;
} DragonSet;

extern "C" {
 DragonSet* dragon_set_new;
 void dragon_set_add(DragonSet* s, int64_t val);
 int64_t dragon_set_contains(DragonSet* s, int64_t val);
 void dragon_set_remove(DragonSet* s, int64_t val); // raises KeyError
 void dragon_set_discard(DragonSet* s, int64_t val); // no error
 int64_t dragon_set_len(DragonSet* s);
 void dragon_set_clear(DragonSet* s);
 DragonSet* dragon_set_copy(DragonSet* s);
 DragonSet* dragon_set_union(DragonSet* a, DragonSet* b);
 DragonSet* dragon_set_intersection(DragonSet* a, DragonSet* b);
 DragonSet* dragon_set_difference(DragonSet* a, DragonSet* b);
 DragonSet* dragon_set_symmetric_difference(DragonSet* a, DragonSet* b);
 int64_t dragon_set_issubset(DragonSet* a, DragonSet* b);
 int64_t dragon_set_issuperset(DragonSet* a, DragonSet* b);
 int64_t dragon_set_isdisjoint(DragonSet* a, DragonSet* b);
 int64_t dragon_set_pop(DragonSet* s);
 void dragon_set_update(DragonSet* a, DragonSet* b);
 void dragon_print_set(DragonSet* s);
}
```

**`src/CodeGen.cpp`** - replace stub `visit(SetExpr&)`:
1. Declare runtime functions
2. Call `dragon_set_new`
3. For each element: visit, then `dragon_set_add(set, val)`

**`src/CodeGen.cpp`** - `in` operator: for sets, emit `dragon_set_contains`.

**Tests** (~12):
- Create, add, contains, remove, discard, len, clear
- Union, intersection, difference, symmetric_difference
- issubset, issuperset, isdisjoint
- Print, E2E

#### A.3 List Method Expansion

**`lib/Runtime/runtime.cpp`** - add 10 list methods:

| Method | Signature | Notes |
|--------|-----------|-------|
| `insert` | `dragon_list_insert(DragonList*, int64_t idx, int64_t val)` | Shift elements right |
| `remove` | `dragon_list_remove(DragonList*, int64_t val)` | First occurrence, ValueError if absent |
| `pop` | `dragon_list_pop(DragonList*, int64_t idx)` → `int64_t` | Default idx=-1 |
| `clear` | `dragon_list_clear(DragonList*)` | Reset length to 0 |
| `extend` | `dragon_list_extend(DragonList*, DragonList*)` | Append all from other |
| `index` | `dragon_list_index(DragonList*, int64_t val)` → `int64_t` | ValueError if absent |
| `count` | `dragon_list_count(DragonList*, int64_t val)` → `int64_t` | Count occurrences |
| `sort` | `dragon_list_sort(DragonList*)` | In-place ascending |
| `reverse` | `dragon_list_reverse(DragonList*)` | In-place reverse |
| `copy` | `dragon_list_copy(DragonList*)` → `DragonList*` | Shallow copy |

**`src/CodeGen.cpp`** - extend list method dispatch in `visit(CallExpr&)` / attribute handling.

**Tests** (~10): one per method + edge cases.

#### A.4 Dict Method Expansion

**`lib/Runtime/runtime.cpp`** - add 8 dict methods:

| Method | Signature | Notes |
|--------|-----------|-------|
| `values` | `dragon_dict_values(DragonDict*)` → `DragonList*` | List of values |
| `items` | `dragon_dict_items(DragonDict*)` → `DragonList*` | List of (key, value) tuples |
| `pop` | `dragon_dict_pop(DragonDict*, const char*)` → `int64_t` | KeyError if absent |
| `pop` | `dragon_dict_pop_default(DragonDict*, const char*, int64_t)` → `int64_t` | With default |
| `popitem` | `dragon_dict_popitem(DragonDict*)` → `DragonTuple*` | LIFO order |
| `clear` | `dragon_dict_clear(DragonDict*)` | Remove all |
| `update` | `dragon_dict_update(DragonDict*, DragonDict*)` | Merge other into self |
| `setdefault` | `dragon_dict_setdefault(DragonDict*, const char*, int64_t)` → `int64_t` | Get or insert |
| `copy` | `dragon_dict_copy(DragonDict*)` → `DragonDict*` | Shallow copy |

**`src/CodeGen.cpp`** - extend dict method dispatch.

**Tests** (~10): one per method + edge cases.

**Phase A output:** All 4 collection types (list, tuple, dict, set) fully functional with all Python methods.

---

### Phase B: Unpacking & Assignment Targets

**Goal:** `a, b = 1, 2` and `a, *rest = [1, 2, 3, 4]` work everywhere.

#### B.1 Tuple Unpacking in Assignment

**`src/Parser.cpp`** - modify assignment parsing:
- After parsing LHS expression, if next token is `,`, continue parsing as tuple of targets
- Result: `AssignStmt` where `target` is a `TupleExpr` containing `NameExpr` elements

**`src/CodeGen.cpp`** - extend `visit(AssignStmt&)`:
- If target is `TupleExpr`: evaluate RHS, then for each element in target tuple:
 - Call `dragon_tuple_get(rhs, i)` or `dragon_list_get(rhs, i)` depending on RHS type
 - Store into the named variable

**Dragon .dr mode:** Same syntax - `a: int, b: int = 1, 2` (with types) or `a, b = 1, 2` (inferred).

**Tests** (~10):
- `a, b = 1, 2`
- `a, b, c = (10, 20, 30)`
- `first, second = "ab"` (string unpacking)
- `a, b = b, a` (swap)
- Nested: `(a, b), c = (1, 2), 3`
- In annotated assignment: `a: int, b: int = 1, 2`

#### B.2 Starred Unpacking in Targets

**`src/Parser.cpp`** - allow `*name` in assignment target tuple:
- Parse `*` before a name in target position → StarredExpr
- Exactly one starred target allowed per tuple

**`src/CodeGen.cpp`** - starred unpacking logic:
- Count non-starred targets before and after the starred one
- Assign first N elements to pre-star targets
- Collect middle elements into a list for the starred target
- Assign last M elements to post-star targets

**Tests** (~8):
- `first, *rest = [1, 2, 3, 4]`
- `*init, last = [1, 2, 3, 4]`
- `first, *mid, last = [1, 2, 3, 4, 5]`
- `a, *b = "hello"` (string unpacking)

#### B.3 Double-Star in Calls

**`src/Parser.cpp`** - in call argument parsing, handle `**expr`:
- Already partially supported via StarredExpr
- Ensure `**kwargs` in calls generates dict unpacking

**`src/CodeGen.cpp`** - at call site, expand `**dict` into keyword arguments.

**Tests** (~4):
- `func(**{"key": val})`
- `func(*args, **kwargs)`

#### B.4 For-Loop Tuple Unpacking

**`src/CodeGen.cpp`** - extend `visit(ForStmt&)`:
- If target is TupleExpr, each iteration element is unpacked
- Works with lists of tuples, dict.items, enumerate

**Tests** (~6):
- `for k, v in [(1, 2), (3, 4)]:`
- `for i, x in enumerate(items):`
- `for k, v in d.items:`

**Phase B output:** Full unpacking support in assignments, loops, and calls.

---

### Phase C: Comprehensions & Generators

**Goal:** All 4 comprehension types + generators + nesting.

#### C.1 Set Comprehensions

**`include/dragon/AST.h`** - add `SetCompExpr`:
```cpp
class SetCompExpr : public Expr {
 std::unique_ptr<Expr> element;
 std::string varName;
 std::unique_ptr<Expr> iterable;
 std::unique_ptr<Expr> condition; // optional
};
```

**`src/Parser.cpp`** - in `parseDict`, after determining it's a set (no `:`), check for `for` keyword → parse as set comprehension.

**`src/CodeGen.cpp`** - emit loop: `dragon_set_new` + loop body calls `dragon_set_add`.

**Tests** (~6):
- `{x**2 for x in range(10)}`
- `{x for x in items if x > 0}`

#### C.2 Nested Comprehensions

**`include/dragon/AST.h`** - extend comprehension nodes with clause list:
```cpp
struct CompClause {
 std::vector<std::string> varNames; // loop variables (supports tuple unpacking)
 std::unique_ptr<Expr> iterable;
 std::vector<std::unique_ptr<Expr>> conditions; // if-filters
};
```

Extend `ListCompExpr`, `DictCompExpr`, `SetCompExpr` to use `vector<CompClause> clauses`.

**`src/Parser.cpp`** - after first `for` clause, continue parsing additional `for`/`if` clauses.

**`src/CodeGen.cpp`** - emit nested loops. Each `for` clause is an outer loop, each `if` is a conditional skip.

**Tests** (~8):
- `[x*y for x in a for y in b]`
- `[(x,y) for x in range(3) for y in range(3) if x != y]`
- `{k: v for d in dicts for k, v in d.items}`

#### C.3 Generator Expressions

**`include/dragon/AST.h`** - add `GeneratorExpr` (identical structure to `ListCompExpr`).

**`src/Parser.cpp`** - in parenthesized expression parsing:
- After first expression, if next token is `for`, parse as generator expression
- Distinguish from tuple: `(x for x in y)` is generator, `(x, y)` is tuple

**`src/CodeGen.cpp`** - **eager evaluation**: generate as list internally. True lazy generators require coroutine support (deferred). This is sufficient for `sum(x for x in items)`, `any(pred(x) for x in items)`, etc.

**Tests** (~8):
- `sum(x for x in range(10))`
- `any(x > 5 for x in items)`
- `list(x**2 for x in range(5))`
- `(x for x in y if x > 0)` - assigned to variable

**Phase C output:** All comprehension types with nesting and conditions. Generators eagerly evaluated.

---

### Phase D: Comparison & Expression Fixes

#### D.1 Chained Comparisons

**`include/dragon/AST.h`** - add `ChainedCompExpr`:
```cpp
class ChainedCompExpr : public Expr {
 std::vector<std::unique_ptr<Expr>> operands; // [a, b, c]
 std::vector<TokenType> operators; // [<, <]
};
```

**`src/Parser.cpp`** - modify comparison parsing:
- After `a < b`, if next token is also a comparison operator, collect chain
- Build `ChainedCompExpr` instead of nested `BinaryExpr`

**`src/CodeGen.cpp`**:
- Emit: `tmp_b = b; (a < tmp_b) && (tmp_b < c)`
- Each intermediate operand evaluated once, stored in temporary

**Tests** (~8):
- `a < b < c` → true iff both hold
- `0 <= x < 100`
- `a == b == c`
- `a < b <= c < d` - 4-element chain
- `a is b is c`
- `a in b in c` (rare but valid)

#### D.2 Walrus Operator (`:=`)

**`include/dragon/AST.h`** - add `WalrusExpr`:
```cpp
class WalrusExpr : public Expr {
 std::string name;
 std::unique_ptr<Expr> value;
};
```

**`src/Parser.cpp`** - in expression parsing, after `NameExpr`, check for `:=`:
- If found, parse RHS expression, create `WalrusExpr`
- Precedence: lower than comparisons, higher than comma

**`src/CodeGen.cpp`**:
- Evaluate RHS, store to variable, return value as expression result

**Tests** (~6):
- `if (n := len(a)) > 10:`
- `while line := input:`
- `[y for x in data if (y := f(x)) is not None]`

#### D.3 F-String Full Expression Support

Current state: f-strings only interpolate simple variable names (`{x}`). Need full expression support.

**Approach**: Keep lexer treating f-strings as single tokens. Enhance CodeGen f-string lowering to parse `{...}` blocks as Dragon expressions using a sub-parser.

**`src/CodeGen.cpp`** - rewrite f-string lowering:
1. Scan f-string value for `{...}` blocks (handling `{{`/`}}` escapes and nested braces)
2. Extract expression text from each `{...}` block
3. Parse each expression using Dragon's own parser (create temp lexer + parser)
4. Visit the parsed expression AST to get its LLVM value
5. Convert to string via type-appropriate `dragon_*_to_str` function
6. Concatenate all segments

**Format specs** (`{x:.2f}`): Split on `:` inside `{}`, pass format spec to `dragon_format` runtime function.

**Conversion flags** (`{x!r}`, `{x!s}`, `{x!a}`): Apply `repr`, `str`, `ascii` before string conversion.

**`lib/Runtime/runtime.cpp`** - add:
- `dragon_format_int(int64_t val, const char* spec)` → formatted string
- `dragon_format_float(double val, const char* spec)` → formatted string
- `dragon_format_str(const char* val, const char* spec)` → formatted string

**Tests** (~12):
- `f"{x + 1}"` - expression
- `f"{obj.attr}"` - attribute access
- `f"{func}"` - function call
- `f"{x:.2f}"` - float format
- `f"{name:>20}"` - string alignment
- `f"{x!r}"` - repr conversion
- `f"{'nested':^10}"` - nested string
- `f"{x:#010x}"` - hex format

**Phase D output:** Chained comparisons correct, walrus operator works, f-strings handle arbitrary expressions and format specs.

---

### Phase E: Function Parameter Extensions

#### E.1 Positional-Only Parameters (`/`)

**`include/dragon/AST.h`** - add to `FunctionDecl`:
```cpp
int posOnlyEnd = -1; // index after last positional-only param (1 = none)
```

**`src/Parser.cpp`** - in parameter parsing, recognize bare `/` token:
- `/` already lexed as `DIV` - in parameter context, treat as separator
- All params before `/` are positional-only

**`src/TypeChecker.cpp`** - at call sites, enforce positional-only params cannot be passed by keyword.

**Tests** (~6):
- `def f(x, /, y):` - x is positional-only, y is both
- `def f(x, y, /, z, *, w):` - full combination
- Error: `f(x=1)` when x is positional-only

#### E.2 Keyword-Only Parameters (bare `*`)

**`include/dragon/AST.h`** - add to `FunctionDecl`:
```cpp
int kwOnlyStart = -1; // index of first keyword-only param (1 = none)
```

**`src/Parser.cpp`** - recognize bare `*` (not followed by name) as separator:
- All params after bare `*` are keyword-only

**`src/TypeChecker.cpp`** - at call sites, enforce keyword-only params must be passed by keyword.

**Tests** (~6):
- `def f(a, *, b):` - b is keyword-only
- `def f(a, *, b, c=3):` - b required keyword, c optional keyword
- Error: `f(1, 2)` when b is keyword-only

#### E.3 Decorator Application in CodeGen

**`src/CodeGen.cpp`** - in `visit(FunctionDecl&)`:
- After generating the function, apply decorators bottom-to-top
- Each decorator: `decorated_func = decorator(original_func)`
- Store the decorated function pointer under the original name

**Tests** (~4):
- Simple decorator: `@my_decorator def func:`
- Parameterized: `@cache(ttl=300) def func:`

**Phase E output:** Full Python parameter syntax. Decorators applied in codegen.

---

### Phase F: Match/Case (PEP 634)

#### F.1 Soft Keywords

`match` and `case` are identifiers that become keywords contextually:
- `match` at statement start followed by expression + `:` or `{`
- `case` at start of a match case followed by pattern + `:` or `{`

**Implementation:** Lexer emits as `IDENTIFIER`. Parser checks identifier text at statement position.

#### F.2 AST Nodes

**`include/dragon/AST.h`** - new nodes:

```cpp
class MatchStmt : public Stmt {
 std::unique_ptr<Expr> subject;
 std::vector<MatchCase> cases;
};

struct MatchCase {
 std::unique_ptr<Pattern> pattern;
 std::unique_ptr<Expr> guard; // optional "if" guard
 std::vector<std::unique_ptr<Stmt>> body;
};

// Pattern hierarchy
class Pattern : public ASTNode { ... };
class LiteralPattern : public Pattern { // case 42, case "hello"
 std::unique_ptr<Expr> value;
};
class CapturePattern : public Pattern { // case x (binds to name)
 std::string name;
};
class WildcardPattern : public Pattern {}; // case _
class SequencePattern : public Pattern { // case [a, b, c] or case (a, b)
 std::vector<std::unique_ptr<Pattern>> elements;
};
class MappingPattern : public Pattern { // case {"key": val}
 std::vector<std::pair<std::unique_ptr<Expr>, std::unique_ptr<Pattern>>> entries;
 std::unique_ptr<Pattern> rest; // **rest
};
class ClassPattern : public Pattern { // case Point(x=0, y=0)
 std::string className;
 std::vector<std::unique_ptr<Pattern>> positionalArgs;
 std::vector<std::pair<std::string, std::unique_ptr<Pattern>>> keywordArgs;
};
class OrPattern : public Pattern { // case 1 | 2 | 3
 std::vector<std::unique_ptr<Pattern>> alternatives;
};
class AsPattern : public Pattern { // case pattern as name
 std::unique_ptr<Pattern> pattern;
 std::string name;
};
class StarPattern : public Pattern { // case [first, *rest]
 std::string name; // empty for bare *
};
```

#### F.3 Parser

`parseMatchStmt`:
1. Consume `match` identifier at statement position
2. Parse subject expression
3. Expect `:` (.py) or `{` (.dr)
4. Parse cases until `DEDENT` (.py) or `}` (.dr)

`parseMatchCase`:
1. Consume `case` identifier
2. Parse pattern
3. Optionally parse `if guard_expr`
4. Parse body

Pattern sub-parsers handle each pattern type based on leading token.

#### F.4 CodeGen

Lower match/case to chained if/elif with destructuring:

```
match subject:
 case LiteralPattern(val): → if subject == val
 case CapturePattern(name): → name = subject; always matches
 case WildcardPattern: → always matches
 case SequencePattern([a,b]): → if len(subject)==2: a=subject[0]; b=subject[1]
 case MappingPattern({k:v}): → if k in subject: v=subject[k]
 case ClassPattern(Cls(x=p)): → if isinstance(subject, Cls): match p against subject.x
 case OrPattern(a|b): → try a, then try b
 case guard: → additional if-condition
```

#### F.5 Dragon .dr Syntax

```dragon
match command {
 case "quit" {
 exit
 }
 case ("go", direction) {
 move(direction)
 }
 case Point(x=0, y=0) {
 print("Origin")
 }
 case _ {
 pass
 }
}
```

**Tests** (~20): literal, capture, wildcard, sequence, mapping, class, or-pattern, as-pattern, star-pattern, guards, nested patterns.

**Phase F output:** Full PEP 634 match/case in both modes.

---

### Phase G: Builtins - Critical Functions

All implemented as runtime C functions in `lib/Runtime/runtime.cpp` + CodeGen dispatch in `src/CodeGen.cpp`.

#### G.1 Aggregate Functions

| Function | Runtime Signature | Notes |
|----------|------------------|-------|
| `min(a, b)` | `dragon_min_int(i64, i64)`, `dragon_min_float(f64, f64)` | Overloads for 2 args |
| `min(iterable)` | `dragon_min_list(DragonList*)` | Iterate and compare |
| `max(a, b)` | `dragon_max_int(i64, i64)`, `dragon_max_float(f64, f64)` | |
| `max(iterable)` | `dragon_max_list(DragonList*)` | |
| `sum(iterable)` | `dragon_sum_list(DragonList*, i64 start)` | Optional start value |
| `any(iterable)` | `dragon_any_list(DragonList*)` | Short-circuit |
| `all(iterable)` | `dragon_all_list(DragonList*)` | Short-circuit |

**Tests** (~10)

#### G.2 Iteration Helpers

| Function | Runtime Signature | Notes |
|----------|------------------|-------|
| `enumerate(iter, start=0)` | `dragon_enumerate(DragonList*, i64)` → `DragonList*` of tuples | Returns list of (i, elem) |
| `zip(a, b)` | `dragon_zip(DragonList*, DragonList*)` → `DragonList*` of tuples | Shortest length |
| `map(func, iter)` | CodeGen-level: emit loop calling func on each element | Function pointer in IR |
| `filter(func, iter)` | CodeGen-level: emit loop with conditional append | |
| `sorted(iter)` | `dragon_sorted(DragonList*)` → `DragonList*` | Returns new sorted list |
| `reversed(iter)` | `dragon_reversed(DragonList*)` → `DragonList*` | Returns new reversed list |
| `iter(obj)` | Returns object itself (lists are iterable) | |
| `next(iter, default)` | Index-based iteration with sentinel | |

**Tests** (~12)

#### G.3 Type Introspection

| Function | Runtime Implementation | Notes |
|----------|----------------------|-------|
| `isinstance(obj, type)` | Runtime type tag comparison | Classes get unique type IDs |
| `type(obj)` | Return type tag as string | `"int"`, `"str"`, `"list"`, etc. |
| `callable(obj)` | Check if function pointer | |
| `repr(obj)` | Type-dispatched repr | `repr(42)` → `"42"`, `repr("hi")` → `"'hi'"` |
| `hash(obj)` | `dragon_hash_int(i64)`, `dragon_hash_str(const char*)` | FNV-1a or similar |
| `id(obj)` | Return pointer value as int | |

**Tests** (~8)

#### G.4 Numeric Functions

| Function | Runtime Implementation |
|----------|----------------------|
| `ord(char)` | `dragon_ord(const char*)` → first byte as int |
| `chr(code)` | `dragon_chr(i64)` → single-char string |
| `round(n, d=0)` | `dragon_round(f64, i64)` |
| `pow(b, e, m=None)` | Extend existing `dragon_pow_int`; add 3-arg modular version |
| `divmod(a, b)` | `dragon_divmod(i64, i64)` → `DragonTuple*` of (quotient, remainder) |
| `hex(n)` | `dragon_hex(i64)` → `"0x..."` string |
| `oct(n)` | `dragon_oct(i64)` → `"0o..."` string |
| `bin(n)` | `dragon_bin(i64)` → `"0b..."` string |
| `format(val, spec)` | Reuse `dragon_format_*` from Phase D |

**Tests** (~10)

#### G.5 Constructor Builtins

| Function | Implementation |
|----------|---------------|
| `list(iterable)` | `dragon_list_from_iter(DragonList*)` - copy, or convert tuple/set/string to list |
| `dict(iterable)` | `dragon_dict_from_pairs(DragonList*)` - list of (key, value) tuples |
| `set(iterable)` | `dragon_set_from_iter(DragonList*)` - add all elements |
| `tuple(iterable)` | `dragon_tuple_from_iter(DragonList*)` - copy to tuple |
| `int(x, base=10)` | Extend existing - add string-with-base parsing |
| `float(x)` | Extend existing - handle string input |
| `str(x)` | Already implemented |
| `bool(x)` | Already implemented |
| `bytes(source)` | Deferred - needs bytes type |
| `complex(r, i)` | Deferred - needs complex type |
| `slice(start, stop, step)` | CodeGen-level: create range for slicing |

**Tests** (~8)

**Phase G output:** All critical Python builtins work.

---

### Phase H: Builtins - I/O, Class, Dynamic

#### H.1 File I/O

**`lib/Runtime/runtime.cpp`** - new `DragonFile` type wrapping `FILE*`:

| Function | Notes |
|----------|-------|
| `dragon_file_open(const char* name, const char* mode)` → `DragonFile*` | |
| `dragon_file_read(DragonFile*)` → `const char*` | Read entire file |
| `dragon_file_readline(DragonFile*)` → `const char*` | Read one line |
| `dragon_file_readlines(DragonFile*)` → `DragonList*` | List of lines |
| `dragon_file_write(DragonFile*, const char*)` | |
| `dragon_file_close(DragonFile*)` | |
| `dragon_file_is_closed(DragonFile*)` → `int64_t` | |

Context manager support: `with open("f") as f:` calls `open` and `close` automatically (WithStmt codegen).

**Tests** (~6)

#### H.2 Class Support Functions

| Function | Implementation |
|----------|---------------|
| `super` | CodeGen: in method body, resolve parent class vtable, call parent method |
| `property(fget, fset)` | Descriptor protocol - store getter/setter function pointers on class |
| `classmethod(func)` | Decorator that passes class instead of instance as first arg |
| `staticmethod(func)` | Decorator that omits self/cls |
| `getattr(obj, name, default)` | Runtime attribute table lookup (string → offset) |
| `setattr(obj, name, value)` | Runtime attribute table write |
| `hasattr(obj, name)` | Check attribute table |
| `delattr(obj, name)` | Remove from attribute table |
| `dir(obj)` | Return list of attribute names from table |
| `vars(obj)` | Return dict of attribute name→value |

**Runtime attribute tables:** Each class gets a `DragonAttrTable` mapping string names to byte offsets in the class struct. Generated at class creation time.

**Tests** (~10)

#### H.3 Dynamic Compilation (Bytecode Interpreter, NOT LLJIT)

**Architecture**: Two-tier approach:

**Tier 1 - Compile-time eval (zero cost):**
- If `eval("literal_expr")` has a string literal argument, evaluate at compile time
- Dragon parser + constant folder runs during compilation, result inlined as constant
- `eval("1 + 2")` → `3` (no runtime cost)

**Tier 2 - Bytecode interpreter (~50KB, ~10x native speed):**
- Simple stack-based bytecode VM embedded in runtime
- Opcodes: LOAD_CONST, LOAD_NAME, STORE_NAME, BINARY_OP, CALL_FUNCTION, etc. (~30 opcodes)
- `eval(dynamic_string)` → lex + parse + emit bytecode + interpret
- `exec(dynamic_string)` → same but for statements
- `compile(source, filename, mode)` → lex + parse + emit bytecode → return code object

**Why NOT LLJIT**: 20-100ms per function compilation, tens of MB binary bloat. Julia spent 10+ years fighting this. A bytecode interpreter at ~10x slowdown is the right tradeoff for inherently-dynamic code.

**MIR JIT as stretch goal**: ~75µs/function, 175KB, 94% of GCC -O2 speed. If bytecode interpreter becomes a bottleneck for specific workloads, MIR is the upgrade path - NOT LLJIT.

**Tests** (~6):
- `eval("1 + 2")` → 3 (compile-time path)
- `s = "len('hello')"; eval(s)` → 5 (runtime bytecode path)
- `exec("x = 42"); print(x)` → 42
- `code = compile("x + 1", "<string>", "eval"); eval(code)` → uses code object

**Phase H output:** File I/O, class introspection, dynamic compilation foundation.

---

### Phase I: Remaining Syntax

#### I.1 Type Alias Statement (PEP 695)

**Lexer/Parser:** `type` as soft keyword at statement position.
```python
type Point = tuple[int, int]
type Matrix[T] = list[list[T]]
```

**`include/dragon/AST.h`** - add `TypeAliasStmt`:
```cpp
class TypeAliasStmt : public Stmt {
 std::string name;
 std::vector<std::string> typeParams; // generic params
 std::unique_ptr<TypeExpr> value;
};
```

**CodeGen:** No-op (type aliases are compile-time only). TypeChecker registers alias.

**Tests** (~6)

#### I.2 Exception Groups (PEP 654)

**`include/dragon/AST.h`** - add `isStar: bool` to `TryStmt::ExceptHandler`.

**Parser:** Recognize `except*` (or `catch*` in .dr mode).

**CodeGen:** Iterate over exception group members, match each handler.

**Tests** (~4)

#### I.3 Class Keyword Arguments

Already in AST (`ClassDecl::keywords`). Wire through to CodeGen - store for metaclass/other keyword processing.

**Tests** (~4)

#### I.4 Multiple Inheritance

Already parsed. CodeGen needs C3 linearization for method resolution order (MRO).

**Tests** (~6)

#### I.5 Async/Await

State machine transformation for coroutines (Rust approach - zero-cost, no JIT):
- `async def` → generate state machine struct with resume function
- `await expr` → yield point: save state, return Future, resume on completion
- Runtime event loop: epoll-based (Linux), single-threaded with cooperative scheduling
- `asyncio.run`, `asyncio.gather`, `asyncio.sleep` - minimal asyncio compatibility

**Why state machine (not stackful coroutines):**
- Zero heap allocation for simple coroutines
- Compiler knows exact state size → struct allocated on stack
- Proven by Rust: zero-cost async without garbage collector

**Phase I output:** Type aliases, exception groups, class extensions, async/await.

---

### Phase J: Dual-Mode Check:

For every new construct, verify both `.dr` (brace) and `.py` (indent) modes:

| Feature | Python (.py) | Dragon (.dr) |
|---------|-------------|-------------|
| match/case | `match x:` + indent | `match x {` ... `}` |
| case body | `case pat:` + indent | `case pat {` ... `}` |
| type alias | `type X = Y` | `type X = Y` (same) |
| except* | `except* Group:` + indent | `catch* Group {` ... `}` |
| tuple unpack | `a, b = 1, 2` | `a, b = 1, 2` (same) |
| comprehensions | `{x for x in y}` | `{x for x in y}` (same) |
| walrus | `(n := len(a))` | `(n := len(a))` (same) |
| chained comp | `a < b < c` | `a < b < c` (same) |

Most expression-level features are mode-independent. Statement-level features need brace/indent variants.

**Tests** (~20): Dual-mode parser tests for each new statement type.

**Phase J output:** Full dual-mode coverage verified.

---

## Python 3.12 Grammar Checklist

### Statements
- [x] `simple_stmt` (expr, assign, aug_assign, ann_assign, return, raise, break, continue, pass, del, assert, global, nonlocal, import, from_import)
- [ ] `type_stmt` (PEP 695) - Phase I
- [x] `compound_stmt` (if, while, for, try, with, def, class)
- [ ] `match_stmt` - Phase F
- [x] `decorated` (functions and classes with @decorator)

### Expressions
- [x] `assignment_expression` (walrus `:=`) - lexed; Phase D for parser + codegen
- [x] `star_expression` (`*expr`)
- [ ] `double_star_expression` (`**expr` in calls) - Phase B
- [x] `comparison` - Phase D fixes chained semantics
- [x] `lambda_def`
- [x] `conditional_expression` (ternary)
- [x] `list_comp`, `dict_comp`
- [ ] `set_comp` - Phase C
- [ ] `generator_exp` - Phase C
- [x] `yield_expr` (parsed)

### Parameters
- [x] `param_with_default`
- [ ] `slash_separator` (`/`) - Phase E
- [ ] `star_separator` (bare `*`) - Phase E
- [x] `star_args` (`*args`), `kwargs` (`**kwargs`)

### Patterns (PEP 634)
- [ ] All pattern types - Phase F

### Builtins (68 total)
- [x] print, len, int, float, str, bool, input, abs, range (9)
- [ ] 59 remaining - Phases G, H

---

## Files Modified

| File | Phases | Changes |
|------|--------|---------|
| `include/dragon/AST.h` | A-I | ~15 new/modified node types |
| `include/dragon/Token.h` | D | Soft keyword support if needed |
| `src/Lexer.cpp` | F | Soft keyword context for match/case |
| `src/Parser.cpp` | B-F, I | Unpacking, comprehensions, match/case, params, chained comp, walrus, type alias |
| `src/Sema.cpp` | A-I | New node visitors |
| `src/TypeChecker.cpp` | A-I | New node type checking, param enforcement |
| `src/CodeGen.cpp` | A-I | All new codegen visitors, builtin dispatch, f-string rewrite |
| `lib/Runtime/runtime.cpp` | A, G, H | Tuple type, set type, ~50+ new functions |
| `test/CodeGenTest.cpp` | A-I | ~150 new tests |
| `test/ParserTest.cpp` | B-F, I | ~60 new tests |
| `decisions/002-syntax-parity.md` | - | This document |

## Implementation Order

**Revised: "Eat the frog" - hard things first, building blocks early.**

**A → B → C → D → E → F → G → H → I → J**

Each phase is testable on its own (build + all tests pass). Phase A is prerequisite for B, C, G (tuples/sets needed for unpacking, comprehensions, builtins). All other phases are independent or only depend on earlier phases.

**Priority notes:**
- Phase A is first because tuples/sets are prerequisites for nearly everything else
- Async/await (I.5) and dynamic features (H.3) use zero-cost approaches (state machines, bytecode interpreter) - not LLJIT
- The three-tier dict system (TypedDict→struct, homogeneous, tagged union) is implemented incrementally across phases A.4 and G

## Check:

1. **Per-phase:** `cmake --build build && ctest --test-dir build` - all suites pass
2. **After Phase F:** Parse 100 random CPython `Lib/*.py` files - 0 syntax errors
3. **After Phase J:** Parse `requests`, `flask`, `click` source files - 0 syntax errors
4. **After Phase J:** Full Python 3.12 grammar coverage confirmed via checklist above
5. **Projected test count after full parity:** ~600+ (current ~400 + ~200 new)
