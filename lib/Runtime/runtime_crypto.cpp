/// Dragon Runtime - Crypto digests (hashlib / hmac / crypto.dr), backed by
/// mbedTLS (ADR 038 Phase 7).
///
/// The hand-rolled SHA-256/SHA-1/MD5 cores (formerly lib/Runtime/crypto.h) are
/// retired: mbedTLS's streaming, hardware-accelerated paths are faster (motto
/// #1) and there's no reason to keep a slower duplicate now that mbedTLS is in
/// the tree. secrets/urandom stay pure-Dragon (dragon_urandom in
/// runtime_platform.cpp, direct getrandom); the .dr KATs in test/dr/ remain the
/// correctness oracle.
///
/// These functions live in their OWN translation unit - not runtime_platform.cpp
/// - so the linker pulls them (and thus mbedtls_* symbols) ONLY when a program
/// references a crypto entry point. Folding them into the always-linked
/// runtime_platform.cpp.o would force every Dragon binary to resolve mbedtls
/// symbols even when it never hashes anything. This mirrors how the TLS shim is
/// isolated in runtime_tls.cpp.

#include "runtime_internal.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <mbedtls/sha512.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha1.h>
#include <mbedtls/md5.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/gcm.h>
#include <mbedtls/chachapoly.h>
#include <mbedtls/platform_util.h>

