# ReplOwnershipTest cases

Each block is one REPL cell. A test drives them as a sequence of turns against a
live `ReplSession` and runs under ASan/LSan, because everything the REPL adds
sits on top of the module-global ownership paths in `src/codegen/Assign.cpp` and
`src/codegen/AugAnnAssign.cpp`.

Cross-turn rebinding is the whole risk surface: a REPL global is released on
reassignment rather than at scope exit, and it is stored through the same
shared-global write barrier that lets a fired vthread read it without racing the
refcount. Getting either wrong is a leak or a use-after-free per assignment.

## Rebind churn

A list global rebound thousands of times. Each rebind must release the previous
list exactly once. A leak here is linear and shows up immediately; an
over-release is a use-after-free ASan catches on the spot.

#### :rebind_churn_seed

```dr
xs: list[int] = [1, 2, 3]
```

#### :rebind_churn_step

```dr
xs = [1, 2, 3, 4]
```

The string case runs the `dragon_mark_shared_str` arm of the barrier instead.
Heap strings are refcounted and shared-marked, so a missed release leaks and a
double release corrupts the heap.

#### :str_rebind_churn_seed

```dr
s: str = "seed"
```

#### :str_rebind_churn_step

```dr
s = "a longer replacement value that will not fit in any small-string buffer"
```

A union global boxes its value, so rebinding runs `dragon_mark_shared_boxed`.

#### :union_rebind_churn_seed

```dr
v: int | str = 1
```

#### :union_rebind_churn_step

```dr
v = "now a string"
```

## Container mutated across turns

The element-kind metadata for `xs` is name-keyed and lives in `CodeGen::Impl`,
which is destroyed at the end of every turn. Turn N must re-seed it from the
session's type environment rather than inherit it. If the kind is wrong, the
append writes through the wrong element path.

#### :container_mutate_seed

```dr
items: list[str] = []
```

#### :container_mutate_step

```dr
items.append("one more")
```

#### :container_mutate_read

```dr
print(len(items))
```

## Class instance global rebound

Rebinding an instance global must run the class-instance dealloc path on the
previous value, not the scalar one.

#### :class_instance_seed

```dr
class Holder {
    label: str

    def(label: str) {
        self.label = label
    }
}

h: Holder = Holder("first")
```

#### :class_instance_step

```dr
h = Holder("replacement")
```

#### :class_instance_read

```dr
print(h.label)
```

## Raise mid-assignment

The turn's module is already in the JIT and `y`'s global already exists,
zero-initialised, by the time the exception fires. Rolling back must leave no
binding and no symbol, and the name must be reusable afterwards. Keeping the
symbol collides on the next annotation; keeping the binding leaves `y` a null
string.

#### :raise_mid_assign_helper

```dr
def boom() -> str {
    raise ValueError("no config")
}
```

#### :raise_mid_assign

```dr
y: str = boom()
```

#### :raise_mid_assign_rebind

```dr
y: str = "recovered"
```

#### :raise_mid_assign_read

```dr
print(y)
```

## Uncaught raise with live temporaries

`s.strip()` allocates a temporary that is still alive when `int()` raises.
Exceptions lower through setjmp/longjmp, so LLVM never runs cleanups on the
unwind path: the shell's catch frame must run the same unwind cleanup an
`except` block gets, or every raise at the prompt leaks the skipped frames'
temporaries.

#### :uncaught_raise_helper

```dr
def parse_it(s: str) -> int {
    return int(s.strip())
}
```

#### :uncaught_raise_step

```dr
print(parse_it("  not a number  "))
```

## Global captured by a fired vthread

A global bound in one turn and read by a vthread fired in a later turn is
exactly the scenario the shared-global write barrier exists for. Without the
mark, the vthread races the refcount.

#### :fired_vthread_seed

```dr
shared_label: str = "visible from the vthread"
```

#### :fired_vthread_step

```dr
def show() -> int {
    return len(shared_label)
}

t: Task[int] = fire show()
print(await t)
```

## Reset must release what the globals hold

`:reset` clears the session's JITDylib, which unmaps the globals' storage. The
values those globals point at are separate heap allocations and have to be
released first, or every reset in a long-lived session leaks the live set.

These cases are driven as cycles of bind-then-reset. After the final reset
nothing is live, so an LSan report here is a real leak and not the
JIT-roots-are-not-scanned false positive that a session with live globals shows.

#### :reset_cycle_list

```dr
xs: list[int] = [1, 2, 3, 4, 5, 6, 7, 8]
```

#### :reset_cycle_str

```dr
s: str = "a heap string long enough to be its own allocation, not a literal"
```

#### :reset_cycle_union

```dr
v: int | str = "boxed payload that must be released through the union path"
```

#### :reset_cycle_instance

```dr
class Held {
    name: str

    def(name: str) {
        self.name = name
    }
}

held: Held = Held("instance that reset must release")
```

#### :reset_cycle_nested

```dr
rows: list[list[str]] = [["a", "b"], ["c", "d"]]
```

## Function and class defined in one turn, used in a later one

Not an ownership case, but the resident-symbol path it exercises is what the
ownership cases ride on, so it is checked in the same suite.

#### :cross_turn_def

```dr
def double(n: int) -> int {
    return n * 2
}
```

#### :cross_turn_use

```dr
print(double(21))
```
