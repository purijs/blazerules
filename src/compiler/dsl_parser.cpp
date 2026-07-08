#include "blazerules/dsl_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

#include "blazerules/resource_resolver.h"
#include "blazerules/sql_expr_parser.h"

namespace {

std::string lower(std::string_view input) {
    std::string result(input);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string upper(std::string_view input) {
    std::string result(input);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

bool action_from_string(const std::string& s, ActionType& out) {
    std::string v = lower(s);
    if (v == "flag") { out = ActionType::FLAG; return true; }
    if (v == "block") { out = ActionType::BLOCK; return true; }
    if (v == "score") { out = ActionType::SCORE; return true; }
    if (v == "review") { out = ActionType::REVIEW; return true; }
    if (v == "approve") { out = ActionType::APPROVE; return true; }
    return false;
}

std::string decision_label(std::string_view input) {
    std::string out = upper(input);
    for (char& c : out) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) {
            c = '_';
        }
    }
    return out.empty() ? "FLAG" : out;
}

bool severity_from_string(const std::string& s, RuleSeverity& out) {
    std::string v = upper(s);
    if (v == "HIGH") { out = RuleSeverity::HIGH; return true; }
    if (v == "MEDIUM") { out = RuleSeverity::MEDIUM; return true; }
    if (v == "LOW") { out = RuleSeverity::LOW; return true; }
    if (v == "CRITICAL") { out = RuleSeverity::CRITICAL; return true; }
    return false;
}

bool column_type_from_string(const std::string& s, ColumnType& out) {
    std::string v = lower(s);
    if (v == "float32" || v == "f32" || v == "float") { out = ColumnType::FLOAT32; return true; }
    if (v == "float64" || v == "f64" || v == "double") { out = ColumnType::FLOAT64; return true; }
    if (v == "int32" || v == "i32" || v == "integer") { out = ColumnType::INT32; return true; }
    if (v == "int64" || v == "i64" || v == "long") { out = ColumnType::INT64; return true; }
    if (v == "categorical" || v == "category" || v == "enum") { out = ColumnType::CATEGORICAL; return true; }
    if (v == "entity_key" || v == "entity" || v == "key") { out = ColumnType::ENTITY_KEY; return true; }
    if (v == "timestamp_ms" || v == "timestamp" || v == "datetime") { out = ColumnType::TIMESTAMP_MS; return true; }
    if (v == "bool" || v == "boolean") { out = ColumnType::BOOLEAN; return true; }
    if (v == "string" || v == "text") { out = ColumnType::STRING; return true; }
    return false;
}

OpType op_from_string(const std::string& s) {
    std::string v = lower(s);
    if (v == "gt") return OpType::GT;
    if (v == "lt") return OpType::LT;
    if (v == "gte") return OpType::GTE;
    if (v == "lte") return OpType::LTE;
    if (v == "eq") return OpType::EQ;
    if (v == "neq") return OpType::NEQ;
    if (v == "in") return OpType::IN;
    if (v == "not_in") return OpType::NOT_IN;
    if (v == "between_including") return OpType::BETWEEN_INCLUDING;
    if (v == "between_excluding") return OpType::BETWEEN_EXCLUDING;
    if (v == "is_null" || v == "is_none") return OpType::IS_NULL;
    if (v == "is_not_null" || v == "not_null" || v == "is_not_none") {
        return OpType::IS_NOT_NULL;
    }
    if (v == "is_empty") return OpType::IS_EMPTY;
    if (v == "is_not_empty" || v == "not_empty") return OpType::IS_NOT_EMPTY;
    if (v == "gt_field") return OpType::GT_FIELD;
    if (v == "lt_field") return OpType::LT_FIELD;
    if (v == "gte_field") return OpType::GTE_FIELD;
    if (v == "lte_field") return OpType::LTE_FIELD;
    if (v == "eq_field") return OpType::EQ_FIELD;
    if (v == "neq_field") return OpType::NEQ_FIELD;
    if (v == "contains_any") return OpType::CONTAINS_ANY;
    if (v == "contains_all") return OpType::CONTAINS_ALL;
    if (v == "intersects") return OpType::INTERSECTS;
    if (v == "not_intersects") return OpType::NOT_INTERSECTS;
    if (v == "array_len_gt") return OpType::ARRAY_LEN_GT;
    if (v == "array_len_lt") return OpType::ARRAY_LEN_LT;
    if (v == "array_len_eq") return OpType::ARRAY_LEN_EQ;
    if (v == "flags_any") return OpType::FLAGS_ANY;
    if (v == "flags_all") return OpType::FLAGS_ALL;
    if (v == "flags_none") return OpType::FLAGS_NONE;
    if (v == "contains") return OpType::CONTAINS;
    if (v == "starts_with") return OpType::STARTS_WITH;
    if (v == "ends_with") return OpType::ENDS_WITH;
    if (v == "ci_eq") return OpType::CI_EQ;
    if (v == "length_gt") return OpType::LENGTH_GT;
    if (v == "length_lt") return OpType::LENGTH_LT;
    if (v == "length_eq") return OpType::LENGTH_EQ;
    if (v == "ip_in_subnet" || v == "in_subnet") return OpType::IP_IN_SUBNET;
    if (v == "ip_not_in_subnet" || v == "not_in_subnet") return OpType::IP_NOT_IN_SUBNET;
    if (v == "before") return OpType::BEFORE;
    if (v == "after") return OpType::AFTER;
    if (v == "within_last") return OpType::WITHIN_LAST;
    if (v == "day_of_week_in") return OpType::DAY_OF_WEEK_IN;
    if (v == "time_of_day_between") return OpType::TIME_OF_DAY_BETWEEN;
    if (v == "distance_gt" || v == "geo_distance_gt") return OpType::DISTANCE_GT;
    if (v == "distance_lt" || v == "geo_distance_lt") return OpType::DISTANCE_LT;
    if (v == "regex") return OpType::REGEX;
    if (v == "not_regex") return OpType::NOT_REGEX;
    if (v == "in_lookup") return OpType::IN_LOOKUP;
    if (v == "not_in_lookup") return OpType::NOT_IN_LOOKUP;
    return OpType::INVALID;
}

WindowFn window_fn_from_string(const std::string& s) {
    std::string v = lower(s);
    if (v == "sum") return WindowFn::SUM;
    if (v == "min") return WindowFn::MIN;
    if (v == "max") return WindowFn::MAX;
    if (v == "avg") return WindowFn::AVG;
    if (v == "ratio") return WindowFn::RATIO;
    return WindowFn::COUNT;
}

uint64_t parse_uint64_value(const YAML::Node& node) {
    if (node.IsScalar()) {
        std::string s = node.as<std::string>();
        int base = 10;
        size_t pos = 0;
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            pos = 2;
        }
        uint64_t out = 0;
        for (; pos < s.size(); ++pos) {
            char c = s[pos];
            int digit = 0;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = 10 + c - 'a';
            else if (c >= 'A' && c <= 'F') digit = 10 + c - 'A';
            else continue;
            out = out * static_cast<uint64_t>(base) + static_cast<uint64_t>(digit);
        }
        return out;
    }
    return node.as<uint64_t>();
}

