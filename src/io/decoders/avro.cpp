#include "blazerules_io/decoder.h"

#ifdef BLAZERULES_IO_AVRO

#include <avro/Compiler.hh>
#include <avro/DataFile.hh>
#include <avro/Decoder.hh>
#include <avro/Encoder.hh>
#include <avro/Generic.hh>
#include <avro/Stream.hh>

#include <arrow/api.h>

#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace blazerules_io {

namespace {

std::string_view strip_confluent_avro_prefix(std::string_view frame) {
    if (frame.size() > 5 && static_cast<unsigned char>(frame[0]) == 0x00) {
        return frame.substr(5);
    }
    return frame;
}

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

struct AvroUnionInfo {
    bool nullable = false;
    avro::NodePtr value_node;
};

AvroUnionInfo unwrap_nullable_avro_node(const avro::NodePtr& node) {
    if (node->type() != avro::AVRO_UNION) return {false, node};

    AvroUnionInfo out;
    for (size_t i = 0; i < node->leaves(); ++i) {
        const auto& leaf = node->leafAt(i);
        if (leaf->type() == avro::AVRO_NULL) {
            out.nullable = true;
        } else if (!out.value_node) {
            out.value_node = leaf;
        } else {
            throw std::runtime_error(
                "Avro direct Arrow decode supports nullable unions with one non-null branch");
        }
    }
    if (!out.value_node) out.value_node = node->leafAt(0);
    return out;
}

std::shared_ptr<arrow::DataType> arrow_type_from_avro(const avro::NodePtr& node);

std::shared_ptr<arrow::Field> arrow_field_from_avro(const std::string& name,
                                                    const avro::NodePtr& node) {
    auto unwrapped = unwrap_nullable_avro_node(node);
    return arrow::field(name, arrow_type_from_avro(unwrapped.value_node), unwrapped.nullable);
}

std::shared_ptr<arrow::DataType> arrow_type_from_avro(const avro::NodePtr& node) {
    auto unwrapped = unwrap_nullable_avro_node(node);
    const auto& value_node = unwrapped.value_node;
    switch (value_node->type()) {
        case avro::AVRO_BOOL:
            return arrow::boolean();
        case avro::AVRO_INT:
            return arrow::int32();
        case avro::AVRO_LONG:
            return arrow::int64();
        case avro::AVRO_FLOAT:
            return arrow::float32();
        case avro::AVRO_DOUBLE:
            return arrow::float64();
        case avro::AVRO_STRING:
        case avro::AVRO_ENUM:
            return arrow::utf8();
        case avro::AVRO_BYTES:
        case avro::AVRO_FIXED:
            return arrow::binary();
        case avro::AVRO_ARRAY:
            return arrow::list(arrow_field_from_avro("item", value_node->leafAt(0)));
        case avro::AVRO_RECORD: {
            std::vector<std::shared_ptr<arrow::Field>> fields;
            fields.reserve(value_node->leaves());
            for (size_t i = 0; i < value_node->leaves(); ++i) {
                fields.push_back(arrow_field_from_avro(value_node->nameAt(i),
                                                       value_node->leafAt(i)));
            }
            return arrow::struct_(std::move(fields));
        }
        default:
            throw std::runtime_error("Unsupported Avro type for direct Arrow decode");
    }
}

std::shared_ptr<arrow::Schema> arrow_schema_from_avro_record(const avro::NodePtr& root) {
    auto unwrapped = unwrap_nullable_avro_node(root);
    if (unwrapped.value_node->type() != avro::AVRO_RECORD) {
        throw std::runtime_error("Avro direct Arrow decode expects a top-level record schema");
    }
    std::vector<std::shared_ptr<arrow::Field>> fields;
    fields.reserve(unwrapped.value_node->leaves());
    for (size_t i = 0; i < unwrapped.value_node->leaves(); ++i) {
        fields.push_back(arrow_field_from_avro(unwrapped.value_node->nameAt(i),
                                               unwrapped.value_node->leafAt(i)));
    }
    return arrow::schema(std::move(fields));
}

void append_avro_value(arrow::ArrayBuilder* builder, const avro::GenericDatum& datum) {
    if (datum.type() == avro::AVRO_NULL) {
        throw_if_not_ok(builder->AppendNull(), "append Avro null");
        return;
    }

    switch (datum.type()) {
        case avro::AVRO_BOOL:
            throw_if_not_ok(static_cast<arrow::BooleanBuilder*>(builder)->Append(
                                datum.value<bool>()),
                            "append Avro bool");
            break;
        case avro::AVRO_INT:
            throw_if_not_ok(static_cast<arrow::Int32Builder*>(builder)->Append(
                                datum.value<int32_t>()),
                            "append Avro int");
            break;
        case avro::AVRO_LONG:
            throw_if_not_ok(static_cast<arrow::Int64Builder*>(builder)->Append(
                                datum.value<int64_t>()),
                            "append Avro long");
            break;
        case avro::AVRO_FLOAT:
            throw_if_not_ok(static_cast<arrow::FloatBuilder*>(builder)->Append(
                                datum.value<float>()),
                            "append Avro float");
            break;
        case avro::AVRO_DOUBLE:
            throw_if_not_ok(static_cast<arrow::DoubleBuilder*>(builder)->Append(
                                datum.value<double>()),
                            "append Avro double");
            break;
        case avro::AVRO_STRING: {
            const auto& value = datum.value<std::string>();
            throw_if_not_ok(static_cast<arrow::StringBuilder*>(builder)->Append(value),
                            "append Avro string");
            break;
        }
        case avro::AVRO_BYTES: {
            const auto& value = datum.value<std::vector<uint8_t>>();
            throw_if_not_ok(static_cast<arrow::BinaryBuilder*>(builder)->Append(
                                value.data(), static_cast<int32_t>(value.size())),
                            "append Avro bytes");
            break;
        }
        case avro::AVRO_FIXED: {
            const auto& value = datum.value<avro::GenericFixed>().value();
            throw_if_not_ok(static_cast<arrow::BinaryBuilder*>(builder)->Append(
                                value.data(), static_cast<int32_t>(value.size())),
                            "append Avro fixed");
            break;
        }
        case avro::AVRO_ENUM: {
            const auto& value = datum.value<avro::GenericEnum>().symbol();
            throw_if_not_ok(static_cast<arrow::StringBuilder*>(builder)->Append(value),
                            "append Avro enum");
            break;
        }
        case avro::AVRO_ARRAY: {
            auto* list_builder = static_cast<arrow::ListBuilder*>(builder);
            throw_if_not_ok(list_builder->Append(), "append Avro array");
            auto* value_builder = list_builder->value_builder();
            for (const auto& value : datum.value<avro::GenericArray>().value()) {
                append_avro_value(value_builder, value);
            }
            break;
        }
        case avro::AVRO_RECORD: {
            const auto& record = datum.value<avro::GenericRecord>();
            auto* struct_builder = static_cast<arrow::StructBuilder*>(builder);
            throw_if_not_ok(struct_builder->Append(), "append Avro record");
            for (size_t i = 0; i < record.fieldCount(); ++i) {
                append_avro_value(struct_builder->field_builder(static_cast<int>(i)),
                                  record.fieldAt(i));
            }
            break;
        }
        default:
            throw std::runtime_error("Unsupported Avro datum type for direct Arrow decode");
    }
}

}  // namespace

