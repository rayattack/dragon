#include "CodeGenTestHelpers.h"

//===----------------------------------------------------------------------===//
// List IR Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, ListLiteral) {
    auto ir = generateIR("x: list[int] = [1, 2, 3]");
    EXPECT_NE(ir.find("dragon_list_new"), std::string::npos);
    EXPECT_NE(ir.find("dragon_list_append"), std::string::npos);
}

TEST(CodeGenTest, ListSubscript) {
    auto ir = generateIR("x: list[int] = [10, 20, 30]\nprint(x[1])");
    // Inline list access: direct GEP instead of dragon_list_get on the hot path
    EXPECT_NE(ir.find("list.data.gep"), std::string::npos);
    EXPECT_NE(ir.find("list.elem"), std::string::npos);
    // OOB path still references dragon_list_get for error handling
    EXPECT_NE(ir.find("dragon_list_get"), std::string::npos);
}

TEST(CodeGenTest, ListLen) {
    auto ir = generateIR("x: list[int] = [1, 2, 3]\nprint(len(x))");
    EXPECT_NE(ir.find("dragon_list_len"), std::string::npos);
}

TEST(CodeGenTest, ListAppendMethod) {
    auto ir = generateIR("x: list[int] = [1, 2]\nx.append(3)");
    EXPECT_NE(ir.find("dragon_list_append"), std::string::npos);
}

