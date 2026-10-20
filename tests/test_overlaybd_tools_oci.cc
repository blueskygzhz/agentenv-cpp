// SPDX-License-Identifier: MIT
// Tests for the overlaybd CLI tool wrappers.
//
// The real binaries are not available here, so the three tools are stood in
// for by shell scripts. That is not a weakening of the test: everything this
// module is responsible for -- argv composition, the working directory, the
// `image.json` contract, the dirty-work-dir refusal, which artifacts survive
// a success and which survive a failure, and the zstd pipe -- is observable
// from the stub's point of view. What is *not* covered is whether the real
// tools accept that argv, which no test here could establish either way.
//
// Rust: storage/overlaybd/src/tools/oci.rs
#include "microtest.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "agentenv/core/fs.h"
#include "agentenv/core/json.h"
#include "agentenv/core/process.h"
#include "agentenv/storage/overlaybd/tools_oci.h"

using namespace agentenv::storage::overlaybd::tools;
namespace core = agentenv::core;

namespace {

std::string MakeTempDir() {
    char tmpl[] = "/tmp/agentenv_tools_XXXXXX";
    char* made = ::mkdtemp(tmpl);
    if (made == 0) {
        std::fprintf(stderr, "  FAIL: mkdtemp\n");
        std::exit(1);
    }
    return std::string(made);
}

void RemoveTreeBestEffort(const std::string& dir) {
    const std::string cmd = "rm -rf '" + dir + "'";
    if (::system(cmd.c_str()) != 0) { /* ignored on purpose */ }
}

/// Write an executable stub at `path`.
bool WriteStub(const std::string& path, const std::string& body) {
    const std::string script = "#!/bin/sh\n" + body;
    if (!core::fs::Write(path, script).ok()) return false;
    return ::chmod(path.c_str(), 0755) == 0;
}

/// A bin dir whose three tools record how they were called.
///
/// Each stub appends its argv and its own working directory to a log, so a
/// case can assert both without the tools existing. The create and commit
/// stubs also produce the files the next step expects, since the conversion
/// checks for them.
struct StubTools {
    std::string root;
    std::string bin;
    std::string log;

    bool Install(const std::string& apply_body) {
        bin = core::fs::Join(root, "bin");
        log = core::fs::Join(root, "calls.log");
        if (!core::fs::CreateDirAll(bin).ok()) return false;

        // `pwd` is what proves the working directory was set: the tools are
        // given absolute paths, so nothing else would notice.
        const std::string header = "echo \"$(basename $0) cwd=$(pwd) args=$*\" >> '" + log +
                                   "'\n";
        // create: produce the upper pair it was asked for.
        if (!WriteStub(core::fs::Join(bin, "overlaybd-create"),
                       header + ": > \"$1\"\n: > \"$2\"\n")) {
            return false;
        }
        if (!WriteStub(core::fs::Join(bin, "overlaybd-apply"), header + apply_body)) {
            return false;
        }
        // commit: the last positional argument is the commit file.
        if (!WriteStub(core::fs::Join(bin, "overlaybd-commit"),
                       header + "eval \"last=\\${$#}\"\n: > \"$last\"\n")) {
            return false;
        }
        return true;
    }

    std::string ReadLog() const {
        const core::Expected<std::string, std::string> text = core::fs::ReadToString(log);
        return text.ok() ? text.value() : std::string();
    }
};

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

/// A minimal uncompressed tar-ish input. Content does not matter: the stubs
/// never read it, and the only code that looks at it reads four magic bytes.
bool WritePlainInput(const std::string& path) {
    return core::fs::Write(path, std::string(1024, 'x')).ok();
}

}  // namespace

// ============================================================
// zstd magic
// ============================================================

