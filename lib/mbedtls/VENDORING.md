# Vendored mbedTLS

Per ADR 038, Dragon embeds mbedTLS as its TLS + X.509 + crypto engine.

- **Upstream:** https://github.com/Mbed-TLS/mbedtls
- **Version:** `mbedtls-3.6.6` (the 3.6 LTS line — security backports without API churn)
- **Source tarball:** https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.6/mbedtls-3.6.6.tar.bz2
- **Tarball SHA-256:** `8fb65fae8dcae5840f793c0a334860a411f884cc537ea290ce1c52bb64ca007a`
- **License:** Apache-2.0. mbedTLS is dual-licensed Apache-2.0 OR GPL-2.0-or-later; Dragon takes **Apache-2.0**. See `LICENSE`.
- **Fetched:** 2026-05-20

## Kept vs. stripped

Kept the library-only subset: `library/`, `include/`, `3rdparty/`, `LICENSE`, `ChangeLog`.

Stripped (not needed for a static library build): `tests/` (32M), `framework/` (7M), `programs/`, `docs/`, `doxygen/`, `visualc/`, `scripts/`, `cmake/`, `configs/`, `pkgconfig/`.

## How it's built

Built SQLite-style by Dragon's top-level `CMakeLists.txt` (target `dragon_mbedtls`): compile `library/*.c` directly with `-I include` + `-I library` (the release tarball pre-generates `psa_crypto_driver_wrappers.h`, `version_features.c`, `error.c` into `library/`). 3rdparty everest/p256-m sources are present but not compiled unless their config options are enabled.

Config: uses mbedTLS's **default** `include/mbedtls/mbedtls_config.h` for now. A trimmed, in-scope-only config (TLS 1.2/1.3, modern AEAD suites, X25519/P-256, ECDSA/RSA, X.509; legacy compiled out) is a follow-up per ADR 038.

## Updating

Re-fetch the next `3.6.x` LTS tarball, replace `library/` `include/` `3rdparty/`, and update the version + SHA-256 above. **Track mbedTLS security advisories for the 3.6 LTS line** — each relevant CVE is a Dragon release.
