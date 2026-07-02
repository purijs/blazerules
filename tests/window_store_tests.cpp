#include <gtest/gtest.h>

#include "blazerules/window_store.h"

#include <arrow/api.h>

#include <memory>
#include <stdexcept>
#include <vector>

namespace {

std::shared_ptr<arrow::RecordBatch> make_batch(const std::vector<int32_t>& entity_ids,
                                               const std::vector<float>& amounts) {
    arrow::Int32Builder id_builder;
    arrow::FloatBuilder amount_builder;
    auto check = [](const arrow::Status& status) {
        if (!status.ok()) throw std::runtime_error(status.ToString());
    };
    for (int32_t id : entity_ids) check(id_builder.Append(id));
    for (float amount : amounts) check(amount_builder.Append(amount));

    std::shared_ptr<arrow::Array> id_array;
    std::shared_ptr<arrow::Array> amount_array;
    check(id_builder.Finish(&id_array));
    check(amount_builder.Finish(&amount_array));

    auto schema = arrow::schema({arrow::field("card_token", arrow::int32()),
                                 arrow::field("amount", arrow::float32())});
    return arrow::RecordBatch::Make(schema, static_cast<int64_t>(entity_ids.size()), {id_array, amount_array});
}

std::vector<WindowChannelSpec> count_and_sum_specs() {
    WindowChannelSpec count;
    count.entity_col_index = 0;
    count.entity_field = "card_token";
    count.function = WindowFn::COUNT;
    count.duration_seconds = 10;
    count.injected_col_index = 2;
    count.injected_name = "__window_card_token_count_10";
    count.column_type = ColumnType::INT32;

    WindowChannelSpec sum = count;
    sum.function = WindowFn::SUM;
    sum.sum_col_index = 1;
    sum.sum_field = "amount";
    sum.injected_col_index = 3;
    sum.injected_name = "__window_card_token_sum_10";
    sum.column_type = ColumnType::FLOAT32;
    return {count, sum};
}

}  // namespace

TEST(WindowStoreTest, ReadsPriorBatchThenWritesCurrentBatch) {
    WindowStore store;
    store.configure(count_and_sum_specs());

    auto first = make_batch({1, 2, 1}, {10.0f, 7.0f, 5.0f});
    std::vector<std::vector<double>> out;
    store.query_all_channels(*first, 100, out);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_DOUBLE_EQ(out[0][0], 0.0);
    EXPECT_DOUBLE_EQ(out[1][0], 0.0);

    store.update_all_channels(*first, 100);

    auto second = make_batch({1, 2, 3}, {2.0f, 3.0f, 4.0f});
    store.query_all_channels(*second, 105, out);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_DOUBLE_EQ(out[0][0], 2.0);
    EXPECT_DOUBLE_EQ(out[1][0], 15.0);
    EXPECT_DOUBLE_EQ(out[0][1], 1.0);
    EXPECT_DOUBLE_EQ(out[1][1], 7.0);
    EXPECT_DOUBLE_EQ(out[0][2], 0.0);
    EXPECT_DOUBLE_EQ(out[1][2], 0.0);

    store.update_all_channels(*second, 105);

    auto expired = make_batch({1, 2}, {1.0f, 1.0f});
    store.query_all_channels(*expired, 111, out);
    EXPECT_DOUBLE_EQ(out[0][0], 1.0);
    EXPECT_DOUBLE_EQ(out[1][0], 2.0);
    EXPECT_DOUBLE_EQ(out[0][1], 1.0);
    EXPECT_DOUBLE_EQ(out[1][1], 3.0);
}

TEST(WindowStoreTest, ResetClearsAllState) {
    WindowStore store;
    store.configure(count_and_sum_specs());
    auto batch = make_batch({1}, {42.0f});
    store.update_all_channels(*batch, 10);
    EXPECT_EQ(store.num_active_entities(0), 1);
    store.reset_all();
    EXPECT_EQ(store.num_active_entities(0), 0);

    std::vector<std::vector<double>> out;
    store.query_all_channels(*batch, 11, out);
    EXPECT_DOUBLE_EQ(out[0][0], 0.0);
}
