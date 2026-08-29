#include <gtest/gtest.h>

#include <string>

#include "ReplSessionTestSupport.h"

using dragon::ReplOptions;

namespace {

class ReplImportTest : public dragon::test::ReplSessionTest {
protected:
    std::string caseFile() const override { return "ReplImportTest.md"; }

    ReplOptions replOptions() override {
        ReplOptions options;
        options.searchPaths.push_back(DRAGON_STDLIB_DIR);
        return options;
    }
};

TEST_F(ReplImportTest, ATurnAfterAnImportStillRuns) {
    ASSERT_TRUE(runOk("import_sqrt"));
    EXPECT_EQ("2\n", echoOf("plain_sum"));
}

TEST_F(ReplImportTest, AnImportedNameIsCallableInALaterTurn) {
    ASSERT_TRUE(runOk("import_sqrt"));
    EXPECT_EQ("4.0\n", echoOf("call_sqrt"));
}

TEST_F(ReplImportTest, AnImportedModuleIsUsableInALaterTurn) {
    ASSERT_TRUE(runOk("import_math"));
    EXPECT_EQ("4.0\n", echoOf("call_math_sqrt"));
}

TEST_F(ReplImportTest, TwoModulesImportedInSeparateTurns) {
    ASSERT_TRUE(runOk("import_sqrt"));
    ASSERT_TRUE(runOk("import_io_open"));
    EXPECT_EQ("2\n", echoOf("plain_sum"));
}

TEST_F(ReplImportTest, ImportingTheSameModuleTwiceIsHarmless) {
    ASSERT_TRUE(runOk("import_sqrt"));
    ASSERT_TRUE(runOk("import_sqrt"));
    EXPECT_EQ("4.0\n", echoOf("call_sqrt"));
}

TEST_F(ReplImportTest, ResetRecoversASessionThatImported) {
    ASSERT_TRUE(runOk("import_sqrt"));
    std::string refusal;
    ASSERT_TRUE(session->reset(refusal)) << refusal;
    EXPECT_EQ("14\n", echoOf("seven"));
}

TEST_F(ReplImportTest, AGenericInstantiatedInTwoTurns) {
    ASSERT_TRUE(runOk("import_repeat"));
    EXPECT_EQ("[1, 1]\n", echoOf("repeat_one"));
    EXPECT_EQ("[3, 3]\n", echoOf("repeat_three"));
}

TEST_F(ReplImportTest, AGenericDefinedInTheSessionInstantiatedInTwoTurns) {
    ASSERT_TRUE(runOk("define_first"));
    EXPECT_EQ("1\n", echoOf("first_ints"));
    EXPECT_EQ("3\n", echoOf("first_more_ints"));
    EXPECT_EQ("'a'\n", echoOf("first_strs"));
}

TEST_F(ReplImportTest, ResetRecoversASessionThatDefinedAGeneric) {
    ASSERT_TRUE(runOk("define_first"));
    EXPECT_EQ("1\n", echoOf("first_ints"));
    std::string refusal;
    ASSERT_TRUE(session->reset(refusal)) << refusal;
    EXPECT_EQ("14\n", echoOf("seven"));
}

TEST_F(ReplImportTest, AModuleInitializerRunsOnceForTheSession) {
    const std::string first = echoOf("import_this");
    EXPECT_NE(std::string::npos, first.find("The Zen of Dragon"));

    const std::string second = echoOf("plain_sum");
    EXPECT_EQ(std::string::npos, second.find("The Zen of Dragon"));
    EXPECT_EQ("2\n", second);
}

}
