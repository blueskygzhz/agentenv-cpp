// SPDX-License-Identifier: MIT
// Rust: src/volume.rs `mod tests` plus coverage for the record/error helpers
// that the upstream tests exercise only indirectly.
#include "agentenv/volume.h"

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "agentenv/cfg.h"
#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/snapshot/layers.h"
#include "agentenv/storage/overlaybd/config.h"
#include "microtest.h"
#include "volume_fakes.h"

namespace {

using agentenv::core::Optional;
using agentenv::core::Unit;
using agentenv::snapshot::ExternalLayer;
using agentenv::snapshot::ManagedLayer;
using agentenv::snapshot::OverlaybdLayerRef;
using agentenv::volume::VolumeError;
using agentenv::volume::VolumeErrorKind;
using agentenv::volume::VolumeManager;
using agentenv::volume::VolumeMode;
using agentenv::volume::VolumeRecord;
using agentenv::volume::VolumeStatus;
namespace obd = agentenv::storage::overlaybd;
namespace repo = agentenv::snapshot::repository;

// The repository fake and the manager fixture live in volume_fakes.h so the
// API-layer volume tests can reuse them.
using agentenv::testing::FakeVolumeRepository;
using agentenv::testing::Fixture;
}  // namespace

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------

MT_TEST(volume_component_validation_matches_rust_rules) {
    MT_EXPECT_TRUE(agentenv::volume::IsValidVolumeComponent("vol_01234abcXYZ"));
    MT_EXPECT_TRUE(agentenv::volume::IsValidVolumeComponent("a-b_c"));
    MT_EXPECT_TRUE(agentenv::volume::IsValidVolumeComponent(std::string(128, 'a')));

    MT_EXPECT_TRUE(!agentenv::volume::IsValidVolumeComponent(""));
    MT_EXPECT_TRUE(!agentenv::volume::IsValidVolumeComponent(std::string(129, 'a')));
    // Path separators and dots are what make a decoded cursor dangerous.
    MT_EXPECT_TRUE(!agentenv::volume::IsValidVolumeComponent("../escape"));
    MT_EXPECT_TRUE(!agentenv::volume::IsValidVolumeComponent("has/slash"));
    MT_EXPECT_TRUE(!agentenv::volume::IsValidVolumeComponent("has.dot"));
    MT_EXPECT_TRUE(!agentenv::volume::IsValidVolumeComponent("has space"));
    // Explicit length: a bare literal would truncate at the NUL and wrongly
    // test the (valid) string "nul".
    MT_EXPECT_TRUE(!agentenv::volume::IsValidVolumeComponent(std::string("nul\0byte", 8)));

    // Upstream's predicate is `is_ascii_alphanumeric`, and the *ascii* half
    // carries weight twice over: these ids become path components, and the
    // cursor decoder leans on this check to subsume the `String::from_utf8`
    // that upstream runs separately. A byte-range test that stops at 'z'
    // silently admits everything above it.
    MT_EXPECT_TRUE(!agentenv::volume::IsValidVolumeComponent("vol_\xc3\xa9"));
    MT_EXPECT_TRUE(!agentenv::volume::IsValidVolumeComponent("vol{brace}"));
    MT_EXPECT_TRUE(!agentenv::volume::IsValidVolumeComponent("vol~tilde"));
    MT_EXPECT_TRUE(!agentenv::volume::IsValidVolumeComponent(std::string("\xff\xfe", 2)));
}

MT_TEST(volume_cursor_round_trips_and_rejects_tampering) {
    const std::string id = "vol_0190f3a9c2b47d5e8f1a2b3c4d5e6f70";
    const std::string token = agentenv::volume::EncodeVolumeCursor(id);
    // base64url, so no '+', '/' or '=' may appear.
    MT_EXPECT_TRUE(token.find('+') == std::string::npos);
    MT_EXPECT_TRUE(token.find('/') == std::string::npos);
    MT_EXPECT_TRUE(token.find('=') == std::string::npos);

    const agentenv::volume::VolumeResult<std::string> decoded =
        agentenv::volume::DecodeVolumeCursor(token);
    MT_EXPECT_TRUE(decoded.ok());
    MT_EXPECT_EQ(decoded.value(), id);

    // Not base64 at all.
    MT_EXPECT_TRUE(agentenv::volume::DecodeVolumeCursor("!!!").error() ==
                   VolumeError::InvalidNextToken());
    // Valid base64 whose payload is a traversal attempt.
    const std::string evil = agentenv::volume::EncodeVolumeCursor("../../etc/passwd");
    MT_EXPECT_TRUE(agentenv::volume::DecodeVolumeCursor(evil).error() ==
                   VolumeError::InvalidNextToken());
    MT_EXPECT_TRUE(agentenv::volume::DecodeVolumeCursor("").error() ==
                   VolumeError::InvalidNextToken());
}

MT_TEST(volume_cursor_encodes_every_input_length) {
    // Exercises all three tail cases of the base64 encoder (0, 1, 2 leftover
    // bytes), since a wrong tail silently corrupts pagination.
    const char* const inputs[] = {"a", "ab", "abc", "abcd", "abcde", "abcdef"};
    for (std::size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        const std::string original = inputs[i];
        const agentenv::volume::VolumeResult<std::string> decoded =
            agentenv::volume::DecodeVolumeCursor(agentenv::volume::EncodeVolumeCursor(original));
        MT_EXPECT_TRUE(decoded.ok());
        MT_EXPECT_EQ(decoded.value(), original);
    }
}

MT_TEST(volume_error_messages_match_thiserror_text) {
    MT_EXPECT_EQ(VolumeError::InvalidName().ToString(),
                 std::string("volume name must contain only letters, numbers, underscores, "
                             "or hyphens"));
    MT_EXPECT_EQ(VolumeError::NameConflict("data").ToString(),
                 std::string("volume name already exists: data"));
    MT_EXPECT_EQ(VolumeError::NotFound("vol_x").ToString(),
                 std::string("volume not found: vol_x"));
    MT_EXPECT_EQ(VolumeError::Reserved("sbx_1").ToString(),
                 std::string("volume is reserved by sandbox sbx_1"));
    MT_EXPECT_EQ(VolumeError::MultipleSources().ToString(),
                 std::string("fromVolume and image cannot be used together"));
    MT_EXPECT_EQ(VolumeError::SizeLimitExceeded(65536).ToString(),
                 std::string("volume size exceeds the configured maximum of 65536 MiB"));
    MT_EXPECT_EQ(VolumeError::TooManyMountedVolumes(24).ToString(),
                 std::string("sandbox cannot mount more than 24 volumes"));
    MT_EXPECT_EQ(VolumeError::Storage("disk on fire").ToString(),
                 std::string("volume catalog storage failed: disk on fire"));

    // Payload-carrying variants must not compare equal across payloads --
    // Rust's derived `Eq` compares them, and the upstream tests assert on
    // whole error values, so a dropped payload makes two different failures
    // indistinguishable. Each of the three payload shapes is checked, because
    // they are compared by three separate branches.
    MT_EXPECT_TRUE(VolumeError::SizeLimitExceeded(1) != VolumeError::SizeLimitExceeded(2));
    MT_EXPECT_TRUE(VolumeError::TooManyMountedVolumes(1) !=
                   VolumeError::TooManyMountedVolumes(2));
    MT_EXPECT_TRUE(VolumeError::NotFound("a") != VolumeError::NotFound("b"));
    MT_EXPECT_TRUE(VolumeError::SizeMismatch() == VolumeError::SizeMismatch());
    // And the same payload still compares equal, so the branches are not just
    // returning false.
    MT_EXPECT_TRUE(VolumeError::SizeLimitExceeded(1) == VolumeError::SizeLimitExceeded(1));
    MT_EXPECT_TRUE(VolumeError::TooManyMountedVolumes(1) ==
                   VolumeError::TooManyMountedVolumes(1));
}

