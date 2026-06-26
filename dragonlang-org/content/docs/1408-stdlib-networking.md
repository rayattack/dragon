# Networking

Dragon's networking stack is layered the way you would expect. At the bottom is `socket`, a thin set of wrappers over the BSD socket syscalls. Above that, `http.client` speaks HTTP/1.1 over a TCP stream. Above *that*, `urllib.request` adds redirect-following and a convenient `urlopen` entry point, and `urllib.parse` handles the URL and query-string bookkeeping that every HTTP client needs. The four modules compose top to bottom: `urllib.request` is written in terms of `http.client`, which is written in terms of `socket`.

One property runs through the whole stack and is worth stating up front: **the I/O is colorless and scheduler-aware**. The socket calls present a blocking API - `do_send` returns when the bytes are queued, `do_recv` returns when data arrives - but underneath, when a call would block, it yields the current green thread to the scheduler instead of parking the carrier OS thread. Issue your network work from a `fire`'d vthread (see [Concurrency](/docs/1101-green-threads)) and thousands of in-flight connections share a handful of OS threads. Off a green thread the same calls fall back to `poll()`, so nothing breaks. You write straight-line blocking code; the runtime makes it scale. This is the design spec's I/O model and it is the reason there is no `async`/`await` split in any of these signatures.

This chapter covers the **client** side. The HTTP *server* - routing, request parsing, the `App`/`Router` API - lives in its own chapter, [Web Application](/docs/1701-web-application); the minimal hand-rolled servers below exist only to give the client examples something to talk to.

## urllib.parse - URLs and query strings

`urllib.parse` is pure computation: no sockets, no I/O, just RFC 3986 string surgery. It is the one module here you can exercise without a network, and the one you will reach for most often. Import the functions you need:

```dragon
from urllib.parse import quote, unquote, quote_plus, unquote_plus
```

### Percent-encoding

`quote(s: str, safe: str = "/")` percent-encodes a string for use in a URL path. The RFC 3986 *unreserved* set (`A-Z a-z 0-9 - . _ ~`) is always left verbatim; the `safe` argument names additional characters to leave alone (default `/`, so path separators survive). `unquote(s: str)` reverses it. Bytes outside ASCII are UTF-8 encoded first, then each byte is escaped as `%HH` with uppercase hex.

```dragon
from urllib.parse import quote, unquote

print(quote("a b/c?d"))        # a%20b/c%3Fd
print(quote("a b/c?d", ""))    # a%20b%2Fc%3Fd   (slash now escaped)
print(unquote("a%20b%2Fc"))    # a b/c
```

`quote_plus(s: str, safe: str = "")` is the form-encoding variant: spaces become `+` and the default `safe` set is empty, so `/` is escaped too. `unquote_plus(s: str)` reverses it, turning `+` back into a space. This is the pair you want for `application/x-www-form-urlencoded` data.

```dragon
from urllib.parse import quote_plus, unquote_plus

original: str = "hello world & more=stuff"
encoded: str = quote_plus(original)
print(encoded)                  # hello+world+%26+more%3Dstuff
print(unquote_plus(encoded))    # hello world & more=stuff
```

### Building and parsing query strings

`urlencode(query: dict[str, str])` turns a dict into a `&`-joined query string, percent-encoding both keys and values with `quote_plus`:

```dragon
from urllib.parse import urlencode

params: dict[str, str] = {"q": "dragon lang", "page": "2"}
print(urlencode(params))   # q=dragon+lang&page=2
```

> **Differs from Python.** Dragon's `urlencode` takes a `dict[str, str]` only - there is no `doseq` argument and no list-of-pairs input. A key cannot repeat, because a Dragon dict cannot hold a duplicate key. To emit a repeated parameter, build the string yourself.

Going the other way, `parse_qsl(qs, keep_blank_values=False)` returns the pairs in order as a `list[tuple[str, str]]`, and `parse_qs(qs, keep_blank_values=False)` groups them into a `dict[str, list[str]]` so a repeated key collects all its values:

