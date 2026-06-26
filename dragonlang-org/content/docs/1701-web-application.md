# Building a Web Application

Most languages ask you to assemble a web application from parts that
were never designed to meet. In Python you reach for Flask plus an ORM
plus a template engine plus a WSGI server, glue them with config, and
hope the versions agree. Go gives you a fast `net/http` but leaves
routing, templating, and the database to a churn of third-party
libraries. Rust gives you Actix or Axum and a borrow-checker fight over
shared state. In every case the *server* - the thing that turns bytes on
a socket into a function call - is somebody else's crate, and the day it
has a bug you are reading code you did not write.

Dragon ships the whole stack in its standard library, written in Dragon,
and it dogfoods it in public: **dragonlang.org itself runs on
`http.server`** (see `dragonlang-org/src/app.dr`). The router, the
HTTP/1.1 framer, sessions, gzip, static files, even the WebSocket codec
are all `.dr` modules you can read. There is no C web framework hiding
under the hood - the only native code is the socket syscalls and the
llhttp parser. When your service is slow or wrong, the call stack stays
inside the language you wrote it in.

This chapter builds a real, database-backed web service end to end: a
leaderboard that accepts scores over a form, stores them in SQLite, and
renders them as HTML - with the cross-site-scripting hole closed, which
is the part most tutorials skip. Every program here was compiled and run
against a live server before it went into the page.

## The shape of a Dragon server

There is no `main`, no `app.run()` ceremony hiding a magic entry point,
no decorator that registers routes by side effect at import time. As
everywhere in Dragon, **the file you hand to `dragon run` is the
program**, and its top-level statements execute top to bottom (see
[Modules](/docs/1001-modules)). A server is therefore just four moves at
the top level:

1. construct a `Router` bound to a port and host,
2. register handlers with `app.GET(...)`, `app.POST(...)`, and friends,
3. open whatever resources you need (a database, say),
4. call `app.listen()`, which blocks and runs the accept loop forever.

`app.listen()` is the last line because it never returns under normal
operation - it is the program's body. Everything above it is setup that
runs once at startup. Here is the entire leaderboard, which is what the
rest of the chapter dissects:

```dragon
from http.server import Router, Request, Response, Context
from html import escape
import database
from database import SQL

db: database.Connection = database.open("sqlite:///tmp/players.db")
db.raw("create table if not exists players (id integer primary key, name text, score integer)")

def render_page() -> str {
    rows: list[dict[str, Any]] = db.all(template[SQL] { select name, score from players order by score desc })
    items: str = ""
    for row in rows {
        nm: str = row["name"]
        sc: Any = row["score"]
        items = items + "<li>" + escape(nm) + ": " + str(sc) + "</li>"
    }
    return template {
        <!doctype html>
        <title>Players</title>
        <h1>Leaderboard</h1>
        <ul>!{items}</ul>
        <form method="post" action="/add">
          <input name="name"><input name="score"><button>Add</button>
        </form>
    }
}

app: Router = Router(8731, "127.0.0.1")

app.GET("/", lambda (req: Request, res: Response, ctx: Context) -> None {
    res.html(render_page())
})

app.POST("/add", lambda (req: Request, res: Response, ctx: Context) -> None {
    form: dict[str, str] = req.form()
    nm: str = form["name"]
    sc: int = int(form["score"])
    db.run(template[SQL] { insert into players (name, score) values (!{nm}, !{sc}) })
    res.redirect("/")
})

app.listen()
```

Build it to a binary and run it, then talk to it with `curl`:

```bash
dragon build app.dr -o app && ./app &
sleep 1.2
curl --data "name=Ada&score=10"            http://127.0.0.1:8731/add
curl --data "name=<b>Linus</b>&score=50"   http://127.0.0.1:8731/add
curl --data "name=Grace&score=30"          http://127.0.0.1:8731/add
curl http://127.0.0.1:8731/
```

The `POST`s each answer `307 Temporary Redirect` back to `/`, and the
final `GET /` returns the page with the players sorted by score:

```html
<ul><li>&lt;b&gt;Linus&lt;/b&gt;: 50</li><li>Grace: 30</li><li>Ada: 10</li></ul>
```

