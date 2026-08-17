#include "runtime_internal.h"
#include <sys/stat.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
  #include <direct.h>
  #include <lmcons.h>
  #include <ws2tcpip.h>
#else
  #include <sys/utsname.h>
  #include <dirent.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <fcntl.h>
  #if defined(__APPLE__) || defined(__FreeBSD__)
    #include <sys/random.h>
  #endif
  #include <sys/syscall.h>
#endif
#include "llhttp.h"
#ifndef _WIN32
  #include <regex.h>
#endif

extern "C" {

#ifdef _WIN32
extern "C" void dragon_win_wsa_startup(void);
#endif

void* dragon_sockaddr_in_new(int64_t port, const char* addr) {
#ifdef _WIN32
    dragon_win_wsa_startup();
#endif
    struct sockaddr_in* sa = (struct sockaddr_in*)dragon_xmalloc(sizeof(struct sockaddr_in));
    memset(sa, 0, sizeof(struct sockaddr_in));
    sa->sin_family = AF_INET;
    sa->sin_port = htons((uint16_t)port);
    if (addr && strcmp(addr, "0.0.0.0") == 0) {
        sa->sin_addr.s_addr = INADDR_ANY;
    } else if (addr) {
        if (inet_pton(AF_INET, addr, &sa->sin_addr) != 1) {
            free(sa);
            dragon_raise_exc_cstr(50 ,
                                  "invalid IPv4 address (host did not resolve?)");
            return nullptr;
        }
    }
    return (void*)sa;
}

const char* dragon_resolve4(const char* host) {
    char ipbuf[INET_ADDRSTRLEN];
    const char* ip = host;
    struct in_addr probe;
    if (host && inet_pton(AF_INET, host, &probe) != 1) {
#ifdef _WIN32
        dragon_win_wsa_startup();
#endif
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = nullptr;
        if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) {
            return dragon_string_dup_cstr("");
        }
        struct sockaddr_in* sa = (struct sockaddr_in*)res->ai_addr;
        inet_ntop(AF_INET, &sa->sin_addr, ipbuf, sizeof(ipbuf));
        freeaddrinfo(res);
        ip = ipbuf;
    }
    return dragon_string_dup_cstr(ip);
}



const char* dragon_default_ca_file() {
    const char* env = getenv("SSL_CERT_FILE");
    if (env && env[0] && access(env, 0) == 0) {
        return dragon_string_dup_cstr(env);
    }
    static const char* sys_paths[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/cert.pem",
        "/etc/ssl/certs/ca-bundle.crt",
        "/usr/local/share/certs/ca-root-nss.crt",
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
        nullptr
    };
    for (int i = 0; sys_paths[i]; i++) {
        if (access(sys_paths[i], 0) == 0) return dragon_string_dup_cstr(sys_paths[i]);
    }
#ifdef DRAGON_CA_BUNDLE
    if (access(DRAGON_CA_BUNDLE, 0) == 0) return dragon_string_dup_cstr(DRAGON_CA_BUNDLE);
#endif
    return dragon_string_dup_cstr("");
}

const char* dragon_default_ca_dir() {
    const char* env = getenv("SSL_CERT_DIR");
    return dragon_string_dup_cstr((env && env[0]) ? env : "");
}

int64_t dragon_sockaddr_in_size() {
    return (int64_t)sizeof(struct sockaddr_in);
}

void dragon_ptr_write_i32(void* p, int64_t offset, int64_t val) {
    *((int32_t*)((char*)p + offset)) = (int32_t)val;
}

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

void* dragon_ptr_deref(void** pp) {
    return pp ? *pp : nullptr;
}


#define DRAGON_HTTP_MAX_HEADERS 64
#define DRAGON_HTTP_MAX_BODY (1024 * 1024)
#define DRAGON_HTTP_HEADERS_MAX (64 * 1024)

typedef struct {
    char method[16];
    int method_len;
    char* url;
    int url_len;
    int url_cap;
    char* header_keys[DRAGON_HTTP_MAX_HEADERS];
    int   header_key_lens[DRAGON_HTTP_MAX_HEADERS];
    char* header_vals[DRAGON_HTTP_MAX_HEADERS];
    int   header_val_lens[DRAGON_HTTP_MAX_HEADERS];
    int   num_headers;
    int   in_value;
    int   header_bytes;
    char* body;
    int body_len;
    int body_cap;
    int complete;
    uint8_t http_major;
    uint8_t http_minor;
} HttpParseState;

