#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>

#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include <arrow/api.h>
#include <arrow/c/abi.h>
#include <arrow/c/bridge.h>

#include "blazerules/bitmask.h"
#include "blazerules/engine.h"
#include "blazerules/resource_resolver.h"
#include "blazerules/simd_kernels.h"
#include "blazerules/version.h"

namespace py = pybind11;

namespace {

py::object& blazerules_error_type() {
    static py::object type;
    return type;
}

py::object& blazerules_config_error_type() {
    static py::object type;
    return type;
}

py::object& blazerules_parse_error_type() {
    static py::object type;
    return type;
}

py::object& blazerules_schema_error_type() {
    static py::object type;
    return type;
}

void raise_blazerules_error(const BlazeRulesError& error, py::object type) {
    py::object instance = type(error.message);
    instance.attr("code") = blazerules_error_code_name(error.code);
    instance.attr("domain") = blazerules_error_domain_name(error.domain);
    instance.attr("message") = error.message;
    instance.attr("source") = error.source;
    instance.attr("rule_id") = error.rule_id;
    instance.attr("field") = error.field;
    instance.attr("line") = error.line;
    instance.attr("row_index") = error.row_index;
    instance.attr("column_name") = error.column_name;
    instance.attr("cause") = error.cause;
    PyErr_SetObject(type.ptr(), instance.ptr());
}

void raise_blazerules_exception(const BlazeRulesException& exc, py::object type) {
    raise_blazerules_error(exc.error(), std::move(type));
}

[[noreturn]] void throw_python_blazerules_exception(const BlazeRulesException& exc) {
    if (dynamic_cast<const BlazeRulesConfigError*>(&exc)) {
        raise_blazerules_exception(exc, blazerules_config_error_type());
    } else if (dynamic_cast<const BlazeRulesParseError*>(&exc)) {
        raise_blazerules_exception(exc, blazerules_parse_error_type());
    } else if (dynamic_cast<const BlazeRulesSchemaError*>(&exc)) {
        raise_blazerules_exception(exc, blazerules_schema_error_type());
    } else {
        raise_blazerules_exception(exc, blazerules_error_type());
    }
    throw py::error_already_set();
}

py::object python_error_type_for(const BlazeRulesError& error) {
    switch (error.domain) {
        case BlazeRulesError::Domain::CONFIG:
        case BlazeRulesError::Domain::WINDOW_STATE:
            return blazerules_config_error_type();
        case BlazeRulesError::Domain::RULE_PARSE:
        case BlazeRulesError::Domain::INGEST:
            return blazerules_parse_error_type();
        case BlazeRulesError::Domain::RULE_VALIDATION:
        case BlazeRulesError::Domain::SCHEMA:
        case BlazeRulesError::Domain::LOOKUP:
        case BlazeRulesError::Domain::ARROW:
        case BlazeRulesError::Domain::PARQUET:
            return blazerules_schema_error_type();
        case BlazeRulesError::Domain::INTERNAL:
            return blazerules_error_type();
    }
    return blazerules_error_type();
}

BlazeRulesError::Code code_from_name(const std::string& code) {
    if (code == "YAML_SYNTAX_ERROR") return BlazeRulesError::YAML_SYNTAX_ERROR;
    if (code == "MISSING_REQUIRED_FIELD") return BlazeRulesError::MISSING_REQUIRED_FIELD;
    if (code == "UNKNOWN_FIELD_NAME") return BlazeRulesError::UNKNOWN_FIELD_NAME;
    if (code == "TYPE_MISMATCH") return BlazeRulesError::TYPE_MISMATCH;
    if (code == "DUPLICATE_RULE_ID") return BlazeRulesError::DUPLICATE_RULE_ID;
    if (code == "SCHEMA_MISMATCH") return BlazeRulesError::SCHEMA_MISMATCH;
    if (code == "INCOMPATIBLE_PARQUET_SCHEMA") return BlazeRulesError::INCOMPATIBLE_PARQUET_SCHEMA;
    if (code == "MALFORMED_JSON") return BlazeRulesError::MALFORMED_JSON;
    if (code == "FIELD_TYPE_COERCION_FAILED") return BlazeRulesError::FIELD_TYPE_COERCION_FAILED;
    if (code == "INVALID_ENGINE_CONFIG") return BlazeRulesError::INVALID_ENGINE_CONFIG;
    if (code == "UNKNOWN_OP") return BlazeRulesError::UNKNOWN_OP;
    if (code == "UNKNOWN_ACTION") return BlazeRulesError::UNKNOWN_ACTION;
    if (code == "UNKNOWN_SEVERITY") return BlazeRulesError::UNKNOWN_SEVERITY;
    if (code == "FILE_IO_ERROR") return BlazeRulesError::FILE_IO_ERROR;
    if (code == "HOT_RELOAD_FAILED") return BlazeRulesError::HOT_RELOAD_FAILED;
    if (code == "DEAD_LETTER_WRITE_FAILED") return BlazeRulesError::DEAD_LETTER_WRITE_FAILED;
    return BlazeRulesError::INTERNAL_ERROR;
}

BlazeRulesError infer_rule_error_from_message(const std::exception& exc) {
    std::string message = exc.what();
    BlazeRulesError error{BlazeRulesError::YAML_SYNTAX_ERROR, message, "rules", "",
                   -1, BlazeRulesError::Domain::RULE_PARSE};
    if (message.find("unknown field") != std::string::npos) {
        error.code = BlazeRulesError::UNKNOWN_FIELD_NAME;
        error.domain = BlazeRulesError::Domain::RULE_VALIDATION;
    } else if (message.find("type") != std::string::npos ||
               message.find("operator requires") != std::string::npos) {
        error.code = BlazeRulesError::TYPE_MISMATCH;
        error.domain = BlazeRulesError::Domain::RULE_VALIDATION;
    } else if (message.find("duplicate rule id") != std::string::npos) {
        error.code = BlazeRulesError::DUPLICATE_RULE_ID;
        error.domain = BlazeRulesError::Domain::RULE_VALIDATION;
    } else if (message.find("regex") != std::string::npos ||
               message.find("unknown op") != std::string::npos) {
        error.code = BlazeRulesError::UNKNOWN_OP;
        error.domain = BlazeRulesError::Domain::RULE_VALIDATION;
    } else if (message.find("lookup") != std::string::npos) {
        error.code = BlazeRulesError::MISSING_REQUIRED_FIELD;
        error.domain = BlazeRulesError::Domain::LOOKUP;
    } else if (message.find("unknown action") != std::string::npos) {
        error.code = BlazeRulesError::UNKNOWN_ACTION;
    } else if (message.find("unknown severity") != std::string::npos) {
        error.code = BlazeRulesError::UNKNOWN_SEVERITY;
    }
    return error;
}

[[noreturn]] void throw_python_std_exception(const std::exception& exc,
                                             BlazeRulesError::Code code,
                                             BlazeRulesError::Domain domain,
                                             std::string source) {
    BlazeRulesError error{code, exc.what(), std::move(source), "", -1, domain};
    py::object type = python_error_type_for(error);
    raise_blazerules_error(error, std::move(type));
    throw py::error_already_set();
}

template <typename F>
decltype(auto) call_blazerules(F&& f,
                               BlazeRulesError::Code fallback_code = BlazeRulesError::INTERNAL_ERROR,
                               BlazeRulesError::Domain fallback_domain = BlazeRulesError::Domain::INTERNAL,
                               std::string fallback_source = "cpp") {
    using R = decltype(f());
    try {
        if constexpr (std::is_void_v<R>) {
            f();
            return;
        } else {
            return f();
        }
    } catch (const BlazeRulesException& exc) {
        throw_python_blazerules_exception(exc);
    } catch (const std::exception& exc) {
        throw_python_std_exception(exc, fallback_code, fallback_domain,
                                   std::move(fallback_source));
    }
}

// Fully validate a batch that crossed the FFI boundary before the engine touches
// its raw buffers. ValidateFull() checks offset buffers (length n+1, monotonic,
// in-bounds) and UTF-8, so a malformed/corrupt batch is rejected here with a clean
// exception instead of causing an out-of-bounds read deep in the hot path
// (e.g. the string-offset fast path in dict_encoder). Runs once per batch, not per row.
std::shared_ptr<arrow::RecordBatch> validate_imported_batch(
        arrow::Result<std::shared_ptr<arrow::RecordBatch>> imported) {
    if (!imported.ok()) {
        throw std::runtime_error("failed to import Arrow record batch: " +
                                 imported.status().ToString());
    }
    std::shared_ptr<arrow::RecordBatch> batch = imported.MoveValueUnsafe();
    arrow::Status status = batch->ValidateFull();
    if (!status.ok()) {
        throw std::runtime_error("invalid Arrow record batch: " + status.ToString());
    }
    return batch;
}

std::shared_ptr<arrow::RecordBatch> import_record_batch(py::object batch) {
    if (py::hasattr(batch, "__arrow_c_array__")) {
        py::object exported = batch.attr("__arrow_c_array__")();
        py::tuple pair = py::reinterpret_borrow<py::tuple>(exported);
        auto* c_schema = reinterpret_cast<ArrowSchema*>(
            PyCapsule_GetPointer(pair[0].ptr(), "arrow_schema"));
        auto* c_array = reinterpret_cast<ArrowArray*>(
            PyCapsule_GetPointer(pair[1].ptr(), "arrow_array"));
        return validate_imported_batch(arrow::ImportRecordBatch(c_array, c_schema));
    }

    ArrowArray c_array;
    ArrowSchema c_schema;
    batch.attr("_export_to_c")(reinterpret_cast<uintptr_t>(&c_array),
                               reinterpret_cast<uintptr_t>(&c_schema));
    return validate_imported_batch(arrow::ImportRecordBatch(&c_array, &c_schema));
}

py::array_t<bool> bitmask_to_bool(const std::vector<uint8_t>& bm, int n) {
    py::array_t<bool> arr(n);
    bool* out = arr.mutable_data();
    for (int i = 0; i < n; ++i) out[i] = (bm[i / 8] >> (i % 8)) & 1;
    return arr;
}

py::array_t<int32_t> copy_int32_array(const std::vector<int32_t>& values) {
    py::array_t<int32_t> arr(values.size());
    if (!values.empty()) {
        std::memcpy(arr.mutable_data(), values.data(), values.size() * sizeof(int32_t));
    }
    return arr;
}

struct BorrowedMessages {
    std::vector<py::object> keepalive;
    std::vector<std::string_view> views;
};

BorrowedMessages borrow_messages_from_python(py::sequence seq) {
    BorrowedMessages out;
    out.keepalive.reserve(seq.size());
    out.views.reserve(seq.size());
    for (py::handle item : seq) {
        Py_ssize_t size = 0;
        const char* data = nullptr;
        if (PyBytes_Check(item.ptr())) {
            data = PyBytes_AsString(item.ptr());
            size = PyBytes_Size(item.ptr());
            out.keepalive.emplace_back(py::reinterpret_borrow<py::object>(item));
        } else {
            data = PyUnicode_AsUTF8AndSize(item.ptr(), &size);
            if (!data) throw py::type_error("messages must contain str or bytes");
            out.keepalive.emplace_back(py::reinterpret_borrow<py::object>(item));
        }
        out.views.emplace_back(data, static_cast<size_t>(size));
    }
    return out;
}

std::vector<std::string> paths_from_python(py::object obj) {
    if (py::isinstance<py::str>(obj)) return {py::str(obj).cast<std::string>()};
    std::vector<std::string> out;
    for (py::handle item : py::reinterpret_borrow<py::sequence>(obj)) {
        out.push_back(py::str(item).cast<std::string>());
    }
    return out;
}

} // namespace

