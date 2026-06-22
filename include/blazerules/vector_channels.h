#ifndef BLAZERULES_VECTOR_CHANNELS_H
#define BLAZERULES_VECTOR_CHANNELS_H

#include <vector>

#include <arrow/api.h>

#include "kernel_sequence.h"

// Compute each vector-similarity channel over the batch, writing one scalar-per-row
// column into out[c] (out[c].size() == batch.num_rows()). The embedding is gathered from
// each channel's dim_col_indices and compared (cosine / L2 / dot) against the channel's
// constant reference via the NEON kernels in simd_vec.h. Empty channels -> no work.
void compute_vector_channels(const arrow::RecordBatch& batch,
                             const std::vector<VectorChannelSpec>& channels,
                             std::vector<std::vector<double>>& out);

#endif // BLAZERULES_VECTOR_CHANNELS_H
