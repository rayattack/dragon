# Command-Line Tools

Every language eventually has to answer the same humble question: how do
I write a program that takes arguments, does something, and tells the
shell whether it worked? In C you wade through `int main(int argc, char
**argv)` and parse `argv` by hand, or pull in `getopt`. Go gives you
`os.Args` and a `flag` package. Rust reaches for `clap`, a whole crate.
Python hands you `sys.argv` and the batteries-included `argparse`. The
mechanics differ, but the shape is identical everywhere: read the words
the user typed, interpret the flags, do the work, and exit with a status
code the shell can branch on.

Dragon has all of these pieces, and - true to the model you met in
[Modules and Packages](/docs/1001-modules) - it adds one twist that makes
CLI programs *simpler* than in any of those languages: **there is no
`main`.** The file you hand to `dragon build` *is* the program; its
top-level statements *are* the command-line tool. You don't declare an
entry point, register a handler, or guard with `if __name__ ==
"__main__"`. You write the steps top to bottom, build the file to a
binary, and run it. This chapter shows the whole arc - reading `argv`,
parsing flags with `argparse`, reading a line from stdin, and exiting
with a status code - and it ends with a real `grep`-shaped tool you can
build and run.

Two things up front are deliberate divergences from Python, and naming
them now will save you a debugging session later:

- **`sys.argv` is a function - `sys.argv()` - not an attribute.** You
  *call* it to get the `list[str]`.
- **`argparse` gives everything back as a `str`.** There is no typed
  coercion: you call `int(...)` / `float(...)` on the values yourself.

Both fall out of Dragon's typed, compiled model, and both are easy to
live with once you know them.

## The arguments are just a list: `sys.argv()`

The lowest-level way to read the command line is `sys.argv()`. It
returns a `list[str]`: index `0` is the program's own path, and the real
arguments follow from index `1` - exactly the C/Python convention.

```dragon
from sys import argv

args: list[str] = argv()
print("program: " + args[0])
print("got " + str(len(args) - 1) + " arguments")
for i in range(1, len(args)) {
    print("  arg " + str(i) + ": " + args[i])
}
```

Build it and run it with two arguments:

```bash
dragon build args.dr -o args
./args hello world
```

```
program: ./args
got 2 arguments
  arg 1: hello
  arg 2: world
```

> **`argv` is a call, not a field.** Python lets you write `sys.argv`
> and read it like a list. Dragon's `sys.argv` is a *function*; you must
> write `argv()` (or `sys.argv()`). A bare `sys.argv` without the
> parentheses is not the argument list - it's a reference to the
> function. This is the single most common porting slip from Python, so
> it's worth burning in.

> **Test against the built binary.** `argv()` reflects the arguments of
> the *compiled program*. If you run `dragon run prog.dr one two`, the
> trailing words `one two` are taken as *more files to compile*, not as
> your program's arguments. Build first (`dragon build prog.dr -o
> prog`), then run `./prog one two`. Every example in this chapter is
> built, then run.

For a one-flag throwaway script, reading the list directly is fine. The
moment you have more than a flag or two - defaults, a mix of positional
and optional arguments, a help string - reach for `argparse`.

## Parsing flags with `argparse`

`argparse` is the standard library's argument parser. Its shape mirrors
Python's: you create an `ArgumentParser`, declare each argument with
`add_argument`, then call `parse_args` on the argument list. Here is the
canonical first program - a greeter with a `--name` and a `--count`:

```dragon
from argparse import ArgumentParser
import sys

parser: ArgumentParser = ArgumentParser("greet", "Greet someone")
parser.add_argument("--name", "str", "world", "who to greet")
parser.add_argument("--count", "int", "1", "how many times")

args: dict[str, str] = parser.parse_args(sys.argv())
name: str = args["name"]
count: int = int(args["count"])
for i in range(count) {
    print("Hello, " + name + "!")
}
```

Build and run it:

```bash
dragon build greet.dr -o greet
./greet --name Ada --count 2
```

```
Hello, Ada!
Hello, Ada!
```

Run it with no arguments and the declared defaults take over:

```bash
./greet
```

```
Hello, world!
```

Walk through the four moving parts:

1. **`ArgumentParser(prog, description)`** - the program name and a
   one-line description, both `str`. (Both are stored for help text;
   automatic `--help` generation is not part of this subset.)
2. **`add_argument(name, arg_type, default, help)`** - declares one
   argument. Note the signature carefully: every parameter is a **string
   literal**.
   - `name` - `"--name"` for an optional argument, `"name"` (no dashes)
     for a positional one.
   - `arg_type` - a *string* naming the type: `"str"`, `"int"`, or
     `"bool"`. This is a label, not a Dragon type; the parser stores
     everything as text regardless.
   - `default` - the default value, **also as a string** (`"1"`, not
     `1`).
   - `help` - a help string for the argument.
3. **`parse_args(argv())`** - takes the `list[str]` from `sys.argv()`
   and returns a `dict[str, str]` keyed by the argument's canonical name
   (the leading dashes are stripped, so `--name` becomes the key
   `"name"`).
4. **Read the values out of the dict.** `args["name"]` is a `str`. For a
   numeric argument you convert at the use site: `int(args["count"])`.

> **Everything comes back as a `str` - you convert it.** Python's
> `argparse` lets you declare `type=int` and hands you an `int`. Dragon's
> stores types and defaults as strings and returns a uniform
> `dict[str, str]`; the `arg_type` you pass is metadata, not a coercion
> instruction. So a numeric flag is always read as `int(args["port"])`,
> a float as `float(args["rate"])`. This keeps the return type a single,
> honest `dict[str, str]` instead of a heterogeneous bag, and the
> conversion is one explicit call where you need the value.

Short options work too - declare `"-n"` and read it under the key
`"n"`:

```dragon
from argparse import ArgumentParser
import sys

