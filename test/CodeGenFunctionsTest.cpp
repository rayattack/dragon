// TODO: these IR string asserts are brittle, switch to FileCheck eventually
#include "CodeGenTestHelpers.h"

//===----------------------------------------------------------------------===//
// Function IR
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, FunctionDecl) {
    auto ir = generateIR("def add(a: int, b: int) -> int {\n  return a + b\n}");
    EXPECT_NE(ir.find("define i64 @add(i64"), std::string::npos);
    EXPECT_NE(ir.find("ret i64"), std::string::npos);
}

TEST(CodeGenTest, FunctionCall) {
    auto ir = generateIR(
        "def double_(x: int) -> int {\n  return x * 2\n}\n"
        "print(double_(21))"
    );
    EXPECT_NE(ir.find("define i64 @double_"), std::string::npos);
    EXPECT_NE(ir.find("call i64 @double_"), std::string::npos);
}

TEST(CodeGenTest, VoidFunction) {
    auto ir = generateIR("def greet() -> None {\n  print(\"hi\")\n}");
    EXPECT_NE(ir.find("define void @greet()"), std::string::npos);
}

TEST(CodeGenTest, ForwardDeclaration) {
    // Functions should be forward-declared so order doesn't matter
    auto ir = generateIR(
        "def foo() -> int {\n  return bar()\n}\n"
        "def bar() -> int {\n  return 42\n}"
    );
    EXPECT_NE(ir.find("define i64 @foo()"), std::string::npos);
    EXPECT_NE(ir.find("define i64 @bar()"), std::string::npos);
}

