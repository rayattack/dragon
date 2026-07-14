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

" Ownership modifiers. `own` and `dub` are contextual keywords (they may be
" ordinary identifiers elsewhere), so highlight them only in modifier position:
" immediately before an identifier. That matches every real site - `own d: T`,
" `own field: T`, `f(own x)`, `f(dub x)` - and leaves a variable named `own`
" (followed by `=`, `.`, `(`, or end-of-line) alone. `del` stays a keyword.
syn match  dragonStorage        "\<\%(own\|dub\)\>\ze\s\+\h"

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

" ── Typed templates: embedded HTML / SQL / JSON / CSS / XML ──────────────────
" A `template[TYPE] { ... }` block is brace-DEPTH delimited by the Dragon lexer.
" Vim has no depth counter, so we rebuild it: literal `{ }` pairs in content are
" balanced contained regions (so a brace pair never ends the template early; a
" LONE `}` still does, exactly matching the lexer). Two sigils switch mode:
"   !{ expr }   breaks OUT of content into Dragon code   (content -> code)
"   :{ frag }   breaks BACK into content from inside code (code -> content)
"   !!{  !!}    literal escapes for a stray { or }
" Content mode embeds the target grammar; code mode is Dragon (contains=TOP).

" Pull the embedded grammars into clusters. `syn include` needs b:current_syntax
" unset between includes or the second one silently no-ops.
syn include @dragonHtml syntax/html.vim
unlet! b:current_syntax
syn include @dragonSql  syntax/sql.vim
unlet! b:current_syntax
syn include @dragonJson syntax/json.vim
unlet! b:current_syntax
syn include @dragonCss  syntax/css.vim
unlet! b:current_syntax
syn include @dragonXml  syntax/xml.vim
unlet! b:current_syntax

" Escapes are shared across all content types.
syn match  dragonTplEscape "!!{\|!!}" contained

" Dragon code inside interpolations. NOTE: we must NOT use `contains=TOP` here -
" TOP silently suppresses the explicitly-listed contained brace/fragment regions
" (verified with vim-textmate-style probes), which would let the interpolation's
" `end=}` fire on an inner loop brace. Instead enumerate the real Dragon groups.
syn cluster dragonCode contains=dragonKeyword,dragonStorage,dragonOperator,dragonConstant,dragonBuiltin,dragonType,dragonException,dragonSelf,dragonString,dragonNumber,dragonComment,dragonDecorator,dragonOperatorSym,dragonArrow,dragonTypeAnnotation,dragonTemplateHtml,dragonTemplateSql,dragonTemplateJson,dragonTemplateCss,dragonTemplateXml

" Balanced `{ }` code block inside `!{ ... }` (loop / if bodies). Per content
" type so a `:{}` inside it re-enters the right grammar.
syn region dragonTplCodeBraceHtml matchgroup=NONE start="{" end="}" contained transparent contains=@dragonCode,dragonTplFragmentHtml,dragonTplCodeBraceHtml,dragonTplEscape
syn region dragonTplCodeBraceSql  matchgroup=NONE start="{" end="}" contained transparent contains=@dragonCode,dragonTplFragmentSql,dragonTplCodeBraceSql,dragonTplEscape
syn region dragonTplCodeBraceJson matchgroup=NONE start="{" end="}" contained transparent contains=@dragonCode,dragonTplFragmentJson,dragonTplCodeBraceJson,dragonTplEscape
syn region dragonTplCodeBraceCss  matchgroup=NONE start="{" end="}" contained transparent contains=@dragonCode,dragonTplFragmentCss,dragonTplCodeBraceCss,dragonTplEscape
syn region dragonTplCodeBraceXml  matchgroup=NONE start="{" end="}" contained transparent contains=@dragonCode,dragonTplFragmentXml,dragonTplCodeBraceXml,dragonTplEscape

