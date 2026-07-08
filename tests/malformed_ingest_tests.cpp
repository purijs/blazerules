#include <gtest/gtest.h>

#include "blazerules/engine.h"

#include <string>

namespace {

const char* kRules = R"YAML(
schema_version: "2.1"
fields:
  amount: {type: float32, nullable: false}
  country: {type: categorical, nullable: false}
ruleset:
  name: malformed-ingest
  version: "1.0.0"
  rules:
    - id: high_amount
      action: flag
      severity: HIGH
      conditions: {field: amount, op: gt, value: 100}
)YAML";

}  // namespace

TEST(MalformedIngestTest, MidStreamBadLineIsSkippedNotCrash) {
    RuleEngine engine;
    engine.load_rules_from_string(kRules);
    const std::string nd =
        "{\"amount\":150,\"country\":\"US\"}\n"
        "{oops\n"
        "{\"amount\":200,\"country\":\"XX\"}\n";
    BatchResult r = engine.evaluate_ndjson(nd);
    EXPECT_EQ(r.n_records, 2);           // the two valid rows survive
    EXPECT_GE(r.messages_skipped, 1);   // the malformed row is recorded as skipped
    EXPECT_EQ(r.n_matched, 2);          // 150 and 200 both fire high_amount
}

TEST(MalformedIngestTest, LeadingBadLineIsSkippedNotCrash) {
    RuleEngine engine;
    engine.load_rules_from_string(kRules);
    const std::string nd =
        "{bad\n"
        "{\"amount\":200,\"country\":\"XX\"}\n";
    BatchResult r = engine.evaluate_ndjson(nd);
    EXPECT_EQ(r.n_records, 1);
}

TEST(MalformedIngestTest, MultipleConsecutiveBadLines) {
    RuleEngine engine;
    engine.load_rules_from_string(kRules);
    const std::string nd =
        "{\"amount\":150,\"country\":\"US\"}\n"
        "{oops\n"
        "also not json\n"
        "{\"amount\":50,\"country\":\"US\"}\n";
    BatchResult r = engine.evaluate_ndjson(nd);
    EXPECT_EQ(r.n_records, 2);
}

TEST(MalformedIngestTest, BadMessageInListIsSkippedNotCrash) {
    RuleEngine engine;
    engine.load_rules_from_string(kRules);
    std::vector<std::string> messages = {
        "{\"amount\":150,\"country\":\"US\"}",
        "{not valid",
        "{\"amount\":10,\"country\":\"US\"}",
    };
    BatchResult r = engine.evaluate_messages(messages);
    EXPECT_EQ(r.n_records, 2);
}

TEST(MalformedIngestTest, AllValidStillFullyProcessed) {
    RuleEngine engine;
    engine.load_rules_from_string(kRules);
    const std::string nd =
        "{\"amount\":1,\"country\":\"US\"}\n"
        "{\"amount\":2,\"country\":\"US\"}\n"
        "{\"amount\":200,\"country\":\"XX\"}\n";
    BatchResult r = engine.evaluate_ndjson(nd);
    EXPECT_EQ(r.n_records, 3);
    EXPECT_EQ(r.messages_skipped, 0);
    EXPECT_EQ(r.n_matched, 1);
}

TEST(MalformedIngestTest, UnclosedObjectDoesNotSwallowNextLine) {
    RuleEngine engine;
    engine.load_rules_from_string(kRules);
    const std::string nd =
        "{\"amount\":150,\"country\":\"US\"}\n"
        "{\"country\":\n"
        "{\"amount\":200,\"country\":\"XX\"}\n";
    BatchResult r = engine.evaluate_ndjson(nd);
    EXPECT_EQ(r.n_records, 2);
    EXPECT_GE(r.messages_skipped, 1);
    EXPECT_EQ(r.n_matched, 2);
}

TEST(MalformedIngestTest, LeadingUnclosedObjectDoesNotSwallowNextLine) {
    RuleEngine engine;
    engine.load_rules_from_string(kRules);
    const std::string nd =
        "{\"country\":\n"
        "{\"amount\":200,\"country\":\"XX\"}\n";
    BatchResult r = engine.evaluate_ndjson(nd);
    EXPECT_EQ(r.n_records, 1);
    EXPECT_GE(r.messages_skipped, 1);
}

TEST(MalformedIngestTest, MalformedFieldRecordsOffendingColumnAndMessage) {
    RuleEngine engine;
    engine.load_rules_from_string(kRules);
    const std::string nd =
        "{\"country\":\"US\",\"amount\": }\n"
        "{\"amount\":200,\"country\":\"XX\"}\n";
    BatchResult r = engine.evaluate_ndjson(nd);
    EXPECT_EQ(r.n_records, 1);
    ASSERT_FALSE(r.error_samples.empty());
    EXPECT_FALSE(r.error_samples[0].column_name.empty());
    EXPECT_NE(r.error_samples[0].message, r.error_samples[0].code);
    EXPECT_NE(r.error_samples[0].message.find("field"), std::string::npos);
}
