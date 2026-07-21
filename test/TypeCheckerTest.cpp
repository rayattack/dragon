#include <gtest/gtest.h>
#include "TestHelpers.h"
#include "dragon/Privacy.h"

using namespace dragon;
using namespace dragon::test;

// Helper to type check and expect no errors
static bool checkOk(const std::string& source) {
    auto module = parse(source);
    if (!module) return false;
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    return tc.check(*module);
}

// Helper to type check and expect errors
static bool checkHasErrors(const std::string& source) {
    auto module = parse(source);
    if (!module) return false;
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    return tc.hasErrors();
}

// Helper to get the type of an expression after type checking
static std::shared_ptr<Type> getExprType(const std::string& source) {
    auto module = parse(source);
    if (!module || module->body.empty()) return nullptr;
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    // Get the last expression statement
    for (int i = static_cast<int>(module->body.size()) - 1; i >= 0; --i) {
        if (auto* es = dynamic_cast<ExprStmt*>(module->body[i].get())) {
            return es->expr ? es->expr->type : nullptr;
        }
    }
    return nullptr;
}

//===----------------------------------------------------------------------===//
// Basic Tests
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, EmptyModule) {
    EXPECT_TRUE(checkOk(""));
}

TEST(TypeCheckerTest, PassStatement) {
    EXPECT_TRUE(checkOk("pass"));
}

TEST(TypeCheckerTest, IntegerLiteral) {
    EXPECT_TRUE(checkOk("42"));
}

TEST(TypeCheckerTest, FloatLiteral) {
    EXPECT_TRUE(checkOk("3.14"));
}

TEST(TypeCheckerTest, StringLiteral) {
    EXPECT_TRUE(checkOk("\"hello\""));
}

TEST(TypeCheckerTest, BooleanLiteral) {
    EXPECT_TRUE(checkOk("True"));
}

TEST(TypeCheckerTest, NoneLiteral) {
    EXPECT_TRUE(checkOk("None"));
}

//===----------------------------------------------------------------------===//
// Literal Type Inference
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, IntegerLiteralType) {
    auto t = getExprType("42");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
    EXPECT_EQ(t->toString(), "int");
}

TEST(TypeCheckerTest, FloatLiteralType) {
    auto t = getExprType("3.14");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Float);
    EXPECT_EQ(t->toString(), "float");
}

TEST(TypeCheckerTest, StringLiteralType) {
    auto t = getExprType("\"hello\"");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Str);
    EXPECT_EQ(t->toString(), "str");
}

TEST(TypeCheckerTest, BoolLiteralType) {
    auto t = getExprType("True");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Bool);
}

TEST(TypeCheckerTest, NoneLiteralType) {
    auto t = getExprType("None");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::None_);
}

//===----------------------------------------------------------------------===//
// Arithmetic Type Rules
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, IntPlusInt) {
    auto t = getExprType("1 + 2");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, IntPlusFloat) {
    auto t = getExprType("1 + 2.0");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Float);
}

TEST(TypeCheckerTest, FloatPlusFloat) {
    auto t = getExprType("1.0 + 2.0");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Float);
}

TEST(TypeCheckerTest, IntMinusInt) {
    auto t = getExprType("5 - 3");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, IntTimesInt) {
    auto t = getExprType("3 * 4");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, IntTimesFloat) {
    auto t = getExprType("3 * 4.0");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Float);
}

TEST(TypeCheckerTest, TrueDivisionAlwaysFloat) {
    auto t = getExprType("10 / 3");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Float);
}

TEST(TypeCheckerTest, FloorDivIntInt) {
    auto t = getExprType("10 // 3");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, FloorDivIntFloat) {
    auto t = getExprType("10 // 3.0");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Float);
}

TEST(TypeCheckerTest, ModuloIntInt) {
    auto t = getExprType("10 % 3");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, PowerIntInt) {
    auto t = getExprType("2 ** 10");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, StringConcatenation) {
    auto t = getExprType("\"hello\" + \" world\"");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Str);
}

TEST(TypeCheckerTest, StringRepetition) {
    auto t = getExprType("\"ha\" * 3");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Str);
}

TEST(TypeCheckerTest, IntPlusStringError) {
    EXPECT_TRUE(checkHasErrors("1 + \"hello\""));
}

//===----------------------------------------------------------------------===//
// Function values are not containers (the sys.argv `argv[1]` mistake).
// Subscript, len(), and iteration over a function value previously fell
// through to Unknown and miscompiled into garbage reads; each must be a
// compile error that says "call it first".
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, FunctionValueSubscriptRejected) {
    EXPECT_TRUE(checkHasErrors(
        "def args() -> list[str] {\n"
        "    return [\"a\"]\n"
        "}\n"
        "x: str = args[1]\n"));
}

TEST(TypeCheckerTest, FunctionValueLenRejected) {
    EXPECT_TRUE(checkHasErrors(
        "def args() -> list[str] {\n"
        "    return [\"a\"]\n"
        "}\n"
        "n: int = len(args)\n"));
}

TEST(TypeCheckerTest, FunctionValueIterationRejected) {
    EXPECT_TRUE(checkHasErrors(
        "def args() -> list[str] {\n"
        "    return [\"a\"]\n"
        "}\n"
        "for a in args {\n"
        "    print(a)\n"
        "}\n"));
}

TEST(TypeCheckerTest, CalledFunctionResultStaysUsable) {
    // The corrected forms must all stay legal.
    EXPECT_FALSE(checkHasErrors(
        "def args() -> list[str] {\n"
        "    return [\"a\"]\n"
        "}\n"
        "x: str = args()[0]\n"
        "n: int = len(args())\n"
        "for a in args() {\n"
        "    print(a)\n"
        "}\n"));
}

//===----------------------------------------------------------------------===//
// List variance (expected-type-directed covariance for FRESH literals only)
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, FreshListLiteralCovariantToBase) {
    // A fresh literal of subclass instances is assignable to list[Base].
    EXPECT_FALSE(checkHasErrors(
        "class Animal { def() {} }\n"
        "class Dog(Animal) { def() {} }\n"
        "pets: list[Animal] = [Dog()]\n"));
}

TEST(TypeCheckerTest, NamedListStaysInvariant) {
    // A named (aliasable, mutable) list[Dog] is NOT assignable to
    // list[Animal] - that would be the unsound blanket-covariance hole.
    EXPECT_TRUE(checkHasErrors(
        "class Animal { def() {} }\n"
        "class Dog(Animal) { def() {} }\n"
        "xs: list[Dog] = [Dog()]\n"
        "ys: list[Animal] = xs\n"));
}

TEST(TypeCheckerTest, FreshListNonSubclassRejected) {
    // Covariance only fires when every element IS a subtype of the base.
    EXPECT_TRUE(checkHasErrors(
        "class Animal { def() {} }\n"
        "class Plant { def() {} }\n"
        "pets: list[Animal] = [Plant()]\n"));
}

//===----------------------------------------------------------------------===//
// Heterogeneous literal rejection (per-element check beats first-elem inference)
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, HeterogeneousListLiteralRejected) {
    // First-element inference said list[int]; the str element must still be
    // caught (previously silently dropped -> garbage at runtime).
    EXPECT_TRUE(checkHasErrors("xs: list[int] = [1, 2, \"three\"]\n"));
}

TEST(TypeCheckerTest, HomogeneousListLiteralOk) {
    EXPECT_FALSE(checkHasErrors("xs: list[int] = [1, 2, 3]\n"));
}

TEST(TypeCheckerTest, ListLiteralIntToFloatPromotionOk) {
    // int <: float widening inside a list[float] literal stays valid.
    EXPECT_FALSE(checkHasErrors("xs: list[float] = [1, 2.0, 3]\n"));
}

TEST(TypeCheckerTest, ListLiteralAnyAcceptsHeterogeneous) {
    EXPECT_FALSE(checkHasErrors("xs: list[Any] = [1, \"two\", 3.0]\n"));
}

//===----------------------------------------------------------------------===//
// List representation invariance wrt Any: list[T] and list[Any] have
// different element layouts (monomorphized 8B/elem vs boxed 16B/elem), so a
// concrete list VALUE must never flow as list[Any] - only a fresh literal is
// admitted, by being retyped and BUILT as a box list.
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, NamedConcreteListNotAssignableToListAny) {
    EXPECT_TRUE(checkHasErrors(
        "names: list[str] = [\"a\", \"b\"]\n"
        "xs: list[Any] = names\n"));
}

TEST(TypeCheckerTest, ConcreteListArgNotAssignableToListAnyParam) {
    EXPECT_TRUE(checkHasErrors(
        "def first(xs: list[Any]) -> int {\n"
        "    return len(xs)\n"
        "}\n"
        "def run() -> None {\n"
        "    names: list[str] = [\"a\", \"b\"]\n"
        "    n: int = first(names)\n"
        "}\n"));
}

TEST(TypeCheckerTest, FreshLiteralStillAssignableToListAny) {
    EXPECT_FALSE(checkHasErrors("xs: list[Any] = [\"a\", \"b\"]\n"));
}

TEST(TypeCheckerTest, FreshLiteralArgStillPassableToListAnyParam) {
    EXPECT_FALSE(checkHasErrors(
        "def first(xs: list[Any]) -> int {\n"
        "    return len(xs)\n"
        "}\n"
        "def run() -> None {\n"
        "    n: int = first([\"a\", \"b\"])\n"
        "}\n"));
}

