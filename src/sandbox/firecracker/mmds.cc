// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/mmds.rs
//
// MmdsMetadata builds the per-VM MMDS document with the e2b-compatible field
// names and hashes the (currently always empty) access token with SHA-512.
// A self-contained SHA-512 is included so the port needs no crypto dependency;
// it matches the FIPS 180-4 test vectors and the Rust `hash_access_token`.
#include "agentenv/sandbox/firecracker/mmds.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace agentenv {
namespace sandbox {
namespace firecracker {

namespace {

// ---- minimal SHA-512 (FIPS 180-4) ----
class Sha512 {
 public:
    Sha512() { Reset(); }

    void Update(const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; ++i) {
         buffer_[buffer_len_++] = data[i];
  if (buffer_len_ == 128) {
       ProcessBlock(buffer_);
     total_len_ += 128;
              buffer_len_ = 0;
            }
 }
    }

    void Final(uint8_t out[64]) {
     uint64_t total_bits_low = (total_len_ + buffer_len_) * 8;
        // Append 0x80 then pad to 112 mod 128, then 128-bit length (we only fill
// the low 64 bits which is plenty for these inputs).
        uint8_t pad = 0x80;
        size_t pre_len = buffer_len_;
        (void)pre_len;
        Update(&pad, 1);
    uint8_t zero = 0x00;
        while (buffer_len_ != 112) {
  Update(&zero, 1);
  }
        uint8_t lenbuf[16];
        std::memset(lenbuf, 0, sizeof(lenbuf));
        for (int i = 0; i < 8; ++i) {
  lenbuf[15 - i] = static_cast<uint8_t>((total_bits_low >> (8 * i)) & 0xFF);
        }
        Update(lenbuf, 16);
        // buffer_len_ is now 0 and h_ holds the digest.
        for (int i = 0; i < 8; ++i) {
    for (int j = 0; j < 8; ++j) {
       out[i * 8 + j] = static_cast<uint8_t>((h_[i] >> (56 - 8 * j)) & 0xFF);
    }
        }
    }

 private:
    void Reset() {
        static const uint64_t kInit[8] = {
            0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
            0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
       0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
      };
        for (int i = 0; i < 8; ++i) h_[i] = kInit[i];
        buffer_len_ = 0;
   total_len_ = 0;
    }

    static uint64_t Ror(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

    void ProcessBlock(const uint8_t* p) {
        static const uint64_t k[80] = {
            0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
        0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
            0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
          0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
 0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
          0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
   0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
            0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
      0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
            0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
            0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
      0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
        0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
            0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
      0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
  0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
  0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
   0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
      0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
            0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL,
        };
        uint64_t w[80];
     for (int i = 0; i < 16; ++i) {
            w[i] = 0;
       for (int j = 0; j < 8; ++j) {
 w[i] = (w[i] << 8) | p[i * 8 + j];
            }
        }
        for (int i = 16; i < 80; ++i) {
            uint64_t s0 = Ror(w[i-15], 1) ^ Ror(w[i-15], 8) ^ (w[i-15] >> 7);
     uint64_t s1 = Ror(w[i-2], 19) ^ Ror(w[i-2], 61) ^ (w[i-2] >> 6);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
    uint64_t a=h_[0],b=h_[1],c=h_[2],d=h_[3],e=h_[4],f=h_[5],g=h_[6],hh=h_[7];
        for (int i = 0; i < 80; ++i) {
      uint64_t S1 = Ror(e,14) ^ Ror(e,18) ^ Ror(e,41);
 uint64_t ch = (e & f) ^ ((~e) & g);
         uint64_t t1 = hh + S1 + ch + k[i] + w[i];
       uint64_t S0 = Ror(a,28) ^ Ror(a,34) ^ Ror(a,39);
       uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
       uint64_t t2 = S0 + maj;
          hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
h_[0]+=a; h_[1]+=b; h_[2]+=c; h_[3]+=d; h_[4]+=e; h_[5]+=f; h_[6]+=g; h_[7]+=hh;
    }

    uint64_t h_[8];
 uint8_t  buffer_[128];
    size_t   buffer_len_;
    uint64_t total_len_;
};

std::string to_hex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(d[(p[i] >> 4) & 0xF]);
s.push_back(d[p[i] & 0xF]);
    }
    return s;
}

}  // namespace

std::string MmdsMetadata::HashAccessToken(const std::string& token) {
    Sha512 h;
    h.Update(reinterpret_cast<const uint8_t*>(token.data()), token.size());
    uint8_t out[64];
    h.Final(out);
    return to_hex(out, 64);
}

MmdsMetadata::MmdsMetadata(const core::SandboxId& sandbox_id, const std::string& snapshot_id)
: sandbox_id_(sandbox_id.ToString()),
      snapshot_id_(snapshot_id),
      logs_collector_address_(""),
      access_token_hash_(HashAccessToken("")) {}

const char* const kMmdsReservedFields[4] = {"instanceID", "envID", "address",
                                            "accessTokenHash"};

bool IsReservedMmdsField(const std::string& key) {
    for (std::size_t i = 0; i < 4; ++i) {
        if (key == kMmdsReservedFields[i]) return true;
    }
    return false;
}

MmdsMetadata& MmdsMetadata::WithExtra(const std::string& key, const std::string& json_value) {
    // Rust `extra.retain(|key, _| !RESERVED_FIELDS.contains(...))`. Extras are
    // opaque data the API layer passes through to the guest, so a caller must
    // not be able to restate `instanceID` or `accessTokenHash` and have the
    // duplicate win — MMDS is what the guest reads its own identity from, and
    // the hash is what it authenticates against. Dropped silently, as in Rust:
    // a rejected key is not an error the API layer can act on.
    if (IsReservedMmdsField(key)) return *this;
    extra_.push_back(std::make_pair(key, json_value));
    return *this;
}

void MmdsMetadata::SetAccessToken(const std::string& token) {
    access_token_hash_ = HashAccessToken(token);
}

static std::string json_escape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
    if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else out.push_back(c);
  }
    return out;
}

std::string MmdsMetadata::ToJson() const {
    std::string j = "{";
    j += "\"instanceID\":\"" + json_escape(sandbox_id_) + "\",";
    j += "\"envID\":\"" + json_escape(snapshot_id_) + "\",";
    j += "\"address\":\"" + json_escape(logs_collector_address_) + "\",";
    j += "\"accessTokenHash\":\"" + json_escape(access_token_hash_) + "\"";
    for (size_t i = 0; i < extra_.size(); ++i) {
        j += ",\"" + json_escape(extra_[i].first) + "\":" + extra_[i].second;
  }
    j += "}";
    return j;
}

}  // namespace firecracker
}  // namespace sandbox
}  // namespace agentenv
