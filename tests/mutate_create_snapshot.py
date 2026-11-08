#!/usr/bin/env python3
"""Mutation harness for the snapshot publication chain.

Covers `ApiImpl::CreateSnapshot` and its two helpers, the
`POST /sandboxes/{id}/snapshots` route, and the manifest contract
`FirecrackerSandboxBackend::Snapshot` now carries.

Status is decided **by exit code only**: the suite calls `exit(1)` on the
first failing assertion, so its summary line is not printed on failure and
scraping stdout for "failed" would misreport a caught mutation as a survivor.

Mutations are lists of edits rather than single substitutions, because two of
the contracts here are about *order* -- the repository is demanded before the
guest is paused, and the unpublishable-capture refusal is reported after the
resume it owes. Moving a block takes a delete plus an insert; expressing it as
one substitution is impossible, and leaving it out would have left the only
assertions that make those orderings observable unverified.
"""

import pathlib
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
IMPLS = ROOT / "src/api/impls.cc"
SERVER = ROOT / "src/api/server.cc"
BACKEND = ROOT / "src/sandbox/firecracker/sandbox_backend.cc"

API_TARGET = "test_api_create_snapshot"
BACKEND_TARGET = "test_firecracker_sandbox_backend"

REPO_BLOCK = """    const core::Expected<snapshot::repository::SnapshotRepository*, ApiError> repository =
        RequireRepository();
    if (!repository.ok()) return core::make_unexpected(repository.error());

"""

ALIAS_BLOCK = """    core::Optional<snapshot::SnapshotAlias> alias;
    if (req.name.has_value()) {
        const core::Expected<snapshot::SnapshotAlias, std::string> parsed =
            snapshot::SnapshotAlias::Parse(*req.name);
        if (!parsed.ok()) {
            // Rust `format!("invalid snapshot alias: {}", err)`.
            return core::make_unexpected(
                ApiError::Make(400, "invalid snapshot alias: " + parsed.error()));
        }
        alias = core::Optional<snapshot::SnapshotAlias>(parsed.value());
    }
"""

MANIFEST_CHECK = """    if (!captured.value().manifest) {
        CleanupFailedSnapshotDir(spec.snapshot_dir);
        return core::make_unexpected(SandboxCaptureError::Recoverable(
            "snapshot: this sandbox has no overlaybd ublk rootfs to capture, so the capture "
            "cannot be described for publication"));
    }
    return CapturedSandboxSnapshot(*captured.value().manifest, spec.snapshot_dir);
"""

RESUME_BLOCK = """    const core::Expected<core::Unit, std::string> resumed = impl_->instance->Resume();
    if (!resumed.ok()) {
        return core::make_unexpected(SandboxCaptureError::Terminal(
            "snapshot captured but sandbox could not be resumed: " + resumed.error()));
    }
"""

REQUIRED_BODY = """        if (req.body.empty()) {
            return WriteError(ApiError::Make(400, "a request body is required"), resp);
        }
"""

ROUTE_201 = """        // 201: the snapshot is a new resource, and the body names it. A 204
        // would leave the caller with no id to launch from.
        WriteJson(r.value().ToJson(), resp);
        resp->status = 201;
"""

