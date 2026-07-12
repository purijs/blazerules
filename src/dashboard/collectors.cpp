#include "collectors.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <shared_mutex>
#include <sstream>
#include <tuple>
#include <utility>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/ipc/api.h>
#include <httplib.h>

#include "blazerules/resource_resolver.h"
#include "json_util.h"

namespace fs = std::filesystem;

namespace {

constexpr int64_t kDecisionRowsPerPoll = 250000;
constexpr uintmax_t kDecisionBytesPerPoll = 64ULL * 1024ULL * 1024ULL;

}  // namespace

DecisionLogTailer::DecisionLogTailer(std::string root, bool is_dir, size_t tail_lines, size_t index_capacity)
    : root_(std::move(root)), is_dir_(is_dir), tail_lines_(tail_lines),
      index_capacity_(index_capacity) {}

DecisionLogTailer::~DecisionLogTailer() = default;

std::vector<std::string> DecisionLogTailer::model_columns() const {
    std::shared_lock<std::shared_mutex> lock(index_mu_);
    return model_columns_;
}

namespace {

bool is_actioned_decision(const DecisionRow& row) {
    if (row.matched) return true;
    if (!row.winning_rule_id.empty()) return true;
    return row.decision == "BLOCK" || row.decision == "REVIEW" || row.decision == "FLAG";
}

bool looks_like_dead_letter(const std::string& filename) {
    std::string low;
    low.reserve(filename.size());
    for (char c : filename) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return low.find("dlq") != std::string::npos ||
           low.find("dead_letter") != std::string::npos ||
           low.find("dead-letter") != std::string::npos ||
           low.find("deadletter") != std::string::npos;
}

std::string dead_letter_instance_from_path(const std::string& path) {
    std::string stem = fs::path(path).stem().string();
    const std::vector<std::string> suffixes = {
        "_dlq", "-dlq", ".dlq",
        "_dead_letter", "-dead_letter", ".dead_letter",
        "_dead-letter", "-dead-letter", ".dead-letter",
        "_deadletter", "-deadletter", ".deadletter"
    };
    for (const auto& suffix : suffixes) {
        if (stem.size() > suffix.size() &&
            stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0) {
            stem.resize(stem.size() - suffix.size());
            break;
        }
    }
    return stem;
}

void parse_ndjson_model_scores(std::string_view line, std::vector<std::pair<std::string, double>>& out) {
    const std::string key = "\"model_scores\":{";
    size_t p = line.find(key);
    if (p == std::string_view::npos) return;
    p += key.size();
    while (p < line.size() && line[p] != '}') {
        size_t q1 = line.find('"', p);
        if (q1 == std::string_view::npos) break;
        size_t q2 = line.find('"', q1 + 1);
        if (q2 == std::string_view::npos) break;
        std::string name(line.substr(q1 + 1, q2 - q1 - 1));
        size_t colon = line.find(':', q2);
        if (colon == std::string_view::npos) break;
        size_t v = colon + 1;
        size_t end = v;
        while (end < line.size() && line[end] != ',' && line[end] != '}') ++end;
        double value = 0.0;
        try {
            value = std::stod(std::string(line.substr(v, end - v)));
        } catch (...) {
            value = 0.0;
        }
        out.emplace_back(std::move(name), value);
        p = end;
        if (p < line.size() && line[p] == ',') ++p;
    }
}

}  // namespace

int32_t DecisionLogTailer::intern_label(std::vector<std::string>& labels,
                                        std::unordered_map<std::string, int32_t>& ids,
                                        const std::string& value) {
    auto it = ids.find(value);
    if (it != ids.end()) return it->second;
    int32_t id = static_cast<int32_t>(labels.size());
    labels.push_back(value);
    ids.emplace(value, id);
    return id;
}

void DecisionLogTailer::index_reset() {
    index_head_ = 0;
    index_count_ = 0;
    index_total_ = 0;
    idx_ts_ms_.clear();
    idx_score_.clear();
    idx_batch_row_.clear();
    idx_matched_.clear();
    idx_decision_.clear();
    idx_risk_.clear();
    idx_rule_.clear();
    idx_version_.clear();
    idx_instance_.clear();
    decision_labels_.clear();
    risk_labels_.clear();
    rule_labels_.clear();
    version_labels_.clear();
    instance_labels_.clear();
    decision_ids_.clear();
    risk_ids_.clear();
    rule_ids_.clear();
    version_ids_.clear();
    instance_ids_.clear();
    for (auto& col : idx_model_scores_) col.clear();
    instance_agg_.clear();
}

