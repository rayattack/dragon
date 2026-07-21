# Decision 013: Built-in HTTP Server

> **Status:** Implemented. pure-Dragon Router/Request/Response/Context in `stdlib/http/server.dr`, dogfooding the Heaven router API; built on socket + ssl + urllib.parse.

I spent a week staring at Heaven's `.venv` install (Python/ASGI) at 2am with cold coffee, trying to figure out why our Dragon HTTP spike felt like duct tape. Go has `net/http`, Deno has `Deno.serve`, Python has `http.server` (fine for demos, useless in prod). We want `dr build` → serving traffic with zero extra deps and LLVM-native speed, not another Python runtime stack.

So I'm shipping HTTP in the stdlib: `from http import HTTPServer`, `from http.server import Router`. Dragon-native, compiled in, not a wrapper around someone else's framework.

The API follows Heaven's capitalized-method convention (`.GET`, `.POST`, `.BEFORE`, `.AFTER`) because I already had that muscle memory from debugging route tables at 3am. Dragon-specific bits on top: compile-time handler resolution, typed route params, subdomain routing, router mounting, WebSockets, built-in CORS/sessions, dot-access on request objects. The main pitch vs Python ASGI is still speed - no interpreter in the hot path.

## Module Structure

```
http/
 __init__.dr -> exports HTTPServer, Request, Response, Context, Key
 server.dr -> exports Router (alias: App, Application, Server)
 status.dr -> exports status code constants
 websocket.dr -> exports WebSocket types
 cors.dr -> exports CORS middleware
 sessions.dr -> exports session middleware + SecureSerializer
```

## Core Types

### Request

```dragon
class Request {
 // HTTP basics
 method: str // "GET", "POST", etc.
 path: str // "/v1/customers/42"
 url: str // full URL path
 scheme: str // "http" or "https"
 host: str // Host header value

 // Parsed components
 params: dict[str, Any] // route params: {id: 42} (typed via :id:int)
 query: dict[str, Any] // query string: {page: 1} (typed via ?page:int)
 headers: dict[str, str] // request headers (lowercased keys)
 cookies: dict[str, str] // parsed cookies (lazy, parsed on first access)

 // Body
 body: bytes // raw body bytes
 _raw_body: str // raw body as string

 // Routing metadata
 route: str // matched route pattern e.g. "/v1/customers/:id:int"
 subdomain: str // detected subdomain (default: "www")
 app: Router // application instance
 mounted: Router = None // parent router if mounted

 // Client info
 ip: str // client IP address
 port: int // client port

 // Methods
 def json -> dict[str, Any] {
 return json.loads(self._raw_body)
 }
}
```

In `.dr` mode, request fields use dot-access :
```dragon
const name: str = req.params.name // dot-access on dict
const page: int = req.query.page // typed via route query hints
```

### Response

```dragon
class Response {
 status: int = 200 // default 200 OK
 headers: dict[str, str] = {} // response headers
 app: Router // back-reference to app (injected)
 _body: str = "" // serialized body
 _aborted: bool = false // short-circuit flag

 // Body setters
 def json(data: Any) -> Response {
 self.headers["content-type"] = "application/json"
 self._body = json.dumps(data)
 return self
 }

 def text(data: str) -> Response {
 self.headers["content-type"] = "text/plain"
 self._body = data
 return self
 }

 def html(data: str) -> Response {
 self.headers["content-type"] = "text/html"
 self._body = data
 return self
 }

 // Navigation
 def redirect(url: str, permanent: bool = false) -> None {
 self.status = 308 if permanent else 307
 self.headers["location"] = url
 }

 // File serving
 def file(filepath: str, filename: str = "") -> Response {
 // Sets Content-Type from MIME detection, streams file
 return self
 }

 // Short-circuit pipeline
 def abort(payload: Any = None) -> None {
 self._aborted = true
 if payload != None {
 self.status = 404
 self._body = str(payload)
 }
 }

 // Deferred tasks (run after response is sent)
 def defer(task: fn(app: Router) -> None) -> None {
 self._deferred_tasks.append(task)
 }

 // Cookie setting
 def cookie(name: str, value: str, max_age: int = 0,
 path: str = "/", secure: bool = false,
 httponly: bool = false, samesite: str = "Lax") -> Response {
 // Set-Cookie header
 return self
 }

 // Convenience: set status + body + headers in one call
 def out(status: int, body: Any, headers: dict[str, str] = {}) -> Response {
 self.status = status
 self.json(body) if isinstance(body, dict) else self.text(str(body))
 self.headers.update(headers)
 return self
 }
}
```

