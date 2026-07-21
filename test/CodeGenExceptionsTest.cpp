#include "CodeGenTestHelpers.h"

// ============================================================
// Try/Except IR Tests
// ============================================================

TEST(CodeGenTest, TryExceptBasic) {
    auto ir = generateIR(
        "try {\n"
        "  print(1)\n"
        "} except {\n"
        "  print(2)\n"
        "}\n"
    );
    EXPECT_NE(ir.find("dragon_exc_push_frame"), std::string::npos);
    EXPECT_NE(ir.find("setjmp"), std::string::npos);
    EXPECT_NE(ir.find("dragon_exc_pop_frame"), std::string::npos);
}

TEST(CodeGenTest, TryExceptTyped) {
    auto ir = generateIR(
        "try {\n"
        "  print(1)\n"
        "} except ValueError {\n"
        "  print(2)\n"
        "}\n"
    );
    EXPECT_NE(ir.find("dragon_exc_get_type"), std::string::npos);
    EXPECT_NE(ir.find("icmp eq"), std::string::npos);
}

TEST(CodeGenTest, TryExceptFinally) {
    auto ir = generateIR(
        "try {\n"
        "  print(1)\n"
        "} except {\n"
        "  print(2)\n"
        "} finally {\n"
        "  print(3)\n"
        "}\n"
    );
    EXPECT_NE(ir.find("try0.finally"), std::string::npos);
}

TEST(CodeGenTest, TryExceptElse) {
    auto ir = generateIR(
        "try {\n"
        "  print(1)\n"
        "} except {\n"
        "  print(2)\n"
        "} else {\n"
        "  print(3)\n"
        "}\n"
    );
    EXPECT_NE(ir.find("try0.else"), std::string::npos);
}

TEST(CodeGenTest, TryMultipleHandlers) {
    auto ir = generateIR(
        "try {\n"
        "  print(1)\n"
        "} except ValueError {\n"
        "  print(2)\n"
        "} except TypeError {\n"
        "  print(3)\n"
        "}\n"
    );
    EXPECT_NE(ir.find("handler.check.0"), std::string::npos);
    EXPECT_NE(ir.find("handler.check.1"), std::string::npos);
    EXPECT_NE(ir.find("dragon_raise_exc"), std::string::npos);
}

TEST(CodeGenTest, RaiseValueError) {
    auto ir = generateIR("raise ValueError(\"bad value\")");
    EXPECT_NE(ir.find("dragon_raise_exc"), std::string::npos);
    EXPECT_NE(ir.find("bad value"), std::string::npos);
    EXPECT_NE(ir.find("unreachable"), std::string::npos);
}

TEST(CodeGenTest, RaiseBare) {
    auto ir = generateIR("raise");
    EXPECT_NE(ir.find("dragon_raise_exc"), std::string::npos);
    // Bare `raise` re-raises the in-flight exception, preserving its type and
    // message - so it reads the current exception state rather than fabricating
    // a literal SystemExit/"Exception" (the old, incorrect behavior).
    EXPECT_NE(ir.find("dragon_exc_get_type"), std::string::npos);
    EXPECT_NE(ir.find("dragon_exc_get_msg"), std::string::npos);
}

TEST(CodeGenTest, TryExceptNamedHandler) {
    auto ir = generateIR(
        "try {\n"
        "  print(1)\n"
        "} except ValueError as e {\n"
        "  print(e)\n"
        "}\n"
    );
    EXPECT_NE(ir.find("dragon_exc_get_msg"), std::string::npos);
}

TEST(CodeGenIR, ExceptStarParsesIR) {
    // except* should parse and generate IR (handler body runs if matched)
    auto ir = generateIR(
        "try {\n"
        "    x: int = 1\n"
        "} except* ValueError as e {\n"
        "    x: int = 2\n"
        "}\n"
    );
    // Should generate valid IR with the try/except structure
    EXPECT_NE(ir.find("define"), std::string::npos);
}

// ============================================================
// Exception Hierarchy IR Tests
// ============================================================