void DecisionLogTailer::index_push(const DecisionRow& row) {
    if (index_capacity_ == 0) return;
    const int32_t decision_id = row.decision.empty() ? -1 : intern_label(decision_labels_, decision_ids_, row.decision);
    const int32_t risk_id = row.risk_band.empty() ? -1 : intern_label(risk_labels_, risk_ids_, row.risk_band);
    const int32_t rule_id = row.winning_rule_id.empty() ? -1 : intern_label(rule_labels_, rule_ids_, row.winning_rule_id);
    const int32_t version_id = row.ruleset_version.empty() ? -1 : intern_label(version_labels_, version_ids_, row.ruleset_version);
    const int32_t instance_id = row.instance.empty() ? -1 : intern_label(instance_labels_, instance_ids_, row.instance);
    const float missing = std::numeric_limits<float>::quiet_NaN();
    for (const auto& ms : row.model_scores) {
        if (model_column_ids_.find(ms.first) == model_column_ids_.end()) {
            model_column_ids_.emplace(ms.first, model_columns_.size());
            model_columns_.push_back(ms.first);
            idx_model_scores_.emplace_back(idx_score_.size(), missing);
        }
    }
    if (index_count_ < index_capacity_) {
        idx_ts_ms_.push_back(row.ts_ms);
        idx_score_.push_back(static_cast<float>(row.score));
        idx_batch_row_.push_back(static_cast<int32_t>(row.batch_row));
        idx_matched_.push_back(row.matched ? 1 : 0);
        idx_decision_.push_back(decision_id);
        idx_risk_.push_back(risk_id);
        idx_rule_.push_back(rule_id);
        idx_version_.push_back(version_id);
        idx_instance_.push_back(instance_id);
        for (auto& col : idx_model_scores_) col.push_back(missing);
        for (const auto& ms : row.model_scores) {
            idx_model_scores_[model_column_ids_[ms.first]].back() = static_cast<float>(ms.second);
        }
        ++index_count_;
    } else {
        const size_t slot = index_head_;
        idx_ts_ms_[slot] = row.ts_ms;
        idx_score_[slot] = static_cast<float>(row.score);
        idx_batch_row_[slot] = static_cast<int32_t>(row.batch_row);
        idx_matched_[slot] = row.matched ? 1 : 0;
        idx_decision_[slot] = decision_id;
        idx_risk_[slot] = risk_id;
        idx_rule_[slot] = rule_id;
        idx_version_[slot] = version_id;
        idx_instance_[slot] = instance_id;
        for (auto& col : idx_model_scores_) col[slot] = missing;
        for (const auto& ms : row.model_scores) {
            idx_model_scores_[model_column_ids_[ms.first]][slot] = static_cast<float>(ms.second);
        }
        index_head_ = (index_head_ + 1) % index_capacity_;
    }
    ++index_total_;
}

DecisionQueryResult DecisionLogTailer::query(const DecisionQuery& q) const {
    std::shared_lock<std::shared_mutex> lock(index_mu_);
    DecisionQueryResult result;
    result.indexed_rows = static_cast<int64_t>(index_count_);
    result.truncated = index_total_ > static_cast<int64_t>(index_count_);
    const size_t limit = std::max<size_t>(1, q.limit);

    int32_t want_decision = -2;
    if (!q.decision.empty()) {
        auto it = decision_ids_.find(q.decision);
        want_decision = it == decision_ids_.end() ? -1 : it->second;
    }
    int32_t want_risk = -2;
    if (!q.risk_band.empty()) {
        auto it = risk_ids_.find(q.risk_band);
        want_risk = it == risk_ids_.end() ? -1 : it->second;
    }
    int32_t want_instance = -2;
    if (!q.instance.empty()) {
        auto it = instance_ids_.find(q.instance);
        want_instance = it == instance_ids_.end() ? -1 : it->second;
    }
    if (want_decision == -1 || want_risk == -1 || want_instance == -1) return result;

    const bool rule_filter = !q.rule.empty();
    std::vector<uint8_t> rule_match(rule_labels_.size(), rule_filter ? 0 : 1);
    if (rule_filter) {
        for (size_t i = 0; i < rule_labels_.size(); ++i) {
            if (rule_labels_[i].find(q.rule) != std::string::npos) rule_match[i] = 1;
        }
    }

    int64_t rank = 0;
    for (size_t k = index_count_; k-- > 0;) {
        const size_t slot = (index_head_ + k) % index_capacity_;
        if (want_decision != -2 && idx_decision_[slot] != want_decision) continue;
        if (want_risk != -2 && idx_risk_[slot] != want_risk) continue;
        if (want_instance != -2 && idx_instance_[slot] != want_instance) continue;
        if (rule_filter) {
            const int32_t rid = idx_rule_[slot];
            if (rid < 0 || rid >= static_cast<int32_t>(rule_match.size()) || !rule_match[static_cast<size_t>(rid)]) continue;
        }
        const int64_t ts = idx_ts_ms_[slot];
        if (q.from_ms > 0 && ts < q.from_ms) continue;
        if (q.to_ms > 0 && ts > q.to_ms) continue;

        const int32_t dec_id = idx_decision_[slot];
        const int32_t risk_id = idx_risk_[slot];
        const int32_t inst_id = idx_instance_[slot];
        if (dec_id >= 0) result.decision_facets[decision_labels_[static_cast<size_t>(dec_id)]] += 1;
        if (risk_id >= 0) result.risk_band_facets[risk_labels_[static_cast<size_t>(risk_id)]] += 1;
        if (inst_id >= 0) result.instance_facets[instance_labels_[static_cast<size_t>(inst_id)]] += 1;

        if (rank >= static_cast<int64_t>(q.offset) && result.rows.size() < limit) {
            DecisionRow row;
            row.ts_ms = ts;
            row.score = idx_score_[slot];
            row.matched = idx_matched_[slot] != 0;
            row.batch_row = idx_batch_row_[slot];
            row.decision = dec_id >= 0 ? decision_labels_[static_cast<size_t>(dec_id)] : "";
            row.risk_band = risk_id >= 0 ? risk_labels_[static_cast<size_t>(risk_id)] : "";
            const int32_t rule_id = idx_rule_[slot];
            row.winning_rule_id = rule_id >= 0 ? rule_labels_[static_cast<size_t>(rule_id)] : "";
            const int32_t version_id = idx_version_[slot];
            row.ruleset_version = version_id >= 0 ? version_labels_[static_cast<size_t>(version_id)] : "";
            row.instance = inst_id >= 0 ? instance_labels_[static_cast<size_t>(inst_id)] : "";
            for (size_t c = 0; c < model_columns_.size(); ++c) {
                const float v = idx_model_scores_[c][slot];
                if (v == v) row.model_scores.emplace_back(model_columns_[c], static_cast<double>(v));
            }
            result.rows.push_back(std::move(row));
        }
        ++rank;
    }
    result.total_matches = rank;
    return result;
}

