#include "blazerules/engine.h"

#include "blazerules/arrow_util.h"
#include "blazerules/compiler.h"
#include "blazerules/dsl_parser.h"
#include "blazerules/mmap_reader.h"
#include "blazerules/orchestrator.h"
#include "blazerules/schema_inference.h"
#include "blazerules/simd_kernels.h"
#include "blazerules/vector_channels.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>

#include <arrow/io/file.h>
#include <arrow/table.h>
#include <arrow/util/bit_util.h>
#include <re2/re2.h>
#include <parquet/arrow/reader.h>

namespace {

int64_t micros_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
}

int64_t epoch_millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void accumulate_diff(const BatchResult& a, const BatchResult& b,
                     int64_t& a_only, int64_t& b_only, int64_t& both) {
    size_t i = 0, j = 0;
    while (i < a.matched_record_indices.size() || j < b.matched_record_indices.size()) {
        if (j >= b.matched_record_indices.size() ||
            (i < a.matched_record_indices.size() && a.matched_record_indices[i] < b.matched_record_indices[j])) {
            ++a_only; ++i;
        } else if (i >= a.matched_record_indices.size() ||
                   b.matched_record_indices[j] < a.matched_record_indices[i]) {
            ++b_only; ++j;
        } else {
            ++both; ++i; ++j;
        }
    }
}