extern "C" {

/// Convert raw bytes buffer to hex string
const char* dragon_raw_bytes_hex(const void* data, int64_t len) {
    DragonString* ds = dragon_string_alloc_raw(len * 2);
    const unsigned char* bytes = (const unsigned char*)data;
    for (int64_t i = 0; i < len; i++) {
        sprintf(ds->data + i * 2, "%02x", bytes[i]);
    }
    ds->data[len * 2] = '\0';
    return ds->data;
}

// A valid non-NULL pointer for zero-length inputs: mbedTLS's *_update returns
// early on ilen==0, but we never want to hand it a NULL buffer.
static const unsigned char dragon_empty_input[1] = {0};

/// SHA-256 hash - returns hex string
const char* dragon_sha256(const char* data, int64_t len) {
    uint8_t hash[32];
    mbedtls_sha256((const unsigned char*)data, (size_t)len, hash, 0);
    return dragon_raw_bytes_hex(hash, 32);
}

/// SHA-1 hash - returns hex string
const char* dragon_sha1(const char* data, int64_t len) {
    uint8_t hash[20];
    mbedtls_sha1((const unsigned char*)data, (size_t)len, hash);
    return dragon_raw_bytes_hex(hash, 20);
}

/// MD5 hash - returns hex string
const char* dragon_md5(const char* data, int64_t len) {
    uint8_t hash[16];
    mbedtls_md5((const unsigned char*)data, (size_t)len, hash);
    return dragon_raw_bytes_hex(hash, 16);
}

/// SHA-256 hash - returns raw 32-byte DragonBytes
DragonBytes* dragon_sha256_bytes(DragonBytes* data) {
    uint8_t hash[32];
    size_t n = data ? (size_t)data->len : 0;
    const unsigned char* p = (data && data->data) ? (const unsigned char*)data->data
                                                   : dragon_empty_input;
    mbedtls_sha256(p, n, hash, 0);
    return dragon_bytes_new(hash, 32);
}

/// SHA-1 hash - returns raw 20-byte DragonBytes
DragonBytes* dragon_sha1_bytes(DragonBytes* data) {
    uint8_t hash[20];
    size_t n = data ? (size_t)data->len : 0;
    const unsigned char* p = (data && data->data) ? (const unsigned char*)data->data
                                                   : dragon_empty_input;
    mbedtls_sha1(p, n, hash);
    return dragon_bytes_new(hash, 20);
}

/// MD5 hash - returns raw 16-byte DragonBytes
DragonBytes* dragon_md5_bytes(DragonBytes* data) {
    uint8_t hash[16];
    size_t n = data ? (size_t)data->len : 0;
    const unsigned char* p = (data && data->data) ? (const unsigned char*)data->data
                                                   : dragon_empty_input;
    mbedtls_md5(p, n, hash);
    return dragon_bytes_new(hash, 16);
}

// --- SHA-2 wide family (224/384/512) -------------------------------------
//
// SHA-224 is SHA-256 truncated to 28 bytes (`is224=1`); SHA-384 is SHA-512
// truncated to 48 bytes (`is384=1`). mbedTLS writes the full state into the
// 32/64-byte buffer either way and the leading bytes are the digest, so we
// allocate the full width and emit only the truncated prefix.

/// SHA-224 hash - returns hex string
const char* dragon_sha224(const char* data, int64_t len) {
    uint8_t hash[32];
    mbedtls_sha256((const unsigned char*)data, (size_t)len, hash, 1);
    return dragon_raw_bytes_hex(hash, 28);
}

/// SHA-384 hash - returns hex string
const char* dragon_sha384(const char* data, int64_t len) {
    uint8_t hash[64];
    mbedtls_sha512((const unsigned char*)data, (size_t)len, hash, 1);
    return dragon_raw_bytes_hex(hash, 48);
}

/// SHA-512 hash - returns hex string
const char* dragon_sha512(const char* data, int64_t len) {
    uint8_t hash[64];
    mbedtls_sha512((const unsigned char*)data, (size_t)len, hash, 0);
    return dragon_raw_bytes_hex(hash, 64);
}

/// SHA-224 hash - returns raw 28-byte DragonBytes
DragonBytes* dragon_sha224_bytes(DragonBytes* data) {
    uint8_t hash[32];
    size_t n = data ? (size_t)data->len : 0;
    const unsigned char* p = (data && data->data) ? (const unsigned char*)data->data
                                                   : dragon_empty_input;
    mbedtls_sha256(p, n, hash, 1);
    return dragon_bytes_new(hash, 28);
}

/// SHA-384 hash - returns raw 48-byte DragonBytes
DragonBytes* dragon_sha384_bytes(DragonBytes* data) {
    uint8_t hash[64];
    size_t n = data ? (size_t)data->len : 0;
    const unsigned char* p = (data && data->data) ? (const unsigned char*)data->data
                                                   : dragon_empty_input;
    mbedtls_sha512(p, n, hash, 1);
    return dragon_bytes_new(hash, 48);
}

/// SHA-512 hash - returns raw 64-byte DragonBytes
DragonBytes* dragon_sha512_bytes(DragonBytes* data) {
    uint8_t hash[64];
    size_t n = data ? (size_t)data->len : 0;
    const unsigned char* p = (data && data->data) ? (const unsigned char*)data->data
                                                   : dragon_empty_input;
    mbedtls_sha512(p, n, hash, 0);
    return dragon_bytes_new(hash, 64);
}

/// HMAC(key, msg) for digestmod "sha256" | "sha1" | "md5" - returns raw
/// DragonBytes. mbedTLS performs the full RFC 2104 construction, including
/// block-size key normalization, replacing the former two-pass `.dr` HMAC.
/// `name` is unrecognized -> returns NULL (hmac.dr validates names first).
DragonBytes* dragon_hmac(const char* name, DragonBytes* key, DragonBytes* msg) {
    mbedtls_md_type_t mdt;
    if (strcmp(name, "sha256") == 0)      mdt = MBEDTLS_MD_SHA256;
    else if (strcmp(name, "sha512") == 0) mdt = MBEDTLS_MD_SHA512;
    else if (strcmp(name, "sha384") == 0) mdt = MBEDTLS_MD_SHA384;
    else if (strcmp(name, "sha224") == 0) mdt = MBEDTLS_MD_SHA224;
    else if (strcmp(name, "sha1") == 0)   mdt = MBEDTLS_MD_SHA1;
    else if (strcmp(name, "md5") == 0)    mdt = MBEDTLS_MD_MD5;
    else return nullptr;

    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(mdt);
    if (!info) return nullptr;

    size_t klen = key ? (size_t)key->len : 0;
    const unsigned char* kp = (key && key->data) ? (const unsigned char*)key->data
                                                  : dragon_empty_input;
    size_t mlen = msg ? (size_t)msg->len : 0;
    const unsigned char* mp = (msg && msg->data) ? (const unsigned char*)msg->data
                                                  : dragon_empty_input;

    uint8_t out[64];  // SHA-512 is the widest digest we support
    mbedtls_md_hmac(info, kp, klen, mp, mlen, out);
    DragonBytes* result = dragon_bytes_new(out, (int64_t)mbedtls_md_get_size(info));
    // wipe MAC scratch: HMAC tag derived from the secret key - clear from stack.
    mbedtls_platform_zeroize(out, sizeof(out));
    return result;
}

// --- Asymmetric sign/verify (RSA / ECDSA) --------------------------------
//
// Dragon superset surface (crypto.dr): primitives Python's stdlib never had.
// One sign/verify pair covers BOTH algorithms - mbedTLS's `pk` layer dispatches
// on the parsed key type (RSA key -> PKCS#1 v1.5, EC key -> ECDSA DER), so the
// caller picks the algorithm by which key they pass. Messages are hashed here
// with `md_name`; the signature is computed over that digest.

static mbedtls_md_type_t dragon_md_type(const char* name) {
    if (strcmp(name, "sha256") == 0) return MBEDTLS_MD_SHA256;
    if (strcmp(name, "sha512") == 0) return MBEDTLS_MD_SHA512;
    if (strcmp(name, "sha384") == 0) return MBEDTLS_MD_SHA384;
    if (strcmp(name, "sha224") == 0) return MBEDTLS_MD_SHA224;
    if (strcmp(name, "sha1") == 0)   return MBEDTLS_MD_SHA1;
    return MBEDTLS_MD_NONE;
}

// Hash `msg` with `mdt` into `out` (>=64 bytes). Returns digest length, 0 on
// failure / unknown algorithm.
static size_t dragon_md_digest(mbedtls_md_type_t mdt, const unsigned char* msg,
                               size_t mlen, unsigned char* out) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(mdt);
    if (!info) return 0;
    if (mbedtls_md(info, msg, mlen, out) != 0) return 0;
    return mbedtls_md_get_size(info);
}

