#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <arrow/c/abi.h>
#include <arrow/c/bridge.h>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "blazerules/resource_resolver.h"
#include "blazerules_io/cdc.h"
#include "blazerules_io/decoder.h"
#include "blazerules_io/file_reader.h"
#ifdef BLAZERULES_IO_KAFKA
#include "blazerules_io/kafka.h"
#include "blazerules_io/stream_runtime.h"
#endif

namespace py = pybind11;

namespace {

struct BorrowedFrames {
    std::vector<py::object> keepalive;
    std::vector<std::string_view> views;
};

BorrowedFrames borrow_frames(py::sequence seq) {
    BorrowedFrames out;
    out.keepalive.reserve(static_cast<size_t>(py::len(seq)));
    out.views.reserve(static_cast<size_t>(py::len(seq)));
    for (py::handle item : seq) {
        Py_ssize_t size = 0;
        const char* data = nullptr;
        if (PyBytes_Check(item.ptr())) {
            data = PyBytes_AsString(item.ptr());
            size = PyBytes_Size(item.ptr());
            out.keepalive.emplace_back(py::reinterpret_borrow<py::object>(item));
        } else {
            data = PyUnicode_AsUTF8AndSize(item.ptr(), &size);
            if (!data) throw py::type_error("frames must contain bytes or str");
            out.keepalive.emplace_back(py::reinterpret_borrow<py::object>(item));
        }
        out.views.emplace_back(data, static_cast<size_t>(size));
    }
    return out;
}

void release_arrow_schema_capsule(PyObject* capsule) {
    const char* name = PyCapsule_GetName(capsule);
    if (!name) {
        PyErr_Clear();
        return;
    }
    auto* schema = reinterpret_cast<ArrowSchema*>(PyCapsule_GetPointer(capsule, name));
    if (!schema) {
        PyErr_Clear();
        return;
    }
    if (schema->release) schema->release(schema);
    delete schema;
}

void release_arrow_array_capsule(PyObject* capsule) {
    const char* name = PyCapsule_GetName(capsule);
    if (!name) {
        PyErr_Clear();
        return;
    }
    auto* array = reinterpret_cast<ArrowArray*>(PyCapsule_GetPointer(capsule, name));
    if (!array) {
        PyErr_Clear();
        return;
    }
    if (array->release) array->release(array);
    delete array;
}

py::object record_batch_to_pyarrow(const std::shared_ptr<arrow::RecordBatch>& batch) {
    if (!batch) return py::none();
    auto* c_schema = new ArrowSchema{};
    auto* c_array = new ArrowArray{};
    arrow::Status status = arrow::ExportRecordBatch(*batch, c_array, c_schema);
    if (!status.ok()) {
        delete c_schema;
        delete c_array;
        throw std::runtime_error(status.ToString());
    }
    py::capsule schema_capsule(c_schema, "arrow_schema", release_arrow_schema_capsule);
    py::capsule array_capsule(c_array, "arrow_array", release_arrow_array_capsule);
    py::object pa = py::module_::import("pyarrow");
    return pa.attr("RecordBatch").attr("_import_from_c_capsule")(schema_capsule, array_capsule);
}

py::list batches_to_pyarrow_list(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) {
    py::list out;
    for (const auto& batch : batches) out.append(record_batch_to_pyarrow(batch));
    return out;
}

}  // namespace

