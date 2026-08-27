#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>

#define DRAGON_DBG(...) do { if (getenv("DRAGON_UI_DEBUG")) { fprintf(stderr, "[shell] " __VA_ARGS__); fflush(stderr); } } while (0)

extern "C" const char* dragon_string_dup(const char* s);

extern "C" char* dragon_str_to_utf8_alloc(const char* s, int64_t* out_byte_len);

static const char* dragon__utf8_view(const char* s, char** owned, int64_t* len_out) {
    if (!s) { *owned = NULL; if (len_out) *len_out = 0; return NULL; }
    int64_t len = 0;
    *owned = dragon_str_to_utf8_alloc(s, &len);
    if (len_out) *len_out = len;
    return *owned ? *owned : s;
}

extern "C" {
typedef struct DragonUiAsset {
    const char* path;
    const unsigned char* data;
    unsigned long len;
} DragonUiAsset;
extern const DragonUiAsset dragon_ui_assets[];
extern const unsigned long dragon_ui_asset_count;
}

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

typedef void (*DragonMsgHandler)(const char*);

typedef struct DragonWebView {
    GtkWidget*      window;
    WebKitWebView*  webview;
    DragonMsgHandler handler;
    gboolean        shown;
} DragonWebView;

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

static DragonWebView* g_current_wv = NULL;

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

static int g_open_windows = 0;

