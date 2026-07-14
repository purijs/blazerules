#include "blazerules_io/decoder.h"

#ifdef BLAZERULES_IO_PROTOBUF

#include <arrow/api.h>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>

namespace blazerules_io {

namespace {

std::string status_message(const arrow::Status& status, std::string_view context) {
    std::string out(context);
    out += ": ";
    out += status.ToString();
    return out;
}

void throw_if_not_ok(const arrow::Status& status, std::string_view context) {
    if (!status.ok()) throw std::runtime_error(status_message(status, context));
}

template <typename T>
T value_or_throw(arrow::Result<T> result, std::string_view context) {
    if (!result.ok()) throw std::runtime_error(status_message(result.status(), context));
    return std::move(result).ValueOrDie();
}

// protobuf's ParseFromArray takes an int length. Reject frames that would overflow
// that cast (>2GB) before parsing, so a hostile/corrupt frame cannot wrap the length
// negative and drive an out-of-bounds read inside protobuf.
int checked_frame_size(std::string_view frame) {
    if (frame.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("protobuf frame too large: " + std::to_string(frame.size()) +
                                 " bytes exceeds 2GB limit");
    }
    return static_cast<int>(frame.size());
}

bool read_varint(std::string_view data, size_t& pos, uint64_t& value) {
    value = 0;
    int shift = 0;
    while (pos < data.size() && shift <= 63) {
        uint8_t byte = static_cast<uint8_t>(data[pos++]);
        value |= static_cast<uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) return true;
        shift += 7;
    }
    return false;
}

// Plain istreambuf_iterator-range string construction reads one character at
// a time through iterator overhead on some standard library implementations
// -- measured ~9x slower than this for a 600MB file (2.35s vs 0.26s). Single
// seek-to-end + one bulk read() avoids that entirely.
std::string read_whole_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("failed to open file: " + path);
    std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::string data(static_cast<size_t>(size), '\0');
    if (size > 0 && !in.read(data.data(), size)) {
        throw std::runtime_error("failed to read file: " + path);
    }
    return data;
}

// Cheap, sequential boundary-finding for a varint-length-delimited file --
// no protobuf parsing here, just walking length prefixes (reusing the same
// read_varint used for Confluent-prefix stripping) so the actual parse work
// in decode_batch can be deferred to per-chunk callers, sequential or
// parallel alike.
std::vector<std::string_view> extract_delimited_frames(std::string_view data) {
    std::vector<std::string_view> frames;
    size_t pos = 0;
    while (pos < data.size()) {
        const size_t frame_start = pos;
        uint64_t length = 0;
        if (!read_varint(data, pos, length)) {
            throw std::runtime_error("protobuf delimited file: truncated length prefix at offset " +
                                     std::to_string(frame_start));
        }
        if (length > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
            throw std::runtime_error("protobuf delimited file: message length " +
                                     std::to_string(length) + " exceeds 2GB limit at offset " +
                                     std::to_string(frame_start));
        }
        if (pos + static_cast<size_t>(length) > data.size()) {
            throw std::runtime_error(
                "protobuf delimited file: truncated message at offset " +
                std::to_string(frame_start) + " (needs " + std::to_string(length) +
                " bytes, only " + std::to_string(data.size() - pos) + " remain)");
        }
        frames.emplace_back(data.data() + pos, static_cast<size_t>(length));
        pos += static_cast<size_t>(length);
    }
    return frames;
}

std::string_view strip_confluent_protobuf_prefix(std::string_view frame) {
    if (frame.size() <= 5 || static_cast<unsigned char>(frame[0]) != 0x00) return frame;
    size_t pos = 5;
    uint64_t first = 0;
    if (!read_varint(frame, pos, first)) return frame.substr(5);
    if (first == 0) return frame.substr(pos);
    const uint64_t count = first;
    for (uint64_t i = 0; i < count && pos < frame.size(); ++i) {
        uint64_t ignored = 0;
        if (!read_varint(frame, pos, ignored)) return frame.substr(5);
    }
    return frame.substr(pos);
}

std::string to_std_string(absl::string_view value) {
    return std::string(value.data(), value.size());
}