" `!{ ... }` interpolation: content -> Dragon code.
syn region dragonTplInterpHtml matchgroup=dragonTplDelim start="!{" end="}" contained contains=@dragonCode,dragonTplFragmentHtml,dragonTplCodeBraceHtml,dragonTplEscape
syn region dragonTplInterpSql  matchgroup=dragonTplDelim start="!{" end="}" contained contains=@dragonCode,dragonTplFragmentSql,dragonTplCodeBraceSql,dragonTplEscape
syn region dragonTplInterpJson matchgroup=dragonTplDelim start="!{" end="}" contained contains=@dragonCode,dragonTplFragmentJson,dragonTplCodeBraceJson,dragonTplEscape
syn region dragonTplInterpCss  matchgroup=dragonTplDelim start="!{" end="}" contained contains=@dragonCode,dragonTplFragmentCss,dragonTplCodeBraceCss,dragonTplEscape
syn region dragonTplInterpXml  matchgroup=dragonTplDelim start="!{" end="}" contained contains=@dragonCode,dragonTplFragmentXml,dragonTplCodeBraceXml,dragonTplEscape

" `:{ ... }` fragment: code -> content mode (re-embeds the grammar).
syn region dragonTplFragmentHtml matchgroup=dragonTplDelim start=":{" end="}" contained contains=@dragonHtml,dragonTplInterpHtml,dragonTplEscape,dragonTplLitBraceHtml
syn region dragonTplFragmentSql  matchgroup=dragonTplDelim start=":{" end="}" contained contains=@dragonSql,dragonTplInterpSql,dragonTplEscape,dragonTplLitBraceSql
syn region dragonTplFragmentJson matchgroup=dragonTplDelim start=":{" end="}" contained contains=@dragonJson,dragonTplInterpJson,dragonTplEscape,dragonTplLitBraceJson
syn region dragonTplFragmentCss  matchgroup=dragonTplDelim start=":{" end="}" contained contains=@dragonCss,dragonTplInterpCss,dragonTplEscape,dragonTplLitBraceCss
syn region dragonTplFragmentXml  matchgroup=dragonTplDelim start=":{" end="}" contained contains=@dragonXml,dragonTplInterpXml,dragonTplEscape,dragonTplLitBraceXml

" Balanced literal `{ }` inside content: keeps a brace pair from ending the block.
syn region dragonTplLitBraceHtml matchgroup=NONE start="{" end="}" contained transparent contains=@dragonHtml,dragonTplInterpHtml,dragonTplEscape,dragonTplLitBraceHtml
syn region dragonTplLitBraceSql  matchgroup=NONE start="{" end="}" contained transparent contains=@dragonSql,dragonTplInterpSql,dragonTplEscape,dragonTplLitBraceSql
syn region dragonTplLitBraceJson matchgroup=NONE start="{" end="}" contained transparent contains=@dragonJson,dragonTplInterpJson,dragonTplEscape,dragonTplLitBraceJson
syn region dragonTplLitBraceCss  matchgroup=NONE start="{" end="}" contained transparent contains=@dragonCss,dragonTplInterpCss,dragonTplEscape,dragonTplLitBraceCss
syn region dragonTplLitBraceXml  matchgroup=NONE start="{" end="}" contained transparent contains=@dragonXml,dragonTplInterpXml,dragonTplEscape,dragonTplLitBraceXml

" The template[TYPE] { ... } blocks themselves.
syn region dragonTemplateHtml matchgroup=dragonTemplate start="\<template\[HTML\]\s*{" end="}" contains=@dragonHtml,dragonTplInterpHtml,dragonTplEscape,dragonTplLitBraceHtml
syn region dragonTemplateSql  matchgroup=dragonTemplate start="\<template\[SQL\]\s*{"  end="}" contains=@dragonSql,dragonTplInterpSql,dragonTplEscape,dragonTplLitBraceSql
syn region dragonTemplateJson matchgroup=dragonTemplate start="\<template\[JSON\]\s*{" end="}" contains=@dragonJson,dragonTplInterpJson,dragonTplEscape,dragonTplLitBraceJson
syn region dragonTemplateCss  matchgroup=dragonTemplate start="\<template\[CSS\]\s*{"  end="}" contains=@dragonCss,dragonTplInterpCss,dragonTplEscape,dragonTplLitBraceCss
syn region dragonTemplateXml  matchgroup=dragonTemplate start="\<template\[XML\]\s*{"  end="}" contains=@dragonXml,dragonTplInterpXml,dragonTplEscape,dragonTplLitBraceXml

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
hi def link dragonTemplate      Keyword
hi def link dragonTplDelim      Special
hi def link dragonTplEscape     SpecialChar

let b:current_syntax = "dragon"
