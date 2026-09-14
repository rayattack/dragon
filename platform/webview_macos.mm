#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>

#define DRAGON_DBG(...) do { if (getenv("DRAGON_UI_DEBUG")) { fprintf(stderr, "[shell] " __VA_ARGS__); fflush(stderr); } } while (0)

extern "C" const char* dragon_string_dup_cstr(const char* s);
extern "C" void dragon_foreign_enter(void);
extern "C" void dragon_foreign_exit(void);
extern "C" int64_t dragon_safe_region_suspend(void);
extern "C" void dragon_safe_region_resume(int64_t token);

extern "C" char* dragon_str_to_utf8_alloc(const char* s, int64_t* out_byte_len);

static const char* dragon__utf8_view(const char* s, char** owned, int64_t* len_out) {
    if (!s) { *owned = NULL; if (len_out) *len_out = 0; return NULL; }
    int64_t len = 0;
    *owned = dragon_str_to_utf8_alloc(s, &len);
    if (len_out) *len_out = len;
    return *owned ? *owned : s;
}

static NSString* dragon__nsstring(const char* s) {
    char* owned = NULL;
    const char* bytes = dragon__utf8_view(s, &owned, NULL);
    NSString* out = bytes ? [NSString stringWithUTF8String:bytes] : @"";
    if (owned) free(owned);
    return out ?: @"";
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

static NSString* dragon__asset_mime(NSString* path) {
    NSString* ext = [[path pathExtension] lowercaseString];
    if ([ext isEqualToString:@"html"] || [ext isEqualToString:@"htm"]) return @"text/html";
    if ([ext isEqualToString:@"css"])   return @"text/css";
    if ([ext isEqualToString:@"js"])    return @"text/javascript";
    if ([ext isEqualToString:@"mjs"])   return @"text/javascript";
    if ([ext isEqualToString:@"json"])  return @"application/json";
    if ([ext isEqualToString:@"svg"])   return @"image/svg+xml";
    if ([ext isEqualToString:@"png"])   return @"image/png";
    if ([ext isEqualToString:@"jpg"] || [ext isEqualToString:@"jpeg"]) return @"image/jpeg";
    if ([ext isEqualToString:@"gif"])   return @"image/gif";
    if ([ext isEqualToString:@"webp"])  return @"image/webp";
    if ([ext isEqualToString:@"ico"])   return @"image/x-icon";
    if ([ext isEqualToString:@"woff2"]) return @"font/woff2";
    if ([ext isEqualToString:@"woff"])  return @"font/woff";
    if ([ext isEqualToString:@"ttf"])   return @"font/ttf";
    if ([ext isEqualToString:@"txt"])   return @"text/plain";
    if ([ext isEqualToString:@"wasm"])  return @"application/wasm";
    return @"application/octet-stream";
}

typedef void (*DragonMsgHandler)(const char*);

static NSString* const DRAGON_SHIM_JS =
    @"(function(){"
    @"  if (window.__drInit) return; window.__drInit = true;"
    @"  function post(s){ try{ window.webkit.messageHandlers.dragon.postMessage(String(s)); }catch(e){} }"
    @"  window.dr = {"
    @"    invoke: function(name){ post(name); },"
    @"    _seq: 0,"
    @"    _pending: {},"
    @"    call: function(name, payload){"
    @"      var id = ++window.dr._seq;"
    @"      return new Promise(function(resolve, reject){"
    @"        window.dr._pending[id] = { ok: resolve, err: reject };"
    @"        post('rpc:' + id + ':' + name + ':' + (payload == null ? '' : String(payload)));"
    @"      });"
    @"    },"
    @"    _settle: function(id, ok, data){"
    @"      var p = window.dr._pending[id];"
    @"      if (!p) return;"
    @"      delete window.dr._pending[id];"
    @"      if (ok) p.ok(data); else p.err(new Error(data));"
    @"    },"
    @"    _patch: function(op){"
    @"      var el = document.querySelector('[data-dr=\"'+op.id+'\"]');"
    @"      if(!el) return;"
    @"      if(op.op==='text') el.textContent = op.value;"
    @"      else if(op.op==='attr') el.setAttribute(op.name, op.value);"
    @"      else if(op.op==='html') el.innerHTML = op.value;"
    @"    }"
    @"  };"
    @"})();";

@interface DragonWindow : NSObject <WKScriptMessageHandler, WKURLSchemeHandler, NSWindowDelegate>
@property (nonatomic, strong) NSWindow* window;
@property (nonatomic, strong) WKWebView* webview;
@property (nonatomic, strong) NSString* document;
@property (nonatomic, assign) DragonMsgHandler handler;
@property (nonatomic, assign) BOOL shown;
@property (nonatomic, assign) unsigned long loadSeq;
@end

static int g_open_windows = 0;

static DragonWindow* g_current_wv = nil;

static NSMutableArray<DragonWindow*>* g_windows = nil;

static void dragon__stop_app(void) {
    [NSApp stop:nil];
    NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                       location:NSZeroPoint
                                  modifierFlags:0
                                      timestamp:0
                                   windowNumber:0
                                        context:nil
                                        subtype:0
                                          data1:0
                                          data2:0];
    [NSApp postEvent:wake atStart:YES];
}

