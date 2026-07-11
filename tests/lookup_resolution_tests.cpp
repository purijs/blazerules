#include <gtest/gtest.h>

#include "blazerules/engine.h"

#include <filesystem>
#include <fstream>
#include <string>

// These tests lock in lookup loading and the path-resolution contract: a relative
// `lookups: {path: ...}` in a rules file resolves against the RULE FILE's own directory
// (not the process CWD), and a lookup file that cannot be resolved must fail loudly
// rather than silently disabling the rule. This guards the exact failure mode where a
// ruleset moved into a subdirectory (away from its lookups/) silently stops matching.

namespace {

namespace fs = std::filesystem;

fs::path make_case_dir(const std::string& name) {
    fs::path dir = fs::temp_directory_path() / ("blazerules_lookup_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void write_file(const fs::path& path, const std::string& contents) {
    std::ofstream out(path);
    out << contents;
}

const char* kRulesTemplate = R"YAML(
schema_version: "2.1"
fields:
  merchant: {type: categorical, nullable: false}
  amount: {type: float32, nullable: false}
lookups:
  risky: {type: string_set, path: LOOKUP_PATH}
ruleset:
  name: lookup-resolution
  version: "1.0.0"
  rules:
    - id: risky_merchant
      action: block
      conditions: {field: merchant, op: in_lookup, lookup: risky}
)YAML";

std::string rules_with(const std::string& lookup_path) {
    std::string yaml = kRulesTemplate;
    const std::string token = "LOOKUP_PATH";
    yaml.replace(yaml.find(token), token.size(), lookup_path);
    return yaml;
}

}  // namespace

TEST(LookupResolutionTest, ResolvesRelativeLookupAndMatches) {
    // The lookup CSV sits beside the rules file and is referenced by a bare relative
    // name; resolution must find it relative to the rules file's directory.
    fs::path dir = make_case_dir("relative");
    write_file(dir / "merchants.csv", "value\nM1\nM2\n");
    write_file(dir / "rules.yaml", rules_with("merchants.csv"));

    RuleEngine engine;
    engine.load_rules((dir / "rules.yaml").string());
    BatchResult r = engine.evaluate_ndjson(
        "{\"merchant\":\"M1\",\"amount\":5}\n"   // in the lookup -> blocks
        "{\"merchant\":\"M9\",\"amount\":5}\n");  // not in the lookup -> no match
    EXPECT_EQ(r.n_records, 2);
    EXPECT_EQ(r.n_matched, 1);
    ASSERT_EQ(r.decisions.size(), 2u);
    EXPECT_EQ(r.decisions[0], "BLOCK");
    EXPECT_NE(r.decisions[1], "BLOCK");
}

TEST(LookupResolutionTest, ResolvesLookupInSubdirRelativeToRuleFile) {
    // Lookups in a subdirectory next to the rules file resolve against the rule file's
    // directory, independent of the process working directory.
    fs::path dir = make_case_dir("subdir");
    fs::create_directories(dir / "lookups");
    write_file(dir / "lookups" / "merchants.csv", "value\nM1\n");
    write_file(dir / "rules.yaml", rules_with("lookups/merchants.csv"));

    RuleEngine engine;
    engine.load_rules((dir / "rules.yaml").string());
    BatchResult r = engine.evaluate_ndjson("{\"merchant\":\"M1\",\"amount\":5}\n");
    EXPECT_EQ(r.n_records, 1);
    EXPECT_EQ(r.n_matched, 1);
}

TEST(LookupResolutionTest, MissingLookupFileFailsLoudly) {
    // A lookup path that resolves to a non-existent file must raise, not silently load
    // an empty set (which would make the rule match nothing without warning).
    fs::path dir = make_case_dir("missing");
    write_file(dir / "rules.yaml", rules_with("does_not_exist.csv"));

    RuleEngine engine;
    const std::string rules_path = (dir / "rules.yaml").string();
    EXPECT_ANY_THROW({
        engine.load_rules(rules_path);
        engine.evaluate_ndjson("{\"merchant\":\"M1\",\"amount\":5}\n");
    });
}