MT_TEST(replace_owner_moves_exclusive_and_read_only_leases) {
    VolumeRecord record;
    record.id = "vol_1";
    record.reserved_by_sandbox_id = std::string("old");
    MT_EXPECT_TRUE(record.ReplaceOwner("old", Optional<std::string>(std::string("new"))));
    MT_EXPECT_EQ(*record.reserved_by_sandbox_id, std::string("new"));

    // Releasing clears the field entirely.
    MT_EXPECT_TRUE(record.ReplaceOwner("new", Optional<std::string>()));
    MT_EXPECT_TRUE(!record.reserved_by_sandbox_id.has_value());
    // A no-op replacement reports no change.
    MT_EXPECT_TRUE(!record.ReplaceOwner("absent", Optional<std::string>()));

    VolumeRecord shared;
    shared.read_only_mounts.push_back("a");
    shared.read_only_mounts.push_back("b");
    MT_EXPECT_TRUE(shared.ReplaceOwner("a", Optional<std::string>(std::string("b"))));
    // "b" already held a lease, so it must not be duplicated.
    MT_EXPECT_EQ(shared.read_only_mounts.size(), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(shared.read_only_mounts[0], std::string("b"));

    MT_EXPECT_TRUE(shared.MountedBy("b"));
    MT_EXPECT_TRUE(!shared.MountedBy("a"));
}

MT_TEST(validate_catalog_update_blocks_illegal_transitions) {
    VolumeRecord current;
    current.id = "vol_1";
    current.name = "data";
    current.size_mb = 1024;
    current.status = VolumeStatus::Ready;

    // Identity fields are immutable.
    VolumeRecord renamed = current;
    renamed.name = "other";
    MT_EXPECT_TRUE(!current.ValidateCatalogUpdate(renamed).ok());
    VolumeRecord resized = current;
    resized.size_mb = 2048;
    MT_EXPECT_TRUE(!current.ValidateCatalogUpdate(resized).ok());
    // All three identity fields, separately. The mode is the one that reads
    // like a mere flag: flipping an exclusive volume to read-only would strand
    // whichever sandbox holds its write lease, and flipping the other way
    // would hand a writer to a volume several sandboxes have mounted.
    VolumeRecord remoded = current;
    remoded.mode = VolumeMode::ReadOnly;
    MT_EXPECT_TRUE(!current.ValidateCatalogUpdate(remoded).ok());
    MT_EXPECT_EQ(current.ValidateCatalogUpdate(remoded).error(),
                 std::string("volume identity fields cannot be changed"));

    // Replacing the backing requires passing through Uploading first.
    VolumeRecord relayered = current;
    ManagedLayer layer;
    layer.digest = "sha256:abc";
    layer.size = 4;
    relayered.backing_layers.push_back(OverlaybdLayerRef::Managed(layer));
    MT_EXPECT_TRUE(!current.ValidateCatalogUpdate(relayered).ok());
    current.status = VolumeStatus::Uploading;
    MT_EXPECT_TRUE(current.ValidateCatalogUpdate(relayered).ok());

    // Failed may not jump straight back to Ready.
    VolumeRecord failed = current;
    failed.status = VolumeStatus::Failed;
    VolumeRecord ready = failed;
    ready.status = VolumeStatus::Ready;
    MT_EXPECT_TRUE(!failed.ValidateCatalogUpdate(ready).ok());

    // A record being deleted refuses every update.
    VolumeRecord deleting = current;
    deleting.deleting = true;
    MT_EXPECT_TRUE(!deleting.ValidateCatalogUpdate(current).ok());
}

MT_TEST(layer_ref_equality_distinguishes_variants) {
    ManagedLayer managed;
    managed.digest = "sha256:abc";
    managed.size = 10;
    ExternalLayer external;
    external.digest = "sha256:abc";
    external.size = 10;

    // Same digest and size, but different variants are never equal.
    MT_EXPECT_TRUE(OverlaybdLayerRef::Managed(managed) != OverlaybdLayerRef::External(external));
    MT_EXPECT_TRUE(OverlaybdLayerRef::Managed(managed) == OverlaybdLayerRef::Managed(managed));

    ManagedLayer with_uuid = managed;
    with_uuid.uuid = std::string("u1");
    MT_EXPECT_TRUE(OverlaybdLayerRef::Managed(with_uuid) != OverlaybdLayerRef::Managed(managed));
}

// ---------------------------------------------------------------------------
// Durable record format
// ---------------------------------------------------------------------------
//
// These assert literal key spellings against the Rust shape rather than
// round-tripping `ToJson` through `FromJson`. A round-trip is self-consistent
// by construction: rename every key on both sides and it still passes, which
// is exactly how this file came to be written in snake_case while the record
// it has to interoperate with is `#[serde(rename_all = "camelCase")]`.

MT_TEST(record_serialises_the_camel_case_keys_rust_persists) {
    VolumeRecord record;
    record.id = "vol_abc";
    record.name = "demo";
    record.mode = VolumeMode::Exclusive;
    record.size_mb = 1024;
    record.status = VolumeStatus::Ready;

    const std::string json = record.ToJson().ToString();

    // The four multi-word fields. Rust's `encode_record` writes this same
    // document to `volumes/records/<id>.json`, and `size_mb` carries no
    // `#[serde(default)]`, so a snake_case key here is not a cosmetic
    // difference -- it makes the record unreadable to the other side.
    MT_EXPECT_TRUE(json.find("\"sizeMb\":1024") != std::string::npos);
    MT_EXPECT_TRUE(json.find("\"reservedBySandboxId\":") != std::string::npos);
    MT_EXPECT_TRUE(json.find("\"backingLayers\":") != std::string::npos);
    MT_EXPECT_TRUE(json.find("\"readOnlyMounts\":") != std::string::npos);

    // And none of the snake_case spellings survive anywhere in the document.
    MT_EXPECT_TRUE(json.find("size_mb") == std::string::npos);
    MT_EXPECT_TRUE(json.find("reserved_by_sandbox_id") == std::string::npos);
    MT_EXPECT_TRUE(json.find("backing_layers") == std::string::npos);
    MT_EXPECT_TRUE(json.find("read_only_mounts") == std::string::npos);

    // The enum values follow different rules from the keys, and both are
    // deliberate: `VolumeMode` has no `rename_all` and stays PascalCase,
    // `VolumeStatus` is `rename_all = "lowercase"`.
    MT_EXPECT_TRUE(json.find("\"mode\":\"Exclusive\"") != std::string::npos);
    MT_EXPECT_TRUE(json.find("\"status\":\"ready\"") != std::string::npos);

    // `deleting` is `skip_serializing_if = "std::ops::Not::not"`, so a false
    // value is absent rather than written as `false`.
    MT_EXPECT_TRUE(json.find("deleting") == std::string::npos);
    record.deleting = true;
    MT_EXPECT_TRUE(record.ToJson().ToString().find("\"deleting\":true") != std::string::npos);
}

MT_TEST(record_parses_a_document_written_by_rust) {
    // Copied from the upstream fixture at
    // crates/e2e-tests/tests/snapshot_oss_e2e_test.rs `volume_record`, which
    // feeds it to `serde_json::from_value::<VolumeRecord>` -- so this is the
    // shape Rust both writes and accepts, not a guess at it.
    const char* rust_document =
        "{\"id\":\"vol_abc\",\"name\":\"demo\",\"mode\":\"Exclusive\",\"sizeMb\":1024,"
        "\"status\":\"ready\",\"reservedBySandboxId\":null,\"backingLayers\":[],"
        "\"readOnlyMounts\":[]}";

    const agentenv::core::Expected<agentenv::core::Json, agentenv::core::AnyError> parsed =
        agentenv::core::Json::Parse(rust_document);
    MT_EXPECT_TRUE(parsed.ok());
    if (!parsed.ok()) return;

    const agentenv::core::Expected<VolumeRecord, std::string> record =
        VolumeRecord::FromJson(parsed.value());
    MT_EXPECT_TRUE(record.ok());
    if (!record.ok()) return;

    MT_EXPECT_EQ(record.value().id, std::string("vol_abc"));
    MT_EXPECT_EQ(record.value().size_mb, static_cast<uint64_t>(1024));
    MT_EXPECT_EQ(record.value().mode, VolumeMode::Exclusive);
    MT_EXPECT_EQ(record.value().status, VolumeStatus::Ready);
    // An explicit null reads as absent, not as the string "null".
    MT_EXPECT_TRUE(!record.value().reserved_by_sandbox_id.has_value());

    // Re-encoding reproduces the same keys, which is what makes the record
    // survive a read-modify-write by this implementation.
    const std::string back = record.value().ToJson().ToString();
    MT_EXPECT_TRUE(back.find("\"sizeMb\":1024") != std::string::npos);
}

MT_TEST(a_snake_case_document_is_rejected_rather_than_silently_defaulted) {
    const char* legacy = "{\"id\":\"vol_abc\",\"name\":\"demo\",\"size_mb\":1024}";
    const agentenv::core::Expected<agentenv::core::Json, agentenv::core::AnyError> parsed =
        agentenv::core::Json::Parse(legacy);
    MT_EXPECT_TRUE(parsed.ok());
    if (!parsed.ok()) return;

    // Rust would fail the same way -- only `reservedBySandboxId` and
    // `deleting` may be absent, and serde reports the first missing field in
    // declaration order, which is `mode`.
    const agentenv::core::Expected<VolumeRecord, std::string> record =
        VolumeRecord::FromJson(parsed.value());
    MT_EXPECT_TRUE(!record.ok());
    if (!record.ok()) MT_EXPECT_EQ(record.error(), std::string("missing field `mode`"));

    // With every other field present, the snake_case size is still not
    // `sizeMb`: defaulting it to zero would claim a zero-byte volume.
    const agentenv::core::Expected<agentenv::core::Json, agentenv::core::AnyError> sized =
        agentenv::core::Json::Parse(
            "{\"id\":\"vol_abc\",\"name\":\"demo\",\"mode\":\"Exclusive\",\"size_mb\":1024,"
            "\"status\":\"ready\",\"backingLayers\":[],\"readOnlyMounts\":[]}");
    MT_EXPECT_TRUE(sized.ok());
    const agentenv::core::Expected<VolumeRecord, std::string> unsized =
        VolumeRecord::FromJson(sized.value());
    MT_EXPECT_TRUE(!unsized.ok());
    if (!unsized.ok()) MT_EXPECT_EQ(unsized.error(), std::string("missing field `sizeMb`"));

    // The two list fields are required too, and these are the easiest to
    // wave through because an empty list looks like a harmless default. It is
    // not: an absent `backingLayers` would describe a volume with no backing
    // at all, which reads as a blank disk rather than as an unreadable
    // record, and an absent `readOnlyMounts` would report every reader's
    // lease as released and let the volume be deleted under them.
    const char* const required_lists[] = {
        "{\"id\":\"v\",\"name\":\"d\",\"mode\":\"Exclusive\",\"sizeMb\":1,"
        "\"status\":\"ready\",\"readOnlyMounts\":[]}",
        "{\"id\":\"v\",\"name\":\"d\",\"mode\":\"Exclusive\",\"sizeMb\":1,"
        "\"status\":\"ready\",\"backingLayers\":[]}",
    };
    const char* const expected_errors[] = {
        "missing field `backingLayers`",
        "missing field `readOnlyMounts`",
    };
    for (std::size_t i = 0; i < 2; ++i) {
        const agentenv::core::Expected<agentenv::core::Json, agentenv::core::AnyError> doc =
            agentenv::core::Json::Parse(required_lists[i]);
        MT_EXPECT_TRUE(doc.ok());
        if (!doc.ok()) continue;
        const agentenv::core::Expected<VolumeRecord, std::string> record =
            VolumeRecord::FromJson(doc.value());
        MT_EXPECT_TRUE(!record.ok());
        if (!record.ok()) MT_EXPECT_EQ(record.error(), std::string(expected_errors[i]));
    }
}

MT_TEST(a_record_field_of_the_wrong_type_is_rejected_not_coerced) {
    // serde's derive refuses a present-but-wrong-typed field, and coercing or
    // defaulting instead would turn a corrupt catalog entry into a plausible
    // volume. `sizeMb` is the dangerous one: a zero there claims a zero-byte
    // volume for a record that has real data behind it.
    struct Case {
        const char* document;
        const char* expected;
    };
    const Case cases[] = {
        {"{\"id\":\"v\",\"name\":\"d\",\"mode\":\"Exclusive\",\"sizeMb\":\"1024\","
         "\"status\":\"ready\",\"backingLayers\":[],\"readOnlyMounts\":[]}",
         "invalid type for field `sizeMb`: expected u64"},
        // A float is not a `u64` even when it is integral.
        {"{\"id\":\"v\",\"name\":\"d\",\"mode\":\"Exclusive\",\"sizeMb\":1024.0,"
         "\"status\":\"ready\",\"backingLayers\":[],\"readOnlyMounts\":[]}",
         "invalid type for field `sizeMb`: expected u64"},
        {"{\"id\":\"v\",\"name\":\"d\",\"mode\":\"Exclusive\",\"sizeMb\":-1,"
         "\"status\":\"ready\",\"backingLayers\":[],\"readOnlyMounts\":[]}",
         "invalid type for field `sizeMb`: expected u64"},
        {"{\"id\":\"v\",\"name\":\"d\",\"mode\":\"Exclusive\",\"sizeMb\":1,"
         "\"status\":\"ready\",\"backingLayers\":[],\"readOnlyMounts\":\"sbx_1\"}",
         "invalid type for field `readOnlyMounts`: expected a sequence"},
        {"{\"id\":\"v\",\"name\":\"d\",\"mode\":\"Exclusive\",\"sizeMb\":1,"
         "\"status\":\"ready\",\"backingLayers\":[],\"readOnlyMounts\":[1]}",
         "invalid type for field `readOnlyMounts`: expected a string"},
        {"{\"id\":\"v\",\"name\":\"d\",\"mode\":\"Exclusive\",\"sizeMb\":1,"
         "\"status\":\"ready\",\"backingLayers\":[],\"readOnlyMounts\":[],"
         "\"reservedBySandboxId\":7}",
         "invalid type for field `reservedBySandboxId`: expected a string"},
        // `deleting` is `#[serde(default)]`, which makes it optional rather
        // than lenient: a present `null` is still the wrong type.
        {"{\"id\":\"v\",\"name\":\"d\",\"mode\":\"Exclusive\",\"sizeMb\":1,"
         "\"status\":\"ready\",\"backingLayers\":[],\"readOnlyMounts\":[],"
         "\"deleting\":null}",
         "invalid type for field `deleting`: expected a boolean"},
        {"{\"id\":\"v\",\"name\":7,\"mode\":\"Exclusive\",\"sizeMb\":1,"
         "\"status\":\"ready\",\"backingLayers\":[],\"readOnlyMounts\":[]}",
         "invalid type for field `name`: expected a string"},
    };

    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const agentenv::core::Expected<agentenv::core::Json, agentenv::core::AnyError> parsed =
            agentenv::core::Json::Parse(cases[i].document);
        MT_EXPECT_TRUE(parsed.ok());
        if (!parsed.ok()) continue;
        const agentenv::core::Expected<VolumeRecord, std::string> record =
            VolumeRecord::FromJson(parsed.value());
        MT_EXPECT_TRUE(!record.ok());
        if (!record.ok()) MT_EXPECT_EQ(record.error(), std::string(cases[i].expected));
    }

    // A non-object is not a struct at all.
    const agentenv::core::Expected<VolumeRecord, std::string> array =
        VolumeRecord::FromJson(agentenv::core::Json(agentenv::core::JsonArray()));
    MT_EXPECT_TRUE(!array.ok());
}

