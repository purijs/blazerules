//
// Created by Jaskaran Singh Puri on 29.05.26.
//

#ifndef VDE_RESULT_H
#define VDE_RESULT_H

#include <string>
#include <vector>
#include "rule_spec.h"
#include "kernel_sequence.h"

enum class ErrorCode {
    YAML_PARSE_FAILED,
    MISSING_FIELD,
    UNKNOWN_OP,
    UNKNOWN_FIELD,
    TYPE_MISMATCH,
    UNKNOWN_ACTION,
    UNKNOWN_SEVERITY,
};

struct VdeError {
    ErrorCode code;
    std::string message;
    std::string rule_id;
};

struct ParseFileResult {
    bool ok;
    RuleFileSpec value;
    VdeError error;
};

struct CompileResult {
    bool ok;
    CompiledRuleSet value;
    VdeError error;
};

struct ParseRuleResult {
    bool ok;
    RuleSpec value;
    VdeError error;
};

struct ParseConditionResult {
    bool ok;
    ConditionSpec value;
    VdeError error;
};

#endif //VDE_RESULT_H