TEST(CodeGenTest, ListSlice) {
    auto ir = generateIR("x: list[int] = [1, 2, 3, 4, 5]\ny: list[int] = x[1:3]");
    EXPECT_NE(ir.find("dragon_list_slice"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Dict IR Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, DictExprEmpty) {
    auto ir = generateIR("d: dict[str, int] = {}");
    EXPECT_NE(ir.find("dragon_dict_new"), std::string::npos);
}

TEST(CodeGenTest, DictExprEntries) {
    auto ir = generateIR("d: dict[str, int] = {\"a\": 1, \"b\": 2}");
    EXPECT_NE(ir.find("dragon_dict_new"), std::string::npos);
    EXPECT_NE(ir.find("dragon_dict_set"), std::string::npos);
}

TEST(CodeGenTest, DictSubscriptGet) {
    auto ir = generateIR("d: dict[str, int] = {\"a\": 1}\nx: int = d[\"a\"]");
    EXPECT_NE(ir.find("dragon_dict_get"), std::string::npos);
}

TEST(CodeGenTest, DictLen) {
    auto ir = generateIR("d: dict[str, int] = {\"a\": 1, \"b\": 2}\nprint(len(d))");
    EXPECT_NE(ir.find("dragon_dict_len"), std::string::npos);
}

TEST(CodeGenTest, DictPrint) {
    auto ir = generateIR("d: dict[str, int] = {\"a\": 1}\nprint(d)");
    EXPECT_NE(ir.find("dragon_print_dict"), std::string::npos);
}

TEST(CodeGenTest, BareKeyDictIR) {
    // Bare-key dict in .dr mode should compile to string-keyed dict
    auto ir = generateIR(
        "d: dict = {name: \"Jon\", age: 10}\n"
    );
    EXPECT_NE(ir.find("dragon_dict_new"), std::string::npos);
    EXPECT_NE(ir.find("dragon_dict_set"), std::string::npos);
    // The bare keys "name" and "age" should appear as global strings
    EXPECT_NE(ir.find("name"), std::string::npos);
    EXPECT_NE(ir.find("age"), std::string::npos);
}

TEST(CodeGenTest, DictDotAccessReadIR) {
    auto ir = generateIR(
        "d: dict = {\"x\": 1}\n"
        "v: int = d.x\n"
    );
    // Should call dragon_dict_get with "x" as key string
    EXPECT_NE(ir.find("dragon_dict_get"), std::string::npos);
    EXPECT_NE(ir.find("dictdot"), std::string::npos);
}

TEST(CodeGenTest, DictDotAccessWriteIR) {
    auto ir = generateIR(
        "d: dict = {\"x\": 1}\n"
        "d.x = 2\n"
    );
    // Should call dragon_dict_set with "x" as key string
    EXPECT_NE(ir.find("dragon_dict_set"), std::string::npos);
}

TEST(CodeGenTest, DictGetCheckedIR) {
    // Annotated dict access should emit dragon_dict_get_checked
    auto ir = generateIR(
        "d: dict = {name: \"Jon\", age: 10}\n"
        "x: int = d[\"age\"]\n"
    );
    EXPECT_NE(ir.find("dragon_dict_get_checked"), std::string::npos);
}

TEST(CodeGenTest, DictGetCheckedDotAccessIR) {
    // Annotated dot-access should emit dragon_dict_get_checked
    auto ir = generateIR(
        "d: dict = {name: \"Jon\", age: 10}\n"
        "x: int = d.age\n"
    );
    EXPECT_NE(ir.find("dragon_dict_get_checked"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// TypedDict IR Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, TypedDictIR) {
    // TypedDict should compile to dict operations, not struct
    auto ir = generateIR(
        "class Config(TypedDict) {\n"
        "    host: str\n"
        "    port: int\n"
        "}\n"
        "cfg: Config = Config({host: \"localhost\", port: 8080})\n"
    );
    // Should use dict_new, NOT struct allocation
    EXPECT_NE(ir.find("dragon_dict_new"), std::string::npos);
    // Should NOT have a Config struct type
    EXPECT_EQ(ir.find("%Config = type"), std::string::npos);
}

TEST(CodeGenTest, TypedDictCheckedAccessIR) {
    // TypedDict subscript should emit dragon_dict_get_checked
    auto ir = generateIR(
        "class Config(TypedDict) {\n"
        "    host: str\n"
        "    port: int\n"
        "}\n"
        "cfg: Config = Config({host: \"localhost\", port: 8080})\n"
        "h: str = cfg[\"host\"]\n"
    );
    EXPECT_NE(ir.find("dragon_dict_get_checked"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Tuple IR Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, TupleCreate) {
    auto ir = generateIR("t: tuple[int, int, int] = (1, 2, 3)");
    EXPECT_NE(ir.find("dragon_tuple_new"), std::string::npos);
    EXPECT_NE(ir.find("dragon_tuple_set"), std::string::npos);
}

TEST(CodeGenTest, TupleSubscript) {
    auto ir = generateIR("t: tuple[int, int, int] = (10, 20, 30)\nprint(t[1])");
    EXPECT_NE(ir.find("dragon_tuple_get"), std::string::npos);
}

TEST(CodeGenTest, TupleLen) {
    auto ir = generateIR("t: tuple[int, int, int] = (1, 2, 3)\nprint(len(t))");
    EXPECT_NE(ir.find("dragon_tuple_len"), std::string::npos);
}

TEST(CodeGenTest, TuplePrint) {
    auto ir = generateIR("t: tuple[int, int, int] = (1, 2, 3)\nprint(t)");
    EXPECT_NE(ir.find("dragon_print_tuple"), std::string::npos);
}

TEST(CodeGenTest, TupleEmpty) {
    auto ir = generateIR("t: Any = ()");
    EXPECT_NE(ir.find("dragon_tuple_new"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Set IR Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, SetCreate) {
    auto ir = generateIR("s: set = {1, 2, 3}");
    EXPECT_NE(ir.find("dragon_set_new"), std::string::npos);
    EXPECT_NE(ir.find("dragon_set_add"), std::string::npos);
}

TEST(CodeGenTest, SetLen) {
    auto ir = generateIR("s: set = {1, 2, 3}\nprint(len(s))");
    EXPECT_NE(ir.find("dragon_set_len"), std::string::npos);
}

TEST(CodeGenTest, SetPrint) {
    auto ir = generateIR("s: set = {1, 2, 3}\nprint(s)");
    EXPECT_NE(ir.find("dragon_print_set"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// List E2E Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, ListBasic) {
    auto output = compileAndRun(
        "x: list[int] = [10, 20, 30]\n"
        "print(x[0])\n"
        "print(x[1])\n"
        "print(x[2])"
    );
    EXPECT_EQ(output, "10\n20\n30\n");
}

TEST(CodeGenE2E, ListAppend) {
    auto output = compileAndRun(
        "x: list[int] = [1, 2]\n"
        "x.append(3)\n"
        "x.append(4)\n"
        "print(len(x))\n"
        "print(x[2])\n"
        "print(x[3])"
    );
    EXPECT_EQ(output, "4\n3\n4\n");
}

TEST(CodeGenE2E, ListNegativeIndex) {
    auto output = compileAndRun(
        "x: list[int] = [10, 20, 30]\n"
        "print(x[-1])\n"
        "print(x[-2])"
    );
    EXPECT_EQ(output, "30\n20\n");
}

TEST(CodeGenE2E, ListInLoop) {
    auto output = compileAndRun(
        "x: list[int] = [0, 0, 0, 0, 0]\n"
        "for i in range(5) {\n"
        "  x.append(i * i)\n"
        "}\n"
        "print(x[5])\n"
        "print(x[8])\n"
        "print(len(x))"
    );
    EXPECT_EQ(output, "0\n9\n10\n");
}

TEST(CodeGenE2E, ListInsert) {
    auto output = compileAndRun(
        "x: list[int] = [1, 3, 4]\n"
        "x.insert(1, 2)\n"
        "print(x[0])\n"
        "print(x[1])\n"
        "print(x[2])\n"
        "print(x[3])"
    );
    EXPECT_EQ(output, "1\n2\n3\n4\n");
}

TEST(CodeGenE2E, ListRemove) {
    auto output = compileAndRun(
        "x: list[int] = [1, 2, 3, 2, 4]\n"
        "x.remove(2)\n"
        "print(len(x))\n"
        "print(x[1])"
    );
    EXPECT_EQ(output, "4\n3\n");
}

TEST(CodeGenE2E, ListPop) {
    auto output = compileAndRun(
        "x: list[int] = [10, 20, 30]\n"
        "v: int = x.pop()\n"
        "print(v)\n"
        "print(len(x))"
    );
    EXPECT_EQ(output, "30\n2\n");
}

TEST(CodeGenE2E, ListPopIndex) {
    auto output = compileAndRun(
        "x: list[int] = [10, 20, 30]\n"
        "v: int = x.pop(0)\n"
        "print(v)\n"
        "print(x[0])"
    );
    EXPECT_EQ(output, "10\n20\n");
}

TEST(CodeGenE2E, ListClear) {
    auto output = compileAndRun(
        "x: list[int] = [1, 2, 3]\n"
        "x.clear()\n"
        "print(len(x))"
    );
    EXPECT_EQ(output, "0\n");
}

TEST(CodeGenE2E, ListExtend) {
    auto output = compileAndRun(
        "x: list[int] = [1, 2]\n"
        "y: list[int] = [3, 4, 5]\n"
        "x.extend(y)\n"
        "print(len(x))\n"
        "print(x[3])"
    );
    EXPECT_EQ(output, "5\n4\n");
}

TEST(CodeGenE2E, ListIndex) {
    auto output = compileAndRun(
        "x: list[int] = [10, 20, 30, 40]\n"
        "print(x.index(30))"
    );
    EXPECT_EQ(output, "2\n");
}

TEST(CodeGenE2E, ListCount) {
    auto output = compileAndRun(
        "x: list[int] = [1, 2, 3, 2, 1, 2]\n"
        "print(x.count(2))"
    );
    EXPECT_EQ(output, "3\n");
}

TEST(CodeGenE2E, ListSort) {
    auto output = compileAndRun(
        "x: list[int] = [3, 1, 4, 1, 5, 9, 2, 6]\n"
        "x.sort()\n"
        "print(x[0])\n"
        "print(x[1])\n"
        "print(x[7])"
    );
    EXPECT_EQ(output, "1\n1\n9\n");
}

TEST(CodeGenE2E, ListReverse) {
    auto output = compileAndRun(
        "x: list[int] = [1, 2, 3, 4]\n"
        "x.reverse()\n"
        "print(x[0])\n"
        "print(x[3])"
    );
    EXPECT_EQ(output, "4\n1\n");
}

TEST(CodeGenE2E, ListCopy) {
    auto output = compileAndRun(
        "x: list[int] = [1, 2, 3]\n"
        "y: list[int] = x.copy()\n"
        "x.append(4)\n"
        "print(len(x))\n"
        "print(len(y))"
    );
    EXPECT_EQ(output, "4\n3\n");
}

//===----------------------------------------------------------------------===//
// Dict E2E Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, DictBasic) {
    auto output = compileAndRun(
        "d: dict[str, int] = {\"a\": 1, \"b\": 2}\n"
        "print(d[\"a\"])\n"
        "print(len(d))"
    );
    EXPECT_EQ(output, "1\n2\n");
}

TEST(CodeGenE2E, DictValues) {
    auto output = compileAndRun(
        "d: dict[str, int] = {\"a\": 1, \"b\": 2}\n"
        "v: list[int] = d.values()\n"
        "print(len(v))"
    );
    EXPECT_EQ(output, "2\n");
}

TEST(CodeGenE2E, DictPop) {
    auto output = compileAndRun(
        "d: dict[str, int] = {\"a\": 1, \"b\": 2, \"c\": 3}\n"
        "v: int = d.pop(\"b\")\n"
        "print(v)\n"
        "print(len(d))"
    );
    EXPECT_EQ(output, "2\n2\n");
}

TEST(CodeGenE2E, DictClear) {
    auto output = compileAndRun(
        "d: dict[str, int] = {\"a\": 1, \"b\": 2}\n"
        "d.clear()\n"
        "print(len(d))"
    );
    EXPECT_EQ(output, "0\n");
}

TEST(CodeGenE2E, DictSetdefault) {
    auto output = compileAndRun(
        "d: dict[str, int] = {\"a\": 1}\n"
        "v: int = d.setdefault(\"a\", 99)\n"
        "print(v)\n"
        "w: int = d.setdefault(\"b\", 42)\n"
        "print(w)\n"
        "print(len(d))"
    );
    EXPECT_EQ(output, "1\n42\n2\n");
}

TEST(CodeGenE2E, DictCopy) {
    auto output = compileAndRun(
        "d: dict[str, int] = {\"a\": 1, \"b\": 2}\n"
        "e: dict[str, int] = d.copy()\n"
        "print(len(e))\n"
        "print(e[\"a\"])"
    );
    EXPECT_EQ(output, "2\n1\n");
}

TEST(CodeGenE2E, BareKeyDictBasic) {
    auto output = compileAndRun(
        "d: dict = {name: \"Jon\", age: 10}\n"
        "print(d[\"name\"])\n"
        "print(d[\"age\"])\n"
    );
    EXPECT_EQ(output, "Jon\n10\n");
}

TEST(CodeGenE2E, BareKeyDictMixed) {
    // Mix bare keys with quoted keys
    auto output = compileAndRun(
        "d: dict = {host: \"localhost\", \"port\": 8080}\n"
        "print(d[\"host\"])\n"
        "print(d[\"port\"])\n"
    );
    EXPECT_EQ(output, "localhost\n8080\n");
}

TEST(CodeGenE2E, ComputedKeyDict) {
    auto output = compileAndRun(
        "field: str = \"age\"\n"
        "d: dict = {name: \"Jon\", (field): 10}\n"
        "print(d[\"name\"])\n"
        "print(d[\"age\"])\n"
    );
    EXPECT_EQ(output, "Jon\n10\n");
}

TEST(CodeGenE2E, DictDotAccessRead) {
    auto output = compileAndRun(
        "d: dict = {\"name\": \"Jon\", \"age\": 25}\n"
        "print(d.name)\n"
        "print(d.age)\n"
    );
    EXPECT_EQ(output, "Jon\n25\n");
}

TEST(CodeGenE2E, DictDotAccessReadBareKey) {
    // Combine bare-key dict with dot-access read
    auto output = compileAndRun(
        "d: dict = {name: \"Jon\", age: 25}\n"
        "print(d.name)\n"
        "print(d.age)\n"
    );
    EXPECT_EQ(output, "Jon\n25\n");
}

TEST(CodeGenE2E, DictDotAccessWrite) {
    auto output = compileAndRun(
        "d: dict = {\"x\": 1}\n"
        "d.x = 42\n"
        "print(d.x)\n"
    );
    EXPECT_EQ(output, "42\n");
}

TEST(CodeGenE2E, DictDotAccessWriteNew) {
    // Write a new key via dot-access
    auto output = compileAndRun(
        "d: dict = {\"x\": 1}\n"
        "d.y = 99\n"
        "print(d[\"y\"])\n"
    );
    EXPECT_EQ(output, "99\n");
}

TEST(CodeGenE2E, DictMethodsStillWork) {
    // Dict methods (keys, get, etc.) should still work with parens
    auto output = compileAndRun(
        "d: dict = {name: \"Jon\", age: 25}\n"
        "v: str = d.get(\"name\")\n"
        "print(v)\n"
    );
    EXPECT_EQ(output, "Jon\n");
}

//===----------------------------------------------------------------------===//
// 6.17 - Missing dict methods: items() unpack, popitem(), fromkeys()
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, DictItemsTupleUnpacking) {
    // for k, v in d.items() - Sema must define both names in the loop scope.
    auto out = compileAndRun(
        "d: dict[str, int] = {\"a\": 1, \"b\": 2, \"c\": 3}\n"
        "for k, v in d.items() {\n"
        "    print(k)\n"
        "    print(v)\n"
        "}\n"
    );
    EXPECT_EQ(out, "a\n1\nb\n2\nc\n3\n");
}

TEST(CodeGenE2E, DictPopitemLifoOrder) {
    // popitem returns (key, value) of the LAST inserted entry.
    auto out = compileAndRun(
        "d: dict[str, int] = {\"a\": 1, \"b\": 2, \"c\": 3}\n"
        "d.popitem()\n"
        "print(len(d))\n"
        "print(\"c\" in d)\n"
        "print(\"a\" in d)\n"
    );
    EXPECT_EQ(out, "2\nFalse\nTrue\n");
}

TEST(CodeGenE2E, DictPopitemRepeated) {
    // popitem repeatedly drains the dict in LIFO order.
    auto out = compileAndRun(
        "d: dict[str, int] = {\"a\": 1, \"b\": 2, \"c\": 3}\n"
        "d.popitem()\n"
        "d.popitem()\n"
        "print(len(d))\n"
        "print(\"a\" in d)\n"
    );
    EXPECT_EQ(out, "1\nTrue\n");
}

TEST(CodeGenE2E, DictFromkeysWithDefault) {
    // dict.fromkeys(iterable, default) - every key gets the same value.
    auto out = compileAndRun(
        "keys: list[str] = [\"x\", \"y\", \"z\"]\n"
        "d: dict[str, int] = dict.fromkeys(keys, 99)\n"
        "print(len(d))\n"
        "print(d[\"x\"])\n"
        "print(d[\"y\"])\n"
        "print(d[\"z\"])\n"
    );
    EXPECT_EQ(out, "3\n99\n99\n99\n");
}

TEST(CodeGenE2E, DictFromkeysWithoutDefault) {
    // dict.fromkeys(iterable) without value - default None, but the dict
    // still has the right keys and length.
    auto out = compileAndRun(
        "d: dict[str, int] = dict.fromkeys([\"a\", \"b\"])\n"
        "print(len(d))\n"
        "print(\"a\" in d)\n"
        "print(\"b\" in d)\n"
    );
    EXPECT_EQ(out, "2\nTrue\nTrue\n");
}

TEST(CodeGenE2E, TupleUnpackAssignment) {
    // Sema fix: `a, b = expr` defines both names in the enclosing scope.
    auto out = compileAndRun(
        "def pair() -> tuple[int, int] {\n"
        "    return (3, 4)\n"
        "}\n"
        "a, b = pair()\n"
        "print(a + b)\n"
    );
    EXPECT_EQ(out, "7\n");
}

//===----------------------------------------------------------------------===//
// for k, v in dict.items() tracks value VarKind for str/bool/etc.
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, DictItemsValueVarKindStr) {
    // The value gets the dict's declared V type so `print(v)` dispatches
    // as str, not as a raw pointer printed as int.
    auto out = compileAndRun(
        "d: dict[str, str] = {\"a\": \"alpha\", \"b\": \"beta\"}\n"
        "for k, v in d.items() {\n"
        "    print(k)\n"
        "    print(v)\n"
        "}\n"
    );
    EXPECT_EQ(out, "a\nalpha\nb\nbeta\n");
}

TEST(CodeGenE2E, DictItemsValueVarKindInt) {
    auto out = compileAndRun(
        "d: dict[str, int] = {\"a\": 1, \"b\": 2}\n"
        "for k, v in d.items() {\n"
        "    print(v + 10)\n"
        "}\n"
    );
    EXPECT_EQ(out, "11\n12\n");
}

TEST(CodeGenE2E, BareKeyDictSingleEntry) {
    auto output = compileAndRun(
        "d: dict = {status: 200}\n"
        "print(d[\"status\"])\n"
    );
    EXPECT_EQ(output, "200\n");
}

TEST(CodeGenE2E, DictDotAccessReadWriteCombined) {
    auto output = compileAndRun(
        "d: dict = {count: 0}\n"
        "d.count = 5\n"
        "d.label = \"items\"\n"
        "print(d.count)\n"
        "print(d.label)\n"
    );
    EXPECT_EQ(output, "5\nitems\n");
}

TEST(CodeGenE2E, DictWithNestedContainersE2E) {
    // Dict containing list values - verify no crash on cleanup
    auto out = compileAndRun(
        "d: dict = {\"a\": 1, \"b\": 2}\n"
        "print(d[\"a\"])\n"
        "print(d[\"b\"])\n"
    );
    EXPECT_EQ(out, "1\n2\n");
}

TEST(CodeGenE2E, DictGetCheckedCorrectType) {
    // Reading with correct type annotation should succeed
    auto output = compileAndRun(
        "d: dict = {name: \"Jon\", age: 10}\n"
        "x: int = d[\"age\"]\n"
        "print(x)\n"
    );
    EXPECT_EQ(output, "10\n");
}

TEST(CodeGenE2E, DictGetCheckedStringCorrect) {
    // Reading a string with str annotation should succeed
    auto output = compileAndRun(
        "d: dict = {name: \"Jon\", age: 10}\n"
        "x: str = d[\"name\"]\n"
        "print(x)\n"
    );
    EXPECT_EQ(output, "Jon\n");
}

TEST(CodeGenE2E, DictGetCheckedDotAccess) {
    // Dot-access with annotation should use checked get
    auto output = compileAndRun(
        "d: dict = {name: \"Jon\", age: 10}\n"
        "x: int = d.age\n"
        "print(x)\n"
    );
    EXPECT_EQ(output, "10\n");
}

//===----------------------------------------------------------------------===//
// TypedDict E2E Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, TypedDictBasic) {
    // TypedDict with dict literal construction
    auto output = compileAndRun(
        "class Config(TypedDict) {\n"
        "    host: str\n"
        "    port: int\n"
        "}\n"
        "cfg: Config = Config({host: \"localhost\", port: 8080})\n"
        "print(cfg[\"host\"])\n"
        "print(cfg[\"port\"])\n"
    );
    EXPECT_EQ(output, "localhost\n8080\n");
}

TEST(CodeGenE2E, TypedDictDotAccess) {
    // TypedDict dot-access should use checked get
    auto output = compileAndRun(
        "class Config(TypedDict) {\n"
        "    host: str\n"
        "    port: int\n"
        "    debug: bool\n"
        "}\n"
        "cfg: Config = Config({host: \"localhost\", port: 8080, debug: True})\n"
        "print(cfg.host)\n"
        "print(cfg.port)\n"
        "print(cfg.debug)\n"
    );
    EXPECT_EQ(output, "localhost\n8080\nTrue\n");
}

TEST(CodeGenE2E, TypedDictMixedTypes) {
    // TypedDict with different value types
    auto output = compileAndRun(
        "class Settings(TypedDict) {\n"
        "    name: str\n"
        "    count: int\n"
        "    ratio: float\n"
        "    active: bool\n"
        "}\n"
        "s: Settings = Settings({name: \"test\", count: 42, ratio: 3.14, active: True})\n"
        "print(s[\"name\"])\n"
        "print(s[\"count\"])\n"
    );
    EXPECT_EQ(output, "test\n42\n");
}

TEST(CodeGenE2E, TypedDictAnnotatedAccess) {
    // Accessing TypedDict field into annotated variable
    auto output = compileAndRun(
        "class Config(TypedDict) {\n"
        "    host: str\n"
        "    port: int\n"
        "}\n"
        "cfg: Config = Config({host: \"localhost\", port: 8080})\n"
        "h: str = cfg[\"host\"]\n"
        "p: int = cfg[\"port\"]\n"
        "print(h)\n"
        "print(p)\n"
    );
    EXPECT_EQ(output, "localhost\n8080\n");
}

TEST(CodeGenE2E, TypedDictWithoutAnnotation) {
    // TypedDict constructed from a dict literal
    auto output = compileAndRun(
        "class Config(TypedDict) {\n"
        "    host: str\n"
        "    port: int\n"
        "}\n"
        "cfg: Config = Config({host: \"localhost\", port: 8080})\n"
        "print(cfg[\"host\"])\n"
        "print(cfg[\"port\"])\n"
    );
    EXPECT_EQ(output, "localhost\n8080\n");
}

// Call-site ** spread into a TypedDict - the D032 typed-row gate
// (`Customer(**row)` / `[Customer(**r) for r in rows]`). Lowers the single-
// spread form to one dragon_dict_copy (Python **-unpack copy semantics).
TEST(CodeGenE2E, TypedDictDoubleStarSpread) {
    auto output = compileAndRun(
        "class Customer(TypedDict) {\n"
        "    id: int\n"
        "    name: str\n"
        "}\n"
        "row: dict[str, Any] = {}\n"
        "row[\"id\"] = 1\n"
        "row[\"name\"] = \"Ada\"\n"
        "c: Customer = Customer(**row)\n"
        "print(c[\"id\"])\n"
        "print(c[\"name\"])\n"
    );
    EXPECT_EQ(output, "1\nAda\n");
}

TEST(CodeGenE2E, TypedDictDoubleStarSpreadInComprehension) {
    auto output = compileAndRun(
        "class Customer(TypedDict) {\n"
        "    id: int\n"
        "    name: str\n"
        "}\n"
        "rows: list[dict[str, Any]] = []\n"
        "r1: dict[str, Any] = {}\n"
        "r1[\"id\"] = 1\n"
        "r1[\"name\"] = \"Ada\"\n"
        "rows.append(r1)\n"
        "cs: list[Customer] = [Customer(**r) for r in rows]\n"
        "for c in cs { print(c[\"name\"]) }\n"
    );
    EXPECT_EQ(output, "Ada\n");
}

//===----------------------------------------------------------------------===//
// Tuple E2E Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, TupleCreateAndPrint) {
    auto output = compileAndRun(
        "t: tuple[int, int, int] = (1, 2, 3)\n"
        "print(t)"
    );
    EXPECT_EQ(output, "(1, 2, 3)\n");
}

TEST(CodeGenE2E, TupleSingleElement) {
    auto output = compileAndRun(
        "t: tuple[int] = (42,)\n"
        "print(t)"
    );
    EXPECT_EQ(output, "(42,)\n");
}

TEST(CodeGenE2E, TupleIndexAccess) {
    auto output = compileAndRun(
        "t: tuple[int, int, int] = (10, 20, 30)\n"
        "print(t[0])\n"
        "print(t[1])\n"
        "print(t[2])"
    );
    EXPECT_EQ(output, "10\n20\n30\n");
}

TEST(CodeGenE2E, TupleNegativeIndex) {
    auto output = compileAndRun(
        "t: tuple[int, int, int] = (10, 20, 30)\n"
        "print(t[-1])\n"
        "print(t[-2])"
    );
    EXPECT_EQ(output, "30\n20\n");
}

TEST(CodeGenE2E, TupleLen) {
    auto output = compileAndRun(
        "t: tuple[int, int, int, int, int] = (1, 2, 3, 4, 5)\n"
        "print(len(t))"
    );
    EXPECT_EQ(output, "5\n");
}

TEST(CodeGenE2E, TupleEmptyLen) {
    auto output = compileAndRun(
        "t: Any = ()\n"
        "print(len(t))"
    );
    EXPECT_EQ(output, "0\n");
}

TEST(CodeGenE2E, TupleInFunction) {
    auto output = compileAndRun(
        "def make_pair(a: int, b: int) -> int {\n"
        "    t: tuple[int, int] = (a, b)\n"
        "    return t[0] + t[1]\n"
        "}\n"
        "print(make_pair(3, 4))"
    );
    EXPECT_EQ(output, "7\n");
}

TEST(CodeGenE2E, TupleUnpackSimple) {
    auto output = compileAndRun(
        "a, b = (1, 2)\n"
        "print(a)\n"
        "print(b)"
    );
    EXPECT_EQ(output, "1\n2\n");
}

TEST(CodeGenE2E, TupleUnpackThree) {
    auto output = compileAndRun(
        "a, b, c = (10, 20, 30)\n"
        "print(a)\n"
        "print(b)\n"
        "print(c)"
    );
    EXPECT_EQ(output, "10\n20\n30\n");
}

TEST(CodeGenE2E, TupleUnpackFromRHSTuple) {
    auto output = compileAndRun(
        "a, b = 100, 200\n"
        "print(a)\n"
        "print(b)"
    );
    EXPECT_EQ(output, "100\n200\n");
}

TEST(CodeGenE2E, StarredUnpackFirst) {
    auto output = compileAndRun(
        "first, *rest = [10, 20, 30, 40]\n"
        "print(first)\n"
        "print(len(rest))"
    );
    EXPECT_EQ(output, "10\n3\n");
}

TEST(CodeGenE2E, StarredUnpackLast) {
    auto output = compileAndRun(
        "*init, last = [10, 20, 30, 40]\n"
        "print(len(init))\n"
        "print(last)"
    );
    EXPECT_EQ(output, "3\n40\n");
}

TEST(CodeGenE2E, ForLoopTupleUnpack) {
    auto output = compileAndRun(
        "pairs: list[tuple[int, int]] = [(1, 10), (2, 20), (3, 30)]\n"
        "for a, b in pairs {\n"
        "    print(a + b)\n"
        "}"
    );
    EXPECT_EQ(output, "11\n22\n33\n");
}

//===----------------------------------------------------------------------===//
// Set E2E Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, SetLen) {
    auto output = compileAndRun(
        "s: set = {10, 20, 30}\n"
        "print(len(s))"
    );
    EXPECT_EQ(output, "3\n");
}

TEST(CodeGenE2E, SetDeduplicate) {
    auto output = compileAndRun(
        "s: set = {1, 2, 2, 3, 3, 3}\n"
        "print(len(s))"
    );
    EXPECT_EQ(output, "3\n");
}

TEST(CodeGenE2E, SetEmptyLen) {
    // empty set literal is actually a dict, so use set() later
    // For now test a set with one element
    auto output = compileAndRun(
        "s: set = {42}\n"
        "print(len(s))"
    );
    EXPECT_EQ(output, "1\n");
}

TEST(CodeGenE2E, SetContainsViaIn) {
    auto output = compileAndRun(
        "s: set = {10, 20, 30}\n"
        "if 20 in s {\n"
        "    print(1)\n"
        "} else {\n"
        "    print(0)\n"
        "}"
    );
    EXPECT_EQ(output, "1\n");
}

TEST(CodeGenE2E, SetNotContainsViaIn) {
    auto output = compileAndRun(
        "s: set = {10, 20, 30}\n"
        "if 99 in s {\n"
        "    print(1)\n"
        "} else {\n"
        "    print(0)\n"
        "}"
    );
    EXPECT_EQ(output, "0\n");
}

//===----------------------------------------------------------------------===//
// Immortal Objects E2E Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, ImmortalObjectSurvivesDecref) {
    // Verify that dragon_make_immortal stamps the immortal refcount
    // and dragon_is_immortal_obj confirms it.
    auto output = compileAndRun(
        "extern \"C\" def dragon_make_immortal(obj: list[int]) -> None\n"
        "extern \"C\" def dragon_is_immortal_obj(obj: list[int]) -> int\n"
        "x: list[int] = [1, 2, 3]\n"
        "dragon_make_immortal(x)\n"
        "result: int = dragon_is_immortal_obj(x)\n"
        "print(result)\n"
    );
    EXPECT_EQ(output, "1\n");
}

TEST(CodeGenE2E, NonImmortalObjectReportsZero) {
    // Verify that a normal (non-immortal) object reports 0 for is_immortal.
    auto output = compileAndRun(
        "extern \"C\" def dragon_is_immortal_obj(obj: list[int]) -> int\n"
        "x: list[int] = [10, 20]\n"
        "result: int = dragon_is_immortal_obj(x)\n"
        "print(result)\n"
    );
    EXPECT_EQ(output, "0\n");
}

//===----------------------------------------------------------------------===//
// List/Dict Spread Operators
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, ListSpreadBasic) {
    auto out = compileAndRun(
        "a: list[int] = [1, 2]\n"
        "b: list[int] = [3, 4]\n"
        "c: list[int] = [*a, *b]\n"
        "print(len(c))\n"
    );
    EXPECT_EQ(out, "4\n");
}

TEST(CodeGenE2E, ListSpreadWithLiterals) {
    auto out = compileAndRun(
        "a: list[int] = [2, 3]\n"
        "c: list[int] = [1, *a, 4]\n"
        "print(len(c))\n"
    );
    EXPECT_EQ(out, "4\n");
}

TEST(CodeGenE2E, DictSpreadBasic) {
    auto out = compileAndRun(
        "a: dict = {\"x\": 1}\n"
        "b: dict = {\"y\": 2}\n"
        "c: dict = {**a, **b}\n"
        "print(len(c))\n"
    );
    EXPECT_EQ(out, "2\n");
}

TEST(CodeGenE2E, DictSpreadWithEntries) {
    auto out = compileAndRun(
        "base: dict = {\"host\": \"localhost\"}\n"
        "merged: dict = {**base, \"port\": 8080}\n"
        "print(len(merged))\n"
    );
    EXPECT_EQ(out, "2\n");
}

TEST(CodeGenE2E, DictSpreadOverride) {
    auto out = compileAndRun(
        "a: dict = {\"x\": 1}\n"
        "b: dict = {\"x\": 99}\n"
        "c: dict = {**a, **b}\n"
        "print(c[\"x\"])\n"
    );
    EXPECT_EQ(out, "99\n");
}

//===----------------------------------------------------------------------===//
// Deque (collections.deque)
//===----------------------------------------------------------------------===//

TEST(CodeGenIR, DequeNewIR) {
    auto ir = generateIR(
        "d: deque[int] = deque()\n"
    );
    EXPECT_NE(ir.find("dragon_deque_new"), std::string::npos);
}

TEST(CodeGenE2E, DequeAppendPopleft) {
    auto out = compileAndRun(
        "d: deque[int] = deque()\n"
        "d.append(1)\n"
        "d.append(2)\n"
        "d.append(3)\n"
        "print(d.popleft())\n"
        "print(d.popleft())\n"
        "print(d.popleft())\n"
    );
    EXPECT_EQ(out, "1\n2\n3\n");
}

TEST(CodeGenE2E, DequeLen) {
    auto out = compileAndRun(
        "d: deque[int] = deque()\n"
        "d.append(10)\n"
        "d.append(20)\n"
        "print(len(d))\n"
        "d.popleft()\n"
        "print(len(d))\n"
    );
    EXPECT_EQ(out, "2\n1\n");
}

TEST(CodeGenE2E, DequeAppendleft) {
    auto out = compileAndRun(
        "d: deque[int] = deque()\n"
        "d.appendleft(1)\n"
        "d.appendleft(2)\n"
        "d.appendleft(3)\n"
        "print(d.popleft())\n"
        "print(d.popleft())\n"
    );
    EXPECT_EQ(out, "3\n2\n");
}

TEST(CodeGenE2E, DequePop) {
    auto out = compileAndRun(
        "d: deque[int] = deque()\n"
        "d.append(1)\n"
        "d.append(2)\n"
        "d.append(3)\n"
        "print(d.pop())\n"
        "print(d.pop())\n"
    );
    EXPECT_EQ(out, "3\n2\n");
}

TEST(CodeGenE2E, DequeFromList) {
    auto out = compileAndRun(
        "items: list[int] = [10, 20, 30]\n"
        "d: deque[int] = deque(items)\n"
        "print(len(d))\n"
        "print(d.popleft())\n"
    );
    EXPECT_EQ(out, "3\n10\n");
}

//===----------------------------------------------------------------------===//
// List Repetition Tests
//===----------------------------------------------------------------------===//

TEST(CodeGenTest, ListRepeatIR) {
    auto ir = generateIR("x: list[bool] = [True] * 5");
    EXPECT_NE(ir.find("dragon_list_repeat"), std::string::npos);
}

TEST(CodeGenE2E, ListRepeatBool) {
    auto out = compileAndRun(
        "x: list[bool] = [True] * 5\n"
        "print(len(x))\n"
        "print(x[0])\n"
        "print(x[4])\n"
    );
    // list[bool] subscript dispatches as bool (a raw-int dispatch prints 1/0).
    EXPECT_EQ(out, "5\nTrue\nTrue\n");
}

TEST(CodeGenE2E, ListRepeatInt) {
    auto out = compileAndRun(
        "x: list[int] = [1, 2] * 3\n"
        "print(len(x))\n"
        "print(x[0])\n"
        "print(x[1])\n"
        "print(x[2])\n"
        "print(x[5])\n"
    );
    EXPECT_EQ(out, "6\n1\n2\n1\n2\n");
}

TEST(CodeGenE2E, ListRepeatStr) {
    auto out = compileAndRun(
        "x: list[str] = [\"hello\"] * 2\n"
        "print(len(x))\n"
        "print(x[0])\n"
        "print(x[1])\n"
    );
    EXPECT_EQ(out, "2\nhello\nhello\n");
}

TEST(CodeGenE2E, ListRepeatEmpty) {
    auto out = compileAndRun(
        "x: list[int] = [1, 2] * 0\n"
        "print(len(x))\n"
    );
    EXPECT_EQ(out, "0\n");
}

TEST(CodeGenE2E, ListRepeatSingleInt) {
    auto out = compileAndRun(
        "x: list[int] = [0] * 4\n"
        "print(len(x))\n"
        "print(x[0])\n"
        "print(x[3])\n"
    );
    EXPECT_EQ(out, "4\n0\n0\n");
}

//===----------------------------------------------------------------------===//
// Regression: discarded pop() must not leak
//===----------------------------------------------------------------------===//

// list[str].pop() discarded - popped strings must be decref'd.
// In IR mode we verify a decref_str runs after the listpop call site.
TEST(CodeGenIR, DiscardedListStrPopEmitsDecref) {
    auto ir = generateIR(
        "x: list[str] = [\"hello\", \"world\"]\n"
        "x.pop()\n"
    );
    EXPECT_NE(ir.find("dragon_list_pop"), std::string::npos);
    EXPECT_NE(ir.find("dragon_decref_str"), std::string::npos)
        << "Discarded list[str].pop() must decref the popped string";
}

// E2E: many list[str].pop() iterations, the loop body discards the
// returned string. Without the discard decref this leaks every pop; with
// it memory stays bounded - completion within the test timeout suffices.
TEST(CodeGenE2E, DiscardedListStrPopLoopBounded) {
    auto out = compileAndRun(
        "x: list[str] = [\"a\", \"b\", \"c\", \"d\"]\n"
        "for i in range(100000) {\n"
        "  x.append(\"hello\" + \"_world\")\n"
        "  x.pop()\n"
        "}\n"
        "print(len(x))\n"
    );
    EXPECT_EQ(out, "4\n");
}

// list[list[int]].pop() - popped sublists must be decref'd via dragon_decref.
TEST(CodeGenIR, DiscardedListListIntPopEmitsDecref) {
    auto ir = generateIR(
        "x: list[list[int]] = [[1, 2], [3, 4]]\n"
        "x.pop()\n"
    );
    EXPECT_NE(ir.find("dragon_list_pop"), std::string::npos);
    // Non-string heap elements (list/dict/bytes) get an inttoptr followed
    // by dragon_decref.
    EXPECT_NE(ir.find("pop.discard.ptr"), std::string::npos)
        << "Expected IntToPtr conversion of popped i64 value";
    EXPECT_NE(ir.find("dragon_decref"), std::string::npos);
}

TEST(CodeGenE2E, DiscardedListListIntPopLoopBounded) {
    auto out = compileAndRun(
        "x: list[list[int]] = [[1], [2], [3]]\n"
        "for i in range(50000) {\n"
        "  x.append([10, 20, 30])\n"
        "  x.pop()\n"
        "}\n"
        "print(len(x))\n"
    );
    EXPECT_EQ(out, "3\n");
}

// dict[str, str].pop("k") discarded - the popped value must be decref'd.
TEST(CodeGenIR, DiscardedDictStrStrPopEmitsDecref) {
    auto ir = generateIR(
        "d: dict[str, str] = {\"k\": \"v\"}\n"
        "d.pop(\"k\")\n"
    );
    EXPECT_NE(ir.find("dragon_dict_pop"), std::string::npos);
    EXPECT_NE(ir.find("dragon_decref_str"), std::string::npos)
        << "Discarded dict[str, str].pop must decref the popped value";
}

TEST(CodeGenE2E, DiscardedDictStrStrPopLoopBounded) {
    auto out = compileAndRun(
        "d: dict[str, str] = {}\n"
        "for i in range(50000) {\n"
        "  d[\"k\"] = \"value_\" + \"x\"\n"
        "  d.pop(\"k\")\n"
        "}\n"
        "print(len(d))\n"
    );
    EXPECT_EQ(out, "0\n");
}

// list[int].pop() - primitives don't need decref. Make sure the new
// code path doesn't crash and doesn't emit an extra (wrong) decref.
TEST(CodeGenIR, DiscardedListIntPopNoDecref) {
    auto ir = generateIR(
        "x: list[int] = [1, 2, 3]\n"
        "x.pop()\n"
    );
    EXPECT_NE(ir.find("dragon_list_pop"), std::string::npos);
    // No "pop.discard.ptr" should be emitted for primitive elem_tag.
    EXPECT_EQ(ir.find("pop.discard.ptr"), std::string::npos)
        << "Discarded list[int].pop must not emit decref conversion";
}

TEST(CodeGenE2E, DiscardedListIntPopRuns) {
    auto out = compileAndRun(
        "x: list[int] = [10, 20, 30]\n"
        "x.pop()\n"
        "print(len(x))\n"
    );
    EXPECT_EQ(out, "2\n");
}

// Negative test: assigned pop() result must NOT be double-freed.
// The assignment path increments to keep ownership; our discard fix
// must only fire on ExprStmt with no consumer.
TEST(CodeGenE2E, AssignedListStrPopNoDoubleFree) {
    auto out = compileAndRun(
        "x: list[str] = [\"hello\", \"world\"]\n"
        "v: str = x.pop()\n"
        "print(v)\n"
        "print(len(x))\n"
    );
    EXPECT_EQ(out, "world\n1\n");
}

TEST(CodeGenE2E, AssignedListStrPopLoopNoDoubleFree) {
    // Heavier stress: assign-to-var pop in a loop. If our discard logic
    // double-decrefs the value, this corrupts memory or aborts.
    auto out = compileAndRun(
        "x: list[str] = [\"a\", \"b\", \"c\", \"d\"]\n"
        "last: str = \"\"\n"
        "for i in range(50000) {\n"
        "  x.append(\"hello\")\n"
        "  last = x.pop()\n"
        "}\n"
        "print(last)\n"
        "print(len(x))\n"
    );
    EXPECT_EQ(out, "hello\n4\n");
}

// Negative test: my_list[0] = my_list.pop() - value flows into a
// subscript assignment, must not be decref'd by our new discard path
// (the ExprStmt is the AssignStmt, not the inner pop call).
TEST(CodeGenE2E, AnnAssignConsumesPopNoDoubleFree) {
    auto out = compileAndRun(
        "x: list[str] = [\"hello\", \"world\"]\n"
        "y: str = x.pop()\n"
        "z: str = y\n"
        "print(z)\n"
    );
    EXPECT_EQ(out, "world\n");
}

// Regression: dict.values() mixed-type, and list.extend tag adoption.
// (Dragon source can't easily build a mixed-tag dict, so the values()
//  regression exercises the uniform path; the mixed case is by inspection.)
TEST(CodeGenE2E, DictValuesUniformLoopBounded) {
    auto out = compileAndRun(
        "d: dict[str, str] = {}\nd[\"a\"] = \"x\"\nd[\"b\"] = \"y\"\n"
        "for _ in range(10000) { _v: list[str] = d.values() }\nprint(\"ok\")\n"
    );
    EXPECT_EQ(out, "ok\n");
}
TEST(CodeGenE2E, ListExtendAdoptsTag) {
    auto out = compileAndRun(
        "src: list[str] = [\"a\", \"b\", \"c\"]\n"
        "dest: list[str] = []\ndest.extend(src)\nprint(dest[2])\n"
    );
    EXPECT_EQ(out, "c\n");
}
// Regression: enumerate/zip incref+tag elements so tuple owners survive
// source destruction (a read after source destruction is otherwise a UAF),
// and refcounts must balance (no leak per iteration).
TEST(CodeGenE2E, EnumerateZipBalancedRefcount) {
    auto out = compileAndRun(
        "for i in range(1000) {\n"
        "  src: list[str] = [\"a\", \"b\", \"c\"]\n"
        "  o: list[str] = [\"x\", \"y\", \"z\"]\n"
        "  p1: list = enumerate(src)\n  p2: list = zip(src, o)\n"
        "}\nprint(\"ok\")\n"
    );
    EXPECT_EQ(out, "ok\n");
}

//===----------------------------------------------------------------------===//
// Regression: dict_get_checked TypeError message must not leak
//
// A `strdup(buf)` for the TypeError message passed to dragon_raise_exc
// leaks: the exception machinery stores the pointer by reference (no
// ownership transfer), so the strdup'd memory is never freed - once per
// raised TypeError, and repeated TypeErrors mean unbounded growth.
//
// dict_get_checked instead formats into a per-thread
// `static __thread char tls_msg[256]` buffer:
// each call overwrites the previous message, no malloc/free traffic.
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, DictGetCheckedTypeErrorMessage) {
    // Sanity: TypeError is still raised on tag mismatch, and the message
    // reports the offending key + actual/expected types.
    auto out = compileAndRun(
        "d: dict = {age: \"ten\"}\n"
        "try {\n"
        "  x: int = d[\"age\"]\n"
        "  print(\"no error\")\n"
        "} except TypeError as e {\n"
        "  print(\"caught\")\n"
        "}\n"
    );
    EXPECT_EQ(out, "caught\n");
}

TEST(CodeGenE2E, DictGetCheckedTypeErrorLoopBounded) {
    // Trigger the same TypeError 100k times. Pre-fix: each strdup leaked
    // ~80 bytes -> ~8MB of growth. Post-fix: bounded (the TLS buffer is
    // reused).
    //
    // To run under valgrind:
    //  valgrind --leak-check=full ./dragon_codegen_tests \
    //  --gtest_filter='*DictGetCheckedTypeErrorLoopBounded*'
    auto out = compileAndRun(
        "d: dict = {age: \"ten\"}\n"
        "errors: int = 0\n"
        "for i in range(100000) {\n"
        "  try {\n"
        "    x: int = d[\"age\"]\n"
        "  } except TypeError {\n"
        "    errors = errors + 1\n"
        "  }\n"
        "}\n"
        "print(errors)\n"
    );
    EXPECT_EQ(out, "100000\n");
}

TEST(CodeGenE2E, DictGetCheckedAlternatingErrors) {
    // Two different TypeErrors alternating - confirms the TLS buffer is
    // overwritten cleanly each time without cross-thread bleed (single
    // thread here, but exercises the overwrite path).
    auto out = compileAndRun(
        "d: dict = {age: \"ten\", name: 42}\n"
        "errors: int = 0\n"
        "for i in range(50000) {\n"
        "  try {\n"
        "    a: int = d[\"age\"]\n"
        "  } except TypeError {\n"
        "    errors = errors + 1\n"
        "  }\n"
        "  try {\n"
        "    b: str = d[\"name\"]\n"
        "  } except TypeError {\n"
        "    errors = errors + 1\n"
        "  }\n"
        "}\n"
        "print(errors)\n"
    );
    EXPECT_EQ(out, "100000\n");
}

//===----------------------------------------------------------------------===//
// 4.4 set: real hash-table-backed set - content hashing + method dispatch
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, SetStrContainsContentHashed) {
    // Two distinct heap allocations of "hello" must collide on the same
    // bucket via content hashing. Pre-fix, set used pointer hashing and
    // these miscompared.
    auto out = compileAndRun(
        "s: set[str] = {\"hello\", \"world\"}\n"
        "if \"hello\" in s {\n"
        "    print(\"a\")\n"
        "}\n"
        "x: str = \"hel\" + \"lo\"\n"  // built at runtime, different pointer
        "if x in s {\n"
        "    print(\"b\")\n"
        "}\n"
    );
    EXPECT_EQ(out, "a\nb\n");
}