MT_TEST(zstd_magic_covers_frames_and_skippable_frames) {
    // Both forms have to count. A layer beginning with a skippable frame is
    // still zstd, and handing it to overlaybd-apply as a plain tar fails --
    // which is the bug the whole detour exists to avoid.
    const uint8_t frame[4] = {0x28, 0xb5, 0x2f, 0xfd};
    MT_EXPECT_TRUE(IsZstdMagic(frame));

    // 0x184D2A50..=0x184D2A5F, little-endian on disk.
    const uint8_t skip_low[4] = {0x50, 0x2a, 0x4d, 0x18};
    const uint8_t skip_high[4] = {0x5f, 0x2a, 0x4d, 0x18};
    MT_EXPECT_TRUE(IsZstdMagic(skip_low));
    MT_EXPECT_TRUE(IsZstdMagic(skip_high));

    // ...and the bounds are exclusive on both sides.
    const uint8_t below[4] = {0x4f, 0x2a, 0x4d, 0x18};
    const uint8_t above[4] = {0x60, 0x2a, 0x4d, 0x18};
    MT_EXPECT_TRUE(!IsZstdMagic(below));
    MT_EXPECT_TRUE(!IsZstdMagic(above));

    // gzip and plain tar are not zstd.
    const uint8_t gzip[4] = {0x1f, 0x8b, 0x08, 0x00};
    const uint8_t tar[4] = {'u', 's', 't', 'a'};
    MT_EXPECT_TRUE(!IsZstdMagic(gzip));
    MT_EXPECT_TRUE(!IsZstdMagic(tar));
}

// ============================================================
// image.json
// ============================================================

MT_TEST(image_json_uses_the_field_names_overlaybd_apply_expects) {
    // These names are a wire contract with overlaybd-apply. A typo would not
    // fail here -- it would make the tool report that it cannot find its
    // upper, somewhere far from this code.
    const std::string dir = MakeTempDir();
    const std::string path = core::fs::Join(dir, "image.json");

    std::vector<std::string> lowers;
    MT_EXPECT_TRUE(WriteImageJson(path, lowers, "/work/upper.data", "/work/upper.index",
                                  "/work/result.txt")
                       .ok());

    const core::Expected<std::string, std::string> text = core::fs::ReadToString(path);
    MT_EXPECT_TRUE(text.ok());
    if (!text.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }
    const core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(text.value());
    MT_EXPECT_TRUE(parsed.ok());
    if (!parsed.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }

    const core::Json& root = parsed.value();
    MT_EXPECT_TRUE(root.kind() == core::Json::Kind::Object);
    MT_EXPECT_TRUE(root["lowers"].kind() == core::Json::Kind::Array);
    MT_EXPECT_EQ(static_cast<long long>(root["lowers"].as_array().size()),
                 static_cast<long long>(0));
    MT_EXPECT_TRUE(root["upper"]["data"].as_string() == "/work/upper.data");
    MT_EXPECT_TRUE(root["upper"]["index"].as_string() == "/work/upper.index");
    // `resultFile`, camel-cased, not `result_file`.
    MT_EXPECT_TRUE(root["resultFile"].as_string() == "/work/result.txt");

    RemoveTreeBestEffort(dir);
}

MT_TEST(image_json_keeps_the_lower_layers_in_order) {
    // The order is the stacking order, so reversing it would silently build
    // an image whose layers mask each other the wrong way round.
    const std::string dir = MakeTempDir();
    const std::string path = core::fs::Join(dir, "image.json");

    std::vector<std::string> lowers;
    lowers.push_back("/a/layer0.commit");
    lowers.push_back("/b/layer1.commit");
    MT_EXPECT_TRUE(WriteImageJson(path, lowers, "/work/upper.data", "/work/upper.index",
                                  "/work/result.txt")
                       .ok());

    const core::Expected<std::string, std::string> text = core::fs::ReadToString(path);
    MT_EXPECT_TRUE(text.ok());
    if (!text.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }
    const core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(text.value());
    MT_EXPECT_TRUE(parsed.ok());
    if (parsed.ok()) {
        const core::JsonArray& arr = parsed.value()["lowers"].as_array();
        MT_EXPECT_EQ(static_cast<long long>(arr.size()), static_cast<long long>(2));
        if (arr.size() == 2) {
            MT_EXPECT_TRUE(arr[0]["file"].as_string() == "/a/layer0.commit");
            MT_EXPECT_TRUE(arr[1]["file"].as_string() == "/b/layer1.commit");
        }
    }
    RemoveTreeBestEffort(dir);
}

