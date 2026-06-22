#include "blazerules/schema_inference.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <simdjson.h>

namespace {

template <class... Ts>
struct Overloaded : Ts... { using Ts::operator()...; };
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool null_like_string(std::string_view sv) {
    std::string value(sv);
    value = lower(value);
    return value.empty() || value == "null" || value == "none" || value == "nan";
}

enum class TypeNeed {
    ANY,
    NUMERIC,
    CATEGORICAL,
    STRING,
    TEXT,
    TEMPORAL,
    BITFIELD,
    CIDR,
    ENTITY,
    BOOLEAN
};

int need_rank(TypeNeed need) {
    switch (need) {
        case TypeNeed::ENTITY: return 100;
        case TypeNeed::STRING: return 90;
        case TypeNeed::CIDR: return 85;
        case TypeNeed::TEMPORAL: return 80;
        case TypeNeed::BITFIELD: return 70;
        case TypeNeed::NUMERIC: return 60;
        case TypeNeed::CATEGORICAL: return 50;
        case TypeNeed::TEXT: return 45;
        case TypeNeed::BOOLEAN: return 40;
        case TypeNeed::ANY: return 0;
    }
    return 0;
}

struct FieldInference {
    std::string name;
    TypeNeed need = TypeNeed::ANY;
    bool has_hint_type = false;
    ColumnType hint_type = ColumnType::STRING;
    bool has_hint_nullable = false;
    bool hint_nullable = true;
    bool entity_hint = false;
    bool from_arrow = false;
    ColumnType arrow_type = ColumnType::STRING;
    bool arrow_nullable = true;
    int64_t rows_sampled = 0;
    int64_t present = 0;
    int64_t nulls = 0;
    int64_t bools = 0;
    int64_t ints = 0;
    int64_t floats = 0;
    int64_t strings = 0;
    int64_t arrays = 0;
    int64_t objects = 0;
    int64_t min_int = std::numeric_limits<int64_t>::max();
    int64_t max_int = std::numeric_limits<int64_t>::min();
    std::vector<std::string> closed_values;
    std::unordered_set<std::string> string_samples;
    bool field_seen_this_row = false;
};

class InferenceBuilder {
public:
    explicit InferenceBuilder(const RuleFileSpec& spec) {
        lookup_types_.reserve(spec.lookups.size());
        for (const auto& lookup : spec.lookups) {
            lookup_types_.emplace(lookup.name, lower(lookup.type));
        }
        for (const auto& hint : spec.field_hints) {
            FieldInference& field = ensure(hint.name);
            field.has_hint_type = hint.has_type;
            field.hint_type = hint.type;
            field.has_hint_nullable = hint.has_nullable;
            field.hint_nullable = hint.nullable;
            field.entity_hint = hint.is_entity_field || hint.type == ColumnType::ENTITY_KEY;
            append_closed_values(field, hint.values);
            if (field.entity_hint) promote(field, TypeNeed::ENTITY);
            else if (hint.has_type) promote_from_type(field, hint.type);
        }
        for (const auto& rule : spec.rules) collect_condition(rule.root_condition);
        build_prefixes();
    }

    bool wants_field(std::string_view name) const {
        return fields_.find(std::string(name)) != fields_.end();
    }

    bool wants_descendants(std::string_view prefix) const {
        return prefixes_.find(std::string(prefix)) != prefixes_.end();
    }

    void begin_row() {
        for (auto& field : ordered_) field->field_seen_this_row = false;
    }

    void end_row() {
        for (auto& field : ordered_) {
            ++field->rows_sampled;
            if (!field->field_seen_this_row) ++field->nulls;
        }
    }

    void observe_json_field(std::string_view name, simdjson::ondemand::value value, int depth) {
        auto it = fields_.find(std::string(name));
        if (it == fields_.end()) {
            if (depth < options_.max_depth && wants_descendants(name)) {
                recurse_if_object(name, value, depth);
            }
            return;
        }
        FieldInference& field = *it->second;
        field.field_seen_this_row = true;
        ++field.present;
        observe_json_value(field, value, depth);
    }

    void observe_arrow_field(const std::string& name, ColumnType type, bool nullable, int64_t rows) {
        FieldInference& field = ensure(name);
        field.from_arrow = true;
        field.arrow_type = type;
        field.arrow_nullable = nullable;
        field.rows_sampled = rows;
        field.present = rows;
    }

