/* Dragon-adapted replacement for OpenSSH-portable's crypto_api.h.
 *
 * The upstream header wires crypto_hash_sha512 to OpenSSL/<sha2.h> and
 * randombytes to arc4random_buf, and pulls in "includes.h" - none of which
 * exist in Dragon's tree. This replacement keeps the exact symbol contract the
 * vendored ed25519.c depends on, but routes the two backends to Dragon's
 * runtime (SHA-512 -> mbedTLS, randombytes -> getrandom), both defined in
 * lib/Runtime/runtime_ed25519.cpp. ed25519.c itself is untouched (save the
 * documented seed_keypair addition). */
#ifndef crypto_api_h
#define crypto_api_h

#include <stdint.h>
#include <stdlib.h>

typedef int8_t crypto_int8;
typedef uint8_t crypto_uint8;
typedef int16_t crypto_int16;
typedef uint16_t crypto_uint16;
typedef int32_t crypto_int32;
typedef uint32_t crypto_uint32;
typedef int64_t crypto_int64;
typedef uint64_t crypto_uint64;

#ifdef __cplusplus
extern "C" {
#endif

/* Backends supplied by runtime_ed25519.cpp. */
void dragon_ed25519_randombytes(unsigned char *buf, size_t len);
int  crypto_hash_sha512(unsigned char *out, const unsigned char *in,
                        unsigned long long inlen);

#define randombytes(buf, buf_len) dragon_ed25519_randombytes((unsigned char *)(buf), (size_t)(buf_len))
#define crypto_hash_sha512_BYTES 64U

#define crypto_sign_ed25519_SECRETKEYBYTES 64U
#define crypto_sign_ed25519_PUBLICKEYBYTES 32U
#define crypto_sign_ed25519_BYTES 64U

int crypto_sign_ed25519(unsigned char *, unsigned long long *,
    const unsigned char *, unsigned long long, const unsigned char *);
int crypto_sign_ed25519_open(unsigned char *, unsigned long long *,
    const unsigned char *, unsigned long long, const unsigned char *);
int crypto_sign_ed25519_keypair(unsigned char *, unsigned char *);
/* Dragon addition (see ed25519.c): deterministic keypair from a 32-byte seed. */
int crypto_sign_ed25519_seed_keypair(unsigned char *, unsigned char *,
    const unsigned char *);

#ifdef __cplusplus
}
#endif

#endif /* crypto_api_h */
