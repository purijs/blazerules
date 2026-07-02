#ifndef BLAZERULES_IO_KAFKA_H
#define BLAZERULES_IO_KAFKA_H

#ifdef BLAZERULES_IO_KAFKA

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace RdKafka {
class KafkaConsumer;
class Producer;
}  // namespace RdKafka

namespace blazerules_io {

struct KafkaMessage {
    std::string topic;
    int partition = -1;
    int64_t offset = -1;
    int64_t timestamp_ms = 0;
    std::string key;
    std::string value;
};

class KafkaConsumer {
public:
    KafkaConsumer(const std::string& brokers, const std::string& group_id,
                  const std::vector<std::string>& topics,
                  const std::map<std::string, std::string>& extra_conf = {});
    ~KafkaConsumer();
    KafkaConsumer(const KafkaConsumer&) = delete;
    KafkaConsumer& operator=(const KafkaConsumer&) = delete;

    std::vector<std::string> poll_batch(int max_messages, int timeout_ms);
    std::vector<KafkaMessage> poll_records(int max_messages, int timeout_ms);
    void commit();
    void close();

private:
    std::unique_ptr<RdKafka::KafkaConsumer> consumer_;
};

class KafkaProducer {
public:
    KafkaProducer(const std::string& brokers,
                  const std::map<std::string, std::string>& extra_conf = {});
    ~KafkaProducer();
    KafkaProducer(const KafkaProducer&) = delete;
    KafkaProducer& operator=(const KafkaProducer&) = delete;

    void produce(const std::string& topic, const std::string& value, const std::string& key = "");
    // Flush pending messages. Returns true iff the queue drained within the timeout and
    // no broker delivery report reported a failure; a false return means one or more
    // messages were NOT delivered and the caller must not treat them as durable.
    bool flush(int timeout_ms = 5000);
    // Cumulative count of failed deliveries reported by the broker since construction.
    uint64_t delivery_errors() const;

private:
    class DeliveryReporter;
    std::unique_ptr<DeliveryReporter> reporter_;
    std::unique_ptr<RdKafka::Producer> producer_;
};

}  // namespace blazerules_io

#endif  // BLAZERULES_IO_KAFKA
#endif  // BLAZERULES_IO_KAFKA_H
