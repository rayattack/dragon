/// Dragon Runtime - Platform: HTTP, OS, Regex, Crypto, Atomics, Templates
#include "runtime_internal.h"
#include <sys/stat.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#ifdef _WIN32
  // winsock2.h / windows.h come from runtime_internal.h.
  #include <io.h>
  #include <fcntl.h>           // _O_* flags for dragon_create_excl
  #include <direct.h>
  #include <lmcons.h>          // UNLEN for GetUserNameA
  #include <ws2tcpip.h>        // getaddrinfo / inet_ntop for dragon_resolve4
#else
  #include <sys/utsname.h>
  #include <dirent.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>           // getaddrinfo for dragon_resolve4
  #include <fcntl.h>
  #ifdef __APPLE__
    #include <sys/random.h>    // getentropy lives here on macOS, not unistd.h
  #endif
#endif
#include "llhttp.h"
#ifndef _WIN32
  // POSIX <regex.h> isn't available on Windows MinGW. Dragon's stdlib re.dr
  // uses PCRE2 directly via FFI, so the dragon_re_* shim below is POSIX-only.
  #include <regex.h>
#endif

extern "C" {

//===----------------------------------------------------------------------===//
// Socket Helpers (for stdlib/socket.dr)
//===----------------------------------------------------------------------===//

// Forward declaration - the Winsock startup helper lives in runtime_concurrency.cpp.
#ifdef _WIN32
extern "C" void dragon_win_wsa_startup(void);
#endif

/// Create a sockaddr_in struct - Dragon can't pack C structs directly
void* dragon_sockaddr_in_new(int64_t port, const char* addr) {
#ifdef _WIN32
    dragon_win_wsa_startup();
#endif
    struct sockaddr_in* sa = (struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
    memset(sa, 0, sizeof(struct sockaddr_in));
    sa->sin_family = AF_INET;
    sa->sin_port = htons((uint16_t)port);
    if (addr && strcmp(addr, "0.0.0.0") == 0) {
        sa->sin_addr.s_addr = INADDR_ANY;
    } else if (addr) {
        // inet_pton returns 1 only for a well-formed dotted-quad. 0 means the
        // string wasn't a valid address, -1 an address-family error. Ignoring
        // it left sin_addr as the memset 0 - i.e. 0.0.0.0 - so a typo'd or
        // empty host (dragon_resolve4 returns "" on DNS failure) silently
        // became bind-to-all-interfaces for a server or INADDR_ANY for a
        // client. Reject it instead of binding somewhere the caller never asked.
        if (inet_pton(AF_INET, addr, &sa->sin_addr) != 1) {
            free(sa);
            dragon_raise_exc_cstr(50 /* OSError */,
                                  "invalid IPv4 address (host did not resolve?)");
            return nullptr;
        }
    }
    return (void*)sa;
}

/// Resolve a hostname to a dotted IPv4 string (for stdlib/socket.dr).
/// Literal IPv4 addresses pass through unchanged. Returns "" on resolution
/// failure so the caller's connect() fails cleanly instead of dereferencing
/// null. IPv4-only, matching the AF_INET sockaddr_in the socket layer uses.
const char* dragon_resolve4(const char* host) {
    char ipbuf[INET_ADDRSTRLEN];
    const char* ip = host;
    struct in_addr probe;
    if (host && inet_pton(AF_INET, host, &probe) != 1) {
        // Not a literal IPv4 - resolve via DNS.
#ifdef _WIN32
        dragon_win_wsa_startup();
#endif
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = nullptr;
        if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) {
            DragonString* empty = dragon_string_alloc_raw(0);
            empty->data[0] = '\0';
            return empty->data;
        }
        struct sockaddr_in* sa = (struct sockaddr_in*)res->ai_addr;
        inet_ntop(AF_INET, &sa->sin_addr, ipbuf, sizeof(ipbuf));
        freeaddrinfo(res);
        ip = ipbuf;
    }
    size_t n = ip ? strlen(ip) : 0;
    DragonString* ds = dragon_string_alloc_raw((int64_t)n);
    if (n) memcpy(ds->data, ip, n);
    ds->data[n] = '\0';
    return ds->data;
}

// Build a fresh Dragon string from a C string (or "" if null).
static const char* dragon_str_dup(const char* s) {
    size_t n = s ? strlen(s) : 0;
    DragonString* ds = dragon_string_alloc_raw((int64_t)n);
    if (n) memcpy(ds->data, s, n);
    ds->data[n] = '\0';
    return ds->data;
}

/// Default CA trust-store file for ssl.SSLContext.load_default_certs().
/// Precedence: $SSL_CERT_FILE (if present) → first existing well-known system
/// bundle → Dragon's bundled Mozilla cacert.pem. Returns "" if none found.
/// (access(path, 0) is an existence check, portable across POSIX and Windows.)
const char* dragon_default_ca_file() {
    const char* env = getenv("SSL_CERT_FILE");
    if (env && env[0] && access(env, 0) == 0) {
        return dragon_str_dup(env);
    }
    static const char* sys_paths[] = {
        "/etc/ssl/certs/ca-certificates.crt",                 // Debian/Ubuntu/Arch
        "/etc/pki/tls/certs/ca-bundle.crt",                   // RHEL/Fedora/CentOS
        "/etc/ssl/cert.pem",                                  // Alpine/OpenBSD/macOS
        "/etc/ssl/certs/ca-bundle.crt",
        "/usr/local/share/certs/ca-root-nss.crt",             // FreeBSD
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  // RHEL ca-trust
        nullptr
    };
    for (int i = 0; sys_paths[i]; i++) {
        if (access(sys_paths[i], 0) == 0) return dragon_str_dup(sys_paths[i]);
    }
#ifdef DRAGON_CA_BUNDLE
    if (access(DRAGON_CA_BUNDLE, 0) == 0) return dragon_str_dup(DRAGON_CA_BUNDLE);
#endif
    return dragon_str_dup("");
}

/// $SSL_CERT_DIR (a directory of CA certs) or "" if unset. Consumed by
/// ssl.SSLContext.load_default_certs() as a capath.
const char* dragon_default_ca_dir() {
    const char* env = getenv("SSL_CERT_DIR");
    return dragon_str_dup((env && env[0]) ? env : "");
}

int64_t dragon_sockaddr_in_size() {
    return (int64_t)sizeof(struct sockaddr_in);
}

/// Write a 32-bit int at offset in a buffer
void dragon_ptr_write_i32(void* p, int64_t offset, int64_t val) {
    *((int32_t*)((char*)p + offset)) = (int32_t)val;
}

/// Receive data from socket and return as null-terminated string
const char* dragon_recv_to_str(int64_t fd, void* buf, int64_t length, int64_t flags) {
#ifdef _WIN32
    int n = recv((SOCKET)fd, (char*)buf,
                 (int)(length > 0 ? length - 1 : 0), (int)flags);
#else
    ssize_t n = recv((int)fd, buf, (size_t)(length > 0 ? length - 1 : 0), (int)flags);
#endif
    if (n < 0) n = 0;
    ((char*)buf)[n] = '\0';
    return dragon_string_alloc((const char*)buf, (int64_t)n);
}

/// UDP sendto wrapper
int64_t dragon_udp_sendto(int64_t fd, const char* buf, int64_t len,
                          int64_t flags, void* addr, int64_t addrlen) {
#ifdef _WIN32
    return (int64_t)sendto((SOCKET)fd, buf, (int)len, (int)flags,
                           (struct sockaddr*)addr, (int)addrlen);
#else
    return (int64_t)sendto((int)fd, buf, (size_t)len, (int)flags,
                           (struct sockaddr*)addr, (socklen_t)addrlen);
#endif
}

/// Dereference a pointer-to-pointer (read void** → void*)
void* dragon_ptr_deref(void** pp) {
    return pp ? *pp : nullptr;
}


//===----------------------------------------------------------------------===//
// HTTP Parsing (llhttp - bundled Node.js HTTP/1.1 parser)
//===----------------------------------------------------------------------===//

// llhttp.h included at top of file

// Maximum number of headers and max body size for safety
#define DRAGON_HTTP_MAX_HEADERS 64
#define DRAGON_HTTP_MAX_BODY (1024 * 1024)  // 1MB default
// Cap on combined URL + header bytes. Mirrors nginx's
// `large_client_header_buffers` default (4 * 8k = 32k for headers, plus
// request line) - 64 KiB total comfortably covers any legitimate client and
// shuts down request-line/header amplification before realloc growth runs away.
#define DRAGON_HTTP_HEADERS_MAX (64 * 1024)

typedef struct {
    // Method and URL
    char method[16];
    int method_len;
    char* url;
    int url_len;
    int url_cap;
    // Headers (key-value pairs, lowercased keys)
    char* header_keys[DRAGON_HTTP_MAX_HEADERS];
    int   header_key_lens[DRAGON_HTTP_MAX_HEADERS];
    char* header_vals[DRAGON_HTTP_MAX_HEADERS];
    int   header_val_lens[DRAGON_HTTP_MAX_HEADERS];
    int   num_headers;
    int   in_value;  // 1 when accumulating value vs field
    int   header_bytes;  // running sum of URL + header key/value bytes (capped)
    // Body
    char* body;
    int body_len;
    int body_cap;
    // State
    int complete;
    uint8_t http_major;
    uint8_t http_minor;
} HttpParseState;

static int http_on_url(llhttp_t* p, const char* at, size_t len) {
    HttpParseState* s = (HttpParseState*)p->data;
    // Refuse oversize request lines before any growth - caps amplification
    // attacks that try to push the realloc loop to GBs of allocation.
    if (s->header_bytes + (int)len > DRAGON_HTTP_HEADERS_MAX) return -1;
    // Grow URL buffer if needed. Realloc into a temp: on NULL the original
    // buffer is still valid; self-assigning the result would leak the live
    // buffer + NULL-deref on the next memcpy.
    while (s->url_len + (int)len >= s->url_cap) {
        int new_cap = s->url_cap * 2;
        char* tmp = (char*)realloc(s->url, new_cap);
        if (!tmp) { fprintf(stderr, "dragon: out of memory\n"); abort(); }
        s->url = tmp;
        s->url_cap = new_cap;
    }
    memcpy(s->url + s->url_len, at, len);
    s->url_len += (int)len;
    s->url[s->url_len] = '\0';
    s->header_bytes += (int)len;
    return 0;
}

static int http_on_header_field(llhttp_t* p, const char* at, size_t len) {
    HttpParseState* s = (HttpParseState*)p->data;
    if (s->in_value && s->num_headers < DRAGON_HTTP_MAX_HEADERS) {
        // Starting a new header field - advance to next slot
        s->num_headers++;
        s->in_value = 0;
    }
    int idx = s->num_headers;
    if (idx >= DRAGON_HTTP_MAX_HEADERS) return 0;
    if (s->header_bytes + (int)len > DRAGON_HTTP_HEADERS_MAX) return -1;
    int old_len = s->header_key_lens[idx];
    char* tmp = (char*)realloc(s->header_keys[idx], old_len + len + 1);
    if (!tmp) { fprintf(stderr, "dragon: out of memory\n"); abort(); }
    s->header_keys[idx] = tmp;
    // Lowercase the header name during copy
    for (size_t i = 0; i < len; i++) {
        s->header_keys[idx][old_len + i] = (at[i] >= 'A' && at[i] <= 'Z')
            ? at[i] + 32 : at[i];
    }
    s->header_key_lens[idx] += (int)len;
    s->header_keys[idx][s->header_key_lens[idx]] = '\0';
    s->header_bytes += (int)len;
    return 0;
}

static int http_on_header_value(llhttp_t* p, const char* at, size_t len) {
    HttpParseState* s = (HttpParseState*)p->data;
    s->in_value = 1;
    int idx = s->num_headers;
    if (idx >= DRAGON_HTTP_MAX_HEADERS) return 0;
    if (s->header_bytes + (int)len > DRAGON_HTTP_HEADERS_MAX) return -1;
    int old_len = s->header_val_lens[idx];
    char* tmp = (char*)realloc(s->header_vals[idx], old_len + len + 1);
    if (!tmp) { fprintf(stderr, "dragon: out of memory\n"); abort(); }
    s->header_vals[idx] = tmp;
    memcpy(s->header_vals[idx] + old_len, at, len);
    s->header_val_lens[idx] += (int)len;
    s->header_vals[idx][s->header_val_lens[idx]] = '\0';
    s->header_bytes += (int)len;
    return 0;
}

static int http_on_body(llhttp_t* p, const char* at, size_t len) {
    HttpParseState* s = (HttpParseState*)p->data;
    if (s->body_len + (int)len > DRAGON_HTTP_MAX_BODY) return -1; // too large
    while (s->body_len + (int)len >= s->body_cap) {
        int new_cap = s->body_cap * 2;
        if (new_cap > DRAGON_HTTP_MAX_BODY) new_cap = DRAGON_HTTP_MAX_BODY + 1;
        char* tmp = (char*)realloc(s->body, new_cap);
        if (!tmp) { fprintf(stderr, "dragon: out of memory\n"); abort(); }
        s->body = tmp;
        s->body_cap = new_cap;
    }
    memcpy(s->body + s->body_len, at, len);
    s->body_len += (int)len;
    s->body[s->body_len] = '\0';
    return 0;
}

static int http_on_message_complete(llhttp_t* p) {
    HttpParseState* s = (HttpParseState*)p->data;
    // Count the last header if we were accumulating a value
    if (s->in_value && s->num_headers < DRAGON_HTTP_MAX_HEADERS) {
        s->num_headers++;
    }
    s->complete = 1;
    s->http_major = p->http_major;
    s->http_minor = p->http_minor;
    return 0;
}

/// Parse an HTTP/1.1 request buffer. Returns opaque handle to parsed state.
/// Caller must free with dragon_http_parsed_free().
void* dragon_http_parse_request(const char* buf, int64_t len) {
    HttpParseState* state = (HttpParseState*)calloc(1, sizeof(HttpParseState));
    state->url_cap = 256;
    state->url = (char*)malloc(state->url_cap);
    state->url[0] = '\0';
    state->body_cap = 1024;
    state->body = (char*)malloc(state->body_cap);
    state->body[0] = '\0';

    llhttp_settings_t settings;
    memset(&settings, 0, sizeof(settings));
    settings.on_url           = http_on_url;
    settings.on_header_field  = http_on_header_field;
    settings.on_header_value  = http_on_header_value;
    settings.on_body          = http_on_body;
    settings.on_message_complete = http_on_message_complete;

    llhttp_t parser;
    llhttp_init(&parser, HTTP_REQUEST, &settings);
    parser.data = state;

    llhttp_errno_t err = llhttp_execute(&parser, buf, (size_t)len);
    if (err != HPE_OK && !state->complete) {
        // Parse failed - store method as empty to signal error
        state->method[0] = '\0';
        state->method_len = 0;
    } else {
        // Copy method name
        const char* m = llhttp_method_name((llhttp_method_t)parser.method);
        int mlen = (int)strlen(m);
        if (mlen > 15) mlen = 15;
        memcpy(state->method, m, mlen);
        state->method[mlen] = '\0';
        state->method_len = mlen;
    }
    return state;
}

/// Get the HTTP method as a Dragon string
const char* dragon_http_parsed_method(void* handle) {
    HttpParseState* s = (HttpParseState*)handle;
    return dragon_string_alloc(s->method, s->method_len);
}

/// Get the URL/path as a Dragon string
const char* dragon_http_parsed_url(void* handle) {
    HttpParseState* s = (HttpParseState*)handle;
    return dragon_string_alloc(s->url, s->url_len);
}

/// Get the request body as a Dragon string
const char* dragon_http_parsed_body(void* handle) {
    HttpParseState* s = (HttpParseState*)handle;
    return dragon_string_alloc(s->body, s->body_len);
}

/// Get the number of headers
int64_t dragon_http_parsed_header_count(void* handle) {
    HttpParseState* s = (HttpParseState*)handle;
    return s->num_headers;
}

/// Get a header key by index as a Dragon string
const char* dragon_http_parsed_header_key(void* handle, int64_t idx) {
    HttpParseState* s = (HttpParseState*)handle;
    if (idx < 0 || idx >= s->num_headers) return dragon_string_alloc("", 0);
    return dragon_string_alloc(s->header_keys[idx], s->header_key_lens[idx]);
}

/// Get a header value by index as a Dragon string
const char* dragon_http_parsed_header_value(void* handle, int64_t idx) {
    HttpParseState* s = (HttpParseState*)handle;
    if (idx < 0 || idx >= s->num_headers) return dragon_string_alloc("", 0);
    return dragon_string_alloc(s->header_vals[idx], s->header_val_lens[idx]);
}

/// Check if parsing completed successfully
int64_t dragon_http_parsed_ok(void* handle) {
    HttpParseState* s = (HttpParseState*)handle;
    return s->complete ? 1 : 0;
}

/// Get HTTP version as "1.0" or "1.1"
const char* dragon_http_parsed_version(void* handle) {
    HttpParseState* s = (HttpParseState*)handle;
    if (s->http_minor == 0) return dragon_string_alloc("1.0", 3);
    return dragon_string_alloc("1.1", 3);
}

/// Free the parsed request state
void dragon_http_parsed_free(void* handle) {
    HttpParseState* s = (HttpParseState*)handle;
    if (!s) return;
    free(s->url);
    free(s->body);
    // Free ALL header slots, not just [0, num_headers). num_headers only
    // counts *committed* headers; on a request that never reaches
    // on_message_complete (a malformed or half-received request - remotely
    // triggerable), http_on_header_field has already realloc'd the key (and
    // maybe the value) of the IN-FLIGHT slot at index num_headers, which the
    // old `i < num_headers` loop skipped. A flood of such requests leaked that
    // slot every time -> unbounded server RSS growth. Unused slots are NULL
    // (the state was calloc'd) and free(NULL) is a no-op, so scanning the full
    // fixed array is safe and closes the leak regardless of which callback ran last.
    for (int i = 0; i < DRAGON_HTTP_MAX_HEADERS; i++) {
        free(s->header_keys[i]);
        free(s->header_vals[i]);
    }
    free(s);
}

/// Build an HTTP response string: "HTTP/1.1 {status} {reason}\r\n{headers}\r\n\r\n{body}"
///
/// The wire is bytes, not code points: the body may contain non-ASCII text
/// (em-dashes, accents, etc.) and must travel as UTF-8 with a byte-accurate
/// content-length. Two boundary issues we handle here:
///
/// 1) A Dragon kind=4 (UCS-4) body has zero bytes mid-buffer (each ASCII
///    char is stored as 4 bytes with three leading zeros). `strlen` on it
///    truncates at the first NUL - so we encode through `dragon_str_to_utf8_alloc`
///    to recover real UTF-8 bytes and the real byte length.
/// 2) The returned response Dragon string must be kind=1 so the caller's
///    `len(response_str)` (cp count) equals the wire byte count and the
///    nb_send loop transmits exactly the right number of bytes. We allocate
///    via `dragon_string_alloc_raw` (always kind=1) and memcpy the bytes.
const char* dragon_http_build_response(int64_t status, const char* headers, const char* body) {
    const char* reason;
    switch (status) {
        // 1xx informational. 101 is the WebSocket / HTTP-upgrade handshake
        // (RFC 6455 §4.2.2): the server must echo the exact "Switching
        // Protocols" reason phrase. The caller pairs this with a header block
        // that carries no content-length and an empty body (Response._no_body).
        case 101: reason = "Switching Protocols"; break;
        case 200: reason = "OK"; break;
        case 201: reason = "Created"; break;
        case 204: reason = "No Content"; break;
        case 301: reason = "Moved Permanently"; break;
        case 302: reason = "Found"; break;
        case 304: reason = "Not Modified"; break;
        case 307: reason = "Temporary Redirect"; break;
        case 308: reason = "Permanent Redirect"; break;
        case 400: reason = "Bad Request"; break;
        case 401: reason = "Unauthorized"; break;
        case 403: reason = "Forbidden"; break;
        case 404: reason = "Not Found"; break;
        case 405: reason = "Method Not Allowed"; break;
        case 409: reason = "Conflict"; break;
        case 413: reason = "Payload Too Large"; break;
        case 422: reason = "Unprocessable Entity"; break;
        // 426 is the RFC 6455 §4.4 WebSocket version-negotiation reject - the
        // server advertises the versions it speaks (13) and refuses the upgrade.
        case 426: reason = "Upgrade Required"; break;
        case 429: reason = "Too Many Requests"; break;
        case 500: reason = "Internal Server Error"; break;
        case 502: reason = "Bad Gateway"; break;
        case 503: reason = "Service Unavailable"; break;
        default:  reason = "Unknown"; break;
    }
    // Encode body to UTF-8 bytes. For kind=1 Dragon strings (or string
    // literals) `dragon_str_to_utf8_alloc` returns NULL and reports the
    // existing byte count - no copy needed; the data ptr is already UTF-8.
    int64_t body_len = 0;
    char* body_owned = NULL;
    const char* body_bytes = NULL;
    if (body) {
        body_owned = dragon_str_to_utf8_alloc(body, &body_len);
        body_bytes = body_owned ? body_owned : body;
    }
    // Headers are emitted by the stdlib HTTP server as ASCII (status names,
    // numeric content-length, header keys/values), so kind=1; treating them
    // as a NUL-terminated C string for the prefix snprintf is safe.
    int prefix_len = snprintf(NULL, 0, "HTTP/1.1 %d %s\r\n%s\r\n",
                              (int)status, reason, headers ? headers : "");
    if (prefix_len < 0) {
        if (body_owned) free(body_owned);
        return dragon_string_alloc("", 0);
    }
    int64_t total = (int64_t)prefix_len + body_len;
    DragonString* out = dragon_string_alloc_raw(total);
    int off = snprintf(out->data, (size_t)prefix_len + 1, "HTTP/1.1 %d %s\r\n%s\r\n",
                       (int)status, reason, headers ? headers : "");
    if (off < 0) off = 0;
    if (body_bytes && body_len > 0) {
        memcpy(out->data + off, body_bytes, (size_t)body_len);
        off += (int)body_len;
    }
    out->data[off] = '\0';
    out->len = off;
    if (body_owned) free(body_owned);
    return out->data;
}

// NOTE: Crypto digests (dragon_sha256/sha1/md5[/_bytes], dragon_hmac,
// dragon_raw_bytes_hex) moved to lib/Runtime/runtime_crypto.cpp (ADR 038
// Phase 7) so their mbedtls_* dependency is pulled only when a program
// actually hashes. dragon_urandom stays here - it's pure getrandom, no mbedTLS.

// HashDoS defense: per-process random key for the dict/set SipHash-1-3.
// Seeded once at startup (constructor below) from the OS CSPRNG so an attacker
// cannot precompute colliding keys. Defined here because the OS entropy source
// already lives in this TU.
uint64_t __dragon_hash_k0 = 0;
uint64_t __dragon_hash_k1 = 0;

// Fill `buf` with `n` bytes from the OS CSPRNG. Returns bytes obtained. Shares
// the platform entropy paths used by dragon_urandom below.
static int64_t dragon_fill_os_random(unsigned char* buf, int64_t n) {
    int64_t got = 0;
#ifdef _WIN32
    extern "C" {
        long __stdcall BCryptGenRandom(void* hAlgorithm, unsigned char* pbBuffer,
                                       unsigned long cbBuffer, unsigned long dwFlags);
    }
    if (BCryptGenRandom(nullptr, buf, (unsigned long)n, 2) >= 0) got = n;
#else
    #if defined(__linux__)
    while (got < n) {
        long r = syscall(318L, buf + got, (size_t)(n - got), 0u);
        if (r <= 0) break;
        got += r;
    }
    #elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    while (got < n) {
        size_t chunk = (size_t)(n - got);
        if (chunk > 256) chunk = 256;
        if (getentropy(buf + got, chunk) == 0) got += (int64_t)chunk; else break;
    }
    #endif
    if (got < n) {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            while (got < n) {
                ssize_t r = read(fd, buf + got, (size_t)(n - got));
                if (r <= 0) break;
                got += r;
            }
            close(fd);
        }
    }
