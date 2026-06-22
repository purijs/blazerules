#ifndef BLAZERULES_SCHEMA_H
#define BLAZERULES_SCHEMA_H

#include <memory>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <arrow/api.h>

enum class ColumnType {
    FLOAT32 = 0,
    FLOAT64 = 1,
    INT32 = 2,
    INT64 = 3,
    CATEGORICAL = 4,
    ENTITY_KEY = 5,
    TIMESTAMP_MS = 6,
    BOOLEAN = 7,
    STRING = 8
};

inline bool is_numeric(ColumnType t) {
    return t == ColumnType::FLOAT32 || t == ColumnType::FLOAT64 ||
           t == ColumnType::INT32 || t == ColumnType::INT64 ||
           t == ColumnType::TIMESTAMP_MS;
}

inline bool is_string_like(ColumnType t) {
    return t == ColumnType::CATEGORICAL || t == ColumnType::ENTITY_KEY;
}

inline bool is_text(ColumnType t) {
    return t == ColumnType::STRING || t == ColumnType::CATEGORICAL ||
           t == ColumnType::ENTITY_KEY;
}

inline int numeric_type_index(ColumnType t) {
    switch (t) {
        case ColumnType::FLOAT32: return 0;
        case ColumnType::FLOAT64: return 1;
        case ColumnType::INT32: return 2;
        case ColumnType::INT64:
        case ColumnType::TIMESTAMP_MS: return 3;
        default: return 2;
    }
}

struct FieldSpec {
    enum SchemaSource {
        USER_PROVIDED_JSON,
        DERIVED_FROM_ARROW,
        INFERRED_AND_BOUND
    };

    std::string name;
    ColumnType type = ColumnType::FLOAT64;
    bool nullable = true;
    bool is_entity_field = false;
    SchemaSource source = USER_PROVIDED_JSON;
    std::vector<ColumnType> coerce_from;
    std::vector<std::string> closed_values; // optional fixed enumeration
};

struct WindowColumnSpec {
    std::string name;
    ColumnType type = ColumnType::INT32;
};

class BlazeRulesSchema {
public:
    BlazeRulesSchema() = default;
    explicit BlazeRulesSchema(std::vector<FieldSpec> fields);

    bool has_field(const std::string& name) const;
    int index_of(const std::string& name) const;
    int find_column_index(const std::string& name) const { return index_of(name); }
    ColumnType type_of(int index) const;
    ColumnType get_column_type(int index) const { return type_of(index); }
    bool is_nullable(int index) const { return fields_[index].nullable; }
    bool is_entity_field(int index) const { return fields_[index].is_entity_field; }
    const FieldSpec& field_at(int index) const;
    const std::string& name_at(int index) const;
    const std::string& column_name(int index) const { return name_at(index); }
    int num_fields() const;
    int num_columns() const { return num_fields(); }
    int num_ingested_columns() const { return ingested_columns_; }
    int num_window_columns() const { return num_fields() - ingested_columns_; }
    const std::vector<FieldSpec>& all_fields() const { return fields_; }

    std::shared_ptr<arrow::Schema> arrow_schema() const;
    BlazeRulesSchema with_window_columns(const std::vector<WindowColumnSpec>& window_cols) const;

private:
    std::vector<FieldSpec> fields_;
    absl::flat_hash_map<std::string, int> name_to_index_;
    int ingested_columns_ = 0;
};

#endif // BLAZERULES_SCHEMA_H
