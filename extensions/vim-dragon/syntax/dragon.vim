" Vim syntax file for the Dragon programming language
" Language:    Dragon
" Maintainer:  tbhi
" Last Change: 2026-04-04

if exists("b:current_syntax")
  finish
endif

" Keywords
syn keyword dragonKeyword       if elif else while for in break continue return
syn keyword dragonKeyword       pass yield raise try except catch finally with
syn keyword dragonKeyword       match case lambda global nonlocal
syn keyword dragonKeyword       def       skipwhite nextgroup=dragonFunction
syn keyword dragonKeyword       class     skipwhite nextgroup=dragonClass
syn keyword dragonKeyword       assert del type import from as
syn keyword dragonKeyword       fire thread async await

" Storage modifiers
syn keyword dragonStorage       const static extern

" Logical operators
syn keyword dragonOperator      and or not is

" Constants
syn keyword dragonConstant      True False None Ellipsis NotImplemented

" Built-in functions
syn keyword dragonBuiltin       abs all any bin bool callable chr classmethod
syn keyword dragonBuiltin       compile delattr dir divmod enumerate eval exec
syn keyword dragonBuiltin       filter float format getattr globals hasattr hash
syn keyword dragonBuiltin       help hex id input int isinstance issubclass iter
syn keyword dragonBuiltin       len list locals map max min next object oct open
syn keyword dragonBuiltin       ord pow print property range repr reversed round
syn keyword dragonBuiltin       set setattr slice sorted staticmethod str sum
syn keyword dragonBuiltin       super tuple type vars zip Lock __import__

" Built-in types
syn keyword dragonType          int float complex bool str bytes bytearray list
syn keyword dragonType          tuple set dict frozenset range object void ptr

" Stdlib types
syn keyword dragonType          Optional List Dict Set Tuple Any Union Callable
syn keyword dragonType          Iterable Iterator Generator Sequence Mapping Type
syn keyword dragonType          TypeVar IO TextIO BinaryIO Pattern Match NoReturn
syn keyword dragonType          Thread SyncList SyncDict

" Exception types
syn keyword dragonException     BaseException SystemExit KeyboardInterrupt
syn keyword dragonException     GeneratorExit Exception StopIteration
syn keyword dragonException     ArithmeticError ZeroDivisionError OverflowError
syn keyword dragonException     FloatingPointError AssertionError AttributeError
syn keyword dragonException     BufferError EOFError ImportError ModuleNotFoundError
syn keyword dragonException     LookupError IndexError KeyError MemoryError
syn keyword dragonException     NameError UnboundLocalError OSError
syn keyword dragonException     FileNotFoundError FileExistsError PermissionError
syn keyword dragonException     IsADirectoryError NotADirectoryError
syn keyword dragonException     InterruptedError ProcessLookupError TimeoutError
syn keyword dragonException     ConnectionError ConnectionAbortedError
syn keyword dragonException     ConnectionRefusedError ConnectionResetError
syn keyword dragonException     BrokenPipeError BlockingIOError ChildProcessError
syn keyword dragonException     ReferenceError RuntimeError NotImplementedError
syn keyword dragonException     RecursionError StopAsyncIteration SyntaxError
syn keyword dragonException     IndentationError TabError SystemError TypeError
syn keyword dragonException     ValueError UnicodeError UnicodeDecodeError
syn keyword dragonException     UnicodeEncodeError UnicodeTranslateError Warning
syn keyword dragonException     UserWarning DeprecationWarning
syn keyword dragonException     PendingDeprecationWarning RuntimeWarning
syn keyword dragonException     SyntaxWarning FutureWarning

" Special variables
syn keyword dragonSelf          self cls

" Strings
syn region dragonString         start=+"""+ end=+"""+ contains=dragonEscape,dragonInterpolation
syn region dragonString         start=+'''+ end=+'''+ contains=dragonEscape
syn region dragonString         start=+f"+ end=+"+ contains=dragonEscape,dragonInterpolation
syn region dragonString         start=+f'+ end=+'+ contains=dragonEscape,dragonInterpolation
syn region dragonString         start=+r"+ end=+"+
syn region dragonString         start=+r'+ end=+'+
syn region dragonString         start=+b"+ end=+"+ contains=dragonEscape
syn region dragonString         start=+b'+ end=+'+ contains=dragonEscape
syn region dragonString         start=+"+ end=+"+ skip=+\\"+ contains=dragonEscape
syn region dragonString         start=+'+ end=+'+ skip=+\\'+ contains=dragonEscape

syn match  dragonEscape         +\\.+ contained
syn region dragonInterpolation  start=+{+ end=+}+ contained contains=TOP

" Numbers
syn match  dragonNumber         "\<0[xX][0-9a-fA-F_]\+\>"
syn match  dragonNumber         "\<0[oO][0-7_]\+\>"
syn match  dragonNumber         "\<0[bB][01_]\+\>"
syn match  dragonNumber         "\<[0-9][0-9_]*\.\=[0-9_]*\([eE][+-]\=[0-9_]\+\)\=\>"

" Comments
syn match  dragonComment        "#.*$"

" Decorators
syn match  dragonDecorator      "^\s*@[a-zA-Z_][a-zA-Z0-9_.]*"

" Function and class names (anchored via nextgroup off the def/class keywords)
syn match  dragonFunction       "[a-zA-Z_][a-zA-Z0-9_]*" contained
syn match  dragonClass          "[a-zA-Z_][a-zA-Z0-9_]*" contained

" Type annotations (after : and ->)
syn match  dragonTypeAnnotation "->\s*\zs[a-zA-Z_][a-zA-Z0-9_\[\], ]*"

" Operators
syn match  dragonOperatorSym    "[+\-*/%@&|^~<>=!]\+"
syn match  dragonArrow          "->"

" Highlight links
hi def link dragonKeyword       Keyword
hi def link dragonStorage       StorageClass
hi def link dragonOperator      Keyword
hi def link dragonConstant      Constant
hi def link dragonBuiltin       Function
hi def link dragonType          Type
hi def link dragonException     Type
hi def link dragonSelf          Identifier
hi def link dragonString        String
hi def link dragonEscape        SpecialChar
hi def link dragonInterpolation Special
hi def link dragonNumber        Number
hi def link dragonComment       Comment
hi def link dragonDecorator     PreProc
hi def link dragonFunction      Function
hi def link dragonClass         Type
hi def link dragonTypeAnnotation Type
hi def link dragonOperatorSym   Operator
hi def link dragonArrow         Operator

let b:current_syntax = "dragon"
