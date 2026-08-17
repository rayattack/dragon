#include "TestHelpers.h"
#include "dragon/DefiniteAssignment.h"
#include <gtest/gtest.h>

using namespace dragon;
using namespace dragon::test;

namespace {

bool daHasError(const std::string& src, bool isDragon = true) {
    auto mod = parse(src, isDragon);
    Sema sema;
    sema.analyze(*mod);
    DefiniteAssignment da;
    return !da.analyze(*mod);
}

}

TEST(ModuleInitOrderTest, DirectForwardConstRead_ClassTyped) {
    EXPECT_TRUE(daHasError(
        "class Dep {\n"
        "    v: int\n"
        "    def(x: int) { self.v = x }\n"
        "}\n"
        "class Svc {\n"
        "    d: Dep\n"
        "    def(dep: Dep) { self.d = dep }\n"
        "    def get() -> int { return self.d.v }\n"
        "}\n"
        "const S: Svc = Svc(D)\n"
        "const D: Dep = Dep(42)\n"
        "print(S.get())\n"));
}

TEST(ModuleInitOrderTest, DirectForwardConstRead_Scalar) {
    EXPECT_TRUE(daHasError(
        "const A: int = B + 1\n"
        "const B: int = 41\n"
        "print(A)\n"));
}

TEST(ModuleInitOrderTest, InterprocForwardConstRead) {
    EXPECT_TRUE(daHasError(
        "def read_later() -> int { return LATER }\n"
        "const A: int = read_later()\n"
        "const LATER: int = 42\n"
        "print(A)\n"));
}

TEST(ModuleInitOrderTest, TwoConstInitCycle) {
    EXPECT_TRUE(daHasError(
        "const X: int = Y + 1\n"
        "const Y: int = X + 1\n"
        "print(X)\n"
        "print(Y)\n"));
}

TEST(ModuleInitOrderTest, InterprocInitCycle) {
    EXPECT_TRUE(daHasError(
        "def f() -> int { return B }\n"
        "def g() -> int { return A }\n"
        "const A: int = f()\n"
        "const B: int = g()\n"
        "print(A)\n"));
}

TEST(ModuleInitOrderTest, CorrectOrderConstChain_ClassTyped) {
    EXPECT_FALSE(daHasError(
        "class Dep {\n"
        "    v: int\n"
        "    def(x: int) { self.v = x }\n"
        "}\n"
        "class Svc {\n"
        "    d: Dep\n"
        "    def(dep: Dep) { self.d = dep }\n"
        "    def get() -> int { return self.d.v }\n"
        "}\n"
        "const D: Dep = Dep(42)\n"
        "const S: Svc = Svc(D)\n"
        "print(S.get())\n"));
}

TEST(ModuleInitOrderTest, CorrectOrderConstChain_Scalar) {
    EXPECT_FALSE(daHasError(
        "const B: int = 41\n"
        "const A: int = B + 1\n"
        "print(A)\n"));
}

TEST(ModuleInitOrderTest, ForwardFunctionRefPureHelper) {
    EXPECT_FALSE(daHasError(
        "const A: int = compute()\n"
        "def compute() -> int { return 7 }\n"
        "print(A)\n"));
}

TEST(ModuleInitOrderTest, ForwardFunctionReadsConstButNotCalledDuringInit) {
    EXPECT_FALSE(daHasError(
        "def check(n: int) -> bool { return n < LIMIT }\n"
        "const LIMIT: int = 100\n"
        "print(LIMIT)\n"));
}

TEST(ModuleInitOrderTest, InterprocConstReadConstDefinedEarlier) {
    EXPECT_FALSE(daHasError(
        "const DATA_DIR: str = \"data\"\n"
        "def ensure() -> str { return DATA_DIR }\n"
        "const READY: str = ensure()\n"
        "print(READY)\n"));
}


TEST(ModuleInitOrderTest, MainAtTopCallsHelpersBelow) {
    EXPECT_FALSE(daHasError(
        "def main() -> None { helper() }\n"
        "def helper() -> None { print(\"hi\") }\n"
        "main()\n"));
}
