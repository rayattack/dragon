# CodeGenModuleTest cases

Source programs for `test/CodeGenModuleTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :module_global_ir

```dr
x: int = 42
def show() {
  print(x)
}
show()
```

#### :module_global_read_from_func

```dr
x: int = 10
def show() {
  print(x)
}
show()
```

#### :module_global_write_from_func

```dr
x: int = 10
def bump() {
  global x
  x = 20
}
bump()
print(x)
```

#### :module_global_aug_assign

```dr
counter: int = 0
def inc() {
  global counter
  counter += 1
}
inc()
inc()
inc()
print(counter)
```

#### :module_global_string

```dr
name: str = "Dragon"
def greet() {
  print(name)
}
greet()
```

#### :module_global_bool

```dr
ready: bool = True
def check() {
  if ready {
    print(1)
  }
}
check()
```

#### :module_global_multiple_funcs

```dr
val: int = 0
def set_val() {
  global val
  val = 42
}
def get_val() {
  print(val)
}
set_val()
get_val()
```

#### :module_global_local_shadow

```dr
x: int = 10
def shadow() {
  x: int = 99
  print(x)
}
shadow()
print(x)
```

#### :py_global_keyword

```py
x: int = 10
def bump():
    global x
    x = 20
bump()
print(x)
```

#### :py_global_read

```py
x: int = 10
def show():
    global x
    print(x)
show()
```

#### :http_parse_request_e2_e

```dr
extern "C" def dragon_http_parse_request_head(buf: str, length: int) -> ptr
extern "C" def dragon_http_parsed_method(handle: ptr) -> str
extern "C" def dragon_http_parsed_url(handle: ptr) -> str
extern "C" def dragon_http_parsed_header_count(handle: ptr) -> int
extern "C" def dragon_http_parsed_header_key(handle: ptr, idx: int) -> str
extern "C" def dragon_http_parsed_header_value(handle: ptr, idx: int) -> str
extern "C" def dragon_http_parsed_ok(handle: ptr) -> int
extern "C" def dragon_http_parsed_free(handle: ptr)

const crlf: str = "\r\n"
const raw: str = "GET /hello?name=world HTTP/1.1" + crlf + "Host: localhost" + crlf + "Content-Type: text/plain" + crlf + crlf
const parsed: ptr = dragon_http_parse_request_head(raw, len(raw))
print(dragon_http_parsed_ok(parsed))
print(dragon_http_parsed_method(parsed))
print(dragon_http_parsed_url(parsed))
print(dragon_http_parsed_header_count(parsed))
print(dragon_http_parsed_header_key(parsed, 0))
print(dragon_http_parsed_header_value(parsed, 0))
print(dragon_http_parsed_header_key(parsed, 1))
print(dragon_http_parsed_header_value(parsed, 1))
dragon_http_parsed_free(parsed)
```

#### :http_build_response_e2_e

```dr
extern "C" def dragon_http_build_response(status: int, headers: str, body: bytes) -> bytes

const crlf: str = "\r\n"
const hdrs: str = "content-type: text/plain" + crlf + "content-length: 5" + crlf
const resp: bytes = dragon_http_build_response(200, hdrs, b"hello")
print(len(resp))
```

The parser reads a request head and stops there. A declared `Content-Length`
must not keep it waiting for a body it will never be given: the framing layer
has already separated head from body and only ever hands over the head.

#### :http_parse_head_with_declared_body_e2_e

```dr
extern "C" def dragon_http_parse_request_head(buf: str, length: int) -> ptr
extern "C" def dragon_http_parsed_method(handle: ptr) -> str
extern "C" def dragon_http_parsed_url(handle: ptr) -> str
extern "C" def dragon_http_parsed_version(handle: ptr) -> str
extern "C" def dragon_http_parsed_ok(handle: ptr) -> int
extern "C" def dragon_http_parsed_free(handle: ptr)

