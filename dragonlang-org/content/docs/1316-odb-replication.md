# Replication and HA

One `.odb` file is one lineage of committed history, identified by the pair
every replica carries: the file uuid and the commit sequence number (CSN).
Replication ships that history; HA lets a cluster agree on it. Both ride the
same commit log you met in the [previous chapter](/docs/1315-odb-history) -
there is no second machinery.

## Followers: shipped history

A follower is a complete local database that applies a primary's committed
log entries at the primary's own CSNs, so the lineage pair stays in lockstep.
Reads on a follower are ordinary local reads - snapshot-consistent, never
waiting on the network.

Serve the primary (the native wire protocol is a few lines):

```dragon
from odb import connect, ODB
from odb.server import ODBServer

primary: ODB = connect("app.odb")
srv: ODBServer = ODBServer(primary, 7440)
serving: Task[None] = fire srv.serve()
# ... your application runs ...
await serving
```

And follow it:

```dragon
from odb import connect, ODB
from odb.client import ODBClient
from odb.repl import Follower

f: Follower = Follower(connect("replica.odb"), ODBClient("127.0.0.1", 7440))
applied: int = f.sync_once()      # pull and apply everything new
```

Seeding is a choice of two honest starts: **copy the file bytes** (a hot
byte-copy inherits the uuid and CSN and resumes right where the copy was
taken), or start with a **brand-new empty file**, which adopts the primary's
identity and replays full history from the beginning - provided the
primary's retention window still holds it. A follower that has fallen behind
the window gets a loud re-seed error; history is never silently skipped.

While paired, the follower's own file refuses local writes - a local commit
would fork the lineage. `detach()` lifts the latch and the file becomes an
ordinary writable database: that is promotion, and it is deliberate.

**Read-your-writes** falls out of CSNs: every write returns under a CSN, a
client carries the largest CSN it has written, and `f.wait_for(csn, ms)`
holds a follower read until the follower has caught up to it. A served
follower does the same for its own clients by wiring a `FollowerWaiter` into
its `ODBServer`.

## HA: the cluster agrees before anyone commits

For automatic failover, replicas form a Raft cluster over the commit log.
The consensus engine is the `raft` egg (`packages/raft`) - a standalone,
deterministic state machine, storm-tested against message loss, reordering,
partitions, and crash-restarts - and `odb.ha` binds it to the database:

```dragon
from odb import connect, ODB
from odb.ha import HAReplica, RaftTransport, CONFIRMED

db: ODB = connect("node1.odb")
transport: RaftTransport = RaftTransport()
seed: int = 42
rep: HAReplica = HAReplica(db, 1, [1, 2, 3], transport, seed)
```

The `RaftTransport` seam is how protocol messages leave a replica; deliver
incoming ones with `rep.deliver(msg)` and drive time with `rep.tick()`.

The write path is the point. The elected leader executes a write exactly
like a local one - validation, docids, integrity - but nothing commits until
a majority of the cluster holds the entry:

```dragon
idx: int = rep.run("""add orders { id: 1, customer_id: 4, status: "paid", total: 60 }""")

# ... the cluster exchanges messages ...

if rep.outcome(idx) == CONFIRMED {
    token: int = idx + 1          # this write's CSN, on every replica
}
```

`CONFIRMED` means durable on a quorum, applied at the same CSN clusterwide,
and impossible to lose to a failover. The other answer is `SUPERSEDED`: the
proposing leader lost leadership before a quorum held the write, consensus
gave that slot to the new leader's history, and the write applied *nowhere* -
not even on the node that proposed it. Retry it against the new leader.
There is no third outcome, which is precisely the claim: no replica's
database can fork.

When a leader dies, the cluster elects a new one and writes continue; when
the old leader returns, it rejoins as a follower and is caught up
automatically - through Raft for the recent tail, through the committed
history feed when it is further behind. Every replica keeps consensus state
(term, vote, and the unapplied tail) inside its own `.odb` file, so a
restarted node resumes from the file alone.

Reads never change: any replica serves its local snapshot, and the CSN token
from a confirmed write is the same read-your-writes hold as in log shipping.

## Choosing between them

- **One writer, many read replicas, manual promotion:** followers. Simplest
operationally; a follower is also a live, always-current backup.
- **Automatic failover, no lost acknowledged writes:** an HA cluster of
three or five replicas. Quorum on every commit is the price; an election
instead of a pager duty call is the payoff.
- **Sharding:** no. ODB replicates; it does not shard. The single-file
thesis is the product.