TEST(TypeCheckerTest, ListAnyNotAssignableToConcreteList) {
    // The reverse view is equally unsound (boxed elements read natively).
    EXPECT_TRUE(checkHasErrors(
        "xs: list[Any] = [\"a\", \"b\"]\n"
        "names: list[str] = xs\n"));
}

TEST(TypeCheckerTest, DictValueCovarianceToAnyStillAllowed) {
    // Dicts have ONE uniform tagged representation - dict[K, V] may still
    // flow as dict[K, Any].
    EXPECT_FALSE(checkHasErrors(
        "m: dict[str, int] = {\"a\": 1}\n"
        "d: dict[str, Any] = m\n"));
}

//===----------------------------------------------------------------------===//
// Positional argument type checking (catches str passed to int param, etc.)
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, StrArgToIntParamRejected) {
    EXPECT_TRUE(checkHasErrors(
        "def sq(n: int) -> int { return n * n }\n"
        "sq(\"x\")\n"));
}

TEST(TypeCheckerTest, FloatArgToIntParamRejected) {
    EXPECT_TRUE(checkHasErrors(
        "def g(n: int) -> int { return n + 1 }\n"
        "g(3.5)\n"));
}

TEST(TypeCheckerTest, ScalarArgToContainerParamRejected) {
    EXPECT_TRUE(checkHasErrors(
        "def f(xs: list[int]) -> int { return len(xs) }\n"
        "f(\"hello\")\n"));
}

TEST(TypeCheckerTest, IntArgToFloatParamOk) {
    // int <: float promotion at the call boundary.
    EXPECT_FALSE(checkHasErrors(
        "def area(w: float, h: float) -> float { return w * h }\n"
        "area(3, 4)\n"));
}

TEST(TypeCheckerTest, SubclassArgToBaseParamOk) {
    EXPECT_FALSE(checkHasErrors(
        "class Animal { def() {} }\n"
        "class Dog(Animal) { def() {} }\n"
        "def greet(a: Animal) -> int { return 1 }\n"
        "d: Dog = Dog()\n"
        "greet(d)\n"));
}

TEST(TypeCheckerTest, FreshListLiteralArgToBaseListParamOk) {
    // The unittest `main([SubTest()])` idiom: a fresh list literal of subclass
    // instances passed to a list[Base] param must not trip the arg check
    // (containers are invariant, but a fresh literal is covariant-sound).
    EXPECT_FALSE(checkHasErrors(
        "class Animal { def() {} }\n"
        "class Dog(Animal) { def() {} }\n"
        "def feed(pets: list[Animal]) -> int { return len(pets) }\n"
        "feed([Dog()])\n"));
}

// A generic call consults its args for type-parameter inference but previously
// never type-checked the CONCRETE (non-type-variable) params - so a bad arg to
// a non-generic param (e.g. `str` where the `sql: SQL` of `db.all[T](sql: SQL)`
// is declared) slipped past `check` and only failed at codegen. The concrete
// param `tag: str` here stands in for that SQL param without needing stdlib.
TEST(TypeCheckerTest, GenericCallConcreteParamMismatchRejected) {
    EXPECT_TRUE(checkHasErrors(
        "def first[T](xs: list[T], tag: str) -> T { return xs[0] }\n"
        "first([1, 2], 5)\n"));
}

TEST(TypeCheckerTest, GenericCallConcreteParamOkStillInfers) {
    // The type-variable param (list[T]) is fixed by inference and must not be
    // flagged; the concrete `tag: str` matches, so no error.
    EXPECT_FALSE(checkHasErrors(
        "def first[T](xs: list[T], tag: str) -> T { return xs[0] }\n"
        "first([1, 2], \"label\")\n"));
}

// match class/type-test patterns: `case int()` is a supported type test, but
// field destructuring and unknown type names must error cleanly at check time
// (not parser-cascade or silently never-match).
TEST(TypeCheckerTest, MatchTypeTestPatternOk) {
    EXPECT_FALSE(checkHasErrors(
        "v: int | str = 5\n"
        "match v {\n"
        "    case int() { print(\"i\") }\n"
        "    case str() { print(\"s\") }\n"
        "}\n"));
}

// Positional field destructuring (`case Point(a, b)`) is now supported - the
// behavior is dogfooded in test/dr/test_match_type_patterns.dr. What must still
// be REJECTED is a pattern with MORE sub-patterns than the class has fields
// (a must-not-compile check, so it stays a gtest).
TEST(TypeCheckerTest, MatchClassPatternArityMismatchRejected) {
    EXPECT_TRUE(checkHasErrors(
        "class Point { def(x: int) { self.x = x } }\n"
        "p: Point = Point(1)\n"
        "match p { case Point(a, b, c) { print(\"x\") } }\n"));
}

TEST(TypeCheckerTest, MatchClassPatternUnknownTypeRejected) {
    EXPECT_TRUE(checkHasErrors(
        "x: int = 5\n"
        "match x { case Nope() { print(\"x\") } }\n"));
}

// #2 exhaustiveness: a match over a CLOSED scrutinee (union / bool) with no
// catch-all must cover every case, else a value silently falls through.
TEST(TypeCheckerTest, MatchNonExhaustiveUnionRejected) {
    EXPECT_TRUE(checkHasErrors(
        "v: int | str = 5\n"
        "match v {\n"
        "    case int() { print(\"i\") }\n"
        "}\n"));
}

TEST(TypeCheckerTest, MatchNonExhaustiveBoolRejected) {
    EXPECT_TRUE(checkHasErrors(
        "b: bool = True\n"
        "match b {\n"
        "    case True { print(\"t\") }\n"
        "}\n"));
}

// (The positive cases - an exhaustive union match with no `_`, and an open-type
// match relying on intentional fall-through - compile and RUN, so they are
// dogfooded in test/dr/test_match_type_patterns.dr, not asserted here.)

// #3 reachability: an arm after an unguarded catch-all, or a duplicate literal,
// is dead.
TEST(TypeCheckerTest, MatchUnreachableAfterWildcardRejected) {
    EXPECT_TRUE(checkHasErrors(
        "x: int = 1\n"
        "match x {\n"
        "    case _ { print(\"any\") }\n"
        "    case 1 { print(\"one\") }\n"
        "}\n"));
}

TEST(TypeCheckerTest, MatchDuplicateLiteralRejected) {
    EXPECT_TRUE(checkHasErrors(
        "x: int = 1\n"
        "match x {\n"
        "    case 1 { print(\"a\") }\n"
        "    case 1 { print(\"b\") }\n"
        "    case _ { print(\"c\") }\n"
        "}\n"));
}

TEST(TypeCheckerTest, StringMinusStringError) {
    EXPECT_TRUE(checkHasErrors("\"a\" - \"b\""));
}

//===----------------------------------------------------------------------===//
// Comparison Operators
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, ComparisonReturnsBool) {
    auto t = getExprType("1 < 2");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Bool);
}

TEST(TypeCheckerTest, EqualityReturnsBool) {
    auto t = getExprType("1 == 2");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Bool);
}

TEST(TypeCheckerTest, NotEqualReturnsBool) {
    auto t = getExprType("1 != 2");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Bool);
}

TEST(TypeCheckerTest, GreaterEqualReturnsBool) {
    auto t = getExprType("1 >= 2");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Bool);
}

//===----------------------------------------------------------------------===//
// Unary Operators
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, UnaryMinusInt) {
    auto t = getExprType("-42");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, UnaryMinusFloat) {
    auto t = getExprType("-3.14");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Float);
}

TEST(TypeCheckerTest, UnaryNotReturnsBool) {
    auto t = getExprType("not True");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Bool);
}

TEST(TypeCheckerTest, UnaryMinusStringError) {
    EXPECT_TRUE(checkHasErrors("-\"hello\""));
}

//===----------------------------------------------------------------------===//
// Bitwise Operators
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, BitwiseAndIntInt) {
    auto t = getExprType("5 & 3");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, BitwiseOrIntInt) {
    auto t = getExprType("5 | 3");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, BitwiseXorIntInt) {
    auto t = getExprType("5 ^ 3");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, LeftShiftIntInt) {
    auto t = getExprType("1 << 3");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, BitwiseStringError) {
    EXPECT_TRUE(checkHasErrors("\"a\" & \"b\""));
}

//===----------------------------------------------------------------------===//
// Type Annotations & Assignments
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, AnnotatedAssignmentOk) {
    EXPECT_TRUE(checkOk("x: int = 5"));
}

TEST(TypeCheckerTest, AnnotatedAssignmentTypeMismatch) {
    EXPECT_TRUE(checkHasErrors("x: int = \"hello\""));
}

TEST(TypeCheckerTest, AnnotatedAssignmentStrOk) {
    EXPECT_TRUE(checkOk("x: str = \"hello\""));
}

TEST(TypeCheckerTest, AnnotatedAssignmentBoolToInt) {
    // bool <: int, so this should be OK
    EXPECT_TRUE(checkOk("x: int = True"));
}

TEST(TypeCheckerTest, AnnotatedAssignmentIntToFloat) {
    // int <: float, so this should be OK
    EXPECT_TRUE(checkOk("x: float = 5"));
}

TEST(TypeCheckerTest, AnnotatedAssignmentFloatToInt) {
    // float is NOT subtype of int
    EXPECT_TRUE(checkHasErrors("x: int = 3.14"));
}

