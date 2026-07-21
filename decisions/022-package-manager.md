# Decision 022: Package Manager (`grab` / `drop` / `sync`)

> **Status:** Proposed. CLI uses flat four-char verbs (`grab`/`drop`/`sync`/…); no `dragon egg <sub>` prefix; no manifest `scripts` runner.

We need a package manager in the `dragon` CLI. Cargo/npm/pip shaped. Manifest is `dragon.drs` (not TOML), hashes inline (no lockfile), content-addressed tarballs, capability permissions so deps can't silently call FFI or open sockets. Phased rollout: git deps first, registry later. You'll recieve pushback on "no lockfile" until you've merged a few hash conflicts in `dragon.drs` at 1am.

Key choices I'm committing to:

1. Manifest uses Dragon Script (`.drs`), not TOML/JSON/YAML.
2. **No separate lockfile.** Package hashes live in `dragon.drs` next to each dependency - you see what you trust where you declare it.
3. Packages are **content-addressed**. SHA-256 of the tarball *is* the identity.
4. **Capability permissions** - packages can't use FFI, file I/O, or network without explicit opt-in from the consuming project.

Dragon's module resolver (`ModuleResolver`) already supports multi-path import
resolution: source directory, `DRAGON_STDLIB_DIR`, and a `site-packages` fallback.
However, there is no mechanism for:

1. **Declaring dependencies** - projects have no manifest; imports either resolve
 from stdlib or fail.
2. **Fetching third-party code** - users must manually clone repos and adjust paths.
3. **Version management** - no way to pin, constrain, or update dependency versions.
4. **Publishing libraries** - no standard way to share Dragon packages.
5. **Reproducible builds** - without pinned hashes, builds depend on whatever happens
 to be on disk.
6. **Supply chain security** - no verification that fetched code matches what the
 author published.

As Dragon matures beyond the stdlib toward real-world adoption, a package
manager becomes essential infrastructure. Without it, every project beyond a single
file requires manual dependency wiring - and without a security model, every dependency
is a trust-everything gamble.

### Design Principles

- **Integrated, not bolted on** - part of the `dragon` CLI, not a separate tool.
- **Convention over configuration** - sensible defaults, minimal required manifest fields.
- **Offline-first** - fetched packages are cached locally; builds don't require network.
- **Single-module compilation compatible** - Dragon compiles all sources into one LLVM
 module, so the package manager provides source trees, not precompiled artifacts.
- **Dogfood `.drs`** - the manifest format is Dragon Script, not an external
 format. This makes Dragon the first language whose package manifest is programmable.
- **Security by default** - hashes pinned in manifest, capabilities declared explicitly,
 packages are immutable once published.

---

## Alternatives Considered

| Approach | Pros | Cons |
|----------|------|------|
| **Adopt pip/PyPI directly** | Huge ecosystem, familiar | Python packages aren't Dragon packages; `.dr` files need their own resolution; pip's global-install model causes conflicts |
| **Separate tool (`drpkg`)** | Decoupled development | Extra install step, version skew with compiler, fragmented UX |
| **Go-style (no manifest, import URLs)** | Zero config | No version pinning, no offline builds, URL imports are fragile |
| **TOML manifest** | Familiar (Cargo, pyproject.toml) | External dependency (TOML parser in C++); doesn't dogfood Dragon; passive data only |
| **JSON manifest** | Universal, no parser needed | No comments, verbose, not human-friendly for config |
| **`.drs` manifest** | Dogfoods Dragon, programmable, comments, variables, conditionals | Requires stdlib to be implemented first |
| **Separate lockfile** | Standard approach (Cargo, npm) | Redundant file; hash lives far from the declaration; easy to forget to commit |
| **Hashes in manifest** | Single source of truth; security is visible at declaration site | Manifest gets longer; `dragon grab` must fetch before writing |

### Decision

**Flat, top-level CLI verbs** (`grab`, `drop`, `sync`, `push`, …) - not a nested
`dragon egg <sub>` namespace - with a `.drs` manifest (`dragon.drs`),
inline integrity hashes, semver version constraints, a capability permission system,
and a per-project `.drx/` directory.

### Why `.drs` Over TOML

1. **Zero external dependencies** - `.drs` is parsed by `stdlib/drs.dr`, which is
 pure Dragon. No C++ TOML library to bundle, no version to track.
2. **Programmable manifests** - `.drs` supports variables, conditionals, and expressions.
 A manifest can adapt to the environment:
 ```drs
 env = 'production'
 {
 name = 'myapp'
 eggs = {
 http = { version = '^1.0', hash = 'sha256:9f86d0...' }
 if env == 'production' {
 monitoring = { version = '^2.0', hash = 'sha256:a3c1e2...' }
 }
 }
 }
 ```3. **Comments** - `#` line comments and `/* */` block comments, unlike JSON. 4. **Consistency** - Dragon developers learn one format for configs, manifests, and data interchange. Not three (TOML for manifest, JSON for lockfile, YAML for CI).
5. **HTML-safe** - `.drs` uses single quotes only, so manifests can be embedded in web tooling without escaping (relevant for registry frontends). 6. **Dogfooding pressure** - making the package manager depend on `.drs` ensures the format stays robust and well-tested.

### Why Hashes in Manifest, Not a Lockfile

Lockfiles (Cargo.lock, package-lock.json) exist to pin exact versions and checksums. But they're a *second file* that must stay in sync with the manifest. In practice:

- Developers forget to commit the lockfile.
- Merge conflicts in lockfiles are notoriously painful.
- The hash - the thing you actually care about for security - is buried in a generated
 file most people never read.

Dragon's approach: **the hash is part of the dependency declaration.**

```drs
{
 eggs = {
 http = { version = '^1.0', hash = 'sha256:9f86d081884c...' }
 }
}
```

