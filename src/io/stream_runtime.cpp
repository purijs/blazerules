#include "blazerules_io/stream_runtime.h"

#ifdef BLAZERULES_IO_KAFKA

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <librdkafka/rdkafkacpp.h>

#include "blazerules_io/cdc.h"
#include "blazerules_io/decoder.h"
#include "blazerules_io/kafka.h"

namespace blazerules_io {

namespace {

using MessagePtr = std::shared_ptr<RdKafka::Message>;
using PartitionKey = std::pair<std::string, int32_t>;

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity) : capacity_(std::max<size_t>(1, capacity)) {}

    bool push(T value) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [&] { return closed_ || queue_.size() < capacity_; });
        if (closed_) return false;
        queue_.push_back(std::move(value));
        not_empty_.notify_one();
        return true;
    }

    bool pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [&] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) return false;
        value = std::move(queue_.front());
        queue_.pop_front();
        not_full_.notify_one();
        return true;
    }

    template <typename Rep, typename Period>
    bool pop_for(T& value, const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait_for(lock, timeout, [&] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) return false;
        value = std::move(queue_.front());
        queue_.pop_front();
        not_full_.notify_one();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    bool closed_and_empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_ && queue_.empty();
    }

private:
    const size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<T> queue_;
    bool closed_ = false;
};

struct PipelineFailure {
    void capture(std::exception_ptr error) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!exception) exception = std::move(error);
        stop.store(true, std::memory_order_release);
    }

    void rethrow_if_set() const {
        std::lock_guard<std::mutex> lock(mutex);
        if (exception) std::rethrow_exception(exception);
    }

    mutable std::mutex mutex;
    std::exception_ptr exception;
    std::atomic<bool> stop{false};
};

struct WorkItem {
    std::vector<MessagePtr> messages;
};

struct CompletedItem {
    std::vector<MessagePtr> messages;
    std::vector<BatchResult> results;
    std::string error;
    int64_t eval_us = 0;
};

struct AtomicStats {
    std::atomic<int64_t> messages{0};
    std::atomic<int64_t> matched{0};
    std::atomic<int64_t> emitted{0};
    std::atomic<int64_t> eval_us{0};
    std::atomic<int64_t> dlq_routed{0};
};

class CommitAccumulator {
public:
    void acknowledge(const std::vector<MessagePtr>& messages) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& message : messages) {
            if (!message || message->offset() < 0) continue;
            const PartitionKey key{message->topic_name(), message->partition()};
            int64_t& next = offsets_[key];
            next = std::max(next, message->offset() + 1);
        }
    }

    std::map<PartitionKey, int64_t> take() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<PartitionKey, int64_t> out;
        out.swap(offsets_);
        return out;
    }

private:
    std::mutex mutex_;
    std::map<PartitionKey, int64_t> offsets_;
};

