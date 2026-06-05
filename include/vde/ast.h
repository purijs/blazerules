//
// Created by Jaskaran Singh Puri on 26.05.26.
//

#ifndef VDE_AST_H
#define VDE_AST_H

#include <vector>
#include <variant>
#include "rule_spec.h"

enum class ColumnType {FLOAT32, FLOAT64, INT32, INT64};
using Textual = std::variant<std::string, std::vector<std::string>>;

struct NumericCompareNode {
    int column_index;
    ColumnType column_type;
    OpType op;
    double threshold;
};

struct NumericRangeNode {
    int column_index;
    ColumnType column_type;
    OpType op;
    double upper;
    double lower;
};

struct CategoricalCompareNode {
    int column_index;
    OpType op;
    Textual raw_values;
};

struct WindowCountNode {
    int entity_column_index;
    int window_column_index;
    WindowFn windowFn;
    int duration_seconds;
    OpType op;
    double threshold;
};

using ConditionNode = std::variant<
    NumericCompareNode,
    NumericRangeNode,
    CategoricalCompareNode,
    WindowCountNode,
    struct AndNode,
    struct OrNode
>;

struct AndNode {std::vector<ConditionNode> child_condition;};
struct OrNode {std::vector<ConditionNode> child_condition;};

#endif //VDE_AST_H
