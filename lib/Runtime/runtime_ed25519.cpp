#include "runtime_internal.h"
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <mbedtls/sha512.h>
#include <mbedtls/platform_util.h>
#include "crypto_api.h"

extern "C" {

DragonBytes* dragon_urandom(int64_t n);

int crypto_hash_sha512(unsigned char *out, const unsigned char *in,
                       unsigned long long inlen) {
    return mbedtls_sha512(in, (size_t)inlen, out, 0);
}

void dragon_ed25519_randombytes(unsigned char *buf, size_t len) {
    DragonBytes* r = dragon_urandom((int64_t)len);
    if (r && r->data) memcpy(buf, r->data, len);
    dragon_decref((void*)r);
}

static const unsigned char dragon_ed_empty[1] = {0};

DragonBytes* dragon_ed25519_keypair(void) {
    unsigned char pk[32];
    unsigned char sk[64];
    crypto_sign_ed25519_keypair(pk, sk);
    DragonBytes* out = dragon_bytes_new(sk, 64);
    mbedtls_platform_zeroize(sk, sizeof(sk));
    return out;
}

DragonBytes* dragon_ed25519_public_from_seed(DragonBytes* seed) {
    if (!seed || seed->len != 32) {
        dragon_raise_exc_cstr(90, "ed25519: private key (seed) must be 32 bytes");
        return nullptr;
    }
    unsigned char pk[32];
    unsigned char sk[64];
    crypto_sign_ed25519_seed_keypair(pk, sk, (const unsigned char*)seed->data);
    DragonBytes* out = dragon_bytes_new(pk, 32);
    mbedtls_platform_zeroize(sk, sizeof(sk));
    return out;
}

DragonBytes* dragon_ed25519_sign(DragonBytes* seed, DragonBytes* msg) {
    if (!seed || seed->len != 32) {
        dragon_raise_exc_cstr(90, "ed25519: private key (seed) must be 32 bytes");
        return nullptr;
    }
    size_t mlen = msg ? (size_t)msg->len : 0;
    const unsigned char* mp = (msg && msg->data) ? (const unsigned char*)msg->data
                                                 : dragon_ed_empty;
    size_t sm_capacity = mlen + 64;
    unsigned char* sm = (unsigned char*)dragon_xmalloc(sm_capacity);
    DragonBytes* sig = dragon_bytes_new(nullptr, 64);

    unsigned char pk[32];
    unsigned char sk[64];
    crypto_sign_ed25519_seed_keypair(pk, sk, (const unsigned char*)seed->data);
    unsigned long long smlen = 0;
    crypto_sign_ed25519(sm, &smlen, mp, (unsigned long long)mlen, sk);
    memcpy(sig->data, sm, 64);
    mbedtls_platform_zeroize(sk, sizeof(sk));
    mbedtls_platform_zeroize(sm, sm_capacity);
    free(sm);
    return sig;
}

int dragon_ed25519_verify(DragonBytes* pk, DragonBytes* msg, DragonBytes* sig) {
    if (!pk || pk->len != 32) return 0;
    if (!sig || sig->len != 64) return 0;
    size_t mlen = msg ? (size_t)msg->len : 0;
    const unsigned char* mp = (msg && msg->data) ? (const unsigned char*)msg->data
                                                 : dragon_ed_empty;
    unsigned long long smlen = (unsigned long long)mlen + 64;
    unsigned char* sm = (unsigned char*)dragon_xmalloc((size_t)smlen);
    unsigned char* mout = (unsigned char*)dragon_malloc_nullable((size_t)smlen);
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

}
