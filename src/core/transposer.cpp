#include "blazerules/transposer.h"

#include "blazerules/arrow_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <optional>
#include <thread>
#include <type_traits>
#include <unordered_map>

#include <re2/re2.h>

namespace {

constexpr size_t kParallelJsonThresholdBytes = 8 * 1024 * 1024;

int default_thread_count() {
    unsigned int hw = std::thread::hardware_concurrency();
    return hw == 0 ? 1 : static_cast<int>(hw);
}

int estimate_rows_for_chunk(std::string_view ndjson_bytes, size_t start, size_t end) {
    size_t pos = start;
    size_t sampled_bytes = 0;
    int sampled_rows = 0;
    while (pos < end && sampled_rows < 32) {
        size_t newline = ndjson_bytes.find('\n', pos);
        if (newline == std::string_view::npos || newline >= end) break;
        sampled_bytes += newline + 1 - pos;
        pos = newline + 1;
        ++sampled_rows;
    }
    if (sampled_rows == 0 || sampled_bytes == 0) return 1;
    size_t chunk_bytes = end - start;
    size_t estimated = (chunk_bytes * static_cast<size_t>(sampled_rows) * 11) /
                       (sampled_bytes * 10) + 8;
    return static_cast<int>(std::max<size_t>(estimated, 1));
}

void set_bit(std::vector<uint8_t>& bits, int64_t index, bool value) {
    size_t byte_index = static_cast<size_t>(index >> 3);
    uint8_t mask = static_cast<uint8_t>(1u << (index & 7));
    if (byte_index >= bits.size()) bits.resize(byte_index + 1, 0);
    if (value) bits[byte_index] |= mask;
}

void ensure_bit_slot(std::vector<uint8_t>& bits, int64_t index) {
    size_t byte_index = static_cast<size_t>(index >> 3);
    if (byte_index >= bits.size()) bits.resize(byte_index + 1, 0);
}

bool null_like_string(std::string_view value) {
    return value.empty() || value == "NONE" || value == "None" ||
           value == "none" || value == "NULL" || value == "null";
}

uint32_t parse_ipv4_text(std::string_view s) {
    uint32_t parts[4] = {0, 0, 0, 0};
    int part = 0;
    uint32_t value = 0;
    bool saw_digit = false;
    for (char ch : s) {
        if (ch >= '0' && ch <= '9') {
            value = value * 10u + static_cast<uint32_t>(ch - '0');
            saw_digit = true;
        } else if (ch == '.' && part < 3) {
            parts[part++] = value;
            value = 0;
            saw_digit = false;
        } else {
            return 0;
        }
    }
    if (!saw_digit || part != 3) return 0;
    parts[part] = value;
    return (parts[0] << 24u) | (parts[1] << 16u) | (parts[2] << 8u) | parts[3];
}

void parse_cidr_text(std::string_view cidr, uint32_t& network, uint32_t& mask) {
    size_t slash = cidr.find('/');
    std::string_view ip = slash == std::string_view::npos ? cidr : cidr.substr(0, slash);
    int prefix = slash == std::string_view::npos
        ? 32
        : std::max(0, std::min(32, std::stoi(std::string(cidr.substr(slash + 1)))));
    mask = prefix == 0 ? 0u : (0xffffffffu << (32 - prefix));
    network = parse_ipv4_text(ip) & mask;
}

bool ipv4_ranges_contain_value(const std::vector<Ipv4Range>& ranges, uint32_t ip) {
    auto it = std::upper_bound(ranges.begin(), ranges.end(), ip,
        [](uint32_t value, const Ipv4Range& range) { return value < range.start; });
    if (it == ranges.begin()) return false;
    --it;
    return ip >= it->start && ip <= it->end;
}

double deg_to_rad(double v) {
    return v * 0.01745329251994329576923690768489;
}

bool lookup_text_value(simdjson::ondemand::value& value, std::string_view& out, std::string& scratch) {
    simdjson::ondemand::json_type type;
    if (value.type().get(type) || type == simdjson::ondemand::json_type::null) return false;
    if (type == simdjson::ondemand::json_type::string) {
        return !value.get(out) && !null_like_string(out);
    }
    if (type == simdjson::ondemand::json_type::number) {
        int64_t i = 0;
        if (!value.get(i)) {
            scratch = std::to_string(i);
            out = scratch;
            return true;
        }
        double d = 0.0;
        if (!value.get(d)) {
            scratch = std::to_string(d);
            out = scratch;
            return true;
        }
    }
    if (type == simdjson::ondemand::json_type::boolean) {
        bool b = false;
        if (!value.get(b)) {
            scratch = b ? "true" : "false";
            out = scratch;
            return true;
        }
    }
    return false;
}

struct JsonAnyValue {
    bool exists = false;
    bool is_null = false;
    bool has_number = false;
    double number = 0.0;
    bool has_bool = false;
    bool bool_value = false;
    std::string text;
    std::vector<std::string> texts;
};

using JsonAnyRow = std::unordered_map<std::string, JsonAnyValue>;

bool any_op_numeric(double lhs, OpType op, double rhs) {
    switch (op) {
        case OpType::GT: return lhs > rhs;
        case OpType::LT: return lhs < rhs;
        case OpType::GTE: return lhs >= rhs;
        case OpType::LTE: return lhs <= rhs;
        case OpType::EQ: return lhs == rhs;
        case OpType::NEQ: return lhs != rhs;
        default: return false;
    }
}

bool any_op_string(std::string_view lhs, OpType op, std::string_view rhs) {
    switch (op) {
        case OpType::EQ: return lhs == rhs;
        case OpType::NEQ: return lhs != rhs;
        case OpType::CONTAINS: return lhs.find(rhs) != std::string_view::npos;
        case OpType::STARTS_WITH: return lhs.rfind(rhs, 0) == 0;
        case OpType::ENDS_WITH:
            return lhs.size() >= rhs.size() &&
                   lhs.substr(lhs.size() - rhs.size()) == rhs;
        case OpType::CI_EQ: {
            if (lhs.size() != rhs.size()) return false;
            for (size_t i = 0; i < lhs.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
                    std::tolower(static_cast<unsigned char>(rhs[i]))) return false;
            }
            return true;
        }
        default: return false;
    }
}

void collect_json_any_value(JsonAnyRow& row, const std::string& name,
                            simdjson::ondemand::value value, int depth);

void collect_json_any_object(JsonAnyRow& row, std::string& prefix,
                             simdjson::ondemand::object object, int depth) {
    size_t base = prefix.size();
    for (auto field_result : object) {
        simdjson::ondemand::field field;
        if (std::move(field_result).get(field)) continue;
        std::string_view key = field.escaped_key();
        prefix.resize(base);
        if (!prefix.empty()) prefix.push_back('.');
        prefix.append(key.data(), key.size());
        collect_json_any_value(row, prefix, field.value(), depth + 1);
    }
    prefix.resize(base);
}