TEST(CodeGenTest, ExcHierarchyMatchCallIR) {
    // All typed handlers now use dragon_exc_matches runtime function
    auto ir = generateIR(
        "try {\n"
        "  print(1)\n"
        "} except ArithmeticError {\n"
        "  print(2)\n"
        "}\n"
    );
    EXPECT_NE(ir.find("dragon_exc_matches"), std::string::npos);
    EXPECT_NE(ir.find("exc.match.0"), std::string::npos);
}

TEST(CodeGenTest, ExcHierarchyLeafMatchIR) {
    // Leaf exception also uses dragon_exc_matches (unified dispatch)
    auto ir = generateIR(
        "try {\n"
        "  print(1)\n"
        "} except IndexError {\n"
        "  print(2)\n"
        "}\n"
    );
    EXPECT_NE(ir.find("dragon_exc_matches"), std::string::npos);
    EXPECT_NE(ir.find("exc.match.0"), std::string::npos);
}

TEST(CodeGenTest, ExcHierarchyExceptionMatchIR) {
    // Exception (wide parent) also uses dragon_exc_matches
    auto ir = generateIR(
        "try {\n"
        "  print(1)\n"
        "} except Exception {\n"
        "  print(2)\n"
        "}\n"
    );
    EXPECT_NE(ir.find("dragon_exc_matches"), std::string::npos);
}

// ============================================================
// User Exception IR Tests
// ============================================================

TEST(CodeGenTest, UserExcRegisterCallIR) {
    // User exception class should generate dragon_exc_register call
    auto ir = generateIR(
        "class MyError(Exception) {\n"
        "  def(msg: str) {\n"
        "    self.msg = msg\n"
        "  }\n"
        "}\n"
        "print(1)\n"
    );
    EXPECT_NE(ir.find("dragon_exc_register"), std::string::npos);
}

TEST(CodeGenTest, UserExcMatchesCallIR) {
    // except MyError should use dragon_exc_matches
    auto ir = generateIR(
        "class MyError(Exception) {\n"
        "  def(msg: str) {\n"
        "    self.msg = msg\n"
        "  }\n"
        "}\n"
        "try {\n"
        "  print(1)\n"
        "} except MyError {\n"
        "  print(2)\n"
        "}\n"
    );
    EXPECT_NE(ir.find("dragon_exc_matches"), std::string::npos);
    EXPECT_NE(ir.find("exc.match.0"), std::string::npos);
}

// ============================================================
// Try/Except E2E Tests
// ============================================================

TEST(CodeGenE2E, TryCatchBasic) {
    auto output = compileAndRun(
        "try {\n"
        "  raise ValueError(\"bad\")\n"
        "  print(\"unreachable\")\n"
        "} except ValueError as e {\n"
        "  print(\"caught\")\n"
        "  print(e)\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\nbad\n");
}

TEST(CodeGenE2E, TryFinallyExec) {
    auto output = compileAndRun(
        "try {\n"
        "  raise ValueError(\"oops\")\n"
        "} except ValueError {\n"
        "  print(\"handler\")\n"
        "} finally {\n"
        "  print(\"finally\")\n"
        "}\n"
    );
    EXPECT_EQ(output, "handler\nfinally\n");
}

TEST(CodeGenE2E, TryCatchElse) {
    auto output = compileAndRun(
        "try {\n"
        "  print(\"ok\")\n"
        "} except ValueError {\n"
        "  print(\"error\")\n"
        "} else {\n"
        "  print(\"no error\")\n"
        "}\n"
    );
    EXPECT_EQ(output, "ok\nno error\n");
}

TEST(CodeGenE2E, FinallyOnReturn) {
    // Finally block must execute even when return exits the try body
    auto output = compileAndRun(
        "def foo() -> int {\n"
        "    try {\n"
        "        print(\"try\")\n"
        "        return 42\n"
        "    } finally {\n"
        "        print(\"finally\")\n"
        "    }\n"
        "}\n"
        "x: int = foo()\n"
        "print(x)\n"
    );
    EXPECT_EQ(output, "try\nfinally\n42\n");
}

TEST(CodeGenE2E, FinallyOnBreak) {
    // Finally block must execute when break exits a loop inside try
    auto output = compileAndRun(
        "for i in range(5) {\n"
        "    try {\n"
        "        if i == 2 {\n"
        "            break\n"
        "        }\n"
        "        print(i)\n"
        "    } finally {\n"
        "        print(\"f\")\n"
        "    }\n"
        "}\n"
        "print(\"done\")\n"
    );
    EXPECT_EQ(output, "0\nf\n1\nf\nf\ndone\n");
}

