# HTML, CSS, and XML

The [template system](/docs/1201-templates) is generic over content types. This
chapter covers the three markup types that ship in the `html` module - the ones you
reach for when generating a web page, a stylesheet, or an XML document. Each is a
`Template` subclass with an `escape` method tuned to its context, so `!{}`
interpolations are made safe *for that syntax* automatically.

```dragon
from html import HTML, CSS, XML
```

## `HTML` - the web-facing type

`HTML` is the one you'll use most. Its `escape` neutralizes the characters that turn
user input into markup - `<`, `>`, `&`, quotes - so an interpolated `str` can never
break out of its element or inject a tag:

```dragon
from html import HTML

user_input: str = "<script>alert('xss')</script>"
page: HTML = template[HTML] {
  <h1>!{user_input}</h1>
}
print(page.to_str())
# <h1>&lt;script&gt;alert(&#x27;xss&#x27;)&lt;/script&gt;</h1>
```

The dispatch is type-aware, as the previous chapter showed: a `str` is escaped, but
an `HTML` value spliced into an `HTML` template is inserted **raw** - so components
compose without double-escaping. That's the entire reason to keep your functions
returning `HTML` rather than `str`:

```dragon
from html import HTML

def badge(text: str) -> HTML {
    return template[HTML] { <span class="badge">!{text}</span> }
}

card: HTML = template[HTML] {
  <div class="card">
    !{badge("new")}          <!-- HTML value → inserted raw -->
    <p>!{user_input}</p>     <!-- str → escaped -->
  </div>
}
```

When you genuinely have trusted markup to insert verbatim - output from a sanitizer,
a pre-rendered fragment - opt out explicitly with the `raw` filter:
`!{trusted | raw}`. The unsafe path is the one you have to spell out.

## `CSS` - stylesheets

`CSS` generates stylesheet text. The bare `{ }` braces of a CSS rule pass through
literally (only `!{}` interpolates), so a rule body reads exactly like CSS:

```dragon
from html import CSS

accent: str = "red"
sheet: CSS = template[CSS] {
  body { color: !{accent}; }
}
print(sheet.to_str())
# body { color: red; }
```

This is where Dragon's "bare braces are literal" rule pays off - a CSS template
doesn't fight the `{ }` that CSS itself uses for rule blocks.

One caveat: `CSS` escaping covers quotes, backslashes, and angle brackets, but it
does **not** neutralize the `{` and `}` that delimit rule blocks. Interpolate
values into property *positions* (`color: !{accent};`), not into places where a
stray `}` in the value could close the rule early. Treat `!{}` in CSS as "a value
goes here," not "arbitrary CSS goes here."

## `XML` - documents

`XML` escapes the markup-significant characters - `<`, `>`, `&`, and both quote
styles (`"` becomes `&quot;`, `'` becomes `&apos;`) - for XML content, the same
shape as HTML but for document data rather than a browser page:

```dragon
from html import XML

val: str = "a<b>c"
doc: XML = template[XML] {
  <item>!{val}</item>
}
print(doc.to_str())
# <item>a&lt;b&gt;c</item>
```

## A related type: `JSON`

JSON output has the same injection concern - an interpolated string must not break
the surrounding quotes. The `JSON` content type (from the `json` module) escapes for
exactly that:

```dragon
from json import JSON

name: str = "Ada \"the great\""
doc: JSON = template[JSON] {
  {"name": "!{name}"}
}
print(doc.to_str())
# {"name": "Ada \"the great\""}
```

The literal JSON braces survive untouched; only `!{name}` is interpolated, and its
inner quotes are escaped so the document stays valid.

## At a glance

| Content type | Import | Escapes | For |
|--------------|--------|---------|-----|
| `HTML` | `from html import HTML` | `< > & ' "` | web pages (browser output) |
| `CSS`  | `from html import CSS`  | CSS-significant chars | stylesheets |
| `XML`  | `from html import XML`  | `< > & ' "` | XML documents |
| `JSON` | `from json import JSON` | `\ "` and `\n` `\r` `\t` | JSON payloads |

These markup types pair naturally with the [HTTP server](/docs/1701-web-application),
where a route returns an `HTML` page. The last two content types are the ones for
talking to a database and building links - [SQL and URL](/docs/1203-sql-and-url).