// PEM is text: mbedTLS requires the buffer to be NUL-terminated and the length
// to INCLUDE that terminator. DragonBytes need not be, so copy. Caller frees.
static unsigned char* dragon_pem_dup(DragonBytes* b, size_t* outlen) {
    size_t n = b ? (size_t)b->len : 0;
    unsigned char* buf = (unsigned char*)malloc(n + 1);
    if (!buf) { *outlen = 0; return nullptr; }
    if (n && b->data) memcpy(buf, b->data, n);
    buf[n] = 0;
    *outlen = n + 1;
    return buf;
}

/// Sign `msg` (hashed with `md_name`) using a PEM private key (RSA or EC).
/// Returns the signature bytes, or empty bytes on any failure.
DragonBytes* dragon_pk_sign(const char* md_name, DragonBytes* priv_pem,
                            DragonBytes* msg) {
    mbedtls_md_type_t mdt = dragon_md_type(md_name);
    if (mdt == MBEDTLS_MD_NONE) return dragon_bytes_new(nullptr, 0);

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_pk_context pk;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_pk_init(&pk);

    DragonBytes* result = nullptr;
    unsigned char* keybuf = nullptr;
    size_t keybuf_len = 0;
    unsigned char hash[64] = {0};
    unsigned char sig[1024] = {0};   // exceeds any RSA-4096 (512B) / ECDSA DER sig
    const char* pers = "dragon_pk_sign";

    do {
        if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                  (const unsigned char*)pers, strlen(pers)) != 0) break;
        keybuf = dragon_pem_dup(priv_pem, &keybuf_len);
        if (!keybuf) break;
        if (mbedtls_pk_parse_key(&pk, keybuf, keybuf_len, nullptr, 0,
                                 mbedtls_ctr_drbg_random, &ctr_drbg) != 0) break;

        size_t mlen = msg ? (size_t)msg->len : 0;
        const unsigned char* mp = (msg && msg->data) ? (const unsigned char*)msg->data
                                                      : dragon_empty_input;
        size_t hlen = dragon_md_digest(mdt, mp, mlen, hash);
        if (hlen == 0) break;

        size_t siglen = 0;
        if (mbedtls_pk_sign(&pk, mdt, hash, hlen, sig, sizeof(sig), &siglen,
                            mbedtls_ctr_drbg_random, &ctr_drbg) != 0) break;
        result = dragon_bytes_new(sig, (int64_t)siglen);
    } while (0);

    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    if (keybuf) {
        // wipe PEM private-key bytes before free: prevents heap-reuse disclosure.
        mbedtls_platform_zeroize(keybuf, keybuf_len);
        free(keybuf);
    }
    // wipe message digest + signature scratch: digest is derived from caller
    // input and the sig buffer briefly held private-key-derived intermediates.
    mbedtls_platform_zeroize(hash, sizeof(hash));
    mbedtls_platform_zeroize(sig, sizeof(sig));
    return result ? result : dragon_bytes_new(nullptr, 0);
}