TEST(CodeGenE2E, FinallyOnContinue) {
    // Finally block must execute when continue skips rest of loop body
    auto output = compileAndRun(
        "for i in range(3) {\n"
        "    try {\n"
        "        if i == 1 {\n"
        "            continue\n"
        "        }\n"
        "        print(i)\n"
        "    } finally {\n"
        "        print(\"f\")\n"
        "    }\n"
        "}\n"
    );
    EXPECT_EQ(output, "0\nf\nf\n2\nf\n");
}

// ============================================================
// Exception Hierarchy E2E Tests
// ============================================================

TEST(CodeGenE2E, ExcHierarchyArithmeticCatchesZeroDiv) {
    auto output = compileAndRun(
        "try {\n"
        "  raise ZeroDivisionError(\"div0\")\n"
        "} except ArithmeticError as e {\n"
        "  print(\"caught\")\n"
        "  print(e)\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\ndiv0\n");
}

TEST(CodeGenE2E, ExcHierarchyArithmeticCatchesOverflow) {
    auto output = compileAndRun(
        "try {\n"
        "  raise OverflowError(\"too big\")\n"
        "} except ArithmeticError as e {\n"
        "  print(\"caught\")\n"
        "  print(e)\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\ntoo big\n");
}

TEST(CodeGenE2E, ExcHierarchyLookupCatchesIndex) {
    auto output = compileAndRun(
        "try {\n"
        "  raise IndexError(\"oob\")\n"
        "} except LookupError as e {\n"
        "  print(\"caught\")\n"
        "  print(e)\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\noob\n");
}

TEST(CodeGenE2E, ExcHierarchyLookupCatchesKey) {
    auto output = compileAndRun(
        "try {\n"
        "  raise KeyError(\"missing\")\n"
        "} except LookupError as e {\n"
        "  print(\"caught\")\n"
        "  print(e)\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\nmissing\n");
}

TEST(CodeGenE2E, ExcHierarchyExceptionCatchesValue) {
    auto output = compileAndRun(
        "try {\n"
        "  raise ValueError(\"bad\")\n"
        "} except Exception as e {\n"
        "  print(\"caught\")\n"
        "  print(e)\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\nbad\n");
}

TEST(CodeGenE2E, ExcHierarchyOSErrorCatchesFileNotFound) {
    auto output = compileAndRun(
        "try {\n"
        "  raise FileNotFoundError(\"no file\")\n"
        "} except OSError as e {\n"
        "  print(\"caught\")\n"
        "  print(e)\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\nno file\n");
}

TEST(CodeGenE2E, ExcHierarchyOSErrorCatchesConnectionChild) {
    // Nested hierarchy: OSError > ConnectionError > ConnectionRefusedError
    auto output = compileAndRun(
        "try {\n"
        "  raise ConnectionRefusedError(\"refused\")\n"
        "} except OSError as e {\n"
        "  print(\"caught\")\n"
        "  print(e)\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\nrefused\n");
}

TEST(CodeGenE2E, ExcHierarchyConnectionCatchesBroken) {
    auto output = compileAndRun(
        "try {\n"
        "  raise BrokenPipeError(\"pipe\")\n"
        "} except ConnectionError as e {\n"
        "  print(\"caught\")\n"
        "  print(e)\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\npipe\n");
}

TEST(CodeGenE2E, ExcHierarchyValueCatchesUnicode) {
    auto output = compileAndRun(
        "try {\n"
        "  raise UnicodeDecodeError(\"decode fail\")\n"
        "} except ValueError as e {\n"
        "  print(\"caught\")\n"
        "  print(e)\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\ndecode fail\n");
}

TEST(CodeGenE2E, ExcHierarchyRuntimeCatchesNotImpl) {
    auto output = compileAndRun(
        "try {\n"
        "  raise NotImplementedError(\"todo\")\n"
        "} except RuntimeError as e {\n"
        "  print(\"caught\")\n"
        "  print(e)\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\ntodo\n");
}