### Context

```dragon
class Context {
 app: Router // application instance
 _data: dict[str, Any] = {} // per-request state bag
 session: dict[str, Any] = {} // session data (if sessions enabled)

 // Typed key storage
 def keep(key: str, value: Any) -> None {
 self._data[key] = value
 }

 def peek(key: str) -> Any {
 return self._data.get(key, None)
 }

 def unkeep(key: str) -> Any {
 return self._data.pop(key, None)
 }
}
```

In `.dr` mode, context supports dot-access for stored values:
```dragon
ctx.keep("user_id", 42)
const uid: int = ctx.user_id // dot-access via __getattr__ fallback
// Or explicitly:
const uid: int = ctx.peek("user_id")
```

### Typed Keys (compile-time type safety)

```dragon
from http import Key

const USER_KEY: Key[dict] = Key("user")
ctx.keep(USER_KEY, {name: "Alice", role: "admin"})
const user: dict = ctx.peek(USER_KEY) // type-safe retrieval
```

## Router API

### Construction

```dragon
from http.server import Router // also: App, Application, Server

const app: Router = Router({
 host: "0.0.0.0", // bind address
 port: 8000, // listen port
 workers: 4, // thread pool size
 debug: true // show detailed errors
})

// Or with config accessor
const db_url: str = app.CONFIG("database_url")
```

### HTTP Method Registration

All HTTP methods use capitalized names. Every method takes `(route, handler, subdomain)`:

```dragon
app.GET(route: str, handler: Handler | str, subdomain: str = "www")
app.POST(route: str, handler: Handler | str, subdomain: str = "www")
app.PUT(route: str, handler: Handler | str, subdomain: str = "www")
app.PATCH(route: str, handler: Handler | str, subdomain: str = "www")
app.DELETE(route: str, handler: Handler | str, subdomain: str = "www")
app.HEAD(route: str, handler: Handler | str, subdomain: str = "www")
app.OPTIONS(route: str, handler: Handler | str, subdomain: str = "www")
app.HTTP(route: str, handler: Handler | str, subdomain: str = "www") // all methods
```

### Handler Signature

Every route handler and middleware has the same signature:

```dragon
type Handler = fn(req: Request, res: Response, ctx: Context) -> None
```

The trio `(req, res, ctx)` is **freshly allocated per request**: - `req` is populated from the parsed HTTP request - `res` is initialized with `status = 200` and `app` back-reference - `ctx` is initialized with empty state and `app` back-reference

### Route Patterns

| Pattern | Matches | `req.params` |
|---------|---------|-------------|
| `/users` | Exact match | `{}` |
| `/users/:id` | `/users/42` | `{id: "42"}` |
| `/users/:id:int` | `/users/42` (validates int) | `{id: 42}` (typed) |
| `/users/:id:int/posts/:pid:int` | Multi-param | `{id: 42, pid: 7}` |
| `/files/*` | `/files/a/b/c` | `{"*": "a/b/c"}` |
| `/search?q:str&page:int` | Query hints | `req.query = {q: "hello", page: 1}` |

**Supported param types:** `int`, `str`, `float`, `bool`, `uuid`

Failed type coercion returns 400 Bad Request automatically.

### Query String Type Hints

Route definitions can include query parameter type hints after `?`:

```dragon
app.GET("/search?q:str&page:int&active:bool", search_handler)

// In handler:
def search_handler(req: Request, res: Response, ctx: Context) -> None {
 const query: str = req.query.q // string
 const page: int = req.query.page // auto-coerced to int
 const active: bool = req.query.active // auto-coerced to bool
}
```

