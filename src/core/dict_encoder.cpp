#include "blazerules/dict_encoder.h"

#include "blazerules/arrow_util.h"

#include <iomanip>
#include <istream>
#include <limits>
#include <ostream>

DictEncoder::DictEncoder(const BlazeRulesSchema& schema, int max_dict_size_per_column)
    : schema_(schema),
      encoded_schema_(blazerules::build_encoded_schema(schema)),
      dicts_(schema.num_fields()),
      reverse_(schema.num_fields()),
      max_dict_size_per_column_(max_dict_size_per_column) {
    for (int c = 0; c < schema_.num_fields(); ++c) {
        const FieldSpec& field = schema_.field_at(c);
        if (!is_string_like(field.type) || field.closed_values.empty()) continue;
        dicts_[c].reserve(field.closed_values.size());
        reverse_[c].reserve(field.closed_values.size());
        for (const auto& value : field.closed_values) {
            (void)get_or_create(c, value);
        }
    }
}

int32_t DictEncoder::get_or_create(int col_index, std::string_view value) {
    auto& dict = dicts_[col_index];
    auto it = dict.find(value);
    if (it != dict.end()) return it->second;
    if (schema_.type_of(col_index) != ColumnType::ENTITY_KEY &&
        static_cast<int>(dict.size()) >= max_dict_size_per_column_) {
        return -1;
    }
    if (dict.size() >= static_cast<size_t>(std::numeric_limits<int32_t>::max())) return -1;

    int32_t id = static_cast<int32_t>(dict.size());
    dict.emplace(std::string(value), id);
    reverse_[col_index].emplace_back(value);
    return id;
}

int32_t DictEncoder::resolve(int col_index, std::string_view value) const {
    const auto& dict = dicts_[col_index];
    auto it = dict.find(value);
    return it == dict.end() ? -1 : it->second;
}

void DictEncoder::preload(int col_index, const std::vector<std::string>& values) {
    auto& dict = dicts_[col_index];
    dict.reserve(dict.size() + values.size());
    reverse_[col_index].reserve(reverse_[col_index].size() + values.size());
    for (const auto& value : values) {
        (void)get_or_create(col_index, value);
    }
    ++generation_;
}

std::string_view DictEncoder::reverse_lookup(int col_index, int32_t id) const {
    const auto& rev = reverse_[col_index];
    if (id < 0 || static_cast<size_t>(id) >= rev.size()) return {};
    return rev[static_cast<size_t>(id)];
}

int DictEncoder::dict_size(int col_index) const {
    return static_cast<int>(dicts_[col_index].size());
}

const absl::flat_hash_map<std::string, int32_t>& DictEncoder::get_dict(int col_index) const {
    return dicts_[col_index];
}

void DictEncoder::reset() {
    for (auto& d : dicts_) d.clear();
    for (auto& r : reverse_) r.clear();
    ++generation_;
}

std::shared_ptr<arrow::RecordBatch> DictEncoder::encode_batch(
        const std::shared_ptr<arrow::RecordBatch>& in) {
    int n = static_cast<int>(in->num_rows());
    std::vector<std::shared_ptr<arrow::Array>> columns;
    columns.reserve(schema_.num_fields());

    for (int c = 0; c < schema_.num_fields(); ++c) {
        ColumnType t = schema_.type_of(c);
        if (t == ColumnType::CATEGORICAL || t == ColumnType::ENTITY_KEY) {
            if (in->column(c)->type_id() == arrow::Type::INT32 ||
                in->column(c)->type_id() == arrow::Type::INT64) {
                columns.push_back(in->column(c));
                continue;
            }
            if (in->column(c)->type_id() == arrow::Type::DICTIONARY) {
                auto dict_col = std::static_pointer_cast<arrow::DictionaryArray>(in->column(c));
                if (dict_col->indices()->type_id() == arrow::Type::INT32 &&
                    dict_col->dictionary()->type_id() == arrow::Type::STRING) {
                    auto indices = std::static_pointer_cast<arrow::Int32Array>(dict_col->indices());
                    auto dictionary = std::static_pointer_cast<arrow::StringArray>(dict_col->dictionary());
                    std::vector<int32_t> mapped_dictionary(static_cast<size_t>(dictionary->length()));
                    for (int64_t d = 0; d < dictionary->length(); ++d) {
                        mapped_dictionary[static_cast<size_t>(d)] =
                            dictionary->IsNull(d) ? -1 : get_or_create(c, dictionary->GetView(d));
                    }

                    const int32_t* raw_indices = indices->raw_values();
                    const int64_t dict_len = dictionary->length();
                    arrow::Int32Builder dict_builder;
                    (void)dict_builder.Reserve(n);
                    for (int i = 0; i < n; ++i) {
                        // A null index slot, an out-of-range index, or an index that maps
                        // to a null dictionary value (-1) all yield a null output slot —
                        // preserving null-ness so IS_NULL / IN / lookup decisions stay correct.
                        if (indices->IsNull(i)) { (void)dict_builder.AppendNull(); continue; }
                        const int32_t di = raw_indices[i];
                        if (di < 0 || di >= dict_len) { (void)dict_builder.AppendNull(); continue; }
                        const int32_t code = mapped_dictionary[static_cast<size_t>(di)];
                        if (code < 0) (void)dict_builder.AppendNull();
                        else (void)dict_builder.Append(code);
                    }
                    std::shared_ptr<arrow::Array> dict_arr;
                    (void)dict_builder.Finish(&dict_arr);
                    columns.push_back(std::move(dict_arr));
                    continue;
                }
            }
            auto str_col = std::static_pointer_cast<arrow::StringArray>(in->column(c));
            auto reserve_target = static_cast<size_t>(dicts_[c].size()) +
                (t == ColumnType::ENTITY_KEY ? static_cast<size_t>(n) : size_t{1024});
            if (t != ColumnType::ENTITY_KEY) {
                reserve_target = std::min(reserve_target, static_cast<size_t>(max_dict_size_per_column_));
            }
            dicts_[c].reserve(reserve_target);
            reverse_[c].reserve(reserve_target);
            if (str_col->null_count() == 0) {
                auto buffer = arrow::AllocateBuffer(static_cast<int64_t>(n) * sizeof(int32_t)).ValueOrDie();
                auto* ids = reinterpret_cast<int32_t*>(buffer->mutable_data());
                const int32_t* offsets = str_col->raw_value_offsets();
                const auto* value_data = reinterpret_cast<const char*>(str_col->value_data()->data());
                for (int i = 0; i < n; ++i) {
                    ids[i] = get_or_create(c, std::string_view(value_data + offsets[i],
                                                               offsets[i + 1] - offsets[i]));
                }
                auto array_data = arrow::ArrayData::Make(arrow::int32(), n, {nullptr, std::move(buffer)}, 0);
                columns.push_back(arrow::MakeArray(array_data));
            } else {
                arrow::Int32Builder builder;
                (void)builder.Reserve(n);
                for (int i = 0; i < n; ++i) {
                    if (str_col->IsNull(i)) {
                        (void)builder.AppendNull();
                    } else {
                        int32_t id = get_or_create(c, str_col->GetView(i));
                        (void)builder.Append(id);
                    }
                }
                std::shared_ptr<arrow::Array> arr;
                (void)builder.Finish(&arr);
                columns.push_back(std::move(arr));
            }
        } else {
            columns.push_back(in->column(c));
        }
    }

    return arrow::RecordBatch::Make(encoded_schema_, n, columns);
}
