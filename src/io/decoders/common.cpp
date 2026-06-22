#include "blazerules_io/decoder.h"

#include <stdexcept>

#include <arrow/buffer.h>
#include <arrow/io/memory.h>
#include <arrow/json/reader.h>
#include <arrow/table.h>

namespace blazerules_io {

namespace {

std::string status_message(const arrow::Status& status, std::string_view context) {
    std::string out(context);
    out += ": ";
    out += status.ToString();
    return out;
}

template <typename T>
T value_or_throw(arrow::Result<T> result, std::string_view context) {
    if (!result.ok()) throw std::runtime_error(status_message(result.status(), context));
    return std::move(result).ValueOrDie();
}

}  // namespace

std::shared_ptr<arrow::RecordBatch> record_batch_from_ndjson(std::string_view ndjson) {
    if (ndjson.empty()) return nullptr;
    auto buffer = arrow::Buffer::FromString(std::string(ndjson));
    auto input = std::make_shared<arrow::io::BufferReader>(buffer);
    auto read_options = arrow::json::ReadOptions::Defaults();
    read_options.use_threads = true;
    auto parse_options = arrow::json::ParseOptions::Defaults();
    auto reader = value_or_throw(
        arrow::json::StreamingReader::Make(input, read_options, parse_options),
        "json record batch reader");
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    while (true) {
        auto batch = value_or_throw(reader->Next(), "json record batch read");
        if (!batch) break;
        if (batch->num_rows() > 0) batches.push_back(std::move(batch));
    }
    if (batches.empty()) return nullptr;
    if (batches.size() == 1) return batches.front();
    auto table = value_or_throw(arrow::Table::FromRecordBatches(batches),
                                "json record batches to table");
    return value_or_throw(table->CombineChunksToBatch(arrow::default_memory_pool()),
                          "json combine batches");
}

}  // namespace blazerules_io
