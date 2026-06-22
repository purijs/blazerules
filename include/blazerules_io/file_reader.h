#ifndef BLAZERULES_IO_FILE_READER_H
#define BLAZERULES_IO_FILE_READER_H

#include <memory>
#include <string>
#include <vector>

#include <arrow/record_batch.h>

namespace blazerules_io {

enum class FileFormat {
    AUTO,
    ARROWIPC,
    PARQUET,
    CSV,
    NDJSON
};

FileFormat parse_file_format(const std::string& format);

std::vector<std::shared_ptr<arrow::RecordBatch>> read_record_batches(
    const std::string& path,
    FileFormat format = FileFormat::AUTO,
    int64_t batch_size = 65536);

std::string read_ndjson_bytes(const std::string& path);

}  // namespace blazerules_io

#endif  // BLAZERULES_IO_FILE_READER_H
