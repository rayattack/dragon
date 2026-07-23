# Vendored third-party libraries

We vendor these as in-tree source copies (no submodules, no build-time
fetch) so builds are reproducible and offline. This is deliberate. The tradeoff
is that an upstream security advisory is invisible to this repo unless someone (you and I)
watches for it. Record the pinned version here and check the advisory feed
before and when bumping.

| Library  | Pinned version | Upstream                                   | Why it matters |
|----------|----------------|--------------------------------------------|----------------|
| mbedTLS  | 3.6.6          | https://github.com/Mbed-TLS/mbedtls        | TLS + crypto. Highest priority. Subscribe to Mbed-TLS security advisories. |
| SQLite   | 3.53.1         | https://sqlite.org/                         | Storage engine. Watch https://sqlite.org/cves.html |
| PCRE2    | 10.44          | https://github.com/PCRE2Project/pcre2       | Regex engine (ReDoS / CVE surface). |
| llhttp   | 9.3.1          | https://github.com/nodejs/llhttp            | HTTP parser (request-smuggling surface). |
| ed25519  | SUPERCOP ref10 | (confirm exact source drop in lib/ed25519)  | Signing for the package transparency log. Pin the exact upstream commit. |
| Minicoro | (confirm commit in lib/Minicoro) | https://github.com/edubart/minicoro | Coroutines (green threads). Single-file; pin by commit. |

## Updating a vendored library

1. Read the upstream changelog and security advisories since the pinned version.
2. Replace the in-tree copy. Preserve any Dragon-local patches - grep the tree
   for `DRAGON` markers and diff against a clean upstream drop before committing.
3. Update the version in the table above.
4. Rebuild and run the full suite under the memory caps in CONTRIBUTING.md
   (`cmake --build . -j4`, `ctest -j2`, every compiled program under
   `( ulimit -v 4000000; timeout 90 ... )`).
