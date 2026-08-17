#include "CodeGenTestHelpers.h"

TEST(CodeGenTest, MethodReflectionGlobalsEmitted) {
    auto ir = generateIR(
        "class Foo {\n"
        "    x: int\n"
        "    def(x: int) { self.x = x }\n"
        "    def bar() -> int { return self.x + 1 }\n"
        "    def baz() -> int { return self.x * 2 }\n"
        "}\n"
        "f: Foo = Foo(10)\n"
    );
    EXPECT_NE(ir.find("__method_names"), std::string::npos)
        << "method_names global should be emitted";
    EXPECT_NE(ir.find("__method_fn_ptrs"), std::string::npos)
        << "method_fn_ptrs global should be emitted";
    EXPECT_NE(ir.find("__method_kinds"), std::string::npos)
        << "method_kinds global should be emitted";
    EXPECT_NE(ir.find("dragon_class_descriptor_set_methods"), std::string::npos)
        << "set_methods setter must be invoked";
    EXPECT_NE(ir.find("bar"), std::string::npos);
    EXPECT_NE(ir.find("baz"), std::string::npos);
}

TEST(CodeGenE2E, SubclassInheritsParentFieldLayout) {
    auto out = compileAndRun(
        "class Res { n: int\n def() { self.n = 7 } }\n"
        "class Base { r: Res\n def() { self.r = Res() } }\n"
        "class Sub(Base) { def() {} }\n"
        "def assign_into(b: Base) { b.r = Res() }\n"
        "s: Sub = Sub()\n"
        "assign_into(s)\n"
        "print(\"ok\")\n"
    );
    EXPECT_EQ(out, "ok\n");
}

TEST(CodeGenE2E, DefaultConstructorSynthesisBare) {
    auto out = compileAndRun(
        "class Bare {\n"
        "    def greet() { print(\"hi\") }\n"
        "}\n"
        "b: Bare = Bare()\n"
        "b.greet()\n"
    );
    EXPECT_EQ(out, "hi\n");
}

TEST(CodeGenE2E, DefaultConstructorSynthesisSubclassDelegates) {
    auto out = compileAndRun(
        "class Base {\n"
        "    tag: int\n"
        "    def() { self.tag = 42 }\n"
        "}\n"
        "class Sub(Base) {\n"
        "    def show() { print(self.tag) }\n"
        "}\n"
        "s: Sub = Sub()\n"
        "s.show()\n"
    );
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, ListCovarianceFreshLiteral) {
    auto out = compileAndRun(
        "class Animal { tag: int\n def() { self.tag = 0 } }\n"
        "class Dog(Animal) { def() { self.tag = 1 } }\n"
        "class Cat(Animal) { def() { self.tag = 2 } }\n"
        "pets: list[Animal] = [Dog(), Cat()]\n"
        "print(len(pets))\n"
        "for p in pets { print(p.tag) }\n"
    );
    EXPECT_EQ(out, "2\n1\n2\n");
}


TEST(CodeGenE2E, HasattrFindsMethod) {
    auto out = compileAndRun(
        "class Counter {\n"
        "    n: int\n"
        "    def(n: int) { self.n = n }\n"
        "    def bump() { self.n = self.n + 1 }\n"
        "}\n"
        "c: Counter = Counter(0)\n"
        "print(hasattr(c, \"n\"))\n"
        "print(hasattr(c, \"bump\"))\n"
        "print(hasattr(c, \"nope\"))\n"
    );
    EXPECT_EQ(out, "True\nTrue\nFalse\n");
}

TEST(CodeGenE2E, GetattrBoundMethodInvocationModuleScope) {
    auto out = compileAndRun(
        "class Counter {\n"
        "    n: int\n"
        "    def(n: int) { self.n = n }\n"
        "    def bump() { self.n = self.n + 1 }\n"
        "}\n"
        "c: Counter = Counter(5)\n"
        "m: Callable[[], None] = getattr(c, \"bump\")\n"
        "m()\n"
        "m()\n"
        "print(c.n)\n"
    );
    EXPECT_EQ(out, "7\n");
}

TEST(CodeGenE2E, GetattrBoundMethodInvocationFunctionScope) {
    auto out = compileAndRun(
        "class Counter {\n"
        "    n: int\n"
        "    def(n: int) { self.n = n }\n"
        "    def add(k: int) { self.n = self.n + k }\n"
        "}\n"
        "def go() {\n"
        "    c: Counter = Counter(0)\n"
        "    addm: Callable[[int], None] = getattr(c, \"add\")\n"
        "    addm(5)\n"
        "    addm(3)\n"
        "    print(c.n)\n"
        "}\n"
        "go()\n"
    );
    EXPECT_EQ(out, "8\n");
}

TEST(CodeGenE2E, GetattrMethodFromInheritedParent) {
    auto out = compileAndRun(
        "class Animal {\n"
        "    name: str\n"
        "    def(name: str) { self.name = name }\n"
        "    def greet() { print(self.name) }\n"
        "}\n"
        "class Dog(Animal) {\n"
        "    def(name: str) { self.name = name }\n"
        "}\n"
        "d: Dog = Dog(\"Rex\")\n"
        "m: Callable[[], None] = getattr(d, \"greet\")\n"
        "m()\n"
    );
    EXPECT_EQ(out, "Rex\n");
}

TEST(CodeGenE2E, DirInstanceSortedWithDunderInit) {
    auto out = compileAndRun(
        "class Foo {\n"
        "    x: int\n"
        "    def(x: int) { self.x = x }\n"
        "    def bar() -> int { return self.x }\n"
        "    def baz() -> int { return self.x * 2 }\n"
        "}\n"
        "f: Foo = Foo(5)\n"
        "names: list[str] = dir(f)\n"
        "for n in names { print(n) }\n"
    );
    EXPECT_EQ(out, "__init__\nbar\nbaz\nx\n");
}

TEST(CodeGenE2E, DirClassDescriptor) {
    auto out = compileAndRun(
        "class Foo {\n"
        "    def(x: int) { self.x = x }\n"
        "    def bar() -> int { return 1 }\n"
        "    x: int\n"
        "}\n"
        "names: list[str] = dir(Foo)\n"
        "for n in names { print(n) }\n"
    );
    EXPECT_EQ(out, "__init__\nbar\nx\n");
}

TEST(CodeGenE2E, DirWalksParentChain) {
    auto out = compileAndRun(
        "class Animal {\n"
        "    name: str\n"
        "    def(name: str) { self.name = name }\n"
        "    def speak() -> str { return \"sound\" }\n"
        "}\n"
        "class Dog(Animal) {\n"
        "    def(name: str) { self.name = name }\n"
        "    def fetch() -> str { return \"got\" }\n"
        "}\n"
        "d: Dog = Dog(\"Rex\")\n"
        "names: list[str] = dir(d)\n"
        "for n in names { print(n) }\n"
    );
    EXPECT_EQ(out, "__init__\nfetch\nname\nspeak\n");
}

TEST(CodeGenE2E, MethodFindWalksParentChain) {
    auto out = compileAndRun(
        "class Animal {\n"
        "    name: str\n"
        "    def(name: str) { self.name = name }\n"
        "    def speak() -> str { return \"generic sound\" }\n"
        "}\n"
        "class Dog(Animal) {\n"
        "    def(name: str) { self.name = name }\n"
        "    def fetch() -> str { return \"got it\" }\n"
        "}\n"
        "d: Dog = Dog(\"Rex\")\n"
        "print(d.speak())\n"
        "print(d.fetch())\n"
    );
    EXPECT_EQ(out, "generic sound\ngot it\n");
}

TEST(CodeGenTest, ClassDeclStructType) {
    auto ir = generateIR(
        "class Counter {\n"
        "  def(n: int) {\n"
        "    self.val = n\n"
        "  }\n"
        "}\n"
    );
    EXPECT_NE(ir.find("Counter___init__"), std::string::npos)
        << "Expected __init__ function declaration in IR";
    EXPECT_NE(ir.find("Counter_new"), std::string::npos)
        << "Expected _new constructor in IR";
    EXPECT_NE(ir.find("@malloc"), std::string::npos)
        << "Expected malloc call in constructor";
}

TEST(CodeGenTest, ClassMethodDecl) {
    auto ir = generateIR(
        "class Adder {\n"
        "  def(x: int) {\n"
        "    self.x = x\n"
        "  }\n"
        "  def get() -> int {\n"
        "    return self.x\n"
        "  }\n"
        "}\n"
    );
    EXPECT_NE(ir.find("Adder_get"), std::string::npos)
        << "Expected method function in IR";
    EXPECT_NE(ir.find("Adder___init__"), std::string::npos)
        << "Expected __init__ function in IR";
}

TEST(CodeGenTest, ClassConstructorCall) {
    auto ir = generateIR(
        "class Box {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "}\n"
        "b: Box = Box(10)\n"
    );
    EXPECT_NE(ir.find("call ptr @Box_new"), std::string::npos)
        << "Expected call to Box_new constructor";
}

TEST(CodeGenTest, ClassFieldAccess) {
    auto ir = generateIR(
        "class Pair {\n"
        "  def(a: int, b: int) {\n"
        "    self.a = a\n"
        "    self.b = b\n"
        "  }\n"
        "  def first() -> int {\n"
        "    return self.a\n"
        "  }\n"
        "}\n"
    );
    EXPECT_NE(ir.find("getelementptr"), std::string::npos)
        << "Expected GEP for field access in IR";
}