    BlazeRulesResult<BlazeRulesSchema> finish() {
        if (ordered_.empty()) {
            return BlazeRulesResult<BlazeRulesSchema>::err(
                {BlazeRulesError::MISSING_REQUIRED_FIELD,
                 "rules do not reference any input fields to infer",
                 "schema_inference", "", -1, BlazeRulesError::Domain::SCHEMA});
        }

        std::vector<FieldSpec> out;
        out.reserve(ordered_.size());
        for (FieldInference* info : ordered_) {
            auto inferred = infer_field(*info);
            if (inferred.is_error()) return BlazeRulesResult<BlazeRulesSchema>::err(inferred.error());
            out.push_back(std::move(inferred.value()));
        }
        return BlazeRulesResult<BlazeRulesSchema>::ok(BlazeRulesSchema(std::move(out)));
    }

    void set_options(const SchemaInferenceOptions& options) { options_ = options; }

private:
    FieldInference& ensure(const std::string& name) {
        auto it = fields_.find(name);
        if (it != fields_.end()) return *it->second;
        auto owned = std::make_unique<FieldInference>();
        owned->name = name;
        FieldInference* ptr = owned.get();
        fields_.emplace(name, std::move(owned));
        ordered_.push_back(ptr);
        return *ptr;
    }

    void promote(FieldInference& field, TypeNeed need) {
        if (need_rank(need) > need_rank(field.need)) field.need = need;
    }

    void promote_from_type(FieldInference& field, ColumnType type) {
        switch (type) {
            case ColumnType::FLOAT32:
            case ColumnType::FLOAT64:
            case ColumnType::INT32:
            case ColumnType::INT64: promote(field, TypeNeed::NUMERIC); break;
            case ColumnType::TIMESTAMP_MS: promote(field, TypeNeed::TEMPORAL); break;
            case ColumnType::BOOLEAN: promote(field, TypeNeed::BOOLEAN); break;
            case ColumnType::CATEGORICAL: promote(field, TypeNeed::CATEGORICAL); break;
            case ColumnType::ENTITY_KEY: promote(field, TypeNeed::ENTITY); break;
            case ColumnType::STRING: promote(field, TypeNeed::STRING); break;
        }
    }

    void append_closed_values(FieldInference& field, const std::vector<std::string>& values) {
        if (values.empty()) return;
        std::unordered_set<std::string> seen(field.closed_values.begin(), field.closed_values.end());
        for (const auto& value : values) {
            if (seen.insert(value).second) field.closed_values.push_back(value);
        }
    }

    void add_categorical_values(FieldInference& field, const Textual& values) {
        if (std::holds_alternative<std::string>(values)) {
            std::vector<std::string> one{std::get<std::string>(values)};
            append_closed_values(field, one);
        } else {
            append_closed_values(field, std::get<std::vector<std::string>>(values));
        }
    }

    void collect_expr(const ArithmeticExprSpec& expr) {
        if (expr.kind == ArithmeticExprKind::FIELD && !expr.field.empty()) {
            promote(ensure(expr.field), TypeNeed::NUMERIC);
        }
        for (const auto& child : expr.children) collect_expr(child);
    }

