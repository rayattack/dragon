# Styling, Assets, and Shipping

An inline `style="..."` attribute is fine for one element, but a real app wants a
stylesheet - and then it wants to ship as a native binary. This page covers both:
styling a window, a complete app, and the command that builds it.

## Styling a window

A window's body is an [`HTML`](/docs/1202-html-css-xml) value, so you style it the
same way you style any HTML document: with a `<style>` block at the top of the
view. Bare CSS braces pass through literally (only `!{}` interpolates), so the
rules read exactly like CSS:

```dragon
import ui
from html import HTML
from ui.desktop import Window

view: HTML = template[HTML] {
  <style>
    body { font-family: system-ui; max-width: 22rem; margin: 2rem auto; }
    .total { font-size: 2rem; text-align: center; }
  </style>
  <h1>My App</h1>
  <div class="total">Ready</div>
}

win: Window = Window("My App", 800, 600)
win.body = view
win.show()
ui.App.run()
```

Because the stylesheet lives in the typed `HTML` view, you can factor it into a
function that returns `HTML` and reuse it across windows - the same composition
that works for any other markup. If you keep a larger stylesheet in a file, read
it at startup with [the io module](/docs/1402-stdlib-io) and insert it into the
`<style>` block with the `raw` filter (`!{css | raw}`) - it is trusted content you
authored, so opting out of escaping is correct there.

## A complete app

Here is a small but real app - a tip calculator - that ties together
signals, handlers, reactive text, and an inline stylesheet. The bill and
tip percentage are signals; the per-person total is a derived reactive
expression.

`tip.dr`:

```dragon
import ui
from html import HTML
from ui import Signal
from ui.desktop import Window

bill: Signal[int] = Signal(40)     # whole dollars, kept simple
tip: Signal[int] = Signal(18)      # percent
people: Signal[int] = Signal(2)

def bill_up() -> None   { bill.set(bill() + 5) }
def bill_down() -> None { bill.set(bill() - 5) }
def tip_up() -> None    { tip.set(tip() + 1) }
def tip_down() -> None  { tip.set(tip() - 1) }
def add_person() -> None    { people.set(people() + 1) }
def remove_person() -> None { people.set(people() - 1) }

view: HTML = template[HTML] {
  <style>
    body { font-family: system-ui; max-width: 22rem; margin: 2rem auto; }
    .row { display: flex; justify-content: space-between; align-items: center; }
    button { font-size: 1.1rem; width: 2.2rem; }
    .total { font-size: 2rem; text-align: center; margin-top: 1rem; }
  </style>
  <h1>Tip Calculator</h1>

  <div class="row">
    <span>Bill</span>
    <span><button onclick="!{bill_down}">−</button> $!{bill()} <button onclick="!{bill_up}">+</button></span>
  </div>
  <div class="row">
    <span>Tip</span>
    <span><button onclick="!{tip_down}">−</button> !{tip()}% <button onclick="!{tip_up}">+</button></span>
  </div>
  <div class="row">
    <span>People</span>
    <span><button onclick="!{remove_person}">−</button> !{people()} <button onclick="!{add_person}">+</button></span>
  </div>

  <div class="total">$!{(bill() + bill() * tip() // 100) // people()} each</div>
}

win: Window = Window("Tip Calculator", 380, 320)
win.body = view
win.show()
ui.App.run()
```

Every button mutates one signal; every `!{...}` that reads a signal
becomes a live node. The total line reads `bill`, `tip`, and `people`, so
it re-computes when any of the three changes - and only that node is
patched. The whole app is one file, stylesheet and all, with no
framework, no IPC, and no virtual DOM.

## Distributing the app

On Linux, build the finished app the same way as any `ui` program -
compile the webview shell in and link webkit:

```bash
INCS=( $(pkg-config --cflags-only-I webkit2gtk-4.1 | sed 's/-I/-I /g') )
LIBS=( $(pkg-config --libs-only-l webkit2gtk-4.1) )
dragon build tip.dr --cc-source stdlib/ui/desktop/webview_linux.cpp \
    "${INCS[@]}" "${LIBS[@]}" -lpthread -o tip
```

That produces a single native executable. The renderer itself is the OS's
webview - so the binary is a few megabytes, not the hundred-plus a bundled
browser engine would add. It is not fully self-contained, though: it
dynamically links GTK3 and webkit2gtk-4.1, so the target machine needs those
libraries installed (they ship with most Linux desktops).

The same `.dr` source is designed to build on every desktop platform. The
platform shell - the only non-Dragon code in the stack - is selected at build
time: WebKitGTK on Linux, WKWebView on macOS, WebView2 on Windows. Your signals,
views, and handlers are identical across all three. For installers (`.deb`,
`.dmg`, `.msi`), see [Packaging](/docs/1003-packaging-eggs).