// ============================================================
// binary presence
// ============================================================

MT_TEST(a_missing_binary_is_reported_as_such) {
    // Distinct from a tool that ran and failed: this is a provisioning
    // problem, and the caller can act on it (reinstall) rather than retry.
    const std::string dir = MakeTempDir();
    const OverlaybdTools tools = OverlaybdTools::FromBinDir(core::fs::Join(dir, "bin"));

    CreateLayerRequest req;
    req.data_file = core::fs::Join(dir, "upper.data");
    req.index_file = core::fs::Join(dir, "upper.index");
    req.virtual_size_gib = 4;

    const core::Expected<StepSummary, ToolExecutionError> out =
        tools.CreateWritableLayer(req, dir);
    MT_EXPECT_TRUE(!out.ok());
    if (!out.ok()) {
        MT_EXPECT_TRUE(out.error().kind == ToolExecutionError::kMissingBinary);
        MT_EXPECT_TRUE(Contains(out.error().path, "overlaybd-create"));
    }
    RemoveTreeBestEffort(dir);
}

MT_TEST(a_directory_where_a_binary_belongs_is_also_missing) {
    // A directory carries the execute bit, so a check based on executability
    // alone would call this runnable and the failure would surface as an
    // exec error instead.
    const std::string dir = MakeTempDir();
    const std::string bin = core::fs::Join(dir, "bin");
    MT_EXPECT_TRUE(core::fs::CreateDirAll(core::fs::Join(bin, "overlaybd-create")).ok());

    const OverlaybdTools tools = OverlaybdTools::FromBinDir(bin);
    CreateLayerRequest req;
    req.data_file = core::fs::Join(dir, "upper.data");
    req.index_file = core::fs::Join(dir, "upper.index");
    req.virtual_size_gib = 1;

    const core::Expected<StepSummary, ToolExecutionError> out =
        tools.CreateWritableLayer(req, dir);
    MT_EXPECT_TRUE(!out.ok());
    if (!out.ok()) {
        MT_EXPECT_TRUE(out.error().kind == ToolExecutionError::kMissingBinary);
    }
    RemoveTreeBestEffort(dir);
}

MT_TEST(install_root_resolves_through_a_bin_subdirectory) {
    const OverlaybdTools tools = OverlaybdTools::FromOverlaybdInstallRoot("/opt/overlaybd");
    MT_EXPECT_TRUE(tools.CreateBin() == "/opt/overlaybd/bin/overlaybd-create");
    MT_EXPECT_TRUE(tools.ApplyBin() == "/opt/overlaybd/bin/overlaybd-apply");
    MT_EXPECT_TRUE(tools.CommitBin() == "/opt/overlaybd/bin/overlaybd-commit");
}

// ============================================================
// argv and the working directory
// ============================================================

