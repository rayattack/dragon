# Files and the Filesystem

A program that never touches a file, an environment variable, or a
command-line argument is a calculator. The moment it has to *do*
something useful, it reaches out: it reads a config, lists a directory,
copies a tree, checks whether a path exists, looks up `$HOME`, or carves
out a scratch directory in `/tmp`. This chapter is about those reaches -
the standard-library modules that connect a Dragon program to the
filesystem it lives on:

- **`io`** - open, read, and write files.
- **`os`** - the filesystem and the process: directory listings, stat,
  recursive walks, environment variables, the working directory.
- **`os.path`** - pure string surgery on paths: join, split, and the
  existence/type checks you guard every read with.
- **`shutil`** - high-level file operations: copy, move, delete a tree.
- **`stat`** - interpret the mode bits `os` hands back.
- **`glob`** / **`fnmatch`** - match paths and names against shell
  wildcards (`*.txt`, `src/**/*.dr`).
- **`tempfile`** - scratch files and directories under `/tmp`.
- **`pathlib`** - an object-oriented `Path` over `os.path`.
- **`sys`** - the command line, via `argv()`.

Almost everything here is a thin, typed wrapper over libc - `fopen`,
`opendir`, `getenv`, `access`, `stat` - so it costs exactly what the C
call costs. The Python *names and shapes* are kept where they don't
fight that goal; where Dragon's typed, compiled model improves on Python
(a `with` block that genuinely closes the handle, a path check that's a
real `bool`), it takes the improvement. The higher-level modules
(`shutil`, `glob`, `pathlib`, `fnmatch`, `tempfile`, `stat`) are written
in Dragon itself, on top of those wrappers.

## Reading and writing files: `open`, `make`, `push`

File I/O is a reader/writer split, imported from `io`. One verb per
intent, and the verb *is* the mode - there are no `"r"`/`"w"`/`"a"` mode
strings to remember:

| Verb | Returns | Use |
|------|---------|-----|
| `open(path)` | `Reader` | read an existing file |
| `make(path)` | `Writer` | create it, or replace it (truncate) |
| `push(path)` | `Writer` | append to it |

```dragon
from io import open, make, push
```

Reading from a `Writer`, or writing to a `Reader`, is a *compile* error:
the split is enforced by the type system, not discovered at runtime.

### Reading

The whole-file readers - `text()`, `bytes()`, `lines()` - read everything
and close the handle for you, so the common case is a one-liner with no
`with` block and no leak:

```dragon
from io import open

content: str = open("/etc/hostname").text()    # whole file as a str
raw: bytes = open("/etc/hostname").bytes()      # whole file as raw bytes
names: list[str] = open("names.txt").lines()    # one entry per line
```

Each line from `lines()` keeps its trailing newline (same as Python), so
`.strip()` it when you only want the content:

```dragon
from io import open

for raw in open("names.txt").lines() {
    name: str = raw.strip()
    if len(name) > 0 {
        print(name)
    }
}
```

To stream a large file instead of slurping it whole, open it in a `with`
block and pull one piece at a time. A `Reader` is iterable - `for line in
r` yields lines lazily - and `line()` / `take(n)` read a single line or up
to `n` bytes from the current position:

```dragon
from io import open

with open("huge.log") as r {
    for line in r {
        if "ERROR" in line {
            print(line.strip())
        }
    }
}
```

The `with` block closes the handle when it exits - even on an early
`return` or a raised exception. (The whole-file readers above close
themselves once they have handed back the bytes; the streaming reads do
not, which is exactly why you scope them with `with`.)

Opening a file that does not exist raises a catchable `FileNotFoundError`:

```dragon
from io import open

try {
    print(open("/etc/nope.conf").text())
} except FileNotFoundError as e {
    print(f"no config: {e}")
}
```

When a missing file is an expected case with a default, guard with
`os.path.exists` instead - it sidesteps the exception entirely:

```dragon
from io import open
from os.path import exists

const path: str = "config.ini"
if exists(path) {
    print(open(path).text())
} else {
    print(f"no config at {path}, using defaults")
}
```