DecisionState DecisionLogTailer::scoped_state(const std::string& instance) const {
    std::shared_lock<std::shared_mutex> lock(index_mu_);
    DecisionState s;
    if (instance.empty()) return s;
    auto it = instance_agg_.find(instance);
    if (it == instance_agg_.end()) return s;
    const InstanceAgg& a = it->second;
    s.rows_seen = a.rows;
    s.matched_seen = a.matched;
    s.decision_counts = a.decision_counts;
    s.risk_band_counts = a.risk_counts;
    s.winning_rule_counts = a.rule_counts;
    s.instance_counts[instance] = a.rows;
    if (a.rows > 0) s.avg_score = a.score_sum / static_cast<double>(a.rows);
    return s;
}

std::vector<ModelHistogram> DecisionLogTailer::model_histograms(int bins, const std::string& instance) const {
    std::shared_lock<std::shared_mutex> lock(index_mu_);
    std::vector<ModelHistogram> out;
    if (bins < 1) bins = 1;
    int32_t want_instance = -2;
    if (!instance.empty()) {
        auto it = instance_ids_.find(instance);
        if (it == instance_ids_.end()) {
            for (const auto& name : model_columns_) {
                ModelHistogram h;
                h.name = name;
                out.push_back(std::move(h));
            }
            return out;
        }
        want_instance = it->second;
    }
    for (size_t c = 0; c < model_columns_.size(); ++c) {
        ModelHistogram h;
        h.name = model_columns_[c];
        const std::vector<float>& col = idx_model_scores_[c];
        double mn = std::numeric_limits<double>::infinity();
        double mx = -std::numeric_limits<double>::infinity();
        double sum = 0.0;
        int64_t cnt = 0;
        for (size_t k = 0; k < index_count_; ++k) {
            const size_t slot = (index_head_ + k) % index_capacity_;
            if (want_instance != -2 && idx_instance_[slot] != want_instance) continue;
            const float v = col[slot];
            if (v != v) continue;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            sum += v;
            ++cnt;
        }
        h.count = cnt;
        if (cnt > 0) {
            h.min = mn;
            h.max = mx;
            h.mean = sum / static_cast<double>(cnt);
            const double span = (mx > mn) ? (mx - mn) : 1.0;
            h.bins.resize(static_cast<size_t>(bins));
            for (int b = 0; b < bins; ++b) {
                h.bins[static_cast<size_t>(b)].lo = mn + span * b / bins;
                h.bins[static_cast<size_t>(b)].hi = mn + span * (b + 1) / bins;
            }
            for (size_t k = 0; k < index_count_; ++k) {
                const size_t slot = (index_head_ + k) % index_capacity_;
                if (want_instance != -2 && idx_instance_[slot] != want_instance) continue;
                const float v = col[slot];
                if (v != v) continue;
                int b = static_cast<int>((v - mn) / span * bins);
                if (b < 0) b = 0;
                if (b >= bins) b = bins - 1;
                ++h.bins[static_cast<size_t>(b)].count;
            }
        }
        if (h.count == 0 && want_instance != -2) continue;
        out.push_back(std::move(h));
    }
    return out;
}

void DecisionLogTailer::discover_sources() {
    std::error_code ec;
    if (is_dir_) {
        if (!fs::is_directory(root_, ec)) return;
        for (const auto& entry : fs::directory_iterator(root_, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".arrow" && ext != ".ndjson") continue;
            if (looks_like_dead_letter(entry.path().filename().string())) continue;
            const std::string p = entry.path().string();
            bool known = false;
            for (const auto& s : sources_) {
                if (s.path == p) { known = true; break; }
            }
            if (!known) {
                LogSource src;
                src.path = p;
                src.instance_label = entry.path().stem().string();
                src.dict_memo = std::make_shared<arrow::ipc::DictionaryMemo>();
                sources_.push_back(std::move(src));
            }
        }
    } else if (sources_.empty() && !root_.empty()) {
        LogSource src;
        src.path = root_;
        src.instance_label = fs::path(root_).stem().string();
        src.dict_memo = std::make_shared<arrow::ipc::DictionaryMemo>();
        sources_.push_back(std::move(src));
    }
}

DecisionState DecisionLogTailer::update(SourceStatus& status) {
    status.name = "decision_log";
    status.location = root_;
    status.configured = !root_.empty();
    if (root_.empty()) {
        status.active = false;
        cached_ = DecisionState{};
        recent_.clear();
        score_sum_ = 0.0;
        return cached_;
    }

    discover_sources();

    uintmax_t total_bytes = 0;
    bool any_active = false;
    bool shrink = false;
    std::string last_error;
    std::map<std::string, int64_t> log_bytes;
    for (const auto& src : sources_) {
        std::error_code ec;
        uintmax_t bytes = fs::file_size(src.path, ec);
        if (ec) { last_error = "not readable: " + src.path; continue; }
        total_bytes += bytes;
        log_bytes[src.instance_label] += static_cast<int64_t>(bytes);
        any_active = true;
        if (bytes < src.read_offset) shrink = true;
    }
    status.bytes = total_bytes;
    status.active = any_active;
    status.last_error = last_error;

    std::unique_lock<std::shared_mutex> index_lock(index_mu_);
    if (!any_active) {
        cached_ = DecisionState{};
        recent_.clear();
        score_sum_ = 0.0;
        sources_.clear();
        index_reset();
        status.last_success_ms = now_ms();
        return cached_;
    }
    if (shrink) {
        cached_ = DecisionState{};
        recent_.clear();
        score_sum_ = 0.0;
        for (auto& src : sources_) {
            src.read_offset = 0;
            src.format_detected = false;
            src.arrow_mode = false;
            src.arrow_schema.reset();
            src.dict_memo = std::make_shared<arrow::ipc::DictionaryMemo>();
        }
        index_reset();
    }

    for (auto& src : sources_) {
        int64_t rows_budget = kDecisionRowsPerPoll;
        uintmax_t bytes_budget = kDecisionBytesPerPoll;
        std::error_code ec;
        uintmax_t bytes = fs::file_size(src.path, ec);
        if (ec) continue;
        if (!src.format_detected && bytes >= 4) {
            std::ifstream probe(src.path, std::ios::binary);
            unsigned char magic[4] = {0, 0, 0, 0};
            probe.read(reinterpret_cast<char*>(magic), 4);
            if (probe.gcount() == 4) {
                src.arrow_mode = magic[0] == 0xFF && magic[1] == 0xFF && magic[2] == 0xFF && magic[3] == 0xFF;
                src.format_detected = true;
            }
        }
        if (!src.format_detected) continue;
        if (src.arrow_mode) read_arrow(src, status, rows_budget, bytes_budget);
        else read_ndjson(src, bytes, rows_budget, bytes_budget);
    }

    cached_.recent.assign(recent_.begin(), recent_.end());
    if (cached_.rows_seen > 0) cached_.avg_score = score_sum_ / static_cast<double>(cached_.rows_seen);
    cached_.instance_log_bytes = std::move(log_bytes);
    status.last_success_ms = now_ms();
    return cached_;
}