@implementation DragonWindow

- (void)userContentController:(WKUserContentController*)ucm
      didReceiveScriptMessage:(WKScriptMessage*)message {
    (void) ucm;
    NSString* body = [message.body isKindOfClass:[NSString class]]
        ? (NSString*) message.body
        : [NSString stringWithFormat:@"%@", message.body];
    const char* utf8 = [body UTF8String];
    DRAGON_DBG("script-message: '%s' handler=%p\n", utf8 ? utf8 : "(null)", (void*) self.handler);
    if (self.handler && utf8) {
        const int64_t region = dragon_safe_region_suspend();
        self.handler(dragon_string_dup_cstr(utf8));
        dragon_safe_region_resume(region);
    }
}

- (void)webView:(WKWebView*)webView startURLSchemeTask:(id<WKURLSchemeTask>)task {
    (void) webView;
    NSURL* url = task.request.URL;
    NSString* key = url.path ?: @"";
    if (url.host.length) key = [NSString stringWithFormat:@"%@%@", url.host, key];
    while ([key hasPrefix:@"/"]) key = [key substringFromIndex:1];
    DRAGON_DBG("app:// request: '%s' -> key '%s'\n",
               [[url absoluteString] UTF8String] ?: "(null)", [key UTF8String] ?: "");

    NSData* data = nil;
    NSString* mime = nil;
    if (key.length == 0) {
        data = [(self.document ?: @"") dataUsingEncoding:NSUTF8StringEncoding];
        mime = @"text/html";
    } else {
        const char* want = [key UTF8String];
        for (unsigned long i = 0; want && i < dragon_ui_asset_count; ++i) {
            if (strcmp(dragon_ui_assets[i].path, want) != 0) continue;
            data = [NSData dataWithBytes:dragon_ui_assets[i].data
                                  length:(NSUInteger) dragon_ui_assets[i].len];
            mime = dragon__asset_mime(key);
            break;
        }
    }
    if (!data) {
        NSError* err = [NSError errorWithDomain:NSURLErrorDomain
                                           code:NSURLErrorResourceUnavailable
                                       userInfo:@{NSLocalizedDescriptionKey:
                            [NSString stringWithFormat:@"app://%@: no such embedded asset", key]}];
        [task didFailWithError:err];
        return;
    }
    NSURLResponse* resp = [[NSURLResponse alloc] initWithURL:url
                                                    MIMEType:mime
                                       expectedContentLength:(NSInteger) data.length
                                            textEncodingName:@"utf-8"];
    [task didReceiveResponse:resp];
    [task didReceiveData:data];
    [task didFinish];
}

- (void)webView:(WKWebView*)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task {
    (void) webView; (void) task;
}

- (void)windowWillClose:(NSNotification*)note {
    (void) note;
    DRAGON_DBG("window close: %p (open=%d)\n", (__bridge void*) self, g_open_windows);
    // Nil the ivars so a late call on a still-held `Window.handle` is a no-op,
    // never a use-after-free. The object outlives this, held by g_windows.
    self.window = nil;
    self.webview = nil;
    self.handler = NULL;
    if (g_current_wv == self) g_current_wv = nil;
    if (self.shown) {
        self.shown = NO;
        if (--g_open_windows <= 0) dragon__stop_app();
    }
}

@end

extern "C" {

void dragon_webview_init(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        if (!g_windows) g_windows = [NSMutableArray array];
    }
}

void dragon_webview_enable_inspector(void* handle) {
    DragonWindow* wv = (__bridge DragonWindow*) handle;
    if (!wv || !wv.webview) return;
    @autoreleasepool {
        if ([wv.webview respondsToSelector:@selector(setInspectable:)])
            [wv.webview setValue:@YES forKey:@"inspectable"];
        else
            [wv.webview.configuration.preferences setValue:@YES
                                                    forKey:@"developerExtrasEnabled"];
    }
}

