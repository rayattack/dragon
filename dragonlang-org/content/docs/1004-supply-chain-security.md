# Supply-Chain Security

[Packaging and Sharing Code (Eggs)](/docs/1003-packaging-eggs) describes how a
dependency gets *named, fetched, and pinned*. This chapter is about the harder
question underneath it: **why should you trust the bytes that arrive?** Supply-chain
attacks - a compromised maintainer, a tampered mirror, a typosquatted name, a
postinstall script that steals your tokens - are the defining security problem of
modern package managers. npm, PyPI, RubyGems, and crates.io have all been hit.

Dragon's answer is designed from day one, not bolted on after the first incident.
The guiding idea is one sentence:

> **The registry is a verifiable trust root, not a trusted party.** Every byte you
> install is checked against a hash you pinned and an append-only, signed log -
> so a compromised registry, CDN, or mirror cannot feed you code you did not
> agree to, and cannot do so undetectably.

> **Status.** The model below is implemented in the registry service and the egg
> client, with end-to-end tests. It is **not yet a shipped `dragon` CLI verb** -
> the compiler today has only `dragon run`, `dragon build`, and `dragon check`
> (see [Packaging and Sharing Code](/docs/1003-packaging-eggs)). Read this as the
> security architecture the egg workflow is built on, not commands you can type in
> the shipped compiler yet. The authoritative design is the design spec.

## The four problems, kept separate

Most package-manager security failures come from conflating four independent
concerns. Dragon keeps them apart:

| Concern | Question | Dragon's answer |
|---|---|---|
| **Identity** | What *is* `http`? | A name in a registry you own, never a forge URL |
| **Availability** | Will the bytes always be there? | Own an immutable, content-addressed byte store |
| **Integrity** | Are these the bytes the author published? | SHA-256 pinned in `dragon.drs`, verified on every fetch |
| **Provenance** | Did everyone get the same bytes, and who signed them? | An append-only, Ed25519-signed transparency log |

A hash gives you integrity but *not* availability, identity, or provenance. So a
hash alone is necessary but not sufficient - which is why the rest of this chapter
exists.

## Hashes pinned in the manifest, verified on every fetch

There is no separate lockfile. The SHA-256 of each dependency lives **inline** in
`dragon.drs`, right next to the version:

```drs
eggs = {
  http = { version = '^1.0', hash = 'sha256:9f86d081884c7d65...' }
}
```

`grab` computes that hash from the bytes it actually received and writes it.
`sync` re-downloads and **hard-errors if `sha256(tarball)` does not match the pin**.
The security-relevant fact is visible in your PR diff, not buried in a generated
file nobody reads. The registry is never trusted for integrity: the bytes are.

## Packages are source, and nothing runs on install

The defining supply-chain disaster - npm's self-replicating *Shai-Hulud* worm
(2025) - spread through `postinstall` scripts: arbitrary commands a *dependency*
declares that run *automatically* on install, with full access to your tokens and
network.

Dragon removes the entire shape. **There is no install-time execution.** `grab` and
`sync` only fetch and verify *source*; a Dragon package is never an executable, and
nothing in a dependency runs on your machine until *you* compile and run *your*
program. There is also **no `scripts`/`tasks` block** in the manifest and no command
to run one - a project command is just a `.dr` file you `dragon run`. That single
choice neutralizes the attack class that dominates npm and PyPI.

## Capability permissions

A package must declare the system capabilities it needs (`net`, `fs:read`,
`fs:write`, `proc`, `env`, `ffi`); the consuming project must explicitly grant them.
Undeclared use is a compile-time error, not a warning - the code never reaches code
generation. The safe default is **pure**: no file access, no network, no FFI, no
subprocesses. When a JSON formatter asks for `net` or `ffi`, that is a red flag you
see in your `dragon.drs` before you ever run the code.

## The manifest is sandboxed against hostile input

Dragon's manifest is `.drs`, which is *programmable* (it has `include`, `for`,
conditionals). That is a power neither Cargo's TOML nor Go's manifests have - and a
risk they do not have either, because a dependency's `dragon.drs` is *evaluated* by
the resolver before you have decided to trust the package. So `.drs` evaluation is
confined:

- `for i in range(N)` is capped, so a manifest cannot generate an unbounded list to
  exhaust memory.
- `include` is confined to the document's own directory - no absolute paths, no
  `..` traversal - and is rejected entirely for a manifest parsed from a string.

A malicious dependency can neither read your `/etc/passwd` nor hang your build by
being resolved.

## The transparency log: verify the registry, do not trust it

