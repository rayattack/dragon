# Decision 048: Generic JSON Decode (schema-directed default, `Any` escape hatch)

Approved. The **`loads → Any` (boxed) tier is implemented** - byte-scanning recursive-descent decoder in `json.dr` (`loads`/`loads_bytes`/`loads_obj`) + `Request.json` returning the boxed tree (old raw-body `json` dropped - use `req.body`), with `\uXXXX`/surrogate-pair unescape (drove a `chr` UTF-8 fix) and offset-bearing errors. The **zero-alloc schema-directed tier (`decode[T]`) remains pending generics** - until then the typed path is `T(**json.loads_obj(s))` (boxed intermediate).

`json.dr` had monomorphic typed decoders (`loads_str`, `loads_int`, `loads_list_int`, …) and `detect_type` (recognizes `JSON_OBJECT` but can't decode into it). No nested decode. `Request.json` used to hand back the raw body. `dumps(obj: Any)` had no symmetric decode. Two decode forms, one byte scanner:

| Form | Target | Cost | When |
|---|---|---|---|
| **Schema-directed** (default) | a `TypedDict` / class / typed container - the static type *is* the schema | zero boxing; each field lands in its native slot; the scanner skips unrequested keys | request bodies, config, any decode where the shape is known |
| **`loads → Any`** (escape hatch) | a boxed `Any` tree (`dict[str, Any]` / `list[Any]` / scalar box) | one box per node + a tag dispatch per field access | genuinely dynamic / unknown-shape JSON, kept off hot paths |

Both scan teh **raw UTF-8 bytes** with `dragon_bytes_get` (an `i64`-returning, **zero-allocation** byte
read - verified: `bytes[i]` lowers to a direct `dragon_bytes_get` call) and integer comparisons. The
difference is only the **materialization target**: typed slots vs. boxed nodes. This is what lets the
schema-directed path reach Rust-`serde`-into-struct class throughput while `Any` stays at Go
`interface{}` class.

## Context / Motivation

Web request-body parsng (`Request.json`, `server.dr`) is the headline consumer, but the tension is
general and hits #1 head-on:

- A naive `loads → Any` written in the obvious Pythonic way - walk the string with `s[i:i+1]`
 slices - **allocates a fresh heap string per character scanned** and **boxes every scalar**. That is
 strictly slower than Go/Rust: O(n) allocations just to tokenize, before any value materializes. It
 must not be the foundation.
- The fast foundation is to scan over **bytes**, not code-point string slices: `dragon_bytes_get(b, i)`
 returns the byte as an `i64` with no allocation and no bounds-box, so the tokenizer is integer
 compares over a contiguous buffer - the same shape a C/Rust parser has.
- Given a byte scanner, the boxing is the *only* remaining cost, and it is **avoidable when the shape is
 known**: decode straight into the destination's native slots. When the shape is unknown, boxing is
 unavoidable and acceptable - but it should be opt-in, not the default that every typed consumer pays.

The registry's hot endpoints (package metadata JSON) are low-volume, so JSON is not their bottleneck -
but the principle (don't bake an allocation-per-byte parser into the stdlib) holds regardless, and the
HTTP server's request path is exactly where a fast decoder matters.

## Options considered

1. **`loads → Any` only (Python parity).** One recursive decoder returning a boxed tree; every field
 access pays a runtime tag dispatch, every node is a heap box. Rejected as the *default*: it taxes the
 common case (known shape) with boxing the type system could have elided, and invites the
 per-character-slice anti-pattern. Kept as an **opt-in** form - it is the honest answer when the shape
 truly is dynamic.

2. **Schema-directed decode only (into `TypedDict`/class).** Fastest and most Dragon-idiomatic (the
 static type is the schema; monomorphizes a decoder per target type; no boxing). Rejected as the
 *sole* option: some inputs are legitimately unknown-shape (third-party webhooks, `dumps`-round-trip of
 heterogeneous data), and forcing a schema there is a worse ergonomic than a boxed tree.

3. **Hybrid: schema-directed default + `Any` escape hatch, one shared byte scanner (CHOSEN).** The
 scanner/tokenizer is written once over `dragon_bytes_get`; two materializers consume its events. Pays
 for boxing only when the caller asked for `Any`. Matches the encode side's spirit (`dumps` already
 takes `Any`, so `loads → Any` completes the symmetry) without making the typed path subsidize it.

A runtime-polymorphic single decoder (à la option 1 but reused for typed via reflection) is rejected for
the same reason rejects runtime-arity dispatch: it would impose the boxed representation on the
typed path. Monomorphized per-type decoders keep the typed path branch-predictable and box-free.

## Decision

### One scanner, two materializers

A single internal tokenizer scans the raw body **bytes** left-to-right with `dragon_bytes_get` + integer
classification (no per-character string allocation, no regex). It exposes the minimal event surface a
JSON grammar needs: object-start/key/value-start, array-start/element, scalar (with the byte span of the
literal), and end markers. Two materializers sit on top:

- **Schema-directed** (`json.decode[T](body)` / `T`-typed entry, exact spelling settled in
 implementation): drives the scanner with the *target type* in hand. Object keys are matched against the
 `TypedDict`/class field set; a matched scalar is parsed directly into that field's native slot
 (`int`→`i64`, `float`→`f64`, `str`→heap str, nested `T'`→recursive schema-directed decode); an
 **unrequested key's value is skipped without materializing** (the scanner walks past it). No box is
 ever allocated. This is the read-side mirror of 's `T(**row)`.

- **`loads → Any`** (`json.loads(body) -> Any`): materializes every node into a box - objects into
 `dict[str, Any]`, arrays into `list[Any]`, scalars into a tagged box (`TAG_INT` / `TAG_FLOAT` /
 `TAG_STR` / `TAG_BOOL` / `TAG_NONE`). Field access then pays the usual `Any` narrowing (`isinstance`
 extracts the payload at its native type, per Phase 4).

### `Request.json` consumer

**Implemented:** `req.json` parses the verbatim body bytes into a boxed `Any` tree (the old
raw-body-returning `json` was redundant with `req.body` and was dropped - the raw body is still
`req.body`). For a known shape today, construct with `T(**json.loads_obj(req.body))` .

**Pending :** a schema-directed `req.json_into[T]` (or the settled spelling) - the recommended
path once generics land; the body's static type *is* the validation schema, and a missing-required /
type-mismatched field becomes a clean decode error rather than a silent junk value. The docs will steer
toward schema-directed; the boxed `req.json` is the dynamic-body escape hatch.

### Errors

A malformed document, a type mismatch against the schema, or (schema-directed) a missing required field
raises a `ValueError`/`JSONDecodeError` with the byte offset - never a partial or sentinel result, so a
bad body can't be mistaken for a valid-but-empty one (same contract as `verify_session` in `sessions.dr`).

## Speed vs parity, spelled out

- **Speed (#1):** schema-directed decode is allocation-free except for the destination's own
 heap members (strings, nested objects) - serde-into-struct class. `Any` is one box per node - Go
 `interface{}` class - and is never on a typed consumer's critical path unless explicitly chosen. The
 shared byte scanner means neither form ever allocates per character.
- **Parity (#3):** `json.loads(s) -> Any` gives the Python-shaped convenience for dynamic
 data; `dumps`/`loads` become symmetric. Schema-directed decode has no direct CPython analogue (Python
 has no static schema) but matches the `pydantic`/`serde`/`encoding/json`-into-struct idiom developers
 reach for.
- **Composes with :** `T(**json.loads(s))` works once both land, but schema-directed decode is
 strictly better where the shape is known (no intermediate boxed dict).
- **Existing monomorphic decoders** (`loads_int`, `loads_list_str`, …) remain the zero-overhead scalar/
 homogeneous-list fast paths and are unaffected; `detect_type` becomes the scanner's lookahead helper
 rather than a dead-end recognizer.

## Implementation notes (follow-up, not part of this ADR)

1. Byte-level scanner over `dragon_bytes_get` (string-literal unescaping reuses `json.dr`'s existing
 `unescape`, but fed byte spans, not per-char slices).
2. `Any` materializer (`loads`) - needs `dict[str, Any]` / `list[Any]` box construction; this is the
 first heavy in-language consumer of nested `Any` containers, so expect to shake out box-in-
 container edges.
3. Schema-directed materializer - monomorphized per target type ; skip-value fast path for
 unrequested keys; recursion for nested `TypedDict`/class fields.
4. `Request.json_into[T]` / `json_any` wiring in `server.dr` (the existing `Request.json` raw-body
 accessor stays for back-compat).
5. `test/dr/` coverage: round-trip `dumps`→`loads`, schema-directed decode of nested objects, skip of
 unrequested keys, malformed-body errors with offsets, and a decode benchmark vs. the boxed path to
 keep the no-per-char-alloc invariant honest.
