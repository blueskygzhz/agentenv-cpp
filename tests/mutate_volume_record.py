#!/usr/bin/env python3
"""Mutation harness for the `VolumeRecord` half of Rust's src/volume/mod.rs.

This covers the part of the volume module that has no behaviour of its own:
the record struct, its three guard methods, the two enums, the fifteen error
variants, the pagination cursor codec and the two validators. Nothing here
starts a VM or touches a disk, which is exactly why it is worth mutating --
every one of these is a pure function whose only job is to agree with the
other implementation, and a disagreement shows up not as a crash but as a
catalog entry one side can write and the other cannot read.

Two groups are load-bearing in ways that are easy to miss:

- The *document* format. Rust's `encode_record` is
  `serde_json::to_vec_pretty(record)` into `volumes/records/<id>.json`, and
  this port writes the same path. `#[serde(rename_all = "camelCase")]` governs
  the keys while the enum *values* follow their own rules -- `VolumeMode` has
  no rename and stays PascalCase, `VolumeStatus` is `rename_all = "lowercase"`
  -- so there are four separate conventions in one document and swapping any
  of them produces a record the other side rejects outright.
- The cursor codec. `URL_SAFE_NO_PAD` decoding is strict about padding,
  length and trailing bits. A lenient decoder does not fail; it makes two
  distinct tokens name the same page, which turns pagination into a silent
  loop or a silent skip.

Two properties are deliberately **not** mutated here, because no assertion can
distinguish them.

The first is admitting the *standard* base64 alphabet in the cursor decoder
alongside the url-safe one. It looks like an obvious hole and is not one: `+`
and `/` carry the 6-bit values 62 and 63, and a group of either forces a
decoded byte of `>`, `~`, or something above 0x7F, whichever position it lands
in. `is_valid_volume_component` admits none of those, so the payload check at
the end of the decoder refuses the token anyway -- with the same
`InvalidNextToken` the alphabet check would have produced. The mutation is
therefore behaviour-preserving rather than uncaught, and it is also the reason
the same argument runs the other way: a canonical cursor for a *valid* volume
id can never contain `-` or `_` either.

The second is the key *order* in the encoded document. `serde_json` emits
fields in declaration order and `core::JsonObject` is a `std::map`, so this
port emits them sorted. Neither parser is order-sensitive, so the two are
compatible in both directions and no test can tell the orders apart through
`FromJson`; the only observable consequence is that a digest of a record
document is not portable between the implementations, and that is a property
of `core::Json` port-wide rather than of volumes. It is recorded in
`src/volume/record.cc` rather than pretended about here.

Status is decided **by exit code only**: the suite calls `exit(1)` on the
first failing assertion, so its summary line is not printed on failure and
scraping stdout would misreport a caught mutation as a survivor.
"""

import pathlib
import shutil
import subprocess
import sys

# `tests/` is not a package and a harness runs as a script from the repo
# root, so the shared guard is imported by path rather than by package.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import mutation_residue


ROOT = pathlib.Path(__file__).resolve().parent.parent
RECORD = ROOT / "src/volume/record.cc"
TARGET = "test_volume"