MT_TEST(an_unknown_record_key_is_ignored_and_the_skipped_one_is_not_read) {
    // There is no `deny_unknown_fields` upstream, so a record written by a
    // newer node must still load -- rejecting it would leave a rolling
    // upgrade unable to read back its own catalog.
    //
    // `backingImageConfig` is the key that matters: the field exists on the
    // struct but is `#[serde(skip)]`, so it is an unknown *document* key and
    // has to be ignored rather than populate the field. Reading it would
    // import another node's local cache path and leave this node pointing at
    // a file it does not have.
    const char* document =
        "{\"id\":\"vol_abc\",\"name\":\"demo\",\"mode\":\"Exclusive\",\"sizeMb\":8,"
        "\"status\":\"ready\",\"backingLayers\":[],\"readOnlyMounts\":[],"
        "\"backingImageConfig\":\"/some/other/node/config.json\","
        "\"somethingFromTheFuture\":{\"nested\":true}}";
    const agentenv::core::Expected<agentenv::core::Json, agentenv::core::AnyError> parsed =
        agentenv::core::Json::Parse(document);
    MT_EXPECT_TRUE(parsed.ok());
    if (!parsed.ok()) return;

    const agentenv::core::Expected<VolumeRecord, std::string> record =
        VolumeRecord::FromJson(parsed.value());
    MT_EXPECT_TRUE(record.ok());
    if (!record.ok()) return;
    MT_EXPECT_EQ(record.value().id, std::string("vol_abc"));
    MT_EXPECT_TRUE(!record.value().backing_image_config.has_value());
    // `deleting` absent is false, per `#[serde(default)]`.
    MT_EXPECT_TRUE(!record.value().deleting);

    // And it does not come back out: `#[serde(skip)]` applies in both
    // directions, so a locally-set path must not reach the durable record.
    VolumeRecord local = record.value();
    local.backing_image_config = std::string("/var/lib/agentenv/volumes/vol_abc/config.json");
    const std::string encoded = local.ToJson().ToString();
    MT_EXPECT_TRUE(encoded.find("backingImageConfig") == std::string::npos);
    MT_EXPECT_TRUE(encoded.find("/var/lib/agentenv") == std::string::npos);
}