std::unique_ptr<RdKafka::Conf> make_conf(
    const std::string& brokers,
    const std::map<std::string, std::string>& extra) {
    std::unique_ptr<RdKafka::Conf> conf(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
    std::string err;
    if (conf->set("bootstrap.servers", brokers, err) != RdKafka::Conf::CONF_OK) {
        throw std::runtime_error("kafka conf bootstrap.servers: " + err);
    }
    for (const auto& kv : extra) {
        if (conf->set(kv.first, kv.second, err) != RdKafka::Conf::CONF_OK) {
            throw std::runtime_error("kafka conf " + kv.first + ": " + err);
        }
    }
    return conf;
}

std::unique_ptr<RdKafka::KafkaConsumer> make_consumer(const StreamRunConfig& config) {
    auto conf = make_conf(config.brokers, config.consumer_conf);
    std::string err;
    if (conf->set("group.id", config.group_id, err) != RdKafka::Conf::CONF_OK ||
        conf->set("enable.auto.commit", "false", err) != RdKafka::Conf::CONF_OK ||
        conf->set("enable.auto.offset.store", "false", err) != RdKafka::Conf::CONF_OK) {
        throw std::runtime_error("kafka consumer configuration: " + err);
    }
    if (config.consumer_conf.find("auto.offset.reset") == config.consumer_conf.end() &&
        conf->set("auto.offset.reset", "earliest", err) != RdKafka::Conf::CONF_OK) {
        throw std::runtime_error("kafka consumer auto.offset.reset: " + err);
    }
    RdKafka::KafkaConsumer* raw = RdKafka::KafkaConsumer::create(conf.get(), err);
    if (!raw) throw std::runtime_error("kafka consumer create: " + err);
    std::unique_ptr<RdKafka::KafkaConsumer> consumer(raw);
    const RdKafka::ErrorCode rc = consumer->subscribe(config.input_topics);
    if (rc != RdKafka::ERR_NO_ERROR) {
        throw std::runtime_error("kafka subscribe: " + RdKafka::err2str(rc));
    }
    return consumer;
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

size_t worker_for(const MessagePtr& message, size_t worker_count, bool partition_affine) {
    if (!partition_affine) {
        static std::atomic<size_t> next{0};
        return next.fetch_add(1, std::memory_order_relaxed) % worker_count;
    }
    size_t value = std::hash<std::string>{}(message->topic_name());
    value ^= static_cast<size_t>(message->partition()) + 0x9e3779b9U + (value << 6) + (value >> 2);
    return value % worker_count;
}

std::vector<MessagePtr> poll_messages(RdKafka::KafkaConsumer& consumer,
                                      int max_messages,
                                      int timeout_ms) {
    std::vector<MessagePtr> messages;
    messages.reserve(static_cast<size_t>(std::max(0, max_messages)));
    while (static_cast<int>(messages.size()) < max_messages) {
        MessagePtr message(consumer.consume(messages.empty() ? timeout_ms : 0));
        if (!message) break;
        const RdKafka::ErrorCode error = message->err();
        if (error == RdKafka::ERR_NO_ERROR) {
            if (message->payload() && message->len() > 0) {
                messages.push_back(std::move(message));
            }
            continue;
        }
        if (error == RdKafka::ERR__TIMED_OUT || error == RdKafka::ERR__PARTITION_EOF) break;
        throw std::runtime_error("kafka consume: " + message->errstr());
    }
    return messages;
}

std::vector<std::string_view> message_views(const std::vector<MessagePtr>& messages) {
    std::vector<std::string_view> views;
    views.reserve(messages.size());
    for (const auto& message : messages) {
        views.emplace_back(static_cast<const char*>(message->payload()), message->len());
    }
    return views;
}

class DecoderSession {
public:
    explicit DecoderSession(const StreamRunConfig& config)
        : format_(lower_copy(config.payload_format)), arrow_validation_(config.arrow_validation) {
#ifdef BLAZERULES_IO_AVRO
        if (format_ == "avro") {
            if (config.avro_schema_json.empty()) {
                throw std::runtime_error("kafka avro stream requires avro_schema_json");
            }
            avro_ = std::make_unique<AvroDecoder>(config.avro_schema_json);
        }
#endif
#ifdef BLAZERULES_IO_PROTOBUF
        if (format_ == "protobuf" || format_ == "proto") {
            if (config.protobuf_descriptor_set.empty() || config.protobuf_message_type.empty()) {
                throw std::runtime_error(
                    "kafka protobuf stream requires protobuf_descriptor_set and protobuf_message_type");
            }
            protobuf_ = std::make_unique<ProtobufDecoder>(
                config.protobuf_descriptor_set, config.protobuf_message_type);
        }
#endif
    }

    std::vector<BatchResult> evaluate(RuleEngine& engine,
                                      const StreamRunConfig& config,
                                      const std::vector<MessagePtr>& messages) {
        const auto views = message_views(messages);
        std::vector<BatchResult> results;
        if (format_ == "json" || format_ == "ndjson" || format_ == "jsonl") {
            results.push_back(engine.evaluate_message_views(views));
            return results;
        }
        if (format_ == "debezium") {
            std::string ndjson = unwrap_debezium(views, config.debezium_op_field);
            results.push_back(engine.evaluate_ndjson_padded(ndjson));
            return results;
        }
        if (format_ == "arrow" || format_ == "arrow-ipc" || format_ == "arrow_ipc" ||
            format_ == "ipc") {
            std::vector<ArrowIpcFrame> frames;
            frames.reserve(messages.size());
            for (const auto& message : messages) {
                std::shared_ptr<void> owner(message, message.get());
                frames.emplace_back(static_cast<const uint8_t*>(message->payload()),
                                    static_cast<int64_t>(message->len()), std::move(owner));
            }
            ArrowIpcReadOptions options;
            options.validation = arrow_validation_;
            arrow_.decode_each(frames, [&](const std::shared_ptr<arrow::RecordBatch>& batch) {
                results.push_back(engine.evaluate_batch(batch));
                return true;
            }, options);
            return results;
        }
        if (format_ == "avro") {
#ifdef BLAZERULES_IO_AVRO
            results.push_back(engine.evaluate_batch(avro_->decode_batch(views)));
            return results;
#else
            throw std::runtime_error("this build does not include Avro support");
#endif
        }
        if (format_ == "protobuf" || format_ == "proto") {
#ifdef BLAZERULES_IO_PROTOBUF
            results.push_back(engine.evaluate_batch(protobuf_->decode_batch(views)));
            return results;
#else
            throw std::runtime_error("this build does not include Protobuf support");
#endif
        }
        throw std::runtime_error("unknown kafka payload format: " + config.payload_format);
    }

private:
    std::string format_;
    ArrowIpcValidationLevel arrow_validation_;
    ArrowIpcDecoder arrow_;
#ifdef BLAZERULES_IO_AVRO
    std::unique_ptr<AvroDecoder> avro_;
#endif
#ifdef BLAZERULES_IO_PROTOBUF
    std::unique_ptr<ProtobufDecoder> protobuf_;
#endif
};

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buffer[7];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                  static_cast<unsigned char>(ch));
                    out += buffer;
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    return out;
}

