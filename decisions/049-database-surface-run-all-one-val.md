# Decision 049: Database Surface - `run` / `all` / `one` / `val` + `Results` (Amendment to D032)

Done. implemented & tested (ctest 100/100; SQLite E2E + live Postgres/MySQL). D032 shipped the database stdlib with a minimal query surface:

> `db.query`, `db.one`, `db.scalar`, `db.exec`, `db.exec_raw`; each takes a `template[SQL]` and returns `list[Row]` / `Row` / scalar / `ExecResult`.

This amendment replaces that surface only - engine untouched. Constant-folded `template[SQL]`, interned-canonical prepared-statement cache, native-typed param binding, `dict[str, Any]` rows, `@register_driver` seam, TLS, pooling, error hierarchy, virtual dispatch all stay verbatim. Rename layer plus `transaction` and typed `[T]` fetch.

Two surface problems that annoyed me (engine still untouched):

1. **Naming vs #3.** correctly rejected PEP-249's per-driver architecture, then borrowed
Go verbs (`query`/`exec`/`exec_raw`) and SQLAlchemy's (`scalar`/`one`). Never really argued as a parity choice.

2. **Verb sprawl + an awkward split.** Four read/write verbs on the connection (`query`/`one`/`scalar`/`exec`) plus `exec_raw`, with no single "run this" primitive and no `commit`/`rollback`/transaction surface at all (deferred `Tx` and never shipped it).

Not strict PEP-249. #3 is lowest priority here; renaming is free on #1/#2.
Surface optimized for brevity: `run` / `runs` / `ran`, fetch shapes `all` / `one` / `val`.
Documented #3 trade: DX clarity over literal Python mimicry.

## Options Considered

### Option A - Verbs on the connection only (`db.all[T](sql)`, `db.one[T](sql)`, `db.val[T](sql)`)
Shortest common path; one call does execute + fetch + map. But DML doesn't fit "fetch" verbs (needs its own), and there is no result handle for streaming / `rowcount` / insert-id.

### Option B - One verb, everything on the result (`db.run(sql).all[T]`)
Uniform, one connection verb, streaming-native. But `db.run(sql).one[Customer]` is wordier than `db.one[Customer](sql)` for the 90% case.

### Option C - Both: `run` is the primitive, `all`/`one`/`val` are sugar over it (chosen)
`db.all[T](sql) == db.run(sql).all[T]`. Get Option A's brevity for the common case and Option B's power (`Results` handle, streaming, `.xid`/`.ran`) for the rest, with the read verbs written **once** on the base `Connection` over the driver-overridden `run` primitive - the same "shared surface on top of one primitive, reach the driver via the vtable" pattern already uses for `one`/`scalar`. Mirrors how Python's stdlib (`Connection.execute` shortcut) and SQLAlchemy (`Connection.scalar(stmt)` forwarding) are layered.

## Decision

### `Connection` surface

```dragon
db: Connection = database.open("postgres://localhost/shop?pool=10")

# ---- the primitive: run a query, get a Results back ----
db.run(sql: SQL) -> Results # SELECT or DML; ignore the return for fire-and-forget

# ---- read sugar (== db.run(sql).<verb>); [T] optional ----
db.all[T](sql: SQL) -> list[T] # db.all(sql) -> list[dict[str, Any]]
db.one[T](sql: SQL) -> T # db.one(sql) -> dict[str, Any] (exactly one; raises NoRows/MultipleRows)
db.val[T](sql: SQL) -> T # db.val(sql) -> Any (first column of the single row)

# ---- batch + raw ----
db.runs(stmts: list[SQL]) -> Results # run a batch (each a finished template[SQL])
db.raw(text: str) -> Results # raw / multi-statement DDL escape hatch (was exec_raw)

# ---- transactions (new - fills the gap deferred) ----
with db.transaction as tx { tx.run(a); tx.run(b); tx.commit } # rolls back unless committed
db.commit db.rollback db.close
```

### `Results` (returned by `run` / `runs` / `raw`)

