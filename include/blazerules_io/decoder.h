#ifndef BLAZERULES_IO_DECODER_H
#define BLAZERULES_IO_DECODER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <arrow/record_batch.h>

#ifdef BLAZERULES_IO_AVRO
#include <avro/ValidSchema.hh>
#endif

namespace blazerules_io {

std::shared_ptr<arrow::RecordBatch> record_batch_from_ndjson(std::string_view ndjson);

enum class ArrowIpcValidationLevel {
    FULL,
    STRUCTURAL,
    TRUSTED
};

struct ArrowIpcReadOptions {
    std::vector<int> included_fields;
    ArrowIpcValidationLevel validation = ArrowIpcValidationLevel::FULL;
    bool use_threads = true;
};

struct ArrowIpcFrame {
    const uint8_t* data = nullptr;
    int64_t size = 0;
    std::shared_ptr<void> owner;

    ArrowIpcFrame() = default;
    ArrowIpcFrame(const uint8_t* data, int64_t size,
                  std::shared_ptr<void> owner = {})
        : data(data), size(size), owner(std::move(owner)) {}
};

using RecordBatchVisitor =
    std::function<bool(const std::shared_ptr<arrow::RecordBatch>&)>;

class ArrowIpcDecoder {
public:
    int64_t decode_each(const std::vector<std::string_view>& frames,
                        const RecordBatchVisitor& visitor,
                        const ArrowIpcReadOptions& options = {}) const;

    int64_t decode_each(const std::vector<ArrowIpcFrame>& frames,
                        const RecordBatchVisitor& visitor,
                        const ArrowIpcReadOptions& options = {}) const;

    int64_t decode_file_each(const std::string& path,
                             const RecordBatchVisitor& visitor,
                             const ArrowIpcReadOptions& options = {}) const;

    std::vector<std::shared_ptr<arrow::RecordBatch>> decode_batches(
        const std::vector<std::string_view>& frames,
        const ArrowIpcReadOptions& options = {}) const;

    std::shared_ptr<arrow::RecordBatch> decode_batch(
        const std::vector<std::string_view>& frames,
        const ArrowIpcReadOptions& options = {}) const;

    std::vector<std::shared_ptr<arrow::RecordBatch>> decode_file(
        const std::string& path,
        const ArrowIpcReadOptions& options = {}) const;
};

#ifdef BLAZERULES_IO_AVRO
class AvroDecoder {
public:
    explicit AvroDecoder(std::string schema_json);
    std::string decode_ndjson(const std::vector<std::string_view>& frames) const;
    std::shared_ptr<arrow::RecordBatch> decode_batch(
        const std::vector<std::string_view>& frames) const;

private:
    std::string schema_json_;
    std::shared_ptr<avro::ValidSchema> schema_;
};

// True if `bytes` starts with the Avro Object Container File magic
// ("Obj\x01") -- unambiguous, safe to auto-detect (unlike Protobuf, which has
// no equivalent magic bytes).
bool looks_like_avro_ocf(std::string_view bytes);

// Reads a real multi-record Avro OCF file (the container format produced by
// Spark/Hadoop/Kafka Connect etc., with its own embedded schema, sync
// markers, and codec), calling `visitor` once per batch of up to
// `batch_size` records. Uses the file's own embedded schema -- no
// AvroDecoder/--schema needed, unlike the bare single-frame decode_batch
// path above. Returns the total number of records visited.
int64_t decode_avro_ocf_file_each(const std::string& path,
                                  const RecordBatchVisitor& visitor,
                                  int64_t batch_size = 10000);
#endif

#ifdef BLAZERULES_IO_PROTOBUF
struct ProtobufDecoderImpl;

class ProtobufDecoder {
public:
    ProtobufDecoder(std::string descriptor_set_bytes, std::string message_type);
    std::string decode_ndjson(const std::vector<std::string_view>& frames) const;
    std::shared_ptr<arrow::RecordBatch> decode_batch(
        const std::vector<std::string_view>& frames) const;

    // Reads a file containing N varint-length-delimited protobuf messages --
    // the same convention protobuf's own C++ library ships for this exact
    // purpose (google/protobuf/util/delimited_message_util.h's
    // SerializeDelimitedToCodedStream/ParseDelimitedFromCodedStream) -- and
    // calls `visitor` once per batch of up to `batch_size` records. Protobuf
    // has no magic bytes, so this is never auto-detected; it must be reached
    // via an explicit opt-in (the CLI's `--input protobuf-delimited`), never
    // by silently reinterpreting `--input protobuf`. Returns the total
    // number of records visited.
    int64_t decode_delimited_file_each(const std::string& path,
                                       const RecordBatchVisitor& visitor,
                                       int64_t batch_size = 10000) const;

    // Same as decode_delimited_file_each, but the CPU-heavy protobuf parse
    // step for each batch_size-sized chunk runs concurrently across
    // worker_count threads (tbb::parallel_for) -- safe because decode_batch
    // reads only the immutable descriptor/prototype and mutates no shared
    // state. The (cheap) frame-boundary scan and the (already sequential)
    // visitor/evaluation calls are unchanged -- only decoding is parallel.
    // worker_count <= 1 delegates straight to decode_delimited_file_each.
    int64_t decode_delimited_file_parallel(const std::string& path,
                                           const RecordBatchVisitor& visitor,
                                           int64_t batch_size, int worker_count) const;

private:
    std::string descriptor_set_bytes_;
    std::string message_type_;
    std::shared_ptr<ProtobufDecoderImpl> impl_;
};
#endif

}  // namespace blazerules_io

#endif  // BLAZERULES_IO_DECODER_H