PYBIND11_MODULE(blazerules, m) {
    m.doc() = "BLAZERULES - vectorized decision engine (blazerules)";
    m.attr("__version__") = blazerules::VERSION;
    m.attr("BLAZERULES_VERSION") = blazerules::VERSION;
    m.attr("RULE_YAML_COMPATIBILITY") = blazerules::RULE_YAML_COMPATIBILITY;

    py::enum_<SimdBackend>(m, "SimdBackend")
        .value("SCALAR", SimdBackend::SCALAR)
        .value("NEON", SimdBackend::NEON)
        .value("SSE2", SimdBackend::SSE2)
        .value("AVX2", SimdBackend::AVX2)
        .value("AVX512", SimdBackend::AVX512)
        .export_values();

    py::class_<CpuFeatures>(m, "CpuFeatures")
        .def_readonly("x86_64", &CpuFeatures::x86_64)
        .def_readonly("arm64", &CpuFeatures::arm64)
        .def_readonly("sse2", &CpuFeatures::sse2)
        .def_readonly("avx", &CpuFeatures::avx)
        .def_readonly("avx2", &CpuFeatures::avx2)
        .def_readonly("avx512f", &CpuFeatures::avx512f)
        .def_readonly("avx512bw", &CpuFeatures::avx512bw)
        .def_readonly("avx512vl", &CpuFeatures::avx512vl)
        .def_readonly("avx512dq", &CpuFeatures::avx512dq)
        .def_readonly("avx512vpopcntdq", &CpuFeatures::avx512vpopcntdq)
        .def_readonly("fma", &CpuFeatures::fma)
        .def_readonly("bmi2", &CpuFeatures::bmi2)
        .def_readonly("popcnt", &CpuFeatures::popcnt)
        .def_readonly("lzcnt", &CpuFeatures::lzcnt)
        .def_readonly("neon", &CpuFeatures::neon)
        .def_readonly("os_avx", &CpuFeatures::os_avx)
        .def_readonly("os_avx512", &CpuFeatures::os_avx512)
        .def_readonly("arch", &CpuFeatures::arch)
        .def_readonly("compiler", &CpuFeatures::compiler)
        .def_readonly("os", &CpuFeatures::os);

    m.def("simd_backend", [] { return std::string(simd_backend_name()); },
          "Return the selected SIMD backend name for this process.");
    m.def("simd_backend_enum", &simd_backend,
          "Return the selected SIMD backend enum for this process.");
    m.def("cpu_features", [] { return cpu_features(); },
          "Return detected CPU/OS SIMD feature flags.");
    m.def("cpu_features_summary", &cpu_features_summary,
          "Return a compact CPU/SIMD diagnostics string.");

    m.def("set_aws_profile", &blazerules::set_aws_profile,
          py::arg("profile"), py::arg("clear_env_credentials") = true,
          "Use an AWS CLI profile for s3:// rules, lookups, models, and files. "
          "When clear_env_credentials is true, stale AWS_ACCESS_KEY_ID-style env vars are ignored.");
    m.def("clear_aws_profile", &blazerules::clear_aws_profile,
          "Clear the explicit AWS profile set for BlazeRules S3 resolution.");
    m.def("current_aws_profile", &blazerules::current_aws_profile,
          "Return BLAZERULES_AWS_PROFILE or AWS_PROFILE if set.");
    m.def("set_aws_region", &blazerules::set_aws_region,
          py::arg("region"),
          "Set the AWS region for s3:// resolution. Also updates AWS_REGION/AWS_DEFAULT_REGION.");
    m.def("clear_aws_region", &blazerules::clear_aws_region,
          "Clear the explicit AWS region for S3 resolution.");
    m.def("current_aws_region", &blazerules::current_aws_region,
          "Return BLAZERULES_AWS_REGION, AWS_REGION, or AWS_DEFAULT_REGION if set.");
    m.def("set_aws_endpoint_url", &blazerules::set_aws_endpoint_url,
          py::arg("endpoint_url"),
          "Set a custom S3 endpoint URL for AWS CLI based s3:// resolution, e.g. MinIO/R2/LocalStack.");
    m.def("clear_aws_endpoint_url", &blazerules::clear_aws_endpoint_url,
          "Clear the custom S3 endpoint URL.");
    m.def("current_aws_endpoint_url", &blazerules::current_aws_endpoint_url,
          "Return BLAZERULES_AWS_ENDPOINT_URL, AWS_ENDPOINT_URL, or AWS_ENDPOINT_URL_S3 if set.");
    m.def("set_aws_credentials", &blazerules::set_aws_credentials,
          py::arg("access_key_id"), py::arg("secret_access_key"),
          py::arg("session_token") = "", py::arg("region") = "",
          "Set process environment credentials for AWS CLI based s3:// resolution. "
          "Prefer set_aws_profile for long-running production processes.");
    m.def("clear_aws_credentials", &blazerules::clear_aws_credentials,
          "Clear AWS_ACCESS_KEY_ID/AWS_SECRET_ACCESS_KEY/AWS_SESSION_TOKEN for this process.");

    auto base_error = py::register_exception<BlazeRulesException>(m, "BlazeRulesError");
    py::register_exception<BlazeRulesConfigError>(m, "BlazeRulesConfigError", base_error.ptr());
    py::register_exception<BlazeRulesParseError>(m, "BlazeRulesParseError", base_error.ptr());
    py::register_exception<BlazeRulesSchemaError>(m, "BlazeRulesSchemaError", base_error.ptr());
    blazerules_error_type() = py::reinterpret_borrow<py::object>(base_error.ptr());
    blazerules_config_error_type() = m.attr("BlazeRulesConfigError");
    blazerules_parse_error_type() = m.attr("BlazeRulesParseError");
    blazerules_schema_error_type() = m.attr("BlazeRulesSchemaError");
    py::register_exception_translator([](std::exception_ptr p) {
        if (!p) return;
        try {
            std::rethrow_exception(p);
        } catch (const BlazeRulesConfigError& e) {
            raise_blazerules_exception(e, blazerules_config_error_type());
        } catch (const BlazeRulesParseError& e) {
            raise_blazerules_exception(e, blazerules_parse_error_type());
        } catch (const BlazeRulesSchemaError& e) {
            raise_blazerules_exception(e, blazerules_schema_error_type());
        } catch (const BlazeRulesException& e) {
            raise_blazerules_exception(e, blazerules_error_type());
        }
    });

    py::enum_<ColumnType>(m, "ColumnType")
        .value("FLOAT32", ColumnType::FLOAT32)
        .value("FLOAT64", ColumnType::FLOAT64)
        .value("INT32", ColumnType::INT32)
        .value("INT64", ColumnType::INT64)
        .value("CATEGORICAL", ColumnType::CATEGORICAL)
        .value("ENTITY_KEY", ColumnType::ENTITY_KEY)
        .value("TIMESTAMP_MS", ColumnType::TIMESTAMP_MS)
        .value("BOOLEAN", ColumnType::BOOLEAN)
        .value("STRING", ColumnType::STRING)
        .export_values();

    py::enum_<ActionType>(m, "ActionType")
        .value("FLAG", ActionType::FLAG)
        .value("BLOCK", ActionType::BLOCK)
        .value("SCORE", ActionType::SCORE)
        .value("REVIEW", ActionType::REVIEW)
        .value("APPROVE", ActionType::APPROVE)
        .export_values();

    py::enum_<RuleFileFormat>(m, "RuleFileFormat")
        .value("YAML", RuleFileFormat::YAML)
        .value("JSON", RuleFileFormat::JSON)
        .export_values();

    py::enum_<EngineConfig::OutputDetail>(m, "OutputDetail")
        .value("COUNTS", EngineConfig::OUTPUT_COUNTS)
        .value("CODES", EngineConfig::OUTPUT_CODES)
        .value("DECISIONS", EngineConfig::OUTPUT_DECISIONS)
        .value("BITMASKS", EngineConfig::OUTPUT_BITMASKS)
        .export_values();

    py::enum_<EngineConfig::IngestErrorMode>(m, "IngestErrorMode")
        .value("SKIP_AND_COUNT", EngineConfig::SKIP_AND_COUNT)
        .value("SKIP_TO_DEAD_LETTER", EngineConfig::SKIP_TO_DEAD_LETTER)
        .value("HARD_FAIL", EngineConfig::HARD_FAIL)
        .export_values();

    py::enum_<EngineConfig::TypeMismatchMode>(m, "TypeMismatchMode")
        .value("NULL_ON_TYPE_ERROR", EngineConfig::NULL_ON_TYPE_ERROR)
        .value("COERCE", EngineConfig::COERCE)
        .value("HARD_FAIL_TYPE", EngineConfig::HARD_FAIL_TYPE)
        .export_values();

    py::class_<FieldSpec>(m, "FieldSpec")
        .def(py::init<>())
        .def_readwrite("name", &FieldSpec::name)
        .def_readwrite("type", &FieldSpec::type)
        .def_readwrite("nullable", &FieldSpec::nullable)
        .def_readwrite("is_entity_field", &FieldSpec::is_entity_field);

    m.def("Field",
          [](std::string name, ColumnType type, bool nullable) {
              FieldSpec f;
              f.name = std::move(name);
              f.type = type;
              f.nullable = nullable;
              f.is_entity_field = (type == ColumnType::ENTITY_KEY);
              return f;
          },
          py::arg("name"), py::arg("type"), py::arg("nullable") = true);

    py::class_<EngineConfig>(m, "EngineConfig")
        .def(py::init<>())
        .def_readwrite("batch_size", &EngineConfig::batch_size)
        .def_readwrite("parallel_threshold", &EngineConfig::parallel_threshold)
        .def_readwrite("eval_thread_count", &EngineConfig::eval_thread_count)
        .def_readwrite("model_intra_op_threads", &EngineConfig::model_intra_op_threads)
        .def_readwrite("trace_sample_rate", &EngineConfig::trace_sample_rate)
        .def_readwrite("output_detail", &EngineConfig::output_detail)
        .def_readwrite("max_window_entities", &EngineConfig::max_window_entities)
        .def_readwrite("arena_size_bytes", &EngineConfig::arena_size_bytes)
        .def_readwrite("max_dict_size_per_column", &EngineConfig::max_dict_size_per_column)
        .def_readwrite("ingest_error_mode", &EngineConfig::ingest_error_mode)
        .def_readwrite("type_mismatch_mode", &EngineConfig::type_mismatch_mode)
        .def_readwrite("enable_selection_vectors", &EngineConfig::enable_selection_vectors)
        .def_readwrite("selection_vector_threshold", &EngineConfig::selection_vector_threshold)
        .def_readwrite("enable_adaptive_predicate_ordering", &EngineConfig::enable_adaptive_predicate_ordering)
        .def_readwrite("enable_no_validity_fast_path", &EngineConfig::enable_no_validity_fast_path)
        .def_readwrite("enable_prefetch", &EngineConfig::enable_prefetch)
        .def_readwrite("enable_thread_affinity", &EngineConfig::enable_thread_affinity)
        .def_readwrite("result_buffer_reuse", &EngineConfig::result_buffer_reuse)
        .def_readwrite("simd_backend_override", &EngineConfig::simd_backend_override)
        .def_readwrite("enable_avx512", &EngineConfig::enable_avx512)
        .def_readwrite("hot_reload_poll_seconds", &EngineConfig::hot_reload_poll_seconds)
        .def_readwrite("hot_reload_validate_conflicts", &EngineConfig::hot_reload_validate_conflicts)
        .def_readwrite("hot_reload_keep_previous_on_failure", &EngineConfig::hot_reload_keep_previous_on_failure)
        .def_readwrite("max_error_samples", &EngineConfig::max_error_samples)
        .def_readwrite("decision_log_path", &EngineConfig::decision_log_path)
        .def_readwrite("dead_letter_path", &EngineConfig::dead_letter_path);

    py::class_<ConflictReport>(m, "ConflictReport")
        .def("has_issues", &ConflictReport::has_any_issues)
        .def_readonly("total_rules_analyzed", &ConflictReport::total_rules_analyzed)
        .def_property_readonly("num_conflicts", [](const ConflictReport& r) { return r.conflicts.size(); })
        .def_property_readonly("num_subsumptions", [](const ConflictReport& r) { return r.subsumptions.size(); })
        .def_property_readonly("num_dead_rules", [](const ConflictReport& r) { return r.dead_rules.size(); })
        .def("__repr__", &ConflictReport::to_string)
        .def("__str__", &ConflictReport::to_string);

    py::class_<BatchErrorSample>(m, "BatchErrorSample")
        .def_readonly("code", &BatchErrorSample::code)
        .def_readonly("message", &BatchErrorSample::message)
        .def_readonly("source", &BatchErrorSample::source)
        .def_readonly("row_index", &BatchErrorSample::row_index)
        .def_readonly("column_name", &BatchErrorSample::column_name);

    py::class_<BatchResult>(m, "BatchResult")
        .def_property_readonly("n_records", [](const BatchResult& r) { return r.n_records; })
        .def_property_readonly("n_matched", [](const BatchResult& r) { return r.n_matched; })
        .def("__getitem__",
             [](const BatchResult& r, const std::string& rule_id) {
                 auto it = r.rule_bitmasks.find(rule_id);
                 if (it == r.rule_bitmasks.end()) throw py::key_error(rule_id);
                 return bitmask_to_bool(it->second, r.n_records);
             })
        .def_property_readonly("matched_indices",
             [](const BatchResult& r) {
                 return py::array_t<int32_t>(r.matched_record_indices.size(),
                                             r.matched_record_indices.data());
             })
        .def_property_readonly("decisions",
             [](const BatchResult& r) { return r.decisions; })
        .def_property_readonly("decision_codes",
             [](const BatchResult& r) {
                 return py::array_t<int32_t>(r.decision_codes.size(), r.decision_codes.data());
             })
        .def_property_readonly("decision_label_map",
             [](const BatchResult& r) {
                 py::dict out;
                 for (size_t i = 0; i < r.decision_labels.size(); ++i) {
                     out[py::int_(static_cast<int>(i))] = r.decision_labels[i];
                 }
                 return out;
             })
        .def_property_readonly("scores",
             [](const BatchResult& r) { return r.scores; })
        .def_property_readonly("risk_bands",
             [](const BatchResult& r) { return r.risk_bands; })
        .def_property_readonly("winning_rule_ids",
             [](const BatchResult& r) { return r.winning_rule_ids; })
        .def("rule_bitmask_buffer",
             [](BatchResult& r, const std::string& rule_id) -> py::object {
                 auto it = r.rule_bitmasks.find(rule_id);
                 if (it == r.rule_bitmasks.end()) throw py::key_error(rule_id);
                 if (it->second.empty()) return py::none();
                 return py::memoryview::from_memory(it->second.data(), it->second.size());
             },
             py::arg("rule_id"), py::keep_alive<0, 1>())
        .def("matched_indices_buffer",
             [](BatchResult& r) -> py::object {
                 if (r.matched_record_indices.empty()) return py::none();
                 auto* ptr = reinterpret_cast<uint8_t*>(r.matched_record_indices.data());
                 return py::memoryview::from_memory(
                     ptr, r.matched_record_indices.size() * sizeof(int32_t));
             }, py::keep_alive<0, 1>())
        .def("decision_codes_buffer",
             [](BatchResult& r) -> py::object {
                 if (r.decision_codes.empty()) return py::none();
                 auto* ptr = reinterpret_cast<uint8_t*>(r.decision_codes.data());
                 return py::memoryview::from_memory(
                     ptr, r.decision_codes.size() * sizeof(int32_t));
             }, py::keep_alive<0, 1>())
        .def("indices_for_decision",
             [](const BatchResult& r, const std::string& decision) {
                 auto it = r.grouped_decision_indices.find(decision);
                 if (it == r.grouped_decision_indices.end()) {
                     return py::array_t<int32_t>(0);
                 }
                 return py::array_t<int32_t>(it->second.size(), it->second.data());
             },
             py::arg("decision"))
        .def("indices_for_not_decision",
             [](const BatchResult& r, const std::string& decision) {
                 std::vector<int32_t> out;
                 out.reserve(static_cast<size_t>(r.n_records));
                 for (const auto& [label, indices] : r.grouped_decision_indices) {
                     if (label == decision) continue;
                     out.insert(out.end(), indices.begin(), indices.end());
                 }
                 return copy_int32_array(out);
             },
             py::arg("decision"))
        .def("grouped_decision_indices",
             [](const BatchResult& r) {
                 py::dict out;
                 for (const auto& [label, indices] : r.grouped_decision_indices) {
                     out[py::str(label)] = py::array_t<int32_t>(indices.size(), indices.data());
                 }
                 return out;
             })
        .def("indices_for_rule",
             [](const BatchResult& r, const std::string& rule_id) {
                 auto it = r.rule_bitmasks.find(rule_id);
                 if (it == r.rule_bitmasks.end()) throw py::key_error(rule_id);
                 std::vector<int32_t> out;
                 blazerules::find_set_bits(it->second.data(), r.n_records, out);
                 return copy_int32_array(out);
             },
             py::arg("rule_id"))
        .def("grouped_winning_rule_indices",
             [](const BatchResult& r) {
                 py::dict out;
                 for (const auto& [rule_id, indices] : r.grouped_winning_rule_indices) {
                     out[py::str(rule_id)] = py::array_t<int32_t>(indices.size(), indices.data());
                 }
                 return out;
             })
        .def_property_readonly("match_counts",
             [](const BatchResult& r) {
                 std::map<std::string, int> out;
                 for (const auto& [k, v] : r.rule_match_counts) out[k] = v;
                 return out;
             })
        .def_property_readonly("explanations",
             [](const BatchResult&) { return py::dict(); })
        .def_property_readonly("timing",
             [](const BatchResult& r) { return r.timing_ms(); })
        .def_readonly("messages_processed", &BatchResult::messages_processed)
        .def_readonly("messages_skipped", &BatchResult::messages_skipped)
        .def_readonly("last_ingest_error", &BatchResult::last_ingest_error)
        .def_property_readonly("error_counts",
             [](const BatchResult& r) {
                 std::map<std::string, int> out;
                 for (const auto& [k, v] : r.error_counts) out[k] = v;
                 return out;
             })
        .def_property_readonly("model_names",
             [](const BatchResult& r) {
                 std::vector<std::string> names;
                 std::unordered_map<std::string, int> seen;
                 for (const auto& mo : r.model_outputs) {
                     int occ = seen[mo.model_name]++;
                     std::string label = "model." + mo.model_name;
                     if (occ > 0) label += "#" + std::to_string(occ);
                     names.push_back(label);
                 }
                 return names;
             },
             "Model channel labels (model.<name>), matching the agent's Arrow columns / NDJSON model_scores keys.")
        .def_property_readonly("model_scores",
             [](const BatchResult& r) {
                 py::dict out;
                 std::unordered_map<std::string, int> seen;
                 for (const auto& mo : r.model_outputs) {
                     int occ = seen[mo.model_name]++;
                     std::string label = "model." + mo.model_name;
                     if (occ > 0) label += "#" + std::to_string(occ);
                     out[py::str(label)] = py::array_t<float>(
                         static_cast<py::ssize_t>(mo.values.size()), mo.values.data());
                 }
                 return out;
             },
             "Dict of model label -> per-record prediction ndarray (float32).")
        .def("model_scores_buffer",
             [](BatchResult& r, const std::string& model_name) -> py::object {
                 std::unordered_map<std::string, int> seen;
                 for (auto& mo : r.model_outputs) {
                     int occ = seen[mo.model_name]++;
                     std::string label = "model." + mo.model_name;
                     if (occ > 0) label += "#" + std::to_string(occ);
                     if (label == model_name || mo.model_name == model_name) {
                         if (mo.values.empty()) return py::none();
                         return py::memoryview::from_memory(
                             reinterpret_cast<uint8_t*>(mo.values.data()),
                             static_cast<py::ssize_t>(mo.values.size() * sizeof(float)));
                     }
                 }
                 throw py::key_error(model_name);
             },
             py::arg("model_name"), py::keep_alive<0, 1>(),
             "Zero-copy float32 memoryview of one model's per-record predictions (np.frombuffer(..., np.float32)).")
        .def_readonly("error_samples", &BatchResult::error_samples);

    py::class_<BacktestReport>(m, "BacktestReport")
        .def_readonly("rule_set_a_name", &BacktestReport::rule_set_a_name)
        .def_readonly("rule_set_b_name", &BacktestReport::rule_set_b_name)
        .def_readonly("total_records", &BacktestReport::total_records)
        .def_readonly("fire_rate_a", &BacktestReport::fire_rate_a)
        .def_readonly("fire_rate_b", &BacktestReport::fire_rate_b)
        .def_readonly("new_positives", &BacktestReport::new_positives)
        .def_readonly("lost_positives", &BacktestReport::lost_positives)
        .def_readonly("agreement_rate", &BacktestReport::agreement_rate)
        .def_readonly("precision_a", &BacktestReport::precision_a)
        .def_readonly("recall_a", &BacktestReport::recall_a)
        .def_readonly("precision_b", &BacktestReport::precision_b)
        .def_readonly("recall_b", &BacktestReport::recall_b);

    py::class_<HotReloadStatus>(m, "HotReloadStatus")
        .def_readonly("active_version", &HotReloadStatus::active_version)
        .def_readonly("pending_path", &HotReloadStatus::pending_path)
        .def_readonly("last_attempt_ms", &HotReloadStatus::last_attempt_ms)
        .def_readonly("last_success_ms", &HotReloadStatus::last_success_ms)
        .def_readonly("reload_count", &HotReloadStatus::reload_count)
        .def_readonly("failed_reload_count", &HotReloadStatus::failed_reload_count)
        .def_readonly("last_error_code", &HotReloadStatus::last_error_code)
        .def_readonly("last_error_message", &HotReloadStatus::last_error_message);

    py::enum_<RuleEngine::SchemaState>(m, "SchemaState")
        .value("UNBOUND", RuleEngine::SchemaState::UNBOUND)
        .value("INFERRED_BOUND", RuleEngine::SchemaState::INFERRED_BOUND)
        .value("USER_BOUND", RuleEngine::SchemaState::USER_BOUND)
        .export_values();

    py::class_<RuleEngine>(m, "RuleEngine")
        .def(py::init([](EngineConfig config) {
                 return call_blazerules([&]() {
                     return std::make_unique<RuleEngine>(config);
                 }, BlazeRulesError::INVALID_ENGINE_CONFIG, BlazeRulesError::Domain::CONFIG, "engine");
             }),
             py::arg("config") = EngineConfig{})
        .def(py::init([](std::vector<FieldSpec> schema, EngineConfig config) {
                 return call_blazerules([&]() {
                     return std::make_unique<RuleEngine>(std::move(schema), config);
                 }, BlazeRulesError::INVALID_ENGINE_CONFIG, BlazeRulesError::Domain::CONFIG, "engine");
             }),
             py::arg("schema"), py::arg("config") = EngineConfig{})
        .def("load_rules",
             [](RuleEngine& e, const std::string& rules_path) {
                 try {
                     return e.load_rules(rules_path);
                 } catch (const BlazeRulesException& exc) {
                     throw_python_blazerules_exception(exc);
                 } catch (const std::exception& exc) {
                     HotReloadStatus status = e.hot_reload_status();
                     BlazeRulesError error{code_from_name(status.last_error_code), exc.what(),
                                    "rules", "", -1, BlazeRulesError::Domain::RULE_VALIDATION};
                     raise_blazerules_error(error, python_error_type_for(error));
                     throw py::error_already_set();
                 }
             },
             py::arg("rules_path"))
        .def("reload_rules_now",
             [](RuleEngine& e, const std::string& rules_path) {
                 try {
                     return e.reload_rules_now(rules_path);
                 } catch (const BlazeRulesException& exc) {
                     throw_python_blazerules_exception(exc);
                 } catch (const std::exception& exc) {
                     HotReloadStatus status = e.hot_reload_status();
                     BlazeRulesError error{code_from_name(status.last_error_code), exc.what(),
                                    "rules", "", -1, BlazeRulesError::Domain::RULE_VALIDATION};
                     raise_blazerules_error(error, python_error_type_for(error));
                     throw py::error_already_set();
                 }
             },
             py::arg("rules_path"))
        .def("load_rules_from_string",
             [](RuleEngine& e, const std::string& rules_yaml_or_json, RuleFileFormat format) {
                 try {
                     return e.load_rules_from_string(rules_yaml_or_json, format);
                 } catch (const BlazeRulesException& exc) {
                     throw_python_blazerules_exception(exc);
                 } catch (const std::exception& exc) {
                     BlazeRulesError error = infer_rule_error_from_message(exc);
                     raise_blazerules_error(error, python_error_type_for(error));
                     throw py::error_already_set();
                 }
             },
             py::arg("rules_yaml_or_json"),
             py::arg("format") = RuleFileFormat::YAML)
        .def("analyze_conflicts",
             [](RuleEngine& e, const std::string& rules_path) {
                 return call_blazerules([&]() { return e.analyze_conflicts(rules_path); },
                                 BlazeRulesError::YAML_SYNTAX_ERROR, BlazeRulesError::Domain::RULE_PARSE, "rules");
             },
             py::arg("rules_path"))
        .def("enable_hot_reload",
             [](RuleEngine& e, const std::string& rules_path, int poll_interval_seconds) {
                 call_blazerules([&]() {
                     e.enable_hot_reload(rules_path, std::chrono::seconds(poll_interval_seconds));
                 }, BlazeRulesError::HOT_RELOAD_FAILED, BlazeRulesError::Domain::CONFIG, "hot_reload");
             },
             py::arg("rules_path"), py::arg("poll_interval_seconds") = 5)
        .def("stop_hot_reload", &RuleEngine::stop_hot_reload)
        .def("hot_reload_status", &RuleEngine::hot_reload_status)
        .def_property_readonly("schema_bound", &RuleEngine::schema_bound)
        .def_property_readonly("schema_state", &RuleEngine::schema_state)
        .def_property_readonly("schema",
             [](const RuleEngine& e) {
                 return e.schema().all_fields();
             })
        .def("evaluate_messages",
             [](RuleEngine& e, py::sequence messages) {
                 auto borrowed = borrow_messages_from_python(messages);
                 try {
                     py::gil_scoped_release release;
                     return e.evaluate_message_views(borrowed.views);
                 } catch (const BlazeRulesException& exc) {
                     throw_python_blazerules_exception(exc);
                 } catch (const std::exception& exc) {
                     throw_python_std_exception(exc, BlazeRulesError::MALFORMED_JSON,
                                                BlazeRulesError::Domain::INGEST, "ingest");
                 }
             },
             py::arg("messages"))
        .def("evaluate_ndjson",
             [](RuleEngine& e, py::object payload) {
                 Py_buffer view;
                 if (PyObject_GetBuffer(payload.ptr(), &view, PyBUF_CONTIG_RO) != 0) {
                     throw py::type_error("evaluate_ndjson expects bytes-like contiguous input");
                 }
                 std::string_view bytes(static_cast<const char*>(view.buf),
                                        static_cast<size_t>(view.len));
                 try {
                     BatchResult result;
                     {
                         py::gil_scoped_release release;
                         result = e.evaluate_ndjson(bytes);
                     }
                     PyBuffer_Release(&view);
                     return result;
                 } catch (const BlazeRulesException& exc) {
                     PyBuffer_Release(&view);
                     throw_python_blazerules_exception(exc);
                 } catch (const py::error_already_set&) {
                     PyBuffer_Release(&view);
                     throw;
                 } catch (const std::exception& exc) {
                     PyBuffer_Release(&view);
                     throw_python_std_exception(exc, BlazeRulesError::MALFORMED_JSON,
                                                BlazeRulesError::Domain::INGEST, "ingest");
                 } catch (...) {
                     PyBuffer_Release(&view);
                     throw;
                 }
             },
             py::arg("payload"))
        .def("evaluate_ndjson_padded",
             [](RuleEngine& e, py::object payload, size_t logical_size) {
                 Py_buffer view;
                 if (PyObject_GetBuffer(payload.ptr(), &view, PyBUF_CONTIG_RO) != 0) {
                     throw py::type_error("evaluate_ndjson_padded expects bytes-like contiguous input");
                 }
                 if (logical_size > static_cast<size_t>(view.len)) {
                     PyBuffer_Release(&view);
                     throw py::value_error("logical_size exceeds payload length");
                 }
                 std::string_view bytes(static_cast<const char*>(view.buf), logical_size);
                 try {
                     BatchResult result;
                     {
                         py::gil_scoped_release release;
                         result = e.evaluate_ndjson_padded(bytes);
                     }
                     PyBuffer_Release(&view);
                     return result;
                 } catch (const BlazeRulesException& exc) {
                     PyBuffer_Release(&view);
                     throw_python_blazerules_exception(exc);
                 } catch (const py::error_already_set&) {
                     PyBuffer_Release(&view);
                     throw;
                 } catch (const std::exception& exc) {
                     PyBuffer_Release(&view);
                     throw_python_std_exception(exc, BlazeRulesError::MALFORMED_JSON,
                                                BlazeRulesError::Domain::INGEST, "ingest");
                 } catch (...) {
                     PyBuffer_Release(&view);
                     throw;
                 }
             },
             py::arg("payload"), py::arg("logical_size"))
        .def("evaluate_json_array",
             [](RuleEngine& e, py::object payload) {
                 Py_buffer view;
                 if (PyObject_GetBuffer(payload.ptr(), &view, PyBUF_CONTIG_RO) != 0) {
                     throw py::type_error("evaluate_json_array expects bytes-like contiguous input");
                 }
                 std::string_view bytes(static_cast<const char*>(view.buf),
                                        static_cast<size_t>(view.len));
                 try {
                     BatchResult result;
                     {
                         py::gil_scoped_release release;
                         result = e.evaluate_json_array(bytes);
                     }
                     PyBuffer_Release(&view);
                     return result;
                 } catch (const BlazeRulesException& exc) {
                     PyBuffer_Release(&view);
                     throw_python_blazerules_exception(exc);
                 } catch (const py::error_already_set&) {
                     PyBuffer_Release(&view);
                     throw;
                 } catch (const std::exception& exc) {
                     PyBuffer_Release(&view);
                     throw_python_std_exception(exc, BlazeRulesError::MALFORMED_JSON,
                                                BlazeRulesError::Domain::INGEST, "ingest");
                 } catch (...) {
                     PyBuffer_Release(&view);
                     throw;
                 }
             },
             py::arg("payload"))
        .def("evaluate_json_array_padded",
             [](RuleEngine& e, py::object payload, size_t logical_size) {
                 Py_buffer view;
                 if (PyObject_GetBuffer(payload.ptr(), &view, PyBUF_CONTIG_RO) != 0) {
                     throw py::type_error("evaluate_json_array_padded expects bytes-like contiguous input");
                 }
                 if (logical_size > static_cast<size_t>(view.len)) {
                     PyBuffer_Release(&view);
                     throw py::value_error("logical_size exceeds payload length");
                 }
                 std::string_view bytes(static_cast<const char*>(view.buf), logical_size);
                 try {
                     BatchResult result;
                     {
                         py::gil_scoped_release release;
                         result = e.evaluate_json_array_padded(bytes);
                     }
                     PyBuffer_Release(&view);
                     return result;
                 } catch (const BlazeRulesException& exc) {
                     PyBuffer_Release(&view);
                     throw_python_blazerules_exception(exc);
                 } catch (const py::error_already_set&) {
                     PyBuffer_Release(&view);
                     throw;
                 } catch (const std::exception& exc) {
                     PyBuffer_Release(&view);
                     throw_python_std_exception(exc, BlazeRulesError::MALFORMED_JSON,
                                                BlazeRulesError::Domain::INGEST, "ingest");
                 } catch (...) {
                     PyBuffer_Release(&view);
                     throw;
                 }
             },
             py::arg("payload"), py::arg("logical_size"))
        .def("evaluate_batch",
             [](RuleEngine& e, py::object batch) {
                 auto rb = import_record_batch(batch);
                 try {
                     py::gil_scoped_release release;
                     return e.evaluate_record_batch(rb);
                 } catch (const BlazeRulesException& exc) {
                     throw_python_blazerules_exception(exc);
                 } catch (const std::exception& exc) {
                     throw_python_std_exception(exc, BlazeRulesError::SCHEMA_MISMATCH,
                                                BlazeRulesError::Domain::ARROW, "arrow");
                 }
             },
             py::arg("batch"))
        .def("create_shards",
             [](const RuleEngine& e, int shard_count) {
                 py::list out;
                 auto shards = e.create_shards(shard_count);
                 for (auto& shard : shards) out.append(py::cast(std::move(shard)));
                 return out;
             },
             py::arg("shard_count"))
        .def("evaluate_partition_messages",
             [](RuleEngine& e, int partition_id, py::sequence messages) {
                 auto borrowed = borrow_messages_from_python(messages);
                 try {
                     py::gil_scoped_release release;
                     return e.evaluate_partition(partition_id, borrowed.views);
                 } catch (const BlazeRulesException& exc) {
                     throw_python_blazerules_exception(exc);
                 } catch (const std::exception& exc) {
                     throw_python_std_exception(exc, BlazeRulesError::MALFORMED_JSON,
                                                BlazeRulesError::Domain::INGEST, "ingest");
                 }
             },
             py::arg("partition_id"), py::arg("messages"))
        .def("evaluate_partition_ndjson_padded",
             [](RuleEngine& e, int partition_id, py::object payload, size_t logical_size) {
                 Py_buffer view;
                 if (PyObject_GetBuffer(payload.ptr(), &view, PyBUF_CONTIG_RO) != 0) {
                     throw py::type_error("evaluate_partition_ndjson_padded expects bytes-like contiguous input");
                 }
                 if (logical_size > static_cast<size_t>(view.len)) {
                     PyBuffer_Release(&view);
                     throw py::value_error("logical_size exceeds payload length");
                 }
                 std::string_view bytes(static_cast<const char*>(view.buf), logical_size);
                 try {
                     BatchResult result;
                     {
                         py::gil_scoped_release release;
                         result = e.evaluate_partition_ndjson_padded(partition_id, bytes);
                     }
                     PyBuffer_Release(&view);
                     return result;
                 } catch (const BlazeRulesException& exc) {
                     PyBuffer_Release(&view);
                     throw_python_blazerules_exception(exc);
                 } catch (const py::error_already_set&) {
                     PyBuffer_Release(&view);
                     throw;
                 } catch (const std::exception& exc) {
                     PyBuffer_Release(&view);
                     throw_python_std_exception(exc, BlazeRulesError::MALFORMED_JSON,
                                                BlazeRulesError::Domain::INGEST, "ingest");
                 } catch (...) {
                     PyBuffer_Release(&view);
                     throw;
                 }
             },
             py::arg("partition_id"), py::arg("payload"), py::arg("logical_size"))
        .def("evaluate_partition_batch",
             [](RuleEngine& e, int partition_id, py::object batch) {
                 auto rb = import_record_batch(batch);
                 try {
                     py::gil_scoped_release release;
                     return e.evaluate_partition(partition_id, rb);
                 } catch (const BlazeRulesException& exc) {
                     throw_python_blazerules_exception(exc);
                 } catch (const std::exception& exc) {
                     throw_python_std_exception(exc, BlazeRulesError::SCHEMA_MISMATCH,
                                                BlazeRulesError::Domain::ARROW, "arrow");
                 }
             },
             py::arg("partition_id"), py::arg("batch"))
        .def("backtest",
             [](RuleEngine& e, py::object parquet_path, std::string rules_a,
                std::string rules_b, py::object label_column) {
                 auto paths = paths_from_python(parquet_path);
                 std::string label;
                 if (!label_column.is_none()) label = py::str(label_column);
                 try {
                     py::gil_scoped_release release;
                     return e.backtest(paths, rules_a, rules_b, label);
                 } catch (const BlazeRulesException& exc) {
                     throw_python_blazerules_exception(exc);
                 } catch (const std::exception& exc) {
                     throw_python_std_exception(exc, BlazeRulesError::INCOMPATIBLE_PARQUET_SCHEMA,
                                                BlazeRulesError::Domain::PARQUET, "parquet");
                 }
             },
             py::arg("parquet_path"), py::arg("rules_a"), py::arg("rules_b"),
             py::arg("label_column") = py::none())
        .def("reset_window_state", &RuleEngine::reset_window_state)
        .def("num_window_channels", &RuleEngine::num_window_channels)
        .def("register_model", &RuleEngine::register_model,
             py::arg("name"), py::arg("path"),
             "Register an ONNX model (the single ML backend) for model_score rules, from a "
             ".onnx file path. XGBoost/LightGBM/scikit-learn/NN all export to ONNX. Requires "
             "an ONNX-enabled build; export classifiers with zipmap=False.")
        .def("num_models", &RuleEngine::num_models)
        .def("model_channel_columns", &RuleEngine::model_channel_columns,
             "List of (model_name, injected_column) for each model_score channel in the loaded ruleset.")
        .def("evaluate_ndjson_file",
             [](RuleEngine& e, const std::string& path) {
                 BatchResult result;
                 {
                     py::gil_scoped_release release;
                     result = e.evaluate_ndjson_file(path);
                 }
                 return result;
             },
             py::arg("path"),
             "mmap an NDJSON file and evaluate it zero-copy (offline/batch replay).")
        .def("enable_metrics", &RuleEngine::enable_metrics,
             "Enable in-process metrics collection (no external dependency).")
        .def("reset_metrics", &RuleEngine::reset_metrics)
        .def_property_readonly("metrics_enabled", &RuleEngine::metrics_enabled)
        .def("metrics_snapshot",
             [](RuleEngine& e) {
                 py::dict out;
                 py::dict counters;
                 for (const auto& kv : e.metrics_counters()) counters[py::str(kv.first)] = kv.second;
                 py::dict gauges;
                 for (const auto& kv : e.metrics_gauges()) gauges[py::str(kv.first)] = kv.second;
                 py::dict histograms;
                 for (const auto& kv : e.metrics_histograms()) {
                     py::dict h;
                     h["count"] = kv.second.count;
                     h["sum"] = kv.second.sum;
                     h["min"] = kv.second.min;
                     h["max"] = kv.second.max;
                     h["mean"] = kv.second.mean();
                     histograms[py::str(kv.first)] = h;
                 }
                 out["counters"] = counters;
                 out["gauges"] = gauges;
                 out["histograms"] = histograms;
                 return out;
             },
             "Return collected metrics as {counters, gauges, histograms}.")
        .def_property_readonly("active_rule_set_version", &RuleEngine::active_rule_set_version);
}