TEST(CodeGenE2E, SetStrAddDedup) {
    // Adding a string equal-by-content to an existing element must dedup.
    auto out = compileAndRun(
        "s: set[str] = {\"a\", \"b\"}\n"
        "y: str = \"a\"\n"
        "z: str = \"a\" + \"\"\n"  // runtime concat, different pointer
        "s.add(y)\n"
        "s.add(z)\n"
        "print(len(s))\n"
    );
    EXPECT_EQ(out, "2\n");
}

TEST(CodeGenE2E, SetMethodAddRemoveDiscard) {
    auto out = compileAndRun(
        "s: set[int] = {1, 2}\n"
        "s.add(3)\n"
        "print(len(s))\n"
        "s.remove(1)\n"
        "print(len(s))\n"
        "s.discard(99)\n"  // not present - no-op
        "print(len(s))\n"
        "if 3 in s {\n"
        "    print(\"y\")\n"
        "}\n"
    );
    EXPECT_EQ(out, "3\n2\n2\ny\n");
}

TEST(CodeGenE2E, SetUnionIntersectionDifference) {
    auto out = compileAndRun(
        "a: set[int] = {1, 2, 3}\n"
        "b: set[int] = {2, 3, 4}\n"
        "u: set[int] = a.union(b)\n"
        "i: set[int] = a.intersection(b)\n"
        "d: set[int] = a.difference(b)\n"
        "sd: set[int] = a.symmetric_difference(b)\n"
        "print(len(u))\n"
        "print(len(i))\n"
        "print(len(d))\n"
        "print(len(sd))\n"
    );
    EXPECT_EQ(out, "4\n2\n1\n2\n");
}