Three things to notice before we go further. The list is sorted highest
first because the SQL says so. The `score` column comes back as a real
integer (`50`, `30`, `10`), not a quoted string. And the player who tried
to inject `<b>Linus</b>` is rendered as the inert text
`&lt;b&gt;Linus&lt;/b&gt;` - the script-injection attempt was neutered.
That last point is not an accident; it is `escape()` doing its job, and
we will come back to why it matters.

> **Why a binary, not `dragon run`?** `dragon run app.dr` works, but
> trailing words on its command line are taken as *more files to
> compile*, not program arguments - and a long-lived server is easier to
> background and kill as a built binary. Build once, run the artifact.

## Routing

A `Router` is constructed with a port and a host:

```dragon
app: Router = Router(8731, "127.0.0.1")
```

Both arguments have defaults (`Router(8000, "0.0.0.0")`), so a bare
`Router()` listens on every interface on port 8000. Use `127.0.0.1` to
bind to loopback only - the right default while developing.

You attach handlers with one method per HTTP verb. The full set is
`GET`, `POST`, `PUT`, `PATCH`, `DELETE`, `HEAD`, `OPTIONS`, `TRACE`, and
`CONNECT`; there is also `HTTP`, which registers the same handler for
every verb at once. Each takes a path pattern and a handler:

```dragon
app.GET("/", home)
app.POST("/add", add_player)
app.DELETE("/players/:id:int", remove_player)
```

### Path parameters

A path segment beginning with `:` is a parameter that matches any value
in that position and captures it by name. You can pin the *type* of a
parameter by appending `:int`, `:float`, `:bool`, or `:uuid` - and this
is more than documentation. A typed segment only matches when the value
actually parses as that type; otherwise the route is skipped and the
request falls through (typically to a `404`). So `/players/:id:int`
matches `/players/42` but **not** `/players/abc`:

```dragon
from http.server import Router, Request, Response, Context

app: Router = Router(8732, "127.0.0.1")

app.GET("/players/:id:int", lambda (req: Request, res: Response, ctx: Context) -> None {
    pid: int = req.param_int("id")
    res.json("{\"id\": " + str(pid) + "}")
})

app.listen()
```

A `GET /players/42` returns `{"id": 42}`; a `GET /players/abc` returns
`Not Found` with status `404`, because no route claimed it. Inside the
handler, read a parameter with the typed accessor that matches its
declared type: `param_int(name)`, `param_float(name)`, `param_bool(name)`
for the typed forms, or `req.params[name]` for the raw string. A trailing
`*` segment is a wildcard that captures the rest of the path into
`req.params["*"]` - that is exactly how static-file serving works,
below.

### Query strings

Query parameters (`?q=dragon&page=2`) are read off `req`, never off the
path. The string accessor takes a default for the missing case, so you
never branch on presence:

```dragon
app.GET("/search", lambda (req: Request, res: Response, ctx: Context) -> None {
    term: str = req.query_str("q", "none")
    res.text("searching for: " + term)
})
```

`GET /search?q=dragon` answers `searching for: dragon`; a bare
`GET /search` answers `searching for: none`. The typed siblings
`query_int(name, default)`, `query_float(name, default)`, and
`query_bool(name, default)` parse and fall back to the default when the
value is absent or unparseable - no exception, no `400`, just the
default. That is the Python `request.args.get(name, default)` contract,
typed.

## Handlers are `Callable[[Request, Response, Context], None]`

Every route handler has exactly one signature:

```dragon
lambda (req: Request, res: Response, ctx: Context) -> None { ... }
```

- a `Request` you read from, a `Response` you write into, a `Context`
scratch-bag for passing data between middleware and the handler, and a
`None` return. You do not return the response; you *mutate* `res` and the
router serializes it for you after the handler runs. That is why every
example calls `res.html(...)` or `res.redirect(...)` rather than
`return`ing anything.

Lambdas are the idiom for short handlers because they read inline at the
registration site - the route and its behavior sit together, and there
is no name to invent for a three-line body. But a lambda is just an
anonymous function; for a substantial handler, define a named `def` with
the same signature and pass it by name. This compiles and runs
identically:

