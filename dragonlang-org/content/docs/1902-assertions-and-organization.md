# Assertions and Test Organization

[Writing Tests with unittest](/docs/1901-unittest) got a suite running:
subclass `TestCase`, write `test_*` methods, call `main([...])`. This
chapter is the other half - the full vocabulary of assertions you check
with, and the conventions for organizing tests once a project has more
than one file of them.

## The assertion catalog

Every assertion is a method on `self`, named to match Python. These are
the methods that actually ship in `stdlib/unittest.dr` - no more, no less:

| Method | Passes when |
|--------|-------------|
| `assertEqual(a, b)` | `a == b` (deep compare for lists, dicts, bytes) |
| `assertNotEqual(a, b)` | `a != b` |
| `assertTrue(x)` | `x` is truthy |
| `assertFalse(x)` | `x` is falsy |
| `assertIsNone(x)` | `x` is `None` |
| `assertIsNotNone(x)` | `x` is not `None` |
| `assertIn(needle, haystack)` | `needle` is a substring of `haystack` |
| `assertNotIn(needle, haystack)` | `needle` is not in `haystack` |
| `assertGreater(a, b)` | `a > b` |
| `assertLess(a, b)` | `a < b` |
| `assertGreaterEqual(a, b)` | `a >= b` |
| `assertLessEqual(a, b)` | `a <= b` |
| `assertAlmostEqual(a, b, places=7)` | `a` and `b` agree to `places` decimals |
| `assertRaises(ExcType, fn)` | calling `fn()` raises `ExcType` |

Every assertion also accepts a trailing `msg: str` argument that replaces
the auto-generated failure text:

```dragon
from unittest import TestCase, main

class TotalTests(TestCase) {
    def test_running_total() {
        total: int = 40 + 60
        self.assertEqual(total, 100, "running total drifted")
    }
}

main([TotalTests()])
```

The four sections below cover the assertions with behaviour worth knowing
beyond the one-liner.

### Equality is deep

`assertEqual` compares through `Any`, so it recurses into containers. Two
distinct list allocations holding the same elements are equal, exactly as
in Python:

```dragon
from unittest import TestCase, main

class EqualityTests(TestCase) {
    def test_list_equality() {
        self.assertEqual([1, 2, 3], [1, 2, 3])          # passes
        self.assertEqual({"a": 1}, {"a": 1})            # passes
    }
}

main([EqualityTests()])
```

You don't get a shallow pointer comparison and a confusing failure - the
runtime walks the structure.

### Floats: `assertAlmostEqual`

Floating-point equality is a trap (`0.1 + 0.2 != 0.3`), so compare floats
with `assertAlmostEqual`, which checks the two values agree to `places`
decimal places (default 7):

```dragon
from unittest import TestCase, main

class FloatTests(TestCase) {
    def test_float_math() {
        self.assertAlmostEqual(0.1 + 0.2, 0.3)          # passes
        self.assertAlmostEqual(3.14159, 3.14, places=2) # passes
    }
}

main([FloatTests()])
```

### Strings and containment

`assertIn` checks substring containment for strings:

```dragon
from unittest import TestCase, main

def render_title(text: str) -> str {
    return "<h1>" + text + "</h1>"
}

class RenderTests(TestCase) {
    def test_rendering() {
        page: str = render_title("Welcome")
        self.assertIn("<h1>Welcome</h1>", page)
        self.assertNotIn("<script>", page)
    }
}

main([RenderTests()])
```

### Exceptions: `assertRaises`

To assert that code *should* fail, pass the expected exception type and a
zero-argument callable. The test passes if calling it raises that type (or
a subclass - the integer-code range model matches parent types, mirroring
Python):

```dragon
from unittest import TestCase, main

def parse_port(s: str) -> int {
    n: int = int(s)
    if n < 0 {
        raise ValueError("port must be non-negative")
    }
    return n
}

def parse_negative() -> None {
    parse_port("-1")
}

class ParseTests(TestCase) {
    def test_rejects_negative() {
        self.assertRaises(ValueError, parse_negative)
    }
}

main([ParseTests()])
```

`assertRaises` takes the **callable form** - a function reference, not a
block. The context-manager form (`with self.assertRaises(ValueError) {
... }`) is not yet implemented; wrap the failing call in a small named
function or a `lambda` and pass that. See
[Error Handling](/docs/0901-exceptions) for the `raise`/`try`/`except`
mechanics being tested here.

## Fixtures: set up by hand

Python's `setUp`/`tearDown` per-test hooks are **not** auto-invoked yet.
Until they are, do the setup explicitly. The clean pattern is a helper
method the tests call themselves:

