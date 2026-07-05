#include "blazerules_io/file_reader.h"

#include "blazerules/resource_resolver.h"
#include "blazerules_io/decoder.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <stdexcept>

#include <arrow/api.h>
#include <arrow/csv/reader.h>
#include <arrow/filesystem/filesystem.h>
#include <arrow/io/file.h>
#include <arrow/io/interfaces.h>
#include <arrow/json/reader.h>
#include <arrow/table.h>
#include <parquet/arrow/reader.h>

namespace blazerules_io {

namespace {

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string extension_of(const std::string& path) {
    std::string ext = lower_copy(std::filesystem::path(path).extension().string());
    if (!ext.empty() && ext[0] == '.') ext.erase(ext.begin());
    return ext;
}

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

bool is_s3_path(const std::string& path) {
    return path.rfind("s3://", 0) == 0;
}

std::string local_io_path(const std::string& path) {
    if (!is_s3_path(path)) return path;
    return blazerules::resolve_resource_to_local(path);
}


FileFormat infer_format(const std::string& path) {
    const std::string ext = extension_of(path);
    if (ext == "arrow" || ext == "arrows" || ext == "ipc" || ext == "feather") {
        return FileFormat::ARROWIPC;
    }
    if (ext == "parquet" || ext == "pq") return FileFormat::PARQUET;
    if (ext == "csv") return FileFormat::CSV;
    if (ext == "json" || ext == "jsonl" || ext == "ndjson") return FileFormat::NDJSON;
    throw std::invalid_argument("cannot infer file format from extension: " + path);
}

std::shared_ptr<arrow::io::InputStream> open_input_stream(const std::string& path) {
    std::string inner_path;
    auto fs = value_or_throw(
        arrow::fs::FileSystemFromUriOrPath(path, &inner_path), "filesystem from uri");
    return value_or_throw(fs->OpenInputStream(inner_path), "open input stream");
}

std::shared_ptr<arrow::io::RandomAccessFile> open_input_file(const std::string& path) {
    std::string inner_path;
    auto fs = value_or_throw(
        arrow::fs::FileSystemFromUriOrPath(path, &inner_path), "filesystem from uri");
    return value_or_throw(fs->OpenInputFile(inner_path), "open input file");
}

void drain_reader(arrow::RecordBatchReader* reader,
                  std::vector<std::shared_ptr<arrow::RecordBatch>>& out) {
    while (true) {
        auto batch = value_or_throw(reader->Next(), "read record batch");
        if (!batch) break;
        if (batch->num_rows() > 0) out.push_back(std::move(batch));
    }
}

std::vector<std::shared_ptr<arrow::RecordBatch>> read_parquet_batches(
    const std::string& path, int64_t batch_size) {
    std::string local_path = local_io_path(path);
    auto input = open_input_file(local_path);
    auto reader = value_or_throw(parquet::arrow::OpenFile(input, arrow::default_memory_pool()),
                                 "open parquet");
    reader->set_use_threads(true);
    reader->set_batch_size(batch_size);
    auto batch_reader = value_or_throw(reader->GetRecordBatchReader(),
                                       "parquet record batch reader");
    std::vector<std::shared_ptr<arrow::RecordBatch>> out;
    drain_reader(batch_reader.get(), out);
    return out;
}

std::vector<std::shared_ptr<arrow::RecordBatch>> read_csv_batches(
    const std::string& path, int64_t batch_size) {
    std::string local_path = local_io_path(path);
    auto input = open_input_stream(local_path);
    auto read_options = arrow::csv::ReadOptions::Defaults();
    read_options.block_size = std::max<int64_t>(1 << 20, batch_size * 256);
    read_options.use_threads = true;
    auto parse_options = arrow::csv::ParseOptions::Defaults();
    auto convert_options = arrow::csv::ConvertOptions::Defaults();
    auto reader = value_or_throw(
        arrow::csv::StreamingReader::Make(arrow::io::default_io_context(), input,
                                          read_options, parse_options, convert_options),
        "csv streaming reader");
    std::vector<std::shared_ptr<arrow::RecordBatch>> out;
    drain_reader(reader.get(), out);
    return out;
}

std::vector<std::shared_ptr<arrow::RecordBatch>> read_json_batches(
    const std::string& path, int64_t batch_size) {
    std::string local_path = local_io_path(path);
    auto input = open_input_stream(local_path);
    auto read_options = arrow::json::ReadOptions::Defaults();
    read_options.block_size = std::max<int64_t>(1 << 20, batch_size * 512);
    read_options.use_threads = true;
    auto parse_options = arrow::json::ParseOptions::Defaults();
    auto reader = value_or_throw(
        arrow::json::StreamingReader::Make(input, read_options, parse_options),
        "json streaming reader");
    std::vector<std::shared_ptr<arrow::RecordBatch>> out;
    drain_reader(reader.get(), out);
    return out;
}

}  // namespace

FileFormat parse_file_format(const std::string& format) {
    const std::string f = lower_copy(format);
    if (f.empty() || f == "auto") return FileFormat::AUTO;
    if (f == "arrow" || f == "arrow_ipc" || f == "arrow-ipc" || f == "ipc" || f == "feather") {
        return FileFormat::ARROWIPC;
    }
    if (f == "parquet" || f == "pq") return FileFormat::PARQUET;
    if (f == "csv") return FileFormat::CSV;
    if (f == "json" || f == "jsonl" || f == "ndjson") return FileFormat::NDJSON;
    throw std::invalid_argument("unknown file format: " + format);
}

std::vector<std::shared_ptr<arrow::RecordBatch>> read_record_batches(
    const std::string& path, FileFormat format, int64_t batch_size) {
    if (batch_size <= 0) batch_size = 65536;
    if (format == FileFormat::AUTO) format = infer_format(path);
    switch (format) {
        case FileFormat::ARROWIPC:
            return ArrowIpcDecoder{}.decode_file(local_io_path(path));
        case FileFormat::PARQUET:
            return read_parquet_batches(path, batch_size);
        case FileFormat::CSV:
            return read_csv_batches(path, batch_size);
        case FileFormat::NDJSON:
            return read_json_batches(path, batch_size);
        case FileFormat::AUTO:
            break;
    }
    return {};
}

std::string read_ndjson_bytes(const std::string& path) {
    std::string local_path = local_io_path(path);
    auto input = open_input_stream(local_path);
    std::string out;
    std::array<uint8_t, 1 << 20> buffer{};
    while (true) {
        auto n = value_or_throw(input->Read(static_cast<int64_t>(buffer.size()), buffer.data()),
                                "read ndjson");
        if (n <= 0) break;
        out.append(reinterpret_cast<const char*>(buffer.data()), static_cast<size_t>(n));
    }
    return out;
}

}  // namespace blazerules_io
