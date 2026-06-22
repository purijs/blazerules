#ifndef BLAZERULES_RESULT_H
#define BLAZERULES_RESULT_H

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "kernel_sequence.h"
#include "rule_spec.h"

struct BlazeRulesError {
    enum class Domain {
        CONFIG,
        RULE_PARSE,
        RULE_VALIDATION,
        SCHEMA,
        LOOKUP,
        INGEST,
        ARROW,
        PARQUET,
        WINDOW_STATE,
        INTERNAL
    };

    enum Code {
        YAML_SYNTAX_ERROR,
        MISSING_REQUIRED_FIELD,
        UNKNOWN_FIELD_NAME,
        TYPE_MISMATCH,
        DUPLICATE_RULE_ID,
        SCHEMA_MISMATCH,
        INCOMPATIBLE_PARQUET_SCHEMA,
        MALFORMED_JSON,
        JSON_CAPACITY_EXCEEDED,
        FIELD_TYPE_COERCION_FAILED,
        ARENA_EXHAUSTED,
        KERNEL_COLUMN_INDEX_OOB,
        INVALID_ENGINE_CONFIG,
        UNKNOWN_OP,
        UNKNOWN_ACTION,
        UNKNOWN_SEVERITY,
        FILE_IO_ERROR,
        HOT_RELOAD_FAILED,
        DEAD_LETTER_WRITE_FAILED,
        INTERNAL_ERROR,
    };

    Code code = YAML_SYNTAX_ERROR;
    std::string message;
    std::string source;
    std::string rule_id;
    int line = -1;
    Domain domain = Domain::INTERNAL;
    std::string field;
    int64_t row_index = -1;
    std::string column_name;
    std::string cause;
};

inline const char* blazerules_error_code_name(BlazeRulesError::Code code) {
    switch (code) {
        case BlazeRulesError::YAML_SYNTAX_ERROR: return "YAML_SYNTAX_ERROR";
        case BlazeRulesError::MISSING_REQUIRED_FIELD: return "MISSING_REQUIRED_FIELD";
        case BlazeRulesError::UNKNOWN_FIELD_NAME: return "UNKNOWN_FIELD_NAME";
        case BlazeRulesError::TYPE_MISMATCH: return "TYPE_MISMATCH";
        case BlazeRulesError::DUPLICATE_RULE_ID: return "DUPLICATE_RULE_ID";
        case BlazeRulesError::SCHEMA_MISMATCH: return "SCHEMA_MISMATCH";
        case BlazeRulesError::INCOMPATIBLE_PARQUET_SCHEMA: return "INCOMPATIBLE_PARQUET_SCHEMA";
        case BlazeRulesError::MALFORMED_JSON: return "MALFORMED_JSON";
        case BlazeRulesError::JSON_CAPACITY_EXCEEDED: return "JSON_CAPACITY_EXCEEDED";
        case BlazeRulesError::FIELD_TYPE_COERCION_FAILED: return "FIELD_TYPE_COERCION_FAILED";
        case BlazeRulesError::ARENA_EXHAUSTED: return "ARENA_EXHAUSTED";
        case BlazeRulesError::KERNEL_COLUMN_INDEX_OOB: return "KERNEL_COLUMN_INDEX_OOB";
        case BlazeRulesError::INVALID_ENGINE_CONFIG: return "INVALID_ENGINE_CONFIG";
        case BlazeRulesError::UNKNOWN_OP: return "UNKNOWN_OP";
        case BlazeRulesError::UNKNOWN_ACTION: return "UNKNOWN_ACTION";
        case BlazeRulesError::UNKNOWN_SEVERITY: return "UNKNOWN_SEVERITY";
        case BlazeRulesError::FILE_IO_ERROR: return "FILE_IO_ERROR";
        case BlazeRulesError::HOT_RELOAD_FAILED: return "HOT_RELOAD_FAILED";
        case BlazeRulesError::DEAD_LETTER_WRITE_FAILED: return "DEAD_LETTER_WRITE_FAILED";
        case BlazeRulesError::INTERNAL_ERROR: return "INTERNAL_ERROR";
    }
    return "UNKNOWN";
}

