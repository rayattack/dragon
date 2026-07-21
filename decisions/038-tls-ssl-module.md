# Decision 038: TLS Support - Embed mbedTLS behind a Python-parity `ssl` Module

**Status:** Approved

I wanted pure-Dragon TLS for dogfooding and CVE control. Then I did the math: 5-9 months to first HTTPS, ~5× slower than OpenSSL until asm lands, and me rolling my own constant-time AES-GCM. That's how you get silent MITMs and a very bad year. mbedTLS is ~3-10 MB, permissive license, TLS 1.3-capable - embedding it is the boring correct choice.

## Revision History

- **Original.** Considered vendoring aws-lc and chose **Option D: a pure-Dragon `ssl.dr`** - TLS 1.3 state machine, X.509, AEAD, ECC, RSA all in `.dr`, with asm fast paths.
- **Reversed to embed mbedTLS.** ADR was never approved-by-implementation, so edited in place rather than superseded. Pure-Dragon TLS is a 5-9 month block that ships **self-rolled, unaudited** crypto running **~5× slower than OpenSSL** until asm lands (~year two), and motto #1 (speed) therefore *favors* a mature C engine for the foreseeable future. mbedTLS is the only candidate that is permissive-licensed, self-contained, maintained, TLS 1.3-capable, and small.

## Summary

Dragon ships TLS by **embedding mbedTLS** (3.6 LTS), statically linked into the Dragon binary, behind a CPython-parity `stdlib/ssl.dr` surface (`SSLContext` / `wrap_socket` / `SSLError` / …). mbedTLS supplies TLS 1.2 **and** 1.3 state machines, X.509 path validation, AEAD ciphers, ECDHE/X25519/P-256, ECDSA/RSA, and a CSPRNG-seeded DRBG.

No reverse proxy. No system TLS dependency. No third-party TLS pulled at runtime - mbedTLS lives inside the Dragon binary, same as CPython/Node bundle OpenSSL, same as Dragon already bundles minicoro, libuv, SQLite, and PCRE2. `hashlib` / `hmac` / `secrets` remain the public hashing/MAC/token API (KAT-pinned in `test/dr/`), but the hand-rolled crypto *engine* behind `hashlib`/`hmac` is **retired in favor of mbedTLS** (`secrets`/`urandom` stay pure-Dragon on direct `getrandom`); the `.dr` KATs become the correctness oracle. Testing dogfoods `stdlib/unittest.dr` (`.dr` KATs, `ssl`-surface tests, HTTPS round-trips) wherever expressible; mbedTLS's own fuzzed test suite covers engine internals.

## Context / Motivation

`stdlib/http/server.dr` is HTTP-only. Every Dragon-served public site and every client of a non-trivial third-party API or cloud database (ADR 032) needs TLS. "Terminate at nginx" violates the no-workarounds rule; "install OpenSSL before `dragon run` works" fails on Windows (ships none), macOS (deprecated system OpenSSL since 10.11), Alpine (LibreSSL), Linux distros split across `.so.3`/`.so.1.1`. TLS must be **batteries-included in the binary**.

The original ADR chose **pure-Dragon** for dogfooding, source-tree footprint, and CVE control. On harder examination those reasons don't survive (I had to convince myself on this one):

1. **Rolling your own crypto is the cardinal security sin.** New, unaudited AES-GCM, X.509 path validation, and constant-time code from a small team. A cert-validation bypass or timing side-channel is a *silent* MITM - worse than plaintext because it manufactures false confidence. Original ADR flagged constant-time as "the single highest-risk area" and predicted 5-15% real-world interop failure at v1.
2. **Motto #1 (speed) backfires for years.** Pure-Dragon v1.0 runs ~5× slower than CPython (= OpenSSL) until asm fast paths at Phases 19-20 (~year two). For that window Dragon ships *slower* TLS than embedding a mature C engine. "Pick the fastest emitted code" argues *against* from-scratch in the near and mid term.
3. **5-9 months to first HTTPS** blocks ADR 032 cloud-database support and every HTTPS use case for most of a year.
4. **Sole-upstream CVE burden.** Pure-Dragon makes us the sole upstream for *every* TLS vulnerability, small team, no fuzzing/audit infrastructure - larger ongoing liability than tracking a hardened library.