void DecisionLogTailer::ingest_row(DecisionRow&& row) {
    cached_.rows_seen += 1;
    if (is_actioned_decision(row)) cached_.matched_seen += 1;
    score_sum_ += row.score;
    if (!row.decision.empty()) cached_.decision_counts[row.decision] += 1;
    if (!row.risk_band.empty()) cached_.risk_band_counts[row.risk_band] += 1;
    if (!row.winning_rule_id.empty()) cached_.winning_rule_counts[row.winning_rule_id] += 1;
    if (!row.ruleset_version.empty()) cached_.ruleset_versions.insert(row.ruleset_version);
    if (!row.instance.empty()) {
        cached_.instance_counts[row.instance] += 1;
        InstanceAgg& a = instance_agg_[row.instance];
        a.rows += 1;
        if (is_actioned_decision(row)) a.matched += 1;
        a.score_sum += row.score;
        if (!row.decision.empty()) a.decision_counts[row.decision] += 1;
        if (!row.risk_band.empty()) a.risk_counts[row.risk_band] += 1;
        if (!row.winning_rule_id.empty()) a.rule_counts[row.winning_rule_id] += 1;
    }
    index_push(row);
    recent_.push_back(std::move(row));
    while (recent_.size() > tail_lines_) recent_.pop_front();
}

void DecisionLogTailer::read_ndjson(LogSource& src, uintmax_t bytes, int64_t& rows_budget, uintmax_t& bytes_budget) {
    std::ifstream in(src.path, std::ios::binary);
    if (!in) return;
    in.seekg(static_cast<std::streamoff>(src.read_offset));
    std::string line;
    uintmax_t consumed = src.read_offset;
    const uintmax_t start_offset = src.read_offset;
    while (rows_budget > 0 && bytes_budget > 0 && consumed < bytes && std::getline(in, line)) {
        // A final line with no trailing '\n' (a partial record still being appended)
        // leaves eofbit set: skip it and re-read it complete on the next poll. Only
        // advance the offset past fully '\n'-terminated lines to avoid double-counting.
        if (in.eof()) break;
        const uintmax_t line_bytes = static_cast<uintmax_t>(line.size()) + 1;
        consumed += line_bytes;
        bytes_budget = line_bytes >= bytes_budget ? 0 : bytes_budget - line_bytes;
        std::string_view sv(line);
        if (!sv.empty() && sv.back() == '\r') sv.remove_suffix(1);
        if (trim(sv).empty()) continue;
        DecisionRow row;
        row.ts_ms = static_cast<int64_t>(json_number(sv, "ts_ms").value_or(0));
        row.ruleset_version = json_string(sv, "ruleset_version").value_or("");
        row.instance = json_string(sv, "instance").value_or(src.instance_label);
        row.batch_row = static_cast<int64_t>(json_number(sv, "batch_row").value_or(0));
        row.matched = json_bool(sv, "matched").value_or(false);
        row.decision = json_string(sv, "decision").value_or("");
        row.score = json_number(sv, "score").value_or(0.0);
        row.risk_band = json_string(sv, "risk_band").value_or("");
        row.winning_rule_id = json_string(sv, "winning_rule_id").value_or("");
        parse_ndjson_model_scores(sv, row.model_scores);
        ingest_row(std::move(row));
        --rows_budget;
        if (consumed - start_offset >= kDecisionBytesPerPoll) break;
    }
    src.read_offset = consumed;
}