```dragon
from urllib.parse import parse_qsl, parse_qs

items: list[tuple[str, str]] = parse_qsl("a=1&b=2&a=3")
for kv in items {
    print(f"{kv[0]} -> {kv[1]}")
}
# a -> 1
# b -> 2
# a -> 3

grouped: dict[str, list[str]] = parse_qs("a=1&b=2&a=3")
for v in grouped["a"] {
    print(v)
}
# 1
# 3
```

By default a key with no value (`b=` or a bare `c`) is dropped, matching CPython. Pass `True` for `keep_blank_values` to retain them with an empty-string value:

```dragon
from urllib.parse import parse_qsl

for kv in parse_qsl("a=1&b=&c", True) {
    print(f"[{kv[0]}]=[{kv[1]}]")
}
# [a]=[1]
# [b]=[]
# [c]=[]
```

### Splitting and joining whole URLs

`urlsplit(url: str)` cracks a URL into its components and returns a `SplitResult` with fields `scheme`, `netloc`, `path`, `query`, and `fragment`. The result has a `geturl()` method that reassembles the original. `urlparse(url: str)` is the same but additionally peels a rarely-used `;params` segment off the path, returning a `ParseResult` with the extra `params` field.

```dragon
from urllib.parse import urlsplit, SplitResult

sr: SplitResult = urlsplit("https://example.com:8080/path/to?q=1#frag")
print(f"scheme={sr.scheme} netloc={sr.netloc} path={sr.path}")
# scheme=https netloc=example.com:8080 path=/path/to
print(sr.query)      # q=1
print(sr.fragment)   # frag
print(sr.geturl())   # https://example.com:8080/path/to?q=1#frag
```

`urljoin(base, ref)` resolves a reference against a base URL (RFC 3986 §5.3), and `urldefrag(url)` splits off the `#fragment`, returning a `tuple[str, str]` of the URL and the fragment:

```dragon
from urllib.parse import urljoin, urldefrag

print(urljoin("http://host/a/b", "/x"))   # http://host/x

frag: tuple[str, str] = urldefrag("http://h/p#section")
print(f"{frag[0]} | {frag[1]}")            # http://h/p | section
```

> **Differs from Python.** `urlsplit`/`urlparse` return plain classes with public attributes and a `geturl()` method - not the named-tuple-with-extra-properties CPython returns, so there is no `port`/`hostname`/`username` decomposition on the result and no tuple indexing. `urljoin` does the structural merge but does **not** normalize `..`/`.` path segments, so `urljoin("http://host/a/b/c", "../d")` yields `http://host/a/b/../d` rather than collapsing the dot segment.

## socket - TCP and UDP sockets

`socket` wraps the BSD socket syscalls behind three classes. It is deliberately object-oriented rather than mirroring CPython's single `socket.socket()` factory: a server uses `TcpListener`, a connection (either end) is a `TcpStream`, and datagrams go through `UdpSocket`.

`TcpStream.open(host: str, port: int)` is a static method that resolves the host, connects, and returns a connected stream. On it you call `do_send(data: str) -> int`, `do_recv(size: int) -> str`, and `close() -> int`. For binary protocols there are `send_bytes(data: bytes)` and `recv_bytes(size: int) -> bytes` on the same scheduler-yielding path. A `TcpStream` is a context manager, so `with` closes it for you.

On the server side, construct `TcpListener(host, port)`, then call `do_bind()`, `do_listen(backlog)`, and `do_accept() -> TcpStream`. `gethostbyname(host: str) -> str` resolves a name to a dotted IPv4 string (a literal IP passes through unchanged):

```dragon
from socket import gethostbyname

print(gethostbyname("127.0.0.1"))   # 127.0.0.1
```