#endif
    return got;
}

void dragon_hash_secret_init(void) {
    if (__dragon_hash_k0 || __dragon_hash_k1) return;  // already seeded
    unsigned char seed[16];
    if (dragon_fill_os_random(seed, 16) == 16) {
        __dragon_hash_k0 = dragon_hash_read_le64(seed);
        __dragon_hash_k1 = dragon_hash_read_le64(seed + 8);
    } else {
        // CSPRNG unavailable at startup (sandbox without /dev/urandom): fall
        // back to address-space + pid mixing. Weaker than the CSPRNG but still
        // unpredictable to a remote attacker who cannot observe these values,
        // and far better than a hard-coded key. Guaranteed nonzero.
        uintptr_t a = (uintptr_t)&__dragon_hash_k0;
        uintptr_t b = (uintptr_t)(intptr_t)getpid();
        __dragon_hash_k0 = (uint64_t)a * 0x9E3779B97F4A7C15ULL ^ 0xD1B54A32D192ED03ULL;
        __dragon_hash_k1 = ((uint64_t)b ^ (uint64_t)a) * 0xC2B2AE3D27D4EB4FULL | 1ULL;
    }
}

__attribute__((constructor))
static void dragon_hash_secret_ctor(void) { dragon_hash_secret_init(); }

