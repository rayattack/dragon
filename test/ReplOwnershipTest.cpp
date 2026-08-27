#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "ReplSessionTestSupport.h"

using dragon::TurnStatus;
using dragon::test::describeTurn;

namespace {

int churnTurns() {
    if (const char* override = std::getenv("DRAGON_REPL_CHURN_TURNS"))
        return std::atoi(override);
    return 400;
}

const int kChurnTurns = churnTurns();
constexpr int kResetCycles = 40;

class ReplOwnershipTest : public dragon::test::ReplSessionTest {
protected:
    std::string caseFile() const override { return "ReplOwnershipTest.md"; }

    [[nodiscard]] ::testing::AssertionResult churn(const std::string& seed,
                                                   const std::string& step) {
        if (auto seeded = runOk(seed); !seeded) return seeded;
        return repeat(step, kChurnTurns);
    }
};

TEST_F(ReplOwnershipTest, ListRebindChurnIsBalanced) {
    ASSERT_TRUE(churn("rebind_churn_seed", "rebind_churn_step"));
}

TEST_F(ReplOwnershipTest, StringRebindChurnIsBalanced) {
    ASSERT_TRUE(churn("str_rebind_churn_seed", "str_rebind_churn_step"));
}

TEST_F(ReplOwnershipTest, UnionRebindChurnIsBalanced) {
    ASSERT_TRUE(churn("union_rebind_churn_seed", "union_rebind_churn_step"));
}

TEST_F(ReplOwnershipTest, ContainerGlobalMutatedAcrossTurns) {
    ASSERT_TRUE(runOk("container_mutate_seed"));
    ASSERT_TRUE(repeat("container_mutate_step", kChurnTurns));
    ASSERT_TRUE(runOk("container_mutate_read"));
}

TEST_F(ReplOwnershipTest, ClassInstanceGlobalRebindIsBalanced) {
    ASSERT_TRUE(runOk("class_instance_seed"));
    ASSERT_TRUE(repeat("class_instance_step", kChurnTurns));
    ASSERT_TRUE(runOk("class_instance_read"));
}

TEST_F(ReplOwnershipTest, RaiseMidAssignmentRollsBackAndNameIsReusable) {
    ASSERT_TRUE(runOk("raise_mid_assign_helper"));

    auto raised = run("raise_mid_assign");
    ASSERT_EQ(TurnStatus::Raised, raised.status) << describeTurn(raised);
    EXPECT_NE(raised.exceptionText.find("ValueError"), std::string::npos)
        << raised.exceptionText;

    ASSERT_TRUE(runOk("raise_mid_assign_rebind"));
    ASSERT_TRUE(runOk("raise_mid_assign_read"));
}

TEST_F(ReplOwnershipTest, FailedTurnLeavesTheSessionUnchanged) {
    ASSERT_TRUE(runOk("raise_mid_assign_helper"));
    const int before = session->committedTurnCount();

    auto raised = run("raise_mid_assign");
    ASSERT_EQ(TurnStatus::Raised, raised.status) << describeTurn(raised);

    EXPECT_EQ(before, session->committedTurnCount())
        << "a failed turn must not commit";
}

TEST_F(ReplOwnershipTest, UncaughtRaiseReleasesSkippedFrameTemporaries) {
    ASSERT_TRUE(runOk("uncaught_raise_helper"));
    const std::string step = cell("uncaught_raise_step");
    for (int i = 0; i < kChurnTurns; ++i) {
        auto result = session->evaluate(step);
        ASSERT_EQ(TurnStatus::Raised, result.status)
            << "iteration " << i << ": " << describeTurn(result);
    }
}

TEST_F(ReplOwnershipTest, GlobalCapturedByAFiredVThread) {
    ASSERT_TRUE(runOk("fired_vthread_seed"));
    ASSERT_TRUE(runOk("fired_vthread_step"));
}

TEST_F(ReplOwnershipTest, ResetReleasesWhatGlobalsHold) {
    static const char* kCases[] = {
        "reset_cycle_list", "reset_cycle_str", "reset_cycle_union",
        "reset_cycle_instance", "reset_cycle_nested",
    };
    for (const char* block : kCases) {
        for (int cycle = 0; cycle < kResetCycles; ++cycle) {
            ASSERT_TRUE(runOk(block)) << "cycle " << cycle;
            std::string refusal;
            ASSERT_TRUE(session->reset(refusal))
                << block << " cycle " << cycle << ": " << refusal;
            ASSERT_EQ(0, session->committedTurnCount());
        }
    }
}

TEST_F(ReplOwnershipTest, FunctionDefinedInOneTurnIsCallableInAnother) {
    ASSERT_TRUE(runOk("cross_turn_def"));
    ASSERT_TRUE(runOk("cross_turn_use"));
}

TEST_F(ReplOwnershipTest, ResetIsRefusedOrClearsCleanly) {
    ASSERT_TRUE(runOk("rebind_churn_seed"));
    std::string refusal;
    if (session->reset(refusal)) {
        EXPECT_EQ(0, session->committedTurnCount());
    } else {
        EXPECT_FALSE(refusal.empty()) << "a refused reset must say why";
    }
}

}