void DecisionLogTailer::read_arrow(LogSource& src, SourceStatus& status, int64_t& rows_budget, uintmax_t& bytes_budget) {
    auto file_result = arrow::io::ReadableFile::Open(src.path);
    if (!file_result.ok()) {
        status.last_error = "failed to open arrow decision log";
        return;
    }
    std::shared_ptr<arrow::io::RandomAccessFile> file = *file_result;
    if (!file->Seek(static_cast<int64_t>(src.read_offset)).ok()) return;
    const uintmax_t start_offset = src.read_offset;
    const arrow::ipc::IpcReadOptions read_options = arrow::ipc::IpcReadOptions::Defaults();
    while (rows_budget > 0 && bytes_budget > 0) {
        auto message_result = arrow::ipc::ReadMessage(file.get());
        if (!message_result.ok()) break;
        std::unique_ptr<arrow::ipc::Message> message = std::move(*message_result);
        if (!message) break;
        if (message->type() == arrow::ipc::MessageType::SCHEMA) {
            auto schema_result = arrow::ipc::ReadSchema(*message, src.dict_memo.get());
            if (schema_result.ok()) src.arrow_schema = *schema_result;
        } else if (message->type() == arrow::ipc::MessageType::RECORD_BATCH && src.arrow_schema) {
            auto batch_result = arrow::ipc::ReadRecordBatch(*message, src.arrow_schema,
                                                            src.dict_memo.get(), read_options);
            if (batch_result.ok()) {
                index_arrow_batch(**batch_result, src.instance_label);
                rows_budget -= std::max<int64_t>(0, (*batch_result)->num_rows());
            }
        }
        auto position = file->Tell();
        if (!position.ok()) break;
        const uintmax_t new_offset = static_cast<uintmax_t>(*position);
        if (new_offset > src.read_offset) {
            const uintmax_t delta = new_offset - src.read_offset;
            bytes_budget = delta >= bytes_budget ? 0 : bytes_budget - delta;
        }
        src.read_offset = static_cast<uintmax_t>(*position);
        if (src.read_offset - start_offset >= kDecisionBytesPerPoll) break;
    }
}

void DecisionLogTailer::index_arrow_batch(const arrow::RecordBatch& batch, const std::string& fallback_instance) {
    auto ts = std::dynamic_pointer_cast<arrow::Int64Array>(batch.GetColumnByName("ts_ms"));
    auto batch_row = std::dynamic_pointer_cast<arrow::Int32Array>(batch.GetColumnByName("batch_row"));
    auto version = std::dynamic_pointer_cast<arrow::StringArray>(batch.GetColumnByName("ruleset_version"));
    auto instance = std::dynamic_pointer_cast<arrow::StringArray>(batch.GetColumnByName("instance"));
    auto matched = std::dynamic_pointer_cast<arrow::BooleanArray>(batch.GetColumnByName("matched"));
    auto decision = std::dynamic_pointer_cast<arrow::StringArray>(batch.GetColumnByName("decision"));
    auto score = std::dynamic_pointer_cast<arrow::DoubleArray>(batch.GetColumnByName("score"));
    auto risk = std::dynamic_pointer_cast<arrow::StringArray>(batch.GetColumnByName("risk_band"));
    auto rule = std::dynamic_pointer_cast<arrow::StringArray>(batch.GetColumnByName("winning_rule_id"));
    std::vector<std::pair<std::string, std::shared_ptr<arrow::DoubleArray>>> model_cols;
    for (const auto& field : batch.schema()->fields()) {
        if (field->name().rfind("model.", 0) != 0) continue;
        auto arr = std::dynamic_pointer_cast<arrow::DoubleArray>(batch.GetColumnByName(field->name()));
        if (arr) model_cols.emplace_back(field->name(), arr);
    }
    const int64_t n = batch.num_rows();
    for (int64_t i = 0; i < n; ++i) {
        DecisionRow row;
        if (ts && ts->IsValid(i)) row.ts_ms = ts->Value(i);
        if (batch_row && batch_row->IsValid(i)) row.batch_row = batch_row->Value(i);
        if (version && version->IsValid(i)) row.ruleset_version = version->GetString(i);
        row.instance = (instance && instance->IsValid(i)) ? instance->GetString(i) : fallback_instance;
        if (matched && matched->IsValid(i)) row.matched = matched->Value(i);
        if (decision && decision->IsValid(i)) row.decision = decision->GetString(i);
        if (score && score->IsValid(i)) row.score = score->Value(i);
        if (risk && risk->IsValid(i)) row.risk_band = risk->GetString(i);
        if (rule && rule->IsValid(i)) row.winning_rule_id = rule->GetString(i);
        for (const auto& mc : model_cols) {
            if (mc.second->IsValid(i)) row.model_scores.emplace_back(mc.first, mc.second->Value(i));
        }
        ingest_row(std::move(row));
    }
}

DeadLetterTailer::DeadLetterTailer(std::string root, bool is_dir, size_t tail_lines)
    : root_(std::move(root)), is_dir_(is_dir), tail_lines_(tail_lines) {}

void DeadLetterTailer::read_file(const std::string& path, const std::string& instance, uintmax_t& offset) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    in.seekg(static_cast<std::streamoff>(offset));
    std::string line;
    uintmax_t consumed = offset;
    while (std::getline(in, line)) {
        if (in.eof()) break;
        consumed += static_cast<uintmax_t>(line.size()) + 1;
        std::string_view sv(line);
        if (!sv.empty() && sv.back() == '\r') sv.remove_suffix(1);
        if (trim(sv).empty()) continue;
        ErrorRow row;
        row.ts_ms = static_cast<int64_t>(json_number(sv, "ts_ms").value_or(0));
        row.instance = json_string(sv, "instance").value_or(instance);
        row.code = json_string(sv, "code").value_or("");
        row.message = json_string(sv, "message").value_or("");
        row.source = json_string(sv, "source").value_or("");
        row.row_index = static_cast<int64_t>(json_number(sv, "row_index").value_or(-1));
        row.column_name = json_string(sv, "column_name").value_or("");
        cached_.rows_seen += 1;
        if (!row.code.empty()) cached_.code_counts[row.code] += 1;
        if (!row.instance.empty()) {
            cached_.instance_counts[row.instance] += 1;
            if (!row.code.empty()) cached_.code_counts_by_instance[row.instance][row.code] += 1;
        }
        recent_.push_back(std::move(row));
        while (recent_.size() > tail_lines_) recent_.pop_front();
    }
    offset = consumed;
}