/// Cryptographically-secure random bytes. Returns `n` bytes from the OS CSPRNG.
/// On Linux/BSD this is getrandom(2); on macOS getentropy(3); on Windows
/// BCryptGenRandom. Falls back to /dev/urandom if the modern syscalls are
/// unavailable (older kernels, sandboxed environments).
///
/// If every source fails to fill the buffer, raise OSError. Mirrors CPython's
/// os.urandom contract: callers MUST be able to assume returned bytes came
/// from the kernel CSPRNG. Returning a partially-filled buffer would leak
/// uninitialized heap memory into "cryptographically secure" tokens.
DragonBytes* dragon_urandom(int64_t n) {
    if (n <= 0) return dragon_bytes_new(nullptr, 0);
    auto* buf = (uint8_t*)malloc((size_t)n);
    int64_t got = 0;
#ifdef _WIN32
    extern "C" {
        long __stdcall BCryptGenRandom(void* hAlgorithm, unsigned char* pbBuffer,
                                       unsigned long cbBuffer, unsigned long dwFlags);
    }
    long st = BCryptGenRandom(nullptr, buf, (unsigned long)n,
                              2 /* BCRYPT_USE_SYSTEM_PREFERRED_RNG */);
    if (st >= 0) got = n;
#else
    #if defined(__linux__)
    while (got < n) {
        // SYS_getrandom = 318 on x86_64 Linux. Use the syscall directly so
        // we don't pull in glibc 2.25+ symbols (older base distros lack them).
        long r = syscall(318L, buf + got, (size_t)(n - got), 0u);
        if (r <= 0) break;
        got += r;
    }
    #elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    while (got < n) {
        size_t chunk = (size_t)(n - got);
        if (chunk > 256) chunk = 256;     // getentropy max
        if (getentropy(buf + got, chunk) == 0) got += (int64_t)chunk;
        else break;
    }
    #endif
    if (got < n) {
        // Fallback path: /dev/urandom
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            while (got < n) {
                ssize_t r = read(fd, buf + got, (size_t)(n - got));
                if (r <= 0) break;
                got += r;
            }
            close(fd);
        }
    }
