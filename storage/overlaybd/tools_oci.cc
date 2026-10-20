// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/tools/oci.rs
#include "agentenv/storage/overlaybd/tools_oci.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "agentenv/core/fs.h"
#include "agentenv/core/json.h"
#include "agentenv/core/logging.h"
#include "agentenv/core/process.h"

namespace agentenv {
namespace storage {
namespace overlaybd {
namespace tools {

const char* const kUpperData = "upper.data";
const char* const kUpperIndex = "upper.index";
const char* const kImageJson = "image.json";
const char* const kLayerCommit = "layer.commit";
const char* const kResultTxt = "result.txt";

namespace {

ToolExecutionError MissingBinary(const std::string& path) {
    ToolExecutionError e;
    e.kind = ToolExecutionError::kMissingBinary;
    e.path = path;
    e.message = "overlaybd tool binary not found at " + path;
    return e;
}

ToolExecutionError InvalidRequest(const std::string& reason) {
    ToolExecutionError e;
    e.kind = ToolExecutionError::kInvalidRequest;
    e.message = reason;
    return e;
}

ToolExecutionError Other(const std::string& context) {
    ToolExecutionError e;
    e.kind = ToolExecutionError::kOther;
    e.message = context;
    return e;
}

ToolExecutionError OtherErrno(const std::string& context, int err) {
    return Other(context + ": " + std::strerror(err));
}

/// Rust's `format!("{cmd:?}")` — enough of the command to identify it in an
/// error. Not shell-quoted, and not meant to be: it is diagnostic text, and
/// presenting it as copy-pasteable would invite someone to paste it.
std::string CommandLine(const std::vector<std::string>& argv) {
    std::string out;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) out += " ";
        out += argv[i];
    }
    return out;
}

ToolExecutionError NonZeroExit(const std::vector<std::string>& argv,
                               const core::process::Output& output) {
    ToolExecutionError e;
    e.kind = ToolExecutionError::kNonZeroExit;
    e.command_line = CommandLine(argv);
    e.stdout_text = output.stdout_text;
    e.stderr_text = output.stderr_text;
    // Rust reports `None` for a signal death. `Output::code` folds that into
    // `128 + signal`, so the signal case is recovered rather than guessed.
    if (output.code >= 128) {
        e.exit_code = core::Optional<int>();
    } else {
        e.exit_code = core::Optional<int>(output.code);
    }

    std::ostringstream o;
    o << "tool failed: " << e.command_line << "\nexit code: ";
    if (e.exit_code.has_value()) {
        o << *e.exit_code;
    } else {
        o << "None";
    }
    o << "\nstderr: " << e.stderr_text;
    e.message = o.str();
    return e;
}

/// Rust `run`.
core::Expected<StepSummary, ToolExecutionError> Run(const std::vector<std::string>& argv,
                                                      const std::string& work_dir,
                                                      int stdin_fd) {
    AGENTENV_DEBUG("exec: " << CommandLine(argv));

    core::process::RunOptions options;
    options.cwd = work_dir;
    options.stdin_fd = stdin_fd;

    const core::Expected<core::process::Output, std::string> output =
        core::process::RunWithOptions(argv, options);
    if (!output.ok()) {
        return core::make_unexpected(Other(CommandLine(argv) + ": " + output.error()));
    }

    StepSummary summary;
    summary.stdout_text = output.value().stdout_text;
    summary.stderr_text = output.value().stderr_text;

    if (!output.value().success()) {
        return core::make_unexpected(NonZeroExit(argv, output.value()));
    }
    return summary;
}

/// Rust `ensure_binary_exists`.
///
/// A path that exists but is not a regular file reports `MissingBinary` too,
/// matching Rust: a directory where the tool should be is the same
/// provisioning failure, and calling it `Other` would send the caller looking
/// for an I/O problem.
core::Expected<core::Unit, ToolExecutionError> EnsureBinaryExists(const std::string& bin) {
    struct stat st;
    if (::stat(bin.c_str(), &st) != 0) {
        if (errno == ENOENT) return core::make_unexpected(MissingBinary(bin));
        return core::make_unexpected(
            OtherErrno("inspect bundled overlaybd binary " + bin, errno));
    }
    if (!S_ISREG(st.st_mode)) return core::make_unexpected(MissingBinary(bin));
    return core::Unit();
}

/// Rust `read_layer_magic` + `layer_uses_zstd_decompression`.
core::Expected<bool, ToolExecutionError> LayerUsesZstdDecompression(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return core::make_unexpected(
            OtherErrno("open input OCI layer for magic detection " + path, errno));
    }

