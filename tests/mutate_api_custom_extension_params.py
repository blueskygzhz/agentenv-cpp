#!/usr/bin/env python3
"""Mutation harness for the custom-extension-params routes.

Two things were missing and they fail differently.

The *routes* were missing, which no amount of handler testing could show:
`PatchSandboxCustomExtensionParams` was complete in the orchestrator -- the
hook call, the state guard, the metadata write -- and no URL reached it. So the
first group mutates the registration itself, and is caught by
`test_api_routes.cc` reading the route table back rather than by any assertion
about behaviour.

The *status mapping* is the rest. This route deliberately does not use the
shared `ToApiError`, and the two places it disagrees are both cases where the
wrong answer still looks like a plausible error:

- a paused sandbox is 409, not 400. A 400 tells the client its request was
  malformed and not to retry; the request is fine and a resume fixes it.
- a hook failure splits on *who* failed. 4xx from the extension means the
  extension refused this patch (the caller must change it, 400); 5xx means the
  extension broke (the caller cannot fix it, 500). `ToApiError` calls the whole
  variant 500 and would have a client retry a rejected patch forever.

That split is only possible because `OrchestratorError` now carries the typed
`source` chain alongside the flattened `detail`. The mutation that drops it
back to text is in here, because with it gone a 403 and a 502 from an extension
become indistinguishable and the tests must notice.

Status is decided **by exit code only**: both suites `exit(1)` on the first
failing assertion, so their summary line is absent on failure and scraping
stdout would misreport a caught mutation as a survivor.
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
IMPLS = ROOT / "src/api/impls.cc"
SERVER = ROOT / "src/api/server.cc"
SERVICE = ROOT / "src/orchestrator/service.cc"

# Two suites: one reads the route table, the other the status mapping. A
# mutation is caught if *either* fails, which is what lets the registration
# mutations below be caught at all.
TARGETS = ["test_api_routes", "test_api_custom_extension_params"]

MUTATIONS = [
    # ---- whether the routes exist ------------------------------------------
    #
    # The original defect: a complete handler nothing could reach. Caught only
    # by the route-table test, which is why that test exists.
    (
        "the_patch_route_is_never_registered",
        SERVER,
        """    server->Route("PATCH", "/sandboxes/{sandbox_id}/custom-extension-params",""",
        """    if (false) server->Route("PATCH", "/sandboxes/{sandbox_id}/custom-extension-params",""",
    ),
    (
        "the_get_route_is_never_registered",
        SERVER,
        """    server->Route("GET", "/sandboxes/{sandbox_id}/custom-extension-params",""",
        """    if (false) server->Route("GET", "/sandboxes/{sandbox_id}/custom-extension-params",""",
    ),
    # Registered as PUT, which reads as "replace the params". The hook performs
    # a merge, so the verb would promise semantics nothing implements.
    (
        "the_patch_route_is_registered_as_a_put",
        SERVER,
        """    server->Route("PATCH", "/sandboxes/{sandbox_id}/custom-extension-params",""",
        """    server->Route("PUT", "/sandboxes/{sandbox_id}/custom-extension-params",""",
    ),
    # ---- the GET -----------------------------------------------------------
    #
    # Absent params as `null` instead of `{}`. The response is documented as a
    # map, so a generated client deserializing into one breaks on the ordinary
    # case of a sandbox that never set any.
    (
        "absent_params_are_reported_as_null",
        IMPLS,
        """    if (!params.has_value() || params->json_bytes.empty()) {
        return core::Json(core::JsonObject());
    }""",
        """    if (!params.has_value() || params->json_bytes.empty()) {
        return core::Json();
    }""",
    ),
    # Stored params dropped: every sandbox reports `{}`. A caller that just
    # patched successfully reads back nothing.
    (
        "stored_params_are_not_returned",
        IMPLS,
        """    return parsed.value();
}

/// The status mapping for `custom_extension_params_patch`,""",
        """    return core::Json(core::JsonObject());
}

/// The status mapping for `custom_extension_params_patch`,""",
    ),
    # A missing sandbox reported as empty params rather than 404 -- the caller
    # cannot tell a deleted sandbox from one with no extension config.
    (
        "a_missing_sandbox_reads_as_empty_params",
        IMPLS,
        """    if (!got.value().has_value()) {
        return core::make_unexpected(SandboxNotFound(sandbox_id));
    }
    return CustomExtensionParamsModel(got.value()->custom_extension_params);""",
        """    if (!got.value().has_value()) {
        return core::Json(core::JsonObject());
    }
    return CustomExtensionParamsModel(got.value()->custom_extension_params);""",
    ),
    # ---- the PATCH body ----------------------------------------------------
    #
    # A non-object body accepted. The body *is* the patch map, so an array
    # would reach the hook as a document it cannot merge.
    (
        "a_non_object_body_is_accepted",
        IMPLS,
        """    if (patch.kind() != core::Json::Kind::Object) {""",
        """    if (false) {""",
    ),
    # The patch is swallowed and the hook is handed an empty one, so a
    # successful-looking call changes nothing.
    (
        "the_patch_body_never_reaches_the_hook",
        IMPLS,
        """    params.json_bytes = patch.ToString();""",
        """    params.json_bytes = "{}";""",
    ),
    # The response echoes what was sent rather than what the hook decided.
    # Plausible and wrong: the extension owns the merge, so a client would
    # believe keys the extension dropped are in effect.
    (
        "the_patch_echoes_the_request_instead_of_the_result",
        IMPLS,
        """    return CustomExtensionParamsModel(patched.value());
}""",
        """    return patch;
}""",
    ),
    # ---- the status mapping ------------------------------------------------
    #
    # The whole point of not reusing `ToApiError`. Both of the route's
    # disagreements with it vanish at once.
    (
        "the_shared_error_mapping_is_used_instead",
        IMPLS,
        """        return core::make_unexpected(PatchParamsApiError(patched.error()));""",
        """        return core::make_unexpected(ToApiError(patched.error()));""",
    ),
    # A paused sandbox as 400: "your request is wrong", when a resume fixes it.
    (
        "a_wrong_state_is_a_client_error",
        IMPLS,
        """            return ApiError::Make(
                409, std::string("sandbox custom extension params cannot be patched from ") +""",
        """            return ApiError::Make(
                400, std::string("sandbox custom extension params cannot be patched from ") +""",
    ),
    # The state is omitted from the message, so an operator cannot tell whether
    # it was paused, forking or being deleted.
    (
        "the_blocking_state_is_not_named",
        IMPLS,
        """                         orchestrator::SandboxStateName(err.state) + " state");""",
        """                         "another" + std::string(" state"));""",
    ),
    # Every hook failure becomes a 500: a rejected patch is retried forever.
    (
        "a_rejected_patch_is_reported_as_a_server_fault",
        IMPLS,
        """            if (hook != NULL && hook->IsServerError()) {
                return ApiError::Make(500, hook->ToString());
            }
            return ApiError::Make(400, err.Message());""",
        """            return ApiError::Make(500, err.Message());""",
    ),
    # And the inverse: a broken extension becomes a 400, blaming the caller for
    # a fault they cannot fix and cannot retry away.
    (
        "a_broken_extension_is_blamed_on_the_caller",
        IMPLS,
        """            if (hook != NULL && hook->IsServerError()) {
                return ApiError::Make(500, hook->ToString());
            }""",
        """            if (false) {
                return ApiError::Make(500, hook->ToString());
            }""",
    ),
    # The server-error filter dropped, so any hook status -- including 403 --
    # takes the 500 arm. Upstream's `.filter(|e| e.is_server_error())`.
    (
        "any_hook_status_counts_as_a_server_error",
        IMPLS,
        """            if (hook != NULL && hook->IsServerError()) {""",
        """            if (hook != NULL) {""",
    ),
    # ---- the structured cause ----------------------------------------------
    #
    # The change that makes the split above possible at all. Flattened to
    # text, the chain still *reads* correctly in logs, so nothing but a test
    # that distinguishes a 403 from a 502 notices.
    (
        "the_hook_cause_is_flattened_to_text",
        SERVICE,
        """        return core::make_unexpected(OrchestratorError::SandboxOperationFailed(
            id, SandboxOperation::PatchCustomExtensionParams, hooked.error()));""",
        """        return core::make_unexpected(OrchestratorError::SandboxOperationFailed(
            id, SandboxOperation::PatchCustomExtensionParams,
            hooked.error().chain()));""",
    ),
    # A conflict as 500 rather than 409: a transient collision with another
    # operation on the same sandbox, reported as a bug in the server.
    (
        "an_operation_conflict_is_a_server_error",
        IMPLS,
        """        case orchestrator::OrchestratorErrorKind::SandboxOperationConflict:
            return ApiError::Make(409, err.Message());""",
        """        case orchestrator::OrchestratorErrorKind::SandboxOperationConflict:
            return ApiError::Make(500, err.Message());""",
    ),
    # A missing sandbox as 400 instead of 404. Anchored through the `switch`
    # line above it: `ToApiError` has a textually identical arm, and matching
    # both would mutate the shared mapping instead of this one.
    (
        "a_missing_sandbox_is_a_bad_request",
        IMPLS,
        """    switch (err.kind) {
        case orchestrator::OrchestratorErrorKind::SandboxNotFound:
            return SandboxNotFound(err.sandbox_id.ToString());
        case orchestrator::OrchestratorErrorKind::InvalidSandboxState:
            return ApiError::Make(
                409,""",
        """    switch (err.kind) {
        case orchestrator::OrchestratorErrorKind::SandboxNotFound:
            return ApiError::Make(400, "bad sandbox id");
        case orchestrator::OrchestratorErrorKind::InvalidSandboxState:
            return ApiError::Make(
                409,""",
    ),
]


def build():
    for target in TARGETS:
        if subprocess.run(
            ["cmake", "--build", "build", "-j", "8", "--target", target],
            cwd=ROOT, capture_output=True, text=True,
        ).returncode != 0:
            return False
    return True


def run():
    """Non-zero if any suite fails, so either one can catch a mutation."""
    for target in TARGETS:
        if subprocess.run([str(ROOT / "build/bin" / target)],
                          cwd=ROOT, capture_output=True, text=True).returncode != 0:
            return 1
    return 0


def main():
    # Refuses if a previous run was killed mid-mutation: taking a baseline
    # from a tree that still holds one would copy the mutation into the
    # backup and lose the original. See tests/mutation_residue.py.
    mutation_residue.refuse_if_residue()
    # A full pass rebuilds once per mutation and routinely outlives a
    # command timeout; without this, SIGTERM skips the restore below.
    mutation_residue.restore_on_signal()

    sources = {IMPLS, SERVER, SERVICE}
    originals = {p: p.read_text() for p in sources}
    backups = {p: p.with_suffix(p.suffix + ".mutation-backup") for p in sources}
    for p, b in backups.items():
        shutil.copy(p, b)

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