MT_TEST(a_variant_rejection_is_worded_the_way_serde_words_it) {
    // Upstream has no hand-written parser for either enum: serde's derive is
    // what rejects an unknown value, and the text it produces is what the
    // catalog surfaces when a record fails to decode. serde switches wording
    // on the variant count -- two variants read "`a` or `b`", three or more
    // read "one of `a`, `b`, `c`" -- so these two lines are *supposed* to be
    // phrased differently, and asserting only one of them is how the status
    // parser came to say "unsupported volume status" while its sibling was
    // aligned.
    const agentenv::core::Expected<VolumeMode, std::string> mode =
        agentenv::volume::VolumeModeParse("exclusive");
    MT_EXPECT_TRUE(!mode.ok());
    if (!mode.ok()) {
        MT_EXPECT_EQ(mode.error(),
                     std::string("unknown variant `exclusive`, expected `ReadOnly` or "
                                 "`Exclusive`"));
    }

    const agentenv::core::Expected<VolumeStatus, std::string> status =
        agentenv::volume::VolumeStatusParse("Ready");
    MT_EXPECT_TRUE(!status.ok());
    if (!status.ok()) {
        MT_EXPECT_EQ(status.error(),
                     std::string("unknown variant `Ready`, expected one of `ready`, "
                                 "`uploading`, `failed`"));
    }

    // The two case conventions are not interchangeable, which is why
    // "exclusive" and "Ready" above are both rejections rather than one of
    // them being a parse: `VolumeMode` carries no `rename_all`, `VolumeStatus`
    // is `rename_all = "lowercase"`.
    MT_EXPECT_TRUE(agentenv::volume::VolumeModeParse("Exclusive").ok());
    MT_EXPECT_TRUE(agentenv::volume::VolumeModeParse("ReadOnly").ok());
    MT_EXPECT_TRUE(agentenv::volume::VolumeStatusParse("ready").ok());
    MT_EXPECT_TRUE(agentenv::volume::VolumeStatusParse("uploading").ok());
    MT_EXPECT_TRUE(agentenv::volume::VolumeStatusParse("failed").ok());
}

MT_TEST(mounted_by_covers_the_exclusive_owner_as_well_as_the_readers) {
    // Two separate lease kinds, and `mounted_by` is what `delete` consults
    // before removing a volume -- missing either one would let a volume be
    // deleted out from under a running sandbox.
    VolumeRecord exclusive;
    exclusive.reserved_by_sandbox_id = std::string("sbx_owner");
    MT_EXPECT_TRUE(exclusive.MountedBy("sbx_owner"));
    MT_EXPECT_TRUE(!exclusive.MountedBy("sbx_other"));

    VolumeRecord shared;
    shared.read_only_mounts.push_back("sbx_reader");
    MT_EXPECT_TRUE(shared.MountedBy("sbx_reader"));
    MT_EXPECT_TRUE(!shared.MountedBy("sbx_owner"));

    // An unleased record is mounted by nobody, including by the empty string:
    // `reserved_by_sandbox_id` is `None`, not `Some("")`, so the comparison
    // has to go through `has_value()` rather than through a default.
    VolumeRecord idle;
    MT_EXPECT_TRUE(!idle.MountedBy(""));
    MT_EXPECT_TRUE(!idle.MountedBy("sbx_owner"));
}

MT_TEST(record_equality_includes_the_field_serde_skips) {
    // `backing_image_config` is `#[serde(skip)]` but is still part of Rust's
    // derived `PartialEq`, and the combination is easy to get wrong in one
    // breath: the field never appears in the document, so dropping it from
    // the comparison looks free. It is not -- `current_local_backing` decides
    // whether a node-local image config may be reused by comparing records,
    // so two records differing only in that path are not interchangeable.
    VolumeRecord a;
    a.id = "vol_1";
    a.name = "demo";
    VolumeRecord b = a;
    MT_EXPECT_TRUE(a == b);

    b.backing_image_config = std::string("/var/lib/agentenv/volumes/vol_1/config.json");
    MT_EXPECT_TRUE(a != b);

    // The field that is omitted from the document when false still counts.
    VolumeRecord deleting = a;
    deleting.deleting = true;
    MT_EXPECT_TRUE(a != deleting);

    // `read_only_mounts` is a `Vec`, so order is part of equality.
    VolumeRecord ab = a;
    ab.read_only_mounts.push_back("x");
    ab.read_only_mounts.push_back("y");
    VolumeRecord ba = a;
    ba.read_only_mounts.push_back("y");
    ba.read_only_mounts.push_back("x");
    MT_EXPECT_TRUE(ab != ba);
}

MT_TEST(the_same_predicate_reports_a_name_and_an_id_differently) {
    // `validate_name` and `validate_volume_id` share
    // `is_valid_volume_component` but not their error: a bad *name* came from
    // the caller and is `InvalidName`, while a bad *id* was read out of the
    // catalog and is `Storage`, because it means the store is corrupt rather
    // than the request. Collapsing them blames the client for the node's own
    // broken record.
    MT_EXPECT_TRUE(agentenv::volume::ValidateName("data_1").ok());
    MT_EXPECT_TRUE(agentenv::volume::ValidateVolumeId("vol_1").ok());

    const agentenv::core::Expected<Unit, VolumeError> bad_name =
        agentenv::volume::ValidateName("has/slash");
    MT_EXPECT_TRUE(!bad_name.ok());
    if (!bad_name.ok()) MT_EXPECT_TRUE(bad_name.error() == VolumeError::InvalidName());

    const agentenv::core::Expected<Unit, VolumeError> bad_id =
        agentenv::volume::ValidateVolumeId("has/slash");
    MT_EXPECT_TRUE(!bad_id.ok());
    if (!bad_id.ok()) {
        MT_EXPECT_TRUE(bad_id.error().kind == VolumeErrorKind::Storage);
        MT_EXPECT_EQ(bad_id.error().ToString(),
                     std::string("volume catalog storage failed: invalid volume id "
                                 "'has/slash'"));
    }
}