## Middleware

### BEFORE / AFTER Hooks

```dragon
app.BEFORE(route: str, handler: Handler, subdomain: str = "www")
app.AFTER(route: str, handler: Handler, subdomain: str = "www")
```

**Matching rules:** Hooks match by specificity: 1. Exact match: `/users/42` matches BEFORE/AFTER on `/users/42` 2. Wildcard ancestors: `/users/42` also matches `/users/*` and `/*` 3. Applied in order: most specific first

```dragon
app.BEFORE("/*", log_all) // runs for ALL routes
app.BEFORE("/v1/*", auth_v1) // runs for /v1/* routes
app.BEFORE("/v1/admin/*", admin_check) // runs for /v1/admin/* routes

// Request to /v1/admin/users runs: log_all → auth_v1 → admin_check → handler
```

**Deduplication:** Same handler registered twice on the same pattern runs only once.

### Short-Circuit via `res.abort`

```dragon
def auth_middleware(req: Request, res: Response, ctx: Context) -> None {
 const token: str = req.headers.get("authorization", "")
 if token == "" {
 res.abort
 res.status = 401
 res.json({error: "unauthorized"})
 return // remaining BEFORE hooks + handler skipped
 }
 ctx.keep("user_id", validate_token(token))
}
```

When `res.abort` is called: - Remaining BEFORE middleware is skipped - The route handler is skipped
- AFTER middleware **still runs** (for cleanup, logging, CORS headers) - Response is serialized and sent

## Lifecycle Hooks

```dragon
// Startup - runs before server accepts connections
app.ONCE(connect_database) // shorthand
app.ONCE(HTTPServer.STARTUP, connect_database) // explicit
app.ON(HTTPServer.STARTUP, connect_database) // alias for ONCE

// Shutdown - runs when server is stopping
app.ONCE(HTTPServer.SHUTDOWN, close_database)
```

| Event | When | Signature |
|-------|------|-----------|
| `HTTPServer.STARTUP` | Before accepting connections | `fn(app: Router) -> None` |
| `HTTPServer.SHUTDOWN` | After last connection closes | `fn(app: Router) -> None` |

## Subdomain Routing

```dragon
// Create subdomain-scoped context
const api: SubdomainRouter = app.subdomain("api")

// Register routes on subdomain
api.GET("/users", list_users) // matches api.example.com/users
api.POST("/users", create_user)
api.BEFORE("/users/*", auth_middleware)

// Or inline subdomain parameter
app.GET("/dashboard", dashboard_handler, subdomain="admin")
// matches admin.example.com/dashboard
```

**Subdomain detection:** - Extracted from Host header automatically - IP addresses and single-label hosts default to `"www"` - Wildcard subdomain `"*"` matches any subdomain

## Router Mounting

Mount a sub-router onto a parent for modular application structure:

```dragon
// users_router.dr
from http.server import Router

const users: Router = Router
users.GET("/", list_users)
users.GET("/:id:int", get_user)
users.POST("/", create_user)
users.PUT("/:id:int", update_user)
users.DELETE("/:id:int", delete_user)

// app.dr
from http.server import Router
from users_router import users
from orders_router import orders

const app: Router = Router({host: "0.0.0.0", port: 8000})

app.mount("/v1/users", users) // /v1/users, /v1/users/:id:int, etc.
app.mount("/v1/orders", orders) // /v1/orders, /v1/orders/:id:int, etc.

app.listen
```

**Mount behavior:** - Child routes are prefixed with the mount path - Child BEFORE/AFTER hooks are scoped to mounted prefix - Parent BEFORE/AFTER hooks still apply (run first) - `req.mounted` is set to the child router in mounted handlers - Isolated by default - child router state is independent

## WebSocket Support

```dragon
from http import HTTPServer
from http.server import Router

const app: Router = Router({port: 8000})

app.SOCKET("/ws", ws_handler) // register WebSocket route
app.WEBSOCKET("/ws", ws_handler) // alias
app.WS("/ws", ws_handler) // alias
```

