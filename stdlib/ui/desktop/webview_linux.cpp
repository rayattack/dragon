// webview_linux.cpp - Dragon UI: Linux webview shell (GTK3 + webkit2gtk-4.1).
//
// The "unexpressible platform glue" the dogfooding policy permits in C++:
// window + webview lifecycle, the GTK main loop, the JS<->Dragon message bridge,
// and the window.dr shim injection. Everything above this - Signal/effect, the
// binding-table patch protocol, Window/App API objects - is .dr.
//
// Compiled into the program AUTOMATICALLY by the compiler: when a program
// references dragon_webview_* externs (`import ui`), linkExecutable builds this
// file like an ADR-041 `--cc-source` shim, with GTK/webkit include and link
// flags resolved via pkg-config (webkit2gtk-4.1 preferred, 4.0 accepted - the
// eval_js version guard covers pre-2.40 webkit). Passing your own webview
// `--cc-source` overrides the auto path. Nothing here is pulled into a Dragon
// binary that does not import `ui`, so non-UI programs keep their tiny
// footprint.
//
// Exposes a flat extern "C" API consumed by stdlib/ui/desktop/desktop.dr and
// stdlib/ui/ui.dr.

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <cstdio>
#include <cstring>

// DEBUG (temporary): set DRAGON_UI_DEBUG=1 to trace load + bridge events to stderr.
#define DRAGON_DBG(...) do { if (getenv("DRAGON_UI_DEBUG")) { fprintf(stderr, "[shell] " __VA_ARGS__); fflush(stderr); } } while (0)

// Dragon runtime: wrap a plain C string into a refcounted Dragon string. A Dragon
// `str` is a `const char*` pointing at the managed string's data (the refcount
// header sits behind it), so the value returned here can be handed straight to a
// Dragon `def msg: str` parameter, which lowers to `void(const char*)`.
extern "C" const char* dragon_string_dup(const char* s);

// D031 app:// assets: the compiler emits one generated TU per ui build holding
// every file of the program's assets/ directory (empty table when there is
// none); the scheme handler below resolves app://<path> against it. No socket,
// no port, no HTTP round-trip.
extern "C" {
typedef struct DragonUiAsset {
    const char* path;              // relative to assets/, e.g. "app.css"
    const unsigned char* data;
    unsigned long len;
} DragonUiAsset;
extern const DragonUiAsset dragon_ui_assets[];
extern const unsigned long dragon_ui_asset_count;
}

// Content-Type by extension - the handler serves from memory, so this is the
// only place a type can come from.
static const char* dragon__asset_mime(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    ++dot;
    if (!strcmp(dot, "html") || !strcmp(dot, "htm")) return "text/html";
    if (!strcmp(dot, "css"))   return "text/css";
    if (!strcmp(dot, "js"))    return "text/javascript";
    if (!strcmp(dot, "mjs"))   return "text/javascript";
    if (!strcmp(dot, "json"))  return "application/json";
    if (!strcmp(dot, "svg"))   return "image/svg+xml";
    if (!strcmp(dot, "png"))   return "image/png";
    if (!strcmp(dot, "jpg") || !strcmp(dot, "jpeg")) return "image/jpeg";
    if (!strcmp(dot, "gif"))   return "image/gif";
    if (!strcmp(dot, "webp"))  return "image/webp";
    if (!strcmp(dot, "ico"))   return "image/x-icon";
    if (!strcmp(dot, "woff2")) return "font/woff2";
    if (!strcmp(dot, "woff"))  return "font/woff";
    if (!strcmp(dot, "ttf"))   return "font/ttf";
    if (!strcmp(dot, "txt"))   return "text/plain";
    if (!strcmp(dot, "wasm"))  return "application/wasm";
    return "application/octet-stream";
}

