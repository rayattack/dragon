# Page Script and the Bridge

[Event handlers](/docs/1805-event-handlers) wire clicks on markup that
Dragon renders and re-paints. But the webview is a real browser: a page can
carry its own `<script>`, and anything that runs on a web page runs here -
a chart library, an editor, a keyboard shortcut layer, plain vanilla JS you
wrote for one input. The moment page script owns some interactivity, it
needs a way to reach Dragon - to load data, to save, to do the things only
the native side can do.

That is the bridge. Page script calls a *named* Dragon handler and gets a
Promise; Dragon registers the handler by name:

```dragon
from ui import rpc

def lookup(payload: str) -> str {
    ...
}

rpc("weather.lookup", lookup)
```

```js
window.dr.call('weather.lookup', JSON.stringify({ city: 'Lagos' }))
  .then(function (s) { console.log(JSON.parse(s)); });
```

A handler takes one `str` and returns one `str`. The bridge does not
impose a format, but JSON is the convention on both sides:
[`decode[dict[str, Any]]`](/docs/1404-stdlib-data) / `json.dumps` in Dragon,
`JSON.parse` / `JSON.stringify` in the page. Names are plain strings; the
dots are just a naming convention. Register everything before
`App.run()` - registration is setup, the run loop is the program's body.

## A complete app

The page below owns its own little UI in vanilla JS; Dragon owns the data.
The button's click handler is page script, not a Dragon event handler, and
it reaches Dragon through the bridge:

```dragon
import ui
import json
from json import decode
from html import HTML
from ui import rpc
from ui.desktop import Window

def lookup(payload: str) -> str {
    const d: dict[str, Any] = decode[dict[str, Any]](payload.encode("utf-8"))
    const city: str = d["city"]
    temps: dict[str, int] = {Lagos: 31, Abuja: 28, Jos: 22}
    if city not in temps {
        raise ValueError(f"no reading for {city}")
    }
    return json.dumps({city: city, celsius: temps[city]})
}

rpc("weather.lookup", lookup)

view: HTML = template[HTML] {
  <body style="font-family:system-ui;max-width:22rem;margin:2rem auto">
    <h1>Weather</h1>
    <input id="city" value="Lagos">
    <button id="go">Look up</button>
    <p id="out"></p>
    <script>
      document.getElementById('go').onclick = function () {
        var city = document.getElementById('city').value;
        window.dr.call('weather.lookup', JSON.stringify({ city: city }))
          .then(function (s) {
            var r = JSON.parse(s);
            document.getElementById('out').textContent = r.city + ': ' + r.celsius + ' C';
          })
          .catch(function (e) {
            document.getElementById('out').textContent = e.message;
          });
      };
    </script>
  </body>
}

win: Window = Window("Weather", 420, 260)
win.body = view
win.show()
ui.App.run()
```

Type a city, click the button, and the page script round-trips through
Dragon: `window.dr.call` posts the request to native, `lookup` runs, and
the Promise resolves with its return value.

## Errors reject the Promise

Look at the handler again: an unknown city does not return an error blob,
it *raises*. A raised exception rejects the page's Promise, and the
exception message becomes the rejection's `Error.message` - so the page
handles failure where JS already handles failure, in `.catch`:

```js
window.dr.call('weather.lookup', JSON.stringify({ city: 'Nowhere' }))
  .catch(function (e) { console.log(e.message); });   // "no reading for Nowhere"
```

Calling a name nothing registered rejects the same way. One channel for
success, one for failure, on both sides of the bridge.

## Handlers never block the window

Every handler runs on a [green thread](/docs/1101-green-threads), off the
UI thread. A handler can read a database, call a web API over the
[http client](/docs/1408-stdlib-networking), or crunch for a second - the
window keeps painting, scrolling, and answering clicks the whole time, and
the Promise settles when the work is done. Two calls in flight at once are
two green threads; they do not queue behind each other.

The flip side of that freedom is the usual one: handlers run concurrently,
so shared mutable state needs the same care as any
[concurrent code](/docs/1104-synchronization). A module-level
[`Lock`](/docs/1104-synchronization) around your store is the boring,
correct default.

## Which tool when

- **Dragon owns the state, Dragon paints it** - use
  [signals](/docs/1804-reactivity) and
  [event handlers](/docs/1805-event-handlers). No JS to write at all.
- **The page owns some interactivity in JS** (a library from
  [`assets/`](/docs/1806-assets-and-shipping), an editor, custom canvas
  work) - give it data and actions with `ui.rpc` + `window.dr.call`.

The two compose freely in one window: reactive Dragon views around a
JS-driven island is a normal shape, not a compromise.