MT_TEST(a_non_canonical_cursor_is_refused) {
    // `URL_SAFE_NO_PAD` decoding is strict on each of these, and every one of
    // them would otherwise let two different tokens name the same page --
    // which turns pagination into a silent loop or a silent skip.
    //
    // Padding is not in the no-pad alphabet.
    MT_EXPECT_TRUE(agentenv::volume::DecodeVolumeCursor("dm9s==").error() ==
                   VolumeError::InvalidNextToken());
    // A length of 4n+1 cannot be produced by any encoder. "dm9sA" is the case
    // that isolates the length rule: its fifth symbol is `A`, so its leftover
    // bits are zero and the trailing-bits rule below lets it through, and the
    // first four symbols decode cleanly to the valid component "vol". Only the
    // length check stands between it and being accepted as a cursor for a
    // volume whose real token is "dm9s".
    MT_EXPECT_TRUE(agentenv::volume::DecodeVolumeCursor("dm9sA").error() ==
                   VolumeError::InvalidNextToken());
    MT_EXPECT_TRUE(agentenv::volume::DecodeVolumeCursor("dm9s").ok());
    // Standard-alphabet characters are not in the url-safe one. Note this one
    // is over-determined: `+` and `/` carry the 6-bit values 62 and 63, and a
    // group of either forces a decoded byte of `>`, `~` or something above
    // 0x7F -- none of which `is_valid_volume_component` admits. So the payload
    // check would refuse this token even if the alphabet did not, which is why
    // the harness carries no mutation for the alphabet.
    MT_EXPECT_TRUE(agentenv::volume::DecodeVolumeCursor("ab+/").error() ==
                   VolumeError::InvalidNextToken());

    // Non-zero trailing bits: "dg" and "dh" both carry the byte 0x76, and only
    // the first is canonical. `base64` 0.22 rejects the second
    // (`decode_allow_trailing_bits` is off by default), so this must too, or
    // the two would be interchangeable cursors for the same volume.
    const agentenv::volume::VolumeResult<std::string> canonical =
        agentenv::volume::DecodeVolumeCursor("dg");
    MT_EXPECT_TRUE(canonical.ok());
    if (canonical.ok()) MT_EXPECT_EQ(canonical.value(), std::string("v"));
    MT_EXPECT_TRUE(agentenv::volume::DecodeVolumeCursor("dh").error() ==
                   VolumeError::InvalidNextToken());
}

// ---------------------------------------------------------------------------
// Manager behaviour
// ---------------------------------------------------------------------------

MT_TEST(default_limits_come_from_the_volume_config) {
    const agentenv::volume::VolumeLimits limits = agentenv::volume::VolumeLimits::Default();

    // Rust's `impl Default for VolumeLimits` reads `cfg::VolumeConfig`, whose
    // defaults are 262144 MiB and 4 mounts. Pinned as literals because the
    // two constants sitting next to them are tempting and wrong:
    // `kDefaultVolumeSizeMb` (65536) is the size a new volume gets when the
    // request names none, and `kMaxVolumeMounts` (24) is the hardware ceiling
    // the config validator clamps against. Using either here moves a real
    // limit -- too small a cap refuses legitimate volumes, too large a mount
    // count stops enforcing the policy altogether.
    MT_EXPECT_EQ(limits.max_size_mb, static_cast<uint64_t>(262144));
    MT_EXPECT_EQ(limits.max_mounts, static_cast<std::size_t>(4));

    const agentenv::cfg::VolumeConfig config;
    MT_EXPECT_EQ(limits.max_size_mb, config.max_size_mb);
    MT_EXPECT_EQ(limits.max_mounts, config.max_volume_count);

    // The default sits below the ceiling, so the clamp in
    // `OpenWithRepositoryAndLimits` is a guard on operator input rather than
    // something the default itself depends on.
    MT_EXPECT_TRUE(limits.max_mounts < agentenv::volume::kMaxVolumeMounts);
}

MT_TEST(open_requires_a_catalog_path_with_a_parent) {
    std::shared_ptr<FakeVolumeRepository> repository(new FakeVolumeRepository("/tmp/unused"));
    const agentenv::volume::VolumeResult<std::shared_ptr<VolumeManager> > opened =
        VolumeManager::OpenWithRepository("catalog", repository);
    MT_EXPECT_TRUE(!opened.ok());
    MT_EXPECT_EQ(opened.error().kind, VolumeErrorKind::Storage);
}

MT_TEST(limits_are_clamped_to_the_firecracker_drive_ceiling) {
    std::shared_ptr<FakeVolumeRepository> repository(new FakeVolumeRepository("/tmp/unused"));
    agentenv::volume::VolumeLimits limits;
    limits.max_size_mb = 4096;
    limits.max_mounts = 1000;  // beyond /dev/vdc..vdz
    const agentenv::volume::VolumeResult<std::shared_ptr<VolumeManager> > opened =
        VolumeManager::OpenWithRepositoryAndLimits("/tmp/aenv/catalog", repository, limits);
    MT_EXPECT_TRUE(opened.ok());
    MT_EXPECT_EQ(opened.value()->limits().max_mounts, agentenv::volume::kMaxVolumeMounts);
    MT_EXPECT_EQ(opened.value()->limits().max_size_mb, static_cast<uint64_t>(4096));
}

MT_TEST(create_validates_name_size_and_sources) {
    Fixture fixture;

    MT_EXPECT_EQ(fixture.manager
                     ->Create("bad name", VolumeMode::Exclusive, Optional<std::string>(),
                              Optional<std::string>(), 1024)
                     .error(),
                 VolumeError::InvalidName());
    MT_EXPECT_EQ(fixture.manager
                     ->Create("zero", VolumeMode::Exclusive, Optional<std::string>(),
                              Optional<std::string>(), 0)
                     .error(),
                 VolumeError::InvalidSize());
    MT_EXPECT_EQ(fixture.manager
                     ->Create("huge", VolumeMode::Exclusive, Optional<std::string>(),
                              Optional<std::string>(),
                              fixture.manager->limits().max_size_mb + 1)
                     .error(),
                 VolumeError::SizeLimitExceeded(fixture.manager->limits().max_size_mb));
    MT_EXPECT_EQ(fixture.manager
                     ->Create("both", VolumeMode::Exclusive,
                              Optional<std::string>(std::string("vol_a")),
                              Optional<std::string>(std::string("/tmp/x.json")), 1024)
                     .error(),
                 VolumeError::MultipleSources());
    // A missing source is reported as SourceNotFound, not NotFound.
    MT_EXPECT_EQ(fixture.manager
                     ->Create("orphan", VolumeMode::Exclusive,
                              Optional<std::string>(std::string("vol_missing")),
                              Optional<std::string>(), 1024)
                     .error(),
                 VolumeError::SourceNotFound("vol_missing"));
}

MT_TEST(create_rejects_duplicate_names) {
    Fixture fixture;
    fixture.PublishedVolume();
    const agentenv::volume::VolumeResult<VolumeRecord> again = fixture.manager->Create(
        "parent", VolumeMode::Exclusive, Optional<std::string>(), Optional<std::string>(), 1024);
    MT_EXPECT_EQ(again.error(), VolumeError::NameConflict("parent"));
}

