// SPDX-License-Identifier: MIT
// Tests for the startup trace and manifest wire formats.
// Rust: storage/overlaybd/src/{startup_pack,startup_manifest}.rs tests.
//
// These are byte formats read by a different process, and for the manifest by
// other nodes, so the assertions are on the bytes and on what gets refused --
// a round-trip alone would pass for two implementations that agreed with each
// other and with nothing else.
#include "microtest.h"

#include "agentenv/storage/overlaybd/startup_pack.h"

using namespace agentenv::storage::overlaybd::startup;
namespace core = agentenv::core;

namespace {

const uint64_t kGiB = 1ull << 30;

std::vector<uint64_t> Pages(const uint64_t* v, std::size_t n) {
    return std::vector<uint64_t>(v, v + n);
}

core::Expected<std::pair<uint64_t, std::vector<uint64_t> >, std::string> Decode(
    const std::vector<uint8_t>& b) {
    return DecodeTrace(b.empty() ? NULL : &b[0], b.size());
}

core::Expected<StartupManifest, std::string> DecodeM(const std::vector<uint8_t>& b) {
    return DecodeManifest(b.empty() ? NULL : &b[0], b.size());
}

}  // namespace

// ============================================================
// trace
// ============================================================

MT_TEST(trace_round_trips_and_keeps_first_touch_order) {
    // Deliberately not in address order: position in this list is the page's
    // first-touch rank, and the codec must not reorder it.
    const uint64_t raw[] = {7 * kPackPageBytes, 0, 3 * kPackPageBytes};
    const std::vector<uint64_t> offsets = Pages(raw, 3);

    core::Expected<std::vector<uint8_t>, std::string> enc = EncodeTrace(kGiB, offsets);
    MT_EXPECT_TRUE(enc.ok());
    if (!enc.ok()) return;

    core::Expected<std::pair<uint64_t, std::vector<uint64_t> >, std::string> dec =
        Decode(enc.value());
    MT_EXPECT_TRUE(dec.ok());
    if (!dec.ok()) return;
    MT_EXPECT_EQ(dec.value().first, kGiB);
    MT_EXPECT_TRUE(dec.value().second == offsets);
}

MT_TEST(trace_header_is_the_documented_bytes) {
    const uint64_t raw[] = {0, kPackPageBytes};
    core::Expected<std::vector<uint8_t>, std::string> enc = EncodeTrace(kGiB, Pages(raw, 2));
    MT_EXPECT_TRUE(enc.ok());
    if (!enc.ok()) return;
    const std::vector<uint8_t>& b = enc.value();

    // 32-byte header then 8 bytes per page. Asserted literally because another
    // process parses these offsets; a field that moved would be caught here
    // rather than by that process reading the wrong one.
    MT_EXPECT_EQ(b.size(), static_cast<std::size_t>(32 + 2 * 8));
    MT_EXPECT_TRUE(std::string(reinterpret_cast<const char*>(&b[0]), 8) == "AENVTRC1");
    MT_EXPECT_EQ(b[8], static_cast<uint8_t>(1));    // version, little-endian
    MT_EXPECT_EQ(b[12], static_cast<uint8_t>(32));  // header size
    MT_EXPECT_EQ(b[24], static_cast<uint8_t>(2));   // page count
    // Little-endian: the low byte of 1 GiB is zero and bit 30 sits in byte 3.
    MT_EXPECT_EQ(b[16], static_cast<uint8_t>(0));
    MT_EXPECT_EQ(b[19], static_cast<uint8_t>(0x40));
    // Reserved bytes are zero, which decode then requires.
    MT_EXPECT_EQ(b[28], static_cast<uint8_t>(0));
    MT_EXPECT_EQ(b[31], static_cast<uint8_t>(0));
}

