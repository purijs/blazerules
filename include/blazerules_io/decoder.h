#ifndef BLAZERULES_IO_DECODER_H
#define BLAZERULES_IO_DECODER_H

#include <cstddef>
#include <cstdint>
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

class ArrowIpcDecoder {
public:
    std::vector<std::shared_ptr<arrow::RecordBatch>> decode_batches(
        const std::vector<std::string_view>& frames) const;

    std::shared_ptr<arrow::RecordBatch> decode_batch(
        const std::vector<std::string_view>& frames) const;

    std::vector<std::shared_ptr<arrow::RecordBatch>> decode_file(
        const std::string& path) const;
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
