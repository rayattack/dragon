# SQLite, PostgreSQL, and MySQL

Everything in this part - [the verbs](/docs/1301-databases),
[the SQL template](/docs/1302-sql-template), [transactions](/docs/1303-transactions)
- is identical across the three backends Dragon supports. The *only* thing that
changes when you move from SQLite to Postgres to MySQL is the **connection string**.
Your queries don't move, because the canonical `$$N` parameter form is rewritten to
each backend's dialect at prepare time.

## One API, three DSN schemes

`database.open(dsn)` reads the scheme to pick the backend:

```dragon
import database

a: database.Connection = database.open("sqlite::memory:")
b: database.Connection = database.open("sqlite://./app.db")
c: database.Connection = database.open("postgres://user:pass@db.example.com/shop")
d: database.Connection = database.open("mysql://user:pass@127.0.0.1/shop")
```

| DSN | Backend |
|-----|---------|
| `sqlite::memory:` (or `:memory:`) | in-memory SQLite |
| `sqlite://PATH` (or a bare path) | file-backed SQLite |
| `postgres://...`, `postgresql://...` | PostgreSQL |
| `mysql://...` | MySQL |

## SQLite - the bundled C library

SQLite is the C amalgamation, **statically linked into your binary**. There's no
server to run and no file to install: `sqlite::memory:` gives you a throwaway
in-memory database (ideal for tests), and `sqlite://./app.db` gives you a
file-backed one created on first use. It's the right default for local tools,
embedded storage, and test fixtures.

## PostgreSQL and MySQL - pure-Dragon wire drivers

The Postgres and MySQL clients are written **in Dragon**, speaking each database's
wire protocol directly over the same raw epoll/kqueue sockets the
[HTTP server](/docs/1701-web-application) uses. There is **no `libpq`, no
`libmysqlclient`** - nothing to link against or install on the host. The Postgres
driver implements the Extended Query Protocol (parse/bind/execute) with SCRAM-SHA-256
authentication; the MySQL driver speaks its native protocol. `dragon run app.dr`
connects to a remote Postgres on a fresh machine with no `apt install libpq-dev` -
that's the whole setup.

Because every blocking database call yields the green thread rather than the OS
thread (see [Concurrency](/docs/1101-green-threads)), a server can pool a handful of
connections and serve thousands of concurrent requests through them, the way a Go
service would.

## The same query, three dialects

The injection-safe `$$N` placeholder form from [the SQL template](/docs/1302-sql-template)
is canonical and dialect-free. Each backend rewrites it to its own placeholder
syntax, once, at prepare time:

| Canonical | SQLite | Postgres | MySQL |
|-----------|:------:|:--------:|:-----:|
| `$$0`, `$$1`, … | `?1`, `?2` | `$1`, `$2` | `?`, `?` |

You never see this - you write `!{x}` and the right thing happens for whichever
backend the DSN selected. The same `template[SQL] { select * from users where id = !{id} }`
is correct on all three.

## Switching backends

Because the API and the query text are identical, moving between databases is a
one-line change to the DSN:

```dragon
# development: a throwaway in-memory database
db: database.Connection = database.open("sqlite::memory:")

# production: Postgres - every query above is unchanged
db: database.Connection = database.open("postgres://user:pass@prod-db/shop")
```

This is the payoff of treating the query as a typed `SQL` value rather than a
backend-specific string: write against the standard API, develop against SQLite,
deploy against Postgres or MySQL, and the only thing you touch is the connection
string.

## At a glance

| Backend | DSN | Linkage |
|---------|-----|---------|
| SQLite (memory) | `sqlite::memory:` | bundled C amalgamation, static |
| SQLite (file) | `sqlite://./app.db` | bundled C amalgamation, static |
| PostgreSQL | `postgres://user:pass@host/db` | pure-Dragon wire driver (SCRAM), no `libpq` |
| MySQL | `mysql://user:pass@host/db` | pure-Dragon wire driver, no `libmysqlclient` |

That closes Part 13 - and the database story: a query is a typed value, parameters
are always bound, transactions are explicit, and the backend is one line. Next, the
breadth of what else ships: [the Standard Library](/docs/1401-stdlib-overview).
