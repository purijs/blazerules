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
#endif

#ifdef BLAZERULES_IO_PROTOBUF
struct ProtobufDecoderImpl;

class ProtobufDecoder {
public:
    ProtobufDecoder(std::string descriptor_set_bytes, std::string message_type);
    std::string decode_ndjson(const std::vector<std::string_view>& frames) const;
    std::shared_ptr<arrow::RecordBatch> decode_batch(
        const std::vector<std::string_view>& frames) const;

private:
    std::string descriptor_set_bytes_;
    std::string message_type_;
    std::shared_ptr<ProtobufDecoderImpl> impl_;
};
#endif

}  // namespace blazerules_io

#endif  // BLAZERULES_IO_DECODER_H