    void collect_condition(const ConditionSpec& condition) {
        std::visit(Overloaded{
            [&](const NumericConditionSpec& c) { promote(ensure(c.field), TypeNeed::NUMERIC); },
            [&](const NumericRangeConditionSpec& c) { promote(ensure(c.field), TypeNeed::NUMERIC); },
            [&](const CategoricalConditionSpec& c) {
                FieldInference& field = ensure(c.field);
                promote(field, TypeNeed::CATEGORICAL);
                add_categorical_values(field, c.values);
            },
            [&](const ArrayLenConditionSpec& c) {
                promote(ensure(c.field), TypeNeed::CATEGORICAL);
            },
            [&](const NullConditionSpec& c) {
                FieldInference& field = ensure(c.field);
                if (c.op == OpType::IS_EMPTY || c.op == OpType::IS_NOT_EMPTY) promote(field, TypeNeed::TEXT);
            },
            [&](const CrossFieldConditionSpec& c) {
                FieldInference& left = ensure(c.field);
                FieldInference& right = ensure(c.other_field);
                bool numeric_compare = c.op == OpType::GT_FIELD || c.op == OpType::LT_FIELD ||
                    c.op == OpType::GTE_FIELD || c.op == OpType::LTE_FIELD;
                promote(left, numeric_compare ? TypeNeed::NUMERIC : TypeNeed::TEXT);
                promote(right, numeric_compare ? TypeNeed::NUMERIC : TypeNeed::TEXT);
            },
            [&](const BitfieldConditionSpec& c) { promote(ensure(c.field), TypeNeed::BITFIELD); },
            [&](const StringConditionSpec& c) { promote(ensure(c.field), TypeNeed::STRING); },
            [&](const RegexConditionSpec& c) { promote(ensure(c.field), TypeNeed::STRING); },
            [&](const LookupConditionSpec& c) {
                FieldInference& field = ensure(c.field);
                auto it = lookup_types_.find(c.lookup_name);
                if (it != lookup_types_.end() && it->second == "int_set") promote(field, TypeNeed::NUMERIC);
                else if (it != lookup_types_.end() && it->second == "ipv4_cidr_set") promote(field, TypeNeed::CIDR);
                else promote(field, TypeNeed::TEXT);
            },
            [&](const CidrConditionSpec& c) { promote(ensure(c.field), TypeNeed::CIDR); },
            [&](const TemporalConditionSpec& c) { promote(ensure(c.field), TypeNeed::TEMPORAL); },
            [&](const GeoDistanceConditionSpec& c) {
                promote(ensure(c.lat_field), TypeNeed::NUMERIC);
                promote(ensure(c.lon_field), TypeNeed::NUMERIC);
                promote(ensure(c.other_lat_field), TypeNeed::NUMERIC);
                promote(ensure(c.other_lon_field), TypeNeed::NUMERIC);
            },
            [&](const ArithmeticConditionSpec& c) {
                collect_expr(c.expr);
                if (!c.other_field.empty()) promote(ensure(c.other_field), TypeNeed::NUMERIC);
            },
            [&](const WindowConditionSpec& c) {
                promote(ensure(c.field), TypeNeed::ENTITY);
                if (!c.sum_field.empty()) promote(ensure(c.sum_field), TypeNeed::NUMERIC);
                if (!c.denominator_field.empty()) promote(ensure(c.denominator_field), TypeNeed::NUMERIC);
            },
            [&](const ModelScoreConditionSpec& c) {
                for (const auto& f : c.features) promote(ensure(f), TypeNeed::NUMERIC);
            },
            [&](const VectorDistanceConditionSpec& c) {
                for (const auto& f : c.dims) promote(ensure(f), TypeNeed::NUMERIC);
            },
            [&](const ArrayAnyConditionSpec& c) {
                FieldInference& field = ensure(c.synthetic_field);
                field.has_hint_type = true;
                field.hint_type = ColumnType::INT32;
                field.has_hint_nullable = true;
                field.hint_nullable = false;
                promote(field, TypeNeed::NUMERIC);
            },
            [&](const AndConditionSpec& c) { for (const auto& child : c.child_condition) collect_condition(child); },
            [&](const OrConditionSpec& c) { for (const auto& child : c.child_condition) collect_condition(child); },
            [&](const NotConditionSpec& c) { for (const auto& child : c.child_condition) collect_condition(child); }
        }, condition.node);
    }

    void build_prefixes() {
        prefixes_.clear();
        for (const auto& [name, _] : fields_) {
            size_t pos = name.find('.');
            while (pos != std::string::npos) {
                prefixes_.insert(name.substr(0, pos));
                pos = name.find('.', pos + 1);
            }
        }
    }