std::shared_ptr<arrow::DataType> arrow_type_from_proto_field(
    const google::protobuf::FieldDescriptor* field, int depth = 0);

std::shared_ptr<arrow::DataType> arrow_value_type_from_proto_field(
    const google::protobuf::FieldDescriptor* field, int depth = 0) {
    if (depth > 32) {
        throw std::runtime_error(
            "protobuf message nesting exceeds 32 levels (recursive/self-referential type?)");
    }
    using FD = google::protobuf::FieldDescriptor;
    switch (field->type()) {
        case FD::TYPE_DOUBLE:
            return arrow::float64();
        case FD::TYPE_FLOAT:
            return arrow::float32();
        case FD::TYPE_INT64:
        case FD::TYPE_SINT64:
        case FD::TYPE_SFIXED64:
            return arrow::int64();
        case FD::TYPE_UINT64:
        case FD::TYPE_FIXED64:
            return arrow::uint64();
        case FD::TYPE_INT32:
        case FD::TYPE_SINT32:
        case FD::TYPE_SFIXED32:
            return arrow::int32();
        case FD::TYPE_UINT32:
        case FD::TYPE_FIXED32:
            return arrow::uint32();
        case FD::TYPE_BOOL:
            return arrow::boolean();
        case FD::TYPE_STRING:
        case FD::TYPE_ENUM:
            return arrow::utf8();
        case FD::TYPE_BYTES:
            return arrow::binary();
        case FD::TYPE_MESSAGE: {
            const auto* descriptor = field->message_type();
            std::vector<std::shared_ptr<arrow::Field>> fields;
            fields.reserve(static_cast<size_t>(descriptor->field_count()));
            for (int i = 0; i < descriptor->field_count(); ++i) {
                const auto* nested = descriptor->field(i);
                fields.push_back(arrow::field(to_std_string(nested->name()),
                                              arrow_type_from_proto_field(nested, depth + 1),
                                              nested->has_presence()));
            }
            return arrow::struct_(std::move(fields));
        }
        default:
            throw std::runtime_error("Unsupported protobuf field type for direct Arrow decode");
    }
}

std::shared_ptr<arrow::DataType> arrow_type_from_proto_field(
    const google::protobuf::FieldDescriptor* field, int depth) {
    auto value_type = arrow_value_type_from_proto_field(field, depth);
    if (field->is_repeated()) {
        return arrow::list(arrow::field("item", value_type, false));
    }
    return value_type;
}

std::shared_ptr<arrow::Schema> arrow_schema_from_proto_descriptor(
    const google::protobuf::Descriptor* descriptor) {
    std::vector<std::shared_ptr<arrow::Field>> fields;
    fields.reserve(static_cast<size_t>(descriptor->field_count()));
    for (int i = 0; i < descriptor->field_count(); ++i) {
        const auto* field = descriptor->field(i);
        fields.push_back(arrow::field(to_std_string(field->name()),
                                      arrow_type_from_proto_field(field),
                                      field->has_presence()));
    }
    return arrow::schema(std::move(fields));
}

std::string enum_name(const google::protobuf::FieldDescriptor* field, int value) {
    const auto* enum_value = field->enum_type()->FindValueByNumber(value);
    if (enum_value) return to_std_string(enum_value->name());
    return std::to_string(value);
}

void append_proto_singular_value(arrow::ArrayBuilder* builder,
                                 const google::protobuf::Message& message,
                                 const google::protobuf::FieldDescriptor* field);