TEST(TypeCheckerTest, AnnotationOnly) {
    EXPECT_TRUE(checkOk("x: int"));
}

TEST(TypeCheckerTest, AnnAssignTypeMismatch) {
    EXPECT_TRUE(checkHasErrors("x: int = \"hello\""));
}

TEST(TypeCheckerTest, AnnAssignNoneToInt) {
    EXPECT_TRUE(checkHasErrors("x: int = None"));
}

//===----------------------------------------------------------------------===//
// Variable Type Tracking
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, VariableTypeFromAssignment) {
    auto module = parse("x = 42\nx");
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    ASSERT_GE(module->body.size(), 2u);
    auto* es = dynamic_cast<ExprStmt*>(module->body[1].get());
    ASSERT_NE(es, nullptr);
    ASSERT_NE(es->expr, nullptr);
    ASSERT_NE(es->expr->type, nullptr);
    EXPECT_EQ(es->expr->type->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, VariableTypeFromAnnotation) {
    auto module = parse("x: str = \"hello\"\nx");
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    ASSERT_GE(module->body.size(), 2u);
    auto* es = dynamic_cast<ExprStmt*>(module->body[1].get());
    ASSERT_NE(es, nullptr);
    ASSERT_NE(es->expr, nullptr);
    ASSERT_NE(es->expr->type, nullptr);
    EXPECT_EQ(es->expr->type->kind(), Type::Kind::Str);
}

//===----------------------------------------------------------------------===//
// Function Type Checking
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, FunctionDeclOk) {
    EXPECT_TRUE(checkOk(
        "def add(x: int, y: int) -> int {\n"
        "  return x + y\n"
        "}"
    ));
}

TEST(TypeCheckerTest, FunctionReturnTypeMismatch) {
    EXPECT_TRUE(checkHasErrors(
        "def foo() -> int {\n"
        "  return \"hello\"\n"
        "}"
    ));
}

TEST(TypeCheckerTest, FunctionReturnNoneFromIntFunc) {
    EXPECT_TRUE(checkHasErrors(
        "def foo() -> int {\n"
        "  return\n"
        "}"
    ));
}

TEST(TypeCheckerTest, FunctionReturnNoneOk) {
    EXPECT_TRUE(checkOk(
        "def foo() -> None {\n"
        "  return\n"
        "}"
    ));
}

TEST(TypeCheckerTest, FunctionCallReturnType) {
    auto module = parse(
        "def add(x: int, y: int) -> int {\n"
        "  return x + y\n"
        "}\n"
        "add(1, 2)"
    );
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    ASSERT_GE(module->body.size(), 2u);
    auto* es = dynamic_cast<ExprStmt*>(module->body[1].get());
    ASSERT_NE(es, nullptr);
    ASSERT_NE(es->expr, nullptr);
    ASSERT_NE(es->expr->type, nullptr);
    EXPECT_EQ(es->expr->type->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, FunctionParamTypes) {
    EXPECT_TRUE(checkOk(
        "def greet(name: str) -> str {\n"
        "  return \"Hello \" + name\n"
        "}"
    ));
}

TEST(TypeCheckerTest, FunctionParamBadReturn) {
    EXPECT_TRUE(checkHasErrors(
        "def foo(x: str) -> int {\n"
        "  return x\n"
        "}"
    ));
}

//===----------------------------------------------------------------------===//
// Collections
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, ListLiteral) {
    auto t = getExprType("[1, 2, 3]");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::List);
    auto& lt = static_cast<ListType&>(*t);
    EXPECT_EQ(lt.elementType->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, EmptyListLiteral) {
    auto t = getExprType("[]");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::List);
}

TEST(TypeCheckerTest, DictLiteral) {
    auto t = getExprType("{\"a\": 1, \"b\": 2}");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Dict);
    auto& dt = static_cast<DictType&>(*t);
    EXPECT_EQ(dt.keyType->kind(), Type::Kind::Str);
    EXPECT_EQ(dt.valueType->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, TupleLiteral) {
    auto t = getExprType("(1, \"a\", True)");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Tuple);
    auto& tt = static_cast<TupleType&>(*t);
    ASSERT_EQ(tt.elementTypes.size(), 3u);
    EXPECT_EQ(tt.elementTypes[0]->kind(), Type::Kind::Int);
    EXPECT_EQ(tt.elementTypes[1]->kind(), Type::Kind::Str);
    EXPECT_EQ(tt.elementTypes[2]->kind(), Type::Kind::Bool);
}

//===----------------------------------------------------------------------===//
// Subscript Typing
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, ListSubscriptType) {
    auto module = parse("x: list[int] = [1, 2, 3]\nx[0]");
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    ASSERT_GE(module->body.size(), 2u);
    auto* es = dynamic_cast<ExprStmt*>(module->body[1].get());
    ASSERT_NE(es, nullptr);
    ASSERT_NE(es->expr, nullptr);
    ASSERT_NE(es->expr->type, nullptr);
    EXPECT_EQ(es->expr->type->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, StringSubscriptType) {
    auto module = parse("s: str = \"hello\"\ns[0]");
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    ASSERT_GE(module->body.size(), 2u);
    auto* es = dynamic_cast<ExprStmt*>(module->body[1].get());
    ASSERT_NE(es, nullptr);
    ASSERT_NE(es->expr, nullptr);
    ASSERT_NE(es->expr->type, nullptr);
    EXPECT_EQ(es->expr->type->kind(), Type::Kind::Str);
}

//===----------------------------------------------------------------------===//
// Type Resolution
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, ResolveListType) {
    EXPECT_TRUE(checkOk("x: list[int] = [1, 2, 3]"));
}

TEST(TypeCheckerTest, ResolveDictType) {
    EXPECT_TRUE(checkOk("x: dict[str, int] = {\"a\": 1}"));
}

TEST(TypeCheckerTest, ResolveUnknownType) {
    EXPECT_TRUE(checkHasErrors("x: Foo = 5"));
}

//===----------------------------------------------------------------------===//
// Subtyping
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, BoolSubtypeOfInt) {
    auto boolType = std::make_shared<PrimitiveType>(Type::Kind::Bool);
    auto intType = std::make_shared<PrimitiveType>(Type::Kind::Int);
    EXPECT_TRUE(boolType->isSubtypeOf(*intType));
}

TEST(TypeCheckerTest, IntSubtypeOfFloat) {
    auto intType = std::make_shared<PrimitiveType>(Type::Kind::Int);
    auto floatType = std::make_shared<PrimitiveType>(Type::Kind::Float);
    EXPECT_TRUE(intType->isSubtypeOf(*floatType));
}

TEST(TypeCheckerTest, BoolSubtypeOfFloat) {
    auto boolType = std::make_shared<PrimitiveType>(Type::Kind::Bool);
    auto floatType = std::make_shared<PrimitiveType>(Type::Kind::Float);
    EXPECT_TRUE(boolType->isSubtypeOf(*floatType));
}

TEST(TypeCheckerTest, IntNotSubtypeOfStr) {
    auto intType = std::make_shared<PrimitiveType>(Type::Kind::Int);
    auto strType = std::make_shared<PrimitiveType>(Type::Kind::Str);
    EXPECT_FALSE(intType->isSubtypeOf(*strType));
}

TEST(TypeCheckerTest, FloatNotSubtypeOfInt) {
    auto floatType = std::make_shared<PrimitiveType>(Type::Kind::Float);
    auto intType = std::make_shared<PrimitiveType>(Type::Kind::Int);
    EXPECT_FALSE(floatType->isSubtypeOf(*intType));
}

TEST(TypeCheckerTest, SubtypeOfAny) {
    auto intType = std::make_shared<PrimitiveType>(Type::Kind::Int);
    auto anyType = std::make_shared<AnyType>();
    EXPECT_TRUE(intType->isSubtypeOf(*anyType));
}

TEST(TypeCheckerTest, SubtypeOfUnion) {
    auto intType = std::make_shared<PrimitiveType>(Type::Kind::Int);
    auto strType = std::make_shared<PrimitiveType>(Type::Kind::Str);
    std::vector<std::shared_ptr<Type>> types = {intType, strType};
    auto unionType = std::make_shared<UnionType>(std::move(types));
    EXPECT_TRUE(intType->isSubtypeOf(*unionType));
    EXPECT_TRUE(strType->isSubtypeOf(*unionType));
}

//===----------------------------------------------------------------------===//
// Type Equality
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, PrimitiveTypeEquality) {
    auto a = std::make_shared<PrimitiveType>(Type::Kind::Int);
    auto b = std::make_shared<PrimitiveType>(Type::Kind::Int);
    EXPECT_TRUE(a->equals(*b));
}

TEST(TypeCheckerTest, PrimitiveTypeInequality) {
    auto a = std::make_shared<PrimitiveType>(Type::Kind::Int);
    auto b = std::make_shared<PrimitiveType>(Type::Kind::Str);
    EXPECT_FALSE(a->equals(*b));
}

TEST(TypeCheckerTest, ListTypeEquality) {
    auto intType = std::make_shared<PrimitiveType>(Type::Kind::Int);
    auto a = std::make_shared<ListType>(intType);
    auto b = std::make_shared<ListType>(intType);
    EXPECT_TRUE(a->equals(*b));
}

TEST(TypeCheckerTest, FunctionTypeEquality) {
    auto intType = std::make_shared<PrimitiveType>(Type::Kind::Int);
    auto a = std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{intType, intType}, intType);
    auto b = std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{intType, intType}, intType);
    EXPECT_TRUE(a->equals(*b));
}

