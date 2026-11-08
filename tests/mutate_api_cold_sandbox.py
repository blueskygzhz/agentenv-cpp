#!/usr/bin/env python3
"""Mutation harness for `POST /sandboxes-cold`.

Each mutation breaks one part of the OCI-reference -> Firecracker path in a
way that still answers 201. That shape of failure is exactly how the route
came to be missing in the first place: every piece it needs was ported and
tested in isolation, and nothing checked that anything called them.

Status is decided **by exit code only**: the suite calls `exit(1)` on the
first failing assertion, so its summary line is not printed on failure and
scraping stdout would misreport a caught mutation as a survivor.
"""

import pathlib
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
IMPLS = ROOT / "src/api/impls.cc"
DTO = ROOT / "src/api/dto.cc"
TARGET = "test_api_cold_sandbox"

MUTATIONS = [
    # The image's OCI context is dropped, so the guest boots in `/` as root
    # without the image's own command.
    (
        "oci_context_not_inherited",
        IMPLS,
        "    request.source.context =\n"
        "        snapshot::CommandContext::FromEnvAndWorkdir(base.env_vars, base.workdir);",
        "    request.source.context = snapshot::CommandContext();",
    ),
    # The workdir is copied raw instead of normalised, so an image carrying an
    # empty `WorkingDir` becomes a `cd ''`.
    #
    # Assigned field by field rather than through `CommandContext::New`, which
    # was the first attempt: that constructor normalises the workdir too, so
    # the "mutation" was behaviourally identical and survived for a reason
    # that said nothing about the tests.
    (
        "blank_workdir_not_normalized",
        IMPLS,
        "    request.source.context =\n"
        "        snapshot::CommandContext::FromEnvAndWorkdir(base.env_vars, base.workdir);",
        "    request.source.context.env_vars = base.env_vars;\n"
        "    request.source.context.workdir =\n"
        "        base.workdir.has_value() ? *base.workdir : std::string(\"/\");",
    ),
    # `user`/`cmd`/`labels` stop travelling.
    (
        "oci_user_and_cmd_not_inherited",
        IMPLS,
        "    request.source.context.WithUser(base.user)",
        "    request.source.context.WithUser(core::Optional<std::string>())",
    ),
    # Validated resources are dropped, so every cold sandbox silently gets the
    # node's defaults regardless of what the caller asked for.
    (
        "validated_resources_not_applied",
        IMPLS,
        "    request.source.image_resources =\n"
        "        core::Optional<sandbox::SandboxResources>(resources.value());",
        "    request.source.image_resources = core::Optional<sandbox::SandboxResources>();",
    ),
    # Resource validation stops rejecting anything, so a non-gibibyte disk gets
    # silently rounded by the block layer.
    (
        "resource_validation_result_ignored",
        IMPLS,
        "    if (!resources.ok()) return core::make_unexpected(resources.error());",
        "    if (false) return core::make_unexpected(resources.error());",
    ),
    # The rootfs OCI config stops being recorded, so a snapshot taken from the
    # sandbox publishes no `Env`/`Entrypoint` of its own.
    (
        "rootfs_image_config_not_recorded",
        IMPLS,
        "    if (resolved.value().raw_config.has_value()) {",
        "    if (false) {",
    ),
    # The rootfs entry is recorded under a drive id, so `RootfsConfig` no
    # longer matches it -- the failure mode that hands a volume's config to a
    # rootfs export.
    (
        "rootfs_image_config_recorded_as_a_drive",
        IMPLS,
        "        request.source.image_configs.Add(core::Optional<std::string>(), \"/\",",
        "        request.source.image_configs.Add(\n"
        "            core::Optional<std::string>(std::string(\"vdb\")), \"/\",",
    ),
    # A 500-class registry fault is reported as the caller's fault, so a client
    # stops retrying something that would have succeeded.
    (
        "image_error_class_ignored",
        IMPLS,
        "        const int code = resolved.error().IsUserError() ? 400 : 500;",
        "        const int code = 400;",
    ),
    # A node with no image resolver stops saying so.
    (
        "absent_image_resolver_not_reported",
        IMPLS,
        "    if (!image_resolver_) {\n"
        "        return core::make_unexpected(ApiError::Make(\n"
        "            501, \"creating a sandbox from an image requires an image resolver\"));\n"
        "    }",
        "    if (!image_resolver_) {\n"
        "        return core::make_unexpected(ApiError::Make(500, \"no image resolver\"));\n"
        "    }",
    ),
    # `autoPause` absent starts deleting sandboxes on timeout instead of
    # pausing them.
    #
    # Anchored on the following line, because `CreateSandbox` now carries the
    # identical expression: an ambiguous anchor would mutate whichever handler
    # came first and this suite would not be the one to notice.
    (
        "auto_pause_default_flips_to_delete",
        IMPLS,
        "    request.timeout_action = req.auto_pause.has_value() && !*req.auto_pause\n"
        "                                 ? orchestrator::SandboxTimeoutAction::Delete\n"
        "                                 : orchestrator::SandboxTimeoutAction::Pause;\n"
        "    // Rust `body.auto_resume.as_ref().is_some_and(|cfg| cfg.enabled)`.\n"
        "    request.auto_resume = req.auto_resume.has_value() && *req.auto_resume;",
        "    request.timeout_action = !req.auto_pause.has_value() || !*req.auto_pause\n"
        "                                 ? orchestrator::SandboxTimeoutAction::Delete\n"
        "                                 : orchestrator::SandboxTimeoutAction::Pause;\n"
        "    // Rust `body.auto_resume.as_ref().is_some_and(|cfg| cfg.enabled)`.\n"
        "    request.auto_resume = req.auto_resume.has_value() && *req.auto_resume;",
    ),
    # The cold path stops honouring `autoResume`, so a paused cold sandbox can
    # never be woken by an incoming request.
    (
        "cold_auto_resume_never_requested",
        IMPLS,
        "    request.auto_resume = req.auto_resume.has_value() && *req.auto_resume;\n"
        "    request.custom_extension_params = params.value();",
        "    request.custom_extension_params = params.value();",
    ),
    # Non-empty params with no configured extension stop being refused, so the
    # sandbox starts and the params are silently dropped.
    (
        "cold_custom_extension_params_validation_skipped",
        IMPLS,
        "    if (!params_ok.ok()) {\n"
        "        return core::make_unexpected(ApiError::Make(400, params_ok.error()));\n"
        "    }\n"
        "\n    orchestrator::CreateSandboxRequest request;",
        "\n    orchestrator::CreateSandboxRequest request;",
    ),
    # The cold timeout is read as milliseconds, as `NewSandbox`'s is: a factor
    # of 1000 in how long a sandbox lives.
    #
    # Anchored on the following line, since `CreateSandbox` now converts the
    # same way: both bodies carry `timeout` in seconds.
    (
        "cold_timeout_read_as_milliseconds",
        IMPLS,
        "        request.timeout_ms = core::Optional<int64_t>(*req.timeout_secs * 1000);\n"
        "    }\n"
        "    for (std::size_t i = 0; i < req.env_vars.size(); ++i) {\n"
        "        // `KEY=VALUE`, as in `CreateSandbox`.",
        "        request.timeout_ms = core::Optional<int64_t>(*req.timeout_secs);\n"
        "    }\n"
        "    for (std::size_t i = 0; i < req.env_vars.size(); ++i) {\n"
        "        // `KEY=VALUE`, as in `CreateSandbox`.",
    ),
    # The requested volumes never reach the launch, so the guest comes up
    # without the disks the caller asked for while the API still answers 201.
    #
    # The first version of this mutation moved them to `request.extra_drives`
    # instead. It survived -- and correctly so: the orchestrator's fresh branch
    # concatenates `source.extra_drives` with `request.extra_drives`, so both
    # fields reach the build spec. That survivor was a behaviourally equivalent
    # mutation, not a test gap, and it corrected a comment in `impls.cc` that
    # claimed the choice was load-bearing.
    (
        "volume_drives_dropped",
        IMPLS,
        "    request.source.extra_drives = prepared.value().drives;",
        "    request.source.extra_drives = std::vector<sandbox::ExtraDrive>();",
    ),
    # A body with no image resolves the empty reference.
    (
        "missing_image_field_accepted",
        DTO,
        "    auto image = GetString(j, \"image\");\n"
        "    if (!image.ok()) return core::make_unexpected(std::move(image.error()));\n"
        "    if (image.value().empty()) {\n"
        "        return core::make_unexpected(core::err(\"invalid field: image must not be empty\"));\n"
        "    }\n"
        "    r.image = image.value();",
        "    auto image = GetString(j, \"image\");\n"
        "    if (image.ok()) r.image = image.value();",
    ),
    # A present non-boolean reads as the field's default, which for `autoPause`
    # is the opposite of what `\"autoPause\": \"false\"` meant.
    (
        "non_boolean_flag_silently_defaulted",
        DTO,
        "    if (v.kind() != core::Json::Kind::Bool) {\n"
        "        return core::make_unexpected(\n"
        "            core::err(\"invalid field: \" + key + \" must be a boolean\"));\n"
        "    }",
        "    if (v.kind() != core::Json::Kind::Bool) return core::Optional<bool>();",
    ),
    # An omitted resource field becomes an explicit zero, which the validator
    # then rejects -- or worse, accepts as a real size.
    (
        "omitted_resource_becomes_zero",
        DTO,
        "    if (v.kind() == core::Json::Kind::Null) return core::Optional<uint32_t>();",
        "    if (v.kind() == core::Json::Kind::Null) {\n"
        "        return core::Optional<uint32_t>(static_cast<uint32_t>(0));\n"
        "    }",
    ),
    # The absent-versus-empty distinction collapses for `volumeMounts`, which
    # `CreateSandbox`'s replacement check depends on.
    (
        "absent_mount_list_becomes_empty",
        DTO,
        "    if (mounts.kind() == core::Json::Kind::Null) {\n"
        "        return core::Optional<std::vector<SandboxVolumeMountModel> >();\n"
        "    }",
        "    if (mounts.kind() == core::Json::Kind::Null) {\n"
        "        return core::Optional<std::vector<SandboxVolumeMountModel> >(\n"
        "            std::vector<SandboxVolumeMountModel>());\n"
        "    }",
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
    sources = {IMPLS, DTO}
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