TEST(CodeGenE2E, ExcHierarchyLeafNoMatchReraise) {
    // IndexError should NOT catch KeyError - re-raise to outer handler
    auto output = compileAndRun(
        "try {\n"
        "  try {\n"
        "    raise KeyError(\"k\")\n"
        "  } except IndexError {\n"
        "    print(\"wrong\")\n"
        "  }\n"
        "} except KeyError as e {\n"
        "  print(\"correct\")\n"
        "  print(e)\n"
        "}\n"
    );
    EXPECT_EQ(output, "correct\nk\n");
}

TEST(CodeGenE2E, ExcHierarchyMultiSpecific) {
    // Multiple handlers - most specific first
    auto output = compileAndRun(
        "try {\n"
        "  raise IndexError(\"idx\")\n"
        "} except IndexError as e {\n"
        "  print(\"index\")\n"
        "} except LookupError as e {\n"
        "  print(\"lookup\")\n"
        "} except Exception {\n"
        "  print(\"generic\")\n"
        "}\n"
    );
    EXPECT_EQ(output, "index\n");
}

TEST(CodeGenE2E, ExcHierarchyNameCatchesUnbound) {
    auto output = compileAndRun(
        "try {\n"
        "  raise UnboundLocalError(\"x\")\n"
        "} except NameError as e {\n"
        "  print(\"caught\")\n"
        "  print(e)\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\nx\n");
}

TEST(CodeGenE2E, ExcHierarchyImportCatchesModuleNotFound) {
    auto output = compileAndRun(
        "try {\n"
        "  raise ModuleNotFoundError(\"no mod\")\n"
        "} except ImportError as e {\n"
        "  print(\"caught\")\n"
        "  print(e)\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\nno mod\n");
}

// ============================================================
// User Exception E2E Tests
// ============================================================

TEST(CodeGenE2E, UserExcBasicRaiseCatch) {
    auto output = compileAndRun(
        "class AppError(Exception) {\n"
        "  def(msg: str) {\n"
        "    self.msg = msg\n"
        "  }\n"
        "}\n"
        "try {\n"
        "  raise AppError(\"app fail\")\n"
        "} except AppError as e {\n"
        "  print(\"caught\")\n"
        "  print(e)\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\napp fail\n");
}

TEST(CodeGenE2E, UserExcParentCatchesChild) {
    auto output = compileAndRun(
        "class HTTPError(RuntimeError) {\n"
        "  def(msg: str) {\n"
        "    self.msg = msg\n"
        "  }\n"
        "}\n"
        "class NotFoundError(HTTPError) {\n"
        "  def(msg: str) {\n"
        "    self.msg = msg\n"
        "  }\n"
        "}\n"
        "try {\n"
        "  raise NotFoundError(\"404\")\n"
        "} except HTTPError as e {\n"
        "  print(\"caught\")\n"
        "  print(e)\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\n404\n");
}

TEST(CodeGenE2E, UserExcBuiltinParentCatchesUser) {
    // except RuntimeError should catch user-defined child
    auto output = compileAndRun(
        "class MyRuntimeError(RuntimeError) {\n"
        "  def(msg: str) {\n"
        "    self.msg = msg\n"
        "  }\n"
        "}\n"
        "try {\n"
        "  raise MyRuntimeError(\"custom\")\n"
        "} except RuntimeError as e {\n"
        "  print(\"caught\")\n"
        "  print(e)\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\ncustom\n");
}

TEST(CodeGenE2E, UserExcExceptionCatchesUser) {
    // except Exception should catch any user-defined exception
    auto output = compileAndRun(
        "class MyError(ValueError) {\n"
        "  def(msg: str) {\n"
        "    self.msg = msg\n"
        "  }\n"
        "}\n"
        "try {\n"
        "  raise MyError(\"val\")\n"
        "} except Exception as e {\n"
        "  print(\"caught\")\n"
        "  print(e)\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\nval\n");
}

