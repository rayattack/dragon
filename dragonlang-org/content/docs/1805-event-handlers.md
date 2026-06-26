# Event Handlers

[Reactive text](/docs/1804-reactivity) wired the *output* - the screen
follows the signal. Event handlers wire the *input*: to respond to a click
you point an HTML event attribute at a Dragon function.

```dragon
import ui
from html import HTML
from ui import Signal
from ui.desktop import Window

count: Signal[int] = Signal(0)

def bump() -> None {
    count.set(count() + 1)
}

view: HTML = template[HTML] {
  <button onclick="!{bump}">clicked !{count()} times</button>
}

win: Window = Window("Clicker", 320, 160)
win.body = view
win.show()
ui.App.run()
```

When the compiler sees `onclick="!{bump}"` it does not stringify `bump`
into the page. It registers the function, gives it an id, and emits
`onclick="window.dr.invoke(0)"` into the markup. At runtime the click
posts that id back across the bridge and Dragon calls `bump()`. The
handler runs in-process, in Dragon, with full access to your state - no
serialization, no `eval`, no string-named callback to keep in sync. A
handler can be a named `def` or an inline `lambda () -> None { ... }`.

Note what the type system is doing here: the interpolation in
`onclick="!{bump}"` is a Dragon *callable*, not a string, so the compiler
knows to register it rather than escape-and-insert it. A plain
`onclick="!{some_js_string}"` still interpolates as text - the handler
path triggers only on an unambiguous function or lambda.

## The complete reactive counter

Put input and output together and you have the whole loop: a button
mutates the signal, and the interpolation that reads it re-paints itself.

```dragon
import ui
from html import HTML
from ui import Signal
from ui.desktop import Window

count: Signal[int] = Signal(0)

def bump() -> None {
    count.set(count() + 1)
}

view: HTML = template[HTML] {
  <main style="font-family:system-ui;text-align:center;margin-top:3rem">
    <h1>Count: !{count()}</h1>
    <button onclick="!{bump}" style="font-size:1.2rem;padding:.5rem 1rem">+1</button>
  </main>
}

win: Window = Window("Counter", 360, 240)
win.body = view
win.show()
ui.App.run()
```

This is the complete, working reactive counter, and there is no glue you
maintain between the two halves. `onclick="!{bump}"` is the input edge;
`!{count()}` is the output edge; `count` is the single source of truth in
the middle. Click the button → `bump()` runs in Dragon → `count.set(...)`
walks its subscriber list → the one `<h1>` patches. No virtual DOM, no
re-render of the whole view, no IPC you wrote.

Next: dress it up with real stylesheets and assets, then ship it as one
binary - [Styling, Assets, and Shipping](/docs/1806-assets-and-shipping).