ErrorState DeadLetterTailer::update(SourceStatus& status) {
    status.name = "dead_letter_log";
    status.location = root_;
    status.configured = !root_.empty();
    if (root_.empty()) {
        status.active = false;
        cached_ = ErrorState{};
        recent_.clear();
        offsets_.clear();
        return cached_;
    }
    std::error_code ec;
    std::vector<std::string> files;
    if (is_dir_) {
        if (fs::is_directory(root_, ec)) {
            for (const auto& entry : fs::directory_iterator(root_, ec)) {
                if (ec) break;
                if (!entry.is_regular_file(ec)) continue;
                if (entry.path().extension().string() != ".ndjson") continue;
                if (!looks_like_dead_letter(entry.path().filename().string())) continue;
                files.push_back(entry.path().string());
            }
        }
    } else {
        files.push_back(root_);
    }

    uintmax_t total_bytes = 0;
    bool any_active = false;
    bool shrink = false;
    for (const auto& f : files) {
        uintmax_t b = fs::file_size(f, ec);
        if (ec) continue;
        total_bytes += b;
        any_active = true;
        if (b < offsets_[f]) shrink = true;
    }
    status.bytes = total_bytes;
    status.active = any_active;
    status.last_error.clear();

    if (!any_active) {
        cached_ = ErrorState{};
        recent_.clear();
        offsets_.clear();
        status.last_success_ms = now_ms();
        return cached_;
    }
    if (shrink) {
        cached_ = ErrorState{};
        recent_.clear();
        for (auto& kv : offsets_) kv.second = 0;
    }
    for (const auto& f : files) {
        const std::string instance = is_dir_ ? dead_letter_instance_from_path(f) : std::string{};
        read_file(f, instance, offsets_[f]);
    }

    cached_.recent.assign(recent_.begin(), recent_.end());
    status.last_success_ms = now_ms();
    return cached_;
}

namespace {

struct ParsedUrl {
    std::string host;
    int port = 80;
    std::string path = "/";
    std::string error;
};

ParsedUrl parse_http_url(const std::string& url) {
    ParsedUrl out;
    const std::string prefix = "http://";
    if (url.rfind(prefix, 0) != 0) {
        out.error = "only http:// metrics URLs are supported";
        return out;
    }
    std::string rest = url.substr(prefix.size());
    size_t slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        out.host = hostport.substr(0, colon);
        out.port = std::atoi(hostport.substr(colon + 1).c_str());
    } else {
        out.host = hostport;
    }
    if (out.host.empty() || out.port <= 0) out.error = "invalid metrics URL";
    return out;
}

struct RulesetParseSummary {
    bool valid = false;
    std::string error;
    int64_t rule_count = 0;
    std::map<std::string, int64_t> operator_counts;
    std::map<std::string, int64_t> action_counts;
    std::map<std::string, int64_t> field_counts;
};

std::string inline_yaml_scalar_after_key(std::string_view value) {
    std::string out = trim(value);
    if (out.empty()) return out;
    if (out.front() == '"' || out.front() == '\'') {
        char quote = out.front();
        size_t end = 1;
        bool escaped = false;
        for (; end < out.size(); ++end) {
            char c = out[end];
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == quote) {
                break;
            }
        }
        if (end < out.size()) out = out.substr(0, end + 1);
    } else {
        size_t stop = out.find_first_of(",}]");
        if (stop != std::string::npos) out = out.substr(0, stop);
    }
    return strip_yaml_scalar(out);
}

RulesetParseSummary summarize_rules_yaml(const std::string& text) {
    RulesetParseSummary out;
    if (trim(text).empty()) {
        out.error = "empty rules file";
        return out;
    }
    bool saw_rules = false;
    auto lines = split_text_lines(text);
    for (const auto& raw : lines) {
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        if (line == "rules:" || line.rfind("rules:", 0) == 0) saw_rules = true;
        if (line.rfind("- id:", 0) == 0 || line.rfind("id:", 0) == 0) out.rule_count += 1;
        auto scan_key = [&](const char* key, std::map<std::string, int64_t>& counts) {
            std::string prefix = std::string(key) + ":";
            size_t pos = line.find(prefix);
            if (pos == std::string::npos) return;
            if (pos > 0 && (std::isalnum(static_cast<unsigned char>(line[pos - 1])) || line[pos - 1] == '_')) return;
            std::string value = inline_yaml_scalar_after_key(line.substr(pos + prefix.size()));
            if (!value.empty() && value.front() != '[' && value.front() != '{') counts[value] += 1;
        };
        scan_key("op", out.operator_counts);
        scan_key("action", out.action_counts);
        scan_key("decision", out.action_counts);
        scan_key("field", out.field_counts);
    }
    if (!saw_rules) {
        out.error = "missing top-level rules:";
        return out;
    }
    if (out.rule_count == 0) {
        out.error = "no rule ids found";
        return out;
    }
    out.valid = true;
    return out;
}

void load_rules_file(const std::string& path, std::string& yaml, bool& valid, std::string& error,
                     int64_t& rule_count, int64_t& bytes, int64_t& modified_ms,
                     std::map<std::string, int64_t>* ops,
                     std::map<std::string, int64_t>* actions,
                     std::map<std::string, int64_t>* fields) {
    std::error_code ec;
    uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        bytes = 0;
        valid = false;
        error = "file not found or not readable";
        return;
    }
    bytes = static_cast<int64_t>(size);
    modified_ms = file_time_ms(path);
    std::ifstream in(path);
    if (!in) {
        valid = false;
        error = "failed to open rules file";
        return;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    yaml = ss.str();
    auto summary = summarize_rules_yaml(yaml);
    valid = summary.valid;
    error = summary.error;
    rule_count = summary.rule_count;
    if (ops) *ops = std::move(summary.operator_counts);
    if (actions) *actions = std::move(summary.action_counts);
    if (fields) *fields = std::move(summary.field_counts);
}

