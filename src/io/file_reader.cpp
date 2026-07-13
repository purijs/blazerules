#include "blazerules_io/file_reader.h"

#include "blazerules/resource_resolver.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include <arrow/api.h>
#include <arrow/csv/reader.h>
#include <arrow/filesystem/filesystem.h>
#ifdef BLAZERULES_IO_S3
#include <arrow/filesystem/s3fs.h>
#endif
#include <arrow/io/interfaces.h>
#include <arrow/ipc/reader.h>
#include <arrow/json/reader.h>
#include <parquet/arrow/reader.h>

namespace blazerules_io {

namespace {

struct OpenedSource {
    std::shared_ptr<arrow::fs::FileSystem> filesystem;
    std::string path;
};

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string extension_of(const std::string& path) {
    std::string ext = lower_copy(std::filesystem::path(path).extension().string());
    if (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
    return ext;
}

std::string status_message(const arrow::Status& status, std::string_view context) {
    return std::string(context) + ": " + status.ToString();
}

template <typename T>
T value_or_throw(arrow::Result<T> result, std::string_view context) {
    if (!result.ok()) throw std::runtime_error(status_message(result.status(), context));
    return std::move(result).ValueOrDie();
}

bool is_s3_path(const std::string& path) {
    return path.rfind("s3://", 0) == 0;
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

#ifdef BLAZERULES_IO_S3
struct S3Lifecycle {
    std::mutex mutex;
    bool initialized = false;
    bool finalized = false;
};

S3Lifecycle& s3_lifecycle() {
    static auto* lifecycle = new S3Lifecycle();
    return *lifecycle;
}

void ensure_s3_initialized() {
    auto& lifecycle = s3_lifecycle();
    std::lock_guard lock(lifecycle.mutex);
    if (lifecycle.finalized) {
        throw std::runtime_error("native S3 runtime has already been finalized");
    }
    if (lifecycle.initialized) return;
    const auto status = arrow::fs::EnsureS3Initialized();
    if (!status.ok()) throw std::runtime_error(status_message(status, "initialize S3"));
    lifecycle.initialized = true;
}

OpenedSource open_native_s3(const std::string& uri) {
    ensure_s3_initialized();
    auto options = arrow::fs::S3Options::Defaults();
    const std::string region = blazerules::current_aws_region();
    if (!region.empty()) options.region = region;

    std::string endpoint = blazerules::current_aws_endpoint_url();
    if (!endpoint.empty()) {
        if (endpoint.rfind("http://", 0) == 0) {
            options.scheme = "http";
            endpoint.erase(0, 7);
        } else if (endpoint.rfind("https://", 0) == 0) {
            options.scheme = "https";
            endpoint.erase(0, 8);
        }
        while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
        options.endpoint_override = endpoint;
    }

    OpenedSource out;
    out.filesystem = value_or_throw(arrow::fs::S3FileSystem::Make(options),
                                    "create S3 filesystem");
    out.path = uri.substr(5);
    return out;
}
#endif

OpenedSource open_source(const std::string& path, const FileReadOptions& options) {
    if (is_s3_path(path) && options.native_s3) {
#ifdef BLAZERULES_IO_S3
        return open_native_s3(path);
#else
        if (!options.allow_s3_cli_fallback) {
            throw std::runtime_error("native S3 support is not compiled into this build");
        }
#endif
    }

    const std::string local = is_s3_path(path)
        ? blazerules::resolve_resource_to_local(path)
        : path;
    OpenedSource out;
    out.filesystem = value_or_throw(
        arrow::fs::FileSystemFromUriOrPath(local, &out.path), "filesystem from path");
    return out;
}

std::shared_ptr<arrow::io::InputStream> open_input_stream(const OpenedSource& source) {
    return value_or_throw(source.filesystem->OpenInputStream(source.path), "open input stream");
}

std::shared_ptr<arrow::io::RandomAccessFile> open_input_file(const OpenedSource& source) {
    return value_or_throw(source.filesystem->OpenInputFile(source.path), "open input file");
}

void validate_batch(const std::shared_ptr<arrow::RecordBatch>& batch,
                    ArrowIpcValidationLevel level) {
    if (level == ArrowIpcValidationLevel::TRUSTED) return;
    const auto status = level == ArrowIpcValidationLevel::FULL
        ? batch->ValidateFull()
        : batch->Validate();
    if (!status.ok()) throw std::runtime_error(status_message(status, "record batch validation"));
}

int64_t visit_reader(arrow::RecordBatchReader* reader,
                     const FileBatchVisitor& visitor,
                     ArrowIpcValidationLevel validation = ArrowIpcValidationLevel::TRUSTED) {
    int64_t rows = 0;
    while (true) {
        auto batch = value_or_throw(reader->Next(), "read record batch");
        if (!batch) break;
        if (batch->num_rows() == 0) continue;
        validate_batch(batch, validation);
        rows += batch->num_rows();
        if (!visitor(batch)) break;
    }
    return rows;
}

arrow::ipc::IpcReadOptions ipc_options(const FileReadOptions& options) {
    auto out = arrow::ipc::IpcReadOptions::Defaults();
    out.included_fields = options.included_field_indices;
    out.use_threads = options.use_threads;
    return out;
}

int64_t visit_arrow_ipc(const OpenedSource& source,
                        const FileBatchVisitor& visitor,
                        const FileReadOptions& options) {
    auto stream = open_input_stream(source);
    auto stream_reader = arrow::ipc::RecordBatchStreamReader::Open(stream, ipc_options(options));
    if (stream_reader.ok()) {
        return visit_reader(stream_reader.ValueOrDie().get(), visitor, options.arrow_validation);
    }

    auto input = open_input_file(source);
    auto file_reader = value_or_throw(
        arrow::ipc::RecordBatchFileReader::Open(input, ipc_options(options)),
        "open Arrow IPC file");
    int64_t rows = 0;
    for (int i = 0; i < file_reader->num_record_batches(); ++i) {
        auto batch = value_or_throw(file_reader->ReadRecordBatch(i), "read Arrow IPC batch");
        if (!batch || batch->num_rows() == 0) continue;
        validate_batch(batch, options.arrow_validation);
        rows += batch->num_rows();
        if (!visitor(batch)) break;
    }
    return rows;
}

std::vector<int> selected_parquet_columns(const std::shared_ptr<arrow::Schema>& schema,
                                          const std::vector<std::string>& fields) {
    if (fields.empty()) return {};
    std::vector<int> indices;
    std::unordered_set<int> seen;
    for (const std::string& field : fields) {
        const std::string top = field.substr(0, field.find('.'));
        const int index = schema->GetFieldIndex(top);
        if (index >= 0 && seen.insert(index).second) indices.push_back(index);
    }
    return indices;
}

int64_t visit_parquet(const OpenedSource& source,
                      const FileBatchVisitor& visitor,
                      const FileReadOptions& options) {
    auto input = open_input_file(source);
    auto reader = value_or_throw(parquet::arrow::OpenFile(input, arrow::default_memory_pool()),
                                 "open Parquet");
    reader->set_use_threads(options.use_threads);
    reader->set_batch_size(std::max<int64_t>(1, options.batch_size));

    std::unique_ptr<arrow::RecordBatchReader> batch_reader;
    if (options.included_fields.empty()) {
        batch_reader = value_or_throw(reader->GetRecordBatchReader(),
                                      "create Parquet batch reader");
    } else {
        std::shared_ptr<arrow::Schema> schema;
        const auto schema_status = reader->GetSchema(&schema);
        if (!schema_status.ok()) {
            throw std::runtime_error(status_message(schema_status, "read Parquet schema"));
        }
        auto columns = selected_parquet_columns(schema, options.included_fields);
        std::vector<int> row_groups(
            static_cast<size_t>(reader->parquet_reader()->metadata()->num_row_groups()));
        std::iota(row_groups.begin(), row_groups.end(), 0);
        batch_reader = value_or_throw(reader->GetRecordBatchReader(row_groups, columns),
                                      "create projected Parquet batch reader");
    }
    return visit_reader(batch_reader.get(), visitor);
}

int64_t visit_csv(const OpenedSource& source,
                  const FileBatchVisitor& visitor,
                  const FileReadOptions& options) {
    auto read_options = arrow::csv::ReadOptions::Defaults();
    read_options.block_size = std::max<int64_t>(1 << 20, options.batch_size * 256);
    read_options.use_threads = options.use_threads;
    auto convert_options = arrow::csv::ConvertOptions::Defaults();
    convert_options.include_columns = options.included_fields;
    auto reader = value_or_throw(
        arrow::csv::StreamingReader::Make(arrow::io::default_io_context(),
                                          open_input_stream(source), read_options,
                                          arrow::csv::ParseOptions::Defaults(), convert_options),
        "create CSV streaming reader");
    return visit_reader(reader.get(), visitor);
}

int64_t visit_json(const OpenedSource& source,
                   const FileBatchVisitor& visitor,
                   const FileReadOptions& options) {
    auto read_options = arrow::json::ReadOptions::Defaults();
    read_options.block_size = std::max<int64_t>(1 << 20, options.batch_size * 512);
    read_options.use_threads = options.use_threads;
    auto reader = value_or_throw(
        arrow::json::StreamingReader::Make(open_input_stream(source), read_options,
                                           arrow::json::ParseOptions::Defaults()),
        "create JSON streaming reader");
    return visit_reader(reader.get(), visitor);
}

template <typename Fn>
auto with_s3_fallback(const std::string& path,
                      const FileReadOptions& options,
                      const bool* delivered,
                      Fn&& fn) -> decltype(fn(std::declval<const OpenedSource&>())) {
    try {
        return fn(open_source(path, options));
    } catch (...) {
        if (!is_s3_path(path) || !options.native_s3 ||
            !options.allow_s3_cli_fallback || (delivered && *delivered)) {
            throw;
        }
        FileReadOptions fallback = options;
        fallback.native_s3 = false;
        return fn(open_source(path, fallback));
    }
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

int64_t for_each_record_batch(const std::string& path,
                              FileFormat format,
                              const FileBatchVisitor& visitor,
                              const FileReadOptions& options) {
    if (!visitor) throw std::invalid_argument("for_each_record_batch requires a visitor");
    if (format == FileFormat::AUTO) format = infer_format(path);
    bool delivered = false;
    const FileBatchVisitor guarded_visitor = [&](const auto& batch) {
        delivered = true;
        return visitor(batch);
    };
    return with_s3_fallback(path, options, &delivered, [&](const OpenedSource& source) {
        switch (format) {
            case FileFormat::ARROWIPC: return visit_arrow_ipc(source, guarded_visitor, options);
            case FileFormat::PARQUET: return visit_parquet(source, guarded_visitor, options);
            case FileFormat::CSV: return visit_csv(source, guarded_visitor, options);
            case FileFormat::NDJSON: return visit_json(source, guarded_visitor, options);
            case FileFormat::AUTO: break;
        }
        return int64_t{0};
    });
}

std::vector<std::shared_ptr<arrow::RecordBatch>> read_record_batches(
    const std::string& path, FileFormat format, int64_t batch_size) {
    FileReadOptions options;
    options.batch_size = batch_size > 0 ? batch_size : 65536;
    std::vector<std::shared_ptr<arrow::RecordBatch>> out;
    for_each_record_batch(path, format, [&](const auto& batch) {
        out.push_back(batch);
        return true;
    }, options);
    return out;
}

int64_t for_each_ndjson_chunk(const std::string& path,
    const NdjsonChunkVisitor& visitor,
    const FileReadOptions& options) {
    if (!visitor) throw std::invalid_argument("for_each_ndjson_chunk requires a visitor");
    bool delivered = false;
    return with_s3_fallback(path, options, &delivered, [&](const OpenedSource& source) {
        auto input = open_input_stream(source);
        const int64_t chunk_size = std::max<int64_t>(64 * 1024, options.ndjson_chunk_bytes);
        std::vector<uint8_t> buffer(static_cast<size_t>(chunk_size));
        std::string pending;
        pending.reserve(static_cast<size_t>(chunk_size + 4096));
        int64_t rows = 0;
        bool keep_reading = true;
        while (keep_reading) {
            const int64_t n = value_or_throw(input->Read(chunk_size, buffer.data()), "read NDJSON");
            if (n <= 0) break;
            pending.append(reinterpret_cast<const char*>(buffer.data()), static_cast<size_t>(n));
            const size_t last_newline = pending.rfind('\n');
            if (last_newline == std::string::npos) continue;
            const std::string_view complete(pending.data(), last_newline + 1);
            rows += static_cast<int64_t>(std::count(complete.begin(), complete.end(), '\n'));
            delivered = true;
            keep_reading = visitor(complete);
            pending.erase(0, last_newline + 1);
        }
        if (keep_reading && !pending.empty()) {
            ++rows;
            delivered = true;
            (void)visitor(pending);
        }
        return rows;
    });
}

std::string read_ndjson_bytes(const std::string& path) {
    FileReadOptions options;
    std::string out;
    for_each_ndjson_chunk(path, [&](std::string_view chunk) {
        out.append(chunk);
        return true;
    }, options);
    return out;
}

void finalize_filesystems() noexcept {
#ifdef BLAZERULES_IO_S3
    auto& lifecycle = s3_lifecycle();
    std::lock_guard lock(lifecycle.mutex);
    if (!lifecycle.initialized || lifecycle.finalized) return;
    (void)arrow::fs::EnsureS3Finalized();
    lifecycle.finalized = true;
#endif
}

}  // namespace blazerules_io
