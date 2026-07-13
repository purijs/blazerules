#include "blazerules_io/decoder.h"

#include <filesystem>
#include <stdexcept>

#include <arrow/api.h>
#include <arrow/buffer.h>
#include <arrow/io/file.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <arrow/ipc/options.h>
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

class OwnerBackedBuffer final : public arrow::Buffer {
public:
    OwnerBackedBuffer(const uint8_t* data, int64_t size, std::shared_ptr<void> owner)
        : arrow::Buffer(data, size), owner_(std::move(owner)) {}

private:
    std::shared_ptr<void> owner_;
};

std::shared_ptr<arrow::Buffer> buffer_from_frame(const ArrowIpcFrame& frame) {
    if (!frame.data || frame.size <= 0) return std::make_shared<arrow::Buffer>(nullptr, 0);
    return std::make_shared<OwnerBackedBuffer>(frame.data, frame.size, frame.owner);
}

arrow::ipc::IpcReadOptions arrow_options(const ArrowIpcReadOptions& options) {
    auto out = arrow::ipc::IpcReadOptions::Defaults();
    out.included_fields = options.included_fields;
    out.use_threads = options.use_threads;
    return out;
}

// Arrow IPC frames arrive from untrusted sources (Kafka, files). Fully validate
// each batch (offset buffers, bounds, UTF-8) before the engine reads its raw
// buffers, so a corrupt/malicious frame fails cleanly here rather than causing an
// out-of-bounds read downstream. Once per batch, not per row.
void validate_batch(const std::shared_ptr<arrow::RecordBatch>& batch,
                    ArrowIpcValidationLevel level) {
    if (level == ArrowIpcValidationLevel::TRUSTED) return;
    arrow::Status status = level == ArrowIpcValidationLevel::FULL
        ? batch->ValidateFull()
        : batch->Validate();
    if (!status.ok()) throw std::runtime_error(status_message(status, "arrow ipc batch validation"));
}

int64_t drain_stream(const std::shared_ptr<arrow::ipc::RecordBatchStreamReader>& reader,
                     const RecordBatchVisitor& visitor,
                     ArrowIpcValidationLevel validation) {
    int64_t rows = 0;
    while (true) {
        auto batch = value_or_throw(reader->Next(), "arrow ipc stream read");
        if (!batch) break;
        if (batch->num_rows() > 0) {
            validate_batch(batch, validation);
            rows += batch->num_rows();
            if (!visitor(batch)) break;
        }
    }
    return rows;
}

int64_t read_ipc_from_buffer(const std::shared_ptr<arrow::Buffer>& buffer,
                             const RecordBatchVisitor& visitor,
                             const ArrowIpcReadOptions& options) {
    const auto ipc_options = arrow_options(options);
    auto input = std::make_shared<arrow::io::BufferReader>(buffer);

    auto stream_reader = arrow::ipc::RecordBatchStreamReader::Open(input, ipc_options);
    if (stream_reader.ok()) {
        return drain_stream(stream_reader.ValueOrDie(), visitor, options.validation);
    }

    input = std::make_shared<arrow::io::BufferReader>(buffer);
    auto file_reader = value_or_throw(
        arrow::ipc::RecordBatchFileReader::Open(input, ipc_options), "arrow ipc file open");
    const int batches = file_reader->num_record_batches();
    int64_t rows = 0;
    for (int i = 0; i < batches; ++i) {
        auto batch = value_or_throw(file_reader->ReadRecordBatch(i), "arrow ipc file read");
        if (batch && batch->num_rows() > 0) {
            validate_batch(batch, options.validation);
            rows += batch->num_rows();
            if (!visitor(batch)) break;
        }
    }
    return rows;
}

}  // namespace

int64_t ArrowIpcDecoder::decode_each(
    const std::vector<std::string_view>& frames,
    const RecordBatchVisitor& visitor,
    const ArrowIpcReadOptions& options) const {
    std::vector<ArrowIpcFrame> borrowed;
    borrowed.reserve(frames.size());
    for (std::string_view frame : frames) {
        borrowed.emplace_back(reinterpret_cast<const uint8_t*>(frame.data()),
                              static_cast<int64_t>(frame.size()));
    }
    return decode_each(borrowed, visitor, options);
}

