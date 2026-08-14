# OQL: Writing and Transactions

Reads get punctuation; writes get visible words: `add`, `set`, `unset`,
`put`, `del`. You write them as `template[OQL]` values too, so every `!{expr}`
is a bound value and never part of the statement text; `db.run()` returns the
number of documents affected, and every single call is one atomic
transaction - including its integrity checks.

```dragon
from odb import connect, ODB, OQL

db: ODB = connect("shop.odb")

name: str = "Tunde"
n: int = db.run(template[OQL] { add customers { id: 4, name: !{name}, email: "t@x.com", city: "Lagos" } })
print(n)                                   # 1

# batch add: one transaction, all-or-nothing
db.run(template[OQL] { add orders [
    { id: 20, customer_id: 4, status: "paid", total: 60 },
    { id: 21, customer_id: 4, status: "draft", total: 15 }] })
```

## The write vocabulary

**`set` is a deep merge.** Objects merge recursively; scalars and arrays are
replaced whole. The right-hand side is a real expression over the current
document, so counters and derived updates stay in the database - and a bound
`!{value}` slots into that expression as data:

```dragon
from odb import connect, ODB, OQL

db: ODB = connect("shop.odb")

city: str = "Ibadan"
bump: int = 5
db.run(template[OQL] { set customers ? id == 4 { city: !{city} } })
db.run(template[OQL] { set orders ? id == 20 { total: total + !{bump} } })
```

**`unset` removes optional fields.** It is the only way to remove a field -
there is no magic null - and unsetting a required field is a prepare-time
error, because the document would no longer match its schema:

```dragon
from odb import connect, ODB, OQL

db: ODB = connect("shop.odb")

db.run(template[OQL] { unset customers ? id == 4 { vip } })
```

**`put` replaces the whole document.** The docid and primary key survive; a
primary key in the new body must match or be absent:

```dragon
from odb import connect, ODB, OQL

db: ODB = connect("shop.odb")

name: str = "Tunde"
email: str = "t@x.com"
city: str = "Ibadan"
db.run(template[OQL] { put customers ? id == 4 { id: 4, name: !{name}, email: !{email}, city: !{city} } })
```

**`del` deletes whatever matches**, honoring every ref's `on_delete` rule at
commit:

```dragon
from odb import connect, ODB, OQL

db: ODB = connect("shop.odb")

status: str = "draft"
db.run(template[OQL] { del orders ? status == !{status} })
```

## atomic blocks

`atomic { ... }` groups statements into one transaction. Everything commits
together or nothing does, and integrity is judged on the *final* state - so
inside the block you may reorder freely, even leaving a ref dangling for a
statement or two:

```dragon
from odb import connect, ODB, OQL

db: ODB = connect("shop.odb")

db.run(template[OQL] { atomic {
    add orders { id: 22, customer_id: 4, status: "paid", total: 60 }
    set customers ? id == 4 { city: "Lagos" }
} })
```

The return value is the total count of affected documents across the block.

## Handles or OQL?

The `Documents` handle from the [first chapter](/docs/1311-odb) and OQL
writes are the same machinery. Use the handle when you hold a specific
document (`doc.edit({...})`, `doc.wipe()`, `customers.save({...})`); use OQL
when the *predicate* is the point (`set customers ? city == !{c} { ... }`
touches them all in one transaction). Neither is faster by category; both
validate, both maintain indexes, both are atomic.

## When a write is refused

The error taxonomy is small and each class means one thing:


| Error             | Meaning                                            |
| ----------------- | -------------------------------------------------- |
| `SchemaError`     | a definition or registration problem               |
| `PrepareError`    | the statement was rejected before executing        |
| `IntegrityError`  | unique, ref, or on_delete violation at commit      |
| `ConflictError`   | optimistic commit collision; retry the transaction |
| `CorruptionError` | checksum or structural damage; never swallowed     |
| `NotFound`        | a document or schema that was expected to exist    |


A refused write rolls back completely. There is no partial state to clean
up, ever - that is what "integrity at commit" buys:

```dragon
from odb import connect, ODB, OQL
from odb.errors import IntegrityError

db: ODB = connect("shop.odb")

try {
    db.run(template[OQL] { del customers ? id == 4 })   # orders still reference it
} except IntegrityError {
    pass                                                # nothing was deleted, nothing changed
}
```

When the statement text itself is dynamic - a migration tool, a shell - `run`
also takes plain text with `$name` parameters (`db.run("del orders ? status ==
$s", {"s": "draft"})`), injected as values just like `!{}`. Reach for it only
when you are not the one writing the query.