#endif
    if (got < n) {
        free(buf);
        dragon_raise_exc_cstr(50 /* OSError */, "os.urandom: kernel CSPRNG unavailable");
        return nullptr;
    }
    auto* b = dragon_bytes_new(buf, n);
    free(buf);
    return b;
}

//===----------------------------------------------------------------------===//
// Regex (for stdlib/re.dr) - POSIX regex, no external dependencies
//
// stdlib/re.dr uses PCRE2 directly via FFI on all platforms. This thin POSIX
// wrapper exists for FFI users who'd rather call regex.h-style functions; it's
// only available on platforms with <regex.h>. On Windows MinGW (which lacks
// POSIX regex), every dragon_re_* below is replaced with a stub returning the
// "no match" sentinel so any unintentional caller gets a clean failure.
//===----------------------------------------------------------------------===//

#ifndef _WIN32

/// Compile a POSIX extended regex - returns heap-allocated regex_t*
void* dragon_re_compile(const char* pattern) {
    regex_t* re = (regex_t*)malloc(sizeof(regex_t));
    int rc = regcomp(re, pattern, REG_EXTENDED);
    if (rc != 0) {
        free(re);
        return nullptr;
    }
    return re;
}

/// Match: returns number of matches (>0 if match, 0 if no match)
int64_t dragon_re_match(void* compiled, const char* subject) {
    if (!compiled) return 0;
    regex_t* re = (regex_t*)compiled;
    regmatch_t match[10];
    int rc = regexec(re, subject, 10, match, 0);
    if (rc != 0) return 0;
    // Count how many groups matched
    int count = 0;
    for (int i = 0; i < 10; i++) {
        if (match[i].rm_so >= 0) count++;
        else break;
    }
    return count;
}

/// Search: find first match and return it as a string
const char* dragon_re_search(void* compiled, const char* subject) {
    if (!compiled) return dragon_string_alloc("", 0);
    regex_t* re = (regex_t*)compiled;
    regmatch_t match[1];
    int rc = regexec(re, subject, 1, match, 0);
    if (rc != 0 || match[0].rm_so < 0) return dragon_string_alloc("", 0);
    int len = match[0].rm_eo - match[0].rm_so;
    return dragon_string_alloc(subject + match[0].rm_so, (int64_t)len);
}

/// Group: extract a specific capture group from a match
const char* dragon_re_group(void* compiled, const char* subject, int64_t index) {
    if (!compiled) return dragon_string_alloc("", 0);
    regex_t* re = (regex_t*)compiled;
    regmatch_t matches[10];
    int rc = regexec(re, subject, 10, matches, 0);
    if (rc != 0 || index >= 10 || matches[index].rm_so < 0) return dragon_string_alloc("", 0);
    int len = matches[index].rm_eo - matches[index].rm_so;
    return dragon_string_alloc(subject + matches[index].rm_so, (int64_t)len);
}

/// Free a compiled regex
void dragon_re_free(void* compiled) {
    if (compiled) {
        regfree((regex_t*)compiled);
        free(compiled);
    }
}