void collect_json_any_value(JsonAnyRow& row, const std::string& name,
                            simdjson::ondemand::value value, int depth) {
    simdjson::ondemand::json_type type;
    if (value.type().get(type)) return;
    JsonAnyValue out;
    out.exists = true;
    if (type == simdjson::ondemand::json_type::null) {
        out.is_null = true;
        row[name] = std::move(out);
        return;
    }
    if (type == simdjson::ondemand::json_type::number) {
        double d = 0.0;
        if (!value.get(d)) {
            out.has_number = true;
            out.number = d;
            out.text = std::to_string(d);
        }
    } else if (type == simdjson::ondemand::json_type::boolean) {
        bool b = false;
        if (!value.get(b)) {
            out.has_bool = true;
            out.bool_value = b;
            out.has_number = true;
            out.number = b ? 1.0 : 0.0;
            out.text = b ? "true" : "false";
        }
    } else if (type == simdjson::ondemand::json_type::string) {
        std::string_view sv;
        if (!value.get(sv)) {
            out.text.assign(sv.data(), sv.size());
            if (null_like_string(sv)) out.is_null = true;
        }
    } else if (type == simdjson::ondemand::json_type::array) {
        simdjson::ondemand::array arr;
        if (!value.get_array().get(arr)) {
            std::string scratch;
            for (auto item_result : arr) {
                simdjson::ondemand::value item;
                if (std::move(item_result).get(item)) continue;
                std::string_view sv;
                if (lookup_text_value(item, sv, scratch)) {
                    out.texts.emplace_back(sv.data(), sv.size());
                }
            }
        }
    } else if (type == simdjson::ondemand::json_type::object && depth < 16) {
        simdjson::ondemand::object object;
        if (!value.get_object().get(object)) {
            std::string prefix = name;
            collect_json_any_object(row, prefix, object, depth);
            return;
        }
    }
    row[name] = std::move(out);
}

const JsonAnyValue* any_find(const JsonAnyRow& row, const std::string& field) {
    auto it = row.find(field);
    return it == row.end() ? nullptr : &it->second;
}

bool text_in_values(std::string_view text, const Textual& values) {
    if (std::holds_alternative<std::string>(values)) return text == std::get<std::string>(values);
    for (const auto& value : std::get<std::vector<std::string>>(values)) {
        if (text == value) return true;
    }
    return false;
}

bool any_eval_condition(const ConditionSpec& c, const JsonAnyRow& row,
                        const LookupRegistry& lookups);

double any_eval_expr(const ArithmeticExprSpec& expr, const JsonAnyRow& row) {
    if (expr.kind == ArithmeticExprKind::LITERAL) return expr.literal;
    if (expr.kind == ArithmeticExprKind::FIELD) {
        const JsonAnyValue* v = any_find(row, expr.field);
        return v && v->has_number ? v->number : 0.0;
    }
    double left = expr.children.empty() ? 0.0 : any_eval_expr(expr.children[0], row);
    double right = expr.children.size() < 2 ? 0.0 : any_eval_expr(expr.children[1], row);
    switch (expr.kind) {
        case ArithmeticExprKind::ADD: return left + right;
        case ArithmeticExprKind::SUB: return left - right;
        case ArithmeticExprKind::MUL: return left * right;
        case ArithmeticExprKind::DIV: return right == 0.0 ? 0.0 : left / right;
        default: return left;
    }
}

