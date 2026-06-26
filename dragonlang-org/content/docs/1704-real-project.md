# A Real Project: Building a Docs Site in Dragon

The site you are reading right now is written in Dragon. Not "scaffolded by a
generator and then served by Dragon" - the routing, the markdown renderer, the
syntax highlighter, the page layout, and the sidebar you see on the left are
all `.dr` source compiled into a single binary. There is no Node, no Hugo, no
mdBook, no Jinja, no Prism. When you collapse a chapter or read a colored code
block, you are looking at the output of a Dragon program.

The live site folds that machinery into a larger app (it also serves the
package registry). To tour it without the extra moving parts, the repository
ships a distilled, self-contained copy under `examples/docs_site/` - the same
renderer and highlighter wired to a tiny sample book, which you can build and
run in one command. This chapter walks that example, and every snippet below
is lifted from its source.

It is deliberately a *real* project rather than a toy - it surfaces routing,
file I/O, string processing, the template engine, classes, and the HTTP server
all at once, and shows how they compose.

## The shape of it

```bash
examples/docs_site/
├── main.dr        # routing, sidebar, page layout, app wiring
├── markdown.dr    # a small Markdown → HTML renderer
├── highlight.dr   # the syntax highlighter (tokenizer)
├── run.sh         # build + run helper
├── content/docs/  # the book: SUMMARY.md + NNN-*.md chapters
└── static/        # app.css, app.js
```

Three Dragon files, a directory of Markdown, and a stylesheet. Run from that
directory, `dragon build main.dr` turns all of it into one executable that
serves the whole site.

## Routing on the HTTP server

The standard library ships an HTTP server (`stdlib/http/server.dr`) with a
familiar router. `main.dr` opens with its imports and a `Router`:

```dragon
from http.server import Router, Request, Response, Context
from io import open
from os.path import join as path_join, exists, isfile
from markdown import render
from html import HTML

const port: int = 2018
const host: str = "127.0.0.1"

const app: Router = Router(port, host)
```

(2018 is the year Dragon was born; the port is a small nod to it.) Routes are
registered with HTTP-verb methods. A route handler takes a `Request`, a
`Response`, and a `Context`; here the handlers are lambdas, but a named
function works identically:

```dragon
app.GET('/', lambda (req: Request, res: Response, ctx: Context) -> None {
    res.redirect("/docs/0001-foreword")
})

app.GET('/docs/:slug', lambda (req: Request, res: Response, ctx: Context) -> None {
    const slug: str = req.params.slug
    _serve_chapter("docs", DOCS_DIR, slug, res)
})

app.listen()
```

Two things to notice. `:slug` is a path parameter, read back as
`req.params.slug` - dot-access on the params, no string-key bracket dance.
And `app.listen()` is the last line of the *file*; remember from
[Modules and Packages](/docs/1001-modules) that Dragon has no magic `main()` -
the file's top-level statements *are* the program, so registering routes and
calling `listen()` at module level is the whole entry point.

Static assets get a wildcard route:

```dragon
app.ASSETS(STATIC_DIR, "/assets/*")
```

`ASSETS` serves files out of `STATIC_DIR` for any path under `/assets/`, which
is how `app.css` and `app.js` reach the browser.

## Reading a chapter from disk

Serving a chapter is: find the file, render it, wrap it in the layout, send it.

```dragon
def _serve_chapter(kind: str, dir: str, slug: str, res: Response) -> None {
    const path: str = path_join(dir, slug + ".md")
    if not isfile(path) {
        res.status = 404
        res.text("Not Found")
        return
    }
    const md: str = open(path).text()
    const html: str = render(md)
    const title: str = _extract_title(md, slug)
    const page: HTML = _render_page(kind, slug, title, html)
    res.html(page)
}
```

`open(path).text()` (from [the io module](/docs/1402-stdlib-io)) slurps the file
as a `str`. `render` (our own `markdown.dr`) turns it into HTML. `_render_page`
returns an `HTML` value - a string subtype the compiler tracks as "already-safe
markup" - and `res.html(page)` sends it with the right `Content-Type`. The 404
path sets `res.status` and sends plain text. This is the entire request
lifecycle for a documentation page.

## Classes model the table of contents

The sidebar is built from `SUMMARY.md`. Each entry becomes a `Chapter` - an
ordinary class, with Dragon's nameless `def()` constructor (see
[Classes and Objects](/docs/0601-classes)):

