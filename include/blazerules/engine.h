#ifndef BLAZERULES_ENGINE_H
#define BLAZERULES_ENGINE_H

#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arrow/api.h>

#include "batch_result.h"
#include "conflict.h"
#include "dict_encoder.h"
#include "kernel_sequence.h"
#include "model_registry.h"
#include "orchestrator.h"
#include "result.h"
#include "rule_spec.h"
#include "schema.h"
#include "transposer.h"
#include "window_store.h"

enum class RuleFileFormat { YAML, JSON };

struct EngineConfig {
    int batch_size = 10000;

    enum IngestErrorMode { SKIP_AND_COUNT, SKIP_TO_DEAD_LETTER, HARD_FAIL };
    IngestErrorMode ingest_error_mode = SKIP_AND_COUNT;

    enum TypeMismatchMode { NULL_ON_TYPE_ERROR, COERCE, HARD_FAIL_TYPE };
    TypeMismatchMode type_mismatch_mode = NULL_ON_TYPE_ERROR;

    int eval_thread_count = 0;
    int parallel_threshold = 1000;
    // ONNX intra-op threads per model session. Default 1 avoids oversubscription
    // when many shards each run inference concurrently; raise for a single-engine
    // ONNX-heavy workload to let one Run() use idle cores.
    int model_intra_op_threads = 1;

    enum TraceMode { TRACE_NONE, TRACE_SAMPLED, TRACE_ALL };
    TraceMode trace_mode = TRACE_NONE;
    double trace_sample_rate = 0.05;

    enum OutputDetail { OUTPUT_COUNTS, OUTPUT_CODES, OUTPUT_DECISIONS, OUTPUT_BITMASKS };
    OutputDetail output_detail = OUTPUT_BITMASKS;

    size_t max_window_entities = 10000000;
    std::chrono::minutes eviction_sweep_interval = std::chrono::minutes(5);
    size_t arena_size_bytes = 8 * 1024 * 1024;
    int max_dict_size_per_column = 100000;

    bool enable_selection_vectors = true;
    double selection_vector_threshold = 0.20;
    bool enable_adaptive_predicate_ordering = true;
    bool enable_no_validity_fast_path = true;
    bool enable_prefetch = false;
    bool enable_thread_affinity = false;
    bool result_buffer_reuse = true;
    std::string simd_backend_override = "auto";
    bool enable_avx512 = false;

    int hot_reload_poll_seconds = 5;
    bool hot_reload_validate_conflicts = true;
    bool hot_reload_keep_previous_on_failure = true;
    int max_error_samples = 16;
    std::string decision_log_path;
    std::string dead_letter_path;
};

struct EngineStats {
    int64_t batches_evaluated = 0;
    int64_t records_evaluated = 0;
    int64_t records_skipped = 0;
};

struct HotReloadStatus {
    std::string active_version;
    std::string pending_path;
    int64_t last_attempt_ms = 0;
    int64_t last_success_ms = 0;
    int64_t reload_count = 0;
    int64_t failed_reload_count = 0;
    std::string last_error_code;
    std::string last_error_message;
};

struct BacktestConfig {
    std::vector<std::string> parquet_paths;
    std::string rules_file_a;
    std::string rules_file_b;
    std::string label_column;
    int batch_size = 500000;
};

using MetricLabels = std::vector<std::pair<std::string, std::string>>;

class MetricsEmitter {
public:
    virtual void increment_counter(std::string_view name, int64_t delta,
        const MetricLabels& labels) = 0;
    virtual void observe_histogram(std::string_view name, double value,
        const MetricLabels& labels) = 0;
    virtual void set_gauge(std::string_view name, double value,
        const MetricLabels& labels) = 0;
    // When false, the engine skips the per-batch metrics work entirely (the default
    // Noop path pays nothing — not even the per-rule loop).
    virtual bool enabled() const { return false; }
    virtual ~MetricsEmitter() = default;
};