```dragon
from http.server import Router, Request, Response, Context
import database
from database import SQL

db: database.Connection = database.open("sqlite::memory:")
db.raw("create table t (id integer primary key, n text)")
db.run(template[SQL] { insert into t (n) values (!{"alpha"}) })

# A named handler function - same signature as the lambda form.
def home(req: Request, res: Response, ctx: Context) -> None {
    row: dict[str, Any] = db.one(template[SQL] { select n from t where id = !{1} })
    name: str = row["n"]
    total: int = db.val(template[SQL] { select count(*) from t })
    res.text("first=" + name + " count=" + str(total))
}

app: Router = Router(8737, "127.0.0.1")
app.GET("/", home)
app.listen()
```

This serves `first=alpha count=1`. Reach for a named function the moment
the body grows past a few lines or you want to reuse it on more than one
route; reach for a lambda when the handler is small enough to read at a
glance.

> **Sharing state with handlers.** The `db` in these examples is a
> module-level global, declared at the top level and *read* (not
> reassigned) inside the handlers - which needs no `global` keyword (see
> [Modules](/docs/1001-modules)). That is the clean way to hand a
> connection, a config value, or a template cache to every handler: open
> it once at the top level, read it freely.

## Reading input

`Request` exposes the body three ways, depending on what the client
sent.

`req.form()` parses an HTML form body (`application/x-www-form-urlencoded`
or `multipart/form-data`) into a `dict[str, str]`. It is what the
leaderboard's `/add` handler uses:

```dragon
form: dict[str, str] = req.form()
nm: str = form["name"]
sc: int = int(form["score"])
```

Form values are always strings, so coerce the numeric ones with `int(...)`
or `float(...)` yourself - the wire has no types. For file uploads,
`req.files()` returns the uploaded parts instead.

`req.json()` *decodes* the request body, returning an `Any` tree - JSON
objects become `dict[str, Any]`, arrays become `list[Any]`, and scalars
are boxed. It decodes the verbatim body bytes and raises `ValueError`
(with a byte offset) on malformed JSON. When you want the undecoded body
to validate or parse yourself, read `req.body`, the raw request body as a
`str`. For a known shape, `T(**json.loads_obj(req.body))` decodes
straight into your type. `req.query_str(name, default)` and its typed
siblings, covered above, read the query string. Between these you can
read any request a browser or API client will send.

## Writing responses

You shape the reply by calling methods on `res`. The body setters each
also set the right `Content-Type`:

| Call | Sets `Content-Type` | Use for |
|------|---------------------|---------|
| `res.html(body)` | `text/html` | rendered pages |
| `res.text(body)` | `text/plain` | plain text, debug output |
| `res.json(body)` | `application/json` | API responses (pass a `str` of JSON) |
| `res.out(status, body)` | unchanged | set status + body in one call |
| `res.redirect(url)` | - | `307` redirect to `url` |
| `res.redirect(url, true)` | - | `308` permanent redirect |

Beyond the body, `res.set_header(key, value)` adds an arbitrary header,
`res.status = 404` sets the status code directly, and
`res.cookie(name, value, ...)` queues a `Set-Cookie` line (with optional
`max_age`, `path`, `secure`, `httponly`, and `samesite`). Most of these
chain - `res.set_header(...)` and `res.text(...)` return the `Response`
- so you can write `res.set_header("x-powered-by", "dragon").text("ok")`
when it reads better.

`res.out(status, body)` is the workhorse for non-200 replies:

```dragon
app.GET("/teapot", lambda (req: Request, res: Response, ctx: Context) -> None {
    res.out(418, "I'm a teapot")
})
```

The redirect-after-POST that the leaderboard uses is the standard guard
against duplicate form submissions: the browser `POST`s `/add`, gets a
`307` to `/`, and re-`GET`s the page - so a refresh re-runs the harmless
`GET`, not the `POST`.

## HTML escaping: closing the XSS hole

This is the part the leaderboard gets right and most quick examples get
wrong. When you splice user-supplied text into HTML, you must escape it,
or a player named `<script>steal()</script>` runs their script in every
visitor's browser. The leaderboard built each row by hand, so it escaped
by hand:

