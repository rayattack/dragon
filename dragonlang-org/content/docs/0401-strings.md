# Strings and Unicode

A string is where languages quietly reveal what they think text *is*. C says a
string is a byte array with a `NUL` on the end - fast, and wrong the moment a
character needs more than one byte. Rust splits the world into `String` and
`&str` and makes you manage the boundary. Python decided a string is a sequence
of Unicode **code points**, immutable, and gave it a generous method set.

Dragon follows Python here without reservation: one `str` type, Unicode
code-point based, immutable, with the methods you already know. `len` counts code
points, not bytes; indexing gives you a one-character string; and the whole
familiar toolkit - `upper`, `split`, `strip`, `replace`, `find` - is present and
behaves as you expect. The byte-level view, when you need it, is a separate type
covered in [Bytes and Encoding](/docs/0403-bytes).

## Literals

Strings are written with single or double quotes, and triple quotes for text
that spans lines. Escape sequences (`\n`, `\t`, `\\`, …) work inside all of them:

```dragon
const a: str = "double"
const b: str = 'single'
const c: str = """a
multi-line
string"""
const tabbed: str = "col1\tcol2"
```

## Length, indexing, and slicing

Index from the front with `0`-based positions, from the back with negatives, and
slice with `[start:stop]` (stop exclusive) or `[start:stop:step]` - a step of
`-1` reverses:

```dragon
const name: str = "Dragon"
print(len(name))     # 6
print(name[0])       # D
print(name[-1])      # n
print(name[:3])      # Dra
print(name[3:])      # gon
print(name[1:4])     # rag
print(name[::2])     # Dao   - every second character
print(name[::-1])    # nogarD - reversed
```

## Unicode is the unit

`len` and indexing work in **code points**, not bytes, so accented and non-Latin
text behaves intuitively. `café` is four characters, and `word[3]` is the `é`:

```dragon
const word: str = "café"
print(len(word))     # 4   - code points, not the 5 UTF-8 bytes
print(word[3])       # é
print(ord("é"))      # 233
print(chr(233))      # é
```

The contrast with the byte count - `len("café".encode())` is `5` - is exactly the
text/bytes boundary that [Bytes and Encoding](/docs/0403-bytes) is about.

## Concatenation and repetition

`+` joins strings and `*` repeats them (in either order):

```dragon
print("Dr" + "agon")   # Dragon
print("ab" * 3)        # ababab
print(3 * "xy")        # xyxyxy
```

Strings are immutable: every method that "changes" a string returns a new one and
leaves the original untouched.

## Case

```dragon
const s: str = "Hello, World"
print(s.upper())               # HELLO, WORLD
print(s.lower())               # hello, world
print("hello world".title())   # Hello World
```

## Trimming

`strip` with no argument removes leading and trailing whitespace; with a string
argument it removes any of those *characters* from the ends. `lstrip` and
`rstrip` trim only one side:

```dragon
print("  hi  ".strip())     # hi
print("xxhixx".strip("x"))  # hi
print("xxhi".lstrip("x"))   # hi
print("hixx".rstrip("x"))   # hi
```

## Searching

`find` returns the index of the first match, or `-1` if there is none - which
makes it the safe way to search (test the `-1` rather than handle an error).
`count`, `startswith`, `endswith`, and the `in` operator round out the set:

```dragon
const s: str = "Hello, World"
print(s.find("World"))        # 7
print(s.rfind("l"))           # 10   - last match
print(s.find("xyz"))          # -1   - not found
print(s.count("o"))           # 2
print(s.startswith("Hello"))  # True
print(s.endswith("World"))    # True
print("rag" in "Dragon")      # True - substring test
```

## Splitting and joining

`split` breaks a string into a `list[str]`; an optional second argument caps the
number of splits, and `rsplit` counts from the right. `split` with no argument
splits on runs of whitespace. `join` is the inverse, and `partition` splits once
into a three-part tuple:

```dragon
print("a,b,c".split(","))          # ['a', 'b', 'c']
print("a,b,c".split(",", 1))       # ['a', 'b,c']   - at most one split
print("a,b,c".rsplit(",", 1))      # ['a,b', 'c']   - from the right
print("hello world  foo".split())  # ['hello', 'world', 'foo']
print("a\nb\nc".splitlines())      # ['a', 'b', 'c']
print("-".join(["a", "b", "c"]))   # a-b-c
print("key=val".partition("="))    # ('key', '=', 'val')
```

`split` produces a [list](/docs/0501-lists); `join` consumes one.

## Replacing

`replace` swaps every occurrence, or the first *n* if you pass a count:

```dragon
print("Hello".replace("l", "L"))     # HeLLo
print("a-b-c".replace("-", "_", 1))  # a_b-c   - only the first
```

## Padding

`rjust`, `ljust`, and `center` pad to a width with a fill character; `zfill`
zero-pads a numeric string:

```dragon
print("abc".rjust(6, "."))   # ...abc
print("abc".center(7, "*"))  # **abc**
print("42".zfill(5))         # 00042
```

For richer alignment and numeric formatting, an f-string format spec is usually
the better tool - see [F-strings and the Format Mini-Language](/docs/0402-formatting).

## Predicates

The `is…` family asks yes/no questions about a string's content:

```dragon
print("Hello".isalpha())   # True
print("123".isdigit())     # True
print("a1".isalnum())      # True
print("   ".isspace())     # True
print("HELLO".isupper())   # True
print("hello".islower())   # True
```

## Iterating

A `for` over a string yields its characters, one at a time, each a one-character
`str`:

```dragon
for ch in "abc" {
    print(ch)
}
# a
# b
# c
```

## At a glance

| You want to... | Write |
|----------------|-------|
| Length (in code points) | `len(s)` |
| A character / a slice | `s[i]`, `s[a:b]`, `s[::-1]` |
| Concatenate / repeat | `s + t`, `s * 3` |
| Upper / lower / title | `s.upper()`, `s.lower()`, `s.title()` |
| Trim | `s.strip()`, `s.strip("x")`, `s.lstrip(...)` |
| Find (or `-1`) | `s.find(sub)`, `s.rfind(sub)` |
| Membership | `sub in s` |
| Split / join | `s.split(",")`, `"-".join(parts)` |
| Replace | `s.replace(old, new)`, `s.replace(old, new, 1)` |
| Pad | `s.rjust(n, ".")`, `s.zfill(n)` |

Next, the other half of working with text: building strings out of values, with
[F-strings and the Format Mini-Language](/docs/0402-formatting).
