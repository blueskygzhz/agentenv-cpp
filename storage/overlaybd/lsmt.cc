// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/lsmt/{index,format}.rs — faithful C++11 port.
#include "agentenv/storage/overlaybd/lsmt.h"

#include <cstdio>
#include <utility>

namespace agentenv {
namespace storage {
namespace overlaybd {
namespace lsmt {

// ---------------- Segment ----------------
uint64_t Segment::ForwardOffsetTo(uint64_t x) {
    if (x <= offset) return 0;
    uint64_t delta = x - offset;
    if (delta >= static_cast<uint64_t>(length)) {
        length = 0;
        offset = x;
        return delta;
    }
    length -= static_cast<uint32_t>(delta);
    offset = x;
    return delta;
}

void Segment::BackwardEndTo(uint64_t x) {
    if (x > offset) {
        length = static_cast<uint32_t>(x - offset);
    } else {
        length = 0;
    }
}

// ---------------- SegmentMapping ----------------
void SegmentMapping::ForwardOffsetTo(uint64_t x) {
    uint64_t delta = segment.ForwardOffsetTo(x);
    if (!zeroed && delta > 0) {
        moffset += delta;
    }
}

// ---------------- compress ----------------
size_t CompressRawIndex(std::vector<SegmentMapping>* mapping) {
    const size_t n = mapping->size();
    if (n < 2) return n;
    std::vector<SegmentMapping>& v = *mapping;
    size_t i = 0;
    for (size_t j = 1; j < n; ++j) {
        const SegmentMapping m_i = v[i];
        const SegmentMapping m_j = v[j];
        bool can_merge = m_i.End() == m_j.Offset() &&
                         m_i.MEnd() == m_j.moffset &&
                         m_i.zeroed == m_j.zeroed &&
                         m_i.tag == m_j.tag &&
                         (static_cast<uint64_t>(m_i.Length()) +
                          static_cast<uint64_t>(m_j.Length())) <=
                             static_cast<uint64_t>(Segment::kMaxLength);
        if (can_merge) {
            v[i].segment.length += m_j.Length();
        } else {
            ++i;
            v[i] = m_j;
        }
    }
    size_t compressed = i + 1;
    v.resize(compressed);
    return compressed;
}

size_t CompressRawIndexPredict(const std::vector<SegmentMapping>& mapping) {
    const size_t n = mapping.size();
    if (n < 2) return n;
    size_t i = 0;
    SegmentMapping m = mapping[0];
    for (size_t k = 1; k < n; ++k) {
        const SegmentMapping& item = mapping[k];
        bool can_merge = m.End() == item.Offset() &&
                         m.MEnd() == item.moffset &&
                         m.tag == item.tag &&
                         m.zeroed == item.zeroed &&
                         (static_cast<uint64_t>(m.Length()) +
                          static_cast<uint64_t>(item.Length())) <=
                             static_cast<uint64_t>(Segment::kMaxLength);
        if (can_merge) {
            m.segment.length += item.Length();
        } else {
            m = item;
            ++i;
        }
    }
    return i + 1;
}

// ---------------- ReadOnlyIndex ----------------
ReadOnlyIndex ReadOnlyIndex::Merge(const std::vector<const ReadOnlyIndex*>& indexes) {
    MutableIndex mi;
    // Iterate newest-first in reverse so lower (older) layers are inserted first;
    // tag = layer index (smaller tag == newer).
    for (size_t ri = indexes.size(); ri-- > 0;) {
        const ReadOnlyIndex* idx = indexes[ri];
        for (size_t k = 0; k < idx->mappings_.size(); ++k) {
            SegmentMapping m_new = idx->mappings_[k];
            m_new.tag = static_cast<uint8_t>(ri);
            mi.Insert(m_new);
        }
    }
    return ReadOnlyIndex(mi.Dump());
}

size_t ReadOnlyIndex::Lookup(Segment query, std::vector<SegmentMapping>* dst) const {
    if (query.length == 0 || mappings_.empty()) return 0;
    const uint64_t end_offset = query.End();

    // partition_point: first index whose End() > query.offset.
    size_t idx = 0;
    {
        size_t lo = 0, hi = mappings_.size();
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (mappings_[mid].End() <= query.offset) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        idx = lo;
    }

    const size_t start_len = dst->size();
    for (size_t k = idx; k < mappings_.size(); ++k) {
        const SegmentMapping& m = mappings_[k];
        if (m.Offset() >= end_offset) break;
        SegmentMapping mp = m;
        if (mp.Offset() < query.offset) mp.ForwardOffsetTo(query.offset);
        if (mp.End() > end_offset) mp.BackwardEndTo(end_offset);
        if (mp.Length() > 0) dst->push_back(mp);
    }
    return dst->size() - start_len;
}

// ---------------- MutableIndex ----------------
void MutableIndex::Insert(SegmentMapping m) {
    if (m.Length() == 0) return;

    std::vector<SegmentMapping> to_remove;
    // Scan mappings starting strictly before m.End(), backwards.
    SegmentMapping scan_end_key(m.End(), 0, 0, false, 0);
    std::set<SegmentMapping, ByOffset>::iterator it = mappings_.lower_bound(scan_end_key);
    while (it != mappings_.begin()) {
        --it;
        if (it->End() <= m.Offset()) break;
        to_remove.push_back(*it);
    }

    for (size_t i = 0; i < to_remove.size(); ++i) {
        const SegmentMapping old = to_remove[i];
        mappings_.erase(old);
        if (old.Offset() < m.Offset()) {
            SegmentMapping left = old;
            left.BackwardEndTo(m.Offset());
            if (left.Length() > 0) mappings_.insert(left);
        }
        if (old.End() > m.End()) {
            SegmentMapping right = old;
            right.ForwardOffsetTo(m.End());
            if (right.Length() > 0) mappings_.insert(right);
        }
    }
    mappings_.insert(m);
}

std::vector<SegmentMapping> MutableIndex::Dump() const {
    std::vector<SegmentMapping> out;
    out.reserve(mappings_.size());
    for (std::set<SegmentMapping, ByOffset>::const_iterator it = mappings_.begin();
         it != mappings_.end(); ++it) {
        out.push_back(*it);
    }
    return out;
}

size_t MutableIndex::Lookup(Segment query, std::vector<SegmentMapping>* dst) const {
    if (query.length == 0) return 0;
    const uint64_t end_offset = query.End();
    const size_t start_len = dst->size();

    SegmentMapping query_start_key(query.offset, 0, 0, false, 0);
    // left neighbour (largest offset < query.offset)
    std::set<SegmentMapping, ByOffset>::iterator lb = mappings_.lower_bound(query_start_key);
    if (lb != mappings_.begin()) {
        std::set<SegmentMapping, ByOffset>::iterator left = lb;
        --left;
        if (left->End() > query.offset) {
            SegmentMapping mp = *left;
            if (mp.Offset() < query.offset) mp.ForwardOffsetTo(query.offset);
            if (mp.End() > end_offset) mp.BackwardEndTo(end_offset);
            if (mp.Length() > 0) dst->push_back(mp);
        }
    }

    SegmentMapping scan_end_key(end_offset, 0, 0, false, 0);
    for (std::set<SegmentMapping, ByOffset>::iterator it = mappings_.lower_bound(query_start_key);
         it != mappings_.end() && ByOffset()(*it, scan_end_key); ++it) {
        SegmentMapping mp = *it;
        if (mp.End() > end_offset) mp.BackwardEndTo(end_offset);
        if (mp.Length() > 0) dst->push_back(mp);
    }
    return dst->size() - start_len;
}

// ---------------- ComboIndex ----------------
ComboIndex::ComboIndex(MutableIndex upper, std::shared_ptr<ReadOnlyIndex> lower,
                       uint8_t ro_layers_count)
    : lower_(lower) {
    if (ro_layers_count > 0) {
        std::vector<SegmentMapping> old = upper.Dump();
        MutableIndex fresh;
        for (size_t i = 0; i < old.size(); ++i) {
            SegmentMapping m = old[i];
            m.tag = static_cast<uint8_t>(m.tag + ro_layers_count);
            fresh.Insert(m);
        }
        upper_ = fresh;
    } else {
        upper_ = upper;
    }
}

size_t ComboIndex::Lookup(Segment query, std::vector<SegmentMapping>* dst) const {
    if (query.length == 0) return 0;
    const size_t start_len = dst->size();

    std::vector<SegmentMapping> upper_results;
    upper_.Lookup(query, &upper_results);

    if (!lower_) {
        for (size_t i = 0; i < upper_results.size(); ++i) dst->push_back(upper_results[i]);
        return dst->size() - start_len;
    }

    uint64_t current_pos = query.offset;
    const uint64_t end_pos = query.End();
    for (size_t i = 0; i < upper_results.size(); ++i) {
        const SegmentMapping& m = upper_results[i];
        if (m.Offset() > current_pos) {
            uint32_t gap_len = static_cast<uint32_t>(m.Offset() - current_pos);
            lower_->Lookup(Segment(current_pos, gap_len), dst);
        }
        dst->push_back(m);
        current_pos = m.End();
    }
    if (current_pos < end_pos) {
        uint32_t gap_len = static_cast<uint32_t>(end_pos - current_pos);
        lower_->Lookup(Segment(current_pos, gap_len), dst);
    }
    return dst->size() - start_len;
}

// ---------------- DiskSegmentMapping (bit packing) ----------------
DiskSegmentMapping DiskSegmentMapping::FromMemory(const SegmentMapping& m) {
    const uint64_t OFFSET_MASK = (1ULL << 50) - 1;
    const uint64_t LENGTH_MASK = (1ULL << 14) - 1;
    const uint64_t MOFFSET_MASK = (1ULL << 55) - 1;
    const uint64_t TAG_MASK = 0xFFULL;

    uint64_t offset = m.Offset() & OFFSET_MASK;
    uint64_t length = static_cast<uint64_t>(m.Length()) & LENGTH_MASK;
    uint64_t low = offset | (length << 50);

    uint64_t moffset = m.moffset & MOFFSET_MASK;
    uint64_t zeroed = m.zeroed ? 1ULL : 0ULL;
    uint64_t tag = static_cast<uint64_t>(m.tag) & TAG_MASK;
    uint64_t high = moffset | (zeroed << 55) | (tag << 56);

    DiskSegmentMapping d;
    d.data_low = low;
    d.data_high = high;
    return d;
}

SegmentMapping DiskSegmentMapping::ToMemory() const {
    const uint64_t OFFSET_MASK = (1ULL << 50) - 1;
    const uint64_t LENGTH_MASK = (1ULL << 14) - 1;
    const uint64_t MOFFSET_MASK = (1ULL << 55) - 1;
    const uint64_t ZEROED_MASK = 1ULL;

    uint64_t offset = data_low & OFFSET_MASK;
    uint64_t length = (data_low >> 50) & LENGTH_MASK;
    uint64_t moffset = data_high & MOFFSET_MASK;
    bool zeroed = ((data_high >> 55) & ZEROED_MASK) != 0;
    uint8_t tag = static_cast<uint8_t>(data_high >> 56);
    return SegmentMapping(offset, static_cast<uint32_t>(length), moffset, zeroed, tag);
}

// ---------------- HeaderTrailer ----------------
const uint8_t kHeaderTrailerMagic1[16] = {
    0x65, 0x7e, 0x63, 0xd2, 0x94, 0x44, 0x08, 0x4c,
    0xa2, 0xd2, 0xc8, 0xec, 0x4f, 0xcf, 0xae, 0x8a,
};

HeaderTrailer::HeaderTrailer() {
    magic0 = 0;
    for (int i = 0; i < 16; ++i) magic1[i] = 0;
    for (int i = 0; i < 37; ++i) { uuid[i] = 0; parent_uuid[i] = 0; }
 for (int i = 0; i < 256; ++i) user_tag[i] = 0;
}

HeaderTrailer HeaderTrailer::New() {
  HeaderTrailer h;
    h.magic0 = kMagic0;
    for (int i = 0; i < 16; ++i) h.magic1[i] = kHeaderTrailerMagic1[i];
    h.size = static_cast<uint32_t>(kOnDiskSize);
    h.version = 1;
    h.sub_version = 1;
    return h;
}

bool HeaderTrailer::VerifyMagic() const {
    if (magic0 != kMagic0) return false;
    for (int i = 0; i < 16; ++i) {
        if (magic1[i] != kHeaderTrailerMagic1[i]) return false;
    }
    return true;
}

namespace {
void put_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
void put_u32(uint8_t* p, uint32_t v) {
 for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}
void put_u64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}
uint16_t get_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
uint32_t get_u32(const uint8_t* p) {
    uint32_t v = 0;
for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}
uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}
}  // namespace

// Field offsets in the #[repr(C, packed)] layout (total 390 bytes):
//   magic0@0(8) magic1@8(16) size@24(4) flags@28(4) index_offset@32(8)
//   index_size@40(8) virtual_size@48(8) uuid@56(37) parent_uuid@93(37)
//   reserved@130(2) version@132(1) sub_version@133(1) user_tag@134(256)
void HeaderTrailer::Serialize(uint8_t out[kOnDiskSize]) const {
    put_u64(out + 0, magic0);
    for (int i = 0; i < 16; ++i) out[8 + i] = magic1[i];
    put_u32(out + 24, size);
    put_u32(out + 28, flags);
    put_u64(out + 32, index_offset);
    put_u64(out + 40, index_size);
    put_u64(out + 48, virtual_size);
    for (int i = 0; i < 37; ++i) out[56 + i] = uuid[i];
    for (int i = 0; i < 37; ++i) out[93 + i] = parent_uuid[i];
    put_u16(out + 130, reserved);
    out[132] = version;
    out[133] = sub_version;
    for (int i = 0; i < 256; ++i) out[134 + i] = user_tag[i];
}

bool HeaderTrailer::Deserialize(const uint8_t* buf, size_t len, HeaderTrailer* out) {
    if (len < kOnDiskSize || out == nullptr) return false;
    out->magic0 = get_u64(buf + 0);
  for (int i = 0; i < 16; ++i) out->magic1[i] = buf[8 + i];
    out->size = get_u32(buf + 24);
    out->flags = get_u32(buf + 28);
out->index_offset = get_u64(buf + 32);
    out->index_size = get_u64(buf + 40);
    out->virtual_size = get_u64(buf + 48);
    for (int i = 0; i < 37; ++i) out->uuid[i] = buf[56 + i];
    for (int i = 0; i < 37; ++i) out->parent_uuid[i] = buf[93 + i];
    out->reserved = get_u16(buf + 130);
    out->version = buf[132];
    out->sub_version = buf[133];
 for (int i = 0; i < 256; ++i) out->user_tag[i] = buf[134 + i];
    return true;
}

// ---------------- LoadIndexAndResetTags ----------------
std::vector<SegmentMapping>
LoadIndexAndResetTags(const uint8_t* index_region, size_t region_len, size_t count) {
    std::vector<SegmentMapping> out;
const size_t stride = 16;  // sizeof(DiskSegmentMapping)
    if (count == 0 || index_region == nullptr) return out;
    if (region_len < count * stride) return out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* p = index_region + i * stride;
        DiskSegmentMapping d;
        d.data_low = get_u64(p);
        d.data_high = get_u64(p + 8);
        SegmentMapping m = d.ToMemory();
        // Skip invalid entries: length==0 or offset==u64::MAX.
        if (m.Length() == 0) continue;
     if (m.Offset() == 0xFFFFFFFFFFFFFFFFULL ||
            m.Offset() == ((1ULL << 50) - 1)) {  // INVALID_SEGMENT_OFFSET (masked)
          continue;
     }
        m.tag = 0;  // reset_tag
        out.push_back(m);
    }
    return out;
}

// ---------------- OpenIndexFile ----------------
std::shared_ptr<ReadOnlyIndex>
OpenIndexFile(const std::string& path, uint64_t* virtual_size_out, std::string* err) {
    auto set_err = [&](const std::string& m) { if (err) *err = m; };

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { set_err("cannot open " + path); return nullptr; }

    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); set_err("seek end failed"); return nullptr; }
    long fsize_l = std::ftell(f);
    if (fsize_l < 0) { std::fclose(f); set_err("ftell failed"); return nullptr; }
    uint64_t file_size = static_cast<uint64_t>(fsize_l);
    const uint64_t HEADER_SIZE = HeaderTrailer::kSpace;

    if (file_size < HEADER_SIZE * 2) {
        std::fclose(f); set_err("file too small for header+trailer"); return nullptr;
    }

    std::vector<uint8_t> block(static_cast<size_t>(HEADER_SIZE));

    // 1) header @ 0
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(&block[0], 1, block.size(), f) != block.size()) {
        std::fclose(f); set_err("read header failed"); return nullptr;
    }
    HeaderTrailer header;
    if (!HeaderTrailer::Deserialize(&block[0], block.size(), &header) ||
        !header.VerifyMagic() || !header.IsHeader()) {
        std::fclose(f); set_err("bad header magic/type"); return nullptr;
    }

    // 2) trailer @ file_size - HEADER_SIZE
    std::fseek(f, static_cast<long>(file_size - HEADER_SIZE), SEEK_SET);
    if (std::fread(&block[0], 1, block.size(), f) != block.size()) {
        std::fclose(f); set_err("read trailer failed"); return nullptr;
    }
    HeaderTrailer trailer;
    if (!HeaderTrailer::Deserialize(&block[0], block.size(), &trailer) ||
        !trailer.VerifyMagic() || !trailer.IsTrailer() ||
        !trailer.IsDataFile() || !trailer.IsSealed()) {
        std::fclose(f); set_err("bad trailer magic/type/sealedness"); return nullptr;
    }

    // 3) index region
    const uint64_t idx_off = trailer.index_offset;
    const uint64_t idx_cnt = trailer.index_size;
    const size_t stride = 16;
  if (idx_cnt > 0) {
        uint64_t region_bytes = idx_cnt * stride;
      if (idx_off + region_bytes > file_size) {
          std::fclose(f); set_err("index region out of file bounds"); return nullptr;
     }
        std::vector<uint8_t> region(static_cast<size_t>(region_bytes));
        std::fseek(f, static_cast<long>(idx_off), SEEK_SET);
 if (std::fread(&region[0], 1, region.size(), f) != region.size()) {
   std::fclose(f); set_err("read index region failed"); return nullptr;
   }
        std::fclose(f);
        std::vector<SegmentMapping> mappings =
            LoadIndexAndResetTags(&region[0], region.size(), static_cast<size_t>(idx_cnt));
      if (virtual_size_out) *virtual_size_out = trailer.virtual_size;
        return std::shared_ptr<ReadOnlyIndex>(new ReadOnlyIndex(std::move(mappings)));
    }

    std::fclose(f);
    if (virtual_size_out) *virtual_size_out = trailer.virtual_size;
    return std::shared_ptr<ReadOnlyIndex>(new ReadOnlyIndex());
}