ArithmeticExprKind arithmetic_kind_from_string(const std::string& s) {
    std::string v = lower(s);
    if (v == "literal" || v == "const") return ArithmeticExprKind::LITERAL;
    if (v == "add" || v == "+") return ArithmeticExprKind::ADD;
    if (v == "sub" || v == "-") return ArithmeticExprKind::SUB;
    if (v == "mul" || v == "*") return ArithmeticExprKind::MUL;
    if (v == "div" || v == "/") return ArithmeticExprKind::DIV;
    return ArithmeticExprKind::FIELD;
}

ArithmeticExprSpec parse_arithmetic_expr(const YAML::Node& node) {
    ArithmeticExprSpec spec;
    if (node.IsScalar()) {
        try {
            spec.kind = ArithmeticExprKind::LITERAL;
            spec.literal = node.as<double>();
        } catch (const YAML::BadConversion&) {
            spec.kind = ArithmeticExprKind::FIELD;
            spec.field = node.as<std::string>();
        }
        return spec;
    }
    if (node["field"]) {
        spec.kind = ArithmeticExprKind::FIELD;
        spec.field = node["field"].as<std::string>();
        return spec;
    }
    if (node["literal"] || node["value"]) {
        spec.kind = ArithmeticExprKind::LITERAL;
        spec.literal = (node["literal"] ? node["literal"] : node["value"]).as<double>();
        return spec;
    }
    spec.kind = arithmetic_kind_from_string(node["op"].as<std::string>("field"));
    YAML::Node left = node["left"];
    YAML::Node right = node["right"];
    if (left && right) {
        spec.children.push_back(parse_arithmetic_expr(left));
        spec.children.push_back(parse_arithmetic_expr(right));
    } else if (node["args"]) {
        for (const auto& arg : node["args"]) spec.children.push_back(parse_arithmetic_expr(arg));
    }
    return spec;
}

