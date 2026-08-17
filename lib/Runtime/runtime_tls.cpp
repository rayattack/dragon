#include "runtime_internal.h"

#include <mbedtls/ssl.h>
#include <mbedtls/ssl_ciphersuites.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <sys/socket.h>


static const uint16_t kDragonTlsGroups[] = {
    MBEDTLS_SSL_IANA_TLS_GROUP_X25519,
    MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1,
    0,
};

static const uint16_t kDragonTlsSigAlgs[] = {
    MBEDTLS_TLS1_3_SIG_ECDSA_SECP256R1_SHA256,
    MBEDTLS_TLS1_3_SIG_RSA_PSS_RSAE_SHA256,
    MBEDTLS_TLS1_3_SIG_RSA_PSS_RSAE_SHA384,
    MBEDTLS_TLS1_3_SIG_RSA_PSS_RSAE_SHA512,
    MBEDTLS_TLS1_3_SIG_RSA_PKCS1_SHA256,
    MBEDTLS_TLS1_3_SIG_RSA_PKCS1_SHA384,
    MBEDTLS_TLS1_3_SIG_RSA_PKCS1_SHA512,
    MBEDTLS_TLS1_3_SIG_NONE,
};

static const int kDragonTlsCiphersuites[] = {
    MBEDTLS_TLS1_3_AES_128_GCM_SHA256,
    MBEDTLS_TLS1_3_AES_256_GCM_SHA384,
    MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    0,
};

struct DragonTlsCtx {
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
    mbedtls_x509_crt cacert;
    mbedtls_x509_crt owncert;
    mbedtls_pk_context ownkey;
    bool has_ca;
    bool has_own_cert;
    int verify_mode;
};

struct DragonTlsConn {
    mbedtls_ssl_context ssl;
    int fd;
    int64_t read_deadline_ms;
};

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int dragon_tls_bio_send(void* ctx, const unsigned char* buf, size_t len) {
    int fd = ((DragonTlsConn*)ctx)->fd;
    for (;;) {
        ssize_t n = ::send(fd, buf, len, MSG_NOSIGNAL);
        if (n >= 0) return (int)n;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (dragon_io_wait_writable(fd) < 0) return MBEDTLS_ERR_NET_SEND_FAILED;
            continue;
        }
        if (errno == EPIPE || errno == ECONNRESET) return MBEDTLS_ERR_NET_CONN_RESET;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
}

static int dragon_tls_bio_recv(void* ctx, unsigned char* buf, size_t len) {
    DragonTlsConn* conn = (DragonTlsConn*)ctx;
    int fd = conn->fd;
    for (;;) {
        ssize_t n = ::recv(fd, buf, len, 0);
        if (n >= 0) return (int)n;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (conn->read_deadline_ms > 0) {
                if (dragon_io_wait_readable_timeout(fd, conn->read_deadline_ms) != 0)
                    return MBEDTLS_ERR_NET_RECV_FAILED;
            } else {
                if (dragon_io_wait_readable(fd) < 0) return MBEDTLS_ERR_NET_RECV_FAILED;
            }
            continue;
        }
        if (errno == ECONNRESET) return MBEDTLS_ERR_NET_CONN_RESET;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
}

extern "C" {

void dragon_tls_ctx_free(void* handle) {
    if (!handle) return;
    DragonTlsCtx* c = (DragonTlsCtx*)handle;
    mbedtls_pk_free(&c->ownkey);
    mbedtls_x509_crt_free(&c->owncert);
    mbedtls_x509_crt_free(&c->cacert);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_ctr_drbg_free(&c->drbg);
    mbedtls_entropy_free(&c->entropy);
    free(c);
}

void* dragon_tls_ctx_new(int64_t is_server) {
    DragonTlsCtx* c = (DragonTlsCtx*)dragon_calloc_nullable(1, sizeof(DragonTlsCtx));
    if (!c) return nullptr;
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_ctr_drbg_init(&c->drbg);
    mbedtls_entropy_init(&c->entropy);
    mbedtls_x509_crt_init(&c->cacert);
    mbedtls_x509_crt_init(&c->owncert);
    mbedtls_pk_init(&c->ownkey);

    static const char kPers[] = "dragon-tls";
    if (mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy,
                              (const unsigned char*)kPers, sizeof(kPers) - 1) != 0) {
        dragon_tls_ctx_free(c);
        return nullptr;
    }

    int endpoint = is_server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT;
    if (mbedtls_ssl_config_defaults(&c->conf, endpoint,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        dragon_tls_ctx_free(c);
        return nullptr;
    }

    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);

    mbedtls_ssl_conf_min_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_groups(&c->conf, kDragonTlsGroups);
    mbedtls_ssl_conf_sig_algs(&c->conf, kDragonTlsSigAlgs);
    mbedtls_ssl_conf_ciphersuites(&c->conf, kDragonTlsCiphersuites);
    mbedtls_ssl_conf_cert_profile(&c->conf, &mbedtls_x509_crt_profile_default);

    c->verify_mode = is_server ? MBEDTLS_SSL_VERIFY_NONE : MBEDTLS_SSL_VERIFY_REQUIRED;
    mbedtls_ssl_conf_authmode(&c->conf, c->verify_mode);

    return c;
}

