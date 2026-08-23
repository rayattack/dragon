#include "TestHelpers.h"
#include "dragon/DefiniteAssignment.h"
#include <gtest/gtest.h>
#include "CodeBlock.h"

using namespace dragon;
using namespace dragon::test;

static std::string code(const std::string& block) {
    return extractCode("DefiniteAssignmentTest.md", block);
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

TEST(DefiniteAssignmentTest, ReadUnassignedLocalErrors) {
    EXPECT_TRUE(daHasError("x: int\nprint(x)\n"));
}

TEST(DefiniteAssignmentTest, AssignedBeforeReadOk) {
    EXPECT_FALSE(daHasError("x: int = 5\nprint(x)\n"));
    EXPECT_FALSE(daHasError("x: int\nx = 5\nprint(x)\n"));
}

TEST(DefiniteAssignmentTest, AssignedInBothBranchesOk) {
    EXPECT_FALSE(daHasError(code("assigned_in_both_branches_ok")));
}

TEST(DefiniteAssignmentTest, AssignedInOnlyOneBranchErrors) {
    EXPECT_TRUE(daHasError(code("assigned_in_only_one_branch_errors")));
}

TEST(DefiniteAssignmentTest, ElseReturnsGuardOk) {
    EXPECT_FALSE(daHasError(code("else_returns_guard_ok")));
}

TEST(DefiniteAssignmentTest, WhileTrueBreakAssignsOk) {
    EXPECT_FALSE(daHasError(code("while_true_break_assigns_ok")));
}

TEST(DefiniteAssignmentTest, GeneralWhileMayNotRunErrors) {
    EXPECT_TRUE(daHasError(code("general_while_may_not_run_errors")));
}

TEST(DefiniteAssignmentTest, AugAssignOnUnassignedErrors) {
    EXPECT_TRUE(daHasError(code("aug_assign_on_unassigned_errors")));
}

TEST(DefiniteAssignmentTest, ParameterIsAssignedOk) {
    EXPECT_FALSE(daHasError(code("parameter_is_assigned_ok")));
}

TEST(DefiniteAssignmentTest, GlobalDeclaredNameOk) {
    EXPECT_FALSE(daHasError(code("global_declared_name_ok")));
}

TEST(DefiniteAssignmentTest, CtorForgetsFieldErrors) {
    EXPECT_TRUE(daHasError(code("ctor_forgets_field_errors")));
}

TEST(DefiniteAssignmentTest, CtorAssignsAllFieldsOk) {
    EXPECT_FALSE(daHasError(code("ctor_assigns_all_fields_ok")));
}

TEST(DefiniteAssignmentTest, FieldWithDefaultOk) {
    EXPECT_FALSE(daHasError(code("field_with_default_ok")));
}

TEST(DefiniteAssignmentTest, FieldAssignedInOneCtorBranchErrors) {
    EXPECT_TRUE(daHasError(code("field_assigned_in_one_ctor_branch_errors")));
}

TEST(DefiniteAssignmentTest, DeferredInitViaMethodOk) {
    EXPECT_FALSE(daHasError(code("deferred_init_via_method_ok")));
}

TEST(DefiniteAssignmentTest, CtorAssignsViaSelfHelperOk) {
    EXPECT_FALSE(daHasError(code("ctor_assigns_via_self_helper_ok")));
}

TEST(DefiniteAssignmentTest, StaticFieldNotRequiredOk) {
    EXPECT_FALSE(daHasError(code("static_field_not_required_ok")));
}

TEST(DefiniteAssignmentTest, EarlyRaiseBeforeAssignOk) {
    EXPECT_FALSE(daHasError(code("early_raise_before_assign_ok")));
}
