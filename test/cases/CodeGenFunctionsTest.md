# CodeGenFunctionsTest cases

Source programs for `test/CodeGenFunctionsTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :function_call

```dr
def double_(x: int) -> int {
  return x * 2
}
print(double_(21))
```

#### :forward_declaration

```dr
def foo() -> int {
  return bar()
}
def bar() -> int {
  return 42
}
```

#### :lambda_simple

```dr
f: ptr = lambda (x: int) -> int {
  return x + 1
}
```

#### :type_alias_no_op

```dr
type IntList = list[int]
x: int = 42
print(x)
```

#### :py_type_alias_ir

```py
type IntList = list[int]
x: int = 42
print(x)
```

#### :indirect_call_ir

```dr
def inc(x: int) -> int {
  return x + 1
}
f: ptr = inc
y: int = f(10)
```

#### :function_and_loop

```dr
def fib(n: int) -> int {
  if n <= 1 {
    return n
  }
  return fib(n - 1) + fib(n - 2)
}
print(fib(10))
for i in range(3) {
  print(i)
}
```

#### :recursive_power

```dr
def power(base: int, exp: int) -> int {
  if exp == 0 {
    return 1
  }
  return base * power(base, exp - 1)
}
print(power(2, 10))
```

#### :multiple_returns

```dr
def classify(n: int) -> str {
  if n < 0 {
    return "negative"
  }
  if n == 0 {
    return "zero"
  }
  return "positive"
}
print(classify(-5))
print(classify(0))
print(classify(42))
```

#### :positional_only_param_func

```dr
def add(a: int, b: int, /) -> int {
    return a + b
}
print(add(3, 4))
```

#### :keyword_only_param_func

```dr
def greet(a: int, *, count: int) -> int {
    return a + count
}
print(greet(1, 5))
```

#### :mixed_param_separators

```dr
def foo(a: int, /, b: int, *, c: int) -> int {
    return a + b + c
}
print(foo(1, 2, 3))
```

#### :lambda_assigned_to_variable_then_called

```dr
square: ptr = lambda (x: int) -> int {
  return x * x
}
print(square(4))
```

#### :named_function_assigned_to_variable_then_called

```dr
def double(x: int) -> int {
  return x * 2
}
fn: ptr = double
print(fn(5))
```

#### :function_passed_as_ptr_parameter

```dr
def apply(f: ptr, x: int) -> int {
  return f(x)
}
def triple(x: int) -> int {
  return x * 3
}
result: int = apply(triple, 7)
print(result)
```

#### :lambda_passed_directly_as_argument

```dr
def apply(f: ptr, x: int) -> int {
  return f(x)
}
result: int = apply(lambda (n: int) -> int { return n + 10 }, 5)
print(result)
```

#### :first_class_func_multiple_args

```dr
def add(a: int, b: int) -> int {
  return a + b
}
op: ptr = add
print(op(3, 4))
```

#### :higher_order_function_returning_function

```dr
def get_doubler() -> ptr {
  return lambda (x: int) -> int { return x * 2 }
}
dbl: ptr = get_doubler()
print(dbl(10))
```

#### :first_class_func_existing_calls_still_work

```dr
def greet(name: str) -> str {
  return "hello"
}
print(greet("world"))
```

#### :lambda_assigned_with_annotation

```dr
fn: ptr = lambda (x: int) -> int {
  return x * x
}
print(fn(6))
```

#### :closure_capture_int

```dr
def make_adder(n: int) -> int {
    f: ptr = lambda (x: int) -> int { return x + n }
    return f(10)
}
print(make_adder(5))
print(make_adder(20))
```

#### :closure_capture_str

```dr
def greet(greeting: str, name: str) -> str {
    f: ptr = lambda (n: str) -> str { return greeting + ", " + n }
    return f(name)
}
print(greet("Hello", "World"))
print(greet("Hi", "Dragon"))
```

#### :closure_multiple_captures

```dr
def fmt(prefix: str, multiplier: int, x: int) -> str {
    f: ptr = lambda (v: int) -> str {
        return prefix + ": " + str(v * multiplier)
    }
    return f(x)
}
print(fmt("Result", 3, 7))
print(fmt("Result", 3, 10))
```

#### :closure_by_value_semantics

```dr
def test() -> int {
    x: int = 10
    f: ptr = lambda () -> int { return x }
    x = 20
    return f()
}
print(test())
```

#### :non_capturing_lambda_unchanged

```dr
f: ptr = lambda (x: int, y: int) -> int { return x + y }
print(f(3, 4))
```

#### :closure_capture_bool

```dr
def check(flag: bool, x: int) -> str {
    f: ptr = lambda (v: int) -> str {
        if flag {
            return "yes: " + str(v)
        }
        return "no: " + str(v)
    }
    return f(x)
}
print(check(True, 42))
print(check(False, 42))
```

#### :closure_capture_float

```dr
def scale(factor: float, x: float) -> float {
    f: ptr = lambda (v: float) -> float { return v * factor }
    return f(x)
}
print(scale(2.0, 3.5))
```

#### :closure_returned_from_function

```dr
def make_adder(n: int) -> int {
    adder: ptr = lambda (x: int) -> int { return x + n }
    return adder(100)
}
print(make_adder(5))
print(make_adder(42))
```

#### :nested_def_with_capture

```dr
def outer(x: int) -> int {
    def inner(y: int) -> int {
        return x + y
    }
    return inner(10)
}
print(outer(5))
```

#### :nested_def_without_capture

```dr
def outer() -> int {
    def inner(y: int) -> int {
        return y * 2
    }
    return inner(7)
}
print(outer())
```

#### :nested_def_siblings

```dr
def outer() -> int {
    def first(a: int) -> int {
        return a + 1
    }
    def second(a: int) -> int {
        return a * 10
    }
    x: int = first(5)
    y: int = second(3)
    return x + y
}
print(outer())
```

#### :nested_def_recursive

```dr
def outer(n: int) -> int {
    def fact(k: int) -> int {
        if k <= 1 {
            return 1
        }
        return k * fact(k - 1)
    }
    return fact(n)
}
print(outer(5))
```

#### :nested_def_recursive_with_capture

```dr
def outer(base: int, n: int) -> int {
    def step(k: int) -> int {
        if k == 0 {
            return base
        }
        return step(k - 1) + base
    }
    return step(n)
}
print(outer(7, 4))
```

#### :nested_def_str_capture

```dr
def greet(name: str) -> str {
    def make_greeting(suffix: str) -> str {
        return name + suffix
    }
    return make_greeting("!")
}
print(greet("hello"))
```

#### :nested_def_does_not_leak_into_module

```dr
def inner(x: int) -> int {
    return x + 100
}
def outer() -> int {
    def inner(y: int) -> int {
        return y - 1
    }
    return inner(50)
}
print(outer())
print(inner(50))
```

#### :generator_stored_in_var_ir

```dr
def nums() {
    yield 1
    yield 2
    yield 3
}

