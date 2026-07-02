#ifndef BLAZERULES_IO_STREAM_RUNTIME_H
#define BLAZERULES_IO_STREAM_RUNTIME_H

#ifdef BLAZERULES_IO_KAFKA

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "blazerules/engine.h"

namespace blazerules_io {

struct StreamRunConfig {
    std::string brokers;
    std::string group_id;
    std::vector<std::string> input_topics;
    std::string output_topic;
    std::map<std::string, std::string> consumer_conf;
    std::map<std::string, std::string> producer_conf;
    int batch_size = 2048;
    int poll_timeout_ms = 1000;
    int flush_timeout_ms = 5000;
    int64_t max_messages = 0;
    int64_t max_batches = 0;
    bool commit_offsets = true;
};

struct StreamRunStats {
    int64_t batches = 0;
    int64_t messages = 0;
    int64_t matched = 0;
    int64_t emitted = 0;
    int64_t eval_us = 0;
    int64_t delivery_errors = 0;
};

StreamRunStats run_stream(RuleEngine& engine, const StreamRunConfig& config);

}  // namespace blazerules_io

#endif  // BLAZERULES_IO_KAFKA

#endif  // BLAZERULES_IO_STREAM_RUNTIME_H