parser: ArgumentParser = ArgumentParser("greet", "Greet someone")
parser.add_argument("-n", "str", "world", "who to greet")

args: dict[str, str] = parser.parse_args(sys.argv())
print("Hello, " + args["n"] + "!")
```

```bash
dragon build greet.dr -o greet
./greet -n Bob          # Hello, Bob!
```

### Positional arguments

Drop the dashes from the `name` and you've declared a **positional**
argument - one the user supplies by position, not by flag. Positionals
are filled left to right from the words that aren't flags:

```dragon
from argparse import ArgumentParser
import sys

parser: ArgumentParser = ArgumentParser("wc", "Count words in a string")
parser.add_argument("text", "str", "", "the text to count")
parser.add_argument("--verbose", "bool", "false", "print the words too")

args: dict[str, str] = parser.parse_args(sys.argv())
text: str = args["text"]
words: list[str] = text.split(" ")
print("word count: " + str(len(words)))

if args["verbose"] == "true" {
    for w in words {
        print("  " + w)
    }
}
```

```bash
dragon build wc.dr -o wc
./wc "the quick brown fox" --verbose
```

```
word count: 4
  the
  quick
  brown
  fox
```

The positional `text` swallowed the quoted string; the `--verbose` flag
was recognized as optional and didn't compete for the positional slot.

### Boolean flags

A `"bool"` optional argument is a **flag**: present-or-absent. Passing
`--verbose` sets its value to the literal string `"true"`; leaving it off
keeps the default. Because the value is still a `str`, you test it by
comparing against `"true"`:

```dragon
if args["verbose"] == "true" {
    # the flag was passed
}
```

There is no `--no-verbose`, no `store_false`, no value after the flag -
its presence *is* the signal, which is exactly how `grep -v` or `ls -l`
behave. (A bool flag does not consume the next word; `--verbose foo`
leaves `foo` free to fill a positional.)

## Exit codes: `sys.exit_code`

A command-line tool's return value is its **exit status** - the integer
the shell reads as `$?`. Zero means success; anything non-zero means
something went wrong, and conventions assign meaning (`1` general error,
`2` misuse). Dragon exits the process with `sys.exit_code(code)`.

> **The function is `exit_code`, not `exit`.** Python spells this
> `sys.exit(code)`. Dragon's `sys` module exposes it as
> `exit_code(code: int)` (the bare `exit` is the raw libc extern). Use
> `sys.exit_code(0)` for success, `sys.exit_code(1)` for failure. This
> is a naming divergence - the behavior is the same: it ends the process
> immediately with that status.

```dragon
from argparse import ArgumentParser
import sys

parser: ArgumentParser = ArgumentParser("wc", "Count words in a string")
parser.add_argument("text", "str", "", "the text to count")

args: dict[str, str] = parser.parse_args(sys.argv())
text: str = args["text"]

if len(text) == 0 {
    print("error: no text given")
    sys.exit_code(1)
}

words: list[str] = text.split(" ")
print("word count: " + str(len(words)))
sys.exit_code(0)
```

```bash
dragon build wc.dr -o wc
./wc "one two three" ; echo "exit=$?"
```

```
word count: 3
exit=0
```

And the error path:

```bash
./wc ; echo "exit=$?"
```

```
error: no text given
exit=1
```

You don't *have* to call `sys.exit_code` - a program that simply runs off
the end of its top-level statements exits `0`. But calling it explicitly
makes failure paths unambiguous, and it lets you bail out early from deep
inside a branch without restructuring the whole file. (You can also reach
for it from the `from sys import argv, exit_code` form and call
`exit_code(1)` bare - the import style is your choice, the behavior is
identical.)

## Reading from standard input: `input()`

The other half of a CLI's plumbing is **stdin** - the channel a user
types into, or that a pipe feeds. The `input()` builtin reads one line
from standard input and returns it as a `str` (the trailing newline
stripped). Pass an optional prompt string and it's written to stdout
first:

```dragon
print("What is your name?")
name: str = input("> ")
print("Hello, " + name + "!")
```

```bash
dragon build ask.dr -o ask
echo "Ada" | ./ask
```

```
What is your name?
> Hello, Ada!
```

The `Ada` arrived on stdin from the pipe; `input` returned it, and the
program greeted it. Run `./ask` interactively and you'd type the name at
the `>` prompt instead. This is the same `input()` you'd write in a
script - the difference from Python is only that Dragon's is a compiled,
typed call returning a `str`.

`input()` is the right tool for prompts and small line-oriented filters.
For reading whole files or streaming large inputs, use the `io` module's
`open` reader covered in
[Files and the Filesystem](/docs/1402-stdlib-io) - that chapter is
the companion to this one, owning the file and environment side of a
program's I/O while this chapter owns the *command-line* side.

## A worked example: a tiny `grep`

Here is the whole chapter in one program - a `grep`-shaped tool that
combines `argparse` (a pattern positional, a path positional, a
`--count` flag), file I/O guarded by `os.path.exists`, and meaningful
exit codes. It prints the lines of a file that contain a word, or - with
`--count` - just how many matched. This compiles and runs exactly as
shown:

```dragon
from argparse import ArgumentParser
from os.path import exists
from io import open
import sys

