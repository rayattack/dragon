# Cryptography and Hashing

Dragon's cryptography surface comes in two layers. The first is **Python-parity**: `hashlib`, `hmac`, and `secrets` mirror the CPython modules of the same names, signature for signature, so code you already know transfers directly. The second is the **superset**: `crypto` exposes primitives Python's standard library never shipped (authenticated encryption, RSA/ECDSA/Ed25519 signatures), and `argon2id` and `totp` are modern, batteries-included modules with no CPython stdlib peer at all.

Everything here is backed by the same mbedTLS engine Dragon already links for TLS, with two exceptions noted in place: `argon2id`'s memory-hard core and `hashlib.scrypt` are native Dragon implementations (mbedTLS ships neither), and `totp` is pure Dragon over `hmac`. Each primitive is verified against its published test vectors (RFC 4226/6238/7914/8018/9106).

One convention runs through all of these modules: **binary in, binary out**. Hash and MAC inputs are `bytes`, raw digests are `bytes`, and you call `.hex()` (on a `bytes` value) or `.hexdigest()` (on a hash object) when you want a hex `str`. This matches CPython, where crypto inputs are binary rather than text.

## hashlib - cryptographic hash functions

`hashlib` provides SHA-256, SHA-224, SHA-512, SHA-384, SHA-1, and MD5 through a Python-compatible incremental-hashing API. Each factory function - `sha256`, `sha224`, `sha512`, `sha384`, `sha1`, `md5`, or the generic `new(algorithm, data=b"")` - returns a `Hash` object:

```dragon
import hashlib

h: hashlib.Hash = hashlib.sha256(b"hello world")
print(h.hexdigest())   # b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9
print(h.digest_size())  # 32
print(h.name())         # sha256
```

The `Hash` object is incremental: feed it data with `update(more: bytes)` and read the digest when you are done. `digest()` returns raw `bytes`; `hexdigest()` returns the hex `str`; `copy()` returns an independent snapshot.

```dragon
import hashlib

h: hashlib.Hash = hashlib.new("sha256")
h.update(b"hello ")
h.update(b"world")
print(h.hexdigest())   # b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9
```

For a one-shot digest without an intermediate object, `hexdigest(algorithm, data)` is a convenience that hashes in a single call:

```dragon
import hashlib

print(hashlib.hexdigest("md5", b"abc"))   # 900150983cd24fb0d6963f7d28e17f72
print(hashlib.sha512(b"abc").hexdigest()[0:16])   # ddaf35a193617aba
```

`algorithms_available()` returns the supported names as a list: `['md5', 'sha1', 'sha224', 'sha256', 'sha384', 'sha512']`.

> **Differs from Python.** The factories are limited to the six algorithms above - there is no BLAKE2, SHA-3, or SHAKE in the `hashlib` factory set (BLAKE2b exists internally as the engine inside `argon2id`, but is not exposed here). `update()` buffers data and computes the digest lazily on the first `digest()`/`hexdigest()` call, which is observable only if you mix updates and digests; the result is identical to CPython.

### pbkdf2_hmac - password-based key derivation

`pbkdf2_hmac(hash_name, password, salt, iterations, dklen=0)` implements PBKDF2 (RFC 8018 §5.2) over HMAC. It is the key-stretching primitive behind SCRAM-SHA-256 (Postgres/MySQL authentication). A `dklen` of `0` (or less) means "the hash's natural digest size", matching CPython's `dklen=None`.

```dragon
import hashlib

dk: bytes = hashlib.pbkdf2_hmac("sha256", b"password", b"salt", 1)
print(dk.hex())   # 120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b

# Stretch a password into a 16-byte key with a realistic round count.
key: bytes = hashlib.pbkdf2_hmac("sha256", b"correct horse", b"battery staple", 100000, 16)
print(len(key))   # 16
```

### scrypt - memory-hard key derivation

`scrypt(password, salt, n, r, p, maxmem=0, dklen=64)` implements scrypt (RFC 7914), a memory-hard KDF that resists GPU/ASIC attacks. `n` is the CPU/memory cost (a power of two greater than 1), `r` the block size, `p` the parallelization. `maxmem` of `0` applies CPython's default 32 MiB cap on the working set; the function raises `ValueError` if the parameters would exceed it.

```dragon
import hashlib

# RFC 7914 §12 vector: password "password", salt "NaCl", N=1024, r=8, p=16.
dk: bytes = hashlib.scrypt(b"password", b"NaCl", 1024, 8, 16, 0, 64)
print(dk.hex()[0:16])   # fdbabe1c9d347200
```

> **Differs from Python.** `scrypt` is a native Dragon implementation rather than a wrapper over a C library, because mbedTLS does not ship scrypt. The API and results match `hashlib.scrypt` exactly; in CPython, `scrypt` additionally requires OpenSSL to be built with scrypt support, whereas in Dragon it is always available.