MT_TEST(each_tool_runs_in_the_work_dir_with_the_expected_argv) {
    const std::string dir = MakeTempDir();
    StubTools stubs;
    stubs.root = dir;
    MT_EXPECT_TRUE(stubs.Install(": > \"$(dirname $2)/result.txt\"\n"));

    const std::string work = core::fs::Join(dir, "work");
    const std::string input = core::fs::Join(dir, "layer.tar");
    MT_EXPECT_TRUE(WritePlainInput(input));

    ConvertLayerRequest req;
    req.work_dir = work;
    req.input_layer_path = input;
    req.global_config_path = core::fs::Join(dir, "global.json");
    req.virtual_size_gib = 8;
    req.mkfs = true;
    req.lower_layers.push_back("/lower/one.commit");
    req.uuid = "11111111-2222-3333-4444-555555555555";
    req.parent_uuid = "66666666-7777-8888-9999-aaaaaaaaaaaa";

    const OverlaybdTools tools = OverlaybdTools::FromBinDir(stubs.bin);
    const core::Expected<ConvertLayerResult, ToolExecutionError> out =
        tools.ConvertLocalOciLayerToOverlaybd(req);
    MT_EXPECT_TRUE(out.ok());
    if (!out.ok()) {
        std::fprintf(stderr, "  (error: %s)\n", out.error().message.c_str());
        RemoveTreeBestEffort(dir);
        return;
    }
    MT_EXPECT_TRUE(out.value().layer_commit == core::fs::Join(work, "layer.commit"));

    const std::string log = stubs.ReadLog();

    // All three ran, in order, each with `work` as its working directory.
    // `pwd` is the only way to see this: every path passed to the tools is
    // absolute, so nothing else would change if the cwd were wrong.
    MT_EXPECT_TRUE(Contains(log, "overlaybd-create cwd=" + work));
    MT_EXPECT_TRUE(Contains(log, "overlaybd-apply cwd=" + work));
    MT_EXPECT_TRUE(Contains(log, "overlaybd-commit cwd=" + work));
    MT_EXPECT_TRUE(log.find("overlaybd-create") < log.find("overlaybd-apply"));
    MT_EXPECT_TRUE(log.find("overlaybd-apply") < log.find("overlaybd-commit"));

    // create: data, index, size in GiB.
    MT_EXPECT_TRUE(Contains(log, "args=" + core::fs::Join(work, "upper.data") + " " +
                                     core::fs::Join(work, "upper.index") + " 8"));
    // apply: the service config flag, --mkfs, the input, then image.json.
    MT_EXPECT_TRUE(Contains(log, "--service_config_path=" + req.global_config_path));
    MT_EXPECT_TRUE(Contains(log, "--mkfs"));
    // commit: both identity flags, then data/index/commit.
    MT_EXPECT_TRUE(Contains(log, "--uuid " + req.uuid));
    MT_EXPECT_TRUE(Contains(log, "--parent-uuid " + req.parent_uuid));

    RemoveTreeBestEffort(dir);
}

MT_TEST(the_identity_flags_are_omitted_when_no_uuid_is_given) {
    // Omitted, not passed empty: `--uuid ""` would have the tool stamp an
    // empty identity instead of generating one.
    const std::string dir = MakeTempDir();
    StubTools stubs;
    stubs.root = dir;
    MT_EXPECT_TRUE(stubs.Install(": > \"$(dirname $2)/result.txt\"\n"));

    const std::string work = core::fs::Join(dir, "work");
    const std::string input = core::fs::Join(dir, "layer.tar");
    MT_EXPECT_TRUE(WritePlainInput(input));

    ConvertLayerRequest req;
    req.work_dir = work;
    req.input_layer_path = input;
    req.global_config_path = core::fs::Join(dir, "global.json");
    req.virtual_size_gib = 2;

    const OverlaybdTools tools = OverlaybdTools::FromBinDir(stubs.bin);
    MT_EXPECT_TRUE(tools.ConvertLocalOciLayerToOverlaybd(req).ok());

    const std::string log = stubs.ReadLog();
    MT_EXPECT_TRUE(!Contains(log, "--uuid"));
    MT_EXPECT_TRUE(!Contains(log, "--parent-uuid"));
    // ...and `--mkfs` likewise, since the request did not ask for it.
    MT_EXPECT_TRUE(!Contains(log, "--mkfs"));

    RemoveTreeBestEffort(dir);
}

// ============================================================
// the work directory's contract
// ============================================================

