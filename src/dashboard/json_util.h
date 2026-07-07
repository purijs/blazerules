#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "state.h"

int64_t now_ms();
std::string trim(std::string_view s);
std::string json_escape(std::string_view s);
std::string json_pair(std::string_view key, std::string_view value);
std::string json_pair(std::string_view key, double value);
std::string json_pair(std::string_view key, int64_t value);
std::string json_bool_pair(std::string_view key, bool value);
std::optional<std::string> json_string(std::string_view line, std::string_view key);
std::optional<double> json_number(std::string_view line, std::string_view key);
std::optional<bool> json_bool(std::string_view line, std::string_view key);
std::map<std::string, std::string> parse_labels(std::string_view labels);
std::vector<std::string> read_tail_lines(const std::string& path, size_t max_lines, size_t max_bytes);
std::string strip_yaml_scalar(std::string value);
int64_t file_time_ms(const std::filesystem::path& path);
std::vector<std::string> split_text_lines(const std::string& text);
double metric_value(const MetricsState& metrics, const std::string& name);
double histogram_mean_us(const MetricsState& metrics, const std::string& base);
std::string map_to_json(const std::map<std::string, int64_t>& m);
std::string source_json(const SourceStatus& s);
std::string metric_entry_json(const MetricEntry& e);
double median(std::vector<double> values);
