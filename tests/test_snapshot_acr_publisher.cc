// SPDX-License-Identifier: MIT
// Rust: src/snapshot/image_export/acr/publisher.rs — `validates_oci_snapshot_tags`.
//
// The tag is checked before any blob is uploaded. A tag rejected late would
// leave the blobs pushed with no manifest naming them, so the next publish
// re-uploads them and nothing ever collects the orphans — which is why these
// rules are pinned rather than left to the registry's 400.
#include "agentenv/snapshot/image_export/acr_publisher.h"

#include <string>

#include "agentenv/core/digest.h"
#include "agentenv/snapshot/record.h"
#include "microtest.h"

using namespace agentenv;                           // NOLINT
using namespace agentenv::snapshot::image_export;   // NOLINT
using namespace agentenv::snapshot::repository;     // NOLINT

namespace {

core::SnapshotId FixtureId() {
    core::Uuid uuid;
    MT_EXPECT_TRUE(core::Uuid::Parse("018f0d93-aaaa-bbbb-cccc-0123456789ab", &uuid));
    return core::SnapshotId(uuid);
}

bool Accepts(const std::string& tag) { return ValidateSnapshotTag(tag).ok(); }

bool StartsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// ValidateSnapshotTag
// ---------------------------------------------------------------------------

MT_TEST(a_generated_snapshot_tag_is_a_valid_oci_tag) {
    MT_EXPECT_TRUE(Accepts("agentenv-snapshot-018f0d93-aaaa-bbbb-cccc-0123456789ab"));
    MT_EXPECT_TRUE(Accepts("_leading_underscore"));
}

/// `-` may not lead, though it is allowed afterwards: a leading `-` would be
/// read as a flag by the registry tooling a tag is pasted into. That is the
/// whole reason the start and continuation rules differ.
MT_TEST(a_tag_may_not_start_with_a_dash_or_a_dot_but_may_contain_them) {
    MT_EXPECT_TRUE(!Accepts("-bad"));
    MT_EXPECT_TRUE(!Accepts(".bad"));
    MT_EXPECT_TRUE(Accepts("a-b.c_d"));
    // A digit or `_` start is fine.
    MT_EXPECT_TRUE(Accepts("0leading-digit"));
}

MT_TEST(a_tag_with_a_separator_is_refused) {
    // `/` would change which repository the manifest is addressed to, and `:`
    // would split off a second tag.
    MT_EXPECT_TRUE(!Accepts("bad/tag"));
    MT_EXPECT_TRUE(!Accepts("bad:tag"));
    MT_EXPECT_TRUE(!Accepts("bad tag"));
    MT_EXPECT_TRUE(!Accepts("bad@tag"));
}

MT_TEST(the_length_limit_is_128_and_an_empty_tag_is_refused) {
    MT_EXPECT_TRUE(Accepts(std::string(128, 'a')));
    MT_EXPECT_TRUE(!Accepts(std::string(129, 'a')));
    // Empty has no first character to check, and upstream refuses it there
    // rather than letting the loop pass vacuously.
    MT_EXPECT_TRUE(!Accepts(""));
}

/// Non-ASCII is refused. Pinned because the limit is a *byte* count upstream
/// (`str::len()`), so a tag of multi-byte characters must not slip under it on
/// a character count.
MT_TEST(a_non_ascii_tag_is_refused) {
    MT_EXPECT_TRUE(!Accepts("caf\xc3\xa9"));
    MT_EXPECT_TRUE(!Accepts("\xe6\x95\xb0\xe6\x8d\xae"));
    // 50 three-byte characters: 150 bytes, 50 characters. Refused either way,
    // but it must be refused.
    std::string wide;
    for (int i = 0; i < 50; ++i) wide += "\xe6\x95\xb0";
    MT_EXPECT_TRUE(!Accepts(wide));
}

MT_TEST(the_refusal_names_the_tag_and_is_an_invalid_request) {
    const RepositoryResult<core::Unit> out = ValidateSnapshotTag("bad/tag");
    MT_EXPECT_TRUE(!out.ok());
    if (!out.ok()) {
        // InvalidRequest, not Backend: the caller fixes this by passing a
        // different tag, and a retry of the same one can never succeed.
        MT_EXPECT_TRUE(out.error().kind == RepositoryErrorKind::InvalidRequest);
        MT_EXPECT_TRUE(out.error().ToString().find("bad/tag") != std::string::npos);
        MT_EXPECT_TRUE(out.error().ToString().find("not a valid OCI tag") !=
                       std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// SnapshotTag
// ---------------------------------------------------------------------------

MT_TEST(the_rootfs_subject_tags_with_the_plain_snapshot_tag) {
    const core::SnapshotId id = FixtureId();
    MT_EXPECT_EQ(SnapshotTag(id, DiskImageSubject::Rootfs()),
                 agentenv::snapshot::RootfsSnapshotImageTag(id));
    MT_EXPECT_EQ(SnapshotTag(id, DiskImageSubject::Rootfs()),
                 std::string("agentenv-snapshot-018f0d93-aaaa-bbbb-cccc-0123456789ab"));
}

MT_TEST(an_attached_drive_tag_is_the_rootfs_tag_plus_a_sanitized_drive_suffix) {
    const core::SnapshotId id = FixtureId();
    const std::string tag = SnapshotTag(id, DiskImageSubject::AttachedDrive("data:cache"));
    MT_EXPECT_TRUE(StartsWith(
        tag, agentenv::snapshot::RootfsSnapshotImageTag(id) + "-drive-data-cache-"));
    // Whatever the drive id was, the result must still be publishable.
    MT_EXPECT_TRUE(Accepts(tag));
}

/// Every generated tag has to pass the validator, including for ids built from
/// characters a tag may not contain. A drive id is operator-supplied, so this
/// is the case that actually occurs.
MT_TEST(generated_drive_tags_are_always_valid_tags) {
    const core::SnapshotId id = FixtureId();
    const char* const drive_ids[] = {
        "data:cache", "data/cache", "/abs/path", "-leading-dash", ".leading-dot",
        "with space", "UPPER_case", "sym#$%^&*()", "",
        "0123456789012345678901234567890123456789",  // longer than the 32 cap
        "caf\xc3\xa9",                                // non-ASCII
        "\xe6\x95\xb0\xe6\x8d\xae",                   // all non-ASCII
    };
    for (std::size_t i = 0; i < sizeof(drive_ids) / sizeof(drive_ids[0]); ++i) {
        const std::string tag = SnapshotTag(id, DiskImageSubject::AttachedDrive(drive_ids[i]));
        MT_EXPECT_TRUE(Accepts(tag));
        if (!Accepts(tag)) std::printf("  drive_id=[%s] tag=[%s]\n", drive_ids[i], tag.c_str());
    }
}

// ---------------------------------------------------------------------------
// SanitizeDriveTagComponent
// ---------------------------------------------------------------------------

/// The hash is computed over the *original* id, so ids that sanitise onto the
/// same text stay distinct. Without it, two drives of one snapshot would
/// publish to the same tag and the second would overwrite the first.
MT_TEST(ids_that_sanitize_alike_still_get_different_tags) {
    const std::string colon = SanitizeDriveTagComponent("data:cache");
    const std::string slash = SanitizeDriveTagComponent("data/cache");
    const std::string dash = SanitizeDriveTagComponent("data-cache");
    // All three sanitise to the same visible text...
    MT_EXPECT_EQ(colon.substr(0, 10), std::string("data-cache"));
    MT_EXPECT_EQ(slash.substr(0, 10), std::string("data-cache"));
    MT_EXPECT_EQ(dash.substr(0, 10), std::string("data-cache"));
    // ...and are still three different tags.
    MT_EXPECT_TRUE(colon != slash);
    MT_EXPECT_TRUE(slash != dash);
    MT_EXPECT_TRUE(colon != dash);
}

/// The 32-character cap applies to the sanitised text only; the hash suffix is
/// always appended, which is what keeps long ids sharing a prefix distinct.
MT_TEST(a_long_id_is_truncated_to_32_characters_and_still_disambiguated) {
    const std::string a = SanitizeDriveTagComponent(std::string(40, 'a') + "-one");
    const std::string b = SanitizeDriveTagComponent(std::string(40, 'a') + "-two");
    MT_EXPECT_EQ(a.substr(0, 32), std::string(32, 'a'));
    MT_EXPECT_EQ(b.substr(0, 32), std::string(32, 'a'));
    // 32 sanitised + "-" + 12 hex.
    MT_EXPECT_EQ(a.size(), static_cast<std::size_t>(45));
    MT_EXPECT_TRUE(a != b);
}

MT_TEST(an_empty_drive_id_becomes_drive_plus_its_hash) {
    const std::string out = SanitizeDriveTagComponent("");
    MT_EXPECT_EQ(out, std::string("drive-") + core::Sha256Hex(std::string()).substr(0, 12));
}

/// One character in, one `-` out — even for a multi-byte one. Iterating bytes
/// instead would turn a three-byte character into three dashes, so the same
/// drive id would publish under a different tag than upstream.
MT_TEST(a_multi_byte_character_collapses_to_exactly_one_dash) {
    // U+6570 is three bytes; the result must be one dash, not three.
    MT_EXPECT_EQ(SanitizeDriveTagComponent("\xe6\x95\xb0").substr(0, 1), std::string("-"));
    MT_EXPECT_EQ(SanitizeDriveTagComponent("a\xe6\x95\xb0" "b").substr(0, 3),
                 std::string("a-b"));
    // Two-byte (U+00E9) and four-byte (U+1F600) sequences likewise.
    MT_EXPECT_EQ(SanitizeDriveTagComponent("caf\xc3\xa9").substr(0, 4), std::string("caf-"));
    MT_EXPECT_EQ(SanitizeDriveTagComponent("a\xf0\x9f\x98\x80" "b").substr(0, 3),
                 std::string("a-b"));

    // And the cap counts output characters, so 40 multi-byte characters give
    // 32 dashes rather than being cut mid-sequence.
    std::string wide;
    for (int i = 0; i < 40; ++i) wide += "\xe6\x95\xb0";
    MT_EXPECT_EQ(SanitizeDriveTagComponent(wide).substr(0, 32), std::string(32, '-'));
}

/// Characters a tag already allows are passed through untouched, so a
/// well-formed drive id stays readable in the published tag.
MT_TEST(allowed_characters_survive_sanitisation) {
    MT_EXPECT_EQ(SanitizeDriveTagComponent("data_1.v-2").substr(0, 10),
                 std::string("data_1.v-2"));
}

// ---------------------------------------------------------------------------
// DiskImageSubject
// ---------------------------------------------------------------------------

MT_TEST(the_subject_log_label_distinguishes_the_two_kinds) {
    MT_EXPECT_EQ(std::string(DiskImageSubject::Rootfs().LogLabel()), std::string("rootfs"));
    MT_EXPECT_EQ(std::string(DiskImageSubject::AttachedDrive("d").LogLabel()),
                 std::string("attached_drive"));
}

/// `Rootfs` carries no drive id, so a stale one left by a reassignment must
/// not take part in equality.
MT_TEST(a_rootfs_subject_ignores_a_stale_drive_id) {
    DiskImageSubject subject = DiskImageSubject::AttachedDrive("data");
    subject = DiskImageSubject::Rootfs();
    MT_EXPECT_TRUE(subject == DiskImageSubject::Rootfs());
    MT_EXPECT_TRUE(subject != DiskImageSubject::AttachedDrive("data"));
    MT_EXPECT_TRUE(DiskImageSubject::AttachedDrive("a") !=
                   DiskImageSubject::AttachedDrive("b"));
}

MT_MAIN