**WebSocket handler signature:**

```dragon
type WSHandler = fn(sender: fn(str) -> None,
 receiver: fn -> str,
 req: Request,
 ctx: Context) -> None

def ws_handler(sender: fn(str) -> None, receiver: fn -> str,
 req: Request, ctx: Context) -> None {
 // sender(data) - send message to client
 // receiver - block until message from client (returns str)

 const msg: str = receiver
 sender(f"Echo: {msg}")
}
```

**WebSocket lifecycle:** 1. Client sends HTTP Upgrade request 2. Server accepts upgrade automatically 3. Handler receives `sender`/`receiver` function pair 4. Handler can send/receive in a loop 5. Connection closes when handler returns or client disconnects

## Built-in CORS Middleware

```dragon
// Simple - allow everything
app.cors(origin="*", methods="*", headers="*")

// Restrictive
app.cors(
 origin=["https://example.com", "https://app.example.com"],
 methods="GET,POST,PUT,DELETE",
 headers="Content-Type,Authorization",
 credentials=true,
 max_age=3600
)

// Custom handler
app.cors(handler=custom_cors_function)
```

Registers BEFORE + AFTER middleware that: - Sets `Access-Control-Allow-Origin` and related headers
- Handles OPTIONS preflight requests automatically - Supports credential mode and max-age caching

## Built-in Sessions Middleware

```dragon
app.sessions(secret_key="your-secret", cookie_name="session", max_age=3600)

def dashboard(req: Request, res: Response, ctx: Context) -> None {
 const user_id: int = ctx.session.user_id // dot-access
 if user_id == None {
 res.redirect("/login")
 return
 }
 res.json({user: user_id})
}
```

**Session flow:** 1. BEFORE middleware: reads signed cookie, decodes, populates `ctx.session` 2. Handler runs, can read/modify `ctx.session` 3. AFTER middleware: if session modified, signs and sets cookie

**Secure serialization:** Sessions are signed with HMAC-SHA256. Tampered cookies are rejected. Expired sessions (beyond `max_age`) are discarded.

## Static File Serving

```dragon
app.ASSETS("public", route="/static/*")

// Serves:
// GET /static/css/style.css → ./public/css/style.css
// GET /static/js/app.js → ./public/js/app.js
```

**Features:** - MIME type detection from file extension - `sendfile` for zero-copy transfer on Linux - Configurable directory path - 404 for missing files

**Manual file serving in handlers:**

```dragon
def download(req: Request, res: Response, ctx: Context) -> None {
 res.file("reports/q4.pdf", filename="Q4-Report.pdf")
}
```

## App-Level Storage

```dragon
// Store shared state on the app (database pools, config, caches)
app.keep("db", database_pool)
app.keep("cache", cache_instance)

// Access in handlers via ctx.app
def handler(req: Request, res: Response, ctx: Context) -> None {
 const db: Any = ctx.app.peek("db")
 const result: dict = db.query("SELECT * FROM users")
 res.json(result)
}
```

## Deferred Tasks (Background Work)

```dragon
def create_user(req: Request, res: Response, ctx: Context) -> None {
 const user: dict = req.json
 // ... insert into database ...

 // Run AFTER response is sent to client
 res.defer(fn(app: Router) -> None {
 send_welcome_email(user.email)
 })

 res.status = HTTPServer.CREATED
 res.json(user)
}
```

Deferred tasks run after the response bytes are written to the socket. The client receives a fast response; slow operations (email, analytics, logging) happen afterward.

## Status Codes

