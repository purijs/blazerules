#include <gtest/gtest.h>

#include "blazerules/engine.h"

#include <string>
#include <vector>

namespace {

const char* kCustomLabelRules = R"YAML(
schema_version: "2.1"
fields:
  amount: {type: float32, nullable: false}
decisions:
  default: approve
  precedence: [approve, score, flag, review, bot_block, block]
ruleset:
  name: custom-label
  version: "1.0.0"
  rules:
    - id: bot
      action: block
      label: bot_block
      severity: HIGH
      conditions: {field: amount, op: gt, value: 100}
)YAML";

}  // namespace

TEST(DecisionLabelTest, CustomLabelFlowsToDecisionAndGrouping) {
    RuleEngine engine;
    engine.load_rules_from_string(kCustomLabelRules);
    std::vector<std::string> messages = {
        "{\"amount\":50}",
        "{\"amount\":150}",
    };
    BatchResult r = engine.evaluate_messages(messages);
    ASSERT_EQ(r.n_records, 2);
    EXPECT_EQ(r.decisions[0], "APPROVE");
    EXPECT_EQ(r.decisions[1], "BOT_BLOCK");
    EXPECT_EQ(r.winning_rule_ids[1], "bot");
    EXPECT_EQ(r.grouped_decision_indices.count("BOT_BLOCK"), 1u);
}

TEST(DecisionLabelTest, DefaultActionLabelStaysBuiltIn) {
    const char* rules = R"YAML(
schema_version: "2.1"
fields:
  amount: {type: float32, nullable: false}
ruleset:
  name: builtin-label
  version: "1.0.0"
  rules:
    - id: block_high
      action: block
      severity: HIGH
      conditions: {field: amount, op: gt, value: 100}
)YAML";
    RuleEngine engine;
    engine.load_rules_from_string(rules);
    std::vector<std::string> messages = {"{\"amount\":150}"};
    BatchResult r = engine.evaluate_messages(messages);
    ASSERT_EQ(r.n_records, 1);
    EXPECT_EQ(r.decisions[0], "BLOCK");
}
