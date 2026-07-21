# Decision 032: Database Stdlib - `template[SQL]` + Bundled Wire-Protocol Drivers

**Status:** **Done** - all phases implemented & tested. Phase 0 (constant-folded `template[SQL]`) + Phase 1 (SQLite) shipped; Phases 2-5 (Postgres + MySQL wire-protocol drivers, TLS, connection pool) shipped. See revision history. Design revised for constant-folded `template[SQL]` lowering and for rows as `dict[str, Any]`, not a `Row` class.

Every serious language in 2026 needs Postgres/MySQL/SQLite without "first install libpq." I wanted pure-Dragon wire-protocol drivers over our raw sockets, `template[SQL]` as the query primitive (no string-concat injection surface), and one binary that talks to all three. Took longer than I hoped. MySQL binary prepared statements alone ate a week. But it's done.

## Revision history

- **Surface amendment.** The user-facing query surface is renamed and extended: `query`/`one`/`scalar`/`exec`/`exec_raw` + `ExecResult` become `run` (the primitive) + `all`/`one`/`val` read sugar + `runs`/`raw`, all returning/operating on a `Results` object carrying `xid`/`ran`; `transaction`/`commit`/`rollback` are added; each fetch verb takes an optional `[T]` (bare → `dict[str, Any]`, `[T]` → `T(**row)` via D047, with the generic `[T]` forms gated on D044). **The engine below the surface - constant-folded `template[SQL]`, the interned-canonical prepared-statement cache, native-typed param binding, `dict[str, Any]` rows, the `@register_driver` seam, TLS, pooling, and the error model - is unchanged.** The §"The user-facing API", §"Row shape" scalar references, the `Conn` verb names in §"Driver interface", and the surface clause of §"Decision" below are superseded by D049; everything else in this ADR stands. See `decisions/049-database-surface-run-all-one-val.md`.

- **Phases 2-5 shipped; ADR Done.** The remaining backends and transport landed:
 - **Module layout (acyclic).** `stdlib/database/base.dr` holds the shared core (abstract `Connection`, the error hierarchy, the `SQL` content type, `ExecResult`); `sqlite.dr` / `postgres.dr` / `mysql.dr` each subclass `Connection`; the package root `database.dr` re-exports the core, imports the drivers, and owns `open(dsn)`. Circular imports are rejected by the resolver, so the base **must** sit below the drivers. The base's `one`/`scalar`/`with`-protocol are written once on the per-backend `query`/`exec`/`exec_raw` primitives and reach the concrete driver via the vtable.
 - **Postgres (Phase 2)** - full v3 wire protocol over `socket.TcpStream`: StartupMessage + SCRAM-SHA-256 mutual auth, extended-query Parse/Bind/Execute/Sync with TEXT-format `template[SQL]` params (`$$N`→`$N+1`), oid→Dragon type rows.
 - **MySQL (Phase 3)** - client/server protocol: handshake with `mysql_native_password` (SHA1) + `caching_sha2_password` fast path (SHA256) + the mid-handshake auth switch; **binary**-protocol prepared statements (COM_STMT_PREPARE/EXECUTE/CLOSE, `$$N`→`?`); binary result rows decoded via `stdlib/struct.dr` (FLOAT/DOUBLE arrive as raw little-endian IEEE-754 - there is no text option, so this needed the new `__float_bits`/`__float32_bits` reinterpret intrinsics). `caching_sha2` *full* auth (cold cache) needs RSA or TLS and raises a clear error; native_password and the fast path are covered.
 - **TLS (Phase 4)** - `?sslmode=disable|prefer|require` on the Postgres DSN; `prefer`/`require` send the SSLRequest (Int32 8 + magic 80877103) and, on `'S'`, wrap the socket via `ssl.wrap_socket`, routing all subsequent framing through the TLS conn. `prefer` (default) falls back to plaintext on `'N'`; `require` fails.
 - **Pool (Phase 5)** - `database.Pool(dsn, max_idle)` with `acquire`/`release`/`close`, guarded by a `threading.Lock` (never held across a connect or query), reusing idle connections and closing on overflow so the idle set stays bounded.
 - **Tests:** `test/dr/test_database.dr` (SQLite + pool), `test/dr/test_postgres.dr` (live, self-gating: auth/params/types/injection + TLS + pool), `test/dr/test_mysql.dr` (live, self-gating: binary prepared statements + decode), `test/dr/test_struct.dr` (the float-bits primitive). Landing this required fixing several root-cause compiler bugs (heap-reassign VarKind misinference; static-method-factory field class; unannotated-procedure `void` return type; module-qualified variadic-call packing; `varClassNames` name-key pollution) - none worked around.

- **Row shape and early phases.** Two implementation decisions settled while building Phases 0-1:
 - **Result rows are `dict[str, Any]`** (column name → native value), *not* a wrapper `Row` class. The compiler's `**`-spread lowers `Customer(**row)` to a single bulk `dragon_dict_copy` whose fast path keys on a *dict* operand; a `Row` *class instance* passed there is misread as a `DragonDict` and crashes. Making rows dicts makes the typed-conversion gate a single bulk copy (faster than a per-column `keys`/`__getitem__` protocol over a wrapper - commandment #1) and gives `row["name"]` + `row.name` (Dragon dict dot-access) for free. Cost: no positional `row[0]` (a dict is name-keyed; use `db.scalar` for a one-row first column). The `### Row shape` section below reflects this.
 - **Content-type domain co-location.** The `SQL` content type lives in **`stdlib/database/database.dr`** (imported `from database import SQL`), not a separate `sql_template.dr`, under a project-wide rule: each `template[X]` content type ships from the module that owns its domain - `HTML`/`CSS`/`XML`→`html`, `JSON`→`json`, `URL`→`urllib.parse`, `SQL`→`database`; the `Template` base stays in `template`. Mirrors CPython's module layout (`html`, `json`, `urllib`).
 - **Call-site spread is scoped to TypedDict construction.** `T(**dict)` is implemented (the gate); general `f(*list)`/`f(**dict)` into arbitrary callables raises an explicit "not yet implemented" error rather than failing silently.
 - **Shipped & tested:** Phase 0 (`template[SQL]` codegen) + Phase 1 (SQLite driver: `query`/`one`/`scalar`/`exec`/`exec_raw`, native-typed param binding, injection safety, `Customer(**row)`). Covered by `CodeGenTemplateTest` (SQL lowering/interning) and `InteropTest.Database*` (E2E). Phases 2+ unchanged.