When you review a PR that adds a dependency, the hash is right there in the diff. When you audit your dependencies, you read one file. The manifest *is* the lockfile.

**How it works:**

1. `dragon grab http` - fetches the latest compatible version, computes SHA-256 of the package tarball, writes both version and hash into `dragon.drs`. 2. `dragon sync` (no args) - fetches each dependency, verifies the tarball hash matches what's in `dragon.drs`. **Mismatch = hard error, fetch aborts.** 3. `dragon bump http` - fetches the latest compatible version, updates both the version string and hash in `dragon.drs`. 4. Git deps use commit hash: `hash = 'git:a1b2c3d4e5f6...'` 5. Path deps (local) have no hash - they're your own code.

---

## Design

### 1. Manifest Format - `dragon.drs`

```drs
# Dragon package manifest
{
 name = 'myapp'
 version = '0.1.0'
 description = 'A web service in Dragon'
 authors = ['Tersoo <tersoo@example.com>']
 license = 'MIT'
 entry = 'main.dr' # default entry point for `dragon run`
 dragon = '>=0.2.0' # minimum compiler version

 eggs = {
 # Registry packages - version + hash
 http = { version = '^1.0', hash = 'sha256:9f86d081884c7d65...' }
 csv-utils = { version = '~0.3.2', hash = 'sha256:2cf24dba5fb0a30e...' }

 # Git dependency - pinned to commit
 logging = {
 version = '^0.1'
 git = 'https://github.com/user/dragon-logging.git'
 hash = 'git:a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2'
 }

 # Local path dependency - no hash (your own code)
 local-lib = { path = '../my-lib' }

 # Dev-only eggs - use .drs conditionals
 if env == 'dev' {
 bench = { version = '^0.1', hash = 'sha256:e3b0c44298fc1c14...' }
 }
 }

 # Capability grants - see Security section
 permissions = {
 http = ['net'] # http package may use network
 logging = ['fs:write'] # logging may write files
 }
}
```

**Field semantics:**

- `name` - package name, lowercase alphanumeric + hyphens, maps to import name (hyphens become underscores: `csv-utils` imports as `csv_utils`). - `version` - semver (MAJOR.MINOR.PATCH). - `entry` - default file for `dragon run` when no file argument is given. - `dragon` - compiler version constraint (advisory in early versions, enforced later). - `hash` - SHA-256 of the package tarball (registry) or git commit hash (git deps). Verified on every install. **No hash = install refuses** (except path deps). - `permissions` - explicit capability grants for dependencies (see Section 8).

#### Shorthand Syntax

For simple registry deps where you don't need extra fields, a short form is available. The hash is still required - `dragon grab` writes the full form automatically:

```drs
{
 eggs = {
 # These are equivalent:
 http = { version = '^1.0', hash = 'sha256:9f86d0...' }
 http = '^1.0 sha256:9f86d0...' # shorthand: 'version hash'
 }
}
```

#### Programmable Manifests

Because `.drs` supports header variables and conditionals, manifests can be dynamic:

```drs
# Header zone - computed before the payload
platform = 'linux'

{
 name = 'myapp'
 version = '0.1.0'

 eggs = {
 http = { version = '^1.0', hash = 'sha256:9f86d0...' }
 if platform == 'linux' {
 inotify = { version = '^0.2', hash = 'sha256:a3c1e2...' }
 } else {
 fsevents = { version = '^0.3', hash = 'sha256:b4d2f3...' }
 }
 }
}
```

The package manager parses the `.drs` with `drs.parse(Manifest, content, envs=drs.envs(platform='linux', arch='x86_64'))`, where `Manifest` is a typed class declared in the package-manager source. Context variables are injected via `drs.envs(...)` so the manifest's `if/else`/`${}` resolves against the build's environment before binding to typed fields. This is strictly more powerful than TOML's static tables or Cargo's `[target.'cfg(...)'.dependencies]` - and it uses the same syntax Dragon developers already know. (See for the full API.)

### 2. Package Directory - `.drx/`

Dependencies are fetched into a project-local `.drx/` directory:

```
myapp/
 dragon.drs
 .drx/
 http/
 dragon.drs
 src/
 http.dr
 request.dr
 response.dr
 csv_utils/
 dragon.drs
 csv_utils.dr
 src/
 main.dr
```

- **Per-project, not global** - avoids pip's infamous environment conflicts. - **Added to `.gitignore`** - not committed; reproducible via hashes in `dragon.drs`. - **Flat layout** - all transitive deps at top level (like npm's deduped layout). Version conflicts are an error (no duplicate packages with different versions in MVP).

### 3. Global Cache - `~/.drx/eggs/`

Fetched packages are cached globally by hash:

```
~/.drx/eggs/
 sha256/
 9f86d081884c7d65.tar.dr # http@1.0.3
 2cf24dba5fb0a30e.tar.dr # csv-utils@0.3.2
 e3b0c44298fc1c14.tar.dr # bench@0.1.0
```

- **Content-addressed** - the filename is the hash. Two projects using the same version of `http` share one cached copy. - `dragon grab` / `dragon sync` check the cache first, only fetching on miss. - `dragon wipe` clears it. - Cache is a performance optimization only - correctness comes from the hash in `dragon.drs`, not from cache contents.

### 4. Module Resolution Changes

`ModuleResolver` search path becomes:

```
1. Source directory (relative imports)
2. .drx/ <-- NEW
3. DRAGON_STDLIB_DIR (stdlib)
4. site-packages (fallback)
```

Package names map to directories in `.drx/`. A package's `entry` field (or `<name>/<name>.dr` by default) is the module root.

### 5. CLI Commands