TEST(TypeCheckerTest, TypeToString) {
    EXPECT_EQ(std::make_shared<PrimitiveType>(Type::Kind::Int)->toString(), "int");
    EXPECT_EQ(std::make_shared<PrimitiveType>(Type::Kind::Float)->toString(), "float");
    EXPECT_EQ(std::make_shared<PrimitiveType>(Type::Kind::Bool)->toString(), "bool");
    EXPECT_EQ(std::make_shared<PrimitiveType>(Type::Kind::Str)->toString(), "str");
    EXPECT_EQ(std::make_shared<PrimitiveType>(Type::Kind::None_)->toString(), "None");

    auto intType = std::make_shared<PrimitiveType>(Type::Kind::Int);
    auto strType = std::make_shared<PrimitiveType>(Type::Kind::Str);
    EXPECT_EQ(std::make_shared<ListType>(intType)->toString(), "list[int]");
    EXPECT_EQ(std::make_shared<DictType>(strType, intType)->toString(), "dict[str, int]");
    EXPECT_EQ(std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{intType, intType}, intType)->toString(),
        "(int, int) -> int");
}

//===----------------------------------------------------------------------===//
// Control Flow
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, IfStatement) {
    EXPECT_TRUE(checkOk(
        "x = 10\n"
        "if x > 5 {\n"
        "  print(x)\n"
        "}"
    ));
}

TEST(TypeCheckerTest, WhileLoop) {
    EXPECT_TRUE(checkOk(
        "x = 0\n"
        "while x < 10 {\n"
        "  x = x + 1\n"
        "}"
    ));
}

TEST(TypeCheckerTest, ForLoop) {
    EXPECT_TRUE(checkOk(
        "for i in range(10) {\n"
        "  print(i)\n"
        "}"
    ));
}

TEST(TypeCheckerTest, TryStatement) {
    EXPECT_TRUE(checkOk(
        "try {\n"
        "  x = 10 / 0\n"
        "} catch Exception as e {\n"
        "  print(\"error\")\n"
        "}"
    ));
}

//===----------------------------------------------------------------------===//
// Class Type Checking
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, ClassDecl) {
    EXPECT_TRUE(checkOk(
        "class Point {\n"
        "  def(x: int, y: int) -> None {\n"
        "    pass\n"
        "  }\n"
        "}"
    ));
}

//===----------------------------------------------------------------------===//
// Implicit Self (Decision 006)
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, ImplicitSelfMethodType) {
    // FunctionType for method should exclude self
    auto module = parse(
        "class Point {\n"
        "  def(x: int, y: int) -> None {\n"
        "    self.x = x\n"
        "    self.y = y\n"
        "  }\n"
        "  def distance() -> float {\n"
        "    return 0.0\n"
        "  }\n"
        "}\n"
    );
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    auto exports = tc.getExports();
    auto it = exports.find("Point");
    ASSERT_NE(it, exports.end());
    auto* classType = dynamic_cast<ClassType*>(it->second.get());
    ASSERT_NE(classType, nullptr);
    // distance() should have 0 params in its FunctionType
    auto methodIt = classType->methods.find("distance");
    ASSERT_NE(methodIt, classType->methods.end());
    auto* funcType = dynamic_cast<FunctionType*>(methodIt->second.get());
    ASSERT_NE(funcType, nullptr);
    EXPECT_EQ(funcType->paramTypes.size(), 0u);
}

TEST(TypeCheckerTest, ImplicitSelfFieldAccess) {
    // self.x should type check correctly inside method
    EXPECT_TRUE(checkOk(
        "class Point {\n"
        "  def(x: int, y: int) -> None {\n"
        "    self.x = x\n"
        "    self.y = y\n"
        "  }\n"
        "}"
    ));
}

TEST(TypeCheckerTest, ExplicitSelfPyModeType) {
    // .py mode: explicit self in params, but FunctionType still excludes self
    auto module = parse(
        "class Point:\n"
        "    def distance(self) -> float:\n"
        "        return 0.0\n",
        /*isDragon=*/false
    );
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    auto exports = tc.getExports();
    auto it = exports.find("Point");
    ASSERT_NE(it, exports.end());
    auto* classType = dynamic_cast<ClassType*>(it->second.get());
    ASSERT_NE(classType, nullptr);
    auto methodIt = classType->methods.find("distance");
    ASSERT_NE(methodIt, classType->methods.end());
    auto* funcType = dynamic_cast<FunctionType*>(methodIt->second.get());
    ASSERT_NE(funcType, nullptr);
    // self is excluded from FunctionType even in .py mode
    EXPECT_EQ(funcType->paramTypes.size(), 0u);
}

TEST(TypeCheckerTest, ClassInstantiation) {
    auto module = parse(
        "class Foo {\n"
        "  pass\n"
        "}\n"
        "Foo()"
    );
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    ASSERT_GE(module->body.size(), 2u);
    auto* es = dynamic_cast<ExprStmt*>(module->body[1].get());
    ASSERT_NE(es, nullptr);
    ASSERT_NE(es->expr, nullptr);
    ASSERT_NE(es->expr->type, nullptr);
    EXPECT_EQ(es->expr->type->kind(), Type::Kind::Instance);
}

//===----------------------------------------------------------------------===//
// Builtin Function Types
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, PrintReturnsNone) {
    auto t = getExprType("print(\"hello\")");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::None_);
}

TEST(TypeCheckerTest, LenReturnsInt) {
    auto t = getExprType("len([1, 2, 3])");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Int);
}

TEST(TypeCheckerTest, InputReturnsStr) {
    auto t = getExprType("input(\"prompt\")");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::Str);
}

TEST(TypeCheckerTest, RangeReturnsList) {
    auto t = getExprType("range(10)");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), Type::Kind::List);
}

//===----------------------------------------------------------------------===//
// Integration Tests
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, FibonacciFunction) {
    EXPECT_TRUE(checkOk(
        "def fib(n: int) -> int {\n"
        "  if n <= 1 {\n"
        "    return n\n"
        "  }\n"
        "  return fib(n - 1) + fib(n - 2)\n"
        "}"
    ));
}

TEST(TypeCheckerTest, AnnotatedVariableUsedInExpression) {
    EXPECT_TRUE(checkOk(
        "x: int = 10\n"
        "y: int = x + 5\n"
        "print(y)"
    ));
}

TEST(TypeCheckerTest, MultipleStatements) {
    EXPECT_TRUE(checkOk(
        "x = 1\n"
        "y = 2\n"
        "z = x + y\n"
        "print(z)"
    ));
}

// Slice type inference tests
TEST(TypeCheckerTest, ListSliceReturnsListType) {
    auto type = getExprType("x: list[int] = [1, 2, 3]\nx[1:3]");
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->kind(), Type::Kind::List);
}

TEST(TypeCheckerTest, StringSliceReturnsStr) {
    auto type = getExprType("s: str = \"hello\"\ns[1:3]");
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->kind(), Type::Kind::Str);
}

TEST(TypeCheckerTest, StringSubscriptReturnsStr) {
    auto type = getExprType("s: str = \"hello\"\ns[0]");
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->kind(), Type::Kind::Str);
}

// New string method type tests
TEST(TypeCheckerTest, StringCasefoldReturnsStr) {
    auto type = getExprType("s: str = \"Hello\"\ns.casefold()");
    ASSERT_NE(type, nullptr);
    if (type->kind() == Type::Kind::Function) {
        auto& ft = static_cast<FunctionType&>(*type);
        EXPECT_EQ(ft.returnType->kind(), Type::Kind::Str);
    }
}

TEST(TypeCheckerTest, StringPartitionReturnsList) {
    auto type = getExprType("s: str = \"hello world\"\ns.partition(\" \")");
    ASSERT_NE(type, nullptr);
    // partition returns a call result, which should be list[str] through the function return
}

TEST(TypeCheckerTest, IsOperatorReturnsBool) {
    auto type = getExprType("x = 5\nx is 5");
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->kind(), Type::Kind::Bool);
}

TEST(TypeCheckerTest, InOperatorReturnsBool) {
    auto type = getExprType("x = 5\nx in [1, 2, 3]");
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->kind(), Type::Kind::Bool);
}

//===----------------------------------------------------------------------===//
// Cross-File Type Checking Tests
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, RegisterExternalModuleResolvesImport) {
    // Parse a module that imports from "utils"
    auto module = parse(
        "from utils import add\n"
        "result: int = add(1, 2)\n"
    );
    ASSERT_NE(module, nullptr);

    Sema sema;
    sema.analyze(*module);

    // Register external module exports
    std::unordered_map<std::string, std::shared_ptr<Type>> exports;
    exports["add"] = std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{
            std::make_shared<PrimitiveType>(Type::Kind::Int),
            std::make_shared<PrimitiveType>(Type::Kind::Int)
        },
        std::make_shared<PrimitiveType>(Type::Kind::Int)
    );

    TypeChecker tc;
    tc.registerExternalModule("utils", exports);
    tc.check(*module);

    // The call to add() should resolve to int return type
    // Find the assignment value
    for (auto& stmt : module->body) {
        if (auto* assign = dynamic_cast<AssignStmt*>(stmt.get())) {
            if (assign->value && assign->value->type) {
                EXPECT_EQ(assign->value->type->kind(), Type::Kind::Int);
            }
        }
    }
}