parser: ArgumentParser = ArgumentParser("dgrep", "Print lines containing a word")
parser.add_argument("pattern", "str", "", "the word to look for")
parser.add_argument("path", "str", "", "the file to search")
parser.add_argument("--count", "bool", "false", "print only the match count")

args: dict[str, str] = parser.parse_args(sys.argv())
pattern: str = args["pattern"]
path: str = args["path"]

if len(pattern) == 0 or len(path) == 0 {
    print("usage: dgrep <pattern> <path> [--count]")
    sys.exit_code(2)
}

if not exists(path) {
    print("dgrep: " + path + ": no such file")
    sys.exit_code(1)
}

lines: list[str] = open(path).lines()
matches: int = 0
count_only: str = args["count"]
for raw in lines {
    line: str = raw.strip()
    if pattern in line {
        matches = matches + 1
        if count_only != "true" {
            print(line)
        }
    }
}

if count_only == "true" {
    print(str(matches))
}

if matches == 0 {
    sys.exit_code(1)
}
sys.exit_code(0)
```

Build it, make a data file, and exercise every path:

```bash
dragon build dgrep.dr -o dgrep
printf 'apple pie\nbanana bread\napple tart\ncherry cake\n' > menu.txt

./dgrep apple menu.txt          ; echo "exit=$?"
./dgrep apple menu.txt --count  ; echo "exit=$?"
./dgrep zucchini menu.txt       ; echo "exit=$?"
./dgrep apple nope.txt          ; echo "exit=$?"
./dgrep                         ; echo "exit=$?"
```

```
apple pie
apple tart
exit=0
2
exit=0
exit=1
dgrep: nope.txt: no such file
exit=1
usage: dgrep <pattern> <path> [--count]
exit=2
```

Read the exit codes like a shell would: `0` when there were matches, `1`
when there were none (mirroring real `grep`) or the file was missing, and
`2` for misuse. A caller can chain on them - `./dgrep apple menu.txt &&
echo found` - because the tool reports its outcome the way the rest of
the Unix toolbox does. Notice there is no `main`, no argument-handler
registration: the file's top-level statements, run top to bottom, *are*
`dgrep`.

## Divergences from Python, collected

Because the differences are the part most likely to trip a Python
porter, here they are in one place:

| Concern | Python | Dragon |
|---------|--------|--------|
| Read the argument list | `sys.argv` (attribute) | `sys.argv()` (**function call**) |
| Exit with a status | `sys.exit(code)` | `sys.exit_code(code)` |
| `argparse` value types | typed (`type=int` → `int`) | always `str`; you call `int(...)` / `float(...)` |
| `parse_args` return | `Namespace` object, `args.name` | `dict[str, str]`, `args["name"]` |
| `add_argument` parameters | mixed types, keywords | four **string** positionals: `name, arg_type, default, help` |
| Entry point | `if __name__ == "__main__":` | none - the file *is* the program |

None of these costs you anything at runtime; each is the typed, compiled
shape of the same idea.

## At a glance

| You want to... | Write |
|----------------|-------|
| Read the raw argument list | `args: list[str] = argv()` *(args from index 1)* |
| Make a parser | `parser: ArgumentParser = ArgumentParser("prog", "desc")` |
| Declare an optional flag | `parser.add_argument("--name", "str", "default", "help")` |
| Declare a positional | `parser.add_argument("path", "str", "", "help")` |
| Declare a boolean flag | `parser.add_argument("--verbose", "bool", "false", "help")` |
| Parse | `args: dict[str, str] = parser.parse_args(argv())` |
| Read a string value | `name: str = args["name"]` |
| Read a number value | `count: int = int(args["count"])` |
| Test a boolean flag | `if args["verbose"] == "true" { ... }` |
| Read one line of stdin | `line: str = input("prompt> ")` |
| Exit successfully | `sys.exit_code(0)` |
| Exit with an error | `sys.exit_code(1)` *(or `2` for misuse)* |

The shape of a Dragon CLI is the shape of the file: declare your parser,
parse `argv()`, do the work, and exit with a status the shell can read.
No magic function announces the entry point - the file you built *is* the
tool. For the file, directory, and environment plumbing these programs
lean on, see [Files and the Filesystem](/docs/1402-stdlib-io); for
tools that fan out work across threads, see
[Concurrency](/docs/1101-green-threads).