MT_TEST(published_volume_publishes_local_layers_and_keeps_external_refs) {
    Fixture fixture;
    const VolumeRecord parent = fixture.PublishedVolume();

    MT_EXPECT_EQ(parent.status, VolumeStatus::Ready);
    MT_EXPECT_EQ(parent.backing_layers.size(), static_cast<std::size_t>(2));
    // The local layer became repository-owned, digested by content.
    MT_EXPECT_TRUE(parent.backing_layers[0].is_managed());
    MT_EXPECT_EQ(parent.backing_layers[0].managed.digest,
                 agentenv::core::Sha256Digest("published volume layer"));
    MT_EXPECT_EQ(parent.backing_layers[0].managed.size, static_cast<uint64_t>(22));
    // The remote layer stayed a reference.
    MT_EXPECT_TRUE(parent.backing_layers[1].is_external());
    MT_EXPECT_EQ(parent.backing_layers[1].external.repo_blob_url,
                 std::string("https://registry.example.test/v2/cache/blobs"));
}

MT_TEST(published_volume_clones_survive_parent_deletion_without_local_files) {
    // Rust: `published_volume_clones_survive_parent_deletion_without_local_files`.
    Fixture fixture;
    const VolumeRecord parent = fixture.PublishedVolume();

    // A stale local config must not override the committed backing.
    MT_EXPECT_TRUE(
        agentenv::core::fs::Write(*parent.backing_image_config, "stale config").ok());

    std::vector<VolumeRecord> children;
    const VolumeMode modes[] = {VolumeMode::ReadOnly, VolumeMode::Exclusive};
    for (std::size_t index = 0; index < 2; ++index) {
        if (index == 1) fixture.Reopen();  // cold manager, empty local cache
        std::ostringstream name;
        name << "child-" << index;
        const agentenv::volume::VolumeResult<VolumeRecord> child =
            fixture.manager->Create(name.str(), modes[index],
                                    Optional<std::string>(parent.id),
                                    Optional<std::string>(), 1024);
        MT_EXPECT_TRUE(child.ok());
        // Cloning committed layers copies descriptors, never local files.
        MT_EXPECT_TRUE(agentenv::snapshot::LayerRefsEqual(child.value().backing_layers,
                                                          parent.backing_layers));
        MT_EXPECT_EQ(child.value().mode, modes[index]);
        MT_EXPECT_TRUE(!child.value().backing_image_config.has_value());
        MT_EXPECT_TRUE(!agentenv::core::fs::Exists(fixture.manager->DataDir(child.value().id)));
        children.push_back(child.value());
    }

    MT_EXPECT_TRUE(fixture.manager->Remove(parent.id).ok());
    fixture.Reopen();

    for (std::size_t i = 0; i < children.size(); ++i) {
        const agentenv::volume::VolumeResult<VolumeRecord> materialized =
            fixture.manager->MaterializeBacking(children[i].id);
        MT_EXPECT_TRUE(materialized.ok());
        const agentenv::core::Expected<obd::ImageConfig, std::string> config =
            obd::LoadImageConfig(*materialized.value().backing_image_config);
        MT_EXPECT_TRUE(config.ok());
        // The managed layer's bytes survived the parent's deletion.
        const agentenv::core::Expected<std::string, std::string> bytes =
            agentenv::core::fs::ReadToString(config.value().lowers[0].file);
        MT_EXPECT_TRUE(bytes.ok());
        MT_EXPECT_EQ(bytes.value(), std::string("published volume layer"));
        MT_EXPECT_EQ(config.value().lowers[0].digest,
                     agentenv::core::Sha256Digest("published volume layer"));
        MT_EXPECT_EQ(config.value().lowers[0].size, static_cast<uint64_t>(22));
        // The external layer has no local file, only its URL.
        MT_EXPECT_TRUE(config.value().lowers[1].file.empty());
        MT_EXPECT_EQ(config.value().lowers[1].repo_blob_url,
                     std::string("https://registry.example.test/v2/cache/blobs"));
    }
}

MT_TEST(published_volume_clone_validates_source_state_and_size) {
    // Rust: `published_volume_clone_validates_source_state_and_size`.
    Fixture fixture;
    VolumeRecord parent = fixture.PublishedVolume();

    MT_EXPECT_EQ(fixture.manager
                     ->Create("wrong-size", VolumeMode::ReadOnly,
                              Optional<std::string>(parent.id), Optional<std::string>(), 2048)
                     .error(),
                 VolumeError::SizeMismatch());

    MT_EXPECT_TRUE(fixture.manager->Reserve(parent.id, "owner").ok());
    MT_EXPECT_EQ(fixture.manager
                     ->Create("reserved", VolumeMode::ReadOnly,
                              Optional<std::string>(parent.id), Optional<std::string>(), 1024)
                     .error(),
                 VolumeError::Reserved("owner"));

    // Uploading and Failed sources are both unusable, with distinct errors.
    const VolumeStatus statuses[] = {VolumeStatus::Uploading, VolumeStatus::Failed};
    for (std::size_t i = 0; i < 2; ++i) {
        parent.status = statuses[i];
        MT_EXPECT_TRUE(fixture.manager->PersistCatalog(parent).ok());
        const agentenv::volume::VolumeResult<VolumeRecord> unready = fixture.manager->Create(
            "unready", VolumeMode::ReadOnly, Optional<std::string>(parent.id),
            Optional<std::string>(), 1024);
        const VolumeError expected = statuses[i] == VolumeStatus::Uploading
                                         ? VolumeError::Uploading(parent.id)
                                         : VolumeError::Failed(parent.id);
        MT_EXPECT_EQ(unready.error(), expected);
    }
}

MT_TEST(reserved_volume_clone_retains_unpublished_local_layers) {
    // Rust: `reserved_volume_clone_retains_unpublished_local_layers`.
    Fixture fixture;
    const VolumeRecord parent = fixture.PublishedVolume();
    MT_EXPECT_TRUE(fixture.manager->Reserve(parent.id, "source").ok());
    fixture.AppendLocalLayer(parent);

    const agentenv::volume::VolumeResult<VolumeRecord> child =
        fixture.manager->CreateChildForOwner(parent.id, "fork", VolumeMode::Exclusive, 1024,
                                             "source", "child");
    MT_EXPECT_TRUE(child.ok());
    // A reserved parent may hold newer local writes, so the child clones files
    // instead of inheriting descriptors.
    MT_EXPECT_TRUE(child.value().backing_layers.empty());
    MT_EXPECT_TRUE(child.value().backing_image_config.has_value());

    std::vector<std::string> parent_ids;
    parent_ids.push_back(parent.id);
    MT_EXPECT_TRUE(
        fixture.manager->ReplaceOwnerFor("source", Optional<std::string>(), parent_ids).ok());
    MT_EXPECT_TRUE(fixture.manager->Remove(parent.id).ok());

    std::vector<std::string> child_ids;
    child_ids.push_back(child.value().id);
    MT_EXPECT_TRUE(fixture.manager->RecoverAndPublishBackings("child", child_ids).ok());

    const agentenv::volume::VolumeResult<VolumeRecord> published =
        fixture.manager->Get(child.value().id);
    MT_EXPECT_TRUE(published.ok());
    MT_EXPECT_EQ(published.value().backing_layers.size(), static_cast<std::size_t>(3));
    ManagedLayer expected;
    expected.digest = agentenv::core::Sha256Digest("new volume writes");
    expected.size = 17;
    MT_EXPECT_TRUE(published.value().backing_layers[2] == OverlaybdLayerRef::Managed(expected));
}