```dragon
from unittest import TestCase, main
import database
from database import SQL

class UserTests(TestCase) {
    def fresh_db() -> database.Connection {
        db: database.Connection = database.open("sqlite::memory:")
        db.raw("create table users(id integer primary key, name text)")
        return db
    }

    def test_insert() {
        db: database.Connection = self.fresh_db()
        name: str = "Ada"
        db.run(template[SQL] { insert into users(name) values(!{name}) })
        cnt: int = db.val(template[SQL] { select count(*) from users })
        self.assertEqual(cnt, 1)
        db.close()
    }
}

main([UserTests()])
```

Each test opens its own in-memory database, so they don't bleed state into
each other - the manual approach makes the per-test isolation impossible to
forget. (See [Databases](/docs/1301-databases) for the `template[SQL]`
binding that keeps this injection-safe.)

## The project convention

Dragon's own test suite splits along a clear line, and your projects should
follow the same split:

- **Behavioral and end-to-end tests live in `test/dr/*.dr`** and use
  `unittest`. Anything you can express as "run this Dragon code and check
  the result" - stdlib API behavior, crypto known-answer tests, the
  database driver, HTTP handlers - belongs here. These are real programs
  exercising the real compiler and runtime.
- **GoogleTest C++ suites are for compiler internals only** - IR-shape
  assertions, lexer token streams, parser AST construction, type-checker
  diagnostics. They test *the compiler*, not programs written in Dragon.

The dividing question is simple: *am I testing how Dragon behaves, or am I
testing how the compiler is built?* If it's the former, write a `.dr` test.

### Auto-registration with CTest

You don't wire `test/dr/*.dr` files into the build by hand. The build
globs them and registers each as a CTest case automatically:

```bash
file(GLOB DR_TEST_FILES CONFIGURE_DEPENDS .../dr/test_*.dr)
foreach(_dr ${DR_TEST_FILES})
    get_filename_component(_name ${_dr} NAME_WE)
    add_test(NAME dr_${_name} COMMAND $<TARGET_FILE:dragon> run ${_dr})
endforeach()
```

`CONFIGURE_DEPENDS` re-globs on every build, so dropping a new
`test_whatever.dr` into `test/dr/` makes it a CTest case named
`dr_test_whatever` the next time you build - no edit to any CMake file.
Because each file ends in `main([...])`, which exits non-zero on failure,
CTest gates purely on the process exit code. There is nothing for the test
runner to parse; the exit status *is* the result.

Run the whole `.dr` suite the same way you run everything else:

```bash
ctest -R dr_                 # every Dragon-language test
ctest -R dr_test_math        # one of them
```

## Putting it together

A self-contained suite covering equality, ordering, floats, containment,
and an expected failure - the shape a real `test/dr/*.dr` file takes:

```dragon
from unittest import TestCase, main

def normalize(name: str) -> str {
    return name.strip().lower()
}

def withdraw(balance: int, amount: int) -> int {
    if amount > balance {
        raise ValueError("insufficient funds")
    }
    return balance - amount
}

def overdraw() -> None {
    withdraw(50, 100)
}

class AccountTests(TestCase) {
    def test_normalize() {
        self.assertEqual(normalize("  Ada  "), "ada")
        self.assertIn("d", normalize("Ada"))
    }

    def test_withdraw() {
        self.assertEqual(withdraw(100, 30), 70)
        self.assertGreaterEqual(withdraw(100, 0), 100)
    }

    def test_interest() {
        rate: float = 0.05
        self.assertAlmostEqual(1000.0 * rate, 50.0)
    }

    def test_overdraw_raises() {
        self.assertRaises(ValueError, overdraw)
    }
}

main([AccountTests()])
```

```
ok: test_interest
ok: test_normalize
ok: test_overdraw_raises
ok: test_withdraw
Ran 4 test(s); failures: 0, errors: 0
OK
```

## At a glance

| You want to... | Write |
|----------------|-------|
| Assert equality | `self.assertEqual(a, b)` (deep for containers) |
| Assert inequality | `self.assertNotEqual(a, b)` |
| Assert truthiness | `self.assertTrue(x)` / `self.assertFalse(x)` |
| Assert `None` | `self.assertIsNone(x)` / `self.assertIsNotNone(x)` |
| Compare floats | `self.assertAlmostEqual(a, b, places=7)` |
| Check substring | `self.assertIn(part, whole)` / `assertNotIn` |
| Compare ordering | `assertGreater` / `Less` / `GreaterEqual` / `LessEqual` |
| Assert it raises | `self.assertRaises(ValueError, fn)` (callable form) |
| Custom failure text | trailing `msg` arg on any assertion |
| Set up per test | call a helper yourself (no auto `setUp` yet) |
| Organize project tests | behavioral → `test/dr/*.dr`; compiler internals → GoogleTest |
| Run via CTest | drop `test_*.dr` into `test/dr/`; `ctest -R dr_` |

A `.dr` test file is the cleanest demonstration of Dragon's
[entry-point model](/docs/0301-functions): no harness reaches in and calls
your tests - *you* call them, on the last line, and the file you run is
the program.
