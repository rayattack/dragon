# Views Are HTML Templates

Hand-concatenating HTML strings gets old fast, and a `str` carries no
guarantee that what is inside it is safe markup. Dragon's
[template literals](/docs/1201-templates) give you first-class markup with
interpolation and automatic escaping, and the [`HTML`](/docs/1202-html-css-xml)
content type makes the *type* say so:

```dragon
import ui
from html import HTML
from ui.desktop import Window

title: str = "Dashboard"
items: list[str] = ["Inbox", "Today", "Done"]

view: HTML = template[HTML] {
  <main style="font-family:system-ui;padding:1rem">
    <h1>!{title}</h1>
    <ul>
      !{ for item in items { :{ <li>!{item}</li> } } }
    </ul>
  </main>
}

win: Window = Window(title, 420, 320)
win.body = view
win.show()
ui.App.run()
```

A `template[HTML] { ... }` evaluates to an `HTML` value, and
`Window.body` takes exactly that - so the view you build flows into the
window with no `str` conversion in between. That is the point of the
`HTML` type: it tracks, at compile time, that a value is markup intended
for an HTML context.

## Escaping is by type, not by discipline

Interpolations are **HTML-escaped by default** - a `title` of
`"<script>"` renders as text, not a tag - so the markup you build from
user data is safe without you remembering to escape it (the same guarantee
the [web chapter](/docs/1701-web-application) leans on):

```dragon
user_input: str = "<script>alert('xss')</script>"
view: HTML = template[HTML] {
  <h1>!{user_input}</h1>
}
# renders: <h1>&lt;script&gt;alert(&#x27;xss&#x27;)&lt;/script&gt;</h1>
```

Because escaping keys off the *type* of the interpolated value, composition
just works: a `str` spliced into an `HTML` template is escaped, but an
`HTML` value is inserted **raw** - so components nest without
double-escaping. This is exactly why a view-building function should return
`HTML`, not `str`:

```dragon
def badge(text: str) -> HTML {
    return template[HTML] { <span class="badge">!{text}</span> }
}

view: HTML = template[HTML] {
  <div class="card">
    !{badge("new")}          <!-- HTML value -> inserted raw -->
    <p>!{user_input}</p>     <!-- str -> escaped -->
  </div>
}
```

When you genuinely have trusted markup to insert verbatim, opt out
explicitly with the `raw` filter: `!{trusted | raw}`. The unsafe path is
the one you have to spell out. Block interpolation
(`!{ for ... { :{ ... } } }`) loops markup in place. The full content-type
story - `CSS`, `XML`, custom types - is in
[HTML, CSS, and XML](/docs/1202-html-css-xml).

Assigning an `HTML` view to `win.body` paints it once. To make the screen
*follow* your data as it changes, you need state the UI can watch - that is
the next page, [Signals and Reactivity](/docs/1804-reactivity).