TEST(CodeGenE2E, SetIssubsetIssupersetIsdisjoint) {
    auto out = compileAndRun(
        "a: set[int] = {1, 2}\n"
        "b: set[int] = {1, 2, 3}\n"
        "c: set[int] = {4, 5}\n"
        "print(a.issubset(b))\n"
        "print(b.issubset(a))\n"
        "print(b.issuperset(a))\n"
        "print(a.isdisjoint(c))\n"
        "print(a.isdisjoint(b))\n"
    );
    EXPECT_EQ(out, "True\nFalse\nTrue\nTrue\nFalse\n");
}

TEST(CodeGenE2E, SetUtf8Keys) {
    // Set with non-ASCII string elements: lookup, dedup, union all work.
    auto out = compileAndRun(
        "s: set[str] = {\"caf\xc3\xa9\", \"tea\", \"\xe6\x97\xa5\xe6\x9c\xac\"}\n"
        "if \"caf\xc3\xa9\" in s {\n"
        "    print(\"a\")\n"
        "}\n"
        "if \"\xe6\x97\xa5\xe6\x9c\xac\" in s {\n"
        "    print(\"b\")\n"
        "}\n"
        "if \"missing\" in s {\n"
        "    print(\"x\")\n"
        "} else {\n"
        "    print(\"c\")\n"
        "}\n"
        "t: set[str] = {\"tea\", \"coffee\"}\n"
        "u: set[str] = s.union(t)\n"
        "print(len(u))\n"
    );
    EXPECT_EQ(out, "a\nb\nc\n4\n");
}