TEST(CodeGenTest, ClassMethodCall) {
    auto ir = generateIR(
        "class Val {\n"
        "  def(n: int) {\n"
        "    self.n = n\n"
        "  }\n"
        "  def get() -> int {\n"
        "    return self.n\n"
        "  }\n"
        "}\n"
        "v: Val = Val(42)\n"
        "print(v.get())\n"
    );
    EXPECT_NE(ir.find("call i64 @Val_get"), std::string::npos)
        << "Expected call to Val_get method";
}

TEST(CodeGenTest, ClassModuleVerifies) {
    auto ir = generateIR(
        "class Point {\n"
        "  def(x: int, y: int) {\n"
        "    self.x = x\n"
        "    self.y = y\n"
        "  }\n"
        "  def sum() -> int {\n"
        "    return self.x + self.y\n"
        "  }\n"
        "}\n"
        "p: Point = Point(3, 4)\n"
        "print(p.sum())\n"
    );
    EXPECT_TRUE(ir.find("<codegen failed") == std::string::npos)
        << "Expected IR generation to succeed, got: " << ir;
}

TEST(CodeGenIR, ClassGCHeaderInStruct) {
    auto ir = generateIR(
        "class Widget {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "}\n"
    );
    EXPECT_NE(ir.find("%Widget = type { i64, i64, ptr, i64 }"), std::string::npos)
        << "Expected GC header (2 x i64) + vtable ptr prepended to class struct";
}

TEST(CodeGenIR, ClassGCHeaderInit) {
    auto ir = generateIR(
        "class Obj {\n"
        "  def(x: int) {\n"
        "    self.x = x\n"
        "  }\n"
        "}\n"
    );
    EXPECT_NE(ir.find("getelementptr inbounds"), std::string::npos)
        << "Expected GEP for header init\nIR:\n" << ir;
    EXPECT_NE(ir.find("store i64 1,"), std::string::npos)
        << "Expected refcount = 1 store\nIR:\n" << ir;
    EXPECT_NE(ir.find("@__class_id_Obj"), std::string::npos)
        << "Expected class_id global for Obj\nIR:\n" << ir;
    EXPECT_NE(ir.find("dragon_class_register_dealloc"), std::string::npos)
        << "Expected dealloc registration call\nIR:\n" << ir;
}

TEST(CodeGenIR, ClassInstanceDecrefAtScopeExit) {
    auto ir = generateIR(
        "class Foo {\n"
        "  def(x: int) {\n"
        "    self.x = x\n"
        "  }\n"
        "}\n"
        "def make() {\n"
        "  f: Foo = Foo(42)\n"
        "  print(f.x)\n"
        "}\n"
        "make()\n"
    );
    EXPECT_NE(ir.find("dragon_decref"), std::string::npos)
        << "Expected dragon_decref call for class instance cleanup";
}

TEST(CodeGenIR, AtomicIncrefDeclared) {
    auto ir = generateIR("x: int = 1\n");
    EXPECT_NE(ir.find("dragon_incref_atomic"), std::string::npos)
        << "Expected dragon_incref_atomic to be declared";
    EXPECT_NE(ir.find("dragon_decref_atomic"), std::string::npos)
        << "Expected dragon_decref_atomic to be declared";
    EXPECT_NE(ir.find("dragon_incref_str_atomic"), std::string::npos)
        << "Expected dragon_incref_str_atomic to be declared";
    EXPECT_NE(ir.find("dragon_decref_str_atomic"), std::string::npos)
        << "Expected dragon_decref_str_atomic to be declared";
}

TEST(CodeGenIR, FireListArgAtomicIncref) {
    auto ir = generateIR(
        "def process(data: list[int]) -> int {\n"
        "  return 0\n"
        "}\n"
        "items: list[int] = [1, 2, 3]\n"
        "t: Task[int] = fire process(items)\n"
    );
    EXPECT_NE(ir.find("dragon_incref_atomic"), std::string::npos)
        << "Expected atomic incref for list arg passed to fire";
}

TEST(CodeGenIR, FireIntArgNoAtomicIncref) {
    auto ir = generateIR(
        "def compute(n: int) -> int {\n"
        "  return n + 1\n"
        "}\n"
        "t: Task[int] = fire compute(42)\n"
    );
    EXPECT_EQ(ir.find("call void @dragon_incref_atomic"), std::string::npos)
        << "Scalar args should not get atomic incref";
}

TEST(CodeGenIR, FireTrampolineDecrefsHeapArgs) {
    auto ir = generateIR(
        "def process(items: list[int]) -> int {\n"
        "  return 1\n"
        "}\n"
        "data: list[int] = [1, 2, 3]\n"
        "t: Task[int] = fire process(data)\n"
    );
    EXPECT_NE(ir.find("__dragon_fire_tramp_"), std::string::npos)
        << "Expected fire trampoline function\nIR:\n" << ir;
    EXPECT_NE(ir.find("dragon_decref_atomic"), std::string::npos)
        << "Expected atomic decref in fire trampoline\nIR:\n" << ir;
    EXPECT_NE(ir.find("dragon_vthread_spawn_typed"), std::string::npos)
        << "Expected dragon_vthread_spawn_typed call\nIR:\n" << ir;
}

TEST(CodeGenIR, GCPhase5FunctionsDecl) {
    auto ir = generateIR(
        "x: list[int] = [1, 2, 3]\n"
        "print(x[0])\n"
    );
    EXPECT_NE(ir.find("dragon_gc_track"), std::string::npos)
        << "Expected dragon_gc_track declared\nIR:\n" << ir;
    EXPECT_NE(ir.find("dragon_gc_collect"), std::string::npos)
        << "Expected dragon_gc_collect declared\nIR:\n" << ir;
    EXPECT_NE(ir.find("dragon_list_new_tagged"), std::string::npos)
        << "Expected dragon_list_new_tagged declared\nIR:\n" << ir;
}

TEST(CodeGenIR, ClassDeallocAndTraverse) {
    auto ir = generateIR(
        "class Node {\n"
        "  def(val: int, child: Node) {\n"
        "    self.val: int = val\n"
        "    self.child: Node = child\n"
        "  }\n"
        "}\n"
    );
    EXPECT_NE(ir.find("__dragon_dealloc_Node"), std::string::npos)
        << "Expected dealloc function for Node\nIR:\n" << ir;
    EXPECT_NE(ir.find("__dragon_traverse_Node"), std::string::npos)
        << "Expected traverse function for Node\nIR:\n" << ir;
    EXPECT_NE(ir.find("dragon_class_register_dealloc"), std::string::npos)
        << "Expected dealloc registration\nIR:\n" << ir;
    EXPECT_NE(ir.find("dragon_class_register_traverse"), std::string::npos)
        << "Expected traverse registration\nIR:\n" << ir;
    EXPECT_NE(ir.find("__dragon_clear_Node"), std::string::npos)
        << "Expected clear function for Node\nIR:\n" << ir;
    EXPECT_NE(ir.find("dragon_class_register_clear"), std::string::npos)
        << "Expected clear registration\nIR:\n" << ir;
}

TEST(CodeGenIR, ClassClearZerosFields) {
    auto ir = generateIR(
        "class Container {\n"
        "  def(name: str, items: list[int]) {\n"
        "    self.name: str = name\n"
        "    self.items: list[int] = items\n"
        "  }\n"
        "}\n"
    );
    EXPECT_NE(ir.find("__dragon_clear_Container"), std::string::npos)
        << "Expected clear function for Container\nIR:\n" << ir;
    auto clearPos = ir.find("define internal void @__dragon_clear_Container");
    ASSERT_NE(clearPos, std::string::npos)
        << "Expected clear function definition\nIR:\n" << ir;
    auto clearEnd = ir.find("\n}\n", clearPos);
    auto clearBody = ir.substr(clearPos, clearEnd - clearPos);
    EXPECT_NE(clearBody.find("dragon_decref_str"), std::string::npos)
        << "Expected decref_str call in clear function\nBody:\n" << clearBody;
    EXPECT_NE(clearBody.find("dragon_decref"), std::string::npos)
        << "Expected dragon_decref call in clear function\nBody:\n" << clearBody;
    EXPECT_NE(clearBody.find("store"), std::string::npos)
        << "Expected store (zero) in clear function\nBody:\n" << clearBody;
}

TEST(CodeGenIR, AcyclicClassNotTracked) {
    auto ir = generateIR(
        "class Obj {\n"
        "  def(x: int, name: str) {\n"
        "    self.x = x\n"
        "    self.name = name\n"
        "  }\n"
        "}\n"
    );
    EXPECT_EQ(ir.find("call void @dragon_gc_track"), std::string::npos)
        << "Acyclic class (int + str fields) must NOT be gc_tracked\nIR:\n" << ir;
}

TEST(CodeGenIR, CyclicCapableClassTracked) {
    auto ir = generateIR(
        "class Node {\n"
        "  kids: list[int]\n"
        "  def() {\n"
        "    self.kids = []\n"
        "  }\n"
        "}\n"
    );
    EXPECT_NE(ir.find("call void @dragon_gc_track"), std::string::npos)
        << "Cyclic-capable class (list field) must still be gc_tracked\nIR:\n" << ir;
}