void dragon_tls_ctx_set_verify(void* handle, int64_t mode) {
    DragonTlsCtx* c = (DragonTlsCtx*)handle;
    int m = MBEDTLS_SSL_VERIFY_REQUIRED;
    if (mode == 0) m = MBEDTLS_SSL_VERIFY_NONE;
    else if (mode == 1) m = MBEDTLS_SSL_VERIFY_OPTIONAL;
    c->verify_mode = m;
    mbedtls_ssl_conf_authmode(&c->conf, m);
}

int64_t dragon_tls_ctx_load_ca_file(void* handle, const char* path) {
    DragonTlsCtx* c = (DragonTlsCtx*)handle;
    int ret = mbedtls_x509_crt_parse_file(&c->cacert, path);
    if (ret < 0) return ret;
    c->has_ca = true;
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->cacert, nullptr);
    return ret;
}

int64_t dragon_tls_ctx_load_ca_data(void* handle, const char* data, int64_t len) {
    DragonTlsCtx* c = (DragonTlsCtx*)handle;
    int ret = mbedtls_x509_crt_parse(&c->cacert, (const unsigned char*)data,
                                     (size_t)len + 1);
    if (ret < 0) return ret;
    c->has_ca = true;
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->cacert, nullptr);
    return ret;
}

int64_t dragon_tls_ctx_load_ca_path(void* handle, const char* path) {
    DragonTlsCtx* c = (DragonTlsCtx*)handle;
    int ret = mbedtls_x509_crt_parse_path(&c->cacert, path);
    if (ret < 0) return ret;
    c->has_ca = true;
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->cacert, nullptr);
    return ret;
}

int64_t dragon_tls_ctx_load_cert_chain(void* handle, const char* certfile,
                                       const char* keyfile) {
    DragonTlsCtx* c = (DragonTlsCtx*)handle;
    mbedtls_x509_crt_free(&c->owncert);
    mbedtls_x509_crt_init(&c->owncert);
    c->has_own_cert = false;
    int ret = mbedtls_x509_crt_parse_file(&c->owncert, certfile);
    if (ret != 0) return ret;
    ret = mbedtls_pk_parse_keyfile(&c->ownkey, keyfile, nullptr,
                                   mbedtls_ctr_drbg_random, &c->drbg);
    if (ret != 0) return ret;
    ret = mbedtls_ssl_conf_own_cert(&c->conf, &c->owncert, &c->ownkey);
    if (ret != 0) return ret;
    c->has_own_cert = true;
    return 0;
}

void dragon_tls_conn_free(void* handle) {
    if (!handle) return;
    DragonTlsConn* conn = (DragonTlsConn*)handle;
    mbedtls_ssl_free(&conn->ssl);
    free(conn);
}

