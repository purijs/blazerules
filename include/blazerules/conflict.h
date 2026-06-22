#ifndef BLAZERULES_CONFLICT_H
#define BLAZERULES_CONFLICT_H

#include <string>
#include <vector>
#include "rule_spec.h"

struct ConflictReport {
    struct SubsumptionEntry {
        std::string subsumer_rule_id;
        std::string subsumed_rule_id;
        std::string field_name;
        std::string explanation;
    };
    struct ConflictEntry {
        std::string rule_a_id;
        std::string rule_b_id;
        std::string field_name;
        std::string explanation;
    };
    struct DeadRuleEntry {
        std::string rule_id;
        std::string reason;
    };

    std::vector<SubsumptionEntry> subsumptions;
    std::vector<ConflictEntry> conflicts;
    std::vector<DeadRuleEntry> dead_rules;
    int total_rules_analyzed = 0;

    bool has_any_issues() const {
        return !subsumptions.empty() || !conflicts.empty() || !dead_rules.empty();
    }
    std::string to_string() const;
};

ConflictReport analyze_conflicts(const RuleFileSpec& spec);

#endif //BLAZERULES_CONFLICT_H
