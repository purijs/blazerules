#ifndef BLAZERULES_ARROW_UTIL_H
#define BLAZERULES_ARROW_UTIL_H

#include <arrow/api.h>
#include "schema.h"

namespace blazerules {

inline std::shared_ptr<arrow::DataType> ingest_arrow_type(ColumnType t) {
    switch (t) {
        case ColumnType::FLOAT32: return arrow::float32();
        case ColumnType::FLOAT64: return arrow::float64();
        case ColumnType::INT32:   return arrow::int32();
        case ColumnType::INT64:   return arrow::int64();
        case ColumnType::TIMESTAMP_MS: return arrow::int64();
        case ColumnType::BOOLEAN: return arrow::boolean();
        case ColumnType::STRING: return arrow::utf8();
        case ColumnType::CATEGORICAL:
        case ColumnType::ENTITY_KEY: return arrow::utf8();
    }
    return arrow::float64();
}

inline std::shared_ptr<arrow::DataType> encoded_arrow_type(ColumnType t) {
    if (t == ColumnType::CATEGORICAL || t == ColumnType::ENTITY_KEY) return arrow::int32();
    return ingest_arrow_type(t);
}

inline std::shared_ptr<arrow::Schema> build_ingest_schema(const BlazeRulesSchema& s) {
    std::vector<std::shared_ptr<arrow::Field>> fields;
    fields.reserve(s.num_fields());
    for (int i = 0; i < s.num_fields(); ++i) {
        const FieldSpec& f = s.field_at(i);
        fields.push_back(arrow::field(f.name, ingest_arrow_type(f.type), f.nullable));
    }
    return arrow::schema(fields);
}

inline std::shared_ptr<arrow::Schema> build_encoded_schema(const BlazeRulesSchema& s) {
    std::vector<std::shared_ptr<arrow::Field>> fields;
    fields.reserve(s.num_fields());
    for (int i = 0; i < s.num_fields(); ++i) {
        const FieldSpec& f = s.field_at(i);
        fields.push_back(arrow::field(f.name, encoded_arrow_type(f.type), f.nullable));
    }
    return arrow::schema(fields);
}

} // namespace blazerules

#endif //BLAZERULES_ARROW_UTIL_H
