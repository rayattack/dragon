# ReplEchoTest cases

Each block is one REPL cell. Echo is the rule that a bare top-level expression
prints its value, repr-style, so the prompt behaves the way Python's does. Every
qualifying top-level expression statement echoes, in source order: an expression
that silently vanished mid-cell would be a surprise.

Assignments, declarations and calls returning nothing stay silent.

## Scalars

#### :echo_int

```dr
2 + 2
```

#### :echo_float

```dr
1.0 + 1.5
```

#### :echo_bool

```dr
1 < 2
```

## Strings are quoted, print() is not

A bare string echoes repr-style with quotes, which is what tells you at the
prompt that you are looking at a string and not at prose. An explicit `print`
still writes the bare text, and must not also echo.

#### :echo_str

```dr
"hello".upper()
```

#### :echo_print_is_not_doubled

```dr
print("hi")
```

## Containers

#### :echo_list

```dr
[1, 2, 3]
```

#### :echo_nested_dict

```dr
d: dict[str, list[int]] = {"a": [1, 2]}
d
```

#### :echo_tuple

```dr
(1, 2)
```

## Silent forms

An assignment is a statement, not a value. `None` is Python parity. A `def`
contributes a definition and produces nothing.

#### :echo_assignment_is_silent

```dr
x: int = 42
```

#### :echo_none_is_silent

```dr
None
```

#### :echo_def_is_silent

```dr
def helper(n: int) -> int {
    return n
}
```

## Order

Several bare expressions in one cell echo in source order, interleaved correctly
with explicit prints.

#### :echo_multiple_in_order

```dr
1 + 1
print("middle")
3 + 3
```

## Class instances

A class with `__str__` echoes through it. Without one, the runtime's own
instance formatting is used rather than a second format invented for the prompt.

#### :echo_instance_with_str

```dr
class Point {
    x: int
    y: int

    def(x: int, y: int) {
        self.x = x
        self.y = y
    }

    def __str__() -> str {
        return "Point(" + str(self.x) + ", " + str(self.y) + ")"
    }
}

Point(3, 4)
```

## Cross-turn

Echo must work on a name bound in an earlier turn, which is the whole point of
the prompt.

#### :echo_cross_turn_seed

```dr
total: int = 40
```

#### :echo_cross_turn_use

```dr
total + 2
```
