# CodeGenExceptionsTest cases

Source programs for `test/CodeGenExceptionsTest.cpp`. Each block is named by the heading
above it and pulled in with `code("<name>")`.

#### :try_except_basic

```dr
try {
  print(1)
} except {
  print(2)
}
```

#### :try_except_typed

```dr
try {
  print(1)
} except ValueError {
  print(2)
}
```

#### :try_except_finally

```dr
try {
  print(1)
} except {
  print(2)
} finally {
  print(3)
}
```

#### :try_except_else

```dr
try {
  print(1)
} except {
  print(2)
} else {
  print(3)
}
```

#### :try_multiple_handlers

```dr
try {
  print(1)
} except ValueError {
  print(2)
} except TypeError {
  print(3)
}
```

#### :try_except_named_handler

```dr
try {
  print(1)
} except ValueError as e {
  print(e)
}
```

#### :except_star_parses_ir

```dr
try {
    x: int = 1
} except* ValueError as e {
    x: int = 2
}
```

#### :exc_hierarchy_match_call_ir

```dr
try {
  print(1)
} except ArithmeticError {
  print(2)
}
```

#### :exc_hierarchy_leaf_match_ir

```dr
try {
  print(1)
} except IndexError {
  print(2)
}
```

#### :exc_hierarchy_exception_match_ir

```dr
try {
  print(1)
} except Exception {
  print(2)
}
```

#### :user_exc_register_call_ir

```dr
class MyError(Exception) {
  def(msg: str) {
    self.msg = msg
  }
}
print(1)
```

#### :user_exc_matches_call_ir

```dr
class MyError(Exception) {
  def(msg: str) {
    self.msg = msg
  }
}
try {
  print(1)
} except MyError {
  print(2)
}
```

#### :try_catch_basic

```dr
try {
  raise ValueError("bad")
  print("unreachable")
} except ValueError as e {
  print("caught")
  print(e)
}
```

#### :try_finally_exec

```dr
try {
  raise ValueError("oops")
} except ValueError {
  print("handler")
} finally {
  print("finally")
}
```

#### :try_catch_else

```dr
try {
  print("ok")
} except ValueError {
  print("error")
} else {
  print("no error")
}
```

#### :finally_on_return

```dr
def foo() -> int {
    try {
        print("try")
        return 42
    } finally {
        print("finally")
    }
}
x: int = foo()
print(x)
```

#### :finally_on_break

```dr
for i in range(5) {
    try {
        if i == 2 {
            break
        }
        print(i)
    } finally {
        print("f")
    }
}
print("done")
```

#### :finally_on_continue

```dr
for i in range(3) {
    try {
        if i == 1 {
            continue
        }
        print(i)
    } finally {
        print("f")
    }
}
```

#### :exc_hierarchy_arithmetic_catches_zero_div

```dr
try {
  raise ZeroDivisionError("div0")
} except ArithmeticError as e {
  print("caught")
  print(e)
}
```

#### :exc_hierarchy_arithmetic_catches_overflow

```dr
try {
  raise OverflowError("too big")
} except ArithmeticError as e {
  print("caught")
  print(e)
}
```

#### :exc_hierarchy_lookup_catches_index

```dr
try {
  raise IndexError("oob")
} except LookupError as e {
  print("caught")
  print(e)
}
```

#### :exc_hierarchy_lookup_catches_key

```dr
try {
  raise KeyError("missing")
} except LookupError as e {
  print("caught")
  print(e)
}
```

#### :exc_hierarchy_exception_catches_value

```dr
try {
  raise ValueError("bad")
} except Exception as e {
  print("caught")
  print(e)
}
```

#### :exc_hierarchy_os_error_catches_file_not_found

```dr
try {
  raise FileNotFoundError("no file")
} except OSError as e {
  print("caught")
  print(e)
}
```

#### :exc_hierarchy_os_error_catches_connection_child

```dr
try {
  raise ConnectionRefusedError("refused")
} except OSError as e {
  print("caught")
  print(e)
}
```

#### :exc_hierarchy_connection_catches_broken

```dr
try {
  raise BrokenPipeError("pipe")
} except ConnectionError as e {
  print("caught")
  print(e)
}
```

#### :exc_hierarchy_value_catches_unicode

```dr
try {
  raise UnicodeDecodeError("decode fail")
} except ValueError as e {
  print("caught")
  print(e)
}
```

#### :exc_hierarchy_runtime_catches_not_impl

