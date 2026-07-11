#include <gtest/gtest.h>

#include "blazerules/dict_encoder.h"
#include "blazerules/schema.h"

#include <arrow/api.h>

#include <memory>
#include <string>
#include <vector>

// These tests lock in null preservation when re-encoding an already dictionary-encoded
// input column. A null index, an out-of-range index, or an index that maps to a null
// dictionary value must all become a null output slot — otherwise IS_NULL, IN, and
// lookup semantics silently see a bogus code (index 0 / empty string) instead of null.

namespace {

std::shared_ptr<arrow::Array> int32_indices(const std::vector<int32_t>& vals,
                                            const std::vector<bool>& valid) {
    arrow::Int32Builder b;
    for (size_t i = 0; i < vals.size(); ++i) {
        if (valid[i]) (void)b.Append(vals[i]);
        else (void)b.AppendNull();
    }
    std::shared_ptr<arrow::Array> a;
    (void)b.Finish(&a);
    return a;
}

std::shared_ptr<arrow::Array> string_dict(const std::vector<std::string>& vals,
                                          const std::vector<bool>& valid) {
    arrow::StringBuilder b;
    for (size_t i = 0; i < vals.size(); ++i) {
        if (valid[i]) (void)b.Append(vals[i]);
        else (void)b.AppendNull();
    }
    std::shared_ptr<arrow::Array> a;
    (void)b.Finish(&a);
    return a;
}

BlazeRulesSchema one_categorical_schema() {
    FieldSpec f;
    f.name = "cat";
    f.type = ColumnType::CATEGORICAL;
    f.nullable = true;
    return BlazeRulesSchema(std::vector<FieldSpec>{f});
}

// Build a one-column batch whose column is a dictionary<int32, utf8> array. Uses the
// raw DictionaryArray constructor (not FromArrays) so deliberately out-of-range indices
// are allowed through for the encoder to defend against.
std::shared_ptr<arrow::RecordBatch> dict_batch(const std::shared_ptr<arrow::Array>& indices,
                                               const std::shared_ptr<arrow::Array>& dictionary) {
    auto type = arrow::dictionary(arrow::int32(), arrow::utf8());
    auto dict_arr = std::make_shared<arrow::DictionaryArray>(type, indices, dictionary);
    auto schema = arrow::schema({arrow::field("cat", type)});
    return arrow::RecordBatch::Make(schema, indices->length(),
                                    {std::static_pointer_cast<arrow::Array>(dict_arr)});
}

}  // namespace

TEST(DictEncoderTest, NullAndOutOfRangeIndexBecomeNull) {
    auto indices = int32_indices({0, 0, 5}, {true, false, true});  // [0, null, 5(out-of-range)]
    auto dictionary = string_dict({"A", "B"}, {true, true});
    BlazeRulesSchema schema = one_categorical_schema();  // kept alive: DictEncoder holds a ref
    DictEncoder encoder(schema);

    auto out = encoder.encode_batch(dict_batch(indices, dictionary));
    ASSERT_EQ(out->num_columns(), 1);
    auto col = std::static_pointer_cast<arrow::Int32Array>(out->column(0));
    ASSERT_EQ(col->length(), 3);
    EXPECT_TRUE(col->IsValid(0));  // index 0 -> "A"
    EXPECT_TRUE(col->IsNull(1));   // null index stays null
    EXPECT_TRUE(col->IsNull(2));   // out-of-range index -> null, not code 0
}

TEST(DictEncoderTest, IndexToNullDictionaryValueBecomesNull) {
    auto indices = int32_indices({0, 1}, {true, true});
    auto dictionary = string_dict({"A", ""}, {true, false});  // dictionary[1] is null
    BlazeRulesSchema schema = one_categorical_schema();
    DictEncoder encoder(schema);

    auto out = encoder.encode_batch(dict_batch(indices, dictionary));
    auto col = std::static_pointer_cast<arrow::Int32Array>(out->column(0));
    ASSERT_EQ(col->length(), 2);
    EXPECT_TRUE(col->IsValid(0));  // "A"
    EXPECT_TRUE(col->IsNull(1));   // index maps to a null dictionary value -> null
}
