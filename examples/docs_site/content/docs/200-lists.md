# Lists and Loops

A `list[T]` holds many values of one type. The element type travels with the
list, so reading an element gives you a `T`, never a boxed `Any`:

```dragon
const names: list[str] = ["ada", "grace", "edsger"]
for name in names {
    print(name.upper())
}
```

`for` walks any iterable. To count while you go, pair it with an index:

```dragon
i: int = 0
while i < len(names) {
    print(f"{i}: {names[i]}")
    i = i + 1
}
```

Ordered steps read as an ordered list:

1. Declare the list with its element type.
2. Loop over it with `for` or index it with `while`.
3. Each element comes back at its real type - call `str` methods on a
   `list[str]` element with no cast.