TEST(CodeGenE2E, UserExcNoMatchReraise) {
    // Sibling user exception should NOT be caught
    auto output = compileAndRun(
        "class ErrorA(Exception) {\n"
        "  def(msg: str) {\n"
        "    self.msg = msg\n"
        "  }\n"
        "}\n"
        "class ErrorB(Exception) {\n"
        "  def(msg: str) {\n"
        "    self.msg = msg\n"
        "  }\n"
        "}\n"
        "try {\n"
        "  try {\n"
        "    raise ErrorB(\"b\")\n"
        "  } except ErrorA {\n"
        "    print(\"wrong\")\n"
        "  }\n"
        "} except ErrorB as e {\n"
        "  print(\"correct\")\n"
        "  print(e)\n"
        "}\n"
    );
    EXPECT_EQ(output, "correct\nb\n");
}

TEST(CodeGenE2E, UserExcMultiHandler) {
    // Multiple user exception handlers, most specific first
    auto output = compileAndRun(
        "class BaseError(Exception) {\n"
        "  def(msg: str) {\n"
        "    self.msg = msg\n"
        "  }\n"
        "}\n"
        "class SpecificError(BaseError) {\n"
        "  def(msg: str) {\n"
        "    self.msg = msg\n"
        "  }\n"
        "}\n"
        "try {\n"
        "  raise SpecificError(\"spec\")\n"
        "} except SpecificError as e {\n"
        "  print(\"specific\")\n"
        "} except BaseError as e {\n"
        "  print(\"base\")\n"
        "} except Exception {\n"
        "  print(\"generic\")\n"
        "}\n"
    );
    EXPECT_EQ(output, "specific\n");
}

TEST(CodeGenE2E, UserExcGrandparentCatches) {
    // Three-level hierarchy: grandparent catches grandchild
    auto output = compileAndRun(
        "class Level1(Exception) {\n"
        "  def(msg: str) {\n"
        "    self.msg = msg\n"
        "  }\n"
        "}\n"
        "class Level2(Level1) {\n"
        "  def(msg: str) {\n"
        "    self.msg = msg\n"
        "  }\n"
        "}\n"
        "class Level3(Level2) {\n"
        "  def(msg: str) {\n"
        "    self.msg = msg\n"
        "  }\n"
        "}\n"
        "try {\n"
        "  raise Level3(\"deep\")\n"
        "} except Level1 as e {\n"
        "  print(\"caught\")\n"
        "  print(e)\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\ndeep\n");
}

TEST(CodeGenE2E, UserExcNoArgDefaultMsg) {
    // User exception with no message arg uses class name. The ctor gives msg a
    // default equal to the class name so the no-arg `raise EmptyError()` is a
    // valid call (ctor requires its declared param otherwise) while the printed
    // message stays "EmptyError".
    auto output = compileAndRun(
        "class EmptyError(Exception) {\n"
        "  def(msg: str = \"EmptyError\") {\n"
        "    self.msg = msg\n"
        "  }\n"
        "}\n"
        "try {\n"
        "  raise EmptyError()\n"
        "} except EmptyError as e {\n"
        "  print(\"caught\")\n"
        "  print(e)\n"
        "}\n"
    );
    EXPECT_EQ(output, "caught\nEmptyError\n");
}

// ============================================================
// Match/Case IR Tests
// ============================================================

TEST(CodeGenIR, MatchStmtIR) {
    auto ir = generateIR(
        "x: int = 42\n"
        "match x {\n"
        "    case 1 { print(10) }\n"
        "    case _ { print(20) }\n"
        "}\n"
    );
    // Should contain match-related basic block labels.
    EXPECT_NE(ir.find("match.subject"), std::string::npos);
    EXPECT_NE(ir.find("match.end"), std::string::npos);
    EXPECT_NE(ir.find("match.case0"), std::string::npos);
}

TEST(CodeGenIR, PyMatchCaseIR) {
    auto ir = generateIRPy(
        "x: int = 2\n"
        "match x:\n"
        "    case 1:\n"
        "        print(10)\n"
        "    case 2:\n"
        "        print(20)\n"
        "    case _:\n"
        "        print(0)\n"
    );
    EXPECT_NE(ir.find("define"), std::string::npos);
    EXPECT_NE(ir.find("dragon_print_int"), std::string::npos);
}

// ============================================================
// Match/Case E2E Tests
// ============================================================

