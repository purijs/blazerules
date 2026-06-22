#ifndef BLAZERULES_RULE_SPEC_H
#define BLAZERULES_RULE_SPEC_H

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <re2/re2.h>

#include "schema.h"

enum class OpType {
    GT = 0,
    LT = 1,
    GTE = 2,
    LTE = 3,
    EQ = 4,
    NEQ = 5,
    IN = 6,
    NOT_IN = 7,
    BETWEEN_INCLUDING = 8,
    BETWEEN_EXCLUDING = 9,
    IS_NULL = 10,
    IS_NOT_NULL = 11,
    IS_EMPTY = 12,
    IS_NOT_EMPTY = 13,
    GT_FIELD = 14,
    LT_FIELD = 15,
    GTE_FIELD = 16,
    LTE_FIELD = 17,
    EQ_FIELD = 18,
    NEQ_FIELD = 19,
    CONTAINS_ANY = 20,
    CONTAINS_ALL = 21,
    INTERSECTS = 22,
    NOT_INTERSECTS = 23,
    ARRAY_LEN_GT = 24,
    ARRAY_LEN_LT = 25,
    ARRAY_LEN_EQ = 26,
    FLAGS_ANY = 27,
    FLAGS_ALL = 28,
    FLAGS_NONE = 29,
    CONTAINS = 30,
    STARTS_WITH = 31,
    ENDS_WITH = 32,
    CI_EQ = 33,
    LENGTH_GT = 34,
    LENGTH_LT = 35,
    LENGTH_EQ = 36,
    IP_IN_SUBNET = 37,
    IP_NOT_IN_SUBNET = 38,
    BEFORE = 39,
    AFTER = 40,
    WITHIN_LAST = 41,
    DAY_OF_WEEK_IN = 42,
    TIME_OF_DAY_BETWEEN = 43,
    DISTANCE_GT = 44,
    DISTANCE_LT = 45,
    REGEX = 46,
    NOT_REGEX = 47,
    IN_LOOKUP = 48,
    NOT_IN_LOOKUP = 49,
    INVALID = 50
};

enum class WindowFn { COUNT = 0, SUM = 1, MIN = 2, MAX = 3, AVG = 4, RATIO = 5 };
enum class ActionType { FLAG = 0, BLOCK = 1, SCORE = 2, REVIEW = 3, APPROVE = 4 };
enum class RuleSeverity { HIGH = 0, MEDIUM = 1, LOW = 2, CRITICAL = 3 };

using severity = RuleSeverity;
using RuleAction = ActionType;
using Textual = std::variant<std::string, std::vector<std::string>>;

struct NumericConditionSpec {
    std::string field;
    OpType op = OpType::INVALID;
    double threshold = 0.0;
};

struct NumericRangeConditionSpec {
    std::string field;
    OpType op = OpType::BETWEEN_INCLUDING;
    double lower = 0.0;
    double upper = 0.0;
};

struct CategoricalConditionSpec {
    std::string field;
    OpType op = OpType::INVALID;
    Textual values;
};

struct ArrayLenConditionSpec {
    std::string field;
    OpType op = OpType::ARRAY_LEN_GT;
    int length = 0;
};

struct NullConditionSpec {
    std::string field;
    OpType op = OpType::IS_NULL;
};

struct CrossFieldConditionSpec {
    std::string field;
    std::string other_field;
    OpType op = OpType::INVALID;
};

struct BitfieldConditionSpec {
    std::string field;
    OpType op = OpType::FLAGS_ANY;
    uint64_t mask = 0;
};

struct StringConditionSpec {
    std::string field;
    OpType op = OpType::CONTAINS;
    std::string value;
    int length = 0;
};

struct RegexConditionSpec {
    std::string field;
    OpType op = OpType::REGEX;
    std::string pattern;
    std::shared_ptr<const RE2> compiled;
};

struct LookupConditionSpec {
    std::string field;
    OpType op = OpType::IN_LOOKUP;
    std::string lookup_name;
};

struct CidrConditionSpec {
    std::string field;
    OpType op = OpType::IP_IN_SUBNET;
    std::string cidr;
    uint32_t network = 0;
    uint32_t mask = 0;
    bool compiled = false;
};

struct TemporalConditionSpec {
    std::string field;
    OpType op = OpType::WITHIN_LAST;
    int64_t value = 0;
    std::vector<int> values;
    double lower = 0.0;
    double upper = 0.0;
};

struct GeoDistanceConditionSpec {
    std::string lat_field;
    std::string lon_field;
    std::string other_lat_field;
    std::string other_lon_field;
    OpType op = OpType::DISTANCE_GT;
    double threshold_km = 0.0;
};

enum class ArithmeticExprKind { FIELD = 0, LITERAL = 1, ADD = 2, SUB = 3, MUL = 4, DIV = 5 };