    uint8_t magic[4] = {0, 0, 0, 0};
    size_t got = 0;
    while (got < sizeof(magic)) {
        const ssize_t n = ::read(fd, magic + got, sizeof(magic) - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            const int saved = errno;
            ::close(fd);
            return core::make_unexpected(
                OtherErrno("read input OCI layer magic " + path, saved));
        }
        if (n == 0) break;
        got += static_cast<size_t>(n);
    }
    ::close(fd);

    // A file shorter than four bytes is not zstd, and is not an error here:
    // Rust returns `None` and the caller treats that as "not zstd". Whether
    // such a layer is usable at all is overlaybd-apply's verdict to give.
    if (got != sizeof(magic)) return false;
    return IsZstdMagic(magic);
}

/// Rust `cleanup_conversion_artifacts`.
core::Expected<core::Unit, ToolExecutionError> CleanupConversionArtifacts(
    const std::string& image_json, const std::string& result_txt,
    const std::string& work_dir) {
    // `result.txt` alone may legitimately be absent: it is written by
    // overlaybd-apply, not by this code. The other three were created here,
    // so a missing one means something else removed them and is reported.
    const std::string paths[4] = {core::fs::Join(work_dir, kUpperData),
                                  core::fs::Join(work_dir, kUpperIndex), image_json,
                                  result_txt};
    const bool allow_missing[4] = {false, false, false, true};

    for (int i = 0; i < 4; ++i) {
        if (::unlink(paths[i].c_str()) == 0) continue;
        if (allow_missing[i] && errno == ENOENT) continue;
        return core::make_unexpected(
            OtherErrno("remove conversion artifact " + paths[i], errno));
    }
    return core::Unit();
}

}  // namespace

bool IsZstdMagic(const uint8_t magic[4]) {
    static const uint8_t kZstdFrameMagic[4] = {0x28, 0xb5, 0x2f, 0xfd};
    if (std::memcmp(magic, kZstdFrameMagic, 4) == 0) return true;

    // Skippable frames carry 0x184D2A50..=0x184D2A5F, little-endian on disk.
    // They have to count: a layer may legitimately begin with one, and
    // feeding it to overlaybd-apply as a plain tar would fail.
    const uint32_t value = static_cast<uint32_t>(magic[0]) |
                           (static_cast<uint32_t>(magic[1]) << 8) |
                           (static_cast<uint32_t>(magic[2]) << 16) |
                           (static_cast<uint32_t>(magic[3]) << 24);
    return value >= 0x184D2A50u && value <= 0x184D2A5Fu;
}

core::Expected<core::Unit, ToolExecutionError> WriteImageJson(
    const std::string& path, const std::vector<std::string>& lower_layers,
    const std::string& upper_data, const std::string& upper_index,
    const std::string& result_file) {
    // Rust guards each path with `path_to_json_string`, which refuses
    // non-UTF-8. There is no equivalent check here: a `std::string` path is
    // already bytes, and `core::Json` emits them unchanged. The failure Rust
    // is preventing -- a path that cannot be represented in JSON -- cannot
    // arise from this type.
    core::JsonArray lowers;
    for (size_t i = 0; i < lower_layers.size(); ++i) {
        core::JsonObject lower;
        lower["file"] = core::Json(lower_layers[i]);
        lowers.push_back(core::Json(lower));
    }

    core::JsonObject upper;
    upper["data"] = core::Json(upper_data);
    upper["index"] = core::Json(upper_index);

    core::JsonObject config;
    config["lowers"] = core::Json(lowers);
    config["upper"] = core::Json(upper);
    // `resultFile`, not `result_file`: this name is a wire contract with
    // overlaybd-apply.
    config["resultFile"] = core::Json(result_file);

    // Compact rather than Rust's `to_string_pretty`. The consumer is a parser,
    // and the bytes are equivalent JSON; `core::Json` has no pretty form.
    const std::string json = core::Json(config).ToString();
    const core::Expected<core::Unit, std::string> written = core::fs::Write(path, json);
    if (!written.ok()) {
        return core::make_unexpected(Other("write " + path + ": " + written.error()));
    }
    return core::Unit();
}

OverlaybdTools OverlaybdTools::FromBinDir(const std::string& bin_dir) {
    OverlaybdTools tools;
    tools.create_bin_ = core::fs::Join(bin_dir, "overlaybd-create");
    tools.apply_bin_ = core::fs::Join(bin_dir, "overlaybd-apply");
    tools.commit_bin_ = core::fs::Join(bin_dir, "overlaybd-commit");
    return tools;
}

OverlaybdTools OverlaybdTools::FromOverlaybdInstallRoot(const std::string& root) {
    return FromBinDir(core::fs::Join(root, "bin"));
}

core::Expected<StepSummary, ToolExecutionError> OverlaybdTools::CreateWritableLayer(
    const CreateLayerRequest& req, const std::string& work_dir) const {
    const core::Expected<core::Unit, ToolExecutionError> have = EnsureBinaryExists(create_bin_);
    if (!have.ok()) return core::make_unexpected(have.error());

    std::ostringstream gib;
    gib << req.virtual_size_gib;

    std::vector<std::string> argv;
    argv.push_back(create_bin_);
    argv.push_back(req.data_file);
    argv.push_back(req.index_file);
    argv.push_back(gib.str());
    return Run(argv, work_dir, -1);
}

core::Expected<StepSummary, ToolExecutionError> OverlaybdTools::CommitWritableLayer(
    const CommitLayerRequest& req, const std::string& work_dir) const {
    const core::Expected<core::Unit, ToolExecutionError> have = EnsureBinaryExists(commit_bin_);
    if (!have.ok()) return core::make_unexpected(have.error());

    std::vector<std::string> argv;
    argv.push_back(commit_bin_);
    // Omitted entirely when empty, as Rust's `Option` arranges: passing
    // `--uuid ""` would have the tool stamp an empty identity rather than
    // generate one.
    if (!req.uuid.empty()) {
        argv.push_back("--uuid");
        argv.push_back(req.uuid);
    }
    if (!req.parent_uuid.empty()) {
        argv.push_back("--parent-uuid");
        argv.push_back(req.parent_uuid);
    }
    argv.push_back(req.data_file);
    argv.push_back(req.index_file);
    argv.push_back(req.commit_file);
    return Run(argv, work_dir, -1);
}

core::Expected<StepSummary, ToolExecutionError> OverlaybdTools::ApplyOciLayer(
    const std::string& input_layer_path, const std::string& image_config_path,
    const std::string& global_config_path, bool mkfs, const std::string& work_dir) const {
    const core::Expected<core::Unit, ToolExecutionError> have = EnsureBinaryExists(apply_bin_);
    if (!have.ok()) return core::make_unexpected(have.error());

    // overlaybd-apply as of 1.0.16 cannot decompress zstd tar layers. The
    // support landed in master but that build has its own problems, so a
    // zstd layer is piped through `zstd -d` and the tool is handed a plain
    // tar stream on `/dev/stdin`.
    //
    // Detected from the file magic rather than the OCI mediaType: some
    // builders label zstd layers as gzip (Docker's `rootfs.diff.tar.gzip`
    // carrying a `28 b5 2f fd` frame), so trusting the label would send a
    // zstd frame to a gzip decoder. Temporary -- removable once
    // overlaybd-apply handles tar+zstd natively.
    const core::Expected<bool, ToolExecutionError> is_zstd =
        LayerUsesZstdDecompression(input_layer_path);
    if (!is_zstd.ok()) return core::make_unexpected(is_zstd.error());

    std::vector<std::string> argv;
    argv.push_back(apply_bin_);
    argv.push_back("--service_config_path=" + global_config_path);
    if (mkfs) argv.push_back("--mkfs");

    if (!is_zstd.value()) {
        argv.push_back(input_layer_path);
        argv.push_back(image_config_path);
        return Run(argv, work_dir, -1);
    }

    AGENTENV_DEBUG("input layer is zstd-compressed; piping through `zstd -d`: "
                   << input_layer_path);

    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) {
        return core::make_unexpected(
            OtherErrno("pipe for `zstd -d` layer decompression", errno));
    }

