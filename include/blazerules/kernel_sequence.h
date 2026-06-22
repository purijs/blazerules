#ifndef BLAZERULES_KERNEL_SEQUENCE_H
#define BLAZERULES_KERNEL_SEQUENCE_H

#include <memory>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <re2/re2.h>

#include "rule_spec.h"
#include "schema.h"

struct NumericPredicateOp {
    int column_index = 0;
    int output_register = 0;
    ColumnType column_type = ColumnType::FLOAT32;
    OpType op_type = OpType::GT;
    double threshold = 0.0;
};

struct NumericRangePredicateOp {
    int column_index = 0;
    int output_register = 0;
    ColumnType column_type = ColumnType::FLOAT32;
    OpType op_type = OpType::BETWEEN_INCLUDING;
    double lower = 0.0;
    double upper = 0.0;
};

struct CategoricalPredicateOp {
    int column_index = 0;
    int output_register = 0;
    OpType op_type = OpType::IN;
    Textual raw_values;
};

struct ArrayLenPredicateOp {
    int column_index = 0;
    int output_register = 0;
    OpType op_type = OpType::ARRAY_LEN_GT;
    int length = 0;
};

struct NullPredicateOp {
    int column_index = 0;
    int output_register = 0;
    OpType op_type = OpType::IS_NULL;
};

struct CrossFieldPredicateOp {
    int left_column_index = 0;
    int right_column_index = 0;
    int output_register = 0;
    ColumnType left_type = ColumnType::FLOAT32;
    ColumnType right_type = ColumnType::FLOAT32;
    OpType op_type = OpType::GT_FIELD;
};

struct BitfieldPredicateOp {
    int column_index = 0;
    int output_register = 0;
    ColumnType column_type = ColumnType::INT64;
    OpType op_type = OpType::FLAGS_ANY;
    uint64_t mask = 0;
};

struct StringPredicateOp {
    int column_index = 0;
    int output_register = 0;
    OpType op_type = OpType::CONTAINS;
    std::string pattern;
    int length = 0;
};

struct RegexPredicateOp {
    int column_index = 0;
    int output_register = 0;
    OpType op_type = OpType::REGEX;
    std::string pattern;
    std::shared_ptr<const RE2> regex;
};

enum class LookupSetType { STRING_SET = 0, INT_SET = 1, IPV4_CIDR_SET = 2 };

struct Ipv4Range {
    uint32_t start = 0;
    uint32_t end = 0;
};

struct CompiledLookupSet {
    std::string name;
    LookupSetType type = LookupSetType::STRING_SET;
    uint64_t generation = 0;
    std::vector<std::string> strings;
    std::vector<int64_t> ints;
    std::vector<Ipv4Range> ipv4_ranges;
};

using LookupRegistry = absl::flat_hash_map<std::string, std::shared_ptr<const CompiledLookupSet>>;

struct LookupPredicateOp {
    int column_index = 0;
    int output_register = 0;
    ColumnType column_type = ColumnType::STRING;
    OpType op_type = OpType::IN_LOOKUP;
    std::string lookup_name;
    std::shared_ptr<const CompiledLookupSet> lookup;
};

struct CidrPredicateOp {
    int column_index = 0;
    int output_register = 0;
    ColumnType column_type = ColumnType::STRING;
    OpType op_type = OpType::IP_IN_SUBNET;
    uint32_t network = 0;
    uint32_t mask = 0;
};

struct TemporalPredicateOp {
    int column_index = 0;
    int output_register = 0;
    OpType op_type = OpType::WITHIN_LAST;
    int64_t value = 0;
    std::vector<int> values;
    double lower = 0.0;
    double upper = 0.0;
};

struct GeoDistancePredicateOp {
    int lat_column_index = 0;
    int lon_column_index = 0;
    int other_lat_column_index = 0;
    int other_lon_column_index = 0;
    int output_register = 0;
    OpType op_type = OpType::DISTANCE_GT;
    double threshold_km = 0.0;
};

enum class ValueExprKind { FIELD = 0, LITERAL = 1, ADD = 2, SUB = 3, MUL = 4, DIV = 5 };

struct ValueExprNode {
    ValueExprKind kind = ValueExprKind::FIELD;
    int column_index = -1;
    ColumnType column_type = ColumnType::FLOAT64;
    double literal = 0.0;
    int left = -1;
    int right = -1;
};

struct ArithmeticPredicateOp {
    std::vector<ValueExprNode> value_nodes;
    int root_value = -1;
    int output_register = 0;
    OpType op_type = OpType::GT;
    double threshold = 0.0;
    int other_column_index = -1;
    ColumnType other_column_type = ColumnType::FLOAT64;
};

struct WindowPredicateOp {
    int window_column_index = 0;
    int output_register = 0;
    ColumnType column_type = ColumnType::INT32;
    OpType op_type = OpType::GT;
    double threshold = 0.0;
};

struct BitwiseAndOp {
    std::vector<int> input_registers;
    int output_register = 0;
};

struct BitwiseOrOp {
    std::vector<int> input_registers;
    int output_register = 0;
};

struct BitwiseNotOp {
    int input_register = 0;
    int output_register = 0;
};

