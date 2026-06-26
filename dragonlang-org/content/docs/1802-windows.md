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

On Linux (GTK3 + webkit2gtk-4.1), build it by compiling the webview shell
in and pointing the link at webkit:

```bash
INCS=( $(pkg-config --cflags-only-I webkit2gtk-4.1 | sed 's/-I/-I /g') )
LIBS=( $(pkg-config --libs-only-l webkit2gtk-4.1) )
dragon build hello.dr --cc-source stdlib/ui/desktop/webview_linux.cpp \
    "${INCS[@]}" "${LIBS[@]}" -lpthread -o hello
./hello
```

The `ui.desktop` shell is a small C++ file (`webview_linux.cpp`) that wraps
the system webview; `--cc-source` compiles it in, and the `pkg-config` flags
point the link at GTK3 and webkit2gtk-4.1, which must be installed on both the
build and target machines. (`ui.App.run_timeout(ms)` is the same loop with an
automatic quit after `ms` milliseconds - handy for smoke tests and screenshots.)

## The window is its `body`

A `Window` has a `title` and a size, set at construction, and a `body` -
the document it shows. `body` is an [`HTML`](/docs/1202-html-css-xml)
value, the same markup type the template system produces and the web
framework's `res.body` carries. Assigning it paints the window:

```dragon
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
| `ui.App.run_timeout(ms)` | Same loop, but auto-quit after `ms` milliseconds. |

`run_timeout` exists for tests and screenshots - a windowed app otherwise
blocks until a human closes it, which a CI run cannot do. In a shipped app
you call `run()` as the final top-level statement.