YAML::Node value_node(const YAML::Node& node) {
    if (node["values"]) return node["values"];
    return node["value"];
}

uint64_t fnv1a64(std::string_view text) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

std::string sanitize_identifier(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        if (std::isalnum(static_cast<unsigned char>(ch))) out.push_back(ch);
        else out.push_back('_');
    }
    if (out.empty()) out = "array";
    return out;
}

std::string array_any_synthetic_name(std::string_view path, std::string_view fingerprint) {
    std::ostringstream os;
    os << "__array_any_" << sanitize_identifier(path) << '_'
       << std::hex << fnv1a64(fingerprint);
    return os.str();
}

ParseConditionResult ok_condition(ConditionSpec value) {
    return ParseConditionResult{true, std::move(value), BlazeRulesError{}};
}

ParseConditionResult unknown_op_error(const std::string& op_text, const std::string& rule_id) {
    BlazeRulesError error;
    error.code = BlazeRulesError::UNKNOWN_OP;
    error.message = "unknown operator: '" + op_text + "'";
    error.source = "rule";
    error.rule_id = rule_id;
    error.domain = BlazeRulesError::Domain::RULE_PARSE;
    return ParseConditionResult{false, {}, std::move(error)};
}

// Bound recursion so a hostile deeply-nested and/or/not tree in an untrusted rule
// file (S3, hot-reload) is rejected with a clean error instead of overflowing the
// stack. yaml-cpp bounds its own document nesting, but guard explicitly regardless.
constexpr int kMaxConditionDepth = 128;
ParseConditionResult parse_condition(const YAML::Node& node, const std::string& rule_id, int depth = 0);

ParseConditionResult parse_window_leaf(const YAML::Node& node, const std::string& rule_id) {
    const YAML::Node win = node["window"];
    const YAML::Node op_owner = win["op"] ? win : node;
    const YAML::Node val_owner = win["value"] ? win : node;

    WindowConditionSpec spec;
    spec.field = win["entity_field"].as<std::string>();
    spec.windowfn = window_fn_from_string(win["function"].as<std::string>("count"));
    spec.sum_field = win["sum_field"]
        ? win["sum_field"].as<std::string>()
        : (win["value_field"] ? win["value_field"].as<std::string>()
                              : win["numerator_field"].as<std::string>(""));
    spec.denominator_field = win["denominator_field"].as<std::string>("");
    spec.duration_seconds = win["duration_seconds"].as<int>();
    std::string op_text = op_owner["op"].as<std::string>();
    spec.op = op_from_string(op_text);
    if (spec.op == OpType::INVALID) return unknown_op_error(op_text, rule_id);
    spec.threshold = val_owner["value"].as<double>();
    return ok_condition(std::move(spec));
}