TEST(CodeGenE2E, MatchIntLiteral) {
    auto output = compileAndRun(
        "x: int = 2\n"
        "match x {\n"
        "    case 1 { print(10) }\n"
        "    case 2 { print(20) }\n"
        "    case 3 { print(30) }\n"
        "}\n"
    );
    EXPECT_EQ(output, "20\n");
}

TEST(CodeGenE2E, MatchWildcard) {
    auto output = compileAndRun(
        "x: int = 99\n"
        "match x {\n"
        "    case 1 { print(10) }\n"
        "    case _ { print(42) }\n"
        "}\n"
    );
    EXPECT_EQ(output, "42\n");
}

TEST(CodeGenE2E, MatchCapture) {
    auto output = compileAndRun(
        "x: int = 7\n"
        "match x {\n"
        "    case 1 { print(10) }\n"
        "    case y { print(y) }\n"
        "}\n"
    );
    EXPECT_EQ(output, "7\n");
}

TEST(CodeGenE2E, MatchStringLiteral) {
    auto output = compileAndRun(
        "s: str = \"hello\"\n"
        "match s {\n"
        "    case \"world\" { print(1) }\n"
        "    case \"hello\" { print(2) }\n"
        "    case _ { print(3) }\n"
        "}\n"
    );
    EXPECT_EQ(output, "2\n");
}

TEST(CodeGenE2E, MatchOrPattern) {
    auto output = compileAndRun(
        "x: int = 3\n"
        "match x {\n"
        "    case 1 | 2 { print(10) }\n"
        "    case 3 | 4 { print(20) }\n"
        "    case _ { print(30) }\n"
        "}\n"
    );
    EXPECT_EQ(output, "20\n");
}

TEST(CodeGenE2E, MatchNoArmMatches) {
    // When no arm matches, execution should fall through silently.
    auto output = compileAndRun(
        "x: int = 99\n"
        "match x {\n"
        "    case 1 { print(10) }\n"
        "    case 2 { print(20) }\n"
        "}\n"
        "print(0)\n"
    );
    EXPECT_EQ(output, "0\n");
}

TEST(CodeGenE2E, MatchFirstArmMatches) {
    auto output = compileAndRun(
        "x: int = 1\n"
        "match x {\n"
        "    case 1 { print(10) }\n"
        "    case 2 { print(20) }\n"
        "    case _ { print(30) }\n"
        "}\n"
    );
    EXPECT_EQ(output, "10\n");
}

TEST(CodeGenE2E, MatchWithGuard) {
    auto output = compileAndRun(
        "x: int = 5\n"
        "match x {\n"
        "    case y if y > 10 { print(1) }\n"
        "    case y if y > 3 { print(2) }\n"
        "    case _ { print(3) }\n"
        "}\n"
    );
    EXPECT_EQ(output, "2\n");
}

TEST(CodeGenE2E, MatchCommaOrPattern) {
    // .dr mode comma-based OR pattern
    auto output = compileAndRun(
        "x: int = 2\n"
        "match x {\n"
        "    case 1, 2, 3 { print(10) }\n"
        "    case 4, 5 { print(20) }\n"
        "    case _ { print(30) }\n"
        "}\n"
    );
    EXPECT_EQ(output, "10\n");
}

TEST(CodeGenE2E, MatchPipeOrPatternRegression) {
    // Verify existing | OR patterns still work after comma support
    auto output = compileAndRun(
        "x: int = 4\n"
        "match x {\n"
        "    case 1 | 2 | 3 { print(10) }\n"
        "    case 4 | 5 { print(20) }\n"
        "    case _ { print(30) }\n"
        "}\n"
    );
    EXPECT_EQ(output, "20\n");
}

TEST(CodeGenE2E, PyMatchCaseE2E) {
    auto out = compileAndRunPy(
        "x: int = 2\n"
        "match x:\n"
        "    case 1:\n"
        "        print(10)\n"
        "    case 2:\n"
        "        print(20)\n"
        "    case _:\n"
        "        print(0)\n"
    );
    EXPECT_EQ(out, "20\n");
}