void* dragon_tls_conn_new(void* ctx_handle, int64_t fd, const char* server_hostname) {
    DragonTlsCtx* c = (DragonTlsCtx*)ctx_handle;
    bool empty_hostname = !server_hostname || server_hostname[0] == '\0';
    if (empty_hostname && c->verify_mode == MBEDTLS_SSL_VERIFY_REQUIRED) {
        dragon_raise_exc_cstr(50 ,
                         "ssl: empty server_hostname with CERT_REQUIRED");
        return nullptr;
    }
    DragonTlsConn* conn = (DragonTlsConn*)dragon_calloc_nullable(1, sizeof(DragonTlsConn));
    if (!conn) {
        dragon_raise_exc_cstr(43,
                         "MemoryError: ssl: out of memory allocating TLS connection");
        return nullptr;
    }
    conn->fd = (int)fd;
    dragon_set_nonblocking(fd);
    mbedtls_ssl_init(&conn->ssl);
    if (mbedtls_ssl_setup(&conn->ssl, &c->conf) != 0) {
        dragon_tls_conn_free(conn);
        dragon_raise_exc_cstr(50, "ssl: SSL/TLS engine setup failed");
        return nullptr;
    }
    if (!empty_hostname) {
        if (mbedtls_ssl_set_hostname(&conn->ssl, server_hostname) != 0) {
            dragon_tls_conn_free(conn);
            dragon_raise_exc_cstr(50, "ssl: failed to set server hostname (SNI)");
            return nullptr;
        }
    }
    mbedtls_ssl_set_bio(&conn->ssl, conn,
                        dragon_tls_bio_send, dragon_tls_bio_recv, nullptr);
    return conn;
}

int64_t dragon_tls_handshake(void* handle) {
    DragonTlsConn* conn = (DragonTlsConn*)handle;
    for (;;) {
        int ret = mbedtls_ssl_handshake(&conn->ssl);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        return ret;
    }
}

int64_t dragon_tls_read(void* handle, void* buf, int64_t len) {
    DragonTlsConn* conn = (DragonTlsConn*)handle;
    for (;;) {
        int ret = mbedtls_ssl_read(&conn->ssl, (unsigned char*)buf, (size_t)len);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        return ret;
    }
}

extern "C" int64_t dragon_tls_write(void* handle, const void* buf, int64_t len);
int64_t dragon_tls_send_bytes(void* handle, DragonBytes* data) {
    if (!data || data->len == 0) return 0;
    return dragon_tls_write(handle, (const void*)data->data, data->len);
}

int64_t dragon_tls_write(void* handle, const void* buf, int64_t len) {
    DragonTlsConn* conn = (DragonTlsConn*)handle;
    size_t off = 0;
    while (off < (size_t)len) {
        int ret = mbedtls_ssl_write(&conn->ssl,
                                    (const unsigned char*)buf + off,
                                    (size_t)len - off);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (ret < 0) return ret;
        off += (size_t)ret;
    }
    return (int64_t)off;
}

const char* dragon_tls_recv_str(void* handle, int64_t maxlen) {
    if (maxlen <= 0) maxlen = 8192;
    unsigned char* buf = (unsigned char*)dragon_malloc_nullable((size_t)maxlen);
    if (!buf) {
        DragonString* ds = dragon_string_alloc_raw(0);
        ds->data[0] = '\0';
        return ds->data;
    }
    int64_t got = dragon_tls_read(handle, buf, maxlen);
    if (got < 0) got = 0;
    DragonString* ds = dragon_string_alloc_raw(got);
    if (got > 0) memcpy(ds->data, buf, (size_t)got);
    ds->data[got] = '\0';
    free(buf);
    return ds->data;
}

const char* dragon_tls_recv_str_timeout(void* handle, int64_t maxlen, int64_t ms) {
    DragonTlsConn* conn = (DragonTlsConn*)handle;
    conn->read_deadline_ms = ms > 0 ? ms : 0;
    const char* out = dragon_tls_recv_str(handle, maxlen);
    conn->read_deadline_ms = 0;
    return out;
}

int64_t dragon_tls_close(void* handle) {
    if (!handle) return 0;
    DragonTlsConn* conn = (DragonTlsConn*)handle;
    int ret;
    do {
        ret = mbedtls_ssl_close_notify(&conn->ssl);
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    return ret;
}

int64_t dragon_tls_get_verify_result(void* handle) {
    DragonTlsConn* conn = (DragonTlsConn*)handle;
    return (int64_t)mbedtls_ssl_get_verify_result(&conn->ssl);
}

void dragon_tls_error_string(int64_t code, char* buf, int64_t len) {
    if (len <= 0) return;
    mbedtls_strerror((int)code, buf, (size_t)len);
}

}