MT_TEST(a_dirty_work_dir_is_refused) {
    // Reusing a directory that already holds an upper is how a conversion
    // silently inherits a previous run's data and emits a layer containing
    // both. Refused before anything runs.
    const std::string dir = MakeTempDir();
    StubTools stubs;
    stubs.root = dir;
    MT_EXPECT_TRUE(stubs.Install(": > \"$(dirname $2)/result.txt\"\n"));

    const std::string work = core::fs::Join(dir, "work");
    MT_EXPECT_TRUE(core::fs::CreateDirAll(work).ok());
    MT_EXPECT_TRUE(core::fs::Write(core::fs::Join(work, "upper.data"), "stale").ok());

    const std::string input = core::fs::Join(dir, "layer.tar");
    MT_EXPECT_TRUE(WritePlainInput(input));

    ConvertLayerRequest req;
    req.work_dir = work;
    req.input_layer_path = input;
    req.global_config_path = core::fs::Join(dir, "global.json");
    req.virtual_size_gib = 1;

    const OverlaybdTools tools = OverlaybdTools::FromBinDir(stubs.bin);
    const core::Expected<ConvertLayerResult, ToolExecutionError> out =
        tools.ConvertLocalOciLayerToOverlaybd(req);
    MT_EXPECT_TRUE(!out.ok());
    if (!out.ok()) {
        MT_EXPECT_TRUE(out.error().kind == ToolExecutionError::kInvalidRequest);
        MT_EXPECT_TRUE(Contains(out.error().message, "clean work_dir"));
    }
    // Nothing ran: the refusal happens before the first tool.
    MT_EXPECT_TRUE(stubs.ReadLog().empty());

    RemoveTreeBestEffort(dir);
}

MT_TEST(a_successful_conversion_leaves_only_the_commit) {
    const std::string dir = MakeTempDir();
    StubTools stubs;
    stubs.root = dir;
    MT_EXPECT_TRUE(stubs.Install(": > \"$(dirname $2)/result.txt\"\n"));

    const std::string work = core::fs::Join(dir, "work");
    const std::string input = core::fs::Join(dir, "layer.tar");
    MT_EXPECT_TRUE(WritePlainInput(input));

    ConvertLayerRequest req;
    req.work_dir = work;
    req.input_layer_path = input;
    req.global_config_path = core::fs::Join(dir, "global.json");
    req.virtual_size_gib = 1;

    const OverlaybdTools tools = OverlaybdTools::FromBinDir(stubs.bin);
    MT_EXPECT_TRUE(tools.ConvertLocalOciLayerToOverlaybd(req).ok());

    MT_EXPECT_TRUE(core::fs::Exists(core::fs::Join(work, "layer.commit")));
    MT_EXPECT_TRUE(!core::fs::Exists(core::fs::Join(work, "upper.data")));
    MT_EXPECT_TRUE(!core::fs::Exists(core::fs::Join(work, "upper.index")));
    MT_EXPECT_TRUE(!core::fs::Exists(core::fs::Join(work, "image.json")));
    MT_EXPECT_TRUE(!core::fs::Exists(core::fs::Join(work, "result.txt")));

    RemoveTreeBestEffort(dir);
}

MT_TEST(a_failed_conversion_keeps_its_intermediates) {
    // The other half of the cleanup rule, and the reason it is conditional: a
    // conversion that failed halfway is only diagnosable from what it left
    // behind. Cleaning up unconditionally would destroy the evidence.
    const std::string dir = MakeTempDir();
    StubTools stubs;
    stubs.root = dir;
    // apply fails.
    MT_EXPECT_TRUE(stubs.Install("echo 'apply boom' >&2\nexit 3\n"));

    const std::string work = core::fs::Join(dir, "work");
    const std::string input = core::fs::Join(dir, "layer.tar");
    MT_EXPECT_TRUE(WritePlainInput(input));

    ConvertLayerRequest req;
    req.work_dir = work;
    req.input_layer_path = input;
    req.global_config_path = core::fs::Join(dir, "global.json");
    req.virtual_size_gib = 1;

    const OverlaybdTools tools = OverlaybdTools::FromBinDir(stubs.bin);
    const core::Expected<ConvertLayerResult, ToolExecutionError> out =
        tools.ConvertLocalOciLayerToOverlaybd(req);
    MT_EXPECT_TRUE(!out.ok());
    if (!out.ok()) {
        MT_EXPECT_TRUE(out.error().kind == ToolExecutionError::kNonZeroExit);
        MT_EXPECT_TRUE(out.error().exit_code.has_value());
        if (out.error().exit_code.has_value()) {
            MT_EXPECT_EQ(static_cast<long long>(*out.error().exit_code),
                         static_cast<long long>(3));
        }
        // The tool's own complaint has to survive into the error, or a failed
        // conversion is just "exited 3".
        MT_EXPECT_TRUE(Contains(out.error().stderr_text, "apply boom"));
        MT_EXPECT_TRUE(Contains(out.error().command_line, "overlaybd-apply"));
    }

    // The upper and the config are still there.
    MT_EXPECT_TRUE(core::fs::Exists(core::fs::Join(work, "upper.data")));
    MT_EXPECT_TRUE(core::fs::Exists(core::fs::Join(work, "image.json")));
    MT_EXPECT_TRUE(!core::fs::Exists(core::fs::Join(work, "layer.commit")));

    RemoveTreeBestEffort(dir);
}