bool any_eval_condition(const ConditionSpec& c, const JsonAnyRow& row,
                        const LookupRegistry& lookups) {
    return std::visit([&](const auto& spec) -> bool {
        using T = std::decay_t<decltype(spec)>;
        if constexpr (std::is_same_v<T, NumericConditionSpec>) {
            const JsonAnyValue* v = any_find(row, spec.field);
            return v && v->has_number && any_op_numeric(v->number, spec.op, spec.threshold);
        } else if constexpr (std::is_same_v<T, NumericRangeConditionSpec>) {
            const JsonAnyValue* v = any_find(row, spec.field);
            if (!v || !v->has_number) return false;
            return spec.op == OpType::BETWEEN_EXCLUDING
                ? (v->number > spec.lower && v->number < spec.upper)
                : (v->number >= spec.lower && v->number <= spec.upper);
        } else if constexpr (std::is_same_v<T, CategoricalConditionSpec>) {
            const JsonAnyValue* v = any_find(row, spec.field);
            if (!v) return false;
            bool found = false;
            if (!v->text.empty()) found = text_in_values(v->text, spec.values);
            for (const auto& text : v->texts) found = found || text_in_values(text, spec.values);
            bool positive = spec.op == OpType::EQ || spec.op == OpType::IN ||
                            spec.op == OpType::CONTAINS_ANY || spec.op == OpType::INTERSECTS;
            if (spec.op == OpType::CONTAINS_ALL && std::holds_alternative<std::vector<std::string>>(spec.values)) {
                const auto& required = std::get<std::vector<std::string>>(spec.values);
                for (const auto& r : required) {
                    bool one = false;
                    for (const auto& text : v->texts) one = one || text == r;
                    if (!one) return false;
                }
                return true;
            }
            return positive ? found : !found;
        } else if constexpr (std::is_same_v<T, ArrayLenConditionSpec>) {
            const JsonAnyValue* v = any_find(row, spec.field);
            if (!v) return false;
            int len = !v->texts.empty() ? static_cast<int>(v->texts.size())
                                        : static_cast<int>(v->text.size());
            return any_op_numeric(static_cast<double>(len), spec.op == OpType::ARRAY_LEN_LT ? OpType::LT :
                (spec.op == OpType::ARRAY_LEN_EQ ? OpType::EQ : OpType::GT), spec.length);
        } else if constexpr (std::is_same_v<T, NullConditionSpec>) {
            const JsonAnyValue* v = any_find(row, spec.field);
            bool is_null = !v || v->is_null;
            if (spec.op == OpType::IS_NULL) return is_null;
            if (spec.op == OpType::IS_NOT_NULL) return !is_null;
            if (spec.op == OpType::IS_EMPTY) return is_null || (v && v->text.empty() && v->texts.empty());
            if (spec.op == OpType::IS_NOT_EMPTY) return v && (!v->text.empty() || !v->texts.empty());
            return false;
        } else if constexpr (std::is_same_v<T, CrossFieldConditionSpec>) {
            const JsonAnyValue* l = any_find(row, spec.field);
            const JsonAnyValue* r = any_find(row, spec.other_field);
            if (!l || !r) return false;
            if (l->has_number && r->has_number) {
                OpType op = spec.op == OpType::GT_FIELD ? OpType::GT :
                    (spec.op == OpType::LT_FIELD ? OpType::LT :
                    (spec.op == OpType::GTE_FIELD ? OpType::GTE :
                    (spec.op == OpType::LTE_FIELD ? OpType::LTE :
                    (spec.op == OpType::NEQ_FIELD ? OpType::NEQ : OpType::EQ))));
                return any_op_numeric(l->number, op, r->number);
            }
            return spec.op == OpType::NEQ_FIELD ? l->text != r->text : l->text == r->text;
        } else if constexpr (std::is_same_v<T, BitfieldConditionSpec>) {
            const JsonAnyValue* v = any_find(row, spec.field);
            if (!v || !v->has_number) return false;
            uint64_t flags = static_cast<uint64_t>(v->number);
            uint64_t masked = flags & spec.mask;
            if (spec.op == OpType::FLAGS_ALL) return masked == spec.mask;
            if (spec.op == OpType::FLAGS_NONE) return masked == 0;
            return masked != 0;
        } else if constexpr (std::is_same_v<T, StringConditionSpec>) {
            const JsonAnyValue* v = any_find(row, spec.field);
            if (!v) return false;
            if (spec.op == OpType::LENGTH_GT || spec.op == OpType::LENGTH_LT || spec.op == OpType::LENGTH_EQ) {
                OpType op = spec.op == OpType::LENGTH_LT ? OpType::LT :
                    (spec.op == OpType::LENGTH_EQ ? OpType::EQ : OpType::GT);
                return any_op_numeric(static_cast<double>(v->text.size()), op, spec.length);
            }
            return any_op_string(v->text, spec.op, spec.value);
        } else if constexpr (std::is_same_v<T, RegexConditionSpec>) {
            const JsonAnyValue* v = any_find(row, spec.field);
            if (!v) return false;
            if (spec.compiled) {
                bool matched = spec.compiled->ok() && RE2::PartialMatch(v->text, *spec.compiled);
                return spec.op == OpType::NOT_REGEX ? !matched : matched;
            }
            RE2 fallback(spec.pattern);
            bool matched = fallback.ok() && RE2::PartialMatch(v->text, fallback);
            return spec.op == OpType::NOT_REGEX ? !matched : matched;
        } else if constexpr (std::is_same_v<T, LookupConditionSpec>) {
            const JsonAnyValue* v = any_find(row, spec.field);
            auto it = lookups.find(spec.lookup_name);
            if (!v || it == lookups.end() || !it->second) return false;
            const auto& lookup = *it->second;
            bool matched = false;
            if (lookup.type == LookupSetType::STRING_SET) {
                matched = std::binary_search(lookup.strings.begin(), lookup.strings.end(), v->text);
            } else if (lookup.type == LookupSetType::INT_SET && v->has_number) {
                int64_t n = static_cast<int64_t>(v->number);
                matched = std::binary_search(lookup.ints.begin(), lookup.ints.end(), n);
            } else if (lookup.type == LookupSetType::IPV4_CIDR_SET) {
                uint32_t ip = v->has_number ? static_cast<uint32_t>(v->number) : parse_ipv4_text(v->text);
                matched = ipv4_ranges_contain_value(lookup.ipv4_ranges, ip);
            }
            return spec.op == OpType::NOT_IN_LOOKUP ? !matched : matched;
        } else if constexpr (std::is_same_v<T, CidrConditionSpec>) {
            const JsonAnyValue* v = any_find(row, spec.field);
            if (!v) return false;
            uint32_t network = spec.network;
            uint32_t mask = spec.mask;
            if (!spec.compiled) parse_cidr_text(spec.cidr, network, mask);
            uint32_t ip = v->has_number ? static_cast<uint32_t>(v->number) : parse_ipv4_text(v->text);
            bool matched = (ip & mask) == network;
            return spec.op == OpType::IP_NOT_IN_SUBNET ? !matched : matched;
        } else if constexpr (std::is_same_v<T, TemporalConditionSpec>) {
            const JsonAnyValue* v = any_find(row, spec.field);
            if (!v || !v->has_number) return false;
            int64_t ts = static_cast<int64_t>(v->number);
            bool matched = false;
            switch (spec.op) {
                case OpType::BEFORE:
                    matched = ts < spec.value;
                    break;
                case OpType::AFTER:
                    matched = ts > spec.value;
                    break;
                case OpType::WITHIN_LAST: {
                    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    matched = ts >= now_ms - spec.value * 1000;
                    break;
                }
                case OpType::DAY_OF_WEEK_IN: {
                    int64_t days = ts / 86400000LL;
                    int dow = static_cast<int>((days + 4) % 7);
                    if (dow < 0) dow += 7;
                    matched = std::find(spec.values.begin(), spec.values.end(), dow) != spec.values.end();
                    break;
                }
                case OpType::TIME_OF_DAY_BETWEEN: {
                    int64_t ms_day = ts % 86400000LL;
                    if (ms_day < 0) ms_day += 86400000LL;
                    double hour = static_cast<double>(ms_day) / 3600000.0;
                    matched = spec.lower <= spec.upper
                        ? (hour >= spec.lower && hour <= spec.upper)
                        : (hour >= spec.lower || hour <= spec.upper);
                    break;
                }
                default:
                    break;
            }
            return matched;
        } else if constexpr (std::is_same_v<T, GeoDistanceConditionSpec>) {
            const JsonAnyValue* lat = any_find(row, spec.lat_field);
            const JsonAnyValue* lon = any_find(row, spec.lon_field);
            const JsonAnyValue* other_lat = any_find(row, spec.other_lat_field);
            const JsonAnyValue* other_lon = any_find(row, spec.other_lon_field);
            if (!lat || !lon || !other_lat || !other_lon ||
                !lat->has_number || !lon->has_number ||
                !other_lat->has_number || !other_lon->has_number) {
                return false;
            }
            double lat1 = deg_to_rad(lat->number);
            double lon1 = deg_to_rad(lon->number);
            double lat2 = deg_to_rad(other_lat->number);
            double lon2 = deg_to_rad(other_lon->number);
            double dlat = lat2 - lat1;
            double dlon = lon2 - lon1;
            double x = dlon * std::cos((lat1 + lat2) * 0.5);
            double y = dlat;
            double dist_km = 6371.0088 * std::sqrt(x * x + y * y);
            if (dist_km > 1500.0) {
                double a = std::sin(dlat * 0.5) * std::sin(dlat * 0.5) +
                           std::cos(lat1) * std::cos(lat2) *
                           std::sin(dlon * 0.5) * std::sin(dlon * 0.5);
                dist_km = 6371.0088 * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
            }
            return spec.op == OpType::DISTANCE_LT
                ? dist_km < spec.threshold_km
                : dist_km > spec.threshold_km;
        } else if constexpr (std::is_same_v<T, ArithmeticConditionSpec>) {
            double lhs = any_eval_expr(spec.expr, row);
            double rhs = spec.other_field.empty()
                ? spec.threshold
                : (any_find(row, spec.other_field) && any_find(row, spec.other_field)->has_number
                    ? any_find(row, spec.other_field)->number : 0.0);
            return any_op_numeric(lhs, spec.op, rhs);
        } else if constexpr (std::is_same_v<T, AndConditionSpec>) {
            for (const auto& child : spec.child_condition) {
                if (!any_eval_condition(child, row, lookups)) return false;
            }
            return true;
        } else if constexpr (std::is_same_v<T, OrConditionSpec>) {
            for (const auto& child : spec.child_condition) {
                if (any_eval_condition(child, row, lookups)) return true;
            }
            return false;
        } else if constexpr (std::is_same_v<T, NotConditionSpec>) {
            return spec.child_condition.empty() ? true : !any_eval_condition(spec.child_condition.front(), row, lookups);
        } else {
            return false;
        }
    }, c.node);
}

std::shared_ptr<arrow::Buffer> wrap_string_data(const std::string& data) {
    return arrow::Buffer::Wrap(
        reinterpret_cast<const uint8_t*>(data.data()),
        static_cast<int64_t>(data.size()));
}

} // namespace

void BatchTransposer::ColumnBuffer::reset() {
    length = 0;
    null_count = 0;
    f32.clear();
    f64.clear();
    i32.clear();
    i64.clear();
    bool_bits.clear();
    validity_bits.clear();
    offsets.clear();
    string_data.clear();
    if (type == ColumnType::CATEGORICAL || type == ColumnType::ENTITY_KEY ||
        type == ColumnType::STRING) {
        offsets.push_back(0);
    }
}

void BatchTransposer::ColumnBuffer::reserve(int rows) {
    switch (type) {
        case ColumnType::FLOAT32: f32.reserve(rows); break;
        case ColumnType::FLOAT64: f64.reserve(rows); break;
        case ColumnType::INT32: i32.reserve(rows); break;
        case ColumnType::INT64:
        case ColumnType::TIMESTAMP_MS: i64.reserve(rows); break;
        case ColumnType::BOOLEAN: bool_bits.reserve((rows + 7) / 8); break;
        case ColumnType::STRING:
        case ColumnType::CATEGORICAL:
        case ColumnType::ENTITY_KEY:
            offsets.reserve(static_cast<size_t>(rows) + 1);
            string_data.reserve(static_cast<size_t>(rows) * 16);
            break;
    }
    validity_bits.reserve((rows + 7) / 8);
}

