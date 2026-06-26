# Decorators

A decorator is the `@name` line you write directly above a `def`. It is pure
syntax sugar with one rule, identical to Python's: `@dec` on a function `f`
means `f = dec(f)`. The decorator is just an ordinary function
that receives the decorated function and returns whatever should take its
place. Nothing magic happens - the `@` line runs once, at definition time.

```dragon
def loud(f: Callable[[], None]) -> Callable[[], None] {
    return f                      # receive it, hand it straight back
}

@loud
def greet() -> None {
    print("hello")
}

greet()                           # hello
```

`greet` is defined, then immediately rebound to `loud(greet)`. Here `loud`
returns the function unchanged, so `greet` is itself - the decorator only got
a chance to *see* it on the way past. That "see it on the way past" is the
whole game, and it is more useful than it looks.

A function value is just a pointer at runtime, so a decorator's parameter and
return type can be written as the precise `Callable[[...], R]` signature (as
above) or, when you don't care about the shape, the bare `ptr`:

```dragon
def tag(f: ptr) -> ptr {
    return f
}
```

## Registration: the pattern that earns its keep

The reason identity decorators matter is **registration**. A decorator runs
at definition time and can record the function in a table before returning it
untouched. That is exactly how a router collects routes, a test runner
collects tests, or a CLI collects subcommands - without any import-time
reflection or filesystem scanning.

```dragon
commands: list[Callable[[], None]] = []

def command(f: Callable[[], None]) -> Callable[[], None] {
    commands.append(f)            # record it...
    return f                      # ...and return it unchanged
}

@command
def deploy() -> None {
    print("deploying")
}

@command
def rollback() -> None {
    print("rolling back")
}

print(len(commands))              # 2
for cmd in commands {
    cmd()                         # deploying / rolling back
}
```

Each `@command` line appends the function to `commands` as it is defined, then
returns it so `deploy` and `rollback` remain ordinary callable functions. The
table is built by the time the top-level code runs - no central list to keep
in sync by hand, no decorator that has to *wrap* anything.

## Method decorators

Three decorators apply to methods inside a class, and all three match their
Python spelling.

### `@property` - a method that reads like a field

`@property` turns a zero-argument method into a computed attribute: callers
read it with no parentheses. Pair it with a `@name.setter` to make assignment
work too.

```dragon
class Temp {
    _celsius: float = 0.0

    @property
    def celsius() -> float {
        return self._celsius
    }

    @celsius.setter
    def celsius(value: float) -> None {
        self._celsius = value
    }
}

t: Temp = Temp()
t.celsius = 25.0                  # calls the setter
print(t.celsius)                  # 25.0 - calls the getter, no parentheses
```

The leading underscore on `_celsius` is the protected-field convention from
[Member Privacy](/docs/0605-privacy): the stored value is internal, and the
property is the public surface.

### `@staticmethod` - a function that lives on the class

A static method takes no `self` and is called on the class. It's the home for
helpers and factories that belong to a type but don't need an instance.

```dragon
class MathUtil {
    @staticmethod
    def square(n: int) -> int {
        return n * n
    }
}

print(MathUtil.square(7))         # 49
```

In `.dr` files you can also write `static def square(...)` - the two spellings
mean the same thing; `@staticmethod` is the Python-compatible form.

### `@classmethod` - a method called on the class

`@classmethod` marks a method invoked on the class itself, most often an
alternative constructor:

```dragon
class Widget {
    name: str = "widget"

    @classmethod
    def create() -> Widget {
        return Widget()
    }
}

w: Widget = Widget.create()
print(w.name)                     # widget
```

## What does not work yet

Decorators that **wrap** - return a *new* function that calls the original
with extra behavior around it - are not expressible today. A decorator's
return is a function value (`Callable`/`ptr`), and the type checker does not
yet accept a freshly-returned closure in that position:

```dragon
def timed(f: ptr) -> ptr {
    return lambda () -> None {     # error: return type '() -> None'
        print("before")           # does not match declared return type 'ptr'
        f()
    }
}
```

The same limit rules out **decorator factories** - the parameterized
`@route("/path")` form - because a factory must return a function (the actual
decorator), and that is the same rejected shape:

```dragon
def route(path: str) -> ptr {
    return lambda (f: ptr) -> ptr {   # error: returning a function value
        return f
    }
}
```

So `@timed`, `@cache`, `@retry(3)`, and friends that splice behavior around a
call are not decorators in Dragon yet. Until the function-value-as-return gap
closes, reach for one of two things instead: an explicit higher-order function
you call by hand (`run_timed(work)`), or - for the registration use, which is
what most parameterless decorators are really doing - the identity form above.

## At a glance

| You want to... | Write |
|----------------|-------|
| Register or tag a function | `@dec` where `dec(f) -> f` returns it unchanged |
| A computed, read-only attribute | `@property def x() -> T { ... }` |
| Make that attribute assignable | `@x.setter def x(value: T) -> None { ... }` |
| A function on the class, no `self` | `@staticmethod` (or `static def` in `.dr`) |
| An alternative constructor | `@classmethod def create() -> T { ... }` |
| Wrap behavior around a call | not yet - use an explicit higher-order function |
| A parameterized decorator `@dec(arg)` | not yet - same limitation |

That covers the `@name` layer. The last chapter in this part,
[Iterators and Generators](/docs/0803-iterators), is the other half of
"functions as values" - functions that pause, resume, and produce a sequence
on demand.
