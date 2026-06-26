# Hello, Dragon

Every Dragon program is just the top-level statements of the file you hand to
`dragon run`. There is no `main` to declare:

```dragon
print("Hello, Dragon!")
```

Functions take typed parameters and return a typed value. Annotations are
mandatory - they are how the compiler keeps every value flowing at a known,
unboxed type:

```dragon
def greet(name: str) -> str {
    return f"Hello, {name}!"
}

print(greet("world"))
```

A few things to notice:

- `f"..."` is an interpolated string - `{name}` splices the value in.
- `: str` and `-> str` are required; Dragon never guesses a type.
- The call runs because top-level code calls it, not because it is named a
  special name.