Here is a complete loopback echo. Note the ordering: the listener binds and listens on the main thread *before* anything connects, and only the blocking `do_accept()` runs on a fired vthread. That guarantees the socket is listening before the client dials it.

```dragon
from socket import TcpListener, TcpStream

# Bind + listen first so the socket is ready before the client connects.
listener: TcpListener = TcpListener("127.0.0.1", 8765)
listener.do_bind()
listener.do_listen(1)

def serve() -> None {
    conn: TcpStream = listener.do_accept()
    data: str = conn.do_recv(1024)
    conn.do_send(f"echo:{data}")
    conn.close()
}

server: Task[None] = fire serve()

with TcpStream.open("127.0.0.1", 8765) as client {
    client.do_send("hello")
    print(client.do_recv(1024))   # echo:hello
}
server.join()
listener.close()
```

`UdpSocket(host, port)` rounds out the module for datagrams, with `do_bind()`, `sendto(data, host, port) -> int`, `recvfrom(size) -> str`, and `close()`.

> **Differs from Python.** There is no single `socket.socket()` object with `connect`/`bind`/`send`/`recv` methods and a family/type argument - Dragon splits the roles into `TcpListener`, `TcpStream`, and `UdpSocket`, and the read/write methods are named `do_send`/`do_recv`/`do_bind`/`do_accept` (the `do_` prefix keeps `accept`/`bind` free as ordinary words and sidesteps clashes with the libc syscalls of the same name that the module imports). Sockets are IPv4-only (`AF_INET`); there is no IPv6, no `socket.gethostname()`, and no address-family/socket-type constants - the variants are the three classes. Every `TcpStream` sets `TCP_NODELAY` in its constructor, so request/response protocols do not eat a Nagle/delayed-ACK stall.

## http.client - HTTP/1.1 requests

`http.client` speaks HTTP/1.1 over a `TcpStream`. The shape matches CPython's `http.client`: construct a connection, call `request`, then `getresponse`.

`HTTPConnection(host: str, port: int)` creates a (not-yet-connected) connection. `request(method, url, body, headers)` sends the request line plus headers and an optional body; all four arguments are required - pass `""` for an empty body and an empty `dict[str, str]` for no extra headers. `getresponse() -> HTTPResponse` reads and parses the reply.

The `HTTPResponse` exposes `status: int`, `reason: str`, and `headers: dict[str, str]` as fields, plus `read() -> str` (the body, returned once - a second `read()` yields `""`), `getheader(name, default="") -> str` (case-insensitive), and `getheaders() -> list[tuple[str, str]]`.

```dragon
from socket import TcpListener, TcpStream
from http.client import HTTPConnection, HTTPResponse

# A one-shot HTTP server to answer the request below.
listener: TcpListener = TcpListener("127.0.0.1", 8767)
listener.do_bind()
listener.do_listen(1)

def serve() -> None {
    conn: TcpStream = listener.do_accept()
    conn.do_recv(4096)
    conn.do_send("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\npong")
    conn.close()
}

server: Task[None] = fire serve()

http_conn: HTTPConnection = HTTPConnection("127.0.0.1", 8767)
no_headers: dict[str, str] = {}
http_conn.request("GET", "/ping", "", no_headers)
r: HTTPResponse = http_conn.getresponse()
print(f"{r.status} {r.reason}")           # 200 OK
print(r.getheader("Content-Type"))        # text/plain
print(r.read())                           # pong
http_conn.close()
server.join()
listener.close()
```

> **Differs from Python.** `request` has no default arguments - CPython lets you call `conn.request("GET", "/")` with body and headers defaulted, but in Dragon you must pass all four (`""` and `{}`). The connection always sends `Connection: close` and `getresponse()` reads until EOF, so there is no `Content-Length`/chunked early-exit yet and a connection is single-use (open a fresh `HTTPConnection` per request). `read()` takes no `amt` argument - the full body is buffered. There is an `HTTPSConnection` with the same shape, backed by an `ssl.SSLSocket`; see [Cryptography and Hashing](/docs/1407-stdlib-crypto) for the TLS layer it builds on.

