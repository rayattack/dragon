# Contributing to Dragon

First: thank you. Genuinely.

Dragon is a young language, which means the things you notice are probably real. If something confused you, it will confuse the next person, and might even confuse a veteran contributor if they came back to it after 3 months of not contributing. If an error message read
badly, it read badly for everyone. Telling us that is a contribution, whether or not
you ever write a line of code here.

## You do not need permission

Open the issue. Send the draft PR. Ask the question you think is too obvious to ask.

Things that genuinely count as contributions:

- a typo, a dead link, or a paragraph in the docs that only makes sense if you already knew the answer
- a test that pins down behavior nobody had tested
- a bug report with a small reproducer
- an error message that told you what broke but not what to do about it
- an actual compiler, runtime, or stdlib change

If you are not sure something is wanted, open an issue and ask. "Is this worth
doing?" is a perfectly good first message, and you will get a real answer. That answer might point you
to some documentation though ;-)

## Getting it building

You need a C++17 compiler, CMake 3.16 or newer, and **LLVM 21 or newer** with dev
headers. (Codegen uses the Triple-based Module API that landed in LLVM 21, so older
LLVMs are rejected at configure time rather than failing mysteriously later.)

```bash
mkdir build && cd build
cmake .. -DDRAGON_BUILD_TESTS=ON
cmake --build . -j4
ctest -j2 --output-on-failure
```

The compiler lands at `build/dragon`.

If a fresh clone does not build for you, that is a bug in our instructions, not a
failing on your part. Open an issue with your OS and LLVM version and we will fix
the docs.

## Run your test programs under a cap

This is not a warning about Dragon. `main` is not sitting on a pile of leaks, and a
compiled Dragon program behaves like any other native binary.

It is about the code you are in the middle of writing. Dragon compiles straight to
machine code, so there is no interpreter between a loop you got slightly wrong and
your actual RAM. Every systems project has this. It is a little more visible here
because plenty of our tests exist specifically to push on allocation and ownership,
so "allocate until something gives" is a thing you will write on purpose sometimes.

The habit that makes it a non-event:

```bash
( ulimit -v 4000000; timeout 90 ./your_program )      # ~4GB, adjust to taste
( ulimit -v 4000000; ctest -R your_test )             # children inherit the cap
```

With that in place, a runaway program dies on a clean allocation failure inside 90
seconds and you get a useful bug report out of it, rather than a machine you have to
go rescue.

ASan builds are the one exception. Never put `ulimit -v` on them: ASan reserves about
20TB of virtual address space and will abort at startup. Cap physical memory instead:

```bash
ASAN_OPTIONS=hard_rss_limit_mb=2048 timeout 90 ./your_asan_program
```

Two smaller habits from the same family:

- Keep probe loops small, a few thousand iterations. Leaks are linear, so you do not
  need a million of anything to see one.
- Build with `-j4` rather than `-j$(nproc)`. That one is about LLVM rather than
  Dragon: linking is memory hungry and parallelism multiplies it.

## What Dragon is trying to be

Please read [`zen.md`](zen.md) before a larger change. It is short, and it is the
honest answer to "why is it like this?"

Three ideas drive most review comments:

**Speed is the point.** Dragon reads like Python and runs like a compiled language.
If a change makes the hot path allocate, box, or interpret, expect questions. A leak
is a performance bug, not a cosmetic one.

**Fix causes, not symptoms.** We would much rather read a PR that says "I found the
root cause and it is bigger than I thought" than one that papers over it. A silent
fallback is a silent lie. A test that passes for the wrong reason is a bug wearing a
disguise.

**Types stay honest.** If a value's concrete type is knowable, it should flow at
that type. `Any` is a last resort, never a fallback. Ambiguity should be a compile
error you fix with an annotation, not a box that hides it until runtime.

None of this means your first PR has to be perfect. It means these are the things we
will talk about together.

## Writing tests

Dragon is written in Dragon wherever it can be. The stdlib is 75 `.dr` modules and
the suite has 232 dogfooded `.dr` tests, so prefer one of those:

Add a `TestCase` to a file in `test/dr/`, using `from unittest import TestCase, main`,
and wire it into that file's `main([...])` list. These run through the real compiler
end to end and catch things narrower tests quietly miss.

Reach for a C++ gtest in only two situations:

1. you need to assert on captured **stdout** (the `unittest` framework asserts on
   values, not output)
2. the program **must not compile** (a rejection test cannot be a `.dr` test, since
   the suite needs every `.dr` test to compile)

If you are touching ownership, reference counting, or raw memory: please write the
failing test **first** and run it under ASan/LSan before you write the fix. It sounds
like the slow path. It is dramatically faster than finding out three weeks later.

## Sending the pull request

Fork the repo, then work on a topic branch (`fix-parser-crash`, `docs-typo`, whatever
describes it). You *can* open a PR straight from your fork's `main` and it will work
fine, but a branch is kinder to you: it lets you keep more than one PR open, and it
keeps your fork's `main` clean for syncing with Dragon's main branch as it changes.

Open the PR against `main`. A few things, so nothing catches you off guard:

- **CI must be green.** The required check is `Linux build + test`.
- **A secret scan runs on every PR.** If it flags something, it is usually a test
  fixture that looks like a credential. Tell us and we will sort it out with you.
- **`main` requires signed commits, but you do not need to sign yours.** We
  squash-merge outside contributions, which means GitHub signs the resulting commit
  while keeping you as its author. Your name stays on your work.
- **Small PRs get reviewed faster.** If a change is growing, open it as a draft early
  and we will talk through the approach before you spend another weekend on it.

For commit messages, say *why*, not just *what*. "fix: don't decref a borrowed element
on list overwrite" tells a future reader (often you, in six months) far more than
"fix bug".

## On AI

Time is the ultimate currency of mankind - it is more valuable than `I typed every character by hand` bragging
rights. So AI use is welcome here, use it, if you want. It is a tool, and good tools are good.

The one thing we ask for is ownership. If it is in your PR, it is yours. That means:

- you know what the change actually does, not roughly, but actually
- you can explain why it is written that way
- you can defend it in review, and change it when a reviewer has a point
- if it breaks something six months from now, you are the person who understands it

That is the same bar we would hold for code you typed by hand out of a Stack Overflow
answer. The tool is not the issue. Landing code that nobody in the conversation
understands is the issue, because eventually a real person has to debug it at 2am,
and this project is small enough that the person is someone you have talked to.

If AI wrote something you do not fully follow, that is normal. Say so in the PR.
"I am not sure why this branch is needed" is a genuinely useful thing to write, and
far better than quietly hoping nobody asks. We will work it out together, and you
will know the code afterward.

One practical warning: **AI is confidently wrong about Dragon** more often than about
most languages, because it pattern-matches to Python or Rust and Dragon is neither.
Treat suggestions about types, ownership, and `Any` with particular suspicion.

## What to expect from review

We try to be quick and specific. If a review goes quiet, ping the PR. You are not
being ignored, we just dropped it.

We review the code, not the person. If a comment reads blunt, that is brevity and not
irritation. And if we ask for a change and you think we are wrong, please say so. You
may well be right, and "here is why I did it that way" is a normal, welcome reply,
not a confrontation.

Be kind in issues and reviews. That is the whole code of conduct.

## Bugs and security

For ordinary bugs, open an issue: what you did, what happened, what you expected. A
small reproducer is worth a great deal.

For security issues, please do **not** open a public issue. Use GitHub's private
vulnerability reporting on the repository's Security tab so it can be fixed before it
is public. We would rather hear from you privately and awkwardly than read about it
on the internet.

## Licensing

Dragon is MIT licensed (see [`LICENSE.txt`](LICENSE.txt)). By contributing, you agree
your contribution goes out under those same terms. There is no CLA to sign, no
paperwork, and no copyright assignment. Your work stays yours, under a license that
lets everyone use it.

## Finally

If you read this far, you are already the sort of person this project needs. Thank
you for spending your time here. We know your time is the actual currency, and we do
not take it lightly.
