# Amebo on ODB: storage-layer port sketch

Design notes for moving amebo's storage layer off `sqlite::memory:` onto ODB
(the embedded object database, spec in `objectbase.md`), and what that buys for
running a fleet of amebo instances in sync.

Status: sketch only. Nothing here is implemented yet. Milestone tags (M3/M4/M5/
M8/M9/M11) refer to the ODB milestones in `objectbase.md` section 26.

---

## The question this answers

Can you run 3-5 instances of amebo backed by ODB and have them in sync at the DB
level?

## The crux: "embedded" and "in sync across instances" are opposites

ODB is an embedded, single-file, single-writer engine (COW B+tree + MVCC,
LMDB/BoltDB lineage). If each amebo instance opens its own `amebo.odb` in its own
process, five instances are five independent databases. They are not out of sync;
they are five separate brokers. Sharding does not rescue this: ODB makes sharding
a permanent non-goal ("ODB replicates, it does not shard").

So sync is never an embedded feature. It is a served/replicated feature. This is
the same reason amebo's own roadmap already lists Postgres as an open item: the
moment you want N instances, you need a shared endpoint, not an in-process file.

The good news: the ODB file format reserves the machinery for this from M0 (file
uuid = replication identity, in-file commit log region, CSN on every commit). The
path is designed in, not a future rewrite.

## Three deployment shapes, mapped to milestones

1. **Embedded, one file per instance (M3-M7): NOT in sync.** Great for a
   single-node amebo: one binary, one `amebo.odb`, ACID, kill -9 safe, zero
   external deps. Per-instance state. Do not use for a fleet.

2. **One machine, one file, many processes (M7): in sync, single-box.** Many
   readers + one writer across OS processes on one machine, via the lock sidecar
   + group commit. Real, but the box is a single point of failure, and the spec
   scopes this to the "two tools, one file" case, not to serving traffic.

3. **Served + replicated (v2/v3, M9-M11): the real answer.**
   - **Served (M9):** one ODB process owns the file; all 5 amebo instances become
     clients over the wire. They share one consistent DB. This is the Postgres
     deployment model with ODB in Postgres's seat (the served ODB is now the SPOF).
   - **Log shipping (M10):** one primary, N followers. Amebo instances get
     read-your-writes via CSN tokens. Writes to the primary, reads fan out.
     Honest cost: async-replica lag.
   - **Raft HA (M11):** the "5 instances, in sync, no SPOF" answer. ODB forms a
     Raft cluster; commits acknowledge at quorum; failover is automatic. The
     elegant part: the engine already wants exactly one committer (the COW
     meta-page flip is inherently single-writer) and Raft wants exactly one
     leader, so the Raft leader just IS the committer. Consensus and the storage
     engine want the same shape.

Direct answer: yes, when ODB reaches M11 (Raft), run 5 amebo instances against
one HA ODB cluster and they are in sync at the DB level, tolerant of a node loss.

## Two things that drive the port

1. **Amebo's `dblock: Lock` exists only because SQLite-in-memory under a
   threadpool needs a mutex.** On ODB it disappears: MVCC snapshot reads + a
   single group-committer ARE the isolation. Biggest structural change, and a
   real improvement rather than a swap.

2. **The ODB you have today is M3 (CRUD by docid only).** `db.find`/OQL raises
   until M5; uniqueness and indexes are persisted-but-not-enforced until M4.
   Amebo's hot path is all query-by-field (dedupe by `deduper`, fan out to
   subscribers where `action = ? and active`, filtered counts). So the sketch
   below is on the M5 "fully done" surface (spec section 20), with each line
   tagged for the milestone it needs. On today's M3 the only legal spelling is
   `coll.all()` + filter-in-Dragon, which scans every event per publish, an O(n)
   speed defect. The event path therefore waits for M4/M5 rather than shipping a
   scan.

---

## Part A: schema declarations (works on today's M3 API)

`migrate()`'s `create table` DDL becomes schema + metadata. Metadata is persisted
today even though enforcement is M4, so this compiles and runs now.

```dragon
from odb import ODB, Documents, connect

db: ODB = connect(getenv("AMEBO_ODB") if len(getenv("AMEBO_ODB")) > 0 else "amebo.odb")

def migrate() -> None {
    db.schema("applications", {
        type: "object",
        required: ["application", "host", "port", "secret"],
        properties: {
            application: {type: "string"},
            host: {type: "string"},
            port: {type: "integer", minimum: 0},
            secret: {type: "string"},
            active: {type: "boolean", default: true},
        },
    }).meta({primary: "application"})

    db.schema("events", {
        type: "object",
        required: ["event", "action", "payload", "deduper", "timestamped"],
        properties: {
            event: {type: "string"},
            action: {type: "string"},
            payload: {type: "object"},                 # native object, not a JSON text column
            metadata: {type: "object"},
            deduper: {type: "string"},
            timestamped: {type: "string"},
            spelled: {type: "boolean", default: false},
        },
    }).meta({
        primary: "event",
        unique: ["deduper"],                           # M4: enforces dedupe at commit
        index: ["action"],                             # M4: point-probe fan-out lookups
    })

    db.schema("subscriptions", {
        type: "object",
        required: ["subscription", "application", "action", "handler"],
        properties: {
            subscription: {type: "string"},
            application: {type: "string"},
            action: {type: "string"},
            handler: {type: "string"},
            max_retries: {type: "integer", minimum: 1, default: 3},
            active: {type: "boolean", default: true},
        },
    }).meta({
        primary: "subscription",
        index: [["action", "active"]],                 # composite: the fan-out filter
        refs: {action: {to: "actions.action", on_delete: "cascade"}},   # M4: real FK
    })

    db.schema("gists", {                               # the delivery queue
        type: "object",
        required: ["event", "subscription", "completed", "retries", "timestamped"],
        properties: {
            event: {type: "string"},
            subscription: {type: "string"},
            completed: {type: "boolean"},
            retries: {type: "integer"},
            sleep_until: {type: "integer", default: 0},
            timestamped: {type: "string"},
        },
    }).meta({
        index: [["completed", "sleep_until"]],         # the delivery-scan index
        refs: {
            event: {to: "events.event", on_delete: "cascade"},
            subscription: {to: "subscriptions.subscription", on_delete: "cascade"},
        },
    })
    # settings / audit / acl / redactions / spells map the same way.
}
```