MT_TEST(trace_rejects_offsets_it_cannot_serve) {
    // Unaligned: the offset names a page, so an unaligned value would have the
    // consumer read across a page boundary.
    const uint64_t unaligned[] = {123};
    MT_EXPECT_TRUE(!EncodeTrace(kGiB, Pages(unaligned, 1)).ok());

    // The *end* of the page must be in range, not just its start. Rust's case:
    // one byte past the last aligned page of a 1 MiB image.
    const uint64_t past_end[] = {(1ull << 20) - kPackPageBytes + 1};
    MT_EXPECT_TRUE(!EncodeTrace(1ull << 20, Pages(past_end, 1)).ok());

    // The last whole page is fine, which is what makes the bound a check on
    // `offset + page` rather than on `offset`.
    const uint64_t last_page[] = {(1ull << 20) - kPackPageBytes};
    MT_EXPECT_TRUE(EncodeTrace(1ull << 20, Pages(last_page, 1)).ok());

    // Empty describes no prefetch at all, so writing it would publish an
    // artifact whose only effect is to make consumers work to learn that.
    MT_EXPECT_TRUE(!EncodeTrace(kGiB, std::vector<uint64_t>()).ok());

    // Near-overflow must not wrap into looking in-range.
    const uint64_t huge[] = {UINT64_MAX - kPackPageBytes + 1};
    MT_EXPECT_TRUE(!EncodeTrace(UINT64_MAX, Pages(huge, 1)).ok());
}

MT_TEST(trace_decode_rejects_damage) {
    const uint64_t raw[] = {0, 3 * kPackPageBytes, 7 * kPackPageBytes};
    core::Expected<std::vector<uint8_t>, std::string> enc = EncodeTrace(kGiB, Pages(raw, 3));
    MT_EXPECT_TRUE(enc.ok());
    if (!enc.ok()) return;
    const std::vector<uint8_t> good = enc.value();

    // Truncated body: the count in the header no longer matches the bytes.
    std::vector<uint8_t> truncated(good.begin(), good.end() - 8);
    MT_EXPECT_TRUE(!Decode(truncated).ok());

    // Trailing bytes are refused too, not ignored. Extra bytes mean the writer
    // and this reader disagree about the format, which puts the page list in
    // doubt as well.
    std::vector<uint8_t> extra = good;
    extra.push_back(0);
    MT_EXPECT_TRUE(!Decode(extra).ok());

    std::vector<uint8_t> bad_magic = good;
    bad_magic[0] ^= 0xFF;
    MT_EXPECT_TRUE(!Decode(bad_magic).ok());

    // An unknown version is refused rather than guessed at: the consumer may
    // be a different build, and a wrong guess maps wrong bytes into a guest.
    std::vector<uint8_t> bad_version = good;
    bad_version[8] = 2;
    MT_EXPECT_TRUE(!Decode(bad_version).ok());

    std::vector<uint8_t> bad_header = good;
    bad_header[12] = 40;
    MT_EXPECT_TRUE(!Decode(bad_header).ok());

    // Reserved bytes in use mean a format this decoder does not know, and the
    // version field did not catch it.
    std::vector<uint8_t> reserved = good;
    reserved[28] = 1;
    MT_EXPECT_TRUE(!Decode(reserved).ok());

    // A header claiming more pages than the cap must be refused before the
    // page list is reserved, not after an allocation attempt.
    std::vector<uint8_t> huge_count = good;
    huge_count[24] = 0xFF; huge_count[25] = 0xFF;
    huge_count[26] = 0xFF; huge_count[27] = 0xFF;
    MT_EXPECT_TRUE(!Decode(huge_count).ok());

    // Zero pages: rejected on decode as well as on encode.
    std::vector<uint8_t> zero_count = good;
    zero_count[24] = 0;
    MT_EXPECT_TRUE(!Decode(zero_count).ok());

    // Shorter than the header itself.
    std::vector<uint8_t> stub(good.begin(), good.begin() + 16);
    MT_EXPECT_TRUE(!Decode(stub).ok());
}

// ============================================================
// manifest
// ============================================================

MT_TEST(build_manifest_merges_adjacent_pages) {
    // Pages 0,1,2 are contiguous and must become one 12 KiB range; page 7 is
    // separate. Merging is what makes the prefetch worth issuing at all --
    // one request for a span instead of one per 4 KiB page.
    const uint64_t raw[] = {0, kPackPageBytes, 2 * kPackPageBytes, 7 * kPackPageBytes};
    core::Expected<StartupManifest, std::string> m = BuildManifest(kGiB, Pages(raw, 4));
    MT_EXPECT_TRUE(m.ok());
    if (!m.ok()) return;

    MT_EXPECT_EQ(m.value().ranges.size(), static_cast<std::size_t>(2));
    if (m.value().ranges.size() != 2) return;
    MT_EXPECT_EQ(m.value().ranges[0].first, static_cast<uint64_t>(0));
    MT_EXPECT_EQ(m.value().ranges[0].second, 3 * kPackPageBytes);
    MT_EXPECT_EQ(m.value().ranges[1].first, 7 * kPackPageBytes);
    MT_EXPECT_EQ(m.value().ranges[1].second, kPackPageBytes);
    // No producer writes prefix pages today.
    MT_EXPECT_TRUE(m.value().prefix_pages.empty());
}

