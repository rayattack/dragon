# Template Literals and Content Types

Most languages bolt templating on from the outside. Jinja, Go's `html/template`,
Handlebars - they all invent a *second* little language that lives inside `{{ }}`,
gets parsed at runtime, and crashes in production the first time someone fat-fingers
a variable name. The engine never knows your types, the braces collide with the
JSON and JavaScript you're emitting, and auto-escaping is something you remember to
turn on after the first security review.

Dragon takes the opposite stance: **templates are a compile-time feature of the
language itself.** A `template { ... }` block is raw text with two sigils punched
into it, and it compiles to the exact string concatenation you'd write by hand - no
engine, no runtime parsing, no reflection. A typo in `!{naem}` is a *compile error*,
not a blank space in someone's browser. One rule, and everything follows:

> **`!{}` breaks out of content into Dragon code. `:{}` breaks back into content
> from inside code.**

## The first template

A `template { ... }` block is everything between the braces, taken literally, except
where you interpolate with `!{expr}`:

```dragon
name: str = "World"
age: int = 42

greeting: str = template {Hello !{name}, you are !{age} years old}
print(greeting)   # Hello World, you are 42 years old
```

No quotes, no escaping quotes, no concatenation - and the content stands on its own,
newlines included. An untyped `template { ... }` produces a plain `str`, and the
`!{expr}` slots accept any expression: `template {Subtotal: !{price * quantity}}`.

### Bare braces are literal - this is not an f-string

Internalize this early: a **bare `{ }` is literal text.** Only `!{ }` interpolates.
That single decision is why Dragon templates emit JSON, CSS, and JavaScript without
any escaping ceremony:

```dragon
config: str = template {
{"name": "!{name}", "theme": {"color": "red"}}
}
# The JSON braces survive verbatim. Only !{name} is interpolated.
```

The lexer tracks brace depth to find the template's end; balanced content (HTML,
JSON, CSS, SQL) just works. For a stray brace in prose, escape it: `!!}` is a
literal `}`, `!!{` a literal `!{`.

## Typed templates: `template[HTML]`

The untyped form is fine for trusted, internal text. The moment a template touches
**user input destined for a browser**, reach for the typed form. `template[HTML]`
does two things the plain version doesn't: it **auto-escapes every `!{}`** through
`HTML.escape()`, and it **returns the type `HTML`, not `str`**.

```dragon
from html import HTML

user_input: str = "<script>alert('xss')</script>"

page: HTML = template[HTML] {
  <h1>!{user_input}</h1>
}
print(page.to_str())
# <h1>&lt;script&gt;alert(&#x27;xss&#x27;)&lt;/script&gt;</h1>
```

Injection is prevented *by default*, at the language level - you opt *out* with the
`raw` filter when you genuinely trust the content, which is exactly backwards from
most engines. `page.to_str()` (or `str(page)`) unwraps the `HTML` value back into a
plain `str` when you need one. The content types ship in the standard library -
`HTML`, `CSS`, `XML` ([from `html`](/docs/1202-html-css-xml)), `JSON` (from `json`),
and `SQL`, `URL` ([the data formats](/docs/1203-sql-and-url)) - each an ordinary
class extending `Template`.

### The type is real, and it catches mistakes

`template[HTML]` returns `HTML`; `template[SQL]` returns `SQL`. They're different
types, so the compiler stops you from sending a SQL query where a web page belongs:

```dragon
from html import HTML
from database import SQL

def send_response(body: HTML) -> None {
    print(body.to_str())
}

page:  HTML = template[HTML] { <h1>Welcome</h1> }
query: SQL  = template[SQL]  { SELECT name FROM users }

send_response(page)    # ok
send_response(query)   # compile error: SQL is not HTML
send_response("hi")    # compile error: str is not HTML
```

No other mainstream template system gives you this. Jinja, Go, and Askama hand you a
`String` at the end and trust you to keep your HTML and SQL straight; Dragon makes
mixing them a build failure.

## Composition: components are just functions

There's no `{% macro %}`, no partial-registration. A reusable component is a
**function that returns a content type**:

```dragon
from html import HTML

def button(label: str) -> HTML {
    return template[HTML] {
        <button>!{label}</button>
    }
}

ui: HTML = template[HTML] {
    <div>!{button("Save")}</div>
}
print(ui.to_str())
# The template's literal newlines and indentation are preserved verbatim:
#     <div>
#         <button>Save</button>
#     </div>
```

Whitespace inside a `template { ... }` block is **literal** - the newlines and
indentation you write are emitted as-is; Dragon does not collapse or trim them. If
you need compact output, write the markup compactly (or post-process the string).

The dispatch inside a typed template is **type-aware**: an `HTML` value spliced into
an `HTML` template is inserted **raw** (no double-escape), while a `str` is escaped.

