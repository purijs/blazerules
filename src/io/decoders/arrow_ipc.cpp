#include "blazerules_io/decoder.h"

#include <filesystem>
#include <stdexcept>

#include <arrow/api.h>
#include <arrow/buffer.h>
#include <arrow/io/file.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
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

std::shared_ptr<arrow::Buffer> owned_buffer_from_view(std::string_view view) {
    std::vector<uint8_t> bytes(view.begin(), view.end());
    return arrow::Buffer::FromVector(std::move(bytes));
}

// Arrow IPC frames arrive from untrusted sources (Kafka, files). Fully validate
// each batch (offset buffers, bounds, UTF-8) before the engine reads its raw
// buffers, so a corrupt/malicious frame fails cleanly here rather than causing an
// out-of-bounds read downstream. Once per batch, not per row.
void validate_batch(const std::shared_ptr<arrow::RecordBatch>& batch) {
    arrow::Status status = batch->ValidateFull();
    if (!status.ok()) throw std::runtime_error(status_message(status, "arrow ipc batch validation"));
}

void drain_stream(const std::shared_ptr<arrow::ipc::RecordBatchStreamReader>& reader,
                  std::vector<std::shared_ptr<arrow::RecordBatch>>& out) {
    while (true) {
        auto batch = value_or_throw(reader->Next(), "arrow ipc stream read");
        if (!batch) break;
        if (batch->num_rows() > 0) {
            validate_batch(batch);
            out.push_back(std::move(batch));
        }
    }
}

std::vector<std::shared_ptr<arrow::RecordBatch>> read_ipc_from_buffer(
    const std::shared_ptr<arrow::Buffer>& buffer) {
    std::vector<std::shared_ptr<arrow::RecordBatch>> out;
    auto input = std::make_shared<arrow::io::BufferReader>(buffer);

    auto stream_reader = arrow::ipc::RecordBatchStreamReader::Open(input);
    if (stream_reader.ok()) {
        drain_stream(stream_reader.ValueOrDie(), out);
        return out;
    }

    input = std::make_shared<arrow::io::BufferReader>(buffer);
    auto file_reader = value_or_throw(
        arrow::ipc::RecordBatchFileReader::Open(input), "arrow ipc file open");
    const int batches = file_reader->num_record_batches();
    out.reserve(static_cast<size_t>(batches));
    for (int i = 0; i < batches; ++i) {
        auto batch = value_or_throw(file_reader->ReadRecordBatch(i), "arrow ipc file read");
        if (batch && batch->num_rows() > 0) {
            validate_batch(batch);
            out.push_back(std::move(batch));
        }
    }
    return out;
}

}  // namespace

std::vector<std::shared_ptr<arrow::RecordBatch>> ArrowIpcDecoder::decode_batches(
    const std::vector<std::string_view>& frames) const {
    std::vector<std::shared_ptr<arrow::RecordBatch>> out;
    for (std::string_view frame : frames) {
        if (frame.empty()) continue;
        auto batches = read_ipc_from_buffer(owned_buffer_from_view(frame));
        out.insert(out.end(), batches.begin(), batches.end());
    }
    return out;
}

std::shared_ptr<arrow::RecordBatch> ArrowIpcDecoder::decode_batch(
    const std::vector<std::string_view>& frames) const {
    auto batches = decode_batches(frames);
    if (batches.empty()) return nullptr;
    if (batches.size() == 1) return batches.front();

    auto table = value_or_throw(arrow::Table::FromRecordBatches(batches),
                                "arrow ipc batches to table");
    return value_or_throw(table->CombineChunksToBatch(arrow::default_memory_pool()),
                          "arrow ipc combine batches");
}

std::vector<std::shared_ptr<arrow::RecordBatch>> ArrowIpcDecoder::decode_file(
    const std::string& path) const {
    std::vector<std::shared_ptr<arrow::RecordBatch>> out;
    auto stream_file = value_or_throw(arrow::io::ReadableFile::Open(path), "arrow ipc open file");
    auto stream_reader = arrow::ipc::RecordBatchStreamReader::Open(stream_file);
    if (stream_reader.ok()) {
        drain_stream(stream_reader.ValueOrDie(), out);
        return out;
    }

    auto file = value_or_throw(arrow::io::ReadableFile::Open(path), "arrow ipc open file");
    auto file_reader = value_or_throw(
        arrow::ipc::RecordBatchFileReader::Open(
            std::static_pointer_cast<arrow::io::RandomAccessFile>(file)),
        "arrow ipc file open");
    const int batches = file_reader->num_record_batches();
    out.reserve(static_cast<size_t>(batches));
    for (int i = 0; i < batches; ++i) {
        auto batch = value_or_throw(file_reader->ReadRecordBatch(i), "arrow ipc file read");
        if (batch && batch->num_rows() > 0) {
            validate_batch(batch);
            out.push_back(std::move(batch));
        }
    }
    return out;
}

}  // namespace blazerules_io
