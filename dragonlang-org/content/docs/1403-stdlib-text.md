# Text Processing

Most real programs spend more time shaping text than computing
numbers: matching a pattern, padding a column, wrapping a paragraph to
a terminal width, or guessing which command the user meant to type.
Dragon's standard library ships four focused modules for that work:

- **`re`** - regular expressions, backed by bundled PCRE2 (10.44).
- **`string`** - character-class constants and a handful of padding and
  case helpers.
- **`textwrap`** - wrap, fill, indent, dedent, and shorten paragraphs.
- **`difflib`** - string similarity and "did you mean…?" suggestions.

These are pure-Dragon modules (`string`, `textwrap`, `difflib`) or thin
typed wrappers over a statically-linked C library (`re` over PCRE2).
None of them carries a runtime dependency you have to install. The
*names* track Python where it costs nothing, but several of these APIs
diverge from Python in shape - most importantly `re`, which has no
`Match` object. Read each section's "differs from Python" note before
you port code over.

## Regular expressions: `re`

Dragon's `re` does **not** return a Python `Match` object. There is no
`.group()`, `.span()`, or `.groups()` on a result. Instead the module
exposes two things: a compiled **`Pattern`** class with methods, and a
set of module-level convenience functions that compile, run, and free a
pattern in one call. The return types are plain Dragon values - `int`,
`str`, and `list[str]` - chosen so the common cases need no wrapper.

### The convenience functions

For one-shot work, call the module functions directly. Each compiles
the pattern, runs it, and frees it for you:

```dragon
import re

print(re.match("h.llo", "hello world"))    # 1
print(re.search("[0-9]+", "order 42 ready"))   # 42
print(re.findall("[0-9]+", "1 a 22 b 333"))    # ['1', '22', '333']
print(re.split(",", "a,b,c"))               # ['a', 'b', 'c']
print(re.sub("[0-9]+", "#", "a1b22c"))      # a#b#c
```

The signatures are:

- `re.match(pattern: str, subject: str) -> int` - anchored match at the
  start of `subject`. Returns the PCRE2 group count (`1` for a match
  with no captures, `n+1` with `n` captures), or `-1` on no match. It
  is a count, **not** a boolean and **not** a `Match` - test it with
  `>= 1`.
- `re.search(pattern: str, subject: str) -> str` - the first matching
  substring anywhere in `subject`, or `""` if there is no match.
- `re.findall(pattern: str, subject: str) -> list[str]` - every
  non-overlapping whole match, left to right.
- `re.split(pattern: str, subject: str) -> list[str]` - `subject` cut
  on every match.
- `re.sub(pattern: str, replacement: str, subject: str) -> str` -
  every match replaced. The replacement supports backreferences.

A non-match is two different sentinels depending on which function you
called - `match` returns a negative `int`, `search` returns an empty
`str`:

```dragon
import re

print(re.match("zzz", "abc"))            # -1  (no match)
print(re.match("a", "abc"))              # 1   (matched, no groups)
print(f"[{re.search('zzz', 'abc')}]")    # []  (no match -> "")
```

Matching is case-sensitive by default - `re.search("ABC", "abc")`
returns `""`.

### Compiling a `Pattern`

When you run the same pattern many times, compile it once with
`re.compile` and reuse the `Pattern`. Capture groups are read through
`Pattern.group`, which takes the subject and a group index (group `0`
is the whole match):

```dragon
import re

const p: re.Pattern = re.compile("([a-z]+)=([0-9]+)")
print(p.match("port=8080"))         # 3  (whole match + 2 groups)
print(p.find("the port=8080 ok"))   # port=8080  (whole match text)
print(p.group("port=8080", 1))      # port  (first capture)
print(p.group("port=8080", 2))      # 8080  (second capture)
p.destroy()
```

The `Pattern` methods are:

- `p.match(subject) -> int` - same count/-1 semantics as the module
  `re.match`.
- `p.find(subject) -> str` - the whole matched text (the module
  `re.search` is a wrapper over this), `""` on no match.
- `p.group(subject, index: int) -> str` - the text of capture group
  `index` for the first match.
- `p.findall(subject) -> list[str]`, `p.split(subject) -> list[str]`,
  `p.sub(replacement, subject) -> str` - as the module functions.
- `p.destroy()` - frees the underlying PCRE2 code. Call it when you are
  done with a long-lived compiled pattern.

### Replacement with backreferences

`re.sub` (and `Pattern.sub`) expands numbered backreferences in the
replacement template: `\1` through `\9`, and the `\g<N>` form. `\\`
collapses to a single backslash. This lets you reorder captured text:

```dragon
import re

const p: re.Pattern = re.compile("([a-z]+)=([0-9]+)")
print(p.sub("\\2:\\1", "port=8080"))   # 8080:port
p.destroy()
```

Named groups (`\g<name>`) are **not** supported - PCRE2's name-to-number
lookup is not exposed through the current externs, so only numeric group
references expand.

> **Differs from Python.** There is no `Match` object anywhere in this
> module. `re.match` returns an `int` group-count (test `>= 1`), not a
> truthy/`None` `Match`. `re.search` returns the matched **string**, not
> a `Match` - read captures with a compiled `Pattern.group(subject, n)`.
> `re.findall` always returns the **whole** match even when the pattern
> has capture groups (Python returns the captured groups instead), so
> `re.findall("(a)(b)", "abab")` is `['ab', 'ab']`. There are no
> `flags=` arguments - matching is always case-sensitive, single-line.

## String constants and helpers: `string`

The `string` module is two things: a set of character-class constants,
and a few free functions for padding, casing, and classification. It is
pure Dragon with no C dependency.

The constants mirror Python's `string` module exactly:

```dragon
import string

print(string.ascii_letters)   # abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ
print(string.digits)          # 0123456789
print(string.hexdigits)       # 0123456789abcdefABCDEF
print(string.punctuation)     # !"#$%&'()*+,-./:;<=>?@[\]^_`{|}~
```

The full set is `ascii_lowercase`, `ascii_uppercase`, `ascii_letters`,
`digits`, `hexdigits`, `octdigits`, `punctuation`, `whitespace`, and
`printable`. Each is a `const str`.

The helper functions cover word-casing, padding, and classification:

```dragon
import string

print(string.capwords("the quick brown fox"))   # The Quick Brown Fox
print(string.center("hi", 8, "*"))              # ***hi***
print(string.ljust("hi", 6, "."))               # hi....
print(string.rjust("hi", 6, "."))               # ....hi
print(string.zfill("42", 6))                    # 000042
print(string.repeat("ab", 3))                   # ababab
print(string.is_numeric("123"))                 # True
```

Their signatures:

- `capwords(s: str) -> str` - capitalize each space-delimited word.
- `center(s, width: int, fillchar: str) -> str`,
  `ljust(s, width, fillchar) -> str`,
  `rjust(s, width, fillchar) -> str` - pad to `width`. Unlike the `str`
  methods, `fillchar` is **required** (positional, no default).
- `zfill(s: str, width: int) -> str` - left-pad with `"0"`.
- `repeat(s: str, n: int) -> str` - `s` concatenated `n` times.
- `is_numeric(s) -> bool`, `is_alpha(s) -> bool`, `is_alnum(s) -> bool`,
  `is_space(s) -> bool` - thin wrappers over the matching `str` methods.

> **Differs from Python.** The padding helpers are free functions taking
> the string as their first argument, and `fillchar` is mandatory
> (Python's `str.center(width)` defaults to a space and is a method).
> `repeat`, `is_numeric`, `is_alpha`, `is_alnum`, and `is_space` are
> Dragon additions - Python writes these as `s * n` and `s.isdigit()`,
> which also work in Dragon directly.

## Wrapping paragraphs: `textwrap`

`textwrap` reflows runs of text to a column width. It provides the
module-level shortcuts `wrap`, `fill`, `dedent`, `indent`, and
`shorten`, plus the `TextWrapper` class when you need to set options
once and reuse them.

`wrap` returns a `list[str]` of lines; `fill` returns those lines joined
with newlines:

```dragon
import textwrap

const text: str = "Dragon is a typed compiled language inspired by Python targeting LLVM"
for ln in textwrap.wrap(text, 20) {
    print(ln)
}
# Dragon is a typed
# compiled language
# inspired by Python
# targeting LLVM

print(textwrap.fill(text, 30))
# Dragon is a typed compiled
# language inspired by Python
# targeting LLVM
```

`dedent` strips the longest run of leading whitespace common to every
non-empty line - the standard move for un-indenting a here-doc-style
block. `indent` adds a prefix to every non-empty line. `shorten`
collapses whitespace and truncates to a width, walking back to a word
boundary and appending a placeholder:

```dragon
import textwrap

const doc: str = "    line one\n    line two\n    line three\n"
print(textwrap.dedent(doc))
# line one
# line two
# line three

print(textwrap.indent("a\nb\n", "> "))
# > a
# > b

