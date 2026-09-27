// SPDX-License-Identifier: MIT
// A minimal `pread`-based LayerReader. io_uring path guarded behind
// AGENTENV_WITH_LIBURING (not shown in the skeleton).
#include "agentenv/storage/overlaybd/types.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <memory>

namespace agentenv {
namespace storage {
namespace overlaybd {

class FdLayerReader final : public LayerReader {
 public:
    FdLayerReader(int fd, IndexTree idx) : fd_(fd), idx_(std::move(idx)) {}
    ~FdLayerReader() override { if (fd_ >= 0) ::close(fd_); }

    const IndexTree& Index() const override { return idx_; }

    core::Expected<int64_t, core::AnyError>
    ReadAt(uint64_t vaddr, void* dst, int64_t len) override {
        // Walk mappings, translate + issue pread per segment.
        int64_t written = 0;
        while (len > 0) {
            auto seg_res = idx_.Lookup(vaddr);
            if (!seg_res.ok()) return core::make_unexpected(seg_res.take_error());
            const SegmentMapping* seg = seg_res.value();
            uint64_t offset_in_seg = vaddr - seg->vaddr;
            int64_t  can_read = static_cast<int64_t>(seg->length - offset_in_seg);
            if (can_read > len) can_read = len;
            ssize_t rc = ::pread(fd_,
                                 static_cast<char*>(dst) + written,
                                 static_cast<size_t>(can_read),
                                 static_cast<off_t>(seg->poffset + offset_in_seg));
            if (rc < 0) return core::make_unexpected(core::err("pread failed"));
            written += rc;
            vaddr   += rc;
            len     -= rc;
            if (rc == 0) break;
        }
        return written;
    }

 private:
    int       fd_;
    IndexTree idx_;
};

core::Expected<std::unique_ptr<LayerReader>, core::AnyError>
LayerReader::Open(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return core::make_unexpected(core::err("open failed: " + path));

    // TODO: read the trailer (last 4 KiB), parse magic/version, then read the
    // index tree region and populate IndexTree.  Skeleton returns an empty
    // index — reads will fail with "vaddr not covered", exactly as expected
    // until you implement the loader.
    IndexTree idx;
    return std::unique_ptr<LayerReader>(new FdLayerReader(fd, std::move(idx)));
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
