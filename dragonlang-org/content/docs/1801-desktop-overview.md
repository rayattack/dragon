# Desktop Applications

Every desktop UI toolkit asks you to pay a tax. Electron bundles a whole
copy of Chromium with each app - a hundred megabytes on disk and a few
hundred in RAM before you draw a single pixel. Tauri trims the binary by
borrowing the OS webview, but now you are wiring a Rust backend to a
JavaScript frontend across an IPC seam, with two toolchains and an `npm`
tree to keep happy. Qt and GTK hand you a C++ build system and a widget
hierarchy to learn before "hello window." In each case the *reactivity* -
the part where changing a value updates the screen - is a framework you
bolt on top, in a second language.

Dragon ships a reactive desktop UI in its standard library, written in
Dragon, rendered through the webview the operating system already ships
(WebKitGTK on Linux, WKWebView on macOS, WebView2 on Windows). You write
one language, you get one native binary, and the renderer is the OS's -
so your app stays small. The state lives in Dragon `Signal`s, the view is
a Dragon `HTML` [template](/docs/1201-templates), and the compiler wires
the two together so that `count.set(...)` updates exactly the node on
screen that reads `count` - no virtual DOM, no diff, no IPC glue you
maintain.

This topic builds a real reactive app end to end, one concept per page:

- **[Windows and the App Loop](/docs/1802-windows)** - open a window, set
  its `body`, run the event loop.
- **[Views Are HTML Templates](/docs/1803-views)** - the view is typed
  `HTML`, escaped for its context, composable.
- **[Signals and Reactivity](/docs/1804-reactivity)** - reactive state and
  the bindings that make the screen follow it.
- **[Event Handlers](/docs/1805-event-handlers)** - point an HTML event at
  a Dragon function; build the full interactive counter.
- **[Styling, Assets, and Shipping](/docs/1806-assets-and-shipping)** - a
  complete app, a stylesheet, and one command to a native binary.

Everything above the thin platform shell is `.dr` you can read in
`stdlib/ui/`.

## The model: webview paints, Dragon thinks

A Dragon desktop app is two layers with a sharp line between them. The
**webview** is a rendering surface: it turns HTML and CSS into pixels and
reports clicks. **Dragon** owns everything that matters - the state, the
logic, the decisions about what the screen should say. HTML is the paint,
not the contract; you never ship business logic into JavaScript.

The connection between them is *fine-grained reactivity*. A `Signal` is a
value the UI watches. When you interpolate one into a view -
`<h1>!{count()}</h1>` - the compiler tags that spot with a hidden id and
remembers that this node depends on `count`. The moment `count.set(...)`
runs, Dragon patches that one node and nothing else. There is no diffing
of a shadow tree against the real one, because Dragon already knows the
exact node that changed. It is the React mental model without the React
machinery, resolved at compile time.

Because the renderer belongs to the OS, your binary does not bundle one -
a Dragon desktop app is the same few megabytes as any other Dragon
program. It does link the system webview dynamically (on Linux, GTK3 plus
webkit2gtk-4.1), so that library has to be present on the target machine;
on most desktops it already is.

## When to reach for webview

A webview UI is the right tool when your app is content-shaped: forms,
dashboards, document and media views, anything where HTML and CSS already
express what you want and a few hundred milliseconds of cold-start and a
few tens of megabytes of resident memory are a fine trade for that reach.
It is the same trade Electron and Tauri make, minus the bundled browser
and the second language.

When you need the tightest possible event loop and the smallest possible
footprint (a high-frequency editor, a tool that must idle in single-digit
megabytes), Dragon's native-widget backend (compiling the same `Signal`,
`Window`, and view model to real platform widgets, no webview) is the
faster default for that class of app. The reactive core you learn here
carries over unchanged.