std::vector<std::string> expand_parquet_paths(const std::vector<std::string>& input) {
    std::vector<std::string> files;
    for (const auto& p : input) {
        std::filesystem::path path(p);
        if (std::filesystem::is_directory(path)) {
            for (const auto& entry : std::filesystem::directory_iterator(path)) {
                if (entry.path().extension() == ".parquet") files.push_back(entry.path().string());
            }
        } else {
            files.push_back(p);
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

BlazeRulesError with_domain(BlazeRulesError error, BlazeRulesError::Domain domain) {
    error.domain = domain;
    return error;
}

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(ch));
                    out += buf;
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    return out;
}

bool arrow_type_compatible(ColumnType expected, arrow::Type::type actual) {
    switch (expected) {
        case ColumnType::FLOAT32: return actual == arrow::Type::FLOAT;
        case ColumnType::FLOAT64: return actual == arrow::Type::DOUBLE;
        case ColumnType::INT32: return actual == arrow::Type::INT32;
        case ColumnType::INT64: return actual == arrow::Type::INT64;
        case ColumnType::TIMESTAMP_MS:
            return actual == arrow::Type::INT64 || actual == arrow::Type::TIMESTAMP;
        case ColumnType::BOOLEAN: return actual == arrow::Type::BOOL;
        case ColumnType::STRING: return actual == arrow::Type::STRING || actual == arrow::Type::LARGE_STRING;
        case ColumnType::CATEGORICAL:
        case ColumnType::ENTITY_KEY:
            return actual == arrow::Type::STRING || actual == arrow::Type::LARGE_STRING ||
                   actual == arrow::Type::DICTIONARY ||
                   actual == arrow::Type::INT32 || actual == arrow::Type::INT64;
    }
    return false;
}

std::vector<std::string> split_dotted_path(std::string_view path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t dot = path.find('.', start);
        size_t end = dot == std::string_view::npos ? path.size() : dot;
        if (end > start) parts.emplace_back(path.substr(start, end - start));
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return parts;
}

std::shared_ptr<arrow::Array> apply_parent_validity(
        const std::shared_ptr<arrow::Array>& child,
        const std::vector<std::shared_ptr<arrow::Array>>& parents) {
    if (!child) return nullptr;
    bool has_parent_nulls = false;
    for (const auto& parent : parents) {
        if (parent && parent->null_count() != 0) {
            has_parent_nulls = true;
            break;
        }
    }
    if (!has_parent_nulls) return child;

    const int64_t offset = child->offset();
    const int64_t length = child->length();
    auto bitmap = arrow::AllocateBitmap(offset + length).ValueOrDie();
    std::memset(bitmap->mutable_data(), 0, static_cast<size_t>(bitmap->size()));
    int64_t null_count = 0;
    for (int64_t i = 0; i < length; ++i) {
        bool valid = child->IsValid(i);
        for (const auto& parent : parents) {
            if (parent && !parent->IsValid(i)) {
                valid = false;
                break;
            }
        }
        if (valid) arrow::bit_util::SetBit(bitmap->mutable_data(), offset + i);
        else ++null_count;
    }
    auto data = child->data()->Copy();
    if (data->buffers.empty()) data->buffers.resize(1);
    data->buffers[0] = std::move(bitmap);
    data->null_count = null_count;
    return arrow::MakeArray(data);
}

std::shared_ptr<arrow::Array> resolve_dotted_child_array(
        const std::shared_ptr<arrow::Array>& root,
        const std::vector<std::string>& parts,
        size_t pos,
        std::vector<std::shared_ptr<arrow::Array>>& parents) {
    if (!root) return nullptr;
    if (pos >= parts.size()) return apply_parent_validity(root, parents);
    if (root->type_id() != arrow::Type::STRUCT) return nullptr;
    auto struct_array = std::static_pointer_cast<arrow::StructArray>(root);
    auto struct_type = std::static_pointer_cast<arrow::StructType>(root->type());
    int child_index = struct_type->GetFieldIndex(parts[pos]);
    if (child_index < 0) return nullptr;
    parents.push_back(root);
    auto child = struct_array->field(child_index);
    auto projected = resolve_dotted_child_array(child, parts, pos + 1, parents);
    parents.pop_back();
    return projected;
}

std::shared_ptr<arrow::Array> resolve_dotted_array(
        const std::shared_ptr<arrow::RecordBatch>& batch,
        std::string_view name) {
    if (!batch) return nullptr;
    int exact = batch->schema()->GetFieldIndex(std::string(name));
    if (exact >= 0) return batch->column(exact);
    auto parts = split_dotted_path(name);
    if (parts.empty()) return nullptr;
    int top = batch->schema()->GetFieldIndex(parts.front());
    if (top < 0) return nullptr;
    std::vector<std::shared_ptr<arrow::Array>> parents;
    return resolve_dotted_child_array(batch->column(top), parts, 1, parents);
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

struct ArrowAnyValue {
    bool exists = false;
    bool is_null = false;
    bool has_number = false;
    double number = 0.0;
    bool has_bool = false;
    bool bool_value = false;
    std::string text;
    std::vector<std::string> texts;
};

bool arrow_numeric_value(const std::shared_ptr<arrow::Array>& array,
                         int64_t row,
                         double& out) {
    switch (array->type_id()) {
        case arrow::Type::INT8:
            out = std::static_pointer_cast<arrow::Int8Array>(array)->Value(row);
            return true;
        case arrow::Type::INT16:
            out = std::static_pointer_cast<arrow::Int16Array>(array)->Value(row);
            return true;
        case arrow::Type::INT32:
            out = std::static_pointer_cast<arrow::Int32Array>(array)->Value(row);
            return true;
        case arrow::Type::INT64:
        case arrow::Type::TIMESTAMP:
            out = static_cast<double>(std::static_pointer_cast<arrow::Int64Array>(array)->Value(row));
            return true;
        case arrow::Type::UINT8:
            out = std::static_pointer_cast<arrow::UInt8Array>(array)->Value(row);
            return true;
        case arrow::Type::UINT16:
            out = std::static_pointer_cast<arrow::UInt16Array>(array)->Value(row);
            return true;
        case arrow::Type::UINT32:
            out = std::static_pointer_cast<arrow::UInt32Array>(array)->Value(row);
            return true;
        case arrow::Type::UINT64:
            out = static_cast<double>(std::static_pointer_cast<arrow::UInt64Array>(array)->Value(row));
            return true;
        case arrow::Type::FLOAT:
            out = std::static_pointer_cast<arrow::FloatArray>(array)->Value(row);
            return true;
        case arrow::Type::DOUBLE:
            out = std::static_pointer_cast<arrow::DoubleArray>(array)->Value(row);
            return true;
        case arrow::Type::BOOL:
            out = std::static_pointer_cast<arrow::BooleanArray>(array)->Value(row) ? 1.0 : 0.0;
            return true;
        default:
            return false;
    }
}

std::string arrow_string_value(const std::shared_ptr<arrow::Array>& array, int64_t row) {
    if (array->type_id() == arrow::Type::STRING) {
        auto str = std::static_pointer_cast<arrow::StringArray>(array);
        auto view = str->GetView(row);
        return std::string(view.data(), view.size());
    }
    if (array->type_id() == arrow::Type::LARGE_STRING) {
        auto str = std::static_pointer_cast<arrow::LargeStringArray>(array);
        auto view = str->GetView(row);
        return std::string(view.data(), view.size());
    }
    if (array->type_id() == arrow::Type::DICTIONARY) {
        auto dict = std::static_pointer_cast<arrow::DictionaryArray>(array);
        double idx_d = 0.0;
        if (!arrow_numeric_value(dict->indices(), row, idx_d)) return {};
        int64_t idx = static_cast<int64_t>(idx_d);
        const auto& dictionary = dict->dictionary();
        if (!dictionary || idx < 0 || idx >= dictionary->length() || dictionary->IsNull(idx)) return {};
        return arrow_string_value(dictionary, idx);
    }
    double number = 0.0;
    if (arrow_numeric_value(array, row, number)) return std::to_string(number);
    return {};
}

void append_arrow_list_values(const std::shared_ptr<arrow::Array>& values,
                              int64_t start,
                              int64_t length,
                              ArrowAnyValue& out) {
    for (int64_t i = 0; i < length; ++i) {
        int64_t row = start + i;
        if (values->IsNull(row)) continue;
        if (values->type_id() == arrow::Type::STRING ||
            values->type_id() == arrow::Type::LARGE_STRING ||
            values->type_id() == arrow::Type::DICTIONARY) {
            out.texts.push_back(arrow_string_value(values, row));
            continue;
        }
        double number = 0.0;
        if (arrow_numeric_value(values, row, number)) {
            out.texts.push_back(std::to_string(number));
        }
    }
}

std::shared_ptr<arrow::Array> resolve_field_from_struct(
        const std::shared_ptr<arrow::StructArray>& root,
        const std::vector<std::string>& parts,
        int64_t row) {
    std::shared_ptr<arrow::Array> current = root;
    for (const auto& part : parts) {
        if (!current || current->type_id() != arrow::Type::STRUCT || current->IsNull(row)) {
            return nullptr;
        }
        auto struct_array = std::static_pointer_cast<arrow::StructArray>(current);
        auto struct_type = std::static_pointer_cast<arrow::StructType>(current->type());
        int child_index = struct_type->GetFieldIndex(part);
        if (child_index < 0) return nullptr;
        current = struct_array->field(child_index);
    }
    return current;
}

ArrowAnyValue arrow_value_from_struct(const std::shared_ptr<arrow::StructArray>& root,
                                      std::string_view field,
                                      int64_t row) {
    ArrowAnyValue out;
    auto parts = split_dotted_path(field);
    auto array = resolve_field_from_struct(root, parts, row);
    if (!array) return out;
    out.exists = true;
    if (array->IsNull(row)) {
        out.is_null = true;
        return out;
    }
    double number = 0.0;
    if (arrow_numeric_value(array, row, number)) {
        out.has_number = true;
        out.number = number;
        out.text = std::to_string(number);
        if (array->type_id() == arrow::Type::BOOL) {
            out.has_bool = true;
            out.bool_value = number != 0.0;
            out.text = out.bool_value ? "true" : "false";
        }
        return out;
    }
    if (array->type_id() == arrow::Type::STRING ||
        array->type_id() == arrow::Type::LARGE_STRING ||
        array->type_id() == arrow::Type::DICTIONARY) {
        out.text = arrow_string_value(array, row);
        return out;
    }
    if (array->type_id() == arrow::Type::LIST) {
        auto list = std::static_pointer_cast<arrow::ListArray>(array);
        append_arrow_list_values(list->values(), list->value_offset(row),
                                 list->value_length(row), out);
        return out;
    }
    if (array->type_id() == arrow::Type::LARGE_LIST) {
        auto list = std::static_pointer_cast<arrow::LargeListArray>(array);
        append_arrow_list_values(list->values(), list->value_offset(row),
                                 list->value_length(row), out);
    }
    return out;
}

bool any_op_numeric(double lhs, OpType op, double rhs) {
    switch (op) {
        case OpType::GT:
        case OpType::GT_FIELD: return lhs > rhs;
        case OpType::LT:
        case OpType::LT_FIELD: return lhs < rhs;
        case OpType::GTE:
        case OpType::GTE_FIELD: return lhs >= rhs;
        case OpType::LTE:
        case OpType::LTE_FIELD: return lhs <= rhs;
        case OpType::EQ:
        case OpType::EQ_FIELD: return lhs == rhs;
        case OpType::NEQ:
        case OpType::NEQ_FIELD: return lhs != rhs;
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
            return lhs.size() >= rhs.size() && lhs.substr(lhs.size() - rhs.size()) == rhs;
        case OpType::CI_EQ:
            if (lhs.size() != rhs.size()) return false;
            for (size_t i = 0; i < lhs.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
                    std::tolower(static_cast<unsigned char>(rhs[i]))) {
                    return false;
                }
            }
            return true;
        default:
            return false;
    }
}

bool text_in_values(std::string_view text, const Textual& values) {
    if (std::holds_alternative<std::string>(values)) return text == std::get<std::string>(values);
    for (const auto& value : std::get<std::vector<std::string>>(values)) {
        if (text == value) return true;
    }
    return false;
}

double eval_arrow_expr(const ArithmeticExprSpec& expr,
                       const std::shared_ptr<arrow::StructArray>& row_array,
                       int64_t row) {
    if (expr.kind == ArithmeticExprKind::LITERAL) return expr.literal;
    if (expr.kind == ArithmeticExprKind::FIELD) {
        ArrowAnyValue v = arrow_value_from_struct(row_array, expr.field, row);
        return v.has_number ? v.number : 0.0;
    }
    double left = expr.children.empty() ? 0.0 : eval_arrow_expr(expr.children[0], row_array, row);
    double right = expr.children.size() < 2 ? 0.0 : eval_arrow_expr(expr.children[1], row_array, row);
    switch (expr.kind) {
        case ArithmeticExprKind::ADD: return left + right;
        case ArithmeticExprKind::SUB: return left - right;
        case ArithmeticExprKind::MUL: return left * right;
        case ArithmeticExprKind::DIV: return right == 0.0 ? 0.0 : left / right;
        default: return left;
    }
}

double deg_to_rad(double v) {
    return v * 0.01745329251994329576923690768489;
}

bool eval_arrow_array_any_condition(const ConditionSpec& c,
                                    const std::shared_ptr<arrow::StructArray>& row_array,
                                    int64_t row,
                                    const LookupRegistry& lookups) {
    return std::visit([&](const auto& spec) -> bool {
        using T = std::decay_t<decltype(spec)>;
        if constexpr (std::is_same_v<T, NumericConditionSpec>) {
            ArrowAnyValue v = arrow_value_from_struct(row_array, spec.field, row);
            return v.has_number && any_op_numeric(v.number, spec.op, spec.threshold);
        } else if constexpr (std::is_same_v<T, NumericRangeConditionSpec>) {
            ArrowAnyValue v = arrow_value_from_struct(row_array, spec.field, row);
            if (!v.has_number) return false;
            return spec.op == OpType::BETWEEN_EXCLUDING
                ? (v.number > spec.lower && v.number < spec.upper)
                : (v.number >= spec.lower && v.number <= spec.upper);
        } else if constexpr (std::is_same_v<T, CategoricalConditionSpec>) {
            ArrowAnyValue v = arrow_value_from_struct(row_array, spec.field, row);
            if (!v.exists) return false;
            bool found = !v.text.empty() && text_in_values(v.text, spec.values);
            for (const auto& text : v.texts) found = found || text_in_values(text, spec.values);
            if (spec.op == OpType::CONTAINS_ALL && std::holds_alternative<std::vector<std::string>>(spec.values)) {
                const auto& required = std::get<std::vector<std::string>>(spec.values);
                for (const auto& required_value : required) {
                    bool one = false;
                    for (const auto& text : v.texts) one = one || text == required_value;
                    if (!one) return false;
                }
                return true;
            }
            bool positive = spec.op == OpType::EQ || spec.op == OpType::IN ||
                            spec.op == OpType::CONTAINS_ANY || spec.op == OpType::INTERSECTS;
            return positive ? found : !found;
        } else if constexpr (std::is_same_v<T, ArrayLenConditionSpec>) {
            ArrowAnyValue v = arrow_value_from_struct(row_array, spec.field, row);
            if (!v.exists) return false;
            int len = !v.texts.empty() ? static_cast<int>(v.texts.size())
                                       : static_cast<int>(v.text.size());
            OpType op = spec.op == OpType::ARRAY_LEN_LT ? OpType::LT :
                (spec.op == OpType::ARRAY_LEN_EQ ? OpType::EQ : OpType::GT);
            return any_op_numeric(static_cast<double>(len), op, spec.length);
        } else if constexpr (std::is_same_v<T, NullConditionSpec>) {
            ArrowAnyValue v = arrow_value_from_struct(row_array, spec.field, row);
            bool is_null = !v.exists || v.is_null;
            if (spec.op == OpType::IS_NULL) return is_null;
            if (spec.op == OpType::IS_NOT_NULL) return !is_null;
            if (spec.op == OpType::IS_EMPTY) return is_null || (v.text.empty() && v.texts.empty());
            if (spec.op == OpType::IS_NOT_EMPTY) return v.exists && (!v.text.empty() || !v.texts.empty());
            return false;
        } else if constexpr (std::is_same_v<T, CrossFieldConditionSpec>) {
            ArrowAnyValue l = arrow_value_from_struct(row_array, spec.field, row);
            ArrowAnyValue r = arrow_value_from_struct(row_array, spec.other_field, row);
            if (!l.exists || !r.exists) return false;
            if (l.has_number && r.has_number) return any_op_numeric(l.number, spec.op, r.number);
            bool matched = l.text == r.text;
            return spec.op == OpType::NEQ_FIELD ? !matched : matched;
        } else if constexpr (std::is_same_v<T, BitfieldConditionSpec>) {
            ArrowAnyValue v = arrow_value_from_struct(row_array, spec.field, row);
            if (!v.has_number) return false;
            uint64_t flags = static_cast<uint64_t>(v.number);
            uint64_t masked = flags & spec.mask;
            if (spec.op == OpType::FLAGS_ALL) return masked == spec.mask;
            if (spec.op == OpType::FLAGS_NONE) return masked == 0;
            return masked != 0;
        } else if constexpr (std::is_same_v<T, StringConditionSpec>) {
            ArrowAnyValue v = arrow_value_from_struct(row_array, spec.field, row);
            if (!v.exists) return false;
            if (spec.op == OpType::LENGTH_GT || spec.op == OpType::LENGTH_LT || spec.op == OpType::LENGTH_EQ) {
                OpType op = spec.op == OpType::LENGTH_LT ? OpType::LT :
                    (spec.op == OpType::LENGTH_EQ ? OpType::EQ : OpType::GT);
                return any_op_numeric(static_cast<double>(v.text.size()), op, spec.length);
            }
            return any_op_string(v.text, spec.op, spec.value);
        } else if constexpr (std::is_same_v<T, RegexConditionSpec>) {
            ArrowAnyValue v = arrow_value_from_struct(row_array, spec.field, row);
            if (!v.exists) return false;
            if (spec.compiled) {
                bool matched = spec.compiled->ok() && RE2::PartialMatch(v.text, *spec.compiled);
                return spec.op == OpType::NOT_REGEX ? !matched : matched;
            }
            RE2 fallback(spec.pattern);
            bool matched = fallback.ok() && RE2::PartialMatch(v.text, fallback);
            return spec.op == OpType::NOT_REGEX ? !matched : matched;
        } else if constexpr (std::is_same_v<T, LookupConditionSpec>) {
            ArrowAnyValue v = arrow_value_from_struct(row_array, spec.field, row);
            auto it = lookups.find(spec.lookup_name);
            if (!v.exists || it == lookups.end() || !it->second) return false;
            const auto& lookup = *it->second;
            bool matched = false;
            if (lookup.type == LookupSetType::STRING_SET) {
                matched = std::binary_search(lookup.strings.begin(), lookup.strings.end(), v.text);
            } else if (lookup.type == LookupSetType::INT_SET && v.has_number) {
                matched = std::binary_search(lookup.ints.begin(), lookup.ints.end(),
                                             static_cast<int64_t>(v.number));
            } else if (lookup.type == LookupSetType::IPV4_CIDR_SET) {
                uint32_t ip = v.has_number ? static_cast<uint32_t>(v.number) : parse_ipv4_text(v.text);
                matched = ipv4_ranges_contain_value(lookup.ipv4_ranges, ip);
            }
            return spec.op == OpType::NOT_IN_LOOKUP ? !matched : matched;
        } else if constexpr (std::is_same_v<T, CidrConditionSpec>) {
            ArrowAnyValue v = arrow_value_from_struct(row_array, spec.field, row);
            if (!v.exists) return false;
            uint32_t network = spec.network;
            uint32_t mask = spec.mask;
            if (!spec.compiled) parse_cidr_text(spec.cidr, network, mask);
            uint32_t ip = v.has_number ? static_cast<uint32_t>(v.number) : parse_ipv4_text(v.text);
            bool matched = (ip & mask) == network;
            return spec.op == OpType::IP_NOT_IN_SUBNET ? !matched : matched;
        } else if constexpr (std::is_same_v<T, TemporalConditionSpec>) {
            ArrowAnyValue v = arrow_value_from_struct(row_array, spec.field, row);
            if (!v.has_number) return false;
            int64_t ts = static_cast<int64_t>(v.number);
            bool matched = false;
            switch (spec.op) {
                case OpType::BEFORE: matched = ts < spec.value; break;
                case OpType::AFTER: matched = ts > spec.value; break;
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
            ArrowAnyValue lat = arrow_value_from_struct(row_array, spec.lat_field, row);
            ArrowAnyValue lon = arrow_value_from_struct(row_array, spec.lon_field, row);
            ArrowAnyValue other_lat = arrow_value_from_struct(row_array, spec.other_lat_field, row);
            ArrowAnyValue other_lon = arrow_value_from_struct(row_array, spec.other_lon_field, row);
            if (!lat.has_number || !lon.has_number || !other_lat.has_number || !other_lon.has_number) return false;
            double lat1 = deg_to_rad(lat.number);
            double lon1 = deg_to_rad(lon.number);
            double lat2 = deg_to_rad(other_lat.number);
            double lon2 = deg_to_rad(other_lon.number);
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
            double lhs = eval_arrow_expr(spec.expr, row_array, row);
            double rhs = spec.threshold;
            if (!spec.other_field.empty()) {
                ArrowAnyValue other = arrow_value_from_struct(row_array, spec.other_field, row);
                rhs = other.has_number ? other.number : 0.0;
            }
            return any_op_numeric(lhs, spec.op, rhs);
        } else if constexpr (std::is_same_v<T, AndConditionSpec>) {
            for (const auto& child : spec.child_condition) {
                if (!eval_arrow_array_any_condition(child, row_array, row, lookups)) return false;
            }
            return true;
        } else if constexpr (std::is_same_v<T, OrConditionSpec>) {
            for (const auto& child : spec.child_condition) {
                if (eval_arrow_array_any_condition(child, row_array, row, lookups)) return true;
            }
            return false;
        } else if constexpr (std::is_same_v<T, NotConditionSpec>) {
            return spec.child_condition.empty() ||
                !eval_arrow_array_any_condition(spec.child_condition.front(), row_array, row, lookups);
        } else {
            return false;
        }
    }, c.node);
}

std::shared_ptr<arrow::Array> make_zero_int32_array(int64_t rows) {
    auto buffer = arrow::AllocateBuffer(rows * static_cast<int64_t>(sizeof(int32_t))).ValueOrDie();
    std::memset(buffer->mutable_data(), 0, static_cast<size_t>(buffer->size()));
    auto data = arrow::ArrayData::Make(arrow::int32(), rows, {nullptr, std::move(buffer)}, 0);
    return arrow::MakeArray(data);
}

std::shared_ptr<arrow::Array> compute_array_any_channel(
        const std::shared_ptr<arrow::RecordBatch>& batch,
        const ArrayAnyChannelSpec& channel,
        const LookupRegistry& lookups) {
    auto list_array_base = resolve_dotted_array(batch, channel.path);
    if (!list_array_base ||
        (list_array_base->type_id() != arrow::Type::LIST &&
         list_array_base->type_id() != arrow::Type::LARGE_LIST)) {
        return make_zero_int32_array(batch->num_rows());
    }

    auto buffer = arrow::AllocateBuffer(batch->num_rows() * static_cast<int64_t>(sizeof(int32_t))).ValueOrDie();
    auto* out = reinterpret_cast<int32_t*>(buffer->mutable_data());
    std::memset(out, 0, static_cast<size_t>(buffer->size()));

    auto fill_output = [&](const auto& list) {
        auto values = list->values();
        if (!values || values->type_id() != arrow::Type::STRUCT) return;
        auto struct_values = std::static_pointer_cast<arrow::StructArray>(values);
        for (int64_t row = 0; row < batch->num_rows(); ++row) {
            if (list_array_base->IsNull(row)) continue;
            int64_t start = list->value_offset(row);
            int64_t length = list->value_length(row);
            for (int64_t i = 0; i < length; ++i) {
                int64_t child_row = start + i;
                if (struct_values->IsNull(child_row)) continue;
                if (eval_arrow_array_any_condition(channel.where, struct_values, child_row, lookups)) {
                    out[row] = 1;
                    break;
                }
            }
        }
    };

    if (list_array_base->type_id() == arrow::Type::LIST) {
        fill_output(std::static_pointer_cast<arrow::ListArray>(list_array_base));
    } else {
        fill_output(std::static_pointer_cast<arrow::LargeListArray>(list_array_base));
    }
    auto data = arrow::ArrayData::Make(arrow::int32(), batch->num_rows(), {nullptr, std::move(buffer)}, 0);
    return arrow::MakeArray(data);
}

void collect_array_any_synthetic_fields(const ConditionSpec& condition,
                                        std::vector<std::string>& out) {
    std::visit([&](const auto& spec) {
        using T = std::decay_t<decltype(spec)>;
        if constexpr (std::is_same_v<T, ArrayAnyConditionSpec>) {
            if (!spec.synthetic_field.empty()) out.push_back(spec.synthetic_field);
        } else if constexpr (std::is_same_v<T, AndConditionSpec>) {
            for (const auto& child : spec.child_condition) collect_array_any_synthetic_fields(child, out);
        } else if constexpr (std::is_same_v<T, OrConditionSpec>) {
            for (const auto& child : spec.child_condition) collect_array_any_synthetic_fields(child, out);
        } else if constexpr (std::is_same_v<T, NotConditionSpec>) {
            for (const auto& child : spec.child_condition) collect_array_any_synthetic_fields(child, out);
        }
    }, condition.node);
}

BlazeRulesSchema schema_with_array_any_fields(const BlazeRulesSchema& schema,
                                              const RuleFileSpec& rules,
                                              bool& changed) {
    std::vector<std::string> synthetic_names;
    for (const auto& rule : rules.rules) {
        collect_array_any_synthetic_fields(rule.root_condition, synthetic_names);
    }
    if (synthetic_names.empty()) {
        changed = false;
        return BlazeRulesSchema(schema.all_fields());
    }

    std::vector<FieldSpec> fields = schema.all_fields();
    std::unordered_set<std::string> existing;
    existing.reserve(fields.size() + synthetic_names.size());
    for (const auto& field : fields) existing.insert(field.name);
    changed = false;
    for (const auto& name : synthetic_names) {
        if (!existing.insert(name).second) continue;
        FieldSpec field;
        field.name = name;
        field.type = ColumnType::INT32;
        field.nullable = false;
        field.source = FieldSpec::INFERRED_AND_BOUND;
        fields.push_back(std::move(field));
        changed = true;
    }
    return BlazeRulesSchema(std::move(fields));
}

} // namespace

RuleEngine::RuleEngine(EngineConfig config)
    : config_(std::move(config)),
      metrics_emitter_(std::make_shared<NoopMetricsEmitter>()) {
    validate_engine_config();
}

RuleEngine::RuleEngine(std::vector<FieldSpec> fields, EngineConfig config)
    : RuleEngine(std::move(config)) {
    bind_schema(BlazeRulesSchema(std::move(fields)), SchemaState::USER_BOUND);
}

BlazeRulesResult<std::unique_ptr<RuleEngine>> RuleEngine::create(BlazeRulesSchema schema, EngineConfig config) {
    return BlazeRulesResult<std::unique_ptr<RuleEngine>>::ok(
        std::make_unique<RuleEngine>(schema.all_fields(), config));
}

RuleEngine::~RuleEngine() {
    stop_hot_reload();
}

std::shared_ptr<CompiledRuleSet> RuleEngine::active_ruleset() const {
    return std::atomic_load_explicit(&ruleset_, std::memory_order_acquire);
}

void RuleEngine::bind_schema(BlazeRulesSchema schema, SchemaState state) {
    schema_ = std::move(schema);
    schema_state_ = state;
    dict_encoder_ = std::make_unique<DictEncoder>(schema_, config_.max_dict_size_per_column);
    transposer_ = std::make_unique<BatchTransposer>(schema_);
    transposer_->set_max_error_samples(config_.max_error_samples);
}

void RuleEngine::compile_pending_rules_after_bind() {
    if (!pending_rules_) return;
    CompileResult compiled = compile_rule_file(*pending_rules_, schema_);
    if (!compiled.ok) {
        BlazeRulesError error = compiled.error;
        error.domain = error.source == "lookup" ? BlazeRulesError::Domain::LOOKUP : BlazeRulesError::Domain::RULE_VALIDATION;
        throw BlazeRulesSchemaError(std::move(error));
    }

    auto next = std::make_shared<CompiledRuleSet>(std::move(compiled.value));
    preload_lookup_dictionaries(*next);
    next->dict_generation = dict_encoder_->generation();
    install_ruleset(next);
    {
        std::lock_guard<std::mutex> lock(reload_status_mutex_);
        hot_reload_status_.active_version = next->version;
        hot_reload_status_.last_success_ms = epoch_millis();
        ++hot_reload_status_.reload_count;
        hot_reload_status_.last_error_code.clear();
        hot_reload_status_.last_error_message.clear();
    }
    pending_rules_.reset();
    pending_rules_path_.clear();
}

void RuleEngine::ensure_schema_bound_from_messages(const std::vector<std::string_view>& messages) {
    if (schema_state_ != SchemaState::UNBOUND) return;
    if (!pending_rules_) {
        throw BlazeRulesConfigError({BlazeRulesError::MISSING_REQUIRED_FIELD,
                              "load rules before evaluating with inferred schema",
                              "schema_inference", "", -1, BlazeRulesError::Domain::CONFIG});
    }
    auto inferred = infer_schema_from_messages(*pending_rules_, messages);
    if (inferred.is_error()) throw BlazeRulesSchemaError(inferred.error());
    bind_schema(std::move(inferred.value()), SchemaState::INFERRED_BOUND);
    compile_pending_rules_after_bind();
}

void RuleEngine::ensure_schema_bound_from_ndjson(std::string_view ndjson_bytes) {
    if (schema_state_ != SchemaState::UNBOUND) return;
    if (!pending_rules_) {
        throw BlazeRulesConfigError({BlazeRulesError::MISSING_REQUIRED_FIELD,
                              "load rules before evaluating with inferred schema",
                              "schema_inference", "", -1, BlazeRulesError::Domain::CONFIG});
    }
    auto inferred = infer_schema_from_ndjson(*pending_rules_, ndjson_bytes);
    if (inferred.is_error()) throw BlazeRulesSchemaError(inferred.error());
    bind_schema(std::move(inferred.value()), SchemaState::INFERRED_BOUND);
    compile_pending_rules_after_bind();
}

void RuleEngine::ensure_schema_bound_from_arrow(const std::shared_ptr<arrow::RecordBatch>& batch) {
    if (schema_state_ != SchemaState::UNBOUND) return;
    if (!pending_rules_) {
        throw BlazeRulesConfigError({BlazeRulesError::MISSING_REQUIRED_FIELD,
                              "load rules before evaluating with inferred schema",
                              "schema_inference", "", -1, BlazeRulesError::Domain::CONFIG});
    }
    auto inferred = infer_schema_from_arrow(*pending_rules_, batch);
    if (inferred.is_error()) throw BlazeRulesSchemaError(inferred.error());
    bind_schema(std::move(inferred.value()), SchemaState::INFERRED_BOUND);
    compile_pending_rules_after_bind();
}

ConflictReport RuleEngine::load_rules(const std::string& rules_path) {
    return compile_and_install_rules(rules_path, true);
}

ConflictReport RuleEngine::reload_rules_now(const std::string& rules_path) {
    return compile_and_install_rules(rules_path, true);
}

ConflictReport RuleEngine::compile_and_install_rules(const std::string& rules_path,
                                                     bool update_reload_status) {
    if (update_reload_status) {
        std::lock_guard<std::mutex> lock(reload_status_mutex_);
        hot_reload_status_.pending_path = rules_path;
        hot_reload_status_.last_attempt_ms = epoch_millis();
        hot_reload_status_.last_error_code.clear();
        hot_reload_status_.last_error_message.clear();
    }

    ParseFileResult parsed = parse_rule_file(rules_path);
    if (!parsed.ok) {
        BlazeRulesError error = with_domain(parsed.error, BlazeRulesError::Domain::RULE_PARSE);
        if (update_reload_status) {
            std::lock_guard<std::mutex> lock(reload_status_mutex_);
            ++hot_reload_status_.failed_reload_count;
            hot_reload_status_.last_error_code = blazerules_error_code_name(error.code);
            hot_reload_status_.last_error_message = error.message;
        }
        metrics_emitter_->increment_counter("blazerules.hot_reload_failed_total", 1, {});
        throw BlazeRulesParseError(std::move(error));
    }
    return install_or_defer_rules(std::move(parsed), update_reload_status, rules_path);
}

ConflictReport RuleEngine::install_or_defer_rules(ParseFileResult parsed,
                                                  bool update_reload_status,
                                                  const std::string& rules_path) {
    std::unique_lock<std::shared_mutex> state_lock(state_mutex_);
    ConflictReport report = ::analyze_conflicts(parsed.value);
    if (schema_state_ == SchemaState::UNBOUND) {
        pending_rules_ = std::move(parsed.value);
        pending_rules_path_ = rules_path;
        pending_conflict_report_ = report;
        if (update_reload_status) {
            std::lock_guard<std::mutex> lock(reload_status_mutex_);
            hot_reload_status_.pending_path = rules_path;
            hot_reload_status_.last_success_ms = epoch_millis();
            hot_reload_status_.last_error_code.clear();
            hot_reload_status_.last_error_message.clear();
        }
        return report;
    }

    bool schema_changed = false;
    BlazeRulesSchema compile_schema = schema_with_array_any_fields(schema_, parsed.value, schema_changed);
    CompileResult compiled = compile_rule_file(parsed.value, compile_schema);
    if (!compiled.ok) {
        BlazeRulesError error = compiled.error;
        if (error.source == "lookup") error.domain = BlazeRulesError::Domain::LOOKUP;
        else error.domain = BlazeRulesError::Domain::RULE_VALIDATION;
        if (update_reload_status) {
            std::lock_guard<std::mutex> lock(reload_status_mutex_);
            ++hot_reload_status_.failed_reload_count;
            hot_reload_status_.last_error_code = blazerules_error_code_name(error.code);
            hot_reload_status_.last_error_message = error.message;
        }
        metrics_emitter_->increment_counter("blazerules.hot_reload_failed_total", 1, {});
        throw BlazeRulesSchemaError(std::move(error));
    }

    if (schema_changed) {
        bind_schema(std::move(compile_schema), schema_state_);
    }
    auto next = std::make_shared<CompiledRuleSet>(std::move(compiled.value));
    preload_lookup_dictionaries(*next);
    next->dict_generation = dict_encoder_->generation();
    install_ruleset(next);
    if (update_reload_status) {
        std::lock_guard<std::mutex> lock(reload_status_mutex_);
        hot_reload_status_.active_version = next->version;
        hot_reload_status_.last_success_ms = epoch_millis();
        ++hot_reload_status_.reload_count;
        hot_reload_status_.last_error_code.clear();
        hot_reload_status_.last_error_message.clear();
    }
    metrics_emitter_->increment_counter("blazerules.hot_reload_success_total", 1, {});
    return report;
}

ConflictReport RuleEngine::load_rules_from_string(const std::string& rules_yaml_or_json,
                                                  RuleFileFormat) {
    ParseFileResult parsed = parse_rule_string(rules_yaml_or_json);
    if (!parsed.ok) throw BlazeRulesParseError(with_domain(parsed.error, BlazeRulesError::Domain::RULE_PARSE));
    return install_or_defer_rules(std::move(parsed), false, "string");
}

ConflictReport RuleEngine::analyze_conflicts(const std::string& rules_path) {
    ParseFileResult parsed = parse_rule_file(rules_path);
    if (!parsed.ok) throw BlazeRulesParseError(with_domain(parsed.error, BlazeRulesError::Domain::RULE_PARSE));
    return ::analyze_conflicts(parsed.value);
}

std::string RuleEngine::active_rule_set_version() const {
    auto rs = active_ruleset();
    return rs ? rs->version : std::string();
}

HotReloadStatus RuleEngine::hot_reload_status() const {
    std::lock_guard<std::mutex> lock(reload_status_mutex_);
    HotReloadStatus status = hot_reload_status_;
    auto rs = active_ruleset();
    if (rs) status.active_version = rs->version;
    return status;
}

void RuleEngine::enable_hot_reload(const std::string& rules_file_path,
                                   std::chrono::seconds poll_interval) {
    stop_hot_reload();
    if (poll_interval.count() <= 0) {
        poll_interval = std::chrono::seconds(config_.hot_reload_poll_seconds);
    }
    {
        std::lock_guard<std::mutex> lock(reload_status_mutex_);
        hot_reload_status_.pending_path = rules_file_path;
    }
    hot_reload_stop_.store(false);
    hot_reload_thread_ = std::thread(&RuleEngine::hot_reload_loop, this, rules_file_path, poll_interval);
}

void RuleEngine::stop_hot_reload() {
    hot_reload_stop_.store(true);
    if (hot_reload_thread_.joinable()) hot_reload_thread_.join();
}

void RuleEngine::hot_reload_loop(std::string rules_file_path, std::chrono::seconds poll_interval) {
    namespace fs = std::filesystem;
    auto last = fs::exists(rules_file_path) ? fs::last_write_time(rules_file_path) : fs::file_time_type{};
    while (!hot_reload_stop_.load()) {
        std::this_thread::sleep_for(poll_interval);
        if (!fs::exists(rules_file_path)) continue;
        auto current = fs::last_write_time(rules_file_path);
        if (current != last) {
            last = current;
            try {
                (void)reload_rules_now(rules_file_path);
            } catch (const BlazeRulesException&) {
                if (!config_.hot_reload_keep_previous_on_failure) {
                    // Clear main engine AND shards together so no shard keeps evaluating
                    // stale rules after a failed reload.
                    clear_ruleset();
                }
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(reload_status_mutex_);
                ++hot_reload_status_.failed_reload_count;
                hot_reload_status_.last_error_code = blazerules_error_code_name(BlazeRulesError::HOT_RELOAD_FAILED);
                hot_reload_status_.last_error_message = e.what();
            }
        }
    }
}

void RuleEngine::configure_projection(const CompiledRuleSet& ruleset) {
    std::vector<int> cols;
    auto add_value_expr_columns = [&](const ArithmeticPredicateOp& a) {
        for (const auto& node : a.value_nodes) {
            if (node.column_index >= 0) cols.push_back(node.column_index);
        }
        if (a.other_column_index >= 0) cols.push_back(a.other_column_index);
    };
    for (const auto& rule : ruleset.rules) {
        for (const auto& op : rule.op) {
            if (const auto* n = std::get_if<NumericPredicateOp>(&op)) cols.push_back(n->column_index);
            else if (const auto* r = std::get_if<NumericRangePredicateOp>(&op)) cols.push_back(r->column_index);
            else if (const auto* c = std::get_if<CategoricalPredicateOp>(&op)) cols.push_back(c->column_index);
            else if (const auto* z = std::get_if<NullPredicateOp>(&op)) cols.push_back(z->column_index);
            else if (const auto* c = std::get_if<CrossFieldPredicateOp>(&op)) {
                cols.push_back(c->left_column_index);
                cols.push_back(c->right_column_index);
            } else if (const auto* b = std::get_if<BitfieldPredicateOp>(&op)) cols.push_back(b->column_index);
            else if (const auto* s = std::get_if<StringPredicateOp>(&op)) cols.push_back(s->column_index);
            else if (const auto* r = std::get_if<RegexPredicateOp>(&op)) cols.push_back(r->column_index);
            else if (const auto* l = std::get_if<LookupPredicateOp>(&op)) cols.push_back(l->column_index);
            else if (const auto* c = std::get_if<CidrPredicateOp>(&op)) cols.push_back(c->column_index);
            else if (const auto* t = std::get_if<TemporalPredicateOp>(&op)) cols.push_back(t->column_index);
            else if (const auto* g = std::get_if<GeoDistancePredicateOp>(&op)) {
                cols.push_back(g->lat_column_index);
                cols.push_back(g->lon_column_index);
                cols.push_back(g->other_lat_column_index);
                cols.push_back(g->other_lon_column_index);
            } else if (const auto* a = std::get_if<ArithmeticPredicateOp>(&op)) {
                add_value_expr_columns(*a);
            }
        }
    }
    for (const auto& ch : ruleset.window_channels) {
        cols.push_back(ch.entity_col_index);
        if (ch.sum_col_index >= 0) cols.push_back(ch.sum_col_index);
        if (ch.denominator_col_index >= 0) cols.push_back(ch.denominator_col_index);
    }
    for (const auto& mc : ruleset.model_channels) {
        for (int idx : mc.feature_col_indices) {
            if (idx >= 0) cols.push_back(idx);
        }
    }
    for (const auto& vc : ruleset.vector_channels) {
        for (int idx : vc.dim_col_indices) {
            if (idx >= 0) cols.push_back(idx);
        }
    }
    std::sort(cols.begin(), cols.end());
    cols.erase(std::unique(cols.begin(), cols.end()), cols.end());
    transposer_->set_projected_fields(std::move(cols));
}

EvalOptions RuleEngine::eval_options() const {
    EvalOptions options;
    options.parallel_threshold = config_.parallel_threshold;
    options.enable_selection_vectors = config_.enable_selection_vectors;
    options.selection_vector_threshold = config_.selection_vector_threshold;
    options.enable_adaptive_predicate_ordering = config_.enable_adaptive_predicate_ordering;
    options.enable_no_validity_fast_path = config_.enable_no_validity_fast_path;
    options.enable_prefetch = config_.enable_prefetch;
    options.enable_thread_affinity = config_.enable_thread_affinity;
    options.result_buffer_reuse = config_.result_buffer_reuse;
    options.materialize_rule_bitmasks = config_.output_detail == EngineConfig::OUTPUT_BITMASKS;
    options.arena_size_bytes = config_.arena_size_bytes;
    return options;
}

void RuleEngine::validate_engine_config() const {
    if (config_.batch_size <= 0) {
        throw BlazeRulesConfigError({BlazeRulesError::INVALID_ENGINE_CONFIG,
                              "batch_size must be positive",
                              "engine", "", -1, BlazeRulesError::Domain::CONFIG});
    }
    if (config_.parallel_threshold < 0) {
        throw BlazeRulesConfigError({BlazeRulesError::INVALID_ENGINE_CONFIG,
                              "parallel_threshold must be non-negative",
                              "engine", "", -1, BlazeRulesError::Domain::CONFIG});
    }
    if (config_.selection_vector_threshold < 0.0 || config_.selection_vector_threshold > 1.0) {
        throw BlazeRulesConfigError({BlazeRulesError::INVALID_ENGINE_CONFIG,
                              "selection_vector_threshold must be between 0 and 1",
                              "engine", "", -1, BlazeRulesError::Domain::CONFIG});
    }
    if (config_.hot_reload_poll_seconds <= 0) {
        throw BlazeRulesConfigError({BlazeRulesError::INVALID_ENGINE_CONFIG,
                              "hot_reload_poll_seconds must be positive",
                              "engine", "", -1, BlazeRulesError::Domain::CONFIG});
    }
    if (config_.max_error_samples < 0) {
        throw BlazeRulesConfigError({BlazeRulesError::INVALID_ENGINE_CONFIG,
                              "max_error_samples must be non-negative",
                              "engine", "", -1, BlazeRulesError::Domain::CONFIG});
    }
    std::string simd_error;
    if (!set_simd_backend_override(config_.simd_backend_override,
                                   config_.enable_avx512,
                                   &simd_error)) {
        throw BlazeRulesConfigError({BlazeRulesError::INVALID_ENGINE_CONFIG,
                              simd_error,
                              "engine.simd", "", -1, BlazeRulesError::Domain::CONFIG});
    }
}

void RuleEngine::validate_record_batch_schema(const std::shared_ptr<arrow::RecordBatch>& batch) const {
    if (!batch) {
        throw BlazeRulesSchemaError({BlazeRulesError::SCHEMA_MISMATCH,
                              "record batch is null",
                              "arrow", "", -1, BlazeRulesError::Domain::ARROW});
    }
    if (batch->num_columns() < schema_.num_fields()) {
        throw BlazeRulesSchemaError({BlazeRulesError::SCHEMA_MISMATCH,
                              "record batch has fewer columns than BLAZERULES schema",
                              "arrow", "", -1, BlazeRulesError::Domain::ARROW});
    }
    for (int c = 0; c < schema_.num_fields(); ++c) {
        const auto& arr = batch->column(c);
        if (!arr) {
            BlazeRulesError error{BlazeRulesError::SCHEMA_MISMATCH,
                           "record batch contains null Arrow column",
                           "arrow", "", -1, BlazeRulesError::Domain::ARROW};
            error.column_name = schema_.name_at(c);
            throw BlazeRulesSchemaError(std::move(error));
        }
        if (!arrow_type_compatible(schema_.type_of(c), arr->type_id())) {
            BlazeRulesError error{BlazeRulesError::TYPE_MISMATCH,
                           "record batch Arrow column type does not match BLAZERULES schema",
                           "arrow", "", -1, BlazeRulesError::Domain::ARROW};
            error.column_name = schema_.name_at(c);
            throw BlazeRulesSchemaError(std::move(error));
        }
    }
}

std::shared_ptr<arrow::RecordBatch> RuleEngine::align_record_batch_to_schema(
        const std::shared_ptr<arrow::RecordBatch>& batch) const {
    if (!batch) return batch;
    bool already_aligned = batch->num_columns() == schema_.num_fields();
    auto rs = active_ruleset();
    std::vector<std::shared_ptr<arrow::Array>> columns;
    std::vector<std::shared_ptr<arrow::Field>> fields;
    columns.reserve(schema_.num_fields());
    fields.reserve(schema_.num_fields());
    for (int c = 0; c < schema_.num_fields(); ++c) {
        int source = batch->schema()->GetFieldIndex(schema_.name_at(c));
        std::shared_ptr<arrow::Array> column;
        if (source >= 0) {
            column = batch->column(source);
            if (source != c) already_aligned = false;
        } else if (rs) {
            for (const auto& channel : rs->array_any_channels) {
                if (channel.synthetic_col_index == c) {
                    column = compute_array_any_channel(batch, channel, rs->lookups);
                    already_aligned = false;
                    break;
                }
            }
        }
        if (!column) {
            column = resolve_dotted_array(batch, schema_.name_at(c));
            if (column) already_aligned = false;
        }
        if (!column) {
            BlazeRulesError error{BlazeRulesError::SCHEMA_MISMATCH,
                           "record batch missing required BLAZERULES field: " + schema_.name_at(c),
                           "arrow", "", -1, BlazeRulesError::Domain::ARROW};
            error.column_name = schema_.name_at(c);
            throw BlazeRulesSchemaError(std::move(error));
        }
        columns.push_back(std::move(column));
        fields.push_back(arrow::field(schema_.name_at(c),
                                      blazerules::ingest_arrow_type(schema_.type_of(c)),
                                      schema_.is_nullable(c)));
    }
    if (already_aligned) return batch;
    return arrow::RecordBatch::Make(arrow::schema(std::move(fields)), batch->num_rows(), std::move(columns));
}

void RuleEngine::emit_dead_letter_log(const BatchResult& result) {
    if (config_.dead_letter_path.empty() || result.error_samples.empty()) return;
    if (!dead_letter_stream_) {
        dead_letter_stream_ = std::make_unique<std::ofstream>(
            config_.dead_letter_path, std::ios::out | std::ios::app);
        if (!*dead_letter_stream_) {
            throw BlazeRulesParseError({BlazeRulesError::DEAD_LETTER_WRITE_FAILED,
                                 "failed to open dead-letter log: " + config_.dead_letter_path,
                                 "dead_letter", "", -1, BlazeRulesError::Domain::INGEST});
        }
    }

    std::string buffer;
    buffer.reserve(result.error_samples.size() * 160);
    for (const auto& sample : result.error_samples) {
        buffer += "{\"ts_ms\":";
        buffer += std::to_string(result.evaluation_timestamp_ms ? result.evaluation_timestamp_ms : epoch_millis());
        buffer += ",\"code\":\"";
        buffer += json_escape(sample.code);
        buffer += "\",\"message\":\"";
        buffer += json_escape(sample.message);
        buffer += "\",\"source\":\"";
        buffer += json_escape(sample.source);
        buffer += "\",\"row_index\":";
        buffer += std::to_string(sample.row_index);
        buffer += ",\"column_name\":\"";
        buffer += json_escape(sample.column_name);
        buffer += "\"}\n";
    }
    (*dead_letter_stream_) << buffer;
    dead_letter_stream_->flush();
    if (!*dead_letter_stream_) {
        throw BlazeRulesParseError({BlazeRulesError::DEAD_LETTER_WRITE_FAILED,
                             "failed to write dead-letter log: " + config_.dead_letter_path,
                             "dead_letter", "", -1, BlazeRulesError::Domain::INGEST});
    }
}

void RuleEngine::apply_ingest_error_policy(const BatchTransposer& transposer, BatchResult& result) {
    result.error_counts.clear();
    result.error_samples.clear();
    for (const auto& [code, count] : transposer.error_counts()) {
        result.error_counts[code] = count;
    }
    result.error_samples = transposer.error_samples();

    bool has_malformed = result.error_counts.contains("MALFORMED_JSON");
    bool has_type_error = result.error_counts.contains("FIELD_TYPE_COERCION_FAILED");
    if ((config_.ingest_error_mode == EngineConfig::HARD_FAIL && has_malformed) ||
        (config_.type_mismatch_mode == EngineConfig::HARD_FAIL_TYPE && has_type_error)) {
        BlazeRulesError error{has_malformed ? BlazeRulesError::MALFORMED_JSON : BlazeRulesError::FIELD_TYPE_COERCION_FAILED,
                       result.last_ingest_error.empty() ? "ingest failed" : result.last_ingest_error,
                       "ingest", "", -1, BlazeRulesError::Domain::INGEST};
        if (!result.error_samples.empty()) {
            error.row_index = result.error_samples.front().row_index;
            error.column_name = result.error_samples.front().column_name;
        }
        throw BlazeRulesParseError(std::move(error));
    }

    if (config_.ingest_error_mode == EngineConfig::SKIP_TO_DEAD_LETTER) {
        emit_dead_letter_log(result);
    }
}

void RuleEngine::emit_decision_log(const BatchResult& result) {
    if (config_.decision_log_path.empty() || result.n_records <= 0) return;
    if (!decision_log_stream_) {
        decision_log_stream_ = std::make_unique<std::ofstream>(
            config_.decision_log_path, std::ios::out | std::ios::app);
        if (!*decision_log_stream_) {
            throw BlazeRulesConfigError({BlazeRulesError::FILE_IO_ERROR,
                                  "failed to open decision log: " + config_.decision_log_path,
                                  "decision_log", "", -1, BlazeRulesError::Domain::CONFIG});
        }
    }

    std::string buffer;
    buffer.reserve(static_cast<size_t>(result.n_records) * 160);
    std::vector<uint8_t> matched(static_cast<size_t>(result.n_records), 0);
    for (int32_t idx : result.matched_record_indices) {
        if (idx >= 0 && idx < result.n_records) matched[static_cast<size_t>(idx)] = 1;
    }
    for (int row = 0; row < result.n_records; ++row) {
        std::string_view decision = row < static_cast<int>(result.decisions.size())
            ? std::string_view(result.decisions[static_cast<size_t>(row)])
            : std::string_view("APPROVE");
        double score = row < static_cast<int>(result.scores.size())
            ? result.scores[static_cast<size_t>(row)]
            : 0.0;
        std::string_view risk_band = row < static_cast<int>(result.risk_bands.size())
            ? std::string_view(result.risk_bands[static_cast<size_t>(row)])
            : std::string_view("LOW");
        std::string_view winning = row < static_cast<int>(result.winning_rule_ids.size())
            ? std::string_view(result.winning_rule_ids[static_cast<size_t>(row)])
            : std::string_view();

        buffer += "{\"ts_ms\":";
        buffer += std::to_string(result.evaluation_timestamp_ms);
        buffer += ",\"ruleset_version\":\"";
        buffer += json_escape(result.rule_set_version);
        buffer += "\",\"batch_row\":";
        buffer += std::to_string(row);
        buffer += ",\"matched\":";
        buffer += matched[static_cast<size_t>(row)] ? "true" : "false";
        buffer += ",\"decision\":\"";
        buffer += json_escape(decision);
        buffer += "\",\"score\":";
        buffer += std::to_string(score);
        buffer += ",\"risk_band\":\"";
        buffer += json_escape(risk_band);
        buffer += "\",\"winning_rule_id\":\"";
        buffer += json_escape(winning);
        buffer += "\"}\n";
    }
    (*decision_log_stream_) << buffer;
    decision_log_stream_->flush();
    if (!*decision_log_stream_) {
        throw BlazeRulesConfigError({BlazeRulesError::FILE_IO_ERROR,
                              "failed to write decision log: " + config_.decision_log_path,
                              "decision_log", "", -1, BlazeRulesError::Domain::CONFIG});
    }
}

void RuleEngine::install_ruleset(std::shared_ptr<CompiledRuleSet> next) {
    window_store_.configure(next->window_channels);
    configure_projection(*next);
    if (transposer_) {
        transposer_->set_array_any_channels(next->array_any_channels, next->lookups);
    }
    std::atomic_store_explicit(&ruleset_, next, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(reload_status_mutex_);
        hot_reload_status_.active_version = next->version;
    }
    for (auto& shard : partition_shards_) {
        if (shard) {
            std::unique_lock<std::shared_mutex> shard_lock(shard->state_mutex_);
            shard->install_ruleset(next);
        }
    }
}

void RuleEngine::clear_ruleset() {
    std::atomic_store_explicit(&ruleset_, std::shared_ptr<CompiledRuleSet>{},
                               std::memory_order_release);
    for (auto& shard : partition_shards_) {
        if (shard) {
            std::unique_lock<std::shared_mutex> shard_lock(shard->state_mutex_);
            shard->clear_ruleset();
        }
    }
}

void RuleEngine::preload_lookup_dictionaries(const CompiledRuleSet& ruleset) {
    for (const auto& [_, lookup] : ruleset.lookups) {
        if (!lookup || lookup->type != LookupSetType::STRING_SET) continue;
        for (const auto& op : ruleset.global_plan.predicates) {
            const auto* pred = std::get_if<LookupPredicateOp>(&op);
            if (!pred || pred->lookup_name != lookup->name) continue;
            ColumnType t = schema_.type_of(pred->column_index);
            if (t == ColumnType::CATEGORICAL || t == ColumnType::ENTITY_KEY) {
                dict_encoder_->preload(pred->column_index, lookup->strings);
            }
        }
    }
}

namespace {

std::vector<int32_t> resolve_textual_values(const DictEncoder& dict_encoder,
                                            int col_index,
                                            const Textual& values) {
    std::vector<int32_t> ids;
    if (std::holds_alternative<std::string>(values)) {
        ids.push_back(dict_encoder.resolve(col_index, std::get<std::string>(values)));
    } else {
        const auto& raw = std::get<std::vector<std::string>>(values);
        ids.reserve(raw.size());
        for (const auto& value : raw) ids.push_back(dict_encoder.resolve(col_index, value));
    }
    return ids;
}

std::vector<int32_t> resolve_lookup_values(const DictEncoder& dict_encoder,
                                           int col_index,
                                           const std::vector<std::string>& values) {
    std::vector<int32_t> ids;
    ids.reserve(values.size());
    for (const auto& value : values) {
        int32_t id = dict_encoder.resolve(col_index, value);
        if (id >= 0) ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

} // namespace

ResolvedKernelBindings RuleEngine::build_resolved(const CompiledRuleSet& ruleset) const {
    ResolvedKernelBindings resolved;
    resolved.per_rule.resize(ruleset.rules.size());
    for (size_t r = 0; r < ruleset.rules.size(); ++r) {
        const EvalKernelSequence& ks = ruleset.rules[r];
        resolved.per_rule[r].resize(ks.register_count);
        for (const KernelOp& op : ks.op) {
            if (const auto* cat = std::get_if<CategoricalPredicateOp>(&op)) {
                resolved.per_rule[r][cat->output_register] =
                    resolve_textual_values(*dict_encoder_, cat->column_index, cat->raw_values);
            }
        }
    }

    resolved.global_predicates.resize(ruleset.global_plan.predicates.size());
    for (size_t p = 0; p < ruleset.global_plan.predicates.size(); ++p) {
        if (const auto* cat = std::get_if<CategoricalPredicateOp>(&ruleset.global_plan.predicates[p])) {
            resolved.global_predicates[p] =
                resolve_textual_values(*dict_encoder_, cat->column_index, cat->raw_values);
        } else if (const auto* lookup = std::get_if<LookupPredicateOp>(&ruleset.global_plan.predicates[p])) {
            if (lookup->lookup && lookup->lookup->type == LookupSetType::STRING_SET &&
                (lookup->column_type == ColumnType::CATEGORICAL ||
                 lookup->column_type == ColumnType::ENTITY_KEY)) {
                resolved.global_predicates[p] =
                    resolve_lookup_values(*dict_encoder_, lookup->column_index, lookup->lookup->strings);
            }
        }
    }
    return resolved;
}

BatchResult RuleEngine::evaluate_internal(const std::shared_ptr<arrow::RecordBatch>& base,
                                          int messages_processed,
                                          int messages_skipped,
                                          const std::string& last_ingest_error) {
    BatchResult result;
    evaluate_internal_into(base, messages_processed, messages_skipped, last_ingest_error, result);
    return result;
}

void RuleEngine::evaluate_internal_into(const std::shared_ptr<arrow::RecordBatch>& base,
                                        int messages_processed,
                                        int messages_skipped,
                                        const std::string& last_ingest_error,
                                        BatchResult& result) {
    auto total_start = std::chrono::steady_clock::now();
    auto rs = active_ruleset();
    if (!rs) {
        result = BatchResult{};
        return;
    }
    validate_record_batch_schema(base);

    if (!config_.result_buffer_reuse || result.rule_set_version != rs->version) {
        result.rule_bitmasks.clear();
    }
    int64_t incoming_transpose_us = result.timing.transpose_us;
    result.timing = BatchResult::Timing{};
    result.timing.transpose_us = incoming_transpose_us;
    result.rule_match_counts.clear();
    result.matched_record_indices.clear();
    result.explanations.clear();

    result.messages_processed = messages_processed;
    result.messages_skipped = messages_skipped;
    result.last_ingest_error = last_ingest_error;
    result.evaluation_timestamp_ms = epoch_millis();

    auto stage = std::chrono::steady_clock::now();
    auto encoded = dict_encoder_->encode_batch(base);
    result.timing.dict_encode_us = micros_since(stage);

    int64_t ts = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    stage = std::chrono::steady_clock::now();
    std::vector<std::vector<double>> totals;
    window_store_.query_all_channels(*encoded, ts, totals);
    result.timing.window_read_us = micros_since(stage);

    // Derived-score pre-passes: compute each ML model score column and each vector-
    // similarity column once per batch (read feature/embedding columns from the dict-
    // encoded batch). Both early-return when no rule uses them -> zero added hot-path cost.
    stage = std::chrono::steady_clock::now();
    std::vector<std::vector<double>> model_scores;
    model_registry_.score_all_channels(*encoded, rs->model_channels, model_scores);
    std::vector<std::vector<double>> vector_scores;
    compute_vector_channels(*encoded, rs->vector_channels, vector_scores);
    result.timing.model_score_us = micros_since(stage);

    stage = std::chrono::steady_clock::now();
    // Map each derived-column slot to its computed per-row values. Window slots read from
    // `totals`, model slots from `model_scores`, vector slots from `vector_scores`. TS
    // producers wire in the same way in a later phase.
    std::vector<const double*> slot_values(rs->derived_plan.slots.size(), nullptr);
    for (size_t k = 0; k < rs->derived_plan.slots.size(); ++k) {
        const auto& slot = rs->derived_plan.slots[k];
        if (slot.kind == DerivedColumnKind::WINDOW &&
            slot.producer_index < static_cast<int>(totals.size())) {
            slot_values[k] = totals[slot.producer_index].data();
        } else if (slot.kind == DerivedColumnKind::MODEL_SCORE &&
                   slot.producer_index < static_cast<int>(model_scores.size())) {
            slot_values[k] = model_scores[slot.producer_index].data();
        } else if (slot.kind == DerivedColumnKind::VECTOR_DISTANCE &&
                   slot.producer_index < static_cast<int>(vector_scores.size())) {
            slot_values[k] = vector_scores[slot.producer_index].data();
        }
    }
    auto augmented = inject_derived_columns(encoded, rs->derived_plan, slot_values);
    result.timing.window_inject_us = micros_since(stage);

    stage = std::chrono::steady_clock::now();
    auto resolved = build_resolved(*rs);
    result.timing.kernel_bind_us = micros_since(stage);

    stage = std::chrono::steady_clock::now();
    evaluate_rules(*augmented, *rs, resolved, eval_options(), result);
    result.timing.evaluation_us = micros_since(stage);

    stage = std::chrono::steady_clock::now();
    window_store_.update_all_channels(*encoded, ts);
    result.timing.window_write_us = micros_since(stage);

    result.messages_processed = messages_processed;
    result.messages_skipped = messages_skipped;
    result.last_ingest_error = last_ingest_error;
    result.evaluation_timestamp_ms = epoch_millis();
    result.timing.total_us = result.timing.transpose_us + micros_since(total_start);

    ++stats_.batches_evaluated;
    stats_.records_evaluated += result.n_records;
    stats_.records_skipped += result.messages_skipped;
    emit_batch_metrics(result);
    emit_decision_log(result);
}

BatchResult RuleEngine::evaluate_messages(const std::vector<std::string>& messages) {
    BatchResult result;
    evaluate_messages_into(messages, result);
    return result;
}

void RuleEngine::evaluate_messages_into(const std::vector<std::string>& messages, BatchResult& result) {
    std::unique_lock<std::shared_mutex> state_lock(state_mutex_);
    std::vector<std::string_view> views;
    views.reserve(messages.size());
    for (const std::string& m : messages) views.emplace_back(m.data(), m.size());
    ensure_schema_bound_from_messages(views);

    auto stage = std::chrono::steady_clock::now();
    transposer_->reset();
    transposer_->set_max_error_samples(config_.max_error_samples);
    transposer_->reserve(static_cast<int>(messages.size()));
    transposer_->add_json_messages(views);
    int skipped = transposer_->skipped_count();
    std::string last = transposer_->last_error();
    auto batch = transposer_->finish();
    result.timing.transpose_us = micros_since(stage);
    result.messages_processed = static_cast<int>(messages.size());
    result.messages_skipped = skipped;
    result.last_ingest_error = last;
    result.evaluation_timestamp_ms = epoch_millis();
    apply_ingest_error_policy(*transposer_, result);
    evaluate_internal_into(batch, static_cast<int>(messages.size()), skipped, last, result);
}

BatchResult RuleEngine::evaluate_message_views(const std::vector<std::string_view>& messages) {
    BatchResult result;
    evaluate_message_views_into(messages, result);
    return result;
}

void RuleEngine::evaluate_message_views_into(const std::vector<std::string_view>& messages,
                                             BatchResult& result) {
    std::unique_lock<std::shared_mutex> state_lock(state_mutex_);
    ensure_schema_bound_from_messages(messages);
    auto stage = std::chrono::steady_clock::now();
    transposer_->reset();
    transposer_->set_max_error_samples(config_.max_error_samples);
    transposer_->reserve(static_cast<int>(messages.size()));
    transposer_->add_json_messages(messages);
    int skipped = transposer_->skipped_count();
    std::string last = transposer_->last_error();
    auto batch = transposer_->finish();
    result.timing.transpose_us = micros_since(stage);
    result.messages_processed = static_cast<int>(messages.size());
    result.messages_skipped = skipped;
    result.last_ingest_error = last;
    result.evaluation_timestamp_ms = epoch_millis();
    apply_ingest_error_policy(*transposer_, result);
    evaluate_internal_into(batch, static_cast<int>(messages.size()), skipped, last, result);
}

BatchResult RuleEngine::evaluate_ndjson(std::string_view ndjson_bytes) {
    BatchResult result;
    evaluate_ndjson_into(ndjson_bytes, result);
    return result;
}

void RuleEngine::evaluate_ndjson_into(std::string_view ndjson_bytes, BatchResult& result) {
    std::unique_lock<std::shared_mutex> state_lock(state_mutex_);
    ensure_schema_bound_from_ndjson(ndjson_bytes);
    auto stage = std::chrono::steady_clock::now();
    transposer_->reset();
    transposer_->set_max_error_samples(config_.max_error_samples);
    transposer_->add_ndjson(ndjson_bytes, config_.eval_thread_count);
    int processed = transposer_->current_size() + transposer_->skipped_count();
    int skipped = transposer_->skipped_count();
    std::string last = transposer_->last_error();
    auto batch = transposer_->finish();
    result.timing.transpose_us = micros_since(stage);
    result.messages_processed = processed;
    result.messages_skipped = skipped;
    result.last_ingest_error = last;
    result.evaluation_timestamp_ms = epoch_millis();
    apply_ingest_error_policy(*transposer_, result);
    evaluate_internal_into(batch, processed, skipped, last, result);
}

BatchResult RuleEngine::evaluate_ndjson_padded(std::string_view ndjson_bytes) {
    BatchResult result;
    evaluate_ndjson_padded_into(ndjson_bytes, result);
    return result;
}

void RuleEngine::evaluate_ndjson_padded_into(std::string_view ndjson_bytes, BatchResult& result) {
    std::unique_lock<std::shared_mutex> state_lock(state_mutex_);
    ensure_schema_bound_from_ndjson(ndjson_bytes);
    auto stage = std::chrono::steady_clock::now();
    transposer_->reset();
    transposer_->set_max_error_samples(config_.max_error_samples);
    transposer_->add_ndjson_padded(ndjson_bytes, config_.eval_thread_count);
    int processed = transposer_->current_size() + transposer_->skipped_count();
    int skipped = transposer_->skipped_count();
    std::string last = transposer_->last_error();
    auto batch = transposer_->finish();
    result.timing.transpose_us = micros_since(stage);
    result.messages_processed = processed;
    result.messages_skipped = skipped;
    result.last_ingest_error = last;
    result.evaluation_timestamp_ms = epoch_millis();
    apply_ingest_error_policy(*transposer_, result);
    evaluate_internal_into(batch, processed, skipped, last, result);
}

BatchResult RuleEngine::evaluate_record_batch(const std::shared_ptr<arrow::RecordBatch>& batch) {
    BatchResult result;
    evaluate_record_batch_into(batch, result);
    return result;
}

void RuleEngine::evaluate_record_batch_into(const std::shared_ptr<arrow::RecordBatch>& batch,
                                            BatchResult& out) {
    std::unique_lock<std::shared_mutex> state_lock(state_mutex_);
    out.timing.transpose_us = 0;
    out.error_counts.clear();
    out.error_samples.clear();
    ensure_schema_bound_from_arrow(batch);
    auto aligned = align_record_batch_to_schema(batch);
    validate_record_batch_schema(aligned);
    evaluate_internal_into(aligned, static_cast<int>(aligned->num_rows()), 0, {}, out);
}

std::vector<std::unique_ptr<RuleEngine>> RuleEngine::create_shards(int shard_count) const {
    std::shared_lock<std::shared_mutex> state_lock(state_mutex_);
    std::vector<std::unique_ptr<RuleEngine>> shards;
    if (shard_count <= 0) return shards;
    shards.reserve(shard_count);
    auto rs = active_ruleset();
    for (int i = 0; i < shard_count; ++i) {
        auto shard = std::make_unique<RuleEngine>(config_);
        if (schema_state_ != SchemaState::UNBOUND) {
            shard->bind_schema(BlazeRulesSchema(schema_.all_fields()), schema_state_);
        }
        shard->pending_rules_ = pending_rules_;
        shard->pending_rules_path_ = pending_rules_path_;
        shard->pending_conflict_report_ = pending_conflict_report_;
        if (rs) shard->install_ruleset(rs);
        shards.push_back(std::move(shard));
    }
    return shards;
}

void RuleEngine::ensure_partition_shards(int shard_count) {
    std::unique_lock<std::shared_mutex> state_lock(state_mutex_);
    if (shard_count <= static_cast<int>(partition_shards_.size())) return;
    auto rs = active_ruleset();
    int old_size = static_cast<int>(partition_shards_.size());
    partition_shards_.reserve(shard_count);
    for (int i = old_size; i < shard_count; ++i) {
        auto shard = std::make_unique<RuleEngine>(config_);
        if (schema_state_ != SchemaState::UNBOUND) {
            shard->bind_schema(BlazeRulesSchema(schema_.all_fields()), schema_state_);
        }
        shard->pending_rules_ = pending_rules_;
        shard->pending_rules_path_ = pending_rules_path_;
        shard->pending_conflict_report_ = pending_conflict_report_;
        if (rs) shard->install_ruleset(rs);
        partition_shards_.push_back(std::move(shard));
    }
}

BatchResult RuleEngine::evaluate_partition(int partition_id,
                                           const std::vector<std::string>& messages) {
    if (partition_id < 0) partition_id = 0;
    ensure_partition_shards(partition_id + 1);
    RuleEngine* shard = nullptr;
    {
        std::shared_lock<std::shared_mutex> state_lock(state_mutex_);
        shard = partition_shards_[partition_id].get();
    }
    return shard->evaluate_messages(messages);
}

BatchResult RuleEngine::evaluate_partition(int partition_id,
                                           const std::vector<std::string_view>& messages) {
    if (partition_id < 0) partition_id = 0;
    ensure_partition_shards(partition_id + 1);
    RuleEngine* shard = nullptr;
    {
        std::shared_lock<std::shared_mutex> state_lock(state_mutex_);
        shard = partition_shards_[partition_id].get();
    }
    return shard->evaluate_message_views(messages);
}

BatchResult RuleEngine::evaluate_partition_ndjson_padded(int partition_id,
                                                         std::string_view ndjson_bytes) {
    if (partition_id < 0) partition_id = 0;
    ensure_partition_shards(partition_id + 1);
    RuleEngine* shard = nullptr;
    {
        std::shared_lock<std::shared_mutex> state_lock(state_mutex_);
        shard = partition_shards_[partition_id].get();
    }
    return shard->evaluate_ndjson_padded(ndjson_bytes);
}

BatchResult RuleEngine::evaluate_partition(int partition_id,
                                           const std::shared_ptr<arrow::RecordBatch>& batch) {
    if (partition_id < 0) partition_id = 0;
    ensure_partition_shards(partition_id + 1);
    RuleEngine* shard = nullptr;
    {
        std::shared_lock<std::shared_mutex> state_lock(state_mutex_);
        shard = partition_shards_[partition_id].get();
    }
    return shard->evaluate_record_batch(batch);
}

void RuleEngine::reset_window_state() {
    std::unique_lock<std::shared_mutex> state_lock(state_mutex_);
    window_store_.reset_all();
}

void RuleEngine::register_model(const std::string& name, const std::string& path) {
    std::unique_lock<std::shared_mutex> state_lock(state_mutex_);
    model_registry_.register_model(name, path);
}

int RuleEngine::num_window_channels() const {
    auto rs = active_ruleset();
    return rs ? static_cast<int>(rs->window_channels.size()) : 0;
}

void RuleEngine::set_metrics_emitter(std::shared_ptr<MetricsEmitter> emitter) {
    std::unique_lock<std::shared_mutex> state_lock(state_mutex_);
    metrics_emitter_ = emitter ? std::move(emitter) : std::make_shared<NoopMetricsEmitter>();
}

void RuleEngine::emit_batch_metrics(const BatchResult& result) {
    // Skip entirely on the default (Noop) path -> the 10M benchmark pays nothing,
    // not even the per-rule loop. All per-batch metric work is opt-in via enable_metrics().
    if (!metrics_emitter_ || !metrics_emitter_->enabled()) return;
    metrics_emitter_->increment_counter("blazerules.records_evaluated_total", result.n_records, {});
    metrics_emitter_->increment_counter("blazerules.batches_evaluated_total", 1, {});
    metrics_emitter_->increment_counter("blazerules.records_skipped_total", result.messages_skipped, {});
    metrics_emitter_->increment_counter("blazerules.records_matched_total", result.n_matched, {});
    metrics_emitter_->observe_histogram("blazerules.batch_total_latency_us", result.timing.total_us, {});
    metrics_emitter_->observe_histogram("blazerules.batch_evaluation_latency_us", result.timing.evaluation_us, {});
    metrics_emitter_->observe_histogram("blazerules.batch_transpose_latency_us", result.timing.transpose_us, {});
    for (const auto& [rule_id, count] : result.rule_match_counts) {
        metrics_emitter_->increment_counter("blazerules.rule_fired_total", count, {{"rule_id", rule_id}});
        double fire_rate = result.n_records > 0 ? static_cast<double>(count) / result.n_records : 0.0;
        metrics_emitter_->set_gauge("blazerules.rule_fire_rate", fire_rate, {{"rule_id", rule_id}});
    }
    // Per-action decision counts (O(n); runs only when metrics are enabled).
    for (const auto& decision : result.decisions) {
        if (!decision.empty()) {
            metrics_emitter_->increment_counter("blazerules.decisions_total", 1, {{"action", decision}});
        }
    }
}

void RuleEngine::enable_metrics() {
    std::unique_lock<std::shared_mutex> state_lock(state_mutex_);
    collecting_metrics_ = std::make_shared<CollectingMetricsEmitter>();
    metrics_emitter_ = collecting_metrics_;
}

void RuleEngine::reset_metrics() {
    std::shared_lock<std::shared_mutex> state_lock(state_mutex_);
    if (collecting_metrics_) collecting_metrics_->reset();
}

std::map<std::string, int64_t> RuleEngine::metrics_counters() const {
    return collecting_metrics_ ? collecting_metrics_->counters() : std::map<std::string, int64_t>{};
}

std::map<std::string, double> RuleEngine::metrics_gauges() const {
    return collecting_metrics_ ? collecting_metrics_->gauges() : std::map<std::string, double>{};
}

std::map<std::string, HistogramSnapshot> RuleEngine::metrics_histograms() const {
    return collecting_metrics_ ? collecting_metrics_->histograms()
                               : std::map<std::string, HistogramSnapshot>{};
}

BatchResult RuleEngine::evaluate_ndjson_file(const std::string& path) {
    MmappedNdjson reader(path);
    if (!reader.ok()) {
        BatchResult result;
        result.last_ingest_error = reader.error();
        return result;
    }
    return evaluate_ndjson_padded(reader.view());
}

BacktestReport RuleEngine::backtest(const BacktestConfig& config) {
    RuleEngine engine_a(config_);
    RuleEngine engine_b(config_);
    {
        std::shared_lock<std::shared_mutex> state_lock(state_mutex_);
        if (schema_state_ != SchemaState::UNBOUND) {
            engine_a.bind_schema(BlazeRulesSchema(schema_.all_fields()), schema_state_);
            engine_b.bind_schema(BlazeRulesSchema(schema_.all_fields()), schema_state_);
        }
    }
    engine_a.load_rules(config.rules_file_a);
    engine_b.load_rules(config.rules_file_b);

    BacktestReport report;
    report.rule_set_a_name = config.rules_file_a;
    report.rule_set_b_name = config.rules_file_b;

    int64_t a_only = 0, b_only = 0, both = 0;
    int64_t matched_a = 0, matched_b = 0;
    absl::flat_hash_map<std::string, int64_t> per_rule_counts;

    for (const auto& file : expand_parquet_paths(config.parquet_paths)) {
        auto infile_res = arrow::io::ReadableFile::Open(file);
        if (!infile_res.ok()) continue;
        std::shared_ptr<arrow::io::ReadableFile> infile = *infile_res;

        auto reader_res = parquet::arrow::OpenFile(infile, arrow::default_memory_pool());
        if (!reader_res.ok()) continue;
        std::unique_ptr<parquet::arrow::FileReader> reader = std::move(*reader_res);
        reader->set_use_threads(true);
        reader->set_batch_size(config.batch_size);
        auto batch_reader_res = reader->GetRecordBatchReader();
        if (!batch_reader_res.ok()) continue;
        std::unique_ptr<arrow::RecordBatchReader> batch_reader = std::move(*batch_reader_res);
        std::shared_ptr<arrow::RecordBatch> batch;
        while (batch_reader->ReadNext(&batch).ok() && batch) {
            BatchResult ra = engine_a.evaluate_record_batch(batch);
            BatchResult rb = engine_b.evaluate_record_batch(batch);
            report.total_records += batch->num_rows();
            matched_a += ra.n_matched;
            matched_b += rb.n_matched;
            accumulate_diff(ra, rb, a_only, b_only, both);
            for (const auto& [rule_id, count] : rb.rule_match_counts) per_rule_counts[rule_id] += count;
        }
    }

    report.new_positives = b_only;
    report.lost_positives = a_only;
    int64_t neither = report.total_records - a_only - b_only - both;
    report.agreement_rate = report.total_records > 0
        ? static_cast<double>(both + neither) / report.total_records : 0.0;
    report.fire_rate_a = report.total_records > 0 ? static_cast<double>(matched_a) / report.total_records : 0.0;
    report.fire_rate_b = report.total_records > 0 ? static_cast<double>(matched_b) / report.total_records : 0.0;
    for (const auto& [rule_id, count] : per_rule_counts) {
        report.per_rule_fire_rates[rule_id] =
            report.total_records > 0 ? static_cast<double>(count) / report.total_records : 0.0;
    }
    return report;
}

BacktestReport RuleEngine::backtest(const std::vector<std::string>& parquet_paths,
                                    const std::string& rules_a,
                                    const std::string& rules_b,
                                    const std::string& label_column) {
    BacktestConfig cfg;
    cfg.parquet_paths = parquet_paths;
    cfg.rules_file_a = rules_a;
    cfg.rules_file_b = rules_b;
    cfg.label_column = label_column;
    return backtest(cfg);
}