    // `zstd -dc <file>` writes to the pipe; stderr is left inherited so a
    // decompressor complaint still reaches the log. Rust captures it to put
    // it in the error; the simplification is noted in the header.
    const pid_t zstd_pid = ::fork();
    if (zstd_pid < 0) {
        const int saved = errno;
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return core::make_unexpected(
            OtherErrno("spawn `zstd -d` for layer decompression", saved));
    }
    if (zstd_pid == 0) {
        ::close(pipe_fds[0]);
        if (::dup2(pipe_fds[1], STDOUT_FILENO) < 0) ::_exit(127);
        ::close(pipe_fds[1]);
        const char* zstd_argv[5] = {"zstd", "-dc", input_layer_path.c_str(), NULL, NULL};
        ::execvp("zstd", const_cast<char**>(zstd_argv));
        ::_exit(127);
    }

    // The write end must be closed here, or `overlaybd-apply` would never see
    // EOF on its stdin and both processes would wait forever.
    ::close(pipe_fds[1]);

    argv.push_back("/dev/stdin");
    argv.push_back(image_config_path);
    const core::Expected<StepSummary, ToolExecutionError> applied =
        Run(argv, work_dir, pipe_fds[0]);
    ::close(pipe_fds[0]);

    // Reaped unconditionally, before the apply result is examined: returning
    // early would leave a zombie, and on a failed apply the decompressor is
    // usually still running with a full pipe.
    int zstd_status = 0;
    while (::waitpid(zstd_pid, &zstd_status, 0) < 0) {
        if (errno != EINTR) {
            zstd_status = -1;
            break;
        }
    }

