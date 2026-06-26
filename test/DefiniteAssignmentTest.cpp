// Tests for the definite-assignment pass: reading a no-initializer local
// before it is assigned on all paths, and a constructor that leaves an
// own-declared non-defaulted field unassigned, are both compile errors.

#include "TestHelpers.h"
#include "dragon/DefiniteAssignment.h"
#include <gtest/gtest.h>

using namespace dragon;
using namespace dragon::test;

namespace {

// Parse + Sema + definite-assignment; return true if a DA error was reported.
bool daHasError(const std::string& src, bool isDragon = true) {
    auto mod = parse(src, isDragon);
    Sema sema;
    sema.analyze(*mod);
    DefiniteAssignment da;
    return !da.analyze(*mod);
}

} // namespace

//===----------------------------------------------------------------------===//
// Locals
//===----------------------------------------------------------------------===//

TEST(DefiniteAssignmentTest, ReadUnassignedLocalErrors) {
    EXPECT_TRUE(daHasError("x: int\nprint(x)\n"));
}

TEST(DefiniteAssignmentTest, AssignedBeforeReadOk) {
    EXPECT_FALSE(daHasError("x: int = 5\nprint(x)\n"));
    EXPECT_FALSE(daHasError("x: int\nx = 5\nprint(x)\n"));
}

TEST(DefiniteAssignmentTest, AssignedInBothBranchesOk) {
    EXPECT_FALSE(daHasError(
        "def f(c: bool) -> int {\n"
        "    x: int\n"
        "    if c { x = 1 } else { x = 2 }\n"
        "    return x\n"
        "}\n"));
}

TEST(DefiniteAssignmentTest, AssignedInOnlyOneBranchErrors) {
    EXPECT_TRUE(daHasError(
        "def f(c: bool) -> int {\n"
        "    x: int\n"
        "    if c { x = 1 }\n"
        "    return x\n"
        "}\n"));
}

TEST(DefiniteAssignmentTest, ElseReturnsGuardOk) {
    EXPECT_FALSE(daHasError(
        "def f(c: bool) -> int {\n"
        "    x: int\n"
        "    if c { x = 1 } else { return 0 }\n"
        "    return x\n"
        "}\n"));
}

TEST(DefiniteAssignmentTest, WhileTrueBreakAssignsOk) {
    EXPECT_FALSE(daHasError(
        "def f() -> int {\n"
        "    x: int\n"
        "    while True { x = 42 break }\n"
        "    return x\n"
        "}\n"));
}

TEST(DefiniteAssignmentTest, GeneralWhileMayNotRunErrors) {
    EXPECT_TRUE(daHasError(
        "def f(c: bool) -> int {\n"
        "    x: int\n"
        "    while c { x = 1 }\n"
        "    return x\n"
        "}\n"));
}

TEST(DefiniteAssignmentTest, AugAssignOnUnassignedErrors) {
    EXPECT_TRUE(daHasError(
        "def f() -> int {\n"
        "    x: int\n"
        "    x += 1\n"
        "    return x\n"
        "}\n"));
}

TEST(DefiniteAssignmentTest, ParameterIsAssignedOk) {
    EXPECT_FALSE(daHasError(
        "def f(x: int) -> int {\n"
        "    return x\n"
        "}\n"));
}

TEST(DefiniteAssignmentTest, GlobalDeclaredNameOk) {
    // A name read inside a function that isn't a function-local is treated as
    // an outer/global binding (not tracked) - no false positive.
    EXPECT_FALSE(daHasError(
        "g: int = 0\n"
        "def f() -> int {\n"
        "    return g\n"
        "}\n"));
}

//===----------------------------------------------------------------------===//
// Constructor field initialization
//===----------------------------------------------------------------------===//

TEST(DefiniteAssignmentTest, CtorForgetsFieldErrors) {
    EXPECT_TRUE(daHasError(
        "class C {\n"
        "    x: int\n"
        "    items: list[int]\n"
        "    def(x: int) { self.x = x }\n"
        "}\n"));
}

TEST(DefiniteAssignmentTest, CtorAssignsAllFieldsOk) {
    EXPECT_FALSE(daHasError(
        "class C {\n"
        "    x: int\n"
        "    y: int\n"
        "    def(x: int, y: int) { self.x = x self.y = y }\n"
        "}\n"));
}

TEST(DefiniteAssignmentTest, FieldWithDefaultOk) {
    EXPECT_FALSE(daHasError(
        "class C {\n"
        "    x: int\n"
        "    items: list[int] = []\n"
        "    def(x: int) { self.x = x }\n"
        "}\n"));
}

TEST(DefiniteAssignmentTest, FieldAssignedInOneCtorBranchErrors) {
    EXPECT_TRUE(daHasError(
        "class C {\n"
        "    x: int\n"
        "    items: list[int]\n"
        "    def(x: int) {\n"
        "        self.x = x\n"
        "        if x > 0 { self.items = [1] }\n"
        "    }\n"
        "}\n"));
}

TEST(DefiniteAssignmentTest, DeferredInitViaMethodOk) {
    // A field assigned by a non-constructor method is the deferred-init pattern
    // (Swift would model it as Optional) - exempt from the constructor check.
    EXPECT_FALSE(daHasError(
        "class Engine {\n"
        "    n: int\n"
        "    def(n: int) { self.n = n }\n"
        "}\n"
        "class C {\n"
        "    x: int\n"
        "    engine: Engine\n"
        "    def(x: int) { self.x = x }\n"
        "    def install() { self.engine = Engine(8) }\n"
        "}\n"));
}

TEST(DefiniteAssignmentTest, CtorAssignsViaSelfHelperOk) {
    // self escaping to a helper call defers field-init judgement (the helper
    // may assign it) - no false positive.
    EXPECT_FALSE(daHasError(
        "class C {\n"
        "    x: int\n"
        "    items: list[int]\n"
        "    def(x: int) { self.x = x self._setup() }\n"
        "    def _setup() { self.items = [1] }\n"
        "}\n"));
}

TEST(DefiniteAssignmentTest, StaticFieldNotRequiredOk) {
    EXPECT_FALSE(daHasError(
        "class C {\n"
        "    static count: int = 0\n"
        "    x: int\n"
        "    def(x: int) { self.x = x }\n"
        "}\n"));
}

TEST(DefiniteAssignmentTest, EarlyRaiseBeforeAssignOk) {
    EXPECT_FALSE(daHasError(
        "class C {\n"
        "    x: int\n"
        "    def(x: int) {\n"
        "        if x < 0 { raise ValueError(\"bad\") }\n"
        "        self.x = x\n"
        "    }\n"
        "}\n"));
}