static int http_on_url(llhttp_t* p, const char* at, size_t len) {
    HttpParseState* s = (HttpParseState*)p->data;
    if (s->header_bytes + (int)len > DRAGON_HTTP_HEADERS_MAX) return -1;
    while (s->url_len + (int)len >= s->url_cap) {
        int new_cap = s->url_cap * 2;
        char* tmp = (char*)dragon_xrealloc_or_abort(s->url, new_cap);
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
        s->num_headers++;
        s->in_value = 0;
    }
    int idx = s->num_headers;
    if (idx >= DRAGON_HTTP_MAX_HEADERS) return 0;
    if (s->header_bytes + (int)len > DRAGON_HTTP_HEADERS_MAX) return -1;
    int old_len = s->header_key_lens[idx];
    char* tmp = (char*)dragon_xrealloc_or_abort(s->header_keys[idx], old_len + len + 1);
    s->header_keys[idx] = tmp;
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
    char* tmp = (char*)dragon_xrealloc_or_abort(s->header_vals[idx], old_len + len + 1);
    s->header_vals[idx] = tmp;
    memcpy(s->header_vals[idx] + old_len, at, len);
    s->header_val_lens[idx] += (int)len;
    s->header_vals[idx][s->header_val_lens[idx]] = '\0';
    s->header_bytes += (int)len;
    return 0;
}

static int http_on_body(llhttp_t* p, const char* at, size_t len) {
    HttpParseState* s = (HttpParseState*)p->data;
    if (s->body_len + (int)len > DRAGON_HTTP_MAX_BODY) return -1;
    while (s->body_len + (int)len >= s->body_cap) {
        int new_cap = s->body_cap * 2;
        if (new_cap > DRAGON_HTTP_MAX_BODY) new_cap = DRAGON_HTTP_MAX_BODY + 1;
        char* tmp = (char*)dragon_xrealloc_or_abort(s->body, new_cap);
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
    if (s->in_value && s->num_headers < DRAGON_HTTP_MAX_HEADERS) {
        s->num_headers++;
    }
    s->complete = 1;
    s->http_major = p->http_major;
    s->http_minor = p->http_minor;
    return 0;
}

void* dragon_http_parse_request(const char* buf, int64_t len) {
    HttpParseState* state = (HttpParseState*)dragon_xcalloc_n(1, sizeof(HttpParseState));
    state->url_cap = 256;
    state->url = (char*)dragon_malloc_nullable(state->url_cap);
    if (!state->url) { free(state); dragon_raise_oom(); }
    state->url[0] = '\0';
    state->body_cap = 1024;
    state->body = (char*)dragon_malloc_nullable(state->body_cap);
    if (!state->body) { free(state->url); free(state); dragon_raise_oom(); }
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
        state->method[0] = '\0';
        state->method_len = 0;
    } else {
        const char* m = llhttp_method_name((llhttp_method_t)parser.method);
        int mlen = (int)strlen(m);
        if (mlen > 15) mlen = 15;
        memcpy(state->method, m, mlen);
        state->method[mlen] = '\0';
        state->method_len = mlen;
    }
    return state;
}

const char* dragon_http_parsed_method(void* handle) {
    HttpParseState* s = (HttpParseState*)handle;
    return dragon_string_alloc(s->method, s->method_len);
}

const char* dragon_http_parsed_url(void* handle) {
    HttpParseState* s = (HttpParseState*)handle;
    return dragon_string_alloc(s->url, s->url_len);
}

const char* dragon_http_parsed_body(void* handle) {
    HttpParseState* s = (HttpParseState*)handle;
    return dragon_string_alloc(s->body, s->body_len);
}

int64_t dragon_http_parsed_header_count(void* handle) {
    HttpParseState* s = (HttpParseState*)handle;
    return s->num_headers;
}

const char* dragon_http_parsed_header_key(void* handle, int64_t idx) {
    HttpParseState* s = (HttpParseState*)handle;
    if (idx < 0 || idx >= s->num_headers) return dragon_string_alloc("", 0);
    return dragon_string_alloc(s->header_keys[idx], s->header_key_lens[idx]);
}

