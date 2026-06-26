/* dragon_mbedtls_config.h — Dragon's delta over mbedTLS's default config.
 *
 * Included via MBEDTLS_USER_CONFIG_FILE, AFTER include/mbedtls/mbedtls_config.h
 * and BEFORE config_adjust_*.h + check_config.h (see build_info.h). We only
 * DISABLE here: each #undef trims an out-of-scope module per ADR 038 §2.
 *
 * MBEDTLS_PSA_CRYPTO_CONFIG is off, so PSA_WANT_* are derived from these legacy
 * macros by config_adjust_psa_from_legacy.h — disabling a legacy MBEDTLS_*_C
 * macro disables its PSA twin automatically. So we touch only legacy macros.
 *
 * This file is PUBLIC (propagated to every consumer via CMake) so the library
 * and all callers compile against the IDENTICAL config — a mismatch would mean
 * different struct sizes on each side.
 *
 * Kept deliberately (do NOT trim — real-world interop):
 *   - All NIST + Brainpool + 25519/448 curves: cert chains in the wild are
 *     signed on P-256/384/521 (occasionally Brainpool). The ECDHE *offering*
 *     is narrowed to X25519 + P-256 at runtime via mbedtls_ssl_conf_groups,
 *     not by compiling curves out.
 *   - SHA-1 / MD5 primitives: needed to COMPUTE hashes while parsing legacy
 *     cert chains. SHA-1/MD5 *signatures* are rejected at the cert-profile /
 *     sig-alg layer, never by removing the primitive.
 *   - AES-CBC mode: used to parse encrypted PKCS#8 private keys.
 */
#ifndef DRAGON_MBEDTLS_CONFIG_H
#define DRAGON_MBEDTLS_CONFIG_H

/* --- DTLS: Dragon is stream-TLS only (ADR 038 §2 excludes DTLS). --- */
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID_COMPAT

/* --- Symmetric ciphers outside scope (we ship AES-GCM + ChaCha20-Poly1305). --- */
#undef MBEDTLS_CAMELLIA_C
#undef MBEDTLS_ARIA_C
#undef MBEDTLS_DES_C        /* 3DES — legacy, excluded */
#undef MBEDTLS_CCM_C        /* we negotiate GCM + ChaCha20-Poly1305, not CCM */
#undef MBEDTLS_NIST_KW_C    /* AES key-wrap — not used by TLS */

/* --- Test-only code; no place in a shipped library. --- */
#undef MBEDTLS_SELF_TEST

#endif /* DRAGON_MBEDTLS_CONFIG_H */