```dragon
class Results {
 all[T] -> list[T] # all -> list[dict[str, Any]]
 one[T] -> T # one -> dict[str, Any]; exactly one, raises NoRows/MultipleRows
 val[T] -> T # val -> Any; first column of the single row
 # iterable: `for r in results { ... }` (walks the rows)
 xid: int # last-insert id (was ExecResult.last_insert_id)
 ran: int # rows affected (was ExecResult.rows_affected)
}
```

### Semantics

- **`run` is the sole driver primitive.** Each backend overrides `run(sql) -> Results`, building a `Results` that carries the fetched rows (for SELECT) and/or `xid`/`ran` (for DML) - the wire protocols already distinguish DataRow/CommandComplete (PG) and result-set/OK (MySQL), so this merges the old `query`+`exec` into one. `runs` and `raw` are likewise driver primitives. `all`/`one`/`val` are **written once on the base `Connection`** as `self.run(sql).<verb>` and reach the concrete driver through the vtable - `ExecResult` is deleted, its two fields folding into `Results.xid`/`.ran`.

- **`[T]` is optional, everywhere it appears.** Absent → the raw `dict[str, Any]` row(s). Present → typed, via `T(**row)` - the call-site `**`-spread materialization made general in the compiler (works for any class with a matching explicit `def(...)` ctor, and for TypedDicts). A class used with `[T]` must therefore have a constructor whose parameters match the selected columns; a field-default-only class with no ctor is rejected at compile time ("no parameter metadata for a `**dict` spread"), same as anywhere else `**`-spread is used.

- **The typed `[T]` forms are generic methods .** `db.one[Customer](sql)` / `Results.all[Customer]` monomorphize per `T`. There is exactly **one** generic definition per verb (no `def one` + `def one[T]` overload - the compiler rejects that as ambiguous); a bracket-less `db.one(sql)` infers `T` from the call context (the binding annotation, e.g. `c: Customer = db.one(sql)`), and `row: dict[str, Any] = db.one(sql)` infers `T = dict[str, Any]` (which copies the row via `dict(**row)`). Explicit `db.one[Customer](sql)` always works. The hand-written `[Customer(**r) for r in db.all(sql)]` form remains valid for callers who prefer it.

- **`val` enforces single-row** (first column of the one result row; raises `NoRows`/`MultipleRows`) - identical semantics to the renamed `scalar`. `one` likewise asserts exactly one. A lenient "maybe-one / None-if-absent" companion (`first[T]`) is intentionally omitted in this amendment; it can be added later without disturbing this surface.

