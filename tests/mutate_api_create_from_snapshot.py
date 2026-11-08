#!/usr/bin/env python3
"""Mutation harness for the `templateID` -> runnable-snapshot launch chain.

The chain is: `ApiImpl::CreateSandbox` -> repository `Get` -> resolver
`Resolve` -> `SandboxLaunchSource::FromRunnableSnapshot` -> the orchestrator's
`Kind::Snapshot` branch. Each mutation below breaks one link in a way that
leaves the handler returning 201, which is exactly how the chain managed to be
broken end to end while every test passed.

Status is decided **by exit code only**: the suite calls `exit(1)` on the
first failing assertion, so its summary line is not printed on failure and
scraping stdout for "failed" would misreport a caught mutation as a survivor.
"""

import pathlib
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
IMPLS = ROOT / "src/api/impls.cc"
TYPES = ROOT / "src/orchestrator/types.cc"
MANIFEST = ROOT / "src/sandbox/manifest.cc"
TARGET = "test_api_create_from_snapshot"

# The record lookup, whose result feeds the resolve.
MISSING_RECORD = """    if (!record.value().has_value()) {
        // Upstream's `Ok(None)` arm, verbatim down to the wording.
        return core::make_unexpected(
            ApiError::Make(400, "template " + id_or_alias + " not found"));
    }
"""

NO_RESOLVER_GUARD = """    if (!runtime_resolver_) {"""