| `!{expr}` where `expr` is... | What happens |
|------------------------------|--------------|
| the **same** content type (`HTML` in `HTML`) | inserted raw - no double-escape |
| a `str` | escaped via the content type's `escape()` |
| any other type | `__str__()` then escaped |
| piped through `\| raw` | inserted raw (explicit opt-out) |

That's why components compose without ever double-escaping - the type system tracks
which strings are already safe. **Layouts** fall out of the same idea: a base page is
just a function taking an `HTML` body - `def layout(title: str, body: HTML) -> HTML`
- where `!{body}` is inserted raw and `!{title}` is escaped.

## Logic in templates: `!{}` and `:{}` together

Inside a `!{}` block you can write **full Dragon statements** - `for`, `if`, anything.
To emit markup from inside that code, `:{}` drops back into content mode:

```dragon
from html import HTML

items: list[str] = ["a", "b"]

list_html: HTML = template[HTML] {
  <ul>!{ for item in items { :{ <li>!{item}</li> } } }</ul>
}
print(list_html.to_str())
# <ul> <li>a</li>  <li>b</li> </ul>
```

`!{}` enters code; `:{}` returns to content; you can nest as deep as you like, and
**the content type is inherited** - write `template[HTML]` once and every `:{}`
fragment auto-escapes as HTML. Conditionals work identically
(`!{ if c { :{ ... } } else { :{ ... } } }`). `:{}` is only legal inside a `!{}`
block. For a single value, a conditional *expression* is cleaner than a block:
`!{"Hi " + name if logged_in else "Sign in"}`. And two inline shortcuts render a
list without a full loop: `!{*xs}` (spread, empty separator) and `!{xs | join(", ")}`.

## Filters

A pipe applies a filter to the interpolated value:

| Filter | Effect |
|--------|--------|
| `html` / `sql` / `url` | escape for that context |
| `raw` | insert verbatim - explicit opt-out of auto-escaping |
| `join` / `join(sep)` | concatenate a list, optionally with a separator |
| *your function* | any in-scope **top-level** `(str) -> str` function |

A filter must be a top-level function, not a method - `!{title | upper}` does
**not** work because `upper` is a method on `str`. Wrap it in a function (or use
a conditional expression for one-off transforms):

```dragon
def shout(s: str) -> str { return s.upper() }

template[HTML] { <div>!{trusted_markup | raw}</div> }   # opt out of escaping
template[HTML] { <h1>!{title | shout}</h1> }            # any top-level (str)->str fn
```

That last row is the escape hatch: define `def shout(s: str) -> str { return s.upper() }`
and write `!{name | shout}`.

## File templates and custom content types

`template[HTML]("page.html")` is a compile-time `#include`: the compiler reads the
file *during compilation*, compiles its `!{}` slots against the surrounding scope,
and emits the same concatenation chain - zero runtime file I/O. The path must be a
string literal, resolved relative to the source file.

Any DSL with an escaping discipline can become a typed target. Subclass `Template`
and give it an `escape` method:

```dragon
from template import Template

class SHELL(Template) {
    def(inner: str) {
        self._inner = inner
    }
    @staticmethod
    def escape(s: str) -> str {
        return s.replace("'", "'\\''")
    }
}

username: str = "ada'; rm -rf /"
cmd: SHELL = template[SHELL] { ls -la '/home/!{username}/docs' }
# the quote in username is neutralized by SHELL.escape()
```

`template[GQL]`, `template[MARKDOWN]`, `template[REGEX]` - anything with an escaping
rule can be a typed template. Jinja, Go, and Askama can't express this; they're
hardcoded for HTML.

## Cost: there isn't any

Both `template { }` and `template("file.html")` compile to a chain of string
concatenations - literal segments in `.rodata` interleaved with your interpolated
values. Zero runtime parsing, no interpreter, no reflection, no library in the
binary. A 10 KB HTML template adds 10 KB of string data and nothing else.

## At a glance

| You want to... | Write |
|----------------|-------|
| Interpolate a value | `!{expr}` |
| A plain-string template | `s: str = template { ... }` |
| A safe, typed template | `page: HTML = template[HTML] { ... }` |
| Unwrap to `str` | `page.to_str()` (or `str(page)`) |
| Loop / branch | `!{ for x in xs { :{ <li>!{x}</li> } } }` |
| A component / layout | a function returning / taking a content type |
| Opt out of escaping | `!{x \| raw}` |
| Pull markup from a file | `template[HTML]("page.html")` |
| A custom escaping format | subclass `Template`, use `template[MY_TYPE]` |

Next, the markup content types in detail -
[HTML, CSS, and XML](/docs/1202-html-css-xml).