print(textwrap.shorten("the quick brown fox jumps over", 18))   # the quick [...]
```

Their signatures:

- `wrap(text: str, width: int = 70) -> list[str]`
- `fill(text: str, width: int = 70) -> str`
- `dedent(text: str) -> str`
- `indent(text: str, prefix: str) -> str`
- `shorten(text: str, width: int, placeholder: str = " [...]") -> str`

For control over indentation, long-word breaking, or a line cap, build a
`TextWrapper`. Its constructor takes keyword arguments with Python's
names - `width`, `initial_indent`, `subsequent_indent`,
`break_long_words`, `max_lines`, `placeholder`, and more:

```dragon
from textwrap import TextWrapper

const tw: TextWrapper = TextWrapper(width=24,
                                    initial_indent="* ",
                                    subsequent_indent="  ")
for ln in tw.wrap("one two three four five six seven") {
    print(f"[{ln}]")
}
# [* one two three four]
# [  five six seven]
```

`tw.wrap(text)` and `tw.fill(text)` are the instance counterparts of the
module functions.

> **Differs from Python.** `indent`'s `prefix` is positional and
> required, and it has no `predicate` argument - the prefix is added to
> every non-empty line. The advanced `TextWrapper` flags
> (`fix_sentence_endings`, `break_on_hyphens`) exist for API
> compatibility but use a deliberately simple splitter, not CPython's
> regex-driven one, so corner-case line breaks may differ.

## Fuzzy matching: `difflib`

`difflib` answers two questions: *how similar are two strings?* and
*which of these candidates is closest to what I typed?* It implements
the Ratcliff-Obershelp similarity that CPython's `SequenceMatcher` uses,
so `ratio` scores match CPython for strings.

`ratio(a, b)` returns a similarity in `[0.0, 1.0]`:

```dragon
import difflib

print(difflib.ratio("kitten", "sitting"))   # 0.6153846153846154
print(difflib.ratio("abc", "abc"))          # 1.0
```

`get_close_matches` ranks candidates by that ratio and returns the best
ones, useful for "did you mean…?" diagnostics in a CLI:

```dragon
import difflib

const words: list[str] = ["apple", "apply", "ape", "orange"]
print(difflib.get_close_matches("appel", words))      # ['apple', 'apply', 'ape']
print(difflib.get_close_matches("appel", words, 1))   # ['apple']
print(difflib.get_close_matches("zzz", words))        # []
```

The signatures are:

- `ratio(a: str, b: str) -> float`
- `get_close_matches(word: str, possibilities: list[str], n: int = 3,
  cutoff: float = 0.6) -> list[str]` - up to `n` matches with
  `ratio >= cutoff`, best first.

> **Differs from Python.** `difflib` here is the **string** subset. It
> operates on `str` only; there is no `SequenceMatcher` class and no
> line-oriented `unified_diff`/`ndiff` (those await Dragon's generic
> sequences). `ratio` and `get_close_matches` are the surface.

## At a glance

| You want to... | Write |
|----------------|-------|
| Test a pattern at the start | `if re.match(pat, s) >= 1 { ... }` |
| Find the first match's text | `hit: str = re.search(pat, s)` |
| Find every match | `all: list[str] = re.findall(pat, s)` |
| Replace matches | `out: str = re.sub(pat, repl, s)` |
| Reuse a compiled pattern | `p: re.Pattern = re.compile(pat)` |
| Read a capture group | `g: str = p.group(s, 1)` |
| Reorder with backrefs | `p.sub("\\2:\\1", s)` |
| Character-class constants | `string.digits`, `string.ascii_letters` |
| Pad / justify | `string.center(s, w, "*")`, `string.zfill(s, w)` |
| Title-case words | `string.capwords(s)` |
| Wrap to a width | `lines: list[str] = textwrap.wrap(s, 70)` |
| Wrap into one string | `textwrap.fill(s, 70)` |
| Un-indent a block | `textwrap.dedent(s)` |
| Prefix every line | `textwrap.indent(s, "> ")` |
| Truncate with ellipsis | `textwrap.shorten(s, 40)` |
| String similarity | `r: float = difflib.ratio(a, b)` |
| "Did you mean…?" | `difflib.get_close_matches(word, options)` |

These four modules cover the everyday text work - matching, padding,
wrapping, and fuzzy-matching - that sits between raw `str` methods and a
full document format. When the text you are handling is structured data
rather than prose - JSON, CSV, TOML, INI - reach for the parsing modules
in the next chapter, [Data Formats](/docs/1404-stdlib-data).