void append_proto_repeated_value(arrow::ArrayBuilder* builder,
                                 const google::protobuf::Message& message,
                                 const google::protobuf::FieldDescriptor* field,
                                 int index) {
    using FD = google::protobuf::FieldDescriptor;
    const auto* reflection = message.GetReflection();
    switch (field->type()) {
        case FD::TYPE_DOUBLE:
            throw_if_not_ok(static_cast<arrow::DoubleBuilder*>(builder)->Append(
                                reflection->GetRepeatedDouble(message, field, index)),
                            "append repeated protobuf double");
            break;
        case FD::TYPE_FLOAT:
            throw_if_not_ok(static_cast<arrow::FloatBuilder*>(builder)->Append(
                                reflection->GetRepeatedFloat(message, field, index)),
                            "append repeated protobuf float");
            break;
        case FD::TYPE_INT64:
        case FD::TYPE_SINT64:
        case FD::TYPE_SFIXED64:
            throw_if_not_ok(static_cast<arrow::Int64Builder*>(builder)->Append(
                                reflection->GetRepeatedInt64(message, field, index)),
                            "append repeated protobuf int64");
            break;
        case FD::TYPE_UINT64:
        case FD::TYPE_FIXED64:
            throw_if_not_ok(static_cast<arrow::UInt64Builder*>(builder)->Append(
                                reflection->GetRepeatedUInt64(message, field, index)),
                            "append repeated protobuf uint64");
            break;
        case FD::TYPE_INT32:
        case FD::TYPE_SINT32:
        case FD::TYPE_SFIXED32:
            throw_if_not_ok(static_cast<arrow::Int32Builder*>(builder)->Append(
                                reflection->GetRepeatedInt32(message, field, index)),
                            "append repeated protobuf int32");
            break;
        case FD::TYPE_UINT32:
        case FD::TYPE_FIXED32:
            throw_if_not_ok(static_cast<arrow::UInt32Builder*>(builder)->Append(
                                reflection->GetRepeatedUInt32(message, field, index)),
                            "append repeated protobuf uint32");
            break;
        case FD::TYPE_BOOL:
            throw_if_not_ok(static_cast<arrow::BooleanBuilder*>(builder)->Append(
                                reflection->GetRepeatedBool(message, field, index)),
                            "append repeated protobuf bool");
            break;
        case FD::TYPE_STRING: {
            std::string scratch;
            const auto& value =
                reflection->GetRepeatedStringReference(message, field, index, &scratch);
            throw_if_not_ok(static_cast<arrow::StringBuilder*>(builder)->Append(value),
                            "append repeated protobuf string");
            break;
        }
        case FD::TYPE_BYTES: {
            std::string scratch;
            const auto& value =
                reflection->GetRepeatedStringReference(message, field, index, &scratch);
            throw_if_not_ok(static_cast<arrow::BinaryBuilder*>(builder)->Append(
                                reinterpret_cast<const uint8_t*>(value.data()),
                                static_cast<int32_t>(value.size())),
                            "append repeated protobuf bytes");
            break;
        }
        case FD::TYPE_ENUM: {
            const std::string value =
                enum_name(field, reflection->GetRepeatedEnumValue(message, field, index));
            throw_if_not_ok(static_cast<arrow::StringBuilder*>(builder)->Append(value),
                            "append repeated protobuf enum");
            break;
        }
        case FD::TYPE_MESSAGE:
            append_proto_singular_value(
                builder, reflection->GetRepeatedMessage(message, field, index), field);
            break;
        default:
            throw std::runtime_error("Unsupported repeated protobuf field type");
    }
}

