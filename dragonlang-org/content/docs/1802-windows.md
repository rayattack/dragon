# Windows and the App Loop

The smallest desktop app opens a window, sets its content, and runs the
event loop:

```dragon
import ui
from html import HTML
from ui.desktop import Window

win: Window = Window("Hello", 360, 200)
win.body = template[HTML] { <h1 style="font-family:system-ui;text-align:center">Hello, Dragon</h1> }
win.show()
ui.App.run()
```

As everywhere in Dragon, **the file you hand to `dragon run` is the
program** - there is no `main`, no magic entry point (see
[Modules](/docs/1001-modules)). The top-level statements run in order:
construct a `Window`, set its `body`, show it, then call `ui.App.run()`,
which hands control to the platform event loop and returns only when the
last window closes. `run()` is the last line for the same reason
`app.listen()` is in a server - it *is* the program's body.

On Linux (GTK3 + webkit2gtk), build and run it like any Dragon program:

```bash
dragon build hello.dr -o hello
./hello
```

`import ui` is all the build needs to see. The `ui.desktop` shell is a small
C++ file (`platform/webview_linux.cpp`, from the native-shim tree shipped
beside the stdlib) wrapping the system webview; the compiler
compiles it in automatically and resolves the GTK/webkit include and link
flags through `pkg-config` (webkit2gtk-4.1, or 4.0 on older distros). The
build machine needs the development package (Debian/Ubuntu
`libwebkit2gtk-4.1-dev`, Fedora `webkit2gtk4.1-devel`); the target machine
only needs the ordinary runtime libraries, which ship with most Linux
desktops. To build a patched or custom shell instead, pass it explicitly
with `--cc-source` plus your own `-I`/`-l` flags - an explicit webview shim
always wins over the automatic one. (`ui.App.timeout(ms)` is the same
loop with an automatic quit after `ms` milliseconds - handy for smoke tests
and screenshots.)

## The window is its `body`

A `Window` has a `title` and a size, set at construction, and a `body` -
the document it shows. `body` is an [`HTML`](/docs/1202-html-css-xml)
value, the same markup type the template system produces and the web
framework's `res.body` carries. Assigning it paints the window:

```dragon
from html import HTML
from ui.desktop import Window

win: Window = Window("Hello", 360, 200)
win.body = template[HTML] { <h1>Hello, Dragon</h1> }   # paints now
win.body = template[HTML] { <h1>Updated</h1> }         # repaints
```

There is no `set_html(...)` call and no `str` seam: the view is typed
`HTML` end to end, so the [content-aware escaping](/docs/1202-html-css-xml)
applies and a component can splice another component without
double-escaping. Reading `win.body` returns the document currently shown.
This is the same property shape as `res.headers` in the
[web chapter](/docs/1701-web-application) - assigning *does* something
(here, repainting the webview), so it is a setter, not a bare field.

This is a *static* render: assigning `body` paints once. To make the
screen follow your data as it changes, you reach for the next two pieces -
[HTML views](/docs/1803-views) you can interpolate into, and
[signals](/docs/1804-reactivity) the view can watch.

## The `App` lifecycle

`ui.App` owns the run loop. It has two entry points, both static:

| Call | What it does |
|---|---|
| `ui.App.run()` | Enter the platform run loop; returns when the last window closes. |
| `ui.App.timeout(ms)` | Same loop, but auto-quit after `ms` milliseconds. |

`timeout` exists for tests and screenshots - a windowed app otherwise
blocks until a human closes it, which a CI run cannot do. In a shipped app
you call `run()` as the final top-level statement.

## Many windows, one loop

Windows are independent: construct several, show several - one `run()`
serves them all, and it returns only when the *last* open window closes.
Closing one of three keeps the other two alive.

```dragon
import ui
from ui.desktop import Window

main_win: Window = Window("Post Office", 800, 600)
inspector: Window = Window("Inspector", 400, 600)
main_win.show()
inspector.show()
ui.App.run()   # returns when BOTH windows have been closed
```

`close()` closes a window from code - the same path as the user clicking
its close button:

```dragon
from ui.desktop import Window

inspector: Window = Window("Inspector", 400, 600)

def dismiss_inspector() -> None {
    inspector.close()
}
```

A closed window stays closed: calling `show()`, `close()`, or assigning
`body` on it is a no-op. To bring one back, construct a new `Window`.

## Debugging with the Web Inspector

Every window can host WebKit's Web Inspector - the same DOM, console, and
network panel a browser's devtools give you, pointed at your window's
document. It is off by default. From code, opt a window in at construction
or later:

```dragon
from ui.desktop import Window

win: Window = Window("Hello", 360, 200, inspect = true)   # on from birth

win.enable_inspector()   # make this window inspectable
```

How you then open the inspector depends on the platform: on Linux the
window's right-click menu gains "Inspect Element"; on macOS the window
becomes attachable from Safari's **Develop** menu (Develop > your app >
the page). There is no programmatic "open the pane now" call - WKWebView
has no public API for it, so Dragon does not pretend to offer one.

For a build you cannot or do not want to touch, set `DRAGON_UI_INSPECT=1`
in the environment - every window of the app comes up inspectable, no
recompile:

```bash
DRAGON_UI_INSPECT=1 ./hello
```

The console tab shows anything your view scripts log, plus errors from the
`window.dr` bridge shim, which makes it the quickest way to see why a
binding or an [`rpc` call](/docs/1807-the-bridge) is not doing what you
expect.