- **Constant-folded `template[SQL]` lowering.** The original design (§"`template[SQL]` lowering - the core mechanism") routed every query through a runtime `SQL.build(parts: list[TemplatePart])` call that assembled the canonical text, param list, and type tags on each construction. Revised on speed grounds (commandment #1): the *invariant half* of a `template[SQL]` site - the `$$N` canonical text and its FNV-1a hash - is known at compile time and is now **constant-folded into an interned canonical string literal (`.rodata`) plus a compile-time hash constant**, deduplicated across structurally identical sites. Per-param types and arity are not stored statically - they ride in the `params` box tags, so the `SQL` value is just `{canonical, hash, params}` (no descriptor struct, no FFI accessors; `str`/`list` fields are GC-managed). The only IR emitted per query is the native-typed parameter binding. The prepared-statement cache keys on the canonical *pointer* (interning shares one global across identical sites; the precomputed hash pre-seeds the bucket), so steady-state query text is built and hashed exactly zero times after compile. `build(parts)` is retained as the runtime fallback signal for user-defined `template[DSL]` content types the compiler does not fold. The lowering, `Stmt`, placeholder-rewrite, and Phase 0 sections below reflect the revised design.

## Summary

`stdlib/database/` gives you Postgres, MySQL, and SQLite with zero install steps. Two ideas do most of the work:

1. **`template[SQL]` is the query primitive.** A query is not a string. It's a `template[SQL] { ... }` block whose `!{expr}` interpolations lower to real prepared-statement parameters (`$1, $2, ...` for Postgres; `?` for MySQL/SQLite). No injection surface, query-plan caching for free, native types end-to-end.
2. **Pure-Dragon wire-protocol drivers, statically linked.** Postgres and MySQL clients are `.dr` over our raw epoll/kqueue sockets, not `libpq` / `libmysqlclient`. SQLite uses the bundled C library. TLS comes from **`stdlib/ssl.dr`** . `dragon build` gives you one binary that talks to all three, encrypted, without the user installing anything.

The query API stays small: `db.query`, `db.one`, `db.scalar`, `db.exec`. Each takes a `template[SQL]` and returns plain rows / scalars / `ExecResult`. Typed conversion is opt-in: `customer: Customer = Customer(**row)`. No generic call syntax, no compiler magic for row mapping.

I rejected PEP-249-per-driver (every driver re-implementing pooling, tx, prepared statements) and rejected shipping `libpq`/`libmysqlclient` as native deps. Both could show up later as extra backends behind the same API; neither is required for v1.

## Motivation

Users expect this from a typed compiled language in 2026:

- **No "go install a driver" step.** Run `dragon run app.dr`, connect to Postgres. Done. That's the bar we want to acheive.
- **No string concatenation, ever.** Dynamic queries compose without building a `str` that touches user input.
- **No positional parameter counting.** `$1` / `?` placeholders are a 1990s artifact; the binding *is* the interpolation.
- **No function coloring.** A query in a `fire`'d vthread doesn't block the OS thread; I/O yields cooperatively .
- **TLS on by default.** Cloud Postgres and managed MySQL are TLS-only; a stdlib that can't talk to them out of the box is a non-starter.

Python's PEP-249, Go's `database/sql`, and Java's JDBC each solve part of this. Dragon can hit all of it because we already have:

1. `template[X]` blocks with compile-time content-type safety .
2. TypedDict with dot-access .
3. Raw epoll/kqueue async I/O with vthread yielding .
4. Static linking of bundled libraries (DRAGON_SQLITE3_LIB pattern).

What's missing is a thin `database/` stdlib that wires these together.

## Goals

1. **Zero-install for users.** `import database.postgres` works on a fresh machine. No `apt install libpq-dev`, no `pip install psycopg2`, no DLL hunting.
2. **`template[SQL]` is the query DSL.** Queries are typed expressions, not strings. `!{expr}` becomes a prepared-statement parameter - never string-concat.
3. **Cross-database identical surface.** SQLite, Postgres, MySQL, and future drivers expose the same `Pool` / `Conn` / `Tx` / `Rows` API. Switching DSN switches drivers.
4. **Static typing where it matters, opt-in.** `db.query(sql)` returns `list[Row]`. Users opt into typed rows the ordinary Dragon way: `customers: list[Customer] = [Customer(**r) for r in rows]`. No generic-call syntax, no compiler-special row mapping. The TypeChecker handles it through existing TypedDict construction.
5. **Speed.** Native-typed parameters flow into wire protocols at their LLVM types - no boxing, no `Any` round-trip. Pooling and prepared-statement caching live in the core, not per-driver.
6. **Concurrency-correct.** Every blocking syscall yields the current vthread. A pool with N connections + 10,000 vthreads behaves like Go.
7. **Encrypted by default.** TLS via Dragon's pure-Dragon `stdlib/ssl.dr`, zero user install. Cloud Postgres (Neon, Supabase, RDS, Cloud SQL) and any production MySQL deployment work out of the box once Milestone A lands; `?sslmode=require` (or `disable`) on the DSN is the only knob most users touch.

## Non-Goals (v1)

- **An ORM.** Dragon ships query primitives, not a relational mapper. ORMs can be built on top; the stdlib stays at the query layer.
- **Migrations.** A schema-migration tool is a separate stdlib (`stdlib/database/migrate.dr`) and out of scope here.
- **Streaming `COPY` / `LOAD DATA`.** Bulk-load endpoints are reachable via `Conn.exec_raw` but a typed streaming API can land in v2.
- **Stored-procedure result-set introspection.** `OUT` params, multi-resultset stored procs, and pg `RETURNING` chaining are reachable via raw cursors; the surface is intentionally minimal.
- **Async-only or sync-only.** Dragon's colorless await means there is no split: every method works in any context.
- **Compile-time column/projection checking against the row type.** `db.query` returns generic `Row`. If you want a `Customer`, you construct one (`Customer(**r)`) - the same TypedDict-construction validation that fires anywhere else in Dragon. Compile-time SQL-vs-schema checking would require live-DB introspection (sqlx-style) which we explicitly reject as a build-time dependency.
- **Bundling proprietary / enterprise drivers.** Oracle (TNS/Net8), SQL Server (TDS), Snowflake, ClickHouse, DB2, etc. are *not* shipped in-tree. They are third-party `dragon egg` packages registering through the same `@register_driver` seam - see *Driver distribution & extensibility*. Core maintains only the open-protocol, near-universal drivers (SQLite, Postgres, MySQL).

## Options Considered

### Option A - PEP-249 per driver

Each driver implements its own `connect`, `cursor`, `execute`, parameter binding, and pooling.

- Every driver re-invents pooling, prepared-statement caching, transaction state. N× the code, N× the bugs.
- No compile-time SQL safety; injection is prevented by convention.
- Ignores `template[SQL]` entirely.

### Option B - `database/sql` style (Go)

Thin generic core with a `Driver` interface, drivers register, `sql.Open("postgres", dsn)` returns a `*DB`. Pool, Tx, prepared-statements live in the core.

- Right architecture. Wrong DX. `db.Query("select ... where id = $1", id)` still has positional placeholders and string-typed queries.

### Option C - Generic `query[T](sql)` with compiler-managed row mapping

Earlier draft of this ADR. Rejected for over-engineering: requires `func[T](...)` generic-call syntax (not in Dragon), opens a `SELECT *` runtime-mapping question, and adds a `rowmap.dr` module - none of which are needed when typed conversion is just `Customer(**r)`.

### Option D - `template[SQL]` + minimal API + ordinary TypedDict (chosen)

Take the Go-style core (Pool, Tx, prepared statements, generic driver interface), keep its architectural strengths, and replace the string query with `template[SQL]`. Keep the API tiny: `query`, `one`, `scalar`, `exec`. Typed conversion is `Customer(**row)` - ordinary Dragon, no compiler magic, no new syntax. Bundle the drivers as pure Dragon over raw sockets so the user never installs anything. TLS comes from Dragon's pure-Dragon `stdlib/ssl.dr` - the driver wraps the upgraded fd in `ssl.wrap_socket`.

- Reuses (template[SQL] is a built-in content type - Phase 4).
- Reuses TypedDict for *opt-in* typed conversion.
- Reuses raw I/O + vthread yielding .
- One core, N thin drivers (~1.5-2 KLOC each).
- Compile-time content-type safety: `SQL` ≠ `HTML` ≠ `str`.
- No new language feature beyond Phase 4.

## Design

### Layered architecture

```
┌──────────────────────────────────────────────────────────┐
│ user code: rows = db.query(template[SQL] { ... }) │
│ customers = [Customer(**r) for r in rows] │
├──────────────────────────────────────────────────────────┤
│ stdlib/database/database.dr │
│ Pool, Conn, Tx, Rows, Row, open, driver registry │
│ SQL template lowering: !{expr} → param slot │
├──────────────────────────────────────────────────────────┤
│ stdlib/database/{sqlite,postgres,mysql}.dr │
│ Driver implementations │
│ Wire protocol (postgres/mysql) or C FFI (sqlite) │
├──────────────────────────────────────────────────────────┤
│ stdlib/ssl.dr : pure-Dragon TLS - wrap_socket │
├──────────────────────────────────────────────────────────┤
│ raw I/O : epoll/kqueue + dragon_vthread_io_yield │
│ C FFI : sqlite3_* (statically linked) │
└──────────────────────────────────────────────────────────┘
```

### `template[SQL]` lowering - the core mechanism

 Phase 4 defines `template[X]` content types where each `!{expr}` is passed through `X.escape(s) -> str`. For `SQL`, escape-then-concat is *the wrong primitive* - it gives you injection-safe strings, not prepared statements.

`SQL` opts into **parameter-extraction lowering** instead of escape-and-concat. A `Template` subclass signals this by declaring `build`:

```dragon
class Template {
 @staticmethod
 def escape(s: str) -> str { return s } // existing - used by HTML, CSS, etc.

 @staticmethod
 def build(parts: list[TemplatePart]) -> Template { ... } // opt-in signal
}

class TemplatePart {
 is_literal: bool // true for raw text segments, false for !{expr} slots
 text: str // literal segment (when is_literal)
 value: Any // bound value (when !is_literal)
 value_type: str // dragon type tag for the value (int/str/float/.../SQL)
}
```

Declaring `build` switches the site away from escape-and-concat. For the built-in `SQL` type the compiler performs the lowering **directly as a codegen primitive** - the constant-folded path below - and falls back to a runtime `build(parts)` call only for user-defined DSL content types it does not special-case.

**Constant-folded invariant half.** The invariant half of a `template[SQL]` site - the `$$N` canonical text and its hash - is fully determined at compile time. The compiler emits the canonical as an **interned string literal** in `.rodata` (one global per unique canonical text, shared across structurally identical sites) and the hash as an **i64 constant**. Per-param types and arity are *not* stored statically - they ride along in the `params` box tags (each `Any` box is `{tag, native-payload}`), so the invariant half is just `(canonical_literal*, hash_const)`. No descriptor struct, no static `param_types` array. The runtime `SQL` value carries those two plus the bound values:

```dragon
class SQL(Template) {
 canonical: str // "select * from t where id = $$0 and name = $$1"
 // - interned compile-time literal (.rodata)
 hash: int // FNV-1a of canonical, a compile-time constant
 params: list[Any] // bound values, native-typed (tagged box, never stringified)
 // nparams and per-param types are implicit in `params` (len + box tags);
 // nothing here is rebuilt or re-hashed per query
}
```

So `template[SQL] { select * from t where id = !{user_id} and name = !{name} }` lowers to:

- **compile time** (emitted once, interned): `@.sql.N = c"select * from t where id = $$0 and name = $$1"` and the constant `hash = <fnv1a>`
- **per call** (the only IR at the site): `params = [box(user_id), box(name)]` - native-typed boxes, no stringify - then `SQL { @.sql.N, hash, params }`

The dialect rewrite (`$$N` → `$1` for postgres, `?` for mysql/sqlite) is performed once per (driver, canonical) pair at `Conn.prepare` and cached. The cache keys on the canonical *pointer* - interning makes structurally identical sites share one `.rodata` global, and the precomputed `hash` pre-seeds the bucket - so a steady-state `prepare` is a pointer compare, and canonical text is built and hashed exactly zero times after compile. The user never sees placeholders.

Dynamic composition still works because `SQL` is a value:

```dragon
filters: SQL = template[SQL] {
 !{ if status { :{ and status = !{status} } } }
 !{ if since { :{ and created_at >= !{since} } } }
}
rows = db.query(template[SQL] {
 select * from customers where 1=1 !{filters}
})
```

When a `SQL` value is interpolated into another `SQL` template, the outer site is *not* fully foldable - the inner SQL's text and arity are runtime. The compiler emits a **runtime splice**: it stitches the static outer fragments with the inner SQL's `canonical`, *renumbering* the inner `$$N` by the running parameter count and *appending* the inner `params` - never converting either to strings. The spliced canonical is built and hashed once, producing a heap `str` (the composed `SQL` is otherwise an ordinary `{canonical, hash, params}` value). This is the only path that touches query-text assembly at runtime, and only when you actually compose dynamically; the scalar-only case stays fully folded. The compile-time guarantee that user input cannot escape into the query text holds at any depth - carried by the type, not the runtime.

### The user-facing API

```dragon
import database
import database.postgres // side-effect: registers the driver

class Customer(TypedDict) {
 id: int
 name: str
 email: str
 signup: datetime
}

// Pool - opens lazily, sized from DSN query string (?pool=10)
db = database.open("postgres://localhost/shop?pool=10")

// query - returns list[Row]. Row is dict-like: r["name"], r.name, r[0] all work.
rows = db.query(template[SQL] {
 select id, name, email, signup
 from customers
 where id > !{min_id}
})
for r in rows {
 print(r["name"], r.email) // dict + dot access
}

// Typed view - opt-in, ordinary Dragon
customers: list[Customer] = [Customer(**r) for r in rows]

// one - returns single Row; raises NoRows / MultipleRows on miscount
row = db.one(template[SQL] {
 select id, name, email, signup from customers where id = !{user_id}
})
me: Customer = Customer(**row)

// scalar - returns a single value, typed by the caller's annotation
n: int = db.scalar(template[SQL] { select count(*) from customers })

// exec - for INSERT/UPDATE/DELETE; returns ExecResult { rows_affected, last_insert_id }
res = db.exec(template[SQL] {
 insert into customers(name, email) values(!{name}, !{email})
})

// Transactions
with db.tx as tx {
 tx.exec(template[SQL] { update accounts set bal = bal - !{amt} where id = !{a} })
 tx.exec(template[SQL] { update accounts set bal = bal + !{amt} where id = !{b} })
 // commit on normal exit; rollback on exception
}

// Manual transaction control if you need savepoints, isolation levels, etc.
tx = db.begin(isolation="serializable")
try {
 tx.exec(...)
 tx.commit
} except Exception as e {
 tx.rollback
 raise
}

// Raw access (escape hatch - not preferred)
db.exec_raw("vacuum analyze customers") // no template, no params, runs verbatim
```

### Row shape - `dict[str, Any]` (Option A)

A result row **is a `dict[str, Any]`** keyed by column name, with values at their native types (the dict tags each on insert). There is no `Row` wrapper class. `db.query` returns `list[dict[str, Any]]`; `db.one` returns `dict[str, Any]`.

```dragon
row: dict[str, Any] = db.one(template[SQL] { select id, name, email from customers where id = !{user_id} })
print(row["name"]) // subscript by column name
print(row.name) // Dragon dict identifier dot-access - same value
customer: Customer = Customer(**row) // typed conversion (bulk dragon_dict_copy)
```

**Why a dict, not a wrapper class.** The compiler lowers `Customer(**row)` to a single bulk `dragon_dict_copy` whose fast path requires the `**` operand to *be* a dict; a wrapper-class instance passed there would be misread as a `DragonDict`. A dict makes the gate one bulk copy (faster than a per-column `keys`/`__getitem__` protocol over a wrapper - commandment #1) and yields `row["name"]` + `row.name` for free. **Trade-off:** no positional `row[0]` (a dict is name-keyed) - for the first column of a single-row result use `db.scalar(...)`. `Customer(**row)` is the same `**`-spread protocol Python uses for `**dict`; TypedDict's construction validates field presence/types. TypedDict is **dict-backed**, so the conversion is a copy; the representation trade-off - and why TypedDict is not (yet) lowered to a struct - is recorded in the native extension FFI ADR (TypedDict struct lowering) and the typed collection access ADR § TypedDict.

### Driver interface

```dragon
class Driver {
 name: str // "postgres", "mysql", "sqlite"
 placeholder_style: str // "numbered" ($1) or "qmark" (?)

 def open(dsn: str) -> Conn { ... } // opens one physical connection
}

class Conn {
 def prepare(sql: SQL) -> Stmt { ... } // driver caches by canonical pointer
 def exec(stmt: Stmt, sql: SQL) -> ExecResult { ... }
 def query(stmt: Stmt, sql: SQL) -> Rows { ... }
 def begin(opts: TxOptions) -> None { ... }
 def commit -> None { ... }
 def rollback -> None { ... }
 def close -> None { ... }
}

class Stmt {
 canonical: str // dragon's $$N form - the cache key (interned pointer identity)
 hash: int // precomputed FNV-1a, pre-seeds the cache bucket
 dialect_text: str // driver-specific ($1 or ?), rewritten once at prepare
 handle: ptr // driver-specific
 column_names: list[str] // populated at prepare
 column_types: list[str]
}

class Rows {
 column_names: list[str]
 column_types: list[str]
 def next -> bool { ... }
 def get(i: int) -> Any { ... }
 def close -> None { ... }
}
```

`Pool` (in the core) wraps `Conn`s, manages checkout/checkin, runs healthchecks, and recycles. `Tx` wraps a checked-out `Conn` and pins it for the transaction's lifetime.

### Driver registration

```dragon
// stdlib/database/postgres.dr
import database

@database.register_driver("postgres", "postgresql")
class PostgresDriver(database.Driver) {
 placeholder_style: str = "numbered"
 def open(dsn: str) -> database.Conn { ... }
}
```

`database.open("postgres://...")` reads the scheme, looks up the registered driver, calls `driver.open(dsn)`, and wraps in a `Pool`. Importing `database.postgres` (or `database.mysql`, etc.) is enough to register.

### Bundled drivers

| Driver | Implementation | LOC (Dragon) | Linked |
|---|---|---|---|
| `database.sqlite` | C FFI to `sqlite3.c` (already bundled) | ~400 | `DRAGON_SQLITE3_LIB` |
| `database.postgres` | Pure Dragon over raw TCP/Unix socket; FE/BE protocol v3; TLS via `stdlib/ssl.dr` | ~1800 | - (no native TLS lib) |
| `database.mysql` | Pure Dragon over raw TCP/Unix socket; binary protocol; TLS via `stdlib/ssl.dr` | ~2100 | - (no native TLS lib) |

Wire-protocol implementations cover: startup/auth (md5, scram-sha-256, mysql_native_password, caching_sha2_password), simple+extended query, prepared statements, type oid → dragon type, COPY for postgres bulk insert (later), error codes, notice/warning streams. TLS is wired in at the socket layer, so the protocol code is identical for plaintext and encrypted connections.

Every blocking socket op routes through the existing `dragon_vthread_io_yield(fd, READ|WRITE, timeout_ms)` helper. A 10K-connection benchmark with 10K vthreads issuing concurrent queries against 10 pool connections should behave correctly with no OS-thread blocking.

### Driver distribution & extensibility

Two tiers, one registry. The `@register_driver` seam (above) is the *only* integration point a driver needs, so stdlib drivers and third-party drivers are indistinguishable to user code - `database.open(dsn)` resolves a DSN scheme to whichever driver registered it. The split is about *who maintains and ships* a driver (seperate maintainer, same API), never about what user code can do.

**Tier 1 - stdlib (in-tree).** SQLite, Postgres, MySQL. Open wire protocols, near-universal, stable. They ship in `stdlib/database/` and uphold Goal 1 (zero-install): `import database.postgres` works on a fresh machine with no fetch step. I ship them in-tree.

**Tier 2 - eggs (third-party).** Everything else - Oracle (TNS/Net8), SQL Server (TDS), CockroachDB, ClickHouse, Snowflake, DB2 - is a package distributed via `dragon egg`, not bundled. Same reasoning applied to TLS: keep the source tree lean, don't carry code most users never touch. Core will not carry proprietary/vendor-specific, lower-frequency drivers in-tree. A third-party driver is an ordinary Dragon package that imports `database` and registers itself:

```dragon
// dragon-oracle/oracle.dr (published as an egg)
import database

@database.register_driver("oracle")
class OracleDriver(database.Driver) {
 placeholder_style: str = "numbered_colon" // :1, :2, ...
 def open(dsn: str) -> database.Conn { ... } // TNS handshake, auth, wire I/O - all in .dr
}
```

```dragon
// consumer dragon.drs (manifest - content-addressed, hash-pinned)
[dependencies]
oracle = { egg = "dragon-oracle", version = "1.x", hash = "sha256:..." }
```

```dragon
// consumer code - identical surface; only the import line and DSN scheme differ
import database
import oracle // side-effect: registers "oracle"
db = database.open("oracle://app:pw@host:1521/ORCLPDB1?sslmode=verify-full")
rows = db.query(template[SQL] {
 select id, name from customers where id > !{min_id} fetch first !{n} rows only
})
customers: list[Customer] = [Customer(**r) for r in rows]
```

**Why third-party drivers are safe here.** The injection-safety guarantee does *not* live in the driver. `template[SQL]` lowering, parameter binding, placeholder renumbering, pooling, transactions, and prepared-statement caching all live in the **core** (`database.dr`). A driver only (a) rewrites `$$N` → its dialect placeholder and (b) marshals native-typed params onto the wire. It never receives a query string assembled from user input - it receives `(canonical, params)` where each param is a native-typed box carrying its own type tag - so it *cannot* reintroduce a string-concat injection path. This is the structural difference from PEP-249, where every driver reimplements binding/pooling/tx and can each get them wrong.

**Trust model (via).** A driver egg declares its capabilities in `dragon.drs` - `network` for wire-protocol drivers, `ffi` for C-backed drivers (e.g. an ODBC-bridged MSSQL driver) - and the consuming project opts in explicitly. Drivers are not privileged: an egg cannot open a socket or call FFI unless the consumer granted that capability, and the content-addressed hash pins exactly what is fetched.

**Conformance kit.** `stdlib/database/conformance.dr` ships the behavioral matrix the in-tree drivers pass: placeholder rewrite, native type round-trips, tx commit/rollback, `NoRows`/`MultipleRows`, pool checkout/checkin. A third-party driver runs the same suite; passing it is the bar for a "verified" listing in the registry. The bundled SQLite/Postgres/MySQL drivers are the reference implementations.

**Interface stability - deferred on purpose.** The `Driver` / `Conn` / `Stmt` / `Rows` interface is *documented but not yet a frozen public ABI* in v1. It is validated against the three in-tree drivers first - deliberately spanning the two hard axes (C-FFI vs. raw-socket transport; numbered `$1` vs. qmark `?` placeholders) - and only blessed as a stable third-party contract in a follow-up ADR, once an out-of-tree driver (Oracle or MSSQL) has stress-tested its generality. This avoids the `database/sql/driver` trap, where Go froze a too-narrow interface and had to bolt on optional `Queryer` / `QueryerContext` / `SessionResetter` interfaces it could never remove. Until then, an out-of-tree driver is supported but pinned to a specific Dragon version range it was tested against.

### TLS

TLS is required for cloud databases. Neon, Supabase, RDS, Cloud SQL, managed MySQL: all TLS-only or TLS-by-default. A stdlib that can't talk to them out of the box fails the UX this ADR is aiming for.

This is **outbound client** TLS - your app is the client, the database server is a third party - so it **cannot** be terminated at a reverse proxy / Cloudflare / nginx the way inbound HTTP serving can. The database stdlib needs an in-process TLS client.

**Engine - `stdlib/ssl.dr`, pure-Dragon, no vendored C library.** settles Dragon's TLS strategy globally: a pure-Dragon implementation, no vendored C crypto, on dogfooding + source-tree-footprint grounds. The database stdlib does **not** make its own TLS-library choice; it consumes the one engine provides. There is no `DRAGON_TLS_LIB` and no second TLS stack.

**Dependency - Milestone A.** Cloud-database support is gated on 's **TLS Client MVP** (Milestone A: TLS 1.3 client, server-auth cert verification, ECDHE X25519/P-256, AES-GCM + ChaCha20-Poly1305, RSA + ECDSA cert verification, X.509 chain validation, SNI, hostname check). That milestone is ~80% of 's total effort (a database TLS client *is* a full TLS client) and is months out. Scheduling: the local/plaintext tier (SQLite, self-hosted Postgres/MySQL on a trusted network) ships first and does not block on ; the cloud/TLS tier consumes Milestone A when it lands. TLS 1.3-only is sufficient for all named cloud providers in 2026; older self-hosted servers needing TLS 1.2 wait on Milestone C.

**Integration shape.** The driver performs each database's plaintext→TLS upgrade exchange, then hands the connected fd to `ssl.SSLContext.wrap_socket`:

```dragon
import ssl

# After the DB-specific upgrade handshake says "TLS OK":
ctx: ssl.SSLContext = ssl.create_default_context
if sslmode == "verify-full" {
 ctx.check_hostname = true
 ctx.verify_mode = ssl.CERT_REQUIRED
} else if sslmode == "verify-ca" {
 ctx.check_hostname = false
 ctx.verify_mode = ssl.CERT_REQUIRED
} else { # require / prefer
 ctx.check_hostname = false
 ctx.verify_mode = ssl.CERT_NONE
}
if ca_path != "" { ctx.load_verify_locations(ca_path) }
tls_sock: ssl.SSLSocket = ctx.wrap_socket(raw_sock, server_hostname=host)
```

`SSLSocket.read` / `write` route through the same `dragon_vthread_io_yield` as the plaintext path (§10: blocking-on-blocking-fd for v1, matching `socket.TcpStream`) - TLS records sit on top of the same socket. The only thing the protocol drivers learn is "wrap this `fd` after the upgrade exchange." For Postgres: send the 8-byte `SSLRequest` (`0x04D2162F`), read the `S`/`N` reply, and if `S`, wrap the fd. For MySQL: respond to the server's `SERVER_CAPABILITY_SSL` flag with the SSL request packet, wrap, then continue the auth flow over the encrypted channel. No `stdlib/database/tls.dr` wrapper is needed - `ssl.SSLContext` *is* the wrapper.

**DSN knob.** `?sslmode=...` mirrors the libpq vocabulary so cloud-database docs work verbatim:

| `sslmode` | Behavior |
|---|---|
| `disable` | Plaintext only. |
| `prefer` | Try TLS; fall back to plaintext if server says no. (default for postgres) |
| `require` | TLS mandatory; fail if server refuses. (default for mysql) |
| `verify-ca` | TLS + cert chain validates against CA. |
| `verify-full` | TLS + cert chain + hostname matches `server_name`. (recommended) |

Default for `postgres://` is `prefer`, matching libpq. Default for `mysql://` is `require`, matching mysql-connector. Cloud DSNs pasted from a provider's UI usually include `sslmode=require`, which is upgraded transparently to `verify-full` if a CA bundle is provided either via `?sslrootcert=...` or env (`PGSSLROOTCERT`, `MYSQL_SSL_CA`).

**Trust store.** Owned by, not this ADR - the database stdlib inherits whatever `ssl.create_default_context` resolves. ships the bundled Mozilla CA list and honors `SSL_CERT_FILE` / `SSL_CERT_DIR`. The DB-specific knob `?sslrootcert=PATH` / `MYSQL_SSL_CA` maps to `ctx.load_verify_locations(path)` and takes precedence over the default store, matching libpq/mysql-connector semantics. No DB-specific trust-store code.

**Cost.** ~0 KB incremental binary (the TLS engine is shared with `http`/`ssl`, not a DB-private dependency), ~50 LOC per driver for the plaintext→TLS upgrade exchange + `wrap_socket` call. Runtime cost is whatever 's engine delivers (pure-Dragon initially, asm fast paths at Milestone D).

### Concurrency model

- A `Pool` is shared across vthreads in the same process - `acquire` cooperatively yields if all connections are checked out.
- A `Conn` is **not** safe to share across vthreads simultaneously. The pool enforces single-checkout-at-a-time. Sharing a `Conn` reference across `fire`'d vthreads without going through the pool is a documented foot-gun (we will add a runtime check in debug builds).
- A `Tx` pins one `Conn` for its lifetime; nested `with db.tx` blocks open savepoints when the driver supports them.
- All driver methods are colorless - `await db.query(...)` works, `db.query(...)` works, both correct, both yielding under the hood.

### Placeholder rewrite - lazy, not eager (resolved)

The `SQL` value carries dialect-free `$$0`, `$$1`, ... canonical text (an interned `.rodata` literal) and a precomputed hash. Each driver rewrites once per (driver, canonical) at `Conn.prepare` and caches the dialect-specific text in `Stmt.dialect_text`, keyed on the canonical *pointer* (its hash pre-seeds the bucket). Steady-state cost: a pointer compare - zero string work, zero re-hash. First-call cost: O(text length) string walk. The tradeoff: a single `SQL` value works against any driver, which matters for code that opens both Postgres and SQLite (very common - SQLite for tests, Postgres for prod). Eager rewrite would force `SQL` values to be driver-bound at template-build time, which is ergonomically broken.

### Error model

```dragon
class DatabaseError(Exception) // base
class ConnectionError(DatabaseError) // network, auth, ssl
class QueryError(DatabaseError) // syntax, constraint, type
class NoRows(QueryError) // db.one found 0 rows
class MultipleRows(QueryError) // db.one found >1 rows
class TxError(DatabaseError) // commit/rollback failures
class PoolError(DatabaseError) // exhaustion, timeout
class TLSError(ConnectionError) // handshake, cert verification
```

Postgres `SQLSTATE` codes and MySQL error codes are exposed as `e.code` on `QueryError`. SQLite `extended_errcode` likewise.

### Module layout

```
stdlib/database/
 database.dr core: open, Connection, errors, ExecResult;
 SQL content type (Template subclass w/ build);
 SQLite driver (C FFI). Rows are dict[str, Any].
 # (planned) pool.dr pool implementation (lifecycle, checkout, healthcheck)
 # (planned) sqlite.dr SQLite driver split out once Postgres/MySQL land
 postgres/
 postgres.dr driver entry, registers under "postgres"/"postgresql"
 proto.dr wire protocol (startup, auth, parse, bind, execute, sync)
 types.dr oid → dragon type, dragon type → wire format
 auth.dr md5, scram-sha-256
 mysql/
 mysql.dr driver entry, registers under "mysql"
 proto.dr binary protocol
 types.dr type code → dragon type
 auth.dr native_password, caching_sha2
```

## Phases

| Phase | Scope | Gate |
|---|---|---|
| **0 done** | **Dependency: Phase 4 .** Add the `build`-signal detection + constant-folded `template[SQL]` codegen: per-site interned canonical `$$N` string literal + compile-time FNV-1a hash constant, native-typed param binding into a `list[Any]` (tagged box, no stringify). `SQL` content type (`canonical`, `hash`, `params`). Unit tests on template-only paths (no DB). | **Done .** Template tests green; `template[SQL] { ... }` yields the right canonical + native param pack; structurally identical sites share one interned canonical global; built-in templates HTML/CSS/etc. unchanged. (Composition-splice for nested `template[SQL]` interpolation deferred - errors explicitly for now.) |
| **1 done** | `database.dr` core: `open`, `Connection`, errors, `ExecResult`. SQLite driver via C FFI. `query` / `one` / `scalar` / `exec` / `exec_raw`. Rows are `dict[str, Any]`. Per-query placeholder rewrite. | **Done .** E2E green against in-memory SQLite (`InteropTest.Database*`). `Customer(**row)` round-trip + injection safety verified. (`Pool`/`Tx`/registry deferred to later phases - single-connection for now.) |
| **2** | Postgres driver - **plaintext tier**: pure-Dragon FE/BE v3, md5 + scram-sha-256 auth, prepared statements, type oid mapping, vthread-yielding socket I/O. No TLS yet. *No dependency.* | E2E green against `docker run postgres:16` plaintext; 10K-vthread bench shows no OS-thread blocking. |
| **3** | MySQL driver - **plaintext tier**: pure-Dragon binary protocol, native_password + caching_sha2 auth, prepared statements, type code mapping. No TLS yet. *No dependency.* | E2E green against `docker run mysql:8` plaintext. |
| **4** | **TLS tier - gated on Milestone A** (TLS Client MVP). Wire the plaintext→TLS upgrade in both drivers: Postgres `SSLRequest` (`0x04D2162F`), MySQL SSL-capability packet, then `ssl.create_default_context.wrap_socket(fd, server_hostname=host)`. `?sslmode=` knob (`disable`/`prefer`/`require`/`verify-ca`/`verify-full`) → `verify_mode` + `check_hostname` + `load_verify_locations`. | E2E against real cloud Postgres (Neon free tier) + cloud MySQL over TLS; `verify-full` rejects bad hostname. Blocked until Milestone A lands (TLS 1.3-only suffices for all named cloud providers). |
| **5** | Pool tuning: healthchecks, max-lifetime, max-idle, exponential backoff on `ConnectionError`. Per-statement cache eviction policy. | Bench: pool of 10 sustains 50K QPS on Postgres loopback (plaintext) and ≥35K QPS over TLS (TLS QPS subject to Milestone D asm fast paths). |
| **6** | TLS 1.2 server compat - **gated on Milestone C**. Only needed for older self-hosted DB servers that don't offer TLS 1.3. Fast-follow, not blocking cloud support. | E2E against a TLS-1.2-only self-hosted Postgres/MySQL. |
| **7** | Migration tool (`stdlib/database/migrate.dr`) - separate ADR. | - |

## Cost Analysis

**Runtime cost.** Per-query overhead dominated by network round-trip; the Dragon machinery adds:
- One `dict` lookup for prepared-statement cache (or zero, if the cache is keyed by interned `canonical` pointer).
- One `list` of params, already on hand from the template.
- `Customer(**row)` is a fast path: a single bulk `dragon_dict_copy` of the row dict (no per-column `keys`/`__getitem__` walk), since rows are `dict[str, Any]`.

No GC pressure beyond what container types already incur. Native-typed parameters avoid boxing.

**Compile-time cost.** Template lowering is done by the existing codegen; the only new work is `SQL.build` dispatch. Negligible vs LLVM optimization.

**Implementation cost.**

| Component | Lines | Notes |
|---|---|---|
| Phase 4 (`template[X]`) | 0 | Shipped ; prerequisite already met |
| Constant-folded `template[SQL]` codegen | ~300 | C++: interned canonical literal, compile-time FNV-1a, native param binding into `list[Any]`, composition splice |
| `SQL` content type | ~120 | Dragon: `canonical`/`hash`/`params` fields, `to_str` debug render, `build` fallback |
| `database.dr` core | ~600 | Pool, Conn, Tx, Rows, Row, registry, errors |
| SQLite driver | ~400 | C FFI to bundled sqlite3 |
| Postgres driver | ~1850 | Wire protocol v3 + auth + ~50 LOC TLS upgrade + `wrap_socket` |
| MySQL driver | ~2150 | Binary protocol + auth + ~50 LOC TLS upgrade + `wrap_socket` |
| Tests (Lex/Parse/Type/Codegen) | ~300 | template[SQL] specific |
| Tests (E2E) | ~700 | per-driver matrix incl. TLS paths |
| **Total (only, excl. P4 and)** | **~6200** | Across phases 1-5 |

The TLS engine LOC lives in (`stdlib/ssl.dr`), not counted here - the database stdlib only adds the ~50-LOC-per-driver upgrade exchange + `wrap_socket` call.

**Binary size.** Postgres + MySQL drivers add roughly 80-120 KB. **No DB-private TLS contribution** - the TLS engine is the shared pure-Dragon `stdlib/ssl.dr`, amortized across `http`, `ssl`, and any other TLS consumer; the CA bundle is shipped once by . SQLite is already bundled. No `libpq`, no `libmysqlclient`, no `libssl`, no DLLs.

## Comparison

### Same query, four languages

```python
# Python (psycopg2)
cur = conn.cursor
cur.execute("SELECT id, name, email FROM customers WHERE id > %s AND name ILIKE %s",
 (min_id, pattern))
for row in cur.fetchall:
 print(row[0], row[1], row[2]) # untyped tuple
```

```go
// Go (database/sql + pgx)
rows, _ := db.Query(ctx,
 "SELECT id, name, email FROM customers WHERE id > $1 AND name ILIKE $2",
 minID, pattern)
defer rows.Close
for rows.Next {
 var c Customer
 rows.Scan(&c.ID, &c.Name, &c.Email) // manual destructure
 fmt.Println(c.Name, c.Email)
}
```

```rust
// Rust (sqlx)
let rows = sqlx::query_as!(Customer,
 "SELECT id, name, email FROM customers WHERE id > $1 AND name ILIKE $2",
 min_id, pattern)
 .fetch_all(&pool).await?;
for c in rows { println!("{} {}", c.name, c.email); }
// macro requires DATABASE_URL at compile time
```

```dragon
// Dragon
rows = db.query(template[SQL] {
 select id, name, email
 from customers
 where id > !{min_id} and name ilike !{pattern}
})
for r in rows { print(r.name, r.email) }

// Typed view, opt-in:
customers: list[Customer] = [Customer(**r) for r in rows]
```

### Feature matrix

| Feature | Python (psycopg2) | Go (database/sql) | Rust (sqlx) | Dragon |
|---|---|---|---|---|
| Zero-install | No (`pip install`) | No (`go get`) | No (`cargo add`) | **Yes** |
| TLS bundled | No (system OpenSSL) | No (system) | No (system) | **Yes** (pure-Dragon `ssl.dr`) |
| Inline param binding | No (`%s`) | No (`$1`) | No (`$1`) | **Yes** (`!{x}`) |
| Compile-time SQL safety | No | No | Macro-only | **Yes** (template[SQL]) |
| Typed rows | No | Manual `Scan` | Macro-derived | Opt-in via `T(**row)` |
| Cross-DB API | Per-driver | Yes | Yes | Yes |
| Vthread/fiber-aware | No | Goroutine-aware | tokio-aware | **vthread-native** |
| Compile-time without live DB | Yes | Yes | **No** (DATABASE_URL) | Yes |
| Same code sync+async | No | No | Async-only | **Yes** (colorless) |

## Open Questions

1. **Prepared-statement cache size and eviction.** LRU per `Conn` with a sensible default (e.g., 256 entries). Tunable via DSN query string.
2. **CA bundle freshness.** Owned by (the database stdlib inherits its trust store). handles the refresh cadence and `SSL_CERT_FILE` / `SSL_CERT_DIR` overrides; the DB-specific `?sslrootcert=` / env override maps to `ctx.load_verify_locations`. Not a concern beyond passing the knob through.
3. **Driver-specific extensions.** Postgres `LISTEN/NOTIFY`, `COPY`, MySQL `LOAD DATA LOCAL` - exposed via `Conn` subclass methods (`PgConn`, `MyConn`) reachable via `db.raw_conn` escape hatch. Avoids polluting the core.
4. **Connection-string format.** PEP-3986 URIs (`postgres://user:pw@host:5432/db?sslmode=require&pool=10`) for everything? Yes - uniform across drivers.

## Verification

1. Build: `cmake --build build/`
2. Test: `cd build && ctest`
3. Manual E2E:
 ```dragon
 import database
 import database.postgres

 class Customer(TypedDict) {
 id: int
 name: str
 }

 db = database.open("postgres://localhost/test")
 rows = db.query(template[SQL] { select id, name from customers limit !{n} })
 for r in rows { print(r.id, r.name) }

 customers: list[Customer] = [Customer(**r) for r in rows]
 ```
4. Bench (phase 5): 10K vthreads, pool of 10, sustained QPS against loopback Postgres ≥ 50K plaintext, ≥ 35K over TLS.

## Decision

Build `stdlib/database/` per Option D: pure-Dragon wire-protocol drivers for Postgres and MySQL, C-FFI SQLite, all behind one `database.open(dsn)` API; queries are `template[SQL]` values whose `!{expr}` interpolations lower to real prepared-statement parameters; the API is minimal (`query`, `one`, `scalar`, `exec`) and returns plain `Row` / `list[Row]` / scalar / `ExecResult`; typed conversion is opt-in via ordinary `Customer(**row)`. TLS is provided by Dragon's pure-Dragon `stdlib/ssl.dr` - no vendored C crypto - consumed via `ssl.create_default_context.wrap_socket` with a libpq-compatible `?sslmode=` knob. Phase 4 is a hard prerequisite. The plaintext tier (SQLite + local/self-hosted Postgres/MySQL, Phases 1-3) ships without a TLS dependency; the cloud/TLS tier (Phase 4) is gated on Milestone A. The stdlib bundles only the open-protocol, near-universal drivers (SQLite, Postgres, MySQL); all other backends (Oracle, SQL Server, ClickHouse, …) are third-party `dragon egg` packages registering through the same `@register_driver` seam - with the `Driver` interface documented but not frozen as a public ABI until a follow-up ADR validates it against an out-of-tree driver. Migrations are a follow-up ADR.
