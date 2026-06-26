# Packages and Project Layout

[Modules and Imports](/docs/1001-modules) covered how one file pulls in another.
This chapter is about *organizing* many files - grouping related modules into
packages, telling the compiler where to find code that lives outside your source
directory, and the privacy rules that let a package keep its internals to itself.
A "project" in Dragon is not a construct you opt into; it's a directory of `.dr`
files where one is the entry point and the rest contribute definitions. There's no
`Cargo.toml` or `package.json` you must create first.

## Package directories: `name/name.dr`

When a single module grows into a cluster of related ones, promote it to a
**package** - a directory whose root module shares the directory's name. Dragon's
rule is strict and worth memorizing: the package root is `name/name.dr`, and
submodules are plain files beside it.

```dragon
# textkit/textkit.dr - the package root, imported as `textkit`
def shout(s: str) -> str {
    return s.upper() + "!"
}
```

```dragon
# textkit/case.dr - a submodule, imported as `textkit.case`
def titlecase(s: str) -> str {
    return s.capitalize()
}
```

```dragon
# app.dr
from textkit import shout
from textkit.case import titlecase

print(shout("ready"))       # READY!
print(titlecase("dragon"))  # Dragon
```

Importing `textkit` resolves `textkit/textkit.dr`; importing `textkit.case`
resolves `textkit/case.dr`. No `__init__`, no `mod.rs`, no re-export ceremony. The
standard library's `os` package is the live example: `stdlib/os/os.dr` is the root
(imported as `os`) alongside `path.dr` (imported as `os.path`).

## Dotted imports

A dotted name maps to a path: `from os.path import join` reads `os/path.dr` - the
dots become directory separators and the resolver looks for that file under each
search root. This is the same mechanism whether the package is the standard
library's or your own.

## The flat-file XOR package rule

There is exactly one hard rule, and the compiler enforces it: a module name is
**either** a flat file `name.dr` **or** a package directory `name/` - *never both*
in the same directory. If the resolver finds both `conf.dr` and a `conf/` directory
side by side, it stops instead of guessing:

```text
module conflict: both '/path/conf.dr' and package '/path/conf/' exist - remove one
```

Python's silent precedence rules (package shadows module, sometimes) are a
perennial source of "why is my import resolving to the wrong file" bugs. Dragon
refuses the ambiguity. Pick a flat file or a package; you can't have both wearing
the same name.

## Extra search paths: `-I <dir>`

When code you depend on lives *outside* your source directory - a shared `lib/`
folder, a sibling checkout, a vendored copy - point the compiler at it with `-I`.
It adds a directory to the module search path:

```dragon
# lib/mathx.dr
def double(n: int) -> int {
    return n * 2
}
```

```dragon
# app.dr
from mathx import double
print(double(21))   # 42
```

```bash
dragon build app.dr -I lib/ -o app
```

`from mathx import double` finds `lib/mathx.dr` because `-I lib/` added that
directory to the search roots. You can pass `-I` more than once. This is the
honest, mechanical version of "depending on a package": you tell the compiler where
the files are, and you're responsible for getting them there.

## Overriding the bundled stdlib: `DRAGON_STDLIB_PATH`

The compiler ships its standard library at a baked-in location. To point at a
*different* stdlib tree - testing a local change to a stdlib module - set the
`DRAGON_STDLIB_PATH` environment variable:

```bash
DRAGON_STDLIB_PATH=/path/to/dragon/stdlib dragon build app.dr -o app
```

This is an override, not an addition: it replaces which directory the resolver
treats as the stdlib root. Reach for it when you're working *on* the standard
library; ordinary application code never needs it.

## Python site-packages: `--site-packages`

Dragon is not a Python superset, and the CPython C ecosystem (numpy, pandas, Flask)
does not work. But for *individually typed* `.py` files that stay inside the subset
Dragon implements, the compiler can search your Python `site-packages` as a final
fallback with `--site-packages` - auto-detecting the path via `python3` and using
Python conventions there (`name.py`, `name/__init__.py`). It's an adoption on-ramp
for typed `.py` modules, not a bridge to the dynamic Python ecosystem.

## Resolution order

Putting the knobs together, the resolver searches in this fixed order and takes the
**first** match:

| Order | Source | How you control it |
|------:|--------|--------------------|
| 1 | The source directory (entry file's folder) | Where you put your files |
| 2 | Extra search paths | `-I <dir>` (repeatable) |
| 3 | The standard library | `DRAGON_STDLIB_PATH` env var |
| 4 | Python site-packages (opt-in) | `--site-packages` |

Source-first means **your modules shadow the standard library**: drop a `math.dr`
next to your entry file and `from math import sqrt` resolves to *yours*, not the
stdlib one. Usually you don't want to shadow by accident - but when you need to
override or stub a module (a test double), the rule is predictable: closest file
wins.

## Privacy: `_protected` and `__private`

Python's leading underscore is a *convention* - `_x` is a polite "don't touch,"
`__x` only triggers name-mangling, both still reachable. Dragon makes the
convention **real**: leading underscores are enforced at compile time (the design spec, zero runtime cost). The rule is identical for class members and module-level
names, and identical in `.dr` and `.py`:

| Name shape | Class member | Module-level name |
|---|---|---|
| `name` | public | public - importable anywhere |
| `_name` | **protected** - same package, plus subclasses anywhere | **package-internal** - importable within the package, not from another |
| `__name` | **private** - the declaring class only | **file-private** - not importable, even by a sibling in the same package |
| `__name__` | reserved special method - public | reserved metadata (`__all__`, `__version__`) - public |

The **package** is the unit for the `_` tier: a package directory and its sibling
modules form one encapsulation boundary, and a flat single-file module is its own
package. So importing a package-internal name from *outside* the package is a
compile error:

```dragon
# bank.dr
_rate: float = 0.05          # package-internal helper
```

```dragon
# main.dr (a different package)
from bank import _rate       # ERROR: '_rate' is module-private to 'bank'
```

The class-member side of this - `__balance` reachable only inside its declaring
class - is covered in [Member Privacy](/docs/0605-privacy). Because `__name__`-style
names are the language's reserved protocol/metadata namespace, they stay public, and
defining an unrecognized `__x__` is a compile error so you can't fake a private name.

## At a glance

| You want to... | Write |
|----------------|-------|
| Group related modules | a package dir, root at `name/name.dr` |
| Import a package root / submodule | `from pkg import f` / `from pkg.sub import f` |
| Depend on code outside the source dir | `dragon build app.dr -I lib/` |
| Point at a different stdlib | `DRAGON_STDLIB_PATH=... dragon build app.dr` |
| Search typed Python packages | `dragon build app.dr --site-packages` |
| Make a name package-internal | a leading `_` (`_helper`) |
| Make a name file-private | a leading `__` (`__secret`) |

You can build any multi-file program today with these mechanics. The next rung -
naming, fetching, and pinning a third-party dependency - is the subject of
[Packaging and Sharing Code](/docs/1003-packaging-eggs).
