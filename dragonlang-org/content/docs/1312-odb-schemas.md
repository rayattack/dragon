# Schemas, Metadata, and Integrity

ODB's founding physical principle: **required fields are the relational
skeleton, optional fields are document flesh.** A required field is stored at
a fixed offset compiled from the schema, so predicates and index maintenance
on it never parse the document at all. Optional fields ride along as a
compact tail. You choose, per field, where on the document-vs-table spectrum
your data sits - and only required fields can carry primary keys, refs, and
non-sparse indexes, because only they are guaranteed to be there.

## The schema is JSON Schema

A deliberate subset of it: `type`, `properties`, `required`, `enum`, `const`,
bounds (`minimum`/`maximum`, `minLength`/`maxLength`, `minItems`/`maxItems`),
`pattern`, `items`, `additionalProperties`, `$defs`/`$ref` for shape reuse,
and `format` for richer scalars (`date-time`, `date`, `bytes`, `decimal`).
Schemas compile to a flat validation program and a layout; nothing walks a
schema tree at runtime.

```dragon
from odb import connect, ODB, Documents

db: ODB = connect("shop.odb")

orders: Documents = db.schema("orders", {
    "type": "object",
    "properties": {
        "id":          {"type": "integer"},
        "customer_id": {"type": "integer"},
        "status":      {"type": "string"},
        "total":       {"type": "integer"}
    },
    "required": ["id", "customer_id", "status", "total"]
})
```

Two honesty rules worth knowing up front:

- **Missing and null are two different things.** A field that is not in the
document is *absent*: reads omit it, and you select on presence explicitly
with `exists!()` / `missing!()` or coalesce with `x ?? fallback` in OQL.
`null` is a stored value, allowed only when the type says so
(`"type": ["string", "null"]`).
- **There are no schema-side defaults.** A field is absent, null, or a value
a writer actually wrote - the stored document never contains data nobody
sent. Fallbacks are a read-time decision, made where the read happens
(`vip ?? false`), so changing a fallback never rewrites your data. A
schema carrying a `default` keyword is refused with `SchemaError`.

Each schema name is defined once; redefining it is refused.

## Metadata: keys, indexes, refs

Metadata is a plain document attached with `.meta()`:

```dragon
# doc: no-check
orders.meta({
    "primary": ["id"],
    "index":   ["status"],
    "refs":    {"customer_id": {"to": "customers.id", "on_delete": "restrict"}}
})
```

- `primary` names one required path. It becomes an implicit unique index and
the document's domain handle (`orders.read(id=42)`). Without it, `_id` -
the internal, never-reused docid - is the primary key. Primary key values
are immutable: identity changes are an explicit delete plus add.
- `unique` lists single fields (`"email"`) or composites
(`["customer_id", "idempotency_key"]`).
- `index` lists what you filter and sort by. A path through an array,
written `items[].sku`, makes the index multikey - you can read the
cardinality straight off the path.
- `refs` declare relationships: `"customer_id": {"to": "customers.id"}`
targets any single-path unique index in the other schema. Refs are what
OQL's nested reads join through, and every ref automatically maintains a
reverse index, so enforcing and traversing the relationship backward never
scans.

`on_delete` says what happens to referencing documents when the target goes:
`restrict` (refuse, the default posture), `cascade` (delete them too),
`set_null` (the ref type must include null), or `detach` for array-element
refs (remove just the element).

## Integrity happens at commit

All of it - unique claims, forward ref checks, `on_delete` - is validated
when the transaction commits, not statement by statement. Inside a
transaction you may leave refs temporarily dangling or uniqueness briefly
contested; the commit judges the final state. That is what makes cycles
(an employee's `manager_id` pointing at another employee) writable at all.

You did nothing wrong if you hit one of these - they are the constraints
doing their job:

```dragon
# doc: no-check
from odb.errors import IntegrityError

try {
    orders.save({"id": 13, "customer_id": 99, "status": "paid", "total": 1})
} except IntegrityError {
    print("no customer 99: the ref check refused the write")
}

try {
    db.run("del customers ? id == 1")     # orders still reference customer 1
} except IntegrityError {
    print("restrict: delete the orders first, or declare cascade")
}
```

An `IntegrityError` rolls the whole transaction back; the database is exactly
as it was before the statement.

## What the shape buys you

Because `status` and `total` above are required, `orders ? status == "paid"`
reads one fixed offset per document - no parsing, no boxing - and an index on
`status` is dense (every document has exactly one entry). The speed is not a
cache or a trick; it is the layout you declared. Documents larger than a page
spill transparently; nested required objects flatten into their parent, so
`billing.address.city` is still a single fixed-offset read.

