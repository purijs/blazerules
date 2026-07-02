#include <gtest/gtest.h>

#include "blazerules/dsl_parser.h"

#include <string>

// These tests lock in the fail-closed parsing contract: malformed or ambiguous rule
// input must be rejected with a clear error, never silently degraded (e.g. an unknown
// action must NOT fall through to SCORE, an unknown operator must NOT become a no-op),
// and pathologically deep nesting must be rejected rather than overflowing the stack.

namespace {

std::string wrap_rule(const std::string& body) {
    return std::string(
        "ruleset:\n"
        "  name: fail-closed\n"
        "  version: \"1.0.0\"\n"
        "  rules:\n"
        "    - id: r1\n") + body + "\n";
}

}  // namespace

TEST(FailClosedTest, RejectsUnknownAction) {
    // "blok" is a typo for "block"; it must be rejected, not silently treated as SCORE.
    ParseFileResult parsed = parse_rule_string(wrap_rule(
        "      action: blok\n"
        "      conditions: {field: amount, op: gt, value: 10}"));
    ASSERT_FALSE(parsed.ok);
    EXPECT_EQ(parsed.error.code, BlazeRulesError::UNKNOWN_ACTION);
}

TEST(FailClosedTest, AcceptsAllValidActions) {
    for (const char* action : {"flag", "block", "score", "review", "approve"}) {
        ParseFileResult parsed = parse_rule_string(wrap_rule(
            std::string("      action: ") + action + "\n"
            "      conditions: {field: amount, op: gt, value: 10}"));
        ASSERT_TRUE(parsed.ok) << "action '" << action << "' should parse: " << parsed.error.message;
    }
}

TEST(FailClosedTest, RejectsUnknownOperator) {
    ParseFileResult parsed = parse_rule_string(wrap_rule(
        "      action: flag\n"
        "      conditions: {field: amount, op: definitely_not_an_op, value: 10}"));
    ASSERT_FALSE(parsed.ok);
    EXPECT_EQ(parsed.error.code, BlazeRulesError::UNKNOWN_OP);
}

TEST(FailClosedTest, RejectsUnknownOperatorNestedInAnd) {
    // A parse error in ANY child of an and/or/not tree must fail the whole rule —
    // nested errors must not be swallowed into an empty default condition.
    ParseFileResult parsed = parse_rule_string(wrap_rule(
        "      action: flag\n"
        "      conditions:\n"
        "        and:\n"
        "          - {field: amount, op: gt, value: 10}\n"
        "          - {field: amount, op: not_a_real_op, value: 20}"));
    ASSERT_FALSE(parsed.ok);
    EXPECT_EQ(parsed.error.code, BlazeRulesError::UNKNOWN_OP);
}

TEST(FailClosedTest, RejectsMalformedSqlExpression) {
    ParseFileResult parsed = parse_rule_string(wrap_rule(
        "      action: flag\n"
        "      conditions: {sql: \"amount @@ 10\"}"));
    ASSERT_FALSE(parsed.ok);
    EXPECT_EQ(parsed.error.code, BlazeRulesError::UNKNOWN_OP);
}

TEST(FailClosedTest, RejectsDeeplyNestedYamlConditions) {
    // 300 nested `and` levels (flow style) exceeds the parser's depth guard.
    std::string deep = "{field: amount, op: gt, value: 1}";
    for (int i = 0; i < 300; ++i) deep = "{and: [" + deep + "]}";
    ParseFileResult parsed = parse_rule_string(wrap_rule(
        "      action: flag\n"
        "      conditions: " + deep));
    ASSERT_FALSE(parsed.ok);  // rejected, not a stack overflow
}

TEST(FailClosedTest, RejectsDeeplyNestedSqlParens) {
    std::string sql = "amount > 1";
    for (int i = 0; i < 300; ++i) sql = "(" + sql + ")";
    ParseFileResult parsed = parse_rule_string(wrap_rule(
        "      action: flag\n"
        "      conditions: {sql: \"" + sql + "\"}"));
    ASSERT_FALSE(parsed.ok);  // rejected, not a stack overflow
    EXPECT_NE(parsed.error.message.find("deep"), std::string::npos) << parsed.error.message;
}

TEST(FailClosedTest, AcceptsValidNestedRule) {
    // Sanity: the strict checks must not reject legitimate rules.
    ParseFileResult parsed = parse_rule_string(wrap_rule(
        "      action: review\n"
        "      severity: HIGH\n"
        "      conditions:\n"
        "        and:\n"
        "          - {field: amount, op: gt, value: 1000}\n"
        "          - or:\n"
        "              - {field: country_code, op: in, values: [US, GB]}\n"
        "              - {sql: \"amount > 100 AND account_age_days >= 0\"}"));
    ASSERT_TRUE(parsed.ok) << parsed.error.message;
    ASSERT_EQ(parsed.value.rules.size(), 1u);
}
