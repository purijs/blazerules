#include <gtest/gtest.h>

#include "blazerules_io/decoder.h"
#include "blazerules_io/file_reader.h"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

std::shared_ptr<arrow::RecordBatch> make_batch(int64_t start, int64_t rows) {
    arrow::Int64Builder ids;
    arrow::StringBuilder labels;
    for (int64_t i = 0; i < rows; ++i) {
        EXPECT_TRUE(ids.Append(start + i).ok());
        EXPECT_TRUE(labels.Append("row-" + std::to_string(start + i)).ok());
    }
    auto id_array = ids.Finish().ValueOrDie();
    auto label_array = labels.Finish().ValueOrDie();
    return arrow::RecordBatch::Make(
        arrow::schema({arrow::field("id", arrow::int64()),
                       arrow::field("label", arrow::utf8())}),
        rows, {id_array, label_array});
}

std::shared_ptr<arrow::Buffer> make_stream(
    const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) {
    auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
    auto writer = arrow::ipc::MakeStreamWriter(sink, batches.front()->schema()).ValueOrDie();
    for (const auto& batch : batches) EXPECT_TRUE(writer->WriteRecordBatch(*batch).ok());
    EXPECT_TRUE(writer->Close().ok());
    return sink->Finish().ValueOrDie();
}

std::filesystem::path temporary_path(std::string_view suffix) {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("blazerules-io-" + std::to_string(tick) + std::string(suffix));
}

}  // namespace

TEST(ArrowIpcStreamingTest, VisitsEachBatchAndRetainsFrameOwner) {
    auto encoded = make_stream({make_batch(0, 3), make_batch(3, 2)});
    auto owner = std::make_shared<std::vector<uint8_t>>(
        encoded->data(), encoded->data() + encoded->size());
    std::weak_ptr<std::vector<uint8_t>> weak_owner = owner;
    std::vector<blazerules_io::ArrowIpcFrame> frames;
    frames.emplace_back(owner->data(), static_cast<int64_t>(owner->size()), owner);
    owner.reset();

    blazerules_io::ArrowIpcDecoder decoder;
    int batches = 0;
    int64_t next_id = 0;
    const int64_t rows = decoder.decode_each(frames, [&](const auto& batch) {
        ++batches;
        const auto ids = std::static_pointer_cast<arrow::Int64Array>(batch->column(0));
        for (int64_t row = 0; row < batch->num_rows(); ++row) {
            EXPECT_EQ(ids->Value(row), next_id++);
        }
        return true;
    });

    EXPECT_EQ(rows, 5);
    EXPECT_EQ(batches, 2);
    EXPECT_FALSE(weak_owner.expired());
    frames.clear();
    EXPECT_TRUE(weak_owner.expired());
}

TEST(ArrowIpcStreamingTest, VisitorStopAppliesAcrossFrames) {
    auto first = make_stream({make_batch(0, 2)});
    auto second = make_stream({make_batch(2, 2)});
    std::vector<std::string_view> frames = {
        std::string_view(reinterpret_cast<const char*>(first->data()), first->size()),
        std::string_view(reinterpret_cast<const char*>(second->data()), second->size())};

    blazerules_io::ArrowIpcDecoder decoder;
    int visits = 0;
    const int64_t rows = decoder.decode_each(frames, [&](const auto&) {
        ++visits;
        return false;
    });
    EXPECT_EQ(rows, 2);
    EXPECT_EQ(visits, 1);
}

TEST(FileStreamingTest, ArrowFileIsVisitedWithoutCombiningBatches) {
    auto encoded = make_stream({make_batch(0, 4), make_batch(4, 3)});
    const auto path = temporary_path(".arrow");
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(encoded->data()), encoded->size());
    }

    int batches = 0;
    const int64_t rows = blazerules_io::for_each_record_batch(
        path.string(), blazerules_io::FileFormat::ARROWIPC,
        [&](const auto& batch) {
            ++batches;
            EXPECT_LE(batch->num_rows(), 4);
            return true;
        });
    std::filesystem::remove(path);

    EXPECT_EQ(rows, 7);
    EXPECT_EQ(batches, 2);
}

TEST(FileStreamingTest, NdjsonChunksPreserveLinesAndCanStopEarly) {
    std::string input;
    for (int i = 0; i < 5000; ++i) {
        input += "{\"id\":" + std::to_string(i) +
                 ",\"payload\":\"abcdefghijklmnopqrstuvwxyz0123456789\"}\n";
    }
    input += "{\"id\":5000}";
    const auto path = temporary_path(".ndjson");
    {
        std::ofstream output(path, std::ios::binary);
        output.write(input.data(), static_cast<std::streamsize>(input.size()));
    }

    blazerules_io::FileReadOptions options;
    options.ndjson_chunk_bytes = 64 * 1024;
    std::string reconstructed;
    int chunks = 0;
    const int64_t rows = blazerules_io::for_each_ndjson_chunk(
        path.string(),
        [&](std::string_view chunk) {
            ++chunks;
            reconstructed.append(chunk);
            return true;
        },
        options);
    EXPECT_EQ(rows, 5001);
    EXPECT_GT(chunks, 1);
    EXPECT_EQ(reconstructed, input);

    int stopped_chunks = 0;
    const int64_t partial_rows = blazerules_io::for_each_ndjson_chunk(
        path.string(),
        [&](std::string_view) {
            ++stopped_chunks;
            return false;
        },
        options);
    std::filesystem::remove(path);

    EXPECT_EQ(stopped_chunks, 1);
    EXPECT_GT(partial_rows, 0);
    EXPECT_LT(partial_rows, rows);
}
