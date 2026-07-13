#ifndef BLAZERULES_MODEL_REGISTRY_H
#define BLAZERULES_MODEL_REGISTRY_H

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <arrow/api.h>

#include "kernel_sequence.h"

struct CompiledModel;

class ModelRegistry {
public:
    void register_model(const std::string& name, const std::string& path);
    void set_intra_op_threads(int threads);
    void share_models_from(const ModelRegistry& other);

    bool contains(const std::string& name) const;
    int size() const;

    void score_all_channels(const arrow::RecordBatch& batch,
                            const std::vector<ModelChannelSpec>& channels,
                            std::vector<std::vector<double>>& out) const;

private:
    std::shared_ptr<const CompiledModel> get(const std::string& name) const;

    mutable std::mutex mutex_;
    int intra_op_threads_ = 1;
    absl::flat_hash_map<std::string, std::shared_ptr<const CompiledModel>> models_;
};

#endif // BLAZERULES_MODEL_REGISTRY_H