std::string decision_label(const BatchResult& result, int row) {
    if (row >= 0 && row < static_cast<int>(result.decisions.size())) {
        return result.decisions[static_cast<size_t>(row)];
    }
    if (row >= 0 && row < static_cast<int>(result.decision_codes.size())) {
        const int code = result.decision_codes[static_cast<size_t>(row)];
        if (code >= 0 && code < static_cast<int>(result.decision_labels.size())) {
            return result.decision_labels[static_cast<size_t>(code)];
        }
    }
    return "APPROVE";
}

std::string decision_record(const BatchResult& result, int row, bool matched) {
    const std::string decision = decision_label(result, row);
    const std::string band = row < static_cast<int>(result.risk_bands.size())
        ? result.risk_bands[static_cast<size_t>(row)] : std::string();
    const std::string winning = row < static_cast<int>(result.winning_rule_ids.size())
        ? result.winning_rule_ids[static_cast<size_t>(row)] : std::string();
    const double score = row < static_cast<int>(result.scores.size())
        ? result.scores[static_cast<size_t>(row)] : 0.0;

    std::string out;
    out.reserve(192);
    out += "{\"batch_row\":" + std::to_string(row);
    out += matched ? ",\"matched\":true" : ",\"matched\":false";
    out += ",\"decision\":\"" + json_escape(decision) + "\"";
    out += ",\"score\":" + std::to_string(score);
    if (!band.empty()) out += ",\"risk_band\":\"" + json_escape(band) + "\"";
    if (!winning.empty()) {
        out += ",\"winning_rule_id\":\"" + json_escape(winning) + "\"";
    }
    if (!result.rule_set_version.empty()) {
        out += ",\"ruleset_version\":\"" + json_escape(result.rule_set_version) + "\"";
    }
    out += "}\n";
    return out;
}

int emit_rows(KafkaProducer& producer,
              const std::string& topic,
              const BatchResult& result) {
    int emitted = 0;
    size_t matched_position = 0;
    for (int row = 0; row < result.n_records; ++row) {
        const bool matched = matched_position < result.matched_record_indices.size() &&
            result.matched_record_indices[matched_position] == row;
        if (matched) ++matched_position;
        producer.produce(topic, decision_record(result, row, matched));
        ++emitted;
    }
    return emitted;
}

std::map<std::string, int64_t> grouped_counts(const BatchResult& result) {
    std::map<std::string, int64_t> counts;
    if (!result.grouped_decision_indices.empty()) {
        for (const auto& entry : result.grouped_decision_indices) {
            counts[entry.first] += static_cast<int64_t>(entry.second.size());
        }
        return counts;
    }
    if (!result.decision_codes.empty()) {
        for (int code : result.decision_codes) {
            std::string label = "APPROVE";
            if (code >= 0 && code < static_cast<int>(result.decision_labels.size())) {
                label = result.decision_labels[static_cast<size_t>(code)];
            }
            ++counts[label];
        }
        return counts;
    }
    for (const std::string& decision : result.decisions) ++counts[decision];
    if (counts.empty() && result.n_records > 0) counts["APPROVE"] = result.n_records;
    return counts;
}

