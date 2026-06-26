# Member Privacy

In Python a leading underscore is pure documentation - `_x` is advisory, `__x` is
name-mangled but still reachable, and nothing actually stops outside code from
poking at either. Dragon turns the convention you already write into a guarantee
the compiler enforces. Privacy is checked entirely at compile time by the type
checker; it carries no runtime cost.

The leading underscores you'd write out of habit now carry meaning:

- A **single** leading underscore - `_name` - marks a member **protected**. The
  declaring class and its subclasses may use it, and so may other code in the same
  package. Code in another package may not.
- A **double** leading underscore - `__name` (no trailing `__`) - marks a member
  **private**. Only the declaring class itself may touch it - *not* subclasses,
  *not* outside code, regardless of package.

(Trailing-dunder names like `__str__` are a separate, reserved namespace - the
[special-method protocol](/docs/0604-dunder-methods) - and stay public. That's why
`def __str__()` is reachable from anywhere.)

## Private: the declaring class alone

Inside the class, a private member is ordinary - read and write it through `self`:

```dragon
class Account {
    __balance: int = 0       # private - only Account can touch it

    def deposit(n: int) -> None {
        self.__balance = self.__balance + n
    }
    def balance() -> int {
        return self.__balance
    }
}

a: Account = Account()
a.deposit(100)
a.deposit(50)
print(a.balance())   # 150
```

Reach for `__balance` from *outside* the class and the compiler stops you. This
does **not** compile:

```dragon
a: Account = Account()
a.deposit(100)
print(a.__balance)   # error
```

```text
'__balance' is private to Account; subclasses and outside code cannot access it
```

The error is identical whether you read or write, and whether the access sits in
free code or in a *subclass* of `Account` - a private member is visible to the
declaring class alone.

## Protected: shared with subclasses

A subclass that wants to share state with its parent uses the protected tier.
`_state` below is readable from `Derived` because a subclass is in scope for
protected members:

```dragon
class Base {
    _state: int = 0          # protected - class, subclasses, same package

    def set(n: int) -> None {
        self._state = n
    }
}

class Derived(Base) {
    def doubled() -> int {
        return self._state * 2   # a subclass may read a protected field
    }
}

d: Derived = Derived()
d.set(21)
print(d.doubled())   # 42
```

## Choosing a tier

The two tiers are deliberately soft (`_`) and hard (`__`): use `_` for internals
that collaborating types in the same package - or a subclass - are meant to share,
and `__` for state nothing outside the declaring class has any business seeing.
The same rules apply to methods, and they hold identically in `.dr` and `.py`
files.

The convention also governs **module-level** names: a `_helper` or `__internal`
declared at module scope is package- or file-private in the same way - see
[Modules and Packages](/docs/1001-modules) for module-level privacy.

## At a glance

| Prefix | Tier | Who may access |
|--------|------|----------------|
| `name` | public | anyone |
| `_name` | protected | declaring class, subclasses, same package |
| `__name` | private | declaring class only |
| `__name__` | reserved | the special-method protocol - public |

That completes Part 6. Dragon gives you Python's object model on C's memory layout
- flat structs, fixed-offset fields, direct calls, a single vtable dereference
only where polymorphism demands one, and compiler-enforced encapsulation. Next,
the system that makes all of this checkable: [The Type System](/docs/0701-type-annotations).