MT_TEST(volume_generations_preserve_inherited_descriptors_and_new_writes) {
    // Rust: `volume_generations_preserve_inherited_descriptors_and_new_writes`.
    Fixture fixture;
    const VolumeRecord parent = fixture.PublishedVolume();

    const agentenv::volume::VolumeResult<VolumeRecord> seed = fixture.manager->Create(
        "seed", VolumeMode::ReadOnly, Optional<std::string>(parent.id), Optional<std::string>(),
        1024);
    MT_EXPECT_TRUE(seed.ok());
    MT_EXPECT_TRUE(fixture.manager->Reserve(seed.value().id, "builder").ok());

    const agentenv::volume::VolumeResult<VolumeRecord> child =
        fixture.manager->CreateChildForOwner(seed.value().id, "work", VolumeMode::Exclusive, 1024,
                                             "builder", "builder");
    MT_EXPECT_TRUE(child.ok());
    // The seed holds no local writes, so the child inherits descriptors.
    MT_EXPECT_TRUE(agentenv::snapshot::LayerRefsEqual(child.value().backing_layers,
                                                      parent.backing_layers));

    const agentenv::volume::VolumeResult<VolumeRecord> materialized =
        fixture.manager->MaterializeBacking(child.value().id);
    MT_EXPECT_TRUE(materialized.ok());
    fixture.AppendLocalLayer(materialized.value());

    std::vector<std::string> child_ids;
    child_ids.push_back(child.value().id);
    MT_EXPECT_TRUE(fixture.manager->RecoverAndPublishBackings("builder", child_ids).ok());

    // The builder now releases both leases; without this the volumes stay
    // reserved and cannot seed a further generation.
    std::vector<std::string> builder_ids;
    builder_ids.push_back(seed.value().id);
    builder_ids.push_back(child.value().id);
    MT_EXPECT_TRUE(
        fixture.manager->ReplaceOwnerFor("builder", Optional<std::string>(), builder_ids).ok());

    const agentenv::volume::VolumeResult<VolumeRecord> published =
        fixture.manager->Get(child.value().id);
    MT_EXPECT_TRUE(published.ok());
    MT_EXPECT_EQ(published.value().backing_layers.size(), static_cast<std::size_t>(3));
    // Inherited descriptors are byte-identical; only the new write is appended.
    MT_EXPECT_TRUE(published.value().backing_layers[0] == parent.backing_layers[0]);
    MT_EXPECT_TRUE(published.value().backing_layers[1] == parent.backing_layers[1]);
    ManagedLayer expected;
    expected.digest = agentenv::core::Sha256Digest("new volume writes");
    expected.size = 17;
    MT_EXPECT_TRUE(published.value().backing_layers[2] == OverlaybdLayerRef::Managed(expected));

    // The next generation seeds from the published child without local files.
    const agentenv::volume::VolumeResult<VolumeRecord> next_seed = fixture.manager->Create(
        "next-seed", VolumeMode::ReadOnly, Optional<std::string>(child.value().id),
        Optional<std::string>(), 1024);
    MT_EXPECT_TRUE(next_seed.ok());
    MT_EXPECT_TRUE(agentenv::snapshot::LayerRefsEqual(next_seed.value().backing_layers,
                                                      published.value().backing_layers));
    MT_EXPECT_TRUE(!next_seed.value().backing_image_config.has_value());
}

MT_TEST(create_from_snapshot_preserves_read_only_mode) {
    // Rust: `create_from_snapshot_preserves_read_only_mode`.
    Fixture fixture;
    ManagedLayer layer;
    layer.digest = "sha256:abc";
    layer.size = 4096;
    std::vector<OverlaybdLayerRef> layers;
    layers.push_back(OverlaybdLayerRef::Managed(layer));

    const agentenv::volume::VolumeResult<VolumeRecord> restored =
        fixture.manager->CreateFromSnapshot("restored-read-only", VolumeMode::ReadOnly, 1024,
                                            layers);
    MT_EXPECT_TRUE(restored.ok());
    MT_EXPECT_EQ(restored.value().mode, VolumeMode::ReadOnly);

    const agentenv::volume::VolumeResult<VolumeRecord> persisted =
        fixture.manager->Get(restored.value().id);
    MT_EXPECT_TRUE(persisted.ok());
    MT_EXPECT_EQ(persisted.value().mode, VolumeMode::ReadOnly);

    // An empty layer set is not a restorable snapshot.
    MT_EXPECT_EQ(fixture.manager
                     ->CreateFromSnapshot("empty", VolumeMode::ReadOnly, 1024,
                                          std::vector<OverlaybdLayerRef>())
                     .error(),
                 VolumeError::InvalidSize());
}

MT_TEST(delete_refuses_while_any_lease_is_held) {
    Fixture fixture;
    const VolumeRecord exclusive = fixture.PublishedVolume();
    MT_EXPECT_TRUE(fixture.manager->Reserve(exclusive.id, "owner").ok());
    MT_EXPECT_EQ(fixture.manager->Remove(exclusive.id).error(), VolumeError::Reserved("owner"));

    // Read-only mounts block deletion too.
    std::vector<OverlaybdLayerRef> layers;
    ManagedLayer layer;
    layer.digest = "sha256:abc";
    layer.size = 1;
    layers.push_back(OverlaybdLayerRef::Managed(layer));
    const agentenv::volume::VolumeResult<VolumeRecord> shared =
        fixture.manager->CreateFromSnapshot("shared", VolumeMode::ReadOnly, 1024, layers);
    MT_EXPECT_TRUE(shared.ok());
    MT_EXPECT_TRUE(fixture.manager->Reserve(shared.value().id, "reader").ok());
    MT_EXPECT_EQ(fixture.manager->Remove(shared.value().id).error(),
                 VolumeError::Reserved("reader"));

    // Releasing the lease unblocks it.
    std::vector<std::string> ids;
    ids.push_back(shared.value().id);
    MT_EXPECT_TRUE(fixture.manager->ReplaceOwnerFor("reader", Optional<std::string>(), ids).ok());
    MT_EXPECT_TRUE(fixture.manager->Remove(shared.value().id).ok());
    MT_EXPECT_EQ(fixture.manager->Get(shared.value().id).error(),
                 VolumeError::NotFound(shared.value().id));
}

MT_TEST(reserve_rejects_unusable_states_and_conflicting_owners) {
    Fixture fixture;
    VolumeRecord parent = fixture.PublishedVolume();

    MT_EXPECT_TRUE(fixture.manager->Reserve(parent.id, "first").ok());
    // A second exclusive reservation reports the incumbent.
    MT_EXPECT_EQ(fixture.manager->Reserve(parent.id, "second").error(),
                 VolumeError::Reserved("first"));

    parent.status = VolumeStatus::Uploading;
    MT_EXPECT_TRUE(fixture.manager->PersistCatalog(parent).ok());
    MT_EXPECT_EQ(fixture.manager->Reserve(parent.id, "third").error(),
                 VolumeError::Uploading(parent.id));

    parent.status = VolumeStatus::Failed;
    MT_EXPECT_TRUE(fixture.manager->PersistCatalog(parent).ok());
    MT_EXPECT_EQ(fixture.manager->Reserve(parent.id, "third").error(),
                 VolumeError::Failed(parent.id));
}