g: ptr = nums()
for x in g {
    print(x)
}
```

#### :generator_ir

```dr
def gen() {
    yield 42
}

for x in gen() {
    print(x)
}
```

#### :generator_basic

```dr
def count_up(n: int) {
    i: int = 0
    while i < n {
        yield i
        i = i + 1
    }
}

for x in count_up(5) {
    print(x)
}
```

#### :generator_fibonacci

```dr
def fib(n: int) {
    a: int = 0
    b: int = 1
    i: int = 0
    while i < n {
        yield a
        temp: int = a + b
        a = b
        b = temp
        i = i + 1
    }
}

for x in fib(7) {
    print(x)
}
```

#### :generator_no_args

```dr
def three() {
    yield 10
    yield 20
    yield 30
}

for x in three() {
    print(x)
}
```

#### :generator_empty

```dr
def empty() {
    return
    yield 1
}

for x in empty() {
    print(x)
}
print("done")
```

#### :generator_stored_in_var

```dr
def nums() {
    yield 1
    yield 2
    yield 3
}

g: ptr = nums()
for x in g {
    print(x)
}
```

#### :basic_function_decorator

```dr
def loud(f: ptr) -> ptr {
    # For now, just return the function as-is (identity decorator)
    return f
}
@loud
def greet() {
    print("hello")
}
greet()
```

#### :var_args_ir

```dr
def foo(a: int, *args: int) {
  print(a)
}
foo(1, 2, 3)
```

#### :var_args_empty

```dr
def show(tag: str, *args: int | str) {
  print(tag)
  print(len(args))
}
show("test")
```

#### :var_args_len

```dr
def count(*args: int | str) -> int {
  return len(args)
}
print(count(1, 2, 3))
```

#### :var_args_mixed

```dr
def greet(greeting: str, *names: str) {
  print(greeting)
  print(len(names))
}
greet("hi", "alice", "bob")
```

#### :kwargs_empty

```dr
def config(**kwargs: int | str) {
  print(len(kwargs))
}
config()
```

#### :kwargs_len

```dr
def config(**kwargs: int | str) {
  print(len(kwargs))
}
config(host="localhost", port=8080)
```

#### :var_args_and_kwargs

```dr
def flexfunc(a: int, *args: int | str, **kwargs: int | str) {
  print(a)
  print(len(args))
  print(len(kwargs))
}
flexfunc(1, 2, 3, x=10, y=20)
```

#### :var_args_only_regular

```dr
def add(a: int, b: int, *rest: int | str) -> int {
  return a + b
}
print(add(10, 32))
```

#### :var_args_typed_float

```dr
def addf(*args: float) -> float {
  s: float = 0.0
  for a in args { s = s + a }
  return s
}
print(addf(1.5, 2.5))
```

#### :var_args_typed_str

```dr
def names(*args: str) {
  for a in args { print(a) }
}
names("x", "y", "z")
```

#### :var_args_typed_list_elem

```dr
def f(*args: list[int]) {
  for a in args { print(a[0]) }
}
f([10, 20], [30, 40])
```

#### :var_args_union_elem

```dr
def f(*args: list[int] | str) {
  for a in args { print(a) }
}
f([1, 2], "hi")
```

#### :generator_reraise_emits_scope_cleanup

```dr
def gen() {
  yield 1
}
def caller() {
  buf: str = "hello"
  for x in gen() {
    print(x)
  }
  print(buf)
}
caller()
```

#### :generator_reraise_doesnt_leak_outer_locals

```dr
def explode() {
  yield 1
  raise ValueError("boom")
}
errors: int = 0
for i in range(2000) {
  try {
    buf: str = "per-iter-string-" + str(i)
    for x in explode() {
      _u: str = buf
    }
  } except ValueError {
    errors = errors + 1
  }
}
print(errors)
```

#### :generator_reraise_exception_propagates

```dr
def explode() {
  yield 1
  raise ValueError("propagate me")
}
caught: int = 0
try {
  for x in explode() {
    print(x)
  }
} except ValueError {
  caught = 1
}
print(caught)
```

#### :generator_wrapper_uses_typed_create

```dr
def echo(prefix: str) {
  yield prefix
}
for v in echo("hi") {
  print(v)
}
```

#### :generator_typed_create_declaration

```dr
def gen(s: str) {
  yield s
}
g: ptr = gen("x")
```

#### :generator_with_string_arg_runs_correctly

```dr
def echo(prefix: str) {
  yield prefix
  yield prefix + "!"
}
for v in echo("hello") {
  print(v)
}
```

#### :generator_yields_string_round_trips

```dr
def labels() {
  yield "first"
  yield "second"
  yield "third"
}
for s in labels() {
  print(s)
}
```

#### :generator_abandoned_no_leak

```dr
def chunks(s: str) {
  yield s
  yield s
  yield s
}
def consume_one() {
  s: str = "abandon-me-" + str(7)
  for v in chunks(s) {
    break
  }
}
for i in range(5000) {
  consume_one()
}
print("ok")
```

#### :generator_multiple_heap_args_balance

```dr
def two_strs(a: str, b: str) {
  yield a
  yield b
}
for i in range(2000) {
  s1: str = "s1-" + str(i)
  s2: str = "s2-" + str(i)
  for v in two_strs(s1, s2) {
    break
  }
}
print("ok")
```

#### :generator_int_arg_still_works

```dr
def count_to(n: int) {
  i: int = 0
  while i < n {
    yield i
    i = i + 1
  }
}
total: int = 0
for x in count_to(100) {
  total = total + x
}
print(total)
```

#### :file_read_from_pipe

```dr
extern "C" def fopen(path: str, mode: str) -> ptr
extern "C" def fclose(stream: ptr) -> intc
extern "C" def dragon_file_write_text(handle: ptr, s: str) -> int
extern "C" def dragon_file_read(handle: ptr) -> str
w: ptr = fopen("/tmp/dragon_test_tier210.txt", "w")
_n: int = dragon_file_write_text(w, "abc\ndef\nghi\n")
_w: intc = fclose(w)
g: ptr = fopen("/tmp/dragon_test_tier210.txt", "r")
content: str = dragon_file_read(g)
_g: intc = fclose(g)
print(content)
```

#### :file_read_shell_pipe

```dr
extern "C" def popen(cmd: str, mode: str) -> ptr
extern "C" def pclose(stream: ptr) -> intc
extern "C" def dragon_file_read(handle: ptr) -> str
p: ptr = popen("printf 'line1\\nline2\\nline3\\n'", "r")
content: str = dragon_file_read(p)
_: intc = pclose(p)
print(content)
```

#### :file_read_shell_pipe_large_output

```dr
extern "C" def popen(cmd: str, mode: str) -> ptr
extern "C" def pclose(stream: ptr) -> intc
extern "C" def dragon_file_read(handle: ptr) -> str
extern "C" def dragon_str_len(s: str) -> int
p: ptr = popen("yes | head -c 20000", "r")
content: str = dragon_file_read(p)
_: intc = pclose(p)
print(dragon_str_len(content))
```

#### :file_read_shell_pipe_empty

```dr
extern "C" def popen(cmd: str, mode: str) -> ptr
extern "C" def pclose(stream: ptr) -> intc
extern "C" def dragon_file_read(handle: ptr) -> str
extern "C" def dragon_str_len(s: str) -> int
p: ptr = popen("true", "r")
content: str = dragon_file_read(p)
_: intc = pclose(p)
print(dragon_str_len(content))
```