/// Verify `sig` over `msg` (hashed with `md_name`) using a PEM public key.
/// Returns 1 if the signature is valid, 0 otherwise.
int dragon_pk_verify(const char* md_name, DragonBytes* pub_pem,
                     DragonBytes* msg, DragonBytes* sig) {
    mbedtls_md_type_t mdt = dragon_md_type(md_name);
    if (mdt == MBEDTLS_MD_NONE) return 0;

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int ok = 0;
    unsigned char* keybuf = nullptr;

    do {
        size_t klen = 0;
        keybuf = dragon_pem_dup(pub_pem, &klen);
        if (!keybuf) break;
        if (mbedtls_pk_parse_public_key(&pk, keybuf, klen) != 0) break;

        unsigned char hash[64];
        size_t mlen = msg ? (size_t)msg->len : 0;
        const unsigned char* mp = (msg && msg->data) ? (const unsigned char*)msg->data
                                                      : dragon_empty_input;
        size_t hlen = dragon_md_digest(mdt, mp, mlen, hash);
        if (hlen == 0) break;

        size_t slen = sig ? (size_t)sig->len : 0;
        const unsigned char* sp = (sig && sig->data) ? (const unsigned char*)sig->data
                                                      : dragon_empty_input;
        if (mbedtls_pk_verify(&pk, mdt, hash, hlen, sp, slen) == 0) ok = 1;
    } while (0);

    mbedtls_pk_free(&pk);
    if (keybuf) free(keybuf);
    return ok;
}

// --- AEAD ciphers (AES-GCM, ChaCha20-Poly1305) ---------------------------
//
// Authenticated encryption: encrypt returns ciphertext with the 16-byte tag
// appended (matching the convention of Python's `cryptography` AESGCM/
// ChaCha20Poly1305). decrypt returns the plaintext, or raises ValueError (the
// blessed dragon_raise_exc, ADR 041) on tag-mismatch - it MUST NOT hand back
// unauthenticated plaintext. All mbedTLS contexts are freed before any raise,
// since dragon_raise_exc longjmps out of this frame.

#define DRAGON_AEAD_TAG 16

static const unsigned char* dragon_b_ptr(DragonBytes* b) {
    return (b && b->data) ? (const unsigned char*)b->data : dragon_empty_input;
}
static size_t dragon_b_len(DragonBytes* b) { return b ? (size_t)b->len : 0; }