MT_TEST(read_only_reservations_accumulate_and_are_idempotent) {
    Fixture fixture;
    std::vector<OverlaybdLayerRef> layers;
    ManagedLayer layer;
    layer.digest = "sha256:abc";
    layer.size = 1;
    layers.push_back(OverlaybdLayerRef::Managed(layer));
    const agentenv::volume::VolumeResult<VolumeRecord> shared =
        fixture.manager->CreateFromSnapshot("shared", VolumeMode::ReadOnly, 1024, layers);
    MT_EXPECT_TRUE(shared.ok());

    MT_EXPECT_TRUE(fixture.manager->Reserve(shared.value().id, "a").ok());
    MT_EXPECT_TRUE(fixture.manager->Reserve(shared.value().id, "b").ok());
    MT_EXPECT_TRUE(fixture.manager->Reserve(shared.value().id, "a").ok());  // idempotent

    const agentenv::volume::VolumeResult<VolumeRecord> reloaded =
        fixture.manager->Get(shared.value().id);
    MT_EXPECT_TRUE(reloaded.ok());
    MT_EXPECT_EQ(reloaded.value().read_only_mounts.size(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(reloaded.value().MountedBy("a"));
    MT_EXPECT_TRUE(reloaded.value().MountedBy("b"));
    // A read-only volume never takes an exclusive owner.
    MT_EXPECT_TRUE(!reloaded.value().reserved_by_sandbox_id.has_value());
}

MT_TEST(fail_backings_only_touches_volumes_owned_by_the_caller) {
    Fixture fixture;
    const VolumeRecord parent = fixture.PublishedVolume();
    MT_EXPECT_TRUE(fixture.manager->Reserve(parent.id, "owner").ok());

    std::vector<std::string> ids;
    ids.push_back(parent.id);
    // A different owner must not be able to fail someone else's volume.
    MT_EXPECT_TRUE(fixture.manager->FailBackings("stranger", ids).ok());
    MT_EXPECT_EQ(fixture.manager->Get(parent.id).value().status, VolumeStatus::Ready);

    MT_EXPECT_TRUE(fixture.manager->FailBackings("owner", ids).ok());
    MT_EXPECT_EQ(fixture.manager->Get(parent.id).value().status, VolumeStatus::Failed);
}

MT_TEST(recover_backings_fails_volume_whose_local_backing_vanished) {
    Fixture fixture;
    const VolumeRecord parent = fixture.PublishedVolume();
    MT_EXPECT_TRUE(fixture.manager->Reserve(parent.id, "owner").ok());

    // Simulate a restart that lost the node-local directory.
    MT_EXPECT_TRUE(
        agentenv::core::fs::RemoveDirAll(fixture.manager->DataDir(parent.id)).ok());
    fixture.Reopen();

    std::vector<std::string> ids;
    ids.push_back(parent.id);
    const agentenv::core::Expected<Unit, VolumeError> recovered =
        fixture.manager->RecoverBackings("owner", ids);
    MT_EXPECT_TRUE(!recovered.ok());
    MT_EXPECT_EQ(recovered.error().kind, VolumeErrorKind::Storage);
    // The volume is marked Failed so nobody mounts stale content.
    MT_EXPECT_EQ(fixture.manager->Get(parent.id).value().status, VolumeStatus::Failed);
}

MT_TEST(recover_backings_restores_the_deterministic_local_path) {
    Fixture fixture;
    const VolumeRecord parent = fixture.PublishedVolume();
    MT_EXPECT_TRUE(fixture.manager->Reserve(parent.id, "owner").ok());

    // A cold manager has no local cache, but the file is still on disk.
    fixture.Reopen();
    MT_EXPECT_TRUE(!fixture.manager->Get(parent.id).value().backing_image_config.has_value());

    std::vector<std::string> ids;
    ids.push_back(parent.id);
    MT_EXPECT_TRUE(fixture.manager->RecoverBackings("owner", ids).ok());

    const agentenv::volume::VolumeResult<VolumeRecord> recovered =
        fixture.manager->Get(parent.id);
    MT_EXPECT_TRUE(recovered.value().backing_image_config.has_value());
    MT_EXPECT_EQ(*recovered.value().backing_image_config,
                 agentenv::core::fs::Join(fixture.manager->DataDir(parent.id), "image.json"));
}

MT_TEST(list_page_paginates_and_validates_its_cursor) {
    Fixture fixture;
    // Snapshot-restored volumes need no external tooling, so they are the
    // cheapest way to fill a catalog.
    std::vector<OverlaybdLayerRef> layers;
    ManagedLayer layer;
    layer.digest = "sha256:abc";
    layer.size = 1;
    layers.push_back(OverlaybdLayerRef::Managed(layer));
    for (int i = 0; i < 5; ++i) {
        std::ostringstream name;
        name << "vol-" << i;
        MT_EXPECT_TRUE(
            fixture.manager->CreateFromSnapshot(name.str(), VolumeMode::ReadOnly, 1024, layers)
                .ok());
    }

    MT_EXPECT_EQ(fixture.manager->ListPage(Optional<std::string>(), 0).error(),
                 VolumeError::InvalidPageLimit());
    MT_EXPECT_EQ(
        fixture.manager->ListPage(Optional<std::string>(std::string("!!!")), 2).error(),
        VolumeError::InvalidNextToken());

    // Walk every page and make sure each record appears exactly once.
    std::vector<std::string> seen;
    Optional<std::string> cursor;
    for (int guard = 0; guard < 10; ++guard) {
        const agentenv::volume::VolumeResult<agentenv::volume::VolumePage> page =
            fixture.manager->ListPage(cursor, 2);
        MT_EXPECT_TRUE(page.ok());
        for (std::size_t i = 0; i < page.value().records.size(); ++i) {
            seen.push_back(page.value().records[i].id);
        }
        if (!page.value().next_token.has_value()) break;
        cursor = page.value().next_token;
    }
    MT_EXPECT_EQ(seen.size(), static_cast<std::size_t>(5));
    for (std::size_t i = 1; i < seen.size(); ++i) {
        MT_EXPECT_TRUE(seen[i - 1] < seen[i]);  // strictly increasing: no repeats
    }
}

MT_TEST(get_reports_not_found_for_unknown_references) {
    Fixture fixture;
    MT_EXPECT_EQ(fixture.manager->Get("vol_missing").error(),
                 VolumeError::NotFound("vol_missing"));
    // Lookups resolve by name as well as by id.
    const VolumeRecord parent = fixture.PublishedVolume();
    MT_EXPECT_EQ(fixture.manager->Get("parent").value().id, parent.id);
}

MT_TEST(unsupported_repository_reports_the_upstream_feature_names) {
    // A backend that implements only the snapshot half must fail loudly.
    class SnapshotOnlyRepository : public repo::SnapshotRepository {
     public:
        // `override` throughout: without it these were lookalike members, the
        // class quietly stayed abstract when the interface changed, and the
        // mismatch surfaced at the `new` below instead of at the declaration.
        repo::RepositoryResult<repo::SnapshotRecord> Create(
            const repo::SnapshotRecord& r) override {
            return r;
        }
        repo::RepositoryResult<Optional<repo::SnapshotRecord> > Get(
            const std::string&) override {
            return Optional<repo::SnapshotRecord>();
        }
        repo::RepositoryResult<std::vector<repo::SnapshotRecord> > List(
            const repo::SnapshotListFilter&) override {
            return std::vector<repo::SnapshotRecord>();
        }
        repo::RepositoryResult<Unit> Delete(const std::string&) override { return Unit(); }
        repo::RepositoryResult<Optional<agentenv::core::SnapshotId> > ResolveAlias(
            const std::string&) override {
            return Optional<agentenv::core::SnapshotId>();
        }
        repo::RepositoryResult<repo::SnapshotRecord> TryStartBuild(
            const agentenv::core::SnapshotId&) override {
            return repo::SnapshotRecord();
        }
        repo::RepositoryResult<Unit> MarkBuildError(
            const agentenv::core::SnapshotId&,
            const agentenv::snapshot::TemplateBuildErrorReason&) override {
            return Unit();
        }
    };

    std::shared_ptr<repo::SnapshotRepository> repository(new SnapshotOnlyRepository());
    const agentenv::volume::VolumeResult<std::shared_ptr<VolumeManager> > opened =
        VolumeManager::OpenWithRepository("/tmp/aenv/catalog", repository);
    MT_EXPECT_TRUE(opened.ok());

    // The catalog is built on documents, so that is the feature reported.
    const agentenv::volume::VolumeResult<VolumeRecord> got = opened.value()->Get("anything");
    MT_EXPECT_EQ(got.error().kind, VolumeErrorKind::Storage);
    MT_EXPECT_TRUE(got.error().detail.find("repository documents") != std::string::npos);
}

int main() { return microtest::RunAll(); }