```dragon
class HTTPServer {
 // 2xx
 static const OK: int = 200
 static const CREATED: int = 201
 static const ACCEPTED: int = 202
 static const NO_CONTENT: int = 204

 // 3xx
 static const MOVED_PERMANENTLY: int = 301
 static const FOUND: int = 302
 static const NOT_MODIFIED: int = 304
 static const TEMPORARY_REDIRECT: int = 307
 static const PERMANENT_REDIRECT: int = 308

 // 4xx
 static const BAD_REQUEST: int = 400
 static const UNAUTHORIZED: int = 401
 static const FORBIDDEN: int = 403
 static const NOT_FOUND: int = 404
 static const METHOD_NOT_ALLOWED: int = 405
 static const CONFLICT: int = 409
 static const GONE: int = 410
 static const UNPROCESSABLE_ENTITY: int = 422
 static const TOO_MANY_REQUESTS: int = 429

 // 5xx
 static const INTERNAL_SERVER_ERROR: int = 500
 static const BAD_GATEWAY: int = 502
 static const SERVICE_UNAVAILABLE: int = 503
 static const GATEWAY_TIMEOUT: int = 504

 // Lifecycle event constants
 static const STARTUP: str = "startup"
 static const SHUTDOWN: str = "shutdown"
}
```

## Compile-Time Handler Resolution

String-based handler references (`"controllers.orders.list_all"`) are resolved at **compile time**:

1. **Parser** sees `app.GET("/path", "controllers.orders.list_all")` 2. **Sema/CodeGen** recognizes the string literal as a module path 3. Multi-file compilation resolves `controllers/orders.dr` → finds `list_all` function 4. **TypeChecker** verifies `list_all` matches the `Handler` signature 5. **CodeGen** emits a direct function pointer, not a string lookup

```llvm
; "controllers.orders.list_all" compiles to a direct function pointer:
call void @dragon_router_add_route(
 ptr %router,
 ptr @method_GET,
 ptr @path_str,
 ptr @controllers_orders_list_all ; direct, no runtime lookup
)
```

Both `app.GET("/path", handler_func)` and `app.GET("/path", "module.func")` compile to identical code - a function pointer registration. The string form exists for configuration ergonomics when handler references are in config dicts.

## Request Lifecycle

```
 1. accept(fd)
 2. read(fd, buffer)
 3. llhttp_execute(parser, buffer) → method, path, headers, body
 4. Extract subdomain from Host header
 5. Router.match(method, path, subdomain) → handler + params + middleware chain
 6. Allocate (req, res, ctx) trio
 7. Coerce typed route params (:id:int → int)
 8. Coerce typed query params (?page:int → int)
 9. Run BEFORE middleware (most specific first)
10. If NOT res._aborted:
 Run handler(req, res, ctx)
11. Run AFTER middleware (always, even on abort)
12. Run deferred tasks (if any)
13. Serialize res → HTTP response bytes
14. write(fd, response)
15. Close or keep-alive
```

## Architecture

### Runtime Library: `libdragon_http.a`

```
┌──────────────────────────────────────────────────────┐
│ Compiled Dragon Binary │
│ ┌──────────────────────────────────────────────┐ │
│ │ User Code (LLVM IR → object code) │ │
│ └──────────────┬───────────────────────────────┘ │
│ │ calls │
│ ┌──────────────▼───────────────────────────────┐ │
│ │ libdragon_http.a │ │
│ │ ┌────────────────────────────────────────┐ │ │
│ │ │ TCP Server (socket/bind/listen) │ │ │
│ │ │ Thread Pool (pthreads, N workers) │ │ │
│ │ │ HTTP Parser (llhttp) │ │ │
│ │ │ Router (radix trie, subdomain-aware) │ │ │
│ │ │ Middleware Chain (BEFORE/AFTER) │ │ │
│ │ │ WebSocket Upgrade + Frame Parser │ │ │
│ │ │ Cookie Parser / Session Signer │ │ │
│ │ │ CORS Handler │ │ │
│ │ │ Static File Server (sendfile) │ │ │
│ │ │ Request/Response Builders │ │ │
│ │ └────────────────────────────────────────┘ │ │
│ └──────────────┬───────────────────────────────┘ │
│ │ calls │
│ ┌──────────────▼───────────────────────────────┐ │
│ │ libdragon_runtime.a │ │
│ │ (strings, lists, dicts, json, bytes, etc.) │ │
│ └──────────────────────────────────────────────┘ │
│ │ links │
│ ┌──────────────▼───────────────────────────────┐ │
│ │ System: libc, libpthread │ │
│ └──────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────┘
```