MUTATIONS = [
    # ---- is_valid_volume_component -----------------------------------------
    #
    # This predicate is the only thing between a decoded pagination cursor and
    # the data directory, so each relaxation below is a path-traversal hole
    # rather than a cosmetic one.
    (
        "an_empty_component_is_valid",
        RECORD,
        """    if (value.empty() || value.size() > 128) return false;""",
        """    if (value.size() > 128) return false;""",
    ),
    (
        "the_length_ceiling_is_dropped",
        RECORD,
        """    if (value.empty() || value.size() > 128) return false;""",
        """    if (value.empty()) return false;""",
    ),
    # Off by one at the boundary, which would make a 128-byte id -- a legal one
    # upstream -- unreadable here.
    (
        "the_length_ceiling_is_off_by_one",
        RECORD,
        """    if (value.empty() || value.size() > 128) return false;""",
        """    if (value.empty() || value.size() >= 128) return false;""",
    ),
    # `.` and `/` admitted: a cursor decoding to "../../etc/passwd" becomes a
    # valid volume id.
    (
        "dots_and_slashes_are_admitted",
        RECORD,
        """        if (!IsAsciiAlphanumeric(byte) && byte != '_' && byte != '-') return false;""",
        """        if (byte == 0) return false;""",
    ),
    # Non-ASCII bytes admitted. Upstream's check is `is_ascii_alphanumeric`,
    # and this is also what makes the UTF-8 check in the cursor decoder
    # redundant -- relax it and a non-UTF8 cursor starts round-tripping.
    (
        "non_ascii_bytes_are_admitted",
        RECORD,
        """    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9');""",
        """    return byte >= 'a' || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9');""",
    ),

    # ---- the enum spellings ------------------------------------------------
    #
    # Four conventions in one document. `VolumeMode` carries no `rename_all`
    # and `VolumeStatus` is `rename_all = "lowercase"`, so each of these writes
    # a value the other implementation's derive rejects.
    (
        "the_mode_is_written_in_lower_case",
        RECORD,
        """        case VolumeMode::ReadOnly:
            return "ReadOnly";
        case VolumeMode::Exclusive:
            return "Exclusive";""",
        """        case VolumeMode::ReadOnly:
            return "readOnly";
        case VolumeMode::Exclusive:
            return "exclusive";""",
    ),
    (
        "the_status_is_written_in_pascal_case",
        RECORD,
        """        case VolumeStatus::Ready:
            return "ready";
        case VolumeStatus::Uploading:
            return "uploading";
        case VolumeStatus::Failed:
            return "failed";""",
        """        case VolumeStatus::Ready:
            return "Ready";
        case VolumeStatus::Uploading:
            return "Uploading";
        case VolumeStatus::Failed:
            return "Failed";""",
    ),
    # The mode parser turns case-insensitive, so a document written with the
    # wrong convention loads here and nowhere else -- which is worse than
    # failing, because the record then propagates.
    (
        "the_mode_parser_accepts_the_wrong_case",
        RECORD,
        """    if (raw == "ReadOnly") return VolumeMode::ReadOnly;
    if (raw == "Exclusive") return VolumeMode::Exclusive;""",
        """    if (raw == "ReadOnly" || raw == "readOnly") return VolumeMode::ReadOnly;
    if (raw == "Exclusive" || raw == "exclusive") return VolumeMode::Exclusive;""",
    ),
    (
        "the_status_parser_accepts_the_wrong_case",
        RECORD,
        """    if (raw == "ready") return VolumeStatus::Ready;""",
        """    if (raw == "ready" || raw == "Ready") return VolumeStatus::Ready;""",
    ),
    # An unknown status defaults to Ready instead of failing. This is the worst
    # mutation in the file: a record whose status nobody can read is reported
    # as a healthy, mountable volume.
    (
        "an_unknown_status_defaults_to_ready",
        RECORD,
        """    return core::make_unexpected("unknown variant `" + raw +
                                 "`, expected one of `ready`, `uploading`, `failed`");""",
        """    return VolumeStatus::Ready;""",
    ),
    # serde's wording for the two counts is swapped. Two variants read
    # "`a` or `b`", three or more read "one of `a`, `b`, `c`".
    (
        "the_variant_rejection_uses_the_wrong_serde_wording",
        RECORD,
        """                                 "`, expected one of `ready`, `uploading`, `failed`");""",
        """                                 "`, expected `ready`, `uploading` or `failed`");""",
    ),

    # ---- mounted_by --------------------------------------------------------
    #
    # Two lease kinds, and this is what `delete` consults. Losing either half
    # lets a volume be removed out from under a running sandbox.
    (
        "only_the_exclusive_lease_counts_as_mounted",
        RECORD,
        """    for (std::size_t i = 0; i < read_only_mounts.size(); ++i) {
        if (read_only_mounts[i] == owner) return true;
    }
    return false;
}""",
        """    return false;
}""",
    ),
    (
        "only_the_read_only_leases_count_as_mounted",
        RECORD,
        """    if (reserved_by_sandbox_id.has_value() && *reserved_by_sandbox_id == owner) return true;""",
        """    // exclusive owner ignored""",
    ),
    # An unset exclusive lease matches the empty owner, so a record nobody
    # holds reports as mounted by "".
    (
        "an_unset_lease_matches_the_empty_owner",
        RECORD,
        """    if (reserved_by_sandbox_id.has_value() && *reserved_by_sandbox_id == owner) return true;""",
        """    if (reserved_by_sandbox_id.value_or(std::string()) == owner) return true;""",
    ),

    # ---- replace_owner -----------------------------------------------------
    #
    # Used when a fork hands a lease to its child. A lease left on the parent
    # pins a volume that is no longer attached to anything.
    (
        "the_exclusive_lease_is_not_moved",
        RECORD,
        """    if (reserved_by_sandbox_id.has_value() && *reserved_by_sandbox_id == from) {
        reserved_by_sandbox_id = to;
        changed = true;
    }""",
        """    // exclusive lease left in place""",
    ),
    # The release case stops clearing, so `to == None` keeps the old owner.
    (
        "releasing_a_lease_keeps_the_old_owner",
        RECORD,
        """        reserved_by_sandbox_id = to;
        changed = true;""",
        """        if (to.has_value()) reserved_by_sandbox_id = to;
        changed = true;""",
    ),
    # The read-only lease is handed over without being dropped first, so the
    # departing sandbox keeps a mount it no longer has.
    (
        "the_old_read_only_holder_is_not_removed",
        RECORD,
        """            if (read_only_mounts[i] != from) kept.push_back(read_only_mounts[i]);""",
        """            kept.push_back(read_only_mounts[i]);""",
    ),
    # The duplicate check goes, so a target that already held a read-only lease
    # ends up listed twice -- and one `replace_owner` back releases only one of
    # them.
    (
        "a_read_only_lease_can_be_duplicated",
        RECORD,
        """            if (!already_present) read_only_mounts.push_back(*to);""",
        """            read_only_mounts.push_back(*to);""",
    ),
    # The return value is what tells the caller to persist the record. Always
    # reporting a change costs a write; never reporting one loses the lease
    # move entirely.
    (
        "replace_owner_always_reports_a_change",
        RECORD,
        """    return changed;
}""",
        """    return true;
}""",
    ),

    # ---- validate_catalog_update -------------------------------------------
    #
    # The durable catalog's only guard against an illegal transition.
    (
        "a_deleting_record_still_accepts_updates",
        RECORD,
        """    if (deleting) {""",
        """    if (false) {""",
    ),
    # Identity fields become mutable, so a rename or a resize lands on a volume
    # whose on-disk image is a fixed size.
    (
        "identity_fields_become_mutable",
        RECORD,
        """    if (name != next.name || mode != next.mode || size_mb != next.size_mb) {""",
        """    if (false) {""",
    ),
    (
        "the_size_may_change_as_long_as_the_name_does_not",
        RECORD,
        """    if (name != next.name || mode != next.mode || size_mb != next.size_mb) {""",
        """    if (name != next.name || mode != next.mode) {""",
    ),
    (
        "the_mode_may_change",
        RECORD,
        """    if (name != next.name || mode != next.mode || size_mb != next.size_mb) {""",
        """    if (name != next.name || size_mb != next.size_mb) {""",
    ),
    # A backing swap no longer has to pass through Uploading, so a reader can
    # observe a volume whose layers changed under it.
    (
        "the_backing_can_be_replaced_without_uploading",
        RECORD,
        """    if (!snapshot::LayerRefsEqual(backing_layers, next.backing_layers) &&
        status != VolumeStatus::Uploading) {""",
        """    if (false) {""",
    ),
    # The state half of that check is dropped: every backing change is allowed
    # as long as the layers differ, which is the same hole worded differently.
    (
        "the_uploading_requirement_is_dropped",
        RECORD,
        """    if (!snapshot::LayerRefsEqual(backing_layers, next.backing_layers) &&
        status != VolumeStatus::Uploading) {""",
        """    if (!snapshot::LayerRefsEqual(backing_layers, next.backing_layers) && false) {""",
    ),
    # Failed jumps straight back to Ready, so a volume whose publication failed
    # is advertised as mountable without anything having republished it.
    (
        "a_failed_volume_can_become_ready_directly",
        RECORD,
        """    if (status == VolumeStatus::Failed && next.status == VolumeStatus::Ready) {""",
        """    if (false) {""",
    ),

    # ---- record equality ---------------------------------------------------
    #
    # `backing_image_config` is `#[serde(skip)]` yet part of Rust's derived
    # `PartialEq`. Dropping it looks free because it is absent from the
    # document, but `current_local_backing` compares records to decide whether
    # a node-local image config may be reused.
    (
        "equality_ignores_the_local_backing_path",
        RECORD,
        """    if (backing_image_config.has_value() != other.backing_image_config.has_value()) return false;""",
        """    if (false) return false;""",
    ),
    (
        "equality_ignores_the_deleting_flag",
        RECORD,
        """        status != other.status || deleting != other.deleting) {""",
        """        status != other.status) {""",
    ),
    # Order stops counting, so two records with the same readers in a different
    # order compare equal.
    (
        "equality_ignores_the_read_only_mount_order",
        RECORD,
        """    return read_only_mounts == other.read_only_mounts;""",
        """    return read_only_mounts.size() == other.read_only_mounts.size();""",
    ),

    # ---- the error variants ------------------------------------------------
    #
    # These texts are the API's 4xx bodies. `VolumeError`'s derived `Eq` is
    # also what the upstream tests assert against, so a payload dropped from
    # the comparison makes two different failures indistinguishable.
    (
        "the_size_limit_payload_is_dropped_from_equality",
        RECORD,
        """        case VolumeErrorKind::SizeLimitExceeded:
            return max_size_mb == other.max_size_mb;""",
        """        case VolumeErrorKind::SizeLimitExceeded:
            return true;""",
    ),
    (
        "the_mount_count_payload_is_dropped_from_equality",
        RECORD,
        """        case VolumeErrorKind::TooManyMountedVolumes:
            return max_count == other.max_count;""",
        """        case VolumeErrorKind::TooManyMountedVolumes:
            return true;""",
    ),
    (
        "the_detail_payload_is_dropped_from_equality",
        RECORD,
        """        default:
            return detail == other.detail;""",
        """        default:
            return true;""",
    ),
    (
        "the_name_conflict_message_loses_its_name",
        RECORD,
        """            oss << "volume name already exists: " << detail;""",
        """            oss << "volume name already exists";""",
    ),
    (
        "the_size_limit_message_reports_the_wrong_unit",
        RECORD,
        """            oss << "volume size exceeds the configured maximum of " << max_size_mb << " MiB";""",
        """            oss << "volume size exceeds the configured maximum of " << max_size_mb << " MB";""",
    ),
    (
        "the_storage_message_loses_its_cause",
        RECORD,
        """            oss << "volume catalog storage failed: " << detail;""",
        """            return "volume catalog storage failed";""",
    ),

    # ---- the cursor codec --------------------------------------------------
    #
    # A lenient decoder does not fail. It makes two distinct tokens name the
    # same page, so a client paging a volume list either loops or skips.
    (
        "a_bad_cursor_length_is_accepted",
        RECORD,
        """    if (token.empty() || token.size() % 4 == 1) {""",
        """    if (token.empty()) {""",
    ),
    (
        "non_canonical_trailing_bits_are_accepted",
        RECORD,
        """    if (bits > 0 && (buffer & ((1u << bits) - 1)) != 0) {
        return core::make_unexpected(VolumeError::InvalidNextToken());
    }""",
        """    // trailing bits unchecked""",
    ),
    # The payload check goes, so a cursor decoding to "../../etc/passwd" is
    # handed to the catalog as a volume id.
    (
        "a_decoded_cursor_is_not_validated",
        RECORD,
        """    if (!IsValidVolumeComponent(decoded)) {
        return core::make_unexpected(VolumeError::InvalidNextToken());
    }""",
        """    // decoded payload unchecked""",
    ),
    # The encoder emits padding, which upstream's `URL_SAFE_NO_PAD` decoder
    # rejects -- so a cursor this node hands out is unusable against another.
    (
        "the_encoder_pads_its_output",
        RECORD,
        """        out.push_back(kBase64UrlAlphabet[(chunk >> 12) & 0x3F]);
        // No padding: `URL_SAFE_NO_PAD`.""",
        """        out.push_back(kBase64UrlAlphabet[(chunk >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');""",
    ),
    # The two-leftover-byte tail loses its third character, which silently
    # truncates the encoded id.
    (
        "the_two_byte_tail_is_truncated",
        RECORD,
        """        out.push_back(kBase64UrlAlphabet[(chunk >> 6) & 0x3F]);
    }
    return out;""",
        """    }
    return out;""",
    ),

    # ---- the validators ----------------------------------------------------
    #
    # Same predicate, deliberately different errors: a bad name came from the
    # caller, a bad id came out of the catalog.
    (
        "a_corrupt_catalog_id_is_blamed_on_the_caller",
        RECORD,
        """    std::ostringstream oss;
    oss << "invalid volume id '" << id << "'";
    return core::make_unexpected(VolumeError::Storage(oss.str()));""",
        """    return core::make_unexpected(VolumeError::InvalidName());""",
    ),
    (
        "the_invalid_id_message_loses_the_id",
        RECORD,
        """    oss << "invalid volume id '" << id << "'";""",
        """    oss << "invalid volume id";""",
    ),

    # ---- the document format -----------------------------------------------
    #
    # Rust's `encode_record` writes this document to
    # `volumes/records/<id>.json`, which is the same path this port writes. A
    # mismatched key is not cosmetic: the field is required on the other side,
    # so its parse fails outright on a record this one wrote.
    (
        "the_size_key_reverts_to_snake_case",
        RECORD,
        """    object["sizeMb"] = core::Json(static_cast<int64_t>(size_mb));""",
        """    object["size_mb"] = core::Json(static_cast<int64_t>(size_mb));""",
    ),
    (
        "the_reservation_key_reverts_to_snake_case",
        RECORD,
        """    object["reservedBySandboxId"] = reserved_by_sandbox_id.has_value()""",
        """    object["reserved_by_sandbox_id"] = reserved_by_sandbox_id.has_value()""",
    ),
    (
        "the_layers_key_reverts_to_snake_case",
        RECORD,
        """    object["backingLayers"] = snapshot::LayerRefsToJson(backing_layers);""",
        """    object["backing_layers"] = snapshot::LayerRefsToJson(backing_layers);""",
    ),
    (
        "the_mounts_key_reverts_to_snake_case",
        RECORD,
        """    object["readOnlyMounts"] = core::Json(mounts);""",
        """    object["read_only_mounts"] = core::Json(mounts);""",
    ),
    # `deleting` gains a `false`, which upstream's
    # `skip_serializing_if = "std::ops::Not::not"` never writes.
    (
        "a_false_deleting_flag_is_written",
        RECORD,
        """    if (deleting) object["deleting"] = core::Json(true);""",
        """    object["deleting"] = core::Json(deleting);""",
    ),
    # The node-local cache path reaches the durable record, which makes the
    # catalog node-specific: another node reads a path to a file it does not
    # have.
    (
        "the_local_backing_path_is_persisted",
        RECORD,
        """    // `backing_image_config` is `#[serde(skip)]` and deliberately absent.
    return core::Json(object);""",
        """    if (backing_image_config.has_value()) {
        object["backingImageConfig"] = core::Json(*backing_image_config);
    }
    return core::Json(object);""",
    ),
    # `reservedBySandboxId` has no `skip_serializing_if`, so an unset lease is
    # an explicit null rather than an absent key.
    (
        "an_unset_reservation_key_is_omitted",
        RECORD,
        """    object["reservedBySandboxId"] = reserved_by_sandbox_id.has_value()
                                        ? core::Json(*reserved_by_sandbox_id)
                                        : core::Json();""",
        """    if (reserved_by_sandbox_id.has_value()) {
        object["reservedBySandboxId"] = core::Json(*reserved_by_sandbox_id);
    }""",
    ),

    # ---- the document parse ------------------------------------------------
    #
    # serde's derive makes every field required except the `Option` and the
    # `#[serde(default)]` one. Defaulting a missing field instead of failing
    # turns a corrupt entry into a plausible volume -- a missing `sizeMb`
    # becomes a zero-byte volume that has real data behind it.
    (
        "a_missing_size_defaults_to_zero",
        RECORD,
        """    if (size_field == fields.end()) {
        return core::make_unexpected(std::string("missing field `sizeMb`"));
    }""",
        """    if (size_field == fields.end()) {
        record.size_mb = 0;
    } else""",
    ),
    (
        "a_missing_mount_list_defaults_to_empty",
        RECORD,
        """    if (mounts == fields.end()) {
        return core::make_unexpected(std::string("missing field `readOnlyMounts`"));
    }""",
        """    if (mounts == fields.end()) return record;""",
    ),
    (
        "a_missing_layer_list_defaults_to_empty",
        RECORD,
        """    if (layers == fields.end()) {
        return core::make_unexpected(std::string("missing field `backingLayers`"));
    }""",
        """    if (layers == fields.end()) return record;""",
    ),
    # A present-but-wrong-typed field is coerced rather than refused.
    (
        "a_string_size_is_coerced",
        RECORD,
        """    if (size_field->second.kind() != core::Json::Kind::Int || size_field->second.as_int() < 0) {
        return core::make_unexpected(InvalidType("sizeMb", "u64"));
    }""",
        """    if (size_field->second.kind() != core::Json::Kind::Int) {
        record.size_mb = 0;
    } else""",
    ),
    # A negative size wraps into an enormous unsigned one, which is worse than
    # either failing or zeroing: the volume reports a plausible-looking size
    # that no allocator can satisfy.
    (
        "a_negative_size_wraps",
        RECORD,
        """    if (size_field->second.kind() != core::Json::Kind::Int || size_field->second.as_int() < 0) {""",
        """    if (size_field->second.kind() != core::Json::Kind::Int) {""",
    ),
    (
        "a_non_string_mount_entry_is_skipped",
        RECORD,
        """        if (values[i].kind() != core::Json::Kind::String) {
            return core::make_unexpected(InvalidType("readOnlyMounts", "a string"));
        }""",
        """        if (values[i].kind() != core::Json::Kind::String) continue;""",
    ),
    (
        "a_non_array_mount_list_reads_as_empty",
        RECORD,
        """    if (mounts->second.kind() != core::Json::Kind::Array) {
        return core::make_unexpected(InvalidType("readOnlyMounts", "a sequence"));
    }""",
        """    if (mounts->second.kind() != core::Json::Kind::Array) return record;""",
    ),
    (
        "a_non_boolean_deleting_flag_reads_as_false",
        RECORD,
        """        if (deleting_field->second.kind() != core::Json::Kind::Bool) {
            return core::make_unexpected(InvalidType("deleting", "a boolean"));
        }""",
        """        if (deleting_field->second.kind() != core::Json::Kind::Bool) return record;""",
    ),
    # A non-string reservation is coerced, so a numeric sandbox id silently
    # becomes an unleased volume that something is still holding.
    (
        "a_non_string_reservation_reads_as_unleased",
        RECORD,
        """        if (reserved->second.kind() != core::Json::Kind::String) {
            return core::make_unexpected(InvalidType("reservedBySandboxId", "a string"));
        }""",
        """        if (reserved->second.kind() != core::Json::Kind::String) return record;""",
    ),
    # The document's own `backingImageConfig` is read into the field.
    # `#[serde(skip)]` is both directions upstream, so this imports another
    # node's cache path and leaves this node pointing at a file it lacks.
    (
        "the_skipped_field_is_read_from_the_document",
        RECORD,
        """    // `#[serde(default)]`: absent is `false`.""",
        """    const core::JsonObject::const_iterator cached = fields.find("backingImageConfig");
    if (cached != fields.end() && cached->second.kind() == core::Json::Kind::String) {
        record.backing_image_config = core::Optional<std::string>(cached->second.as_string());
    }

    // `#[serde(default)]`: absent is `false`.""",
    ),
    # A required string field stops being required.
    (
        "a_missing_required_string_defaults_to_empty",
        RECORD,
        """    if (found == fields.end()) {
        return core::make_unexpected(std::string("missing field `") + field + "`");
    }""",
        """    if (found == fields.end()) return std::string();""",
    ),
    (
        "a_non_string_required_field_is_coerced",
        RECORD,
        """    if (found->second.kind() != core::Json::Kind::String) {
        return core::make_unexpected(InvalidType(field, "a string"));
    }""",
        """    if (found->second.kind() != core::Json::Kind::String) return std::string();""",
    ),
    # A non-object document is read as an empty struct.
    (
        "a_non_object_document_is_not_rejected",
        RECORD,
        """    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("invalid type: expected struct VolumeRecord"));
    }""",
        """    if (json.kind() != core::Json::Kind::Object) return VolumeRecord();""",
    ),
]


