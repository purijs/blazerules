#include "blazerules/vector_channels.h"

#include <cmath>
#include <vector>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include "blazerules/simd_vec.h"

namespace {

// Typed column reader resolved once per embedding dimension column.
struct DimReader {
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

DimReader make_dim_reader(const std::shared_ptr<arrow::Array>& arr) {
    DimReader rd;
    if (!arr) return rd;
    const auto& ad = arr->data();
    rd.type = arr->type_id();
    rd.values = ad->buffers.size() > 1 && ad->buffers[1] ? ad->buffers[1]->data() : nullptr;
    bool has_nulls = ad->GetNullCount() > 0;
    rd.validity = has_nulls && !ad->buffers.empty() && ad->buffers[0] ? ad->buffers[0]->data() : nullptr;
    rd.offset = ad->offset;
    return rd;
}

}  // namespace

void compute_vector_channels(const arrow::RecordBatch& batch,
                             const std::vector<VectorChannelSpec>& channels,
                             std::vector<std::vector<double>>& out) {
    const int64_t n = batch.num_rows();
    out.assign(channels.size(), std::vector<double>(static_cast<size_t>(n), 0.0));
    if (channels.empty() || n == 0) return;

    for (size_t c = 0; c < channels.size(); ++c) {
        const VectorChannelSpec& ch = channels[c];
        const int D = static_cast<int>(ch.dim_col_indices.size());
        if (D == 0 || static_cast<int>(ch.reference.size()) != D) continue;  // bad config -> zeros

        std::vector<DimReader> readers(static_cast<size_t>(D));
        for (int d = 0; d < D; ++d) {
            readers[d] = make_dim_reader(batch.column(ch.dim_col_indices[d]));
        }
        const float* ref = ch.reference.data();
        const float inv_norm = ch.reference_inv_norm;
        const int metric = ch.metric;
        double* out_c = out[c].data();

        tbb::parallel_for(tbb::blocked_range<int64_t>(0, n, 8192),
                          [&](const tbb::blocked_range<int64_t>& r) {
            std::vector<float> emb(static_cast<size_t>(D));  // one alloc per task
            for (int64_t row = r.begin(); row < r.end(); ++row) {
                for (int d = 0; d < D; ++d) {
                    emb[d] = readers[d].is_null(row) ? 0.0f : readers[d].value(row);
                }
                float v;
                if (metric == 0) {        // COSINE similarity
                    v = blazerules::cosine_f32(emb.data(), ref, D, inv_norm);
                } else if (metric == 1) { // L2 distance
                    v = std::sqrt(blazerules::l2sq_f32(emb.data(), ref, D));
                } else {                  // DOT product
                    v = blazerules::dot_f32(emb.data(), ref, D);
                }
                out_c[row] = static_cast<double>(v);
            }
        });
    }
}