void append_proto_singular_value(arrow::ArrayBuilder* builder,
                                 const google::protobuf::Message& message,
                                 const google::protobuf::FieldDescriptor* field) {
    using FD = google::protobuf::FieldDescriptor;
    const auto* reflection = message.GetReflection();
    switch (field->type()) {
        case FD::TYPE_DOUBLE:
            throw_if_not_ok(static_cast<arrow::DoubleBuilder*>(builder)->Append(
                                reflection->GetDouble(message, field)),
                            "append protobuf double");
            break;
        case FD::TYPE_FLOAT:
            throw_if_not_ok(static_cast<arrow::FloatBuilder*>(builder)->Append(
                                reflection->GetFloat(message, field)),
                            "append protobuf float");
            break;
        case FD::TYPE_INT64:
        case FD::TYPE_SINT64:
        case FD::TYPE_SFIXED64:
            throw_if_not_ok(static_cast<arrow::Int64Builder*>(builder)->Append(
                                reflection->GetInt64(message, field)),
                            "append protobuf int64");
            break;
        case FD::TYPE_UINT64:
        case FD::TYPE_FIXED64:
            throw_if_not_ok(static_cast<arrow::UInt64Builder*>(builder)->Append(
                                reflection->GetUInt64(message, field)),
                            "append protobuf uint64");
            break;
        case FD::TYPE_INT32:
        case FD::TYPE_SINT32:
        case FD::TYPE_SFIXED32:
            throw_if_not_ok(static_cast<arrow::Int32Builder*>(builder)->Append(
                                reflection->GetInt32(message, field)),
                            "append protobuf int32");
            break;
        case FD::TYPE_UINT32:
        case FD::TYPE_FIXED32:
            throw_if_not_ok(static_cast<arrow::UInt32Builder*>(builder)->Append(
                                reflection->GetUInt32(message, field)),
                            "append protobuf uint32");
            break;
        case FD::TYPE_BOOL:
            throw_if_not_ok(static_cast<arrow::BooleanBuilder*>(builder)->Append(
                                reflection->GetBool(message, field)),
                            "append protobuf bool");
            break;
        case FD::TYPE_STRING: {
            std::string scratch;
            const auto& value = reflection->GetStringReference(message, field, &scratch);
            throw_if_not_ok(static_cast<arrow::StringBuilder*>(builder)->Append(value),
                            "append protobuf string");
            break;
        }
        case FD::TYPE_BYTES: {
            std::string scratch;
            const auto& value = reflection->GetStringReference(message, field, &scratch);
            throw_if_not_ok(static_cast<arrow::BinaryBuilder*>(builder)->Append(
                                reinterpret_cast<const uint8_t*>(value.data()),
                                static_cast<int32_t>(value.size())),
                            "append protobuf bytes");
            break;
        }
        case FD::TYPE_ENUM: {
            const std::string value = enum_name(field, reflection->GetEnumValue(message, field));
            throw_if_not_ok(static_cast<arrow::StringBuilder*>(builder)->Append(value),
                            "append protobuf enum");
            break;
        }
        case FD::TYPE_MESSAGE: {
            const google::protobuf::Message* child = &message;
            if (child->GetDescriptor() != field->message_type()) {
                child = &reflection->GetMessage(message, field);
            }
            auto* struct_builder = static_cast<arrow::StructBuilder*>(builder);
            throw_if_not_ok(struct_builder->Append(), "append protobuf message");
            const auto* descriptor = field->message_type();
            for (int i = 0; i < descriptor->field_count(); ++i) {
                const auto* nested = descriptor->field(i);
                if (nested->is_repeated()) {
                    auto* list_builder =
                        static_cast<arrow::ListBuilder*>(struct_builder->field_builder(i));
                    throw_if_not_ok(list_builder->Append(), "append nested protobuf list");
                    const int size = child->GetReflection()->FieldSize(*child, nested);
                    for (int j = 0; j < size; ++j) {
                        append_proto_repeated_value(list_builder->value_builder(), *child,
                                                    nested, j);
                    }
                } else if (nested->has_presence() &&
                           !child->GetReflection()->HasField(*child, nested)) {
                    throw_if_not_ok(struct_builder->field_builder(i)->AppendNull(),
                                    "append missing nested protobuf field");
                } else {
                    append_proto_singular_value(struct_builder->field_builder(i), *child,
                                                nested);
                }
            }
            break;
        }
        default:
            throw std::runtime_error("Unsupported protobuf field type");
    }
}

// Shared per-record append logic, reused by both decode_batch (bare frames)
// and decode_delimited_file_each (a real multi-message file) so there's one
// place that walks a decoded message's fields into the builders.
void append_proto_message_row(std::vector<std::unique_ptr<arrow::ArrayBuilder>>& builders,
                              const google::protobuf::Message& message,
                              const google::protobuf::Descriptor* descriptor) {
    const auto* reflection = message.GetReflection();
    for (int i = 0; i < descriptor->field_count(); ++i) {
        const auto* field = descriptor->field(i);
        if (field->is_repeated()) {
            auto* list_builder =
                static_cast<arrow::ListBuilder*>(builders[static_cast<size_t>(i)].get());
            throw_if_not_ok(list_builder->Append(), "append protobuf repeated field");
            const int size = reflection->FieldSize(message, field);
            for (int j = 0; j < size; ++j) {
                append_proto_repeated_value(list_builder->value_builder(), message, field, j);
            }
        } else if (field->has_presence() && !reflection->HasField(message, field)) {
            throw_if_not_ok(builders[static_cast<size_t>(i)]->AppendNull(),
                            "append missing protobuf field");
        } else {
            append_proto_singular_value(builders[static_cast<size_t>(i)].get(), message, field);
        }
    }
}