```dragon
items = items + "<li>" + escape(nm) + ": " + str(sc) + "</li>"
```

`from html import escape` gives you Python's `html.escape`: it turns `<`
into `&lt;`, `>` into `&gt;`, `&` into `&amp;`, and quotes into their
entities. That single call is why `<b>Linus</b>` rendered as inert text.
Escape **every** value that came from a request before it enters markup -
names, search terms, comments, anything. The score did not need escaping
because `str(sc)` of an integer can only ever be digits.

Dragon has a sharper tool for the common case, the typed template (see
[Templates](/docs/1201-templates)). A `template[HTML] { ... }` value
escapes every `!{...}` interpolation **automatically**, so you cannot
forget. It yields an `HTML` value, which you turn into the final markup
string with `str(...)`:

```dragon
from html import escape, HTML

name: str = "<b>Linus</b>"
page: HTML = template[HTML] { <p>Hello, !{name}</p> }
rendered: str = str(page)
print(rendered)        # <p>Hello, &lt;b&gt;Linus&lt;/b&gt;</p>
```

Note the import path: the `HTML` content type lives in the `html` module
(`from html import HTML`), alongside `escape`. Prefer `template[HTML]`
for whole fragments - the escaping is structural, not a discipline you
have to remember - and keep `escape()` for the cases where you are
concatenating markup by hand or escaping a single value.

The leaderboard's outer page used the *plain* `template { ... }` form for
its static shell and pre-escaped the dynamic `items` string with
`escape()` before interpolating it with `!{items}`. That is deliberate:
the `items` string already contains intentional `<li>` tags you do *not*
want escaped, so it is built with hand-escaping at the leaf values and
spliced in whole.

## Database-backed handlers

The data layer is the `database` module (see [Databases](/docs/1301-databases)),
and it is built so that the injection-prone path does not exist. You open
a connection by DSN and get back a `Connection`:

```dragon
import database
from database import SQL

db: database.Connection = database.open("sqlite:///tmp/players.db")
```

The DSN scheme picks the backend: `sqlite://PATH` (or `sqlite::memory:`
for a throwaway in-memory DB), `postgres://...`, or `mysql://...`. The
same `Connection` API rides on all three.

Queries are **not strings**. A `template[SQL] { ... }` value compiles the
literal text to a parameterized prepared statement and turns every
`!{expr}` into a bound parameter - never string concatenation. The type
system *rejects* a bare `str` at `all`/`run`, so there is no call site
where an attacker-controlled value can be spliced into SQL text:

```dragon
db.run(template[SQL] { insert into players (name, score) values (!{nm}, !{sc}) })
```

Here `nm` and `sc` are bound parameters; a name of `'); drop table
players; --` is stored as that literal string, not executed. Injection
safety is carried by the type, not by your discipline.

The connection methods:

| Method | Returns | For |
|--------|---------|-----|
| `db.all(sql)` | `list[dict[str, Any]]` | many rows |
| `db.one(sql)` | `dict[str, Any]` | exactly one row (raises on 0 or >1) |
| `db.val(sql)` | `Any` | the first column of the one row |
| `db.run(sql)` | `Results` | INSERT/UPDATE/DELETE |
| `db.raw(text)` | `Results` | the escape hatch: no params, verbatim |

Each fetch method also takes an optional `[T]` that types the result:
`db.all[Customer](sql)` returns a `list[Customer]`, `db.one[Customer](sql)`
a `Customer`, and `db.val[int](sql)` an `int`. When the result is assigned
to an annotated binding the bare form infers `T` from the annotation, so
`n: int = db.val(sql)` and `rows: list[dict[str, Any]] = db.all(sql)` need
no explicit `[T]`; supply it where there is no annotation to read from,
such as in an argument position (`print(db.val[int](sql))`).

`raw` is the one call that takes a plain string, reserved for DDL
and maintenance where there is nothing to parameterize - the leaderboard
uses it for the `create table` at startup. Everything that touches user
data goes through `template[SQL]`. The mutating calls `run` and `raw`
return a `Results`, whose `.ran` is the number of rows affected and whose
`.xid` is the last inserted row id.

