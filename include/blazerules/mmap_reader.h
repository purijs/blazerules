#ifndef BLAZERULES_MMAP_READER_H
#define BLAZERULES_MMAP_READER_H

#include <cstddef>
#include <string>
#include <string_view>

// RAII reader that memory-maps an NDJSON file for zero-copy parsing. simdjson's
// vectorized scan reads up to SIMDJSON_PADDING bytes PAST the logical end, so the
// returned buffer is guaranteed to have at least that much readable trailing space:
// mmap maps whole pages and the kernel zero-fills the partial last page, which provides
// the padding for free in the common case. For the rare page-aligned file (no room in
// the last page) or if mmap fails, it falls back to a single padded heap copy. The view
// is therefore always safe to pass to RuleEngine::evaluate_ndjson_padded.
class MmappedNdjson {
public:
    explicit MmappedNdjson(const std::string& path);
    ~MmappedNdjson();
    MmappedNdjson(const MmappedNdjson&) = delete;
    MmappedNdjson& operator=(const MmappedNdjson&) = delete;

    bool ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }
    std::string_view view() const { return std::string_view(data_, logical_size_); }

private:
    const char* data_ = nullptr;
    size_t logical_size_ = 0;
    size_t mapped_size_ = 0;   // non-zero only when an mmap (not the heap fallback) is held
    std::string fallback_;     // padded heap copy used when mmap can't guarantee padding
    std::string error_;
};

#endif // BLAZERULES_MMAP_READER_H
