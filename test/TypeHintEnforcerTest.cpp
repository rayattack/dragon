#include <gtest/gtest.h>
#include "TestHelpers.h"
#include "dragon/TypeHintEnforcer.h"

using namespace dragon;
using namespace dragon::test;

// Helper: parse as Python (.py), then run enforcer
static bool enforceOk(const std::string& source, EnforcerOptions opts = {}) {
    auto module = parse(source, /*isDragon=*/false);
    if (!module) return false;
    TypeHintEnforcer enforcer(opts);
    return enforcer.enforce(*module);
}

// Helper: parse as Python, run enforcer, return diagnostics
static std::vector<EnforcerDiagnostic> enforceDiags(const std::string& source,
                                                      EnforcerOptions opts = {}) {
    auto module = parse(source, /*isDragon=*/false);
    if (!module) return {};
    TypeHintEnforcer enforcer(opts);
    enforcer.enforce(*module);
    return enforcer.diagnostics();
}

// Helper: parse as Dragon (.dr), then run enforcer
static bool enforceDragonOk(const std::string& source) {
    auto module = parse(source, /*isDragon=*/true);
    if (!module) return false;
    TypeHintEnforcer enforcer;
    return enforcer.enforce(*module);
}

//===----------------------------------------------------------------------===//
// Fully typed .py functions should pass
//===----------------------------------------------------------------------===//

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

//===----------------------------------------------------------------------===//
// Missing parameter types
//===----------------------------------------------------------------------===//

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

//===----------------------------------------------------------------------===//
// Missing return type
//===----------------------------------------------------------------------===//

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

//===----------------------------------------------------------------------===//
// __init__ is exempt from return type
//===----------------------------------------------------------------------===//

TEST(TypeHintEnforcer, InitNoReturnTypeOk) {
    EXPECT_TRUE(enforceOk(
        "class Foo:\n"
        "    def __init__(self, x: int):\n"
        "        pass\n"
    ));
}

//===----------------------------------------------------------------------===//
// Class methods - self/cls parameter exempt
//===----------------------------------------------------------------------===//

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

//===----------------------------------------------------------------------===//
// Module-level variables
//===----------------------------------------------------------------------===//

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
    // __all__, __version__, etc. are exempt
    EXPECT_TRUE(enforceOk(
        "__version__ = \"1.0\"\n"
    ));
}

//===----------------------------------------------------------------------===//
// Options: disable specific checks
//===----------------------------------------------------------------------===//

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

//===----------------------------------------------------------------------===//
// Empty module
//===----------------------------------------------------------------------===//

TEST(TypeHintEnforcer, EmptyModulePasses) {
    EXPECT_TRUE(enforceOk(""));
}

TEST(TypeHintEnforcer, PassOnlyPasses) {
    EXPECT_TRUE(enforceOk("pass"));
}

//===----------------------------------------------------------------------===//
// Multiple errors accumulated
//===----------------------------------------------------------------------===//

TEST(TypeHintEnforcer, MultipleErrors) {
    auto diags = enforceDiags(
        "x = 5\n"
        "def foo(a, b):\n"
        "    return 0\n"
    );
    // x missing type, a missing type, b missing type, foo missing return type
    EXPECT_GE(diags.size(), 3u);
}

//===----------------------------------------------------------------------===//
// Dragon .dr files - enforcer still works but typically not called
//===----------------------------------------------------------------------===//

TEST(TypeHintEnforcer, DragonFileWithTypesOk) {
    // Dragon files parsed with isDragon=true should also pass
    // (In practice, the Driver only calls enforcer for .py files)
    EXPECT_TRUE(enforceDragonOk(
        "def add(x: int, y: int) -> int {\n"
        "    return x + y\n"
        "}\n"
    ));
}

//===----------------------------------------------------------------------===//
// Mixed: some functions typed, some not
//===----------------------------------------------------------------------===//

TEST(TypeHintEnforcer, MixedFunctions) {
    auto diags = enforceDiags(
        "def typed(x: int) -> int:\n"
        "    return x\n"
        "def untyped(y):\n"
        "    return y\n"
    );
    // untyped: missing type for 'y' + missing return type = 2 errors
    EXPECT_EQ(diags.size(), 2u);
}