class NoopMetricsEmitter : public MetricsEmitter {
public:
    void increment_counter(std::string_view, int64_t, const MetricLabels&) override {}
    void observe_histogram(std::string_view, double, const MetricLabels&) override {}
    void set_gauge(std::string_view, double, const MetricLabels&) override {}
    bool enabled() const override { return false; }
};

struct HistogramSnapshot {
    int64_t count = 0;
    double sum = 0.0;
    double min = 0.0;
    double max = 0.0;
    double mean() const { return count > 0 ? sum / static_cast<double>(count) : 0.0; }
};

// In-process metrics sink with zero external dependency. Accumulates counters, gauges,
// and (count/sum/min/max) histograms keyed by "name{label=value,...}", thread-safe.
// A Prometheus HTTP exporter is a thin wrapper over these snapshots.
class CollectingMetricsEmitter : public MetricsEmitter {
public:
    void increment_counter(std::string_view name, int64_t delta, const MetricLabels& labels) override {
        std::lock_guard<std::mutex> lk(mu_);
        counters_[make_key(name, labels)] += delta;
    }
    void observe_histogram(std::string_view name, double value, const MetricLabels& labels) override {
        std::lock_guard<std::mutex> lk(mu_);
        HistogramSnapshot& h = histograms_[make_key(name, labels)];
        if (h.count == 0) { h.min = value; h.max = value; }
        else { if (value < h.min) h.min = value; if (value > h.max) h.max = value; }
        h.count += 1;
        h.sum += value;
    }
    void set_gauge(std::string_view name, double value, const MetricLabels& labels) override {
        std::lock_guard<std::mutex> lk(mu_);
        gauges_[make_key(name, labels)] = value;
    }
    bool enabled() const override { return true; }

    std::map<std::string, int64_t> counters() const {
        std::lock_guard<std::mutex> lk(mu_); return counters_;
    }
    std::map<std::string, double> gauges() const {
        std::lock_guard<std::mutex> lk(mu_); return gauges_;
    }
    std::map<std::string, HistogramSnapshot> histograms() const {
        std::lock_guard<std::mutex> lk(mu_); return histograms_;
    }
    void reset() {
        std::lock_guard<std::mutex> lk(mu_);
        counters_.clear(); gauges_.clear(); histograms_.clear();
    }

private:
    static std::string make_key(std::string_view name, const MetricLabels& labels) {
        std::string key(name);
        if (!labels.empty()) {
            key += '{';
            for (size_t i = 0; i < labels.size(); ++i) {
                if (i) key += ',';
                key += labels[i].first;
                key += '=';
                key += labels[i].second;
            }
            key += '}';
        }
        return key;
    }
    mutable std::mutex mu_;
    std::map<std::string, int64_t> counters_;
    std::map<std::string, double> gauges_;
    std::map<std::string, HistogramSnapshot> histograms_;
};

class RuleEngine {
public:
    enum class SchemaState { UNBOUND, INFERRED_BOUND, USER_BOUND };

    explicit RuleEngine(EngineConfig config = {});
    RuleEngine(std::vector<FieldSpec> fields, EngineConfig config = {});
    static BlazeRulesResult<std::unique_ptr<RuleEngine>> create(BlazeRulesSchema schema, EngineConfig config);

    RuleEngine(const RuleEngine&) = delete;
    RuleEngine& operator=(const RuleEngine&) = delete;
    ~RuleEngine();

    ConflictReport load_rules(const std::string& rules_path);
    ConflictReport load_rules_from_string(const std::string& rules_yaml_or_json,
                                          RuleFileFormat format = RuleFileFormat::YAML);
    ConflictReport reload_rules_now(const std::string& rules_path);
    ConflictReport analyze_conflicts(const std::string& rules_path);
    std::string active_rule_set_version() const;
    HotReloadStatus hot_reload_status() const;

    void enable_hot_reload(const std::string& rules_file_path,
                           std::chrono::seconds poll_interval = std::chrono::seconds(5));
    void stop_hot_reload();

