# Decision 031: UI Toolkit - `template { }` + System WebView

**Status:** Approved. v1 scope locked. Native widgets are now the **default** desktop backend; webview is the **deliberately-labeled opt-in slow path** - whole-app via `--backend=webview`, *and* embeddable as a `WebView` widget inside native trees. Webview ships first (it builds the `Signal`/`Window`/`View` reactive core native reuses wholesale); build order is **IR → webview → native**. Implementation not started.

I want a UI story where `dr build` gives you a real app, not "install Electron and pray." Webview is the fast path to shipping - same `template { }` source on Linux/macOS/Windows/iOS/Android. But native widgets are where commandment #1 actually lives long-term. This ADR locks v1 scope before I sketch another thousand lines of options analysis.

## Summary

`stdlib/ui/` is a first-class UI toolkit: `template { }` for views, the **OS webview** for rendering. The same source code targets Linux desktop (WebKitGTK), macOS (WKWebView), Windows (WebView2), iOS (WKWebView in `UIViewController`), and Android (`WebView` in `Activity`). Reactivity is provided by a small Dragon-native `Signal` primitive that re-renders dependent template subtrees on change. Native capabilities (camera, GPS, file pickers, notifications, biometrics, share sheets) live in `ui/native/` as a shared plugin layer with per-platform implementations behind FFI.

I'm not doing bundling a renderer (Skia/Flutter-style) and rejects native-widget bindings (Qt/GTK-style) as the **primary** path. Both remain viable as future *additional* backends behind the same `ui/` API; neither is required for v1.

## Locked v1 Scope

> **This is v1. If anything below disagrees, this section wins.** - notably: re-render is **targeted node patches** (not innerHTML-replace), asset delivery is an **in-process `app://` scheme handler** (not a localhost server), and the threading model is **concrete** (not " handles it"). The text below this section is retained as options analysis and rationale.

Priority order, no exceptions: **(1) speed, (2) no workarounds, (3) Python parity.** Speed wins ties.

### Ten decisions

| # | Decision | Choice | Why (commandment) |
|---|---|---|---|
| 1 | Primary v1 backend | **System webview (ships first; the deliberate slow path)** | #3 adoption + time-to-market. Native is the **default destination** behind the same `Window`/`View`/`Signal` API; webview is the knowingly-chosen slow path, and the reactive core built here is reused by native. See 's hot-path/slow-path doctrine. |
| 2 | Reactivity | **Fine-grained signals**, owner-explicit (`Signal`/`Watch`/`Computed`, each constructed against an `App`) | #1 - update only what changed; no VDOM/diff cost. Explicit ownership over implicit module-global state (see *Reactivity and lifecycle*) |
| 3 | Re-render mechanism | **Targeted node patches** | #1 - minimal op on the exact node; no HTML re-parse; preserves focus/scroll/selection |
| 4 | Asset/template delivery | **In-process `app://` scheme handler**; assets embedded in the binary | #1 (no socket/HTTP hop) + security (no open port) + single-file ship |
| 5 | High-frequency hot path | **Native backend **; for web surfaces, `SharedSignal` mirror keeps the hot loop local to JS | #1 - a native app's hot path is native (no bridge). `SharedSignal` is *not* a way to make webview match native (the workaround #2 rejects) but how a deliberately-chosen web surface (whole-app, or an embedded `WebView` island) runs 60-120 Hz drag/scroll/draw in its own JS without crossing per event. |
| 6 | Threading | **Reactor hosted on the platform UI runloop**; reuse `fire`/`thread{}`; one new `run_on_ui` marshal | #1 - no cross-thread marshalling on the common path; reuses, no new scheduler |
| 7 | Dogfood boundary | **Hold the line** - C++/FFI only for unexpressible platform glue; everything else `.dr` | #2 + dogfooding policy; `.dr` is C-speed so this costs no #1 |
| 8 | v1 platform target | **Desktop trio** (Linux → macOS → Windows); mobile = later phases | roadmap |
| 9 | v1 reactive/dev features | **All in**: multi-window, `computed`, hot reload (dev), Theme tokens | - |
| 10 | v1 native chrome | **All in**: file/save/alert dialogs, native menubar, notifications, system tray, clipboard | - |

### Targeted-patch protocol (closes the v1 innerHTML gap)

- **Compile time.** Every `!{expr}` interpolation and every signal-bound attribute is assigned a stable node id. The template compiler emits, alongside the HTML, a **binding table**: `signal → [(node_id, slot)]` where `slot ∈ {text, attr:<name>, prop:<name>, children}`. Text-node bindings are anchored with a marker so the exact text node is addressable.
- **Initial render.** Full HTML (with embedded `data-dr` ids) is produced once and handed to the webview via the `app://` document.
- **On signal change.** The runtime recomputes the value, looks up the binding table, and sends one minimal op over the bridge:
 - `{"op":"text","id":"n7","value":"42"}`
 - `{"op":"attr","id":"n7","name":"class","value":"active"}` (`prop` for `value`/`checked`)
 - keyed list ops for `for`-loops: `{"op":"insert"|"remove"|"move","id":...,"key":...,"html":...}`
 - `{"op":"replace-children","id":"n3","html":"..."}` only for a conditional subtree swap.
- **Shim.** The locked `window.dr` shim applies ops directly (`textContent` / `setAttribute` / keyed `replaceChildren`). No diff, no full-subtree re-parse. List reconciliation is keyed - the one place we keep a tiny reconcile, still far cheaper than a full diff.

### `app://` in-process delivery (closes the asset-serving gap)

Per-platform scheme handler, no socket, no port:
- **Linux (WebKitGTK):** `webkit_web_context_register_uri_scheme("app", …)`, treated as secure/local.
- **macOS/iOS (WKWebView):** `WKWebViewConfiguration.setURLSchemeHandler:forURLScheme:"app"`.
- **Windows (WebView2):** `AddWebResourceRequestedFilter("app://*", …)` + `WebResourceRequested`.

Assets (`assets/` + generated template HTML/CSS + the shim) are packed at `dr build` into a read-only blob linked into the executable; the handler resolves `app://<path>` against that blob → single ~8-12MB binary, OS webview not bundled.

### Threading model (closes the " handles it" gap)