# (name, target, [(file, old, new), ...])
MUTATIONS = [
    # ---- what the published record actually says ----------------------------
    ("resources_not_published", API_TARGET, [
        (IMPLS, "    publish.resources           = metadata.resources;\n", ""),
    ]),
    ("alias_not_published", API_TARGET, [
        (IMPLS, "    publish.alias = alias;\n", ""),
    ]),
    ("volumes_not_published", API_TARGET, [
        (IMPLS, "    publish.volume_snapshots    = volume_snapshots.value();\n", ""),
    ]),
    ("source_published_as_template", API_TARGET, [
        (IMPLS,
         "    publish.source = snapshot::SnapshotPublishSource::Sandbox(metadata.id.ToString(),\n"
         "                                                              user_metadata);\n",
         "    publish.source = snapshot::SnapshotPublishSource::Template();\n"),
    ]),
    ("volume_layers_dropped", API_TARGET, [
        (IMPLS, "        entry.layers     = state.value().backing_layers;\n", ""),
    ]),

    # ---- the status a client sees -------------------------------------------
    ("volume_error_status_not_collapsed", API_TARGET, [
        (IMPLS,
         "        return core::make_unexpected(error.code == 500 ? error\n"
         "                                                       : "
         "ApiError::Make(400, error.message));\n",
         "        return core::make_unexpected(error);\n"),
    ]),
    ("alias_conflict_is_a_500", API_TARGET, [
        (IMPLS,
         "        case snapshot::repository::RepositoryErrorKind::AliasConflict:\n", ""),
    ]),
    ("capture_errors_all_become_500", API_TARGET, [
        (IMPLS, "        return core::make_unexpected(ToApiError(capture.error()));\n",
         "        return core::make_unexpected(InternalError(capture.error().Message()));\n"),
    ]),

    # ---- validation, and that it happens before the guest is paused --------
    ("invalid_alias_not_checked", API_TARGET, [
        (IMPLS, ALIAS_BLOCK, "    core::Optional<snapshot::SnapshotAlias> alias;\n"),
    ]),
    ("volume_manager_not_demanded", API_TARGET, [
        (IMPLS,
         "    const core::Expected<volume::VolumeManager*, ApiError> manager = "
         "RequireVolumes();\n    if (!manager.ok()) return core::make_unexpected(manager.error());\n",
         "    const core::Expected<volume::VolumeManager*, ApiError> manager = "
         "RequireVolumes();\n    if (!manager.ok()) return snapshots;\n"),
    ]),
    ("repository_demanded_after_the_capture", API_TARGET, [
        (IMPLS, REPO_BLOCK, ""),
        (IMPLS,
         "    const orchestrator::SandboxMetadata& metadata = capture.value().metadata;\n",
         "    const orchestrator::SandboxMetadata& metadata = capture.value().metadata;\n"
         + REPO_BLOCK),
    ]),

    # ---- the route -----------------------------------------------------------
    ("route_back_to_the_singular_path", API_TARGET, [
        (SERVER, '    server->Route("POST", "/sandboxes/{sandbox_id}/snapshots",\n',
         '    server->Route("POST", "/sandboxes/{sandbox_id}/snapshot",\n'),
    ]),
    ("route_answers_204_with_no_body", API_TARGET, [
        (SERVER, ROUTE_201, "        resp->status = 204;\n"),
    ]),
    ("missing_body_treated_as_empty_object", API_TARGET, [
        (SERVER, REQUIRED_BODY, ""),
    ]),
    ("repository_not_passed_to_the_api_bundle", API_TARGET, [
        (SERVER,
         "    std::shared_ptr<ApiImpl> impl(new ApiImpl(alias, volumes, image_resolver, "
         "repository));\n",
         "    std::shared_ptr<ApiImpl> impl(new ApiImpl(alias, volumes, image_resolver));\n"),
    ]),

    # ---- the manifest `Snapshot` now promises -------------------------------
    ("snapshot_returns_an_empty_manifest", BACKEND_TARGET, [
        (BACKEND, MANIFEST_CHECK,
         "    return CapturedSandboxSnapshot(\n"
         "        captured.value().manifest ? *captured.value().manifest\n"
         "                                  : SandboxSnapshotManifest(),\n"
         "        spec.snapshot_dir);\n"),
    ]),
    ("manifest_refusal_reported_before_the_resume", BACKEND_TARGET, [
        (BACKEND, RESUME_BLOCK, ""),
        (BACKEND, MANIFEST_CHECK, MANIFEST_CHECK.replace(
            "    return CapturedSandboxSnapshot(",
            RESUME_BLOCK + "    return CapturedSandboxSnapshot(")),
    ]),
    # No mutation for "the volume snapshots are read *after* the capture".
    # Swapping the two would need the metadata the capture returns to exist
    # before the capture runs, so there is nothing to read the mounts off --
    # the mutation cannot be written, let alone survive. The ordering is
    # asserted indirectly: the volumes recorded are the ones the capture's
    # metadata names, which is what `snapshot_records_the_mounted_volumes_as_
    # of_the_capture` checks.
]

SOURCES = (IMPLS, SERVER, BACKEND)
TARGETS = (API_TARGET, BACKEND_TARGET)


def build(target):
    r = subprocess.run(
        ["cmake", "--build", "build", "-j", "8", "--target", target],
        cwd=ROOT, capture_output=True, text=True,
    )
    return r.returncode == 0


def run(target):
    return subprocess.run([str(ROOT / "build/bin" / target)],
                          cwd=ROOT, capture_output=True, text=True).returncode


def main():
    originals = {path: path.read_text() for path in SOURCES}
    backups = {path: path.with_suffix(path.suffix + ".mutation-backup")
               for path in SOURCES}
    for path, backup in backups.items():
        shutil.copy(path, backup)

    for target in TARGETS:
        if not build(target):
            print(f"BASELINE BUILD FAILED ({target})")
            return 2
        if run(target) != 0:
            print(f"BASELINE TESTS FAILED ({target})")
            return 2
    print("baseline: build ok, tests pass\n")

    caught, survived, broken = [], [], []
    try:
        for name, target, edits in MUTATIONS:
            for path in SOURCES:
                path.write_text(originals[path])
            for path, old, new in edits:
                text = path.read_text()
                assert text.count(old) == 1, \
                    f"anchor for {name} matched {text.count(old)} times in {path.name}"
                path.write_text(text.replace(old, new, 1))

            if not build(target):
                broken.append(name)
                print(f"  BROKEN   {name} (did not compile)")
                continue

            if run(target) != 0:
                caught.append(name)
                print(f"  caught   {name}")
            else:
                survived.append(name)
                print(f"  SURVIVED {name}")
    finally:
        for path, backup in backups.items():
            shutil.copy(backup, path)
            backup.unlink()
        for target in TARGETS:
            build(target)

    print(f"\n{len(caught)}/{len(MUTATIONS)} caught, "
          f"{len(survived)} survived, {len(broken)} did not compile")
    for label, group in (("survivors", survived), ("did not compile", broken)):
        if group:
            print(f"{label}:")
            for name in group:
                print(f"  - {name}")
    return 1 if (survived or broken) else 0


if __name__ == "__main__":
    raise SystemExit(main())