std::vector<std::unique_ptr<arrow::ArrayBuilder>> make_proto_builders(
    const arrow::Schema& schema) {
    std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders;
    builders.reserve(static_cast<size_t>(schema.num_fields()));
    for (const auto& field : schema.fields()) {
        builders.push_back(
            value_or_throw(arrow::MakeBuilder(field->type()), "create protobuf Arrow builder"));
    }
    return builders;
}

std::shared_ptr<arrow::RecordBatch> finish_proto_batch(
    const std::shared_ptr<arrow::Schema>& schema,
    std::vector<std::unique_ptr<arrow::ArrayBuilder>>& builders, int64_t rows) {
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    arrays.reserve(builders.size());
    for (auto& builder : builders) {
        arrays.push_back(value_or_throw(builder->Finish(), "finish protobuf Arrow array"));
    }
    return arrow::RecordBatch::Make(schema, rows, std::move(arrays));
}

}  // namespace

struct ProtobufDecoderImpl {
    google::protobuf::FileDescriptorSet descriptor_set;
    google::protobuf::DescriptorPool pool;
    google::protobuf::DynamicMessageFactory factory;
    const google::protobuf::Descriptor* descriptor = nullptr;
    const google::protobuf::Message* prototype = nullptr;

    ProtobufDecoderImpl(const std::string& descriptor_set_bytes,
                        const std::string& message_type)
        : pool(google::protobuf::DescriptorPool::generated_pool()), factory(&pool) {
        if (!descriptor_set.ParseFromString(descriptor_set_bytes)) {
            throw std::runtime_error(
                "protobuf descriptor_set_bytes is not a serialized FileDescriptorSet");
        }

        std::vector<bool> built(static_cast<size_t>(descriptor_set.file_size()), false);
        int built_count = 0;
        while (built_count < descriptor_set.file_size()) {
            bool progressed = false;
            for (int i = 0; i < descriptor_set.file_size(); ++i) {
                if (built[static_cast<size_t>(i)]) continue;
                const google::protobuf::FileDescriptor* fd =
                    pool.BuildFile(descriptor_set.file(i));
                if (fd) {
                    built[static_cast<size_t>(i)] = true;
                    ++built_count;
                    progressed = true;
                }
            }
            if (!progressed) {
                throw std::runtime_error(
                    "protobuf descriptor set has unresolved imports or invalid files");
            }
        }

        descriptor = pool.FindMessageTypeByName(message_type);
        if (!descriptor) throw std::runtime_error("protobuf message type not found: " + message_type);

        prototype = factory.GetPrototype(descriptor);
        if (!prototype) {
            throw std::runtime_error("protobuf prototype creation failed: " + message_type);
        }
    }
};

ProtobufDecoder::ProtobufDecoder(std::string descriptor_set_bytes, std::string message_type)
    : descriptor_set_bytes_(std::move(descriptor_set_bytes)),
      message_type_(std::move(message_type)),
      impl_(std::make_shared<ProtobufDecoderImpl>(descriptor_set_bytes_, message_type_)) {}

std::string ProtobufDecoder::decode_ndjson(const std::vector<std::string_view>& frames) const {
    google::protobuf::util::JsonPrintOptions options;
    options.preserve_proto_field_names = true;

    std::string out;
    for (std::string_view frame : frames) {
        if (frame.empty()) continue;
        frame = strip_confluent_protobuf_prefix(frame);
        std::unique_ptr<google::protobuf::Message> message(impl_->prototype->New());
        if (!message->ParseFromArray(frame.data(), checked_frame_size(frame))) {
            throw std::runtime_error("protobuf message parse failed");
        }
        std::string json;
        auto status = google::protobuf::util::MessageToJsonString(*message, &json, options);
        if (!status.ok()) throw std::runtime_error(status.ToString());
        out.append(json);
        out.push_back('\n');
    }
    return out;
}

