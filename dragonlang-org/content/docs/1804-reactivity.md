# Signals and Reactivity

A static view paints once. A *reactive* view re-paints the exact node that
changed, the moment the value behind it changes - with no diffing and no
manual DOM bookkeeping. The machinery is two small pieces: a `Signal`
(reactive state) and the bindings the compiler sets up when you interpolate
one into a view.

## Signals: reactive cells

A `Signal[T]` is a reactive cell. You construct it directly - the binding
annotation pins `T`, the same generic-construction rule as `Box[int] =
Box(99)`. You read it by calling it, and write it with `.set(...)`:

```dragon
from ui import Signal

count: Signal[int] = Signal(0)

x: int = count()        # read  -> 0
count.set(count() + 1)  # write -> any watcher re-runs
```

`Signal(0)` is the whole constructor - there is no `signal(0)` factory to
remember. It reads exactly like any other class: `Signal(0)`, `Lock()`,
`Path("/tmp")`.

## Effects: the engine under reactivity

Reading a signal inside a *watcher* subscribes that watcher to it. The
simplest watcher is an `effect`: it runs once immediately (recording every
signal it touches) and re-runs automatically whenever any of those signals
changes.

```dragon
from ui import Signal, effect

count: Signal[int] = Signal(0)

effect(lambda () -> None {
    print("count is now " + str(count()))
})

count.set(1)   # effect re-runs -> "count is now 1"
count.set(2)   # effect re-runs -> "count is now 2"
```

`effect` is the manual lever; most of the time you will never call it
directly, because interpolating a signal into a view sets up the same
subscription for you. But it is the whole engine: a signal is a value plus
a list of subscribers, and `.set` walks that list. Nothing more.

## Reactive text: the screen follows the signal

Interpolate a signal into a view and the compiler turns that slot into a
live binding - no `effect`, no node id, no update function on your part:

```dragon
import ui
from html import HTML
from ui.desktop import Window

count: Signal[int] = Signal(0)

view: HTML = template[HTML] {
  <main style="font-family:system-ui;text-align:center;margin-top:3rem">
    <h1>Count: !{count()}</h1>
  </main>
}

win: Window = Window("Counter", 360, 240)
win.body = view
win.show()
ui.App.run()
```

The `!{count()}` is the whole trick: the compiler wraps that value in a
node it can find later and registers a binding that re-evaluates `count()`
and patches the node whenever `count` changes. You wrote no node id, no
`querySelector`, no update function - the markup `<h1>Count: 0</h1>` and
the live updates both fall out of `!{count()}`. The moment something calls
`count.set(...)` - a timer, a background task, or (most often) a
[button](/docs/1805-event-handlers) - that one `<h1>` updates and nothing
else.

Patches go through the node's text content, never `innerHTML`, and the
value is escaped on the way across - so a reactive value is **XSS-safe by
construction** even when it comes from user input.

> Reactive interpolations read **module-global** signals - declare your
> app state at the top level of the file, where the view can see it. A
> reactive `!{...}` that reads a function-local signal is a compile error,
> not a silent no-update, so you find out at build time.

## Derived values need no special concept

Reactive interpolations are ordinary expressions, so derived values work
without a separate "computed" primitive - just read the signals you need:

```dragon
celsius: Signal[int] = Signal(20)

view: HTML = template[HTML] {
  <main style="font-family:system-ui;text-align:center">
    <h1>!{celsius()} °C</h1>
    <h2>!{celsius() * 9 // 5 + 32} °F</h2>
  </main>
}
```

Both headings track `celsius`: a change re-runs the binding for the `°C`
node *and* the `°F` node (which depends on `celsius` through the
conversion), and patches each. Each binding subscribes to exactly the
signals its expression reads, so unrelated nodes are never touched.

So far the state changes from code. To let the *user* change it, wire an
event handler - the next page, [Event Handlers](/docs/1805-event-handlers).