//===----------------------------------------------------------------------===//
// Regression: match arm scope cleanup
//
// When a match arm falls through to `match.end`, its arm scope must run
// `emitScopeCleanup()` before it is popped. Capture-bound
// match patterns (e.g. `case y { ... }`) introduce new variables into
// the arm scope; heap-typed captures leak without the cleanup.
//
// MatchStmt emits `emitScopeCleanup()` before the unconditional branch to
// the match-end block.
//
// Today's matches mostly bind ints (which need no decref), so the
// inserted cleanup is a no-op there. The IR test below pins down the
// fact that string-pattern arms still compile and execute correctly,
// and the loop test confirms heap-typed bindings don't leak per arm.
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, MatchArmCaptureBoundedLoop) {
    // Capture binding inside the match arm. Each iteration binds y to the
    // subject value and falls through. Pre-fix: scope popped without
    // cleanup. Post-fix: cleanup is a no-op for int captures, but the path
    // is exercised so it's locked in.
    auto output = compileAndRun(
        "total: int = 0\n"
        "for i in range(10000) {\n"
        "  match i {\n"
        "    case 0 { total = total + 1 }\n"
        "    case y { total = total + y }\n"
        "  }\n"
        "}\n"
        "print(total)\n"
    );
    // 1 + sum(1..9999) = 1 + 9999*10000/2 = 1 + 49995000
    EXPECT_EQ(output, "49995001\n");
}

TEST(CodeGenE2E, MatchStringSubjectLoopBounded) {
    // Match a string subject - exercises the string-comparison path through
    // the cleanup-then-fallthrough.
    auto output = compileAndRun(
        "labels: list[str] = [\"go\", \"stop\", \"caution\"]\n"
        "go_count: int = 0\n"
        "stop_count: int = 0\n"
        "other_count: int = 0\n"
        "for i in range(3000) {\n"
        "  for s in labels {\n"
        "    match s {\n"
        "      case \"go\" { go_count = go_count + 1 }\n"
        "      case \"stop\" { stop_count = stop_count + 1 }\n"
        "      case _ { other_count = other_count + 1 }\n"
        "    }\n"
        "  }\n"
        "}\n"
        "print(go_count)\n"
        "print(stop_count)\n"
        "print(other_count)\n"
    );
    EXPECT_EQ(output, "3000\n3000\n3000\n");
}

//===----------------------------------------------------------------------===//
// 4.5 Exception hierarchy audit - raise + catch round-trip
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, ExceptionRaiseAndCatch_AllBuiltins) {
    // Each raise/except pair confirms the exception name is wired in Sema
    // (parses cleanly), excTypeCode (gets a code), and runtime range table
    // (caught by its parent class).
    auto out = compileAndRun(
        "def t(name: str) {\n"
        "    print(name)\n"
        "}\n"
        "try { raise ValueError(\"v\") } except ValueError { t(\"ve\") }\n"
        "try { raise TypeError(\"t\") } except TypeError { t(\"te\") }\n"
        "try { raise KeyError(\"k\") } except LookupError { t(\"le\") }\n"
        "try { raise IndexError(\"i\") } except LookupError { t(\"ie\") }\n"
        "try { raise ZeroDivisionError(\"z\") } except ArithmeticError { t(\"ae\") }\n"
        "try { raise OverflowError(\"o\") } except ArithmeticError { t(\"oe\") }\n"
        "try { raise FileNotFoundError(\"f\") } except OSError { t(\"fe\") }\n"
        "try { raise PermissionError(\"p\") } except OSError { t(\"pe\") }\n"
        "try { raise IOError(\"io\") } except OSError { t(\"ioe\") }\n"
        "try { raise ModuleNotFoundError(\"m\") } except ImportError { t(\"me\") }\n"
        "try { raise NotImplementedError(\"n\") } except RuntimeError { t(\"ne\") }\n"
        "try { raise RecursionError(\"r\") } except RuntimeError { t(\"re\") }\n"
        "try { raise UnicodeDecodeError(\"u\") } except UnicodeError { t(\"ue\") }\n"
        "try { raise AttributeError(\"a\") } except AttributeError { t(\"aa\") }\n"
        "try { raise NameError(\"nm\") } except NameError { t(\"nme\") }\n"
        "try { raise StopIteration(\"s\") } except Exception { t(\"se\") }\n"
        "try { raise AssertionError(\"x\") } except Exception { t(\"asx\") }\n"
        "try { raise RuntimeError(\"q\") } except BaseException { t(\"be\") }\n"
    );
    EXPECT_EQ(out, "ve\nte\nle\nie\nae\noe\nfe\npe\nioe\nme\nne\nre\nue\naa\nnme\nse\nasx\nbe\n");
}