Webviews require main-thread affinity, so:
- **One thread, runloop as master.** Dragon's epoll/kqueue reactor is added as a **source on the platform UI runloop** - `GSource` (GTK), `CFRunLoopSource`/`CFFileDescriptor` (macOS), `MsgWaitForMultipleObjectsEx` (Windows). `fire`/`await`/I/O run cooperatively on that thread; signal mutation on the UI thread costs nothing extra.
- **Background work** uses the *existing* primitives - `fire` (green, cooperative I/O on the UI loop) and `thread { }` (scoped OS thread, CPU-bound off it).
- **One new primitive:** `run_on_ui(closure)` marshals a DOM-affecting write from a non-UI carrier back to the UI thread (`g_idle_add` / `dispatch_async(main)` / `PostMessage`). `Signal.set` auto-detects current-thread ≠ UI-thread and marshals. No new scheduler - everything else is .

### Dogfood boundary - the ONLY C++/FFI permitted

- **C++/FFI (unexpressible platform glue):** the per-platform webview shell (create window+webview, register `app://` handler, inject the locked `window.dr` shim at document-start, set CSP, intercept navigation), the reactor↔runloop source, `run_on_ui`, and the native-chrome bindings (menubar, dialogs, notifications, tray, clipboard).
- **`.dr` (everything else):** `App`/`Signal`/`Watch`/`Computed`/`SharedSignal`, the binding-table → patch-op logic, the bridge protocol (JSON via `stdlib/json`), the callback/`@exposed` registry, the re-render scheduler, the `Window`/`App`/`Menu`/`Dialog`/`Notification`/`Tray`/`Clipboard` Dragon-side API objects, `Theme`, multi-window orchestration, and the hot-reload watch+recompile driver (reuses the compiler).

### Compiler work required

1. **Template node-identity tagging + binding-table emission** (extends `TemplateExpr` codegen in `src/codegen/Literals.cpp`) - the core new work.
2. **Compile-time event-handler extraction**: `onclick=!{self.inc}` → register handler, emit `onclick="window.dr.invoke('cb_N')"`; missing handler / wrong arity = build error (reuses existing interpolation checking).
3. **`@exposed` decorator**: semantic registration into a per-binary exposed-table; `window.dr.invoke('name', …)` resolves against it; args/returns marshalled at the function's declared types (no `Any`) via `stdlib/json`.
4. **Asset embedding**: build-time packer + linker integration in `src/Driver.cpp` (generate the blob object, link it, expose the symbol to the scheme handler).
5. Nothing else needs compiler magic - reactivity, windows, chrome, theme, navigator are plain `.dr` over FFI + closures.

### Revised v1 phase plan (desktop trio)

- **Phase 0 - Linux core (PoC).** WebKitGTK shell + `app://` handler + locked `window.dr` shim + reactor-on-`GMainLoop` + `run_on_ui`; `ui.dr` `App`/`Signal`/`Watch`/`Computed`; `bridge.dr` (JSON + callback registry); the targeted-patch protocol end-to-end; single `Window`. **Exit:** counter + todo + form work; binary <12MB; first paint <200ms; patches preserve focus/scroll/selection.
- **Phase 1 - Reactivity + hot path + multi-window (Linux).** `SharedSignal` mirror + frame-coalesce; keyed list reconcile; multi-window; per-tick batched updates. **Exit:** 60-120Hz drag/scroll demo with no per-event bridge crossing; 3-window demo.
- **Phase 2 - Native chrome + theming (Linux).** File/save/alert dialogs, native menubar, notifications, system tray, clipboard, `Theme` tokens (+ OS dark-mode signal). **Exit:** all native chrome works on GTK; light/dark follows the OS.
- **Phase 3 - macOS.** Cocoa/WKWebView shell, `app://` via `WKURLSchemeHandler`, reactor-on-`CFRunLoop`, `NSMenu`, `NSOpenPanel`/`NSSavePanel`/`NSAlert`, `UNUserNotificationCenter`, `NSStatusItem`, `NSPasteboard`. **Exit:** Phase 0-2 demos run unchanged with native macOS chrome.
- **Phase 4 - Windows.** WebView2 shell, `app://` via `WebResourceRequested`, reactor-on-`MsgWaitForMultipleObjectsEx`, `HMENU`, `IFileDialog`/`MessageBox`, WinRT toast, `Shell_NotifyIcon`, clipboard. **Exit:** demos run on Windows 10+.
- **Phase 5 - Hot reload (dev).** File-watch → recompile changed template → push patch over the bridge; dev-only, production stays static. **Exit:** edit a `.dr`, UI updates <500ms without losing state.
- **Phase 6 - Packaging.** Finalize single-file embed; reuse existing CPack (DEB/RPM/dmg/msi); macOS notarization + Windows Authenticode; `dr new --ui` scaffold + `dr ui-demo`. **Exit:** signed installers on all three desktops.

**Next major effort (in, not a new ADR):** the **native-widget backend is the default desktop destination** - build order is IR (Ph0) → this webview trio → native (GTK→AppKit→Win32), with the `Signal`/`Window`/`View` reactive core built here reused wholesale by native. Webview then becomes the opt-in slow path and the embeddable `WebView` island (Part 3). The cross-backend layout/styling contract (typed flex/grid/z-stack + `Style`/`Theme` tokens) lives in Part 3; webview realizes it as CSS.

**Other post-v1 (tracked here):** mobile shells (iOS/Android) + native plugins (camera/GPS/biometrics/push); DevTools signal-graph inspector; `ui.web` SSR ; revisit morphdom / JSX-style component syntax / scoped CSS only if benchmarks or real apps demand.

## Reactivity and lifecycle: explicit `App`-owned context

Reactivity and the run loop live on an explicit `App` you construct. That matches the three commandments and Dragon's whole explicit-over-implicit thing (mandatory annotations; `:` declares vs `=` assigns; `global`/`nonlocal` to assign; no magic entry point). The toolkit shows you where state lives instead of hiding it behind globals.

### Why an explicit owner

No module-global reactive state. A global effect registry, a global "current subscriber" pointer, and ids-into-a-registry would mean watchers float in module space - never disposed, disconnected from the run loop that is the only thing that gives them meaning, with nothing marking where the reactive graph is born or torn down. Dragon makes you name what you touch everywhere else. Globals would be weird here.

So **you construct an `App`** and it owns the reactive graph, windows, and run loop. Everything reactive is created through an ordinary constructor that takes its owning `App` as a *visible argument*. `App` is a real stateful type - not an all-`@staticmethod` namespace - which is what earns its existence as a class.