The footprint argument that killed aws-lc (~160 MB) and OpenSSL (~80 MB) **doesn't apply to mbedTLS** (~3-10 MB in-scope source, same ballpark as the original 3-5 MB pure-Dragon estimate). BearSSL rejection (abandoned, no TLS 1.3) doesn't apply either. mbedTLS was rejected *by association* with "no vendored C crypto," not on its own merits.

## Options Considered

### Option A - Vendor aws-lc
~160 MB committed source; single-vendor (AWS) governance. **Rejected:** footprint dwarfs the Dragon tree; CVE cadence on AWS's calendar.

### Option B - Vendor OpenSSL 3.x
~80 MB; ~25-35 CVEs/yr. **Rejected:** footprint + worst CVE-release treadmill in the field.

### Option C - System-link `libssl`
Zero tree cost, but users must *provide* TLS. **Rejected:** "fresh `dragon run` doesn't work on Windows/macOS/Alpine without a setup ritual" is exactly what batteries-included rejects. (May return as optional Linux-server fast path - see Open Questions.)

### Option D - Pure-Dragon TLS (originally chosen, now **rejected**)
TLS 1.3 + X.509 + all crypto in `.dr`, asm fast paths. **Rejected:** see Context items 1-4. Remains a *possible someday-replacement*, never a blocker.

### Option E - Embed mbedTLS - **CHOSEN**
Statically link mbedTLS 3.6 LTS; wrap behind `stdlib/ssl.dr`. **Only** candidate that is all five of: permissive-licensed, self-contained, maintained, TLS 1.3-capable, and small.

| Library | Permissive license | Self-contained crypto+protocol | Maintained | TLS 1.3 | Small | Verdict |
|---|---|---|---|---|---|---|
| **mbedTLS** | done Apache-2.0 | done | done TrustedFirmware/ARM | done | done ~3-10 MB | **CHOSEN** |
| wolfSSL | no GPL-2.0 / commercial | done | done | done | done | reject - copyleft radioactive for permissive tree |
| s2n-tls | done Apache-2.0 | no needs `libcrypto` (aws-lc) | done AWS | done | no (drags in aws-lc) | reject - re-imports aws-lc footprint |
| picotls | done | no needs OpenSSL/minicrypto backend | ~ | done (1.3-only) | done | reject - not self-contained |
| BearSSL | done | done | no abandoned since then | no | done | reject - dead, no 1.3 |
| tlse | done | done (libtomcrypt) | no single-author | claimed | done | reject - unaudited hobby-grade |

**Discriminator:** self-contained **and** permissive **and** maintained **and** TLS 1.3. Only mbedTLS clears all four.

## Decision

### 1. Architecture

Embed **mbedTLS 3.6 LTS**, statically linked. Thin C shim (`lib/Runtime/runtime_tls.cpp`) bridges mbedTLS to Dragon - context lifecycle, BIO `send`/`recv` callbacks over Dragon's `socket.TcpStream` fds, handshake drive, read/write, certificate verification, SNI, ALPN, error-code → `SSLError` mapping - exposed as `extern "C"` `dragon_tls_*` symbols. `stdlib/ssl.dr` wraps the shim into Python-parity surface. Shim is glue, not crypto.

### 2. Scope - v1

mbedTLS provides both protocol versions, so v1 is **broader** than the original 1.3-only plan, at no extra cost:

- **Protocols:** TLS 1.3 (RFC 8446) **and** TLS 1.2 (RFC 5246), client and server.
- **Ciphersuites (modern only, legacy compiled out):** `TLS_AES_128_GCM_SHA256`, `TLS_AES_256_GCM_SHA384`, `TLS_CHACHA20_POLY1305_SHA256`; for 1.2 the ECDHE-ECDSA / ECDHE-RSA AES-GCM and CHACHA20-POLY1305 suites.
- **Key exchange:** X25519, P-256 (P-384/P-521 available in mbedTLS, enabled as needed).
- **Signatures:** ECDSA-P256, RSA-PSS, RSA-PKCS#1 v1.5 *verify* (required for real-world cert chains), Ed25519 (mbedTLS-dependent).
- **X.509:** full PKIX path validation, SAN, basic constraints, key usage, EKU, name constraints, hostname matching (RFC 6125).
- **Features:** SNI, ALPN, session resumption, HelloRetryRequest, certificate verification.
- **Compiled OUT** via `mbedtls_config.h`: TLS ≤ 1.1 / SSLv3, RC4, DES/3DES, MD5/SHA-1 signatures, renegotiation, 0-RTT (deferred), DTLS.

### 3. Vendoring & build

- Pin to **mbedTLS 3.6 LTS** line (security backports without API churn). Vendored under `lib/mbedtls/` - **exempt from file-size rules** like other vendored libs (`lib/SQLite`, `lib/PCRE2`, `lib/Minicoro`).
- Dragon-owned `mbedtls_config.h` enables **only** in-scope features - minimal compiled/CVE surface.
- Static archive via CMake, `DRAGON_MBEDTLS_LIB`, mirroring `DRAGON_SQLITE3_LIB` / `DRAGON_PCRE2_LIB` / `DRAGON_LIBUV_LIB`.
- License: Apache-2.0 (mbedTLS dual Apache-2.0 / GPL-2.0; Dragon takes Apache-2.0). Record in third-party license manifest.

### 4. Dragon API: `stdlib/ssl.dr`

Engine-agnostic Python-parity surface:

```dr
class SSLContext {
 def(protocol: int = PROTOCOL_TLS_CLIENT) { ... }
 def load_cert_chain(certfile: str, keyfile: str = "") -> None { ... }
 def load_verify_locations(cafile: str = "", capath: str = "") -> None { ... }
 def load_default_certs -> None { ... }
 def set_ciphers(ciphers: str) -> None { ... }
 def set_alpn_protocols(protocols: list[str]) -> None { ... }
 def wrap_socket(sock: socket, server_side: bool = false,
 server_hostname: str = "",
 do_handshake_on_connect: bool = true) -> SSLSocket { ... }
}

def create_default_context(purpose: int = Purpose.SERVER_AUTH,
 cafile: str = "", capath: str = "",
 cadata: str = "") -> SSLContext { ... }

const PROTOCOL_TLS_CLIENT: int = ...
const PROTOCOL_TLS_SERVER: int = ...
const CERT_NONE: int = 0
const CERT_OPTIONAL: int = 1
const CERT_REQUIRED: int = 2

class SSLError(OSError) { ... }
class SSLCertVerificationError(SSLError) { ... }
class SSLZeroReturnError(SSLError) { ... }
class SSLWantReadError(SSLError) { ... }
class SSLWantWriteError(SSLError) { ... }
```

### 5. Crypto primitives - replace the engine, keep the surface

`hashlib` / `hmac` / `secrets` stay as **public** Python-parity API; `test/dr/` KATs stay. **Decided:** retire hand-rolled crypto *engine* (`lib/Runtime/crypto.h`, two-pass `.dr` HMAC in `hmac.dr`), back `hashlib`/`hmac` with mbedTLS. Rationale: motto #1 (ours is several× slower than mbedTLS's streaming, hardware-accelerated paths) and no reason to maintain a slower duplicate once mbedTLS is in-tree. `secrets` / `urandom` **stay pure-Dragon** - direct `getrandom` is modern best practice (Go's `crypto/rand`, CPython's `secrets`), no userspace DRBG state, fast enough for tokens. `.dr` KATs flip from *implementation* to **correctness oracle** cross-checking mbedTLS output against NIST/RFC vectors.

### 6. Trust store

