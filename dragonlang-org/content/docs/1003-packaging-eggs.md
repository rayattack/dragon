# Packaging and Sharing Code (Eggs)

[Packages and Project Layout](/docs/1002-packages-and-layout) took you as far as the
shipped compiler goes: sibling files, package directories, and three real knobs
(`-I`, `DRAGON_STDLIB_PATH`, `--site-packages`) for assembling a multi-file program.
What Dragon does *not* have yet is the next rung up the ladder - a way to **name** a
dependency, **fetch** it from somewhere, **pin** it to a verified hash, and have the
compiler find it without you wiring paths by hand. That layer is designed, in
detail, but it is **not shipped**.

This chapter is deliberately split so you never confuse the two halves. If you read
one sentence, read this: **today you vendor your dependencies** - you copy the `.dr`
files into your tree or point `-I` at them - and the egg workflow below is the
design we're building toward, not a command you can type yet.

## What works today: vendoring

Until the egg manager ships, you vendor. Copy the `.dr` files you depend on into
your tree (or a `lib/` folder you point `-I` at), commit them, and import them like
any sibling module - exactly the mechanics in
[Packages and Project Layout](/docs/1002-packages-and-layout). It's more manual than
`dragon grab http` will be, and you own keeping the copies current - but it compiles
to exactly the same single-module program the egg workflow will produce, because
**eggs are *source* distributions, not precompiled artifacts.** Nothing about how
you write the importing code changes when eggs arrive; only how the files got onto
disk does.

> **The shipped CLI has exactly three verbs:** `dragon run`, `dragon build`, and
> `dragon check`. There is **no** `grab`, `sync`, `init`, or `dragon.drs`
> resolution in the compiler today. Everything below is design, not a command you
> can run.

## The egg model (planned - not yet shipped)

An **egg** is a Dragon package: a directory with a manifest, a root module, and
optionally submodules, tests, and a license. The design gives Dragon
the dependency story Cargo gives Rust and pip gives Python - *without* their worst
footguns. Three choices define it:

- **The manifest is Dragon Script (`.drs`)**, not TOML or JSON - parsed by
  `stdlib/drs.dr` (pure Dragon, zero external parsers), with comments, variables,
  and conditionals, so a manifest can adapt to the build environment.
- **No separate lockfile.** The integrity hash of every dependency lives *inline*,
  next to the version, in the one manifest. The manifest *is* the lockfile; when a
  PR adds a dependency, the hash is in the diff.
- **Content-addressed by SHA-256.** A package's identity *is* the hash of its
  tarball. The registry is an immutable store, so a hash you pinned on Tuesday
  resolves to the same bytes on Friday.

### The `dragon.drs` manifest

A project declares its identity and dependencies in a `dragon.drs` at the root. It's
Dragon Script - single-quoted strings, `#` comments, nested `{ }` tables:

```drs
# Dragon package manifest (planned format)
{
  name = 'myapp'
  version = '0.1.0'
  description = 'A web service in Dragon'
  authors = ['Tersoo <tersoo@example.com>']
  license = 'MIT'
  entry = 'main.dr'               # default file for `dragon run`
  dragon = '>=0.2.0'              # minimum compiler version

  eggs = {
    http     = { version = '^1.0', hash = 'sha256:9f86d081884c7d65...',
                 signed_by = 'ed25519:46b7d7e1...' }   # also pin the publisher's key
    logging  = { version = '^0.1', git = 'https://github.com/u/dragon-logging.git',
                 hash = 'git:a1b2c3d4e5f6...' }
    local-lib = { path = '../my-lib' }     # your own code, no hash
  }

  permissions = {
    http    = ['net']             # http may open network connections
    logging = ['fs:write']        # logging may write files
  }
}
```

