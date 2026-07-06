#include "blazerules_io/stream_runtime.h"

#ifdef BLAZERULES_IO_KAFKA

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <librdkafka/rdkafkacpp.h>

#include "blazerules_io/cdc.h"
#include "blazerules_io/decoder.h"
#include "blazerules_io/kafka.h"

namespace blazerules_io {

namespace {

std::unique_ptr<RdKafka::Conf> make_conf(const std::string& brokers,
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
    conf->set("group.id", config.group_id, err);
    conf->set("enable.auto.commit", "false", err);
    conf->set("auto.offset.reset", "earliest", err);
    RdKafka::KafkaConsumer* c = RdKafka::KafkaConsumer::create(conf.get(), err);
    if (!c) throw std::runtime_error("kafka consumer create: " + err);
    std::unique_ptr<RdKafka::KafkaConsumer> consumer(c);
    RdKafka::ErrorCode rc = consumer->subscribe(config.input_topics);
    if (rc != RdKafka::ERR_NO_ERROR) {
        throw std::runtime_error("kafka subscribe: " + RdKafka::err2str(rc));
    }
    return consumer;
}

struct OwnedMessageBatch {
    std::vector<std::unique_ptr<RdKafka::Message>> messages;
    std::vector<std::string_view> views;
};

OwnedMessageBatch poll_owned_batch(RdKafka::KafkaConsumer& consumer,
                                   int max_messages,
                                   int timeout_ms) {
    OwnedMessageBatch out;
    out.messages.reserve(static_cast<size_t>(std::max(0, max_messages)));
    out.views.reserve(static_cast<size_t>(std::max(0, max_messages)));
    while (static_cast<int>(out.views.size()) < max_messages) {
        std::unique_ptr<RdKafka::Message> msg(
            consumer.consume(out.views.empty() ? timeout_ms : 0));
        if (!msg || msg->err() != RdKafka::ERR_NO_ERROR) break;
        if (!msg->payload() || msg->len() == 0) continue;
        out.views.emplace_back(static_cast<const char*>(msg->payload()), msg->len());
        out.messages.push_back(std::move(msg));
    }
    return out;
}

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
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(ch));
                    out += buf;
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    return out;
}

std::string decision_record(const BatchResult& result, int row, bool matched) {
    std::string decision = matched ? "FLAG" : "APPROVE";
    if (row < static_cast<int>(result.decisions.size())) decision = result.decisions[static_cast<size_t>(row)];
    std::string band;
    if (row < static_cast<int>(result.risk_bands.size())) band = result.risk_bands[static_cast<size_t>(row)];
    std::string winning;
    if (row < static_cast<int>(result.winning_rule_ids.size())) winning = result.winning_rule_ids[static_cast<size_t>(row)];
    double score = 0.0;
    if (row < static_cast<int>(result.scores.size())) score = result.scores[static_cast<size_t>(row)];

    std::string out;
    out.reserve(160);
    out += "{\"batch_row\":";
    out += std::to_string(row);
    out += ",\"matched\":";
    out += matched ? "true" : "false";
    out += ",\"decision\":\"";
    out += json_escape(decision);
    out += "\",\"score\":";
    out += std::to_string(score);
    if (!band.empty()) {
        out += ",\"risk_band\":\"";
        out += json_escape(band);
        out += '"';
    }
    if (!winning.empty()) {
        out += ",\"winning_rule_id\":\"";
        out += json_escape(winning);
        out += '"';
    }
    out += "}\n";
    return out;
}