// ============================================================
// the zstd detour
// ============================================================

MT_TEST(a_zstd_layer_is_piped_through_the_decompressor) {
    // `zstd` is stubbed too, through PATH. That keeps the compression
    // algorithm out of the test while leaving everything this module owns
    // real: the fork, the pipe, `/dev/stdin` as the apply argument, closing
    // the write end so the tool sees EOF, and reaping the decompressor.
    //
    // The input carries a genuine zstd frame magic, because detection reads
    // the file -- a stub cannot fake that part.
    const std::string dir = MakeTempDir();
    StubTools stubs;
    stubs.root = dir;
    // apply records its stdin, which is what shows the pipe was wired rather
    // than the path being passed straight through.
    MT_EXPECT_TRUE(stubs.Install("cat > '" + core::fs::Join(dir, "piped.bin") +
                                 "'\n: > \"$(dirname $2)/result.txt\"\n"));

    // The stub decompressor: `zstd -dc <file>` writes the payload that the
    // frame below stands for.
    const std::string payload(4096, 'Z');
    const std::string fake_zstd = core::fs::Join(stubs.bin, "zstd");
    MT_EXPECT_TRUE(WriteStub(fake_zstd, "printf '%s' \"$(cat '" +
                                            core::fs::Join(dir, "payload.txt") + "')\"\n"));
    MT_EXPECT_TRUE(core::fs::Write(core::fs::Join(dir, "payload.txt"), payload).ok());

    const std::string work = core::fs::Join(dir, "work");
    // A real zstd frame magic followed by arbitrary bytes. Deliberately not
    // named `.zst`: detection must come from the magic, not the name.
    const std::string input = core::fs::Join(dir, "layer.tar");
    std::string framed;
    framed.push_back(static_cast<char>(0x28));
    framed.push_back(static_cast<char>(0xb5));
    framed.push_back(static_cast<char>(0x2f));
    framed.push_back(static_cast<char>(0xfd));
    framed.append(64, '\0');
    MT_EXPECT_TRUE(core::fs::Write(input, framed).ok());

    // The stub bin dir has to win over any real `zstd`.
    const char* old_path = ::getenv("PATH");
    const std::string saved_path = old_path != 0 ? old_path : "";
    MT_EXPECT_TRUE(::setenv("PATH", (stubs.bin + ":" + saved_path).c_str(), 1) == 0);

    ConvertLayerRequest req;
    req.work_dir = work;
    req.input_layer_path = input;
    req.global_config_path = core::fs::Join(dir, "global.json");
    req.virtual_size_gib = 1;

    const OverlaybdTools tools = OverlaybdTools::FromBinDir(stubs.bin);
    const core::Expected<ConvertLayerResult, ToolExecutionError> out =
        tools.ConvertLocalOciLayerToOverlaybd(req);

    // Restored before asserting, so a failure does not leave PATH mutated for
    // the cases that follow.
    if (saved_path.empty()) {
        ::unsetenv("PATH");
    } else {
        ::setenv("PATH", saved_path.c_str(), 1);
    }

    MT_EXPECT_TRUE(out.ok());
    if (!out.ok()) {
        std::fprintf(stderr, "  (error: %s)\n", out.error().message.c_str());
        RemoveTreeBestEffort(dir);
        return;
    }

    // `/dev/stdin` went to apply instead of the layer path.
    const std::string log = stubs.ReadLog();
    MT_EXPECT_TRUE(Contains(log, "/dev/stdin"));
    MT_EXPECT_TRUE(!Contains(log, input));

    // ...and the bytes arrived through the pipe.
    const core::Expected<std::string, std::string> piped =
        core::fs::ReadToString(core::fs::Join(dir, "piped.bin"));
    MT_EXPECT_TRUE(piped.ok());
    if (piped.ok()) MT_EXPECT_TRUE(piped.value() == payload);

    RemoveTreeBestEffort(dir);
}

