/// Dragon Runtime - Ed25519 (EdDSA), via the vendored OpenSSH ref10
/// implementation in lib/ed25519/ed25519.c. Own translation unit (mirrors
/// runtime_crypto.cpp / runtime_tls.cpp) so ref10's field arithmetic links only
/// when a program signs with Ed25519.
///
/// API mirrors RFC 8032 / Python's `cryptography`: private key is a 32-byte
/// seed, public key 32 bytes, signatures 64-byte detached.

#include "runtime_internal.h"
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <mbedtls/sha512.h>
#include <mbedtls/platform_util.h>
#include "crypto_api.h"   // ed25519 API + backend decls (C linkage via its guard)

extern "C" {

// Defined in runtime_platform.cpp (not declared in runtime_internal.h).
DragonBytes* dragon_urandom(int64_t n);

// Backends required by the vendored ed25519.c.

int crypto_hash_sha512(unsigned char *out, const unsigned char *in,
                       unsigned long long inlen) {
    return mbedtls_sha512(in, (size_t)inlen, out, 0);  // is384=0 selects SHA-512
}

void dragon_ed25519_randombytes(unsigned char *buf, size_t len) {
    // dragon_urandom hands back a fresh refcount=1 value; release after copying.
    DragonBytes* r = dragon_urandom((int64_t)len);
    if (r && r->data) memcpy(buf, r->data, len);
    dragon_decref((void*)r);
}

// Dragon-facing API (crypto.dr).

static const unsigned char dragon_ed_empty[1] = {0};

/// Fresh keypair -> 64 bytes = seed(32) || public_key(32). crypto.dr splits it.
DragonBytes* dragon_ed25519_keypair(void) {
    unsigned char pk[32];
    unsigned char sk[64];
    crypto_sign_ed25519_keypair(pk, sk);   // sk = seed || pk
    DragonBytes* out = dragon_bytes_new(sk, 64);
    // wipe local sk copy; dragon_bytes_new already copied the secret seed out.
    mbedtls_platform_zeroize(sk, sizeof(sk));
    return out;
}

/// Derive the 32-byte public key from a 32-byte seed private key.
DragonBytes* dragon_ed25519_public_from_seed(DragonBytes* seed) {
    if (!seed || seed->len != 32) {
        dragon_raise_exc_cstr(90, "ed25519: private key (seed) must be 32 bytes");
        return nullptr;
    }
    unsigned char pk[32];
    unsigned char sk[64];
    crypto_sign_ed25519_seed_keypair(pk, sk, (const unsigned char*)seed->data);
    DragonBytes* out = dragon_bytes_new(pk, 32);
    mbedtls_platform_zeroize(sk, sizeof(sk));  // wipe private key; pk is public
    return out;
}

/// Sign `msg` with a 32-byte seed private key -> 64-byte detached signature.
DragonBytes* dragon_ed25519_sign(DragonBytes* seed, DragonBytes* msg) {
    if (!seed || seed->len != 32) {
        dragon_raise_exc_cstr(90, "ed25519: private key (seed) must be 32 bytes");
        return nullptr;
    }
    size_t mlen = msg ? (size_t)msg->len : 0;
    const unsigned char* mp = (msg && msg->data) ? (const unsigned char*)msg->data
                                                 : dragon_ed_empty;
    // Allocations happen before the private key is derived, so an OOM raise
    // here unwinds with no secret material on the stack to wipe.
    size_t sm_capacity = mlen + 64;
    unsigned char* sm = (unsigned char*)dragon_xmalloc(sm_capacity);  // ref10 writes sig||msg
    DragonBytes* sig = dragon_bytes_new(nullptr, 64);       // detached = R(32)||S(32)

    unsigned char pk[32];
    unsigned char sk[64];
    crypto_sign_ed25519_seed_keypair(pk, sk, (const unsigned char*)seed->data);
    unsigned long long smlen = 0;
    crypto_sign_ed25519(sm, &smlen, mp, (unsigned long long)mlen, sk);
    memcpy(sig->data, sm, 64);
    // sm holds signature||msg (public) but is wiped too, defensively, since
    // ref10's scratch may overlap secret-derived state.
    mbedtls_platform_zeroize(sk, sizeof(sk));
    mbedtls_platform_zeroize(sm, sm_capacity);
    free(sm);
    return sig;
}

/// Verify a 64-byte detached signature over `msg` with a 32-byte public key.
int dragon_ed25519_verify(DragonBytes* pk, DragonBytes* msg, DragonBytes* sig) {
    if (!pk || pk->len != 32) return 0;
    if (!sig || sig->len != 64) return 0;
    size_t mlen = msg ? (size_t)msg->len : 0;
    const unsigned char* mp = (msg && msg->data) ? (const unsigned char*)msg->data
                                                 : dragon_ed_empty;
    // ref10's _open consumes the combined sig||msg and recovers the message.
    unsigned long long smlen = (unsigned long long)mlen + 64;
    unsigned char* sm = (unsigned char*)dragon_xmalloc((size_t)smlen);
    unsigned char* mout = (unsigned char*)malloc((size_t)smlen);
    if (!mout) { free(sm); dragon_raise_oom(); }
    memcpy(sm, sig->data, 64);
    if (mlen) memcpy(sm + 64, mp, mlen);
    unsigned long long moutlen = 0;
    int ret = crypto_sign_ed25519_open(mout, &moutlen, sm, smlen,
                                       (const unsigned char*)pk->data);
    free(sm);
    free(mout);
    return ret == 0 ? 1 : 0;
}

}  // extern "C"
