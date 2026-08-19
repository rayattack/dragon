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
`default`, bounds (`minimum`/`maximum`, `exclusiveMinimum`/`exclusiveMaximum`,
`minLength`/`maxLength`, `minItems`/`maxItems`), `pattern`, `items`,
`additionalProperties`, `$defs`/`$ref` for shape reuse, `oneOf` as a
discriminated union, and `format` for richer scalars (`date-time`, `date`,
`bytes`, `decimal`).

Anything outside that subset is a registration error, never a silent no-op. That
covers what odb refuses on principle (`patternProperties`, `not`, `if`/`then`/
`else`, `dependentSchemas`, `unevaluatedProperties`, undiscriminated
`anyOf`/`allOf`, cross-file `$ref`) and equally anything simply not implemented
(`multipleOf`, `uniqueItems`, `prefixItems`, an unknown `format`). A constraint
you wrote is a constraint that is enforced, or you are told at `db.schema()`
which is not.

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
- **`default` means two different things, and the schema says which.** On a
*required* field it is materialized at write: a writer may omit the field and
the value is stamped into the document, because required fields need physical
presence. On an *optional* field it stays virtual: the stored document keeps
its small shape and reads apply the current default, so changing that default
retroactively changes what old documents read as. A default that would violate
its own field is refused at compile time. An absent optional field with no
default is simply omitted from reads, never a silent zero.

`db.schema(name, definition)` defines a name once. To change a shape later, use
`db.redefine(name, definition)`, which bumps the schema version and keeps the old
layout so documents written under it still read correctly. Field identity is a
`u16` id from an append-only per-schema symbol table, not a position, so inserting
a property that sorts early cannot re-map existing records. Pass
`db.redefine(name, definition, {"old": "new"})` to rename a field and keep its id
and its stored values.

A redefine is refused rather than silently reinterpreting your data when it would
change a field's type, make a required field optional, drop a field an index still
uses, or add a required field with no `default`. If it only tightens constraints,
registration scans the stored documents first and fails listing the ones that
would no longer validate.

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
- `index` lists what you filter and sort by. A path may walk into a nested
object (`billing.postcode`) or into array elements (`items[].sku`, which makes
the index multikey, so you read the cardinality straight off the path). Walking
through an array *without* the brackets is refused rather than quietly becoming
multikey.
- Every segment of a path must be required, unless the entry opts out with the
object form: `{"path": "referrer_code", "sparse": true}`. A sparse index holds
entries only for documents where the whole path exists, so `unique` + `sparse`
means uniqueness among the values that are present.
- `refs` declare relationships: `"customer_id": {"to": "customers.id"}`
targets any single-path unique index in the other schema. Refs are what
OQL's nested reads join through, and every ref automatically maintains a
reverse index, so enforcing and traversing the relationship backward never
scans.

`on_delete` says what happens to referencing documents when the target goes:
`restrict` (refuse, the default posture), `cascade` (delete them too),
`set_null` (the ref type must include null), or `detach` for a ref held in an
array field (remove just that element).

A ref sources from a top-level field, including an array one. Indexes accept
nested and `items[].sku` paths, but a ref does not yet source from one, and says
so at registration rather than accepting it and skipping the check.

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

Because `status` and `total` above are required, they live at a fixed offset in
every document rather than behind a key lookup, and an index on `status` is
dense (every document has exactly one entry). The shape you declare is the
layout on disk, not a cache or a trick. Documents larger than a page spill to
overflow pages transparently, at any size.

Two parts of that layout are declared but not yet realised in full: predicate
evaluation currently decodes a document before testing it rather than reading
the required region in place, and a nested required object is stored as one
value in the variable region instead of flattening into its parent. So
`billing.address.city` is a real, indexable path today (`unique` and `index`
accept it, as they accept `items[].sku`), but reaching it is a walk, not a
single fixed-offset read.

