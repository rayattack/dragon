#include "CodeGenTestHelpers.h"

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
    auto ir = generateIR(
        "try {\n"
        "    x: int = 1\n"
        "} except* ValueError as e {\n"
        "    x: int = 2\n"
        "}\n"
    );
    EXPECT_NE(ir.find("define"), std::string::npos);
}

TEST(CodeGenTest, ExcHierarchyMatchCallIR) {
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
    auto ir = generateIR(
        "try {\n"
        "  print(1)\n"
        "} except Exception {\n"
        "  print(2)\n"
        "}\n"
    );
    EXPECT_NE(ir.find("dragon_exc_matches"), std::string::npos);
}

TEST(CodeGenTest, UserExcRegisterCallIR) {
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

TEST(CodeGenIR, MatchStmtIR) {
    auto ir = generateIR(
        "x: int = 42\n"
        "match x {\n"
        "    case 1 { print(10) }\n"
        "    case _ { print(20) }\n"
        "}\n"
    );
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

TEST(CodeGenE2E, MatchArmCaptureBoundedLoop) {
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
    EXPECT_EQ(output, "49995001\n");
}

TEST(CodeGenE2E, MatchStringSubjectLoopBounded) {
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

TEST(CodeGenE2E, ExceptionRaiseAndCatch_AllBuiltins) {
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
    auto ir = generateIR(
        "x: int = 7\n"
        "match x {\n"
        "    case 0 { print(0) }\n"
        "    case y { print(y) }\n"
        "}\n"
    );
    EXPECT_NE(ir.find("match.end"), std::string::npos)
        << "Expected match.end basic block\nIR:\n" << ir;
    EXPECT_NE(ir.find("match.subject"), std::string::npos);
    EXPECT_NE(ir.find("match.case0"), std::string::npos);
}

TEST(CodeGenE2E, ExceptionClassAsValue) {
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
