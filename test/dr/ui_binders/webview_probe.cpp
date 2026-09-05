/* A headless stand-in for platform/webview_linux.cpp, so the reactive binders in
 * stdlib/ui/ui.dr can be asserted on without a GTK window or a WebKit process.
 *
 * The basename MUST contain "webview": that is the switch CodeGen.cpp uses to
 * decide the program brings its own shell, and without it `import ui` also
 * compiles and links the real GTK shim, which then collides on every symbol
 * defined here.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" const char* dragon_string_alloc(const char* src, int64_t len);
extern "C" const char* dragon_string_dup(const char* s);
extern "C" void dragon_decref_str(const char* s);
extern "C" char* dragon_str_to_utf8_alloc(const char* s, int64_t* out_byte_len);

namespace {

typedef void (*DragonMsgHandler)(const char*);

std::vector<std::string> g_evaluated_js;
DragonMsgHandler g_handler = nullptr;

/* A Dragon `str` reaching C is a C string only while it stays ASCII; anything
 * else is UCS-4 internally and has to be converted, exactly as the real shell
 * does before handing bytes to WebKit. */
std::string utf8_of(const char* s) {
    if (!s) return std::string();
    int64_t len = 0;
    char* owned = dragon_str_to_utf8_alloc(s, &len);
    std::string out(owned ? owned : s, owned ? (size_t) len : strlen(s));
    if (owned) free(owned);
    return out;
}

}

extern "C" {

void dragon_webview_eval_js(const char* js) { g_evaluated_js.push_back(utf8_of(js)); }
void dragon_webview_set_handler(void* fn) { g_handler = (DragonMsgHandler) fn; }
void dragon_webview_run(void) {}
void dragon_webview_run_timeout(int ms) { (void) ms; }
void dragon_webview_post(void* fn) { if (fn) ((void (*)(void)) fn)(); }

int64_t probe_js_count(void) { return (int64_t) g_evaluated_js.size(); }

const char* probe_js_at(int64_t i) {
    if (i < 0 || i >= (int64_t) g_evaluated_js.size())
        return dragon_string_alloc("", 0);
    const std::string& js = g_evaluated_js[(size_t) i];
    return dragon_string_alloc(js.data(), (int64_t) js.size());
}

void probe_js_reset(void) { g_evaluated_js.clear(); }

void probe_dispatch(const char* msg) {
    if (!g_handler) return;
    std::string m = utf8_of(msg);
    const char* dispatched = dragon_string_dup(m.c_str());
    g_handler(dispatched);
    dragon_decref_str(dispatched);
}

}