A result row is a `dict[str, Any]` keyed by column name, in column order.
Read a column with `row["name"]`; because the value type is `Any`, bind a
known-string column straight into a `str` and a numeric column into an
`Any` you can `str(...)` for display - exactly what `render_page` does:

```dragon
rows: list[dict[str, Any]] = db.all(template[SQL] { select name, score from players order by score desc })
for row in rows {
    nm: str = row["name"]
    sc: Any = row["score"]
    items = items + "<li>" + escape(nm) + ": " + str(sc) + "</li>"
}
```

> **Bind the query result before iterating.** Write `rows: list[...] =
> db.all(...)` and loop over `rows`, not `for row in db.all(...)`
> directly - binding first keeps the element type and reads more clearly.

## Serving static assets

`app.ASSETS(folder, route)` maps a URL prefix to a directory on disk and
serves files from it, guessing the `Content-Type` from the extension.
The route pattern ends in a `*` wildcard that captures the rest of the
path as the filename:

```dragon
from http.server import Router, Request, Response, Context

app: Router = Router(8734, "127.0.0.1")
app.ASSETS("public", "/assets/*")        # GET /assets/style.css -> public/style.css

app.GET("/", lambda (req: Request, res: Response, ctx: Context) -> None {
    res.html("<link rel=\"stylesheet\" href=\"/assets/style.css\"><h1>home</h1>")
})

app.listen()
```

A `GET /assets/style.css` serves `public/style.css` with
`Content-Type: text/css`; a missing file yields `404`. Binary files -
images, fonts, archives - travel byte-exact because `ASSETS` reads them
raw, so a PNG or WOFF2 is not corrupted by a UTF-8 round-trip.

## Middleware

`app.BEFORE(pattern, handler)` runs a handler before the matched route
handler; `app.AFTER(pattern, handler)` runs one after. The pattern uses
the same matching as routes, so `"/*"` runs on every path. Middleware has
the same `Callable[[Request, Response, Context], None]` signature as a
route handler - it reads and writes the same `req`/`res`/`ctx`:

```dragon
app.BEFORE("/*", lambda (req: Request, res: Response, ctx: Context) -> None {
    res.set_header("x-app", "leaderboard")
})
```

Every response now carries `x-app: leaderboard`. A `BEFORE` hook can call
`res.abort()` to short-circuit the request before the route handler runs
- the pattern for authentication checks: verify the request in `BEFORE`,
abort with a `401` if it fails, and the handler never sees an
unauthenticated request. Use `ctx.keep(key, value)` and `ctx.peek(key)`
to pass data from a `BEFORE` hook down to the handler (a resolved user id,
say).

## Mounting sub-routers

For anything larger than a handful of routes, build independent `Router`s
and compose them with `app.mount(prefix, router)`, which merges the
child's routes under a path prefix. This keeps an API surface, an admin
surface, and the public site in separate files that each own their
routes:

```dragon
from http.server import Router, Request, Response, Context

# A sub-router for the API surface.
api: Router = Router()
api.GET("/ping",  lambda (req: Request, res: Response, ctx: Context) -> None { res.json("{\"pong\": true}") })
api.POST("/echo", lambda (req: Request, res: Response, ctx: Context) -> None { res.json(req.json()) })

app: Router = Router(8733, "127.0.0.1")
app.mount("/api/v1", api)               # api routes now live under /api/v1
app.GET("/", lambda (req: Request, res: Response, ctx: Context) -> None { res.text("root") })

app.listen()
```

`GET /` answers `root`, `GET /api/v1/ping` answers `{"pong": true}`, and
a `POST /api/v1/echo` with `{"hi":"there"}` echoes it straight back.
`mount` preserves the child's middleware and subdomain assignments too,
so a fully-configured sub-router drops in whole.

## Sessions and CSRF

Two calls turn on the security middleware most form-driven apps need.
`app.enable_sessions(secret)` enables signed-cookie sessions: the session
data lives in an HMAC-signed cookie under the given secret, so there is
**no server-side store to grow unbounded** - it is leak-free by
construction, the Flask/itsdangerous model. `app.enable_csrf()` builds on
that to require a CSRF token on unsafe methods (anything but `GET`,
`HEAD`, `OPTIONS`, `TRACE`):