The `Reader` surface:

| Method | Does |
|--------|------|
| `text()` | whole file as a `str`, then close |
| `bytes()` | whole file as raw `bytes` (no decode), then close |
| `lines()` | whole file as a `list[str]` (newlines kept), then close |
| `line()` | one line from the current position (newline kept); `""` at EOF |
| `take(n)` | up to `n` bytes of text from the current position |
| `close()` | close the handle (idempotent) |
| `for line in r` | iterate lines lazily |

### Writing

`make` opens a fresh (truncating) `Writer`; `push` opens one positioned to
append. Unlike a `Reader`, a `Writer` holds its handle open until you
close it - Dragon has no finalizer - so a `Writer` lives inside a `with`
block (or you `close()` it yourself):

```dragon
from io import make, push

with make("log.txt") as w {
    w.write("starting up\n")     # str, UTF-8 encoded
    w.line("ready")              # write + a trailing newline
}

with push("log.txt") as w {
    w.line("first event")        # appended to the existing file
}
```

`write` is overloaded on its argument: a `str` is UTF-8 encoded, a `bytes`
is written verbatim - one method covers both text and binary:

```dragon
from io import make

with make("/tmp/out.bin") as w {
    w.write("a header line\n")        # str   -> UTF-8
    w.write(bytes([0, 1, 2, 255]))    # bytes -> verbatim
}
```

To create an empty file, `make(path).close()` is the one-liner: it opens
and closes, leaving a zero-byte file. Writing creates the file if it is
missing, but the parent *directory* must already exist - reach for
`os.makedirs` first if it might not.

The `Writer` surface:

| Method | Does |
|--------|------|
| `write(s)` | write a `str` (UTF-8) or `bytes` (verbatim); return bytes written |
| `line(s)` | write `s`, then a newline |
| `lines(xs)` | write each `str` in `xs` verbatim (no separators added) |
| `flush()` | flush buffered writes to disk |
| `close()` | close the handle (idempotent; the `with` block does it for you) |

> **Why is there no `make(path).write(s)` one-liner?** A whole-file
> *reader* can close itself the moment it hands back the bytes, so
> `open(p).text()` is safe. A *writer* cannot - it has no way to know you
> are done - and with no finalizer the handle would leak. Scope every
> writer with `with` (or call `close()`), and the file is flushed and the
> handle released deterministically at block exit.

> **In-memory streams.** `io` also provides `BytesIO` and `StringIO` -
> growable in-memory buffers with a read/write/`seek`/`tell` surface - for
> code that builds or parses bytes without touching disk. They are also
> where you reach for random access (`seek`), which the sequential file
> `Reader`/`Writer` do not offer.

> **Differs from Python.** Python has one `open()` builtin with mode
> strings, plus `pathlib.Path.read_text`/`write_text`. Dragon splits the
> intent into three verbs (`open`/`make`/`push`) returning distinct
> `Reader`/`Writer` types, so a mode mismatch is a compile error rather
> than a runtime `io.UnsupportedOperation`. There is no mode-string
> argument and no `Path.read_text` - the path string goes straight to
> `open`/`make`/`push`.

## Listing a directory

`os.listdir` returns the entries of a directory as a `list[str]` - names
only, with `.` and `..` already filtered out, exactly like Python:

```dragon
from os import listdir

names: list[str] = listdir("/etc")
print(f"{len(names)} entries")
for name in names {
    print(name)
}
```

Binding the result to a `list[str]` first (as above) reads more clearly
than looping the `listdir(...)` call in place, though either works.

To do anything *with* an entry - check its type, get its size - you need
its full path, which is `os.path`'s job. Here's the canonical "count the
subdirectories" loop:

```dragon
from os import listdir
from os.path import join, isdir

const root: str = "/etc"
names: list[str] = listdir(root)
dirs: int = 0
for name in names {
    full: str = join(root, name)
    if isdir(full) {
        dirs = dirs + 1
    }
}
print(f"{dirs} subdirectories under {root}")
```