std::vector<DiffRow> diff_rules(const std::string& active, const std::string& candidate, size_t limit) {
    std::vector<DiffRow> out;
    if (active.empty() || candidate.empty()) return out;
    auto a = split_text_lines(active);
    auto b = split_text_lines(candidate);
    size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n && out.size() < limit; ++i) {
        std::string av = i < a.size() ? a[i] : "";
        std::string bv = i < b.size() ? b[i] : "";
        if (av == bv) continue;
        DiffRow row;
        row.line = static_cast<int>(i + 1);
        row.active = av;
        row.candidate = bv;
        row.kind = av.empty() ? "added" : (bv.empty() ? "removed" : "changed");
        out.push_back(std::move(row));
    }
    return out;
}

std::vector<RulesetVersionRow> read_versions(const std::string& dir) {
    std::vector<RulesetVersionRow> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec || !entry.is_regular_file()) continue;
        auto path = entry.path();
        if (path.extension() != ".yaml" && path.extension() != ".yml") continue;
        RulesetVersionRow row;
        row.path = path.string();
        uintmax_t size = fs::file_size(path, ec);
        row.bytes = ec ? 0 : static_cast<int64_t>(size);
        row.modified_ms = file_time_ms(path);
        std::string yaml;
        bool valid = false;
        std::string error;
        int64_t rules = 0;
        int64_t bytes = 0;
        int64_t modified = 0;
        load_rules_file(row.path, yaml, valid, error, rules, bytes, modified, nullptr, nullptr, nullptr);
        row.valid = valid;
        row.error = error;
        row.rule_count = rules;
        out.push_back(std::move(row));
    }
    std::sort(out.begin(), out.end(), [](const RulesetVersionRow& a, const RulesetVersionRow& b) {
        return a.modified_ms > b.modified_ms;
    });
    if (out.size() > 200) out.resize(200);
    return out;
}

} // namespace

PrometheusScraper::PrometheusScraper(std::string url) : url_(std::move(url)) {}

MetricsState PrometheusScraper::update(SourceStatus& status) {
    status.name = "metrics_url";
    status.location = url_;
    status.configured = !url_.empty();
    if (url_.empty()) {
        status.active = false;
        return cached_;
    }
    ParsedUrl parsed = parse_http_url(url_);
    if (!parsed.error.empty()) {
        cached_.last_error = parsed.error;
        status.active = false;
        status.last_error = parsed.error;
        return cached_;
    }
    httplib::Client client(parsed.host, parsed.port);
    client.set_connection_timeout(0, 300000);
    client.set_read_timeout(0, 500000);
    auto res = client.Get(parsed.path);
    if (!res || res->status != 200) {
        std::string err = res ? "metrics endpoint returned HTTP " + std::to_string(res->status)
                              : "metrics endpoint unavailable";
        cached_.last_error = err;
        status.active = false;
        status.last_error = err;
        return cached_;
    }
    MetricsState next;
    std::istringstream in(res->body);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t sp = line.find_first_of(" \t");
        if (sp == std::string::npos) continue;
        std::string lhs = line.substr(0, sp);
        std::string rhs = trim(std::string_view(line).substr(sp + 1));
        char* end = nullptr;
        double value = std::strtod(rhs.c_str(), &end);
        if (end == rhs.c_str()) continue;
        MetricEntry entry;
        size_t brace = lhs.find('{');
        if (brace != std::string::npos) {
            entry.name = lhs.substr(0, brace);
            size_t close = lhs.rfind('}');
            if (close != std::string::npos && close > brace) {
                entry.labels = parse_labels(std::string_view(lhs).substr(brace + 1, close - brace - 1));
            }
        } else {
            entry.name = lhs;
        }
        entry.value = value;
        next.entries.push_back(std::move(entry));
    }
    next.last_success_ms = now_ms();
    cached_ = std::move(next);
    status.active = true;
    status.bytes = res->body.size();
    status.last_error.clear();
    status.last_success_ms = cached_.last_success_ms;
    return cached_;
}

BenchmarkReader::BenchmarkReader(std::string path) : path_(std::move(path)) {}