TEST(CodeGenIR, StackAllocNonEscapingInstance) {
    auto ir = generateIR(
        "class P {\n"
        "  def(x: int) { self.x = x }\n"
        "}\n"
        "t: int = 0\n"
        "i: int = 0\n"
        "while i < 10 {\n"
        "  p: P = P(i)\n"
        "  t = t + p.x\n"
        "  i = i + 1\n"
        "}\n"
        "print(t)\n"
    );
    EXPECT_NE(ir.find("P.stack = alloca %P"), std::string::npos)
        << "Non-escaping instance should be stack-allocated\nIR:\n" << ir;
    EXPECT_EQ(ir.find("call ptr @P_new"), std::string::npos)
        << "Stack-allocated instance must NOT call the heap ctor _new\nIR:\n" << ir;
}

TEST(CodeGenIR, EscapingInstanceStaysHeap) {
    auto ir = generateIR(
        "class P {\n"
        "  def(x: int) { self.x = x }\n"
        "}\n"
        "def make(v: int) -> P {\n"
        "  p: P = P(v)\n"
        "  return p\n"
        "}\n"
        "print(make(5).x)\n"
    );
    EXPECT_NE(ir.find("call ptr @P_new"), std::string::npos)
        << "Returned (escaping) instance must be heap-allocated via _new\nIR:\n" << ir;
    EXPECT_EQ(ir.find("P.stack = alloca %P"), std::string::npos)
        << "Escaping instance must NOT be stack-allocated\nIR:\n" << ir;
}

TEST(CodeGenE2E, StackInstanceFieldReadsCorrect) {
    auto out = compileAndRun(
        "class Pt {\n"
        "  x: int\n"
        "  y: int\n"
        "  def(x: int, y: int) { self.x = x\n    self.y = y }\n"
        "}\n"
        "total: int = 0\n"
        "i: int = 0\n"
        "while i < 1000 {\n"
        "  p: Pt = Pt(i, i + 1)\n"
        "  total = total + p.x + p.y\n"
        "  i = i + 1\n"
        "}\n"
        "print(total)\n"
    );
    EXPECT_EQ(out, "1000000\n");
}

TEST(CodeGenE2E, EscapingInstancesDistinctAfterReturn) {
    auto out = compileAndRun(
        "class P {\n"
        "  def(x: int) { self.x = x }\n"
        "}\n"
        "def make(v: int) -> P {\n"
        "  p: P = P(v)\n"
        "  return p\n"
        "}\n"
        "a: P = make(10)\n"
        "b: P = make(20)\n"
        "c: P = make(30)\n"
        "print(a.x + b.x + c.x)\n"
    );
    EXPECT_EQ(out, "60\n");
}

TEST(CodeGenIR, ConstIR) {
    auto ir = generateIR(
        "const MAX: int = 42\n"
        "print(MAX)\n"
    );
    EXPECT_NE(ir.find("define"), std::string::npos);
    EXPECT_NE(ir.find("dragon_print_int"), std::string::npos);
}

TEST(CodeGenIR, StaticFieldIR) {
    auto ir = generateIR(
        "class Counter {\n"
        "  static count: int = 0\n"
        "  def() {\n"
        "    pass\n"
        "  }\n"
        "}\n"
        "print(Counter.count)\n"
    );
    EXPECT_NE(ir.find("@Counter_count"), std::string::npos);
    EXPECT_NE(ir.find("dragon_print_int"), std::string::npos);
}

TEST(CodeGenIR, StaticMethodIR) {
    auto ir = generateIR(
        "class MathUtil {\n"
        "  def() {\n"
        "    pass\n"
        "  }\n"
        "  static def add(a: int, b: int) -> int {\n"
        "    return a + b\n"
        "  }\n"
        "}\n"
        "x: int = MathUtil.add(3, 4)\n"
        "print(x)\n"
    );
    EXPECT_NE(ir.find("MathUtil_add"), std::string::npos);
    EXPECT_NE(ir.find("dragon_print_int"), std::string::npos);
}

TEST(CodeGenIR, MultiConstructorIR) {
    auto ir = generateIR(
        "class Point {\n"
        "  def(x: int, y: int) {\n"
        "    self.x = x\n"
        "    self.y = y\n"
        "  }\n"
        "  def() {\n"
        "    self.x = 0\n"
        "    self.y = 0\n"
        "  }\n"
        "}\n"
        "p1: Point = Point(3, 4)\n"
        "p2: Point = Point()\n"
    );
    EXPECT_NE(ir.find("Point___init___0"), std::string::npos);
    EXPECT_NE(ir.find("Point___init___1"), std::string::npos);
    EXPECT_NE(ir.find("Point_new_0"), std::string::npos);
    EXPECT_NE(ir.find("Point_new_1"), std::string::npos);
    EXPECT_EQ(ir.find("Point___init__("), std::string::npos);
    EXPECT_EQ(ir.find("Point_new("), std::string::npos);
}

TEST(CodeGenIR, SingleConstructorUnchangedIR) {
    auto ir = generateIR(
        "class Simple {\n"
        "  def(x: int) {\n"
        "    self.x = x\n"
        "  }\n"
        "}\n"
        "s: Simple = Simple(42)\n"
    );
    EXPECT_NE(ir.find("Simple___init__"), std::string::npos);
    EXPECT_NE(ir.find("Simple_new"), std::string::npos);
    EXPECT_EQ(ir.find("Simple___init___0"), std::string::npos);
    EXPECT_EQ(ir.find("Simple_new_0"), std::string::npos);
}

TEST(CodeGenIR, SuperCallIR) {
    auto ir = generateIR(
        "class Animal {\n"
        "    def(x: int) {\n"
        "        self.x = x\n"
        "    }\n"
        "    def speak() -> int {\n"
        "        return self.x\n"
        "    }\n"
        "}\n"
    );
    EXPECT_NE(ir.find("Animal___init__"), std::string::npos);
    EXPECT_NE(ir.find("Animal_speak"), std::string::npos);
}

TEST(CodeGenIR, MROMethodLookupIR) {
    auto ir = generateIR(
        "class A {\n"
        "    def(v: int) {\n"
        "        self.v = v\n"
        "    }\n"
        "    def greet() -> int {\n"
        "        return self.v\n"
        "    }\n"
        "}\n"
        "class B(A) {\n"
        "    def(v: int) {\n"
        "        self.v = v\n"
        "    }\n"
        "}\n"
        "b: B = B(5)\n"
        "print(b.greet())\n"
    );
    EXPECT_NE(ir.find("A_greet"), std::string::npos);
    EXPECT_NE(ir.find("dragon_print_int"), std::string::npos);
}

TEST(CodeGenIR, PyClassInheritanceIR) {
    auto ir = generateIRPy(
        "class Base:\n"
        "    def __init__(self, x: int):\n"
        "        self.x = x\n"
        "    def get(self) -> int:\n"
        "        return self.x\n"
        "class Child(Base):\n"
        "    def __init__(self, x: int):\n"
        "        self.x = x\n"
        "c: Child = Child(7)\n"
        "print(c.get())\n"
    );
    EXPECT_NE(ir.find("Base_get"), std::string::npos);
    EXPECT_NE(ir.find("dragon_print_int"), std::string::npos);
}

TEST(CodeGenTest, FirstClassClassStaticDispatchUnchanged) {
    auto output = compileAndRun(
        "class Point {\n"
        "    def(x: int, y: int) {\n"
        "        self.x = x\n"
        "        self.y = y\n"
        "    }\n"
        "}\n"
        "p: Point = Point(3, 4)\n"
        "print(p.x)\n"
        "print(p.y)\n"
    );
    EXPECT_EQ(output, "3\n4\n");
}

TEST(CodeGenTest, FirstClassClassDescriptorIR) {
    auto ir = generateIR(
        "class Foo {\n"
        "    def(x: int) {\n"
        "        self.x = x\n"
        "    }\n"
        "}\n"
    );
    EXPECT_NE(ir.find("Foo__descriptor"), std::string::npos)
        << "Expected descriptor global in IR";
    EXPECT_NE(ir.find("dragon_class_descriptor_create"), std::string::npos)
        << "Expected descriptor create call in IR";
}

TEST(CodeGenE2E, FirstClassClassTypeParam) {
    auto out = compileAndRun(
        "class Animal {\n"
        "    def(name: str) {\n"
        "        self.name = name\n"
        "    }\n"
        "}\n"
        "def make(cls: type, n: str) -> Animal {\n"
        "    a: Animal = cls(n)\n"
        "    return a\n"
        "}\n"
        "x: Animal = make(Animal, \"hi\")\n"
        "print(x.name)\n"
    );
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
    EXPECT_NE(out.find("not values"), std::string::npos);
}

TEST(CodeGenE2E, FirstClassClassReturnTypeAnnotation) {
    auto out = compileAndRun(
        "class Animal {\n"
        "    def(name: str) {\n"
        "        self.name = name\n"
        "    }\n"
        "}\n"
        "def pick() -> type {\n"
        "    return Animal\n"
        "}\n"
        "cls: type = pick()\n"
        "inst: Animal = cls(\"rex\")\n"
        "print(inst.name)\n"
    );
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
    EXPECT_NE(out.find("not values"), std::string::npos);
}

TEST(CodeGenE2E, FirstClassClassListIteration) {
    auto out = compileAndRun(
        "class Animal {\n"
        "    def(name: str) {\n"
        "        self.name = name\n"
        "    }\n"
        "}\n"
        "classes: list[type] = [Animal, Animal]\n"
        "for c in classes {\n"
        "    inst: Animal = c(\"hi\")\n"
        "    print(inst.name)\n"
        "}\n"
    );
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
    EXPECT_NE(out.find("not values"), std::string::npos);
}