/// Convenience: match without pre-compiled pattern
int64_t dragon_re_match_str(const char* pattern, const char* subject) {
    void* re = dragon_re_compile(pattern);
    int64_t result = dragon_re_match(re, subject);
    dragon_re_free(re);
    return result;
}

/// Convenience: search without pre-compiled pattern
const char* dragon_re_search_str(const char* pattern, const char* subject) {
    void* re = dragon_re_compile(pattern);
    const char* result = dragon_re_search(re, subject);
    dragon_re_free(re);
    return result;
}

#else  // _WIN32 - stub regex.h API

void*       dragon_re_compile(const char*) { return nullptr; }
int64_t     dragon_re_match(void*, const char*) { return 0; }
const char* dragon_re_search(void*, const char*) { return dragon_string_alloc("", 0); }
const char* dragon_re_group(void*, const char*, int64_t) { return dragon_string_alloc("", 0); }
void        dragon_re_free(void*) {}
int64_t     dragon_re_match_str(const char*, const char*) { return 0; }
const char* dragon_re_search_str(const char*, const char*) { return dragon_string_alloc("", 0); }

#endif // _WIN32

/// Extract match from PCRE2 ovector (size_t* pairs: [start, end, ...])
const char* dragon_re_get_match(const char* subject, int64_t* ovector, int64_t index) {
    int64_t start = ovector[index * 2];
    int64_t end = ovector[index * 2 + 1];
    if (start < 0 || end < start) return dragon_string_alloc("", 0);
    int64_t len = end - start;
    return dragon_string_alloc(subject + start, len);
}

//===----------------------------------------------------------------------===//
// OS Helpers - stat, readdir
//===----------------------------------------------------------------------===//

// On Windows MinGW we use _stat (the MSVCRT 32-bit time variant). MSYS2 also
// provides stat(); the cast types are compatible since we only read st_mode
// and *_time fields back as int64_t.
#ifdef _WIN32
typedef struct _stat dragon_stat_t;
static int dragon_stat(const char* path, dragon_stat_t* st) { return _stat(path, st); }
#else
typedef struct stat dragon_stat_t;
static int dragon_stat(const char* path, dragon_stat_t* st) { return stat(path, st); }
#endif

#ifdef _WIN32
  #ifndef S_ISREG
    #define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
  #endif
  #ifndef S_ISDIR
    #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
  #endif
  // Windows has no canonical symlink mode bit accessible via _stat; treat as
  // "not a link". (Junctions and symlinks need GetFileAttributesW for proper
  // detection - defer to a future revision.)
#endif

