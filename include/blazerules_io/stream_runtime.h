#ifndef BLAZERULES_IO_STREAM_RUNTIME_H
#define BLAZERULES_IO_STREAM_RUNTIME_H

#ifdef BLAZERULES_IO_KAFKA

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "blazerules/engine.h"
#include "blazerules_io/decoder.h"

namespace blazerules_io {

struct StreamRunConfig {
    std::string brokers;
    std::string group_id;
    std::vector<std::string> input_topics;
    std::string output_topic;
    std::string dlq_topic;
    std::map<std::string, std::string> consumer_conf;
    std::map<std::string, std::string> producer_conf;
    int batch_size = 2048;
    int worker_count = 1;
    int queue_depth = 64;
    int poll_timeout_ms = 1000;
    int flush_timeout_ms = 5000;
    int flush_interval_ms = 250;
    int64_t max_messages = 0;
    int64_t max_batches = 0;
    bool commit_offsets = true;
    bool partition_affine = true;
    std::string output_mode = "rows";
    std::string payload_format = "json";
    ArrowIpcValidationLevel arrow_validation = ArrowIpcValidationLevel::STRUCTURAL;
    std::string avro_schema_json;
    std::string protobuf_descriptor_set;
    std::string protobuf_message_type;
    std::string debezium_op_field = "__op";
};

struct StreamRunStats {
    int64_t batches = 0;
    int64_t messages = 0;
    int64_t matched = 0;
    int64_t emitted = 0;
    int64_t eval_us = 0;
    int64_t delivery_errors = 0;
    int64_t dlq_routed = 0;
};

StreamRunStats run_stream(RuleEngine& engine, const StreamRunConfig& config);

}  // namespace blazerules_io

#endif  // BLAZERULES_IO_KAFKA

#endif  // BLAZERULES_IO_STREAM_RUNTIME_H
