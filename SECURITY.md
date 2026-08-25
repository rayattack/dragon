# Security policy

## Reporting a vulnerability

**Please do not open a public issue, discussion, or pull request for a security
problem.** A public report is readable by everyone the moment you press submit,
including people who would rather have the exploit than the fix.

Use one of these instead:

* [**Report a vulnerability privately**](https://github.com/rayattack/dragon/security/advisories/new)
  on the Security tab. Only the maintainers and you can see it. It stays private
  until we publish it.
* Email **security@dragonlang.org** if you would rather not use GitHub, or you do
  not have an account.

Tell us what you can. A rough report today beats a polished one next month:

* what an attacker gets (crash, memory disclosure, arbitrary write, code execution,
  a filter they can walk past)
* the smallest `.dr` program that shows it, and the exact command you ran
* how you installed Dragon and what `dragon --version` says
* whether the input has to come from somewhere untrusted (a socket, a request body,
  a filename, an archive) for it to fire

If you have a patch, hold it until we have a private advisory open, then attach it
there rather than opening a pull request.

We would rather hear from you privately and awkwardly than read about it on the
internet.

## What happens next

1. We confirm we received it, and reproduce it.
2. We fix the root cause, and add a regression test that fails without the fix.
3. We publish an advisory naming the affected versions, and credit you unless you
   ask us not to.

We do not run a bug bounty. We do say thank you in public, and mean it.

## Supported versions

Dragon is pre-1.0 and moves fast. Only the latest release gets security fixes, and
there are no backports to older tags. If you are running an older release, the fix
is to upgrade.

| Version        | Supported |
| -------------- | --------- |
| latest release | yes       |
| anything older | no        |

## Upgrading the compiler is not enough

Dragon links its runtime **statically** into every binary you build. A program you
compiled last month still carries the runtime it was built with, so a fix in the
compiler does not reach it.

After a security release, a deployed Dragon program is only fixed once you:

1. install the new toolchain, then
2. **rebuild** the program with it, and
3. **redeploy** the new binary.

This is the price of shipping one native binary with no runtime on the server. Plan
for it: know which of your binaries were built with which release, so that when an
advisory names a version you already know what has to go out again.
