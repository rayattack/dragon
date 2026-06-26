# SQL and URL Templates

The [markup content types](/docs/1202-html-css-xml) escape text so it can't break
out of a document. The last two content types do something more interesting: `SQL`
turns interpolations into **bound parameters** rather than text, and `URL`
percent-encodes for the query string. Both close an injection class by construction.

## `SQL` - interpolations become bound parameters

This is the one that surprises people. In a `template[SQL]`, a `!{}` interpolation
does **not** paste the value into the query text. It adds a **bound parameter** - a
placeholder in the SQL, with the value carried alongside, separately:

```dragon
from database import SQL

id: int = 5
q: SQL = template[SQL] {
  select name from users where id = !{id}
}
print(q.to_str())
# select name from users where id = $$0
```

Notice the output: `!{id}` became `$$0`, a parameter placeholder - *not* the literal
`5`. The value `5` is held by the `SQL` object, bound to that slot. When you execute
the query through the [database module](/docs/1301-databases), the driver sends the
SQL text and the parameter values as two separate things, so a value can **never** be
interpreted as SQL. Each `!{}` becomes the next placeholder, in order:

```dragon
from database import SQL

name: str = "Ada"
active: bool = true
q: SQL = template[SQL] {
  select * from users where name = !{name} and active = !{active}
}
# → ... where name = $$0 and active = $$1, with name and active bound
```

This is structurally stronger than escaping. A classic injection payload - a `name`
of `'; DROP TABLE users; --` - can't do anything, because it's never part of the
query *text*; it's a bound value compared against the `name` column. You get
parameterized queries for free, with the readability of an inline literal:

```dragon
# This looks like string interpolation, but it is a safe, parameterized query:
q: SQL = template[SQL] { select * from orders where customer = !{user_id} }
```

You build `SQL` values with `template[SQL]`; you *execute* them - `db.all(q)`,
`db.one(q)`, `db.run(q)` - in the [Databases](/docs/1301-databases) chapter, which
covers connections, result handling, and transactions.

Two limits to know inside `template[SQL]`: each `!{}` must be a **single bound
expression** - the block forms from the previous chapter (`!{ for ... }`,
`!{ if ... }`) are not supported here - and you cannot splice one `SQL` value into
another (nesting a `SQL` interpolation is a compile error). Both are rejected at
build time, not silently miscompiled.

## `URL` - percent-encoding for query strings

`URL` escapes interpolated values for a URL query string: spaces, `&`, `=`, `?`,
`#`, and `+` are percent-encoded so a value can't add a second parameter or
otherwise alter the query's structure:

```dragon
from urllib.parse import URL

query: str = "a b&c"
link: URL = template[URL] {
  https://example.com/search?q=!{query}
}
print(link.to_str())
# https://example.com/search?q=a%20b%26c
```

The space becomes `%20` and the `&` becomes `%26`, so the interpolated `query` stays
a single query-string value instead of injecting a second `&c` parameter. The literal
structure of the URL - the scheme, host, path, and the `?q=` you wrote - passes
through untouched.

`URL`'s built-in escape targets the characters that matter most in a query string;
it does **not** percent-encode every RFC 3986 reserved character (`/`, `:`, `;`,
`,`, and `@` pass through). When you need to encode a value fully - everything
outside the unreserved set - use the `| url` filter (`!{value | url}`) or
`urllib.parse.quote(value, "")`, both of which encode `/`, `:`, `;`, `,`, and the
rest:

```dragon
from urllib.parse import quote

print(quote("a/b;c", ""))   # a%2Fb%3Bc
```

## At a glance

| Content type | Import | What `!{x}` does | For |
|--------------|--------|------------------|-----|
| `SQL` | `from database import SQL` | becomes a **bound parameter** (`$$0`, `$$1`, …) | safe, parameterized queries |
| `URL` | `from urllib.parse import URL` | **percent-encoded** | safe query-string values |

Together with [HTML/CSS/XML](/docs/1202-html-css-xml) and the generic
[template system](/docs/1201-templates), these give you typed, injection-safe text
generation for every layer of a web service - page, query, and link. That closes
Part 12. Executing the `SQL` values you just built is the subject of the next part:
[Databases](/docs/1301-databases).