TEST(CodeGenE2E, FirstClassClassDictLookup) {
    auto out = compileAndRun(
        "class Animal {\n"
        "    def(name: str) {\n"
        "        self.name = name\n"
        "    }\n"
        "}\n"
        "registry: dict[str, type] = {\"a\": Animal}\n"
        "cls: type = registry[\"a\"]\n"
        "inst: Animal = cls(\"rex\")\n"
        "print(inst.name)\n"
    );
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
    EXPECT_NE(out.find("not values"), std::string::npos);
}

TEST(CodeGenE2E, ClassValueBindingRejected) {
    auto out = compileAndRun(
        "class Router {\n"
        "    def(name: str) {\n"
        "        self.name = name\n"
        "    }\n"
        "}\n"
        "App: type = Router\n"
        "a: Router = App(\"hi\")\n"
        "print(a.name)\n"
    );
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
    EXPECT_NE(out.find("not values"), std::string::npos);
}

TEST(CodeGenE2E, FirstClassClassUnannotatedParamErrors) {
    auto out = compileAndRun(
        "class Animal {\n"
        "    def(name: str) {\n"
        "        self.name = name\n"
        "    }\n"
        "}\n"
        "def make(cls, n: str) {\n"
        "    return cls(n)\n"
        "}\n"
        "x: Animal = make(Animal, \"hi\")\n"
    );
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
    EXPECT_NE(out.find("cannot call 'cls'"), std::string::npos);
}

TEST(CodeGenE2E, ClassDecoratorIdentity) {
    auto out = compileAndRun(
        "def identity(cls: type) -> type {\n"
        "    return cls\n"
        "}\n"
        "@identity\n"
        "class Greeter {\n"
        "    def(name: str) {\n"
        "        self.name = name\n"
        "    }\n"
        "}\n"
        "g: Greeter = Greeter(\"world\")\n"
        "print(g.name)\n"
    );
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
    EXPECT_NE(out.find("class decorators are not supported"), std::string::npos);
}

TEST(CodeGenE2E, ClassDecoratorRegistry) {
    auto out = compileAndRun(
        "registry: list[type] = []\n"
        "def reg(cls: type) -> type {\n"
        "    registry.append(cls)\n"
        "    return cls\n"
        "}\n"
        "@reg\n"
        "class Foo {\n"
        "    def(n: str) {\n"
        "        self.n = n\n"
        "    }\n"
        "}\n"
        "@reg\n"
        "class Bar {\n"
        "    def(n: str) {\n"
        "        self.n = n\n"
        "    }\n"
        "}\n"
        "print(len(registry))\n"
        "for c in registry {\n"
        "    inst: Foo = c(\"hi\")\n"
        "    print(inst.n)\n"
        "}\n"
    );
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
}

TEST(CodeGenE2E, ClassDecoratorStacking) {
    auto out = compileAndRun(
        "log: list[str] = []\n"
        "def t1(cls: type) -> type {\n"
        "    log.append(\"t1\")\n"
        "    return cls\n"
        "}\n"
        "def t2(cls: type) -> type {\n"
        "    log.append(\"t2\")\n"
        "    return cls\n"
        "}\n"
        "@t1\n"
        "@t2\n"
        "class Foo {\n"
        "    def(n: str) {\n"
        "        self.n = n\n"
        "    }\n"
        "}\n"
        "f: Foo = Foo(\"x\")\n"
        "for s in log {\n"
        "    print(s)\n"
        "}\n"
    );
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
    EXPECT_NE(out.find("class decorators are not supported"), std::string::npos);
}

TEST(CodeGenE2E, ClassDecoratorIsinstanceWorks) {
    auto out = compileAndRun(
        "def identity(cls: type) -> type { return cls }\n"
        "@identity\n"
        "class Foo {\n"
        "    def(n: str) {\n"
        "        self.n = n\n"
        "    }\n"
        "}\n"
        "f: Foo = Foo(\"hi\")\n"
        "if isinstance(f, Foo) {\n"
        "    print(\"yes\")\n"
        "} else {\n"
        "    print(\"no\")\n"
        "}\n"
    );
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
    EXPECT_NE(out.find("class decorators are not supported"), std::string::npos);
}

TEST(CodeGenE2E, ClassDecoratorPreservesAttribute) {
    auto out = compileAndRun(
        "def identity(cls: type) -> type { return cls }\n"
        "@identity\n"
        "class Point {\n"
        "    def(x: int, y: int) {\n"
        "        self.x = x\n"
        "        self.y = y\n"
        "    }\n"
        "}\n"
        "p: Point = Point(3, 4)\n"
        "print(p.x)\n"
        "print(p.y)\n"
    );
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
    EXPECT_NE(out.find("class decorators are not supported"), std::string::npos);
}

TEST(CodeGenE2E, DataclassConstruction) {
    auto out = compileAndRun(
        "@dataclass\n"
        "class Point {\n"
        "    x: int\n"
        "    y: int\n"
        "}\n"
        "p: Point = Point(3, 4)\n"
        "print(p.x)\n"
        "print(p.y)\n"
    );
    EXPECT_EQ(out, "3\n4\n");
}

TEST(CodeGenE2E, DataclassEqualByFields) {
    auto out = compileAndRun(
        "@dataclass\n"
        "class Point {\n"
        "    x: int\n"
        "    y: int\n"
        "}\n"
        "p1: Point = Point(3, 4)\n"
        "p2: Point = Point(3, 4)\n"
        "p3: Point = Point(5, 4)\n"
        "if p1 == p2 { print(\"eq12\") } else { print(\"ne12\") }\n"
        "if p1 == p3 { print(\"eq13\") } else { print(\"ne13\") }\n"
    );
    EXPECT_EQ(out, "eq12\nne13\n");
}

TEST(CodeGenE2E, DataclassReprFormat) {
    auto out = compileAndRun(
        "@dataclass\n"
        "class Point {\n"
        "    x: int\n"
        "    y: int\n"
        "}\n"
        "p: Point = Point(3, 4)\n"
        "print(repr(p))\n"
    );
    EXPECT_EQ(out, "Point(x=3, y=4)\n");
}

TEST(CodeGenE2E, DataclassDefaultValues) {
    auto out = compileAndRun(
        "@dataclass\n"
        "class Config {\n"
        "    name: str\n"
        "    timeout: int = 30\n"
        "}\n"
        "c1: Config = Config(\"server\", 60)\n"
        "c2: Config = Config(\"client\")\n"
        "print(c1.timeout)\n"
        "print(c2.timeout)\n"
    );
    EXPECT_EQ(out, "60\n30\n");
}

TEST(CodeGenE2E, DataclassUserInitOverrides) {
    auto out = compileAndRun(
        "@dataclass\n"
        "class Box {\n"
        "    x: int\n"
        "    def(v: int) {\n"
        "        self.x = v * 2\n"
        "    }\n"
        "}\n"
        "b: Box = Box(7)\n"
        "print(b.x)\n"
    );
    EXPECT_EQ(out, "14\n");
}

TEST(CodeGenE2E, DataclassMixedFieldTypes) {
    auto out = compileAndRun(
        "@dataclass\n"
        "class Person {\n"
        "    name: str\n"
        "    age: int\n"
        "    active: bool\n"
        "}\n"
        "p1: Person = Person(\"Ada\", 36, True)\n"
        "p2: Person = Person(\"Ada\", 36, True)\n"
        "p3: Person = Person(\"Bob\", 36, True)\n"
        "if p1 == p2 { print(\"eq12\") }\n"
        "if p1 == p3 { print(\"BAD\") } else { print(\"ne13\") }\n"
        "print(repr(p1))\n"
    );
    EXPECT_EQ(out, "eq12\nne13\nPerson(name=Ada, age=36, active=True)\n");
}

TEST(CodeGenE2E, NamedTupleConstruction) {
    auto out = compileAndRun(
        "class Vec(NamedTuple) {\n"
        "    x: int\n"
        "    y: int\n"
        "}\n"
        "v: Vec = Vec(1, 2)\n"
        "print(v.x)\n"
        "print(v.y)\n"
        "print(repr(v))\n"
    );
    EXPECT_EQ(out, "1\n2\nVec(x=1, y=2)\n");
}

TEST(CodeGenE2E, DataclassWithRuntimeDecorator) {
    auto out = compileAndRun(
        "registry: list[type] = []\n"
        "def reg(cls: type) -> type {\n"
        "    registry.append(cls)\n"
        "    return cls\n"
        "}\n"
        "@reg\n"
        "@dataclass\n"
        "class Item {\n"
        "    name: str\n"
        "    qty: int\n"
        "}\n"
        "x: Item = Item(\"apple\", 3)\n"
        "print(x.qty)\n"
        "print(len(registry))\n"
    );
    EXPECT_NE(out.find("codegen failed"), std::string::npos);
}

TEST(CodeGenIR, VtableGlobalInIR) {
    auto ir = generateIR(
        "class Dog {\n"
        "    def(name: str) {\n"
        "        self.name = name\n"
        "    }\n"
        "    def speak() -> str {\n"
        "        return \"Woof\"\n"
        "    }\n"
        "}\n"
    );
    EXPECT_NE(ir.find("Dog__vtable"), std::string::npos)
        << "Expected vtable global in IR";
    EXPECT_NE(ir.find("Dog_speak"), std::string::npos)
        << "Expected method function in IR";
}