A few field semantics that matter: `name` maps to the import name with hyphens
becoming underscores (`csv-utils` imports as `csv_utils`); `entry` designates a
**file**, never a magic `main` function, consistent with Dragon's entry model;
`hash` is verified on every install, and **no hash means the install refuses**
(except `path` deps, which are your own code). The optional `signed_by` pins the
publisher's Ed25519 key, so a republish under a different key is rejected - see
[Supply-Chain Security](/docs/1004-supply-chain-security) for that and the rest of
the trust model (the signed transparency log, Merkle proofs, and `dragon scan`).
Because `.drs` is programmable, a manifest can branch on the environment - dev-only
eggs, platform-specific deps - which is why it's Dragon Script rather than another
static config dialect.

### Where eggs land: `.drx/`

Fetched packages install into a project-local `.drx/` directory (git-ignored,
reconstructible from the hashes in `dragon.drs`) - per-project, not global, which
sidesteps the environment-conflict mess pip's global model creates. A global
content-addressed cache (`~/.drx/eggs/`) sits behind it so two projects on the same
`http` version share one download. When the manager ships, the resolver gains one
new row - `.drx/` slots in just after the source directory - so the resolution
table from the previous chapter *grows*, it doesn't change.

### The planned CLI verbs

Every package verb is a flat, four-character command - no `dragon egg <sub>` prefix.
**None of these run in the shipped compiler yet:**

| Verb | Planned behavior |
|------|------------------|
| `init` | Create `dragon.drs` (interactive prompts) |
| `grab <egg>` | Fetch, add to manifest, pin version + hash |
| `drop <egg>` | Remove a dependency and its permissions |
| `sync` | Install every dep in `dragon.drs`, verify every hash |
| `bump [egg]` | Update to the latest compatible version + hash |
| `list` / `info <egg>` | Show the dependency tree / package metadata |
| `hash` / `scan` | Re-verify hashes / audit deps against the signed transparency log |
| `keygen` | Generate the author's Ed25519 signing key |
| `find <q>` / `push [--sign]` / `yank` / `wipe` | Search / publish (optionally signed) / retract / clear cache |

The flow is the familiar one: `dragon grab http` writes both the version constraint
and the freshly-computed hash into `dragon.drs`; `dragon sync` later re-fetches and
**hard-errors on any hash mismatch**. Notably absent is any script-runner
(`npm run`) - install-time script execution is the vector behind the worst
supply-chain incidents, and it's unnecessary here: a "task" is just a `.dr` file you
`dragon run`.

### Permissions: capabilities, not trust-everything

The piece with no analogue in pip or npm is **capability permissions**. An egg
declares what system access it needs; the consuming project must explicitly grant
it; **undeclared use is a compile error.**

| Capability | What it allows |
|-----------|----------------|
| `pure` | Pure computation only (the default) |
| `fs:read` / `fs:write` | Read / write-create-delete files |
| `net` | Open network connections |
| `env` / `proc` | Read environment vars / spawn subprocesses |
| `ffi` | Use `extern "C"` / call native code (full trust) |

A package with no declared capabilities is **pure** - it can only compute with the
data passed to it. Because Dragon compiles every source into one module and the
resolver knows which file belongs to which package, the compiler can check each
capability-gated construct against your manifest's grants and refuse to emit IR if a
grant is missing - so when a JSON formatter asks for `net` and `ffi`, you see it in
your `dragon.drs` before the code runs. (`ffi` is the trust boundary: it reaches the
OS directly, so granting it is granting full trust.)

## Status, stated plainly

| Capability | Today | Planned |
|-----------|:-----:|:--------------:|
| Sibling-file imports, package dirs, `-I`, `DRAGON_STDLIB_PATH`, `--site-packages` | **Works** | - |
| Declare deps in a manifest (`dragon.drs`) | No | Yes |
| Fetch / pin / verify (`grab`/`sync`/`bump`) | No | Yes |
| Content-addressed hashes, no lockfile | No | Yes |
| Capability permissions | No | Yes |
| Central registry, publishing (`push`) | No | Yes |

**Until the egg manager ships, you vendor** - and lay your packages out as
`name/name.dr` so they're egg-shaped when `grab` and `sync` arrive. That closes Part
10. For how modules signal failure across boundaries, see
[Error Handling](/docs/0901-exceptions); for what they can reach out and touch, see
[the Standard Library](/docs/1401-stdlib-overview).
