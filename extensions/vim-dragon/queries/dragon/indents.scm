; Dragon indentation rules for Zed

; Blocks increase indent
(block "{" @indent "}" @outdent) @indent

; Class bodies
(class_body "{" @indent "}" @outdent) @indent

; Match statement body
(match_statement "{" @indent "}" @outdent) @indent

; If/elif/else
(if_statement) @indent
(elif_clause) @indent
(else_clause) @indent

; Loops
(while_statement) @indent
(for_statement) @indent

; Try/catch/finally
(try_statement) @indent
(catch_clause) @indent
(finally_clause) @indent

; With statement
(with_statement) @indent

; Case clause
(case_clause) @indent

; Function and class definitions
(function_definition) @indent
(class_definition) @indent
(self_constructor) @indent

; Thread block
(thread_block) @indent

; Collections spanning multiple lines
(list "[" @indent "]" @outdent)
(dictionary "{" @indent "}" @outdent)
(argument_list "(" @indent ")" @outdent)
(parameter_list "(" @indent ")" @outdent)
