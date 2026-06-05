//
// Created by Jaskaran Singh Puri on 27.05.26.
//

#ifndef VDE_KERNEL_SEQUENCE_H
#define VDE_KERNEL_SEQUENCE_H

#include <vector>
#include <map>
#include <tuple>
#include <variant>
#include "rule_spec.h"
#include "ast.h"

using Textual = std::variant<std::string, std::vector<std::string>>;

struct NumericPredicateOp {
    int column_index;
    int output_register;
    ColumnType column_type;
    OpType op_type;
    double threshold;
};

struct NumericRangePredicateOp {
    int column_index;
    int output_register;
    ColumnType column_type;
    OpType op_type;
    double upper;
    double lower;
};

struct CategoricalPredicateOp {
    int column_index;
    OpType op_type;
    Textual raw_values;
    int output_register;
};

struct WindowPredicateOp {
    int window_column_index;
    int output_register;
    OpType op_type;
    double threshold;
};

struct BitwiseAndOp {
    std::vector<int> input_registers;
    int output_register;
};

struct BitwiseOrOp {
    int output_register;
    std::vector<int> input_registers;
};

using KernelOp = std::variant<
    NumericPredicateOp,
    NumericRangePredicateOp,
    CategoricalPredicateOp,
    WindowPredicateOp,
    BitwiseAndOp,
    BitwiseOrOp>;

struct EvalKernelSequence {
    std::string rule_id;
    ActionType action;
    std::string severity;
    std::vector<KernelOp> op;
    int register_count;
    int final_register;
};

struct CompiledRuleSet {
    std::string version;
    std::vector<EvalKernelSequence> rules;
    std::map<std::tuple<std::string, WindowFn, int>, int> window_channel_indices;
};

#endif //VDE_KERNEL_SEQUENCE_H