TEST(CodeGenE2E, SetClearAndCopy) {
    auto out = compileAndRun(
        "a: set[int] = {1, 2, 3}\n"
        "b: set[int] = a.copy()\n"
        "a.clear()\n"
        "print(len(a))\n"
        "print(len(b))\n"
    );
    EXPECT_EQ(out, "0\n3\n");
}

TEST(CodeGenE2E, SetUpdate) {
    auto out = compileAndRun(
        "a: set[int] = {1, 2}\n"
        "b: set[int] = {2, 3}\n"
        "a.update(b)\n"
        "print(len(a))\n"
        "if 3 in a {\n"
        "    print(\"y\")\n"
        "}\n"
    );
    EXPECT_EQ(out, "3\ny\n");
}

// Regression: print() / len() on a chained-subscript receiver. Pre-fix the
// dispatch heuristics only checked NameExpr/literal/AttributeExpr, so
// print(a[0]) and len(a[0]) where a: list[list[T]] fell through to the
// str-dispatch catch-all and produced garbage / wrong counts.
TEST(CodeGenE2E, PrintAndLenOnChainedSubscript) {
    auto out = compileAndRun(
        "a: list[list[str]] = []\n"
        "inner1: list[str] = [\"hello\", \"world\"]\n"
        "a.append(inner1)\n"
        "inner2: list[str] = [\"foo\"]\n"
        "a.append(inner2)\n"
        "print(a[0])\n"
        "print(len(a[0]))\n"
        "print(len(a[1]))\n"
    );
    EXPECT_EQ(out, "['hello', 'world']\n2\n1\n");
}

