#include <gtest/gtest.h>

#include "blazerules/compiler.h"
#include "blazerules/dsl_parser.h"

#include <variant>

namespace {

BlazeRulesSchema sample_schema() {
    return BlazeRulesSchema({
        {"card_token", ColumnType::ENTITY_KEY, false, true},
        {"amount", ColumnType::FLOAT32, false, false},
        {"amount_limit", ColumnType::FLOAT32, false, false},
        {"country_code", ColumnType::CATEGORICAL, false, false},
        {"device_type", ColumnType::STRING, false, false},
        {"event_ts_ms", ColumnType::TIMESTAMP_MS, false, false},
    });
}

}  // namespace

TEST(ParserCompileTest, ParsesRulesetShapeAndNestedConditions) {
    const char* yaml = R"YAML(
schema_version: "2.1"
fields:
  card_token: {type: entity_key, nullable: false}
  amount: {type: float32, nullable: false}
  amount_limit: {type: float32, nullable: false}
  country_code:
    type: categorical
    values: [US, GB, IN]
  device_type: {type: string}
  event_ts_ms: {type: timestamp_ms}
ruleset:
  name: parser-smoke
  version: "1.0.0"
  rules:
    - id: mixed_rule
      action: review
      severity: HIGH
      conditions:
        and:
          - {field: amount, op: gt, value: 1000}
          - {field: amount, op: gt_field, other_field: amount_limit}
          - or:
              - {field: country_code, op: in, values: [US, GB]}
              - {field: device_type, op: regex, value: "emu.*"}
          - not:
              field: country_code
              op: eq
              value: CN
          - window:
              entity_field: card_token
              function: count
              duration_seconds: 3600
              op: gt
              value: 3
)YAML";

    ParseFileResult parsed = parse_rule_string(yaml);
    ASSERT_TRUE(parsed.ok) << parsed.error.message;
    EXPECT_EQ(parsed.value.name, "parser-smoke");
    ASSERT_EQ(parsed.value.rules.size(), 1u);
    EXPECT_EQ(parsed.value.rules[0].id, "mixed_rule");

    const auto* root = std::get_if<AndConditionSpec>(&parsed.value.rules[0].root_condition.node);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->child_condition.size(), 5u);

    CompileResult compiled = compile_rule_file(parsed.value, sample_schema());
    ASSERT_TRUE(compiled.ok) << compiled.error.message;
    ASSERT_EQ(compiled.value.rules.size(), 1u);
    EXPECT_GE(compiled.value.rules[0].op.size(), 5u);
    ASSERT_EQ(compiled.value.window_channels.size(), 1u);
    EXPECT_EQ(compiled.value.window_channels[0].duration_seconds, 3600);
}

TEST(ParserCompileTest, RejectsUnknownFieldDuringCompile) {
    const char* yaml = R"YAML(
schema_version: "2.1"
ruleset:
  name: invalid-field
  version: "1.0.0"
  rules:
    - id: bad_field
      conditions:
        and:
          - {field: missing_amount, op: gt, value: 10}
)YAML";

    ParseFileResult parsed = parse_rule_string(yaml);
    ASSERT_TRUE(parsed.ok) << parsed.error.message;
    CompileResult compiled = compile_rule_file(parsed.value, sample_schema());
    ASSERT_FALSE(compiled.ok);
    EXPECT_EQ(compiled.error.code, BlazeRulesError::UNKNOWN_FIELD_NAME);
}