Ship **Mozilla CA bundle** (certifi-derived PEM) in `share/dragon/certs/cacert.pem`, refreshed each release; `create_default_context.load_default_certs` parses via mbedTLS; honor `SSL_CERT_FILE` / `SSL_CERT_DIR`. macOS Keychain / Windows cert-store bridging is follow-up (Open Questions).

### 7. HTTP integration

- `http.client.HTTPSConnection(host, port, context=None)` - mirrors CPython; replaces placeholder at `stdlib/http/client.dr:13`.
- `http.server.Router` accepts optional `ssl_context: SSLContext`; `app.listen` does TLS handshake per accepted connection (default port 443). Convenience `app.listen_tls(certfile, keyfile)`.

### 8. I/O model

v1 is **blocking on blocking fds**, matching `socket.TcpStream` today, via mbedTLS BIO callbacks over Dragon's recv/send. Non-blocking TLS lands with whatever async socket layer arrives - not blocking v1.

### 9. Phased plan (weeks, not months)

| Phase | Deliverable | Status |
|---|---|---|
| **0** | Audit existing primitives; KAT-pin SHA-256/1, MD5, HMAC, `secrets`, `compare_digest` in `test/dr/` via `stdlib/unittest.dr`; wire into ctest. (Also fixed: entry-module method name-resolution in CodeGen, and `unittest.main` non-zero exit on failure.) | **DONE ** |
| **1** | Vendor mbedTLS 3.6 LTS under `lib/mbedtls/`; Dragon-owned `mbedtls_config.h`; CMake static link + `DRAGON_MBEDTLS_LIB`. | **DONE** |
| **2** | C shim `lib/Runtime/runtime_tls.cpp`: context lifecycle, BIO over `TcpStream`, handshake/read/write, cert verify, SNI, ALPN, error mapping; `extern "C" dragon_tls_*`. | **DONE** (~352 lines) |
| **3** | `stdlib/ssl.dr` Python-parity surface. | **DONE** |
| **4** | Mozilla CA bundle + `load_default_certs` + `SSL_CERT_FILE`/`SSL_CERT_DIR`. | **DONE** |
| **5** | `http.client.HTTPSConnection`; `http.server` TLS (`ssl_context=`, `listen_tls`). | **DONE** |
| **6** | `.dr` `unittest` suites - `test/dr/test_ssl.dr` + `test/dr/test_ssl_roundtrip.dr`, ctest-registered. | **DONE ** |
| **7** | Retire hand-rolled crypto engine; back `hashlib`/`hmac` with mbedTLS; keep `secrets`/`urandom` pure-Dragon; KATs as oracle. | **DONE ** - deleted `crypto.h`; digests + HMAC via mbedTLS in `runtime_crypto.cpp`. `hmac.dr` delegates to `dragon_hmac`. KAT oracle green; ctest 17/17. |
| **8** | **Scheduler-aware (non-blocking) socket + TLS I/O** - `socket.dr` and TLS BIO yield to green-thread scheduler on `EAGAIN`. | **DONE ** - `dragon_nb_*` for socket; BIO does `EAGAIN → dragon_io_wait → retry`. `test/dr/test_socket_vthread.dr`; ctest 19/19. |

**Phase 6 scope note .** Landed: `test_ssl.dr` (12 surface tests) and `test_ssl_roundtrip.dr` (4 live tests - TLS 1.3 ECDSA-P256 loopback with peer verification, plus untrusted-chain, hostname-mismatch, expired-cert negatives). Loopback runs server on dedicated `threading.Thread`, client on main thread (`fire` vthread would stall carrier). Embedded ECDSA-P256 PEM fixtures to temp files, self-contained and offline. **Deferred:** `HTTPSConnection` ↔ `listen_tls` round-trip (needs stoppable in-process server), real external-server interop runner.

Two **root-cause compiler fixes** for `.dr` tests (no test-side workarounds): (1) user-defined exception class as value (`assertRaises(SSLError, …)`) lowered to class descriptor instead of type-code - fixed in `Expressions.cpp`; (2) `bytes` methods had no result type in TypeChecker, boxing `b.decode` into `Any` mis-tagged `str` as `TAG_INT` - fixed in `TypeChecker.cpp`.