```dr
try {
  raise NotImplementedError("todo")
} except RuntimeError as e {
  print("caught")
  print(e)
}
```

#### :exc_hierarchy_leaf_no_match_reraise

```dr
try {
  try {
    raise KeyError("k")
  } except IndexError {
    print("wrong")
  }
} except KeyError as e {
  print("correct")
  print(e)
}
```

#### :exc_hierarchy_multi_specific

```dr
try {
  raise IndexError("idx")
} except IndexError as e {
  print("index")
} except LookupError as e {
  print("lookup")
} except Exception {
  print("generic")
}
```

#### :exc_hierarchy_name_catches_unbound

```dr
try {
  raise UnboundLocalError("x")
} except NameError as e {
  print("caught")
  print(e)
}
```

#### :exc_hierarchy_import_catches_module_not_found

```dr
try {
  raise ModuleNotFoundError("no mod")
} except ImportError as e {
  print("caught")
  print(e)
}
```

#### :user_exc_basic_raise_catch

```dr
class AppError(Exception) {
  def(msg: str) {
    self.msg = msg
  }
}
try {
  raise AppError("app fail")
} except AppError as e {
  print("caught")
  print(e)
}
```

#### :user_exc_parent_catches_child

```dr
class HTTPError(RuntimeError) {
  def(msg: str) {
    self.msg = msg
  }
}
class NotFoundError(HTTPError) {
  def(msg: str) {
    self.msg = msg
  }
}
try {
  raise NotFoundError("404")
} except HTTPError as e {
  print("caught")
  print(e)
}
```

#### :user_exc_builtin_parent_catches_user

```dr
class MyRuntimeError(RuntimeError) {
  def(msg: str) {
    self.msg = msg
  }
}
try {
  raise MyRuntimeError("custom")
} except RuntimeError as e {
  print("caught")
  print(e)
}
```

#### :user_exc_exception_catches_user

```dr
class MyError(ValueError) {
  def(msg: str) {
    self.msg = msg
  }
}
try {
  raise MyError("val")
} except Exception as e {
  print("caught")
  print(e)
}
```

#### :user_exc_no_match_reraise

```dr
class ErrorA(Exception) {
  def(msg: str) {
    self.msg = msg
  }
}
class ErrorB(Exception) {
  def(msg: str) {
    self.msg = msg
  }
}
try {
  try {
    raise ErrorB("b")
  } except ErrorA {
    print("wrong")
  }
} except ErrorB as e {
  print("correct")
  print(e)
}
```

#### :user_exc_multi_handler

```dr
class BaseError(Exception) {
  def(msg: str) {
    self.msg = msg
  }
}
class SpecificError(BaseError) {
  def(msg: str) {
    self.msg = msg
  }
}
try {
  raise SpecificError("spec")
} except SpecificError as e {
  print("specific")
} except BaseError as e {
  print("base")
} except Exception {
  print("generic")
}
```

#### :user_exc_grandparent_catches

```dr
class Level1(Exception) {
  def(msg: str) {
    self.msg = msg
  }
}
class Level2(Level1) {
  def(msg: str) {
    self.msg = msg
  }
}
class Level3(Level2) {
  def(msg: str) {
    self.msg = msg
  }
}
try {
  raise Level3("deep")
} except Level1 as e {
  print("caught")
  print(e)
}
```

#### :user_exc_no_arg_default_msg

```dr
class EmptyError(Exception) {
  def(msg: str = "EmptyError") {
    self.msg = msg
  }
}
try {
  raise EmptyError()
} except EmptyError as e {
  print("caught")
  print(e)
}
```

#### :match_stmt_ir

```dr
x: int = 42
match x {
    case 1 { print(10) }
    case _ { print(20) }
}
```

#### :py_match_case_ir

```py
x: int = 2
match x:
    case 1:
        print(10)
    case 2:
        print(20)
    case _:
        print(0)
```

#### :match_int_literal

```dr
x: int = 2
match x {
    case 1 { print(10) }
    case 2 { print(20) }
    case 3 { print(30) }
}
```

#### :match_wildcard

```dr
x: int = 99
match x {
    case 1 { print(10) }
    case _ { print(42) }
}
```

#### :match_capture

```dr
x: int = 7
match x {
    case 1 { print(10) }
    case y { print(y) }
}
```

#### :match_string_literal

```dr
s: str = "hello"
match s {
    case "world" { print(1) }
    case "hello" { print(2) }
    case _ { print(3) }
}
```

#### :match_or_pattern