BatchTransposer::BatchTransposer(const BlazeRulesSchema& schema, arrow::MemoryPool* pool)
    : schema_(schema),
      pool_(pool),
      projected_(schema.num_fields(), 1) {
    make_buffers();
    rebuild_projected_indices();
}

void BatchTransposer::make_buffers() {
    columns_.clear();
    columns_.resize(schema_.num_fields());
    direct_encoded_.assign(schema_.num_fields(), 0);
    closed_value_ids_.clear();
    closed_value_ids_.resize(schema_.num_fields());
    field_to_index_.clear();
    field_to_index_.reserve(schema_.num_fields());
    std::vector<std::shared_ptr<arrow::Field>> arrow_fields;
    arrow_fields.reserve(schema_.num_fields());
    for (int i = 0; i < schema_.num_fields(); ++i) {
        const FieldSpec& field = schema_.field_at(i);
        bool direct_encoded = field.type == ColumnType::CATEGORICAL && !field.closed_values.empty();
        direct_encoded_[i] = direct_encoded
            ? (field.closed_values.size() < 63 ? 2 : 1)
            : 0;
        if (direct_encoded) {
            closed_value_ids_[i].reserve(field.closed_values.size());
            for (size_t id = 0; id < field.closed_values.size(); ++id) {
                closed_value_ids_[i].emplace(field.closed_values[id], static_cast<int32_t>(id));
            }
        }
        columns_[i].type = direct_encoded_[i] == 2
            ? ColumnType::INT64
            : (direct_encoded_[i] == 1 ? ColumnType::INT32 : field.type);
        columns_[i].reset();
        field_to_index_.emplace(field.name, i);
        std::shared_ptr<arrow::DataType> arrow_type = direct_encoded_[i] == 2
            ? arrow::int64()
            : (direct_encoded_[i] == 1 ? arrow::int32() : blazerules::ingest_arrow_type(field.type));
        arrow_fields.push_back(arrow::field(field.name, arrow_type, field.nullable));
    }
    arrow_schema_ = arrow::schema(std::move(arrow_fields));
    seen_generation_.assign(schema_.num_fields(), 0);
    current_size_ = 0;
    skipped_count_ = 0;
    row_generation_ = 1;
    seen_count_ = 0;
    last_error_.clear();
}

void BatchTransposer::reset() {
    for (auto& column : columns_) column.reset();
    current_size_ = 0;
    skipped_count_ = 0;
    std::fill(seen_generation_.begin(), seen_generation_.end(), 0);
    row_generation_ = 1;
    input_row_index_ = 0;
    seen_count_ = 0;
    last_error_.clear();
    error_counts_.clear();
    error_samples_.clear();
}

void BatchTransposer::reserve(int rows) {
    for (int i : projected_indices_) columns_[i].reserve(rows);
}

void BatchTransposer::set_max_error_samples(int max_samples) {
    max_error_samples_ = std::max(0, max_samples);
}

void BatchTransposer::set_projected_fields(std::vector<int> projected_indices) {
    projected_.assign(schema_.num_fields(), 0);
    if (projected_indices.empty()) {
        std::fill(projected_.begin(), projected_.end(), 1);
        rebuild_projected_indices();
        return;
    }
    for (int idx : projected_indices) {
        if (idx >= 0 && idx < schema_.num_fields()) projected_[idx] = 1;
    }
    rebuild_projected_indices();
}

void BatchTransposer::set_array_any_channels(std::vector<ArrayAnyChannelSpec> channels,
                                             LookupRegistry lookups) {
    array_any_channels_ = std::move(channels);
    array_any_lookups_ = std::move(lookups);
}

bool BatchTransposer::should_materialize(int col_index) const {
    return projected_.empty() || projected_[col_index] != 0;
}

void BatchTransposer::rebuild_projected_indices() {
    projected_indices_.clear();
    projected_indices_.reserve(schema_.num_fields());
    for (int i = 0; i < schema_.num_fields(); ++i) {
        if (should_materialize(i)) projected_indices_.push_back(i);
    }
}

bool BatchTransposer::mark_column_seen(int col_index) {
    if (seen_generation_[col_index] == row_generation_) return false;
    seen_generation_[col_index] = row_generation_;
    ++seen_count_;
    return true;
}

void BatchTransposer::record_error(std::string code,
                                   std::string message,
                                   std::string source,
                                   int64_t row_index,
                                   std::string column_name,
                                   bool skip_record) {
    if (skip_record) ++skipped_count_;
    last_error_ = message;
    ++error_counts_[code];
    if (max_error_samples_ > 0 &&
        error_samples_.size() < static_cast<size_t>(max_error_samples_)) {
        BatchErrorSample sample;
        sample.code = std::move(code);
        sample.message = std::move(message);
        sample.source = std::move(source);
        sample.row_index = row_index;
        sample.column_name = std::move(column_name);
        error_samples_.push_back(std::move(sample));
    }
}

void BatchTransposer::record_type_error(int col_index, int64_t row_index) {
    record_error("FIELD_TYPE_COERCION_FAILED",
                 "field type coercion failed",
                 "json",
                 row_index,
                 schema_.name_at(col_index),
                 false);
}

void BatchTransposer::append_null(int col_index) {
    ColumnBuffer& column = columns_[col_index];
    bool nullable = schema_.is_nullable(col_index);
    if (nullable) {
        ensure_bit_slot(column.validity_bits, column.length);
        ++column.null_count;
    }
    switch (column.type) {
        case ColumnType::FLOAT32: column.f32.push_back(0.0f); break;
        case ColumnType::FLOAT64: column.f64.push_back(0.0); break;
        case ColumnType::INT32: column.i32.push_back(0); break;
        case ColumnType::INT64:
        case ColumnType::TIMESTAMP_MS: column.i64.push_back(0); break;
        case ColumnType::BOOLEAN: set_bit(column.bool_bits, column.length, false); break;
        case ColumnType::STRING:
        case ColumnType::CATEGORICAL:
        case ColumnType::ENTITY_KEY: column.offsets.push_back(column.offsets.back()); break;
    }
    if (!nullable && !column.validity_bits.empty()) set_bit(column.validity_bits, column.length, true);
    ++column.length;
}

void BatchTransposer::append_int32_literal(int col_index, int32_t value) {
    ColumnBuffer& column = columns_[col_index];
    if (schema_.is_nullable(col_index)) {
        set_bit(column.validity_bits, column.length, true);
    }
    column.i32.push_back(value);
    ++column.length;
}

void BatchTransposer::append_default(int col_index) {
    append_null(col_index);
}