## hmac - keyed-hash message authentication

`hmac` implements HMAC (RFC 2104) backed by the mbedTLS keyed-hash engine. Construct an incremental MAC with `new(key, msg=b"", digestmod="sha256")`, append data with `update`, and read it with `digest()` (raw `bytes`) or `hexdigest()` (`str`):

```dragon
import hmac

h: hmac.HMAC = hmac.new(b"key", b"The quick brown fox jumps over the lazy dog", "sha256")
print(h.hexdigest())
# f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8
```

For a one-shot MAC, `digest(key, msg, digestmod)` returns the raw bytes directly:

```dragon
import hmac

sig: bytes = hmac.digest(b"key", b"message", "sha256")
print(sig.hex())
# 6e9ef29b75fffc5b7abae527d58fdadb2fe42e7219011976917343065f58ed4a
```

When you compare a MAC you computed against one a client supplied, use `compare_digest(a, b)`, **not** `==`. It walks every byte without short-circuiting, so the time it takes does not reveal how many leading bytes matched - closing a timing side channel that a naive comparison would leak:

```dragon
import hmac

expected: bytes = hmac.digest(b"key", b"message", "sha256")
supplied: bytes = hmac.digest(b"key", b"message", "sha256")
print(hmac.compare_digest(expected, supplied))   # True
```

> **Differs from Python.** `digestmod` is a string name (`"sha256"`, `"sha1"`, ...), not a module reference or callable - Dragon has no `hashlib.sha256` callable to pass. The same six algorithms `hashlib` supports are valid here.

## secrets - cryptographically strong randomness

`secrets` draws from the OS CSPRNG (`getrandom` on Linux, `getentropy` on BSD/macOS, `BCryptGenRandom` on Windows). Reach for it whenever you need a value an attacker must not be able to predict: session IDs, API tokens, password-reset codes.

- `token_bytes(nbytes=32)` - raw random `bytes`.
- `token_hex(nbytes=32)` - `nbytes` of randomness as a hex `str` (so `2 * nbytes` characters).
- `token_urlsafe(nbytes=32)` - URL-safe base64 with `=` padding stripped.
- `randbelow(n)` - a uniform random `int` in `[0, n)`, rejection-sampled so the distribution does not skew.
- `randbits(k)` - a random `int` with `k` random bits.
- `choice(seq: list[int])` - a uniformly random element of a non-empty list.

```dragon
import secrets

print(len(secrets.token_hex(16)))     # 32
print(len(secrets.token_bytes(16)))   # 16

t: str = secrets.token_urlsafe(16)
print("=" in t)                       # False (padding stripped)

n: int = secrets.randbelow(100)
print(n >= 0 and n < 100)             # True
```

`secrets` also re-exports `compare_digest` (the same constant-time comparator as `hmac`) so a verifier needs only one import for both the random token and the check.

> **Differs from Python.** `choice` is typed `list[int]` rather than accepting any sequence, because Dragon's containers are monomorphic. Use `seq[secrets.randbelow(len(seq))]` directly for a list of another element type.

## crypto - digests, signatures, and authenticated encryption

`crypto` is Dragon's home for the cryptographic *superset* - primitives Python's standard library never exposed. It also offers a string-oriented digest shortcut for the cases where you have text rather than bytes in hand.

### Hex digests of strings

`sha256`, `sha224`, `sha512`, `sha384`, `sha1`, and `md5` each take a `str` and return its hex digest as a `str`. This is the str-in/hex-out companion to `hashlib`'s bytes-in/bytes-out API - reach for `hashlib` when you need incremental hashing, raw bytes, HMAC, or PBKDF2.

```dragon
import crypto

print(crypto.sha256("hello world"))
# b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9
print(crypto.md5("abc"))
# 900150983cd24fb0d6963f7d28e17f72
```

### Authenticated encryption (AEAD)

Two AEAD ciphers are available: AES-GCM and ChaCha20-Poly1305 (RFC 8439). Both follow the same shape. `encrypt(key, nonce, plaintext, aad=b"")` returns the ciphertext with a 16-byte authentication tag appended; pass that whole buffer back to `decrypt(key, nonce, ciphertext, aad=b"")`. Decryption **raises `ValueError`** on a tag or associated-data mismatch - it never returns unverified plaintext. A `(key, nonce)` pair must **never** be reused.

For AES-GCM the key is 16/24/32 bytes (AES-128/192/256) and the nonce is typically 12 bytes:

```dragon
import crypto

key: bytes = bytes([0] * 32)     # 32-byte AES-256 key
nonce: bytes = bytes([0] * 12)   # 12-byte nonce
ct: bytes = crypto.aes_gcm_encrypt(key, nonce, b"secret message")
pt: bytes = crypto.aes_gcm_decrypt(key, nonce, ct)
print(pt.decode("utf-8"))   # secret message
print(len(ct))              # 30  (14 plaintext + 16 tag)
```

ChaCha20-Poly1305 requires a 32-byte key and a 12-byte nonce, and demonstrates associated data (`aad`) - extra bytes that are authenticated but not encrypted, and must match on decrypt:

```dragon
import crypto

key: bytes = bytes([1] * 32)
nonce: bytes = bytes([2] * 12)
ct: bytes = crypto.chacha20poly1305_encrypt(key, nonce, b"top secret", b"header")
pt: bytes = crypto.chacha20poly1305_decrypt(key, nonce, ct, b"header")
print(pt.decode("utf-8"))   # top secret
```

### Ed25519 signatures

Ed25519 (EdDSA) gives you fast, deterministic elliptic-curve signatures with small keys. The private key is a 32-byte seed, the public key is 32 bytes, and signatures are 64-byte detached values. Generate a keypair with `ed25519_keypair() -> tuple[bytes, bytes]` (private, public), then sign and verify:

```dragon
import crypto

kp: tuple[bytes, bytes] = crypto.ed25519_keypair()
priv: bytes = kp[0]
pub: bytes = kp[1]

sig: bytes = crypto.ed25519_sign(priv, b"authenticate me")
print(len(sig))                                       # 64
print(crypto.ed25519_verify(pub, b"authenticate me", sig))   # True
print(crypto.ed25519_verify(pub, b"tampered", sig))          # False
```

`ed25519_public_key(private_key)` re-derives the public key from a seed if you stored only the seed.

### RSA and ECDSA signatures