extern "C" {

// Inbound message callback: a top-level Dragon `def on_message(msg: str)` passed
// as a `ptr`. Lowers to this C signature.
typedef void (*DragonMsgHandler)(const char*);

// Opaque handle handed back to Dragon as a `ptr`.
typedef struct DragonWebView {
    GtkWidget*      window;
    WebKitWebView*  webview;
    DragonMsgHandler handler;   // JS -> Dragon dispatch (set by dragon_webview_set_handler)
    gboolean        shown;      // counted in g_open_windows once shown
} DragonWebView;

// The locked window.dr shim, injected at document-start so it exists before any
// page script runs. invoke() posts a message to native; _patch() applies a single
// targeted node op (text/attr/html) - the v1 re-render mechanism (no innerHTML
// replace). call() is the JSON bridge (Decision 031 Phase 0): a named Dragon
// handler invoked by promise, settled from native via _settle(). Hardening
// (Object.freeze / defineProperty) lands with the security pass; this is the
// functional core.
static const char* DRAGON_SHIM_JS =
    "(function(){"
    "  if (window.__drInit) return; window.__drInit = true;"
    "  function post(s){ try{ window.webkit.messageHandlers.dragon.postMessage(String(s)); }catch(e){} }"
    "  window.dr = {"
    "    invoke: function(name){ post(name); },"
    "    _seq: 0,"
    "    _pending: {},"
    "    call: function(name, payload){"
    "      var id = ++window.dr._seq;"
    "      return new Promise(function(resolve, reject){"
    "        window.dr._pending[id] = { ok: resolve, err: reject };"
    "        post('rpc:' + id + ':' + name + ':' + (payload == null ? '' : String(payload)));"
    "      });"
    "    },"
    "    _settle: function(id, ok, data){"
    "      var p = window.dr._pending[id];"
    "      if (!p) return;"
    "      delete window.dr._pending[id];"
    "      if (ok) p.ok(data); else p.err(new Error(data));"
    "    },"
    "    _patch: function(op){"
    "      var el = document.querySelector('[data-dr=\"'+op.id+'\"]');"
    "      if(!el) return;"
    "      if(op.op==='text') el.textContent = op.value;"
    "      else if(op.op==='attr') el.setAttribute(op.name, op.value);"
    "      else if(op.op==='html') el.innerHTML = op.value;"
    "    }"
    "  };"
    "})();";

// app:// scheme handler: serve straight from the embedded asset table. The
// URI's authority and path are both part of the lookup key ("app://a/b.css"
// resolves the asset "a/b.css"), query/fragment are ignored.
static void dragon__on_app_request(WebKitURISchemeRequest* request, gpointer) {
    const char* uri = webkit_uri_scheme_request_get_uri(request);
    const char* p = uri ? strstr(uri, "://") : NULL;
    p = p ? p + 3 : "";
    while (*p == '/') ++p;
    char key[1024];
    size_t n = 0;
    for (; p[n] && p[n] != '?' && p[n] != '#' && n < sizeof(key) - 1; ++n)
        key[n] = p[n];
    key[n] = 0;
    DRAGON_DBG("app:// request: '%s' -> key '%s'\n", uri ? uri : "(null)", key);
    for (unsigned long i = 0; i < dragon_ui_asset_count; ++i) {
        if (strcmp(dragon_ui_assets[i].path, key) != 0) continue;
        GInputStream* stream = g_memory_input_stream_new_from_data(
            dragon_ui_assets[i].data, (gssize) dragon_ui_assets[i].len, NULL);
        webkit_uri_scheme_request_finish(request, stream,
                                         (gint64) dragon_ui_assets[i].len,
                                         dragon__asset_mime(key));
        g_object_unref(stream);
        return;
    }
    GError* err = g_error_new(G_FILE_ERROR, G_FILE_ERROR_NOENT,
                              "app://%s: no such embedded asset", key);
    webkit_uri_scheme_request_finish_error(request, err);
    g_error_free(err);
}

// Single-window Phase 0: the most-recently-created window is "current", so the
// bridge entry points (set_handler / eval_js) need no handle. Multi-window adds
// explicit handles later.
static DragonWebView* g_current_wv = NULL;

// GTK signal: a page called window.webkit.messageHandlers.dragon.postMessage(...).
// Extract the string, wrap it as a Dragon string, and dispatch to the Dragon
// handler.
static void dragon__on_script_message(WebKitUserContentManager* ucm,
                                       WebKitJavascriptResult* res,
                                       gpointer user_data) {
    (void) ucm;
    DragonWebView* wv = (DragonWebView*) user_data;
    JSCValue* value = webkit_javascript_result_get_js_value(res);
    char* s = jsc_value_to_string(value);
    DRAGON_DBG("script-message: '%s' handler=%p\n", s ? s : "(null)", (void*) wv->handler);
    if (wv->handler && s) {
        const char* dstr = dragon_string_dup(s);
        wv->handler(dstr);
    }
    if (s) g_free(s);
}

static void dragon__on_load_changed(WebKitWebView* view, WebKitLoadEvent ev, gpointer) {
    (void) view;
    DRAGON_DBG("load-changed: %d (3=finished)\n", (int) ev);
}

static void dragon__on_web_process_terminated(WebKitWebView* view, guint reason, gpointer) {
    (void) view;
    DRAGON_DBG("WEB PROCESS TERMINATED, reason=%u\n", reason);
}

// Open (= shown and not yet destroyed) window count. `App.run()` returns when
// the LAST open window closes, not when the first one does; each window's
// destroy signal lands here instead of straight in gtk_main_quit.
static int g_open_windows = 0;

static void dragon__on_window_destroy(GtkWidget* widget, gpointer user_data) {
    (void) widget;
    DragonWebView* wv = (DragonWebView*) user_data;
    DRAGON_DBG("window destroy: %p (open=%d)\n", (void*) wv, g_open_windows);
    // The GTK widgets are being torn down; null the struct so any late Dragon
    // call on this window (a still-held `Window.handle`) is a safe no-op, never
    // a use-after-free. The struct itself stays allocated because Dragon still
    // points at it; it is reclaimed at process exit.
    wv->window = NULL;
    wv->webview = NULL;
    wv->handler = NULL;
    if (g_current_wv == wv) g_current_wv = NULL;
    if (wv->shown) {
        wv->shown = FALSE;
        if (--g_open_windows <= 0 && gtk_main_level() > 0) gtk_main_quit();
    }
}

void dragon_webview_init(void) {
    gtk_init(NULL, NULL);
    // Register app:// once on the default context (every webview shares it).
    // Secure + CORS-enabled so documents can reference embedded assets the
    // way they would any https origin.
    static gboolean scheme_registered = FALSE;
    if (!scheme_registered) {
        scheme_registered = TRUE;
        WebKitWebContext* ctx = webkit_web_context_get_default();
        webkit_web_context_register_uri_scheme(ctx, "app",
                                               dragon__on_app_request,
                                               NULL, NULL);
        WebKitSecurityManager* sm = webkit_web_context_get_security_manager(ctx);
        webkit_security_manager_register_uri_scheme_as_secure(sm, "app");
        webkit_security_manager_register_uri_scheme_as_cors_enabled(sm, "app");
    }
}

void* dragon_webview_window_new(const char* title, int width, int height) {
    DragonWebView* wv = g_new0(DragonWebView, 1);
    wv->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(wv->window), title ? title : "Dragon");
    gtk_window_set_default_size(GTK_WINDOW(wv->window), width, height);

    // Bridge: a user-content manager carries the "dragon" message channel and the
    // injected shim. The webview is created bound to it.
    WebKitUserContentManager* ucm = webkit_user_content_manager_new();
    webkit_user_content_manager_register_script_message_handler(ucm, "dragon");
    g_signal_connect(ucm, "script-message-received::dragon",
                     G_CALLBACK(dragon__on_script_message), wv);
    WebKitUserScript* shim = webkit_user_script_new(
        DRAGON_SHIM_JS,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        NULL, NULL);
    webkit_user_content_manager_add_script(ucm, shim);

    wv->webview = WEBKIT_WEB_VIEW(webkit_web_view_new_with_user_content_manager(ucm));
    gtk_container_add(GTK_CONTAINER(wv->window), GTK_WIDGET(wv->webview));
    g_signal_connect(wv->window, "destroy", G_CALLBACK(dragon__on_window_destroy), wv);
    g_signal_connect(wv->webview, "load-changed", G_CALLBACK(dragon__on_load_changed), NULL);
    g_signal_connect(wv->webview, "web-process-terminated",
                     G_CALLBACK(dragon__on_web_process_terminated), NULL);
    g_current_wv = wv;
    return wv;
}