bool BatchTransposer::append_array_any_value(const std::string& path,
                                            simdjson::ondemand::value& value) {
    std::vector<const ArrayAnyChannelSpec*> channels;
    for (const auto& channel : array_any_channels_) {
        if (channel.path != path || channel.synthetic_col_index < 0 ||
            !should_materialize(channel.synthetic_col_index) ||
            seen_generation_[channel.synthetic_col_index] == row_generation_) {
            continue;
        }
        channels.push_back(&channel);
    }
    if (channels.empty()) return false;

    std::vector<int32_t> matched(channels.size(), 0);
    simdjson::ondemand::json_type type;
    if (!value.type().get(type) && type == simdjson::ondemand::json_type::array) {
        simdjson::ondemand::array arr;
        if (!value.get_array().get(arr)) {
            for (auto item_result : arr) {
                simdjson::ondemand::value item;
                if (std::move(item_result).get(item)) continue;
                simdjson::ondemand::json_type item_type;
                if (item.type().get(item_type) ||
                    item_type != simdjson::ondemand::json_type::object) {
                    continue;
                }
                simdjson::ondemand::object object;
                if (item.get_object().get(object)) continue;
                JsonAnyRow row;
                std::string prefix;
                collect_json_any_object(row, prefix, object, 0);
                bool all_done = true;
                for (size_t i = 0; i < channels.size(); ++i) {
                    if (matched[i]) continue;
                    if (any_eval_condition(channels[i]->where, row, array_any_lookups_)) {
                        matched[i] = 1;
                    }
                    all_done = all_done && matched[i] != 0;
                }
                if (all_done) break;
            }
        }
    }
    for (size_t i = 0; i < channels.size(); ++i) {
        mark_column_seen(channels[i]->synthetic_col_index);
        append_int32_literal(channels[i]->synthetic_col_index, matched[i]);
    }
    return true;
}

void BatchTransposer::append_json_value(int col_index, simdjson::ondemand::value& value) {
    ColumnBuffer& column = columns_[col_index];
    simdjson::ondemand::json_type json_type;
    bool has_type = !value.type().get(json_type);
    if (!has_type || json_type == simdjson::ondemand::json_type::null) {
        append_null(col_index);
        return;
    }

    auto mark_valid = [&]() {
        if (schema_.is_nullable(col_index)) {
            set_bit(column.validity_bits, column.length, true);
        }
    };

    if (direct_encoded_[col_index] == 2) {
        uint64_t mask = 0;
        std::string scratch;
        if (json_type == simdjson::ondemand::json_type::array) {
            simdjson::ondemand::array arr;
            if (!value.get_array().get(arr)) {
                for (auto item_result : arr) {
                    simdjson::ondemand::value item;
                    if (std::move(item_result).get(item)) continue;
                    std::string_view sv;
                    if (lookup_text_value(item, sv, scratch)) {
                        auto it = closed_value_ids_[col_index].find(sv);
                        if (it != closed_value_ids_[col_index].end() && it->second >= 0 && it->second < 63) {
                            mask |= uint64_t{1} << static_cast<unsigned>(it->second);
                        }
                    }
                }
            }
        } else {
            std::string_view sv;
            if (lookup_text_value(value, sv, scratch)) {
                auto it = closed_value_ids_[col_index].find(sv);
                if (it != closed_value_ids_[col_index].end() && it->second >= 0 && it->second < 63) {
                    mask = uint64_t{1} << static_cast<unsigned>(it->second);
                }
            } else if (schema_.is_nullable(col_index)) {
                append_null(col_index);
                return;
            }
        }
        mark_valid();
        column.i64.push_back(static_cast<int64_t>(mask));
        ++column.length;
        return;
    }

    if (direct_encoded_[col_index] == 1) {
        std::string_view sv;
        std::string scratch;
        int32_t id = -1;
        if (json_type == simdjson::ondemand::json_type::array) {
            simdjson::ondemand::array arr;
            if (!value.get_array().get(arr)) {
                for (auto item_result : arr) {
                    simdjson::ondemand::value item;
                    if (std::move(item_result).get(item)) continue;
                    if (lookup_text_value(item, sv, scratch)) {
                        auto it = closed_value_ids_[col_index].find(sv);
                        if (it != closed_value_ids_[col_index].end()) {
                            id = it->second;
                            break;
                        }
                    }
                }
            }
        } else if (lookup_text_value(value, sv, scratch)) {
            auto it = closed_value_ids_[col_index].find(sv);
            if (it != closed_value_ids_[col_index].end()) id = it->second;
        } else if (schema_.is_nullable(col_index)) {
            append_null(col_index);
            return;
        }
        mark_valid();
        column.i32.push_back(id);
        ++column.length;
        return;
    }

    auto append_first_numeric_from_array = [&](double& out) -> bool {
        simdjson::ondemand::array arr;
        if (value.get_array().get(arr)) return false;
        for (auto item_result : arr) {
            simdjson::ondemand::value item;
            if (std::move(item_result).get(item)) continue;
            if (!item.get(out)) return true;
        }
        return false;
    };

    switch (column.type) {
        case ColumnType::FLOAT32: {
            double v;
            bool ok = json_type == simdjson::ondemand::json_type::array
                ? append_first_numeric_from_array(v)
                : !value.get(v);
            if (!ok) {
                record_type_error(col_index, input_row_index_);
                append_null(col_index);
                return;
            }
            mark_valid();
            column.f32.push_back(static_cast<float>(v));
            break;
        }
        case ColumnType::FLOAT64: {
            double v;
            bool ok = json_type == simdjson::ondemand::json_type::array
                ? append_first_numeric_from_array(v)
                : !value.get(v);
            if (!ok) {
                record_type_error(col_index, input_row_index_);
                append_null(col_index);
                return;
            }
            mark_valid();
            column.f64.push_back(v);
            break;
        }
        case ColumnType::INT32: {
            int64_t v = 0;
            double dv = 0.0;
            bool ok;
            if (json_type == simdjson::ondemand::json_type::array) {
                ok = append_first_numeric_from_array(dv);
                v = static_cast<int64_t>(dv);
            } else {
                ok = !value.get(v);
                if (!ok && !value.get(dv)) {  // float literal (e.g. 12000.0) -> coerce to int
                    v = static_cast<int64_t>(dv);
                    ok = true;
                }
            }
            if (!ok) {
                record_type_error(col_index, input_row_index_);
                append_null(col_index);
                return;
            }
            mark_valid();
            column.i32.push_back(static_cast<int32_t>(v));
            break;
        }
        case ColumnType::INT64:
        case ColumnType::TIMESTAMP_MS: {
            int64_t v = 0;
            double dv = 0.0;
            bool ok;
            if (json_type == simdjson::ondemand::json_type::array) {
                ok = append_first_numeric_from_array(dv);
                v = static_cast<int64_t>(dv);
            } else {
                ok = !value.get(v);
                if (!ok && !value.get(dv)) {  // float literal -> coerce to int
                    v = static_cast<int64_t>(dv);
                    ok = true;
                }
            }
            if (!ok) {
                record_type_error(col_index, input_row_index_);
                append_null(col_index);
                return;
            }
            mark_valid();
            column.i64.push_back(v);
            break;
        }
        case ColumnType::BOOLEAN: {
            bool v;
            if (value.get(v)) {
                record_type_error(col_index, input_row_index_);
                append_null(col_index);
                return;
            }
            mark_valid();
            set_bit(column.bool_bits, column.length, v);
            break;
        }
        case ColumnType::STRING:
        case ColumnType::CATEGORICAL:
        case ColumnType::ENTITY_KEY: {
            std::string_view sv;
            if (value.get(sv)) {
                record_type_error(col_index, input_row_index_);
                append_null(col_index);
                return;
            }
            if (null_like_string(sv)) {
                append_null(col_index);
                return;
            }
            mark_valid();
            column.string_data.append(sv.data(), sv.size());
            column.offsets.push_back(static_cast<int32_t>(column.string_data.size()));
            break;
        }
    }
    ++column.length;
}

void BatchTransposer::append_json_value_if_new(int col_index, simdjson::ondemand::value& value) {
    if (col_index < 0 || !should_materialize(col_index) || !mark_column_seen(col_index)) return;
    append_json_value(col_index, value);
}