    BatchResult evaluate_messages(const std::vector<std::string>& messages);
    BatchResult evaluate_message_views(const std::vector<std::string_view>& messages);
    BatchResult evaluate_ndjson(std::string_view ndjson_bytes);
    BatchResult evaluate_ndjson_padded(std::string_view ndjson_bytes);
    BatchResult evaluate_json_array(std::string_view json_bytes);
    BatchResult evaluate_json_array_padded(std::string_view json_bytes);
    BatchResult evaluate_record_batch(const std::shared_ptr<arrow::RecordBatch>& batch);
    BatchResult evaluate_batch(const std::shared_ptr<arrow::RecordBatch>& batch) {
        return evaluate_record_batch(batch);
    }
    void evaluate_messages_into(const std::vector<std::string>& messages, BatchResult& out);
    void evaluate_message_views_into(const std::vector<std::string_view>& messages, BatchResult& out);
    void evaluate_ndjson_into(std::string_view ndjson_bytes, BatchResult& out);
    void evaluate_ndjson_padded_into(std::string_view ndjson_bytes, BatchResult& out);
    void evaluate_json_array_into(std::string_view json_bytes, BatchResult& out);
    void evaluate_json_array_padded_into(std::string_view json_bytes, BatchResult& out);
    void evaluate_record_batch_into(const std::shared_ptr<arrow::RecordBatch>& batch, BatchResult& out);

    std::vector<std::unique_ptr<RuleEngine>> create_shards(int shard_count) const;
    BatchResult evaluate_partition(int partition_id, const std::vector<std::string>& messages);
    BatchResult evaluate_partition(int partition_id, const std::vector<std::string_view>& messages);
    BatchResult evaluate_partition_ndjson_padded(int partition_id, std::string_view ndjson_bytes);
    BatchResult evaluate_partition(int partition_id, const std::shared_ptr<arrow::RecordBatch>& batch);

    BacktestReport backtest(const BacktestConfig& config);
    BacktestReport backtest(const std::vector<std::string>& parquet_paths,
                            const std::string& rules_a,
                            const std::string& rules_b,
                            const std::string& label_column = {});

    void reset_window_state();

    // Register a native linear/logistic model (Tier A ML scoring). `weights` has one
    // entry per feature in the order a rule's model_score `features:` list references.
    // Models are looked up by name at score time, so this may be called before or after
    // load_rules and re-registration hot-swaps the model.
    // Register an ONNX model under `name` from a filesystem path, used by `model_score`
    // rules. ONNX is the single ML backend (XGBoost/LightGBM/sklearn/NN all export to it).
    // Requires an ONNX-enabled build (BLAZERULES_ENABLE_ONNX, default ON); throws otherwise
    // or on load failure. Re-registration hot-swaps the model atomically.
    void register_model(const std::string& name, const std::string& path);
    int num_models() const { return model_registry_.size(); }

    // mmap an NDJSON file and evaluate it zero-copy (the reader guarantees the simdjson
    // trailing-padding the parser needs). Offline/batch replay without a Python read.
    BatchResult evaluate_ndjson_file(const std::string& path);

    // In-process metrics (zero external dependency). After enable_metrics() the engine
    // accumulates per-batch counters/histograms/gauges readable via the snapshot
    // accessors; a Prometheus exporter is a thin wrapper over these.
    void enable_metrics();
    void reset_metrics();
    bool metrics_enabled() const { return collecting_metrics_ != nullptr; }
    std::map<std::string, int64_t> metrics_counters() const;
    std::map<std::string, double> metrics_gauges() const;
    std::map<std::string, HistogramSnapshot> metrics_histograms() const;

    EngineStats stats() const { return stats_; }
    void set_metrics_emitter(std::shared_ptr<MetricsEmitter> emitter);
    void emit_batch_metrics(const BatchResult& result);