int emit_groups(KafkaProducer& producer,
                const std::string& topic,
                const BatchResult& result) {
    int emitted = 0;
    for (const auto& entry : grouped_counts(result)) {
        std::string out = "{\"decision\":\"" + json_escape(entry.first) +
            "\",\"count\":" + std::to_string(entry.second) +
            ",\"batch_records\":" + std::to_string(result.n_records);
        if (!result.rule_set_version.empty()) {
            out += ",\"ruleset_version\":\"" + json_escape(result.rule_set_version) + "\"";
        }
        out += "}\n";
        producer.produce(topic, out);
        ++emitted;
    }
    return emitted;
}

void commit_completed(RdKafka::KafkaConsumer& consumer, CommitAccumulator& commits) {
    const auto ready = commits.take();
    if (ready.empty()) return;
    std::vector<RdKafka::TopicPartition*> offsets;
    offsets.reserve(ready.size());
    for (const auto& entry : ready) {
        offsets.push_back(RdKafka::TopicPartition::create(
            entry.first.first, entry.first.second, entry.second));
    }
    const RdKafka::ErrorCode error = consumer.commitSync(offsets);
    RdKafka::TopicPartition::destroy(offsets);
    if (error != RdKafka::ERR_NO_ERROR) {
        throw std::runtime_error("kafka commit: " + RdKafka::err2str(error));
    }
}

}  // namespace