| Command | Description |
|---------|-------------|
| `dragon init` | Create `dragon.drs` in the current directory (interactive prompts) |
| `dragon init --lib` | Create a library package layout (no `entry`, adds `src/`) |
| `dragon grab <name> [...]` | Fetch + add specific eggs to `dragon.drs` with version + hash |
| `dragon grab -r <file>` | Bulk-grab eggs listed in a requirements file |
| `dragon sync` | Fetch every dep in `dragon.drs`, **verify every hash**, populate `.drx/` |
| `dragon drop <name>` | Remove a dependency and its permission grants |
| `dragon bump` | Resolve latest compatible versions, **update hashes** in `dragon.drs` |
| `dragon bump <name>` | Bump a single dependency's version and hash |
| `dragon list` | List installed packages, versions, and hash status |
| `dragon hash` | Re-hash all installed packages, report any mismatches |
| `dragon scan` | Check packages against the vulnerability database + transparency log |
| `dragon find <query>` | Search the registry (Phase 3+) |
| `dragon push` | Publish to the registry (Phase 3+) |
| `dragon info <name>` | Show package metadata, capabilities, and trust status |
| `dragon wipe` | Clear the global package cache |
| `dragon auth` | Log in to the registry (shows status if already logged in) (Phase 3+) |
| `dragon auth stop` | Log out - clear stored credentials (Phase 3+) |
| `dragon auth status` | Show current login / token state (Phase 3+) |
| `dragon auth token create/list/revoke` | Manage scoped publish tokens for CI (Phase 3+) |
| `dragon auth keygen` | Generate an Ed25519 author signing keypair (Phase 3+) |
| `dragon yank` | Retract a published version from resolution (Phase 3+) |

**Every package verb is exactly four characters** (`grab`, `drop`, `sync`, `bump`, `find`, `push`, `hash`, `yank`, `wipe`, `scan`, `init`, `info`, `list`), so the consume side of the package manager is a tight, memorable surface. Account and registry operations live under the **`auth` group** (`auth`, `auth stop`, `auth status`, `auth token …`, `auth keygen`) - `auth` is a four-character verb and a *meaningful* grouping, not the redundant `egg` prefix we dropped. The rule we killed was a dead word wedged onto every command, **not** nesting where nesting actually carries meaning (`auth token create` groups a real sub-operation).

There is deliberately **no script-runner command** (no `npm run` / `cargo`-style task
alias). A project command in Dragon is just a `.dr` file you run with `dragon run` -
see Section 14, "No Script Runner," for the security rationale. This removes a
config-embedded execution surface entirely.

### 6. Package Layout Convention

A published package must contain:

```
<package-name>/
 dragon.drs # required - manifest
 src/ # or top-level .dr files
 <name>.dr # package root module
 ...
 README.md # optional
 LICENSE # recommended
 tests/ # optional
```

### 7. Version Resolution

- **Semver** with `^` (compatible) and `~` (patch-only) operators, plus exact (`=`), range (`>=1.0, <2.0`), and wildcard (`1.*`). - **MVP: flat resolution** - all packages at one version. Conflicts are errors with a clear message showing which packages require conflicting versions. - **Future: SAT-based solver** - like Cargo's resolver, for complex dependency graphs.

---

## Security Model

Supply chain attacks are the defining security problem of modern package management. npm, PyPI, and crates.io have all been hit. Dragon's security model is designed from day one, not bolted on after the first incident.

### Threat Model

| Threat | Example | Mitigation |
|--------|---------|------------|
| **Tampered package** | Registry compromised, serves modified tarball | SHA-256 hash in manifest; mismatch = hard error |
| **Typosquatting** | `htpp` instead of `http` | Name similarity detection on publish; reserved name list |
| **Dependency confusion** | Internal `auth` package shadowed by public `auth` | Scoped packages (`@org/auth`); explicit registry source |
| **Malicious code in legit package** | Maintainer account compromised, pushes backdoor | Capability permissions; author signing; immutable publishes |
| **Transitive supply chain** | Your dep's dep is compromised | Hash verification is transitive; capability permissions propagate |
| **Install-time code execution** | npm `postinstall` scripts run arbitrary code | **No install scripts.** Dragon packages are source code, not executables |
| **Exfiltration via FFI** | Package uses `extern "C"` to call `system` | `ffi` capability required; denied by default |
| **Data exfiltration via network** | Package phones home during compilation | `net` capability required; denied by default |

### 8. Capability Permissions

Dragon packages must declare what system capabilities they need. The consuming
project must explicitly grant those capabilities. **Undeclared capability use =
compile-time error.**

#### How It Works

**Package side** - the package's `dragon.drs` declares what it needs:

```drs
# http package's dragon.drs
{
 name = 'http'
 version = '1.0.3'
 capabilities = ['net', 'ffi'] # "I need network access and FFI"
}
```

**Consumer side** - your project's `dragon.drs` grants (or denies) those capabilities:

```drs
{
 eggs = {
 http = { version = '^1.0', hash = 'sha256:9f86d0...' }
 }

 permissions = {
 http = ['net', 'ffi'] # "I trust http with network and FFI"
 }
}
```

#### Capability Taxonomy

| Capability | What it allows | Risk level |
|-----------|---------------|------------|
| `pure` | Pure computation only (default) | None |
| `fs:read` | Read files from disk | Low |
| `fs:write` | Write/create/delete files | Medium |
| `net` | Open network connections | High |
| `ffi` | Use `extern "C"` / call native code | Critical |
| `env` | Read environment variables | Medium |
| `proc` | Spawn subprocesses | Critical |

#### Enforcement - Three Layers

Capability enforcement happens at three points, each catching different things:

##### Layer 1: Install-Time Static Scan (Advisory)

When `dragon grab` (or `dragon sync`) fetches a package, it performs a fast static scan of the source files before placing them in `.drx/`:

| Pattern detected | Capability required |
|-----------------|-------------------|
| `extern "C"` declaration | `ffi` |
| `import io`, `from io import` | `fs:read` or `fs:write` |
| `import http`, `from http import` | `net` |
| `import os`, `os.system`, `os.popen` | `proc` |
| `os.getenv`, `os.environ` | `env` |

This scan compares detected patterns against the package's declared `capabilities`. Mismatches produce warnings:

```
warn Package 'sketchy-json' declares capabilities: [pure]
 But source contains:
 src/parser.dr:42 extern "C" fn system(cmd: ptr) -> int
 src/parser.dr:87 from io import open
 Run `dragon info sketchy-json --capabilities` for details.
 Install anyway? [y/N]
```

This layer is **advisory, not a hard gate** - it catches honest mistakes and flags obvious red flags, but a determined attacker could obfuscate patterns. That's why Layer 2 exists.

##### Layer 2: Compile-Time Enforcement (Hard Gate)

This is the real security boundary. Dragon compiles all sources into a single LLVM module, and the compiler already knows which source file belongs to which package (via `ModuleResolver`). During compilation, the compiler tracks the "current package context" and enforces capabilities:

| Code construct | Capability checked | Enforcement mechanism |
|---------------|-------------------|----------------------|
| `extern "C" fn ...` or `extern "C" { }` | `ffi` | Parser tags FFI decls with source package; CodeGen checks grant |
| `import io` / `from io import open` | `fs:read` or `fs:write` | Import resolver checks grant before resolving |
| `import http` / `from http import ...` | `net` | Import resolver checks grant before resolving |
| `import os` / `os.system` / `os.popen` | `proc` | Import resolver + Sema checks grant |
| `os.getenv` / `os.environ` | `env` | Sema checks grant on name resolution |

If a capability wasn't granted → **compile error, not a warning:**

```
error[E0901]: capability violation in package 'sketchy-json'
 --> .drx/sketchy_json/src/parser.dr:42:1
 |
42 | extern "C" fn system(cmd: ptr) -> int
 | ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 |
 = package 'sketchy-json' declares capabilities: [pure]
 = but this code requires: [ffi]
 = to allow this, add to your dragon.drs:
 | permissions = {
 | sketchy-json = ['ffi']
 | }
 = note: granting 'ffi' allows unrestricted native code execution
```

The code **never reaches LLVM IR generation**, let alone execution.

**How the compiler tracks package context:**

1. `ModuleResolver` already maps source files to packages (it knows `.drx/http/src/request.dr` belongs to the `http` package). 2. When entering a file from a package, the compiler sets `currentPackageContext` with the package's granted capabilities (read from the consumer's `permissions` block). 3. Each capability-gated construct checks `currentPackageContext` before proceeding. 4. The root project's own code runs with all capabilities (you trust your own code).

##### Layer 3: The `ffi` Escape Hatch (Trust Boundary)

`ffi` is special. A package with `ffi` can call *any* C function - `socket`, `open`, `system`, `dlopen`. **Granting `ffi` is granting full trust.** The other capabilities (`net`, `fs:read`, `proc`) only gate access to Dragon's stdlib wrappers; `ffi` bypasses all of them by going directly to the OS.

This is by design. The capability system isn't a sandbox - it's a **declaration of intent**
that makes trust decisions visible and reviewable. The security guarantee is:

- **Without `ffi`:** a package can only do what Dragon's stdlib allows, and each stdlib
 capability is individually gated. The compiler enforces this at compile time.
- **With `ffi`:** a package can do anything. You are trusting the author.

In practice, most packages don't need `ffi`. Data structures, algorithms, parsers,
formatters, template engines - all pure. When you see `ffi` in a package's capabilities,
it should prompt the question: *why does this package need native code?*

```
# This should make you pause:
permissions = {
 json-formatter = ['ffi'] # why does a JSON formatter need FFI?
}

# This is expected:
permissions = {
 sqlite = ['ffi', 'fs:read', 'fs:write'] # database driver, obviously needs FFI
}
```

##### Transitive Propagation

Capabilities propagate through the dependency chain. If package A depends on package B, and B requires `ffi`:

1. B's `dragon.drs` declares `capabilities = ['ffi']` 2. A's `dragon.drs` must grant it: `permissions = { B = ['ffi'] }` 3. A's own `capabilities` must include `ffi` (since A transitively uses it) 4. The root project must grant `ffi` to A: `permissions = { A = ['ffi'] }`

The full capability chain is visible. You can trace exactly *why* you're granting `ffi`
all the way down to the package that actually uses `extern "C"`.

`dragon list --capabilities` shows the full tree:

```
myapp (root)
├── http ^1.0 [net]
│ └── tls ^0.5 [ffi, net] ← http needs tls, tls needs ffi for OpenSSL
├── json ^2.0 [pure] ← no capabilities needed
└── sqlite ^3.0 [ffi, fs:read, fs:write]
```

#### The "No Permissions" Default

A package with no `capabilities` declaration is **pure** - it can only do computation with the data passed to it. No file access, no network, no FFI, no subprocesses. This is the safe default. Most utility packages (data structures, algorithms, parsers, formatters) should be pure.

If you install a JSON formatting library and it demands `net` + `ffi`, that's a red flag you can see immediately in your `dragon.drs` before you ever run the code.

The install-time scan (Layer 1) catches packages that *forgot* to declare capabilities.
The compile-time check (Layer 2) catches packages that *lied* about not needing them.
And the `ffi` trust boundary (Layer 3) ensures that when a package *does* need full
native access, you make that decision explicitly and visibly.

### 9. Package Signing

Authors sign packages with Ed25519 keys. Verification is optional in early phases,
mandatory for the registry in Phase 3+.

```drs
{
 eggs = {
 http = {
 version = '^1.0'
 hash = 'sha256:9f86d0...'
 signed-by = 'ed25519:B4a3F7c9...' # author's public key
 }
 }
}
```

**Key management:**