### `scandir` - the listing plus the metadata

`os.listdir` makes you re-`stat` each entry through `os.path`. When you
need both the name *and* its type or size, `os.scandir` returns a
`list[os.DirEntry]` - one object per child, each carrying the name, the
full path, and methods that answer the type/size questions directly:

```dragon
import os

for e in os.scandir("/etc") {
    if e.is_dir() {
        print(f"dir:  {e.name}")
    } elif e.is_file() {
        print(f"file: {e.name} ({e.stat_size()} bytes)")
    }
}
```

A `DirEntry` exposes `name`, `path`, `is_file()`, `is_dir()`,
`is_symlink()`, `stat_size()`, `stat_mtime()`, `stat_mode()`, and
`inode()`.

> **Differs from Python.** CPython's `os.scandir` returns a lazy
> iterator that doubles as a context manager, and a `DirEntry` exposes
> `.stat()` returning a `stat_result`. Dragon returns a `list` you can
> `for`-loop directly, and the size/mtime/mode live in flat
> `stat_*()` methods.

### Walking a tree

For a recursive sweep, `os` gives you two flat collectors:
`os.walk_files(top)` returns every regular file beneath `top` as a
`list[str]` of full paths, and `os.walk_dirs(top)` returns every
subdirectory:

```dragon
import os

files: list[str] = os.walk_files("stdlib/os")
print(f"{len(files)} files in the tree")
for path in files {
    print(path)
}
```

> **Differs from Python.** Python's `os.walk` yields
> `(dirpath, dirnames, filenames)` tuples lazily. Dragon splits the two
> common needs into `walk_files` / `walk_dirs`, each returning a flat
> list of full paths.

## Creating and removing directories

`os.makedirs(path)` creates a directory and any missing parents in one
call - the moral equivalent of `mkdir -p`:

```dragon
import os
from os.path import isdir

os.makedirs("/tmp/build/cache/objects")   # makes all three levels
print(isdir("/tmp/build/cache/objects"))  # True
```

It raises `OSError` only on a genuine failure (a permission problem, a
path component that is a file). Re-creating a path that already exists is
a no-op - it does **not** raise, so you rarely need to guard it.

> **Differs from Python.** CPython's `os.makedirs` raises
> `FileExistsError` for an existing leaf unless you pass `exist_ok=True`.
> Dragon's `makedirs` is idempotent by default, like `mkdir -p`.

The other directory and file primitives on `os`:

| Call | Does |
|------|------|
| `os.makedirs(path)` | create `path` and any missing parents (raises `OSError` on failure) |
| `os.mkdir(path, mode)` | create one level; returns a libc `int` rc (`0` ok), does **not** raise |
| `os.rmdir(path)` | remove one *empty* directory |
| `os.remove(path)` / `os.unlink(path)` | delete a file (raises `OSError` if missing) |
| `os.rename(src, dst)` | rename / move within a filesystem |
| `os.replace(src, dst)` | rename, atomically replacing `dst` if it exists |

To delete a *non-empty* tree, reach for `shutil.rmtree` (below) -
`os.rmdir` only removes empty directories.

## Path manipulation: `os.path`

`os.path` is pure string work - it never touches the disk *except* for
the existence and type checks. Import exactly the functions you need:

```dragon
from os.path import join, basename, dirname, splitext, normpath

print(join("/var/log", "app.log"))      # /var/log/app.log
print(basename("/usr/local/bin/dragon")) # dragon
print(dirname("/usr/local/bin/dragon"))  # /usr/local/bin
print(normpath("/a/b/../c/./d"))          # /a/c/d

parts: list[str] = splitext("report.txt")
print(f"stem={parts[0]} ext={parts[1]}")  # stem=report ext=.txt
```

`splitext` peels only the *last* extension, exactly like Python:

```dragon
from os.path import splitext

parts: list[str] = splitext("archive.tar.gz")
print(f"{parts[0]} | {parts[1]}")   # archive.tar | .gz
```

The disk-touching trio - `exists`, `isfile`, `isdir` - return a real
`bool`, so they read naturally in a condition:

```dragon
from os.path import exists, isfile, isdir

print(exists("/etc/hosts"))   # True
print(isfile("/etc/hosts"))   # True
print(isdir("/etc"))          # True
print(isdir("/etc/hosts"))    # False
```

| Function | Returns |
|----------|---------|
| `join(a, b)` | `a/b`, inserting the separator only as needed |
| `basename(p)` | the final component (`dragon`) |
| `dirname(p)` | everything before it (`/usr/local/bin`) |
| `splitext(p)` | `[stem, ext]`, e.g. `["report", ".txt"]` |
| `split(p)` | `[dirname, basename]` |
| `normpath(p)` | collapse `.`, `..`, and doubled slashes |
| `abspath(p)` | resolve to an absolute path |
| `relpath(p, start)` | `p` expressed relative to `start` |
| `commonpath(paths)` | longest shared sub-path (segment-aware) |
| `isabs(p)` | `bool`: is the path absolute? |
| `exists(p)` / `isfile(p)` / `isdir(p)` | `bool`, stat-backed |
| `getsize(p)` | file size in bytes |
| `getmtime(p)` / `getatime(p)` / `getctime(p)` | timestamps, seconds since epoch |
| `expanduser(p)` / `expandvars(p)` | expand `~` / `$VAR` |

```dragon
from os.path import split, relpath, commonpath

sp: list[str] = split("/usr/local/bin/dragon")
print(f"{sp[0]} | {sp[1]}")             # /usr/local/bin | dragon
print(relpath("/a/b/c", "/a/b"))        # c
print(commonpath(["/usr/lib", "/usr/local"]))  # /usr
```

> **`join` is binary, not variadic.** Python's `os.path.join` takes any
> number of components; Dragon's takes exactly two. To chain, nest the
> calls - `join(join(root, "sub"), name)` - or reach for `pathlib`'s `/`
> operator (below), which reads far better for multi-segment paths.

> **`split` and `splitext` return a `list[str]`, not a tuple.** Index
> with `[0]` / `[1]` (or bind the two elements). Python hands back a
> 2-tuple; Dragon's wrapper hands back a two-element list. The data is
> the same; the indexing is identical.

## High-level file operations: `shutil`

`os` gives you one-level primitives; `shutil` gives you the recursive,
high-level moves you'd otherwise hand-roll over `listdir` + `copyfile`.
It is written in pure Dragon over `os` and `io`.

```dragon
import os
import shutil
from io import make
from os.path import join, exists

const base: str = "/tmp/shutil_demo"
if exists(base) { shutil.rmtree(base) }
os.makedirs(base)

with make(join(base, "a.txt")) as w { w.write("hello") }
os.mkdir(join(base, "sub"), 493)   # 493 == 0o755

# copy a file *into* a directory (keeps the basename)
shutil.copy(join(base, "a.txt"), join(base, "sub"))
print(exists(join(join(base, "sub"), "a.txt")))   # True

# move (= rename) a file
shutil.move(join(base, "a.txt"), join(base, "b.txt"))
print(exists(join(base, "b.txt")))   # True

# copy a whole directory tree to a new location
shutil.copytree(join(base, "sub"), join(base, "sub2"))

# delete the tree, files and all
shutil.rmtree(base)
print(exists(base))   # False
```

`shutil.which` locates an executable on `$PATH`, returning the full
path or the empty string when nothing matches:

```dragon
import shutil

const sh: str = shutil.which("sh")
print(len(sh) > 0)   # True
print(shutil.which("definitely-not-a-program"))   # (empty line)
```