```dragon
class Chapter {
    def(slug: str, title: str, kind: str, level: int, number: str) {
        self.slug = slug
        self.title = title
        self.kind = kind
        self.level = level
        self.number = number
    }
}
```

`_load_summary` parses `SUMMARY.md` once at startup into a `list[Chapter]`,
assigning chapter numbers (`1.`, `1.1.`) and detecting the appendix block. It
runs at module load, bound to a `const`:

```dragon
const DOCS_CHAPTERS: list[Chapter] = _load_summary(DOCS_DIR)
```

Because it's top-level code in the entry file, it executes exactly once when
the binary starts - the table of contents is parsed at boot, not per request.

## The Markdown renderer

`markdown.dr` is a single-pass block renderer: it walks the source line by
line, opening and closing `<ul>`/`<ol>`/`<blockquote>` wrappers as runs of
like lines begin and end, and treating fenced ` ``` ` blocks specially. The
core is a `while` loop over the lines with a little state:

```dragon
def render(md: str) -> str {
    const lines: list[str] = md.split("\n")
    out: str = ""
    in_ul: bool = false
    in_ol: bool = false
    in_quote: bool = false
    in_code: bool = false
    code_lang: str = ""
    code_lines: list[str] = []
    para: list[str] = []
    # ... close_para() / close_lists() helpers, then the line loop ...
}
```

It is small enough to read in one sitting and covers exactly what the book
uses: ATX headings, fenced code, paragraphs, lists, blockquotes, GFM tables,
and inline `code`/`**bold**`/`*italic*`/links. Anything it doesn't need, it
doesn't implement - a renderer that fits the content rather than a general
engine that fits everything.

When the loop hits a fenced block it buffers the body and, at the closing
fence, hands the whole block to the highlighter:

```dragon
def _render_code_block(lang: str, lines: list[str]) -> str {
    body: str = ""
    i: int = 0
    while i < len(lines) {
        if i > 0 {
            body = body + "\n"
        }
        body = body + lines[i]
        i = i + 1
    }
    open_tag: str = "<pre><code>"
    if len(lang) > 0 {
        open_tag = "<pre><code class=\"language-" + _html_escape(lang) + "\">"
    }
    return open_tag + highlight(lang, body) + "</code></pre>\n"
}
```

## Syntax highlighting, in Dragon

`highlight.dr` is a hand-written lexer. It walks the code once, classifies each
token, and wraps it in a `<span class="tok-...">` that `app.css` colors. The
keyword table is a `const` set literal (literal sets hash string members
correctly for a `w in S` membership test):

```dragon
# The full keyword set lives in highlight.dr; a representative slice:
const _KW: set[str] = {"if", "elif", "else", "while", "for", "def", "class", "const", "return", "import", "from", "lambda", "template", "match", "case", "fire", "thread", "await"}
```

The classifier checks a word against the tables in order - keyword, constant,
`self`/`cls`, type, builtin - and otherwise asks whether the next non-space
character is `(` to decide if it's a function call:

```dragon
def _classify_dragon(word: str, code: str, j: int, n: int) -> str {
    if word in _KW {
        return "tok-kw"
    }
    if word in _CONST {
        return "tok-const"
    }
    if word == "self" or word == "cls" {
        return "tok-self"
    }
    if _is_type(word) {
        return "tok-type"
    }
    if _is_builtin(word) {
        return "tok-builtin"
    }
    k: int = j
    while k < n and (code[k] == " " or code[k] == "\t") {
        k = k + 1
    }
    if k < n and code[k] == "(" {
        return "tok-fn"
    }
    return ""
}
```

No client-side JavaScript runs to highlight code - the spans are in the HTML
the server sends, so a reader with scripts disabled still gets colored code,
and there is no third-party highlighter library shipped. The colors themselves
are CSS custom properties that flip with the light/dark theme.

## The page layout is a typed-HTML template

The full HTML page is a `template[HTML] { ... }` block (see
[Templates](/docs/1201-templates)). The literal markup passes through verbatim;
the `!{}` slots interpolate the rendered pieces, escaping any plain `str`
automatically and composing `HTML` values without re-escaping:

```dragon
def _render_page(kind: str, slug: str, title: str, content_html: str) -> HTML {
    const sidebar_html: HTML = _render_sidebar(kind, slug)
    const prevnext_html: HTML = _render_prev_next(kind, slug)
    return template[HTML] {
        <!DOCTYPE html>
        <html lang="en" data-theme="light">
        <head>
            <meta charset="utf-8">
            <meta name="viewport" content="width=device-width, initial-scale=1">
            <title>!{title} - Dragon</title>
            <link rel="stylesheet" href="/assets/app.css">
        </head>
        <body>
            <div class="layout">
                <aside class="sidebar">
                    !{sidebar_html}
                </aside>
                <main class="content">
                    <article>
                        !{content_html | raw}
                    </article>
                    !{prevnext_html}
                </main>
            </div>
            <script src="/assets/app.js"></script>
        </body>
        </html>
    }
}
```

Two slots earn a closer look. `!{title}` is a plain `str`, so the template
escapes it - a chapter title containing a `<` can't break the markup.
`!{content_html | raw}` is the already-rendered markdown; the `| raw` says
"this is trusted HTML, splice it verbatim," the one place we opt out of
escaping on purpose. The whole thing compiles to a chain of string
concatenations - the literal segments live in `.rodata`, the `!{}` values are
spliced in at runtime. There is no template interpreter in the binary and
nothing to parse at request time; the cost is the same as if the concatenation
were written by hand, but the `HTML` type makes the escaping decisions for you
instead of leaving them to a hand-built string.

## The foldable sidebar

`_render_sidebar` turns the flat `list[Chapter]` into the navigation tree.
Front-matter entries are plain links; each numbered chapter becomes a native
`<details>` element so it folds with no JavaScript at all. The whole fragment
is built with `template[HTML]`, not string concatenation - each piece returns
an `HTML` value and composes into the next without manual escaping:

```dragon
return template[HTML] {
  <details class="sb-chapter" open data-ch="!{idx}"!{active_attr | raw}>
    <summary class="sb-chapter-head"><span class="sb-caret" aria-hidden="true"></span>!{num}<span class="sb-title">!{c.title}</span></summary>
    <ol class="sb-sections">!{ for s in sections { :{ !{_render_section_li(s, active_kind, active_slug)} } } }</ol>
  </details>
}
```

`!{c.title}` is escaped as a plain string; `!{num}` and each
`_render_section_li(...)` are `HTML` values, so they compose raw. The lone
`| raw` is `active_attr`, a trusted attribute fragment (it carries the `="1"`
quotes that must not be escaped). The `for ... { :{ ... } }` is a template loop
- it splices one row per section, in order. The server renders every chapter
expanded and tags the one containing the current page so it stays open.

A few lines of `app.js` persist which chapters the reader has collapsed, and
the CSS draws the disclosure caret and rotates it on `[open]`. Everything else
about the sidebar - the structure, the active highlight, the numbering - is
decided on the server, in Dragon.

## Building and running

`run.sh` wraps the two commands you actually use:

```bash
# from examples/docs_site/, build the binary then run it
dragon build main.dr -o /tmp/docs_site
/tmp/docs_site
# → Dragon docs site: http://127.0.0.1:2018/   (6 chapters loaded)
```

That's the deploy story: **one statically-linked binary**. The HTTP server, the
markdown renderer, the highlighter, and your content's file paths are all
inside it. There is no `node_modules`, no runtime dependency to install on the
server, no build pipeline to keep alive. Copy the binary and the `content/` +
`static/` directories to a host and run it.

## Why this matters

A documentation site is a modest program, but building it end-to-end in one
language with no external toolchain is exactly the bar Dragon sets for itself.
The same `template[HTML]` you'd reach for in a web app renders these pages; the
same `Router` you'd use for an API serves them; the same string and file
primitives you learned in the early chapters parse the Markdown. Nothing here
is special-cased for documentation - it is just Dragon, used for a real thing,
and the live dragonlang.org runs the same renderer and highlighter folded into
a larger app.

When you hit a wall building your own project - a missing string method, a
rough edge in the server - that is the same signal this site's authors follow:
the fix belongs in the language or the standard library, so the next person
doesn't hit the same wall. The site is the dogfood, and so is yours.

## At a glance

| Piece | Where | Dragon feature it leans on |
|-------|-------|----------------------------|
| Routing | `main.dr` | `Router`, lambdas, path params |
| Reading content | `_serve_chapter` | `io.open`, `os.path` |
| Table of contents | `Chapter` class | classes, `const` at module load |
| Markdown → HTML | `markdown.dr` | strings, lists, single-pass parsing |
| Syntax highlighting | `highlight.dr` | set literals, char scanning, spans |
| Page layout | `_render_page` | `template[HTML] { ... }` interpolation |
| Foldable sidebar | `_render_sidebar` | typed-HTML composition + native `<details>` |
| Ship it | `dragon build` | one static binary, zero install |
