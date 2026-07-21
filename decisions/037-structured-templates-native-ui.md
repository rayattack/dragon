# Decision 037: Structured Templates and Native UI Backends

**Status:** Approved. desktop strategy locked. Native widgets are the **default** desktop backend; webview is the **deliberately-labeled opt-in slow path**, whole-app (`--backend=webview`) and as an embeddable `WebView` widget inside native trees. Implementation not started; build order is **IR → webview → native**.

**Builds toward:** the `ui.qt` / native-widget future-work slot promised in D031 §Future Work

D031 commits me to webview for v1 - right call for time-to-market. But I kept staring at the per-event cost table (~1.5-5 ms webview vs ~0.1-0.5 ms native) and couldn't un-see it. Structured templates (IR trees instead of string concat) are the bridge: same `template { }` syntax, different codegen, feeds native backends without rewriting the UI layer.

## Summary

Two-part decision:

1. **Structured templates (IR primitive).** Extend with a `StructTemplate(Template)` base class. When `template[X] { ... }` has `X` deriving from `StructTemplate`, the compiler lowers the markup body to a chain of constructor calls instead of `dragon_str_concat`. Source syntax unchanged from Phase 4 - only codegen differs. Output is a tree of Dragon objects, not a string.

2. **Native UI backends (consumers).** Use that IR for native-rendering backends alongside 's webview, behind the same `Window` / `View` / `Signal` API. Selected at build time:
 - `dr build` / `dr build --backend=native` → (per-platform native widgets via FFI) - **default desktop backend** (commandment #1: the hot path)
 - `dr build --backend=webview` → (HTML into OS webview) - **deliberately-chosen slow path** (opt in when you want the web platform whole-app)
 - `dr build --backend=skia` →, far-future (own scenegraph, Flutter-style)

 Webview also survives *inside* native as a first-class `WebView`/`HtmlView` widget - embedded web island for Canvas/WebGL/Monaco/rich-text - so a native app reaches the web platform per-surface without leaving native. See Part 3 → "The `WebView` island widget."

The two parts are coupled (IR exists to feed backends) but ship in distinct phases. IR is useful on its own (SQL builders, AST construction, protobuf builders, etc.).

## Motivation

 commits Dragon to webview rendering for v1. Right call for time-to-market and target breadth, but per zen.md commandment #1 ("Speed… pick the fastest one - no exceptions") it's not the right *destination* (webview is fine for v1, just not where I stop). Concrete per-event cost on a small re-render:

| Stage | webview | Native widgets via |
|---|---|---|
| Event dispatch | JS handler → `window.dr.invoke` | native control callback (in-process) |
| Bridge crossing in | JSON serialize + IPC + parse: ~50µs | direct call: ~10ns |
| Re-render | regen HTML via `dragon_str_concat`: ~10µs | property store + invalidate: ~100ns |
| Bridge crossing out | JSON wrap + IPC: ~50µs | none |
| Render path | HTML parse → layout → paint | native layout → paint |
| **Per-event total** | **~1.5-5 ms** | **~0.1-0.5 ms** |

Memory: ~50 MB idle webview vs ~5-15 MB native. First paint: ~100-300 ms (cold webview boot) vs <50 ms.

**Hot-path / slow-path doctrine .** Commandment #1 forbids *forcing* slowness, not *offering* a slow path the developer knowingly chooses. Resolution: **native is the default** (~0.1-0.5 ms/event, 5-15 MB idle), **webview is the labeled slow path** - whole-app via `--backend=webview`, or per-surface via embedded `WebView`. A labeled opt-in slow path is fine under #1, same as offering `fire`/`thread`/`Thread` without forcing one.

So `SharedSignal[T]` is **reframed**, not deleted: it's how a deliberately-chosen web surface (whole-app webview, or embedded `WebView` island) keeps its 60-120 Hz hot loop local to JS. For a *native* app, hot paths are native - no mirror, no bridge.

 §Future Work already promises this slot (`ui.qt`). makes it concrete across platforms, with the IR change as the enabling primitive.

## Goals

1. **Same `template { }` syntax** drives string-mode and tree-mode . No new keywords, no JSX.
2. **Source-compatible UI code.** `View`/`Signal`/`template { }` source compiles unchanged against either backend; build flag picks lowering.
3. **Per-platform native rendering** on Apple (UIKit/AppKit), Android, Windows, Linux - without bundling a renderer.
4. **Single shared widget vocabulary.** `Widget` base + curated set (Stack, Grid, ZStack, Text, Button, Input, ScrollView, ListView, Image, **WebView**) mapping to each platform's native equivalents.
5. **Coexistence with .** Native and webview backends ship side-by-side. Apps choose per build target.
6. **Honest scope on Skia.** Multi-quarter work, deferred to v3. Native-widget bindings deliver most of the value at a fraction of the cost.

## Non-Goals (v1 of)

- **Skia/Flutter-style own renderer.** Mentioned for completeness; not in the first three phases.
- **Pixel-identical rendering across platforms.** Each platform looks like itself. Feature, not bug.
- **Auto-conversion of webview apps to native.** Shared source surface, but moving from `--backend=webview` to `--backend=native` may require dropping `<script>`-based features. Document the portable subset.
- **Full Win32 / GTK / UIKit / AppKit / Android API coverage.** v1 binds the curated `stdlib/ui/widget.dr` set. Beyond that: user-extensible via `Widget` protocol or direct FFI.

## Part 1 - Structured Templates IR

### The protocol

```python
class StructTemplate {
 # Maps tag name → constructor class. Read by the compiler at codegen time;
 # must be a literal-evaluable @staticmethod.
 @staticmethod
 def __tags__ -> dict[str, type] {
 return {}
 }

 # Optional: name of the children attribute on tag classes (default "children").
 __children_attr__: str = "children"
}
```

Each tag class is a regular Dragon class. Attributes → keyword args; nested elements → children list.

```python
# stdlib/ui/widget.dr
class Widget(StructTemplate) {
 @staticmethod
 def __tags__ -> dict[str, type] {
 return {
 "stack": Stack,
 "grid": Grid,
 "zstack": ZStack,
 "text": Text,
 "button": Button,
 "input": Input,
 "image": Image,
 "scroll": ScrollView,
 "list": ListView,
 "webview": WebView,
 }
 }
}

class Stack(Widget) {
 padding: int = 0
 children: list[Widget] = []
}

class Text(Widget) {
 content: str
 size: int = 14
}

class Button(Widget) {
 label: str
 on_press: -> None
}
```

### Source surface (identical to Phase 4)

```python
def login -> None { ... }

screen: Widget = template[Widget] {
 <stack padding=16>
 <text size=24>Welcome, !{username}</text>
 <input bind=!{username_var} />
 !{ if logged_in {
 :{ <button label="Sign out" on_press=!{logout} /> }
 } else {
 :{ <button label="Sign in" on_press=!{login} /> }
 }}
 </stack>
}
```

Two attribute forms:

| Position | Form | Lowering |
|---|---|---|
| `<text size=24>` | bare scalar | passed as integer literal |
| `<button label="Save">` | string literal | passed as `str` |
| `<button label="!{x}">` | string with `!{}` | concat then pass as `str`; struct mode applies `escape` if defined |
| `<button on_press=!{login}>` | bare `!{expr}` | typed value (callable, int, Widget, …); no escape |
| `<text>!{user_input}</text>` | child position | str → `Text(content=...)`; Widget → appended; list[Widget] → splatted |

### Block interpolation in struct mode

 Phase 4's `!{...}` block + `:{}` content alias reused verbatim. Only difference from string mode is buffer element type:

| Construct | string mode | struct mode |
|---|---|---|
| `!{expr}` in content | stringify + escape into `str` buffer | append `Widget` (with `str→Text` wrap) |
| `:{ ... }` fragment | nested string template | nested `Widget` subtree |
| `!{ for x in xs { :{ ... } } }` | block buffer is `str` | block buffer is `list[Widget]` |
| `!{ if c { :{ ... } else: { ... } } }` | branches append str | branches append children |
| `!{*xs}` / `!{xs \| join}` | string concat | list splat into children |

Detection rule from Phase 4 (`parseExpression` first, fall back to `parseBlock`) is **identical**. CodeGen branches on content type:

```
template[X] { body }
 │
 ▼
 Look up X
 │
 ├─ X derives from Template only → string mode (unchanged)
 ├─ X derives from StructTemplate → struct mode (new)
 └─ neither → compile error
```

### Lowered IR sketch

```python
btn: Widget = template[Widget] {
 <button label="Save" on_press=!{login} />
}
```

lowers to roughly:

```llvm
%btn = call ptr @Button.__new__
call void @Button.__set_label__(%btn, ptr @str.Save)
call void @Button.__set_on_press__(%btn, ptr @login)
```

Nested tree: children list built first, handed to parent constructor. One allocation per tag, N field stores per attribute. Same IR shape as hand-written `Stack(padding=16, children=[Text(content="Hello"), Text(content=name)])`.

Zero parsing, zero string concat, zero reflection. Same cost as direct construction. The JSX/SwiftUI/Compose trick, Dragon syntax.

### Effect on existing code

| What | Effect |
|---|---|
| `template { ... }` (untyped → str) | none |
| `template[HTML] { ... }`, `[SQL]`, etc. (typed string templates) | none - `Template` base unchanged |
| Phase 4 mechanics (`:{}`, block interp, `\| join`, `*` spread) | reused; only buffer element type differs in struct mode |
| User-defined `class FOO(Template)` | none - still string mode |
| v1 (webview) | none - emits `template[HTML]`, string mode |

### IR-only cost

| Component | Lines | Notes |
|---|---|---|
| AST | ~30 | tag-element node; StructTemplate flag on `TemplateExpr` |
| Parser | ~60 | extend body scanner for `<tag attrs>...</tag>` element tree |
| TypeChecker | ~80 | `StructTemplate` detection, `__tags__` resolution, attr type-check |
| CodeGen | ~150 | dual-mode dispatch; struct path emits alloc+store recursion |
| Tests | ~250 | IR + E2E for tree construction, type errors, composition |
| **IR-only total** | **~570** | |

IR ships independently. Phase 0 proves it without UI work.

## Part 2 - Native UI Backends

### Architecture

Same shape as, renderer slot = native shell instead of OS webview:

```
┌──────────────────────────────────────────────┐
│ Dragon process (single address space) │
│ │
│ user code: View / Signal / template[Widget] │
│ │ │
│ ▼ │
│ stdlib/ui/widget.dr - abstract base │
│ │ │
│ ▼ │
│ stdlib/ui/native/<platform>.dr │
│ (concrete subclasses; FFI thunks) │
│ │ │
│ ▼ │
│ Platform native runtime │
│ - libobjc + UIKit/AppKit (Apple) │
│ - JNI + android.view.* (Android) │
│ - user32/comctl32 / WinUI (Windows) │
│ - libgtk-4 / GObject (Linux) │
└──────────────────────────────────────────────┘
```

No IPC. No JSON. No second JS engine. Every event is an in-process function call.

### What each backend takes

#### `ui.uikit` (iOS) and `ui.appkit` (macOS) - shared Objective-C path

Share FFI layer; UI class set differs.

**FFI layer:**
- Link `libobjc.dylib`. Bind `objc_getClass`, `sel_registerName`, `objc_msgSend` family.
- ABI gotcha: on arm64, `objc_msgSend` is **not** one variadic function. Use `objc_msgSend` for normal returns, `objc_msgSend_stret` for struct returns >16 bytes, `objc_msgSend_fpret` for floating-point returns on x86_64. Codegen picks the right call from the bound method's return type. makes this possible - return types are known at codegen, not laundered through `i64`.
- Method binding: **eager** (Dragon `class UIButton` methods fan out via `objc_msgSend`) or **lazy** (stubs on first use). v1: eager.

**Class bindings (curated):**
- iOS: `UIView`, `UIViewController`, `UIWindow`, `UIStackView`, `UILabel`, `UIButton`, `UITextField`, `UIScrollView`, `UITableView`, `UIImageView`, `UINavigationController`.
- macOS: `NSView`, `NSWindow`, `NSStackView`, `NSTextField`, `NSButton`, `NSScrollView`, `NSTableView`, `NSImageView`.
- Layout: Auto Layout. Stack views handle most cases.

**Event bridging:**
- Target/action pattern. Register `DragonHandler` adapter Objective-C class at startup, single selector (`@selector(_dragonInvoke:)`), unpacks handler-id from `objc_associated_objects`, invokes Dragon callback. One objc class, not one per callback.
- Gesture recognizers same pattern.

**Memory bridge:**
- Platform widgets are Objective-C objects with ARC. Dragon `Button` holds `objc_id`; destructor emits `CFRelease(field)`; assignment `CFRetain(new); CFRelease(old)`.

**Threading:**
- UIKit main thread only. Green threads on worker pool. `dispatch_async(dispatch_get_main_queue, ...)` for UI mutation from vthreads.

**Lifecycle:**
- iOS: UIApplicationDelegate / UISceneDelegate; `App.run` → `UIApplicationMain`.
- macOS: NSApplicationDelegate; `App.run` → `[NSApp run]`.

**LOC estimate:** ~5 KLOC iOS+macOS (mostly shared)

#### `ui.android`

**FFI layer:**
- JNI: `JNI_CreateJavaVM` or `JNI_OnLoad` if loaded as `.so` from Java host. Bind `GetEnv`, `AttachCurrentThread`, `DetachCurrentThread`.
- `GetMethodID` + `CallObjectMethod` / `CallVoidMethod` / `Call<Type>Method`. helps pick the right call variant.
- Class loading via `FindClass` (cached).

**Class bindings:**
- `android.view.View`, `ViewGroup`, `LinearLayout`, `FrameLayout`, `android.widget.Button`, `TextView`, `EditText`, `ImageView`, `ScrollView`, `androidx.recyclerview.widget.RecyclerView`.
- Compose backend later - harder to FFI, benefits from Kotlin shim. v1: traditional Views.

**Event bridging:**
- Precompiled `.dex` (`DragonListener.dex`, ~50 lines Kotlin): `class DragonListener implements View.OnClickListener` with native `onClick(int handlerId)` → JNI → Dragon registry.

**Memory bridge:**
- JVM objects: Dragon holds JNI **global refs**. Destructor `DeleteGlobalRef`. Never store local refs.

**Threading:**
- UI thread = main looper. `Activity.runOnUiThread` / `Handler.post` from background. Green threads call `AttachCurrentThread` first; cache `JNIEnv*` in TLS (per-thread state).

**Lifecycle:**
- `Activity.onCreate/onResume/onPause/onDestroy` → Dragon hooks. Kotlin shim hosts Dragon process.
- Hardware back: `onBackPressed` → `Navigator.pop`.

**Build pipeline:**
- `dr build --target=android` → Gradle, packages `.so` + Kotlin shim + `.dex` into `.aab`.

**LOC estimate:** ~6 KLOC + 500 Kotlin

#### `ui.win32` (Windows)

**Path A - Win32 (v1):** HWND-per-widget, message pump (`GetMessage`/`TranslateMessage`/`DispatchMessage`). No extra deps.
**Path B - WinUI 3:** defer. Heavier FFI, WindowsAppSDK distribution.

**FFI layer (Path A):**
- Link `user32.dll`, `gdi32.dll`, `comctl32.dll`. `CreateWindowExW`, `DefWindowProcW`, `RegisterClassExW`, `SendMessageW`.
- Common controls via window class names (`"BUTTON"`, `"EDIT"`, `"SysListView32"`).

**Event bridging:**
- Subclass window proc via `SetWindowLongPtrW(GWLP_WNDPROC, ...)`. Route `WM_COMMAND`/`WM_NOTIFY` by control-id.

**Memory bridge:**
- Handles aren't refcounted. Destructor `DestroyWindow(hwnd)`.

**Threading:**
- GUI single-threaded per top-level window. `PostMessageW` with custom `WM_DRAGON_DISPATCH`.

**Message loop:**
- Main thread. libuv on Windows integrates Win32 message loop; event loop handles this.

**LOC estimate:** ~3.5 KLOC Windows v1

#### `ui.gtk` (Linux)

**FFI layer:**
- Link `libgtk-4.so`, `libgobject-2.0.so`. `gtk_application_new`, `gtk_window_new`, signal connect/disconnect.
- GObject introspection (`.gir`/`.typelib`) could autogenerate bindings - quarter of work. v1: hand-bind curated set (tedious but straightfoward).

**Class bindings:**
- `GtkApplication`, `GtkWindow`, `GtkBox`, `GtkLabel`, `GtkButton`, `GtkEntry`, `GtkScrolledWindow`, `GtkListBox`, `GtkImage`.

**Event bridging:**
- `g_signal_connect(button, "clicked", callback, user_data)` → Dragon trampoline unpacks handler id.

**Memory bridge:**
- `g_object_ref` / `g_object_unref`. Maps onto destructors.

**Threading:**
- GTK main thread; `g_idle_add` from any thread.

**Wayland vs X11:**
- GTK4 abstracts both. No Dragon-side branching.

**LOC estimate:** ~3.5 KLOC Linux

#### `ui.skia` (own scenegraph) - far-future

Honest scope: **multi-quarter project, comparable to early Flutter.** Skia = 2D drawing only. Dragon'd still need layout, hit testing, text shaping (HarfBuzz/ICU), animation, accessibility (~6 KLOC FFI per platform), IME, HiDPI, GPU integration. Skia binary ~30 MB. Estimated **10-30 KLOC** for a usable subset.

**Defer indefinitely.** Phases 1-6 deliver native rendering via existing toolkits at ~22 KLOC. Re-open Skia only if native widget paths can't satisfy a documented need at speed (probably won't happen, but leave the door open).

