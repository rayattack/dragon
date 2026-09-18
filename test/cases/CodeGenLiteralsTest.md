# CodeGenLiteralsTest cases

Source programs for `test/CodeGenLiteralsTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :string_cmp_declared

```dr
a: str = "x"
b: str = "y"
c: bool = a < b
```

#### :f_string_format_spec_ir

```dr
x: float = 1.0
s: str = f"{x:.2f}"
```

#### :f_string_intermediate_decref

```dr
x: int = 1
y: int = 2
s: str = f"a {x} b {y} c"
```

#### :binary_string_concat_chain_decref

```dr
a: str = "x"
b: str = "y"
c: str = "z"
s: str = a + b + c
```

#### :f_string_arithmetic

```dr
x: int = 3
y: int = 4
print(f"{x + y}")
```

#### :f_string_function_call

```dr
nums: list[int] = [1, 2, 3]
print(f"len={len(nums)}")
```

#### :f_string_multiple_exprs

```dr
a: int = 10
b: int = 20
print(f"{a} + {b} = {a + b}")
```

#### :f_string_float_format

```dr
x: float = 3.14159
print(f"{x:.2f}")
```

#### :f_string_float_format3

```dr
pi: float = 3.14159265
print(f"{pi:.4f}")
```

#### :f_string_int_hex

```dr
x: int = 255
print(f"{x:x}")
```

#### :f_string_int_hex_upper

```dr
x: int = 255
print(f"{x:X}")
```

#### :f_string_int_octal

```dr
x: int = 8
print(f"{x:o}")
```

#### :f_string_int_binary

```dr
x: int = 10
print(f"{x:b}")
```

#### :f_string_int_zero_pad

```dr
x: int = 42
print(f"{x:05d}")
```

#### :f_string_mixed

```dr
name: str = "pi"
val: float = 3.14159
print(f"{name} = {val:.2f}")
```

#### :f_string_multi_interpolation

```dr
x: int = 1
y: int = 2
z: int = 3
print(f"{x} + {y} = {z}")
```

#### :f_string_rejects_percent_n

```dr
x: int = 42
try {
  print(f"{x:%n}")
} except ValueError {
  print("caught")
}
```

#### :f_string_rejects_percent_s

```dr
x: int = 42
try {
  print(f"{x:%s}")
} except ValueError {
  print("caught")
}
```

#### :f_string_long_all_zero_spec

```dr
x: int = 42
print(f"{x:00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000d}")
```

#### :f_string_rejects_huge_width

```dr
x: int = 42
try {
  print(f"{x:999999999d}")
} except ValueError {
  print("caught")
}
```

#### :f_string_rejects_float_percent_n

```dr
v: float = 3.14
try {
  print(f"{v:%n}")
} except ValueError {
  print("caught")
}
```

#### :f_string_valid_specs_still_work

```dr
v: float = 3.14159
x: int = 42
print(f"{v:.2f}")
print(f"{x:x}")
print(f"{x:X}")
print(f"{x:o}")
print(f"{x:b}")
print(f"{x:05d}")
```

#### :string_operations

```dr
s: str = "hello"
print(len(s))
t: str = s + " world"
print(t)
```

#### :string_indexing

```dr
s: str = "Dragon"
print(s[0])
print(s[-1])
```

#### :string_methods

```dr
s: str = "hello"
print(s.upper())
print(s.find("ll"))
```

#### :string_slice

```dr
s: str = "hello world"
print(s[0:5])
print(s[6:11])
```

#### :expr_stmt_string_method_no_leak

```dr
s: str = "hello"
s.upper()
print(s)
```

#### :string_concat_chain_e2_e

```dr
a: str = "hello"
b: str = " "
c: str = "world"
print(a + b + c)
```

#### :string_ordering

```dr
print("apple" < "banana")
print("cat" > "bat")
print("abc" <= "abc")
print("xyz" >= "xyz")
```

#### :string_ordering_variables

```dr
ch: str = "d"
lo: str = "a"
hi: str = "z"
if ch >= lo {
  if ch <= hi {
    print("in range")
  }
}
```

#### :list_str_subscript

```dr
names: list[str] = ["alice", "bob", "charlie"]
print(names[0])
print(names[1])
```

#### :list_float_subscript

```dr
vals: list[float] = [1.5, 2.3, 3.5]
x: float = vals[0]
y: float = vals[1]
print(x + y)
```

#### :bytes_concat_ir

```dr
a: bytes = b"hello"
b: bytes = b" world"
c: bytes = a + b
```

#### :bytes_len_ir

```dr
b: bytes = b"hello"
print(len(b))
```

#### :bytes_decode_ir

```dr
b: bytes = b"hello"
s: str = b.decode()
```

#### :str_encode_ir

```dr
s: str = "hello"
b: bytes = s.encode()
```

#### :bytes_len

```dr
b: bytes = b"hello"
print(len(b))
```

#### :bytes_index

```dr
b: bytes = b"hello"
print(b[0])
```

#### :bytes_neg_index

```dr
b: bytes = b"hello"
print(b[-1])
```

#### :bytes_slice

```dr
b: bytes = b"hello world"
print(b[0:5])
```

#### :bytes_concat

```dr
a: bytes = b"hello"
b: bytes = b" world"
print(a + b)
```

#### :bytes_repeat

```dr
b: bytes = b"ab"
print(b * 3)
```

#### :bytes_eq

```dr
a: bytes = b"abc"
b: bytes = b"abc"
if a == b {
    print("True")
} else {
    print("False")
}
```

#### :bytes_neq

```dr
a: bytes = b"abc"
b: bytes = b"def"
if a != b {
    print("True")
} else {
    print("False")
}
```

#### :bytes_lt

```dr
a: bytes = b"abc"
b: bytes = b"abd"
if a < b {
    print("True")
} else {
    print("False")
}
```

#### :bytes_contains_int

```dr
b: bytes = b"hello"
if 104 in b {
    print("True")
} else {
    print("False")
}
```

#### :bytes_decode

```dr
b: bytes = b"hello"
s: str = b.decode()
print(s)
```

#### :str_encode

```dr
s: str = "hello"
b: bytes = s.encode()
print(b)
```

#### :bytes_hex

```dr
b: bytes = b"hello"
print(b.hex())
```

#### :bytes_fromhex

```dr
b: bytes = bytes.fromhex("68656c6c6f")
print(b)
```

#### :bytes_find

```dr
b: bytes = b"hello"
print(b.find(b"ll"))
```

#### :bytes_rfind

```dr
b: bytes = b"hello hello"
print(b.rfind(b"hello"))
```

#### :bytes_count

```dr
b: bytes = b"abcabc"
print(b.count(b"abc"))
```

#### :bytes_replace

```dr
b: bytes = b"hello"
print(b.replace(b"l", b"r"))
```

#### :bytes_startswith

```dr
b: bytes = b"hello"
print(b.startswith(b"hel"))
```

#### :bytes_upper

```dr
b: bytes = b"hello"
print(b.upper())
```

#### :bytes_lower

```dr
b: bytes = b"HELLO"
print(b.lower())
```

#### :bytes_strip

```dr
b: bytes = b" hello "
print(b.strip())
```

#### :bytes_split

```dr
parts: list[bytes] = b"a,b,c".split(b",")
print(len(parts))
```

#### :concat_intermediate_upper_lower_loop

```dr
s: str = "AbCdE"
last: str = ""
for i in range(10000) {
  last = s.upper() + s.lower()
}
print(last)
```

#### :concat_intermediate_str_coercion_loop

```dr
last: str = ""
for i in range(10000) {
  last = str(i) + str(i + 1)
}
print(last)
```

#### :concat_intermediate_slice_loop

```dr
s: str = "abcdef"
last: str = ""
for i in range(10000) {
  last = s[0:3] + s[3:6]
}
print(last)
```

#### :concat_intermediate_triple

```dr
a: str = "AAA"
b: str = "BBB"
c: str = "xCx"
last: str = ""
for i in range(10000) {
  last = a.upper() + b.lower() + c.replace("x", "y")
}
print(last)
```

#### :concat_intermediate_literal_plus_str_plus_literal

```dr
last: str = ""
for i in range(10000) {
  last = "prefix-" + str(i) + "-suffix"
}
print(last)
```

#### :concat_broad_decref_upper_lower

```dr
s: str = "AbCdE"
r: str = s.upper() + s.lower()
```

#### :concat_broad_decref_int_to_str

```dr
x: int = 1
y: int = 2
r: str = str(x) + str(y)
```

#### :str_join_empty_list

```dr
xs: list[str] = []
r: str = ", ".join(xs)
print("[" + r + "]")
```

#### :str_join_single_element

```dr
xs: list[str] = ["only"]
r: str = ", ".join(xs)
print(r)
```

#### :str_join_all_empty_strings

```dr
xs: list[str] = ["", "", ""]
r: str = "|".join(xs)
print("[" + r + "]")
```

#### :str_join_mixed_empty

```dr
xs: list[str] = ["a", "", "b", "", "c"]
r: str = ",".join(xs)
print(r)
```

#### :str_join_empty_separator

```dr
xs: list[str] = ["foo", "bar", "baz"]
r: str = "".join(xs)
print(r)
```

#### :str_join_loop_bounded

```dr
xs: list[str] = ["alpha", "", "beta"]
last: str = ""
for i in range(5000) {
  last = "-".join(xs)
}
print(last)
```

#### :str_replace_shrink

```dr
s: str = "xxxxxxxx"
r: str = s.replace("xx", "y")
print(r)
```

#### :str_replace_expand

```dr
s: str = "abcabc"
r: str = s.replace("a", "AAA")
print(r)
```

#### :str_replace_equal_length

```dr
s: str = "hello world"
r: str = s.replace("o", "0")
print(r)
```

#### :str_replace_no_match

```dr
s: str = "hello"
r: str = s.replace("z", "Q")
print(r)
```

#### :str_replace_shrink_to_empty

```dr
s: str = "a-b-c-d"
r: str = s.replace("-", "")
print(r)
```

#### :str_replace_full_string

```dr
s: str = "foobar"
r: str = s.replace("foobar", "")
print("[" + r + "]")
```

#### :utf8_len_is_code_point_count

```dr
a: str = "hello"
b: str = "café"
c: str = "héllo wörld"
d: str = "日本語"
print(len(a))
print(len(b))
print(len(c))
print(len(d))
```

#### :utf8_indexing_by_code_point

```dr
s: str = "café"
print(s[0])
print(s[1])
print(s[2])
print(s[3])
print(s[-1])
```

#### :utf8_indexing_multi_byte_only_string

```dr
s: str = "日本語"
print(s[0])
print(s[1])
print(s[2])
```

#### :utf8_slice_preserves_valid_encoding

```dr
s: str = "héllo wörld"
print(s[0:5])
print(s[6:11])
print(s[1:3])
```

#### :utf8_concat_mixed_kind

```dr
a: str = "hello "
b: str = "wörld"
print(a + b)
print("prefix " + b + " suffix")
```

#### :utf8_concat_canonical_downgrade

```dr
s: str = "héllo"
r: str = s.replace("é", "e")
print(r)
print(len(r))
print(r[1])
```

#### :utf8_find_and_contains

```dr
s: str = "héllo wörld"
print(s.find("wörld"))
print(s.find("xyz"))
if "wörld" in s {
    print("yes")
}
if "missing" in s {
    print("x")
} else {
    print("no")
}
```

#### :utf8_startswith_endswith

```dr
s: str = "héllo wörld"
if s.startswith("héllo") {
    print("sp")
}
if s.endswith("wörld") {
    print("ep")
}
if s.startswith("goodbye") {
    print("x")
} else {
    print("sn")
}
```

#### :utf8_replace

```dr
s: str = "café au lait"
print(s.replace("é", "e"))
print(s.replace("au lait", "noir"))
print(s.replace("café", "thé"))
```

#### :utf8_dict_keys

```dr
d: dict[str, int] = {"café": 1, "tea": 2, "日本": 3}
print(d["café"])
print(d["tea"])
print(d["日本"])
h1: bool = "café" in d
h2: bool = "missing" in d
print(h1)
print(h2)
```

#### :utf8_print_round_trip

```dr
print("café")
print("日本語")
print("αβγ")
```

#### :utf8_ascii_upper_lower_still_work

```dr
print("Hello".upper())
print("Hello".lower())
```

#### :utf8_case_maps_latin1

```dr
s: str = "héllo wörld"
print(s.upper())
```

#### :utf8_byte_len_pub_returns_wire_bytes

```dr
extern "C" def dragon_str_byte_len_pub(s: str) -> int
a: str = "café"
b: str = "日本語"
c: str = "hello"
print(dragon_str_byte_len_pub(a))
print(dragon_str_byte_len_pub(b))
print(dragon_str_byte_len_pub(c))
```

#### :print_multi_arg_variables

```dr
a: int = 10
b: str = "hi"
print(a, b)
```

#### :print_multi_arg_with_list

```dr
xs: list[int] = [1, 2, 3]
print("xs:", xs)
```

#### :static_builtin_results_are_typed

A builtin whose result is an owned heap value must carry a static type, or the
release codegen would emit is skipped. These call it in a temp position, where
nothing but the call's own type can classify the result.

```dr
from collections import deque
xs: list[int] = [1, 2]
ss: list[str] = ["a", "b"]
print(str(len(bytes.fromhex("dead"))))
print(str(len(dict.fromkeys(ss, 1))))
print(str(len(set(xs))))
print(str(len(set())))
print(str(len(deque(xs))))
print(str(len(dict.fromkeys(ss, 0))))
```


#### :truncated_u_escape_is_rejected

A `\u` escape needs exactly four hex digits; three must not silently become text.

```dr
print("caf\u00es")
```

#### :out_of_range_big_u_escape_is_rejected

`\U00110000` is past the last Unicode scalar value.

```dr
print("\U00110000")
```

#### :surrogate_escape_is_rejected

A lone surrogate is not a scalar value and cannot be encoded as UTF-8.

```dr
print("\ud800")
```

#### :truncated_x_escape_is_rejected

```dr
print("\x4")
```

#### :named_escape_is_rejected

```dr
print("\N{DRAGON}")
```

#### :truncated_x_escape_in_bytes_is_rejected

```dr
print(len(b"\xf"))
```