- `dragon auth keygen` - generate Ed25519 keypair, store in `~/.dragon/keys/` - `dragon push --sign` - sign the package tarball before upload - Registry stores the signature alongside the tarball - `dragon grab` / `dragon sync` verify the signature if `signed-by` is present in the manifest - **Trust-on-first-use (TOFU):** first install records the author's key; subsequent updates from a different key emit a warning

### 10. Authentication & Account Model

The registry is its own identity provider. **No third-party OAuth** (GitHub, Google, GitLab, Microsoft, Bitbucket, etc.). A Dragon registry that depends on a corporate identity provider is one policy change away from locking out contributors - and binding the language ecosystem to any one platform contradicts the registry's goal of being neutral infrastructure.

#### Account Requirements

To create an account on `eggs.dragonlang.org`:

1. **Username** - unique, lowercase alphanumeric + hyphens, reserved name list applies.
2. **Password** - minimum 12 characters; checked against `haveibeenpwned`'s
 k-anonymity API on signup and reset (known-breached passwords rejected).
3. **Verified email address** - any provider. The registry sends a single-use
 verification token; publish rights activate after click-through.
4. **TOTP 2FA** - mandatory before publish rights activate. Authenticator apps only
 (Aegis, Raivo, 1Password, Bitwarden, etc.). **No SMS** - SIM-swap is too cheap.
 Recovery codes generated at enrollment.

This is **verified email, not KYC.** The registry does not collect government ID,
address, or any identity documents. Pseudonymous publishing is supported and
expected. The email exists solely so account recovery and ownership disputes have
a working contact channel.

#### Password Hashing

**Argon2id** (RFC 9106), with parameters tuned to ~250-500ms per login on the
auth tier:

| Parameter | Minimum | Recommended |
|-----------|---------|-------------|
| Memory (`m`) | 64 MB | 128 MB |
| Iterations (`t`) | 3 | 2 (with higher `m`) |
| Parallelism (`p`) | 4 | 4 |

Not bcrypt. bcrypt's 4 KB working set fits in modern GPU cache (~100k hashes/sec
per card), and its silent 72-byte password truncation is a footgun that bites
passphrase users. Argon2id's memory hardness drops GPU throughput 20-100x and has
no length limit.

#### Authentication for CLI Publishing

`dragon push` uses **scoped publish tokens**, not the user's password:

- `dragon auth` - interactive: prompts for username, password, TOTP code;
 registry returns a long-lived publish token stored in `~/.dragon/credentials`
 (mode 0600).
- `dragon auth token create --scope publish:@myorg/*` - generate a scoped,
 revocable token for CI environments. Tokens can be limited by package scope,
 package name, or expiration date.
- `dragon auth token revoke <token-id>` - invalidate a token immediately.
- Publish tokens never grant account-level operations (password change, email
 change, account deletion) - those require an interactive session with TOTP.

This separation means a leaked CI token can't take over an account, and account
recovery doesn't invalidate ongoing CI pipelines.

#### Rate Limiting & Abuse

- **Login attempts:** per-IP and per-username exponential backoff. Credential
 stuffing is the #1 attack on package registries (PyPI, RubyGems both hit) -
 this is non-negotiable.
- **Account creation:** rate-limited per IP and per email domain.
- **Publish rate:** soft-limited per account to slow automated typosquatting.
- **Token usage:** anomaly detection (sudden geographic shift, unusual package
 scope) triggers a notification email and optional auto-revocation.

#### Account Recovery

| Lost | Recovery |
|------|----------|
| Password (have email + TOTP) | Email-based reset, TOTP still required |
| TOTP device (have email + recovery code) | Recovery codes from enrollment unlock the account |
| TOTP device (no recovery code, have email + password) | 7-day cooldown; email confirmation; TOTP re-enrolled |
| Email (have password + TOTP) | Self-serve email change from account settings |
| Everything | Account is unrecoverable. Document this clearly at signup. |

The "everything lost" case is intentional. Allowing administrative recovery via
out-of-band proof creates a social-engineering attack surface that has burned
every registry that's tried it. Better to be honest: keep your recovery codes,
or lose the account.

#### Future: WebAuthn / Passkeys (Phase 4+)

Once passkey support is broadly available across the Dragon contributor base,
add WebAuthn as an alternative second factor and eventually as a passwordless
login option. Passkeys are phishing-resistant, use no shared secrets, and remain
vendor-neutral (the credential is bound to the user's device, not to a platform
identity provider). Passwords stay supported indefinitely as the bootstrap path.

---

### 11. Registry Architecture

The registry is an **immutable, content-addressed store**. Once a version is published,
it cannot be modified or deleted (except by admin action for security incidents).

#### Hosting

| Layer | Choice |
|-------|--------|
| **Registry / index** | `eggs.dragonlang.org` - self-hosted service (open source) |
| **Tarball storage** | Cloudflare R2 - S3-compatible, zero egress fees, content-addressed |
| **CDN** | Cloudflare in front of R2 (tarballs are immutable, infinite TTL) |
| **Metadata DB** | Postgres - packages, versions, owners, yank state, transparency log |
| **Search** | Postgres full-text initially; revisit at ~10k packages |

The registry frontend at `eggs.dragonlang.org` is open and searchable without an
account. Login is required only to publish, yank, or manage your own eggs.

Storage is decoupled from the trust root: tarballs in R2 are content-addressed
by SHA-256, so a malicious mirror cannot substitute bytes without breaking the
hash check. This means the storage backend is fully untrusted infrastructure
and can be swapped (R2 → S3 → MinIO → IPFS) without changing client behavior.

#### Storage Model