### Concurrency Model

**Phase 1: Thread pool (no async required)**

```
Main Thread Worker Threads (N)
 │ │
 ▼ │
 socket │
 bind │
 listen │
 │ │
 ▼ │
 ┌─accept──────────┐ │
 │ new connection │────────► Thread picks up fd
 │ │ │
 │ new connection │────────► Thread picks up fd
 │ │ ▼
 │ ... │ read HTTP request
 └────────────────────┘ parse headers + body
 detect subdomain
 match route + middleware
 build (req, res, ctx) trio
 run BEFORE chain
 run handler
 run AFTER chain
 serialize Response
 write HTTP response
 run deferred tasks
 close(fd) or keep-alive
```

**Phase 2: epoll/kqueue event loop (requires async codegen)**

When async/await codegen lands, the server can switch to an event loop model for higher concurrency. The API remains identical - handlers become `async def`.

### HTTP Parsing

Embed **llhttp** (Node.js's HTTP parser): - 2 files: `llhttp.h` + `llhttp.c` - Zero-allocation, callback-based - Handles HTTP/1.1 with chunked encoding, keep-alive - MIT licensed - Battle-tested (every Node.js server uses it)

### Router: Radix Trie (Subdomain-Aware)

Route matching uses a **radix trie** per subdomain:

```
www (default):
 /
 ├── v1/
 │ ├── customers/
 │ │ ├── (GET) → list_all
 │ │ └── :id:int/
 │ │ ├── (GET) → get_one
 │ │ ├── (PUT) → update
 │ │ └── (DELETE) → delete
 │ └── orders/
 │ ├── (GET) → list_all
 │ └── (POST) → create
 └── static/* → file server

api:
 /
 ├── users/
 │ ├── (GET) → list_users
 │ └── (POST) → create_user
 └── health (GET) → health_check
```

- O(path-length) matching, not O(num-routes) - Parameter segments (`:id:int`) extracted and typed during traversal - Wildcard segments (`*`) match any suffix - Per-subdomain trie isolation

## Full Example

```dragon
// server.dr
from http import HTTPServer, Key
from http.server import Router
from json import dumps, loads
from datetime import datetime

const app: Router = Router({
 host: "0.0.0.0",
 port: 8000,
 workers: 4,
 debug: true
})

// Typed context key
const DB_KEY: Key[dict] = Key("db")

// Lifecycle: connect to database on startup
app.ONCE(fn(app: Router) -> None {
 const db: dict = connect_database
 app.keep("db", db)
 print("Database connected")
})

// Lifecycle: cleanup on shutdown
app.ONCE(HTTPServer.SHUTDOWN, fn(app: Router) -> None {
 app.peek("db").close
 print("Database disconnected")
})

// Middleware: log every request
app.BEFORE("/*", fn(req: Request, res: Response, ctx: Context) -> None {
 const now: str = datetime.now.isoformat
 print(f"[{now}] {req.method} {req.path}")
})

// Middleware: CORS
app.cors(origin="*", methods="GET,POST,PUT,DELETE", headers="Content-Type,Authorization")

// Middleware: auth for /v1/* routes
def auth(req: Request, res: Response, ctx: Context) -> None {
 const token: str = req.headers.get("authorization", "")
 if token == "" {
 res.abort
 res.status = HTTPServer.UNAUTHORIZED
 res.json({error: "missing token"})
 return
 }
 ctx.keep("user_id", validate_token(token))
}
app.BEFORE("/v1/*", auth)

// Routes: customers
app.GET("/v1/customers", "controllers.customers.list_all")
app.GET("/v1/customers/:id:int", "controllers.customers.get_one")
app.POST("/v1/customers", "controllers.customers.create")
app.PUT("/v1/customers/:id:int", "controllers.customers.update")
app.DELETE("/v1/customers/:id:int", "controllers.customers.delete")

// Routes: search with typed query params
app.GET("/v1/search?q:str&page:int&limit:int", fn(req: Request, res: Response, ctx: Context) -> None {
 const query: str = req.query.q
 const page: int = req.query.page
 res.json({query: query, page: page, results: []})
})

// Static files
app.ASSETS("public", route="/static/*")

// WebSocket
app.WS("/ws/chat", fn(sender: fn(str) -> None, receiver: fn -> str,
 req: Request, ctx: Context) -> None {
 while true {
 const msg: str = receiver
 if msg == "" { break }
 sender(f"Echo: {msg}")
 }
})

// Subdomain routing
const admin: SubdomainRouter = app.subdomain("admin")
admin.GET("/dashboard", "admin.views.dashboard")
admin.GET("/users", "admin.views.user_list")

// Start server
app.listen(fn -> None {
 print(f"Server running on http://{app.host}:{app.port}")
})
```

## Dependencies

| Dependency | Required For | Status |
|------------|-------------|--------|
| Phase A | `__str__`, `__eq__` for Request/Response | Proposed |
| Phase C | `__getitem__` for headers/params dicts | Proposed |
| `json` | Request body parsing, response serialization | Proposed |
| `datetime` | Logging timestamps | Proposed |
| dot-access | `req.params.id`, `ctx.user_id` | Proposed |
| bare-key dicts | `{error: "message"}` in responses | Proposed |
| bytes | Raw request body, file I/O, WebSocket frames | Proposed |
| Multi-file compilation | String-based handler resolution | Working |
| Thread pool | Concurrent request handling | New (in libdragon_http) |
| llhttp | HTTP/1.1 parsing | External (2 files, MIT) |

## Implementation Plan

| Phase | Component | Estimated LOC | Description |
|-------|-----------|---------------|-------------|
| A | TCP server + thread pool | ~400 | socket/bind/listen/accept + pthreads work queue |
| B | HTTP parser integration | ~300 | Embed llhttp, parse to Dragon structs |
| C | Router (radix trie) | ~600 | Pattern matching, param extraction, typed params, subdomain tries |
| D | Request/Response/Context | ~400 | Dragon class definitions + runtime builders |
| E | Middleware chain | ~250 | BEFORE/AFTER chains, abort, deduplication |
| F | Lifecycle hooks + storage | ~150 | STARTUP/SHUTDOWN, app.keep/peek, deferred tasks |
| G | Handler resolution | ~200 | String-to-function compilation, signature validation |
| H | WebSocket | ~400 | HTTP upgrade, frame parser, sender/receiver API |
| I | CORS + Sessions | ~300 | Built-in middleware, cookie signing, HMAC-SHA256 |
| J | Static files + MIME | ~200 | Directory mapping, sendfile, MIME detection |
| K | Router mounting | ~200 | Sub-router prefix scoping, hook merging |

**Total: ~3,400 LOC in libdragon_http + ~300 LOC Dragon class definitions + ~80 tests**

## Affected Components

- **New library:** `lib/HTTP/` - `server.cpp`, `router.cpp`, `parser.cpp`, `websocket.cpp`, `cors.cpp`, `session.cpp`, `static.cpp` - **Build system:** CMake target `dragon_http`, linked when `from http import` is used - **StdlibRegistry:** Register `http`, `http.server`, `http.websocket` modules - **CodeGen:** Detect `from http import`, link `libdragon_http.a` - **Multi-file:** String handler resolution uses existing infrastructure - **Tests:** E2E tests that start server, send HTTP requests, verify responses

## Future Work

- **HTTP/2:** Requires multiplexing. Defer until async is available.
- **TLS/HTTPS:** Embed or link OpenSSL/LibreSSL. Phase 2.
- **Template rendering:** Jinja2-style template engine. Phase 2.
- **Schema validation:** `app.schema.POST("/users", expects=UserSchema)`. Phase 2.
- **OpenAPI docs:** Auto-generated from schema + routes. Phase 2.
- **Async handlers:** When async/await codegen lands, handlers can be `async def`.
 The thread pool model still works - async enables higher concurrency per thread.
- **Database drivers:** `from db.postgres import ...` - separate decision.
- **Plugin system:** `app.plugin(MyPlugin)` for extensibility. Phase 2.