ParseConditionResult parse_model_score_leaf(const YAML::Node& node, const std::string& rule_id) {
    const YAML::Node m = node["model_score"];
    const YAML::Node op_owner = m["op"] ? m : node;
    const YAML::Node val_owner = m["value"] ? m : node;

    ModelScoreConditionSpec spec;
    spec.model_name = m["model"] ? m["model"].as<std::string>()
                                 : m["name"].as<std::string>("");
    if (m["features"]) {
        for (const auto& f : m["features"]) spec.features.push_back(f.as<std::string>());
    }
    spec.output_index = m["output_index"].as<int>(0);
    std::string op_text = op_owner["op"].as<std::string>("gt");
    spec.op = op_from_string(op_text);
    if (spec.op == OpType::INVALID) return unknown_op_error(op_text, rule_id);
    const YAML::Node vn = val_owner["value"];
    if (spec.op == OpType::BETWEEN_INCLUDING || spec.op == OpType::BETWEEN_EXCLUDING) {
        spec.lower = vn[0].as<double>();
        spec.upper = vn[1].as<double>();
    } else {
        spec.threshold = vn.as<double>();
    }
    return ok_condition(std::move(spec));
}

VectorMetric vector_metric_from_string(const std::string& s) {
    std::string v = lower(s);
    if (v == "l2" || v == "euclidean" || v == "distance") return VectorMetric::L2;
    if (v == "dot" || v == "inner") return VectorMetric::DOT;
    return VectorMetric::COSINE;
}

ParseConditionResult parse_vector_distance_leaf(const YAML::Node& node, const std::string& rule_id) {
    const YAML::Node vec = node["vector_distance"];
    const YAML::Node op_owner = vec["op"] ? vec : node;
    const YAML::Node val_owner = vec["value"] ? vec : node;

    VectorDistanceConditionSpec spec;
    if (vec["dims"]) {
        for (const auto& d : vec["dims"]) spec.dims.push_back(d.as<std::string>());
    }
    if (vec["reference"]) {
        for (const auto& r : vec["reference"]) spec.reference.push_back(r.as<float>());
    }
    spec.metric = vector_metric_from_string(vec["metric"].as<std::string>("cosine"));
    std::string op_text = op_owner["op"].as<std::string>("gt");
    spec.op = op_from_string(op_text);
    if (spec.op == OpType::INVALID) return unknown_op_error(op_text, rule_id);
    const YAML::Node vn = val_owner["value"];
    if (spec.op == OpType::BETWEEN_INCLUDING || spec.op == OpType::BETWEEN_EXCLUDING) {
        spec.lower = vn[0].as<double>();
        spec.upper = vn[1].as<double>();
    } else {
        spec.threshold = vn.as<double>();
    }
    return ok_condition(std::move(spec));
}

ParseConditionResult parse_array_any_leaf(const YAML::Node& node, const std::string& rule_id, int depth) {
    const YAML::Node arr = node["array_any"];
    ArrayAnyConditionSpec spec;
    spec.path = arr["path"].as<std::string>();
    auto parsed = parse_condition(arr["where"], rule_id, depth + 1);
    if (!parsed.ok) return parsed;
    spec.where = std::make_shared<ConditionSpec>(std::move(parsed.value));
    spec.synthetic_field = array_any_synthetic_name(spec.path, YAML::Dump(arr));
    return ok_condition(std::move(spec));
}