using KernelOp = std::variant<
    NumericPredicateOp,
    NumericRangePredicateOp,
    CategoricalPredicateOp,
    ArrayLenPredicateOp,
    NullPredicateOp,
    CrossFieldPredicateOp,
    BitfieldPredicateOp,
    StringPredicateOp,
    RegexPredicateOp,
    LookupPredicateOp,
    CidrPredicateOp,
    TemporalPredicateOp,
    GeoDistancePredicateOp,
    ArithmeticPredicateOp,
    WindowPredicateOp,
    BitwiseAndOp,
    BitwiseOrOp,
    BitwiseNotOp>;

struct EvalKernelSequence {
    std::string rule_id;
    std::string rule_name;
    std::string rule_version;
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
    bool enabled = true;
    std::vector<KernelOp> op;
    int register_count = 0;
    int final_register = 0;
};

using CompiledRule = EvalKernelSequence;

enum class RuleExprKind {
    PREDICATE,
    AND,
    OR,
    NOT
};

struct RuleExprNode {
    RuleExprKind kind = RuleExprKind::PREDICATE;
    int predicate_index = -1;
    std::vector<int> children;
    std::vector<int> original_children;
};

struct RuleEvalPlan {
    std::vector<RuleExprNode> nodes;
    int root_node = -1;
};

struct GlobalPredicatePlan {
    bool enabled = false;
    std::vector<KernelOp> predicates;
    std::vector<int> predicate_eval_order;
    std::vector<RuleEvalPlan> rule_plans;
};

struct WindowChannelSpec {
    int entity_col_index = 0;
    std::string entity_field;
    WindowFn function = WindowFn::COUNT;
    int sum_col_index = -1;
    std::string sum_field;
    int denominator_col_index = -1;
    std::string denominator_field;
    int duration_seconds = 0;
    int injected_col_index = 0;
    std::string injected_name;
    ColumnType column_type = ColumnType::INT32;
};

// Which family produced a derived (injected) column. Every producer computes a
// per-row scalar once per batch, the value is injected as an extra Arrow column,
// and an ordinary numeric predicate consumes it. New producers add a kind here.
enum class DerivedColumnKind { WINDOW = 0, MODEL_SCORE = 1, VECTOR_DISTANCE = 2, TS_AGG = 3 };

// One appended column. All producers allocate their injected column index from a
// single shared counter (see DerivedColumnPlan) so indices never collide. Slots are
// stored in allocation order, which is exactly injected_col_index order (== the order
// columns are appended to the batch).
struct DerivedColumnSlot {
    DerivedColumnKind kind = DerivedColumnKind::WINDOW;
    int producer_index = 0;   // index into the producing family's own vector (e.g. window_channels)
    int injected_col_index = 0;
    std::string injected_name;
    ColumnType column_type = ColumnType::INT32;  // FLOAT32 or INT32
};

struct DerivedColumnPlan {
    std::vector<DerivedColumnSlot> slots;
    int count() const { return static_cast<int>(slots.size()); }
};

// One embedded-ML score column. feature_col_indices are resolved ingest column indices
// (in the model's feature order); the score is injected at injected_col_index and read
// by a NumericPredicateOp. The model itself is looked up by name from the engine's
// ModelRegistry at score time (decoupling model registration from rule compilation).
struct ModelChannelSpec {
    std::string model_name;
    std::vector<int> feature_col_indices;
    std::vector<ColumnType> feature_types;
    int output_index = 0;
    int injected_col_index = 0;
    std::string injected_name;
    ColumnType column_type = ColumnType::FLOAT32;
};

// One vector-similarity channel: an embedding gathered from dim_col_indices is compared
// (cosine/L2/dot) against a constant reference; the scalar result is injected and read
// by a NumericPredicateOp. metric: 0=COSINE, 1=L2, 2=DOT.
struct VectorChannelSpec {
    std::vector<int> dim_col_indices;
    std::vector<float> reference;
    float reference_inv_norm = 0.0f;   // 1/||reference|| precomputed for cosine
    int metric = 0;
    int injected_col_index = 0;
    std::string injected_name;
    ColumnType column_type = ColumnType::FLOAT32;
};

struct ArrayAnyChannelSpec {
    std::string path;
    ConditionSpec where;
    int synthetic_col_index = -1;
    std::string synthetic_name;
};

struct CompiledRuleSet {
    std::string name;
    std::string version;
    std::string loaded_at;
    std::string default_decision = "APPROVE";
    std::vector<std::string> decision_labels;
    absl::flat_hash_map<std::string, int> decision_label_to_code;
    std::vector<int> decision_ranks;
    std::vector<EvalKernelSequence> rules;
    GlobalPredicatePlan global_plan;
    std::vector<WindowChannelSpec> window_channels;
    std::vector<ModelChannelSpec> model_channels;
    std::vector<VectorChannelSpec> vector_channels;
    std::vector<ArrayAnyChannelSpec> array_any_channels;
    DerivedColumnPlan derived_plan;
    LookupRegistry lookups;
    absl::flat_hash_map<std::string, int> rule_id_to_index;
    uint64_t dict_generation = 0;
};

#endif // BLAZERULES_KERNEL_SEQUENCE_H