- **`Results` is eager in v1** (it wraps the fully-fetched row list, matching today's `query`); iteration walks the buffer. True lazy/streaming fetch (`fetchmany` over the wire for large result sets) is a deferred enhancement - see Open Questions.

- **`transaction` is written once on the base `Connection`** over `raw("BEGIN")` / `raw("COMMIT")` / `raw("ROLLBACK")` (the same dispatch-through-the-vtable pattern), binding a pinned connection as `tx` so it is pool-correct (a transaction pins one `Conn` for its lifetime, per §"Concurrency model"). For a single `Connection`, `tx` is `db` itself. This is a **query-layer primitive, not a mapper**, so it is fully consistent with 's "no ORM in stdlib" non-goal.

### Rename map

| (today) | (this amendment) |
|---|---|
| `db.query(sql) -> list[dict]` | `db.all(sql)` / `db.all[T](sql)` |
| `db.one(sql)` | `db.one(sql)` / `db.one[T](sql)` *(name retained)* |
| `db.scalar(sql)` | `db.val(sql)` / `db.val[T](sql)` |
| `db.exec(sql) -> ExecResult` | `db.run(sql) -> Results` |
| `db.exec_raw(text) -> int` | `db.raw(text) -> Results` |
| *(none)* | `db.runs(stmts: list[SQL]) -> Results` |
| `ExecResult.last_insert_id` | `Results.xid` |
| `ExecResult.rows_affected` | `Results.ran` |
| *(deferred, never shipped)* | `db.transaction` / `commit` / `rollback` |
| `Customer(**row)` materialization | unchanged ; now also reachable as the `[T]` forms once lands |

### Naming rationale (so it isn't relitigated)

- **`run` not `execute`** - brevity; `run`/`runs`/`ran` is one coherent family.
- **`val` not `scalar`** - brevity; `scalar` is SQLAlchemy's, not stdlib, so no parity is lost.
- **`.xid` not `.oid`** - `oid` collides with Postgres's legacy system-column type and would mislead PG users; `xid` reads as "the id of the row just written" without that baggage.
- **`all`/`one` retained-or-adopted** - already the idiomatic SQLAlchemy `Result` method names; short *and* recognizable.

## Breaking change, worth it

- **Breaking change to a shipped surfce.** Every call site and the `test/dr/test_{database,postgres,mysql}.dr` suites change. The migration is mechanical (a rename), since the engine and row shape are untouched. is pre-1.0 and has no external consumers, so a clean break (no deprecation shims - #2) is correct.

- **Zero performance impact** (#1). The verbs forward to the same constant-folded `template[SQL]` + interned-canonical prepared-statement cache. The only new allocation is the thin `Results` wrapper on the common `db.all`/`db.one` path; the cursorless-fetch fast path is preserved because `all`/`one`/`val` go straight through `run` to the buffered rows. The typed `[T]` forms monomorphize to exactly the hand-written `[T(**r) for r in rows]`, i.e. one bulk `dragon_dict_copy` per row - no runtime reflection.

- **This is the foundation the future ORM egg rides.** A third-party `Repo[T]` (generic classes) calls `db.one[T]` / `db.all[T]` internally and reads `Results.xid` after an insert; `run` virtual-dispatches to the real driver, so the egg is driver-agnostic with no stdlib change. The typed `[T]` fetch *itself* stays in the stdlib (it is ordinary `T(**row)`, not a relational mapper), keeping the egg scoped to genuine ORM concerns (table-name derivation, CRUD, unit-of-work). See the database-egg-readiness analysis.

- **`transaction` closes a real gap.** designed `Tx`/`begin` but never shipped them; serious use (and any ORM egg) needs unit-of-work with rollback-on-exception. It lands here as a base-`Connection` context manager.

## Migraion plan

1. `stdlib/database/base.dr` - rename the abstract primitive to `run(sql) -> Results`; delete `ExecResult` (fold into a new `Results` class with `xid`/`ran` + `all`/`one`/`val` + iteration); write `all`/`one`/`val`/`transaction`/`commit`/`rollback` once on the base over `run`/`raw`. Bare forms first; add the `[T]` generic overloads behind .
2. `stdlib/database/{sqlite,postgres,mysql}.dr` - each driver replaces its `query`/`exec`/`exec_raw` overrides with `run`/`runs`/`raw` returning `Results`. Wire-protocol code is unchanged; only the result-assembly entry point is renamed/merged.
3. `stdlib/database/database.dr` - re-export `Results` (drop `ExecResult` from the public list).
4. `test/dr/test_{database,postgres,mysql}.dr` - migrate to the new verbs; add a `transaction` commit/rollback test and (once lands) typed-`[T]` round-trip tests alongside the existing `Customer(**row)` ones.

## Open Questions

1. **Lazy/streaming `Results`.** v1 is eager (buffers all rows, like today's `query`). A lazy mode that fetches in chunks over the wire (`fetchmany`-style) for large result sets is a worthwhile follow-up; iteration already provides the ergonomic surface for it.
2. **`first[T]` (lenient one).** Omitted now; add if "maybe-one / None-if-absent" demand materializes.
3. **`transaction` binding shape under a `Pool`.** `as tx` pins a checked-out `Conn`; confirm savepoint nesting (`with tx.transaction`) maps to driver savepoints when supported (§"Concurrency model" anticipated this).

## Decision

Replace 's `query`/`one`/`scalar`/`exec`/`exec_raw` + `ExecResult` surface with `run` (the primitive) and the `all`/`one`/`val` read sugar, plus `runs`/`raw`/`transaction`/`commit`/`rollback`, all returning/￼operating on a `Results` object carrying `xid`/`ran`. `[T]` is optional on every fetch verb - bare → `dict[str, Any]`, `[T]` → `T(**row)` ; the generic `[T]` forms are generic methods with `T` inferred from the call context or supplied explicitly. The engine, row shape, driver seam, TLS, pooling, and error model from are unchanged. Brevity over literal PEP-249 names is a deliberate #3 trade that costs nothing on #1/#2.