TEST(TypeCheckerTest, GetExportsReturnsDefinedFunctions) {
    auto module = parse(
        "def multiply(a: int, b: int) -> int {\n"
        "    return a * b\n"
        "}\n"
        "x: int = 42\n"
    );
    ASSERT_NE(module, nullptr);

    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);

    auto exports = tc.getExports();
    // Should contain "multiply" and "x", but not builtins like "print"
    EXPECT_TRUE(exports.count("multiply") > 0);
    EXPECT_TRUE(exports.count("x") > 0);
    EXPECT_TRUE(exports.count("print") == 0);
}

TEST(TypeCheckerTest, UnknownImportProducesError) {
    // Import a name that doesn't exist in the external module
    auto module = parse(
        "from utils import nonexistent\n"
    );
    ASSERT_NE(module, nullptr);

    Sema sema;
    sema.analyze(*module);

    // Register utils with only "add" exported
    std::unordered_map<std::string, std::shared_ptr<Type>> exports;
    exports["add"] = std::make_shared<PrimitiveType>(Type::Kind::Int);

    TypeChecker tc;
    tc.registerExternalModule("utils", exports);
    tc.check(*module);

    // Should have an error about "nonexistent" not found in "utils"
    EXPECT_TRUE(tc.hasErrors());
}

TEST(TypeCheckerTest, CrossModuleTypeInfoFlowsToCallExpr) {
    // Verify that a function imported from an external module
    // gets its return type properly propagated to call expressions
    auto module = parse(
        "from math_utils import square\n"
        "result = square(5)\n"
    );
    ASSERT_NE(module, nullptr);

    Sema sema;
    sema.analyze(*module);

    std::unordered_map<std::string, std::shared_ptr<Type>> exports;
    exports["square"] = std::make_shared<FunctionType>(
        std::vector<std::shared_ptr<Type>>{
            std::make_shared<PrimitiveType>(Type::Kind::Int)
        },
        std::make_shared<PrimitiveType>(Type::Kind::Int)
    );

    TypeChecker tc;
    tc.registerExternalModule("math_utils", exports);
    tc.check(*module);

    // Find the assignment and check the call result type
    for (auto& stmt : module->body) {
        if (auto* assign = dynamic_cast<AssignStmt*>(stmt.get())) {
            if (assign->value) {
                if (auto* call = dynamic_cast<CallExpr*>(assign->value.get())) {
                    ASSERT_NE(call->type, nullptr);
                    EXPECT_EQ(call->type->kind(), Type::Kind::Int);
                }
            }
        }
    }
}

//===----------------------------------------------------------------------===//
// D017 Phase 4 - typed templates (template[X]) protocol enforcement.
// Each `template[X]` content type must extend Template. The reserved-for-
// D037 dispatch hook rejects StructTemplate subclasses with a distinct
// error so users get a clear message before that path ships.
//===----------------------------------------------------------------------===//

static const std::string TPL_BASE_TC =
    "class Template {\n"
    "    def(inner: str) { self._inner = inner }\n"
    "    @staticmethod\n"
    "    def escape(s: str) -> str { return s }\n"
    "}\n";

TEST(TypeCheckerTest, TypedTemplateRequiresTemplateBase) {
    // Bare class without Template base -> must error
    EXPECT_TRUE(checkHasErrors(
        "class Foo {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "}\n"
        "x: str = \"a\"\n"
        "y = template[Foo] {!{x}}\n"
    ));
}

TEST(TypeCheckerTest, TypedTemplateExtendsTemplateOK) {
    // Direct subclass of Template - no errors
    EXPECT_TRUE(checkOk(
        TPL_BASE_TC +
        "class Foo(Template) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "}\n"
        "x: str = \"a\"\n"
        "y = template[Foo] {!{x}}\n"
    ));
}

TEST(TypeCheckerTest, TypedTemplateGrandchildExtendsTemplateOK) {
    // Indirect extension (grandchild) is fine - parent-chain walk finds Template
    EXPECT_TRUE(checkOk(
        TPL_BASE_TC +
        "class HTML(Template) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "}\n"
        "class MyHTML(HTML) {\n"
        "  def(inner: str) { self._inner = inner }\n"
        "}\n"
        "x: str = \"a\"\n"
        "y = template[MyHTML] {!{x}}\n"
    ));
}

TEST(TypeCheckerTest, TypedTemplateUnknownContentTypeErrors) {
    // template[NonExistent] - content type not declared
    EXPECT_TRUE(checkHasErrors(
        "y = template[NoSuchType] {hello}\n"
    ));
}

TEST(TypeCheckerTest, TypedTemplateStructTemplateReservedForD037) {
    // class extending StructTemplate is reserved for the D037 lowering;
    // accepting it silently would mask the missing struct-mode codegen.
    auto module = parse(
        "class StructTemplate {\n"
        "  def() { pass }\n"
        "}\n"
        "class Widget(StructTemplate) {\n"
        "  def() { pass }\n"
        "}\n"
        "y = template[Widget] {hi}\n"
    );
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    ASSERT_TRUE(tc.hasErrors());
    bool sawStructTemplate = false;
    for (auto& d : tc.diagnostics()) {
        if (d.message.find("StructTemplate") != std::string::npos) {
            sawStructTemplate = true;
            break;
        }
    }
    EXPECT_TRUE(sawStructTemplate)
        << "Expected error mentioning StructTemplate reservation";
}

TEST(TypeCheckerTest, UntypedTemplateUnchangedStillStr) {
    // Plain `template { ... }` (no [X]) keeps its str type regardless of
    // any unrelated classes in scope - the protocol check applies only to
    // typed templates.
    auto module = parse("y: str = template {hello}\n");
    ASSERT_NE(module, nullptr);
    Sema sema;
    sema.analyze(*module);
    TypeChecker tc;
    tc.check(*module);
    EXPECT_FALSE(tc.hasErrors());
}

//===----------------------------------------------------------------------===//
// Task[T] - fire / await / async def (D016)
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, FireProducesTaskOfCalleeReturn) {
    EXPECT_TRUE(checkOk(
        "def work() -> int { return 21 }\n"
        "t: Task[int] = fire work()\n"
        "r: int = t.join()\n"));
}

TEST(TypeCheckerTest, AwaitUnwrapsAsyncDefReturn) {
    EXPECT_TRUE(checkOk(
        "async def fetch() -> int { return 99 }\n"
        "r: int = await fetch()\n"));
}

TEST(TypeCheckerTest, BareTaskAnnotationRefinesFromRHS) {
    // `t: Task = fire work()` pins the result type from the concrete RHS,
    // so join() recovers int (assignable to int, not str).
    EXPECT_TRUE(checkOk(
        "def work() -> int { return 1 }\n"
        "t: Task = fire work()\n"
        "r: int = t.join()\n"));
    EXPECT_TRUE(checkHasErrors(
        "def work() -> int { return 1 }\n"
        "t: Task = fire work()\n"
        "r: str = t.join()\n"));
}

TEST(TypeCheckerTest, AwaitIntResultNotAssignableToStr) {
    // The former soundness hole: await produced `unknown`, so this compiled
    // and miscompiled. Now await Task[int] -> int, str assignment is an error.
    EXPECT_TRUE(checkHasErrors(
        "async def fetch() -> int { return 99 }\n"
        "r: str = await fetch()\n"));
}

TEST(TypeCheckerTest, AwaitOnSyncFunctionIsError) {
    EXPECT_TRUE(checkHasErrors(
        "def work() -> int { return 5 }\n"
        "r: int = await work()\n"));
}

TEST(TypeCheckerTest, WrongExplicitTaskParamIsError) {
    EXPECT_TRUE(checkHasErrors(
        "def work() -> int { return 5 }\n"
        "t: Task[str] = fire work()\n"));
}

//===----------------------------------------------------------------------===//
// Dict key-type checks - a dict is monomorphic in its key type, so a key of the
// wrong type is a TYPE error (was a runtime KeyError / late LLVM-verify crash).
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, DictIntKeyIntIndexOk) {
    EXPECT_TRUE(checkOk(
        "d: dict[int, str] = {1: \"a\"}\n"
        "x: str = d[1]\n"));
}

TEST(TypeCheckerTest, DictIntKeyStrIndexIsError) {
    EXPECT_TRUE(checkHasErrors(
        "d: dict[int, str] = {1: \"a\"}\n"
        "x: str = d[\"1\"]\n"));
}

TEST(TypeCheckerTest, DictStrKeyStrIndexOk) {
    EXPECT_TRUE(checkOk(
        "d: dict[str, str] = {\"a\": \"b\"}\n"
        "x: str = d[\"a\"]\n"));
}

TEST(TypeCheckerTest, DictStrKeyIntIndexIsError) {
    EXPECT_TRUE(checkHasErrors(
        "d: dict[str, str] = {\"a\": \"b\"}\n"
        "x: str = d[1]\n"));
}

TEST(TypeCheckerTest, DictWrongKeyTypeOnAssignIsError) {
    // The write side (d[k] = v) is type-checked too.
    EXPECT_TRUE(checkHasErrors(
        "d: dict[int, str] = {1: \"a\"}\n"
        "d[\"1\"] = \"b\"\n"));
}

TEST(TypeCheckerTest, DictHomogeneousLiteralOk) {
    EXPECT_TRUE(checkOk(
        "d: dict[str, int] = {\"a\": 1, \"b\": 2, \"c\": 3}\n"));
}

