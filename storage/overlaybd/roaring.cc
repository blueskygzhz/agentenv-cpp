// SPDX-License-Identifier: MIT
// The Roaring bitmap portable serialization format — see roaring.h.
#include "agentenv/storage/overlaybd/roaring.h"

#include <algorithm>
#include <sstream>

namespace agentenv {
namespace storage {
namespace overlaybd {

namespace {

/// Cookie written when no container is a run container. Followed by a u32
/// container count.
const uint32_t kSerialCookieNoRunContainer = 12346;
/// Cookie written when at least one container is a run container. The count
/// rides in the high 16 bits and a run-flag bitset follows, which is why the
/// two cookies cannot share a header shape.
const uint32_t kSerialCookie = 12347;

/// Below this many containers the offset header is omitted -- but only when
/// run containers are present. Without runs the header is always written, so
/// the reader's rule is `!has_run || count >= 4`.
const uint32_t kNoOffsetThreshold = 4;

/// A container holding more than this many values is written as a bitset;
/// at or below, as a sorted u16 array. 4096 is the break-even point: 4096
/// u16s is 8192 bytes, exactly the size of a 65536-bit bitset.
const uint32_t kArrayMaxCardinality = 4096;

const size_t kBitsetBytes = 8192;

void PutU16(std::vector<uint8_t>* out, uint16_t v) {
    out->push_back(static_cast<uint8_t>(v & 0xFF));
    out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void PutU32(std::vector<uint8_t>* out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

uint16_t GetU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

void RoaringBitmap::Insert(uint32_t value) {
    const uint16_t high = static_cast<uint16_t>(value >> 16);
    const uint16_t low = static_cast<uint16_t>(value & 0xFFFF);
    std::vector<uint16_t>& bucket = buckets_[high];
    const std::vector<uint16_t>::iterator at =
        std::lower_bound(bucket.begin(), bucket.end(), low);
    if (at != bucket.end() && *at == low) return;
    bucket.insert(at, low);
}

void RoaringBitmap::Remove(uint32_t value) {
    const uint16_t high = static_cast<uint16_t>(value >> 16);
    const uint16_t low = static_cast<uint16_t>(value & 0xFFFF);
    const std::map<uint16_t, std::vector<uint16_t> >::iterator found = buckets_.find(high);
    if (found == buckets_.end()) return;
    std::vector<uint16_t>& bucket = found->second;
    const std::vector<uint16_t>::iterator at =
        std::lower_bound(bucket.begin(), bucket.end(), low);
    if (at == bucket.end() || *at != low) return;
    bucket.erase(at);
    // An empty container is dropped rather than kept: the serialized form has
    // no way to express one, since cardinality is stored biased by -1.
    if (bucket.empty()) buckets_.erase(found);
}

bool RoaringBitmap::Contains(uint32_t value) const {
    const uint16_t high = static_cast<uint16_t>(value >> 16);
    const uint16_t low = static_cast<uint16_t>(value & 0xFFFF);
    const std::map<uint16_t, std::vector<uint16_t> >::const_iterator found = buckets_.find(high);
    if (found == buckets_.end()) return false;
    const std::vector<uint16_t>& bucket = found->second;
    const std::vector<uint16_t>::const_iterator at =
        std::lower_bound(bucket.begin(), bucket.end(), low);
    return at != bucket.end() && *at == low;
}

uint64_t RoaringBitmap::Cardinality() const {
    uint64_t total = 0;
    for (std::map<uint16_t, std::vector<uint16_t> >::const_iterator it = buckets_.begin();
         it != buckets_.end(); ++it) {
        total += it->second.size();
    }
    return total;
}

bool RoaringBitmap::IsEmpty() const { return buckets_.empty(); }

std::vector<uint32_t> RoaringBitmap::ToVector() const {
    std::vector<uint32_t> out;
    out.reserve(static_cast<size_t>(Cardinality()));
    // `std::map` iterates in key order and each bucket is kept sorted, so the
    // result is globally ascending without a final sort.
    for (std::map<uint16_t, std::vector<uint16_t> >::const_iterator it = buckets_.begin();
         it != buckets_.end(); ++it) {
        const uint32_t high = static_cast<uint32_t>(it->first) << 16;
        for (size_t i = 0; i < it->second.size(); ++i) {
            out.push_back(high | static_cast<uint32_t>(it->second[i]));
        }
    }
    return out;
}

size_t RoaringBitmap::SerializedSize() const {
    const size_t count = buckets_.size();
    // cookie + container count + (key, cardinality) per container + offsets.
    size_t total = 4 + 4 + count * 4 + count * 4;
    for (std::map<uint16_t, std::vector<uint16_t> >::const_iterator it = buckets_.begin();
         it != buckets_.end(); ++it) {
        total += it->second.size() > kArrayMaxCardinality ? kBitsetBytes
                                                          : it->second.size() * 2;
    }
    return total;
}

std::vector<uint8_t> RoaringBitmap::Serialize() const {
    std::vector<uint8_t> out;
    out.reserve(SerializedSize());

    // No run containers are ever produced, so this is always the
    // no-run-container header: cookie then an explicit u32 count.
    const uint32_t count = static_cast<uint32_t>(buckets_.size());
    PutU32(&out, kSerialCookieNoRunContainer);
    PutU32(&out, count);

    // Descriptors: key and cardinality-1 per container. The bias is why an
    // empty container cannot be represented, and therefore why `Remove`
    // drops them.
    for (std::map<uint16_t, std::vector<uint16_t> >::const_iterator it = buckets_.begin();
         it != buckets_.end(); ++it) {
        PutU16(&out, it->first);
        PutU16(&out, static_cast<uint16_t>(it->second.size() - 1));
    }

    // Offsets are mandatory in this header shape regardless of count: the
    // threshold only applies when run flags are present.
    const size_t offsets_at = out.size();
    for (uint32_t i = 0; i < count; ++i) PutU32(&out, 0);

    size_t index = 0;
    for (std::map<uint16_t, std::vector<uint16_t> >::const_iterator it = buckets_.begin();
         it != buckets_.end(); ++it, ++index) {
        // Offsets are absolute from the start of the serialized bitmap, so
        // they can only be filled in once the preceding container data has
        // been emitted.
        const uint32_t here = static_cast<uint32_t>(out.size());
        out[offsets_at + index * 4 + 0] = static_cast<uint8_t>(here & 0xFF);
        out[offsets_at + index * 4 + 1] = static_cast<uint8_t>((here >> 8) & 0xFF);
        out[offsets_at + index * 4 + 2] = static_cast<uint8_t>((here >> 16) & 0xFF);
        out[offsets_at + index * 4 + 3] = static_cast<uint8_t>((here >> 24) & 0xFF);

        const std::vector<uint16_t>& values = it->second;
        if (values.size() > kArrayMaxCardinality) {
            // Bitset: a fixed 8192 bytes, bit `v` of a little-endian u64 run.
            std::vector<uint8_t> words(kBitsetBytes, 0);
            for (size_t i = 0; i < values.size(); ++i) {
                words[values[i] >> 3] |= static_cast<uint8_t>(1u << (values[i] & 7));
            }
            out.insert(out.end(), words.begin(), words.end());
        } else {
            for (size_t i = 0; i < values.size(); ++i) PutU16(&out, values[i]);
        }
    }

    return out;
}

core::Expected<RoaringBitmap, std::string> RoaringBitmap::Deserialize(const uint8_t* data,
                                                                         size_t len,
                                                                         size_t* consumed) {
    if (data == 0 || len < 4) {
        return core::make_unexpected(std::string("roaring: input too short for cookie"));
    }

    size_t pos = 0;
    const uint32_t cookie = GetU32(data + pos);
    pos += 4;

    uint32_t count = 0;
    bool has_run = false;
    std::vector<uint8_t> run_flags;

    if ((cookie & 0xFFFF) == kSerialCookie) {
        // Run-container header: the count is biased by -1 in the high half of
        // the cookie, and a run-flag bitset follows.
        has_run = true;
        count = (cookie >> 16) + 1;
        const size_t flag_bytes = (static_cast<size_t>(count) + 7) / 8;
        if (len < pos + flag_bytes) {
            return core::make_unexpected(std::string("roaring: truncated run flags"));
        }
        run_flags.assign(data + pos, data + pos + flag_bytes);
        pos += flag_bytes;
    } else if (cookie == kSerialCookieNoRunContainer) {
        if (len < pos + 4) {
            return core::make_unexpected(std::string("roaring: truncated container count"));
        }
        count = GetU32(data + pos);
        pos += 4;
    } else {
        std::ostringstream o;
        o << "roaring: unknown cookie " << cookie;
        return core::make_unexpected(o.str());
    }

    // 65536 buckets is the hard maximum for a 32-bit bitmap; anything beyond
    // means the header is corrupt, and trusting it would size an allocation
    // from garbage.
    if (count > 65536) {
        std::ostringstream o;
        o << "roaring: implausible container count " << count;
        return core::make_unexpected(o.str());
    }

    if (len < pos + static_cast<size_t>(count) * 4) {
        return core::make_unexpected(std::string("roaring: truncated container descriptors"));
    }
    std::vector<uint16_t> keys(count, 0);
    std::vector<uint32_t> cards(count, 0);
    for (uint32_t i = 0; i < count; ++i) {
        keys[i] = GetU16(data + pos);
        // Stored biased by -1, so a stored 0 means one value.
        cards[i] = static_cast<uint32_t>(GetU16(data + pos + 2)) + 1;
        pos += 4;
    }

    // Present unless runs are in play and the bitmap is small. The offsets
    // themselves are not needed -- containers follow in descriptor order --
    // but the header has to be stepped over.
    const bool has_offsets = !has_run || count >= kNoOffsetThreshold;
    if (has_offsets) {
        if (len < pos + static_cast<size_t>(count) * 4) {
            return core::make_unexpected(std::string("roaring: truncated offset header"));
        }
        pos += static_cast<size_t>(count) * 4;
    }

    RoaringBitmap out;
    for (uint32_t i = 0; i < count; ++i) {
        const bool is_run =
            has_run && (run_flags[i / 8] & static_cast<uint8_t>(1u << (i % 8))) != 0;
        const uint32_t high = static_cast<uint32_t>(keys[i]) << 16;

        if (is_run) {
            // Run container: a u16 run count, then (start, length-1) pairs.
            if (len < pos + 2) {
                return core::make_unexpected(std::string("roaring: truncated run count"));
            }
            const uint16_t runs = GetU16(data + pos);
            pos += 2;
            if (len < pos + static_cast<size_t>(runs) * 4) {
                return core::make_unexpected(std::string("roaring: truncated run data"));
            }
            for (uint16_t r = 0; r < runs; ++r) {
                const uint16_t start = GetU16(data + pos);
                // `length - 1`, so a stored 0 is a single value.
                const uint32_t length = static_cast<uint32_t>(GetU16(data + pos + 2)) + 1;
                pos += 4;
                for (uint32_t k = 0; k < length; ++k) {
                    const uint32_t value = static_cast<uint32_t>(start) + k;
                    // A run may not cross its container boundary; stop rather
                    // than spill into the next bucket's key space.
                    if (value > 0xFFFF) break;
                    out.Insert(high | value);
                }
            }
        } else if (cards[i] > kArrayMaxCardinality) {
            if (len < pos + kBitsetBytes) {
                return core::make_unexpected(std::string("roaring: truncated bitset container"));
            }
            for (size_t byte = 0; byte < kBitsetBytes; ++byte) {
                const uint8_t bits = data[pos + byte];
                if (bits == 0) continue;
                for (int bit = 0; bit < 8; ++bit) {
                    if ((bits & (1u << bit)) != 0) {
                        out.Insert(high | static_cast<uint32_t>(byte * 8 + bit));
                    }
                }
            }
            pos += kBitsetBytes;
        } else {
            if (len < pos + static_cast<size_t>(cards[i]) * 2) {
                return core::make_unexpected(std::string("roaring: truncated array container"));
            }
            for (uint32_t k = 0; k < cards[i]; ++k) {
                out.Insert(high | static_cast<uint32_t>(GetU16(data + pos)));
                pos += 2;
            }
        }
    }

    if (consumed != 0) *consumed = pos;
    return out;
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