/// AES-GCM encrypt -> ciphertext || 16-byte tag.
DragonBytes* dragon_aes_gcm_encrypt(DragonBytes* key, DragonBytes* nonce,
                                    DragonBytes* pt, DragonBytes* aad) {
    size_t klen = dragon_b_len(key);
    if (klen != 16 && klen != 24 && klen != 32) {
        dragon_raise_exc_cstr(90, "aes_gcm: key must be 16, 24, or 32 bytes");
        return nullptr;
    }
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    if (mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, dragon_b_ptr(key),
                           (unsigned)(klen * 8)) != 0) {
        mbedtls_gcm_free(&ctx);
        dragon_raise_exc_cstr(90, "aes_gcm: setkey failed");
        return nullptr;
    }
    size_t ptlen = dragon_b_len(pt);
    unsigned char* out = (unsigned char*)malloc(ptlen + DRAGON_AEAD_TAG);
    if (!out) {
        mbedtls_gcm_free(&ctx);
        dragon_raise_exc_cstr(90, "aes_gcm: out of memory");
        return nullptr;
    }
    unsigned char tag[DRAGON_AEAD_TAG];
    int ret = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, ptlen,
                                        dragon_b_ptr(nonce), dragon_b_len(nonce),
                                        dragon_b_ptr(aad), dragon_b_len(aad),
                                        dragon_b_ptr(pt), out, DRAGON_AEAD_TAG, tag);
    mbedtls_gcm_free(&ctx);
    if (ret != 0) { free(out); dragon_raise_exc_cstr(90, "aes_gcm: encryption failed"); return nullptr; }
    memcpy(out + ptlen, tag, DRAGON_AEAD_TAG);
    DragonBytes* result = dragon_bytes_new(out, (int64_t)(ptlen + DRAGON_AEAD_TAG));
    free(out);
    return result;
}

/// AES-GCM decrypt of ciphertext || tag. Raises ValueError on auth failure.
DragonBytes* dragon_aes_gcm_decrypt(DragonBytes* key, DragonBytes* nonce,
                                    DragonBytes* ct, DragonBytes* aad) {
    size_t klen = dragon_b_len(key);
    if (klen != 16 && klen != 24 && klen != 32) {
        dragon_raise_exc_cstr(90, "aes_gcm: key must be 16, 24, or 32 bytes");
        return nullptr;
    }
    size_t ctlen = dragon_b_len(ct);
    if (ctlen < DRAGON_AEAD_TAG) {
        dragon_raise_exc_cstr(90, "aes_gcm: ciphertext shorter than the authentication tag");
        return nullptr;
    }
    size_t ptlen = ctlen - DRAGON_AEAD_TAG;
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    if (mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, dragon_b_ptr(key),
                           (unsigned)(klen * 8)) != 0) {
        mbedtls_gcm_free(&ctx);
        dragon_raise_exc_cstr(90, "aes_gcm: setkey failed");
        return nullptr;
    }
    const unsigned char* base = dragon_b_ptr(ct);
    size_t out_capacity = ptlen ? ptlen : 1;
    unsigned char* out = (unsigned char*)malloc(out_capacity);
    if (!out) {
        mbedtls_gcm_free(&ctx);
        dragon_raise_exc_cstr(90, "aes_gcm: out of memory");
        return nullptr;
    }
    int ret = mbedtls_gcm_auth_decrypt(&ctx, ptlen,
                                       dragon_b_ptr(nonce), dragon_b_len(nonce),
                                       dragon_b_ptr(aad), dragon_b_len(aad),
                                       base + ptlen, DRAGON_AEAD_TAG, base, out);
    mbedtls_gcm_free(&ctx);
    if (ret != 0) {
        // wipe scratch on auth failure: mbedTLS may leave partial plaintext.
        mbedtls_platform_zeroize(out, out_capacity);
        free(out);
        dragon_raise_exc_cstr(90, "aes_gcm: authentication failed");
        return nullptr;
    }
    DragonBytes* result = dragon_bytes_new(out, (int64_t)ptlen);
    // wipe plaintext scratch before free: dragon_bytes_new copied; this buffer
    // is the only intermediate copy and would otherwise linger in heap reuse.
    mbedtls_platform_zeroize(out, out_capacity);
    free(out);
    return result;
}