```
# Registry-side (eggs.dragonlang.org Postgres + R2)
registry-db/
 packages # name, owner, capabilities, yank_state per version
 versions # name, version, hash, signed-by, published_at
 transparency_log # append-only, mirrored to public GitHub repo
 accounts # username, email, argon2id hash, totp secret, recovery codes

r2://dragon-eggs/
 sha256/
 9f/9f86d081884c7d65...tar.zst # http@1.0.3 (sharded by first byte)
 2c/2cf24dba5fb0a30e...tar.zst # csv-utils@0.3.2

# Client-side (~/.drx/eggs) - unchanged from Section 3
```

Tarball format: **`.tar.zst`** (Zstandard). Better compression ratio than gzip, faster decompression, modern. The hash is computed over the compressed bytes; clients verify before extraction.

#### Immutability Rules

| Action | Allowed? | Notes |
|--------|----------|-------|
| Publish new version | Yes | Append-only |
| Modify published version | **No** | Hash would change, breaking all consumers |
| Delete/yank a version | Admin only | Security incidents, legal takedowns |
| Transfer ownership | Yes | Requires both parties to sign |
| Re-publish same version | **No** | Even if identical content |

**Why immutable?** If a package can be silently replaced, the hash in your manifest becomes meaningless. Immutability means the hash you pinned on Tuesday is guaranteed to resolve to the same bytes on Friday. This is the foundation of the entire security model.

#### Transparency Log

Every publish event is appended to a public, append-only transparency log (similar
to certificate transparency logs or Go's sumdb):

```
[T12:00:00Z] PUBLISH http@1.0.3 sha256:9f86d0... signed-by:B4a3F7c9...
[T12:05:00Z] PUBLISH csv-utils@0.3.2 sha256:2cf24d... signed-by:C5b4G8d0...
```

- Anyone can audit the log. - `dragon scan` checks your dependencies against the log. - If a package hash appears in the log with a different signature than expected, something is wrong.

### 12. Anti-Typosquatting

On `dragon push`:

1. **Levenshtein distance check** - if the new package name is within edit distance 2 of an existing popular package, the publish is flagged for manual review. 2. **Reserved names** - stdlib module names (`os`, `io`, `math`, `http`, `json`, etc.) are reserved and cannot be published by third parties. 3. **Scoped packages** - organizations can register a scope (`@myorg/`) and publish under it. `@myorg/auth` cannot be confused with `auth`. 4. **Homoglyph detection** - `httρ` (Greek rho) vs. `http` flagged on publish.

### 13. Dependency Confusion Prevention

When resolving imports, the package manager distinguishes between sources:

```drs
{
 eggs = {
 # Explicitly from registry
 http = { version = '^1.0', hash = 'sha256:9f86d0...', source = 'registry' }

 # Explicitly from private registry
 auth = { version = '^2.0', hash = 'sha256:abc123...', source = 'registry:internal.company.com' }

 # Explicitly from git
 utils = { git = 'https://github.com/org/utils.git', hash = 'git:a1b2c3d...' }
 }
}
```

A package from the public registry **cannot** shadow a package from a private registry or a git dependency. Source is explicit.

### 14. No Script Runner (No `postinstall`, No Task Aliases)

Dragon's manifest has **no `scripts`/`tasks` block and no command that runs them.** This is a security decision, not an omission.

The defining supply-chain disaster of package managers is **config-embedded code
execution.** npm's `postinstall`/lifecycle hooks - arbitrary commands declared in
`package.json` that run automatically on `npm install` - are the vector behind the
worst incidents, including the self-replicating *Shai-Hulud* worm (Sept 2025), which
spread by running install-time scripts from compromised dependencies, harvested npm
and cloud tokens, and used the stolen tokens to republish itself into every package
the victim maintained.

Three properties make that vector lethal: **(1)** the code runs **automatically**, with
no user intent; **(2)** it is authored by a **dependency**, not by you; **(3)** it runs
**arbitrary shell**, so it reaches credentials and the network. Dragon removes the
shape entirely:

- **No install-time execution.** `grab` / `sync` / `bump` only *fetch and verify
 source*. Dragon compiles all sources into one LLVM module - a package is source
 code, never an executable, and nothing in a dependency runs on your machine until
 *you* compile and run *your* program. (This is the "Install-time code execution" row
 of the threat model.)
- **No dependency-authored commands.** Even a root-only, explicitly-invoked `scripts`
 table - the comparatively safe `npm run` shape - normalizes "the manifest can run
 commands," which is the slope that ends in "...and so can a dependency's, at build
 time." We refuse to put a foot on it.

**The functionality folds in for free** via Dragon's entry model (no magic `main`; the
*file* is the unit of execution). A project command is just a Dragon program:

```
dragon run tests/run_all.dr # the "test" task
dragon run tools/lint.dr # the "lint" task
```

Typed, compiled, fast - and with **zero config-embedded execution surface**. The `entry` field already covers the default `dragon run` case; anything beyond that is one more `.dr` file. Capabilities still gate what those programs may touch (`net`, `fs`, `ffi`, `proc`), exactly as for any Dragon code.

---

## Phased Implementation

### Phase 0: Manifest + Project Init (Foundation)

- **Depends on:** Phases 0-5 (`.drs` tokenizer + parser + envs + `drs.parse(Schema, ...)` schema-binding) - `dragon init` - generates `dragon.drs` with interactive prompts - `dragon.drs` parsing in `Driver` via `drs.parse(Manifest, content)` - reads package metadata into a typed `Manifest` class, raising `DragonScriptSchemaError` on missing/typo'd required fields - `dragon build` / `dragon run` respect the `entry` field when no file argument is given

**Output:** Dragon projects have a manifest; `dragon run` works without
specifying a file.

### Phase 1: Local + Git Dependencies + Hash Verification