    void observe_scalar(FieldInference& field, simdjson::ondemand::value value, simdjson::ondemand::json_type type) {
        if (type == simdjson::ondemand::json_type::null) {
            ++field.nulls;
            return;
        }
        if (type == simdjson::ondemand::json_type::boolean) {
            bool v = false;
            if (!value.get(v)) ++field.bools;
            else ++field.nulls;
            return;
        }
        if (type == simdjson::ondemand::json_type::number) {
            int64_t i = 0;
            if (!value.get(i)) {
                ++field.ints;
                field.min_int = std::min(field.min_int, i);
                field.max_int = std::max(field.max_int, i);
                return;
            }
            double d = 0.0;
            if (!value.get(d)) {
                // Not an integer literal (it had a decimal point or exponent) -> count it
                // as a float even when the value is whole (e.g. 12000.0). Trusting the
                // literal form keeps float feature/score fields from being inferred as
                // INT32 and silently truncating/dropping later fractional values.
                ++field.floats;
            } else {
                ++field.nulls;
            }
            return;
        }
        if (type == simdjson::ondemand::json_type::string) {
            std::string_view sv;
            if (!value.get(sv)) {
                if (null_like_string(sv)) ++field.nulls;
                else {
                    ++field.strings;
                    if (field.string_samples.size() < static_cast<size_t>(options_.categorical_max_cardinality)) {
                        field.string_samples.emplace(sv);
                    }
                }
            } else {
                ++field.nulls;
            }
            return;
        }
        if (type == simdjson::ondemand::json_type::object) ++field.objects;
    }

    void observe_json_value(FieldInference& field, simdjson::ondemand::value value, int depth) {
        simdjson::ondemand::json_type type;
        if (value.type().get(type)) {
            ++field.nulls;
            return;
        }
        if (type == simdjson::ondemand::json_type::array) {
            ++field.arrays;
            simdjson::ondemand::array arr;
            if (!value.get_array().get(arr)) {
                for (auto item_result : arr) {
                    simdjson::ondemand::value item;
                    if (std::move(item_result).get(item)) continue;
                    simdjson::ondemand::json_type item_type;
                    if (item.type().get(item_type)) continue;
                    observe_scalar(field, item, item_type);
                }
            }
            return;
        }
        if (type == simdjson::ondemand::json_type::object && depth < options_.max_depth) {
            ++field.objects;
            recurse_if_object(field.name, value, depth);
            return;
        }
        observe_scalar(field, value, type);
    }

    void recurse_if_object(std::string_view prefix, simdjson::ondemand::value value, int depth) {
        simdjson::ondemand::json_type type;
        if (value.type().get(type) || type != simdjson::ondemand::json_type::object) return;
        simdjson::ondemand::object object;
        if (value.get_object().get(object)) return;
        std::string child_prefix(prefix);
        size_t base = child_prefix.size();
        for (auto field_result : object) {
            simdjson::ondemand::field child;
            if (std::move(field_result).get(child)) continue;
            std::string_view key = child.escaped_key();
            child_prefix.resize(base);
            child_prefix.push_back('.');
            child_prefix.append(key.data(), key.size());
            simdjson::ondemand::value child_value = child.value();
            observe_json_field(child_prefix, child_value, depth + 1);
        }
    }

    ColumnType choose_numeric_type(const FieldInference& field) const {
        if (field.floats > 0) return ColumnType::FLOAT64;
        if (field.min_int >= std::numeric_limits<int32_t>::min() &&
            field.max_int <= std::numeric_limits<int32_t>::max()) {
            return ColumnType::INT32;
        }
        return ColumnType::INT64;
    }

    BlazeRulesResult<FieldSpec> infer_field(const FieldInference& field) const {
        FieldSpec out;
        out.name = field.name;
        out.source = field.from_arrow ? FieldSpec::DERIVED_FROM_ARROW : FieldSpec::INFERRED_AND_BOUND;

        if (field.has_hint_type) {
            out.type = field.hint_type;
        } else if (field.from_arrow) {
            out.type = refine_arrow_type(field);
        } else {
            out.type = infer_json_type(field);
        }

        out.is_entity_field = field.entity_hint || out.type == ColumnType::ENTITY_KEY || field.need == TypeNeed::ENTITY;
        if (out.is_entity_field) out.type = ColumnType::ENTITY_KEY;
        out.nullable = field.has_hint_nullable
            ? field.hint_nullable
            : (field.from_arrow ? field.arrow_nullable : field.nulls > 0 || field.present < field.rows_sampled);
        out.closed_values = field.closed_values;
        if (out.closed_values.empty() && out.type == ColumnType::CATEGORICAL &&
            field.string_samples.size() <= static_cast<size_t>(options_.categorical_max_cardinality)) {
            out.closed_values.assign(field.string_samples.begin(), field.string_samples.end());
            std::sort(out.closed_values.begin(), out.closed_values.end());
        }

        if (out.type == ColumnType::FLOAT64 && field.need == TypeNeed::NUMERIC && field.floats == 0 &&
            field.ints == 0 && !field.from_arrow) {
            return BlazeRulesResult<FieldSpec>::err(
                {BlazeRulesError::TYPE_MISMATCH,
                 "unable to infer numeric field from sampled data: " + field.name,
                 "schema_inference", "", -1, BlazeRulesError::Domain::SCHEMA});
        }
        return BlazeRulesResult<FieldSpec>::ok(std::move(out));
    }

