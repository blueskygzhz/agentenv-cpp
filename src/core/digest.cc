// SPDX-License-Identifier: MIT
#include "agentenv/core/digest.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

namespace agentenv {
namespace core {
namespace {

// ---- minimal SHA-256 (FIPS 180-4) ----
class Sha256Ctx {
 public:
    Sha256Ctx() { Reset(); }

    void Update(const uint8_t* data, std::size_t len) {
        for (std::size_t i = 0; i < len; ++i) {
         buffer_[buffer_len_++] = data[i];
            if (buffer_len_ == 64) {
      ProcessBlock(buffer_);
        total_len_ += 64;
                buffer_len_ = 0;
      }
    }
    }

    void Final(uint8_t out[32]) {
        uint64_t total_bits = (total_len_ + buffer_len_) * 8;
   uint8_t pad = 0x80;
    Update(&pad, 1);
   uint8_t zero = 0x00;
    while (buffer_len_ != 56) {
         Update(&zero, 1);
        }
        uint8_t lenbuf[8];
        for (int i = 0; i < 8; ++i) {
   lenbuf[7 - i] = static_cast<uint8_t>((total_bits >> (8 * i)) & 0xFF);
   }
        Update(lenbuf, 8);
        // buffer_len_ is now 0 and h_ holds the digest.
    for (int i = 0; i < 8; ++i) {
     for (int j = 0; j < 4; ++j) {
        out[i * 4 + j] =
            static_cast<uint8_t>((h_[i] >> (24 - 8 * j)) & 0xFF);
        }
}
    }

 private:
    void Reset() {
        static const uint32_t kInit[8] = {
            0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
            0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
     };
 for (int i = 0; i < 8; ++i) h_[i] = kInit[i];
        buffer_len_ = 0;
    total_len_ = 0;
    }

    static uint32_t Ror(uint32_t x, int n) {
        return (x >> n) | (x << (32 - n));
    }

    void ProcessBlock(const uint8_t* p) {
        static const uint32_t k[64] = {
     0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
  0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
         0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
     0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
     0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
  0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
  0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
            0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
            0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
     };
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) |
                 (static_cast<uint32_t>(p[i * 4 + 1]) << 16) |
          (static_cast<uint32_t>(p[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(p[i * 4 + 3]);
     }
     for (int i = 16; i < 64; ++i) {
       uint32_t s0 = Ror(w[i-15], 7) ^ Ror(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = Ror(w[i-2], 17) ^ Ror(w[i-2], 19) ^ (w[i-2] >> 10);
      w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
      uint32_t a=h_[0],b=h_[1],c=h_[2],d=h_[3],e=h_[4],f=h_[5],g=h_[6],hh=h_[7];
        for (int i = 0; i < 64; ++i) {
   uint32_t S1 = Ror(e,6) ^ Ror(e,11) ^ Ror(e,25);
     uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = Ror(a,2) ^ Ror(a,13) ^ Ror(a,22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
 h_[0]+=a; h_[1]+=b; h_[2]+=c; h_[3]+=d;
        h_[4]+=e; h_[5]+=f; h_[6]+=g; h_[7]+=hh;
    }

    uint32_t h_[8];
    uint8_t  buffer_[64];
  std::size_t buffer_len_;
    uint64_t total_len_;
};

std::string HexOf(const uint8_t* bytes, std::size_t n) {
    static const char* kHex = "0123456789abcdef";
    std::string s(n * 2, '\0');
    for (std::size_t i = 0; i < n; ++i) {
    s[2 * i]     = kHex[bytes[i] >> 4];
        s[2 * i + 1] = kHex[bytes[i] & 0xf];
    }
    return s;
}

}  // namespace

Digest::Digest(const uint8_t bytes[32]) {
    std::memcpy(bytes_.data(), bytes, 32);
}

bool Digest::Parse(const std::string& s, Digest* out) {
    static const std::string kPrefix = "sha256:";
    if (s.size() != kPrefix.size() + 64) return false;
    if (s.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    uint8_t bytes[32] = {};
    for (int i = 0; i < 32; ++i) {
        auto hex = [](char c, int* v) {
if (c >= '0' && c <= '9') { *v = c - '0'; return true; }
            if (c >= 'a' && c <= 'f') { *v = c - 'a' + 10; return true; }
    if (c >= 'A' && c <= 'F') { *v = c - 'A' + 10; return true; }
     return false;
        };
        int hi, lo;
 if (!hex(s[kPrefix.size() + 2 * i], &hi) ||
            !hex(s[kPrefix.size() + 2 * i + 1], &lo)) {
    return false;
        }
        bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    *out = Digest(bytes);
    return true;
}

std::string Digest::ToString() const {
    return "sha256:" + HexOf(bytes_.data(), 32);
}

Digest Digest::Sha256(const void* data, std::size_t len) {
 Sha256Ctx ctx;
    ctx.Update(static_cast<const uint8_t*>(data), len);
    uint8_t out[32];
    ctx.Final(out);
    return Digest(out);
}

bool Digest::operator==(const Digest& o) const {
    return std::memcmp(bytes_.data(), o.bytes_.data(), 32) == 0;
}
bool Digest::operator<(const Digest& o) const {
    return std::memcmp(bytes_.data(), o.bytes_.data(), 32) < 0;
}

std::array<uint8_t, 32> Sha256Bytes(const void* data, std::size_t len) {
    Sha256Ctx ctx;
    ctx.Update(static_cast<const uint8_t*>(data), len);
    std::array<uint8_t, 32> out;
    ctx.Final(out.data());
  return out;
}

std::string Sha256Hex(const void* data, std::size_t len) {
    std::array<uint8_t, 32> b = Sha256Bytes(data, len);
    return HexOf(b.data(), 32);
}

std::string Sha256Hex(const std::string& data) {
    return Sha256Hex(data.data(), data.size());
}

std::string Sha256Digest(const void* data, std::size_t len) {
    return "sha256:" + Sha256Hex(data, len);
}

std::string Sha256Digest(const std::string& data) {
    return Sha256Digest(data.data(), data.size());
}

Expected<FileDigest, std::string> DescribeFile(const std::string& path) {
    // Rust `DIGEST_BUFFER_SIZE`.
    const std::size_t kBufferSize = 128 * 1024;

    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == NULL) {
        return make_unexpected(std::string("open ") + path + ": " + std::strerror(errno));
    }

    Sha256Ctx ctx;
    std::vector<uint8_t> buffer(kBufferSize);
    uint64_t size = 0;
    while (true) {
        const std::size_t read = std::fread(&buffer[0], 1, buffer.size(), file);
        if (read > 0) {
            ctx.Update(&buffer[0], read);
            size += read;
        }
        if (read < buffer.size()) {
            // Short read: distinguish a real error from a clean EOF, or a
            // truncated read would silently produce a digest of partial data.
            if (std::ferror(file) != 0) {
                const std::string error = std::strerror(errno);
                std::fclose(file);
                return make_unexpected(std::string("read ") + path + ": " + error);
            }
            break;
        }
    }
    std::fclose(file);

    uint8_t out[32];
    ctx.Final(out);

    FileDigest digest;
    digest.size = size;
    digest.sha256 = "sha256:" + HexOf(out, 32);
    return digest;
}

}  // namespace core
}  // namespace agentenv
