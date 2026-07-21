#include "CodeGenTestHelpers.h"

//===----------------------------------------------------------------------===//
// Comprehension IR Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, ListCompRange) {
    auto ir = generateIR("xs: list[int] = [i * 2 for i in range(5)]");
    EXPECT_NE(ir.find("dragon_list_new"), std::string::npos);
    EXPECT_NE(ir.find("dragon_list_append"), std::string::npos);
}

TEST(CodeGenTest, ListCompWithCond) {
    auto ir = generateIR("xs: list[int] = [i for i in range(10) if i > 5]");
    EXPECT_NE(ir.find("dragon_list_new"), std::string::npos);
    EXPECT_NE(ir.find("dragon_list_append"), std::string::npos);
    EXPECT_NE(ir.find("icmp sgt"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Comprehension E2E Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, ForInAndListComp) {
    auto output = compileAndRun(
        "nums: list[int] = [10, 20, 30]\n"
        "for x in nums {\n"
        "  print(x)\n"
        "}\n"
        "doubled: list[int] = [i * 2 for i in range(3)]\n"
        "print(doubled)"
    );
    EXPECT_EQ(output, "10\n20\n30\n[0, 2, 4]\n");
}

TEST(CodeGenE2E, ListCompOverList) {
    auto output = compileAndRun(
        "nums: list[int] = [1, 2, 3, 4, 5]\n"
        "doubled: list[int] = [x * 2 for x in nums]\n"
        "print(len(doubled))\n"
        "print(doubled[0])\n"
        "print(doubled[4])"
    );
    EXPECT_EQ(output, "5\n2\n10\n");
}

TEST(CodeGenE2E, ListCompOverListWithFilter) {
    auto output = compileAndRun(
        "nums: list[int] = [1, 2, 3, 4, 5, 6]\n"
        "evens: list[int] = [x for x in nums if x % 2 == 0]\n"
        "print(len(evens))\n"
        "print(evens[0])\n"
        "print(evens[1])\n"
        "print(evens[2])"
    );
    EXPECT_EQ(output, "3\n2\n4\n6\n");
}

TEST(CodeGenE2E, SetCompOverRange) {
    auto output = compileAndRun(
        "s: set[int] = {x * x for x in range(5)}\n"
        "print(len(s))"
    );
    EXPECT_EQ(output, "5\n");
}

TEST(CodeGenE2E, SetCompOverRangeWithFilter) {
    auto output = compileAndRun(
        "s: set[int] = {x for x in range(10) if x % 2 == 0}\n"
        "print(len(s))"
    );
    EXPECT_EQ(output, "5\n");
}

TEST(CodeGenE2E, SetCompOverList) {
    auto output = compileAndRun(
        "nums: list[int] = [1, 2, 2, 3, 3, 3]\n"
        "unique: set[int] = {x for x in nums}\n"
        "print(len(unique))"
    );
    EXPECT_EQ(output, "3\n");
}

TEST(CodeGenE2E, DictCompOverRange) {
    auto output = compileAndRun(
        "d: dict[str, int] = {\"k\": i for i in range(3)}\n"
        "print(len(d))"
    );
    // Dict with same key "k" reassigned 3 times -> len is 1
    EXPECT_EQ(output, "1\n");
}

TEST(CodeGenE2E, GeneratorOverRange) {
    auto output = compileAndRun(
        "g: list[int] = (x * 3 for x in range(4))\n"
        "print(len(g))\n"
        "print(g[0])\n"
        "print(g[3])"
    );
    EXPECT_EQ(output, "4\n0\n9\n");
}

TEST(CodeGenE2E, GeneratorOverList) {
    auto output = compileAndRun(
        "nums: list[int] = [10, 20, 30]\n"
        "doubled: list[int] = (x * 2 for x in nums)\n"
        "print(len(doubled))\n"
        "print(doubled[2])"
    );
    EXPECT_EQ(output, "3\n60\n");
}

TEST(CodeGenE2E, NestedListCompRange) {
    auto output = compileAndRun(
        "pairs: list[int] = [x + y for x in range(3) for y in range(3) if x != y]\n"
        "print(len(pairs))\n"
        "print(pairs[0])\n"
        "print(pairs[1])"
    );
    EXPECT_EQ(output, "6\n1\n2\n");
}

//===----------------------------------------------------------------------===//
// Regression: comprehension scope cleanup
//
// list/dict/set/generator comprehensions must call `emitScopeCleanup()`
// before every `popScope()`. With Int loop vars skipping it is
// harmless (no decrefs needed), but heap-typed loop vars (str/list/dict/
// instance) leak through every iteration without the cleanup.
//
// Companion to the codegen scope cleanup: the type checker binds
// the loop variable's type from the iterable's element type, and codegen
// uses the matching VarKind (heap-aware) for the loop var so the body
// can call methods on it. The loop var is marked borrowed so per-iter
// cleanup doesn't free strings still owned by the source list.
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, ListCompRangeStillWorks) {
    auto output = compileAndRun(
        "xs: list[int] = [i * 2 for i in range(5)]\n"
        "print(len(xs))\n"
        "print(xs[4])\n"
    );
    EXPECT_EQ(output, "5\n8\n");
}

TEST(CodeGenE2E, ListCompCollectionStillWorks) {
    auto output = compileAndRun(
        "src: list[int] = [1, 2, 3, 4, 5]\n"
        "doubled: list[int] = [x * 2 for x in src]\n"
        "print(doubled[4])\n"
    );
    EXPECT_EQ(output, "10\n");
}

TEST(CodeGenE2E, NestedListCompStillWorks) {
    // Nested comprehension exercises the extra-clause path's cleanup.
    auto output = compileAndRun(
        "pairs: list[int] = [x + y for x in range(3) for y in range(3) if x != y]\n"
        "print(len(pairs))\n"
    );
    EXPECT_EQ(output, "6\n");
}

TEST(CodeGenE2E, ListCompLoopBounded) {
    // Build the comprehension 10k times. Pre-fix, if a future heap-typed
    // loop var ever landed without cleanup, this pattern would surface
    // the leak. Today it's a regression check on the int path.
    auto output = compileAndRun(
        "last_len: int = 0\n"
        "for i in range(10000) {\n"
        "  out: list[int] = [x * 2 for x in range(50)]\n"
        "  last_len = len(out)\n"
        "}\n"
        "print(last_len)\n"
    );
    EXPECT_EQ(output, "50\n");
}

TEST(CodeGenE2E, SetCompStillWorks) {
    auto output = compileAndRun(
        "nums: list[int] = [1, 2, 2, 3, 3, 3]\n"
        "unique: set[int] = {x for x in nums}\n"
        "print(len(unique))\n"
    );
    EXPECT_EQ(output, "3\n");
}

TEST(CodeGenE2E, DictCompStillWorks) {
    auto output = compileAndRun(
        "d: dict[str, int] = {\"k\" + str(i): i for i in range(3)}\n"
        "print(len(d))\n"
    );
    EXPECT_EQ(output, "3\n");
}

TEST(CodeGenE2E, GeneratorExprStillWorks) {
    // Generator expressions need an explicit list[int] annotation today
    // for the type checker to accept indexed access on the result.
    auto output = compileAndRun(
        "g: list[int] = (x * 2 for x in range(5))\n"
        "print(len(g))\n"
        "print(g[2])\n"
    );
    EXPECT_EQ(output, "5\n4\n");
}

//===----------------------------------------------------------------------===//
// Heap-typed comprehensions - exercises the per-iter scope cleanup with a
// real heap-typed loop variable. Used to fail with `list[<unknown>]` in the
// type checker and "None" output at runtime.
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, ListCompStrIdentity) {
    auto output = compileAndRun(
        "names: list[str] = [\"alice\", \"bob\", \"carol\"]\n"
        "copies: list[str] = [n for n in names]\n"
        "print(copies[0])\n"
        "print(copies[2])\n"
    );
    EXPECT_EQ(output, "alice\ncarol\n");
}