std::shared_ptr<arrow::RecordBatch> ProtobufDecoder::decode_batch(
    const std::vector<std::string_view>& frames) const {
    auto schema = arrow_schema_from_proto_descriptor(impl_->descriptor);
    auto builders = make_proto_builders(*schema);
    for (auto& builder : builders) {
        throw_if_not_ok(builder->Reserve(static_cast<int64_t>(frames.size())),
                        "reserve protobuf Arrow builder");
    }

    int64_t rows = 0;
    for (std::string_view frame : frames) {
        if (frame.empty()) continue;
        frame = strip_confluent_protobuf_prefix(frame);
        std::unique_ptr<google::protobuf::Message> message(impl_->prototype->New());
        if (!message->ParseFromArray(frame.data(), checked_frame_size(frame))) {
            throw std::runtime_error("protobuf message parse failed");
        }
        append_proto_message_row(builders, *message, impl_->descriptor);
        ++rows;
    }

    if (rows == 0) return nullptr;
    return finish_proto_batch(schema, builders, rows);
}

int64_t ProtobufDecoder::decode_delimited_file_each(const std::string& path,
                                                    const RecordBatchVisitor& visitor,
                                                    int64_t batch_size) const {
    if (batch_size <= 0) {
        throw std::runtime_error("decode_delimited_file_each: batch_size must be > 0");
    }

    std::string data = read_whole_file(path);

    // Cheap, sequential pass: just find each message's byte boundaries (no
    // protobuf parsing yet). Reuses the already-proven decode_batch below for
    // the actual parsing, one batch_size-sized chunk at a time, instead of
    // duplicating per-record parse logic here.
    std::vector<std::string_view> frames = extract_delimited_frames(data);

    int64_t total_rows = 0;
    for (size_t start = 0; start < frames.size(); start += static_cast<size_t>(batch_size)) {
        size_t end = std::min(frames.size(), start + static_cast<size_t>(batch_size));
        std::vector<std::string_view> chunk(frames.begin() + static_cast<long>(start),
                                            frames.begin() + static_cast<long>(end));
        auto batch = decode_batch(chunk);
        if (!batch) continue;
        total_rows += batch->num_rows();
        if (!visitor(batch)) break;
    }
    return total_rows;
}

int64_t ProtobufDecoder::decode_delimited_file_parallel(const std::string& path,
                                                        const RecordBatchVisitor& visitor,
                                                        int64_t batch_size,
                                                        int worker_count) const {
    if (worker_count <= 1) return decode_delimited_file_each(path, visitor, batch_size);
    if (batch_size <= 0) {
        throw std::runtime_error("decode_delimited_file_parallel: batch_size must be > 0");
    }

    std::string data = read_whole_file(path);
    std::vector<std::string_view> frames = extract_delimited_frames(data);

    const size_t chunk_size = static_cast<size_t>(batch_size);
    const size_t chunk_count = (frames.size() + chunk_size - 1) / chunk_size;
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches(chunk_count);

    // decode_batch only reads impl_ (descriptor/prototype for New()); it
    // creates a fresh Message + fresh builders per call and mutates nothing
    // shared, so calling it concurrently across chunks from multiple threads
    // is safe -- no engine/RuleEngine involvement at this stage (that stays
    // single-threaded, sequential, and unchanged below), only the CPU-heavy
    // protobuf reflection-based parsing is parallelized.
    tbb::parallel_for(tbb::blocked_range<size_t>(0, chunk_count), [&](const auto& range) {
        for (size_t i = range.begin(); i != range.end(); ++i) {
            size_t start = i * chunk_size;
            size_t end = std::min(frames.size(), start + chunk_size);
            std::vector<std::string_view> chunk(frames.begin() + static_cast<long>(start),
                                                frames.begin() + static_cast<long>(end));
            batches[i] = decode_batch(chunk);
        }
    });

    int64_t total_rows = 0;
    for (auto& batch : batches) {
        if (!batch) continue;
        total_rows += batch->num_rows();
        if (!visitor(batch)) break;
    }
    return total_rows;
}

}  // namespace blazerules_io

#endif  // BLAZERULES_IO_PROTOBUF
