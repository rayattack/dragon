---
name: Bug report
about: Something in the compiler, runtime, or stdlib misbehaves
title: ''
labels: bug
assignees: ''

---

**Stop if this is a security problem.** A crash on untrusted input, memory
corruption, disclosure of memory you should not see, or anything an attacker could
use: do not file it here, because this issue is public the moment you submit it.
[Report it privately instead](https://github.com/rayattack/dragon/security/advisories/new),
or email security@dragonlang.org. See the
[security policy](https://github.com/rayattack/dragon/blob/main/SECURITY.md).

## Environment

* `dragon --version` output:
* OS and architecture (e.g. Ubuntu 24.04 x86_64, macOS 15 arm64):
* How you installed Dragon (deb/rpm, tarball, install.sh, built from source):

## Minimal repro

The smallest `.dr` program that shows the problem:

```dragon
# paste here
```

The exact command you ran (`dragon run repro.dr`, `dragon build ...`, `dragon check ...`):

## Expected behavior

What you expected to happen.

## Actual behavior

What happened instead. Paste the full compiler diagnostic or program output,
and the exit code / signal if the program crashed.
