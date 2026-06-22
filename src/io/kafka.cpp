#include "blazerules_io/kafka.h"

#ifdef BLAZERULES_IO_KAFKA

#include <stdexcept>

#include <librdkafka/rdkafkacpp.h>

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

}  // namespace

KafkaConsumer::KafkaConsumer(const std::string& brokers, const std::string& group_id,
                             const std::vector<std::string>& topics,
                             const std::map<std::string, std::string>& extra_conf) {
    auto conf = make_conf(brokers, extra_conf);
    std::string err;
    conf->set("group.id", group_id, err);
    conf->set("enable.auto.commit", "false", err);     // commit after eval (at-least-once)
    conf->set("auto.offset.reset", "earliest", err);
    RdKafka::KafkaConsumer* c = RdKafka::KafkaConsumer::create(conf.get(), err);
    if (!c) throw std::runtime_error("kafka consumer create: " + err);
    consumer_.reset(c);
    RdKafka::ErrorCode rc = consumer_->subscribe(topics);
    if (rc != RdKafka::ERR_NO_ERROR) {
        throw std::runtime_error("kafka subscribe: " + RdKafka::err2str(rc));
    }
}

KafkaConsumer::~KafkaConsumer() {
    if (consumer_) consumer_->close();
}

std::vector<std::string> KafkaConsumer::poll_batch(int max_messages, int timeout_ms) {
    std::vector<KafkaMessage> records = poll_records(max_messages, timeout_ms);
    std::vector<std::string> out;
    out.reserve(records.size());
    for (auto& record : records) out.push_back(std::move(record.value));
    return out;
}

std::vector<KafkaMessage> KafkaConsumer::poll_records(int max_messages, int timeout_ms) {
    std::vector<KafkaMessage> out;
    if (!consumer_ || max_messages <= 0) return out;
    out.reserve(static_cast<size_t>(max_messages));
    while (static_cast<int>(out.size()) < max_messages) {
        // Block up to timeout_ms for the first message; then drain non-blocking.
        std::unique_ptr<RdKafka::Message> msg(
            consumer_->consume(out.empty() ? timeout_ms : 0));
        const RdKafka::ErrorCode ec = msg->err();
        if (ec == RdKafka::ERR_NO_ERROR) {
            if (msg->payload() && msg->len() > 0) {
                KafkaMessage record;
                record.topic = msg->topic_name();
                record.partition = msg->partition();
                record.offset = msg->offset();
                const RdKafka::MessageTimestamp ts = msg->timestamp();
                record.timestamp_ms = ts.timestamp;
                const std::string* key = msg->key();
                if (key) record.key = *key;
                record.value.assign(static_cast<const char*>(msg->payload()), msg->len());
                out.push_back(std::move(record));
            }
            // (empty payload = tombstone; skip)
        } else {
            // ERR__TIMED_OUT, ERR__PARTITION_EOF, or any other error -> end this batch.
            break;
        }
    }
    return out;
}

void KafkaConsumer::commit() {
    if (consumer_) consumer_->commitSync();
}

void KafkaConsumer::close() {
    if (consumer_) consumer_->close();
}

KafkaProducer::KafkaProducer(const std::string& brokers,
                             const std::map<std::string, std::string>& extra_conf) {
    auto conf = make_conf(brokers, extra_conf);
    std::string err;
    RdKafka::Producer* p = RdKafka::Producer::create(conf.get(), err);
    if (!p) throw std::runtime_error("kafka producer create: " + err);
    producer_.reset(p);
}

KafkaProducer::~KafkaProducer() {
    if (producer_) producer_->flush(2000);
}

void KafkaProducer::produce(const std::string& topic, const std::string& value,
                            const std::string& key) {
    RdKafka::ErrorCode ec = producer_->produce(
        topic, RdKafka::Topic::PARTITION_UA, RdKafka::Producer::RK_MSG_COPY,
        const_cast<char*>(value.data()), value.size(),
        key.empty() ? nullptr : key.data(), key.size(), 0, nullptr);
    if (ec != RdKafka::ERR_NO_ERROR) {
        throw std::runtime_error("kafka produce: " + RdKafka::err2str(ec));
    }
    producer_->poll(0);  // serve delivery callbacks
}

void KafkaProducer::flush(int timeout_ms) {
    if (producer_) producer_->flush(timeout_ms);
}

}  // namespace blazerules_io

#endif  // BLAZERULES_IO_KAFKA
