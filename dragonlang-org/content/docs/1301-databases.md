# Connecting and Querying

Every other ecosystem makes you assemble a database stack before you can run a
single query. Pick a driver (`psycopg2` or `psycopg3`? `mysqlclient` or `PyMySQL`?),
`pip install` it, hope the C headers are on the build machine, wire up a
connection-string parser, and then build your SQL by gluing strings together and
*promising* you'll remember to parameterize.

Dragon throws all of that out. The database library is **in the standard library**,
the drivers are **statically linked into your binary**, and a query is **not a
string** - it's a [`template[SQL]`](/docs/1302-sql-template) block. There's no driver
to install, nothing to `pip`, and no code path that concatenates user input into SQL.

```dragon
import database
from database import SQL

with database.open("sqlite::memory:") as db {
    db.raw("create table products(id integer primary key, name text, price real)")
    db.run(template[SQL] { insert into products(name, price) values(!{"pen"}, !{1.5}) })

    top: dict[str, Any] = db.one(template[SQL] {
        select name, price from products order by price desc limit 1
    })
    print(top.name, top.price)   # pen 1.5
}
```

## Connecting

`database.open(dsn)` opens a connection and returns a `Connection`. The DSN scheme
picks the backend; the rest of the API is identical across all three (full DSN
details in [Backends](/docs/1304-backends)):

```dragon
import database

a: database.Connection = database.open("sqlite::memory:")
b: database.Connection = database.open("sqlite://./app.db")
c: database.Connection = database.open("postgres://user:pass@db.example.com/shop")
d: database.Connection = database.open("mysql://user:pass@127.0.0.1/shop")
```

Switching databases is a one-line DSN change; your queries don't move.

## The query verbs

The surface is intentionally small. Five fetch/run verbs plus a raw escape hatch
cover everything, and the fetch verbs are **generic in the row type**.

### `all`, `one`, `val` - reading

```dragon
rows: list[dict[str, Any]] = db.all(template[SQL] { select id, name from players })

player: dict[str, Any] = db.one(template[SQL] { select id, name from players where id = !{pid} })

total: int = db.val(template[SQL] { select count(*) from players })
```

`all` returns one `dict` per row; `one` returns a single row (and raises `NoRows`
for zero, `MultipleRows` for more than one); `val` returns the **first column** of a
single row - perfect for counts and existence checks.

The fetch verbs are generic `[T]`, and `T` is **inferred from the binding
annotation**: `total: int = db.val(...)` pins `T = int`. When you call a fetch verb
*inline* - with no annotation to infer from - supply the type argument explicitly:

```dragon
print(db.val[int](template[SQL] { select count(*) from players }))   # inline needs [int]
```

### `run` and `runs` - writing

`run` executes a data-changing statement and returns a `Results` carrying `ran`
(rows affected) and `xid` (the new row's id); `runs` executes a batch:

```dragon
from database import Results

res: Results = db.run(template[SQL] {
    insert into players(name, score) values(!{name}, !{score})
})
print(res.ran)    # 1
print(res.xid)    # the new row's id

db.runs([
    template[SQL] { insert into players(name, score) values(!{"Ada"}, !{10}) },
    template[SQL] { insert into players(name, score) values(!{"Linus"}, !{20}) },
])
```

### `raw` - the escape hatch

```dragon
db.raw("create table players(id integer primary key, name text, score integer)")
```

`raw` takes a plain `str` and runs it **verbatim, with no parameters** - for DDL and
maintenance (`CREATE TABLE`, `PRAGMA`, `VACUUM`) where there's no user input to bind.
It's the *only* method that accepts a string, named to make that explicit. Never
pass user input through `raw`; that's exactly the door `template[SQL]` closes.

## Rows are dictionaries

A row is a `dict[str, Any]` keyed by column name, each value at its native type
(`int`, `float`, `str`, or `none` for SQL `NULL`). Dragon dicts support
[dot-access](/docs/0502-dictionaries) for identifier keys, so both forms read the
same value:

```dragon
r: dict[str, Any] = db.one(template[SQL] { select id, name from players where id = !{pid} })
print(r["name"])   # subscript - works for any key
print(r.name)      # dot-access - cleaner for identifier columns
```

There's no positional `r[0]`; a column is addressed by name. (Inside an f-string,
mind the quotes: `f"{r['name']}"` or `f"{r.name}"`, not `f"{r["name"]}"`.)

