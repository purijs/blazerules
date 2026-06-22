#include "blazerules/mmap_reader.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>

#include <simdjson.h>

MmappedNdjson::MmappedNdjson(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        error_ = "open failed: " + std::string(std::strerror(errno));
        return;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        error_ = "fstat failed: " + std::string(std::strerror(errno));
        ::close(fd);
        return;
    }
    const size_t size = static_cast<size_t>(st.st_size);
    if (size == 0) {
        ::close(fd);
        return;  // empty file -> ok(), empty view
    }

    long page = ::sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 16384;  // arm64 macOS page size
    const size_t page_sz = static_cast<size_t>(page);
    // Bytes of zero-filled tail available in the last mapped page beyond `size`.
    const size_t tail = (size % page_sz == 0) ? 0 : (page_sz - (size % page_sz));

    if (tail >= simdjson::SIMDJSON_PADDING) {
        void* p = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (p != MAP_FAILED) {
            data_ = static_cast<const char*>(p);
            logical_size_ = size;
            mapped_size_ = size;  // munmap rounds this up to the page
            return;
        }
        // mmap failed -> fall through to the heap fallback (reopen below)
    } else {
        ::close(fd);
    }

    // Fallback (rare page-aligned size, or mmap failure): one padded heap copy.
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error_ = "reopen for fallback failed";
        return;
    }
    fallback_.resize(size + simdjson::SIMDJSON_PADDING, '\0');
    in.read(fallback_.data(), static_cast<std::streamsize>(size));
    if (static_cast<size_t>(in.gcount()) != size) {
        error_ = "short read in fallback";
        fallback_.clear();
        return;
    }
    data_ = fallback_.data();
    logical_size_ = size;
    mapped_size_ = 0;
}

MmappedNdjson::~MmappedNdjson() {
    if (mapped_size_ > 0 && data_ != nullptr) {
        ::munmap(const_cast<char*>(data_), mapped_size_);
    }
}
