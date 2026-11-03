// SPDX-License-Identifier: MIT
// Rust: src/image/buildkit.rs
#include "agentenv/image/buildkit.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>

#include "agentenv/core/digest.h"

namespace agentenv {
namespace image {

const uint16_t kBuildkitPort = 1234;
const uint64_t kMaxMetadataBytes = 4ULL * 1024 * 1024;
const uint64_t kMaxImageBytes = 64ULL * 1024 * 1024 * 1024;
const int kBuildkitContentMessageTimeoutSecs = 60;

core::Expected<core::Unit, std::string> ValidateDigest(const std::string& digest) {
    if (!core::IsSha256Digest(digest)) {
        return core::make_unexpected(std::string("expected a lowercase sha256 image digest"));
    }
    return core::Unit();
}

/// Rust's `impl AsyncWrite` parameter of `copy`.
class BuildkitContent::Output {
 public:
    virtual ~Output() {}
    virtual core::Expected<core::Unit, std::string> Write(const std::string& data) = 0;
};

namespace {

class StringOutput : public BuildkitContent::Output {
 public:
    explicit StringOutput(std::string* out) : out_(out) {}
    core::Expected<core::Unit, std::string> Write(const std::string& data) override {
        out_->append(data);
        return core::Unit();
    }

 private:
    std::string* out_;
};

class FileOutput : public BuildkitContent::Output {
 public:
    explicit FileOutput(int fd) : fd_(fd) {}
    core::Expected<core::Unit, std::string> Write(const std::string& data) override {
        const char* cursor = data.data();
        std::size_t left = data.size();
        while (left > 0) {
            const ssize_t n = ::write(fd_, cursor, left);
            if (n < 0) {
                if (errno == EINTR) continue;
                return core::make_unexpected(std::string(std::strerror(errno)));
            }
            cursor += n;
            left -= static_cast<std::size_t>(n);
        }
        return core::Unit();
    }

 private:
    int fd_;
};

}  // namespace

core::Expected<BuildkitContent, std::string> BuildkitContent::Connect(const std::string& host,
                                                                      uint16_t port) {
    const core::Expected<std::shared_ptr<BuildkitContentWire>, std::string> wire =
        MakeBuildkitContentWire(host, port);
    if (!wire.ok()) {
        return core::make_unexpected("connect to builder content store: " + wire.error());
    }
    return BuildkitContent(wire.value());
}

core::Expected<std::string, std::string> BuildkitContent::Metadata(
    const std::string& digest) const {
    std::string bytes;
    StringOutput output(&bytes);
    const core::Expected<uint64_t, std::string> copied = Copy(digest, kMaxMetadataBytes, &output);
    if (!copied.ok()) return core::make_unexpected(copied.error());
    return bytes;
}

core::Expected<core::Unit, std::string> BuildkitContent::Download(const std::string& digest,
                                                                  uint64_t size,
                                                                  const std::string& path) const {
    if (size > kMaxImageBytes) {
        return core::make_unexpected(std::string("image layer exceeds 64 GiB"));
    }
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) {
        return core::make_unexpected("create " + path + ": " + std::strerror(errno));
    }
    FileOutput output(fd);
    const core::Expected<uint64_t, std::string> copied = Copy(digest, size, &output);
    if (::close(fd) != 0 && copied.ok()) {
        return core::make_unexpected("close " + path + ": " + std::strerror(errno));
    }
    if (!copied.ok()) return core::make_unexpected(copied.error());
    if (copied.value() != size) {
        std::ostringstream os;
        os << "content size mismatch for " << digest << ": expected " << size << ", got "
           << copied.value();
        return core::make_unexpected(os.str());
    }
    return core::Unit();
}

core::Expected<uint64_t, std::string> BuildkitContent::Copy(const std::string& digest,
                                                            uint64_t limit,
                                                            Output* output) const {
    const core::Expected<core::Unit, std::string> valid = ValidateDigest(digest);
    if (!valid.ok()) return core::make_unexpected(valid.error());

    ReadContentRequest request;
    request.digest = digest;
    const core::Expected<std::shared_ptr<BuildkitStream<ReadContentResponse> >, BuildkitRpcStatus>
        stream = wire_->Read(request);
    if (!stream.ok()) return core::make_unexpected(stream.error().message);

    core::Sha256Stream hash;
    uint64_t offset = 0;
    for (;;) {
        core::Expected<core::Optional<ReadContentResponse>, BuildkitRpcStatus> next =
            stream.value()->Next();
        if (!next.ok()) return core::make_unexpected(next.error().message);
        if (!next.value().has_value()) break;
        const ReadContentResponse& chunk = *next.value();

        if (chunk.data.empty()) return core::make_unexpected(std::string("empty content chunk"));
        if (chunk.offset < 0 || static_cast<uint64_t>(chunk.offset) != offset) {
            return core::make_unexpected(std::string("unexpected content offset"));
        }
        const uint64_t added = static_cast<uint64_t>(chunk.data.size());
        if (added > UINT64_MAX - offset) {
            return core::make_unexpected(std::string("content size overflow"));
        }
        offset += added;
        if (offset > limit) {
            std::ostringstream os;
            os << "content exceeds byte limit " << limit;
            return core::make_unexpected(os.str());
        }
        hash.Update(chunk.data.data(), chunk.data.size());
        const core::Expected<core::Unit, std::string> written = output->Write(chunk.data);
        if (!written.ok()) return core::make_unexpected(written.error());
    }
    if (hash.PeekHex() != digest) {
        return core::make_unexpected("content digest mismatch for " + digest);
    }
    return offset;
}

}  // namespace image
}  // namespace agentenv