### Surface (authoritative)

```dragon
from ui import App, Signal, Watch, Window
from html import HTML

app: App = App

count: Signal[int] = Signal(app, 0)

def bump -> None { count.set(count + 1) }

w: Watch = Watch(app, lambda -> None { print("count:", count) })

view: HTML = template[HTML] {
 <h1>Count: !{count}</h1>
 <button onclick="!{bump}">+1</button>
}

win: Window = Window(app, "Counter", 360, 240)
win.body = view
app.run
```

- **Real, importable types** - `from ui import App, Signal, Watch, Window`. No factory methods; nothing reachable only through an `app.` prefix.
- **Constructors take the owner** - `Signal(app, initial)`, `Watch(app, fn)`, `Window(app, title, w, h)`, and `Computed(app, fn)` for derived values. *Not* `app.signal(...)` factories.
- **`effect` → `Watch`.** The React/Solid/Preact term `effect` is dropped as insider vocabulary. `Watch` is a constructed, *holdable* object with `.stop` for early teardown - the capital `W` is earned because it carries lifecycle, exactly like `Signal`/`Window`. Fire-and-forget is a bare `Watch(app, …)` statement (the `app` retains it); bind `w: Watch = …` only when you intend to `w.stop`.
- **`app.run` is the explicit lifecycle boundary** - realizes the app's windows (no separate `win.show` on the common path), enters the platform loop, and **disposes the reactive graph on exit** (drops watchers and subscriptions). Setup is everything above the call; the live session *is* the call.

### The one justified piece of shared state

Fine-grained reactivity has one bit of unavoidable dynamic state: **"which watcher is subscribing right now."** When `Watch(app, fn)` runs `fn` and `fn` reads `count`, `count` must record the subscriber - but the subscriber is discovered mid-expression *inside a user closure*, so it cannot be passed as an argument. Solid, Vue, and Preact all keep this as a current-computation pointer for precisely this reason; it is mechanism, not a DX choice.

That pointer lives on **`App._active`**, not in a module global. **This is the entire justification for the `app` constructor argument** - it is the explicit reference each `Signal`/`Watch`/`Window` needs to reach the one rendezvous point. A constructor with no owner would have to consult a module global; the `app` argument is the explicit form of the thing that global was hiding. The threading is a real dependency made visible, not ceremony.

