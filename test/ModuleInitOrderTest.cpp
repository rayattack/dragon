#include "TestHelpers.h"
#include "dragon/DefiniteAssignment.h"
#include <gtest/gtest.h>
#include "CodeBlock.h"

using namespace dragon;
using namespace dragon::test;

static std::string code(const std::string& block) {
    return extractCode("ModuleInitOrderTest.md", block);
}

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
    EXPECT_TRUE(daHasError(code("direct_forward_const_read__class_typed")));
}

TEST(ModuleInitOrderTest, DirectForwardConstRead_Scalar) {
    EXPECT_TRUE(daHasError(code("direct_forward_const_read__scalar")));
}

TEST(ModuleInitOrderTest, InterprocForwardConstRead) {
    EXPECT_TRUE(daHasError(code("interproc_forward_const_read")));
}

TEST(ModuleInitOrderTest, TwoConstInitCycle) {
    EXPECT_TRUE(daHasError(code("two_const_init_cycle")));
}

TEST(ModuleInitOrderTest, InterprocInitCycle) {
    EXPECT_TRUE(daHasError(code("interproc_init_cycle")));
}

TEST(ModuleInitOrderTest, CorrectOrderConstChain_ClassTyped) {
    EXPECT_FALSE(daHasError(code("correct_order_const_chain__class_typed")));
}

TEST(ModuleInitOrderTest, CorrectOrderConstChain_Scalar) {
    EXPECT_FALSE(daHasError(code("correct_order_const_chain__scalar")));
}

TEST(ModuleInitOrderTest, ForwardFunctionRefPureHelper) {
    EXPECT_FALSE(daHasError(code("forward_function_ref_pure_helper")));
}

TEST(ModuleInitOrderTest, ForwardFunctionReadsConstButNotCalledDuringInit) {
    EXPECT_FALSE(daHasError(code("forward_function_reads_const_but_not_called_during_init")));
}

TEST(ModuleInitOrderTest, InterprocConstReadConstDefinedEarlier) {
    EXPECT_FALSE(daHasError(code("interproc_const_read_const_defined_earlier")));
}


TEST(ModuleInitOrderTest, MainAtTopCallsHelpersBelow) {
    EXPECT_FALSE(daHasError(code("main_at_top_calls_helpers_below")));
}
