#ifndef BLAZERULES_AST_H
#define BLAZERULES_AST_H

#include <string>
#include <variant>
#include <vector>

#include "rule_spec.h"
#include "schema.h"

struct NumericCompareNode {
    int column_index = 0;
    ColumnType column_type = ColumnType::FLOAT32;
    OpType op = OpType::GT;
    double threshold = 0.0;
};

struct NumericRangeNode {
    int column_index = 0;
    ColumnType column_type = ColumnType::FLOAT32;
    OpType op = OpType::BETWEEN_INCLUDING;
    double lower = 0.0;
    double upper = 0.0;
};

struct CategoricalCompareNode {
    int column_index = 0;
    OpType op = OpType::IN;
    Textual raw_values;
};

struct WindowCountNode {
    int entity_column_index = 0;
    int window_column_index = 0;
    WindowFn windowFn = WindowFn::COUNT;
    int duration_seconds = 0;
    OpType op = OpType::GT;
    double threshold = 0.0;
};

struct ConditionNode;

struct AndNode { std::vector<ConditionNode> children; };
struct OrNode { std::vector<ConditionNode> children; };
struct NotNode { std::vector<ConditionNode> children; };

struct ConditionNode {
    using Variant = std::variant<
        NumericCompareNode,
        NumericRangeNode,
        CategoricalCompareNode,
        WindowCountNode,
        AndNode,
        OrNode,
        NotNode>;

    Variant node;

    ConditionNode() = default;
    template <class T>
    ConditionNode(T value) : node(std::move(value)) {}
};

struct RuleNode {
    std::string id;
    std::string name;
    std::string version;
    ActionType action = ActionType::FLAG;
    RuleSeverity severity = RuleSeverity::LOW;
    bool enabled = true;
    ConditionNode root;
};

#endif // BLAZERULES_AST_H