TEST(TypeCheckerTest, DictMixedKeyLiteralIsError) {
    EXPECT_TRUE(checkHasErrors(
        "d: dict[int, str] = {1: \"a\", \"2\": \"b\"}\n"));
}

//===----------------------------------------------------------------------===//
// D045 - member & module privacy (single-file enforcement) + reserved dunders
//===----------------------------------------------------------------------===//

// Collect error messages from type-checking a single source.
static std::vector<std::string> checkMessages(const std::string& source) {
    std::vector<std::string> out;
    auto module = parse(source);
    if (!module) { out.push_back("PARSE FAIL"); return out; }
    Sema sema; sema.analyze(*module);
    TypeChecker tc; tc.check(*module);
    for (auto& d : tc.diagnostics())
        if (d.level == TypeDiagnostic::Level::Error) out.push_back(d.message);
    return out;
}
static bool msgsContain(const std::vector<std::string>& m, const std::string& needle) {
    for (auto& s : m) if (s.find(needle) != std::string::npos) return true;
    return false;
}

// ---- Positive: legitimate access compiles ----

TEST(TypeCheckerTest, D045_SameClassPrivateAccessOk) {
    EXPECT_TRUE(checkOk(
        "class A {\n"
        "    __secret: int = 5\n"
        "    def() {}\n"
        "    def get() -> int { return self.__secret }\n"
        "}\n"
        "a: A = A()\n"
        "print(a.get())\n"));
}

TEST(TypeCheckerTest, D045_SamePackageProtectedAccessOk) {
    // A free function in the SAME file (= same singleton package) may touch a
    // _protected member.
    EXPECT_TRUE(checkOk(
        "class A {\n"
        "    _shared: int = 5\n"
        "    def() {}\n"
        "}\n"
        "def peek(a: A) -> int { return a._shared }\n"));
}

TEST(TypeCheckerTest, D045_SubclassInheritedProtectedOk) {
    EXPECT_TRUE(checkOk(
        "class Base {\n"
        "    _x: int = 1\n"
        "    def() {}\n"
        "}\n"
        "class Derived(Base) {\n"
        "    def() { super() }\n"
        "    def use() -> int { return self._x }\n"
        "}\n"));
}

TEST(TypeCheckerTest, D045_RecognizedDundersOk) {
    EXPECT_TRUE(checkOk(
        "class A {\n"
        "    def() {}\n"
        "    def __str__() -> str { return \"a\" }\n"
        "    def __eq__(other: A) -> bool { return true }\n"
        "    def __len__() -> int { return 0 }\n"
        "}\n"));
}

TEST(TypeCheckerTest, D045_RecognizedModuleMetadataOk) {
    EXPECT_TRUE(checkOk("__version__: str = \"1.0\"\n"));
}

// ---- Negative: privacy violations are compile errors ----

TEST(TypeCheckerTest, D045_PrivateAcrossClassesRejected) {
    auto m = checkMessages(
        "class A {\n"
        "    __secret: int = 5\n"
        "    def() {}\n"
        "}\n"
        "def peek(a: A) -> int { return a.__secret }\n");
    EXPECT_TRUE(msgsContain(m, "cannot access"));
    EXPECT_TRUE(msgsContain(m, "private"));
}

TEST(TypeCheckerTest, D045_SubclassCannotTouchParentPrivate) {
    // __private is declaring-class-only - a subclass is locked out.
    auto m = checkMessages(
        "class Base {\n"
        "    __secret: int = 1\n"
        "    def() {}\n"
        "}\n"
        "class Derived(Base) {\n"
        "    def() { super() }\n"
        "    def peek() -> int { return self.__secret }\n"
        "}\n");
    EXPECT_TRUE(msgsContain(m, "cannot access"));
}

TEST(TypeCheckerTest, D045_PrivateFromModuleTopLevelRejected) {
    auto m = checkMessages(
        "class A {\n"
        "    __secret: int = 5\n"
        "    def() {}\n"
        "}\n"
        "a: A = A()\n"
        "print(a.__secret)\n");
    EXPECT_TRUE(msgsContain(m, "private to"));
}

TEST(TypeCheckerTest, D045_UnrecognizedClassDunderRejected) {
    auto m = checkMessages(
        "class A {\n"
        "    def() {}\n"
        "    def __frobnicate__() -> int { return 1 }\n"
        "}\n");
    EXPECT_TRUE(msgsContain(m, "reserved"));
    EXPECT_TRUE(msgsContain(m, "not a recognized special method"));
}

TEST(TypeCheckerTest, D045_UnrecognizedModuleDunderRejected) {
    auto m = checkMessages("__weird__: int = 5\n");
    EXPECT_TRUE(msgsContain(m, "not a recognized module metadata name"));
}

// ---- isReservedDunder regression: the predicate is a SUPERSET of every
// dunder the codegen dispatch sites use. If codegen learns a new dunder, this
// test fails until the predicate is extended (and vice-versa keeps it honest).
TEST(TypeCheckerTest, D045_ReservedDunderCoversDispatchedSet) {
    // Enumerated from the codegen dispatch sites (Expressions/Assign/
    // CallBuiltins/Attributes/ForLoop/Exceptions/CallExpr/CodeGenImpl/ImplInit).
    const char* dispatched[] = {
        "__init__", "__str__", "__repr__",
        "__eq__", "__ne__", "__lt__", "__le__", "__gt__", "__ge__", "__hash__",
        "__add__", "__sub__", "__mul__", "__truediv__", "__floordiv__",
        "__mod__", "__pow__",
        "__iadd__", "__isub__", "__imul__", "__itruediv__", "__ifloordiv__",
        "__imod__", "__ipow__",
        "__neg__", "__pos__", "__abs__",
        "__len__", "__getitem__", "__setitem__", "__contains__",
        "__iter__", "__next__",
        "__call__", "__enter__", "__exit__",
        "__bool__", "__int__", "__float__",
        "__doc__", "__members__",
    };
    for (const char* d : dispatched)
        EXPECT_TRUE(isReservedDunder(d)) << "dispatched dunder not reserved: " << d;

    // Names codegen does NOT dispatch must NOT be reserved (no recognized-but-
    // dead wart): bitwise/reflected/etc.
    for (const char* d : {"__and__", "__or__", "__xor__", "__invert__",
                          "__radd__", "__delitem__", "__frob__"})
        EXPECT_FALSE(isReservedDunder(d)) << "unexpectedly reserved: " << d;
}

// classifyName tiers.
TEST(TypeCheckerTest, D045_NameClassification) {
    EXPECT_EQ(classifyName("name"), NameVisibility::Public);
    EXPECT_EQ(classifyName("_"), NameVisibility::Public);      // wildcard
    EXPECT_EQ(classifyName("_x"), NameVisibility::Protected);
    EXPECT_EQ(classifyName("_routes_by_sd"), NameVisibility::Protected);
    EXPECT_EQ(classifyName("__x"), NameVisibility::Private);
    EXPECT_EQ(classifyName("__secret"), NameVisibility::Private);
    EXPECT_EQ(classifyName("__init__"), NameVisibility::ReservedDunder);
    EXPECT_EQ(classifyName("__doc__"), NameVisibility::ReservedDunder);
}

//===----------------------------------------------------------------------===//
// D044 - Generics (monomorphization)
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, GenericClassAndFunctionAccepted) {
    EXPECT_TRUE(checkOk(
        "class Box[T] {\n"
        "    def(v: T) { self.value = v }\n"
        "    def get() -> T { return self.value }\n"
        "}\n"
        "def first[T](xs: list[T]) -> T { return xs[0] }\n"
        "b: Box[int] = Box[int](5)\n"
        "n: int = b.get()\n"
        "xs: list[str] = [\"a\"]\n"
        "s: str = first(xs)\n"));
}

// Unbounded `T`: a method/attribute call on a value of type parameter `T` is
// rejected. A bound (`T: B`) is what lifts this; without one the
// member can't be proven to exist for every `T`.
TEST(TypeCheckerTest, GenericUnboundedMethodCallRejected) {
    EXPECT_TRUE(checkHasErrors(
        "def f[T](t: T) -> int { return t.foo() }\n"
        "x: int = f[int](5)\n"));
}

// A `T`-typed value cannot be subscripted under v1 unbounded rules.
TEST(TypeCheckerTest, GenericUnboundedSubscriptRejected) {
    EXPECT_TRUE(checkHasErrors(
        "def f[T](t: T) -> int { return t[0] }\n"
        "x: int = f[int](5)\n"));
}

// Polymorphic recursion (`Foo[T]` instantiating a strictly deeper `Foo[list[T]]`)
// is bounded by the instantiation-depth cap and reported as a compile error -
// not a silent truncation and, critically, not a hang.
TEST(TypeCheckerTest, GenericPolymorphicRecursionCapped) {
    EXPECT_TRUE(checkHasErrors(
        "class Foo[T] {\n"
        "    def(v: T) { self.v = v }\n"
        "    def deeper() -> Foo[list[T]] { return Foo[list[T]]([self.v]) }\n"
        "}\n"
        "x: Foo[int] = Foo[int](1)\n"));
}

// Wrong type-argument arity on a generic class is rejected.
TEST(TypeCheckerTest, GenericClassArityMismatchRejected) {
    EXPECT_TRUE(checkHasErrors(
        "class Pair[K, V] {\n"
        "    def(k: K, v: V) { self.k = k; self.v = v }\n"
        "}\n"
        "p: Pair[int] = Pair[int](1)\n"));
}