## Memory bridging - interaction with

Dragon `Widget` subclass holds a handle to an *external* runtime object:

| Backend | External runtime memory model | Dragon-side strategy |
|---|---|---|
| `ui.uikit` / `ui.appkit` | Objective-C ARC | `objc_id`; destructor `CFRelease`; assignment `CFRetain(new); CFRelease(old)` |
| `ui.android` | JVM tracing GC; Dragon holds global refs | `jobject` global ref; destructor `DeleteGlobalRef` |
| `ui.win32` | OS owns handles | `HWND`; destructor `DestroyWindow` |
| `ui.gtk` | GObject manual refcounting | `GObject*`; destructor `g_object_unref` |
| `ui.skia` (future) | Owned outright | Direct ownership |

 Phase 3 per-class destructors already support this. specializes them per platform. **No changes needed.**

Cycle collector (Phase 5) doesn't scan into external runtime - those objects are root-owned via `Widget` field. External runtime handles its own cycles.

## API surface - same as

User code unchanged: explicit `App` owns reactive graph, `Signal`/`Watch`/`Window` take owning `app` as visible argument, `app.run` is lifecycle boundary. Only content type differs - `template[Widget]` here vs `template[HTML]` on webview:

```dragon
from ui import App, Signal, Window
from ui.widget import Widget

app: App = App
count: Signal[int] = Signal(app, 0)

def inc -> None { count.set(count + 1) }
def dec -> None { count.set(count - 1) }

win: Window = Window(app, "Counter", 400, 300)
win.body = template[Widget] {
 <stack padding=16>
 <text size=24>Count: !{count}</text>
 <button label="+" on_press=!{inc} />
 <button label="-" on_press=!{dec} />
 </stack>
}
app.run
```

