#include <gtest/gtest.h>
#include "TestHelpers.h"
#include "dragon/TypeHintEnforcer.h"
#include "CodeBlock.h"

using namespace dragon;
using namespace dragon::test;

static std::string code(const std::string& block) {
    return extractCode("TypeHintEnforcerTest.md", block);
}

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
    EXPECT_TRUE(enforceOk(code("typed_function_passes")));
}

TEST(TypeHintEnforcer, TypedFunctionNoParams) {
    EXPECT_TRUE(enforceOk(code("typed_function_no_params")));
}

TEST(TypeHintEnforcer, TypedFunctionReturnsNone) {
    EXPECT_TRUE(enforceOk(code("typed_function_returns_none")));
}

TEST(TypeHintEnforcer, MissingParamType) {
    EXPECT_FALSE(enforceOk(code("missing_param_type")));
    auto diags = enforceDiags(code("missing_param_type_2"));
    ASSERT_EQ(diags.size(), 1u);
    EXPECT_NE(diags[0].message.find("'x'"), std::string::npos);
    EXPECT_NE(diags[0].message.find("add"), std::string::npos);
}

TEST(TypeHintEnforcer, AllParamsMissingTypes) {
    auto diags = enforceDiags(code("all_params_missing_types"));
    EXPECT_EQ(diags.size(), 3u);
}

TEST(TypeHintEnforcer, MissingReturnType) {
    EXPECT_FALSE(enforceOk(code("missing_return_type")));
    auto diags = enforceDiags(code("missing_return_type_2"));
    ASSERT_EQ(diags.size(), 1u);
    EXPECT_NE(diags[0].message.find("return type"), std::string::npos);
    EXPECT_NE(diags[0].message.find("add"), std::string::npos);
}

TEST(TypeHintEnforcer, InitNoReturnTypeOk) {
    EXPECT_TRUE(enforceOk(code("init_no_return_type_ok")));
}

TEST(TypeHintEnforcer, MethodSelfExempt) {
    EXPECT_TRUE(enforceOk(code("method_self_exempt")));
}

TEST(TypeHintEnforcer, ClassMethodClsExempt) {
    EXPECT_TRUE(enforceOk(code("class_method_cls_exempt")));
}

TEST(TypeHintEnforcer, MethodNonSelfParamMissingType) {
    EXPECT_FALSE(enforceOk(code("method_non_self_param_missing_type")));
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
        code("disable_param_type_check"),
        opts
    ));
}

TEST(TypeHintEnforcer, DisableReturnTypeCheck) {
    EnforcerOptions opts;
    opts.requireReturnTypes = false;
    EXPECT_TRUE(enforceOk(
        code("disable_return_type_check"),
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
    auto diags = enforceDiags(code("multiple_errors"));
    EXPECT_GE(diags.size(), 3u);
}

TEST(TypeHintEnforcer, DragonFileWithTypesOk) {
    EXPECT_TRUE(enforceDragonOk(code("dragon_file_with_types_ok")));
}

TEST(TypeHintEnforcer, MixedFunctions) {
    auto diags = enforceDiags(code("mixed_functions"));
    EXPECT_EQ(diags.size(), 2u);
}

TEST(TypeHintEnforcer, DragonUntypedParamRejected) {
    EXPECT_FALSE(enforceDragonOk(code("dragon_untyped_param_rejected")));
}

TEST(TypeHintEnforcer, DragonUntypedParamNamesTheParameter) {
    EnforcerOptions opts;
    opts.requireReturnTypes = false;
    opts.requireModuleVarTypes = false;
    auto module = parse(code("dragon_untyped_param_rejected"), true);
    ASSERT_NE(module, nullptr);
    TypeHintEnforcer enforcer(opts);
    enforcer.enforce(*module);
    ASSERT_EQ(enforcer.diagnostics().size(), 2u);
    EXPECT_NE(enforcer.diagnostics()[0].message.find(
                  "missing type annotation for parameter 'a' in function 'add'"),
              std::string::npos)
        << enforcer.diagnostics()[0].message;
    EXPECT_NE(enforcer.diagnostics()[1].message.find("parameter 'b'"),
              std::string::npos)
        << enforcer.diagnostics()[1].message;
}

TEST(TypeHintEnforcer, DragonUntypedMethodParamRejected) {
    EnforcerOptions opts;
    opts.requireReturnTypes = false;
    opts.requireModuleVarTypes = false;
    auto module = parse(code("dragon_untyped_param_in_method_rejected"), true);
    ASSERT_NE(module, nullptr);
    TypeHintEnforcer enforcer(opts);
    EXPECT_FALSE(enforcer.enforce(*module));
}

TEST(TypeHintEnforcer, DragonNeedsNoReturnAnnotation) {
    EnforcerOptions opts;
    opts.requireReturnTypes = false;
    opts.requireModuleVarTypes = false;
    auto module = parse(code("dragon_no_return_type_still_ok"), true);
    ASSERT_NE(module, nullptr);
    TypeHintEnforcer enforcer(opts);
    EXPECT_TRUE(enforcer.enforce(*module));
}
