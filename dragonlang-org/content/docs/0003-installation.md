# Installation

The dragon toolchain is a single binary, `dragon`, that compiles, runs,
and type-checks `.dr` and `.py` source files. It also ships its own standard
library - there is nothing else to install once you have the binary.

**Mac and Linux**: `curl -fsSL https://dragonlang.org/install.sh | sh`

**Windows**: `coming soon`

## Install from a package

The quickest path on a desktop OS is a native installer from the 
[downloads page](https://dragonlang.org/download)

- For **linux** - You get a `.deb` (Debian/Ubuntu) or an `.rpm` (Fedora/RHEL)
downloadable. Both put `dragon` and `dr` on your `PATH` and the stdlib
under `usr/share/dragon`.
- For **macos** - You get a `.dmg` for Applice Silicon or Intel.
- For **Windows** - You an `.msi` installer that adds the same keywords i.e.
`dragon` and `dr` to your `PATH`.

A portable archive is offered for every platform too (see below), for
when you'd rather not install system-wide.

## From a release archive

The simplest way to get going is to download the prebuilt archive for
your platform, unpack it somewhere on your `PATH`, and verify:

```
$ dragon --version
dragon 0.0.1
```

The archive contains the `dragon` binary, a `dr` symlink (a shorter
alias used in examples throughout this book), and the bundled standard
library under `share/dragon/stdlib/`. If you move the binary, keep the
sibling `share/` and `lib/` directories next to it - `dragon` discovers
the stdlib by looking relative to its own executable path.

## Building from source

If you want to build the compiler from source - to track the bleeding
edge or to contribute - you'll need:

- A C++17 compiler (`gcc` 11+ or `clang` 14+). TODO: Self Hosting ;-)
- CMake 3.16 or newer.
- LLVM 21 or newer (a released LLVM, not a trunk snapshot). Dragon's
codegen uses the `Triple`-based `Module` API introduced in LLVM 21.

On Debian/Ubuntu the development package comes straight from
[apt.llvm.org](https://apt.llvm.org):

```
$ curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
    | sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc >/dev/null
$ echo "deb http://apt.llvm.org/$(. /etc/os-release; echo $VERSION_CODENAME)/ \
    llvm-toolchain-$(. /etc/os-release; echo $VERSION_CODENAME)-22 main" \
    | sudo tee /etc/apt/sources.list.d/llvm.list
$ sudo apt-get update && sudo apt-get install -y llvm-22-dev zlib1g-dev libzstd-dev
```

Then point CMake at it and build:

```
$ git clone https://github.com/tersoo/dragon
$ cd dragon
$ cmake -B build -DLLVM_DIR="$(llvm-config-22 --cmakedir)"
$ cmake --build build -j$(nproc)
$ ./build/dragon --version
```

The full build takes a few minutes on modern hardware. After it
finishes, the binary is in `build/dragon`. Add `build/` to your `PATH`
or copy `build/dragon` somewhere that already is.

## Editor support

Dragon programs are plain UTF-8 text. Any editor will do for the
exercises in this book. For syntax highlighting, look in the project repository,
it ships TextMate grammars under `editor/` that work in VS Code, Sublime,
TextMate, and most editors that consume that format. Tree-sitter
support and a language server are tracked but not required for anything
in this book.

## Verifying your install

A one-liner that exercises the parser, type checker, code generator, and
linker:

```
$ echo 'print("ok")' > /tmp/hello.dr
$ dragon run /tmp/hello.dr
ok
```

If you see `ok`, you're set. If anything went wrong, the troubleshooting
section in the appendix covers the common failures (missing LLVM
libraries, stdlib not found, link errors).

Now let's write something less trivial.