| Function | Does |
|----------|------|
| `copyfile(src, dst)` | copy contents of `src` to the file `dst`; return `dst` |
| `copy(src, dst)` | copy `src` to a file *or directory* `dst`; return the destination |
| `copytree(src, dst)` | recursively copy directory `src` to a new `dst` |
| `move(src, dst)` | rename `src` to `dst` (into `dst` if it's a directory) |
| `rmtree(path)` | recursively delete a directory and everything under it |
| `which(cmd)` | full path of `cmd` on `$PATH`, or `""` |

> **Differs from Python.** `copyfile` is byte-exact (whole-file bytes
> I/O), so it copies binary and text alike. There is no `disk_usage`,
> `chown`, or `copymode`/`copystat` - for permission bits drop to
> `os.chmod_path`.

## Reading the mode bits: `stat`

`os.getmode(path)` (and `DirEntry.stat_mode()`) returns the raw
`st_mode` integer - a packed field of file-type and permission bits. The
`stat` module decodes it. Its `S_IS*` predicates take that integer and
answer one type question each; `S_IMODE` masks off the permission bits;
`filemode` renders the whole thing as an `ls -l` string.

```dragon
import os
import stat
from io import make

const fp: str = "/tmp/stat_demo.txt"
with make(fp) as w { w.write("x") }

const mode: int = os.getmode(fp)
print(stat.S_ISREG(mode))     # True  - a regular file
print(stat.S_ISDIR(mode))     # False
print(stat.filemode(mode))    # -rw-rw-r--

# the permission bits alone, as a number you can oct()
print(oct(stat.S_IMODE(mode)))   # 0o664

os.remove(fp)
```

| Helper | Returns |
|--------|---------|
| `S_ISDIR(m)` / `S_ISREG(m)` / `S_ISLNK(m)` | `bool` for that file type |
| `S_ISCHR(m)` / `S_ISBLK(m)` / `S_ISFIFO(m)` / `S_ISSOCK(m)` | `bool` for the rarer types |
| `S_IMODE(m)` | the permission bits (`m & 0o7777`) |
| `S_IFMT_OF(m)` | the file-type bits alone |
| `filemode(m)` | an `ls -l`-style `str` like `-rwxr-xr-x` |

The named bit constants (`S_IRUSR`, `S_IWGRP`, `S_IXOTH`, `S_ISUID`, …)
are all present as plain `int`s for masking by hand.

## Matching paths: `glob` and `fnmatch`

`fnmatch` matches a single *name* against a shell pattern; `glob`
matches *paths on disk* against one. The wildcard vocabulary is the
shell's: `*` (any run of characters), `?` (one character), `[abc]` /
`[a-z]` (a character class), `[!abc]` (negated class).

`fnmatch.fnmatch(name, pattern)` returns a `bool`, and
`fnmatch.filter_names(names, pattern)` keeps the matching entries of a
list:

```dragon
import fnmatch

print(fnmatch.fnmatch("report.txt", "*.txt"))     # True
print(fnmatch.fnmatch("img01.png", "img??.png"))  # True
print(fnmatch.fnmatch("z.c", "[!ab].c"))          # True

const names: list[str] = ["a.py", "b.txt", "c.py"]
const py: list[str] = fnmatch.filter_names(names, "*.py")
print(len(py))   # 2  - ["a.py", "c.py"]
```

> **Differs from Python.** Matching is always case-sensitive (POSIX),
> so `fnmatch` and `fnmatchcase` behave identically. `translate` is a
> stub that returns the pattern unchanged (no regex back-end), and the
> filtering helper is named `filter_names`, not `filter`.

`glob.glob(pattern)` does the disk walk and returns a `list[str]` of
matching paths. A pattern with no wildcards just tests existence; `**`
recurses into subdirectories:

```dragon
import os
import glob
import shutil
from io import make
from os.path import join, exists

const base: str = "/tmp/glob_demo"
if exists(base) { shutil.rmtree(base) }
os.makedirs(base)
make(join(base, "a.txt")).close()
make(join(base, "b.txt")).close()
make(join(base, "c.log")).close()
os.mkdir(join(base, "sub"), 493)
make(join(join(base, "sub"), "d.txt")).close()

const txts: list[str] = glob.glob(join(base, "*.txt"))
print(len(txts))    # 2  - a.txt, b.txt

const deep: list[str] = glob.glob(join(base, "**/*.txt"))
print(len(deep))    # 3  - a.txt, b.txt, sub/d.txt

shutil.rmtree(base)
```

`glob.has_magic(pattern)` reports whether a string contains any wildcard
characters, and `glob.iglob` is an alias for `glob` (Dragon has no lazy
iterator here yet, so it returns the same list).

> **Differs from Python.** Hidden entries (names beginning with `.`) are
> skipped unless the pattern's component itself starts with `.`, matching
> the shell. `iglob` is eager, not a generator.

## Scratch space: `tempfile`

`tempfile` carves out temporary files and directories under the system
temp directory (`$TMPDIR`, `$TEMP`, or `$TMP`, falling back to `/tmp`).

```dragon
import tempfile
import os
from io import make, open
from os.path import isdir, isfile, exists

print(len(tempfile.gettempdir()) > 0)   # True

# a fresh directory you own (mode 0o700)
const d: str = tempfile.mkdtemp("dragon-", "")
print(isdir(d))   # True

# create an empty temp file; the path is returned
const f: str = tempfile.mkstemp("work-", ".tmp")
print(isfile(f))  # True
with make(f) as w { w.write("scratch") }
print(open(f).text())   # scratch

# mktemp only *names* a free path - it does not create it
const n: str = tempfile.mktemp("plan-", ".txt")
print(exists(n))  # False

# you clean up what you create
os.remove(f)
os.rmdir(d)
```

| Function | Does |
|----------|------|
| `gettempdir()` | the system temp directory as a `str` |
| `mkdtemp(prefix, suffix)` | create and return a unique directory (mode `0o700`) |
| `mkstemp(prefix, suffix)` | create and return a unique empty file |
| `mktemp(prefix, suffix)` | return a unique unused path **without** creating anything |

> **Differs from Python.** The names are explicit `prefix`/`suffix`
> *positional* `str` arguments (pass `""` for either), and `mkstemp`
> returns the **path** as a `str`, not an `(fd, path)` pair. There is no
> `NamedTemporaryFile` or `TemporaryDirectory` context-manager class yet,
> and no auto-deletion - you remove what you create.

## Object paths: `pathlib`

`pathlib.Path` wraps `os.path` with attribute-style access and a `/`
operator for composition. It's the most readable way to build and
inspect paths, especially deep ones where nested `join` calls get noisy.

```dragon
import pathlib
from pathlib import Path

const p: Path = Path("/var/log/app.log")
print(p.name)       # app.log
print(p.stem)       # app
print(p.suffix)     # .log
print(str(p.parent))     # /var/log
print(p.parent.name)     # log - chained access reads naturally

# compose with /  - far cleaner than nested join()
const cfg: Path = Path("/etc") / "app" / "config.ini"
print(str(cfg))     # /etc/app/config.ini

# derive new paths
print(str(p.with_suffix(".json")))    # /var/log/app.json
print(str(p.with_name("error.log")))  # /var/log/error.log

# stat-backed predicates
print(Path("/etc").is_dir())          # True
print(Path("/etc/hosts").is_file())   # True
```

| Member | Returns |
|--------|---------|
| `name` | final component |
| `stem` | name without the extension |
| `suffix` | the extension (with the dot) |
| `parent` | the containing directory, as a `Path` |
| `parts` | the path's segments as a `list[str]` |
| `p / "sub"` / `joinpath("sub")` | a new `Path` with `"sub"` appended |
| `with_suffix(s)` / `with_name(n)` | a new `Path` with the suffix / name replaced |
| `exists()` / `is_file()` / `is_dir()` / `is_symlink()` | `bool`, stat-backed |
| `is_absolute()` / `absolute()` | absolute? / resolve to a `Path` |
| `size()` | file size in bytes |
| `str(p)` | the underlying path string |

`pathlib.cwd()` returns the current directory as a `Path`:

```dragon
import pathlib

const here: pathlib.Path = pathlib.cwd()
print(here.is_absolute())   # True
```

> **Differs from Python.** The scope is POSIX-only - `PurePath` and
> `WindowsPath` are not provided.

## Environment variables

The dictionary-style entry point is `os.environ`: a real
`dict[str, str]` snapshot of the process environment, built once when the
`os` module is imported. It reads exactly like any other string-keyed
dict - bracket lookup, `in`, `len`, `.get(name, default)`, and iteration
over `.items()`:

```dragon
import os

home: str = os.environ["HOME"]
print(f"HOME = {home}")

level: str = os.environ.get("LOG_LEVEL", "info")
print(f"log level: {level}")   # the default when LOG_LEVEL is unset

print("HOME" in os.environ)    # True
print(f"{len(os.environ)} variables set")
```

Because it's a snapshot taken at import, `os.environ` reflects the
environment the process started with; mutating the live environment from
elsewhere does not retroactively change it. To *read* a single variable
without going through the dict, `os` also exposes functions. The raw
reader is `getenv`, which returns the value as a `str` - or the **empty
string** when the variable is unset (it never raises):

```dragon
from os import getenv

home: str = getenv("HOME")
print(f"HOME = {home}")

missing: str = getenv("DEFINITELY_NOT_SET")
print(len(missing))   # 0 - unset reads as ""
```

Because "unset" and "set to empty" both come back as `""`, reach for
`environ_get` when you want a fallback in one call - it's the moral
equivalent of Python's `os.environ.get(name, default)`:

```dragon
from os import environ_get

level: str = environ_get("LOG_LEVEL", "info")
print(f"log level: {level}")   # the default when LOG_LEVEL is unset
```

| Call | Python analogue |
|------|-----------------|
| `os.environ` | `os.environ` - a `dict[str, str]` snapshot |
| `getenv(name)` | `os.getenv(name, "")` |
| `environ_get(name, default)` | `os.environ.get(name, default)` |
| `setenv_val(name, value)` | `os.environ[name] = value` |
| `unsetenv_val(name)` | `del os.environ[name]` |

## The current working directory

`os.cwd()` returns the process's current working directory as a `str`
(Python's `os.getcwd()`, which Dragon also provides under that name):

```dragon
import os

print(os.cwd())
```

To expand `~` and `$VAR` references in a path, `os.path` has
`expanduser` and `expandvars`:

```dragon
from os.path import expanduser, expandvars

print(expanduser("~/projects"))          # /home/you/projects
print(expandvars("$HOME/.config"))       # /home/you/.config
```

## Command-line arguments

`sys.argv` is a **function** - `argv()` - that returns the process
arguments as a `list[str]`. As in C and Python, `argv()[0]` is the
program's own path; the real arguments start at index `1`:

```dragon
from sys import argv

args: list[str] = argv()
print(f"program: {args[0]}")
print(f"argument count: {len(args) - 1}")
for i in range(1, len(args)) {
    print(f"  arg {i}: {args[i]}")
}
```

> **`argv()` reflects the compiled binary's arguments.** Build the
> program (`dragon build prog.dr -o prog`) and run `./prog one two`, and
> you get `["./prog", "one", "two"]`. Under `dragon run prog.dr one
> two`, the trailing words are taken as *more files to compile*, not
> program arguments - so test argument handling against a built binary.

A typical argument check looks like any other Dragon code - it's just a
list:

```dragon
from sys import argv

args: list[str] = argv()
if len(args) < 2 {
    print("usage: greet <name>")
} else {
    print(f"Hello, {args[1]}!")
}
```

> For richer command-line work - flags, options, help text - and for
> launching *other* programs (`subprocess`), reaching the terminal
> (`getpass`), or inspecting the host (`platform`), see
> [Running and Managing Processes](/docs/1409-stdlib-processes).

## Standard output

You've used it in every example: `print`. It writes its arguments to
standard output, space-separated, with a trailing newline - the same
contract as Python's `print`. Multiple arguments, f-strings, and any
value with a string form all work:

```dragon
name: str = "Ada"
count: int = 3
print("plain text")
print("a", "b", "c")              # a b c
print(f"{name} has {count} items")
print([1, 2, 3])                  # [1, 2, 3]
```

> **Escapes vs. f-strings.** A backslash escape like `\n` is processed in
> a *plain* string (`"line\nline"` prints two lines) but **not** inside
> an f-string (`f"line\nline"` prints a literal `\n`). When you want a
> newline in formatted output, use a separate `print`, or
> `"\n".join(parts)` built outside the f-string.

## A worked example: a directory report

Pulling the pieces together - carve a scratch directory with `tempfile`,
fill it, then use `glob` to select the logs, `os.path.getsize` to total
their bytes, and `fnmatch` to flag the odd file out. This compiles and
runs as shown:

```dragon
import os
import tempfile
import glob
import fnmatch
import shutil
from io import make
from os.path import join, basename, getsize

const work: str = tempfile.mkdtemp("report-", "")
with make(join(work, "a.log")) as w { w.write("x\n") }
with make(join(work, "b.log")) as w { w.write("yy\n") }
with make(join(work, "notes.txt")) as w { w.write("ignore me\n") }

# glob just the logs, total their sizes
const logs: list[str] = glob.glob(join(work, "*.log"))
total: int = 0
for path in logs {
    const sz: int = getsize(path)
    total = total + sz
    print(f"{basename(path)}: {sz} bytes")   # a.log: 2 bytes / b.log: 3 bytes
}
print(f"total log bytes: {total}")           # total log bytes: 5

# fnmatch over a plain listing to find the non-logs
for name in os.listdir(work) {
    if fnmatch.fnmatch(name, "*.txt") {
        print(f"text file: {name}")          # text file: notes.txt
    }
}

shutil.rmtree(work)
```

## At a glance

| You want to... | Write |
|----------------|-------|
| Read a whole file | `content: str = open(path).text()` |
| Read into lines | `for line in open(path).lines() { ... }` |
| Handle a missing file | `try { print(open(path).text()) } except FileNotFoundError as e { ... }` |
| Guard a read | `if exists(path) { ... }` |
| Write / replace a file | `with make(path) as w { w.write(s) }` |
| Append to a file | `with push(path) as w { w.write(s) }` |
| Read raw bytes | `data: bytes = open(path).bytes()` |
| List a directory | `names: list[str] = listdir(dir)` |
| List with metadata | `for e in os.scandir(dir) { e.is_file() ... }` |
| Walk a tree | `os.walk_files(top)` / `os.walk_dirs(top)` |
| Make a directory chain | `os.makedirs(path)` |
| Build a path | `join(dir, name)` *(binary)* or `Path(dir) / name` |
| Split a path | `basename(p)`, `dirname(p)`, `splitext(p)` |
| Check a path | `exists(p)`, `isfile(p)`, `isdir(p)` |
| Copy / move / delete a tree | `shutil.copytree`, `shutil.move`, `shutil.rmtree` |
| Find an executable | `shutil.which("git")` |
| Interpret mode bits | `stat.S_ISDIR(m)`, `stat.filemode(m)` |
| Match paths on disk | `glob.glob("src/**/*.dr")` |
| Match one name | `fnmatch.fnmatch(name, "*.txt")` |
| Scratch dir / file | `tempfile.mkdtemp(p, "")` / `tempfile.mkstemp(p, s)` |
| Object-style path | `Path("/a/b").suffix()` *(call accessors with `()`)* |
| Read an env var | `os.environ[name]` / `getenv(name)` / `environ_get(name, default)` |
| Current directory | `os.cwd()` |
| Read CLI arguments | `args: list[str] = argv()` *(args from index 1)* |
| Print to stdout | `print(value)` |

The low-level wrappers are deliberately thin - each is a libc call with a
typed Dragon face on it, so an `os.path.exists` check or an
`open(path).text()` read costs the same as the C you'd write by hand - and the higher-level
modules (`shutil`, `glob`, `pathlib`) are plain Dragon built on top of
them, with no hidden machinery. Once your program has the *bytes* of a
file in hand, the next job is usually to make sense of them: split,
search, match, and reformat the text. That's the subject of the next
chapter, [Text Processing](/docs/1403-stdlib-text).
