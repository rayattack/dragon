# CodeGenTemplateTest cases

Source programs for `test/CodeGenTemplateTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :template_literal_only

```dr
x: str = template {hello world}
print(x)
```

#### :template_int_interpolation

```dr
x: int = 42
print(template {value is !{x}})
```

#### :template_string_interpolation

```dr
name: str = "World"
print(template {Hello !{name}!})
```

#### :template_multiple_exprs

```dr
a: int = 10
b: int = 20
print(template {!{a} + !{b} = !{a + b}})
```

#### :template_balanced_braces

```dr
name: str = "test"
print(template {{"key": "!{name}"}})
```

#### :template_float_interpolation

```dr
x: float = 3.14
print(template {pi=!{x}})
```

#### :template_bool_interpolation

```dr
x: bool = True
print(template {flag=!{x}})
```

#### :template_pipe_html

```dr
s: str = "<b>hello</b>"
print(template {!{s | html}})
```

#### :template_pipe_html_ampersand

```dr
s: str = "a&b"
print(template {!{s | html}})
```

#### :template_pipe_html_quotes

```dr
s: str = "a'b"
print(template {!{s | html}})
```

#### :template_pipe_sql

```dr
name: str = "O'Brien"
print(template {WHERE name = '!{name | sql}'})
```

#### :template_pipe_url

```dr
q: str = "hello world"
print(template {?q=!{q | url}})
```

#### :template_pipe_url_special_chars

```dr
s: str = "a+b=c&d"
print(template {!{s | url}})
```

#### :template_pipe_user_defined

```dr
def exclaim(s: str) -> str {
    return s + "!!!"
}

name: str = "World"
print(template {Hello !{name | exclaim}})
```

#### :template_pipe_mixed

```dr
user: str = "<admin>"
count: int = 5
print(template {User: !{user | html}, count: !{count}})
```

#### :template_pipe_int_to_html

```dr
x: int = 42
print(template {!{x | html}})
```

#### :template_filter_decrefs_owned_int_input

```dr
n: int = 42
y: str = template {!{n | html}}
```

#### :template_filter_decrefs_owned_method_call_input

```dr
s: str = "hi"
y: str = template {!{s.upper() | html}}
```

#### :template_filter_borrowed_input_not_double_decref

```dr
s: str = "<b>x</b>"
y: str = template {!{s | html}}
```

#### :template_filter_bounded_loop_owned_input

```dr
last: str = ""
for i in range(20000) {
  last = template {!{str(i) | html}}
}
print(last)
```

#### :template_filter_bounded_loop_method_chain

```dr
s: str = "<value>"
last: str = ""
for i in range(10000) {
  last = template {!{s.upper() | html}}
}
print(last)
```

#### :template_filter_bounded_loop_user_filter

```dr
def shout(s: str) -> str { return s.upper() }
last: str = ""
for i in range(10000) {
  last = template {!{str(i) | shout}}
}
print(last)
```

#### :template_filter_correctness

```dr
n: int = 5
y: str = template {value=!{n | html}}
print(y)
```

#### :template_block_for_loop_with_content_alias

```dr
items: list[str] = ["a", "b", "c"]
y: str = template {<ul>!{ for x in items { :{<li>!{x}</li>} } }</ul>}
print(y)
```

#### :template_block_if_else_with_content_alias

```dr
logged_in: bool = True
name: str = "Ada"
y: str = template {!{ if logged_in { :{Hi !{name}} } else { :{Please sign in} } }}
print(y)
```

#### :template_block_multi_statement

```dr
items: list[str] = ["x", "y"]
y: str = template {!{
    label: str = "item"
    for x in items { :{[!{label}=!{x}]} }
}}
print(y)
```

#### :template_block_empty_loop

```dr
items: list[str] = []
y: str = template {[!{ for x in items { :{!{x}} } }]}
print(y)
```

#### :template_block_emits_list_and_join_ir

```dr
items: list[str] = ["a"]
y: str = template {!{ for x in items { :{!{x}} } }}
```

#### :template_spread_operator

```dr
items: list[str] = ["foo", "bar", "baz"]
y: str = template {[!{*items}]}
print(y)
```

#### :template_join_filter_empty

```dr
items: list[str] = ["a", "b", "c"]
y: str = template {!{items | join}}
print(y)
```

#### :template_join_filter_with_separator

```dr
items: list[str] = ["a", "b", "c"]
y: str = template {!{items | join(", ")}}
print(y)
```

#### :template_join_filter_with_expr_separator

```dr
items: list[str] = ["x", "y"]
sep: str = " | "
y: str = template {!{items | join(sep)}}
print(y)
```

#### :template_spread_desugars_to_join_ir

```dr
items: list[str] = ["a"]
y: str = template {!{*items}}
```

#### :template_block_nested_loops

```dr
rows: list[str] = ["r1", "r2"]
cols: list[str] = ["c1", "c2"]
y: str = template {!{
  for r in rows {
    :{[!{r}:}
    for c in cols {
      :{!{c},}
    }
    :{]}
  }
}}
print(y)
```

#### :template_block_spread_inside_block

```dr
y: str = template {!{
  parts: list[str] = []
  for i in range(3) {
    parts.append(str(i))
  }
  :{!{*parts}}
}}
print(y)
```