void BatchTransposer::append_json_object_fields(simdjson::ondemand::object& object, std::string& prefix) {
    size_t base_size = prefix.size();
    for (auto field_result : object) {
        simdjson::ondemand::field field;
        if (std::move(field_result).get(field)) continue;
        std::string_view key = field.escaped_key();
        prefix.resize(base_size);
        prefix.push_back('.');
        prefix.append(key.data(), key.size());

        simdjson::ondemand::value value = field.value();
        simdjson::ondemand::json_type json_type;
        bool has_type = !value.type().get(json_type);
        if (append_array_any_value(prefix, value)) continue;
        if (has_type && json_type == simdjson::ondemand::json_type::object) {
            simdjson::ondemand::object child;
            if (!value.get_object().get(child)) append_json_object_fields(child, prefix);
            continue;
        }

        auto it = field_to_index_.find(prefix);
        if (it != field_to_index_.end()) append_json_value_if_new(it->second, value);
    }
    prefix.resize(base_size);
}

template <typename DocumentLike>
bool BatchTransposer::append_json_document_single_pass(DocumentLike& doc) {
    struct ColumnSnapshot {
        int col_index = -1;
        int64_t length = 0;
        int64_t null_count = 0;
        size_t f32 = 0;
        size_t f64 = 0;
        size_t i32 = 0;
        size_t i64 = 0;
        size_t bool_bits = 0;
        size_t validity_bits = 0;
        size_t offsets = 0;
        size_t string_data = 0;
    };
    int starting_size = current_size_;
    std::vector<ColumnSnapshot> snapshots;
    snapshots.reserve(projected_indices_.size());
    for (int col_index : projected_indices_) {
        const ColumnBuffer& column = columns_[col_index];
        snapshots.push_back({col_index, column.length, column.null_count,
                             column.f32.size(), column.f64.size(), column.i32.size(),
                             column.i64.size(), column.bool_bits.size(),
                             column.validity_bits.size(), column.offsets.size(),
                             column.string_data.size()});
    }

    auto advance_skipped_generation = [&]() {
        ++row_generation_;
        if (row_generation_ == 0) {
            std::fill(seen_generation_.begin(), seen_generation_.end(), 0);
            row_generation_ = 1;
        }
        ++input_row_index_;
    };

    auto abort_malformed_row = [&](const std::string& message, const char* source,
                                   const std::string& column) {
        current_size_ = starting_size;
        for (const ColumnSnapshot& snapshot : snapshots) {
            ColumnBuffer& column_buffer = columns_[snapshot.col_index];
            column_buffer.length = snapshot.length;
            column_buffer.null_count = snapshot.null_count;
            column_buffer.f32.resize(snapshot.f32);
            column_buffer.f64.resize(snapshot.f64);
            column_buffer.i32.resize(snapshot.i32);
            column_buffer.i64.resize(snapshot.i64);
            column_buffer.bool_bits.resize(snapshot.bool_bits);
            column_buffer.validity_bits.resize(snapshot.validity_bits);
            column_buffer.offsets.resize(snapshot.offsets);
            column_buffer.string_data.resize(snapshot.string_data);
        }
        record_error("MALFORMED_JSON", message, source, input_row_index_, column, true);
        advance_skipped_generation();
    };

    auto malformed_field_message = [](const std::string& last_key) {
        return last_key.empty() ? std::string("invalid JSON: could not read object fields")
                                : "invalid JSON near field '" + last_key + "'";
    };

    simdjson::ondemand::object object;
    if (doc.get_object().get(object)) {
        abort_malformed_row("record is not a JSON object", "json", {});
        return false;
    }

    bool learning_order = learned_field_order_.empty();

    if (!learning_order && layout_is_flat_) {
        seen_count_ = 0;
        const int projected_total = static_cast<int>(projected_indices_.size());
        size_t field_position = 0;
        std::string last_key;
        for (auto field_result : object) {
            simdjson::ondemand::field field;
            if (std::move(field_result).get(field)) {
                abort_malformed_row(malformed_field_message(last_key), "json", last_key);
                return false;
            }
            std::string_view key = field.escaped_key();
            last_key.assign(key.data(), key.size());
            int col_index;
            if (field_position < learned_field_keys_.size() &&
                key == learned_field_keys_[field_position]) {
                col_index = learned_field_order_[field_position];
            } else {
                auto it = field_to_index_.find(key);
                col_index = (it == field_to_index_.end()) ? -1 : it->second;
            }
            ++field_position;
            simdjson::ondemand::value value = field.value();
            if (append_array_any_value(std::string(key), value)) {
                if (seen_count_ >= projected_total) break;
                continue;
            }
            if (col_index < 0) continue;
            if (!should_materialize(col_index) || !mark_column_seen(col_index)) continue;
            append_json_value(col_index, value);
            if (seen_count_ >= projected_total) break;
        }
        if (seen_count_ < projected_total) {
            for (int col_index : projected_indices_) {
                if (seen_generation_[col_index] != row_generation_) append_default(col_index);
            }
        }
        ++current_size_;
        ++row_generation_;
        if (row_generation_ == 0) {
            std::fill(seen_generation_.begin(), seen_generation_.end(), 0);
            row_generation_ = 1;
        }
        ++input_row_index_;
        return true;
    }

    std::vector<int> learned_this_doc;
    std::vector<std::string> learned_keys_this_doc;
    if (learning_order) {
        learned_this_doc.reserve(schema_.num_fields());
        learned_keys_this_doc.reserve(schema_.num_fields());
    }
    seen_count_ = 0;
    size_t field_position = 0;
    std::string last_key;
    for (auto field_result : object) {
        simdjson::ondemand::field field;
        if (std::move(field_result).get(field)) {
            abort_malformed_row(malformed_field_message(last_key), "json", last_key);
            return false;
        }
        std::string_view key = field.escaped_key();
        last_key.assign(key.data(), key.size());
        simdjson::ondemand::value value = field.value();
        simdjson::ondemand::json_type json_type;
        bool has_type = !value.type().get(json_type);

        if (append_array_any_value(std::string(key), value)) {
            ++field_position;
            continue;
        }

        if (has_type && json_type == simdjson::ondemand::json_type::object) {
            std::string_view key = field.escaped_key();
            if (learning_order) {
                auto it = field_to_index_.find(key);
                learned_this_doc.push_back(it == field_to_index_.end() ? -1 : it->second);
                learned_keys_this_doc.push_back(std::string(key));
            }
            simdjson::ondemand::object child;
            if (!value.get_object().get(child)) {
                std::string prefix(key);
                append_json_object_fields(child, prefix);
            }
            ++field_position;
            continue;
        }

        int col_index = -1;
        if (!learning_order && field_position < learned_field_keys_.size() &&
            key == learned_field_keys_[field_position]) {
            col_index = learned_field_order_[field_position];
        } else {
            auto it = field_to_index_.find(key);
            if (it != field_to_index_.end()) col_index = it->second;
        }
        if (learning_order) {
            learned_this_doc.push_back(col_index);
            learned_keys_this_doc.push_back(std::string(key));
        }
        append_json_value_if_new(col_index, value);
        ++field_position;
    }
    if (learning_order) {
        learned_field_order_ = std::move(learned_this_doc);
        learned_field_keys_ = std::move(learned_keys_this_doc);
    }
    if (seen_count_ < static_cast<int>(projected_indices_.size())) {
        for (int col_index : projected_indices_) {
            if (seen_generation_[col_index] != row_generation_) append_default(col_index);
        }
    }
    ++current_size_;
    ++row_generation_;
    if (row_generation_ == 0) {
        std::fill(seen_generation_.begin(), seen_generation_.end(), 0);
        row_generation_ = 1;
    }
    ++input_row_index_;
    return true;
}

