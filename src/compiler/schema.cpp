#include "blazerules/schema.h"

#include "blazerules/arrow_util.h"

BlazeRulesSchema::BlazeRulesSchema(std::vector<FieldSpec> input_fields)
    : fields_(std::move(input_fields)),
      ingested_columns_(static_cast<int>(fields_.size())) {
    for (size_t i = 0; i < fields_.size(); ++i) {
        name_to_index_.emplace(fields_[i].name, static_cast<int>(i));
        if (fields_[i].type == ColumnType::ENTITY_KEY) {
            fields_[i].is_entity_field = true;
        }
    }
}

bool BlazeRulesSchema::has_field(const std::string& name) const {
    return name_to_index_.contains(name);
}

int BlazeRulesSchema::index_of(const std::string& name) const {
    return name_to_index_.find(name)->second;
}

ColumnType BlazeRulesSchema::type_of(int index) const {
    return fields_[index].type;
}

const FieldSpec& BlazeRulesSchema::field_at(int index) const {
    return fields_[index];
}

const std::string& BlazeRulesSchema::name_at(int index) const {
    return fields_[index].name;
}

int BlazeRulesSchema::num_fields() const {
    return static_cast<int>(fields_.size());
}

std::shared_ptr<arrow::Schema> BlazeRulesSchema::arrow_schema() const {
    return blazerules::build_ingest_schema(*this);
}

BlazeRulesSchema BlazeRulesSchema::with_window_columns(const std::vector<WindowColumnSpec>& window_cols) const {
    std::vector<FieldSpec> next = fields_;
    next.reserve(fields_.size() + window_cols.size());
    for (const auto& col : window_cols) {
        FieldSpec f;
        f.name = col.name;
        f.type = col.type;
        f.nullable = false;
        f.source = FieldSpec::INFERRED_AND_BOUND;
        next.push_back(std::move(f));
    }
    BlazeRulesSchema out(std::move(next));
    out.ingested_columns_ = ingested_columns_;
    return out;
}