ParseConditionResult parse_field_leaf(const YAML::Node& node, const std::string& rule_id) {
    std::string op_text = node["op"].as<std::string>();
    OpType op = op_from_string(op_text);
    if (op == OpType::INVALID) return unknown_op_error(op_text, rule_id);
    if (op == OpType::IS_NULL || op == OpType::IS_NOT_NULL ||
        op == OpType::IS_EMPTY || op == OpType::IS_NOT_EMPTY) {
        NullConditionSpec spec;
        spec.field = node["field"].as<std::string>();
        spec.op = op;
        return ok_condition(std::move(spec));
    }

    if (node["expr"]) {
        ArithmeticConditionSpec spec;
        spec.expr = parse_arithmetic_expr(node["expr"]);
        spec.op = op;
        if (node["other_field"]) spec.other_field = node["other_field"].as<std::string>();
        else spec.threshold = value_node(node).as<double>();
        return ok_condition(std::move(spec));
    }

    if (node["other_field"] || op == OpType::GT_FIELD || op == OpType::LT_FIELD ||
        op == OpType::GTE_FIELD || op == OpType::LTE_FIELD ||
        op == OpType::EQ_FIELD || op == OpType::NEQ_FIELD) {
        CrossFieldConditionSpec spec;
        spec.field = node["field"].as<std::string>();
        spec.other_field = node["other_field"].as<std::string>();
        spec.op = op;
        return ok_condition(std::move(spec));
    }

    if (op == OpType::FLAGS_ANY || op == OpType::FLAGS_ALL || op == OpType::FLAGS_NONE) {
        BitfieldConditionSpec spec;
        spec.field = node["field"].as<std::string>();
        spec.op = op;
        spec.mask = parse_uint64_value(node["mask"] ? node["mask"] : value_node(node));
        return ok_condition(std::move(spec));
    }

    if (op == OpType::CONTAINS || op == OpType::STARTS_WITH || op == OpType::ENDS_WITH ||
        op == OpType::CI_EQ || op == OpType::LENGTH_GT || op == OpType::LENGTH_LT ||
        op == OpType::LENGTH_EQ) {
        StringConditionSpec spec;
        spec.field = node["field"].as<std::string>();
        spec.op = op;
        if (op == OpType::LENGTH_GT || op == OpType::LENGTH_LT || op == OpType::LENGTH_EQ) {
            spec.length = value_node(node).as<int>();
        } else {
            spec.value = value_node(node).as<std::string>();
        }
        return ok_condition(std::move(spec));
    }

    if (op == OpType::REGEX || op == OpType::NOT_REGEX) {
        RegexConditionSpec spec;
        spec.field = node["field"].as<std::string>();
        spec.op = op;
        spec.pattern = value_node(node).as<std::string>();
        return ok_condition(std::move(spec));
    }

    if (op == OpType::IN_LOOKUP || op == OpType::NOT_IN_LOOKUP) {
        LookupConditionSpec spec;
        spec.field = node["field"].as<std::string>();
        spec.op = op;
        spec.lookup_name = node["lookup"].as<std::string>();
        return ok_condition(std::move(spec));
    }

    if (op == OpType::ARRAY_LEN_GT || op == OpType::ARRAY_LEN_LT ||
        op == OpType::ARRAY_LEN_EQ) {
        ArrayLenConditionSpec spec;
        spec.field = node["field"].as<std::string>();
        spec.op = op;
        spec.length = value_node(node).as<int>();
        return ok_condition(std::move(spec));
    }

    if (op == OpType::IP_IN_SUBNET || op == OpType::IP_NOT_IN_SUBNET) {
        CidrConditionSpec spec;
        spec.field = node["field"].as<std::string>();
        spec.op = op;
        spec.cidr = value_node(node).as<std::string>();
        return ok_condition(std::move(spec));
    }

    if (op == OpType::WITHIN_LAST || op == OpType::BEFORE || op == OpType::AFTER ||
        op == OpType::DAY_OF_WEEK_IN || op == OpType::TIME_OF_DAY_BETWEEN) {
        TemporalConditionSpec spec;
        spec.field = node["field"].as<std::string>();
        spec.op = op;
        YAML::Node raw = value_node(node);
        if (op == OpType::DAY_OF_WEEK_IN) {
            for (const auto& v : raw) spec.values.push_back(v.as<int>());
        } else if (op == OpType::TIME_OF_DAY_BETWEEN) {
            spec.lower = raw[0].as<double>();
            spec.upper = raw[1].as<double>();
        } else {
            spec.value = raw.as<int64_t>();
        }
        return ok_condition(std::move(spec));
    }

    if (op == OpType::DISTANCE_GT || op == OpType::DISTANCE_LT || node["lat_field"]) {
        GeoDistanceConditionSpec spec;
        spec.lat_field = node["lat_field"].as<std::string>();
        spec.lon_field = node["lon_field"].as<std::string>();
        spec.other_lat_field = node["other_lat_field"].as<std::string>();
        spec.other_lon_field = node["other_lon_field"].as<std::string>();
        spec.op = op;
        spec.threshold_km = value_node(node).as<double>();
        return ok_condition(std::move(spec));
    }
    YAML::Node raw_value = value_node(node);

    if (raw_value.IsSequence() &&
        (op == OpType::BETWEEN_INCLUDING || op == OpType::BETWEEN_EXCLUDING)) {
        NumericRangeConditionSpec spec;
        spec.field = node["field"].as<std::string>();
        spec.op = op;
        spec.lower = raw_value[0].as<double>();
        spec.upper = raw_value[1].as<double>();
        return ok_condition(std::move(spec));
    }

    if (op == OpType::IN || op == OpType::NOT_IN || op == OpType::CONTAINS_ANY ||
        op == OpType::CONTAINS_ALL || op == OpType::INTERSECTS ||
        op == OpType::NOT_INTERSECTS) {
        std::vector<std::string> values;
        values.reserve(raw_value.size());
        for (const auto& v : raw_value) values.push_back(v.as<std::string>());
        CategoricalConditionSpec spec;
        spec.field = node["field"].as<std::string>();
        spec.op = op;
        spec.values = std::move(values);
        return ok_condition(std::move(spec));
    }

    if (op == OpType::EQ || op == OpType::NEQ) {
        try {
            NumericConditionSpec spec;
            spec.field = node["field"].as<std::string>();
            spec.op = op;
            spec.threshold = raw_value.as<double>();
            return ok_condition(std::move(spec));
        } catch (const YAML::BadConversion&) {
            CategoricalConditionSpec spec;
            spec.field = node["field"].as<std::string>();
            spec.op = op;
            spec.values = raw_value.as<std::string>();
            return ok_condition(std::move(spec));
        }
    }

    NumericConditionSpec spec;
    spec.field = node["field"].as<std::string>();
    spec.op = op;
    spec.threshold = raw_value.as<double>();
    return ok_condition(std::move(spec));
}