/// ChaCha20-Poly1305 encrypt -> ciphertext || 16-byte tag. key=32B, nonce=12B.
DragonBytes* dragon_chacha20poly1305_encrypt(DragonBytes* key, DragonBytes* nonce,
                                             DragonBytes* pt, DragonBytes* aad) {
    if (dragon_b_len(key) != 32) { dragon_raise_exc_cstr(90, "chacha20poly1305: key must be 32 bytes"); return nullptr; }
    if (dragon_b_len(nonce) != 12) { dragon_raise_exc_cstr(90, "chacha20poly1305: nonce must be 12 bytes"); return nullptr; }
    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    if (mbedtls_chachapoly_setkey(&ctx, dragon_b_ptr(key)) != 0) {
        mbedtls_chachapoly_free(&ctx);
        dragon_raise_exc_cstr(90, "chacha20poly1305: setkey failed");
        return nullptr;
    }
    size_t ptlen = dragon_b_len(pt);
    unsigned char* out = (unsigned char*)malloc(ptlen + DRAGON_AEAD_TAG);
    if (!out) {
        mbedtls_chachapoly_free(&ctx);
        dragon_raise_exc_cstr(90, "chacha20poly1305: out of memory");
        return nullptr;
    }
    unsigned char tag[DRAGON_AEAD_TAG];
    int ret = mbedtls_chachapoly_encrypt_and_tag(&ctx, ptlen, dragon_b_ptr(nonce),
                                                 dragon_b_ptr(aad), dragon_b_len(aad),
                                                 dragon_b_ptr(pt), out, tag);
    mbedtls_chachapoly_free(&ctx);
    if (ret != 0) { free(out); dragon_raise_exc_cstr(90, "chacha20poly1305: encryption failed"); return nullptr; }
    memcpy(out + ptlen, tag, DRAGON_AEAD_TAG);
    DragonBytes* result = dragon_bytes_new(out, (int64_t)(ptlen + DRAGON_AEAD_TAG));
    free(out);
    return result;
}

/// ChaCha20-Poly1305 decrypt of ciphertext || tag. Raises ValueError on auth failure.
DragonBytes* dragon_chacha20poly1305_decrypt(DragonBytes* key, DragonBytes* nonce,
                                             DragonBytes* ct, DragonBytes* aad) {
    if (dragon_b_len(key) != 32) { dragon_raise_exc_cstr(90, "chacha20poly1305: key must be 32 bytes"); return nullptr; }
    if (dragon_b_len(nonce) != 12) { dragon_raise_exc_cstr(90, "chacha20poly1305: nonce must be 12 bytes"); return nullptr; }
    size_t ctlen = dragon_b_len(ct);
    if (ctlen < DRAGON_AEAD_TAG) {
        dragon_raise_exc_cstr(90, "chacha20poly1305: ciphertext shorter than the authentication tag");
        return nullptr;
    }
    size_t ptlen = ctlen - DRAGON_AEAD_TAG;
    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    if (mbedtls_chachapoly_setkey(&ctx, dragon_b_ptr(key)) != 0) {
        mbedtls_chachapoly_free(&ctx);
        dragon_raise_exc_cstr(90, "chacha20poly1305: setkey failed");
        return nullptr;
    }
    const unsigned char* base = dragon_b_ptr(ct);
    size_t out_capacity = ptlen ? ptlen : 1;
    unsigned char* out = (unsigned char*)malloc(out_capacity);
    if (!out) {
        mbedtls_chachapoly_free(&ctx);
        dragon_raise_exc_cstr(90, "chacha20poly1305: out of memory");
        return nullptr;
    }
    int ret = mbedtls_chachapoly_auth_decrypt(&ctx, ptlen, dragon_b_ptr(nonce),
                                              dragon_b_ptr(aad), dragon_b_len(aad),
                                              base + ptlen, base, out);
    mbedtls_chachapoly_free(&ctx);
    if (ret != 0) {
        // wipe scratch on auth failure: mbedTLS may leave partial plaintext.
        mbedtls_platform_zeroize(out, out_capacity);
        free(out);
        dragon_raise_exc_cstr(90, "chacha20poly1305: authentication failed");
        return nullptr;
    }
    DragonBytes* result = dragon_bytes_new(out, (int64_t)ptlen);
    // wipe plaintext scratch before free: defense-in-depth, same as AES-GCM.
    mbedtls_platform_zeroize(out, out_capacity);
    free(out);
    return result;
}

}  // extern "C"
