#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "state.h"

class DecisionLogTailer {
public:
    DecisionLogTailer(std::string path, size_t tail_lines);
    DecisionState update(SourceStatus& status);

private:
    std::string path_;
    size_t tail_lines_ = 5000;
    DecisionState cached_;
};

class DeadLetterTailer {
public:
    DeadLetterTailer(std::string path, size_t tail_lines);
    ErrorState update(SourceStatus& status);

private:
    std::string path_;
    size_t tail_lines_ = 5000;
    ErrorState cached_;
};

class PrometheusScraper {
public:
    explicit PrometheusScraper(std::string url);
    MetricsState update(SourceStatus& status);

private:
    std::string url_;
    MetricsState cached_;
};

class BenchmarkReader {
public:
    explicit BenchmarkReader(std::string path);
    BenchmarkState update(SourceStatus& status);

private:
    std::string path_;
    uintmax_t last_size_ = 0;
    int64_t last_mtime_count_ = 0;
    BenchmarkState cached_;
};

class RulesetReader {
public:
    RulesetReader(std::string active_path, std::string candidate_path, std::string history_dir);
    RulesetState update(SourceStatus& status);

private:
    std::string active_path_;
    std::string candidate_path_;
    std::string history_dir_;
    RulesetState cached_;
};