`sign(private_key_pem, message, hash_name="sha256")` and `verify(public_key_pem, message, signature, hash_name="sha256")` work with PEM-encoded keys. The same pair serves both RSA (PKCS#1 v1.5) and EC (ECDSA): mbedTLS dispatches on the parsed key type. `sign` raises `ValueError` if the hash is unsupported or the key cannot be parsed; `verify` returns `false` for any mismatch - it never raises on a bad signature.

The example below uses an EC P-256 key supplied as an inline PEM literal (the newlines are significant - PEM is line-oriented):

```dragon
import crypto

priv: bytes = b"-----BEGIN EC PRIVATE KEY-----\nMHcCAQEEINM4kHHN/Wkn/KNXLg0FVr1g97kVmT12nh2A+KM3vX4HoAoGCCqGSM49\nAwEHoUQDQgAERiqYwJJ7TpuLExOVCMMdXa6bBv+XT/Yf7rz+vvFy0VsXSVYbjfA0\nASfaThgQYtG+yXGsWD+I+prCdROsI3zBXg==\n-----END EC PRIVATE KEY-----\n"
pub: bytes = b"-----BEGIN PUBLIC KEY-----\nMFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAERiqYwJJ7TpuLExOVCMMdXa6bBv+X\nT/Yf7rz+vvFy0VsXSVYbjfA0ASfaThgQYtG+yXGsWD+I+prCdROsI3zBXg==\n-----END PUBLIC KEY-----\n"

msg: bytes = b"transfer 100 to alice"
sig: bytes = crypto.sign(priv, msg, "sha256")
print(crypto.verify(pub, msg, sig))         # True
print(crypto.verify(pub, b"tampered", sig)) # False
```

An RSA key works identically - pass its PEM in place of the EC key. The keys above are throwaway demo keys; generate your own with `openssl genpkey` / `openssl ecparam` and keep the private half secret.

> **Differs from Python.** All of `crypto` beyond the digest helpers is a Dragon superset - CPython has no stdlib equivalent (Python developers reach for the third-party `cryptography` package). The keys are PEM `bytes`; read them from disk with `open(path).bytes()`, fetch them over the network, or inline them as a literal.

## argon2id - modern password hashing

`argon2id` implements Argon2id (RFC 9106), the OWASP-recommended password hash: memory-hard against GPU/ASIC cracking, and side-channel resistant. The memory-hard core (including a from-scratch BLAKE2b) lives in the runtime; this module is a thin, argon2-cffi-flavoured API over it. **This module has no Python stdlib peer** - it parallels the third-party `argon2-cffi` package.

The high-level API stores everything needed to verify in a single self-describing PHC string. `hash(...)` returns `$argon2id$v=19$m=<KiB>,t=<passes>,p=<lanes>$<salt>$<tag>`, and `verify(encoded, password)` re-derives and constant-time compares - it returns `false` (never raises) on a malformed string.

```dragon
import argon2id

# Small cost params for a quick demo; the defaults (t=3, m=64 MiB, p=1) are
# the production recommendation. memory_cost is in KiB.
salt: bytes = bytes([0] * 16)
encoded: str = argon2id.hash(b"hunter2", salt, 2, 1024, 1)
print(encoded[0:9])                          # $argon2id
print(argon2id.verify(encoded, b"hunter2"))  # True
print(argon2id.verify(encoded, b"wrong"))    # False
```

The full signature is `hash(password, salt, time_cost=3, memory_cost=65536, parallelism=1, hash_len=32, secret=b"", ad=b"")`. `secret` is an optional server-side pepper and `ad` optional associated data; neither is stored in the encoded string, so both must be supplied again to `verify`.

A raw `bytes`-in/`bytes`-out API is also available when you manage your own storage: `hash_raw(...)` returns the tag, and `verify_raw(expected, password, salt, ...)` constant-time compares:

```dragon
import argon2id

salt: bytes = bytes([7] * 16)
tag: bytes = argon2id.hash_raw(b"pw", salt, 2, 1024, 1, 32)
print(len(tag))                                            # 32
print(argon2id.verify_raw(tag, b"pw", salt, 2, 1024, 1, 32))   # True
```

> **Beyond Python.** Argon2id is not in CPython's standard library at all. Dragon ships it because password storage is a first-class concern, not an add-on.

## totp - one-time passwords

`totp` implements TOTP (RFC 6238) and HOTP (RFC 4226), the algorithms behind authenticator-app two-factor codes. It is pure Dragon over `hmac` (the only crypto primitive a one-time password needs). **This module has no Python stdlib peer** - it parallels the third-party `pyotp` package.

`secret` is the raw shared-secret `bytes`. (Callers holding a base32 secret decode it first; `totp` pulls in no base32 dependency.)

- `hotp(secret, counter, digits=6, algorithm="sha1") -> str` - HMAC-based code for an explicit counter.
- `totp(secret, time_step=30, digits=6, algorithm="sha1") -> str` - derives the counter from the wall clock.
- `totp_at(secret, for_time, time_step=30, digits=6, algorithm="sha1") -> str` - the same for an explicit Unix timestamp (useful for reproducing the RFC vectors).
- `verify(code, secret, time_step=30, digits=6, skew=1, algorithm="sha1") -> bool` - accepts a code matching any window in `[T - skew, T + skew]`, compared in constant time without short-circuiting.

The HOTP path matches the RFC 4226 Appendix D vectors exactly (secret `"12345678901234567890"`, counters 0 and 1):

```dragon
import totp

secret: bytes = b"12345678901234567890"
print(totp.hotp(secret, 0))   # 755224
print(totp.hotp(secret, 1))   # 287082
```

A live generate-then-verify round trip uses the current clock:

```dragon
import totp

secret: bytes = b"12345678901234567890"
code: str = totp.totp(secret)          # current 6-digit code
print(len(code))                       # 6
print(totp.verify(code, secret))       # True (same window)
```

And `totp_at` reproduces the RFC 6238 Appendix B vector deterministically (`T=59`, `time_step=30`, 8 digits):

```dragon
import totp

secret: bytes = b"12345678901234567890"
print(totp.totp_at(secret, 59, 30, 8))   # 94287082
```

> **Beyond Python.** TOTP/HOTP are not in CPython's standard library. Note the default `algorithm` is `"sha1"`, matching the RFCs and authenticator-app convention - not a weakness, since HOTP's security does not depend on SHA-1's collision resistance.

## At a glance

| Module | Layer | Key entry points | Notes |
|---|---|---|---|
| `hashlib` | Python parity | `sha256`/`sha512`/`new`, `Hash.update`/`hexdigest`, `pbkdf2_hmac`, `scrypt` | bytes in, bytes/hex out; 6 algorithms; `scrypt` is native Dragon |
| `hmac` | Python parity | `new`, `digest`, `compare_digest` | `digestmod` is a string name; use `compare_digest` to check MACs |
| `secrets` | Python parity | `token_hex`/`token_bytes`/`token_urlsafe`, `randbelow`, `randbits` | OS CSPRNG; `choice` is `list[int]`-typed |
| `crypto` | Superset | `aes_gcm_*`, `chacha20poly1305_*`, `ed25519_*`, `sign`/`verify`, `sha256(str)` | AEAD + RSA/ECDSA/Ed25519; no CPython peer |
| `argon2id` | Beyond Python | `hash`/`verify` (PHC string), `hash_raw`/`verify_raw` | OWASP-recommended password hash; memory-hard |
| `totp` | Beyond Python | `hotp`, `totp`, `totp_at`, `verify` | RFC 4226/6238; pure Dragon over `hmac` |

With message integrity and confidentiality covered, the next chapter turns to getting those bytes across the wire: [Networking](/docs/1408-stdlib-networking).
