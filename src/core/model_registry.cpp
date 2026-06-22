#include "blazerules/model_registry.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include "blazerules/resource_resolver.h"

#ifdef BLAZERULES_ENABLE_ONNX
#include <array>
#include <onnxruntime/onnxruntime_cxx_api.h>
#endif

struct CompiledModel {
    int n_features = -1;
    int num_outputs = 0;
#ifdef BLAZERULES_ENABLE_ONNX
    mutable Ort::Session session{nullptr};
    std::vector<std::string> input_names;
    std::vector<std::string> output_names;
    std::vector<const char*> input_name_ptrs;
    std::vector<const char*> output_name_ptrs;
#endif
};

namespace {

#ifdef BLAZERULES_ENABLE_ONNX

struct FeatureReader {
    arrow::Type::type type = arrow::Type::NA;
    const uint8_t* values = nullptr;
    const uint8_t* validity = nullptr;
    int64_t offset = 0;

    bool is_null(int64_t row) const {
        if (!validity) return false;
        int64_t r = offset + row;
        return ((validity[r >> 3] >> (r & 7)) & 1u) == 0;
    }
    float value(int64_t row) const {
        int64_t r = offset + row;
        switch (type) {
            case arrow::Type::FLOAT:  return reinterpret_cast<const float*>(values)[r];
            case arrow::Type::DOUBLE: return static_cast<float>(reinterpret_cast<const double*>(values)[r]);
            case arrow::Type::INT32:  return static_cast<float>(reinterpret_cast<const int32_t*>(values)[r]);
            case arrow::Type::INT64:  return static_cast<float>(reinterpret_cast<const int64_t*>(values)[r]);
            default: return 0.0f;
        }
    }
};

FeatureReader make_feature_reader(const std::shared_ptr<arrow::Array>& arr) {
    FeatureReader rd;
    if (!arr) return rd;
    const auto& ad = arr->data();
    rd.type = arr->type_id();
    rd.values = ad->buffers.size() > 1 && ad->buffers[1] ? ad->buffers[1]->data() : nullptr;
    bool has_nulls = ad->GetNullCount() > 0;
    rd.validity = has_nulls && !ad->buffers.empty() && ad->buffers[0] ? ad->buffers[0]->data() : nullptr;
    rd.offset = ad->offset;
    return rd;
}

Ort::Env& ort_env() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "blazerules");
    return env;
}

#endif  // BLAZERULES_ENABLE_ONNX

}  // namespace

void ModelRegistry::register_model(const std::string& name, const std::string& path) {
#ifndef BLAZERULES_ENABLE_ONNX
    (void)name;
    (void)path;
    throw std::runtime_error(
        "register_model / model_score requires building with BLAZERULES_ENABLE_ONNX");
#else
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(1);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    auto model = std::make_shared<CompiledModel>();
    std::string local_path = blazerules::resolve_resource_to_local(path);
    try {
        model->session = Ort::Session(ort_env(), local_path.c_str(), opts);
    } catch (const Ort::Exception& e) {
        throw std::runtime_error("model '" + name + "': failed to load ONNX: " + e.what());
    }

    Ort::AllocatorWithDefaultOptions alloc;
    size_t n_in = model->session.GetInputCount();
    size_t n_out = model->session.GetOutputCount();
    if (n_in < 1 || n_out < 1) {
        throw std::runtime_error("model '" + name + "': needs at least one input and one output");
    }
    for (size_t i = 0; i < n_in; ++i) {
        auto nm = model->session.GetInputNameAllocated(i, alloc);
        model->input_names.emplace_back(nm.get());
    }
    for (size_t i = 0; i < n_out; ++i) {
        auto nm = model->session.GetOutputNameAllocated(i, alloc);
        model->output_names.emplace_back(nm.get());
    }
    model->num_outputs = static_cast<int>(n_out);

    auto shape = model->session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    model->n_features = (shape.size() >= 2) ? static_cast<int>(shape[1]) : -1;

    for (const auto& s : model->input_names) model->input_name_ptrs.push_back(s.c_str());
    for (const auto& s : model->output_names) model->output_name_ptrs.push_back(s.c_str());

    std::lock_guard<std::mutex> lock(mutex_);
    models_[name] = std::move(model);
#endif
}

bool ModelRegistry::contains(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return models_.find(name) != models_.end();
}

std::shared_ptr<const CompiledModel> ModelRegistry::get(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = models_.find(name);
    return it == models_.end() ? nullptr : it->second;
}

int ModelRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(models_.size());
}

void ModelRegistry::score_all_channels(const arrow::RecordBatch& batch,
                                       const std::vector<ModelChannelSpec>& channels,
                                       std::vector<std::vector<double>>& out) const {
    const int64_t n = batch.num_rows();
    out.assign(channels.size(), std::vector<double>(static_cast<size_t>(n), 0.0));
    if (channels.empty() || n == 0) return;

#ifndef BLAZERULES_ENABLE_ONNX
    (void)batch;
    return;
#else
    for (size_t c = 0; c < channels.size(); ++c) {
        const ModelChannelSpec& ch = channels[c];
        std::shared_ptr<const CompiledModel> model = get(ch.model_name);
        if (!model) continue;
        const int F = static_cast<int>(ch.feature_col_indices.size());
        if (model->n_features >= 0 && model->n_features != F) continue;

        std::vector<FeatureReader> readers(static_cast<size_t>(F));
        for (int f = 0; f < F; ++f) {
            readers[f] = make_feature_reader(batch.column(ch.feature_col_indices[f]));
        }

        std::vector<float> matrix(static_cast<size_t>(n) * static_cast<size_t>(F));
        tbb::parallel_for(tbb::blocked_range<int64_t>(0, n, 8192),
                          [&](const tbb::blocked_range<int64_t>& r) {
                              for (int64_t row = r.begin(); row < r.end(); ++row) {
                                  float* dst = matrix.data() + row * F;
                                  for (int f = 0; f < F; ++f) {
                                      dst[f] = readers[f].is_null(row)
                                          ? std::numeric_limits<float>::quiet_NaN()
                                          : readers[f].value(row);
                                  }
                              }
                          });

        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::array<int64_t, 2> in_shape{n, static_cast<int64_t>(F)};
        Ort::Value input = Ort::Value::CreateTensor<float>(
            mem, matrix.data(), matrix.size(), in_shape.data(), in_shape.size());

        int oi = ch.output_index;
        if (oi < 0 || oi >= model->num_outputs) oi = 0;
        const char* in_names[1] = {model->input_name_ptrs[0]};
        const char* out_names[1] = {model->output_name_ptrs[oi]};

        std::vector<Ort::Value> outputs;
        try {
            outputs = model->session.Run(Ort::RunOptions{nullptr}, in_names, &input, 1, out_names, 1);
        } catch (const Ort::Exception&) {
            continue;
        }
        if (outputs.empty() || !outputs[0].IsTensor()) continue;

        auto info = outputs[0].GetTensorTypeAndShapeInfo();
        int64_t total = info.GetElementCount();
        int width = (n > 0) ? static_cast<int>(total / n) : 1;
        if (width < 1) width = 1;
        const float* od = outputs[0].GetTensorMutableData<float>();
        double* out_c = out[c].data();
        for (int64_t row = 0; row < n; ++row) {
            out_c[row] = static_cast<double>(od[row * width + (width - 1)]);
        }
    }
#endif
}
