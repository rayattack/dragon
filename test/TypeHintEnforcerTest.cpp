#include <gtest/gtest.h>
#include "TestHelpers.h"
#include "dragon/TypeHintEnforcer.h"

using namespace dragon;
using namespace dragon::test;

static bool enforceOk(const std::string& source, EnforcerOptions opts = {}) {
    auto module = parse(source, false);
    if (!module) return false;
    TypeHintEnforcer enforcer(opts);
    return enforcer.enforce(*module);
}

static std::vector<EnforcerDiagnostic> enforceDiags(const std::string& source,
                                                      EnforcerOptions opts = {}) {
    auto module = parse(source, false);
    if (!module) return {};
    TypeHintEnforcer enforcer(opts);
    enforcer.enforce(*module);
    return enforcer.diagnostics();
}

static bool enforceDragonOk(const std::string& source) {
    auto module = parse(source, true);
    if (!module) return false;
    TypeHintEnforcer enforcer;
    return enforcer.enforce(*module);
}

TEST(TypeHintEnforcer, TypedFunctionPasses) {
    EXPECT_TRUE(enforceOk(
        "def add(x: int, y: int) -> int:\n"
        "    return x + y\n"
    ));
}

TEST(TypeHintEnforcer, TypedFunctionNoParams) {
    EXPECT_TRUE(enforceOk(
        "def greet() -> str:\n"
        "    return \"hello\"\n"
    ));
}

TEST(TypeHintEnforcer, TypedFunctionReturnsNone) {
    EXPECT_TRUE(enforceOk(
        "def do_stuff(x: int) -> None:\n"
        "    pass\n"
    ));
}

TEST(TypeHintEnforcer, MissingParamType) {
    EXPECT_FALSE(enforceOk(
        "def add(x, y: int) -> int:\n"
        "    return x + y\n"
    ));
    auto diags = enforceDiags(
        "def add(x, y: int) -> int:\n"
        "    return x + y\n"
    );
    ASSERT_EQ(diags.size(), 1u);
    EXPECT_NE(diags[0].message.find("'x'"), std::string::npos);
    EXPECT_NE(diags[0].message.find("add"), std::string::npos);
}

TEST(TypeHintEnforcer, AllParamsMissingTypes) {
    auto diags = enforceDiags(
        "def process(a, b, c) -> int:\n"
        "    return 0\n"
    );
    EXPECT_EQ(diags.size(), 3u);
}

TEST(TypeHintEnforcer, MissingReturnType) {
    EXPECT_FALSE(enforceOk(
        "def add(x: int, y: int):\n"
        "    return x + y\n"
    ));
    auto diags = enforceDiags(
        "def add(x: int, y: int):\n"
        "    return x + y\n"
    );
    ASSERT_EQ(diags.size(), 1u);
    EXPECT_NE(diags[0].message.find("return type"), std::string::npos);
    EXPECT_NE(diags[0].message.find("add"), std::string::npos);
}

TEST(TypeHintEnforcer, InitNoReturnTypeOk) {
    EXPECT_TRUE(enforceOk(
        "class Foo:\n"
        "    def __init__(self, x: int):\n"
        "        pass\n"
    ));
}

TEST(TypeHintEnforcer, MethodSelfExempt) {
    EXPECT_TRUE(enforceOk(
        "class Foo:\n"
        "    def bar(self, x: int) -> int:\n"
        "        return x\n"
    ));
}

TEST(TypeHintEnforcer, ClassMethodClsExempt) {
    EXPECT_TRUE(enforceOk(
        "class Foo:\n"
        "    def create(cls, name: str) -> str:\n"
        "        return name\n"
    ));
}

TEST(TypeHintEnforcer, MethodNonSelfParamMissingType) {
    EXPECT_FALSE(enforceOk(
        "class Foo:\n"
        "    def bar(self, x) -> int:\n"
        "        return 0\n"
    ));
}

TEST(TypeHintEnforcer, ModuleVarWithTypeOk) {
    EXPECT_TRUE(enforceOk(
        "x: int = 5\n"
    ));
}

TEST(TypeHintEnforcer, ModuleVarWithoutType) {
    EXPECT_FALSE(enforceOk(
        "x = 5\n"
    ));
    auto diags = enforceDiags("x = 5\n");
    ASSERT_EQ(diags.size(), 1u);
    EXPECT_NE(diags[0].message.find("'x'"), std::string::npos);
}

TEST(TypeHintEnforcer, DunderVarExempt) {
    EXPECT_TRUE(enforceOk(
        "__version__ = \"1.0\"\n"
    ));
}

TEST(TypeHintEnforcer, DisableParamTypeCheck) {
    EnforcerOptions opts;
    opts.requireFunctionParamTypes = false;
    EXPECT_TRUE(enforceOk(
        "def add(x, y) -> int:\n"
        "    return 0\n",
        opts
    ));
}

TEST(TypeHintEnforcer, DisableReturnTypeCheck) {
    EnforcerOptions opts;
    opts.requireReturnTypes = false;
    EXPECT_TRUE(enforceOk(
        "def add(x: int, y: int):\n"
        "    return x + y\n",
        opts
    ));
}

TEST(TypeHintEnforcer, DisableModuleVarCheck) {
    EnforcerOptions opts;
    opts.requireModuleVarTypes = false;
    EXPECT_TRUE(enforceOk("x = 5\n", opts));
}

TEST(TypeHintEnforcer, EmptyModulePasses) {
    EXPECT_TRUE(enforceOk(""));
}

TEST(TypeHintEnforcer, PassOnlyPasses) {
    EXPECT_TRUE(enforceOk("pass"));
}

TEST(TypeHintEnforcer, MultipleErrors) {
    auto diags = enforceDiags(
        "x = 5\n"
        "def foo(a, b):\n"
        "    return 0\n"
    );
    EXPECT_GE(diags.size(), 3u);
}

TEST(TypeHintEnforcer, DragonFileWithTypesOk) {
    EXPECT_TRUE(enforceDragonOk(
        "def add(x: int, y: int) -> int {\n"
        "    return x + y\n"
        "}\n"
    ));
}

TEST(TypeHintEnforcer, MixedFunctions) {
    auto diags = enforceDiags(
        "def typed(x: int) -> int:\n"
        "    return x\n"
        "def untyped(y):\n"
        "    return y\n"
    );
    EXPECT_EQ(diags.size(), 2u);
}