ParseConditionResult parse_condition(const YAML::Node& node, const std::string& rule_id, int depth) {
    if (depth > kMaxConditionDepth) {
        BlazeRulesError error;
        error.code = BlazeRulesError::UNKNOWN_OP;
        error.message = "condition nesting too deep (limit " + std::to_string(kMaxConditionDepth) + ")";
        error.source = "rule";
        error.rule_id = rule_id;
        error.domain = BlazeRulesError::Domain::RULE_PARSE;
        return ParseConditionResult{false, {}, std::move(error)};
    }
    if (node["sql"] || (node["expr"] && node["expr"].IsScalar())) {
        const YAML::Node sql = node["sql"] ? node["sql"] : node["expr"];
        SqlParseResult parsed = parse_sql_expression(sql.as<std::string>());
        if (!parsed.ok) {
            BlazeRulesError error;
            error.code = BlazeRulesError::UNKNOWN_OP;
            error.message = "invalid sql expression: " + parsed.error;
            error.source = "rule";
            error.rule_id = rule_id;
            error.domain = BlazeRulesError::Domain::RULE_PARSE;
            return ParseConditionResult{false, {}, std::move(error)};
        }
        return ok_condition(std::move(parsed.condition));
    }
    if (node["and"]) {
        AndConditionSpec spec;
        for (const auto& child : node["and"]) {
            auto parsed = parse_condition(child, rule_id, depth + 1);
            if (!parsed.ok) return parsed;
            spec.child_condition.push_back(std::move(parsed.value));
        }
        return ok_condition(std::move(spec));
    }
    if (node["or"]) {
        OrConditionSpec spec;
        for (const auto& child : node["or"]) {
            auto parsed = parse_condition(child, rule_id, depth + 1);
            if (!parsed.ok) return parsed;
            spec.child_condition.push_back(std::move(parsed.value));
        }
        return ok_condition(std::move(spec));
    }
    if (node["not"]) {
        NotConditionSpec spec;
        auto parsed = parse_condition(node["not"], rule_id, depth + 1);
        if (!parsed.ok) return parsed;
        spec.child_condition.push_back(std::move(parsed.value));
        return ok_condition(std::move(spec));
    }
    if (node["window"]) {
        return parse_window_leaf(node, rule_id);
    }
    if (node["model_score"]) {
        return parse_model_score_leaf(node, rule_id);
    }
    if (node["vector_distance"]) {
        return parse_vector_distance_leaf(node, rule_id);
    }
    if (node["array_any"]) {
        return parse_array_any_leaf(node, rule_id, depth);
    }
    return parse_field_leaf(node, rule_id);
}