Free wins over the SQLite version: `payload`/`metadata` are native objects (no
`json.dumps` into a text column and `json.loads` back out), and `refs` give real
referential integrity at commit (M4) instead of orphaned gists.

## Part B: the hot path, `persist_event` (M5 OQL surface)

Current SQL: count-by-deduper, insert event, `insert into gists ... select from
subscriptions where action=? and active`, count gists. On ODB, the `Lock` is gone
because the transaction is the isolation.

```dragon
class SubRow {                # monomorphized row, no boxing (typed all[T])
    def(subscription: str) { self.subscription = subscription }
}

def persist_event(event_id: str, action: str, payload: Any, metadata: Any,
                  deduper: str, ts: str, wake: int) -> int {
    fanout: int = -1
    with db.txn() as tx {                                  # snapshot isolation; no dblock
        # M4 unique index on deduper -> point probe, not a table scan
        const dup: int = tx.val[int]("events ? deduper == $d { count!() }", d=deduper)
        if dup == 0 {
            tx.run("add events { event: $e, action: $a, payload: $p, metadata: $m, deduper: $d, timestamped: $t }",
                   e=event_id, a=action, p=payload, m=metadata, d=deduper, t=ts)
            # M4 composite index (action, active) -> point probe, not a scan
            const subs: list[SubRow] = tx.all[SubRow](
                "subscriptions ? action == $a & active { subscription }", a=action)
            for s in subs {
                tx.run("add gists { event: $e, subscription: $s, completed: false, retries: 0, sleep_until: $w, timestamped: $t }",
                       e=event_id, s=s.subscription, w=wake, t=ts)
            }
            fanout = len(subs)
        }
        tx.commit()
    }
    return fanout
}
```

Race safety: if two publishers race the same `deduper`, the in-txn dedupe count
need not be perfect. The `unique: ["deduper"]` constraint makes the second
committer lose with a retryable `ConflictError`; `db.atomic(...)` (or the retry
wrapper) re-runs the closure, sees the committed event, and returns `-1`. That is
the "checked at commit, first-committer-wins" model doing the mutex's old job.

Milestone honesty: the `.meta({...})` declarations in Part A run today (M3). This
`persist_event` body needs M4 (unique/index enforcement that makes the probes
real) and M5 (OQL). On M3 the only legal spelling is `coll.all()` + filter in
Dragon, which scans every event per publish, so this function waits for M4/M5.

## Part C: the delivery daemon becomes a watch consumer (M8)

Amebo's `wizard`/delivery loop currently polls `gists` for pending work. ODB's
`watch` (resume token = CSN) turns polling into an exactly-resumable feed.

```dragon
# was: SELECT ... FROM gists WHERE completed=0 AND sleep_until<=now  on a timer
for change in db.watch("gists ? completed == false { event, subscription, retries, sleep_until }") {
    deliver(change)     # crash mid-run? resume from the last CSN, zero missed, zero double-scan
}
```

Amebo is a change-broadcast layer, and ODB hands it a durable resumable change
feed instead of a timer. The same commit log that feeds `watch` is what feeds
replicas, so the broker's core job and ODB's replication job are the same shape.

## Part D: the punchline, fleet is a connection string

Single-node embedded, served, and Raft-HA are the same `core.dr`. Only the
connect line moves.

```dragon
db: ODB = connect("amebo.odb")                      # M3+: single node, one file, embedded
db: ODB = connect("odb://broker-db:6789/amebo")     # M9:  N amebo instances, one served endpoint
db: ODB = connect("odb://broker-1,broker-2,broker-3/amebo")   # M11: Raft, quorum, auto-failover
```

Same file format and engine underneath (spec section 1: format serves all three
modes from day one), so going from one amebo to five-in-sync-no-SPOF is a topology
change, not a data migration and not a rewrite of `core.dr`.

---

## Next step when you pick this up

Option considered but not taken yet: drop Part A + stubbed Part B into
`packages/amebo/core_odb.dr` as a real compiling file (schema decls on today's M3
API; OQL hot paths as honest `raise ODBError("needs M4/M5")` stubs), plus a
`test/dr/test_amebo_odb_schema.dr` that defines the schemas against a temp `.odb`
and asserts the catalog round-trips.