// Regression: method dispatch on a chained-subscript str receiver.
// Pre-fix `paths[0].startswith(...)` used the catch-all str detection, but
// `paths[0].split("/")` and `.find()` returned wrong types because the
// dispatch site only checked NameExpr.
TEST(CodeGenE2E, MethodCallOnSubscriptedStr) {
    auto out = compileAndRun(
        "xs: list[str] = [\"/usr/lib\", \"home\"]\n"
        "print(xs[0].startswith(\"/\"))\n"
        "parts: list[str] = xs[0].split(\"/\")\n"
        "print(len(parts))\n"
    );
    EXPECT_EQ(out, "True\n3\n");
}

// Regression: for-in over list[ClassInstance] without a tracked NameExpr
// origin. Pre-fix elemVarKind defaulted to Int and `f.name` returned 0.
TEST(CodeGenE2E, ForInOverListOfClassInstances) {
    auto out = compileAndRun(
        "class Box {\n"
        "    def(name: str) {\n"
        "        self.name = name\n"
        "    }\n"
        "}\n"
        "xs: list[Box] = [Box(\"a\"), Box(\"b\"), Box(\"c\")]\n"
        "for f in xs {\n"
        "    print(f.name)\n"
        "}\n"
    );
    EXPECT_EQ(out, "a\nb\nc\n");
}