PYBIND11_MODULE(blazerules_io, m) {
    m.doc() = "BlazeRules streaming/IO connectors (CDC, Kafka, decoders).";

    m.def("set_aws_profile", &blazerules::set_aws_profile,
          py::arg("profile"), py::arg("clear_env_credentials") = true,
          "Use an AWS CLI profile for s3:// file reads. Stale key-based env credentials "
          "are ignored when clear_env_credentials is true.");
    m.def("clear_aws_profile", &blazerules::clear_aws_profile,
          "Clear the explicit AWS profile set for BlazeRules IO S3 resolution.");
    m.def("current_aws_profile", &blazerules::current_aws_profile,
          "Return BLAZERULES_AWS_PROFILE or AWS_PROFILE if set.");
    m.def("set_aws_region", &blazerules::set_aws_region,
          py::arg("region"),
          "Set the AWS region for s3:// file reads. Also updates AWS_REGION/AWS_DEFAULT_REGION.");
    m.def("clear_aws_region", &blazerules::clear_aws_region,
          "Clear the explicit AWS region for S3 file reads.");
    m.def("current_aws_region", &blazerules::current_aws_region,
          "Return BLAZERULES_AWS_REGION, AWS_REGION, or AWS_DEFAULT_REGION if set.");
    m.def("set_aws_endpoint_url", &blazerules::set_aws_endpoint_url,
          py::arg("endpoint_url"),
          "Set a custom S3 endpoint URL for file reads, e.g. MinIO/R2/LocalStack.");
    m.def("clear_aws_endpoint_url", &blazerules::clear_aws_endpoint_url,
          "Clear the custom S3 endpoint URL.");
    m.def("current_aws_endpoint_url", &blazerules::current_aws_endpoint_url,
          "Return BLAZERULES_AWS_ENDPOINT_URL, AWS_ENDPOINT_URL, or AWS_ENDPOINT_URL_S3 if set.");
    m.def("set_aws_credentials", &blazerules::set_aws_credentials,
          py::arg("access_key_id"), py::arg("secret_access_key"),
          py::arg("session_token") = "", py::arg("region") = "",
          "Set process environment credentials for AWS CLI based s3:// file reads. "
          "Prefer set_aws_profile for long-running production processes.");
    m.def("clear_aws_credentials", &blazerules::clear_aws_credentials,
          "Clear AWS_ACCESS_KEY_ID/AWS_SECRET_ACCESS_KEY/AWS_SESSION_TOKEN for this process.");

    m.def(
        "unwrap_debezium",
        [](py::sequence messages, const std::string& op_field) -> py::bytes {
            // Copy payloads into owned strings (CDC is a cold/setup-side transform, not
            // the eval hot path), then view them for the decoder.
            std::vector<std::string> owned;
            owned.reserve(static_cast<size_t>(py::len(messages)));
            for (py::handle item : messages) owned.emplace_back(item.cast<std::string>());
            std::vector<std::string_view> views;
            views.reserve(owned.size());
            for (const auto& s : owned) views.emplace_back(s);
            return py::bytes(blazerules_io::unwrap_debezium(views, op_field));
        },
        py::arg("messages"), py::arg("op_field") = "__op",
        "Unwrap Debezium CDC envelopes (list of str/bytes) into NDJSON bytes suitable for "
        "RuleEngine.evaluate_ndjson. Injects op_field (default '__op') so rules can match "
        "the operation; passes through already-flattened messages.");

    py::enum_<blazerules_io::FileFormat>(m, "FileFormat")
        .value("AUTO", blazerules_io::FileFormat::AUTO)
        .value("ARROW_IPC", blazerules_io::FileFormat::ARROWIPC)
        .value("PARQUET", blazerules_io::FileFormat::PARQUET)
        .value("CSV", blazerules_io::FileFormat::CSV)
        .value("NDJSON", blazerules_io::FileFormat::NDJSON);

    py::class_<blazerules_io::ArrowIpcDecoder>(m, "ArrowIpcDecoder")
        .def(py::init<>())
        .def("decode_batches",
             [](const blazerules_io::ArrowIpcDecoder& d, py::sequence frames) {
                 auto borrowed = borrow_frames(frames);
                 std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
                 {
                     py::gil_scoped_release release;
                     batches = d.decode_batches(borrowed.views);
                 }
                 return batches_to_pyarrow_list(batches);
             },
             py::arg("frames"),
             "Decode Arrow IPC stream/file payloads into a list of pyarrow.RecordBatch objects.")
        .def("decode_batch",
             [](const blazerules_io::ArrowIpcDecoder& d, py::sequence frames) {
                 auto borrowed = borrow_frames(frames);
                 std::shared_ptr<arrow::RecordBatch> batch;
                 {
                     py::gil_scoped_release release;
                     batch = d.decode_batch(borrowed.views);
                 }
                 return record_batch_to_pyarrow(batch);
             },
             py::arg("frames"),
             "Decode Arrow IPC frames and combine them into one pyarrow.RecordBatch.")
        .def("decode_file",
             [](const blazerules_io::ArrowIpcDecoder& d, const std::string& path) {
                 std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
                 {
                     py::gil_scoped_release release;
                     batches = d.decode_file(path);
                 }
                 return batches_to_pyarrow_list(batches);
             },
             py::arg("path"));

    m.def("read_record_batches",
          [](const std::string& path, const std::string& format, int64_t batch_size) {
              std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
              {
                  py::gil_scoped_release release;
                  batches = blazerules_io::read_record_batches(
                      path, blazerules_io::parse_file_format(format), batch_size);
              }
              return batches_to_pyarrow_list(batches);
          },
          py::arg("path"), py::arg("format") = "auto", py::arg("batch_size") = 65536,
          "Read Arrow IPC, Parquet, CSV, or NDJSON/JSON files into pyarrow.RecordBatch objects.")
        ;

    m.def("read_ndjson_bytes",
          [](const std::string& path) {
              std::string bytes;
              {
                  py::gil_scoped_release release;
                  bytes = blazerules_io::read_ndjson_bytes(path);
              }
              return py::bytes(bytes);
          },
          py::arg("path"),
          "Read local or s3:// NDJSON bytes for RuleEngine.evaluate_ndjson.");

#ifdef BLAZERULES_IO_PROTOBUF
    py::class_<blazerules_io::ProtobufDecoder>(m, "ProtobufDecoder")
        .def(py::init<std::string, std::string>(),
             py::arg("descriptor_set_bytes"), py::arg("message_type"))
        .def("decode_batch",
             [](const blazerules_io::ProtobufDecoder& d, py::sequence frames) {
                 auto borrowed = borrow_frames(frames);
                 std::shared_ptr<arrow::RecordBatch> batch;
                 {
                     py::gil_scoped_release release;
                     batch = d.decode_batch(borrowed.views);
                 }
                 return record_batch_to_pyarrow(batch);
             },
             py::arg("frames"),
             "Decode binary protobuf frames to one pyarrow.RecordBatch.")
        .def("decode_ndjson",
             [](const blazerules_io::ProtobufDecoder& d, py::sequence frames) {
                 auto borrowed = borrow_frames(frames);
                 std::string out;
                 {
                     py::gil_scoped_release release;
                     out = d.decode_ndjson(borrowed.views);
                 }
                 return py::bytes(out);
             },
             py::arg("frames"),
             "Decode binary protobuf frames to NDJSON using a serialized FileDescriptorSet.");
    m.attr("has_protobuf") = true;
#else
    m.attr("has_protobuf") = false;
#endif

#ifdef BLAZERULES_IO_AVRO
    py::class_<blazerules_io::AvroDecoder>(m, "AvroDecoder")
        .def(py::init<std::string>(), py::arg("schema_json"))
        .def("decode_batch",
             [](const blazerules_io::AvroDecoder& d, py::sequence frames) {
                 auto borrowed = borrow_frames(frames);
                 std::shared_ptr<arrow::RecordBatch> batch;
                 {
                     py::gil_scoped_release release;
                     batch = d.decode_batch(borrowed.views);
                 }
                 return record_batch_to_pyarrow(batch);
             },
             py::arg("frames"),
             "Decode binary Avro frames to one pyarrow.RecordBatch.")
        .def("decode_ndjson",
             [](const blazerules_io::AvroDecoder& d, py::sequence frames) {
                 auto borrowed = borrow_frames(frames);
                 std::string out;
                 {
                     py::gil_scoped_release release;
                     out = d.decode_ndjson(borrowed.views);
                 }
                 return py::bytes(out);
             },
             py::arg("frames"));
    m.attr("has_avro") = true;
#else
    m.attr("has_avro") = false;
#endif

#ifdef BLAZERULES_IO_KAFKA
    py::class_<blazerules_io::KafkaMessage>(m, "KafkaMessage")
        .def_readonly("topic", &blazerules_io::KafkaMessage::topic)
        .def_readonly("partition", &blazerules_io::KafkaMessage::partition)
        .def_readonly("offset", &blazerules_io::KafkaMessage::offset)
        .def_readonly("timestamp_ms", &blazerules_io::KafkaMessage::timestamp_ms)
        .def_readonly("key", &blazerules_io::KafkaMessage::key)
        .def_property_readonly("value",
             [](const blazerules_io::KafkaMessage& m) { return py::bytes(m.value); });

    py::class_<blazerules_io::KafkaConsumer>(m, "KafkaConsumer")
        .def(py::init<const std::string&, const std::string&, const std::vector<std::string>&,
                      const std::map<std::string, std::string>&>(),
             py::arg("brokers"), py::arg("group_id"), py::arg("topics"),
             py::arg("conf") = std::map<std::string, std::string>{})
        .def("poll_batch",
             [](blazerules_io::KafkaConsumer& c, int max_messages, int timeout_ms) {
                 std::vector<std::string> batch;
                 {
                     py::gil_scoped_release release;
                     batch = c.poll_batch(max_messages, timeout_ms);
                 }
                 py::list out;
                 for (auto& s : batch) out.append(py::bytes(s));  // bytes: payloads may be binary
                 return out;
             },
             py::arg("max_messages") = 1000, py::arg("timeout_ms") = 1000,
             "Poll up to max_messages (waiting up to timeout_ms for the first); returns a "
             "list of message payloads as bytes, ready for RuleEngine.evaluate_messages.")
        .def("poll_records",
             [](blazerules_io::KafkaConsumer& c, int max_messages, int timeout_ms) {
                 std::vector<blazerules_io::KafkaMessage> batch;
                 {
                     py::gil_scoped_release release;
                     batch = c.poll_records(max_messages, timeout_ms);
                 }
                 return batch;
             },
             py::arg("max_messages") = 1000, py::arg("timeout_ms") = 1000,
             "Poll Kafka records with topic/partition/offset/key/timestamp metadata and "
             "payload bytes. Use this for CDC-style decision output and partition-affine "
             "evaluation.")
        .def("commit", &blazerules_io::KafkaConsumer::commit, py::call_guard<py::gil_scoped_release>(),
             "Synchronously commit consumed offsets (call after a batch is evaluated).")
        .def("close", &blazerules_io::KafkaConsumer::close, py::call_guard<py::gil_scoped_release>());

    py::class_<blazerules_io::KafkaProducer>(m, "KafkaProducer")
        .def(py::init<const std::string&, const std::map<std::string, std::string>&>(),
             py::arg("brokers"), py::arg("conf") = std::map<std::string, std::string>{})
        .def("produce", &blazerules_io::KafkaProducer::produce,
             py::arg("topic"), py::arg("value"), py::arg("key") = std::string(),
             "Produce a message (decision/enriched row) to a topic.")
        .def("flush", &blazerules_io::KafkaProducer::flush, py::arg("timeout_ms") = 5000,
             py::call_guard<py::gil_scoped_release>());

    py::class_<blazerules_io::StreamRunConfig>(m, "StreamRunConfig")
        .def(py::init<>())
        .def_readwrite("brokers", &blazerules_io::StreamRunConfig::brokers)
        .def_readwrite("group_id", &blazerules_io::StreamRunConfig::group_id)
        .def_readwrite("input_topics", &blazerules_io::StreamRunConfig::input_topics)
        .def_readwrite("output_topic", &blazerules_io::StreamRunConfig::output_topic)
        .def_readwrite("dlq_topic", &blazerules_io::StreamRunConfig::dlq_topic)
        .def_readwrite("consumer_conf", &blazerules_io::StreamRunConfig::consumer_conf)
        .def_readwrite("producer_conf", &blazerules_io::StreamRunConfig::producer_conf)
        .def_readwrite("batch_size", &blazerules_io::StreamRunConfig::batch_size)
        .def_readwrite("poll_timeout_ms", &blazerules_io::StreamRunConfig::poll_timeout_ms)
        .def_readwrite("flush_timeout_ms", &blazerules_io::StreamRunConfig::flush_timeout_ms)
        .def_readwrite("max_messages", &blazerules_io::StreamRunConfig::max_messages)
        .def_readwrite("max_batches", &blazerules_io::StreamRunConfig::max_batches)
        .def_readwrite("commit_offsets", &blazerules_io::StreamRunConfig::commit_offsets)
        .def_readwrite("payload_format", &blazerules_io::StreamRunConfig::payload_format)
        .def_readwrite("avro_schema_json", &blazerules_io::StreamRunConfig::avro_schema_json)
        .def_readwrite("protobuf_descriptor_set", &blazerules_io::StreamRunConfig::protobuf_descriptor_set)
        .def_readwrite("protobuf_message_type", &blazerules_io::StreamRunConfig::protobuf_message_type)
        .def_readwrite("debezium_op_field", &blazerules_io::StreamRunConfig::debezium_op_field);

    py::class_<blazerules_io::StreamRunStats>(m, "StreamRunStats")
        .def_readonly("batches", &blazerules_io::StreamRunStats::batches)
        .def_readonly("messages", &blazerules_io::StreamRunStats::messages)
        .def_readonly("matched", &blazerules_io::StreamRunStats::matched)
        .def_readonly("emitted", &blazerules_io::StreamRunStats::emitted)
        .def_readonly("eval_us", &blazerules_io::StreamRunStats::eval_us)
        .def_readonly("delivery_errors", &blazerules_io::StreamRunStats::delivery_errors)
        .def_readonly("dlq_routed", &blazerules_io::StreamRunStats::dlq_routed);

    m.def("run_stream",
          [](RuleEngine& engine, const blazerules_io::StreamRunConfig& config) {
              py::gil_scoped_release release;
              return blazerules_io::run_stream(engine, config);
          },
          py::arg("engine"), py::arg("config"),
          "Run a C++-owned Kafka microbatch loop using JSON, Arrow IPC, Avro, Protobuf, or Debezium payloads.");
    m.attr("has_kafka") = true;
#else
    m.attr("has_kafka") = false;
#endif
}