int emit_decisions(KafkaProducer& producer,
                   const std::string& topic,
                   const BatchResult& result) {
    int emitted = 0;
    size_t matched_pos = 0;
    for (int row = 0; row < result.n_records; ++row) {
        const bool matched = matched_pos < result.matched_record_indices.size() &&
                             result.matched_record_indices[matched_pos] == row;
        if (matched) ++matched_pos;
        producer.produce(topic, decision_record(result, row, matched));
        ++emitted;
    }
    return emitted;
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

BatchResult evaluate_stream_batch(RuleEngine& engine,
                                  const StreamRunConfig& config,
                                  const std::vector<std::string_view>& views) {
    const std::string format = lower_copy(config.payload_format);
    if (format == "json" || format == "ndjson" || format == "jsonl") {
        return engine.evaluate_message_views(views);
    }
    if (format == "debezium") {
        std::string ndjson = unwrap_debezium(views, config.debezium_op_field);
        return engine.evaluate_ndjson_padded(ndjson);
    }
    if (format == "arrow" || format == "arrow-ipc" || format == "arrow_ipc" ||
        format == "ipc") {
        ArrowIpcDecoder decoder;
        auto batch = decoder.decode_batch(views);
        return engine.evaluate_batch(batch);
    }
    if (format == "avro") {
#ifdef BLAZERULES_IO_AVRO
        if (config.avro_schema_json.empty()) {
            throw std::runtime_error("kafka avro stream requires avro_schema_json");
        }
        AvroDecoder decoder(config.avro_schema_json);
        return engine.evaluate_batch(decoder.decode_batch(views));
#else
        throw std::runtime_error("this build does not include Avro support");
#endif
    }
    if (format == "protobuf" || format == "proto") {
#ifdef BLAZERULES_IO_PROTOBUF
        if (config.protobuf_descriptor_set.empty() || config.protobuf_message_type.empty()) {
            throw std::runtime_error(
                "kafka protobuf stream requires protobuf_descriptor_set and protobuf_message_type");
        }
        ProtobufDecoder decoder(config.protobuf_descriptor_set, config.protobuf_message_type);
        return engine.evaluate_batch(decoder.decode_batch(views));
#else
        throw std::runtime_error("this build does not include Protobuf support");
#endif
    }
    throw std::runtime_error("unknown kafka payload format: " + config.payload_format);
}

}  // namespace

StreamRunStats run_stream(RuleEngine& engine, const StreamRunConfig& config) {
    if (config.batch_size <= 0) throw std::invalid_argument("batch_size must be positive");
    if (config.brokers.empty()) throw std::invalid_argument("brokers is required");
    if (config.group_id.empty()) throw std::invalid_argument("group_id is required");
    if (config.input_topics.empty()) throw std::invalid_argument("input_topics is required");

    auto consumer = make_consumer(config);
    std::unique_ptr<KafkaProducer> producer;
    if (!config.output_topic.empty() || !config.dlq_topic.empty()) {
        producer = std::make_unique<KafkaProducer>(config.brokers, config.producer_conf);
    }

    StreamRunStats stats;
    bool delivery_failed = false;
    while (true) {
        if (config.max_batches > 0 && stats.batches >= config.max_batches) break;
        if (config.max_messages > 0 && stats.messages >= config.max_messages) break;
        int want = config.batch_size;
        if (config.max_messages > 0) {
            want = static_cast<int>(std::min<int64_t>(
                want, config.max_messages - stats.messages));
        }
        auto batch = poll_owned_batch(*consumer, want, config.poll_timeout_ms);
        if (batch.views.empty()) break;

        ++stats.batches;
        stats.messages += static_cast<int64_t>(batch.views.size());

        const auto start = std::chrono::steady_clock::now();
        BatchResult result;
        try {
            result = evaluate_stream_batch(engine, config, batch.views);
        } catch (const std::exception&) {
            if (config.dlq_topic.empty() || !producer) throw;
            for (const auto& view : batch.views) {
                producer->produce(config.dlq_topic, std::string(view));
                ++stats.dlq_routed;
            }
            if (config.commit_offsets) {
                if (!producer->flush(config.flush_timeout_ms)) { delivery_failed = true; break; }
                consumer->commitSync();
            }
            continue;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();

        stats.matched += result.n_matched;
        stats.eval_us += elapsed;

        if (producer && !config.output_topic.empty()) {
            stats.emitted += emit_decisions(*producer, config.output_topic, result);
        }
        if (config.commit_offsets) {
            // At-least-once: durably deliver this batch's decisions before advancing the
            // committed offset. commitSync() commits the consumer *position*, so a later
            // successful batch would commit past a failed one and silently drop it — we
            // must stop committing entirely on failure and let replay from the last good
            // offset recover the affected batch on restart.
            if (producer && !producer->flush(config.flush_timeout_ms)) {
                delivery_failed = true;
                break;
            }
            consumer->commitSync();
        }
    }

    if (producer) {
        producer->flush(config.flush_timeout_ms);
        stats.delivery_errors = static_cast<int64_t>(producer->delivery_errors());
    }
    consumer->close();
    if (delivery_failed) {
        throw std::runtime_error(
            "kafka: decision delivery failed; offsets left uncommitted so the affected "
            "batch will be reprocessed on restart (delivery_errors=" +
            std::to_string(stats.delivery_errors) + ")");
    }
    return stats;
}

}  // namespace blazerules_io

#endif  // BLAZERULES_IO_KAFKA