// Register the Dragon-side JS->Dragon message handler (a top-level `def` as ptr)
// on the current window.
void dragon_webview_set_handler(void* fn) {
    if (g_current_wv) g_current_wv->handler = (DragonMsgHandler) fn;
}

// Hand a no-arg Dragon function to the GTK main loop from ANY thread.
// g_idle_add is thread-safe; the function runs on the UI thread, where
// webkit calls (eval_js and friends) are legal. This is the worker-to-UI
// completion edge of the JSON bridge (Decision 031's run_on_ui, minimal form).
static gboolean dragon__post_trampoline(gpointer data) {
    ((void (*)(void)) data)();
    return G_SOURCE_REMOVE;
}
void dragon_webview_post(void* fn) {
    if (!fn) return;
    g_idle_add(dragon__post_trampoline, fn);
}

// Dragon -> JS: run a script in the current window (used to push targeted patch ops).
void dragon_webview_eval_js(const char* js) {
    if (!g_current_wv || !g_current_wv->webview) return;
#if WEBKIT_CHECK_VERSION(2, 40, 0)
    webkit_web_view_evaluate_javascript(g_current_wv->webview, js, -1,
                                        NULL, NULL, NULL, NULL, NULL);
#else
    // webkit < 2.40 (the webkit2gtk-4.0 era): evaluate_javascript does not
    // exist yet; run_javascript is the same fire-and-forget call here.
    webkit_web_view_run_javascript(g_current_wv->webview, js, NULL, NULL, NULL);
#endif
}