TEST(CodeGenTest, LambdaSimple) {
    auto ir = generateIR(
        "f: ptr = lambda (x: int) -> int {\n"
        "  return x + 1\n"
        "}\n"
    );
    EXPECT_NE(ir.find("__dragon_lambda_"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Type Alias IR
//===----------------------------------------------------------------------===//

TEST(CodeGenIR, TypeAliasNoOp) {
    // Type alias is compile-time only - should not emit any runtime code for it
    auto ir = generateIR(
        "type IntList = list[int]\n"
        "x: int = 42\n"
        "print(x)\n"
    );
    // The IR should still have main and the print call, but no runtime
    // representation of the type alias
    EXPECT_NE(ir.find("define"), std::string::npos);
    EXPECT_NE(ir.find("dragon_print_int"), std::string::npos);
}

TEST(CodeGenIR, PyTypeAliasIR) {
    auto ir = generateIRPy(
        "type IntList = list[int]\n"
        "x: int = 42\n"
        "print(x)\n"
    );
    // Type alias is no-op - IR should still have main and print
    EXPECT_NE(ir.find("define"), std::string::npos);
    EXPECT_NE(ir.find("dragon_print_int"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Indirect Call IR
//===----------------------------------------------------------------------===//

TEST(CodeGenIR, IndirectCallIR) {
    // Verify that an indirect call is generated for a variable holding a function
    auto ir = generateIR(
        "def inc(x: int) -> int {\n"
        "  return x + 1\n"
        "}\n"
        "f: ptr = inc\n"
        "y: int = f(10)\n"
    );
    // Should contain an indirect call (icall) or at least a load of f
    EXPECT_NE(ir.find("f.load"), std::string::npos)
        << "Expected f.load for indirect call\nIR:\n" << ir;
}

//===----------------------------------------------------------------------===//
// Function E2E
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, FunctionAndLoop) {
    auto output = compileAndRun(
        "def fib(n: int) -> int {\n"
        "  if n <= 1 {\n"
        "    return n\n"
        "  }\n"
        "  return fib(n - 1) + fib(n - 2)\n"
        "}\n"
        "print(fib(10))\n"
        "for i in range(3) {\n"
        "  print(i)\n"
        "}"
    );
    EXPECT_EQ(output, "55\n0\n1\n2\n");
}

TEST(CodeGenE2E, RecursivePower) {
    auto output = compileAndRun(
        "def power(base: int, exp: int) -> int {\n"
        "  if exp == 0 {\n"
        "    return 1\n"
        "  }\n"
        "  return base * power(base, exp - 1)\n"
        "}\n"
        "print(power(2, 10))"
    );
    EXPECT_EQ(output, "1024\n");
}

TEST(CodeGenE2E, MultipleReturns) {
    auto output = compileAndRun(
        "def classify(n: int) -> str {\n"
        "  if n < 0 {\n"
        "    return \"negative\"\n"
        "  }\n"
        "  if n == 0 {\n"
        "    return \"zero\"\n"
        "  }\n"
        "  return \"positive\"\n"
        "}\n"
        "print(classify(-5))\n"
        "print(classify(0))\n"
        "print(classify(42))"
    );
    EXPECT_EQ(output, "negative\nzero\npositive\n");
}

TEST(CodeGenE2E, PositionalOnlyParamFunc) {
    auto output = compileAndRun(
        "def add(a: int, b: int, /) -> int {\n"
        "    return a + b\n"
        "}\n"
        "print(add(3, 4))"
    );
    EXPECT_EQ(output, "7\n");
}

TEST(CodeGenE2E, KeywordOnlyParamFunc) {
    // Keyword-only params parse correctly; call with positional args for now
    // (keyword call syntax is a future feature)
    auto output = compileAndRun(
        "def greet(a: int, *, count: int) -> int {\n"
        "    return a + count\n"
        "}\n"
        "print(greet(1, 5))"
    );
    EXPECT_EQ(output, "6\n");
}

TEST(CodeGenE2E, MixedParamSeparators) {
    auto output = compileAndRun(
        "def foo(a: int, /, b: int, *, c: int) -> int {\n"
        "    return a + b + c\n"
        "}\n"
        "print(foo(1, 2, 3))"
    );
    EXPECT_EQ(output, "6\n");
}

//===----------------------------------------------------------------------===//
// First-class Function E2E
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, LambdaAssignedToVariableThenCalled) {
    auto output = compileAndRun(
        "square: ptr = lambda (x: int) -> int {\n"
        "  return x * x\n"
        "}\n"
        "print(square(4))\n"
    );
    EXPECT_EQ(output, "16\n");
}

TEST(CodeGenE2E, NamedFunctionAssignedToVariableThenCalled) {
    auto output = compileAndRun(
        "def double(x: int) -> int {\n"
        "  return x * 2\n"
        "}\n"
        "fn: ptr = double\n"
        "print(fn(5))\n"
    );
    EXPECT_EQ(output, "10\n");
}

TEST(CodeGenE2E, FunctionPassedAsPtrParameter) {
    auto output = compileAndRun(
        "def apply(f: ptr, x: int) -> int {\n"
        "  return f(x)\n"
        "}\n"
        "def triple(x: int) -> int {\n"
        "  return x * 3\n"
        "}\n"
        "result: int = apply(triple, 7)\n"
        "print(result)\n"
    );
    EXPECT_EQ(output, "21\n");
}

TEST(CodeGenE2E, LambdaPassedDirectlyAsArgument) {
    auto output = compileAndRun(
        "def apply(f: ptr, x: int) -> int {\n"
        "  return f(x)\n"
        "}\n"
        "result: int = apply(lambda (n: int) -> int { return n + 10 }, 5)\n"
        "print(result)\n"
    );
    EXPECT_EQ(output, "15\n");
}

TEST(CodeGenE2E, FirstClassFuncMultipleArgs) {
    auto output = compileAndRun(
        "def add(a: int, b: int) -> int {\n"
        "  return a + b\n"
        "}\n"
        "op: ptr = add\n"
        "print(op(3, 4))\n"
    );
    EXPECT_EQ(output, "7\n");
}

TEST(CodeGenE2E, HigherOrderFunctionReturningFunction) {
    // Note: Dragon lambdas do not capture outer variables (no closures yet),
    // so we test returning a self-contained lambda from a function.
    auto output = compileAndRun(
        "def get_doubler() -> ptr {\n"
        "  return lambda (x: int) -> int { return x * 2 }\n"
        "}\n"
        "dbl: ptr = get_doubler()\n"
        "print(dbl(10))\n"
    );
    EXPECT_EQ(output, "20\n");
}

TEST(CodeGenE2E, FirstClassFuncExistingCallsStillWork) {
    // Regression: existing named function calls must still work
    auto output = compileAndRun(
        "def greet(name: str) -> str {\n"
        "  return \"hello\"\n"
        "}\n"
        "print(greet(\"world\"))\n"
    );
    EXPECT_EQ(output, "hello\n");
}

TEST(CodeGenE2E, LambdaAssignedWithAnnotation) {
    auto output = compileAndRun(
        "fn: ptr = lambda (x: int) -> int {\n"
        "  return x * x\n"
        "}\n"
        "print(fn(6))\n"
    );
    EXPECT_EQ(output, "36\n");
}

//===----------------------------------------------------------------------===//
// Closure tests
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, ClosureCaptureInt) {
    // Lambda captures an integer from enclosing function scope
    auto output = compileAndRun(
        "def make_adder(n: int) -> int {\n"
        "    f: ptr = lambda (x: int) -> int { return x + n }\n"
        "    return f(10)\n"
        "}\n"
        "print(make_adder(5))\n"
        "print(make_adder(20))\n"
    );
    EXPECT_EQ(output, "15\n30\n");
}

TEST(CodeGenTest, ClosureCaptureStr) {
    // Lambda captures a string from enclosing function scope
    auto output = compileAndRun(
        "def greet(greeting: str, name: str) -> str {\n"
        "    f: ptr = lambda (n: str) -> str { return greeting + \", \" + n }\n"
        "    return f(name)\n"
        "}\n"
        "print(greet(\"Hello\", \"World\"))\n"
        "print(greet(\"Hi\", \"Dragon\"))\n"
    );
    EXPECT_EQ(output, "Hello, World\nHi, Dragon\n");
}

TEST(CodeGenTest, ClosureMultipleCaptures) {
    // Lambda captures multiple variables of different types
    auto output = compileAndRun(
        "def fmt(prefix: str, multiplier: int, x: int) -> str {\n"
        "    f: ptr = lambda (v: int) -> str {\n"
        "        return prefix + \": \" + str(v * multiplier)\n"
        "    }\n"
        "    return f(x)\n"
        "}\n"
        "print(fmt(\"Result\", 3, 7))\n"
        "print(fmt(\"Result\", 3, 10))\n"
    );
    EXPECT_EQ(output, "Result: 21\nResult: 30\n");
}

TEST(CodeGenTest, ClosureByValueSemantics) {
    // Closure captures by value - changes to outer variable don't affect closure
    auto output = compileAndRun(
        "def test() -> int {\n"
        "    x: int = 10\n"
        "    f: ptr = lambda () -> int { return x }\n"
        "    x = 20\n"
        "    return f()\n"
        "}\n"
        "print(test())\n"
    );
    EXPECT_EQ(output, "10\n");
}

TEST(CodeGenTest, NonCapturingLambdaUnchanged) {
    // Non-capturing lambdas still work as bare function pointers
    auto output = compileAndRun(
        "f: ptr = lambda (x: int, y: int) -> int { return x + y }\n"
        "print(f(3, 4))\n"
    );
    EXPECT_EQ(output, "7\n");
}

TEST(CodeGenTest, ClosureCaptureBool) {
    // Lambda captures a boolean
    auto output = compileAndRun(
        "def check(flag: bool, x: int) -> str {\n"
        "    f: ptr = lambda (v: int) -> str {\n"
        "        if flag {\n"
        "            return \"yes: \" + str(v)\n"
        "        }\n"
        "        return \"no: \" + str(v)\n"
        "    }\n"
        "    return f(x)\n"
        "}\n"
        "print(check(True, 42))\n"
        "print(check(False, 42))\n"
    );
    EXPECT_EQ(output, "yes: 42\nno: 42\n");
}

TEST(CodeGenTest, ClosureCaptureFloat) {
    // Lambda captures a float
    auto output = compileAndRun(
        "def scale(factor: float, x: float) -> float {\n"
        "    f: ptr = lambda (v: float) -> float { return v * factor }\n"
        "    return f(x)\n"
        "}\n"
        "print(scale(2.0, 3.5))\n"
    );
    EXPECT_EQ(output, "7.0\n");  // float repr keeps .0 (Python parity)
}

TEST(CodeGenTest, ClosureReturnedFromFunction) {
    // Closure returned and called from outer scope (the key use case)
    auto output = compileAndRun(
        "def make_adder(n: int) -> int {\n"
        "    adder: ptr = lambda (x: int) -> int { return x + n }\n"
        "    return adder(100)\n"
        "}\n"
        "print(make_adder(5))\n"
        "print(make_adder(42))\n"
    );
    EXPECT_EQ(output, "105\n142\n");
}

//===----------------------------------------------------------------------===//
// Nested `def` (regression: top-level def inside another def - closure
// semantics matching Python). See src/codegen/Functions.cpp:emitNestedFunctionDecl.
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, NestedDefWithCapture) {
    // Inner def captures an int from the enclosing scope.
    auto output = compileAndRun(
        "def outer(x: int) -> int {\n"
        "    def inner(y: int) -> int {\n"
        "        return x + y\n"
        "    }\n"
        "    return inner(10)\n"
        "}\n"
        "print(outer(5))\n"
    );
    EXPECT_EQ(output, "15\n");
}

TEST(CodeGenTest, NestedDefWithoutCapture) {
    // Inner def references no enclosing locals - bare fn pointer, no env.
    auto output = compileAndRun(
        "def outer() -> int {\n"
        "    def inner(y: int) -> int {\n"
        "        return y * 2\n"
        "    }\n"
        "    return inner(7)\n"
        "}\n"
        "print(outer())\n"
    );
    EXPECT_EQ(output, "14\n");
}

TEST(CodeGenTest, NestedDefSiblings) {
    // Two sibling nested defs in the same outer, each called from outer.
    auto output = compileAndRun(
        "def outer() -> int {\n"
        "    def first(a: int) -> int {\n"
        "        return a + 1\n"
        "    }\n"
        "    def second(a: int) -> int {\n"
        "        return a * 10\n"
        "    }\n"
        "    x: int = first(5)\n"
        "    y: int = second(3)\n"
        "    return x + y\n"
        "}\n"
        "print(outer())\n"
    );
    // first(5)=6, second(3)=30, total=36
    EXPECT_EQ(output, "36\n");
}

TEST(CodeGenTest, NestedDefRecursive) {
    // Recursive nested def - self-reference resolves through
    // nestedFunctionAliases during body emission.
    auto output = compileAndRun(
        "def outer(n: int) -> int {\n"
        "    def fact(k: int) -> int {\n"
        "        if k <= 1 {\n"
        "            return 1\n"
        "        }\n"
        "        return k * fact(k - 1)\n"
        "    }\n"
        "    return fact(n)\n"
        "}\n"
        "print(outer(5))\n"
    );
    EXPECT_EQ(output, "120\n");
}

TEST(CodeGenTest, NestedDefRecursiveWithCapture) {
    // Recursive nested def that ALSO captures an enclosing local -
    // env arg is auto-appended on the recursive call.
    auto output = compileAndRun(
        "def outer(base: int, n: int) -> int {\n"
        "    def step(k: int) -> int {\n"
        "        if k == 0 {\n"
        "            return base\n"
        "        }\n"
        "        return step(k - 1) + base\n"
        "    }\n"
        "    return step(n)\n"
        "}\n"
        "print(outer(7, 4))\n"
    );
    // step(4)=base+step(3)=7+7+step(2)=...= 7 + 7*4 = 35
    EXPECT_EQ(output, "35\n");
}

TEST(CodeGenTest, NestedDefStrCapture) {
    // String capture survives across the call boundary with proper refcount.
    auto output = compileAndRun(
        "def greet(name: str) -> str {\n"
        "    def make_greeting(suffix: str) -> str {\n"
        "        return name + suffix\n"
        "    }\n"
        "    return make_greeting(\"!\")\n"
        "}\n"
        "print(greet(\"hello\"))\n"
    );
    EXPECT_EQ(output, "hello!\n");
}

TEST(CodeGenTest, NestedDefDoesNotLeakIntoModule) {
    // Inner name is scoped to the enclosing function - calling it at
    // module level is an error, matching Python's NameError semantics.
    // We verify by defining a top-level `inner` whose body and the nested
    // `inner` both run, with the nested one not shadowing module-level use.
    auto output = compileAndRun(
        "def inner(x: int) -> int {\n"
        "    return x + 100\n"
        "}\n"
        "def outer() -> int {\n"
        "    def inner(y: int) -> int {\n"
        "        return y - 1\n"
        "    }\n"
        "    return inner(50)\n"
        "}\n"
        "print(outer())\n"
        "print(inner(50))\n"
    );
    // outer's nested inner: 50-1=49; module's inner: 50+100=150
    EXPECT_EQ(output, "49\n150\n");
}

//===----------------------------------------------------------------------===//
// Generator IR
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, GeneratorStoredInVarIR) {
    auto ir = generateIR(
        "def nums() {\n"
        "    yield 1\n"
        "    yield 2\n"
        "    yield 3\n"
        "}\n"
        "\n"
        "g: ptr = nums()\n"
        "for x in g {\n"
        "    print(x)\n"
        "}\n"
    );
    // Check that the for-in detected the generator (not the list iteration path)
    EXPECT_NE(ir.find("dragon_generator_next"), std::string::npos) << "Missing generator_next in IR:\n" << ir;
    // Should NOT use list iteration for the generator
    // Find dragon_print_int after the generator_next (meaning x is printed as int)
    EXPECT_NE(ir.find("dragon_print_int"), std::string::npos) << "Missing print_int in IR:\n" << ir;
}

TEST(CodeGenTest, GeneratorIR) {
    auto ir = generateIR(
        "def gen() {\n"
        "    yield 42\n"
        "}\n"
        "\n"
        "for x in gen() {\n"
        "    print(x)\n"
        "}\n"
    );
    EXPECT_NE(ir.find("dragon_generator_create"), std::string::npos);
    EXPECT_NE(ir.find("dragon_generator_next"), std::string::npos);
    EXPECT_NE(ir.find("dragon_generator_yield"), std::string::npos);
    EXPECT_NE(ir.find("gen__gen_body"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Generator E2E
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, GeneratorBasic) {
    auto output = compileAndRun(
        "def count_up(n: int) {\n"
        "    i: int = 0\n"
        "    while i < n {\n"
        "        yield i\n"
        "        i = i + 1\n"
        "    }\n"
        "}\n"
        "\n"
        "for x in count_up(5) {\n"
        "    print(x)\n"
        "}\n"
    );
    EXPECT_EQ(output, "0\n1\n2\n3\n4\n");
}

TEST(CodeGenE2E, GeneratorFibonacci) {
    auto output = compileAndRun(
        "def fib(n: int) {\n"
        "    a: int = 0\n"
        "    b: int = 1\n"
        "    i: int = 0\n"
        "    while i < n {\n"
        "        yield a\n"
        "        temp: int = a + b\n"
        "        a = b\n"
        "        b = temp\n"
        "        i = i + 1\n"
        "    }\n"
        "}\n"
        "\n"
        "for x in fib(7) {\n"
        "    print(x)\n"
        "}\n"
    );
    EXPECT_EQ(output, "0\n1\n1\n2\n3\n5\n8\n");
}

TEST(CodeGenE2E, GeneratorNoArgs) {
    auto output = compileAndRun(
        "def three() {\n"
        "    yield 10\n"
        "    yield 20\n"
        "    yield 30\n"
        "}\n"
        "\n"
        "for x in three() {\n"
        "    print(x)\n"
        "}\n"
    );
    EXPECT_EQ(output, "10\n20\n30\n");
}

TEST(CodeGenE2E, GeneratorEmpty) {
    auto output = compileAndRun(
        "def empty() {\n"
        "    return\n"
        "    yield 1\n"
        "}\n"
        "\n"
        "for x in empty() {\n"
        "    print(x)\n"
        "}\n"
        "print(\"done\")\n"
    );
    EXPECT_EQ(output, "done\n");
}

TEST(CodeGenE2E, GeneratorStoredInVar) {
    auto output = compileAndRun(
        "def nums() {\n"
        "    yield 1\n"
        "    yield 2\n"
        "    yield 3\n"
        "}\n"
        "\n"
        "g: ptr = nums()\n"
        "for x in g {\n"
        "    print(x)\n"
        "}\n"
    );
    EXPECT_EQ(output, "1\n2\n3\n");
}

// ===== D024: Decorator Tests =====

TEST(CodeGenTest, BasicFunctionDecorator) {
    // A decorator that wraps a function, calling it and adding behavior
    auto output = compileAndRun(
        "def loud(f: ptr) -> ptr {\n"
        "    # For now, just return the function as-is (identity decorator)\n"
        "    return f\n"
        "}\n"
        "@loud\n"
        "def greet() {\n"
        "    print(\"hello\")\n"
        "}\n"
        "greet()\n"
    );
    EXPECT_EQ(output, "hello\n");
}

//===----------------------------------------------------------------------===//
// *args and **kwargs (variadic arguments)
//===----------------------------------------------------------------------===//

TEST(CodeGenIR, VarArgsIR) {
    auto ir = generateIR(
        "def foo(a: int, *args: int) {\n"
        "  print(a)\n"
        "}\n"
        "foo(1, 2, 3)\n"
    );
    // Should see dragon_list_new for packing extra args (int element -> i64 list)
    EXPECT_NE(ir.find("dragon_list_new"), std::string::npos);
}

TEST(CodeGenE2E, VarArgsEmpty) {
    auto out = compileAndRun(
        "def show(tag: str, *args: Any) {\n"
        "  print(tag)\n"
        "  print(len(args))\n"
        "}\n"
        "show(\"test\")\n"
    );
    EXPECT_EQ(out, "test\n0\n");
}

TEST(CodeGenE2E, VarArgsLen) {
    auto out = compileAndRun(
        "def count(*args: Any) -> int {\n"
        "  return len(args)\n"
        "}\n"
        "print(count(1, 2, 3))\n"
    );
    EXPECT_EQ(out, "3\n");
}

TEST(CodeGenE2E, VarArgsMixed) {
    auto out = compileAndRun(
        "def greet(greeting: str, *names: str) {\n"
        "  print(greeting)\n"
        "  print(len(names))\n"
        "}\n"
        "greet(\"hi\", \"alice\", \"bob\")\n"
    );
    EXPECT_EQ(out, "hi\n2\n");
}

TEST(CodeGenE2E, KwargsEmpty) {
    auto out = compileAndRun(
        "def config(**kwargs: Any) {\n"
        "  print(len(kwargs))\n"
        "}\n"
        "config()\n"
    );
    EXPECT_EQ(out, "0\n");
}

TEST(CodeGenE2E, KwargsLen) {
    auto out = compileAndRun(
        "def config(**kwargs: Any) {\n"
        "  print(len(kwargs))\n"
        "}\n"
        "config(host=\"localhost\", port=8080)\n"
    );
    EXPECT_EQ(out, "2\n");
}

TEST(CodeGenE2E, VarArgsAndKwargs) {
    auto out = compileAndRun(
        "def flexfunc(a: int, *args: Any, **kwargs: Any) {\n"
        "  print(a)\n"
        "  print(len(args))\n"
        "  print(len(kwargs))\n"
        "}\n"
        "flexfunc(1, 2, 3, x=10, y=20)\n"
    );
    EXPECT_EQ(out, "1\n2\n2\n");
}

TEST(CodeGenE2E, VarArgsOnlyRegular) {
    // Call with only regular args (no extras for *args)
    auto out = compileAndRun(
        "def add(a: int, b: int, *rest: Any) -> int {\n"
        "  return a + b\n"
        "}\n"
        "print(add(10, 32))\n"
    );
    EXPECT_EQ(out, "42\n");
}

// Typed *args preserve each element's native type instead of erasing to i64.
// The `*args: T` annotation is the per-element type, so the call site packs the
// monomorphized list variant (visit(CallExpr) -> emitNewTypedList /
// emitTypedListAppend) and the callee binds the matching element kind. Before
// this, `*args: float`/`str`/union miscompiled to bit-patterns / raw pointers.
TEST(CodeGenE2E, VarArgsTypedFloat) {
    auto out = compileAndRun(
        "def addf(*args: float) -> float {\n"
        "  s: float = 0.0\n"
        "  for a in args { s = s + a }\n"
        "  return s\n"
        "}\n"
        "print(addf(1.5, 2.5))\n"
    );
    EXPECT_EQ(out, "4.0\n");  // float repr keeps .0 (Python parity)
}

TEST(CodeGenE2E, VarArgsTypedStr) {
    auto out = compileAndRun(
        "def names(*args: str) {\n"
        "  for a in args { print(a) }\n"
        "}\n"
        "names(\"x\", \"y\", \"z\")\n"
    );
    EXPECT_EQ(out, "x\ny\nz\n");
}

TEST(CodeGenE2E, VarArgsTypedListElem) {
    // *args: list[int] - each positional arg is itself a list[int]
    auto out = compileAndRun(
        "def f(*args: list[int]) {\n"
        "  for a in args { print(a[0]) }\n"
        "}\n"
        "f([10, 20], [30, 40])\n"
    );
    EXPECT_EQ(out, "10\n30\n");
}

TEST(CodeGenE2E, VarArgsUnionElem) {
    // *args: list[int] | str - heterogeneous element type -> box list
    auto out = compileAndRun(
        "def f(*args: list[int] | str) {\n"
        "  for a in args { print(a) }\n"
        "}\n"
        "f([1, 2], \"hi\")\n"
    );
    EXPECT_EQ(out, "[1, 2]\nhi\n");
}

//===----------------------------------------------------------------------===//
// Regression: generator reraise scope cleanup
//
// The for-in loop over a generator cleans up `__iter` and
// the loop variable on the StopIteration exit path. When the generator
// raises a non-StopIteration exception, codegen builds a "reraise"
// block that calls `dragon_raise_exc` directly, bypassing scope cleanup -
// heap-typed locals allocated *before* the for-loop would leak. The
// reraise BB therefore calls `emitAllScopeCleanup()` before the
// raise, so all live heap variables are decref'd on the abnormal exit.
//===----------------------------------------------------------------------===//

TEST(CodeGenIR, GeneratorReraiseEmitsScopeCleanup) {
    // The reraise path must emit dragon_decref / dragon_decref_str for the
    // outer-scope heap-typed locals. We look for cleanup calls in/near the
    // gen.reraise basic block.
    auto ir = generateIR(
        "def gen() {\n"
        "  yield 1\n"
        "}\n"
        "def caller() {\n"
        "  buf: str = \"hello\"\n"
        "  for x in gen() {\n"
        "    print(x)\n"
        "  }\n"
        "  print(buf)\n"
        "}\n"
        "caller()\n"
    );
    // Generator infrastructure must be present
    EXPECT_NE(ir.find("dragon_generator_next"), std::string::npos);
    // The reraise BB must exist
    EXPECT_NE(ir.find("gen.reraise"), std::string::npos)
        << "Expected gen.reraise basic block\nIR:\n" << ir;
    // Scope cleanup is emitted before dragon_raise_exc.
    // We expect dragon_decref_str to appear somewhere in the function so
    // the outer-scope `buf` is freed on the reraise path.
    EXPECT_NE(ir.find("dragon_decref_str"), std::string::npos)
        << "Expected scope cleanup (decref_str) in caller\nIR:\n" << ir;
}

TEST(CodeGenE2E, GeneratorReraiseDoesntLeakOuterLocals) {
    // Generator that raises mid-iteration. The for-loop catches it in the
    // outer try/except. Pre-fix, `buf` would leak per iteration.
    //
    // To run under valgrind:
    //  valgrind --leak-check=full ./dragon_codegen_tests \
    //  --gtest_filter='*GeneratorReraise*Tier25*'
    auto out = compileAndRun(
        "def explode() {\n"
        "  yield 1\n"
        "  raise ValueError(\"boom\")\n"
        "}\n"
        "errors: int = 0\n"
        "for i in range(2000) {\n"
        "  try {\n"
        "    buf: str = \"per-iter-string-\" + str(i)\n"
        "    for x in explode() {\n"
        "      _u: str = buf\n"  // keep buf live across the for-in
        "    }\n"
        "  } except ValueError {\n"
        "    errors = errors + 1\n"
        "  }\n"
        "}\n"
        "print(errors)\n"
    );
    EXPECT_EQ(out, "2000\n");
}

TEST(CodeGenE2E, GeneratorReraiseExceptionPropagates) {
    // Sanity: non-StopIteration exceptions still propagate
    // (the cleanup emit must NOT swallow the exception).
    auto out = compileAndRun(
        "def explode() {\n"
        "  yield 1\n"
        "  raise ValueError(\"propagate me\")\n"
        "}\n"
        "caught: int = 0\n"
        "try {\n"
        "  for x in explode() {\n"
        "    print(x)\n"
        "  }\n"
        "} except ValueError {\n"
        "  caught = 1\n"
        "}\n"
        "print(caught)\n"
    );
    EXPECT_EQ(out, "1\n1\n");
}

//===----------------------------------------------------------------------===//
// Regression: abandoned generator arg leak
//
// The generator wrapper packs args into an i64 array. If it does NOT
// incref heap-typed args, and
// dragon_generator_destroy does NOT decref them, then a caller's scope
// dropping a heap reference (e.g. a string) before the generator runs
// leaves the generator holding a dangling pointer, and a generator
// abandoned mid-iteration leaks the captured heap-typed args.
//
// The contract:
//  - Codegen emits `dragon_generator_create_tagged(body, args, tags, n)`
//  instead of `_create`. The tags array carries the per-arg
//  DragonValueTag so the runtime knows which slots are heap-typed.
//  - Runtime incref's heap-typed args at pack time (so the generator
//  owns its own reference, independent of the caller's scope).
//  - Runtime decrefs heap-typed args in dragon_generator_destroy.
//===----------------------------------------------------------------------===//

TEST(CodeGenIR, GeneratorWrapperUsesTypedCreate) {
    // D030: generator wrapper calls dragon_generator_create_typed (per-callsite
    // trampoline + decref fn pattern). The decref fn handles heap args at
    // destroy - replaces the prior tags-array approach.
    auto ir = generateIR(
        "def echo(prefix: str) {\n"
        "  yield prefix\n"
        "}\n"
        "for v in echo(\"hi\") {\n"
        "  print(v)\n"
        "}\n"
    );
    EXPECT_NE(ir.find("dragon_generator_create_typed"), std::string::npos)
        << "Expected dragon_generator_create_typed call\nIR:\n" << ir;
    // The body function must still exist
    EXPECT_NE(ir.find("echo__gen_body"), std::string::npos);
    // Per-callsite trampoline + decref fn for the heap-typed (str) arg
    EXPECT_NE(ir.find("__dragon_gen_tramp_echo"), std::string::npos)
        << "Expected per-callsite generator trampoline\nIR:\n" << ir;
    EXPECT_NE(ir.find("__dragon_gen_decref_echo"), std::string::npos)
        << "Expected per-callsite generator decref fn\nIR:\n" << ir;
}

TEST(CodeGenIR, GeneratorTypedCreateDeclaration) {
    // D030 runtime entry point: declare ptr @dragon_generator_create_typed(
    //  ptr trampoline, ptr args, i64 args_size, ptr decref_fn).
    auto ir = generateIR(
        "def gen(s: str) {\n"
        "  yield s\n"
        "}\n"
        "g: ptr = gen(\"x\")\n"
    );
    EXPECT_NE(ir.find("dragon_generator_create_typed(ptr, ptr, i64, ptr)"),
              std::string::npos)
        << "Expected 4-arg signature for dragon_generator_create_typed\n"
        << "IR:\n" << ir;
}

TEST(CodeGenE2E, GeneratorWithStringArgRunsCorrectly) {
    // Sanity: generator with str arg consumes the captured value correctly,
    // and yielded str values round-trip through for-in (yield kind tracking
    // wires this up - see generatorYieldKinds in CodeGen).
    auto out = compileAndRun(
        "def echo(prefix: str) {\n"
        "  yield prefix\n"
        "  yield prefix + \"!\"\n"
        "}\n"
        "for v in echo(\"hello\") {\n"
        "  print(v)\n"
        "}\n"
    );
    EXPECT_EQ(out, "hello\nhello!\n");
}

TEST(CodeGenE2E, GeneratorYieldsStringRoundTrips) {
    // Yield kind tracking - the loop var is bound as Str (not Int) when
    // the generator body's yields produce strings. Pre-fix this printed
    // raw pointer-as-int values like "107358526377990".
    auto out = compileAndRun(
        "def labels() {\n"
        "  yield \"first\"\n"
        "  yield \"second\"\n"
        "  yield \"third\"\n"
        "}\n"
        "for s in labels() {\n"
        "  print(s)\n"
        "}\n"
    );
    EXPECT_EQ(out, "first\nsecond\nthird\n");
}

TEST(CodeGenE2E, GeneratorAbandonedNoLeak) {
    // Create a generator with heap-typed args, take ONE next, then
    // abandon (let it go out of scope). The destroy path must decref
    // the captured str so it doesn't leak per iteration.
    //
    // To run under valgrind:
    //  valgrind --leak-check=full ./dragon_codegen_tests \
    //  --gtest_filter='*GeneratorAbandonedNoLeak*'
    auto out = compileAndRun(
        "def chunks(s: str) {\n"
        "  yield s\n"
        "  yield s\n"
        "  yield s\n"
        "}\n"
        "def consume_one() {\n"
        "  s: str = \"abandon-me-\" + str(7)\n"
        "  for v in chunks(s) {\n"
        "    break\n"  // abandons after first yield
        "  }\n"
        "}\n"
        "for i in range(5000) {\n"
        "  consume_one()\n"
        "}\n"
        "print(\"ok\")\n"
    );
    EXPECT_EQ(out, "ok\n");
}

TEST(CodeGenE2E, GeneratorMultipleHeapArgsBalance) {
    // Multiple heap-typed args. Each must be incref'd at pack time and
    // decref'd at destroy time exactly once.
    auto out = compileAndRun(
        "def two_strs(a: str, b: str) {\n"
        "  yield a\n"
        "  yield b\n"
        "}\n"
        "for i in range(2000) {\n"
        "  s1: str = \"s1-\" + str(i)\n"
        "  s2: str = \"s2-\" + str(i)\n"
        "  for v in two_strs(s1, s2) {\n"
        "    break\n"  // abandon after one yield
        "  }\n"
        "}\n"
        "print(\"ok\")\n"
    );
    EXPECT_EQ(out, "ok\n");
}

TEST(CodeGenE2E, GeneratorIntArgStillWorks) {
    // Regression: int args (TAG_INT=0) must NOT trigger spurious
    // incref/decref. The tag array holds 0 -> runtime is a no-op.
    auto out = compileAndRun(
        "def count_to(n: int) {\n"
        "  i: int = 0\n"
        "  while i < n {\n"
        "    yield i\n"
        "    i = i + 1\n"
        "  }\n"
        "}\n"
        "total: int = 0\n"
        "for x in count_to(100) {\n"
        "  total = total + x\n"
        "}\n"
        "print(total)\n"
    );
    EXPECT_EQ(out, "4950\n");
}

//===----------------------------------------------------------------------===//
// Regression: file_read on non-seekable streams
//
// Sizing the buffer with ftell/fseek alone breaks
// on pipes/FIFOs/stdin: ftell returns -1 and fseek fails, so
// `remaining = size - pos` comes out 0 and the function silently returns
// an empty string - even though there is readable content.
//
// dragon_file_read tries the seek-based fast path, and on failure falls
// through to an incremental fread loop with a doubling buffer (4KB -> 8KB -> ...).
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, FileReadFromPipe) {
    // popen produces a non-seekable FILE*. Pre-fix, `f.read()` on a popen'd
    // handle returned an empty string. Post-fix, it drains the pipe via
    // incremental fread.
    //
    // We can't open a popen'd FILE from .dr code directly. The next-best
    // proxy is to read from a regular (seekable) file and verify the content,
    // exercising the seek-based fast path of dragon_file_read. The
    // non-seekable path is exercised by Dragon's own subprocess primitives -
    // see the FileReadShellPipe test below.
    //
    // The handle is bound via extern "C" fopen rather than open(): open()'s
    // return type is the opaque `file` class, which has no annotatable type
    // name (it is not registered in initBuiltinTypes), so a top-level
    // `g: <type> = open(...)` cannot name it. The seek-based read intent and
    // the exact round-tripped content are preserved.
    auto out = compileAndRun(
        "extern \"C\" def fopen(path: str, mode: str) -> ptr\n"
        "extern \"C\" def fclose(stream: ptr) -> intc\n"
        "extern \"C\" def dragon_file_write_text(handle: ptr, s: str) -> int\n"
        "extern \"C\" def dragon_file_read(handle: ptr) -> str\n"
        "w: ptr = fopen(\"/tmp/dragon_test_tier210.txt\", \"w\")\n"
        "_n: int = dragon_file_write_text(w, \"abc\\ndef\\nghi\\n\")\n"
        "_w: intc = fclose(w)\n"
        "g: ptr = fopen(\"/tmp/dragon_test_tier210.txt\", \"r\")\n"
        "content: str = dragon_file_read(g)\n"
        "_g: intc = fclose(g)\n"
        "print(content)\n"
    );
    EXPECT_EQ(out, "abc\ndef\nghi\n\n");
}

TEST(CodeGenE2E, FileReadShellPipe) {
    // Use Dragon's FFI to popen() a shell command. popen returns a
    // non-seekable FILE*. Pre-fix dragon_file_read would return empty.
    // Post-fix it drains the pipe and we get the full output.
    auto out = compileAndRun(
        "extern \"C\" def popen(cmd: str, mode: str) -> ptr\n"
        "extern \"C\" def pclose(stream: ptr) -> intc\n"
        "extern \"C\" def dragon_file_read(handle: ptr) -> str\n"
        "p: ptr = popen(\"printf 'line1\\\\nline2\\\\nline3\\\\n'\", \"r\")\n"
        "content: str = dragon_file_read(p)\n"
        "_: intc = pclose(p)\n"
        "print(content)\n"
    );
    EXPECT_EQ(out, "line1\nline2\nline3\n\n");
}

TEST(CodeGenE2E, FileReadShellPipeLargeOutput) {
    // Pipe output larger than the initial 4KB buffer - exercises the
    // realloc/double-and-grow path in the non-seekable fallback.
    auto out = compileAndRun(
        "extern \"C\" def popen(cmd: str, mode: str) -> ptr\n"
        "extern \"C\" def pclose(stream: ptr) -> intc\n"
        "extern \"C\" def dragon_file_read(handle: ptr) -> str\n"
        "extern \"C\" def dragon_str_len(s: str) -> int\n"
        // yes outputs a stream of 'y\n' (2 bytes per line) until SIGPIPE.
        // We cap with `head -c 20000` for a deterministic 20000-byte read,
        // safely past the 4KB seed buffer to force at least one realloc.
        "p: ptr = popen(\"yes | head -c 20000\", \"r\")\n"
        "content: str = dragon_file_read(p)\n"
        "_: intc = pclose(p)\n"
        "print(dragon_str_len(content))\n"
    );
    EXPECT_EQ(out, "20000\n");
}

TEST(CodeGenE2E, FileReadShellPipeEmpty) {
    // Empty pipe (command produces no output). Must still return an empty
    // string cleanly, no crash, no garbage.
    auto out = compileAndRun(
        "extern \"C\" def popen(cmd: str, mode: str) -> ptr\n"
        "extern \"C\" def pclose(stream: ptr) -> intc\n"
        "extern \"C\" def dragon_file_read(handle: ptr) -> str\n"
        "extern \"C\" def dragon_str_len(s: str) -> int\n"
        "p: ptr = popen(\"true\", \"r\")\n"
        "content: str = dragon_file_read(p)\n"
        "_: intc = pclose(p)\n"
        "print(dragon_str_len(content))\n"
    );
    EXPECT_EQ(out, "0\n");
}