Everything else is distributed and needs no shared registry: `Signal._subs` holds the **watcher closures directly** (`list[Callable[[], None]]`), and `Signal.set` invokes them with `for f in self._subs { f }`. There is no global effect registry - the subscription graph lives on the individual signals. (Storing the closures directly relies on the closure-iteration fix, bugs.md #17.)

### Rejected alternatives

- **Module-global reactive registry** - implicit, no lifetime. Rejected.
- **Implicit owner managed behind a free `ui.run`** - an ambient root nobody names; contrary to explicit-over-implicit. Rejected.
- **Factory methods** (`app.signal(0)`, `app.watch(fn)`) - sugar that buys nothing a constructor doesn't and breaks the import-and-construct model. Rejected.
- **`win.run` / `win.watch`** - a scope-lie: the loop and the reactive graph are application-global, not per-window (with two windows, *whose* `.run`/`.watch`?). Ownership belongs to `App`. Rejected.
- **A `Watcher` manager with `id = m.watch(fn)` / `m.unwatch(id)`** - forces id bookkeeping, and over a shared graph gives *fake* isolation (two managers silently mutating the same state). The `Watch` handle's `.stop` carries its own identity - no id to track, no wrong-registry footgun. Rejected.
- **`App` → `Application` inheritance** - a two-class hierarchy with no behavioral difference is pure ceremony. There is one `App` class; a subclass-and-override entry point (Kivy/Toga style) may be added later as *optional additive* sugar, never a mandated parallel type. Rejected.

### Compiler-binding routing

Compiler-generated reactive bindings stay explicit through the object graph, never an ambient global: `!{count}` binds through `count`'s owning `app` (the `Signal` carries the reference); `onclick="!{bump}"` registers through the window's `app` (`win` knows its `app`). The graph does the routing; there is no ambient discovery step.

## Motivation

Dragon already has three pieces:

1. **A document-shaped DSL** - `template { ... }` with `!{expr}` interpolation, pipe filters (`html`/`sql`/`url` + user-defined `(str)->str`), and compile-time file includes (`template("file.html")`). It is HTML-shaped by design.
2. **C FFI** - `extern "C"` + `ptr` type, enabling thin native shells per platform.
3. **Concurrency that doesn't block UI** - `fire` green threads, colorless `await`, and platform I/O yielding (epoll/kqueue) already cooperate with event loops.

Missing piece: **mount a template in a window and update it when state changes**. That's the whole gap.

Three families on the table:

- **Native-widget bindings** (Qt, GTK): Robust but small developer pool, LGPL/commercial licensing pressure, thousands of classes to wrap, and doesn't use `template { }`. Users would learn a Dragon-flavored Qt, not Dragon.
- **Own renderer** (Skia + custom scenegraph, Flutter-style): Maximum control and theoretically peak speed, but multi-quarter project that re-invents accessibility, IME, HiDPI, RTL, and decades of widget polish. Doesn't use `template { }` either.
- **System WebView + `template { }`** (this decision): Reuses every existing strength, ships in weeks not quarters, inherits the entire web platform (CSS, flexbox, grid, animations, SVG, Canvas, WebGL) for free, and - uniquely - reaches mobile through the same source.

Speed and adoption are the north stars definately. Webview is the only option that doesn't tank one of them.

## Goals

1. **Dragon-first.** Views, state, business logic stay in `.dr`. The webview is a real browser - drop in `<script>` for Monaco, Pixi, Yjs, or anything else when you need it; stay 100% Dragon when you don't. No mandatory JS build step, no required `npm`.
2. **One codebase, five targets** (Linux, macOS, Windows, iOS, Android). Platform-specific code stays in the shells (~600 LOC each) and `ui/native/`.
3. **`template { }` is the view DSL** - no new view syntax. The compile-time guarantees from (typo'd interpolations are build errors, not runtime blanks) extend to UI code.
4. **Small single binaries** - desktop apps ~10MB (vs Electron's ~150MB), because we use the OS webview rather than bundling Chromium.
5. **Reactivity like Flutter/Solid** - `Signal` + `Watch`, no VDOM library.
6. **Native escapes** - menus, dialogs, notifications, system tray, and hardware APIs go through native code, not webview shims, because users feel the difference.
7. **`window.dr` is the only Dragon JS API** - small, locked, documented. Everything else is normal web stuff.

## Non-Goals (v1)

- **Pixel-perfect native mobile chrome.** Webview-rendered HTML is "good," not "iOS HIG perfect." For 90% of apps this is fine; teams that need it should wait for a future native-widget backend or pick Flutter.
- **Bundling a JS toolchain or framework.** Dragon ships no `tsc`, no esbuild, no React/Vue/Solid. Users who want a bundler or framework install one themselves and drop the output in `assets/` - the toolkit doesn't care.
- **Running a JS engine in the host process.** Dragon code runs natively. The webview's JS engine is the OS's, not ours.
- **DOM-diffing virtual DOM.** v1 uses **targeted node patches** - a minimal op on the exact signal-bound node (see Locked v1 Scope) - not VDOM diffing and not innerHTML-replace. A morphdom-style diff is explicitly *not* needed given fine-grained signals.
- **Hot module reload.** Useful, but not blocking for v1. Restart-on-edit is acceptable for the first milestones.

## Options Considered

### Option A - Wrap Qt via FFI

Mature, native widgets, perfect a11y/HiDPI/IME. But:
- LGPL forces dynamic-link constraints, or a commercial license (~$5500/yr/seat).
- Thousands of classes to bind; MOC preprocessor doesn't fit Dragon's compile model.
- QML exists as Qt's declarative DSL but it's another language users must learn.
- Mobile targets are awkward; Qt for Android/iOS is genuinely behind React Native.
- Worst part: ignores `template { }` entirely.

Kept open as a **future second backend** for users who need true-native a11y in regulated industries.

### Option B - Bundle Skia + own scenegraph

Flutter's path. Total control, GPU-composited animations, consistent across platforms. But:
- Multi-quarter project: rebuild accessibility tree, IME composition, HiDPI, RTL, native menus, IME picker integration, screen readers.
- Doesn't use `template { }`.
- Adoption story is "learn Dragon's bespoke widget vocabulary," not "use what you already know."

### Option C - `template { }` + system WebView (chosen)

- ~600 LOC C++ shell per platform.
- Webview is hardware-accelerated and modern (WebKit / Chromium-derived) on every target.
- iOS App Store rules permit `WKWebView` (forbid bundled JS engines) - we comply.
- Web platform gives flexbox, grid, CSS animations, SVG, Canvas, WebGL, accessibility tree, IME, screen reader support, RTL, all for free.
- Same source reaches mobile.
- `template { }` is already HTML-shaped; users write the views they'd write anyway.

### Option D - Electron

~150MB binaries, 200MB+ RAM at idle, V8 in the host process. Defeats Dragon's speed pitch. Bundling Chromium is the wrong direction.

### Option E - Tauri-style (Rust shell + bring-your-own-frontend) (architecture adopted, default reframed)

Tauri's webview-as-renderer architecture is correct, and we keep its runtime shape. What we change is the *default*: Tauri requires you to choose and configure a JS framework (React, Vue, Solid, …) before you can do anything. Dragon makes `template { }` the default - Dragon-only apps work zero-config - while keeping the webview a real browser, so apps that *want* a JS framework or library (Monaco, Pixi, Yjs) just drop `<script>` tags in. Both paths are first-class; neither is mandatory. (Most apps won't touch JS at all.)

## Decision

**Ship `stdlib/ui/`: `template { }` views, OS webview renderer, `Signal`/`Watch`/`App`-owned reactivity, `ui/native/` for platform APIs, thin C++ shells per target.**

## Module Layout

```
stdlib/ui/
 ui.dr # public API: App, Signal, Watch, Computed, Window helpers
 desktop/
 desktop.dr # Window, Menu, Dialog, Notification, system tray, multi-window orchestration
 linux.dr # WebKitGTK shell - gtk_application_new, webkit_web_view_new
 macos.dr # WKWebView in NSWindow via Cocoa
 windows.dr # WebView2 via COM
 mobile/
 mobile.dr # lifecycle (onPause/onResume), navigation stack, safe-area insets, orientation
 ios.dr # WKWebView in UIViewController
 android.dr # WebView in Activity (JNI)
 native/ # shared plugin layer - same Dragon API, per-platform impl
 camera.dr
 gps.dr
 notification.dr
 biometrics.dr
 filepicker.dr
 share.dr
 bridge/ # internal - not user-facing
 bridge.dr # JSON IPC, callback registry
 reactivity.dr # signal subscription → re-render plumbing
 rerender.dr # template-to-patch (targeted node patches + keyed list reconcile)
```

Imports follow Dragon's existing dotted-import convention (`from os.path import join` precedent):

```dragon
from ui import App, Signal, Watch, Computed
from ui.desktop import Window, Menu, Dialog
from ui.mobile import Navigator, SafeArea
from ui.native import Camera, GPS, Notification
```

## Architecture

```
┌─────────────────────────────────────────┐
│ Dragon process │
│ ┌───────────────────────────────────┐ │
│ │ User code │ │
│ │ - View classes │ │
│ │ - Signal(app,T) / Watch(app,fn) │ │
│ │ - template { } blocks │ │
│ └────────────┬──────────────────────┘ │
│ │ │
│ ┌────────────▼──────────────────────┐ │
│ │ ui/ stdlib (Dragon) │ │
│ │ - Signal subscription bookkeeping │ │
│ │ - Callback registry (id → fn) │ │
│ │ - Re-render scheduler │ │
│ └────────────┬──────────────────────┘ │
│ │ │
│ ┌────────────▼──────────────────────┐ │
│ │ Native shell (C++, per platform) │ │ ~600 LOC
│ │ - Webview lifecycle │ │
│ │ - JSON IPC pump │ │
│ │ - Native menus / dialogs │ │
│ └────────────┬──────────────────────┘ │
└───────────────┼─────────────────────────┘
 │ JSON over webview IPC
┌───────────────▼─────────────────────────┐
│ Platform WebView (a real browser) │
│ ┌───────────────────────────────────┐ │
│ │ Rendered HTML from template { } │ │
│ │ + any user <script> (Monaco, │ │
│ │ Pixi, Yjs, vanilla - anything) │ │
│ │ + locked ~2KB shim: │ │
│ │ window.dr = { │ │
│ │ invoke(id, ...args), │ │
│ │ signals.<id>.{get,set, │ │
│ │ subscribe}, │ │
│ │ on(event, cb), │ │
│ │ } // frozen, non-writable │ │
│ └───────────────────────────────────┘ │
└─────────────────────────────────────────┘
```

### The bridge protocol

Two message kinds:

- **JS → Dragon (events)**
 ```json
 {"kind": "invoke", "cb_id": "cb_7", "args": [{"value": "hello"}]}
 ```
 The native shell forwards this to the Dragon-side callback registry, which calls the registered closure with the deserialized args.

- **Dragon → JS (re-renders)** - **targeted node patches** (see Locked v1 Scope), one minimal op per changed binding:
 ```json
 {"op": "text", "id": "n7", "value": "42"}
 ```
 The shim applies it directly (`textContent` / `setAttribute` / keyed `replaceChildren`) to the exact node - no innerHTML-replace, no re-parse, focus/scroll/selection preserved.

The `window.dr` shim is injected as a *user script at document start* - it runs before any user page script. The binding is locked via `Object.defineProperty(window, 'dr', { writable: false, configurable: false })` and the object itself is `Object.freeze`'d, so user JS cannot overwrite or monkey-patch it. Private state (callback registry, pending message queue) lives in IIFE closure scope, unreachable from page code. Total runtime JS surface: ~150 lines.

### Compile-time event-handler extraction

When the parser encounters a Dragon callable inside an event-attribute interpolation:

```dragon
template { <button onclick=!{self.inc}>+</button> }
```

CodeGen extracts the handler at compile time, registers it in a per-window callback table at runtime startup, and emits HTML like:

```html
<button onclick="window.dr.invoke('cb_7')">+</button>
```

This is the same compile-time guarantee `template { }` already gives for value interpolations - handlers that don't exist or have the wrong arity are build errors.

### Reactivity primitives

Three types in `ui.dr`, all created against the explicit `App` (see *Reactivity and lifecycle* above):

- `Signal[T]` - a refcounted reactive cell with subscriber tracking. Built with `Signal(app, initial)`; the binding annotation pins `T` (`count: Signal[int] = Signal(app, 0)`, the `Box[int] = Box(99)` generic-ctor rule). Read with `s`, written with `s.set(v)`.
- `Watch` - built with `Watch(app, fn)`. Runs `fn` immediately, tracks every signal read during the run, and re-runs whenever any tracked signal changes. A holdable handle with `.stop` for early teardown.
- `Computed[T]` - built with `Computed(app, fn)`; a derived reactive value, read with `c`.

The window document is **`Window.body: HTML`** (a `@property`/`@body.setter`, not `set_html(str)`) - consistent with the web framework's `res.body`/`res.headers` - and views are typed **`HTML`** (`template[HTML] { ... }`), never `str`, so escaping and component composition follow the type. `Window` takes its owning app: `Window(app, title, w, h)`.

When a signal interpolated in a `template { }` block changes, the bound node re-renders via a targeted patch (the smallest containing subtree for a conditional/loop swap).

These are plain Dragon classes - no compiler magic - built on top of closures and the existing refcount GC. The implementation is a few hundred lines of `.dr`.

### Concurrency cooperation

`fire`-spawned vthreads can update signals while the UI remains responsive. **Concrete model (see Locked v1 Scope):** the webview owns the OS main thread and Dragon's epoll/kqueue reactor is hosted *as a source on that platform runloop*, so `fire`/`await`/I/O run cooperatively on it. `thread { }` handles CPU-bound work off-thread; a single new `run_on_ui(closure)` primitive marshals DOM-affecting writes from non-UI carriers back to the UI thread, and `Signal.set` invokes it automatically when not on the UI thread. No new scheduler - everything else is .

## API Surface (developer-facing)

### Hello world

```dragon
from ui import App, Window
from html import HTML

app: App = App

win: Window = Window(app, "Hello", 400, 300)
win.body = template[HTML] {
 <div style="padding: 2rem; font-family: system-ui">
 <h1>Hello from Dragon</h1>
 </div>
}
app.run
```

### State + events

```dragon
from ui import App, Signal, Window
from html import HTML

app: App = App
count: Signal[int] = Signal(app, 0)

def inc -> None { count.set(count + 1) }
def dec -> None { count.set(count - 1) }

win: Window = Window(app, "Counter", 360, 240)
win.body = template[HTML] {
 <div>
 <h1>Count: !{count}</h1>
 <button onclick="!{inc}">+</button>
 <button onclick="!{dec}">-</button>
 </div>
}
app.run
```

### Loops, conditionals

```dragon
def todo_view(items: list[str]) -> Template:
 return template {
 <ul>
 !{for item in items: template { <li>!{item | html}</li> }}
 </ul>
 !{if len(items) == 0: template { <p>No items yet</p> }}
 }
```

This works without any new syntax - `template { }` already composes, and the `html` filter prevents injection.

### Native escapes

```dragon
let menu: desktop.Menu = desktop.Menu.bar([
 desktop.Menu("File", [
 desktop.MenuItem("Open...", shortcut="Cmd+O", action=on_open),
 desktop.MenuItem.separator,
 desktop.MenuItem("Quit", shortcut="Cmd+Q", action=ui.App.quit),
 ]),
])
ui.App.set_menu(menu)

let path: str | None = desktop.Dialog.open_file(filters=["*.dr"])
desktop.Notification(title="Build done", body="Compiled in 1.2s").show
```

### Mobile

```dragon
import ui.mobile as mobile
import ui.native as native

class CameraScreen(mobile.Screen):
 photo: ui.Signal[bytes | None]

 def(self):
 self.photo = ui.signal(None)

 def view(self) -> Template:
 return template {
 <div class="screen">
 !{if self.photo is None:
 template { <button onclick=!{self.snap}>Take Photo</button> }
 else:
 template { <img src="!{self.photo | data_url}" /> }}
 </div>
 }

 def snap(self) -> None:
 fire self.do_snap

 def do_snap(self) -> None:
 let cam: native.Camera = native.Camera
 let img: bytes = await cam.capture
 self.photo.set(img)
```

The same `template { }` + `Signal` pattern works on mobile. The only mobile-specific imports are `ui.mobile` (lifecycle, navigation) and `ui.native` (hardware).

## Dialogs and Dynamic Behavior

Where does JS fit? Three layers:

| Layer | JS engine present? | JS we bundle? | JS the user can write |
|---|---|---|---|
| **Dragon host process** | None (no V8, no Node) | None | N/A - host is pure native code |
| **Webview renderer** | Yes - the OS provides one (WebKit on macOS/iOS/Linux, Chromium on Win/Android) | None - we use the OS's | Anything they want - `<script>` works (CDN, local file, inline) |
| **`window.dr` shim** | Runs inside the webview's JS engine | ~2KB, locked, ships with the toolkit | Read-only - users call it, never overwrite it |

The Dragon process is pure native code. The webview is a real browser, so any JS the user wants - Monaco, Pixi, Yjs, vanilla, a hand-rolled drag library - drops in via `<script>`. The toolkit's only required JS is the locked `window.dr` shim and the inline event handlers our compiler emits (`onclick="window.dr.invoke('cb_7')"`). Most apps need zero user JS; some apps need a lot. Both are first-class.

### Dialogs

Dialogs split into two categories. Neither requires user JS.

**Native OS dialogs** are real OS dialogs. They never touch the webview at all. Dragon calls FFI into the native shell, which calls the platform API directly:

| Dragon API | macOS | Windows | Linux |
|---|---|---|---|
| `Dialog.open_file` | `NSOpenPanel` | `IFileOpenDialog` | `GtkFileChooserDialog` |
| `Dialog.save_file` | `NSSavePanel` | `IFileSaveDialog` | `GtkFileChooserDialog` |
| `Dialog.alert(msg)` | `NSAlert` | `MessageBox` | `GtkMessageDialog` |
| `Dialog.confirm(msg)` | `NSAlert` w/ buttons | `MessageBox MB_YESNO` | `GtkMessageDialog` |
| `Dialog.color_picker` | `NSColorPanel` | `ChooseColor` | `GtkColorChooserDialog` |

```dragon
let path: str | None = desktop.Dialog.open_file(filters=["*.dr", "*.py"])
if path is not None:
 let ok: bool = desktop.Dialog.confirm("Open " + path + "?")
 if ok:
 load(path)
```

The dialog is real, native, and accessible - it looks like the OS's dialog because it *is* the OS's dialog. This is the right behavior: users expect the file picker to match their operating system, not an HTML facsimile.

**In-app modals** are branded dialogs styled to match the app - confirmation prompts, settings panels, custom date pickers. These are conditional `template { }` blocks driven by a signal:

```dragon
class TodoApp:
 items: ui.Signal[list[str]]
 delete_target: ui.Signal[int | None]

 def view(self) -> Template:
 return template {
 <ul>!{for i, item in enumerate(self.items): template {
 <li>!{item | html} <button onclick=!{lambda: self.ask_delete(i)}>x</button></li>
 }}</ul>

 !{if self.delete_target is not None: template {
 <div class="backdrop">
 <div class="modal">
 <p>Delete this item?</p>
 <button onclick=!{self.cancel}>Cancel</button>
 <button onclick=!{self.confirm_delete}>Delete</button>
 </div>
 </div>
 }}
 }

 def ask_delete(self, i: int) -> None: self.delete_target.set(i)
 def cancel(self) -> None: self.delete_target.set(None)
 def confirm_delete(self) -> None:
 let i: int = self.delete_target
 self.items.set([x for j, x in enumerate(self.items) if j != i])
 self.delete_target.set(None)
```

The flow is invariant: DOM event → bridge → Dragon callback → signal update → re-render. CSS handles backdrop, blur, and animation. The HTML `<dialog>` element works too if you want ESC-to-close and focus-trap for free.

### Behaviors that don't *require* JS

| Behavior | Mechanism without JS | User JS required? |
|---|---|---|
| Form validation | `oninput=!{self.validate}` → Dragon callback updates error signal | No |
| Tooltips | CSS `:hover` | No |
| Drag and drop | HTML5 `ondragstart` / `ondrop` events forwarded to Dragon | No |
| Keyboard shortcuts | `onkeydown=!{self.handle}` on focused element, or native menu accelerators | No |
| Toast / snackbar | Signal flips true → CSS animation → Dragon timer flips it back | No |
| Animations / transitions | CSS `transition` / `@keyframes` | No |
| Routing (mobile screen stack) | `Navigator` is a Dragon class with a signal stack; templates re-render based on it | No |
| Modal focus trap, ESC-to-close | HTML `<dialog>` element handles it natively | No |
| Charts / data visualization | SVG generated by `template { }`, or `<canvas>` driven by Dragon callbacks | No |

Pattern: **DOM event → bridge → Dragon callback → signal update → patch**. Same mental model as React/Solid, Dragon owns the controller. If a specific app wants JS for any of these (a hand-tuned drag library, a charting library you already love, a polished gesture-recognition module), drop in a `<script>` - see the next section.

## Using JS Libraries

The webview is a real browser. Anything that runs on a web page runs here - Monaco, CodeMirror, Pixi.js, Three.js, Yjs, ProseMirror, D3, AG Grid, vanilla JS, your own bundle. There is no plugin manifest, no `extern js` declaration, no toolchain to learn. You write `<script>` and it works.

### Loading scripts

```dragon
def view -> Template:
 return template {
 <div id="editor" style="height: 600px"></div>

 <!-- CDN -->
 <script src="https://cdn.jsdelivr.net/npm/monaco-editor@0.45.0/min/vs/loader.js"></script>

 <!-- local file from assets/ (bundled into the binary at build time) -->
 <script src="/assets/canvas-engine.js"></script>

 <!-- inline -->
 <script>
 console.log("hello from inside the webview");
 </script>
 }
```

The `assets/` directory is bundled into the executable at `dr build` and served **in-process by a per-platform `app://` scheme handler** (`WKURLSchemeHandler` / WebView2 `WebResourceRequested` / WebKitGTK `register_uri_scheme`) - no localhost server, no open port (see Locked v1 Scope). Users who want TypeScript or a bundler run them themselves and drop the output in `assets/`.

### The `window.dr` API

The only Dragon-specific JS surface. Locked, frozen, can't be overwritten:

```javascript
// Call a Dragon function exposed via @exposed decorator or onclick=!{handler}
window.dr.invoke(handler_id, ...args) -> Promise<any>

// Read/write/subscribe to a Dragon signal marked `shared`
window.dr.signals[name].get
window.dr.signals[name].set(value)
window.dr.signals[name].subscribe(cb) // returns unsubscribe fn

// Lifecycle hooks
window.dr.on('ready', cb) // shim + initial render done
window.dr.on('beforeunload', cb) // window closing
```

That's it. ~150 lines of shim code, documented and stable.

### Worked example: Monaco

```dragon
class CodeView(View):
 code: SharedSignal[str] // exposed to JS

 def(self):
 self.code = shared_signal("print('hello')", name="code")

 def view(self) -> Template:
 return template {
 <div id="editor" style="height: 600px"></div>
 <script src="https://cdn.jsdelivr.net/npm/monaco-editor@0.45.0/min/vs/loader.js"></script>
 <script>
 require.config({ paths: { vs: 'https://cdn.jsdelivr.net/npm/monaco-editor@0.45.0/min/vs' }});
 require(['vs/editor/editor.main'], => {
 const dr = window.dr;
 const editor = monaco.editor.create(document.getElementById('editor'), {
 value: dr.signals.code.get,
 language: 'python',
 });
 editor.onDidChangeModelContent(=> dr.signals.code.set(editor.getValue));
 dr.signals.code.subscribe(v => {
 if (v !== editor.getValue) editor.setValue(v);
 });
 });
 </script>
 }
```

Monaco loads from CDN, reads/writes the Dragon-side `code` signal through `window.dr.signals.code`, and the rest of the app sees a normal Dragon `Signal[str]`. No `extern js`, no `.d.ts`, no plugin pipeline.

### `SharedSignal[T]` - the high-frequency hot path

For interactions that must not pay bridge round-trip cost on every event (drag, scroll, draw, pan/zoom), declare the signal as **shared**:

```dragon
let cursor: SharedSignal[Point] = shared_signal(Point(0, 0), name="cursor")
```

A shared signal is mirrored in `window.dr.signals.cursor`. JS can read and write it locally at 120Hz with **no bridge crossing per event** - only Dragon-side `effect`/`subscribe` callbacks fire on a coalesced cadence (debounced by frame, batched per tick). This is what makes Miro-class apps viable: gestures and per-frame rendering live in the webview's JS engine; Dragon sees a coherent reactive value on the cold path.

### Reaching back into Dragon

Mark a Dragon function `@exposed` to make it callable from JS via `window.dr.invoke('name', ...)`:

```dragon
@exposed
def save_board(json: str) -> bool:
 File.write("board.json", json)
 return True
```

```javascript
const ok = await window.dr.invoke('save_board', JSON.stringify(state));
```

Args are JSON-serialized; return values are JSON-deserialized.

## Phased Implementation

> **Superseded by the "Revised v1 phase plan" in Locked v1 Scope above** (desktop-trio v1 with the full reactive + native-chrome set). The original proposal phases below are retained for context and detail.

### Phase 0 - Linux desktop proof of concept (target: 2-3 weeks)

- `ui/bridge/bridge.dr` - JSON IPC, callback registry
- `ui/ui.dr` - `App`, `Signal`, `Watch`, `Computed`, `Window` helpers
- `ui/desktop/linux.dr` + C++ shell - WebKitGTK lifecycle, IPC pump
- `ui/desktop/desktop.dr` - `Window` (single-window only), no menus/dialogs yet
- E2E test: counter app runs, increments on click, re-renders correctly
- Ship a `dr ui-demo` CLI shortcut that builds and runs an example

**Exit criteria:** counter, todo list, and a simple form work end-to-end on Ubuntu. Binary size under 12MB. First-paint under 200ms.

### Phase 1 - macOS desktop (target: 2 weeks after Phase 0)

- `ui/desktop/macos.dr` + Cocoa shell - `WKWebView` in `NSWindow`
- Native menu bar (`Menu.bar([...])`)
- Native file/save dialogs, alerts
- `Notification` via `UNUserNotificationCenter`

**Exit criteria:** Phase 0 examples run unchanged on macOS with native menubar.

### Phase 2 - Windows desktop (target: 2 weeks after Phase 1)

- `ui/desktop/windows.dr` + Win32/COM shell - WebView2
- Native menus via `HMENU`
- File dialogs via `IFileDialog`
- Toast notifications via `WinRT`

**Exit criteria:** Phase 0 examples run unchanged on Windows 10+.

### Phase 3 - Reactivity polish + multi-window (target: 2 weeks)

- `effect` dependency tracking refinement; subtree-scoped re-render (not full window)
- Multi-window apps: `Window` returns a fresh window; lifecycle hooks
- `computed` derived signals
- Batched updates within a single tick

**Exit criteria:** complex demo (file manager mock with 3 windows + sidebar + tabs) renders smoothly.

### Phase 4 - Native plugin layer v1 (target: 3 weeks)

- `ui/native/filepicker.dr` (already partially in desktop)
- `ui/native/notification.dr`
- `ui/native/share.dr`
- `ui/native/clipboard.dr`
- Each plugin: shared Dragon API, three platform impls

**Exit criteria:** plugins work on all three desktop platforms with a single user-facing API.

### Phase 5 - iOS shell (target: 4 weeks)

- `ui/mobile/ios.dr` + Objective-C/Swift shell - `WKWebView` in `UIViewController`
- `dr build --target=ios` integrates with Xcode toolchain (`xcodebuild`, codesign, provisioning)
- `ui/mobile/mobile.dr` lifecycle hooks
- Safe-area insets, status bar, orientation

**Exit criteria:** counter app builds as `.ipa`, runs on iOS Simulator and a real device, passes basic App Store guidelines check.

### Phase 6 - Android shell (target: 4 weeks)

- `ui/mobile/android.dr` + JNI shell - `WebView` in `Activity`
- `dr build --target=android` integrates with Gradle, AAB packaging
- Hardware back button, Activity lifecycle

**Exit criteria:** counter app builds as `.aab`, runs on Android emulator and a real device.

### Phase 7 - Mobile native plugins (target: 4 weeks)

- `ui/native/camera.dr` - `AVCaptureSession` (iOS), `CameraX` (Android)
- `ui/native/gps.dr` - `CLLocationManager` (iOS), `FusedLocationProvider` (Android)
- `ui/native/biometrics.dr` - `LocalAuthentication` (iOS), `BiometricPrompt` (Android)
- `ui/native/push.dr` - APNs (iOS), FCM (Android)

**Exit criteria:** photo-capture demo works on both mobile platforms.

### Phase 8 - DOM diffing + hot reload (target: 3 weeks, optional)

> **Obsoleted by Locked v1 Scope.** v1 uses targeted node patches from day one, so there is no innerHTML re-render to "replace with morphdom" - the diffing item is dropped. Hot reload is pulled forward into v1 (Phase 5 of the revised plan).

- ~~Replace innerHTML re-render with morphdom-style diff~~ - N/A; targeted patches are the v1 mechanism.
- File watcher → recompile changed templates → push patch over IPC without restart (now v1)
- Dev-only; production builds keep static assets

**Exit criteria:** edit `.dr` file, see UI update in <500ms without losing state.

## Performance Considerations

- **First paint**: webview boot is ~100-300ms cold. Mitigate with native splash on mobile and a same-process pre-warm on desktop.
- **Re-render cost**: targeted node patches are O(1) per changed binding (one `textContent`/`setAttribute`), not O(subtree). Heavy lists use keyed reconcile + windowing/virtualization (a `<For>` component pattern from Solid is the right shape).
- **Bridge overhead**: JSON serialize + IPC + parse is ~20-100µs per message. Fine for events; batching needed for high-frequency updates (mouse move, scroll).
- **Memory**: native shell ~5MB, OS webview ~30-60MB resident. Total ~50MB idle, comparable to a native Qt app and ~4x smaller than Electron.
- **No V8 in the host process** - Dragon code runs natively. The webview's JS engine handles DOM rendering, the locked `window.dr` shim, and any user JS the app loads via `<script>`.

Targets for now. I'll measure in Phase 8.

## Security Considerations

- **CSP is configurable per `Window`.** Default permits scripts from the app's own bundle (`script-src 'self' 'unsafe-inline'`). Users who load remote libaries (e.g. Monaco from a CDN) extend CSP via `Window(csp="script-src 'self' 'unsafe-inline' https://cdn.jsdelivr.net")`. Default fails closed: no remote XHR, no remote scripts unless allow-listed.
- **`window.dr` is locked.** Defined via `Object.defineProperty` with `writable: false, configurable: false` and `Object.freeze`'d members. User JS - accidental or intentional - cannot overwrite it. Private state (callback registry, message queue) lives in IIFE closure scope.
- **`html` pipe filter is the default for user-controlled values** - 's existing escaping prevents XSS. Documentation must make this clear: `!{user_data}` raw is unsafe; `!{user_data | html}` is the correct form.
- **Bridge invocations are origin-checked** - only `window.dr.invoke` calls from the loaded document fire callbacks. External navigation (clicking a link to a third-party site) is intercepted and either blocked or opened in the system browser.
- **`@exposed` is opt-in.** Only Dragon functions explicitly marked `@exposed` are callable from JS. Everything else is invisible to the webview.
- **Native plugins gate-keep permissions** - camera/GPS/etc. go through OS permission prompts, not Dragon-side checks.

## Open Questions

1. **Should `template { }` gain a JSX-like compile-time component-call syntax?** e.g. `<Counter count=!{n} />` resolving to a `Counter.view` call. v1 says no - composition is `!{counter.view}` and that's fine. Revisit if real apps demand it.

2. **Reactive scope granularity.** v1 re-renders the smallest *template subtree* containing a changed signal. Solid does fine-grained DOM updates (per-attribute). The latter is more complex but yields better performance for high-frequency updates. Decide post-Phase 3 with benchmarks.

3. **Style scoping.** Should user `<style>` blocks inside `template { }` be auto-scoped (Vue SFC-style) or global? v1: global. Component-scoped CSS is a Phase 8+ concern.

4. **Server-side rendering.** Same `template { }` already produces an HTML string. A `ui.web` backend that mounts the same `View` classes on the HTTP server is a near-trivial extension. Out of scope for this decision but explicitly enabled by the design.

5. **Accessibility tree.** Webview a11y "just works" if templates use semantic HTML. We should ship lint rules that flag `<div onclick>` (should be `<button>`) and missing `aria-label` on interactive elements.

## Why I'm betting on webview-first

### Positive

- One ADR covers desktop and mobile UI.
- `template { }` becomes the most reused primitive in the language - same syntax for HTML responses, for desktop UIs, for mobile UIs, for SSR.
- Pitch: if you know HTML, you can ship today. Huge audience.
- Single binary, ~10MB desktop, ~15MB mobile (with the OS webview already on-device).
- No mandatory JS toolchain - Dragon-only apps need zero JS - and yet the webview is a real browser, so apps that need Monaco / Pixi / Yjs / anything else just `<script>` it in. No "escape hatch" framing; using JS libraries is a first-class capability.
- Reactivity primitives (`Signal`, `Watch`) are reusable outside UI - they're useful for any observable-state pattern.

### Negative

- Webview rendering is "good," not "iOS HIG perfect" or "Material You perfect." Apps that need pixel-native mobile feel will not be satisfied; they should wait for a future native-widget backend or pick Flutter.
- Heavy graphical apps (DAWs, video editors, 3D modelers) are not the target. Use Canvas/WebGL or pick a different tool.
- Adds two new build pipelines (Xcode for iOS, Gradle for Android) - `dr build` complexity grows.
- Native plugin layer is open-ended; we'll be writing camera/GPS/biometric bindings for years.

### Neutral

- The decision to use the OS webview means we're at the mercy of OS webview update cadence (mostly evergreen, but old Linux distros and old Android devices will lag). Document the supported floor: macOS 11+, Windows 10+, iOS 14+, Android 7+ (WebView 64+), Ubuntu 20.04+.

## Future Work

- **`ui.qt` backend** - for users who need true-native a11y. Implements the same `Window`/`View`/`Signal` API behind a Qt FFI binding. Selected via build flag.
- **`ui.terminal` backend** - same `View`/`Signal` API rendered to a TUI via existing Dragon terminal stdlib. Useful for SSH-only environments and for tools.
- **`ui.web` backend** - server-side rendering of the same templates, integrated with `http` .
- **DevTools extension** - a built-in inspector that shows the signal graph and re-render trace. Powered by the same bridge.
- **Theming system** - a `Theme` primitive that exposes design tokens as CSS custom properties, automatically dark-mode-aware via system preferences.

---

Desktop, mobile, and native plugins would've been three separate projects. This keeps them on one track. Bet: **`template { }` + system webview** stays the right default for years, with native-widget and Skia paths kept available as additional backends behind the same API.