AvroDecoder::AvroDecoder(std::string schema_json)
    : schema_json_(std::move(schema_json)),
      schema_(std::make_shared<avro::ValidSchema>(
          avro::compileJsonSchemaFromString(schema_json_))) {}

std::string AvroDecoder::decode_ndjson(const std::vector<std::string_view>& frames) const {
    std::string out;
    auto decoder = avro::binaryDecoder();
    auto encoder = avro::jsonEncoder(*schema_);
    for (std::string_view frame : frames) {
        if (frame.empty()) continue;
        frame = strip_confluent_avro_prefix(frame);
        auto input = avro::memoryInputStream(
            reinterpret_cast<const uint8_t*>(frame.data()), frame.size());
        decoder->init(*input);
        avro::GenericDatum datum(*schema_);
        avro::GenericReader reader(*schema_, decoder);
        reader.read(datum);

        auto output = avro::memoryOutputStream();
        encoder->init(*output);
        avro::GenericWriter writer(*schema_, encoder);
        writer.write(datum);
        encoder->flush();
        auto bytes = avro::snapshot(*output);
        out.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        out.push_back('\n');
    }
    return out;
}

std::shared_ptr<arrow::RecordBatch> AvroDecoder::decode_batch(
    const std::vector<std::string_view>& frames) const {
    auto arrow_schema = arrow_schema_from_avro_record(schema_->root());
    std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders;
    builders.reserve(static_cast<size_t>(arrow_schema->num_fields()));
    for (const auto& field : arrow_schema->fields()) {
        auto builder = value_or_throw(arrow::MakeBuilder(field->type()),
                                      "create Avro Arrow builder");
        throw_if_not_ok(builder->Reserve(static_cast<int64_t>(frames.size())),
                        "reserve Avro Arrow builder");
        builders.push_back(std::move(builder));
    }

    auto decoder = avro::binaryDecoder();
    avro::GenericReader reader(*schema_, decoder);
    int64_t rows = 0;
    for (std::string_view frame : frames) {
        if (frame.empty()) continue;
        frame = strip_confluent_avro_prefix(frame);
        auto input = avro::memoryInputStream(
            reinterpret_cast<const uint8_t*>(frame.data()), frame.size());
        decoder->init(*input);
        avro::GenericDatum datum(*schema_);
        reader.read(datum);

        const auto& record = datum.value<avro::GenericRecord>();
        for (size_t i = 0; i < builders.size(); ++i) {
            append_avro_value(builders[i].get(), record.fieldAt(i));
        }
        ++rows;
    }

    if (rows == 0) return nullptr;

    std::vector<std::shared_ptr<arrow::Array>> arrays;
    arrays.reserve(builders.size());
    for (auto& builder : builders) {
        arrays.push_back(value_or_throw(builder->Finish(), "finish Avro Arrow array"));
    }
    return arrow::RecordBatch::Make(arrow_schema, rows, std::move(arrays));
}