    // The apply verdict wins. A failed apply closes the pipe early, which
    // makes `zstd` die of SIGPIPE -- reporting that instead would name the
    // symptom rather than the cause.
    if (!applied.ok()) return applied;

    if (!(WIFEXITED(zstd_status) && WEXITSTATUS(zstd_status) == 0)) {
        ToolExecutionError e;
        e.kind = ToolExecutionError::kNonZeroExit;
        e.command_line = "zstd -dc " + input_layer_path;
        if (WIFEXITED(zstd_status)) {
            e.exit_code = core::Optional<int>(WEXITSTATUS(zstd_status));
        }
        std::ostringstream o;
        o << "tool failed: " << e.command_line << "\nexit code: ";
        if (e.exit_code.has_value()) {
            o << *e.exit_code;
        } else {
            o << "None";
        }
        o << "\nstderr: ";
        e.message = o.str();
        return core::make_unexpected(e);
    }
    return applied;
}

core::Expected<ConvertLayerResult, ToolExecutionError>
OverlaybdTools::ConvertLocalOciLayerToOverlaybd(const ConvertLayerRequest& req) const {
    const std::string& work_dir = req.work_dir;
    const std::string upper_data = core::fs::Join(work_dir, kUpperData);
    const std::string upper_index = core::fs::Join(work_dir, kUpperIndex);
    const std::string image_json = core::fs::Join(work_dir, kImageJson);
    const std::string layer_commit = core::fs::Join(work_dir, kLayerCommit);
    const std::string result_txt = core::fs::Join(work_dir, kResultTxt);

    const core::Expected<core::Unit, std::string> made = core::fs::CreateDirAll(work_dir);
    if (!made.ok()) {
        return core::make_unexpected(Other("create work_dir " + work_dir + ": " + made.error()));
    }

    // Refused rather than overwritten, and checked *after* the directory is
    // created so the rule applies whether or not `work_dir` existed. Reusing
    // a dirty directory is how a conversion silently inherits a previous
    // run's upper and produces a layer containing both.
    const std::string claimed[5] = {upper_data, upper_index, image_json, layer_commit,
                                    result_txt};
    for (int i = 0; i < 5; ++i) {
        if (core::fs::Exists(claimed[i])) {
            return core::make_unexpected(InvalidRequest(
                "target file already exists: " + claimed[i] + "; provide a clean work_dir"));
        }
    }

    AGENTENV_INFO("overlaybd-create: work_dir=" << work_dir);
    CreateLayerRequest create;
    create.data_file = upper_data;
    create.index_file = upper_index;
    create.virtual_size_gib = req.virtual_size_gib;
    const core::Expected<StepSummary, ToolExecutionError> created =
        CreateWritableLayer(create, work_dir);
    if (!created.ok()) return core::make_unexpected(created.error());

    const core::Expected<core::Unit, ToolExecutionError> wrote =
        WriteImageJson(image_json, req.lower_layers, upper_data, upper_index, result_txt);
    if (!wrote.ok()) return core::make_unexpected(wrote.error());

    AGENTENV_INFO("overlaybd-apply: work_dir=" << work_dir);
    const core::Expected<StepSummary, ToolExecutionError> applied = ApplyOciLayer(
        req.input_layer_path, image_json, req.global_config_path, req.mkfs, work_dir);
    if (!applied.ok()) return core::make_unexpected(applied.error());

    AGENTENV_INFO("overlaybd-commit: work_dir=" << work_dir);
    CommitLayerRequest commit;
    commit.data_file = upper_data;
    commit.index_file = upper_index;
    commit.commit_file = layer_commit;
    commit.uuid = req.uuid;
    commit.parent_uuid = req.parent_uuid;
    const core::Expected<StepSummary, ToolExecutionError> committed =
        CommitWritableLayer(commit, work_dir);
    if (!committed.ok()) return core::make_unexpected(committed.error());

    // Only on the success path. Every early return above leaves the
    // intermediates in place, which is the only way a failed conversion can
    // be diagnosed.
    const core::Expected<core::Unit, ToolExecutionError> cleaned =
        CleanupConversionArtifacts(image_json, result_txt, work_dir);
    if (!cleaned.ok()) return core::make_unexpected(cleaned.error());

    ConvertLayerResult result;
    result.layer_commit = layer_commit;
    return result;
}

}  // namespace tools
}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
