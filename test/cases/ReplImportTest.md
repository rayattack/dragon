# ReplImportTest cases

Each block is one REPL cell. An import binds names for the rest of the session,
so the module it pulls in is emitted once and every later turn links against
that copy. A turn that re-emitted it would define the same symbols twice in the
session dylib, which ends the session rather than the turn.

The cells here are deliberately dull. What is under test is the turn that comes
after the import, not the import itself.

## Importing a name

#### :import_sqrt

```dr
from math import sqrt
```

#### :call_sqrt

```dr
sqrt(16.0)
```

## Importing a module

#### :import_math

```dr
import math
```

#### :call_math_sqrt

```dr
math.sqrt(16.0)
```

## A second module, imported in its own turn

#### :import_io_open

```dr
from io import open
```

## Cells that touch nothing the import brought in

#### :plain_sum

```dr
1 + 1
```

#### :seven

```dr
7 + 7
```

## Module initialization

A module body runs when the module is first imported, once. `this` prints the
Zen as its initializer, which makes a re-emitted module visible: the Zen would
print again on the next turn.

#### :import_this

```dr
import this
```

## Monomorphized instantiations

A generic is stamped out per set of type arguments, and the same arguments give
the same code. Two turns calling `repeat(_, _)` with an `int` therefore ask
for one instantiation, not two, and the session must not define it twice.

#### :import_repeat

```dr
from itertools import repeat
```

#### :repeat_one

```dr
repeat(1, 2)
```

#### :repeat_three

```dr
repeat(3, 2)
```

A generic defined in the session is stamped the same way, so calling it twice
with the same type argument asks for one instantiation as well.

#### :define_first

```dr
def first[T](xs: list[T]) -> T {
    return xs[0]
}
```

#### :first_ints

```dr
first([1, 2])
```

#### :first_more_ints

```dr
first([3, 4])
```

#### :first_strs

```dr
first(["a", "b"])
```