MT_TEST(a_failing_decompressor_is_reported) {
    // The decompressor's own failure has to surface when the apply itself
    // succeeded. Without this the only zstd coverage would be the happy path,
    // and the second `waitpid` plus its status check would be untested.
    const std::string dir = MakeTempDir();
    StubTools stubs;
    stubs.root = dir;
    MT_EXPECT_TRUE(stubs.Install("cat > /dev/null\n: > \"$(dirname $2)/result.txt\"\n"));

    // Produces nothing and fails.
    MT_EXPECT_TRUE(
        WriteStub(core::fs::Join(stubs.bin, "zstd"), "echo 'corrupt frame' >&2\nexit 7\n"));

    const std::string work = core::fs::Join(dir, "work");
    const std::string input = core::fs::Join(dir, "layer.tar");
    std::string framed;
    framed.push_back(static_cast<char>(0x28));
    framed.push_back(static_cast<char>(0xb5));
    framed.push_back(static_cast<char>(0x2f));
    framed.push_back(static_cast<char>(0xfd));
    framed.append(64, '\0');
    MT_EXPECT_TRUE(core::fs::Write(input, framed).ok());

    const char* old_path = ::getenv("PATH");
    const std::string saved_path = old_path != 0 ? old_path : "";
    MT_EXPECT_TRUE(::setenv("PATH", (stubs.bin + ":" + saved_path).c_str(), 1) == 0);

    ConvertLayerRequest req;
    req.work_dir = work;
    req.input_layer_path = input;
    req.global_config_path = core::fs::Join(dir, "global.json");
    req.virtual_size_gib = 1;

    const OverlaybdTools tools = OverlaybdTools::FromBinDir(stubs.bin);
    const core::Expected<ConvertLayerResult, ToolExecutionError> out =
        tools.ConvertLocalOciLayerToOverlaybd(req);

    if (saved_path.empty()) {
        ::unsetenv("PATH");
    } else {
        ::setenv("PATH", saved_path.c_str(), 1);
    }

    MT_EXPECT_TRUE(!out.ok());
    if (!out.ok()) {
        MT_EXPECT_TRUE(out.error().kind == ToolExecutionError::kNonZeroExit);
        MT_EXPECT_TRUE(Contains(out.error().command_line, "zstd -dc"));
        MT_EXPECT_TRUE(out.error().exit_code.has_value());
        if (out.error().exit_code.has_value()) {
            MT_EXPECT_EQ(static_cast<long long>(*out.error().exit_code),
                         static_cast<long long>(7));
        }
    }
    RemoveTreeBestEffort(dir);
}

MT_TEST(a_plain_layer_is_passed_by_path) {
    // The control for the case above: without it, a broken detector that
    // always piped would still pass there.
    const std::string dir = MakeTempDir();
    StubTools stubs;
    stubs.root = dir;
    MT_EXPECT_TRUE(stubs.Install(": > \"$(dirname $2)/result.txt\"\n"));

    const std::string work = core::fs::Join(dir, "work");
    const std::string input = core::fs::Join(dir, "layer.tar");
    MT_EXPECT_TRUE(WritePlainInput(input));

    ConvertLayerRequest req;
    req.work_dir = work;
    req.input_layer_path = input;
    req.global_config_path = core::fs::Join(dir, "global.json");
    req.virtual_size_gib = 1;

    const OverlaybdTools tools = OverlaybdTools::FromBinDir(stubs.bin);
    MT_EXPECT_TRUE(tools.ConvertLocalOciLayerToOverlaybd(req).ok());

    const std::string log = stubs.ReadLog();
    MT_EXPECT_TRUE(Contains(log, input));
    MT_EXPECT_TRUE(!Contains(log, "/dev/stdin"));

    RemoveTreeBestEffort(dir);
}

MT_MAIN