Build flag selects backend:

```bash
dr build app.dr # native (default), current desktop
dr build --target=linux app.dr # native Linux (GTK4)
dr build --target=macos app.dr # native macOS (AppKit)
dr build --target=windows app.dr # native Windows (Win32)
dr build --target=ios app.dr # native iOS (UIKit)
dr build --target=android app.dr # native Android
dr build --backend=webview app.dr # deliberate slow path (whole-app web)
dr build --backend=skia --target=ios app.dr # far-future
```

Portable UI stays in `template[Widget]` + curated widget set. Platform-specific widgets import platform modules directly:

```python
from ui.native.uikit import UIVisualEffectView # iOS-only; portable apps avoid
```

## Part 3 - Layout, styling, chrome, hardware

Cross-backend contracts after desktop-strategy lock. **Portable typed contract** on every backend, plus **honest escapes** where you knowingly leave portability (CSS passthrough on webview, `native_style` on native, direct platform imports).

### The `WebView` island widget

Every native toolkit ships a webview as a native widget:

| Backend | Webview-as-widget |
|---|---|
| `ui.gtk` (Linux) | `WebKitWebView` is a `GtkWidget` |
| `ui.appkit` / `ui.uikit` (Apple) | `WKWebView` is `NSView`/`UIView` subclass |
| `ui.win32` (Windows) | WebView2 parented to child `HWND` |
| `ui.android` | `android.webkit.WebView` in `ViewGroup` |