BenchmarkState BenchmarkReader::update(SourceStatus& status) {
    status.name = "results_jsonl";
    status.location = path_;
    status.configured = !path_.empty();
    if (path_.empty()) {
        status.active = false;
        return cached_;
    }
    std::error_code ec;
    uintmax_t size = fs::file_size(path_, ec);
    if (ec) {
        status.bytes = 0;
        status.active = false;
        status.last_error = "file not found or not readable";
        return cached_;
    }
    status.bytes = size;
    status.active = true;
    status.last_error.clear();
    auto mtime = fs::last_write_time(path_, ec);
    int64_t mtime_count = ec ? 0 : static_cast<int64_t>(mtime.time_since_epoch().count());
    if (size == last_size_ && mtime_count == last_mtime_count_ && !cached_.rows.empty()) {
        status.last_success_ms = cached_.last_success_ms;
        return cached_;
    }
    std::ifstream in(path_);
    if (!in) {
        status.active = false;
        status.last_error = "failed to open results JSONL";
        return cached_;
    }
    std::map<std::string, BenchmarkRow> latest;
    std::string line;
    while (std::getline(in, line)) {
        if (trim(line).empty()) continue;
        BenchmarkRow row;
        row.format = json_string(line, "format").value_or("");
        row.status = json_string(line, "status").value_or("");
        row.error = json_string(line, "error").value_or("");
        row.target_json_bytes = static_cast<int64_t>(json_number(line, "target_json_bytes").value_or(0));
        row.actual_avg_json_bytes = static_cast<int64_t>(json_number(line, "actual_avg_json_bytes").value_or(0));
        row.records = static_cast<int64_t>(json_number(line, "records").value_or(0));
        row.batch_size = static_cast<int64_t>(json_number(line, "batch_size").value_or(0));
        row.matched_records = static_cast<int64_t>(json_number(line, "matched_records").value_or(0));
        row.rule_match_count_entries = static_cast<int64_t>(json_number(line, "rule_match_count_entries").value_or(0));
        row.engine_rps = json_number(line, "blazerules_records_per_sec").value_or(0.0);
        row.end_to_end_rps = json_number(line, "end_to_end_records_per_sec").value_or(0.0);
        row.input_mib_per_sec = json_number(line, "input_mib_per_sec").value_or(0.0);
        row.avg_batch_latency_ms = json_number(line, "avg_batch_latency_ms").value_or(0.0);
        row.p50_ms = json_number(line, "p50_batch_latency_ms").value_or(0.0);
        row.p95_ms = json_number(line, "p95_batch_latency_ms").value_or(0.0);
        row.p99_ms = json_number(line, "p99_batch_latency_ms").value_or(0.0);
        if (row.format.empty()) continue;
        std::string key = row.format + "|" + std::to_string(row.target_json_bytes) + "|" +
                          std::to_string(row.records) + "|" + std::to_string(row.batch_size);
        latest[key] = std::move(row);
    }
    BenchmarkState next;
    for (auto& [_, row] : latest) next.rows.push_back(std::move(row));
    std::sort(next.rows.begin(), next.rows.end(), [](const BenchmarkRow& a, const BenchmarkRow& b) {
        return std::tie(a.format, a.target_json_bytes, a.records, a.batch_size) <
               std::tie(b.format, b.target_json_bytes, b.records, b.batch_size);
    });
    next.last_success_ms = now_ms();
    cached_ = std::move(next);
    last_size_ = size;
    last_mtime_count_ = mtime_count;
    status.last_success_ms = cached_.last_success_ms;
    return cached_;
}

RulesetReader::RulesetReader(std::string active_path, std::string rules_dir,
                             std::string candidate_path, std::string history_dir)
    : active_path_(std::move(active_path)),
      rules_dir_(std::move(rules_dir)),
      candidate_path_(std::move(candidate_path)),
      history_dir_(std::move(history_dir)) {}

std::string RulesetReader::resolve_selector_path(const std::string& selector) const {
    if (rules_dir_.empty()) {
        if (active_path_.empty()) return "";
        return blazerules::is_s3_uri(active_path_)
            ? blazerules::resolve_resource_to_local(active_path_) : active_path_;
    }
    std::error_code ec;
    std::vector<std::string> yamls;
    if (fs::is_directory(rules_dir_, ec)) {
        for (const auto& entry : fs::directory_iterator(rules_dir_, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            const std::string ext = entry.path().extension().string();
            if (ext == ".yaml" || ext == ".yml") yamls.push_back(entry.path().string());
        }
    }
    if (yamls.empty()) return "";
    std::sort(yamls.begin(), yamls.end());
    if (selector.empty()) return yamls.front();
    std::string sel;
    for (char c : selector) sel += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& y : yamls) {
            std::string stem = fs::path(y).stem().string();
            std::string low;
            for (char c : stem) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if ((pass == 0 && low == sel) || (pass == 1 && low.find(sel) != std::string::npos)) return y;
        }
    }
    return yamls.front();
}

std::vector<std::string> RulesetReader::ruleset_names() const {
    std::vector<std::string> names;
    if (rules_dir_.empty()) return names;
    std::error_code ec;
    if (fs::is_directory(rules_dir_, ec)) {
        for (const auto& entry : fs::directory_iterator(rules_dir_, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            const std::string ext = entry.path().extension().string();
            if (ext == ".yaml" || ext == ".yml") names.push_back(entry.path().stem().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

RulesetState RulesetReader::ruleset_for(const std::string& selector) const {
    RulesetState next;
    const std::string path = resolve_selector_path(selector);
    next.active_path = path;
    next.active_configured = !path.empty();
    if (!path.empty()) {
        load_rules_file(path, next.active_yaml, next.active_valid, next.active_error,
                        next.active_rule_count, next.active_bytes, next.active_modified_ms,
                        &next.operator_counts, &next.action_counts, &next.field_counts);
    }
    return next;
}

RulesetState RulesetReader::update(SourceStatus& status) {
    status.name = "ruleset";
    const std::string active = resolve_selector_path("");
    status.location = active;
    status.configured = !active.empty();
    RulesetState next;
    next.active_path = active;
    next.candidate_path = candidate_path_;
    next.history_dir = history_dir_;
    next.active_configured = !active.empty();
    next.candidate_configured = !candidate_path_.empty();
    if (!active.empty()) {
        load_rules_file(active, next.active_yaml, next.active_valid, next.active_error,
                        next.active_rule_count, next.active_bytes, next.active_modified_ms,
                        &next.operator_counts, &next.action_counts, &next.field_counts);
        status.active = next.active_valid;
        status.bytes = static_cast<uintmax_t>(std::max<int64_t>(0, next.active_bytes));
        status.last_error = next.active_error;
        if (next.active_valid) status.last_success_ms = now_ms();
    } else {
        status.active = false;
    }
    if (!candidate_path_.empty()) {
        load_rules_file(candidate_path_, next.candidate_yaml, next.candidate_valid, next.candidate_error,
                        next.candidate_rule_count, next.candidate_bytes, next.candidate_modified_ms,
                        nullptr, nullptr, nullptr);
        next.diff_rows = diff_rules(next.active_yaml, next.candidate_yaml, 500);
    }
    if (!history_dir_.empty()) next.versions = read_versions(history_dir_);
    cached_ = std::move(next);
    return cached_;
}
