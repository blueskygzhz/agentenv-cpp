#!/usr/bin/env python3
"""Mutation harness for the two create request bodies.

These are a wire-compatibility contract: `dto.h` claims the E2B schema in
`openapi.yml`, so a key spelled differently here is a field a real client's
body cannot deliver. The port previously used `template_id`, `timeout_ms` and
`env_vars`, none of which the schema contains -- a conforming body parsed as a
request with no template at all.

Status is decided **by exit code only**: the suites call `exit(1)` on the first
failing assertion, so their summary lines are not printed on failure and
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
DTO = ROOT / "src/api/dto.cc"
TARGETS = ["test_dto", "test_api_cold_sandbox"]

MUTATIONS = [
    # The schema's key spellings. Each of these is a field a conforming client
    # sends and this port would then not see.
    (
        "template_id_key_reverts_to_snake_case",
        '    auto tid = GetString(j, "templateID");',
        '    auto tid = GetString(j, "template_id");',
    ),
    (
        "env_vars_key_reverts_to_snake_case",
        '    const auto& envs = j["envVars"];',
        '    const auto& envs = j["env_vars"];',
    ),
    (
        "warm_body_internet_key_uses_the_cold_spelling",
        '    auto internet = ParseOptionalBool(j, "allow_internet_access");',
        '    auto internet = ParseOptionalBool(j, "allowInternetAccess");',
    ),
    (
        "cold_body_internet_key_uses_the_warm_spelling",
        '    auto internet = ParseOptionalBool(j, "allowInternetAccess");',
        '    auto internet = ParseOptionalBool(j, "allow_internet_access");',
    ),
    # The schema's `default: 15` applied server-side, overriding whatever
    # default the operator configured with a value nobody chose.
    (
        "omitted_timeout_defaults_to_fifteen",
        "    if (timeout.kind() == core::Json::Kind::Null) return core::Optional<int64_t>();",
        "    if (timeout.kind() == core::Json::Kind::Null) {\n"
        "        return core::Optional<int64_t>(static_cast<int64_t>(15));\n"
        "    }",
    ),
    # A negative timeout accepted, where upstream's `u32` cannot hold one.
    (
        "negative_timeout_accepted",
        "    if (timeout.as_int() < 0) {\n"
        '        return core::make_unexpected(core::err("invalid field: timeout must not be negative"));\n'
        "    }",
        "    if (false) {\n"
        '        return core::make_unexpected(core::err("invalid field: timeout must not be negative"));\n'
        "    }",
    ),
    # `autoResume` nesting collapsed: a bare boolean would be read, and the
    # schema's object form -- the one a client actually sends -- refused.
    (
        "auto_resume_read_as_a_bare_boolean",
        '    const auto& resume = j["autoResume"];\n'
        "    if (resume.kind() == core::Json::Kind::Null) return core::Optional<bool>();\n"
        "    if (resume.kind() != core::Json::Kind::Object) {",
        '    const auto& resume = j["autoResume"];\n'
        "    if (resume.kind() == core::Json::Kind::Null) return core::Optional<bool>();\n"
        "    if (resume.kind() == core::Json::Kind::Bool) {\n"
        "        return core::Optional<bool>(resume.as_bool());\n"
        "    }\n"
        "    if (resume.kind() != core::Json::Kind::Object) {",
    ),
    # A present `autoResume` missing its required `enabled` reads as disabled,
    # so a caller who asked for auto-resume gets a sandbox that never wakes.
    (
        "auto_resume_without_enabled_reads_as_disabled",
        "    if (enabled.kind() != core::Json::Kind::Bool) {\n"
        "        return core::make_unexpected(\n"
        '            core::err("invalid field: autoResume.enabled must be a boolean"));\n'
        "    }",
        "    if (enabled.kind() != core::Json::Kind::Bool) {\n"
        "        return core::Optional<bool>(false);\n"
        "    }",
    ),
    # The network config's lists stop being read, so an egress policy the
    # caller spelled out is silently replaced by the node default.
    (
        "network_cidr_lists_ignored",
        '    const char* const kLists[2] = {"allowOut", "denyOut"};',
        '    const char* const kLists[2] = {"allowOutX", "denyOutX"};',
    ),
    (
        "network_public_traffic_flag_ignored",
        '    const auto& public_traffic = network["allowPublicTraffic"];',
        '    const auto& public_traffic = network["allowPublicTrafficX"];',
    ),
    # A present-but-empty allow list collapses to absent. Those differ:
    # `SandboxNetworkEgressPolicy` reads an empty allow list as "allow
    # nothing", while no list at all means "no allowlist constraint".
    (
        "empty_network_list_collapses_to_absent",
        "        if (i == 0) {\n"
        "            model.allow_out = core::Optional<std::vector<std::string> >(cidrs);\n"
        "        } else {\n"
        "            model.deny_out = core::Optional<std::vector<std::string> >(cidrs);\n"
        "        }",
        "        if (cidrs.empty()) continue;\n"
        "        if (i == 0) {\n"
        "            model.allow_out = core::Optional<std::vector<std::string> >(cidrs);\n"
        "        } else {\n"
        "            model.deny_out = core::Optional<std::vector<std::string> >(cidrs);\n"
        "        }",
    ),
    # Metadata values silently stringified instead of refused, so a client
    # sending a number gets a value it never wrote.
    (
        "non_string_metadata_values_accepted",
        "        if (entry.second.kind() != core::Json::Kind::String) {\n"
        "            return core::make_unexpected(\n"
        '                core::err("invalid field: metadata values must be strings"));\n'
        "        }\n"
        "        out[entry.first] = entry.second.as_string();",
        "        out[entry.first] = entry.second.ToString();",
    ),
    # `image` becomes optional, so a cold body with no image resolves "".
    (
        "cold_image_allowed_to_be_empty",
        '        return core::make_unexpected(core::err("invalid field: image must not be empty"));',
        "        return r;",
    ),
]


def build():
    return subprocess.run(
        ["cmake", "--build", "build", "-j", "8", "--target"] + TARGETS,
        cwd=ROOT, capture_output=True, text=True,
    ).returncode == 0


def run():
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

    original = DTO.read_text()
    backup = DTO.with_suffix(DTO.suffix + ".mutation-backup")
    shutil.copy(DTO, backup)

    if not build():
        print("BASELINE BUILD FAILED")
        return 2
    if run() != 0:
        print("BASELINE TESTS FAILED")
        return 2
    print("baseline: build ok, tests pass\n")

    caught, survived, broken, noop = [], [], [], []
    try:
        for name, old, new in MUTATIONS:
            DTO.write_text(original)
            text = DTO.read_text()
            assert text.count(old) == 1, \
                f"anchor for {name} matched {text.count(old)} times"
            if old == new:
                noop.append(name)
                print(f"  NO-OP    {name} (mutation is identity)")
                continue
            DTO.write_text(text.replace(old, new, 1))

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
        shutil.copy(backup, DTO)
        backup.unlink()
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