template <typename DocumentLike>
bool BatchTransposer::append_json_document(DocumentLike& doc) {
    return append_json_document_single_pass(doc);
}

void BatchTransposer::add_json_message(std::string_view json_bytes) {
    padded_json_.assign(json_bytes.data(), json_bytes.size());
    padded_json_.resize(json_bytes.size() + simdjson::SIMDJSON_PADDING, '\0');
    simdjson::ondemand::document doc;
    if (parser_.iterate(padded_json_.data(), json_bytes.size(), padded_json_.size()).get(doc)) {
        record_error("MALFORMED_JSON", "malformed json", "json",
                     input_row_index_, {}, true);
        ++input_row_index_;
        return;
    }
    append_json_document(doc);
}

void BatchTransposer::add_json_messages(const std::vector<std::string_view>& messages) {
    if (messages.empty()) return;

    size_t payload_size = 0;
    for (std::string_view m : messages) payload_size += m.size() + 1;
    batch_json_.clear();
    batch_json_.reserve(payload_size + simdjson::SIMDJSON_PADDING);
    for (std::string_view m : messages) {
        batch_json_.append(m.data(), m.size());
        batch_json_.push_back('\n');
    }
    batch_json_.resize(payload_size + simdjson::SIMDJSON_PADDING, '\0');
    parse_ndjson_parallel(std::string_view(batch_json_.data(), payload_size), 0);
}

void BatchTransposer::add_ndjson(std::string_view ndjson_bytes, int thread_count) {
    if (ndjson_bytes.empty()) return;
    batch_json_.assign(ndjson_bytes.data(), ndjson_bytes.size());
    batch_json_.resize(ndjson_bytes.size() + simdjson::SIMDJSON_PADDING, '\0');
    parse_ndjson_parallel(std::string_view(batch_json_.data(), ndjson_bytes.size()), thread_count);
}

void BatchTransposer::add_ndjson_padded(std::string_view ndjson_bytes, int thread_count) {
    if (ndjson_bytes.empty()) return;
    parse_ndjson_parallel(ndjson_bytes, thread_count);
}

void BatchTransposer::parse_ndjson_view(std::string_view ndjson_bytes) {
    if (ndjson_bytes.empty()) return;
    simdjson::ondemand::document_stream docs;
    bool dirty = false;
    int64_t appended = 0;
    if (parser_.iterate_many(ndjson_bytes.data(), ndjson_bytes.size(),
                             simdjson::ondemand::DEFAULT_BATCH_SIZE).get(docs)) {
        dirty = true;
    } else {
        for (auto it = docs.begin(); it != docs.end(); ++it) {
            simdjson::ondemand::document_reference doc;
            if ((*it).get(doc)) { dirty = true; break; }
            if (!append_json_document(doc)) { dirty = true; break; }
            ++appended;
        }
    }
    if (!dirty) {
        int64_t lines = 0;
        for (char c : ndjson_bytes) {
            if (c == '\n') ++lines;
        }
        if (ndjson_bytes.back() != '\n') ++lines;
        if (appended == lines) return;
    }
    reset();
    parse_ndjson_lines_safe(ndjson_bytes);
}

void BatchTransposer::parse_ndjson_lines_safe(std::string_view ndjson_bytes) {
    size_t pos = 0;
    const size_t n = ndjson_bytes.size();
    while (pos < n) {
        const size_t nl = ndjson_bytes.find('\n', pos);
        const size_t end = (nl == std::string_view::npos) ? n : nl;
        std::string_view line = ndjson_bytes.substr(pos, end - pos);
        pos = (nl == std::string_view::npos) ? n : nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                                 line.back() == '\t')) {
            line.remove_suffix(1);
        }
        if (line.empty()) continue;
        add_json_message(line);
    }
}

void BatchTransposer::learn_field_order_from_ndjson(std::string_view ndjson_bytes) {
    if (!learned_field_order_.empty() || ndjson_bytes.empty()) return;

    simdjson::ondemand::document_stream docs;
    if (parser_.iterate_many(ndjson_bytes.data(), ndjson_bytes.size(),
                             simdjson::ondemand::DEFAULT_BATCH_SIZE).get(docs)) {
        return;
    }

    for (auto doc_result : docs) {
        simdjson::ondemand::document_reference doc;
        if (std::move(doc_result).get(doc)) return;
        simdjson::ondemand::object object;
        if (doc.get_object().get(object)) return;

        std::vector<int> learned;
        std::vector<std::string> learned_keys;
        learned.reserve(schema_.num_fields());
        learned_keys.reserve(schema_.num_fields());
        bool any_object = false;
        for (auto field_result : object) {
            simdjson::ondemand::field field;
            if (std::move(field_result).get(field)) return;
            std::string_view key = field.escaped_key();
            auto it = field_to_index_.find(key);
            learned.push_back(it == field_to_index_.end() ? -1 : it->second);
            learned_keys.push_back(std::string(key));
            simdjson::ondemand::value value = field.value();
            simdjson::ondemand::json_type jt;
            if (!value.type().get(jt) && jt == simdjson::ondemand::json_type::object) {
                any_object = true;
            }
        }
        learned_field_order_ = std::move(learned);
        learned_field_keys_ = std::move(learned_keys);
        layout_is_flat_ = !any_object;
        return;
    }
}

std::vector<std::pair<size_t, size_t>> BatchTransposer::split_ndjson(
        std::string_view ndjson_bytes, int thread_count) const {
    std::vector<std::pair<size_t, size_t>> chunks;
    if (thread_count <= 1 || ndjson_bytes.size() < kParallelJsonThresholdBytes) {
        chunks.emplace_back(0, ndjson_bytes.size());
        return chunks;
    }

    thread_count = std::min(thread_count, static_cast<int>(ndjson_bytes.size() / kParallelJsonThresholdBytes) + 1);
    size_t start = 0;
    for (int i = 1; i <= thread_count; ++i) {
        size_t end = (i == thread_count) ? ndjson_bytes.size() : (ndjson_bytes.size() * i / thread_count);
        if (end < ndjson_bytes.size()) {
            size_t newline = ndjson_bytes.find('\n', end);
            end = newline == std::string_view::npos ? ndjson_bytes.size() : newline + 1;
        }
        if (end > start) chunks.emplace_back(start, end);
        start = end;
        if (start >= ndjson_bytes.size()) break;
    }
    return chunks;
}

void BatchTransposer::parse_ndjson_parallel(std::string_view ndjson_bytes, int thread_count) {
    if (thread_count <= 0) thread_count = default_thread_count();
    learn_field_order_from_ndjson(ndjson_bytes);
    auto chunks = split_ndjson(ndjson_bytes, thread_count);
    if (chunks.size() <= 1) {
        parse_ndjson_view(ndjson_bytes);
        return;
    }

    std::vector<std::unique_ptr<BatchTransposer>> workers;
    workers.reserve(chunks.size());
    for (const auto& [start, end] : chunks) {
        auto worker = std::make_unique<BatchTransposer>(schema_, pool_);
        worker->set_projected_fields(projected_indices_);
        worker->set_max_error_samples(max_error_samples_);
        worker->set_array_any_channels(array_any_channels_, array_any_lookups_);
        worker->learned_field_order_ = learned_field_order_;
        worker->learned_field_keys_ = learned_field_keys_;
        worker->layout_is_flat_ = layout_is_flat_;
        worker->reserve(estimate_rows_for_chunk(ndjson_bytes, start, end));
        workers.push_back(std::move(worker));
    }

    std::vector<std::thread> threads;
    threads.reserve(chunks.size());
    for (size_t i = 0; i < chunks.size(); ++i) {
        auto [start, end] = chunks[i];
        BatchTransposer* worker = workers[i].get();
        threads.emplace_back([worker, ndjson_bytes, start, end]() {
            worker->parse_ndjson_view(ndjson_bytes.substr(start, end - start));
        });
    }
    for (auto& thread : threads) thread.join();
    int64_t row_base = input_row_index_;
    for (auto& worker : workers) {
        for (auto& sample : worker->error_samples_) sample.row_index += row_base;
        int64_t consumed = worker->input_row_index_;
        merge_from(*worker);
        row_base += consumed;
    }
    input_row_index_ = row_base;
}