Every publish is appended to an **append-only log**. Each entry is a leaf binding
`(name, version, hash)`; the leaves form an RFC 6962 Merkle tree. The registry signs
the tree's root with an **Ed25519** key in a *checkpoint*, and publishes its public
key. Clients pin that key trust-on-first-use.

This buys three things a hash alone cannot:

- **Authenticity.** A man-in-the-middle or rogue mirror cannot forge a checkpoint -
  it is signed by a key you pinned. A bad signature is rejected.
- **Inclusion.** On first acquisition, the client proves *this exact package* is in
  the signed tree with an O(log n) **inclusion proof** - no need to download the
  whole log.
- **Consistency.** `dragon scan` fetches an O(log n) **consistency proof** that the
  log only ever *appended* since you last looked. A registry that quietly rewrote
  history is caught.

```
$ dragon scan
scan: checkpoint signature verified (size 412, root c87f325a6f4ef983...)
scan: consistency proof verified (append-only from size 390 to 412)
scan: Merkle root verified (412 entries)
scan: 7 registry dep(s) present in the log with matching hashes
```

Because the registry commits, in a structure it cannot silently rewrite, to the
exact bytes published for each version, a registry that later swaps the bytes for a
version is detectable - the log still attests the original hash.

## Author signing

Beyond *what* was published, you can pin *who* is trusted to publish it. An author
generates an Ed25519 keypair (`dragon keygen`) and signs the tarball on publish
(`dragon push --sign`); the manifest pins the author's public key:

```drs
eggs = {
  frames = { version = '^0.1', hash = 'sha256:...', signed_by = 'ed25519:46b7d7e1...' }
}
```

The client verifies the detached signature over the downloaded bytes and refuses an
install whose signature does not match the pinned key. This is what catches a
*compromised-maintainer* republish under a new key: the log faithfully records the
new (attacker) upload, but your pinned author key no longer matches, and the install
is rejected.

## Storing the bytes: own the trust root, commoditize the bytes

Because content-addressing plus the signed log make every fetched byte verifiable
*regardless of where it came from*, the byte store is **untrusted infrastructure**.
That reframes the hosting question entirely:

- The **trust root** - the index, accounts, and the signed transparency log - is
  small, owned, and neutral. It is never tied to a corporate identity provider; the
  registry is its own identity provider precisely so one platform's policy change
  cannot lock contributors out.
- The **bytes** live in an immutable, content-addressed, public-read object store
  (Cloudflare R2 + CDN). Owning this copy is the *availability* guarantee - the
  "left-pad" insurance - the same role Go's module proxy plays. Download needs no
  account; only publishing does.
- **Mirrors** (a GitHub-hosted copy, a university mirror, IPFS) are a free, verified
  fallback: the client tries the next source and still checks the hash and the log.
  A forge is a fine *mirror*; it is the wrong *primary*, because that would re-import
  left-pad risk and the loss of the publish-time chokepoint.

You own the bytes for availability, not for trust. The log provides trust.

## Accounts and publishing

Publishing requires a registry account: a verified email (not KYC -
pseudonymous publishing is expected), a password hashed with **Argon2id** and
checked against known-breach lists, and **mandatory TOTP 2FA** (authenticator apps
only - no SMS). `dragon push` uses **scoped, revocable publish tokens**, never your
password, so a leaked CI token cannot take over an account. Logins are
rate-limited - credential stuffing is the number-one attack on package registries.

## What is deliberately out of scope (for now)

- **Merkle in the client at scale.** Inclusion and consistency proofs are
  implemented; if the log grows very large, witnessing/gossip between independent
  log monitors is the next rung (as in Certificate Transparency).
- **Hard-deleting malicious versions.** Yank *hides* a version from resolution and a
  security-yank *blocks* download, but the hash always resolves so reproducibility is
  never silently broken; a known-bad hash fails closed with an explicit override.

## Summary

| Threat | Mitigation |
|---|---|
| Tampered package / mirror | SHA-256 pin verified on every fetch; signed Merkle inclusion proof |
| Install-time code execution | No install scripts; packages are source, nothing runs until you build |
| Malicious code in a legit package | Capability permissions, denied by default; compile-time enforced |
| Hostile manifest | `.drs` evaluation confined (bounded `range`, confined `include`) |
| Rewritten / equivocating registry | Append-only log, Ed25519-signed checkpoints, consistency proofs, `dragon scan` |
| Compromised maintainer | Author signing + pinned `signed_by`, verified per install |
| Registry/host outage | Owned immutable content-addressed store + verified mirrors |
| Account takeover | Argon2id + breach-list + mandatory TOTP; scoped, revocable publish tokens |
| Typosquatting | Name-similarity + homoglyph checks and reserved names on publish |
