// Module initialization-order (definite-initialization) tests.
//
// Dragon initlizes module-level const/global singletons EAGERLY!, in SOURCE
// order (top to bottom), at module load. An initiliaer that reads the value of
// naother module-level const declared LATER in the same file captures that
// constants `const`'s still zeroed slot
//
//   - pointer/heap-typed const  -> null deref -> SIGSEGV (exit 139, no output)
//   - scalar const              -> silent WRONG ANSWER (reads 0/garbage)
//
// Both break the zen's core promise: "You should know if it is broken when you
// compile." (zen.md line 9). The fix is a definite-INITIALIZATION analysis over
// module-level initializers in source order, layered onto the existing
// definite-ASSIGNMENT pass (DefiniteAssignment). If the analysis cannot PROVE a
// read module-const is already initialized at the point of use - directly, or
// transitively through a function/ctor/method called during initialization -
// it is a compile error ("in the face of ambiguity, annotate").
//
// TEST-FIRST STATUS: the MustNotCompile group below is RED until the
// DefiniteAssignment module-init extension lands. Each asserts the POST-FIX
// contract (daHasError == true). The MustStillCompile group is GREEN today and
// must STAY green: it locks in that order-independent forward FUNCTION
// references (resolved at call time, not module-load time) are untouched.

#include "TestHelpers.h"
#include "dragon/DefiniteAssignment.h"
#include <gtest/gtest.h>

using namespace dragon;
using namespace dragon::test;

namespace {

// Parse + Sema + definite-assignment; true if a DA (definite-init) error fired.
// Mirrors DefiniteAssignmentTest.cpp's harness: the module-init guard lives in
// the same pass, so it is exercised by the same three-line front-end.
bool daHasError(const std::string& src, bool isDragon = true) {
    auto mod = parse(src, isDragon);
    Sema sema;
    sema.analyze(*mod);
    DefiniteAssignment da;
    return !da.analyze(*mod);
}

} // namespace

//===----------------------------------------------------------------------===//
// MUST NOT COMPILE  (RED until the module-init analysis lands)
//===----------------------------------------------------------------------===//

// The flagship crash: `const S = Svc(D)` reads pointer-typed `D` before it is
// initialized. Runtime baseline (dragon run): SIGSEGV, exit 139, empty output.
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
        "const S: Svc = Svc(D)\n"     // reads D ...
        "const D: Dep = Dep(42)\n"    // ... initialized here, later
        "print(S.get())\n"));
}

// The scalar variant is arguably worse than the crash: no SIGSEGV, just a
// SILENT wrong answer. Runtime baseline: prints 1 (B still 0 at read), exit 0.
TEST(ModuleInitOrderTest, DirectForwardConstRead_Scalar) {
    EXPECT_TRUE(daHasError(
        "const A: int = B + 1\n"   // reads B (still 0) -> A == 1, silently wrong
        "const B: int = 41\n"
        "print(A)\n"));
}

// Interprocedural: the const's RHS does not name the later const directly - it
// reaches it THROUGH a call. `read_later()` reads `LATER` during A's init.
// Runtime baseline: prints 0 (LATER still 0), exit 0 - another silent lie.
TEST(ModuleInitOrderTest, InterprocForwardConstRead) {
    EXPECT_TRUE(daHasError(
        "def read_later() -> int { return LATER }\n"
        "const A: int = read_later()\n"   // A reads LATER via the call
        "const LATER: int = 42\n"
        "print(A)\n"));
}

// A two-const initialization cycle: no source order initializes both. X reads
// Y, Y reads X. Runtime baseline: prints 1 then 2 (both garbage), exit 0.
TEST(ModuleInitOrderTest, TwoConstInitCycle) {
    EXPECT_TRUE(daHasError(
        "const X: int = Y + 1\n"
        "const Y: int = X + 1\n"
        "print(X)\n"
        "print(Y)\n"));
}

// Interprocedural cycle: A's init calls f(), f() reads B; B's init calls g(),
// g() reads A. Cyclic even though neither RHS names the other const directly.
TEST(ModuleInitOrderTest, InterprocInitCycle) {
    EXPECT_TRUE(daHasError(
        "def f() -> int { return B }\n"
        "def g() -> int { return A }\n"
        "const A: int = f()\n"
        "const B: int = g()\n"
        "print(A)\n"));
}

//===----------------------------------------------------------------------===//
// MUST STILL COMPILE  (GREEN today; must stay GREEN after the fix)
//===----------------------------------------------------------------------===//

// Correct order: the dependency is initialized before its user. This is the
// deps.dr shape after the manual reorder; it must never be flagged.
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
        "const D: Dep = Dep(42)\n"    // initialized FIRST ...
        "const S: Svc = Svc(D)\n"     // ... then read here
        "print(S.get())\n"));
}

TEST(ModuleInitOrderTest, CorrectOrderConstChain_Scalar) {
    EXPECT_FALSE(daHasError(
        "const B: int = 41\n"
        "const A: int = B + 1\n"
        "print(A)\n"));
}

// The load-bearing invariant: a forward FUNCTION reference at module-const init
// is fine when the callee reads no not-yet-initialized const. `compute` is
// defined BELOW its call site and resolves at call time. Over-conservative
// analysis (banning any call during init) would wrongly break this.
TEST(ModuleInitOrderTest, ForwardFunctionRefPureHelper) {
    EXPECT_FALSE(daHasError(
        "const A: int = compute()\n"
        "def compute() -> int { return 7 }\n"
        "print(A)\n"));
}

// A forward function reference that DOES read a module const, but the function
// is never CALLED during init - so the read happens (if ever) at call time,
// after the const is initialized. `check` is defined above `LIMIT` and reads
// it, yet is only invoked at runtime. Must not be flagged: this is the
// pervasive, intentional forward-function-ref pattern (211 call sites).
TEST(ModuleInitOrderTest, ForwardFunctionReadsConstButNotCalledDuringInit) {
    EXPECT_FALSE(daHasError(
        "def check(n: int) -> bool { return n < LIMIT }\n"
        "const LIMIT: int = 100\n"
        "print(LIMIT)\n"));
}

// INterprocedural, but SAFE: the const read through the call is initialized
// BEFORE the call site. This is the deps.dr's `const _READY = _ensure_data()`
// (which reads the earlier `DATA_DIR`). Occurred when I tried to re-arrange
// consts to check the limits of order indenpendence.
// The analysis must be order-precise, not a blanket ban on calls during init.
TEST(ModuleInitOrderTest, InterprocConstReadConstDefinedEarlier) {
    EXPECT_FALSE(daHasError(
        "const DATA_DIR: str = \"data\"\n"
        "def ensure() -> str { return DATA_DIR }\n"   // reads earlier const
        "const READY: str = ensure()\n"
        "print(READY)\n"));
}


// Mutually recursive top-level defs called at module load - pure forward
// function references, no consts involved. The order-independent name
// resolution this depends on must be preserved
TEST(ModuleInitOrderTest, MainAtTopCallsHelpersBelow) {
    EXPECT_FALSE(daHasError(
        "def main() -> None { helper() }\n"
        "def helper() -> None { print(\"hi\") }\n"
        "main()\n"));
}
