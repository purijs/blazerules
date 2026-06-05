//
// Created by Jaskaran Singh Puri on 26.05.26.
//

#ifndef VDE_RULE_SPEC_H
#define VDE_RULE_SPEC_H

#include <string>
#include <vector>
#include <variant>

enum class OpType {GT, LT, GTE, LTE, EQ, NEQ, IN, NOT_IN, BETWEEN_INCLUDING, BETWEEN_EXCLUDING, INVALID};
enum class WindowFn {COUNT, SUM, MIN, MAX};
enum class ActionType {FLAG, BLOCK, REVIEW, APPROVE};
enum class severity { LOW, MEDIUM, HIGH, CRITICAL };

using Textual = std::variant<std::string, std::vector<std::string>>;

struct NumericConditionSpec {
    std::string field;
    OpType op;
    double threshold;
};

struct CategoricalConditionSpec {
    std::string field;
    OpType op;
    Textual values;
};

struct WindowConditionSpec {
    std::string field;
    WindowFn windowfn;
    int duration_seconds;
    OpType op;
    double threshold;
};

struct NumericRangeConditionSpec {
    std::string field;
    OpType op;
    double upper;
    double lower;
};

using ConditionSpec = std::variant<
    NumericConditionSpec,
    NumericRangeConditionSpec,
    CategoricalConditionSpec,
    WindowConditionSpec,
    struct AndConditionSpec,
    struct OrConditionSpec
>;

struct AndConditionSpec {
    std::vector<ConditionSpec> child_condition;
};

struct OrConditionSpec {
    std::vector<ConditionSpec> child_condition;
};

struct RuleSpec {
    std::string id;
    ActionType action;
    severity severity;
    ConditionSpec root_condition;
};

struct RuleFileSpec {
    std::string name;
    std::string version;
    std::vector<RuleSpec> rules;
};

#endif //VDE_RULE_SPEC_H