// Unbounded `T`: equality (==/!=) is allowed, but ORDERING (<, >, <=, >=) needs a
// bound and is rejected (D044 lists `t < t` as disallowed).
TEST(TypeCheckerTest, GenericEqualityAllowedOrderingRejected) {
    EXPECT_TRUE(checkOk(
        "def eq[T](a: T, b: T) -> bool { return a == b }\n"
        "x: bool = eq[int](1, 1)\n"));
    EXPECT_TRUE(checkHasErrors(
        "def lt[T](a: T, b: T) -> bool { return a < b }\n"
        "x: bool = lt[int](1, 2)\n"));
}

// Generic METHODS (a method declaring its OWN type parameter) - D044+. The
// POSITIVE behavioral cases (accept + run: explicit/inferred type args, list[T]
// return, inheritance, multi-param, T-construction) live in the .dr E2E suite
// test/dr/test_generics_method.dr. Only compiler REJECTION cases live here -
// a program that must fail to compile cannot be a passing .dr unittest.

// D049 - a bracket-less call infers T from the binding annotation, but a
// genuinely CONTEXT-FREE call (no annotation to infer from, no argument that
// pins T) is a clean error, not a crash. (`x: int = r.make()` would now SUCCEED
// by inferring T=int - that is the optional-[T] feature; the unsolved case is
// the discarded-result statement below.)
TEST(TypeCheckerTest, GenericMethodUnsolvedTypeParamRejected) {
    EXPECT_TRUE(checkHasErrors(
        "class Reg {\n"
        "    def make[T]() -> T { return T() }\n"
        "}\n"
        "r: Reg = Reg()\n"
        "r.make()\n"));
}

// Overloading-by-genericity is rejected: a generic method may not share its name
// with another method on the class (the dual-definition footgun). Optional-[T]
// is ONE generic method whose T is inferred at the call site.
TEST(TypeCheckerTest, GenericMethodDualDefinitionRejected) {
    EXPECT_TRUE(checkHasErrors(
        "class Conn {\n"
        "    def one() -> int { return 0 }\n"
        "    def one[T](id: int) -> T { return T(id) }\n"
        "}\n"
        "b: Conn = Conn()\n"));
}

// Wrong explicit type-argument arity on a generic method is rejected.
TEST(TypeCheckerTest, GenericMethodArityMismatchRejected) {
    EXPECT_TRUE(checkHasErrors(
        "class Reg {\n"
        "    def pair[K, V](k: K, v: V) -> V { return v }\n"
        "}\n"
        "r: Reg = Reg()\n"
        "x: int = r.pair[int](1, 2)\n"));
}

// The unbounded-`T` restriction still applies inside a generic method body:
// member access on a `T`-typed value needs a bound; without one it is
// rejected.
TEST(TypeCheckerTest, GenericMethodUnboundedMemberAccessRejected) {
    EXPECT_TRUE(checkHasErrors(
        "class Reg {\n"
        "    def grow[T](x: T) -> int { return x.size() }\n"
        "}\n"
        "r: Reg = Reg()\n"
        "n: int = r.grow[int](3)\n"));
}

// Double monomorphization: a generic method on a generic-class instantiation
// (`Container[int].wrap[str]`) now type-checks clean - the engine composes the
// class frame (T) and method frame (U). (Behavioral/runtime cases live in
// test/dr/test_generics_double_mono.dr.)
TEST(TypeCheckerTest, GenericMethodOnGenericClassDoubleMonoOk) {
    EXPECT_TRUE(checkOk(
        "class Container[T] {\n"
        "    def(v: T) { self.v = v }\n"
        "    def wrap[U](x: U) -> U { return x }\n"
        "    def pair[U](x: U) -> tuple[T, U] { return (self.v, x) }\n"
        "}\n"
        "c: Container[int] = Container[int](7)\n"
        "p: tuple[int, str] = c.pair[str](\"hi\")\n"));
}

// A method type parameter shadowing the generic class's own type parameter
// makes the double-monomorphization substitution ambiguous - rejected.
TEST(TypeCheckerTest, GenericMethodShadowsClassTypeParamRejected) {
    EXPECT_TRUE(checkHasErrors(
        "class C[T] {\n"
        "    def() { }\n"
        "    def m[T](x: T) -> T { return x }\n"
        "}\n"));
}

// Polymorphic recursion through a generic METHOD's type args - the method
// analog of GenericPolymorphicRecursionCapped. Must be a clean compile error,
// not a hang (it drains iteratively, bypassing the class-path depth counter, so
// a structural type-arg-depth cap stops it fast).
TEST(TypeCheckerTest, GenericMethodPolymorphicRecursionCapped) {
    EXPECT_TRUE(checkHasErrors(
        "class R[T] {\n"
        "    def() { }\n"
        "    def go[U](x: U, n: int) -> int {\n"
        "        if n <= 0 { return 0 }\n"
        "        r2: R[T] = R[T]()\n"
        "        ys: list[U] = [x]\n"
        "        return 1 + r2.go[list[U]](ys, n - 1)\n"
        "    }\n"
        "}\n"
        "r: R[int] = R[int]()\n"
        "print(r.go[int](5, 3))\n"));
}

// Subclassing a generic instantiation is not yet supported - rejected clearly.
TEST(TypeCheckerTest, GenericInstantiationSubclassRejected) {
    EXPECT_TRUE(checkHasErrors(
        "class Animal[T] {\n"
        "    def(v: T) { self.tag = v }\n"
        "}\n"
        "class Dog(Animal[str]) {\n"
        "    def(v: str) { self.tag = v }\n"
        "}\n"));
}

// A union used as a generic type argument (both annotation and explicit forms).
TEST(TypeCheckerTest, GenericUnionTypeArgumentAccepted) {
    EXPECT_TRUE(checkOk(
        "class Box[T] {\n"
        "    def(v: T) { self.value = v }\n"
        "    def get() -> T { return self.value }\n"
        "}\n"
        "a: Box[int | str] = Box[int | str](5)\n"
        "b: Box[int | str] = Box(7)\n"));
}

//===----------------------------------------------------------------------===//
// Bounded type parameters (`T: Bound`)
//===----------------------------------------------------------------------===//

// A bounded `T: Animal` may read the bound's attributes and call its methods
// inside the generic body - the unbounded restriction is lifted by the bound.
TEST(TypeCheckerTest, BoundedTypeParamMemberAccessOk) {
    EXPECT_TRUE(checkOk(
        "class Animal {\n"
        "    name: str\n"
        "    def(n: str) { self.name = n }\n"
        "    def speak() -> str { return self.name }\n"
        "}\n"
        "def describe[T: Animal](x: T) -> str { return x.name + x.speak() }\n"
        "a: Animal = Animal(\"k\")\n"
        "s: str = describe[Animal](a)\n"));
}

// The bound class itself and any subclass satisfy the bound.
TEST(TypeCheckerTest, BoundedTypeParamSubclassArgAccepted) {
    EXPECT_TRUE(checkOk(
        "class Animal { def() { } def speak() -> str { return \"a\" } }\n"
        "class Dog(Animal) { def() { } def speak() -> str { return \"woof\" } }\n"
        "def describe[T: Animal](x: T) -> str { return x.speak() }\n"
        "d: Dog = Dog()\n"
        "s: str = describe[Dog](d)\n"));
}

// A bounded generic CLASS: store a `T` and call its bound method.
TEST(TypeCheckerTest, BoundedGenericClassMemberAccessOk) {
    EXPECT_TRUE(checkOk(
        "class Animal { def() { } def speak() -> str { return \"a\" } }\n"
        "class Shelter[T: Animal] {\n"
        "    occupant: T\n"
        "    def(o: T) { self.occupant = o }\n"
        "    def announce() -> str { return self.occupant.speak() }\n"
        "}\n"
        "sh: Shelter[Animal] = Shelter[Animal](Animal())\n"));
}

// A type argument that is neither the bound class nor a subclass is rejected.
TEST(TypeCheckerTest, BoundedTypeParamArgViolatesBoundRejected) {
    EXPECT_TRUE(checkHasErrors(
        "class Animal { def() { } def speak() -> str { return \"a\" } }\n"
        "class Cat { def() { } def meow() -> str { return \"m\" } }\n"
        "def describe[T: Animal](x: T) -> str { return x.speak() }\n"
        "c: Cat = Cat()\n"
        "s: str = describe[Cat](c)\n"));
}

// Same, through a generic-CLASS instantiation with a non-satisfying argument.
TEST(TypeCheckerTest, BoundedGenericClassArgViolatesBoundRejected) {
    EXPECT_TRUE(checkHasErrors(
        "class Animal { def() { } def speak() -> str { return \"a\" } }\n"
        "class Shelter[T: Animal] {\n"
        "    occupant: T\n"
        "    def(o: T) { self.occupant = o }\n"
        "}\n"
        "sh: Shelter[int] = Shelter[int](5)\n"));
}

// Regression: an UNBOUNDED `T` still cannot access members - the bound is what
// lifts the D044 restriction; without one, member access stays an error.
TEST(TypeCheckerTest, UnboundedTypeParamMemberAccessStillRejected) {
    EXPECT_TRUE(checkHasErrors(
        "def bad[T](x: T) -> str { return x.speak() }\n"));
}