const crlf: str = "\r\n"
const head: str = "POST /api/data HTTP/1.1" + crlf + "Host: localhost" + crlf + "Content-Length: 13" + crlf + crlf
const parsed: ptr = dragon_http_parse_request_head(head, len(head))
print(dragon_http_parsed_ok(parsed))
print(dragon_http_parsed_method(parsed))
print(dragon_http_parsed_url(parsed))
print(dragon_http_parsed_version(parsed))
dragon_http_parsed_free(parsed)
```

#### :non_blocking_send_recv_e2_e

```dr
extern "C" def socket(domain: intc, type: intc, protocol: intc) -> intc
extern "C" def bind(fd: intc, addr: ptr, addrlen: intc) -> intc
extern "C" def listen(fd: intc, backlog: intc) -> intc
extern "C" def accept(fd: intc, addr: ptr, addrlen: ptr) -> intc
extern "C" def connect(fd: intc, addr: ptr, addrlen: intc) -> intc
extern "C" def close(fd: intc) -> intc
extern "C" def dragon_sockaddr_in_new(port: intc, addr: str) -> ptr
extern "C" def dragon_sockaddr_in_size() -> intc
extern "C" def dragon_setsockopt_reuse(fd: int)
extern "C" def dragon_nb_send(fd: int, buf: str, length: int) -> int
extern "C" def dragon_nb_recv_str(fd: int, max_len: int) -> str
extern "C" def dragon_close_fd(fd: int)
extern "C" def malloc(size: int) -> ptr
extern "C" def free(p: ptr)

const sfd: int = socket(2, 1, 0)
dragon_setsockopt_reuse(sfd)
const addr: ptr = dragon_sockaddr_in_new(19877, "127.0.0.1")
const addrlen: int = dragon_sockaddr_in_size()
bind(sfd, addr, addrlen)
listen(sfd, 1)

const cfd: int = socket(2, 1, 0)
const addr2: ptr = dragon_sockaddr_in_new(19877, "127.0.0.1")
connect(cfd, addr2, addrlen)

const afd: int = accept(sfd, none, none)

dragon_nb_send(cfd, "hello", 5)
const msg: str = dragon_nb_recv_str(afd, 1024)
print(msg)

close(afd)
close(cfd)
close(sfd)
free(addr)
free(addr2)
```

#### :arithmetic_and_function

```dr
def square(x: int) -> int {
  return x * x
}
print(square(7))
```

#### :http_build_response_long_headers

```dr
extern "C" def dragon_http_build_response(status: int, headers: str, body: bytes) -> bytes
hdrs: str = ""
for i in range(100) {
  hdrs = hdrs + "x-custom: value\r\n"
}
resp: bytes = dragon_http_build_response(200, hdrs, b"hello")
print(len(resp))
```

#### :http_build_response_empty_body

```dr
extern "C" def dragon_http_build_response(status: int, headers: str, body: bytes) -> bytes
hdrs: str = "content-length: 0\r\n"
resp: bytes = dragon_http_build_response(204, hdrs, b"")
print(len(resp))
```

#### :http_build_response_large_body

```dr
extern "C" def dragon_http_build_response(status: int, headers: str, body: bytes) -> bytes
body: bytes = b""
for i in range(1024) {
  body = body + b"0123456789"
}
hdrs: str = "content-length: 10240\r\n"
resp: bytes = dragon_http_build_response(200, hdrs, body)
print(len(resp))
```

#### :http_build_response_loop_bounded

```dr
extern "C" def dragon_http_build_response(status: int, headers: str, body: bytes) -> bytes
hdrs: str = "content-type: text/plain\r\ncontent-length: 5\r\n"
last_len: int = 0
for i in range(5000) {
  resp: bytes = dragon_http_build_response(200, hdrs, b"hello")
  last_len = len(resp)
}
print(last_len)
```

#### :http_build_response_unknown_status

```dr
extern "C" def dragon_http_build_response(status: int, headers: str, body: bytes) -> bytes
resp: bytes = dragon_http_build_response(999, "\r\n", b"x")
print(len(resp))
```