    const BlazeRulesSchema& schema() const { return schema_; }
    bool schema_bound() const { return schema_state_ != SchemaState::UNBOUND; }
    SchemaState schema_state() const { return schema_state_; }
    int num_window_channels() const;
    std::vector<std::pair<std::string, std::string>> model_channel_columns() const;

private:
    BatchResult evaluate_internal(const std::shared_ptr<arrow::RecordBatch>& base,
                                  int messages_processed,
                                  int messages_skipped,
                                  const std::string& last_ingest_error);
    void evaluate_internal_into(const std::shared_ptr<arrow::RecordBatch>& base,
                                int messages_processed,
                                int messages_skipped,
                                const std::string& last_ingest_error,
                                BatchResult& result);
    ResolvedKernelBindings build_resolved(const CompiledRuleSet& ruleset) const;
    void configure_projection(const CompiledRuleSet& ruleset);
    EvalOptions eval_options() const;
    void install_ruleset(std::shared_ptr<CompiledRuleSet> next);
    // Deactivate the active ruleset on the main engine and every partition shard,
    // consistently (under each shard's lock). Used when a hot reload fails and the
    // engine is configured not to keep the previous ruleset, so shards do not keep
    // evaluating stale rules while the main path has none.
    void clear_ruleset();
    ConflictReport compile_and_install_rules(const std::string& rules_path, bool update_reload_status);
    ConflictReport install_or_defer_rules(ParseFileResult parsed, bool update_reload_status,
                                          const std::string& rules_path);
    void bind_schema(BlazeRulesSchema schema, SchemaState state);
    void compile_pending_rules_after_bind();
    void ensure_schema_bound_from_messages(const std::vector<std::string_view>& messages);
    void ensure_schema_bound_from_ndjson(std::string_view ndjson_bytes);
    void ensure_schema_bound_from_json_array(std::string_view json_bytes, bool padded);
    void ensure_schema_bound_from_arrow(const std::shared_ptr<arrow::RecordBatch>& batch);
    std::shared_ptr<arrow::RecordBatch> align_record_batch_to_schema(
        const std::shared_ptr<arrow::RecordBatch>& batch) const;
    void preload_lookup_dictionaries(const CompiledRuleSet& ruleset);
    void validate_engine_config() const;
    void validate_record_batch_schema(const std::shared_ptr<arrow::RecordBatch>& batch) const;
    void apply_ingest_error_policy(const BatchTransposer& transposer, BatchResult& result);
    void emit_decision_log(const BatchResult& result);
    void emit_dead_letter_log(const BatchResult& result);
    void ensure_partition_shards(int shard_count);
    void hot_reload_loop(std::string rules_file_path, std::chrono::seconds poll_interval);
    std::shared_ptr<CompiledRuleSet> active_ruleset() const;

    BlazeRulesSchema schema_;
    SchemaState schema_state_ = SchemaState::UNBOUND;
    EngineConfig config_;
    std::unique_ptr<DictEncoder> dict_encoder_;
    std::unique_ptr<BatchTransposer> transposer_;
    WindowStore window_store_;
    ModelRegistry model_registry_;
    std::shared_ptr<CompiledRuleSet> ruleset_;
    std::optional<RuleFileSpec> pending_rules_;
    std::string pending_rules_path_;
    ConflictReport pending_conflict_report_;
    std::shared_ptr<MetricsEmitter> metrics_emitter_;
    std::shared_ptr<CollectingMetricsEmitter> collecting_metrics_;
    EngineStats stats_;
    std::vector<std::unique_ptr<RuleEngine>> partition_shards_;
    mutable std::shared_mutex state_mutex_;
    mutable std::mutex reload_status_mutex_;
    HotReloadStatus hot_reload_status_;
    std::unique_ptr<std::ofstream> decision_log_stream_;
    std::unique_ptr<std::ofstream> dead_letter_stream_;

    std::atomic<bool> hot_reload_stop_{false};
    std::thread hot_reload_thread_;
};

#endif // BLAZERULES_ENGINE_H
