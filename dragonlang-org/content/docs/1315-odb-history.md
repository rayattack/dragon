# History: Watch, Time Travel, and Retention

Every ODB commit appends one logical entry to a commit log that lives inside
the same `.odb` file: what changed, in which transaction, at which commit
sequence number (CSN). Three features fall straight out of it - change
feeds, time travel, and replication - and one knob governs how much history
the file keeps.

## Retention

```dragon
from odb import connect, ODB

db: ODB = connect("shop.odb")

db.retain("1024 txns")     # the default
db.retain("7d")            # or a duration: s, m, h, d
db.retain("none")          # no history: smallest file, no feeds, no travel
```

Retention pins old snapshots exactly like a live reader would, so the cost of
history is visible where it belongs - in file size - and never hidden. Trim
happens as you commit; `retain` persists in the file and survives reopen.

## Watch: a query over time

A watch is an ordinary OQL query pointed at the future. It sees every
matching committed change after the point it was opened, shaped by its own
projection:

```dragon
from odb import connect, ODB, OQL, Watch, Change

db: ODB = connect("shop.odb")

status: str = "paid"
w: Watch = db.watch(template[OQL] { orders ? status == !{status} { id, total } })

db.run(template[OQL] { add orders { id: 30, customer_id: 4, status: "paid", total: 60 } })

hits: list[Change] = w.poll()
print(hits[0].kind)              # "add"
print(hits[0].doc["total"])      # 60 - the post-image, projected
token: int = w.resume_token()    # a CSN: reopen exactly here later
```

`poll()` drains everything committed since the last poll. A `Change` carries
the kind (`add`, `set`, `put`, `del`, `define`), the projected post-image
(deletes identify themselves by primary key instead), and the CSN. The
resume token makes feeds **exactly resumable**: store it, restart, and
`db.watch(query, token)` continues from precisely that commit. If retention
has already trimmed past your token, the watch fails loudly rather than
silently skipping changes you never saw - re-seed and move on. For the raw,
unprojected stream there is also `db.changes(since_csn)`.

## Time travel

Any read can run against a past snapshot by suffixing the source with the
CSN. `db.csn()` tells you where the database is now, so bracketing a change
is natural:

```dragon
from odb import connect, ODB, Document, OQL

db: ODB = connect("shop.odb")

at: int = db.csn()
db.run(template[OQL] { set customers ? id == 4 { city: "Enugu" } })

# @csn(N) is a structural coordinate - which snapshot - not a value, so it is
# named directly in the source from the trusted CSN db.csn() handed you. Any
# value filter in a time-travel read still binds through !{} or $name.
then: list[Document] = db.find("customers@csn(" + str(at) + ") ? id == 4 { city }")
now_: list[Document] = db.find(template[OQL] { customers ? id == 4 { city } })
print(then[0]["city"], "->", now_[0]["city"])    # Lagos -> Enugu
```

The old snapshot is read through the same copy-on-write pages it was
committed to - no replay, no reconstruction, valid for anything inside the
retention window.

## Backup, restore, and shrink

```dragon
from odb import connect, ODB

db: ODB = connect("shop.odb")

text: str = db.dump()            # every schema, meta, and document, JSONL
db2: ODB = connect("restored.odb")
loaded: int = db2.load(text)     # rebuilt in ref-safe order

db.compact("backup.odb")         # VACUUM-INTO-style snapshot copy
```

Two different tools for two different jobs:

- `dump()`/`load()` is the logical round trip: human-readable, diffable,
version-independent, and the escape hatch a format upgrade asks for.
- `compact(dest)` rebuilds the live data into a fresh, tightly-packed file
while you keep serving - it doubles as a hot backup. Compaction is also
the vacuum: it deliberately drops history, so the copy starts a new
lineage with the retention knob carried over but the log empty. To seed a
replica that must *continue* this file's history, copy the file bytes
instead (see [Replication](/docs/1316-odb-replication)).

And whenever you want proof, not vibes:

```dragon
from odb import connect, ODB

db: ODB = connect("shop.odb")

print(db.check().ok)             # structure, indexes, refs - one pass
```