## Typed rows: `db.all[Customer](...)`

`dict[str, Any]` is convenient but untyped. For a typed value, define a `TypedDict`
and ask the fetch verb for it with `[T]`:

```dragon
class Customer(TypedDict) {
    id: int
    name: str
}

customers: list[Customer] = db.all[Customer](template[SQL] { select id, name from customers })
print(customers[0].id)     # an int, statically typed
print(customers[0].name)   # a str
```

When the binding is annotated, the bare form infers `T`, so
`customers: list[Customer] = db.all(...)` and `db.all[Customer](...)` mean the same
thing. There's no ORM and no row-mapping reflection - it's the same `TypedDict`
construction used everywhere else, compiling to a single bulk dictionary copy, so
the typed view costs essentially nothing.

## Cleaning up, pooling, and errors

A `Connection` is a context manager - `with database.open(dsn) as db { ... }` closes
it at block exit, even on a raise. Without `with`, call `db.close()` yourself. For a
server, pool connections rather than opening one per request:

```dragon
pool: database.Pool = database.Pool("postgres://localhost/shop", 8)
conn: database.Connection = pool.acquire()
total: int = conn.val(template[SQL] { select count(*) from orders })
pool.release(conn)
pool.close()
```

`Pool(dsn, max_idle)` keeps up to `max_idle` connections warm, and it's
concurrency-safe - thousands of [`fire`'d green threads](/docs/1101-green-threads)
share a small pool, because every blocking DB call yields the vthread, not the OS
thread.

Everything the layer raises descends from `DatabaseError`, so one
`except DatabaseError` is a valid catch-all; the specific types let you be precise:

```dragon
from database import DatabaseError, NoRows, MultipleRows

try {
    row: dict[str, Any] = db.one(template[SQL] { select id from customers where email = !{email} })
} except NoRows {
    print("no customer with that email")
} except MultipleRows {
    print("email is not unique - data problem")
} except DatabaseError as e {
    print(f"database error: {e}")
}
```

| Exception | Raised when |
|-----------|-------------|
| `DatabaseError` | base - catches anything below |
| `ConnectionError` | the connection can't be opened |
| `QueryError` | the database rejects a query (bad SQL, constraint) |
| `NoRows` / `MultipleRows` | `one`/`val` got zero / more than one row |
| `TxError` | a transaction operation failed |

## A complete example

```dragon
import database
from database import SQL, Results

class Player(TypedDict) {
    id: int
    name: str
    score: int
}

with database.open("sqlite::memory:") as db {
    db.raw("create table players(id integer primary key, name text, score integer)")

    res: Results = db.run(template[SQL] { insert into players(name, score) values(!{"Ada"}, !{10}) })
    print("inserted id", res.xid, "rows", res.ran)   # inserted id 1 rows 1

    db.run(template[SQL] { insert into players(name, score) values(!{"Linus"}, !{20}) })

    count: int = db.val(template[SQL] { select count(*) from players })
    print("players:", count)                          # players: 2

    leaderboard: list[Player] = db.all[Player](template[SQL] {
        select id, name, score from players order by score desc
    })
    for p in leaderboard {
        print(f"{p.name}: {p.score}")                 # Linus: 20 / Ada: 10
    }
}
```

## At a glance

| You want to... | Write |
|----------------|-------|
| Connect | `db: database.Connection = database.open(dsn)` |
| Many / one / a value | `db.all(...)` / `db.one(...)` / `db.val(...)` |
| Inline fetch (no annotation) | `db.val[int](...)`, `db.all[Customer](...)` |
| Insert / update / delete | `db.run(template[SQL] { ... })` → `Results` (`.ran`, `.xid`) |
| Batch writes | `db.runs([...])` |
| DDL / no-param SQL | `db.raw("create table ...")` |
| Read a column | `r.name` or `r["name"]` |
| Typed rows | a `TypedDict` + `db.all[Customer](...)` |
| Auto-close / pool | `with database.open(dsn) as db { ... }` / `database.Pool(dsn, n)` |

The query text itself - why `!{}` is a bound parameter and not string concatenation
- is the next chapter: [The SQL Template and Parameters](/docs/1302-sql-template).
