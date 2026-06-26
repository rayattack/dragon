# Writing Tests with unittest

Most test frameworks are a runtime affair: a discovery harness scans your
filesystem, a runner reflects over modules at startup, decorators register
callbacks, and a command-line tool (`pytest`, `go test`, `cargo test`)
bootstraps the whole thing. Dragon does less. A test file is **just a
Dragon program** - top-level statements that build a suite and run it. You
launch it the same way you launch anything else:

```bash
dragon run test/dr/test_math.dr
```

There is no special `test` subcommand, no magic file naming the compiler
cares about, no `if __name__ == "__main__"` guard. The file you hand to
`dragon run` *is* the program, and the program's last line is the call
that runs the tests. If that suite fails, the process exits non-zero - so
CI, shell scripts, and CTest all gate on it for free.

The framework itself is `unittest`, and it deliberately mirrors Python's
`unittest`: you subclass `TestCase`, write `test_*` methods, and call the
assertion helpers. If you've written a Python `unittest.TestCase`, you
already know the shape. The whole framework is itself written in Dragon
(`stdlib/unittest.dr`) - there is no C++ test runtime underneath it.

This chapter covers writing and running a suite; the full assertion
vocabulary and the conventions for organizing tests across a project are
the subject of
[Assertions and Test Organization](/docs/1902-assertions-and-organization).

## The first test

A test suite is a class that extends `TestCase`. Each method whose name
begins with `test_` is one test:

```dragon
from unittest import TestCase, main

class MathTests(TestCase) {
    def test_addition() {
        self.assertEqual(2 + 2, 4)
    }

    def test_ordering() {
        self.assertTrue(10 > 3)
        self.assertGreater(10, 3)
    }
}

main([MathTests()])
```

Run it:

```bash
dragon run test_math.dr
```

```
ok: test_addition
ok: test_ordering
Ran 2 test(s); failures: 0, errors: 0
OK
```

Three things are worth pinning down right away, because they are the
whole model:

- **You subclass `TestCase` with `.dr` constructor syntax.** Test methods
  take no parameters except the implicit `self`, and you don't write a
  `def()` constructor unless you need one - `TestCase` already provides it.
- **`main([...])` takes a list of *instances*, not classes.** You write
  `MathTests()`, not `MathTests`. Until module-level class enumeration
  lands, the caller hands the runner a `list[TestCase]` explicitly - no
  decorator magic, no filesystem discovery. It's more typing, but it's
  also fully debuggable: you see exactly which suites run.
- **The last line runs the program.** `main([MathTests()])` is an ordinary
  function call at the top level of the file. Delete it and the file
  compiles, runs, and prints nothing - the tests are defined but never
  invoked. This is the same [no-magic-`main`](/docs/0301-functions) rule
  that governs every Dragon program.

The assertions used here - `assertEqual`, `assertTrue`, `assertGreater` -
are three of the helpers `TestCase` provides; the
[next chapter](/docs/1902-assertions-and-organization) catalogs them all.

## What a failure looks like

When an assertion fails, the runner prints a `FAIL:` line naming the test
and the reason, but **keeps going** - one failing assertion does not abort
the suite. Take a deliberately broken test:

```dragon
from unittest import TestCase, main

class BrokenTests(TestCase) {
    def test_ok() {
        self.assertEqual(2 + 2, 4)
    }

    def test_broken() {
        self.assertEqual(2 + 2, 5)
    }
}

main([BrokenTests()])
```

```
FAIL: test_broken: assertEqual: 4 != 5
ok: test_ok
Ran 2 test(s); failures: 1, errors: 0
FAILED
```

The process exits with code `1`. Two distinctions the summary draws:

- A **failure** is a missed assertion (`failures:`).
- An **error** is an *uncaught* exception thrown inside a test body - the
  runner catches it, reports `ERROR: <test>: <message>`, and counts it
  under `errors:`. A bug in the test or the code under test surfaces as an
  error, not a crash that takes the whole run down with it.

The final line is `OK` if both counts are zero, otherwise `FAILED`, and
the process exits `0` or `1` to match. That non-zero exit is the contract
the rest of your tooling hangs off - CI, a shell `&&` chain, or CTest all
read it directly, with nothing to parse.

> Tests run in the order `dir()` reports the methods, which is
> alphabetical - not definition order. Don't write tests that depend on
> running in a particular sequence; each `test_*` should stand alone.

## Multiple suites in one file

`main` takes a list, so a single file can carry several suites and run
them together:

```dragon
import unittest
from unittest import TestCase

class StringTests(TestCase) {
    def test_upper() { self.assertEqual("ab".upper(), "AB") }
}

class ListTests(TestCase) {
    def test_len() { self.assertEqual(len([1, 2, 3]), 3) }
}

unittest.main([StringTests(), ListTests()])
```

Both the `from unittest import main` and the qualified `unittest.main(...)`
forms work - they're the same function. Use whichever reads better; this
book favours the short import.

That is the whole runner: a file, a `TestCase` subclass, and one line to
kick it off. What remains is the vocabulary of assertions and the
conventions for organizing tests at project scale - both covered next, in
[Assertions and Test Organization](/docs/1902-assertions-and-organization).
