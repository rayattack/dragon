#include <gtest/gtest.h>

#include <string>

#include "ReplSessionTestSupport.h"

namespace {

class ReplEchoTest : public dragon::test::ReplSessionTest {
protected:
    std::string caseFile() const override { return "ReplEchoTest.md"; }
};

TEST_F(ReplEchoTest, IntExpressionEchoes) {
    EXPECT_EQ("4\n", echoOf("echo_int"));
}

TEST_F(ReplEchoTest, FloatExpressionEchoes) {
    EXPECT_EQ("2.5\n", echoOf("echo_float"));
}

TEST_F(ReplEchoTest, BoolExpressionEchoes) {
    EXPECT_EQ("True\n", echoOf("echo_bool"));
}

TEST_F(ReplEchoTest, StringEchoesQuoted) {
    EXPECT_EQ("'HELLO'\n", echoOf("echo_str"));
}

TEST_F(ReplEchoTest, ExplicitPrintIsNotDoubled) {
    EXPECT_EQ("hi\n", echoOf("echo_print_is_not_doubled"));
}

TEST_F(ReplEchoTest, ListEchoes) {
    EXPECT_EQ("[1, 2, 3]\n", echoOf("echo_list"));
}

TEST_F(ReplEchoTest, NestedDictEchoes) {
    EXPECT_EQ("{'a': [1, 2]}\n", echoOf("echo_nested_dict"));
}

TEST_F(ReplEchoTest, TupleEchoes) {
    EXPECT_EQ("(1, 2)\n", echoOf("echo_tuple"));
}

TEST_F(ReplEchoTest, AssignmentIsSilent) {
    EXPECT_EQ("", echoOf("echo_assignment_is_silent"));
}

TEST_F(ReplEchoTest, NoneIsSilent) {
    EXPECT_EQ("", echoOf("echo_none_is_silent"));
}

TEST_F(ReplEchoTest, DefinitionIsSilent) {
    EXPECT_EQ("", echoOf("echo_def_is_silent"));
}

TEST_F(ReplEchoTest, EveryExpressionEchoesInSourceOrder) {
    EXPECT_EQ("2\nmiddle\n6\n", echoOf("echo_multiple_in_order"));
}

TEST_F(ReplEchoTest, InstanceEchoesThroughStr) {
    EXPECT_EQ("Point(3, 4)\n", echoOf("echo_instance_with_str"));
}

TEST_F(ReplEchoTest, EchoesANameBoundInAnEarlierTurn) {
    EXPECT_EQ("", echoOf("echo_cross_turn_seed"));
    EXPECT_EQ("42\n", echoOf("echo_cross_turn_use"));
}

}