MT_TEST(build_manifest_orders_ranges_by_earliest_need) {
    // First touch order: page 9 first, then the run 0,1,2. Address order would
    // put the run first, but the run is not needed until after page 9.
    const uint64_t raw[] = {9 * kPackPageBytes, 0, kPackPageBytes, 2 * kPackPageBytes};
    core::Expected<StartupManifest, std::string> m = BuildManifest(kGiB, Pages(raw, 4));
    MT_EXPECT_TRUE(m.ok());
    if (!m.ok()) return;
    MT_EXPECT_EQ(m.value().ranges.size(), static_cast<std::size_t>(2));
    if (m.value().ranges.size() != 2) return;

    // Table position is the fetch order, which is the whole point of the
    // format: addresses are already in the offsets, so sorting by address
    // would carry no information the consumer does not already have.
    MT_EXPECT_EQ(m.value().ranges[0].first, 9 * kPackPageBytes);
    MT_EXPECT_EQ(m.value().ranges[1].first, static_cast<uint64_t>(0));
    MT_EXPECT_EQ(m.value().ranges[1].second, 3 * kPackPageBytes);
}

MT_TEST(build_manifest_takes_the_earliest_rank_in_a_merged_range) {
    // First touch order: page 0, then the far page 20, then pages 1 and 2
    // which extend the run at 0. The run therefore contains rank 0 -- it holds
    // the very first page the boot touched -- while page 20 is rank 1.
    //
    // The merge pass walks in address order, so the run's pages are seen at
    // ranks 0, 2, 3. Keeping the *last* rank seen (3) instead of the minimum
    // would push the range the boot needed first behind page 20, which is the
    // one thing the range table's order is supposed to express.
    const uint64_t raw[] = {0, 20 * kPackPageBytes, kPackPageBytes, 2 * kPackPageBytes};
    core::Expected<StartupManifest, std::string> m = BuildManifest(kGiB, Pages(raw, 4));
    MT_EXPECT_TRUE(m.ok());
    if (!m.ok()) return;
    MT_EXPECT_EQ(m.value().ranges.size(), static_cast<std::size_t>(2));
    if (m.value().ranges.size() != 2) return;

    MT_EXPECT_EQ(m.value().ranges[0].first, static_cast<uint64_t>(0));
    MT_EXPECT_EQ(m.value().ranges[0].second, 3 * kPackPageBytes);
    MT_EXPECT_EQ(m.value().ranges[1].first, 20 * kPackPageBytes);
    MT_EXPECT_EQ(m.value().ranges[1].second, kPackPageBytes);
}

MT_TEST(manifest_round_trips_and_header_is_the_documented_bytes) {
    const uint64_t raw[] = {0, kPackPageBytes, 7 * kPackPageBytes};
    core::Expected<StartupManifest, std::string> built = BuildManifest(kGiB, Pages(raw, 3));
    MT_EXPECT_TRUE(built.ok());
    if (!built.ok()) return;

    core::Expected<std::vector<uint8_t>, std::string> enc = EncodeManifest(built.value());
    MT_EXPECT_TRUE(enc.ok());
    if (!enc.ok()) return;
    const std::vector<uint8_t>& b = enc.value();

    // 32-byte header, 8 bytes per prefix page (none), 16 per range (two).
    MT_EXPECT_EQ(b.size(), static_cast<std::size_t>(32 + 0 + 2 * 16));
    MT_EXPECT_TRUE(std::string(reinterpret_cast<const char*>(&b[0]), 8) == "AENVMF01");
    MT_EXPECT_EQ(b[8], static_cast<uint8_t>(4));    // version 4
    MT_EXPECT_EQ(b[12], static_cast<uint8_t>(32));  // header size
    MT_EXPECT_EQ(b[24], static_cast<uint8_t>(0));   // prefix page count
    MT_EXPECT_EQ(b[28], static_cast<uint8_t>(2));   // range count

    core::Expected<StartupManifest, std::string> dec = DecodeM(b);
    MT_EXPECT_TRUE(dec.ok());
    if (!dec.ok()) return;
    MT_EXPECT_TRUE(dec.value() == built.value());
}