- `dragon grab <name> --git <url>` - clone, compute commit hash, write to `dragon.drs`
- `dragon grab <name> --path <dir>` - symlink/copy local path dependency (no hash)
- `dragon sync` - fetch all deps, **verify every hash**, populate `.drx/`
- Global cache (`~/.drx/eggs/`) - content-addressed by hash
- `ModuleResolver` integration - add `.drx/` to search path
- Transitive dependency resolution (read dep's `dragon.drs`, fetch its deps)
- `dragon drop` - remove from manifest and `.drx/`
- `dragon hash` - re-hash installed packages, report mismatches

**Output:** Multi-package Dragon projects with hash-verified git dependencies.

### Phase 2: Semver + Version Resolution + Capabilities

- Semver parser and constraint evaluator (`^`, `~`, `>=`, `<`, `=`, `*`)
- Git tag-based version discovery (tags like `v1.2.3`)
- Version resolution algorithm (greedy/backtracking for MVP)
- Conflict detection with clear error messages
- `dragon bump` - resolve latest compatible versions, **update hashes**
- `dragon list` - show dependency tree with versions and capability requirements
- **Capability permission system** - declared in package, granted by consumer, enforced
 at compile time
- **Depends on:** Phase 3 (expression evaluator) for programmable manifests

**Output:** Versioned, capability-gated dependencies with inline hash verification.

### Phase 3: Central Registry + Auth + Signing

- **Registry service at `eggs.dragonlang.org`** - Postgres metadata, R2 tarball
 storage, Cloudflare CDN, open-source server code
- **Authentication** - username + password (Argon2id) + verified email +
 mandatory TOTP 2FA for publish rights; no third-party OAuth; breach-list
 rejection on signup/reset; rate limiting and abuse detection
- **Account commands** (`auth` group) - `dragon auth` (login), `dragon auth stop`
 (logout), `dragon auth status`, `dragon auth token create/list/revoke` (scoped
 publish tokens for CI)
- `dragon push --sign` - sign tarball with Ed25519 key, upload to registry
- `dragon auth keygen` - generate author keypair
- `dragon find` - full-text search over registry (Postgres FTS)
- `dragon info` - show metadata, capabilities, trust chain
- `dragon yank` - soft yank (drop from version resolution) and security yank
 (warn loudly on install); never hard-delete - hash always resolves
- Checksum verification on fetch (SHA-256 against registry index)
- Signature verification (`signed-by` field in manifest)
- Transparency log - append-only publish log, mirrored to a public GitHub repo
- Anti-typosquatting (Levenshtein, homoglyph, reserved names)
- Scoped packages (`@org/name`) - unscoped names reserved for blessed packages
- Registry web frontend (package browser, docs, capability badges, trust indicators)

**Output:** Public package ecosystem with auth, signing, transparency, and anti-abuse.

### Phase 4: Polish + Ecosystem

- `dragon scan` - check packages against the vulnerability database + transparency log
- `dragon docs` - generate documentation from docstrings
- **WebAuthn / passkey support** - add as alternative second factor and eventually
 passwordless login; passwords remain supported as bootstrap path
- Workspace support (monorepo with multiple packages sharing dependency declarations)
- Declarative build section (`build` block in `dragon.drs`) for packages needing native
 compilation - pinned, capability-gated toolchain invocations, **not** an arbitrary
 shell-script escape hatch (see Section 14)
- Feature flags / optional dependencies
- Private registries (for organizations, with dependency confusion protection)
- Dependency graph visualization (`dragon list --graph`)

**Output:** Production-grade package management.

---

## Why this matters for the ecosystem

### Positive

- **Unblocks ecosystem growth** - third-party packages become trivially shareable.
- **Reproducible builds** - hashes in manifest ensure identical dependency trees.
- **No lockfile** - one file (`dragon.drs`) is the single source of truth for
 dependencies, versions, hashes, and permissions. Simpler than manifest + lockfile.
- **Zero external dependencies** - manifest parser is `stdlib/drs.dr`, pure Dragon.
- **Programmable manifests** - conditional deps, platform-specific configs, computed
 values - all in familiar Dragon syntax.
- **Security from day one** - hash verification, capability permissions, signing,
 and transparency are built into the design, not retrofitted after an incident.
- **Capability permissions** - the first mainstream package manager where you can see
 at a glance which packages can touch the filesystem, network, or FFI.
- **Dogfooding `.drs`** - the package manager becomes the highest-pressure test of
 the Dragon Script format.
- **Source distribution** - Dragon compiles everything into one LLVM module, so
 distributing source (not precompiled artifacts) is natural and simple.

### Negative

- **Bootstrapping dependency** - the package manager depends on `stdlib/drs.dr`, which means
 must be implemented first. However, is pure stdlib work with no C++/LLVM
 changes, and the package manager needs it regardless of format choice.
- **Unfamiliar to newcomers** - developers from Rust/Python won't immediately recognize
 `dragon.drs`. Mitigated: the syntax is minimal and `dragon init` generates it.
- **Hashes make manifest noisier** - every dependency has a 64-char hash string.
 Mitigated: `dragon grab` writes them automatically; you rarely type them by hand.
- **Capability enforcement complexity** - compile-time capability checking requires
 the compiler to track which package each source file belongs to, and which imports
 map to which capabilities. Non-trivial but tractable.
- **Maintenance burden** - a package manager is a significant ongoing commitment
 (resolver bugs, registry uptime, security response).

### Neutral

- **Registry governance** - who can publish, naming disputes, malicious packages.
 Detailed in Phase 3 but will evolve with community growth.
- **Interaction with Python ecosystem** - Dragon can already FFI into C but can't
 import Python packages. Bridging to PyPI is out of scope for this decision.

---

## Open Questions

1. ~~**Registry hosting** - self-hosted vs. cloud? Domain name for registry?~~
 **Resolved :** `eggs.dragonlang.org` (self-hosted index/auth) +
 Cloudflare R2 (tarball storage) + Cloudflare CDN. Auth is username + password
 (Argon2id) + verified email + mandatory TOTP; no third-party OAuth. See
 Sections 10 and 11.
2. **Binary caching** - worth adding a content-addressed build cache (like sccache)
 to avoid recompiling unchanged deps?
3. **Python interop** - should the package manager be able to fetch from PyPI for packages
 that are pure-Python compatible? Deferred but worth tracking.
4. **Monorepo / workspace** - needed early or can wait for Phase 4?
5. **Capability granularity** - is `fs:read` / `fs:write` enough, or do we need
 path-scoped permissions (`fs:read:/tmp/*`)?
6. **Transitive capability policy** - should a package be able to grant capabilities
 to its own dependencies, or must the root project grant everything explicitly?
7. **Revocation** - how to handle a published package found to be malicious? Yank
 (hide from resolution but don't break existing hashes) vs. hard delete?

---

## Addendum : Storage Trust Model + Ed25519 Signed Checkpoints

Recorded while implementing the security model (Sections 8-11) end to end. The
kills (fail-closed secret, push allowlist, multi-level yank), the keystone
(client-side hash verification), the `.drs` untrusted-manifest hardening, and the
transparency log of Section 11 are now built and verified. This addendum settles
two questions that the working implementation forced into focus.

### A. Storage: own the canonical bytes on R2 (public-read); every source is untrusted-but-verified

Content-addressing (Section 11) plus the append-only signed transparency log makes
the byte store **untrusted infrastructure**. The client computes the SHA-256 of
every fetched artifact and checks it against both the hash pinned in `dragon.drs`
and the log leaf that commits to it; no host can substitute bytes without immediate,
automatic detection. So "who stores the bytes" is purely an availability and cost
question, never a trust question.

1. **Canonical store = Cloudflare R2, content-addressed, immutable, public-read,**
 CDN in front (infinite TTL). Download requires no account; only publish/yank are
 authenticated. Owning this copy is the availability guarantee: a published
 version becomes an object that never disappears - the role Go's module proxy
 plays (an immutable cached copy so a deleted upstream cannot break builds).
2. **GitHub/GitLab are NOT the primary store.** Making a forge the canonical source
 re-imports the "left-pad" failure (a deleted/privatised repo breaks builds), ties
 the ecosystem's uptime and governance to a third party, and removes the publish
 chokepoint where typosquat/capability/signing checks run. Git dependencies remain
 only as the escape hatch (private or pre-publish packages), pinned to a commit SHA.
3. **Mirrors are a verified fallback,** modelled as an ordered source list in the
 client. Because content-addressing makes any source safe, a mirror (a
 GitHub-hosted copy, a university mirror, IPFS) costs nothing in trust: the client
 tries the next source and still verifies by hash + log. The fetch path is an
 ordered list (R2 first); additional mirrors are configuration, populated when
 resilience / censorship-resistance is wanted.

In one line: own R2 + public-read for **availability**, keep the index + auth +
signed log as the neutral **trust root**, and treat every byte source as
untrusted-but-verified. The bytes are owned for availability, not for trust; the
log provides trust.

### B. Ed25519 client-verifiable signed checkpoints

Section 11's transparency log v1 signs each checkpoint with an **HMAC** keyed by the
server secret, which only the registry (and monitors holding the secret) can verify.
This addendum upgrades the checkpoint to an **Ed25519** signature so that *any*
client can verify, with the registry's public key, that a checkpoint - and the log
head it commits to - genuinely originated from the registry. That closes the
first-acquisition trust gap cryptographically: a man-in-the-middle or a rogue mirror
cannot forge the log a client sees. This is the Go sumdb guarantee: **verify the
registry, do not trust it.**

The Ed25519 primitive already exists in Dragon (`lib/ed25519/`,
`lib/Runtime/runtime_ed25519.cpp`, `stdlib/crypto.dr` exposing
`ed25519_keypair`/`ed25519_sign`/`ed25519_verify`, with a KAT at
`test/dr/test_ed25519.dr`), so this is **wiring, not a cryptographic
implementation**.

1. **Log signing key.** On first start the registry generates an Ed25519 log keypair
 (or loads the private seed from `DRAGON_LOG_SIGNING_KEY`, fail-closed like the
 session secret). The checkpoint signs the body `"size\nchain_head"` and returns
 `{size, chain_head, signature, key_id}`, where `key_id` is a short hash of the
 public key (for rotation).
2. **Public-key distribution.** `GET /api/log/pubkey` returns the registry's public
 key + `key_id`; the fingerprint is published out-of-band (docs) so it is pinnable.
3. **Client verification + trust-on-first-use.** The client keeps a trust store
 (`~/.dragon/trusted-keys`, with a baked default for `eggs.dragonlang.org`).
 `grab`/`sync`/`scan` fetch the checkpoint and verify the Ed25519 signature against
 the pinned public key, rejecting on failure. First contact records the key +
 `key_id`; a later key change emits a loud warning (SSH-style TOFU).
4. **(Separable) Author package signing** (Section 9). `dragon auth keygen` generates
 an author keypair; `dragon push --sign` signs the tarball; the manifest carries
 `signed-by = 'ed25519:...'`; the client verifies per-author on install (TOFU).
 This catches a compromised-maintainer republish under a new key, complementing the
 log (which attests *what* was published, not *who* is trusted to publish).

The HMAC checkpoint is retired once Ed25519 lands; one signature format is cleaner.

**Scope boundary - Merkle proofs.** The log is a hash *chain*, so inclusion and
`scan` checks read the whole log (acceptable at current scale). A Merkle *tree*
gives O(log n) inclusion and consistency proofs. Ed25519 makes the checkpoint
*trustworthy*; a Merkle tree makes verification *succinct* - they are orthogonal.
Signing lands now; the Merkle upgrade is deferred until log size makes full-log
download costly.