def build():
    return subprocess.run(
        ["cmake", "--build", "build", "-j", "8", "--target", TARGET],
        cwd=ROOT, capture_output=True, text=True,
    ).returncode == 0


def run():
    return subprocess.run([str(ROOT / "build/bin" / TARGET)],
                          cwd=ROOT, capture_output=True, text=True).returncode


def main():
    # Refuses if a previous run was killed mid-mutation: taking a baseline
    # from a tree that still holds one would copy the mutation into the
    # backup and lose the original. See tests/mutation_residue.py.
    mutation_residue.refuse_if_residue()
    # A full pass rebuilds once per mutation and routinely outlives a
    # command timeout; without this, SIGTERM skips the restore below.
    mutation_residue.restore_on_signal()

    sources = {RECORD}
    originals = {p: p.read_text() for p in sources}
    backups = {p: p.with_suffix(p.suffix + ".mutation-backup") for p in sources}
    for p, b in backups.items():
        shutil.copy(p, b)

    if not build():
        print("BASELINE BUILD FAILED")
        return 2
    if run() != 0:
        print("BASELINE TESTS FAILED")
        return 2
    print("baseline: build ok, tests pass\n")

    caught, survived, broken, noop = [], [], [], []
    try:
        for name, path, old, new in MUTATIONS:
            for p in sources:
                p.write_text(originals[p])
            text = path.read_text()
            assert text.count(old) == 1, \
                f"anchor for {name} matched {text.count(old)} times in {path.name}"
            if old == new:
                noop.append(name)
                print(f"  NO-OP    {name} (mutation is identity)")
                continue
            path.write_text(text.replace(old, new, 1))

            if not build():
                broken.append(name)
                print(f"  BROKEN   {name} (did not compile)")
                continue
            if run() != 0:
                caught.append(name)
                print(f"  caught   {name}")
            else:
                survived.append(name)
                print(f"  SURVIVED {name}")
    finally:
        for p, b in backups.items():
            shutil.copy(b, p)
            b.unlink()
        build()

    print(f"\n{len(caught)}/{len(MUTATIONS)} caught, "
          f"{len(survived)} survived, {len(broken)} did not compile, "
          f"{len(noop)} were no-ops")
    for label, group in (("survivors", survived),
                         ("did not compile", broken),
                         ("no-ops", noop)):
        if group:
            print(f"{label}:")
            for name in group:
                print(f"  - {name}")
    return 1 if (survived or broken or noop) else 0


if __name__ == "__main__":
    raise SystemExit(main())