void dragon_webview_load_html(void* handle, const char* html) {
    DragonWebView* wv = (DragonWebView*) handle;
    if (!wv || !wv->webview) return;  // closed window: no-op
    DRAGON_DBG("load_html: %zu bytes\n", html ? strlen(html) : 0);
    // Base the document on app:/// so relative asset URLs ("/app.css",
    // "app.css") resolve through the embedded-asset scheme handler.
    webkit_web_view_load_html(wv->webview, html ? html : "", "app:///");
}

void dragon_webview_show(void* handle) {
    DragonWebView* wv = (DragonWebView*) handle;
    if (!wv || !wv->window) return;   // closed window: no-op
    if (!wv->shown) {
        wv->shown = TRUE;
        g_open_windows++;
    }
    gtk_widget_show_all(wv->window);
}

// Close a window programmatically: destroy the GTK toplevel, which fires the
// same "destroy" signal as the user clicking the window's close button, so
// programmatic and user closes share one code path.
void dragon_webview_close(void* handle) {
    DragonWebView* wv = (DragonWebView*) handle;
    if (wv && wv->window) gtk_widget_destroy(wv->window);
}

void dragon_webview_run(void) {
    gtk_main();
}

static gboolean dragon__quit_cb(gpointer data) {
    (void) data;
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}
void dragon_webview_run_timeout(int ms) {
    DRAGON_DBG("run_timeout: entering gtk_main for %d ms\n", ms);
    g_timeout_add(ms, dragon__quit_cb, NULL);
    gtk_main();
    DRAGON_DBG("run_timeout: gtk_main returned\n");
}

}  // extern "C"
