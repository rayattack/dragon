; Dragon language highlights for Zed (Tree-sitter queries)

; ─── Comments ────────────────────────────────────────────────
(comment) @comment

; ─── Strings ─────────────────────────────────────────────────
(simple_string) @string
(raw_string) @string
(byte_string) @string
(fstring) @string
(fstring_escape) @string.escape
(interpolation
  "{" @punctuation.special
  "}" @punctuation.special)

; ─── Numbers ─────────────────────────────────────────────────
(integer) @number
(float) @number

; ─── Constants ───────────────────────────────────────────────
(true) @constant.builtin
(false) @constant.builtin
(none) @constant.builtin
(ellipsis) @constant.builtin

; ─── Keywords ────────────────────────────────────────────────

; Control flow keywords (anonymous tokens in grammar)
[
  "if"
  "elif"
  "else"
  "while"
  "for"
  "in"
  "return"
  "yield"
  "raise"
  "try"
  "catch"
  "except"
  "finally"
  "with"
  "match"
  "case"
] @keyword

; Statement keywords (named nodes - entire statement is the keyword)
(pass_statement) @keyword
(break_statement) @keyword
(continue_statement) @keyword

; Definitions
[
  "def"
  "class"
  "lambda"
] @keyword

; Import
[
  "import"
  "from"
  "as"
] @keyword

; Import module path
(import_statement
  (dotted_name (identifier) @module))
(import_from_statement
  (dotted_name (identifier) @module))

; Imported names
(import_from_statement
  (identifier) @type)

; Concurrency
[
  "fire"
  "thread"
  "await"
] @keyword

; Logical operators
[
  "and"
  "or"
  "not"
  "is"
] @keyword.operator

; Other keywords
[
  "global"
  "nonlocal"
  "assert"
  "del"
  "type"
] @keyword

; Storage modifiers
[
  "const"
  "static"
  "extern"
] @keyword.modifier