ParseRuleResult parse_rule(const YAML::Node& node, const std::string& ruleset_version) {
    RuleSpec spec;
    spec.id = node["id"].as<std::string>();
    spec.name = node["name"].as<std::string>(spec.id);
    spec.version = node["version"].as<std::string>(ruleset_version);
    spec.enabled = node["enabled"].as<bool>(true);
    spec.priority = node["priority"].as<int>(0);
    spec.weight = node["weight"].as<double>(0.0);
    spec.score_cap = node["score_cap"].as<double>(0.0);
    spec.reason_code = node["reason_code"].as<std::string>(spec.id);
    spec.shadow = node["shadow"].as<bool>(false);

    std::string action_text = node["action"].as<std::string>("flag");
    spec.action_label = decision_label(action_text);
    if (!action_from_string(action_text, spec.action)) {
        return ParseRuleResult{false, {}, {BlazeRulesError::UNKNOWN_ACTION,
                                           "unknown action: '" + action_text + "'", "rule", spec.id,
                                           -1, BlazeRulesError::Domain::RULE_PARSE}};
    }
    if (node["label"].IsDefined()) {
        spec.action_label = decision_label(node["label"].as<std::string>());
    }
    if (!severity_from_string(node["severity"].as<std::string>("LOW"), spec.severity)) {
        return ParseRuleResult{false, {}, {BlazeRulesError::UNKNOWN_SEVERITY, "unknown severity", "rule", spec.id,
                                           -1, BlazeRulesError::Domain::RULE_PARSE}};
    }

    auto cond = parse_condition(node["conditions"], spec.id);
    if (!cond.ok) return ParseRuleResult{false, {}, std::move(cond.error)};
    spec.root_condition = std::move(cond.value);
    return ParseRuleResult{true, std::move(spec), BlazeRulesError{}};
}

void parse_field_hint_entry(const std::string& name, const YAML::Node& node, RuleFileSpec& spec) {
    FieldHintSpec hint;
    hint.name = name;
    const YAML::Node type_node = node["type"];
    if (type_node.IsDefined()) {
        ColumnType parsed = ColumnType::STRING;
        if (column_type_from_string(type_node.as<std::string>(), parsed)) {
            hint.has_type = true;
            hint.type = parsed;
            if (parsed == ColumnType::ENTITY_KEY) hint.is_entity_field = true;
        }
    }
    if (node["nullable"].IsDefined()) {
        hint.has_nullable = true;
        hint.nullable = node["nullable"].as<bool>();
    }
    YAML::Node entity_node = node["entity"].IsDefined() ? node["entity"] : node["is_entity_field"];
    if (entity_node.IsDefined()) {
        hint.is_entity_field = entity_node.as<bool>();
        if (hint.is_entity_field && !hint.has_type) {
            hint.has_type = true;
            hint.type = ColumnType::ENTITY_KEY;
        }
    }
    YAML::Node values = node["values"].IsDefined() ? node["values"] : node["closed_values"];
    if (values.IsDefined() && values.IsSequence()) {
        for (const auto& v : values) hint.values.push_back(v.as<std::string>());
        if (!hint.has_type) {
            hint.has_type = true;
            hint.type = ColumnType::CATEGORICAL;
        }
    }
    spec.field_hints.push_back(std::move(hint));
}

void parse_field_hints(const YAML::Node& root, RuleFileSpec& spec) {
    YAML::Node fields_node = root["fields"];
    YAML::Node fields = fields_node.IsDefined() ? fields_node : root["schema"];
    if (!fields.IsDefined()) return;

    if (fields.IsMap()) {
        for (const auto& entry : fields) {
            parse_field_hint_entry(entry.first.as<std::string>(), entry.second, spec);
        }
    } else if (fields.IsSequence()) {
        for (const auto& item : fields) {
            std::string name = item["name"].as<std::string>("");
            if (!name.empty()) parse_field_hint_entry(name, item, spec);
        }
    }
}