    ColumnType refine_arrow_type(const FieldInference& field) const {
        if (field.need == TypeNeed::ENTITY) return ColumnType::ENTITY_KEY;
        if (field.need == TypeNeed::STRING || field.need == TypeNeed::CIDR) return ColumnType::STRING;
        if (field.need == TypeNeed::CATEGORICAL) return ColumnType::CATEGORICAL;
        if (field.need == TypeNeed::TEMPORAL) return ColumnType::TIMESTAMP_MS;
        if (field.need == TypeNeed::BITFIELD) return ColumnType::INT64;
        if (field.need == TypeNeed::NUMERIC && !is_numeric(field.arrow_type)) return ColumnType::FLOAT64;
        if (field.need == TypeNeed::TEXT && field.arrow_type == ColumnType::STRING) return ColumnType::STRING;
        return field.arrow_type;
    }

    ColumnType infer_json_type(const FieldInference& field) const {
        if (field.need == TypeNeed::ENTITY) return ColumnType::ENTITY_KEY;
        if (field.need == TypeNeed::STRING || field.need == TypeNeed::CIDR) return ColumnType::STRING;
        if (field.need == TypeNeed::TEMPORAL) return ColumnType::TIMESTAMP_MS;
        if (field.need == TypeNeed::BITFIELD) return ColumnType::INT64;
        if (field.need == TypeNeed::BOOLEAN) return ColumnType::BOOLEAN;
        if (field.need == TypeNeed::CATEGORICAL) return ColumnType::CATEGORICAL;
        if (field.need == TypeNeed::NUMERIC) return choose_numeric_type(field);
        if (field.strings > 0 || field.arrays > 0) return ColumnType::STRING;
        if (field.bools > 0 && field.ints == 0 && field.floats == 0) return ColumnType::BOOLEAN;
        if (field.ints > 0 || field.floats > 0) return choose_numeric_type(field);
        return ColumnType::STRING;
    }