// A bounded generic METHOD (its own `T: Bound`) resolves members on the bound.
TEST(TypeCheckerTest, BoundedGenericMethodOk) {
    EXPECT_TRUE(checkOk(
        "class Animal { def() { } def speak() -> str { return \"a\" } }\n"
        "class Registry {\n"
        "    def() { }\n"
        "    def loudest[T: Animal](a: T) -> str { return a.speak() }\n"
        "}\n"
        "r: Registry = Registry()\n"
        "s: str = r.loudest[Animal](Animal())\n"));
}

//===----------------------------------------------------------------------===//
// Inferred binding (`:=`) must not silently produce an Any-element container.
// An empty literal has no derivable element type; binding list[Unknown] /
// dict[Unknown, Unknown] is a de-facto Any (accepts anything -> boxes on the
// hot path). The Zen: ambiguity is a compile error to annotate away.
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, WalrusEmptyListInferRejected) {
    EXPECT_TRUE(checkHasErrors("xs := []\n"));
}

TEST(TypeCheckerTest, WalrusEmptyDictInferRejected) {
    EXPECT_TRUE(checkHasErrors("ds := {}\n"));
}

TEST(TypeCheckerTest, WalrusConcreteListInferOk) {
    EXPECT_TRUE(checkOk("xs := [1, 2, 3]\n"));
}

TEST(TypeCheckerTest, AnnotatedEmptyListOk) {
    // The annotation supplies the element type, so the empty literal is fine -
    // this path is an AnnAssign, not a walrus, and must stay valid.
    EXPECT_TRUE(checkOk("xs: list[int] = []\n"));
}

TEST(TypeCheckerTest, WalrusExprConcreteOk) {
    // Walrus in expression position binding a concrete scalar is unaffected.
    EXPECT_TRUE(checkOk("if (n := 5) > 0 {\n  print(n)\n}\n"));
}

// A non-empty literal must UNIFY its element types, not silently take the first.
// A mixed literal has no concrete element type, so an inferred `:=` binding is
// rejected (annotate). Homogeneous stays native; an explicit list[Any]
// annotation is the opt-in for genuine heterogeneity.

TEST(TypeCheckerTest, WalrusMixedListInferRejected) {
    EXPECT_TRUE(checkHasErrors("xs := [1, \"a\"]\n"));
}

TEST(TypeCheckerTest, WalrusIntFloatListInferRejected) {
    // int + float don't unify (the literal codegen can't bit-coerce them - a
    // silent list[float] would store the int's bits as garbage). Annotate
    // `list[float]` instead, where codegen coerces per the target type.
    EXPECT_TRUE(checkHasErrors("xs := [1, 2.0]\n"));
}

TEST(TypeCheckerTest, WalrusHomogeneousListInferOk) {
    EXPECT_TRUE(checkOk("xs := [10, 20, 30]\n"));
}

TEST(TypeCheckerTest, AnnotatedAnyMixedListOk) {
    // Explicit list[Any] is the opt-in for heterogeneous data - must stay valid.
    EXPECT_TRUE(checkOk("xs: list[Any] = [1, \"a\", 3.0]\n"));
}

TEST(TypeCheckerTest, MixedListAgainstIntAnnotationRejected) {
    EXPECT_TRUE(checkHasErrors("xs: list[int] = [1, \"a\"]\n"));
}

//===----------------------------------------------------------------------===//
// Lambda bodies are type-checked. visit(LambdaExpr) used to build
// the FunctionType from the annotations and skip the body entirely, so
// `bad: int = "boy"` inside a lambda compiled clean and generic method calls
// in handlers were never stamped (silent empty results at runtime). The
// runtime half of the fix is dogfooded in test/dr/test_lambda_body_types.dr.
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, LambdaBodyBadBindingRejected) {
    EXPECT_TRUE(checkHasErrors(
        "f: Callable[[], int] = lambda () -> int {\n"
        "    bad: int = \"boy\"\n"
        "    return bad\n"
        "}\n"));
}

TEST(TypeCheckerTest, LambdaBodyReturnTypeMismatchRejected) {
    EXPECT_TRUE(checkHasErrors(
        "f: Callable[[], int] = lambda () -> int {\n"
        "    return \"boy\"\n"
        "}\n"));
}

TEST(TypeCheckerTest, LambdaBodyWellTypedOk) {
    EXPECT_TRUE(checkOk(
        "f: Callable[[int], int] = lambda (n: int) -> int {\n"
        "    doubled: int = n * 2\n"
        "    return doubled\n"
        "}\n"));
}

//===----------------------------------------------------------------------===//
// Member access on `Any` is rejected (commandment #3: no duck typing)
//===----------------------------------------------------------------------===//

// A `Task[int]` stored in a bare `list` (= `list[Any]`) loses the handle tag
// that drives `.join()` dispatch, so pre-fix `for t in tasks { t.join() }`
// silently miscompiled to garbage. Dragon has no runtime member dispatch on
// `Any`, so the access must be a compile error pointing at the annotation.
TEST(TypeCheckerTest, MethodOnAnyReceiverRejected) {
    EXPECT_TRUE(checkHasErrors(
        "def worker(n: int) -> int { return n }\n"
        "tasks: list = []\n"
        "a: Task[int] = fire worker(1)\n"
        "tasks.append(a)\n"
        "for t in tasks {\n"
        "    x: int = t.join()\n"
        "}\n"));
}

// The honest form - the element type is spelled out - resolves the member and
// compiles cleanly.
TEST(TypeCheckerTest, MethodOnTypedTaskListOk) {
    EXPECT_TRUE(checkOk(
        "def worker(n: int) -> int { return n }\n"
        "tasks: list[Task[int]] = []\n"
        "a: Task[int] = fire worker(1)\n"
        "tasks.append(a)\n"
        "for t in tasks {\n"
        "    x: int = t.join()\n"
        "}\n"));
}

// Field read on an `Any` value is rejected for the same reason (not just calls).
TEST(TypeCheckerTest, FieldReadOnAnyReceiverRejected) {
    EXPECT_TRUE(checkHasErrors(
        "xs: list = []\n"
        "for e in xs {\n"
        "    y: int = e.value\n"
        "}\n"));
}

//===----------------------------------------------------------------------===//
// defer: the operand call type-checks like any call, so the own-mode
// signature rules (E13/E14) apply unchanged at the defer statement.
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, DeferOwnArgToBorrowingParamRejected) {
    EXPECT_TRUE(checkHasErrors(
        "def use(d: list[int]) -> None { }\n"
        "def f() -> None {\n"
        "    d: list[int] = [1, 2]\n"
        "    defer use(own d)\n"
        "}\n"));
}

TEST(TypeCheckerTest, DeferMissingOwnAtOwnParamRejected) {
    EXPECT_TRUE(checkHasErrors(
        "def sink(own d: list[int]) -> None { }\n"
        "def f() -> None {\n"
        "    d: list[int] = [1, 2]\n"
        "    defer sink(d)\n"
        "}\n"));
}

TEST(TypeCheckerTest, DeferArgTypeMismatchRejected) {
    EXPECT_TRUE(checkHasErrors(
        "def use(d: list[int]) -> None { }\n"
        "def f() -> None {\n"
        "    defer use(\"not a list\")\n"
        "}\n"));
}

TEST(TypeCheckerTest, DeferWellTypedCallOk) {
    EXPECT_TRUE(checkOk(
        "def sink(own d: list[int]) -> None { }\n"
        "def use(d: list[int]) -> None { }\n"
        "def f() -> None {\n"
        "    d: list[int] = [1, 2]\n"
        "    e: list[int] = [3]\n"
        "    defer use(d)\n"
        "    defer sink(own e)\n"
        "}\n"));
}

//===----------------------------------------------------------------------===//
// Identity resources (docs/1604 "Identity resources": the SocketHandle
// shape). The typechecker owns two of the book's bouncer cases: the
// borrow-forge into an own-param constructor and the dub of a claim holder.
// The move/claim cases (use-after-move, double-move) live in
// OwnershipCheckTest.cpp (OwnCtor*).
//===----------------------------------------------------------------------===//

TEST(TypeCheckerTest, IdentityResourceBorrowIntoOwnCtorRejected) {
    EXPECT_TRUE(checkHasErrors(
        "class H {\n"
        "    _fd: int\n"
        "    def(fd: int) { self._fd = fd }\n"
        "}\n"
        "class R {\n"
        "    own _h: H\n"
        "    def(own h: H) { self._h = h }\n"
        "}\n"
        "def f() -> None {\n"
        "    h: H = H(4)\n"
        "    r: R = R(h)\n"
        "}\n"));
}

TEST(TypeCheckerTest, IdentityResourceDubRejected) {
    EXPECT_TRUE(checkHasErrors(
        "class H {\n"
        "    _fd: int\n"
        "    def(fd: int) { self._fd = fd }\n"
        "}\n"
        "def f() -> None {\n"
        "    h: H = H(4)\n"
        "    h2: H = dub h\n"
        "}\n"));
}

TEST(TypeCheckerTest, IdentityResourceBlessedSpellingsOk) {
    EXPECT_TRUE(checkOk(
        "class H {\n"
        "    _fd: int\n"
        "    def(fd: int) { self._fd = fd }\n"
        "}\n"
        "class R {\n"
        "    own _h: H\n"
        "    def(own h: H) { self._h = h }\n"
        "}\n"
        "def f() -> None {\n"
        "    fresh: R = R(H(4))\n"
        "    h: H = H(5)\n"
        "    moved: R = R(own h)\n"
        "}\n"));
}