### 10. Testing strategy - dogfood `.dr` where expressible

Per ADR 021 and Phase 0:

- **Retained pure-Dragon primitives** - existing KATs (`test_sha256.dr`, etc.).
- **`ssl` surface** - `SSLContext`/`wrap_socket`/exception/constant behavior in `.dr`.
- **HTTPS round-trips** - loopback client↔server over real socket in `.dr`.
- **Cert-verify negatives** - hostname mismatch, expired/untrusted chains via `assertRaises(SSLCertVerificationError, …)`.

What we **don't** re-test in Dragon: mbedTLS's *internal* crypto correctness (AES-GCM/X.509/handshake against NIST/RFC vectors) - that's mbedTLS's fuzzed, reviewed test suite. GoogleTest only for C shim plumbing if isolated from `.dr` caller.

## Trade-offs we're accepting

### Positive
- **HTTPS in weeks, not months.** Unblocks ADR 032 cloud databases and all HTTPS client/server work.
- **Audited, fuzzed, constant-time crypto** from TrustedFirmware/ARM - not self-rolled.
- **Motto #1 satisfied from day one** - mature C with hardware acceleration, no ~2-year slow-transition penalty.
- **TLS 1.2 *and* 1.3 day one** - broader than original 1.3-first scope, free.
- **Small footprint** (~3-10 MB) - comparable to abandoned pure-Dragon estimate, far below aws-lc/OpenSSL.
- **Batteries-included** - statically linked; no nginx, no system `libssl`.

### Negative
- **Dogfooding compromise - consistent with precedent.** TLS engine and heavy crypto are C, not `.dr`. Same class as minicoro, libuv, SQLite, PCRE2 already in-tree. Dogfooding carves out "genuinely better done by mature, audited C library"; mbedTLS joins that set. Business logic, frameworks, surface APIs stay `.dr`.
- **CVE tracking on mbedTLS's calendar.** Every mbedTLS advisory is a Dragon release. Mitigated: small compiled surface, low historical CVE rate, LTS backports - lighter than OpenSSL, lighter than owning all of a self-rolled stack's vulnerabilities.
- **FFI/shim surface** to maintain (`runtime_tls.cpp` + BIO callbacks + error mapping).
- **No FIPS 140-3** - mbedTLS not FIPS-validated. Out of scope.

### Neutral
- Dragon joins **CPython / Node / Ruby / PHP** cohort wrapping a C TLS library, not Go/Java "TLS in language" cohort. Pure-Dragon TLS remains *possible* long-term replacement (Open Questions), not a commitment.

## Open Questions

1. **`hashlib`/`hmac` engine - DECIDED (§5, Phase 7):** retire hand-rolled engine, back with mbedTLS; `secrets`/`urandom` stay pure-Dragon on `getrandom`.
2. **RNG wiring - DECIDED:** mbedTLS `mbedtls_ctr_drbg` seeded from `mbedtls_entropy_func` (OS CSPRNG). Chosen over custom f_rng feeding `dragon_urandom`: CTR_DRBG generates in userspace (no `getrandom` syscall per call), audited standard path, "wrap mbedTLS" not hand-wiring. Not a TLS hot path.
3. **Vendoring mechanism** - git submodule vs pinned copied subtree. Lean: copied subtree, like other vendored libs.
4. **mbedTLS API generation** - PSA Crypto vs legacy `mbedtls_*` API (3.6 supports both). Pick one for shim.
5. **macOS/Windows system trust store** - Mozilla bundle for v1; Keychain / CertStore bridge follow-up.
6. **Async / non-blocking TLS - Phase 8, DONE.** See Phase 8 row.
7. **Optional system-`libssl` fast path on Linux servers** - deferred until measured need.
8. **Dogfooding-policy carve-out** - should `zen.md` explicitly name TLS/crypto as documented exception, cross-referencing this ADR? (Recommended, to prevent a later refactor from "fixing" the C dependancy.)
