"""Decision 044 - generics in .py indentation mode (surface != semantics).

The same monomorphization engine backs `.py` files: `class Foo[T]:` /
`def f[T]()` parse and lower identically to their `.dr` forms. This is the
`.py`-mode half of the test matrix, including bounded `T: B` - the bound syntax
goes through the same unified `parseTypeParams`, so `.py` gets it for free.
"""

from unittest import TestCase, main


class Box[T]:
    def __init__(self, v: T) -> None:
        self.value: T = v

    def get(self) -> T:
        return self.value


class Stack[T]:
    items: list[T] = []

    def push(self, x: T) -> None:
        self.items.append(x)

    def pop(self) -> T:
        return self.items.pop()


def first[T](xs: list[T]) -> T:
    return xs[0]


class Animal:
    name: str

    def __init__(self, name: str) -> None:
        self.name = name

    def speak(self) -> str:
        return self.name + " sound"


class Dog(Animal):
    def __init__(self, name: str) -> None:
        self.name = name

    def speak(self) -> str:
        return self.name + " woof"


# Bounded type parameter: the bound's members are usable on T.
def describe[T: Animal](x: T) -> str:
    return x.name + ": " + x.speak()


class Shelter[T: Animal]:
    def __init__(self, occupant: T) -> None:
        self.occupant: T = occupant

    def announce(self) -> str:
        return self.occupant.speak()


class GenericsPyTests(TestCase):
    def test_box(self) -> None:
        b: Box[int] = Box[int](7)
        self.assertEqual(b.get(), 7)

    def test_box_inferred(self) -> None:
        b: Box[str] = Box("hi")
        self.assertEqual(b.get(), "hi")

    def test_stack(self) -> None:
        s: Stack[int] = Stack[int]()
        s.push(1)
        s.push(2)
        self.assertEqual(s.pop(), 2)

    def test_first(self) -> None:
        xs: list[int] = [100, 200]
        self.assertEqual(first(xs), 100)

    def test_bounded_member_access(self) -> None:
        a: Animal = Animal("Critter")
        self.assertEqual(describe[Animal](a), "Critter: Critter sound")

    def test_bounded_subclass_dispatch(self) -> None:
        d: Dog = Dog("Rex")
        self.assertEqual(describe(d), "Rex: Rex woof")

    def test_bounded_generic_class(self) -> None:
        s: Shelter[Dog] = Shelter[Dog](Dog("Bingo"))
        self.assertEqual(s.announce(), "Bingo woof")


main([GenericsPyTests()])