```dr
x: int = 3
match x {
    case 1 | 2 { print(10) }
    case 3 | 4 { print(20) }
    case _ { print(30) }
}
```

#### :match_no_arm_matches

```dr
x: int = 99
match x {
    case 1 { print(10) }
    case 2 { print(20) }
}
print(0)
```

#### :match_first_arm_matches

```dr
x: int = 1
match x {
    case 1 { print(10) }
    case 2 { print(20) }
    case _ { print(30) }
}
```

#### :match_with_guard

```dr
x: int = 5
match x {
    case y if y > 10 { print(1) }
    case y if y > 3 { print(2) }
    case _ { print(3) }
}
```

#### :match_comma_or_pattern

```dr
x: int = 2
match x {
    case 1, 2, 3 { print(10) }
    case 4, 5 { print(20) }
    case _ { print(30) }
}
```

#### :match_pipe_or_pattern_regression

```dr
x: int = 4
match x {
    case 1 | 2 | 3 { print(10) }
    case 4 | 5 { print(20) }
    case _ { print(30) }
}
```

#### :py_match_case_e2_e

```py
x: int = 2
match x:
    case 1:
        print(10)
    case 2:
        print(20)
    case _:
        print(0)
```

#### :match_arm_capture_bounded_loop

```dr
total: int = 0
for i in range(10000) {
  match i {
    case 0 { total = total + 1 }
    case y { total = total + y }
  }
}
print(total)
```

#### :match_string_subject_loop_bounded

```dr
labels: list[str] = ["go", "stop", "caution"]
go_count: int = 0
stop_count: int = 0
other_count: int = 0
for i in range(3000) {
  for s in labels {
    match s {
      case "go" { go_count = go_count + 1 }
      case "stop" { stop_count = stop_count + 1 }
      case _ { other_count = other_count + 1 }
    }
  }
}
print(go_count)
print(stop_count)
print(other_count)
```

#### :exception_raise_and_catch__all_builtins

```dr
def t(name: str) {
    print(name)
}
try { raise ValueError("v") } except ValueError { t("ve") }
try { raise TypeError("t") } except TypeError { t("te") }
try { raise KeyError("k") } except LookupError { t("le") }
try { raise IndexError("i") } except LookupError { t("ie") }
try { raise ZeroDivisionError("z") } except ArithmeticError { t("ae") }
try { raise OverflowError("o") } except ArithmeticError { t("oe") }
try { raise FileNotFoundError("f") } except OSError { t("fe") }
try { raise PermissionError("p") } except OSError { t("pe") }
try { raise IOError("io") } except OSError { t("ioe") }
try { raise ModuleNotFoundError("m") } except ImportError { t("me") }
try { raise NotImplementedError("n") } except RuntimeError { t("ne") }
try { raise RecursionError("r") } except RuntimeError { t("re") }
try { raise UnicodeDecodeError("u") } except UnicodeError { t("ue") }
try { raise AttributeError("a") } except AttributeError { t("aa") }
try { raise NameError("nm") } except NameError { t("nme") }
try { raise StopIteration("s") } except Exception { t("se") }
try { raise AssertionError("x") } except Exception { t("asx") }
try { raise RuntimeError("q") } except BaseException { t("be") }
```

#### :match_arm_emits_cleanup_before_end_branch

```dr
x: int = 7
match x {
    case 0 { print(0) }
    case y { print(y) }
}
```

#### :exception_class_as_value

```dr
def take(t: type) -> int { return 1 }
print(take(ValueError))
```

#### :exc_matches_exact_type

```dr
matched: bool = False
try {
    raise ValueError("x")
} except Exception as e {
    matched = __exc_matches(ValueError)
}
print(matched)
```

#### :exc_matches_parent_range

```dr
matched: bool = False
try {
    raise ValueError("x")
} except Exception as e {
    matched = __exc_matches(Exception)
}
print(matched)
```

#### :exc_matches_wrong_type

```dr
matched: bool = True
try {
    raise ValueError("x")
} except Exception as e {
    matched = __exc_matches(KeyError)
}
print(matched)
```

#### :int_str_valid_forms

```dr
print(int("42"))
print(int("  -17  "))
print(int("+5"))
print(int("1_000"))
```

#### :int_str_invalid_raises_value_error

```dr
ok: bool = False
try {
    x: int = int("foo")
} except ValueError as e {
    ok = True
}
print(ok)
```

#### :int_str_float_string_raises

```dr
ok: bool = False
try {
    x: int = int("4.5")
} except ValueError as e {
    ok = True
}
print(ok)
```