static void dragon__on_window_destroy(GtkWidget* widget, gpointer user_data) {
    (void) widget;
    DragonWebView* wv = (DragonWebView*) user_data;
    DRAGON_DBG("window destroy: %p (open=%d)\n", (void*) wv, g_open_windows);
    // Null the struct so a late call on a still-held `Window.handle` is a no-op,
    // never a use-after-free. The struct outlives this, freed at process exit.
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

void dragon_webview_enable_inspector(void* handle) {
    DragonWebView* wv = (DragonWebView*) handle;
    if (!wv || !wv->webview) return;
    WebKitSettings* st = webkit_web_view_get_settings(wv->webview);
    webkit_settings_set_enable_developer_extras(st, TRUE);
}

void* dragon_webview_window_new(const char* title, int width, int height) {
    DragonWebView* wv = g_new0(DragonWebView, 1);
    wv->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    char* title_owned = NULL;
    const char* title_utf8 = dragon__utf8_view(title, &title_owned, NULL);
    gtk_window_set_title(GTK_WINDOW(wv->window), title_utf8 ? title_utf8 : "Dragon");
    if (title_owned) free(title_owned);
    gtk_window_set_default_size(GTK_WINDOW(wv->window), width, height);

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
    if (getenv("DRAGON_UI_INSPECT")) dragon_webview_enable_inspector(wv);
    gtk_container_add(GTK_CONTAINER(wv->window), GTK_WIDGET(wv->webview));
    g_signal_connect(wv->window, "destroy", G_CALLBACK(dragon__on_window_destroy), wv);
    g_signal_connect(wv->webview, "load-changed", G_CALLBACK(dragon__on_load_changed), NULL);
    g_signal_connect(wv->webview, "web-process-terminated",
                     G_CALLBACK(dragon__on_web_process_terminated), NULL);
    g_current_wv = wv;
    return wv;
}

void dragon_webview_set_handler(void* fn) {
    if (g_current_wv) g_current_wv->handler = (DragonMsgHandler) fn;
}

static gboolean dragon__post_trampoline(gpointer data) {
    ((void (*)(void)) data)();
    return G_SOURCE_REMOVE;
}
void dragon_webview_post(void* fn) {
    if (!fn) return;
    g_idle_add(dragon__post_trampoline, fn);
}

void dragon_webview_eval_js(const char* js) {
    if (!g_current_wv || !g_current_wv->webview || !js) return;
    char* owned = NULL;
    int64_t blen = 0;
    const char* bytes = dragon__utf8_view(js, &owned, &blen);
#if WEBKIT_CHECK_VERSION(2, 40, 0)
    webkit_web_view_evaluate_javascript(g_current_wv->webview, bytes, (gssize) blen,
                                        NULL, NULL, NULL, NULL, NULL);
#else
    webkit_web_view_run_javascript(g_current_wv->webview, bytes, NULL, NULL, NULL);
#endif
    if (owned) free(owned);
}

void dragon_webview_load_html(void* handle, const char* html) {
    DragonWebView* wv = (DragonWebView*) handle;
    if (!wv || !wv->webview) return;
    char* owned = NULL;
    int64_t blen = 0;
    const char* bytes = html ? dragon__utf8_view(html, &owned, &blen) : NULL;
    DRAGON_DBG("load_html: %lld bytes\n", (long long) blen);
    webkit_web_view_load_html(wv->webview, bytes ? bytes : "", "app:///");
    if (owned) free(owned);
}

void dragon_webview_show(void* handle) {
    DragonWebView* wv = (DragonWebView*) handle;
    if (!wv || !wv->window) return;
    if (!wv->shown) {
        wv->shown = TRUE;
        g_open_windows++;
    }
    gtk_widget_show_all(wv->window);
}

void dragon_webview_maximize(void* handle) {
    DragonWebView* wv = (DragonWebView*) handle;
    if (!wv || !wv->window) return;
    gtk_window_maximize(GTK_WINDOW(wv->window));
}

int64_t dragon_webview_is_maximized(void* handle) {
    DragonWebView* wv = (DragonWebView*) handle;
    if (!wv || !wv->window) return 0;
    return gtk_window_is_maximized(GTK_WINDOW(wv->window)) ? 1 : 0;
}

void dragon_webview_close(void* handle) {
    DragonWebView* wv = (DragonWebView*) handle;
    if (wv && wv->window) gtk_widget_destroy(wv->window);
}

typedef struct {
    GMutex mutex;
    GCond cond;
    gboolean done;
    char* path;
    const char* title;
    const char* start_dir;
} DragonFolderPick;

static gboolean dragon__folder_pick_idle(gpointer data) {
    DragonFolderPick* req = (DragonFolderPick*) data;
    GtkWindow* parent = (g_current_wv && g_current_wv->window)
        ? GTK_WINDOW(g_current_wv->window) : NULL;
    GtkFileChooserNative* dlg = gtk_file_chooser_native_new(
        (req->title && req->title[0]) ? req->title : "Open Folder",
        parent, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Open", "_Cancel");
    if (req->start_dir && req->start_dir[0])
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), req->start_dir);
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
        req->path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
    g_object_unref(dlg);
    g_mutex_lock(&req->mutex);
    req->done = TRUE;
    g_cond_signal(&req->cond);
    g_mutex_unlock(&req->mutex);
    return G_SOURCE_REMOVE;
}

const char* dragon_webview_pick_folder(const char* title, const char* start_dir) {
    char* title_owned = NULL;
    char* start_owned = NULL;
    DragonFolderPick req;
    g_mutex_init(&req.mutex);
    g_cond_init(&req.cond);
    req.done = FALSE;
    req.path = NULL;
    req.title = dragon__utf8_view(title, &title_owned, NULL);
    req.start_dir = dragon__utf8_view(start_dir, &start_owned, NULL);
    if (g_main_context_is_owner(g_main_context_default())) {
        dragon__folder_pick_idle(&req);
    } else {
        g_idle_add(dragon__folder_pick_idle, &req);
        g_mutex_lock(&req.mutex);
        while (!req.done) g_cond_wait(&req.cond, &req.mutex);
        g_mutex_unlock(&req.mutex);
    }
    g_mutex_clear(&req.mutex);
    g_cond_clear(&req.cond);
    if (title_owned) free(title_owned);
    if (start_owned) free(start_owned);
    const char* out = dragon_string_dup(req.path ? req.path : "");
    if (req.path) g_free(req.path);
    return out;
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

}