; ─── Ownership modifiers ─────────────────────────────────────
; `own` and `dub` are contextual keywords (plain identifiers to the grammar),
; so match them by text - the same mechanism used for builtins below. `del` is
; a real keyword, handled above. This is inert until the tree-sitter grammar
; parses these positions; it never breaks query compilation (references only
; `identifier`).
((identifier) @keyword.modifier
  (#match? @keyword.modifier "^(own|dub)$"))

; ─── Functions ───────────────────────────────────────────────

; Function definition
(function_definition
  name: (identifier) @function)

; Extern function declaration
(extern_function_decl
  name: (identifier) @function)

; Extern declaration (single line)
(extern_declaration
  name: (identifier) @function)

; Self constructor
(self_constructor
  "self" @function.constructor)

; Function calls
(call
  function: (primary_expression (identifier) @function.call))

; Method calls
(call
  function: (primary_expression (attribute_expression
    attribute: (identifier) @function.method)))

; Decorator
(decorator
  (dotted_name) @attribute)
(decorator "@" @attribute)

; ─── Classes ─────────────────────────────────────────────────

; Class definition
(class_definition
  name: (identifier) @type)

; Superclass
(superclass
  (identifier) @type)

; ─── Types ───────────────────────────────────────────────────

; Type annotations
(type_annotation
  (type (identifier) @type))
(type_annotation
  (type (generic_type
    (identifier) @type)))

; Return type
(function_definition
  return_type: (type (identifier) @type))
(function_definition
  return_type: (type (generic_type
    (identifier) @type)))

; Extern function return type
(extern_function_decl
  (type (identifier) @type))
(extern_declaration
  (type (identifier) @type))

; Type alias
(type_alias
  (type (identifier) @type))
(type_alias
  (type (function_type
    (type (identifier) @type))))

; ─── Built-in Functions ──────────────────────────────────────
((identifier) @function.builtin
  (#match? @function.builtin "^(abs|all|any|bin|bool|callable|chr|classmethod|compile|delattr|dir|divmod|enumerate|eval|exec|filter|float|format|getattr|globals|hasattr|hash|help|hex|id|input|int|isinstance|issubclass|iter|len|list|locals|map|max|min|next|object|oct|open|ord|pow|print|property|range|repr|reversed|round|set|setattr|slice|sorted|staticmethod|str|sum|super|tuple|type|vars|zip|Lock|__import__)$"))

; ─── Built-in Types ──────────────────────────────────────────
((identifier) @type.builtin
  (#match? @type.builtin "^(int|float|complex|bool|str|bytes|bytearray|list|tuple|set|dict|frozenset|range|object|void|ptr)$"))

; ─── Stdlib Types ────────────────────────────────────────────
((identifier) @type
  (#match? @type "^(Optional|List|Dict|Set|Tuple|Any|Union|Callable|Iterable|Iterator|Generator|Sequence|Mapping|Type|TypeVar|IO|TextIO|BinaryIO|Pattern|Match|NoReturn|Thread|SyncList|SyncDict)$"))

; ─── Exception Types ─────────────────────────────────────────
((identifier) @type
  (#match? @type "^(BaseException|SystemExit|KeyboardInterrupt|GeneratorExit|Exception|StopIteration|ArithmeticError|ZeroDivisionError|OverflowError|FloatingPointError|AssertionError|AttributeError|BufferError|EOFError|ImportError|ModuleNotFoundError|LookupError|IndexError|KeyError|MemoryError|NameError|UnboundLocalError|OSError|FileNotFoundError|FileExistsError|PermissionError|IsADirectoryError|NotADirectoryError|InterruptedError|ProcessLookupError|TimeoutError|ConnectionError|ConnectionAbortedError|ConnectionRefusedError|ConnectionResetError|BrokenPipeError|BlockingIOError|ChildProcessError|ReferenceError|RuntimeError|NotImplementedError|RecursionError|StopAsyncIteration|SyntaxError|IndentationError|TabError|SystemError|TypeError|ValueError|UnicodeError|UnicodeDecodeError|UnicodeEncodeError|UnicodeTranslateError|Warning|UserWarning|DeprecationWarning|PendingDeprecationWarning|RuntimeWarning|SyntaxWarning|FutureWarning)$"))

; ─── Special Variables ───────────────────────────────────────
((identifier) @variable.builtin
  (#match? @variable.builtin "^(self|cls)$"))

; ─── Catch clause exception binding ─────────────────────────
(catch_clause
  (identifier) @type
  "as"
  (identifier) @variable)

; ─── Operators ───────────────────────────────────────────────
[
  "+"
  "-"
  "*"
  "/"
  "//"
  "%"
  "**"
  "@"
  "|"
  "^"
  "&"
  "~"
  "<<"
  ">>"
] @operator

[
  "="
  "+="
  "-="
  "*="
  "/="
  "//="
  "%="
  "**="
  "&="
  "|="
  "^="
  "<<="
  ">>="
  ":="
] @operator

[
  "<"
  "<="
  "=="
  "!="
  ">="
  ">"
] @operator

"->" @operator

; ─── Punctuation ─────────────────────────────────────────────
[
  "("
  ")"
] @punctuation.bracket

[
  "["
  "]"
] @punctuation.bracket

[
  "{"
  "}"
] @punctuation.bracket

["," "." ":" ";"] @punctuation.delimiter

; ─── Identifiers (fallback - must be FIRST so specifics override) ─
(identifier) @variable

; ─── Parameters ──────────────────────────────────────────────
(parameter
  name: (identifier) @variable.parameter)

; ─── Class fields ────────────────────────────────────────────
(class_field
  name: (identifier) @property)

; ─── Attribute access ────────────────────────────────────────
(attribute_expression
  attribute: (identifier) @property)

; ─── Pair keys (dict) ────────────────────────────────────────
(pair
  key: (primary_expression (identifier) @property))

; ─── Wildcard pattern ────────────────────────────────────────
(wildcard_pattern) @variable.builtin

; ─── Extern strings ──────────────────────────────────────────
(extern_declaration
  (string) @string.special)
(extern_block
  (string) @string.special)