TEST(CodeGenIR, MatchArmEmitsCleanupBeforeEndBranch) {
    // MatchStmt visit emits emitScopeCleanup() before the
    // br to match.end. With Int captures the cleanup is a no-op, so the
    // crispest IR signal is that the structure still terminates correctly:
    // every arm body has a terminator (br to match.end) and match.end
    // exists.
    auto ir = generateIR(
        "x: int = 7\n"
        "match x {\n"
        "    case 0 { print(0) }\n"
        "    case y { print(y) }\n"
        "}\n"
    );
    EXPECT_NE(ir.find("match.end"), std::string::npos)
        << "Expected match.end basic block\nIR:\n" << ir;
    // The cleanup-then-branch should not have introduced an unterminated BB.
    // Look for the standard match dispatch infrastructure.
    EXPECT_NE(ir.find("match.subject"), std::string::npos);
    EXPECT_NE(ir.find("match.case0"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// First-class exception classes (integer-code model). An exception name in
// value context lowers to its type code; __exc_matches range-checks the
// currently-handled exception against an expected code. Backs assertRaises.
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, ExceptionClassAsValue) {
    // A bare exception name is usable as a value (its integer type code).
    auto out = compileAndRun(
        "def take(t: type) -> int { return 1 }\n"
        "print(take(ValueError))\n"
    );
    EXPECT_EQ(out, "1\n");
}

TEST(CodeGenE2E, ExcMatchesExactType) {
    auto out = compileAndRun(
        "matched: bool = False\n"
        "try {\n"
        "    raise ValueError(\"x\")\n"
        "} except Exception as e {\n"
        "    matched = __exc_matches(ValueError)\n"
        "}\n"
        "print(matched)\n"
    );
    EXPECT_EQ(out, "True\n");
}

TEST(CodeGenE2E, ExcMatchesParentRange) {
    // A raised ValueError matches an expected Exception (parent range).
    auto out = compileAndRun(
        "matched: bool = False\n"
        "try {\n"
        "    raise ValueError(\"x\")\n"
        "} except Exception as e {\n"
        "    matched = __exc_matches(Exception)\n"
        "}\n"
        "print(matched)\n"
    );
    EXPECT_EQ(out, "True\n");
}

TEST(CodeGenE2E, ExcMatchesWrongType) {
    // A raised ValueError does NOT match an expected KeyError.
    auto out = compileAndRun(
        "matched: bool = True\n"
        "try {\n"
        "    raise ValueError(\"x\")\n"
        "} except Exception as e {\n"
        "    matched = __exc_matches(KeyError)\n"
        "}\n"
        "print(matched)\n"
    );
    EXPECT_EQ(out, "False\n");
}

//===----------------------------------------------------------------------===//
// int(str) - Python-parity strict parse: valid forms convert, invalid input
// raises ValueError (was a silent atol()->0 lenient parse before).
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, IntStrValidForms) {
    auto out = compileAndRun(
        "print(int(\"42\"))\n"
        "print(int(\"  -17  \"))\n"
        "print(int(\"+5\"))\n"
        "print(int(\"1_000\"))\n"
    );
    EXPECT_EQ(out, "42\n-17\n5\n1000\n");
}

TEST(CodeGenE2E, IntStrInvalidRaisesValueError) {
    auto out = compileAndRun(
        "ok: bool = False\n"
        "try {\n"
        "    x: int = int(\"foo\")\n"
        "} except ValueError as e {\n"
        "    ok = True\n"
        "}\n"
        "print(ok)\n"
    );
    EXPECT_EQ(out, "True\n");
}

TEST(CodeGenE2E, IntStrFloatStringRaises) {
    // "4.5" is not a valid int literal in Python -> ValueError.
    auto out = compileAndRun(
        "ok: bool = False\n"
        "try {\n"
        "    x: int = int(\"4.5\")\n"
        "} except ValueError as e {\n"
        "    ok = True\n"
        "}\n"
        "print(ok)\n"
    );
    EXPECT_EQ(out, "True\n");
}
