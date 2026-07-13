#ifndef BLAZERULES_IO_FILE_READER_H
#define BLAZERULES_IO_FILE_READER_H

#include <memory>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <arrow/record_batch.h>

#include "blazerules_io/decoder.h"

namespace blazerules_io {

enum class FileFormat {
    AUTO,
    ARROWIPC,
    PARQUET,
    CSV,
    NDJSON
};

struct FileReadOptions {
    int64_t batch_size = 65536;
    int64_t ndjson_chunk_bytes = 8 * 1024 * 1024;
    std::vector<std::string> included_fields;
    std::vector<int> included_field_indices;
    bool use_threads = true;
    bool native_s3 = true;
    bool allow_s3_cli_fallback = true;
    ArrowIpcValidationLevel arrow_validation = ArrowIpcValidationLevel::FULL;
};

using FileBatchVisitor =
    std::function<bool(const std::shared_ptr<arrow::RecordBatch>&)>;
using NdjsonChunkVisitor = std::function<bool(std::string_view)>;

FileFormat parse_file_format(const std::string& format);

std::vector<std::shared_ptr<arrow::RecordBatch>> read_record_batches(
    const std::string& path,
    FileFormat format = FileFormat::AUTO,
    int64_t batch_size = 65536);

int64_t for_each_record_batch(
    const std::string& path,
    FileFormat format,
    const FileBatchVisitor& visitor,
    const FileReadOptions& options = {});

int64_t for_each_ndjson_chunk(
    const std::string& path,
    const NdjsonChunkVisitor& visitor,
    const FileReadOptions& options = {});

std::string read_ndjson_bytes(const std::string& path);

// Finalize native filesystem runtimes initialized by BlazeRules IO.
// Applications embedding blazerules_io should call this after all file reads finish.
void finalize_filesystems() noexcept;

}  // namespace blazerules_io

#endif  // BLAZERULES_IO_FILE_READER_H
