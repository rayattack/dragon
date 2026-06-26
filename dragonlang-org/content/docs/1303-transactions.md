# Transactions

Some operations only make sense all-or-nothing. Moving money between two accounts is
a debit *and* a credit; if the debit lands but the credit fails, you've destroyed
money. A transaction groups several statements into one atomic unit: they all commit
together, or none of them do. Dragon exposes this through `db.transaction()` and a
`with` block, with one deliberate design choice - **the commit is explicit.**

## `db.transaction()` and explicit commit

`db.transaction()` returns a `Tx`, a context manager. Inside the block you run
statements through the `tx` handle (it has the same `run`/`all`/`one`/`val` verbs as
the connection), and you call `tx.commit()` to make the changes permanent:

```dragon
import database
from database import SQL

with database.open("sqlite::memory:") as db {
    db.raw("create table accounts(id integer primary key, balance integer)")
    db.run(template[SQL] { insert into accounts(id, balance) values(!{1}, !{100}) })
    db.run(template[SQL] { insert into accounts(id, balance) values(!{2}, !{0}) })

    # Move 30 from account 1 to account 2 - atomically.
    with db.transaction() as tx {
        tx.run(template[SQL] { update accounts set balance = balance - !{30} where id = !{1} })
        tx.run(template[SQL] { update accounts set balance = balance + !{30} where id = !{2} })
        tx.commit()
    }

    a: int = db.val(template[SQL] { select balance from accounts where id = !{1} })
    b: int = db.val(template[SQL] { select balance from accounts where id = !{2} })
    print(a, b)   # 70 30
}
```

## No commit means rollback

Here's the safety property. If you leave the block **without** calling `tx.commit()`
- because you returned early, because `tx.rollback()` was called, or because a `raise`
unwound out of the block - the transaction is **rolled back**. Nothing it did
persists:

```dragon
with db.transaction() as tx {
    tx.run(template[SQL] { insert into t(n) values(!{1}) })
    tx.run(template[SQL] { insert into t(n) values(!{2}) })
    # no commit() - these inserts are discarded at block exit
}
# the table is unchanged
```

This is the conservative default: a transaction you don't explicitly finish is
abandoned, not silently saved. It follows directly from Dragon's context-manager
design - `__exit__` takes no exception arguments (see
[Context Managers](/docs/0903-context-managers)), so the `Tx` can't inspect *why* the
block ended to decide whether to commit. Rather than guess, it rolls back unless you
said otherwise. The result is that the dangerous outcome - a half-finished
transaction quietly committed - is one your code can't accidentally produce.

`tx.rollback()` abandons the transaction explicitly, when you've decided mid-block
that it shouldn't proceed:

```dragon
with db.transaction() as tx {
    tx.run(template[SQL] { update inventory set qty = qty - !{n} where sku = !{sku} })
    on_hand: int = tx.val(template[SQL] { select qty from inventory where sku = !{sku} })
    if on_hand < 0 {
        tx.rollback()        # oversold - undo it
    } else {
        tx.commit()
    }
}
```

## Batches: `runs`

When you just have a fixed list of statements to apply together - a migration, a bulk
insert - `db.runs([...])` executes them as one batch without the explicit-commit
ceremony:

```dragon
db.runs([
    template[SQL] { insert into players(name, score) values(!{"Ada"}, !{10}) },
    template[SQL] { insert into players(name, score) values(!{"Linus"}, !{20}) },
    template[SQL] { insert into players(name, score) values(!{"Grace"}, !{30}) },
])
```

Reach for `runs` when the set of statements is known up front; reach for
`transaction()` when the work has conditional logic - a read that decides whether to
write, an early `rollback` on a failed invariant.

## At a glance

| You want to... | Write |
|----------------|-------|
| An atomic group of writes | `with db.transaction() as tx { ... tx.commit() }` |
| Run inside the transaction | `tx.run(...)`, `tx.val(...)`, `tx.all(...)` |
| Make it permanent | `tx.commit()` (required - no commit = rollback) |
| Abandon it | `tx.rollback()`, or just leave the block uncommitted |
| Apply a fixed batch | `db.runs([ ... ])` |

Transactions, queries, and the SQL template are identical across backends. The one
thing that differs - the connection string - is the last chapter:
[SQLite, PostgreSQL, and MySQL](/docs/1304-backends).