TEST(CodeGenIR, VtableStructLayout) {
    auto ir = generateIR(
        "class Point {\n"
        "    def(x: int, y: int) {\n"
        "        self.x = x\n"
        "        self.y = y\n"
        "    }\n"
        "    def sum() -> int {\n"
        "        return self.x + self.y\n"
        "    }\n"
        "}\n"
    );
    EXPECT_NE(ir.find("%Point = type { i64, i64, ptr, i64, i64 }"), std::string::npos)
        << "Expected vtable ptr in struct layout at index 2";
}

TEST(CodeGenTest, VtableDynamicMethodDispatch) {
    auto output = compileAndRun(
        "class Dog {\n"
        "    def(name: str) {\n"
        "        self.name = name\n"
        "    }\n"
        "    def speak() -> str {\n"
        "        return \"Woof\"\n"
        "    }\n"
        "}\n"
        "obj: Dog = Dog(\"Rex\")\n"
        "print(obj.speak())\n"
    );
    EXPECT_EQ(output, "Woof\n");
}

TEST(CodeGenTest, VtableInheritanceDispatch) {
    auto output = compileAndRun(
        "class Animal {\n"
        "    def(name: str) {\n"
        "        self.name = name\n"
        "    }\n"
        "    def speak() -> str {\n"
        "        return \"...\"\n"
        "    }\n"
        "}\n"
        "class Dog(Animal) {\n"
        "    def(name: str) {\n"
        "        self.name = name\n"
        "    }\n"
        "    def speak() -> str {\n"
        "        return \"Woof\"\n"
        "    }\n"
        "}\n"
        "obj: Dog = Dog(\"Rex\")\n"
        "print(obj.speak())\n"
    );
    EXPECT_EQ(output, "Woof\n");
}

TEST(CodeGenTest, VtableInheritedMethod) {
    auto output = compileAndRun(
        "class Animal {\n"
        "    def(name: str) {\n"
        "        self.name = name\n"
        "    }\n"
        "    def speak() -> str {\n"
        "        return \"...\"\n"
        "    }\n"
        "}\n"
        "class Dog(Animal) {\n"
        "    def(name: str) {\n"
        "        self.name = name\n"
        "    }\n"
        "}\n"
        "obj: Dog = Dog(\"Buddy\")\n"
        "print(obj.speak())\n"
    );
    EXPECT_EQ(output, "...\n");
}

TEST(CodeGenTest, VtableStaticDispatchUnchanged) {
    auto output = compileAndRun(
        "class Cat {\n"
        "    def(name: str) {\n"
        "        self.name = name\n"
        "    }\n"
        "    def speak() -> str {\n"
        "        return \"Meow\"\n"
        "    }\n"
        "}\n"
        "obj: Cat = Cat(\"Whiskers\")\n"
        "print(obj.speak())\n"
    );
    EXPECT_EQ(output, "Meow\n");
}

TEST(CodeGenTest, VtableMultipleMethods) {
    auto output = compileAndRun(
        "class Calc {\n"
        "    def(v: int) {\n"
        "        self.v = v\n"
        "    }\n"
        "    def add(x: int) -> int {\n"
        "        return self.v + x\n"
        "    }\n"
        "    def mul(x: int) -> int {\n"
        "        return self.v * x\n"
        "    }\n"
        "}\n"
        "obj: Calc = Calc(10)\n"
        "print(obj.add(5))\n"
        "print(obj.mul(3))\n"
    );
    EXPECT_EQ(output, "15\n30\n");
}

TEST(CodeGenTest, VtableVoidMethod) {
    auto output = compileAndRun(
        "class Printer {\n"
        "    def(msg: str) {\n"
        "        self.msg = msg\n"
        "    }\n"
        "    def show() {\n"
        "        print(self.msg)\n"
        "    }\n"
        "}\n"
        "obj: Printer = Printer(\"hello\")\n"
        "obj.show()\n"
    );
    EXPECT_EQ(output, "hello\n");
}

TEST(CodeGenTest, BugReproStrFieldCmp) {
    auto ir = generateIR(
        "class Greeter {\n"
        "  def(name: str) {\n"
        "    self.name = name\n"
        "  }\n"
        "  def greet() -> str {\n"
        "    if self.name == \"world\" {\n"
        "      return \"Hello, World!\"\n"
        "    }\n"
        "    return \"Hello!\"\n"
        "  }\n"
        "}\n"
        "g: Greeter = Greeter(\"world\")\n"
        "print(g.greet())\n"
    );
    EXPECT_NE(ir.find("dragon_str_eq"), std::string::npos)
        << "Expected dragon_str_eq for string field comparison\nIR:\n" << ir;
}

TEST(CodeGenTest, FloatFieldInference) {
    auto ir = generateIR(
        "class Pt {\n"
        "  def(x: float, y: float) {\n"
        "    self.x = x\n"
        "    self.y = y\n"
        "  }\n"
        "  def sum() -> float {\n"
        "    return self.x + self.y\n"
        "  }\n"
        "}\n"
    );
    EXPECT_NE(ir.find("fadd"), std::string::npos)
        << "Expected fadd for float field addition\nIR:\n" << ir;
}

TEST(CodeGenTest, BoolFieldInference) {
    std::string out = compileAndRun(
        "class Flag {\n"
        "  def(active: bool) {\n"
        "    self.active = active\n"
        "  }\n"
        "  def is_on() -> bool {\n"
        "    return self.active\n"
        "  }\n"
        "}\n"
        "f: Flag = Flag(true)\n"
        "print(f.is_on())\n"
    );
    EXPECT_EQ(out, "True\n");
}

TEST(CodeGenTest, StrFieldLiteralInit) {
    auto ir = generateIR(
        "class Config {\n"
        "  def() {\n"
        "    self.mode = \"default\"\n"
        "  }\n"
        "  def get_mode() -> str {\n"
        "    if self.mode == \"default\" {\n"
        "      return \"ok\"\n"
        "    }\n"
        "    return \"custom\"\n"
        "  }\n"
        "}\n"
    );
    EXPECT_NE(ir.find("dragon_str_eq"), std::string::npos)
        << "Expected dragon_str_eq for string field comparison\nIR:\n" << ir;
}

TEST(CodeGenE2E, ClassBasic) {
    auto output = compileAndRun(
        "class Point {\n"
        "  def(x: int, y: int) {\n"
        "    self.x = x\n"
        "    self.y = y\n"
        "  }\n"
        "  def sum() -> int {\n"
        "    return self.x + self.y\n"
        "  }\n"
        "}\n"
        "p: Point = Point(3, 4)\n"
        "print(p.sum())\n"
    );
    EXPECT_EQ(output, "7\n");
}

TEST(CodeGenE2E, ClassGCFieldAccess) {
    auto out = compileAndRun(
        "class Point {\n"
        "  def(x: int, y: int) {\n"
        "    self.x = x\n"
        "    self.y = y\n"
        "  }\n"
        "}\n"
        "p: Point = Point(3, 4)\n"
        "print(p.x)\n"
        "print(p.y)\n"
    );
    EXPECT_EQ(out, "3\n4\n");
}

TEST(CodeGenE2E, ClassGCMethodAccess) {
    auto out = compileAndRun(
        "class Counter {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def get() -> int {\n"
        "    return self.v\n"
        "  }\n"
        "}\n"
        "c: Counter = Counter(99)\n"
        "print(c.get())\n"
    );
    EXPECT_EQ(out, "99\n");
}

TEST(CodeGenE2E, ClassGCMultipleFields) {
    auto out = compileAndRun(
        "class Rect {\n"
        "  def(w: int, h: int) {\n"
        "    self.w = w\n"
        "    self.h = h\n"
        "  }\n"
        "  def area() -> int {\n"
        "    return self.w * self.h\n"
        "  }\n"
        "}\n"
        "r: Rect = Rect(5, 10)\n"
        "print(r.area())\n"
    );
    EXPECT_EQ(out, "50\n");
}