    SchemaInferenceOptions options_;
    std::unordered_map<std::string, std::unique_ptr<FieldInference>> fields_;
    std::vector<FieldInference*> ordered_;
    std::unordered_set<std::string> prefixes_;
    std::unordered_map<std::string, std::string> lookup_types_;
};

ColumnType arrow_to_column_type(const std::shared_ptr<arrow::DataType>& type, TypeNeed need) {
    switch (type->id()) {
        case arrow::Type::FLOAT: return ColumnType::FLOAT32;
        case arrow::Type::DOUBLE: return ColumnType::FLOAT64;
        case arrow::Type::INT32: return ColumnType::INT32;
        case arrow::Type::INT64: return need == TypeNeed::TEMPORAL ? ColumnType::TIMESTAMP_MS : ColumnType::INT64;
        case arrow::Type::TIMESTAMP: return ColumnType::TIMESTAMP_MS;
        case arrow::Type::BOOL: return ColumnType::BOOLEAN;
        case arrow::Type::STRING:
        case arrow::Type::LARGE_STRING:
            return need == TypeNeed::CATEGORICAL ? ColumnType::CATEGORICAL : ColumnType::STRING;
        case arrow::Type::DICTIONARY:
            return ColumnType::CATEGORICAL;
        default:
            return ColumnType::STRING;
    }
}

void observe_arrow_field_recursive(InferenceBuilder& builder,
                                   const std::string& name,
                                   const std::shared_ptr<arrow::Field>& field,
                                   bool parent_nullable,
                                   int64_t rows) {
    bool nullable = parent_nullable || field->nullable();
    if (builder.wants_field(name)) {
        TypeNeed need = TypeNeed::ANY;
        ColumnType type = arrow_to_column_type(field->type(), need);
        builder.observe_arrow_field(name, type, nullable, rows);
    }
    if (field->type()->id() != arrow::Type::STRUCT || !builder.wants_descendants(name)) {
        return;
    }
    auto struct_type = std::static_pointer_cast<arrow::StructType>(field->type());
    for (int i = 0; i < struct_type->num_fields(); ++i) {
        const auto& child = struct_type->field(i);
        observe_arrow_field_recursive(builder,
                                      name + "." + child->name(),
                                      child,
                                      nullable,
                                      rows);
    }
}

BlazeRulesResult<BlazeRulesSchema> infer_from_padded_messages(const RuleFileSpec& rules,
                                                const std::vector<std::string_view>& messages,
                                                const SchemaInferenceOptions& options) {
    if (messages.empty()) {
        return BlazeRulesResult<BlazeRulesSchema>::err(
            {BlazeRulesError::MISSING_REQUIRED_FIELD,
             "cannot infer schema from an empty first batch",
             "schema_inference", "", -1, BlazeRulesError::Domain::SCHEMA});
    }

    InferenceBuilder builder(rules);
    builder.set_options(options);
    simdjson::ondemand::parser parser;
    std::string padded;
    int64_t rows = 0;
    int64_t bytes = 0;
    for (std::string_view msg : messages) {
        if (rows >= options.sample_rows || bytes >= options.sample_bytes) break;
        bytes += static_cast<int64_t>(msg.size());
        padded.assign(msg.data(), msg.size());
        padded.resize(msg.size() + simdjson::SIMDJSON_PADDING, '\0');
        simdjson::ondemand::document doc;
        if (parser.iterate(padded.data(), msg.size(), padded.size()).get(doc)) continue;
        simdjson::ondemand::object object;
        if (doc.get_object().get(object)) continue;
        builder.begin_row();
        for (auto field_result : object) {
            simdjson::ondemand::field field;
            if (std::move(field_result).get(field)) continue;
            std::string_view key = field.escaped_key();
            simdjson::ondemand::value value = field.value();
            builder.observe_json_field(key, value, 0);
        }
        builder.end_row();
        ++rows;
    }
    return builder.finish();
}

} // namespace

BlazeRulesResult<BlazeRulesSchema> infer_schema_from_messages(
    const RuleFileSpec& rules,
    const std::vector<std::string_view>& messages,
    const SchemaInferenceOptions& options) {
    return infer_from_padded_messages(rules, messages, options);
}

BlazeRulesResult<BlazeRulesSchema> infer_schema_from_ndjson(
    const RuleFileSpec& rules,
    std::string_view ndjson,
    const SchemaInferenceOptions& options) {
    std::vector<std::string_view> lines;
    lines.reserve(std::min<int64_t>(options.sample_rows, 4096));
    size_t start = 0;
    int64_t bytes = 0;
    while (start < ndjson.size() &&
           static_cast<int64_t>(lines.size()) < options.sample_rows &&
           bytes < options.sample_bytes) {
        size_t end = ndjson.find('\n', start);
        if (end == std::string_view::npos) end = ndjson.size();
        if (end > start) {
            lines.emplace_back(ndjson.data() + start, end - start);
            bytes += static_cast<int64_t>(end - start);
        }
        start = end + 1;
    }
    return infer_from_padded_messages(rules, lines, options);
}

BlazeRulesResult<BlazeRulesSchema> infer_schema_from_arrow(
    const RuleFileSpec& rules,
    const std::shared_ptr<arrow::RecordBatch>& batch,
    const SchemaInferenceOptions& options) {
    if (!batch) {
        return BlazeRulesResult<BlazeRulesSchema>::err(
            {BlazeRulesError::SCHEMA_MISMATCH,
             "cannot infer schema from a null Arrow batch",
             "schema_inference", "", -1, BlazeRulesError::Domain::ARROW});
    }
    InferenceBuilder builder(rules);
    builder.set_options(options);
    for (int i = 0; i < batch->num_columns(); ++i) {
        const auto& field = batch->schema()->field(i);
        if (!builder.wants_field(field->name()) && !builder.wants_descendants(field->name())) {
            continue;
        }
        observe_arrow_field_recursive(builder, field->name(), field, false, batch->num_rows());
    }
    return builder.finish();
}
