# Bundled CA trust store

`cacert.pem` - Mozilla's CA root bundle (the certifi / curl.se PEM). Per the design spec.

- **Source:** https://curl.se/ca/cacert.pem
- **Mozilla data as of:** 2026-05-14
- **Fetched:** 2026-05-21
- **SHA-256:** `86a1f3366afac7c6f8ae9f3c779ac221129328c43f0ab2b8817eb2f362a5025c`
- **Certificates:** 121

`ssl.create_default_context()` / `SSLContext.load_default_certs()` use this as the
fallback trust store. Resolution precedence (`dragon_default_ca_file`):
`SSL_CERT_FILE` env → known system bundle paths (Debian/RHEL/Alpine/BSD) → this bundle.

**Refresh on every Dragon release:** re-fetch from `curl.se/ca/cacert.pem` and update
the date + SHA-256 above.