MUTATIONS = [
    # The launch source stops naming the snapshot kind, so the orchestrator
    # takes the fresh branch with an empty `image_config_path` -- the exact
    # pre-change behaviour.
    (
        "launch_source_keeps_the_image_kind",
        TYPES,
        "    s.kind = Kind::Snapshot;\n    s.snapshot_id = record.id.ToString();\n",
        "    s.snapshot_id = record.id.ToString();\n",
    ),
    # The hydrated manifest is dropped, so the backend is handed a resume with
    # no vm_state or image configs to resume from.
    (
        "manifest_paths_not_projected",
        TYPES,
        "    s.snapshot_manifest.vm_state_path            = manifest.vm_state.path;\n",
        "",
    ),
    # Resources come from the node's defaults instead of the record, so a
    # restored sandbox silently gets different CPU/memory than it was captured
    # with.
    (
        "resources_not_inherited_from_the_record",
        TYPES,
        "    s.snapshot_resources           = record.resources;\n",
        "",
    ),
    # Runtime versions are dropped, so a resume would run this node's envd
    # against a guest captured with another.
    (
        "runtime_versions_not_inherited",
        TYPES,
        "    s.snapshot_runtime_versions    = committed.runtime_versions;\n",
        "",
    ),
    # A missing record becomes a successful create: upstream's `Ok(None)` arm
    # removed, so the resolver is handed an absent optional.
    ("missing_record_not_reported", IMPLS, MISSING_RECORD, ""),
    # A node with no resolver stops saying so and falls through.
    (
        "absent_resolver_not_reported",
        IMPLS,
        NO_RESOLVER_GUARD,
        "    if (false) {",
    ),
    # The free-slot check stops firing, so a request for more drives than the
    # capture reserved boots a guest missing them.
    (
        "free_slot_check_disabled",
        IMPLS,
        "    if (requested_drive_count > free_slots) {",
        "    if (false) {",
    ),
    # The five request fields the handler never used to set. Each was plumbed
    # all the way downstream, so breaking one again is invisible except here.
    (
        "network_policy_not_built_from_the_request",
        IMPLS,
        "    request.network_policy = policy.value();\n"
        "\n    // Rust `params_model_to_map`",
        "\n    // Rust `params_model_to_map`",
    ),
    (
        "network_config_half_of_the_policy_ignored",
        IMPLS,
        "        NetworkPolicyFromCreate(req.allow_internet_access,\n"
        "                                req.network.has_value() ? &*req.network : NULL);\n"
        "    if (!policy.ok()) {\n"
        "        return core::make_unexpected(ApiError::Make(400, policy.error()));\n"
        "    }\n"
        "    request.network_policy = policy.value();",
        "        NetworkPolicyFromCreate(req.allow_internet_access, NULL);\n"
        "    if (!policy.ok()) {\n"
        "        return core::make_unexpected(ApiError::Make(400, policy.error()));\n"
        "    }\n"
        "    request.network_policy = policy.value();",
    ),
    (
        "invalid_network_reported_as_a_server_fault",
        IMPLS,
        "    if (!policy.ok()) {\n"
        "        return core::make_unexpected(ApiError::Make(400, policy.error()));\n"
        "    }\n"
        "    request.network_policy = policy.value();",
        "    if (!policy.ok()) {\n"
        "        return core::make_unexpected(InternalError(policy.error()));\n"
        "    }\n"
        "    request.network_policy = policy.value();",
    ),
    (
        "auto_pause_default_flips_to_delete",
        IMPLS,
        "    request.timeout_action = req.auto_pause.has_value() && !*req.auto_pause\n"
        "                                 ? orchestrator::SandboxTimeoutAction::Delete\n"
        "                                 : orchestrator::SandboxTimeoutAction::Pause;\n"
        "    // Rust `body.auto_resume.as_ref().is_some_and(|cfg| cfg.enabled)`. The",
        "    request.timeout_action = orchestrator::SandboxTimeoutAction::Pause;\n"
        "    // Rust `body.auto_resume.as_ref().is_some_and(|cfg| cfg.enabled)`. The",
    ),
    (
        "auto_resume_never_requested",
        IMPLS,
        "    request.auto_resume = req.auto_resume.has_value() && *req.auto_resume;\n"
        "    // Rust `user_metadata: body.metadata.clone()`.",
        "    // Rust `user_metadata: body.metadata.clone()`.",
    ),
    # Anchored on the preceding comment: the identical loop appears in
    # `CreateColdSandbox` too, and an ambiguous anchor would mutate whichever
    # handler came first.
    (
        "request_metadata_dropped",
        IMPLS,
        "    // Rust `user_metadata: body.metadata.clone()`.\n"
        "    for (std::map<std::string, std::string>::const_iterator it = req.metadata.begin();\n"
        "         it != req.metadata.end(); ++it) {\n"
        "        request.user_metadata[it->first] = it->second;\n"
        "    }",
        "",
    ),
    (
        "custom_extension_params_validation_skipped",
        IMPLS,
        "    if (!params_ok.ok()) {\n"
        "        return core::make_unexpected(ApiError::Make(400, params_ok.error()));\n"
        "    }\n"
        "    // Rust's snapshot path passes",
        "    // Rust's snapshot path passes",
    ),
    (
        "timeout_read_as_milliseconds",
        IMPLS,
        "        request.timeout_ms = core::Optional<int64_t>(*req.timeout_secs * 1000);\n"
        "    }\n"
        "    for (std::size_t i = 0; i < req.env_vars.size(); ++i) {\n"
        "        // `KEY=VALUE`, as the DTO carries them.",
        "        request.timeout_ms = core::Optional<int64_t>(*req.timeout_secs);\n"
        "    }\n"
        "    for (std::size_t i = 0; i < req.env_vars.size(); ++i) {\n"
        "        // `KEY=VALUE`, as the DTO carries them.",
    ),
    # `check_snapshot_volume_replacement` stops firing.
    (
        "volume_replacement_check_disabled",
        IMPLS,
        "    if (replaces_mounts && !committed.volume_snapshots.empty()) {",
        "    if (false) {",
    ),
    # The absent-vs-empty distinction collapses, so a present-but-empty
    # `volumeMounts` reads as "restore the captured volumes" instead of as the
    # explicit "no volumes" it is.
    (
        "present_empty_mounts_read_as_absent",
        IMPLS,
        "    const bool replaces_mounts = req.volume_mounts.has_value();",
        "    const bool replaces_mounts = req.volume_mounts.has_value() &&\n"
        "                                 !req.volume_mounts->empty();",
    ),
    # `free_drive_slots` wraps instead of saturating, reporting SIZE_MAX free
    # slots for a manifest whose counts disagree.
    (
        "free_drive_slots_wraps_on_underflow",
        MANIFEST,
        "    return volume_drive_slots > slot_drives ? volume_drive_slots - slot_drives : 0;",
        "    return volume_drive_slots - slot_drives;",
    ),
    # A pre-slots capture reports the stored physical count, which was never
    # written for those records.
    (
        "effective_physical_count_ignores_the_legacy_shape",
        MANIFEST,
        "    if (volume_drive_slots == 0) return attached_drives.size();\n",
        "",
    ),
    # The resolver is handed a record other than the one that was looked up.
    # Caught only because the fake echoes back the id it was given and the
    # tests assert on it; without that assertion a handler could resolve the
    # wrong snapshot and still answer 201.
    (
        "resolver_called_with_the_wrong_record",
        IMPLS,
        "        record = repository.value()->Get(id_or_alias);",
        "        record = repository.value()->Get(id_or_alias + std::string(\"x\"));",
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
    sources = {IMPLS, TYPES, MANIFEST}
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