// ---------------- CompactTo (write path) ----------------
bool CompactTo(const std::string& path, CompactSource* source,
        const std::vector<SegmentMapping>& mappings,
         uint64_t virtual_size, std::string* err) {
    auto set_err = [&](const std::string& m) { if (err) *err = m; };
    if (source == nullptr) { set_err("null CompactSource"); return false; }

    const uint64_t HEADER_SIZE = kLsmtHeaderSize;   // 4096
    const uint64_t ALIGNMENT   = kLsmtAlignment;    // 512

    std::FILE* f = std::fopen(path.c_str(), "wb+");
    if (!f) { set_err("cannot create " + path); return false; }

    // --- header block @ 0 ---
    HeaderTrailer header = HeaderTrailer::New();
    header.size = static_cast<uint32_t>(HeaderTrailer::kOnDiskSize);
    header.virtual_size = virtual_size;
    header.SetHeader();
    header.SetDataFile();
    header.SetSealed();

    std::vector<uint8_t> header_buf(static_cast<size_t>(HEADER_SIZE), 0);
    {
        uint8_t hb[HeaderTrailer::kOnDiskSize];
        header.Serialize(hb);
  for (size_t i = 0; i < sizeof(hb); ++i) header_buf[i] = hb[i];
    }
    if (std::fwrite(&header_buf[0], 1, header_buf.size(), f) != header_buf.size()) {
        std::fclose(f); set_err("write header failed"); return false;
    }

    // --- Phase 1+2: pack non-zeroed data sequentially after the header ---
    std::vector<SegmentMapping> compact_index;
    compact_index.reserve(mappings.size());
    uint64_t dest_moffset = HEADER_SIZE / ALIGNMENT;   // in 512B units

    for (size_t i = 0; i < mappings.size(); ++i) {
        const SegmentMapping& m = mappings[i];
        if (m.zeroed) {
            SegmentMapping zero = m;
          zero.moffset = dest_moffset;   // recorded, occupies no space
            compact_index.push_back(zero);
 continue;
        }
        const uint64_t nbytes = static_cast<uint64_t>(m.Length()) * ALIGNMENT;
        const uint64_t src_byte_off = m.moffset * ALIGNMENT;
        std::vector<uint8_t> data(static_cast<size_t>(nbytes), 0);
if (nbytes > 0 && !source->ReadAt(m.tag, src_byte_off, &data[0], nbytes)) {
 std::fclose(f); set_err("CompactSource read failed"); return false;
        }
        const uint64_t dest_byte_off = dest_moffset * ALIGNMENT;
        if (std::fseek(f, static_cast<long>(dest_byte_off), SEEK_SET) != 0) {
      std::fclose(f); set_err("seek data failed"); return false;
        }
        if (nbytes > 0 && std::fwrite(&data[0], 1, data.size(), f) != data.size()) {
            std::fclose(f); set_err("write data failed"); return false;
        }
        SegmentMapping out = m;
 out.moffset = dest_moffset;
        compact_index.push_back(out);
        dest_moffset += m.Length();
    }

    // --- Phase 3: index + trailer ---
    CompressRawIndex(&compact_index);

    const uint64_t index_offset = dest_moffset * ALIGNMENT;
    std::vector<uint8_t> index_bytes;
    index_bytes.reserve(compact_index.size() * 16);
    for (size_t i = 0; i < compact_index.size(); ++i) {
        DiskSegmentMapping dm = DiskSegmentMapping::FromMemory(compact_index[i]);
        uint8_t rec[16];
        for (int k = 0; k < 8; ++k) rec[k]     = static_cast<uint8_t>((dm.data_low  >> (8 * k)) & 0xFF);
        for (int k = 0; k < 8; ++k) rec[8 + k] = static_cast<uint8_t>((dm.data_high >> (8 * k)) & 0xFF);
        index_bytes.insert(index_bytes.end(), rec, rec + 16);
    }

    // Pad the index array up to a 4096-byte boundary with INVALID entries.
    const size_t n_per_block = 4096 / 16;   // 256
    const size_t remainder = compact_index.size() % n_per_block;
    const size_t padding_count = remainder > 0 ? (n_per_block - remainder) : 0;
    for (size_t i = 0; i < padding_count; ++i) {
      // INVALID entry: offset = INVALID_SEGMENT_OFFSET, length = 0.
        uint8_t rec[16];
        uint64_t low = kInvalidSegmentOffset & ((1ULL << 50) - 1);  // length bits stay 0
        for (int k = 0; k < 8; ++k) rec[k] = static_cast<uint8_t>((low >> (8 * k)) & 0xFF);
     for (int k = 0; k < 8; ++k) rec[8 + k] = 0;
   index_bytes.insert(index_bytes.end(), rec, rec + 16);
    }

    if (std::fseek(f, static_cast<long>(index_offset), SEEK_SET) != 0) {
      std::fclose(f); set_err("seek index failed"); return false;
    }
 if (!index_bytes.empty() &&
        std::fwrite(&index_bytes[0], 1, index_bytes.size(), f) != index_bytes.size()) {
        std::fclose(f); set_err("write index failed"); return false;
    }

  const uint64_t index_size = compact_index.size() + padding_count;
    uint64_t trailer_offset = index_offset + index_bytes.size();
    if (trailer_offset % 4096 != 0) {
        uint64_t pad_len = 4096 - (trailer_offset % 4096);
  std::vector<uint8_t> pad(static_cast<size_t>(pad_len), 0);
        std::fseek(f, static_cast<long>(trailer_offset), SEEK_SET);
        std::fwrite(&pad[0], 1, pad.size(), f);
        trailer_offset += pad_len;
    }

    HeaderTrailer trailer = header;
trailer.SetTrailer();
    trailer.index_offset = index_offset;
    trailer.index_size = index_size;

    std::vector<uint8_t> trailer_buf(4096, 0);
    {
uint8_t tb[HeaderTrailer::kOnDiskSize];
        trailer.Serialize(tb);
   for (size_t i = 0; i < sizeof(tb); ++i) trailer_buf[i] = tb[i];
    }
    if (std::fseek(f, static_cast<long>(trailer_offset), SEEK_SET) != 0) {
        std::fclose(f); set_err("seek trailer failed"); return false;
    }
    if (std::fwrite(&trailer_buf[0], 1, trailer_buf.size(), f) != trailer_buf.size()) {
     std::fclose(f); set_err("write trailer failed"); return false;
    }

    std::fclose(f);
    return true;
}

}  // namespace lsmt
}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