void BatchTransposer::merge_from(BatchTransposer& other) {
    auto merge_validity = [](ColumnBuffer& dst, const ColumnBuffer& src) {
        if (src.length == 0) return;
        bool needs_bitmap = dst.null_count > 0 || src.null_count > 0 ||
                            !dst.validity_bits.empty() || !src.validity_bits.empty();
        if (!needs_bitmap) return;

        int64_t old_length = dst.length;
        if (dst.validity_bits.empty()) {
            for (int64_t row = 0; row < old_length; ++row) set_bit(dst.validity_bits, row, true);
        }
        if (src.validity_bits.empty()) {
            for (int64_t row = 0; row < src.length; ++row) {
                set_bit(dst.validity_bits, old_length + row, true);
            }
        } else {
            for (int64_t row = 0; row < src.length; ++row) {
                bool valid = (src.validity_bits[static_cast<size_t>(row >> 3)] >> (row & 7)) & 1u;
                set_bit(dst.validity_bits, old_length + row, valid);
            }
        }
        dst.null_count += src.null_count;
    };

    for (int i : projected_indices_) {
        ColumnBuffer& dst = columns_[i];
        ColumnBuffer& src = other.columns_[i];
        if (dst.length == 0) {
            dst.f32 = std::move(src.f32);
            dst.f64 = std::move(src.f64);
            dst.i32 = std::move(src.i32);
            dst.i64 = std::move(src.i64);
            dst.bool_bits = std::move(src.bool_bits);
            dst.validity_bits = std::move(src.validity_bits);
            dst.offsets = std::move(src.offsets);
            dst.string_data = std::move(src.string_data);
            dst.length = src.length;
            dst.null_count = src.null_count;
            continue;
        }
        merge_validity(dst, src);
        switch (dst.type) {
            case ColumnType::FLOAT32:
                dst.f32.insert(dst.f32.end(), src.f32.begin(), src.f32.end());
                break;
            case ColumnType::FLOAT64:
                dst.f64.insert(dst.f64.end(), src.f64.begin(), src.f64.end());
                break;
            case ColumnType::INT32:
                dst.i32.insert(dst.i32.end(), src.i32.begin(), src.i32.end());
                break;
            case ColumnType::INT64:
            case ColumnType::TIMESTAMP_MS:
                dst.i64.insert(dst.i64.end(), src.i64.begin(), src.i64.end());
                break;
            case ColumnType::BOOLEAN: {
                int64_t old_length = dst.length;
                for (int64_t row = 0; row < src.length; ++row) {
                    bool value = (src.bool_bits[static_cast<size_t>(row >> 3)] >> (row & 7)) & 1;
                    set_bit(dst.bool_bits, old_length + row, value);
                }
                break;
            }
            case ColumnType::STRING:
            case ColumnType::CATEGORICAL:
            case ColumnType::ENTITY_KEY: {
                int32_t base = static_cast<int32_t>(dst.string_data.size());
                dst.string_data.append(src.string_data);
                for (size_t o = 1; o < src.offsets.size(); ++o) {
                    dst.offsets.push_back(base + src.offsets[o]);
                }
                break;
            }
        }
        dst.length += src.length;
    }
    current_size_ += other.current_size_;
    skipped_count_ += other.skipped_count_;
    for (const auto& [code, count] : other.error_counts_) error_counts_[code] += count;
    for (const auto& sample : other.error_samples_) {
        if (max_error_samples_ <= 0 ||
            error_samples_.size() >= static_cast<size_t>(max_error_samples_)) {
            break;
        }
        error_samples_.push_back(sample);
    }
    if (!other.last_error_.empty()) last_error_ = other.last_error_;
    if (learned_field_order_.empty() && !other.learned_field_order_.empty()) {
        learned_field_order_ = other.learned_field_order_;
        learned_field_keys_ = other.learned_field_keys_;
        layout_is_flat_ = other.layout_is_flat_;
    }
}

std::shared_ptr<arrow::Array> BatchTransposer::finish_unprojected_column(int col_index) const {
    auto type = direct_encoded_[col_index] == 2
        ? arrow::int64()
        : (direct_encoded_[col_index] == 1 ? arrow::int32() : blazerules::ingest_arrow_type(schema_.type_of(col_index)));
    return arrow::MakeArrayOfNull(type, current_size_, pool_).ValueOrDie();
}

std::shared_ptr<arrow::Array> BatchTransposer::finish_column(int col_index) {
    ColumnBuffer& column = columns_[col_index];
    while (column.length < current_size_) append_default(col_index);

    std::shared_ptr<arrow::Buffer> validity =
        column.null_count > 0
            ? arrow::Buffer::Wrap(column.validity_bits)
            : nullptr;
    std::shared_ptr<arrow::ArrayData> data;
    switch (column.type) {
        case ColumnType::FLOAT32:
            data = arrow::ArrayData::Make(arrow::float32(), current_size_,
                                          {validity, arrow::Buffer::Wrap(column.f32)},
                                          column.null_count);
            break;
        case ColumnType::FLOAT64:
            data = arrow::ArrayData::Make(arrow::float64(), current_size_,
                                          {validity, arrow::Buffer::Wrap(column.f64)},
                                          column.null_count);
            break;
        case ColumnType::INT32:
            data = arrow::ArrayData::Make(arrow::int32(), current_size_,
                                          {validity, arrow::Buffer::Wrap(column.i32)},
                                          column.null_count);
            break;
        case ColumnType::INT64:
        case ColumnType::TIMESTAMP_MS:
            data = arrow::ArrayData::Make(arrow::int64(), current_size_,
                                          {validity, arrow::Buffer::Wrap(column.i64)},
                                          column.null_count);
            break;
        case ColumnType::BOOLEAN:
            data = arrow::ArrayData::Make(arrow::boolean(), current_size_,
                                          {validity, arrow::Buffer::Wrap(column.bool_bits)},
                                          column.null_count);
            break;
        case ColumnType::STRING:
        case ColumnType::CATEGORICAL:
        case ColumnType::ENTITY_KEY:
            data = arrow::ArrayData::Make(arrow::utf8(), current_size_,
                                          {validity, arrow::Buffer::Wrap(column.offsets),
                                           wrap_string_data(column.string_data)},
                                          column.null_count);
            break;
    }
    return arrow::MakeArray(data);
}

std::shared_ptr<arrow::RecordBatch> BatchTransposer::finish() {
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    arrays.reserve(columns_.size());
    for (int i = 0; i < static_cast<int>(columns_.size()); ++i) {
        if (!should_materialize(i)) arrays.push_back(finish_unprojected_column(i));
        else arrays.push_back(finish_column(i));
    }
    return arrow::RecordBatch::Make(arrow_schema_, current_size_, arrays);
}