void* dragon_webview_window_new(const char* title, int width, int height) {
    @autoreleasepool {
        DragonWindow* wv = [[DragonWindow alloc] init];
        wv.document = @"";

        WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
        WKUserContentController* ucm = [[WKUserContentController alloc] init];
        [ucm addScriptMessageHandler:wv name:@"dragon"];
        [ucm addUserScript:[[WKUserScript alloc]
            initWithSource:DRAGON_SHIM_JS
             injectionTime:WKUserScriptInjectionTimeAtDocumentStart
          forMainFrameOnly:NO]];
        config.userContentController = ucm;
        [config setURLSchemeHandler:wv forURLScheme:@"app"];

        NSRect frame = NSMakeRect(0, 0, width, height);
        wv.webview = [[WKWebView alloc] initWithFrame:frame configuration:config];

        NSWindow* win = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                 NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        [win setTitle:dragon__nsstring(title)];
        [win setContentView:wv.webview];
        [win setDelegate:wv];
        [win center];
        wv.window = win;

        if (getenv("DRAGON_UI_INSPECT"))
            dragon_webview_enable_inspector((__bridge void*) wv);

        [g_windows addObject:wv];
        g_current_wv = wv;
        return (__bridge void*) wv;
    }
}

void dragon_webview_set_handler(void* fn) {
    if (g_current_wv) g_current_wv.handler = (DragonMsgHandler) fn;
}

void dragon_webview_post(void* fn) {
    if (!fn) return;
    void (*f)(void) = (void (*)(void)) fn;
    dispatch_async(dispatch_get_main_queue(), ^{ f(); });
}

void dragon_webview_eval_js(const char* js) {
    if (!g_current_wv || !g_current_wv.webview || !js) return;
    @autoreleasepool {
        [g_current_wv.webview evaluateJavaScript:dragon__nsstring(js) completionHandler:nil];
    }
}

void dragon_webview_load_html(void* handle, const char* html) {
    DragonWindow* wv = (__bridge DragonWindow*) handle;
    if (!wv || !wv.webview) return;
    @autoreleasepool {
        wv.document = dragon__nsstring(html);
        DRAGON_DBG("load_html: %lu bytes\n", (unsigned long) wv.document.length);
        NSString* root = [NSString stringWithFormat:@"app:///?dr=%lu", ++wv.loadSeq];
        [wv.webview loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:root]]];
    }
}

void dragon_webview_show(void* handle) {
    DragonWindow* wv = (__bridge DragonWindow*) handle;
    if (!wv || !wv.window) return;
    @autoreleasepool {
        if (!wv.shown) {
            wv.shown = YES;
            g_open_windows++;
        }
        [wv.window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }
}

void dragon_webview_maximize(void* handle) {
    DragonWindow* wv = (__bridge DragonWindow*) handle;
    if (!wv || !wv.window) return;
    @autoreleasepool {
        if (![wv.window isZoomed]) [wv.window zoom:nil];
    }
}

int64_t dragon_webview_is_maximized(void* handle) {
    DragonWindow* wv = (__bridge DragonWindow*) handle;
    if (!wv || !wv.window) return 0;
    return [wv.window isZoomed] ? 1 : 0;
}

void dragon_webview_close(void* handle) {
    DragonWindow* wv = (__bridge DragonWindow*) handle;
    if (wv && wv.window) [wv.window performClose:nil];
}

const char* dragon_webview_pick_folder(const char* title, const char* start_dir) {
    @autoreleasepool {
        __block NSString* picked = nil;
        NSString* nsTitle = dragon__nsstring(title);
        NSString* nsStart = dragon__nsstring(start_dir);

        void (^run)(void) = ^{
            @autoreleasepool {
                NSOpenPanel* panel = [NSOpenPanel openPanel];
                panel.canChooseDirectories = YES;
                panel.canChooseFiles = NO;
                panel.allowsMultipleSelection = NO;
                panel.title = nsTitle.length ? nsTitle : @"Open Folder";
                panel.message = nsTitle.length ? nsTitle : @"Open Folder";
                if (nsStart.length)
                    panel.directoryURL = [NSURL fileURLWithPath:nsStart isDirectory:YES];
                if ([panel runModal] == NSModalResponseOK) picked = panel.URL.path;
            }
        };

        dragon_foreign_enter();
        if ([NSThread isMainThread]) run();
        else dispatch_sync(dispatch_get_main_queue(), run);
        dragon_foreign_exit();

        return dragon_string_dup_cstr(picked ? [picked UTF8String] : "");
    }
}

void dragon_webview_run(void) {
    dragon_foreign_enter();
    [NSApp run];
    dragon_foreign_exit();
}

void dragon_webview_run_timeout(int ms) {
    DRAGON_DBG("run_timeout: entering NSApp run for %d ms\n", ms);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) ms * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{ dragon__stop_app(); });
    dragon_foreign_enter();
    [NSApp run];
    dragon_foreign_exit();
    DRAGON_DBG("run_timeout: NSApp run returned\n");
}

}