bool looks_like_avro_ocf(std::string_view bytes) {
    static constexpr char kMagic[4] = {'O', 'b', 'j', static_cast<char>(0x01)};
    return bytes.size() >= sizeof(kMagic) &&
           std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) == 0;
}

// No parallel counterpart yet, unlike Protobuf's decode_delimited_file_parallel:
// DataFileReaderBase's decoder() tracks a single mutable cursor position
// through the file's block/codec/sync-marker structure (not reentrant), and
// while DataFileReaderBase does expose seek()/sync()/pastSync() -- the
// mechanism Hadoop/Spark use to split one OCF file into independent
// byte-ranges per worker -- its exact semantics need validating against a
// real multi-block, multi-codec fixture before relying on it (not done here).
// Until then, this stays sequential; Phase 1's single-threaded decode+eval
// already accounts for the bulk of the available throughput.
int64_t decode_avro_ocf_file_each(const std::string& path,
                                  const RecordBatchVisitor& visitor,
                                  int64_t batch_size) {
    if (batch_size <= 0) throw std::runtime_error("decode_avro_ocf_file_each: batch_size must be > 0");

    avro::DataFileReaderBase base(path.c_str());
    base.init();  // reader schema == the file's own embedded (writer) schema

    const avro::ValidSchema& schema = base.dataSchema();
    auto arrow_schema = arrow_schema_from_avro_record(schema.root());

    int64_t total_rows = 0;
    while (base.hasMore()) {
        std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders;
        builders.reserve(static_cast<size_t>(arrow_schema->num_fields()));
        for (const auto& field : arrow_schema->fields()) {
            builders.push_back(
                value_or_throw(arrow::MakeBuilder(field->type()), "create Avro Arrow builder"));
        }

        int64_t rows_in_batch = 0;
        while (base.hasMore() && rows_in_batch < batch_size) {
            base.decr();
            avro::GenericDatum datum(schema);
            // Static 3-arg form: takes the Decoder& DataFileReaderBase already
            // manages directly (tracks block/codec/sync-marker position
            // internally), with an explicit schema -- not the ambiguous 2-arg
            // static overload, which trusts whatever schema the datum already
            // has, nor the instance-based GenericReader, which wants a
            // DecoderPtr (shared_ptr) rather than the plain reference
            // DataFileReaderBase::decoder() returns.
            avro::GenericReader::read(base.decoder(), datum, schema);

            const auto& record = datum.value<avro::GenericRecord>();
            for (size_t i = 0; i < builders.size(); ++i) {
                append_avro_value(builders[i].get(), record.fieldAt(i));
            }
            ++rows_in_batch;
            ++total_rows;
        }

        std::vector<std::shared_ptr<arrow::Array>> arrays;
        arrays.reserve(builders.size());
        for (auto& builder : builders) {
            arrays.push_back(value_or_throw(builder->Finish(), "finish Avro Arrow array"));
        }
        auto out_batch = arrow::RecordBatch::Make(arrow_schema, rows_in_batch, std::move(arrays));
        if (!visitor(out_batch)) break;
    }

    return total_rows;
}

}  // namespace blazerules_io

#endif  // BLAZERULES_IO_AVRO
