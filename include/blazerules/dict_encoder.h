#ifndef BLAZERULES_DICT_ENCODER_H
#define BLAZERULES_DICT_ENCODER_H

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <arrow/api.h>

#include "schema.h"

class DictEncoder {
public:
    explicit DictEncoder(const BlazeRulesSchema& schema, int max_dict_size_per_column = 100000);

    std::shared_ptr<arrow::RecordBatch> encode_batch(const std::shared_ptr<arrow::RecordBatch>& in);

    int32_t resolve(int col_index, std::string_view value) const;
    void preload(int col_index, const std::vector<std::string>& values);
    std::string_view reverse_lookup(int col_index, int32_t id) const;
    int dict_size(int col_index) const;
    const absl::flat_hash_map<std::string, int32_t>& get_dict(int col_index) const;

    uint64_t generation() const { return generation_; }
    void reset();

private:
    int32_t get_or_create(int col_index, std::string_view value);

    const BlazeRulesSchema& schema_;
    std::shared_ptr<arrow::Schema> encoded_schema_;
    std::vector<absl::flat_hash_map<std::string, int32_t>> dicts_;
    std::vector<std::vector<std::string>> reverse_;
    int max_dict_size_per_column_;
    uint64_t generation_ = 0;
};

#endif // BLAZERULES_DICT_ENCODER_H