```dragon
from http.server import Router, Request, Response, Context
from secrets import token_bytes

app: Router = Router(8736, "127.0.0.1")

# Turn on signed-cookie sessions and CSRF protection.
secret: bytes = token_bytes(32)
app.enable_sessions(secret)
app.enable_csrf()

app.GET("/", lambda (req: Request, res: Response, ctx: Context) -> None {
    res.text("ok")
})

app.listen()
```

Use `secrets.token_bytes(32)` to mint the secret - and in production load
a *stable* secret from the environment, not a fresh random one per boot,
or every restart invalidates every session. Once enabled, CSRF tokens are
verified automatically before unsafe handlers run.

> **Known limitation (current build).** Enabling sessions and CSRF works
> and is safe to ship. The per-request **read/write** API on `req.session`
> (`get`/`set`/`has`/`delete`) is **not yet reliable** - a `get` with a
> default can come back empty and a `set` may not round-trip through the
> cookie. Until that is fixed, treat `enable_sessions`/`enable_csrf` as
> the working surface (signed-cookie issuance + CSRF enforcement) and
> don't build handler logic on reading values back out of the session
> yet. Tracked in `compiler-the issue tracker` (H12).

## How it runs: one green thread per connection

`app.listen()` is the accept loop, and it is built on Dragon's green
threads (see [Concurrency](/docs/1101-green-threads)). Each accepted
connection is handed to a `fire`d vthread that runs the keep-alive loop
for that client - frame a request, route it, serialize the response,
send it, repeat until the client closes. Because the threads are
M:N-scheduled green threads, not OS threads, a Dragon server holds tens
of thousands of concurrent connections without a thread-per-connection
memory blowup, and a handler that blocks on I/O (a database round-trip, a
downstream HTTP call) yields the scheduler to other connections instead
of stalling them.

This is the same engine that serves dragonlang.org. The site's
`main.dr` reads markdown files, renders them, and serves them through
exactly the `Router`/`Request`/`Response` API in this chapter - the
documentation you are reading was delivered by the stack it describes.

## At a glance

| You want to... | Write |
|----------------|-------|
| Make a server | `app: Router = Router(port, host)` |
| Register a route | `app.GET("/path", handler)` (also POST/PUT/PATCH/DELETE/...) |
| Capture a path param | `"/players/:id"` then `req.params["id"]` |
| Type-check a path param | `"/players/:id:int"` then `req.param_int("id")` |
| Read a query param | `req.query_str("q", "default")` (typed: `query_int`, ...) |
| Read a form body | `form: dict[str, str] = req.form()` |
| Read a JSON body | `body: str = req.json()` (parse with the `json` module) |
| Send HTML / text / JSON | `res.html(s)` / `res.text(s)` / `res.json(s)` |
| Set status + body | `res.out(404, "Not Found")` |
| Redirect | `res.redirect("/")` (307) / `res.redirect("/", true)` (308) |
| Set a header / cookie | `res.set_header(k, v)` / `res.cookie(name, value)` |
| Escape user text into HTML | `escape(s)` or `template[HTML] { ... !{x} }` |
| Query the database | `db.all(template[SQL] { ... })` |
| Mutate the database | `db.run(template[SQL] { insert ... !{x} })` |
| Run DDL | `db.raw("create table ...")` |
| Serve static files | `app.ASSETS("public", "/assets/*")` |
| Run code before/after routes | `app.BEFORE("/*", h)` / `app.AFTER("/*", h)` |
| Compose routers | `app.mount("/api/v1", api)` |
| Enable sessions + CSRF | `app.enable_sessions(secret)` / `app.enable_csrf()` |
| Start serving (blocks) | `app.listen()` |

The whole framework rests on the same idea as the rest of the language:
the file you run is the program, handlers are ordinary typed functions,
and the dangerous paths - raw SQL, unescaped HTML - are the ones the type
system and the standard library make hard to reach by accident. For the
query DSL the handlers lean on, see [Databases](/docs/1301-databases) and
[Templates](/docs/1201-templates); for the green-thread engine under
`listen()`, see [Concurrency](/docs/1101-green-threads).