## urllib.request - high-level HTTP

`urllib.request` sits on top of `http.client` and adds the two conveniences most client code wants: a one-call `urlopen` and automatic redirect following (up to five 3xx hops, resolving relative `Location` headers against the previous URL). It returns the underlying `http.client.HTTPResponse`, so you inspect `status`/`headers`/`read()` exactly as above.

`urlopen(target: str) -> HTTPResponse` issues a GET for a URL string. For anything beyond a plain GET, build a `Request(url, data, method)` - its `add_header(key, value)` extends the headers, and `get_method()` defaults to `POST` when `data` is non-empty and `GET` otherwise - then hand it to `urlopen_request(req)`.

```dragon
from socket import TcpListener, TcpStream
from urllib.request import urlopen, urlopen_request, Request
from http.client import HTTPResponse

# Server that answers two requests, then exits.
listener: TcpListener = TcpListener("127.0.0.1", 8768)
listener.do_bind()
listener.do_listen(2)

def serve(n: int) -> None {
    i: int = 0
    while i < n {
        conn: TcpStream = listener.do_accept()
        conn.do_recv(4096)
        conn.do_send("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello")
        conn.close()
        i = i + 1
    }
}

server: Task[None] = fire serve(2)

r1: HTTPResponse = urlopen("http://127.0.0.1:8768/")
print(f"{r1.status}: {r1.read()}")        # 200: hello

req: Request = Request("http://127.0.0.1:8768/submit", "x=1", "POST")
req.add_header("User-Agent", "Dragon/0.2")
r2: HTTPResponse = urlopen_request(req)
print(f"{r2.status}: {r2.read()}")        # 200: hello

server.join()
listener.close()
```

> **Differs from Python.** `Request(url, data, method)` takes three positional arguments - there are no `headers=`/`origin_req_host=` keywords; build the object and then call `add_header`. The dispatch is split: `urlopen` takes a `str`, while a `Request` goes through `urlopen_request` (CPython overloads a single `urlopen` over both). Only `http://` is supported - `urlopen("https://...")` raises `ValueError` because the high-level layer is not yet wired to TLS (use `http.client.HTTPSConnection` directly for HTTPS). `data` is a `str`, not `bytes`.

## At a glance

| Module | Import | Key entry points | Notes |
|---|---|---|---|
| `urllib.parse` | `from urllib.parse import quote, urlencode, urlsplit` | `quote`/`unquote`/`quote_plus`/`unquote_plus`, `urlencode`, `parse_qs`/`parse_qsl`, `urlsplit`/`urlparse`, `urljoin`, `urldefrag` | Pure, no I/O; `urlencode` is dict-only; `urljoin` does not normalize `..` |
| `socket` | `from socket import TcpListener, TcpStream, UdpSocket` | `TcpStream.open`/`do_send`/`do_recv`, `TcpListener.do_bind`/`do_listen`/`do_accept`, `UdpSocket`, `gethostbyname` | IPv4 only; three role classes, not one factory; scheduler-yielding I/O; `with` closes |
| `http.client` | `from http.client import HTTPConnection, HTTPResponse` | `HTTPConnection.request`/`getresponse`, `HTTPResponse.read`/`getheader`/`status` | `request` needs all four args; `Connection: close`, one request per connection; `HTTPSConnection` for TLS |
| `urllib.request` | `from urllib.request import urlopen, Request` | `urlopen`, `urlopen_request`, `Request`/`add_header` | Follows up to 5 redirects; `http://` only; `data` is `str` |

With bytes flowing across the wire, the next concern is the machine those bytes run on - spawning subprocesses, reading the environment, and exiting with a status the shell can test. That is [Processes and the OS](/docs/1409-stdlib-processes).