const char* dragon_http_parsed_header_value(void* handle, int64_t idx) {
    HttpParseState* s = (HttpParseState*)handle;
    if (idx < 0 || idx >= s->num_headers) return dragon_string_alloc("", 0);
    return dragon_string_alloc(s->header_vals[idx], s->header_val_lens[idx]);
}

int64_t dragon_http_parsed_ok(void* handle) {
    HttpParseState* s = (HttpParseState*)handle;
    return s->complete ? 1 : 0;
}

const char* dragon_http_parsed_version(void* handle) {
    HttpParseState* s = (HttpParseState*)handle;
    if (s->http_minor == 0) return dragon_string_alloc("1.0", 3);
    return dragon_string_alloc("1.1", 3);
}

void dragon_http_parsed_free(void* handle) {
    HttpParseState* s = (HttpParseState*)handle;
    if (!s) return;
    free(s->url);
    free(s->body);
    for (int i = 0; i < DRAGON_HTTP_MAX_HEADERS; i++) {
        free(s->header_keys[i]);
        free(s->header_vals[i]);
    }
    free(s);
}

const char* dragon_http_build_response(int64_t status, const char* headers, const char* body) {
    const char* reason;
    switch (status) {
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
        case 426: reason = "Upgrade Required"; break;
        case 429: reason = "Too Many Requests"; break;
        case 500: reason = "Internal Server Error"; break;
        case 502: reason = "Bad Gateway"; break;
        case 503: reason = "Service Unavailable"; break;
        default:  reason = "Unknown"; break;
    }
    int64_t body_len = 0;
    char* body_owned = NULL;
    const char* body_bytes = NULL;
    if (body) {
        body_owned = dragon_str_to_utf8_alloc(body, &body_len);
        body_bytes = body_owned ? body_owned : body;
    }
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

uint64_t __dragon_hash_k0 = 0;
uint64_t __dragon_hash_k1 = 0;

static int64_t dragon_fill_os_random(unsigned char* buf, int64_t n) {
    int64_t got = 0;
#ifdef _WIN32
    extern "C" {
        long __stdcall BCryptGenRandom(void* hAlgorithm, unsigned char* pbBuffer,
                                       unsigned long cbBuffer, unsigned long dwFlags);
    }
    if (BCryptGenRandom(nullptr, buf, (unsigned long)n, 2) >= 0) got = n;
#else
    #if defined(__linux__) && defined(SYS_getrandom)
    while (got < n) {
        long r = syscall(SYS_getrandom, buf + got, (size_t)(n - got), 0u);
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
    if (__dragon_hash_k0 || __dragon_hash_k1) return;
    unsigned char seed[16];
    if (dragon_fill_os_random(seed, 16) == 16) {
        __dragon_hash_k0 = dragon_hash_read_le64(seed);
        __dragon_hash_k1 = dragon_hash_read_le64(seed + 8);
    } else {
        uintptr_t a = (uintptr_t)&__dragon_hash_k0;
        uintptr_t b = (uintptr_t)(intptr_t)getpid();
        __dragon_hash_k0 = (uint64_t)a * 0x9E3779B97F4A7C15ULL ^ 0xD1B54A32D192ED03ULL;
        __dragon_hash_k1 = ((uint64_t)b ^ (uint64_t)a) * 0xC2B2AE3D27D4EB4FULL | 1ULL;
    }
}

__attribute__((constructor))
static void dragon_hash_secret_ctor(void) { dragon_hash_secret_init(); }

DragonBytes* dragon_urandom(int64_t n) {
    if (n <= 0) return dragon_bytes_new(nullptr, 0);
    auto* buf = (uint8_t*)dragon_xmalloc((size_t)n);
    int64_t got = 0;
#ifdef _WIN32
    extern "C" {
        long __stdcall BCryptGenRandom(void* hAlgorithm, unsigned char* pbBuffer,
                                       unsigned long cbBuffer, unsigned long dwFlags);
    }
    long st = BCryptGenRandom(nullptr, buf, (unsigned long)n,
                              2 );
    if (st >= 0) got = n;
#else
    #if defined(__linux__) && defined(SYS_getrandom)
    while (got < n) {
        long r = syscall(SYS_getrandom, buf + got, (size_t)(n - got), 0u);
        if (r <= 0) break;
        got += r;
    }
    #elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    while (got < n) {
        size_t chunk = (size_t)(n - got);
        if (chunk > 256) chunk = 256;
        if (getentropy(buf + got, chunk) == 0) got += (int64_t)chunk;
        else break;
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
    if (got < n) {
        free(buf);
        dragon_raise_exc_cstr(50 , "os.urandom: kernel CSPRNG unavailable");
        return nullptr;
    }
    auto* b = dragon_bytes_new(buf, n);
    free(buf);
    return b;
}

#ifndef _WIN32

void* dragon_re_compile(const char* pattern) {
    regex_t* re = (regex_t*)dragon_xmalloc(sizeof(regex_t));
    int rc = regcomp(re, pattern, REG_EXTENDED);
    if (rc != 0) {
        char errbuf[128];
        regerror(rc, re, errbuf, sizeof(errbuf));
        free(re);
        char msg[192];
        snprintf(msg, sizeof(msg), "ValueError: invalid regex: %s", errbuf);
        dragon_raise_exc_cstr(90, msg);
    }
    return re;
}

int64_t dragon_re_match(void* compiled, const char* subject) {
    if (!compiled) return 0;
    regex_t* re = (regex_t*)compiled;
    regmatch_t match[10];
    int rc = regexec(re, subject, 10, match, 0);
    if (rc != 0) return 0;
    int count = 0;
    for (int i = 0; i < 10; i++) {
        if (match[i].rm_so >= 0) count++;
        else break;
    }
    return count;
}

const char* dragon_re_search(void* compiled, const char* subject) {
    if (!compiled) return dragon_string_alloc("", 0);
    regex_t* re = (regex_t*)compiled;
    regmatch_t match[1];
    int rc = regexec(re, subject, 1, match, 0);
    if (rc != 0 || match[0].rm_so < 0) return dragon_string_alloc("", 0);
    int len = match[0].rm_eo - match[0].rm_so;
    return dragon_string_alloc(subject + match[0].rm_so, (int64_t)len);
}

const char* dragon_re_group(void* compiled, const char* subject, int64_t index) {
    if (!compiled) return dragon_string_alloc("", 0);
    regex_t* re = (regex_t*)compiled;
    regmatch_t matches[10];
    int rc = regexec(re, subject, 10, matches, 0);
    if (rc != 0 || index >= 10 || matches[index].rm_so < 0) return dragon_string_alloc("", 0);
    int len = matches[index].rm_eo - matches[index].rm_so;
    return dragon_string_alloc(subject + matches[index].rm_so, (int64_t)len);
}

void dragon_re_free(void* compiled) {
    if (compiled) {
        regfree((regex_t*)compiled);
        free(compiled);
    }
}

int64_t dragon_re_match_str(const char* pattern, const char* subject) {
    void* re = dragon_re_compile(pattern);
    int64_t result = dragon_re_match(re, subject);
    dragon_re_free(re);
    return result;
}

const char* dragon_re_search_str(const char* pattern, const char* subject) {
    void* re = dragon_re_compile(pattern);
    const char* result = dragon_re_search(re, subject);
    dragon_re_free(re);
    return result;
}

#else

void*       dragon_re_compile(const char*) { return nullptr; }
int64_t     dragon_re_match(void*, const char*) { return 0; }
const char* dragon_re_search(void*, const char*) { return dragon_string_alloc("", 0); }
const char* dragon_re_group(void*, const char*, int64_t) { return dragon_string_alloc("", 0); }
void        dragon_re_free(void*) {}
int64_t     dragon_re_match_str(const char*, const char*) { return 0; }
const char* dragon_re_search_str(const char*, const char*) { return dragon_string_alloc("", 0); }

#endif

const char* dragon_re_get_match(const char* subject, int64_t* ovector, int64_t index) {
    int64_t start = ovector[index * 2];
    int64_t end = ovector[index * 2 + 1];
    if (start < 0 || end < start) return dragon_string_alloc("", 0);
    int64_t len = end - start;
    return dragon_string_alloc(subject + start, len);
}

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
#endif

int64_t dragon_stat_size(const char* path) {
    dragon_stat_t st;
    if (dragon_stat(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

int64_t dragon_stat_mtime(const char* path) {
    dragon_stat_t st;
    if (dragon_stat(path, &st) != 0) return -1;
    return (int64_t)st.st_mtime;
}

int32_t dragon_stat_isfile(const char* path) {
    dragon_stat_t st;
    if (dragon_stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) ? 1 : 0;
}

int32_t dragon_stat_isdir(const char* path) {
    dragon_stat_t st;
    if (dragon_stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

const char* dragon_readdir_name(void* dirp) {
#ifdef _WIN32
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

int64_t dragon_stat_atime(const char* path) {
    dragon_stat_t st;
    if (dragon_stat(path, &st) != 0) return -1;
    return (int64_t)st.st_atime;
}

int64_t dragon_stat_ctime(const char* path) {
    dragon_stat_t st;
    if (dragon_stat(path, &st) != 0) return -1;
    return (int64_t)st.st_ctime;
}

int64_t dragon_stat_mode(const char* path) {
    dragon_stat_t st;
    if (dragon_stat(path, &st) != 0) return -1;
    return (int64_t)st.st_mode;
}

int64_t dragon_stat_dev(const char* path) {
    dragon_stat_t st;
    if (dragon_stat(path, &st) != 0) return -1;
    return (int64_t)st.st_dev;
}

int64_t dragon_stat_ino(const char* path) {
    dragon_stat_t st;
    if (dragon_stat(path, &st) != 0) return -1;
    return (int64_t)st.st_ino;
}

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

#ifndef _WIN32
  #include <sys/wait.h>
#endif

int32_t dragon_execv(const char* path, DragonList* argv) {
#ifdef _WIN32
    (void)path; (void)argv;
    errno = EINVAL;
    return -1;
#else
    int n = (int)dragon_list_len(argv);
    char** args = (char**)dragon_malloc_nullable((size_t)(n + 1) * sizeof(char*));
    char** owned = (char**)dragon_calloc_nullable((size_t)(n > 0 ? n : 1), sizeof(char*));
    if (!args || !owned) { free(args); free(owned); errno = ENOMEM; return -1; }
    for (int i = 0; i < n; i++) {
        const char* raw = (const char*)(uintptr_t)dragon_list_load(argv, i);
        int64_t blen = 0;
        char* enc = dragon_str_to_utf8_alloc(raw, &blen);
        if (enc) { owned[i] = enc; args[i] = enc; }
        else     { args[i] = (char*)(uintptr_t)raw; }
    }
    args[n] = NULL;
    int rc = execv(path, args);
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
    char** args = (char**)dragon_malloc_nullable((size_t)(n + 1) * sizeof(char*));
    char** owned = (char**)dragon_calloc_nullable((size_t)(n > 0 ? n : 1), sizeof(char*));
    if (!args || !owned) { free(args); free(owned); errno = ENOMEM; return -1; }
    for (int i = 0; i < n; i++) {
        const char* raw = (const char*)(uintptr_t)dragon_list_load(argv, i);
        int64_t blen = 0;
        char* enc = dragon_str_to_utf8_alloc(raw, &blen);
        if (enc) { owned[i] = enc; args[i] = enc; }
        else     { args[i] = (char*)(uintptr_t)raw; }
    }
    args[n] = NULL;
    int rc = execvp(file, args);
    for (int i = 0; i < n; i++) free(owned[i]);
    free(owned);
    free(args);
    return (int32_t)rc;
#endif
}

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

const char* dragon_readlink(const char* path) {
#ifdef _WIN32
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

const char* dragon_getcwd(void) {
#ifdef _WIN32
    char* buf = _getcwd(nullptr, 0);
#else
    char* buf = getcwd(nullptr, 0);
#endif
    if (!buf) {
        char msg[160];
        snprintf(msg, sizeof(msg), "OSError: getcwd() failed: %s", strerror(errno));
        dragon_raise_exc_cstr(50, msg);
    }
    const char* out = dragon_string_alloc(buf, (int64_t)strlen(buf));
    free(buf);
    return out;
}

const char* dragon_realpath(const char* path) {
    if (!path) return dragon_string_alloc("", 0);
#ifdef _WIN32
    char* resolved = _fullpath(nullptr, path, 0);
    if (resolved) {
        const char* out = dragon_string_alloc(resolved, (int64_t)strlen(resolved));
        free(resolved);
        return out;
    }
    return dragon_string_alloc(path, (int64_t)strlen(path));
#else
    char* resolved = realpath(path, nullptr);
    if (resolved) {
        const char* out = dragon_string_alloc(resolved, (int64_t)strlen(resolved));
        free(resolved);
        return out;
    }
    if (path[0] == '/') return dragon_string_alloc(path, (int64_t)strlen(path));
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd))) {
        size_t cl = strlen(cwd), pl = strlen(path);
        char* joined = (char*)dragon_xmalloc(cl + 1 + pl + 1);
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

int32_t dragon_create_excl(const char* path) {
    if (!path) return -1;
#ifdef _WIN32
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

int32_t dragon_makedirs(const char* path, int32_t mode) {
#ifdef _WIN32
    (void)mode;
    if (_mkdir(path) == 0) return 0;
    if (errno == EEXIST) {
        dragon_stat_t st;
        if (dragon_stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
        return -1;
    }
    if (errno != ENOENT) return -1;
    char* tmp = _strdup(path);
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
    auto eexist_is_real_dir = [](const char* p) -> bool {
        struct stat st;
        return lstat(p, &st) == 0 && S_ISDIR(st.st_mode);
    };

    if (mkdir(path, (mode_t)mode) == 0) return 0;
    if (errno == EEXIST) return eexist_is_real_dir(path) ? 0 : -1;
    if (errno != ENOENT) return -1;

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
    if (mkdir(path, (mode_t)mode) == 0) return 0;
    if (errno == EEXIST) return eexist_is_real_dir(path) ? 0 : -1;
    return -1;
#endif
}

const char* dragon_uname_sysname() {
#ifdef _WIN32
    return dragon_string_alloc("Windows", 7);
#else
    struct utsname buf;
    if (uname(&buf) != 0) return dragon_string_alloc("", 0);
    return dragon_string_alloc(buf.sysname, (int64_t)strlen(buf.sysname));
#endif
}

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

const char* dragon_uname_release() {
#ifdef _WIN32
    OSVERSIONINFOA vi; memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
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

int64_t dragon_clock_realtime_id(void)  { return (int64_t)CLOCK_REALTIME; }
int64_t dragon_clock_monotonic_id(void) { return (int64_t)CLOCK_MONOTONIC; }
int64_t dragon_clock_process_id(void)   { return (int64_t)CLOCK_PROCESS_CPUTIME_ID; }

void* dragon_timespec_new(int64_t sec, int64_t nsec) {
    struct timespec* ts = (struct timespec*)dragon_xmalloc(sizeof(struct timespec));
    ts->tv_sec = (time_t)sec;
    ts->tv_nsec = (long)nsec;
    return ts;
}

int64_t dragon_timespec_sec(void* ts) {
    return (int64_t)((struct timespec*)ts)->tv_sec;
}

int64_t dragon_timespec_nsec(void* ts) {
    return (int64_t)((struct timespec*)ts)->tv_nsec;
}

int64_t dragon_re_ovector_get(int64_t* ovector, int64_t index) {
    return ovector[index];
}

int64_t dragon_atomic_load(int64_t* p) {
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}

void dragon_atomic_store(int64_t* p, int64_t val) {
    __atomic_store_n(p, val, __ATOMIC_SEQ_CST);
}

int64_t dragon_atomic_add(int64_t* p, int64_t val) {
    return __atomic_fetch_add(p, val, __ATOMIC_SEQ_CST);
}

const char* dragon_template_escape_html(const char* s) {
    if (!s) return dragon_string_alloc("", 0);
    int64_t blen = 0;
    char* owned = dragon_str_to_utf8_alloc(s, &blen);
    const char* b = owned ? owned : s;
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

const char* dragon_template_escape_url(const char* s) {
    if (!s) return dragon_string_alloc("", 0);
    int64_t blen = 0;
    char* owned = dragon_str_to_utf8_alloc(s, &blen);
    const char* b = owned ? owned : s;
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
            if (!eq) continue;
            const char* key = dragon_string_alloc(entry, (int64_t)(eq - entry));
            const char* val = dragon_string_alloc(eq + 1, (int64_t)strlen(eq + 1));
            dragon_dict_set_tagged(d, key, (int64_t)(intptr_t)val, TAG_STR);
        }
    }
    return d;
}


}
