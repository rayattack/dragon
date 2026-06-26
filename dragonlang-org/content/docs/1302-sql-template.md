# The SQL Template and Parameters

[Connecting and Querying](/docs/1301-databases) showed the verbs that run queries.
This chapter is about the queries themselves - why `template[SQL]` makes string
concatenation, and the injection bugs that come with it, *unexpressible*. You met
`template[SQL]` as a [content type](/docs/1203-sql-and-url) in the templates part;
here's what it means when you hand one to the database.

## `!{}` is a bound parameter, not text

If you read the [Templates](/docs/1201-templates) chapter, you know `template[SQL]`
is a typed template: the literal text is fixed at compile time and every `!{expr}`
is an interpolation. What makes it a *query* rather than a string is **where those
interpolations go** - they don't go into the SQL text at all:

```dragon
name: str = "'; DROP TABLE users; --"

q: SQL = template[SQL] {
    select * from users where name = !{name}
}
```

`!{name}` does **not** get spliced into the SQL. It becomes a **bound parameter**.
The text the database actually prepares is the dialect-free canonical form

```text
select * from users where name = $$0
```

and `name` is handed over separately, as data, through the driver's prepared-statement
binding. The malicious payload above is matched as the literal nine-word string it
is; the table is never in danger. This isn't a convention you follow - it's the only
thing `template[SQL]` can do. **There is no `template[SQL]` form that interpolates
into the text.**

```dragon
db.run(template[SQL] { insert into t(name) values(!{evil}) })
cnt: int = db.val(template[SQL] { select count(*) from t })
# cnt == 1 - the row was inserted, the table survived, the payload stored verbatim.
```

> **A bare `str` is rejected.** `db.all`, `db.one`, `db.val`, and `db.run` accept a
> `SQL` value and nothing else - passing a `str` is a compile error, because the
> injection-prone overload isn't there to call. The one deliberate escape hatch is
> `raw`, for parameterless DDL.

## What you can and can't parameterize

A `!{}` binds a **value** - the things SQL lets you parameterize: the right-hand
side of a comparison, an inserted value, a `LIMIT`. What it *cannot* be is a
**structural identifier** - a table name, a column name, a keyword - because those
are part of the query's fixed text, decided at compile time, not data bound at run
time:

```dragon
# values - fine, these are parameters:
db.all(template[SQL] { select * from orders where total > !{minimum} limit !{n} })

# an identifier is NOT a parameter - write it literally in the template:
db.all(template[SQL] { select * from orders order by created_at })   # column name is literal
```

If you genuinely need a dynamic column or table name (a sort key chosen at runtime),
validate it against an allow-list of known-good identifiers and build the *template*
accordingly - never bind it as a parameter, and never reach for `raw` with user
input. This is the same rule every database enforces; Dragon just makes the safe
shape the default one.

## Caching comes free

Because the canonical text is fixed at compile time, the prepared statement is cached
for free: the same query site reuses the same prepared plan on every execution. At
compile time the compiler constant-folds the canonical `$$0`, `$$1`, … text into an
interned string in `.rodata` (deduplicated across structurally identical sites),
precomputes its hash so the statement-cache bucket is pre-seeded, and emits **only
the parameter binding** at the call site - your `!{expr}` values flowing at their
native LLVM types (`int` as `i64`, `float` as `f64`), bound straight into the wire
protocol with no `Any` round-trip and no string building.

Each backend rewrites the canonical `$$N` to its own placeholder dialect once, at
prepare time - `?N` for SQLite, `$N` for Postgres, `?` for MySQL (see
[Backends](/docs/1304-backends)). In steady state, executing a query builds and
hashes no SQL text at all: it binds parameters and steps the cached statement. **The
safe path is also the fast path** - you get injection safety and statement caching
from one design, with no API to remember.

## At a glance

| Fact | Consequence |
|------|-------------|
| `!{expr}` → a bound parameter (`$$0`, `$$1`, …) | values are data, never SQL text |
| Identifiers (table/column) are fixed text | can't be `!{}`-bound - write them literally |
| Only `raw` accepts a `str` | the injection-prone path isn't callable |
| Canonical text fixed at compile time | prepared-statement caching is automatic |
| `$$N` rewritten per backend | one query, every dialect |

The `SQL` type is the proof that a value went through parameter binding, not string
concatenation. Next, grouping several writes into one atomic unit:
[Transactions](/docs/1303-transactions).
