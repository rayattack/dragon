# OQL: Reading

OQL's design rule: **the query looks like the answer.** You write the shape
you want back; filters hang off sources with `?`; relationships you declared
in metadata are traversed by just naming them. Punctuation for reads, words
for writes, and every mistake is a prepare-time error with a position - never
a wrong result at runtime.

You write a query as a `template[OQL]` value. Every `!{expr}` inside it is a
**bound parameter** - injected as a value at prepare time, never spliced into
the query text - so an injection payload is just a string nobody matches, not
a query that runs. The typed verbs return your own row classes:

```dragon
from odb import connect, ODB, Document, OQL

db: ODB = connect("shop.odb")

class Customer {
    id: int
    name: str
    email: str
    city: str

    def(d: Document) {          # a row pulls its fields from the matched document
        self.id = d["id"]
        self.name = d["name"]
        self.email = d["email"]
        self.city = d["city"]
    }
}

# all[T]: every match, hydrated into T
where: str = "Lagos"
rows: list[Customer] = db.all[Customer](template[OQL] { customers ? city == !{where} })
print(rows[0].name)

# one[T]: exactly one match, else NoRows / MultipleRows
uid: int = 1
c: Customer = db.one[Customer](template[OQL] { customers ? id == !{uid} })

# val[T]: one row, one projected field, returned as the scalar
age: int = db.val[int](template[OQL] { customers ? id == !{uid} { age } })
```

`one[T]` raises `NoRows` when nothing matched and `MultipleRows` when more than
one did; `val[T]` additionally requires the projection to name exactly one
plain field. Try to build a query by string-joining user input and you cannot -
there is no string to join; the value goes through `!{}` or it is not in the
query.

## Shapes and filters

A projection block selects fields; no block returns the whole document. Filters
compose with `&`, `|`, `!` and parentheses; comparisons are the usual six
(`== != < <= > >=`). String matching is glob `~` (case-sensitive `*`/`?`) and
`~*` (case-insensitive); membership is `in`:

```dragon
# doc: no-check
db.all[Customer](template[OQL] { customers ? name ~ !{pattern} { name } })
db.find(template[OQL] { orders ? status in ["paid", "shipped"] })
db.find(template[OQL] { customers ? city == !{c} & (vip | missing!(vip)) })
```

`db.find` is the untyped read: it returns `list[Document]` (subscript with
`row["field"]`) and is what you reach for when the result does not map to a
single row class - nested reads and aggregates below both use it.

OQL is its own small language, not embedded Dragon: booleans are spelled
`true` and `false`, and built-in functions take a trailing `!` (`len!`,
`lower!`, `year!`, `count!`) so an engine operation can never collide with a
field that happens to be named `count`.

## Nested reads follow your refs

Declared refs are the join conditions - metadata IS the on-clause. Mention
the related schema inside a projection block and you get it nested, with the
cardinality driving the shape: a 1:N ref nests an array, an N:1 ref nests an
object.

```dragon
from odb import connect, ODB, Document, OQL

db: ODB = connect("shop.odb")

# each customer with their paid orders nested inside
status: str = "paid"
rows: list[Document] = db.find(
    template[OQL] { customers { name, orders ? status == !{status} { id, total } } })
n_paid: int = len(rows[0]["orders"])
```

The parent always appears; a child array may just be empty (a left join, in
SQL words). When you want the join to *filter* the parents instead, use the
existence operators - `+` keeps parents that have a match, `-` keeps parents
that have none:

```dragon
# doc: no-check
db.find(template[OQL] { customers + (orders ? status == !{status}) { name } })
db.find(template[OQL] { customers - orders { name } })   # with no orders at all
```

## Aggregates and grouping

A block of only aggregates summarizes the whole set; `by` groups first and
reads like English:

```dragon
from odb import connect, ODB, Document, OQL

db: ODB = connect("shop.odb")

totals: list[Document] = db.find(template[OQL] { orders { n: count!(), revenue: sum!(total) } })
print(totals[0]["n"], totals[0]["revenue"])

per: list[Document] = db.find(
    template[OQL] { orders by status { status, n: count!(), revenue: sum!(total) } })
```

Available aggregates: `count!()`, `count!(distinct x)`, `sum!`, `avg!`,
`min!`, `max!`, `any!`, `all!`. Aggregates summarize the source you are
standing on: to summarize a relationship, query from the many side (group
`orders by` and join what you need), or nest a block and reduce in Dragon.

## Pipelines

Stages after the block transform the result set, in order:

```dragon
from odb import connect, ODB, Document, OQL

db: ODB = connect("shop.odb")

floor: int = 0
top: list[Document] = db.find(
    template[OQL] { orders ? total > !{floor} { id, total } | sort -total, id | take 10 | skip 0 })
```

`sort` is stable, `-field` descends, ties break on `_id`, and absent values
order after present ones regardless of direction.

## Two-valued logic - no SQL NULL trap

Every predicate is true or false, never *unknown*:

- A comparison touching an absent field is false.
- `!p` is pure negation. The consequence to internalize: `!(x > 3)` is TRUE
for documents that have no `x`, while `x <= 3` is false for them. When
presence is the question, ask it explicitly with `exists!(x)` /
`missing!(x)`.
- `x == null` matches stored nulls only, never absence.
- `x ?? fallback` coalesces both null and absence - which is what callers
almost always want:

```dragon
from odb import connect, ODB, Document, OQL

db: ODB = connect("shop.odb")

rows: list[Document] = db.find(template[OQL] { customers { name, tier: vip ?? false } })
```

## Mistakes fail before they run

OQL is typed at prepare time against your compiled schemas. An unknown
field, a type-mismatched comparison, an aggregate outside a block - all are
`PrepareError` before any document is touched:

```dragon
from odb import connect, ODB, OQL
from odb.errors import PrepareError

db: ODB = connect("shop.odb")

try {
    db.find(template[OQL] { customers ? age == 3 })        # no such field
} except PrepareError as e {
    print(e)
}
try {
    db.find(template[OQL] { customers ? id == "x" })       # int compared to a string
} except PrepareError as e {
    print(e)
}
```

Plans are cached by the query's canonical text, so the prepare cost is paid
once per distinct query, not once per call - and two calls that differ only in
their `!{}` values share the one cached plan.

## When the query text is dynamic

Sometimes the query itself is data: an admin shell, a saved-query field, a
report builder. For those, `find` and `run` also accept plain text with
`$name` parameters, injected as values exactly like `!{}`:

```dragon
from odb import connect, ODB, Document

db: ODB = connect("shop.odb")

def load_saved_query() -> str {
    return "customers ? city == $c { name }"
}

q: str = load_saved_query()                         # text decided at runtime
rows: list[Document] = db.find(q, {"c": "Kano"})    # $c inside q is a bound value
```

Reach for this only when the query text is genuinely not known at compile
time. When you are writing the query, write it as `template[OQL]`: the
compiler interns it, the parameters are values by construction, and the rows
come back typed.