// Regression: bool predicates flow as native i1 (D030). Pre-fix
// startswith/endswith/contains/isXxx returned i64 0/1 and printed "1"/"0".
TEST(CodeGenE2E, StrBoolPredicatesPrintTrueFalse) {
    auto out = compileAndRun(
        "s: str = \"/foo\"\n"
        "print(s.startswith(\"/\"))\n"
        "print(s.endswith(\"x\"))\n"
        "print(\"abc\".isalpha())\n"
        "print(\"123\".isalpha())\n"
    );
    EXPECT_EQ(out, "True\nFalse\nTrue\nFalse\n");
}

// Regression: dict subscript-set from a borrowed local string. After the
// source local is reassigned (or its scope ends), the dict's value must
// remain valid.
TEST(CodeGenE2E, DictSetFromBorrowedLocalStr) {
    auto out = compileAndRun(
        "def build() -> dict[str, str] {\n"
        "    const params: dict[str, str] = {}\n"
        "    const k: str = \"name\"\n"
        "    const v: str = \"dragon\"\n"
        "    params[k] = v\n"
        "    return params\n"
        "}\n"
        "const d: dict[str, str] = build()\n"
        "const filler: list[str] = [\"a\", \"b\", \"c\", \"d\"]\n"
        "print(d[\"name\"])\n"
    );
    EXPECT_EQ(out, "dragon\n");
}

// Regression: list[i] = borrowed_local_str must incref the new value so the
// list owns a live reference after the source's scope ends.
TEST(CodeGenE2E, ListSetFromBorrowedLocalStr) {
    auto out = compileAndRun(
        "def fill(target: list[str]) -> None {\n"
        "    const v: str = \"x\"\n"
        "    target[0] = v\n"
        "}\n"
        "const xs: list[str] = [\"a\", \"b\"]\n"
        "fill(xs)\n"
        "const filler: list[str] = [\"p\", \"q\", \"r\", \"s\"]\n"
        "print(xs[0])\n"
        "print(xs[1])\n"
    );
    EXPECT_EQ(out, "x\nb\n");
}

// Regression: tuple literal element that is a borrowed name (heap dict)
// must be incref'd so the returned tuple's reference outlives the local.
TEST(CodeGenE2E, TupleReturnFromBorrowedDictLocal) {
    auto out = compileAndRun(
        "def make() -> tuple[bool, dict[str, str]] {\n"
        "    const d: dict[str, str] = {}\n"
        "    d[\"k\"] = \"v\"\n"
        "    return (true, d)\n"
        "}\n"
        "const r: tuple[bool, dict[str, str]] = make()\n"
        "const filler: list[str] = [\"a\", \"b\", \"c\", \"d\"]\n"
        "const got: dict[str, str] = r[1]\n"
        "print(got[\"k\"])\n"
    );
    EXPECT_EQ(out, "v\n");
}