struct ArithmeticExprSpec {
    ArithmeticExprKind kind = ArithmeticExprKind::FIELD;
    std::string field;
    double literal = 0.0;
    std::vector<ArithmeticExprSpec> children;
};

struct ArithmeticConditionSpec {
    ArithmeticExprSpec expr;
    OpType op = OpType::GT;
    double threshold = 0.0;
    std::string other_field;
};

struct WindowConditionSpec {
    std::string field;       // entity_field in YAML
    WindowFn windowfn = WindowFn::COUNT;
    std::string sum_field;   // SUM/AVG or RATIO numerator
    std::string denominator_field; // only used for RATIO
    int duration_seconds = 0;
    OpType op = OpType::INVALID;
    double threshold = 0.0;
};

// Embedded ML model score compared against a threshold. The model maps `features`
// (ordered numeric field names) to a scalar score, computed once per batch as a
// derived column; a numeric op then compares it. op is GT/GTE/LT/LTE/EQ/NEQ (uses
// threshold) or BETWEEN_INCLUDING/EXCLUDING (uses lower/upper).
struct ModelScoreConditionSpec {
    std::string model_name;
    std::vector<std::string> features;
    int output_index = 0;          // which output of a multi-output model
    OpType op = OpType::GT;
    double threshold = 0.0;
    double lower = 0.0;
    double upper = 0.0;
};

enum class VectorMetric { COSINE = 0, L2 = 1, DOT = 2 };

// Vector-similarity of a per-row embedding (given as `dims` numeric fields, in order)
// against a constant `reference` vector, compared to a threshold. Computed once per
// batch as an injected derived column (NEON kernels in simd_vec.h), then a numeric op
// compares it. For COSINE the injected value is similarity; for L2 it is the distance;
// for DOT it is the dot product.
struct VectorDistanceConditionSpec {
    std::vector<std::string> dims;   // embedding component fields, in order
    VectorMetric metric = VectorMetric::COSINE;
    std::vector<float> reference;    // constant reference vector (len == dims.size())
    OpType op = OpType::GT;
    double threshold = 0.0;
    double lower = 0.0;
    double upper = 0.0;
};

struct ConditionSpec;

struct ArrayAnyConditionSpec {
    std::string path;
    std::shared_ptr<ConditionSpec> where;
    std::string synthetic_field;
};

struct AndConditionSpec {
    std::vector<ConditionSpec> child_condition;
};

struct OrConditionSpec {
    std::vector<ConditionSpec> child_condition;
};

struct NotConditionSpec {
    std::vector<ConditionSpec> child_condition; // exactly one child in valid rules
};

struct ConditionSpec {
    using Variant = std::variant<
        NumericConditionSpec,
        NumericRangeConditionSpec,
        CategoricalConditionSpec,
        ArrayLenConditionSpec,
        NullConditionSpec,
        CrossFieldConditionSpec,
        BitfieldConditionSpec,
        StringConditionSpec,
        RegexConditionSpec,
        LookupConditionSpec,
        CidrConditionSpec,
        TemporalConditionSpec,
        GeoDistanceConditionSpec,
        ArithmeticConditionSpec,
        WindowConditionSpec,
        ModelScoreConditionSpec,
        VectorDistanceConditionSpec,
        ArrayAnyConditionSpec,
        AndConditionSpec,
        OrConditionSpec,
        NotConditionSpec>;

    Variant node;

    ConditionSpec() = default;
    template <class T>
    ConditionSpec(T value) : node(std::move(value)) {}
};

struct RuleSpec {
    std::string id;
    std::string name;
    std::string version;
    bool enabled = true;
    ActionType action = ActionType::FLAG;
    std::string action_label = "FLAG";
    int action_code = 1;
    int action_rank = 2;
    RuleSeverity severity = RuleSeverity::LOW;
    int priority = 0;
    double weight = 0.0;
    double score_cap = 0.0;
    std::string reason_code;
    bool shadow = false;
    ConditionSpec root_condition;
};

struct LookupSpec {
    std::string name;
    std::string type;
    std::string path;
};

struct FieldHintSpec {
    std::string name;
    bool has_type = false;
    ColumnType type = ColumnType::STRING;
    bool has_nullable = false;
    bool nullable = true;
    bool is_entity_field = false;
    std::vector<std::string> values;
};

struct RuleFileSpec {
    std::string name;
    std::string version;
    std::string schema_version = "1.0";
    std::string base_dir = ".";
    std::string default_decision = "APPROVE";
    std::vector<std::string> decision_precedence;
    std::vector<FieldHintSpec> field_hints;
    std::vector<LookupSpec> lookups;
    std::vector<RuleSpec> rules;
};

#endif // BLAZERULES_RULE_SPEC_H