/// Get file size via stat(2)
int64_t dragon_stat_size(const char* path) {
    dragon_stat_t st;
    if (dragon_stat(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

/// Get file modification time via stat(2)
int64_t dragon_stat_mtime(const char* path) {
    dragon_stat_t st;
    if (dragon_stat(path, &st) != 0) return -1;
    return (int64_t)st.st_mtime;
}

/// Check if path is a regular file
int32_t dragon_stat_isfile(const char* path) {
    dragon_stat_t st;
    if (dragon_stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) ? 1 : 0;
}

/// Check if path is a directory
int32_t dragon_stat_isdir(const char* path) {
    dragon_stat_t st;
    if (dragon_stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

/// Read next directory entry name. Returns "" when done.
const char* dragon_readdir_name(void* dirp) {
#ifdef _WIN32
    // Caller passes a HANDLE returned by FindFirstFileA wrapped via
    // dragon_opendir (see below). The handle's data is appended after the
    // HANDLE in a small struct.
    typedef struct { HANDLE h; WIN32_FIND_DATAA data; int first; } DragonDir;
    DragonDir* d = (DragonDir*)dirp;
    if (!d) return dragon_string_alloc("", 0);
    if (d->first) {
        d->first = 0;
        return dragon_string_alloc(d->data.cFileName,
                                   (int64_t)strlen(d->data.cFileName));
    }
    if (!FindNextFileA(d->h, &d->data)) return dragon_string_alloc("", 0);
    return dragon_string_alloc(d->data.cFileName,
                               (int64_t)strlen(d->data.cFileName));
#else
    struct dirent* entry = readdir((DIR*)dirp);
    if (!entry) return dragon_string_alloc("", 0);
    return dragon_string_alloc(entry->d_name, (int64_t)strlen(entry->d_name));
#endif
}

/// Check if path is a symbolic link (uses lstat, not stat)
int32_t dragon_stat_islink(const char* path) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return 0;
    return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) ? 1 : 0;
#else
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    return S_ISLNK(st.st_mode) ? 1 : 0;
#endif
}

/// Get file access time via stat(2)
int64_t dragon_stat_atime(const char* path) {
    dragon_stat_t st;
    if (dragon_stat(path, &st) != 0) return -1;
    return (int64_t)st.st_atime;
}

/// Get file status change time via stat(2)
int64_t dragon_stat_ctime(const char* path) {
    dragon_stat_t st;
    if (dragon_stat(path, &st) != 0) return -1;
    return (int64_t)st.st_ctime;
}

/// Get file mode bits via stat(2)
int64_t dragon_stat_mode(const char* path) {
    dragon_stat_t st;
    if (dragon_stat(path, &st) != 0) return -1;
    return (int64_t)st.st_mode;
}

/// Get device id (st_dev) via stat(2). Used by samefile() to detect when two
/// paths refer to the same physical filesystem location.
int64_t dragon_stat_dev(const char* path) {
    dragon_stat_t st;
    if (dragon_stat(path, &st) != 0) return -1;
    return (int64_t)st.st_dev;
}

/// Get inode number (st_ino) via stat(2). Used by samefile().
int64_t dragon_stat_ino(const char* path) {
    dragon_stat_t st;
    if (dragon_stat(path, &st) != 0) return -1;
    return (int64_t)st.st_ino;
}

//===----------------------------------------------------------------------===//
// lstat() - like stat() but does not follow symbolic links. Used by lstat()
// stdlib helpers and by ismount() (which compares dev/ino across `path` and
// `path/..` without following symlinks at the boundary).
//
// On Windows there is no lstat(); we fall through to the regular dragon_stat
// since symbolic-link semantics require DeviceIoControl which is deferred.
// dragon_stat_islink() above already documents this asymmetry.
//===----------------------------------------------------------------------===//

#ifdef _WIN32
static int dragon_lstat(const char* path, dragon_stat_t* st) { return _stat(path, st); }
#else
typedef struct stat dragon_lstat_t;
static int dragon_lstat(const char* path, dragon_lstat_t* st) { return lstat(path, st); }
#endif

#ifndef _WIN32
int64_t dragon_lstat_size(const char* path) {
    dragon_lstat_t st;
    if (dragon_lstat(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
}
int64_t dragon_lstat_mtime(const char* path) {
    dragon_lstat_t st;
    if (dragon_lstat(path, &st) != 0) return -1;
    return (int64_t)st.st_mtime;
}
int64_t dragon_lstat_atime(const char* path) {
    dragon_lstat_t st;
    if (dragon_lstat(path, &st) != 0) return -1;
    return (int64_t)st.st_atime;
}
int64_t dragon_lstat_ctime(const char* path) {
    dragon_lstat_t st;
    if (dragon_lstat(path, &st) != 0) return -1;
    return (int64_t)st.st_ctime;
}
int64_t dragon_lstat_mode(const char* path) {
    dragon_lstat_t st;
    if (dragon_lstat(path, &st) != 0) return -1;
    return (int64_t)st.st_mode;
}
int32_t dragon_lstat_isfile(const char* path) {
    dragon_lstat_t st;
    if (dragon_lstat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) ? 1 : 0;
}
int32_t dragon_lstat_isdir(const char* path) {
    dragon_lstat_t st;
    if (dragon_lstat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}
int64_t dragon_lstat_dev(const char* path) {
    dragon_lstat_t st;
    if (dragon_lstat(path, &st) != 0) return -1;
    return (int64_t)st.st_dev;
}
int64_t dragon_lstat_ino(const char* path) {
    dragon_lstat_t st;
    if (dragon_lstat(path, &st) != 0) return -1;
    return (int64_t)st.st_ino;
}
#else
// Windows: lstat semantics aren't available without DeviceIoControl
// reparse-point handling. Fall back to stat() so symlink-aware code at least
// produces consistent results (Dragon's symlink support on Windows is itself
// stubbed - see dragon_stat_islink above).
int64_t dragon_lstat_size(const char* path)  { return dragon_stat_size(path);  }
int64_t dragon_lstat_mtime(const char* path) { return dragon_stat_mtime(path); }
int64_t dragon_lstat_atime(const char* path) { return dragon_stat_atime(path); }
int64_t dragon_lstat_ctime(const char* path) { return dragon_stat_ctime(path); }
int64_t dragon_lstat_mode(const char* path)  { return dragon_stat_mode(path);  }
int32_t dragon_lstat_isfile(const char* path){ return dragon_stat_isfile(path);}
int32_t dragon_lstat_isdir(const char* path) { return dragon_stat_isdir(path); }
int64_t dragon_lstat_dev(const char* path)   { return dragon_stat_dev(path);   }
int64_t dragon_lstat_ino(const char* path)   { return dragon_stat_ino(path);   }
#endif

//===----------------------------------------------------------------------===//
// chown / chroot - POSIX ownership and root-dir change. Stubbed on Windows
// since neither maps cleanly to the NT security model (would require ACL
// rewriting via SetSecurityInfo, and chroot has no equivalent at all).
//===----------------------------------------------------------------------===//

int32_t dragon_chown(const char* path, int32_t uid, int32_t gid) {
#ifdef _WIN32
    (void)path; (void)uid; (void)gid;
    errno = EINVAL;
    return -1;
#else
    return chown(path, (uid_t)uid, (gid_t)gid);
#endif
}

int32_t dragon_chroot(const char* path) {
#ifdef _WIN32
    (void)path;
    errno = EINVAL;
    return -1;
#else
    return chroot(path);
#endif
}

//===----------------------------------------------------------------------===//
// Process management helpers - only the bits Dragon FFI cannot express
// directly. The plain int→int syscalls (fork, umask, getpgrp, setsid,
// setpgid) are FFI'd from os.dr without a wrapper.
//===----------------------------------------------------------------------===//

#ifndef _WIN32
  #include <sys/wait.h>
#endif

// execv / execvp need to materialize a NULL-terminated `char**` argv vector
// from a Dragon list[str]. That allocation can't be expressed in Dragon, so
// the wrapper lives here.
int32_t dragon_execv(const char* path, DragonList* argv) {
#ifdef _WIN32
    (void)path; (void)argv;
    errno = EINVAL;
    return -1;
#else
    int n = (int)dragon_list_len(argv);
    char** args = (char**)malloc((size_t)(n + 1) * sizeof(char*));
    char** owned = (char**)calloc((size_t)(n > 0 ? n : 1), sizeof(char*));
    if (!args || !owned) { free(args); free(owned); errno = ENOMEM; return -1; }
    // Encode each arg to UTF-8: a UCS-4 DragonString handed raw to execv would
    // reach the new image as wide chars, silently corrupting a non-ASCII arg.
    // dragon_str_to_utf8_alloc returns NULL when the raw pointer is already
    // UTF-8 (borrow it), else a fresh buffer we own.
    for (int i = 0; i < n; i++) {
        const char* raw = (const char*)(uintptr_t)dragon_list_load(argv, i);
        int64_t blen = 0;
        char* enc = dragon_str_to_utf8_alloc(raw, &blen);
        if (enc) { owned[i] = enc; args[i] = enc; }
        else     { args[i] = (char*)(uintptr_t)raw; }
    }
    args[n] = NULL;
    int rc = execv(path, args);
    // Only reached if execv FAILED (success replaces the process image).
    for (int i = 0; i < n; i++) free(owned[i]);
    free(owned);
    free(args);
    return (int32_t)rc;
#endif
}

int32_t dragon_execvp(const char* file, DragonList* argv) {
#ifdef _WIN32
    (void)file; (void)argv;
    errno = EINVAL;
    return -1;
#else
    int n = (int)dragon_list_len(argv);
    char** args = (char**)malloc((size_t)(n + 1) * sizeof(char*));
    char** owned = (char**)calloc((size_t)(n > 0 ? n : 1), sizeof(char*));
    if (!args || !owned) { free(args); free(owned); errno = ENOMEM; return -1; }
    // See dragon_execv: encode UCS-4 args to UTF-8 so a non-ASCII argument is
    // not truncated at its first embedded NUL when the new image reads argv.
    for (int i = 0; i < n; i++) {
        const char* raw = (const char*)(uintptr_t)dragon_list_load(argv, i);
        int64_t blen = 0;
        char* enc = dragon_str_to_utf8_alloc(raw, &blen);
        if (enc) { owned[i] = enc; args[i] = enc; }
        else     { args[i] = (char*)(uintptr_t)raw; }
    }
    args[n] = NULL;
    int rc = execvp(file, args);
    // Only reached if execvp FAILED (success replaces the process image).
    for (int i = 0; i < n; i++) free(owned[i]);
    free(owned);
    free(args);
    return (int32_t)rc;
#endif
}

// waitpid takes an out-parameter `int* status` that Dragon can't pass. We
// return [pid, status] as a list[int]; on error the list is [-1, errno] so
// the Dragon wrapper raises OSError uniformly.
DragonList* dragon_waitpid(int32_t pid, int32_t options) {
    DragonList* result = dragon_list_new_tagged(2, TAG_INT);
#ifdef _WIN32
    (void)pid; (void)options;
    dragon_list_append(result, -1);
    dragon_list_append(result, EINVAL);
#else
    int status = 0;
    pid_t r = waitpid((pid_t)pid, &status, (int)options);
    if (r < 0) {
        dragon_list_append(result, -1);
        dragon_list_append(result, errno);
    } else {
        dragon_list_append(result, (int64_t)r);
        dragon_list_append(result, (int64_t)status);
    }
#endif
    return result;
}

/// Read symbolic link target. Returns DragonString, "" on error.
const char* dragon_readlink(const char* path) {
#ifdef _WIN32
    // Real symlink resolution on Windows requires DeviceIoControl with
    // FSCTL_GET_REPARSE_POINT. Defer; the os.path stdlib paths primarily
    // use this for stat checks where a "" stub is acceptable.
    (void)path;
    return dragon_string_alloc("", 0);
#else
    char buf[4096];
    ssize_t len = readlink(path, buf, sizeof(buf) - 1);
    if (len < 0) return dragon_string_alloc("", 0);
    buf[len] = '\0';
    return dragon_string_alloc(buf, (int64_t)len);
#endif
}

/// Canonicalize a path into a fresh Dragon string. Replaces the old raw
/// `extern realpath` binding: libc realpath returns a `malloc`'d char* (or
/// writes into a caller buffer), which the runtime would otherwise treat as a
/// DragonString*, reading a bogus 24-byte-back "header" on decref - heap
/// corruption on every os.path.abspath/realpath call. Here we copy the libc
/// result into a real DragonString and free the libc buffer.
///
/// On failure (a path component does not exist) we fall back to a LEXICAL
/// absolute path so abspath() of a not-yet-created file still returns an
/// absolute path (Python's abspath does not require existence). The fallback
/// does not resolve symlinks; callers needing containment must compare resolved
/// prefixes only when the file exists.
const char* dragon_realpath(const char* path) {
    if (!path) return dragon_string_alloc("", 0);
#ifdef _WIN32
    char* resolved = _fullpath(nullptr, path, 0);  // malloc'd
    if (resolved) {
        const char* out = dragon_string_alloc(resolved, (int64_t)strlen(resolved));
        free(resolved);
        return out;
    }
    return dragon_string_alloc(path, (int64_t)strlen(path));
#else
    char* resolved = realpath(path, nullptr);  // POSIX.1-2008: NULL -> malloc
    if (resolved) {
        const char* out = dragon_string_alloc(resolved, (int64_t)strlen(resolved));
        free(resolved);
        return out;
    }
    if (path[0] == '/') return dragon_string_alloc(path, (int64_t)strlen(path));
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd))) {
        size_t cl = strlen(cwd), pl = strlen(path);
        char* joined = (char*)malloc(cl + 1 + pl + 1);
        memcpy(joined, cwd, cl);
        joined[cl] = '/';
        memcpy(joined + cl + 1, path, pl);
        joined[cl + 1 + pl] = '\0';
        const char* out = dragon_string_alloc(joined, (int64_t)(cl + 1 + pl));
        free(joined);
        return out;
    }
    return dragon_string_alloc(path, (int64_t)strlen(path));
#endif
}

/// Atomically create a brand-new file at `path` for tempfile.mkstemp. Opens
/// with O_CREAT|O_EXCL (fails if it already exists, closing the TOCTOU the old
/// _exists()-then-fopen had) | O_NOFOLLOW (refuses to follow a symlink an
/// attacker pre-planted at the name, so we can't be tricked into truncating a
/// victim file) and mode 0600 (owner-only, vs fopen's umask-default 0644).
/// Returns 0 on success, -1 if the file exists or on any error. The fd is
/// closed here; the stdlib returns the path (its public contract).
int32_t dragon_create_excl(const char* path) {
    if (!path) return -1;
#ifdef _WIN32
    // Windows: _O_EXCL|_O_CREAT is atomic; no symlink-follow concept for the
    // classic CRT open. _S_IREAD|_S_IWRITE ~= 0600.
    int fd = _open(path, _O_RDWR | _O_CREAT | _O_EXCL | _O_BINARY,
                   _S_IREAD | _S_IWRITE);
#else
    int flags = O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC;
    #ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
    #endif
    int fd = open(path, flags, 0600);
#endif
    if (fd < 0) return -1;
#ifdef _WIN32
    _close(fd);
#else
    close(fd);
#endif
    return 0;
}

/// Recursive mkdir -p. Returns 0 on success, -1 on error.
int32_t dragon_makedirs(const char* path, int32_t mode) {
#ifdef _WIN32
    (void)mode;  // POSIX mode bits don't translate cleanly to Windows ACLs.
    if (_mkdir(path) == 0) return 0;
    if (errno == EEXIST) {
        dragon_stat_t st;
        if (dragon_stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
        return -1;
    }
    if (errno != ENOENT) return -1;
    char* tmp = _strdup(path);
    // Find the last separator (either / or \)
    char* slash = NULL;
    for (char* p = tmp + strlen(tmp); p > tmp; --p) {
        if (*p == '/' || *p == '\\') { slash = p; break; }
    }
    if (slash && slash != tmp) {
        *slash = '\0';
        if (dragon_makedirs(tmp, mode) != 0) { free(tmp); return -1; }
    }
    free(tmp);
    if (_mkdir(path) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
#else
    // Confirm an EEXIST path is a REAL directory, not a symlink an attacker
    // pre-planted to redirect later writes (audit 2.12). lstat, not stat:
    // stat() follows the link and reports the target's type, so a symlink->dir
    // would pass and makedirs would hand back success for a path that resolves
    // outside the intended tree. lstat sees the link itself (S_ISLNK, not
    // S_ISDIR) and we reject it. Deliberate divergence from Python's
    // symlink-following os.makedirs - the secure choice for a primitive whose
    // whole job is to create directories.
    auto eexist_is_real_dir = [](const char* p) -> bool {
        struct stat st;
        return lstat(p, &st) == 0 && S_ISDIR(st.st_mode);
    };

    // Try to create the directory directly first
    if (mkdir(path, (mode_t)mode) == 0) return 0;
    if (errno == EEXIST) return eexist_is_real_dir(path) ? 0 : -1;
    if (errno != ENOENT) return -1;

    // Parent doesn't exist - find last '/' and recurse
    char* tmp = strdup(path);
    char* slash = strrchr(tmp, '/');
    if (slash && slash != tmp) {
        *slash = '\0';
        if (dragon_makedirs(tmp, mode) != 0) {
            free(tmp);
            return -1;
        }
    }
    free(tmp);
    // Now create the directory itself
    if (mkdir(path, (mode_t)mode) == 0) return 0;
    if (errno == EEXIST) return eexist_is_real_dir(path) ? 0 : -1;
    return -1;
#endif
}

/// Get uname sysname (e.g., "Linux")
const char* dragon_uname_sysname() {
#ifdef _WIN32
    return dragon_string_alloc("Windows", 7);
#else
    struct utsname buf;
    if (uname(&buf) != 0) return dragon_string_alloc("", 0);
    return dragon_string_alloc(buf.sysname, (int64_t)strlen(buf.sysname));
#endif
}

/// Get uname nodename (hostname)
const char* dragon_uname_nodename() {
#ifdef _WIN32
    char buf[256] = {0};
    DWORD sz = (DWORD)sizeof(buf);
    if (!GetComputerNameA(buf, &sz)) return dragon_string_alloc("", 0);
    return dragon_string_alloc(buf, (int64_t)strlen(buf));
#else
    struct utsname buf;
    if (uname(&buf) != 0) return dragon_string_alloc("", 0);
    return dragon_string_alloc(buf.nodename, (int64_t)strlen(buf.nodename));
#endif
}

/// Get uname release (kernel version string)
const char* dragon_uname_release() {
#ifdef _WIN32
    OSVERSIONINFOA vi; memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    // GetVersionExA is deprecated but still functional under MinGW; the
    // alternative VerifyVersionInfo would require full feature manifests we
    // don't ship.
    if (!GetVersionExA(&vi)) return dragon_string_alloc("", 0);
    char rel[64];
    int n = snprintf(rel, sizeof(rel), "%lu.%lu",
                     (unsigned long)vi.dwMajorVersion,
                     (unsigned long)vi.dwMinorVersion);
    return dragon_string_alloc(rel, (int64_t)n);
#else
    struct utsname buf;
    if (uname(&buf) != 0) return dragon_string_alloc("", 0);
    return dragon_string_alloc(buf.release, (int64_t)strlen(buf.release));
#endif
}

/// Get uname version
const char* dragon_uname_version() {
#ifdef _WIN32
    OSVERSIONINFOA vi; memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (!GetVersionExA(&vi)) return dragon_string_alloc("", 0);
    char ver[64];
    int n = snprintf(ver, sizeof(ver), "Build %lu",
                     (unsigned long)vi.dwBuildNumber);
    return dragon_string_alloc(ver, (int64_t)n);
#else
    struct utsname buf;
    if (uname(&buf) != 0) return dragon_string_alloc("", 0);
    return dragon_string_alloc(buf.version, (int64_t)strlen(buf.version));
#endif
}

/// Get uname machine (e.g., "x86_64")
const char* dragon_uname_machine() {
#ifdef _WIN32
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    const char* arch;
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: arch = "x86_64"; break;
        case PROCESSOR_ARCHITECTURE_INTEL: arch = "x86";    break;
        case PROCESSOR_ARCHITECTURE_ARM:   arch = "arm";    break;
        case PROCESSOR_ARCHITECTURE_ARM64: arch = "arm64";  break;
        default:                           arch = "unknown"; break;
    }
    return dragon_string_alloc(arch, (int64_t)strlen(arch));
#else
    struct utsname buf;
    if (uname(&buf) != 0) return dragon_string_alloc("", 0);
    return dragon_string_alloc(buf.machine, (int64_t)strlen(buf.machine));
#endif
}

/// Get login name, returns "" on failure
const char* dragon_getlogin() {
#ifdef _WIN32
    char buf[UNLEN + 1] = {0};
    DWORD sz = (DWORD)sizeof(buf);
    if (!GetUserNameA(buf, &sz)) return dragon_string_alloc("", 0);
    return dragon_string_alloc(buf, (int64_t)strlen(buf));
#else
    const char* name = getlogin();
    if (!name) return dragon_string_alloc("", 0);
    return dragon_string_alloc(name, (int64_t)strlen(name));
#endif
}

//===----------------------------------------------------------------------===//
// Time Helpers - struct timespec access
//===----------------------------------------------------------------------===//

/// Create a timespec struct (16 bytes: tv_sec + tv_nsec)
void* dragon_timespec_new(int64_t sec, int64_t nsec) {
    struct timespec* ts = (struct timespec*)malloc(sizeof(struct timespec));
    ts->tv_sec = (time_t)sec;
    ts->tv_nsec = (long)nsec;
    return ts;
}

/// Get seconds from a timespec
int64_t dragon_timespec_sec(void* ts) {
    return (int64_t)((struct timespec*)ts)->tv_sec;
}

/// Get nanoseconds from a timespec
int64_t dragon_timespec_nsec(void* ts) {
    return (int64_t)((struct timespec*)ts)->tv_nsec;
}

//===----------------------------------------------------------------------===//
// Regex Helpers - ovector access
//===----------------------------------------------------------------------===//

/// Get a single value from PCRE2 ovector (size_t* array)
int64_t dragon_re_ovector_get(int64_t* ovector, int64_t index) {
    return ovector[index];
}

//===----------------------------------------------------------------------===//
// Atomic Helpers - for threading primitives
//===----------------------------------------------------------------------===//

/// Atomic load of a 64-bit integer
int64_t dragon_atomic_load(int64_t* p) {
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}

/// Atomic store of a 64-bit integer
void dragon_atomic_store(int64_t* p, int64_t val) {
    __atomic_store_n(p, val, __ATOMIC_SEQ_CST);
}

/// Atomic add, returns previous value
int64_t dragon_atomic_add(int64_t* p, int64_t val) {
    return __atomic_fetch_add(p, val, __ATOMIC_SEQ_CST);
}

//===----------------------------------------------------------------------===//
// Template Escape Functions - for template { } pipe filters
//===----------------------------------------------------------------------===//

// All three escapers special-case ASCII metacharacters only; every other byte
// (including all bytes of a multi-byte UTF-8 sequence) passes through. They
// therefore MUST operate on the UTF-8 wire bytes of the input, not on the raw
// data pointer: a kind=4 (UCS-4) Dragon string stores ASCII code points as
// `0xNN 0x00 0x00 0x00`, so strlen() truncates at the first NUL high byte and
// the escape scans only a 1-byte prefix - silent corruption (old Bug B).
// `dragon_str_to_utf8_alloc` returns NULL for kind=1 / literals (use `s`
// directly) or a malloc'd UTF-8 buffer for kind=4 that we free after escaping.
const char* dragon_template_escape_html(const char* s) {
    if (!s) return dragon_string_alloc("", 0);
    int64_t blen = 0;
    char* owned = dragon_str_to_utf8_alloc(s, &blen);
    const char* b = owned ? owned : s;
    // Worst case: every byte becomes &#x27; (6 chars)
    DragonString* ds = dragon_string_alloc_raw(blen * 6);
    size_t j = 0;
    for (int64_t i = 0; i < blen; i++) {
        switch (b[i]) {
            case '&':  memcpy(ds->data + j, "&amp;", 5);   j += 5; break;
            case '<':  memcpy(ds->data + j, "&lt;", 4);     j += 4; break;
            case '>':  memcpy(ds->data + j, "&gt;", 4);     j += 4; break;
            case '"':  memcpy(ds->data + j, "&quot;", 6);   j += 6; break;
            case '\'': memcpy(ds->data + j, "&#x27;", 6);   j += 6; break;
            default:   ds->data[j++] = b[i]; break;
        }
    }
    ds->data[j] = '\0';
    ds->len = (int64_t)j;
    if (owned) free(owned);
    return ds->data;
}

/// SQL escape: ' → '' (single-quote doubling)
const char* dragon_template_escape_sql(const char* s) {
    if (!s) return dragon_string_alloc("", 0);
    int64_t blen = 0;
    char* owned = dragon_str_to_utf8_alloc(s, &blen);
    const char* b = owned ? owned : s;
    size_t quotes = 0;
    for (int64_t i = 0; i < blen; i++) if (b[i] == '\'') quotes++;
    DragonString* ds = dragon_string_alloc_raw(blen + (int64_t)quotes);
    size_t j = 0;
    for (int64_t i = 0; i < blen; i++) {
        if (b[i] == '\'') { ds->data[j++] = '\''; ds->data[j++] = '\''; }
        else { ds->data[j++] = b[i]; }
    }
    ds->data[j] = '\0';
    ds->len = (int64_t)j;
    if (owned) free(owned);
    return ds->data;
}

/// URL percent-encoding: unreserved chars pass through, rest → %XX.
/// Operating on UTF-8 bytes is exactly correct: each byte of a multi-byte
/// sequence is percent-encoded, which is standard form-encoding behavior.
const char* dragon_template_escape_url(const char* s) {
    if (!s) return dragon_string_alloc("", 0);
    int64_t blen = 0;
    char* owned = dragon_str_to_utf8_alloc(s, &blen);
    const char* b = owned ? owned : s;
    // Worst case: every byte becomes %XX (3 chars)
    DragonString* ds = dragon_string_alloc_raw(blen * 3);
    size_t j = 0;
    for (int64_t i = 0; i < blen; i++) {
        unsigned char c = (unsigned char)b[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            ds->data[j++] = c;
        } else {
            static const char hex[] = "0123456789ABCDEF";
            ds->data[j++] = '%';
            ds->data[j++] = hex[c >> 4];
            ds->data[j++] = hex[c & 0x0F];
        }
    }
    ds->data[j] = '\0';
    ds->len = (int64_t)j;
    if (owned) free(owned);
    return ds->data;
}

// Build a dict[str, str] snapshot of the process environment. Backs Python's
// `os.environ` (a mapping) - distinct from os.getenv(name). Keys and values
// are ordinary owned heap DragonStrings the dict's key/value release paths
// reclaim. Keys must never be dragon_str_intern'd (immortal, no dedup): os.dr
// builds environ once at import so that stays bounded in practice, but any
// repeat caller would leak one immortal key per env var per call.
DragonDict* dragon_environ_dict(void) {
    DragonDict* d = dragon_dict_new(64);
#if defined(_WIN32)
    char** envp = _environ;
#elif defined(__APPLE__)
    extern char*** _NSGetEnviron(void);
    char** envp = *_NSGetEnviron();
#else
    extern char** environ;
    char** envp = environ;
#endif
    if (envp) {
        for (char** e = envp; *e; ++e) {
            const char* entry = *e;
            const char* eq = strchr(entry, '=');
            if (!eq) continue;  // skip malformed entries with no '='
            const char* key = dragon_string_alloc(entry, (int64_t)(eq - entry));
            const char* val = dragon_string_alloc(eq + 1, (int64_t)strlen(eq + 1));
            dragon_dict_set_tagged(d, key, (int64_t)(intptr_t)val, TAG_STR);
        }
    }
    return d;
}


} // extern "C"