TEST(CodeGenE2E, ListCompStrMethodCall) {
    auto output = compileAndRun(
        "names: list[str] = [\"alice\", \"bob\", \"carol\"]\n"
        "shouts: list[str] = [n.upper() for n in names]\n"
        "print(shouts[0])\n"
        "print(shouts[1])\n"
        "print(shouts[2])\n"
    );
    EXPECT_EQ(output, "ALICE\nBOB\nCAROL\n");
}

TEST(CodeGenE2E, ListCompStrConcat) {
    auto output = compileAndRun(
        "names: list[str] = [\"alice\", \"bob\"]\n"
        "greetings: list[str] = [\"hi \" + n for n in names]\n"
        "print(greetings[0])\n"
        "print(greetings[1])\n"
    );
    EXPECT_EQ(output, "hi alice\nhi bob\n");
}

TEST(CodeGenE2E, ListCompStrSourcePreserved) {
    // Source list must remain intact after the comprehension consumes it
    // (loop var is a borrowed reference; cleanup must not free its strings).
    auto output = compileAndRun(
        "names: list[str] = [\"alice\", \"bob\"]\n"
        "copies: list[str] = [n for n in names]\n"
        "print(names[0])\n"
        "print(names[1])\n"
        "more: list[str] = [\"hi \" + n for n in names]\n"
        "print(more[0])\n"
        "print(more[1])\n"
    );
    EXPECT_EQ(output, "alice\nbob\nhi alice\nhi bob\n");
}

TEST(CodeGenE2E, ListCompStrNested) {
    auto output = compileAndRun(
        "letters: list[str] = [\"a\", \"b\"]\n"
        "digits: list[str] = [\"1\", \"2\"]\n"
        "pairs: list[str] = [l + d for l in letters for d in digits]\n"
        "print(len(pairs))\n"
        "print(pairs[0])\n"
        "print(pairs[3])\n"
    );
    EXPECT_EQ(output, "4\na1\nb2\n");
}

TEST(CodeGenE2E, ForInOverStrComprehension) {
    auto output = compileAndRun(
        "names: list[str] = [\"alice\", \"bob\", \"carol\"]\n"
        "shouts: list[str] = [n.upper() for n in names]\n"
        "for s in shouts {\n"
        "  print(s)\n"
        "}\n"
    );
    EXPECT_EQ(output, "ALICE\nBOB\nCAROL\n");
}

TEST(CodeGenE2E, ListCompStrLoopBounded) {
    // Bounded heap usage with heap-typed loop var. Per-iter cleanup must
    // be a no-op (borrowed) for the loop var, but must run for the
    // owned concat result before it gets stored in the list.
    auto output = compileAndRun(
        "src: list[str] = [\"foo\", \"bar\", \"baz\"]\n"
        "last: int = 0\n"
        "for i in range(2000) {\n"
        "  out: list[str] = [s + \"!\" for s in src]\n"
        "  last = len(out)\n"
        "}\n"
        "print(last)\n"
    );
    EXPECT_EQ(output, "3\n");
}
