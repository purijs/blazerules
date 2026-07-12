#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "collectors.h"
#include "state.h"

class DashboardServer {
public:
    explicit DashboardServer(Options options);
    ~DashboardServer();

    void start();
    void stop();
    DashboardSnapshot snapshot() const;

    std::string health_json() const;
    std::string summary_json(const std::string& instance) const;
    std::string timeline_json(const std::string& instance, int64_t from_ms, int64_t to_ms, int64_t range_ms) const;
    std::string metrics_json() const;
    std::string decisions_json(const DecisionQuery& query) const;
    std::string models_json(int bins, const std::string& instance) const;
    std::string rules_json(size_t limit, const std::string& instance) const;
    std::string errors_json(size_t limit, const std::string& instance) const;
    std::string benchmarks_json() const;
    std::string ruleset_json(const std::string& selector) const;

private:
    void refresh_once();

    Options options_;
    DecisionLogTailer decision_tailer_;
    DeadLetterTailer dead_letter_tailer_;
    PrometheusScraper metrics_scraper_;
    BenchmarkReader benchmark_reader_;
    RulesetReader ruleset_reader_;
    mutable std::mutex mu_;
    DashboardSnapshot snapshot_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
    double last_records_ = 0.0;
    int64_t last_records_ms_ = 0;
    uintmax_t last_decision_log_bytes_ = 0;
    int64_t last_decision_log_bytes_ms_ = 0;
    double last_input_bytes_ = 0.0;
    double last_input_records_ = 0.0;
    int64_t last_input_bytes_ms_ = 0;
    int64_t last_input_records_ms_ = 0;
    std::map<std::string, double> last_input_bytes_by_instance_;
    std::map<std::string, double> last_input_records_by_instance_;
    std::map<std::string, int64_t> last_input_bytes_ms_by_instance_;
    std::map<std::string, int64_t> last_input_records_ms_by_instance_;
    std::string s3_source_;
    bool s3_source_is_dir_ = false;
    std::string s3_local_root_;
    std::string s3_rules_source_;
    std::string s3_rules_local_;
    int64_t last_s3_sync_ms_ = 0;
};