int64_t ArrowIpcDecoder::decode_each(
    const std::vector<ArrowIpcFrame>& frames,
    const RecordBatchVisitor& visitor,
    const ArrowIpcReadOptions& options) const {
    if (!visitor) throw std::invalid_argument("arrow ipc decode_each requires a visitor");
    int64_t rows = 0;
    bool stopped = false;
    for (const ArrowIpcFrame& frame : frames) {
        if (!frame.data || frame.size <= 0) continue;
        rows += read_ipc_from_buffer(
            buffer_from_frame(frame),
            [&](const std::shared_ptr<arrow::RecordBatch>& batch) {
                const bool keep_reading = visitor(batch);
                stopped = !keep_reading;
                return keep_reading;
            },
            options);
        if (stopped) break;
    }
    return rows;
}

std::vector<std::shared_ptr<arrow::RecordBatch>> ArrowIpcDecoder::decode_batches(
    const std::vector<std::string_view>& frames,
    const ArrowIpcReadOptions& options) const {
    std::vector<std::shared_ptr<arrow::RecordBatch>> out;
    decode_each(frames, [&](const std::shared_ptr<arrow::RecordBatch>& batch) {
        out.push_back(batch);
        return true;
    }, options);
    return out;
}

std::shared_ptr<arrow::RecordBatch> ArrowIpcDecoder::decode_batch(
    const std::vector<std::string_view>& frames,
    const ArrowIpcReadOptions& options) const {
    auto batches = decode_batches(frames, options);
    if (batches.empty()) return nullptr;
    if (batches.size() == 1) return batches.front();

    auto table = value_or_throw(arrow::Table::FromRecordBatches(batches),
                                "arrow ipc batches to table");
    return value_or_throw(table->CombineChunksToBatch(arrow::default_memory_pool()),
                          "arrow ipc combine batches");
}

int64_t ArrowIpcDecoder::decode_file_each(
    const std::string& path,
    const RecordBatchVisitor& visitor,
    const ArrowIpcReadOptions& options) const {
    if (!visitor) throw std::invalid_argument("arrow ipc decode_file_each requires a visitor");
    const auto ipc_options = arrow_options(options);
    auto stream_file = value_or_throw(arrow::io::ReadableFile::Open(path), "arrow ipc open file");
    auto stream_reader = arrow::ipc::RecordBatchStreamReader::Open(stream_file, ipc_options);
    if (stream_reader.ok()) {
        return drain_stream(stream_reader.ValueOrDie(), visitor, options.validation);
    }

    auto file = value_or_throw(arrow::io::ReadableFile::Open(path), "arrow ipc open file");
    auto file_reader = value_or_throw(
        arrow::ipc::RecordBatchFileReader::Open(
            std::static_pointer_cast<arrow::io::RandomAccessFile>(file), ipc_options),
        "arrow ipc file open");
    int64_t rows = 0;
    for (int i = 0; i < file_reader->num_record_batches(); ++i) {
        auto batch = value_or_throw(file_reader->ReadRecordBatch(i), "arrow ipc file read");
        if (batch && batch->num_rows() > 0) {
            validate_batch(batch, options.validation);
            rows += batch->num_rows();
            if (!visitor(batch)) break;
        }
    }
    return rows;
}

std::vector<std::shared_ptr<arrow::RecordBatch>> ArrowIpcDecoder::decode_file(
    const std::string& path,
    const ArrowIpcReadOptions& options) const {
    std::vector<std::shared_ptr<arrow::RecordBatch>> out;
    decode_file_each(path, [&](const std::shared_ptr<arrow::RecordBatch>& batch) {
        out.push_back(batch);
        return true;
    }, options);
    return out;
}

}  // namespace blazerules_io
