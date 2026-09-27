// SPDX-License-Identifier: MIT
// Rust: storage/util/src/mmap_region.rs
#include "agentenv/storage/util/mmap_region.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace agentenv {
namespace storage {
namespace util {

core::Expected<MMapRegion, std::string>
MMapRegion::Open(const std::string& path, bool writable) {
    int flags = writable ? O_RDWR : O_RDONLY;
    int fd = ::open(path.c_str(), flags);
    if (fd < 0) return core::make_unexpected(std::string("open failed: ") + path);
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return core::make_unexpected(std::string("fstat failed"));
  }
    size_t len = static_cast<size_t>(st.st_size);
    int prot = writable ? (PROT_READ | PROT_WRITE) : PROT_READ;
    void* p = len ? ::mmap(nullptr, len, prot, MAP_SHARED, fd, 0) : nullptr;
    ::close(fd);
    if (len && p == MAP_FAILED) {
        return core::make_unexpected(std::string("mmap failed"));
    }
    MMapRegion r;
  r.base_ = static_cast<uint8_t*>(p);
    r.len_ = len;
    return core::Expected<MMapRegion, std::string>(std::move(r));
}

MMapRegion::MMapRegion(MMapRegion&& o) noexcept : base_(o.base_), len_(o.len_) {
    o.base_ = nullptr;
    o.len_ = 0;
}
MMapRegion& MMapRegion::operator=(MMapRegion&& o) noexcept {
    if (this != &o) {
        if (base_ && len_) ::munmap(base_, len_);
        base_ = o.base_;
    len_ = o.len_;
      o.base_ = nullptr;
        o.len_ = 0;
    }
    return *this;
}
MMapRegion::~MMapRegion() {
    if (base_ && len_) ::munmap(base_, len_);
}

core::Expected<const uint8_t*, std::string>
MMapRegion::Slice(size_t offset, size_t len) const {
    if (offset + len > len_) {
        return core::make_unexpected(std::string("mmap slice out of bounds"));
    }
    return base_ + offset;
}

}  // namespace util
}  // namespace storage
}  // namespace agentenv