MT_TEST(manifest_rejects_overlapping_and_misaligned_ranges) {
    StartupManifest m;
    m.mem_virtual_size = kGiB;

    // Overlap is refused rather than merged: two ranges covering one page give
    // the consumer two sources for the same guest bytes and the format does
    // not say which wins.
    m.ranges.push_back(std::make_pair(static_cast<uint64_t>(0), 2 * kPackPageBytes));
    m.ranges.push_back(std::make_pair(kPackPageBytes, 2 * kPackPageBytes));
    MT_EXPECT_TRUE(!EncodeManifest(m).ok());

    // Exactly abutting is not overlap, so it must be accepted -- this is the
    // boundary the check is written on.
    m.ranges.clear();
    m.ranges.push_back(std::make_pair(static_cast<uint64_t>(0), kPackPageBytes));
    m.ranges.push_back(std::make_pair(kPackPageBytes, kPackPageBytes));
    MT_EXPECT_TRUE(EncodeManifest(m).ok());

    // Zero length describes nothing and would make the range table's position
    // meaningless for that entry.
    m.ranges.clear();
    m.ranges.push_back(std::make_pair(static_cast<uint64_t>(0), static_cast<uint64_t>(0)));
    MT_EXPECT_TRUE(!EncodeManifest(m).ok());

    // A length that is not a whole number of pages.
    m.ranges.clear();
    m.ranges.push_back(std::make_pair(static_cast<uint64_t>(0), kPackPageBytes + 1));
    MT_EXPECT_TRUE(!EncodeManifest(m).ok());

    // Past the end of the image.
    m.ranges.clear();
    m.ranges.push_back(std::make_pair(kGiB - kPackPageBytes, 2 * kPackPageBytes));
    MT_EXPECT_TRUE(!EncodeManifest(m).ok());
}

MT_TEST(manifest_decode_rejects_damage) {
    const uint64_t raw[] = {0, kPackPageBytes, 7 * kPackPageBytes};
    core::Expected<StartupManifest, std::string> built = BuildManifest(kGiB, Pages(raw, 3));
    MT_EXPECT_TRUE(built.ok());
    if (!built.ok()) return;
    core::Expected<std::vector<uint8_t>, std::string> enc = EncodeManifest(built.value());
    MT_EXPECT_TRUE(enc.ok());
    if (!enc.ok()) return;
    const std::vector<uint8_t> good = enc.value();

    std::vector<uint8_t> bad_magic = good;
    bad_magic[0] ^= 0xFF;
    MT_EXPECT_TRUE(!DecodeM(bad_magic).ok());

    // Version 3 is refused, not read as a version-4 manifest.
    std::vector<uint8_t> bad_version = good;
    bad_version[8] = 3;
    MT_EXPECT_TRUE(!DecodeM(bad_version).ok());

    std::vector<uint8_t> bad_header = good;
    bad_header[12] = 48;
    MT_EXPECT_TRUE(!DecodeM(bad_header).ok());

    std::vector<uint8_t> truncated(good.begin(), good.end() - 16);
    MT_EXPECT_TRUE(!DecodeM(truncated).ok());

    std::vector<uint8_t> extra = good;
    extra.push_back(0);
    MT_EXPECT_TRUE(!DecodeM(extra).ok());

    // Counts past the cap are refused before anything is reserved.
    std::vector<uint8_t> huge = good;
    huge[28] = 0xFF; huge[29] = 0xFF; huge[30] = 0xFF; huge[31] = 0xFF;
    MT_EXPECT_TRUE(!DecodeM(huge).ok());

    std::vector<uint8_t> stub(good.begin(), good.begin() + 20);
    MT_EXPECT_TRUE(!DecodeM(stub).ok());

    // Overlap introduced in the bytes: validated as a set after the table is
    // read, so a hand-built manifest cannot smuggle it past the decoder.
    std::vector<uint8_t> overlap = good;
    // Second range starts at 0 too (bytes 48..56 are its start field).
    for (int i = 0; i < 8; ++i) overlap[48 + i] = 0;
    MT_EXPECT_TRUE(!DecodeM(overlap).ok());
}

MT_TEST(build_manifest_rejects_an_empty_page_list) {
    MT_EXPECT_TRUE(!BuildManifest(kGiB, std::vector<uint64_t>()).ok());
    // And a page the image cannot hold.
    const uint64_t past[] = {kGiB};
    MT_EXPECT_TRUE(!BuildManifest(kGiB, Pages(past, 1)).ok());
}

MT_MAIN