inline const char* blazerules_error_domain_name(BlazeRulesError::Domain domain) {
    switch (domain) {
        case BlazeRulesError::Domain::CONFIG: return "CONFIG";
        case BlazeRulesError::Domain::RULE_PARSE: return "RULE_PARSE";
        case BlazeRulesError::Domain::RULE_VALIDATION: return "RULE_VALIDATION";
        case BlazeRulesError::Domain::SCHEMA: return "SCHEMA";
        case BlazeRulesError::Domain::LOOKUP: return "LOOKUP";
        case BlazeRulesError::Domain::INGEST: return "INGEST";
        case BlazeRulesError::Domain::ARROW: return "ARROW";
        case BlazeRulesError::Domain::PARQUET: return "PARQUET";
        case BlazeRulesError::Domain::WINDOW_STATE: return "WINDOW_STATE";
        case BlazeRulesError::Domain::INTERNAL: return "INTERNAL";
    }
    return "INTERNAL";
}

class BlazeRulesException : public std::runtime_error {
public:
    explicit BlazeRulesException(BlazeRulesError error)
        : std::runtime_error(error.message), error_(std::move(error)) {}
    const BlazeRulesError& error() const { return error_; }
private:
    BlazeRulesError error_;
};

class BlazeRulesConfigError : public BlazeRulesException { using BlazeRulesException::BlazeRulesException; };
class BlazeRulesParseError : public BlazeRulesException { using BlazeRulesException::BlazeRulesException; };
class BlazeRulesSchemaError : public BlazeRulesException { using BlazeRulesException::BlazeRulesException; };

template <typename T>
class BlazeRulesResult {
public:
    static BlazeRulesResult ok(T value) { return BlazeRulesResult(std::move(value)); }
    static BlazeRulesResult err(BlazeRulesError error) { return BlazeRulesResult(std::move(error)); }

    bool is_ok() const { return std::holds_alternative<T>(storage_); }
    bool is_error() const { return !is_ok(); }
    T& value() { return std::get<T>(storage_); }
    const T& value() const { return std::get<T>(storage_); }
    const BlazeRulesError& error() const { return std::get<BlazeRulesError>(storage_); }
    T value_or(T default_value) const { return is_ok() ? value() : std::move(default_value); }

    template <typename F>
    auto and_then(F&& func) const -> decltype(func(value())) {
        if (is_error()) {
            return decltype(func(value()))::err(error());
        }
        return func(value());
    }

private:
    explicit BlazeRulesResult(T value) : storage_(std::move(value)) {}
    explicit BlazeRulesResult(BlazeRulesError error) : storage_(std::move(error)) {}

    std::variant<T, BlazeRulesError> storage_;
};

template <>
class BlazeRulesResult<void> {
public:
    static BlazeRulesResult ok() { return BlazeRulesResult(true, {}); }
    static BlazeRulesResult err(BlazeRulesError error) { return BlazeRulesResult(false, std::move(error)); }

    bool is_ok() const { return ok_; }
    bool is_error() const { return !ok_; }
    void value() const {}
    const BlazeRulesError& error() const { return error_; }

private:
    BlazeRulesResult(bool ok, BlazeRulesError error) : ok_(ok), error_(std::move(error)) {}
    bool ok_;
    BlazeRulesError error_;
};

struct ParseFileResult {
    bool ok = false;
    RuleFileSpec value;
    BlazeRulesError error;
};

struct CompileResult {
    bool ok = false;
    CompiledRuleSet value;
    BlazeRulesError error;
    std::vector<BlazeRulesError> diagnostics;
};

struct ParseRuleResult {
    bool ok = false;
    RuleSpec value;
    BlazeRulesError error;
};

struct ParseConditionResult {
    bool ok = false;
    ConditionSpec value;
    BlazeRulesError error;
};

#endif // BLAZERULES_RESULT_H