`WebView`/`HtmlView` is first-class in the curated set. Memory-bridge handle *is* the platform webview. Rest of tree is native (zero bridge cost); canvas/WebGL/Monaco/rich-text is a contained web island. Bridge tax (~30-50 MB RSS) only when you instantiate one.

```python
from ui.widget import WebView

<stack dir=col pad=12>
 <text size=18>Native shell</text>
 <button label="Run" on_press=!{self.run} />
 <webview src="app://editor.html" bind=!{self.source} grow=1 />
</stack>
```

`bind=!{signal}` two-way-binds Dragon state to JS inside the island. 60-120 Hz hot loop inside the island uses `SharedSignal` - correct for a deliberately-chosen web surface.

### Layout - typed flex / grid / z-stack (NOT CSS as the contract)

Cross-backend layout = **typed flexbox model** as widget properties, not CSS. CSS is how **webview backend realizes** it; native uses a **Dragon-side Yoga-style solver** computing final frames, native widgets as **leaves** (Flutter minus custom painting). Only approach that (a) is one fast layout pass, (b) is consistent across backends, (c) works on Win32 (no layout engine). Native containers still used where they earn it - `UIScrollView`/`RecyclerView`/`GtkScrolledWindow` for scroll virtualization.

Three layout widgets:

- **`Stack`** - 1-D flex. `dir = row | col`; `gap`, `pad` (`Edges`), `align`, `justify`, `wrap`; per-child `grow`/`shrink`/`basis`, `width`/`height`/`fill`.
- **`Grid`** - 2-D. `cols`/`rows` as `list[Track]` (`Track.px(n)` / `Track.fr(n)` / `Track.auto`), `gap`, optional `areas`.
- **`ZStack`** - z-axis overlay; per-child alignment.