ParseFileResult parse_root(YAML::Node root, std::string base_dir) {
    RuleFileSpec spec;
    spec.schema_version = root["schema_version"].as<std::string>("1.0");
    spec.base_dir = std::move(base_dir);
    parse_field_hints(root, spec);

    YAML::Node lookups = root["lookups"];
    if (lookups.IsDefined() && lookups.IsMap()) {
        for (const auto& entry : lookups) {
            LookupSpec lookup;
            lookup.name = entry.first.as<std::string>();
            lookup.type = entry.second["type"].as<std::string>();
            lookup.path = entry.second["path"].as<std::string>();
            spec.lookups.push_back(std::move(lookup));
        }
    }

    YAML::Node decisions = root["decisions"];
    if (decisions.IsDefined()) {
        spec.default_decision = decision_label(decisions["default"].as<std::string>("APPROVE"));
        YAML::Node precedence = decisions["precedence"];
        if (precedence.IsDefined() && precedence.IsSequence()) {
            for (const auto& item : precedence) {
                spec.decision_precedence.push_back(decision_label(item.as<std::string>()));
            }
        }
    }
    if (spec.decision_precedence.empty()) {
        spec.decision_precedence = {"APPROVE", "SCORE", "FLAG", "REVIEW", "BLOCK"};
    }
    if (std::find(spec.decision_precedence.begin(), spec.decision_precedence.end(),
                  spec.default_decision) == spec.decision_precedence.end()) {
        spec.decision_precedence.insert(spec.decision_precedence.begin(), spec.default_decision);
    }

    YAML::Node ruleset = root["ruleset"];
    if (ruleset) {
        spec.name = ruleset["name"].as<std::string>("ruleset");
        spec.version = ruleset["version"].as<std::string>("1.0.0");
    } else {
        spec.name = root["name"].as<std::string>("ruleset");
        spec.version = root["version"].as<std::string>("1.0.0");
    }

    YAML::Node rules;
    if (ruleset.IsDefined() && ruleset["rules"].IsDefined()) {
        rules = ruleset["rules"];
    } else {
        rules = root["rules"];
    }
    if (!rules.IsDefined()) {
        return ParseFileResult{true, std::move(spec), BlazeRulesError{}};
    }
    for (const auto& rule : rules) {
        auto parsed = parse_rule(rule, spec.version);
        if (!parsed.ok) return ParseFileResult{false, {}, parsed.error};
        if (parsed.ok && parsed.value.enabled) spec.rules.push_back(std::move(parsed.value));
    }
    return ParseFileResult{true, std::move(spec), BlazeRulesError{}};
}

} // namespace

ParseFileResult parse_rule_file(const std::string& yaml_path) {
    try {
        std::string local_path = blazerules::resolve_resource_to_local(yaml_path);
        std::string base;
        if (blazerules::is_s3_uri(yaml_path)) {
            base = blazerules::resource_parent(yaml_path);
        } else {
            std::filesystem::path p(yaml_path);
            base = p.has_parent_path() ? p.parent_path().string() : ".";
        }
        return parse_root(YAML::LoadFile(local_path), base);
    } catch (const std::exception& e) {
        return ParseFileResult{false, {}, {BlazeRulesError::YAML_SYNTAX_ERROR, e.what(), yaml_path, "",
                                           -1, BlazeRulesError::Domain::RULE_PARSE}};
    }
}

ParseFileResult parse_rule_string(const std::string& contents) {
    try {
        return parse_root(YAML::Load(contents), ".");
    } catch (const std::exception& e) {
        return ParseFileResult{false, {}, {BlazeRulesError::YAML_SYNTAX_ERROR, e.what(), "string", "",
                                           -1, BlazeRulesError::Domain::RULE_PARSE}};
    }
}
