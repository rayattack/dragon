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
that works for any other markup. When the stylesheet outgrows an inline block,
move it to a file in the `assets/` directory and link it - that is the next
section.

## Real assets: the `assets/` directory and `app://`

Put an `assets/` directory next to your entry file and `dragon build` embeds
every file in it into the binary. At runtime an in-process `app://` handler
serves them straight from that embedded blob - no localhost server, no open
port, no files to install next to the executable:

```text
weather/
├── main.dr
└── assets/
    ├── app.css
    ├── logo.svg
    └── charts.min.js
```

```dragon
view: HTML = template[HTML] {
  <head>
    <link rel="stylesheet" href="app:///app.css">
    <script src="app:///charts.min.js"></script>
  </head>
  <body>
    <img src="app:///logo.svg" alt="logo">
    ...
  </body>
}
```

The URL path is the file's path relative to `assets/`, so subdirectories keep
their shape (`assets/img/icon.png` is `app:///img/icon.png`). Content types
come from the extension - CSS, JS, images, fonts, JSON, wasm all serve
correctly, and page script can `fetch('app:///data.json')` like any other URL.

This is how third-party frontend libraries arrive, too. The webview is a real
browser, so anything that runs on a web page runs here: download a library
once, drop the built file into `assets/`, link it with `<script
src="app:///...">`. No CDN at runtime, no bundler, no `npm` tree - and the
finished app still works on a machine with no network.

Two things to keep in mind:

- Assets are read **at build time**. `dragon run` recompiles on every run so
  edits just show up; with a prebuilt binary, rebuild to pick up changed
  assets.
- Every byte in `assets/` lands in the binary. A few hundred kilobytes of CSS
  and JS is nothing; think twice before dropping a video in.

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

On Linux, build the finished app the same way as any Dragon program:

```bash
dragon build tip.dr -o tip
```

`import ui` is the whole story: the compiler sees it, compiles the webview
shell in, and resolves the GTK/webkit flags through `pkg-config`. The build
machine needs the webkit2gtk development package
(Debian/Ubuntu `libwebkit2gtk-4.1-dev`, Fedora `webkit2gtk4.1-devel`).

That produces a single native executable, with everything in `assets/`
embedded inside it - copy one file to another machine and the app carries its
stylesheets, images, and vendored libraries with it. The renderer itself is
the OS's webview - so the binary is a few megabytes, not the hundred-plus a
bundled browser engine would add. It is not fully self-contained, though: it
dynamically links GTK3 and webkit2gtk-4.1, so the target machine needs those
libraries installed (they ship with most Linux desktops).

The platform shell - the only non-Dragon code in the stack - is selected at
build time: WebKitGTK on Linux, WKWebView on macOS. Your signals, views, and
handlers are identical on both. Windows has no shell yet, so `import ui` does
not build there. For installers (`.deb`, `.dmg`), see
[Packaging](/docs/1003-packaging-eggs).

One more piece completes the desktop story: when the page runs its own
JavaScript - a library you dropped into `assets/`, or plain script you wrote -
it calls back into Dragon by name over the bridge. That is
[Page Script and the Bridge](/docs/1807-the-bridge).