StreamRunStats run_stream(RuleEngine& engine, const StreamRunConfig& config) {
    if (config.batch_size <= 0) throw std::invalid_argument("batch_size must be positive");
    if (config.worker_count <= 0) throw std::invalid_argument("worker_count must be positive");
    if (config.queue_depth <= 0) throw std::invalid_argument("queue_depth must be positive");
    if (config.flush_interval_ms <= 0) {
        throw std::invalid_argument("flush_interval_ms must be positive");
    }
    if (config.brokers.empty()) throw std::invalid_argument("brokers is required");
    if (config.group_id.empty()) throw std::invalid_argument("group_id is required");
    if (config.input_topics.empty()) throw std::invalid_argument("input_topics is required");

    const std::string output_mode = lower_copy(config.output_mode);
    if (output_mode != "rows" && output_mode != "grouped" && output_mode != "none") {
        throw std::invalid_argument("output_mode must be rows, grouped, or none");
    }

    auto consumer = make_consumer(config);
    std::unique_ptr<KafkaProducer> producer;
    if ((!config.output_topic.empty() && output_mode != "none") || !config.dlq_topic.empty()) {
        producer = std::make_unique<KafkaProducer>(config.brokers, config.producer_conf);
    }

    const size_t worker_count = static_cast<size_t>(config.worker_count);
    auto engine_shards = engine.create_shards(config.worker_count);
    if (engine_shards.size() != worker_count) {
        throw std::runtime_error("failed to create Kafka engine shards");
    }

    std::vector<std::unique_ptr<BoundedQueue<WorkItem>>> worker_queues;
    worker_queues.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) {
        worker_queues.push_back(std::make_unique<BoundedQueue<WorkItem>>(
            static_cast<size_t>(config.queue_depth)));
    }
    BoundedQueue<CompletedItem> completed(static_cast<size_t>(config.queue_depth));
    CommitAccumulator commit_accumulator;
    PipelineFailure failure;
    AtomicStats counters;

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&, worker] {
            try {
                DecoderSession decoder(config);
                WorkItem item;
                while (worker_queues[worker]->pop(item)) {
                    if (failure.stop.load(std::memory_order_acquire)) break;
                    CompletedItem result;
                    result.messages = std::move(item.messages);
                    const auto start = std::chrono::steady_clock::now();
                    try {
                        result.results = decoder.evaluate(
                            *engine_shards[worker], config, result.messages);
                    } catch (const std::exception& error) {
                        result.error = error.what();
                    }
                    result.eval_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - start).count();
                    counters.eval_us.fetch_add(result.eval_us, std::memory_order_relaxed);
                    for (const BatchResult& batch_result : result.results) {
                        counters.matched.fetch_add(batch_result.n_matched,
                                                   std::memory_order_relaxed);
                    }
                    if (!completed.push(std::move(result))) break;
                }
            } catch (...) {
                failure.capture(std::current_exception());
                completed.close();
                for (auto& queue : worker_queues) queue->close();
            }
        });
    }

    std::thread delivery([&] {
        try {
            std::vector<MessagePtr> pending_commits;
            bool pending_delivery = false;
            auto last_flush = std::chrono::steady_clock::now();

            auto flush_pending = [&] {
                if (pending_delivery && producer && !producer->flush(config.flush_timeout_ms)) {
                    throw std::runtime_error(
                        "kafka delivery failed; affected offsets remain uncommitted");
                }
                if (config.commit_offsets && !pending_commits.empty()) {
                    commit_accumulator.acknowledge(pending_commits);
                }
                pending_commits.clear();
                pending_delivery = false;
                last_flush = std::chrono::steady_clock::now();
            };

            CompletedItem item;
            while (true) {
                const bool received = completed.pop_for(
                    item, std::chrono::milliseconds(config.flush_interval_ms));
                if (!received) {
                    if (completed.closed_and_empty()) break;
                    flush_pending();
                    continue;
                }

                bool produced = false;
                if (!item.error.empty()) {
                    if (config.dlq_topic.empty() || !producer) {
                        throw std::runtime_error("kafka batch evaluation failed: " + item.error);
                    }
                    for (const auto& message : item.messages) {
                        producer->produce(config.dlq_topic, std::string(
                            static_cast<const char*>(message->payload()), message->len()));
                        counters.dlq_routed.fetch_add(1, std::memory_order_relaxed);
                        produced = true;
                    }
                } else if (producer && !config.output_topic.empty() && output_mode != "none") {
                    for (const BatchResult& result : item.results) {
                        const int emitted = output_mode == "grouped"
                            ? emit_groups(*producer, config.output_topic, result)
                            : emit_rows(*producer, config.output_topic, result);
                        counters.emitted.fetch_add(emitted, std::memory_order_relaxed);
                        produced = produced || emitted > 0;
                    }
                }

                pending_commits.insert(pending_commits.end(),
                                       item.messages.begin(), item.messages.end());
                pending_delivery = pending_delivery || produced;
                if (!pending_delivery) flush_pending();

                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - last_flush).count();
                if (elapsed >= config.flush_interval_ms) flush_pending();
            }
            flush_pending();
        } catch (...) {
            failure.capture(std::current_exception());
            for (auto& queue : worker_queues) queue->close();
            completed.close();
        }
    });

    const bool partition_affine = config.partition_affine || config.commit_offsets;
    int64_t dispatched_batches = 0;
    try {
        while (!failure.stop.load(std::memory_order_acquire)) {
            if (config.max_messages > 0 &&
                counters.messages.load(std::memory_order_relaxed) >= config.max_messages) {
                break;
            }
            if (config.max_batches > 0 && dispatched_batches >= config.max_batches) break;

            if (config.commit_offsets) commit_completed(*consumer, commit_accumulator);

            int wanted = config.batch_size;
            if (config.max_messages > 0) {
                wanted = static_cast<int>(std::min<int64_t>(
                    wanted, config.max_messages - counters.messages.load(std::memory_order_relaxed)));
            }
            auto messages = poll_messages(*consumer, wanted, config.poll_timeout_ms);
            if (messages.empty()) break;
            counters.messages.fetch_add(static_cast<int64_t>(messages.size()),
                                        std::memory_order_relaxed);

            std::map<size_t, WorkItem> grouped;
            for (auto& message : messages) {
                grouped[worker_for(message, worker_count, partition_affine)]
                    .messages.push_back(std::move(message));
            }
            for (auto& entry : grouped) {
                if (!worker_queues[entry.first]->push(std::move(entry.second))) break;
            }
            ++dispatched_batches;
        }
    } catch (...) {
        failure.capture(std::current_exception());
    }

    for (auto& queue : worker_queues) queue->close();
    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
    completed.close();
    if (delivery.joinable()) delivery.join();

    try {
        if (config.commit_offsets) commit_completed(*consumer, commit_accumulator);
    } catch (...) {
        failure.capture(std::current_exception());
    }

    StreamRunStats stats;
    stats.batches = dispatched_batches;
    stats.messages = counters.messages.load(std::memory_order_relaxed);
    stats.matched = counters.matched.load(std::memory_order_relaxed);
    stats.emitted = counters.emitted.load(std::memory_order_relaxed);
    stats.eval_us = counters.eval_us.load(std::memory_order_relaxed);
    stats.dlq_routed = counters.dlq_routed.load(std::memory_order_relaxed);
    stats.delivery_errors = producer
        ? static_cast<int64_t>(producer->delivery_errors()) : 0;
    consumer->close();
    failure.rethrow_if_set();
    return stats;
}

}  // namespace blazerules_io

#endif  // BLAZERULES_IO_KAFKA
