#include "json_util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string trim(std::string_view s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += "\\u00";
                    const char* hex = "0123456789abcdef";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string json_pair(std::string_view key, std::string_view value) {
    return "\"" + json_escape(key) + "\":\"" + json_escape(value) + "\"";
}

std::string json_pair(std::string_view key, double value) {
    std::ostringstream os;
    os << "\"" << json_escape(key) << "\":";
    if (std::isfinite(value)) os << std::setprecision(12) << value;
    else os << 0;
    return os.str();
}

std::string json_pair(std::string_view key, int64_t value) {
    return "\"" + json_escape(key) + "\":" + std::to_string(value);
}

std::string json_bool_pair(std::string_view key, bool value) {
    return "\"" + json_escape(key) + "\":" + (value ? "true" : "false");
}

namespace {

std::optional<size_t> json_value_pos(std::string_view line, std::string_view key) {
    std::string marker = "\"" + std::string(key) + "\"";
    size_t p = line.find(marker);
    if (p == std::string_view::npos) return std::nullopt;
    p = line.find(':', p + marker.size());
    if (p == std::string_view::npos) return std::nullopt;
    ++p;
    while (p < line.size() && std::isspace(static_cast<unsigned char>(line[p]))) ++p;
    return p;
}

std::string unescape_json_string(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c != '\\' || i + 1 >= s.size()) {
            out += c;
            continue;
        }
        char n = s[++i];
        switch (n) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            default: out += n; break;
        }
    }
    return out;
}

} // namespace

std::optional<std::string> json_string(std::string_view line, std::string_view key) {
    auto vp = json_value_pos(line, key);
    if (!vp || *vp >= line.size() || line[*vp] != '"') return std::nullopt;
    size_t p = *vp + 1;
    bool escaped = false;
    for (; p < line.size(); ++p) {
        char c = line[p];
        if (escaped) {
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            return unescape_json_string(line.substr(*vp + 1, p - (*vp + 1)));
        }
    }
    return std::nullopt;
}

std::optional<double> json_number(std::string_view line, std::string_view key) {
    auto vp = json_value_pos(line, key);
    if (!vp) return std::nullopt;
    std::string tmp(line.substr(*vp));
    char* end = nullptr;
    double v = std::strtod(tmp.c_str(), &end);
    if (end == tmp.c_str()) return std::nullopt;
    return v;
}

std::optional<bool> json_bool(std::string_view line, std::string_view key) {
    auto vp = json_value_pos(line, key);
    if (!vp) return std::nullopt;
    auto tail = line.substr(*vp);
    if (tail.rfind("true", 0) == 0) return true;
    if (tail.rfind("false", 0) == 0) return false;
    return std::nullopt;
}

std::map<std::string, std::string> parse_labels(std::string_view labels) {
    std::map<std::string, std::string> out;
    size_t p = 0;
    while (p < labels.size()) {
        while (p < labels.size() && (labels[p] == ',' || std::isspace(static_cast<unsigned char>(labels[p])))) ++p;
        size_t eq = labels.find('=', p);
        if (eq == std::string_view::npos) break;
        std::string key = std::string(labels.substr(p, eq - p));
        p = eq + 1;
        std::string value;
        if (p < labels.size() && labels[p] == '"') {
            ++p;
            bool escaped = false;
            size_t start = p;
            for (; p < labels.size(); ++p) {
                if (escaped) escaped = false;
                else if (labels[p] == '\\') escaped = true;
                else if (labels[p] == '"') break;
            }
            value = unescape_json_string(labels.substr(start, p - start));
            if (p < labels.size()) ++p;
        } else {
            size_t comma = labels.find(',', p);
            value = std::string(labels.substr(p, comma == std::string_view::npos ? labels.size() - p : comma - p));
            p = comma == std::string_view::npos ? labels.size() : comma + 1;
        }
        out[trim(key)] = value;
    }
    return out;
}

std::vector<std::string> read_tail_lines(const std::string& path, size_t max_lines, size_t max_bytes) {
    std::vector<std::string> lines;
    if (path.empty() || max_lines == 0) return lines;
    std::error_code ec;
    uintmax_t size = fs::file_size(path, ec);
    if (ec) return lines;
    uintmax_t start = size > max_bytes ? size - max_bytes : 0;
    std::ifstream in(path, std::ios::binary);
    if (!in) return lines;
    in.seekg(static_cast<std::streamoff>(start));
    std::string line;
    if (start > 0) std::getline(in, line);
    std::deque<std::string> ring;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ring.push_back(line);
        while (ring.size() > max_lines) ring.pop_front();
    }
    lines.assign(ring.begin(), ring.end());
    return lines;
}

std::string strip_yaml_scalar(std::string value) {
    value = trim(value);
    size_t comment = value.find(" #");
    if (comment != std::string::npos) value = trim(std::string_view(value).substr(0, comment));
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

int64_t file_time_ms(const fs::path& path) {
    std::error_code ec;
    auto ft = fs::last_write_time(path, ec);
    if (ec) return 0;
    auto sctp = std::chrono::time_point_cast<std::chrono::milliseconds>(
        ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return sctp.time_since_epoch().count();
}

std::vector<std::string> split_text_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

double metric_value(const MetricsState& metrics, const std::string& name) {
    for (const auto& entry : metrics.entries) {
        if (entry.name == name && entry.labels.empty()) return entry.value;
    }
    return 0.0;
}

double histogram_mean_us(const MetricsState& metrics, const std::string& base) {
    double sum = 0.0;
    double count = 0.0;
    for (const auto& entry : metrics.entries) {
        if (entry.name == base + "_sum" && entry.labels.empty()) sum = entry.value;
        else if (entry.name == base + "_count" && entry.labels.empty()) count = entry.value;
    }
    return count > 0.0 ? sum / count : 0.0;
}

std::string map_to_json(const std::map<std::string, int64_t>& m) {
    std::string out = "{";
    bool first = true;
    for (const auto& [k, v] : m) {
        if (!first) out += ",";
        first = false;
        out += json_pair(k, v);
    }
    out += "}";
    return out;
}

std::string source_json(const SourceStatus& s) {
    std::string out = "{";
    out += json_pair("name", s.name);
    out += ",";
    out += json_pair("location", s.location);
    out += ",";
    out += json_bool_pair("configured", s.configured);
    out += ",";
    out += json_bool_pair("active", s.active);
    out += ",";
    out += json_pair("bytes", static_cast<int64_t>(s.bytes));
    out += ",";
    out += json_pair("last_success_ms", s.last_success_ms);
    out += ",";
    out += json_pair("last_error", s.last_error);
    out += "}";
    return out;
}

std::string metric_entry_json(const MetricEntry& e) {
    std::string out = "{";
    out += json_pair("name", e.name);
    out += ",";
    out += json_pair("value", e.value);
    out += ",\"labels\":{";
    bool first = true;
    for (const auto& [k, v] : e.labels) {
        if (!first) out += ",";
        first = false;
        out += json_pair(k, v);
    }
    out += "}}";
    return out;
}

double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    size_t mid = values.size() / 2;
    if (values.size() % 2) return values[mid];
    return (values[mid - 1] + values[mid]) / 2.0;
}