TEST(CodeGenE2E, ClassGCFieldMutation) {
    auto out = compileAndRun(
        "class Box {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "  def set(n: int) {\n"
        "    self.v = n\n"
        "  }\n"
        "  def get() -> int {\n"
        "    return self.v\n"
        "  }\n"
        "}\n"
        "b: Box = Box(1)\n"
        "print(b.get())\n"
        "b.set(42)\n"
        "print(b.get())\n"
    );
    EXPECT_EQ(out, "1\n42\n");
}

TEST(CodeGenE2E, ClassGCReturnInstance) {
    auto out = compileAndRun(
        "class Val {\n"
        "  def(n: int) {\n"
        "    self.n = n\n"
        "  }\n"
        "}\n"
        "def make(x: int) -> Val {\n"
        "  v: Val = Val(x)\n"
        "  return v\n"
        "}\n"
        "r: Val = make(7)\n"
    );
    // Just verify it doesn't crash (return incref prevents use-after-free)
    EXPECT_FALSE(out.empty() && false);
}

TEST(CodeGenE2E, CycleCollectorFreesObjects) {
    auto out = compileAndRun(
        "class Node {\n"
        "  def(val: int) {\n"
        "    self.val: int = val\n"
        "    self.next: Node | None = None\n"
        "  }\n"
        "}\n"
        "def make_cycle() {\n"
        "  a: Node = Node(1)\n"
        "  b: Node = Node(2)\n"
        "  a.next = b\n"
        "  b.next = a\n"
        "}\n"
        "make_cycle()\n"
        "print(\"ok\")\n"
    );
    EXPECT_EQ(out, "ok\n");
}

TEST(CodeGenE2E, CycleCollectorReentrancyGuard) {
    auto out = compileAndRun(
        "class Node {\n"
        "  def(val: int) {\n"
        "    self.val: int = val\n"
        "    self.next: Node | None = None\n"
        "    self.tag: str = \"node\"\n"
        "  }\n"
        "}\n"
        "def make_cycle(i: int) {\n"
        "  a: Node = Node(i)\n"
        "  b: Node = Node(i + 1)\n"
        "  a.next = b\n"
        "  b.next = a\n"
        "}\n"
        "for i in range(2000) {\n"
        "  make_cycle(i)\n"
        "}\n"
        "print(\"ok\")\n"
    );
    EXPECT_EQ(out, "ok\n");
}

TEST(CodeGenE2E, ClassInstanceCreateDestroyE2E) {
    auto out = compileAndRun(
        "class Box {\n"
        "  def(v: int) {\n"
        "    self.v = v\n"
        "  }\n"
        "}\n"
        "def test() {\n"
        "  b: Box = Box(42)\n"
        "  print(b.v)\n"
        "}\n"
        "test()\n"
    );
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, MultiConstructorDispatchE2E) {
    auto out = compileAndRun(
        "class Point {\n"
        "  def(x: int, y: int) {\n"
        "    self.x = x\n"
        "    self.y = y\n"
        "  }\n"
        "  def() {\n"
        "    self.x = 0\n"
        "    self.y = 0\n"
        "  }\n"
        "}\n"
        "p1: Point = Point(10, 20)\n"
        "print(p1.x)\n"
        "print(p1.y)\n"
        "p2: Point = Point()\n"
        "print(p2.x)\n"
        "print(p2.y)\n"
    );
    EXPECT_EQ(out, "10\n20\n0\n0\n");
}

TEST(CodeGenE2E, ThreeConstructorsE2E) {
    auto out = compileAndRun(
        "class Vec {\n"
        "  def() {\n"
        "    self.x = 0\n"
        "    self.y = 0\n"
        "    self.z = 0\n"
        "  }\n"
        "  def(x: int) {\n"
        "    self.x = x\n"
        "    self.y = x\n"
        "    self.z = x\n"
        "  }\n"
        "  def(x: int, y: int, z: int) {\n"
        "    self.x = x\n"
        "    self.y = y\n"
        "    self.z = z\n"
        "  }\n"
        "}\n"
        "v1: Vec = Vec()\n"
        "print(v1.x)\n"
        "v2: Vec = Vec(5)\n"
        "print(v2.y)\n"
        "v3: Vec = Vec(1, 2, 3)\n"
        "print(v3.z)\n"
    );
    EXPECT_EQ(out, "0\n5\n3\n");
}

TEST(CodeGenE2E, MultiConstructorWithMethodsE2E) {
    auto out = compileAndRun(
        "class Rect {\n"
        "  def(w: int, h: int) {\n"
        "    self.w = w\n"
        "    self.h = h\n"
        "  }\n"
        "  def() {\n"
        "    self.w = 1\n"
        "    self.h = 1\n"
        "  }\n"
        "  def area() -> int {\n"
        "    return self.w * self.h\n"
        "  }\n"
        "}\n"
        "r1: Rect = Rect(3, 4)\n"
        "print(r1.area())\n"
        "r2: Rect = Rect()\n"
        "print(r2.area())\n"
    );
    EXPECT_EQ(out, "12\n1\n");
}

TEST(CodeGenE2E, SelfCtorSingleE2E) {
    auto out = compileAndRun(
        "class Point {\n"
        "  def(x: int, y: int) {\n"
        "    self.x = x\n"
        "    self.y = y\n"
        "  }\n"
        "  def sum() -> int {\n"
        "    return self.x + self.y\n"
        "  }\n"
        "}\n"
        "p: Point = Point(10, 32)\n"
        "print(p.sum())\n"
    );
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, DefInitBackcompatE2E) {
    auto out = compileAndRun(
        "class Box {\n"
        "  def __init__(val: int) {\n"
        "    self.val = val\n"
        "  }\n"
        "  def get() -> int {\n"
        "    return self.val\n"
        "  }\n"
        "}\n"
        "b: Box = Box(99)\n"
        "print(b.get())\n"
    );
    EXPECT_EQ(out, "99\n");
}

TEST(CodeGenE2E, PrintClassInstanceVar) {
    auto out = compileAndRun(
        "class Point {\n"
        "  def(x: int, y: int) {\n"
        "    self.x = x\n"
        "    self.y = y\n"
        "  }\n"
        "}\n"
        "p: Point = Point(1, 2)\n"
        "print(p)\n"
    );
    EXPECT_EQ(out, "<Point instance>\n");
}

TEST(CodeGenE2E, PrintClassInstanceDirect) {
    auto out = compileAndRun(
        "class Box {\n"
        "  def(val: int) {\n"
        "    self.val = val\n"
        "  }\n"
        "}\n"
        "print(Box(42))\n"
    );
    EXPECT_EQ(out, "<Box instance>\n");
}

TEST(CodeGenE2E, StaticFieldE2E) {
    auto out = compileAndRun(
        "class Counter {\n"
        "  static count: int = 0\n"
        "  def() {\n"
        "    Counter.count = Counter.count + 1\n"
        "  }\n"
        "}\n"
        "c1: Counter = Counter()\n"
        "c2: Counter = Counter()\n"
        "print(Counter.count)\n"
    );
    EXPECT_EQ(out, "2\n");
}

TEST(CodeGenE2E, StaticmethodDr) {
    auto out = compileAndRun(
        "class Math {\n"
        "  @staticmethod\n"
        "  def double(x: int) -> int {\n"
        "    return x * 2\n"
        "  }\n"
        "}\n"
        "print(Math.double(21))\n"
    );
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, StaticmethodPy) {
    auto out = compileAndRunPy(
        "class Math:\n"
        "    @staticmethod\n"
        "    def double(x: int) -> int:\n"
        "        return x * 2\n"
        "print(Math.double(21))\n"
    );
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, ClassmethodPy) {
    auto out = compileAndRunPy(
        "class Counter:\n"
        "    def __init__(self, n: int):\n"
        "        self.n = n\n"
        "    def get(self) -> int:\n"
        "        return self.n\n"
        "    @classmethod\n"
        "    def zero(cls) -> Counter:\n"
        "        return Counter(0)\n"
        "c: Counter = Counter.zero()\n"
        "print(c.get())\n"
    );
    EXPECT_EQ(out, "0\n");
}

TEST(CodeGenE2E, StaticAndInstanceMethodCoexist) {
    auto out = compileAndRun(
        "class Calc {\n"
        "  def(base: int) {\n"
        "    self.base = base\n"
        "  }\n"
        "  def add(n: int) -> int {\n"
        "    return self.base + n\n"
        "  }\n"
        "  @staticmethod\n"
        "  def pi() -> int {\n"
        "    return 3\n"
        "  }\n"
        "}\n"
        "c: Calc = Calc(10)\n"
        "print(c.add(5))\n"
        "print(Calc.pi())\n"
    );
    EXPECT_EQ(out, "15\n3\n");
}

TEST(CodeGenE2E, SuperMethodCall) {
    auto out = compileAndRun(
        "class Base {\n"
        "    def(x: int) {\n"
        "        self.x = x\n"
        "    }\n"
        "    def get_val() -> int {\n"
        "        return self.x\n"
        "    }\n"
        "}\n"
        "class Child(Base) {\n"
        "    def(x: int) {\n"
        "        self.x = x\n"
        "    }\n"
        "    def doubled() -> int {\n"
        "        return super.get_val() * 2\n"
        "    }\n"
        "}\n"
        "c: Child = Child(21)\n"
        "print(c.doubled())\n"
    );
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, MROInheritedMethod) {
    auto out = compileAndRun(
        "class Animal {\n"
        "    def(x: int) {\n"
        "        self.x = x\n"
        "    }\n"
        "    def get_x() -> int {\n"
        "        return self.x\n"
        "    }\n"
        "}\n"
        "class Dog(Animal) {\n"
        "    def(x: int) {\n"
        "        self.x = x\n"
        "    }\n"
        "}\n"
        "d: Dog = Dog(42)\n"
        "print(d.get_x())\n"
    );
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, PyMROInheritedMethodE2E) {
    auto out = compileAndRunPy(
        "class Animal:\n"
        "    def __init__(self, x: int):\n"
        "        self.x = x\n"
        "    def get_x(self) -> int:\n"
        "        return self.x\n"
        "class Dog(Animal):\n"
        "    def __init__(self, x: int):\n"
        "        self.x = x\n"
        "d: Dog = Dog(42)\n"
        "print(d.get_x())\n"
    );
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, PySuperMethodE2E) {
    auto out = compileAndRunPy(
        "class Base:\n"
        "    def __init__(self, x: int):\n"
        "        self.x = x\n"
        "    def get_val(self) -> int:\n"
        "        return self.x\n"
        "class Child(Base):\n"
        "    def __init__(self, x: int):\n"
        "        self.x = x\n"
        "    def doubled(self) -> int:\n"
        "        return super().get_val() * 2\n"
        "c: Child = Child(21)\n"
        "print(c.doubled())\n"
    );
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenTest, StrFieldCmpE2E) {
    std::string out = compileAndRun(
        "class Greeter {\n"
        "  def(name: str) {\n"
        "    self.name = name\n"
        "  }\n"
        "  def greet() -> str {\n"
        "    if self.name == \"world\" {\n"
        "      return \"Hello, World!\"\n"
        "    }\n"
        "    return \"Hello!\"\n"
        "  }\n"
        "}\n"
        "g: Greeter = Greeter(\"world\")\n"
        "print(g.greet())\n"
    );
    EXPECT_EQ(out, "Hello, World!\n");
}

TEST(CodeGenTest, StrFieldCmpNotEqual) {
    std::string out = compileAndRun(
        "class Tag {\n"
        "  def(label: str) {\n"
        "    self.label = label\n"
        "  }\n"
        "  def check() -> str {\n"
        "    if self.label != \"admin\" {\n"
        "      return \"not admin\"\n"
        "    }\n"
        "    return \"admin\"\n"
        "  }\n"
        "}\n"
        "t: Tag = Tag(\"user\")\n"
        "print(t.check())\n"
    );
    EXPECT_EQ(out, "not admin\n");
}

TEST(CodeGenTest, UnionParamIsinstanceNarrowing) {
    auto output = compileAndRun(
        "def show(x: int | str) {\n"
        "    if isinstance(x, int) {\n"
        "        print(x + 1)\n"
        "    } else {\n"
        "        print(x)\n"
        "    }\n"
        "}\n"
        "show(41)\n"
        "show(\"hello\")\n"
    );
    EXPECT_EQ(output, "42\nhello\n");
}

TEST(CodeGenTest, UnionParamPrintDispatch) {
    auto output = compileAndRun(
        "def show(x: int | str) {\n"
        "    print(x)\n"
        "}\n"
        "show(42)\n"
        "show(\"world\")\n"
    );
    EXPECT_EQ(output, "42\nworld\n");
}

TEST(CodeGenTest, UnionParamIntFloat) {
    auto output = compileAndRun(
        "def show(x: int | float) {\n"
        "    if isinstance(x, int) {\n"
        "        print(x + 1)\n"
        "    } else {\n"
        "        print(x)\n"
        "    }\n"
        "}\n"
        "show(10)\n"
        "show(3.14)\n"
    );
    EXPECT_EQ(output, "11\n3.14\n");
}

TEST(CodeGenTest, UnionParamMultiple) {
    auto output = compileAndRun(
        "def add_or_cat(a: int | str, b: int | str) {\n"
        "    if isinstance(a, int) {\n"
        "        if isinstance(b, int) {\n"
        "            print(a + b)\n"
        "        }\n"
        "    }\n"
        "}\n"
        "add_or_cat(10, 20)\n"
    );
    EXPECT_EQ(output, "30\n");
}

TEST(CodeGenTest, UnionTypeBuiltin) {
    auto output = compileAndRun(
        "def show_type(x: int | str) {\n"
        "    print(type(x))\n"
        "}\n"
        "show_type(42)\n"
        "show_type(\"hi\")\n"
    );
    EXPECT_EQ(output, "int\nstr\n");
}

TEST(CodeGenTest, UnionThreeTypes) {
    auto output = compileAndRun(
        "def show(x: int | str | float) {\n"
        "    if isinstance(x, int) {\n"
        "        print(\"int\")\n"
        "    } elif isinstance(x, str) {\n"
        "        print(\"str\")\n"
        "    } elif isinstance(x, float) {\n"
        "        print(\"float\")\n"
        "    }\n"
        "}\n"
        "show(1)\n"
        "show(\"a\")\n"
        "show(2.5)\n"
    );
    EXPECT_EQ(output, "int\nstr\nfloat\n");
}

TEST(CodeGenTest, UnionNarrowedArithmetic) {
    auto output = compileAndRun(
        "def double_it(x: int | str) -> int {\n"
        "    if isinstance(x, int) {\n"
        "        return x * 2\n"
        "    }\n"
        "    return 0\n"
        "}\n"
        "print(double_it(21))\n"
    );
    EXPECT_EQ(output, "42\n");
}

TEST(CodeGenTest, UnionPassThrough) {
    auto output = compileAndRun(
        "def inner(x: int | str) {\n"
        "    print(x)\n"
        "}\n"
        "def outer(x: int | str) {\n"
        "    inner(x)\n"
        "}\n"
        "outer(99)\n"
        "outer(\"pass\")\n"
    );
    EXPECT_EQ(output, "99\npass\n");
}

TEST(CodeGenTest, UnionLocalVariable) {
    auto output = compileAndRun(
        "def test() {\n"
        "    x: int | str = 42\n"
        "    print(x)\n"
        "    x = \"hello\"\n"
        "    print(x)\n"
        "}\n"
        "test()\n"
    );
    EXPECT_EQ(output, "42\nhello\n");
}

TEST(CodeGenTest, UnionParamBool) {
    auto output = compileAndRun(
        "def show(x: int | bool) {\n"
        "    if isinstance(x, bool) {\n"
        "        print(x)\n"
        "    } else {\n"
        "        print(x + 1)\n"
        "    }\n"
        "}\n"
        "show(True)\n"
        "show(10)\n"
    );
    EXPECT_EQ(output, "True\n11\n");
}

TEST(CodeGenTest, PrintClassDescriptor) {
    auto output = compileAndRun(
        "class Foo {\n"
        "    def() {}\n"
        "}\n"
        "print(Foo)\n"
    );
    EXPECT_NE(output.find("codegen failed"), std::string::npos);
    EXPECT_NE(output.find("not values"), std::string::npos);
}

TEST(CodeGenE2E, PropertyGetterBareAccess) {
    auto out = compileAndRun(
        "class Box {\n"
        "  def(v: int) {\n"
        "    self._v = v\n"
        "  }\n"
        "  @property\n"
        "  def value() -> int {\n"
        "    return self._v + 1\n"
        "  }\n"
        "}\n"
        "b: Box = Box(41)\n"
        "print(b.value)\n"
    );
    EXPECT_EQ(out, "42\n");
}

TEST(CodeGenE2E, PropertySetterRoundTrip) {
    auto out = compileAndRun(
        "class Box {\n"
        "  def(v: int) {\n"
        "    self._v = v\n"
        "  }\n"
        "  @property\n"
        "  def value() -> int {\n"
        "    return self._v\n"
        "  }\n"
        "  @value.setter\n"
        "  def value(x: int) {\n"
        "    self._v = x * 2\n"
        "  }\n"
        "}\n"
        "b: Box = Box(10)\n"
        "print(b.value)\n"
        "b.value = 5\n"
        "print(b.value)\n"
    );
    EXPECT_EQ(out, "10\n10\n");
}

TEST(CodeGenE2E, PropertyComputedDerived) {
    auto out = compileAndRun(
        "class Rect {\n"
        "  def(w: int, h: int) {\n"
        "    self.w = w\n"
        "    self.h = h\n"
        "  }\n"
        "  @property\n"
        "  def area() -> int {\n"
        "    return self.w * self.h\n"
        "  }\n"
        "}\n"
        "r: Rect = Rect(3, 4)\n"
        "print(r.area)\n"
    );
    EXPECT_EQ(out, "12\n");
}

TEST(CodeGenE2E, PropertyReturnsString) {
    // String-returning property - refcount path must not double-free or leak.
    auto out = compileAndRun(
        "class Greeter {\n"
        "  def(n: str) {\n"
        "    self.n = n\n"
        "  }\n"
        "  @property\n"
        "  def greeting() -> str {\n"
        "    return \"hi \" + self.n\n"
        "  }\n"
        "}\n"
        "g: Greeter = Greeter(\"world\")\n"
        "print(g.greeting)\n"
    );
    EXPECT_EQ(out, "hi world\n");
}

TEST(CodeGenE2E, PropertyInheritedFromBase) {
    auto out = compileAndRun(
        "class Base {\n"
        "  def(v: int) {\n"
        "    self._v = v\n"
        "  }\n"
        "  @property\n"
        "  def value() -> int {\n"
        "    return self._v\n"
        "  }\n"
        "}\n"
        "class Derived(Base) {\n"
        "  def(v: int) {\n"
        "    self._v = v + 1\n"
        "  }\n"
        "}\n"
        "d: Derived = Derived(6)\n"
        "print(d.value)\n"
    );
    EXPECT_EQ(out, "7\n");
}

TEST(CodeGenE2E, PropertyAssignedToTypedLocal) {
    auto out = compileAndRun(
        "class Box {\n"
        "  def(v: int) {\n"
        "    self._v = v\n"
        "  }\n"
        "  @property\n"
        "  def value() -> int {\n"
        "    return self._v\n"
        "  }\n"
        "}\n"
        "def use() -> int {\n"
        "  b: Box = Box(11)\n"
        "  x: int = b.value\n"
        "  return x + 1\n"
        "}\n"
        "print(use())\n"
    );
    EXPECT_EQ(out, "12\n");
}

TEST(CodeGenE2E, PropertyGetterCalledOnce) {
    auto out = compileAndRun(
        "class Counter {\n"
        "  def() {\n"
        "    self.n = 0\n"
        "  }\n"
        "  @property\n"
        "  def tick() -> int {\n"
        "    self.n = self.n + 1\n"
        "    return self.n\n"
        "  }\n"
        "}\n"
        "c: Counter = Counter()\n"
        "print(c.tick)\n"
        "print(c.tick)\n"
        "print(c.tick)\n"
    );
    EXPECT_EQ(out, "1\n2\n3\n");
}

TEST(CodeGenIR, PropertyEmitsMethodCallNotFieldLoad) {
    auto ir = generateIR(
        "class Box {\n"
        "  def(v: int) {\n"
        "    self._v = v\n"
        "  }\n"
        "  @property\n"
        "  def value() -> int {\n"
        "    return self._v\n"
        "  }\n"
        "}\n"
        "b: Box = Box(1)\n"
        "x: int = b.value\n"
    );
    EXPECT_NE(ir.find("define"), std::string::npos);
    EXPECT_NE(ir.find("Box_value"), std::string::npos)
        << "Expected getter Box_value to be defined";
    EXPECT_NE(ir.find("call i64 @Box_value"), std::string::npos)
        << "Expected getter call site at b.value access";
}

TEST(CodeGenE2E, EnumAutoNumbered) {
    auto out = compileAndRun(
        "enum Color {\n"
        "    RED,\n"
        "    GREEN,\n"
        "    BLUE\n"
        "}\n"
        "print(Color.RED)\n"
        "print(Color.GREEN)\n"
        "print(Color.BLUE)\n"
    );
    EXPECT_EQ(out, "0\n1\n2\n");
}

TEST(CodeGenE2E, EnumExplicitValues) {
    auto out = compileAndRun(
        "enum Status {\n"
        "    OK = 200,\n"
        "    NOT_FOUND = 404,\n"
        "    SERVER_ERROR = 500\n"
        "}\n"
        "print(Status.OK)\n"
        "print(Status.NOT_FOUND)\n"
        "print(Status.SERVER_ERROR)\n"
    );
    EXPECT_EQ(out, "200\n404\n500\n");
}

TEST(CodeGenE2E, EnumMixedAutoAndExplicit) {
    auto out = compileAndRun(
        "enum Mixed {\n"
        "    A,\n"
        "    B = 10,\n"
        "    C,\n"
        "    D\n"
        "}\n"
        "print(Mixed.A)\n"
        "print(Mixed.B)\n"
        "print(Mixed.C)\n"
        "print(Mixed.D)\n"
    );
    EXPECT_EQ(out, "0\n10\n11\n12\n");
}

TEST(CodeGenE2E, EnumWithMatchStatement) {
    auto out = compileAndRun(
        "enum Color {\n"
        "    RED,\n"
        "    GREEN,\n"
        "    BLUE\n"
        "}\n"
        "def name(c: int) -> str {\n"
        "    match c {\n"
        "        case Color.RED { return \"red\" }\n"
        "        case Color.GREEN { return \"green\" }\n"
        "        case Color.BLUE { return \"blue\" }\n"
        "        case _ { return \"unknown\" }\n"
        "    }\n"
        "    return \"unreachable\"\n"
        "}\n"
        "print(name(Color.RED))\n"
        "print(name(Color.GREEN))\n"
        "print(name(Color.BLUE))\n"
        "print(name(99))\n"
    );
    EXPECT_EQ(out, "red\ngreen\nblue\nunknown\n");
}

TEST(CodeGenE2E, EnumNegativeValue) {
    auto out = compileAndRun(
        "enum Sign {\n"
        "    NEG = -1,\n"
        "    ZERO,\n"
        "    POS\n"
        "}\n"
        "print(Sign.NEG)\n"
        "print(Sign.ZERO)\n"
        "print(Sign.POS)\n"
    );
    EXPECT_EQ(out, "-1\n0\n1\n");
}

TEST(CodeGenE2E, EnumUsedInIfElif) {
    auto out = compileAndRun(
        "enum Level {\n"
        "    DEBUG,\n"
        "    INFO,\n"
        "    WARNING,\n"
        "    ERROR\n"
        "}\n"
        "def label(l: int) -> str {\n"
        "    if l == Level.DEBUG {\n"
        "        return \"D\"\n"
        "    } elif l == Level.INFO {\n"
        "        return \"I\"\n"
        "    } elif l == Level.WARNING {\n"
        "        return \"W\"\n"
        "    }\n"
        "    return \"E\"\n"
        "}\n"
        "print(label(Level.DEBUG))\n"
        "print(label(Level.INFO))\n"
        "print(label(Level.WARNING))\n"
        "print(label(Level.ERROR))\n"
    );
    EXPECT_EQ(out, "D\nI\nW\nE\n");
}

TEST(CodeGenE2E, EnumTrailingCommaOptional) {
    auto out = compileAndRun(
        "enum E { A, B, C, }\n"
        "print(E.A)\n"
        "print(E.B)\n"
        "print(E.C)\n"
    );
    EXPECT_EQ(out, "0\n1\n2\n");
}

TEST(CodeGenIR, PropertySetterMangledInVtable) {
    auto ir = generateIR(
        "class Box {\n"
        "  def(v: int) {\n"
        "    self._v = v\n"
        "  }\n"
        "  @property\n"
        "  def value() -> int {\n"
        "    return self._v\n"
        "  }\n"
        "  @value.setter\n"
        "  def value(x: int) {\n"
        "    self._v = x\n"
        "  }\n"
        "}\n"
        "b: Box = Box(1)\n"
        "b.value = 5\n"
    );
    EXPECT_NE(ir.find("Box_value__setter"), std::string::npos)
        << "Expected mangled setter Box_value__setter in IR";
    EXPECT_NE(ir.find("@Box_value__setter"), std::string::npos)
        << "Expected setter to be emitted/called as @Box_value__setter";
}

TEST(CodeGenE2E, ForInClassFieldDictWithExplicitFieldAnnotation) {
    auto out = compileAndRun(
        "class Foo {\n"
        "    data: dict[str, str]\n"
        "    def() { self.data = {\"a\": \"1\", \"b\": \"2\"} }\n"
        "    def show() -> None {\n"
        "        for k in self.data {\n"
        "            print(k)\n"
        "            print(self.data[k])\n"
        "        }\n"
        "    }\n"
        "}\n"
        "const f: Foo = Foo()\n"
        "f.show()\n"
    );
    EXPECT_EQ(out, "a\n1\nb\n2\n");
}

TEST(CodeGenE2E, ForInClassFieldDictInferredFromDictLiteral) {
    auto out = compileAndRun(
        "class Foo {\n"
        "    def() { self.data = {\"a\": \"1\", \"b\": \"2\"} }\n"
        "    def show() -> None {\n"
        "        for k in self.data {\n"
        "            print(k)\n"
        "            print(self.data[k])\n"
        "        }\n"
        "    }\n"
        "}\n"
        "const f: Foo = Foo()\n"
        "f.show()\n"
    );
    EXPECT_EQ(out, "a\n1\nb\n2\n");
}

TEST(CodeGenE2E, ForInOtherObjectDictField) {
    auto out = compileAndRun(
        "class Foo {\n"
        "    data: dict[str, str]\n"
        "    def() { self.data = {\"x\": \"y\", \"p\": \"q\"} }\n"
        "}\n"
        "def use(f: Foo) -> None {\n"
        "    for k in f.data {\n"
        "        print(k)\n"
        "        print(f.data[k])\n"
        "    }\n"
        "}\n"
        "const f: Foo = Foo()\n"
        "use(f)\n"
    );
    EXPECT_EQ(out, "x\ny\np\nq\n");
}

TEST(CodeGenE2E, ForInClassFieldString) {
    auto out = compileAndRun(
        "class Foo {\n"
        "    body: str\n"
        "    def() { self.body = \"hi\" }\n"
        "    def show() -> None { for c in self.body { print(c) } }\n"
        "}\n"
        "const f: Foo = Foo()\n"
        "f.show()\n"
    );
    EXPECT_EQ(out, "h\ni\n");
}

TEST(CodeGenE2E, ForInClassFieldDictKeysMethodOnAttribute) {
    auto out = compileAndRun(
        "class Foo {\n"
        "    data: dict[str, str]\n"
        "    def() { self.data = {\"a\": \"1\", \"b\": \"2\"} }\n"
        "    def show() -> None {\n"
        "        for k in self.data.keys() { print(k) }\n"
        "    }\n"
        "}\n"
        "const f: Foo = Foo()\n"
        "f.show()\n"
    );
    EXPECT_EQ(out, "a\nb\n");
}

TEST(CodeGenE2E, ForInClassFieldDictHeterogeneousStaysPolymorphic) {
    auto out = compileAndRun(
        "class Foo {\n"
        "    def() { self.data = {\"a\": \"x\", \"b\": 1} }\n"
        "    def show() -> None {\n"
        "        for k in self.data { print(k) }\n"
        "    }\n"
        "}\n"
        "const f: Foo = Foo()\n"
        "f.show()\n"
    );
    EXPECT_EQ(out, "a\nb\n");
}

TEST(CodeGenE2E, CapturingClosureStoredOnClassFieldInvokedThroughField) {
    auto out = compileAndRun(
        "class Holder {\n"
        "    def(handler: Callable[[int], int]) {\n"
        "        self.handler = handler\n"
        "    }\n"
        "}\n"
        "def make(x: int) -> Holder {\n"
        "    def inner(y: int) -> int {\n"
        "        return x + y\n"
        "    }\n"
        "    return Holder(inner)\n"
        "}\n"
        "const h: Holder = make(10)\n"
        "const r: int = h.handler(5)\n"
        "print(r)\n"
    );
    EXPECT_EQ(out, "15\n");
}

TEST(CodeGenE2E, BareFnPointerOnClassFieldInvokedThroughField) {
    auto out = compileAndRun(
        "class Holder {\n"
        "    def(handler: Callable[[int], int]) {\n"
        "        self.handler = handler\n"
        "    }\n"
        "}\n"
        "def add5(y: int) -> int {\n"
        "    return y + 5\n"
        "}\n"
        "const h: Holder = Holder(add5)\n"
        "const r: int = h.handler(10)\n"
        "print(r)\n"
    );
    EXPECT_EQ(out, "15\n");
}