On webview → `display:flex` / `display:grid`; on native → solver output.

### Styling - `Style` tokens + `Theme`, with passthrough escapes

1. **Portable `Style` tokens** - `bg`/`fg` (`Color`), `pad`/`margin` (`Edges`), `border`, `font`, `opacity`, `shadow`. `Theme` semantic tokens (`primary`, `surface`, `text`, …), dark-mode-aware.
2. **Passthrough escapes** - `css="…"` (webview-only), `native_style=!{…}` (native-only).

CSS-the-language never becomes the cross-backend contract.

### Native chrome - menus & navigation adapt to form factor

Portable layer = navigation *structure*; chrome *rendering* is platform-divergent (phone nav UI shouldn't look like desktop menu bar).

- **Desktop:** menu bar, OS dialogs, notifications, tray, clipboard.
- **Phone:** nav stack + tabs + drawer + app bar + hardware back. `Navigator.push(Screen)` / `.pop` / `.replace`. Android back auto-calls `Navigator.pop`.

### Hardware & native plugins

`ui.native` plugins, one colorless API, per-platform impls. Camera example:

```python
from ui.native import Camera
cam: Camera = Camera(device=Camera.default)
img: bytes = await cam.capture
```

Desktop webcam is native-backend (AVFoundation / Media Foundation / V4L2), no webview required. Inside `WebView` island or `--backend=webview`, path is `getUserMedia`.

## Phased plan

> **Ordering note .** Desktop native rollout: **GTK (Linux) → AppKit (macOS) → Win32 (Windows)**. Linux leads as dev platform; `g_object_ref`/`unref` maps 1:1 onto . iOS-first sequencing in Phases 2-6 below applies to *mobile* rollout after desktop. Cross-ADR order: **IR (Phase 0) → webview → native**. Reactive core built in, reused here.

### Phase 0 - Structured Templates IR (no UI deps)
- `StructTemplate` base, `__tags__` resolution, dual-mode codegen
- Tests with non-UI tag set (SQL builder, AST tree)
- ~570 LOC; ships independently

**Exit:** struct-mode tests pass; LLVM IR shows alloc-tree, no `dragon_str_concat`.

### Phase 1 - Widget abstract base and curated stdlib
- `Widget(StructTemplate)` in `stdlib/ui/widget.dr`
- Subclasses: Stack, Text, Button, Input, ScrollView, ListView, Image
- Pure data containers; re-render scheduler wired to `Signal`
- ~600 LOC

### Phase 2 - `ui.uikit` (iOS)
- objc FFI, UIKit bindings, `DragonHandler` adapter, main-thread dispatch
- `dr build --target=ios --backend=native`
- ~5 KLOC

**Exit:** counter app on Simulator + device, <0.5 ms/event, <15 MB idle.

### Phase 3 - `ui.appkit` (macOS)
- Reuses Phase 2 FFI. AppKit bindings, menu bar.
- ~1.5 KLOC

### Phase 4 - `ui.android`
- JNI shim, View bindings, Kotlin listeners, Activity host
- ~6 KLOC + 500 Kotlin

### Phase 5 - `ui.win32` (Windows)
- Win32 message pump, common controls
- ~3.5 KLOC

### Phase 6 - `ui.gtk` (Linux)
- GTK4 + GObject bindings
- ~3.5 KLOC

### Phase 7 - Native plugin parity with
- Camera, GPS, biometrics, file picker, share, notification
- Same plugin API as ; FFI instead of webview bridge

### Phase 8+ - `ui.skia` (deferred indefinitely)

## Trade-offs vs

| Concern | webview | native |
|---|---|---|
| Per-event latency | 1.5-5 ms | 0.1-0.5 ms |
| First paint | 100-300 ms (cold webview) | <50 ms |
| Memory | ~50 MB idle | ~5-15 MB idle |
| Binary size | ~10 MB (no webview bundled) | ~10-15 MB (no renderer bundled) |
| LOC to ship v1 | ~3 KLOC (single shell, three OSes) | ~22 KLOC across five backends |
| Time to v1 | weeks | quarters (per-backend phases) |
| A11y / IME / RTL | webview gives free | native widgets give free |
| CSS styling | yes | no - explicit `Widget` props |
| HTML/SVG/Canvas/WebGL | yes | yes - via embedded `WebView` island |
| Pixel-native feel | "good" | actual native |
| Same source on all targets | yes | yes (Widget vocabulary) |
| Pixel-identical across platforms | yes | **no** (native widgets look like the platform) |

 is the perf destination. ships first because it's faster to build. Both coexist permanently.

## Open questions

1. **Styling.** **Resolved.** Typed flex/grid + `Style`/`Theme` tokens; CSS on webview, solver + native appearance on native.
2. **Animation API.** Defer unified façade to post-Phase-6. v1 ships without animation primitives.
3. **`@exposed` analogue for native.** No-op on `--backend=native` - every Dragon function is callable via FFI.
4. **Source-portable subset.** Need doc + lint rule. v1 lint: no platform-specific imports in `@portable` code.
5. **DevTools.** Dragon-side inspector sufficient; rendered tree lives in platform debugger.
6. **Compose / SwiftUI integration.** Defer; v1 binds imperative widget APIs.

## Worth the maintenance bill

### Positive
- Native rendering is the right perf destination, sequenceable.
- IR (Phase 0) useful standalone (SQL builders, AST, protobuf).
- and coexist; no abandoned-bet exposure.
- Source compatibility lets apps try `--backend=native` without rewriting business logic.
- reused as-is.

### Negative
- ~22 KLOC + ~570 IR to maintain across five OS update cadences.
- Platform vendor compatibility windows (UIKit deprecations, Android API levels, Win32 → WinUI). Higher maintenance than 's one webview.
- Not pixel-identical cross-platform - apps that need that stay on .
- Build pipeline grows: Xcode, Gradle, MSVC, system gcc. `dr build` becomes a meta-toolchain coordinator (fun times ahead).

### Neutral
- Platform vendor risk accepted. Bind stable subset; version bindings (`ui.uikit.v17`, etc.) when needed.

## References

- §Phase 4 (block interpolation, `:{}` content alias)
- §Phase 3 (per-class destructors)
- §Phase 2 (Platform abstraction layer)
- - native function ABI for honest platform FFI
- §Future Work - `ui.qt` slot generalizes