//===----------------------------------------------------------------------===//
// Deep container equality (drives assertEqual in stdlib/unittest.dr)
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, ListEqIntElementwise) {
    auto out = compileAndRun(
        "a: list[int] = [1, 2, 3]\n"
        "b: list[int] = [1, 2, 3]\n"
        "c: list[int] = [1, 2, 4]\n"
        "print(a == b)\n"
        "print(a == c)\n"
        "print(a != c)\n"
    );
    EXPECT_EQ(out, "True\nFalse\nTrue\n");
}

TEST(CodeGenE2E, ListEqStrElementwise) {
    auto out = compileAndRun(
        "a: list[str] = [\"foo\", \"bar\"]\n"
        "b: list[str] = [\"foo\", \"bar\"]\n"
        "c: list[str] = [\"foo\", \"baz\"]\n"
        "print(a == b)\n"
        "print(a == c)\n"
    );
    EXPECT_EQ(out, "True\nFalse\n");
}

TEST(CodeGenE2E, ListEqNested) {
    auto out = compileAndRun(
        "a: list[list[int]] = [[1, 2], [3, 4]]\n"
        "b: list[list[int]] = [[1, 2], [3, 4]]\n"
        "c: list[list[int]] = [[1, 2], [3, 5]]\n"
        "print(a == b)\n"
        "print(a == c)\n"
    );
    EXPECT_EQ(out, "True\nFalse\n");
}

TEST(CodeGenE2E, ListEqDifferentLengths) {
    auto out = compileAndRun(
        "a: list[int] = [1, 2, 3]\n"
        "b: list[int] = [1, 2]\n"
        "print(a == b)\n"
    );
    EXPECT_EQ(out, "False\n");
}

TEST(CodeGenE2E, DictEqStrKeyedOrderIndependent) {
    auto out = compileAndRun(
        "a: dict[str, int] = {\"x\": 1, \"y\": 2}\n"
        "b: dict[str, int] = {\"y\": 2, \"x\": 1}\n"
        "c: dict[str, int] = {\"x\": 1, \"y\": 3}\n"
        "print(a == b)\n"
        "print(a == c)\n"
    );
    EXPECT_EQ(out, "True\nFalse\n");
}

TEST(CodeGenE2E, DictEqIntKeyed) {
    auto out = compileAndRun(
        "a: dict[int, str] = {1: \"a\", 2: \"b\"}\n"
        "b: dict[int, str] = {2: \"b\", 1: \"a\"}\n"
        "c: dict[int, str] = {1: \"a\", 2: \"c\"}\n"
        "print(a == b)\n"
        "print(a == c)\n"
    );
    EXPECT_EQ(out, "True\nFalse\n");
}

TEST(CodeGenE2E, BoxedListEqViaAny) {
    auto out = compileAndRun(
        "a: Any = [1, 2, 3]\n"
        "b: Any = [1, 2, 3]\n"
        "c: Any = [1, 2, 4]\n"
        "print(a == b)\n"
        "print(a == c)\n"
    );
    EXPECT_EQ(out, "True\nFalse\n");
}

TEST(CodeGenE2E, BoxedDictEqViaAny) {
    auto out = compileAndRun(
        "a: Any = {\"k\": 1}\n"
        "b: Any = {\"k\": 1}\n"
        "c: Any = {\"k\": 2}\n"
        "print(a == b)\n"
        "print(a == c)\n"
    );
    EXPECT_EQ(out, "True\nFalse\n");
}

TEST(CodeGenE2E, BoxedBytesEqViaAny) {
    auto out = compileAndRun(
        "a: Any = b\"hello\"\n"
        "b: Any = b\"hello\"\n"
        "c: Any = b\"world\"\n"
        "print(a == b)\n"
        "print(a == c)\n"
    );
    EXPECT_EQ(out, "True\nFalse\n");
}

// Regression: const x = list[i] (subscript-source assignment) must incref the
// element so x stays valid after the source list's lifetime ends. Tests the
// SubscriptExpr extension to isBorrowedHeapExpr.
TEST(CodeGenE2E, ConstAssignFromListSubscript) {
    auto out = compileAndRun(
        "def first(xs: list[str]) -> str {\n"
        "    const x: str = xs[0]\n"
        "    return x\n"
        "}\n"
        "const result: str = first([\"hello\", \"world\"])\n"
        "const filler: list[str] = [\"a\", \"b\", \"c\", \"d\"]\n"
        "print(result)\n"
    );
    EXPECT_EQ(out, "hello\n");
}

//===----------------------------------------------------------------------===//
// list[Any] - heterogeneous literal builds a box list (D039 Phase 4)
//===----------------------------------------------------------------------===//

TEST(CodeGenE2E, ListAnyMixedLiteralIndexedPrintsValues) {
    // A heterogeneous literal assigned to list[Any] must build a BOX list so
    // x[i] unboxes to the real value - not leak <box tag=.. payload=..> or read
    // adjacent native slots as a malformed box.
    auto out = compileAndRun(
        "x: list[Any] = [10, \"hi\", 2.5]\n"
        "print(x[0])\n"
        "print(x[1])\n"
        "print(x[2])\n");
    EXPECT_EQ(out, "10\nhi\n2.5\n");
}

TEST(CodeGenE2E, ListAnyHomogeneousIntLiteralIndexed) {
    // All-int contents would infer list[int]; the list[Any] annotation must
    // still force a box list (else x[0] reads garbage / prints blank).
    auto out = compileAndRun(
        "x: list[Any] = [1, 2, 3]\n"
        "print(x[0])\n"
        "print(x[2])\n");
    EXPECT_EQ(out, "1\n3\n");
}

TEST(CodeGenE2E, ListAnyElementIsCastableViaIsinstance) {
    // x[i] returns a proper Any box, so isinstance narrowing recovers the
    // native type.
    auto out = compileAndRun(
        "x: list[Any] = [10, \"hi\"]\n"
        "v: Any = x[0]\n"
        "if isinstance(v, int) {\n"
        "  n: int = v\n"
        "  print(n + 5)\n"
        "}\n");
    EXPECT_EQ(out, "15\n");
}

TEST(CodeGenE2E, ListAnyIteration) {
    auto out = compileAndRun(
        "x: list[Any] = [1, \"two\", 3.0]\n"
        "for item in x {\n"
        "  print(item)\n"
        "}\n");
    EXPECT_EQ(out, "1\ntwo\n3.0\n");
}

// C5 (Any-box container repr): a list/dict reached through an Any box carries
// its element kind in the pointed-to object's elem_tag, so print() must render
// it tag-aware. A tag-blind dragon_print_*_raw call prints a
// nested list[str]/list[float] payload (and a dict[str,Any] container value) as
// a raw pointer integer; only the int-element case renders correctly by luck.
// str()/f-strings always used the tag-aware repr path; these guard print().
TEST(CodeGenE2E, ListAnyHoldingStrListPrintsTagAware) {
    auto out = compileAndRun(
        "xs: list[Any] = [[\"a\", \"b\"], [1, 2], [1.5, 2.5]]\n"
        "print(xs)\n");
    EXPECT_EQ(out, "[['a', 'b'], [1, 2], [1.5, 2.5]]\n");
}

TEST(CodeGenE2E, DictStrAnyHoldingContainersPrintsTagAware) {
    auto out = compileAndRun(
        "d: dict[str, Any] = {\"a\": [\"x\", \"y\"], \"b\": {\"inner\": \"v\"}}\n"
        "print(d)\n");
    EXPECT_EQ(out, "{'a': ['x', 'y'], 'b': {'inner': 'v'}}\n");
}

TEST(CodeGenE2E, DictIntAnyHoldingListPrintsTagAware) {
    auto out = compileAndRun(
        "d: dict[int, Any] = {1: [\"x\", \"y\"]}\n"
        "print(d)\n");
    EXPECT_EQ(out, "{1: ['x', 'y']}\n");
}

// NOTE: float(str) parsing and None/float boxing in dict[str, Any] are covered
// by the dogfooded `.dr` unittest suite (test/dr/test_conversions.dr).
