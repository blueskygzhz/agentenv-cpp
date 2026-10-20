// SPDX-License-Identifier: MIT
// Tests for storage/ublk-daemon's runtime materialization, ported against the
// `mod tests` of `storage/ublk-daemon/src/runtime.rs`.
//
// The tests that need a real sealed overlaybd lower are not ported: those
// cases resolve the base virtual size by opening the image, and every one of
// them passes `known_source_virtual_size` or `read_only` in the variants kept
// below, which reaches the same materialization code without building a layer
// stack. What is *not* covered as a result is named at the bottom of this file
// rather than left to be discovered.
#include "agentenv/storage/ublk-daemon/runtime.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "agentenv/core/fs.h"
#include "agentenv/storage/overlaybd/lsmt_rw.h"
#include "microtest.h"

namespace ud = agentenv::storage::ublk_daemon;
namespace obd = agentenv::storage::overlaybd;
namespace core = agentenv::core;

namespace {

/// A temp directory removed by the destructor, so a failing assertion does not
/// leave a claimed runtime dir behind to confuse the next run.
class TempDir {
 public:
    TempDir() {
        const core::Expected<std::string, std::string> made =
            core::fs::CreateTempDir("agentenv-ud-runtime-");
        // Normalized because `$TMPDIR` may carry a doubled slash, while the
        // code under test resolves paths lexically and collapses one. Without
        // this, every expectation would be comparing two spellings of the same
        // path and failing for a reason that has nothing to do with the daemon.
        if (made.ok()) path_ = obd::LexicallyNormalizePath(made.value());
    }
    ~TempDir() {
        if (!path_.empty()) core::fs::RemoveDirAll(path_);
    }
    const std::string& path() const { return path_; }
    std::string Join(const std::string& leaf) const { return core::fs::Join(path_, leaf); }

 private:
    TempDir(const TempDir&);
    TempDir& operator=(const TempDir&);
    std::string path_;
};

void WriteFile(const std::string& path, const std::string& body) {
    MT_EXPECT_TRUE(core::fs::Write(path, body).ok());
}

std::string ReadFile(const std::string& path) {
    const core::Expected<std::string, std::string> text = core::fs::ReadToString(path);
    MT_EXPECT_TRUE(text.ok());
    return text.value();
}

/// Rust `fake_resize_tool`.
std::string FakeResizeTool(const TempDir& dir, const std::string& name,
                           const std::string& body) {
    const std::string path = dir.Join(name);
    WriteFile(path, "#!/bin/sh\n" + body + "\n");
    MT_EXPECT_TRUE(core::fs::SetPermissions(path, 0755).ok());
    return path;
}

core::Json ParsedFile(const std::string& path) {
    const core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(ReadFile(path));
    MT_EXPECT_TRUE(parsed.ok());
    return parsed.value();
}

std::string StringField(const core::Json& object, const std::string& key) {
    const core::JsonObject& map = object.as_object();
    const core::JsonObject::const_iterator found = map.find(key);
    MT_EXPECT_TRUE(found != map.end());
    MT_EXPECT_TRUE(found->second.kind() == core::Json::Kind::String);
    return found->second.as_string();
}

const core::Json& Field(const core::Json& object, const std::string& key) {
    const core::JsonObject& map = object.as_object();
    const core::JsonObject::const_iterator found = map.find(key);
    MT_EXPECT_TRUE(found != map.end());
    return found->second;
}

/// A source `image.json` with no writable upper — the read-only template
/// shape. The lower names a file that need not exist: nothing here opens it.
std::string WriteLowerOnlySource(const TempDir& dir) {
    const std::string path = dir.Join("source-image.json");
    WriteFile(path,
              "{\"lowers\":[{\"file\":\"" + dir.Join("lower.data") +
                  "\"}],\"upper\":{},\"resultFile\":\"\"}");
    return path;
}

/// Returns the absent p2p URL.
///
/// A helper rather than `ImageServiceCache cache(core::Optional<std::string>())`
/// written inline: that spelling declares a function, not a variable.
core::Optional<std::string> NoP2pPublishUrl() { return core::Optional<std::string>(); }

/// Rust `materialize_test_request`.
ud::MaterializeOverlaybdRuntimeRequest TestRequest(ud::ImageServiceCache* cache,
                                                   const std::string& source,
                                                   const std::string& global_config,
                                                   const std::string& runtime_dir) {
    ud::MaterializeOverlaybdRuntimeRequest request;
    request.image_service_cache = cache;
    request.source_image_config = source;
    request.global_config = global_config;
    request.runtime_dir = runtime_dir;
    request.read_only = false;
    request.runtime_upper_mode = obd::UpperMode::LogStructured;
    request.resize_global_config = global_config;
    request.allow_shrink = false;
    return request;
}

}  // namespace

// ---------------------------------------------------------------------------
// validate_requested_virtual_size
// ---------------------------------------------------------------------------

// Rust `requested_virtual_size_validation_matrix`.
MT_TEST(absent_and_equal_requests_pass_through_unchecked) {
    const uint64_t base = 3ull * ud::kGib + 7;  // deliberately not GiB-aligned

    const core::Expected<uint64_t, std::string> absent =
        ud::ValidateRequestedVirtualSize(core::Optional<uint64_t>(), base, false);
    MT_EXPECT_TRUE(absent.ok());
    MT_EXPECT_EQ(absent.value(), base);

    // The base's own size is accepted even though it is not GiB-aligned: no
    // resize happens, so the tool's granularity never comes into play. Testing
    // the misaligned base specifically, because an implementation that checks
    // alignment first would reject a perfectly usable image.
    const core::Expected<uint64_t, std::string> equal =
        ud::ValidateRequestedVirtualSize(core::Optional<uint64_t>(base), base, false);
    MT_EXPECT_TRUE(equal.ok());
    MT_EXPECT_EQ(equal.value(), base);
}

MT_TEST(a_zero_or_misaligned_request_is_refused) {
    const uint64_t base = 2ull * ud::kGib;

    const core::Expected<uint64_t, std::string> zero =
        ud::ValidateRequestedVirtualSize(core::Optional<uint64_t>(0), base, false);
    MT_EXPECT_TRUE(!zero.ok());
    MT_EXPECT_TRUE(zero.error().find("non-zero") != std::string::npos);

    const core::Expected<uint64_t, std::string> misaligned = ud::ValidateRequestedVirtualSize(
        core::Optional<uint64_t>(3ull * ud::kGib + 1), base, false);
    MT_EXPECT_TRUE(!misaligned.ok());
    MT_EXPECT_TRUE(misaligned.error().find("GiB-aligned") != std::string::npos);
}

MT_TEST(shrinking_is_refused_unless_enabled) {
    const uint64_t base = 4ull * ud::kGib;
    const core::Optional<uint64_t> smaller(2ull * ud::kGib);

    const core::Expected<uint64_t, std::string> refused =
        ud::ValidateRequestedVirtualSize(smaller, base, false);
    MT_EXPECT_TRUE(!refused.ok());
    MT_EXPECT_TRUE(refused.error().find("shrinking is disabled") != std::string::npos);

    const core::Expected<uint64_t, std::string> allowed =
        ud::ValidateRequestedVirtualSize(smaller, base, true);
    MT_EXPECT_TRUE(allowed.ok());
    MT_EXPECT_EQ(allowed.value(), 2ull * ud::kGib);
}

MT_TEST(growing_is_always_allowed) {
    const core::Expected<uint64_t, std::string> grown = ud::ValidateRequestedVirtualSize(
        core::Optional<uint64_t>(8ull * ud::kGib), 4ull * ud::kGib, false);
    MT_EXPECT_TRUE(grown.ok());
    MT_EXPECT_EQ(grown.value(), 8ull * ud::kGib);
}

// ---------------------------------------------------------------------------
// LD_LIBRARY_PATH and bounded output
// ---------------------------------------------------------------------------

// Rust `overlaybd_library_path_prepends_tool_lib_and_ignores_empty_inherited_value`.
MT_TEST(the_tool_lib_dir_is_prepended_and_an_empty_inherited_value_is_dropped) {
    const core::Expected<std::string, std::string> joined = ud::OverlaybdLibraryPath(
        "/overlaybd/lib", core::Optional<std::string>(std::string("/first:/second")));
    MT_EXPECT_TRUE(joined.ok());
    MT_EXPECT_EQ(joined.value(), std::string("/overlaybd/lib:/first:/second"));

    // Not "/overlaybd/lib:" — a trailing separator means "the current
    // directory" to the loader, which would let a stray .so in the cwd win.
    const core::Expected<std::string, std::string> empty =
        ud::OverlaybdLibraryPath("/overlaybd/lib", core::Optional<std::string>(std::string()));
    MT_EXPECT_TRUE(empty.ok());
    MT_EXPECT_EQ(empty.value(), std::string("/overlaybd/lib"));

    const core::Expected<std::string, std::string> unset =
        ud::OverlaybdLibraryPath("/overlaybd/lib", core::Optional<std::string>());
    MT_EXPECT_TRUE(unset.ok());
    MT_EXPECT_EQ(unset.value(), std::string("/overlaybd/lib"));
}

// Rust `read_bounded_output_marks_truncation_within_limit`.
MT_TEST(bounded_output_marks_truncation_and_stays_within_the_limit) {
    TempDir dir;
    const std::string path = dir.Join("big.txt");
    WriteFile(path, std::string(ud::kResizeOutputLimit + 1, 'x'));
    const int fd = ::open(path.c_str(), O_RDONLY);
    MT_EXPECT_TRUE(fd >= 0);
    const core::Expected<std::string, std::string> output = ud::ReadBoundedOutput(fd);
    ::close(fd);

    MT_EXPECT_TRUE(output.ok());
    // The marker is *inside* the limit, not appended past it: the whole point
    // is that the string handed to an error message is bounded.
    MT_EXPECT_EQ(output.value().size(), ud::kResizeOutputLimit);
    MT_EXPECT_TRUE(output.value().find(ud::kResizeOutputTruncatedMarker) != std::string::npos);
}

MT_TEST(bounded_output_leaves_an_under_limit_payload_unmarked) {
    TempDir dir;
    const std::string path = dir.Join("small.txt");
    WriteFile(path, "all of it");
    const int fd = ::open(path.c_str(), O_RDONLY);
    MT_EXPECT_TRUE(fd >= 0);
    const core::Expected<std::string, std::string> output = ud::ReadBoundedOutput(fd);
    ::close(fd);

    MT_EXPECT_TRUE(output.ok());
    MT_EXPECT_EQ(output.value(), std::string("all of it"));
}

// ---------------------------------------------------------------------------
// run_resize_tool
// ---------------------------------------------------------------------------

// Rust `run_resize_tool_handles_failure_and_timeout`.
MT_TEST(a_failing_resize_tool_reports_both_streams) {
    TempDir dir;
    ud::ResizeToolSpec tool;
    tool.binary = FakeResizeTool(dir, "fake-resize-fail.sh",
                                 "echo failed-out; echo failed-err >&2; exit 7");
    tool.timeout_secs = 5;

    const core::Expected<core::Unit, std::string> ran =
        ud::RunResizeTool(tool, dir.Join("image.json"), dir.Join("global.json"), ud::kGib);
    MT_EXPECT_TRUE(!ran.ok());
    // Both, because the tool splits its diagnostics across the two and an
    // error carrying only one of them is routinely empty.
    MT_EXPECT_TRUE(ran.error().find("failed-out") != std::string::npos);
    MT_EXPECT_TRUE(ran.error().find("failed-err") != std::string::npos);
    MT_EXPECT_TRUE(ran.error().find("exit status: 7") != std::string::npos);
}

MT_TEST(a_hanging_resize_tool_is_killed_and_reported_as_a_timeout) {
    TempDir dir;
    ud::ResizeToolSpec tool;
    tool.binary = FakeResizeTool(dir, "fake-resize-hang.sh", "sleep 30");
    tool.timeout_secs = 1;

    const core::Expected<core::Unit, std::string> ran =
        ud::RunResizeTool(tool, dir.Join("image.json"), dir.Join("global.json"), ud::kGib);
    MT_EXPECT_TRUE(!ran.ok());
    // Reported as a timeout, not as "signal: 9": the SIGKILL is how the
    // timeout is enforced, so naming it would blame the symptom.
    MT_EXPECT_TRUE(ran.error().find("timed out after 1 seconds") != std::string::npos);
}

MT_TEST(the_resize_tool_is_passed_gibibytes_not_bytes) {
    TempDir dir;
    const std::string record = dir.Join("argv.txt");
    ud::ResizeToolSpec tool;
    // Records its own argv so the unit conversion is observable; the tool
    // itself takes GiB, and passing bytes would silently ask for a device
    // 2^30 times too large.
    tool.binary =
        FakeResizeTool(dir, "fake-resize-record.sh", "printf '%s\\n' \"$@\" > " + record);
    tool.timeout_secs = 5;

    const core::Expected<core::Unit, std::string> ran = ud::RunResizeTool(
        tool, dir.Join("image.json"), dir.Join("global.json"), 6ull * ud::kGib);
    MT_EXPECT_TRUE(ran.ok());

    const std::string argv = ReadFile(record);
    MT_EXPECT_TRUE(argv.find("--size\n6\n") != std::string::npos);
    MT_EXPECT_TRUE(argv.find("--service_config_path\n") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Upper resolution
// ---------------------------------------------------------------------------

MT_TEST(a_read_only_request_resolves_to_an_absent_upper) {
    TempDir dir;
    // The path does not exist, which is the point: a read-only device must not
    // need the source's upper to be readable to come up.
    const core::Expected<ud::ResolvedUpperMode, std::string> resolved =
        ud::ResolveUpperMode(dir.Join("missing.json"), true, obd::UpperMode::LogStructured);
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_TRUE(resolved.value().kind == ud::ResolvedUpperMode::kAbsent);
}

MT_TEST(a_source_without_a_usable_upper_resolves_to_create) {
    TempDir dir;
    const std::string source = WriteLowerOnlySource(dir);
    const core::Expected<ud::ResolvedUpperMode, std::string> resolved =
        ud::ResolveUpperMode(source, false, obd::UpperMode::HybridLogStructured);
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_TRUE(resolved.value().kind == ud::ResolvedUpperMode::kCreate);
    MT_EXPECT_TRUE(resolved.value().create_mode == obd::UpperMode::HybridLogStructured);
}

MT_TEST(a_source_with_a_complete_upper_resolves_to_existing_with_absolute_paths) {
    TempDir dir;
    const std::string source = dir.Join("source-image.json");
    WriteFile(source,
              "{\"lowers\":[],\"upper\":{\"mode\":\"logStructured\","
              "\"data\":\"./src.data\",\"index\":\"./src.index\","
              "\"target\":\"\",\"gzipIndex\":\"\"},\"resultFile\":\"\"}");

    const core::Expected<core::Optional<ud::ResolvedSourceUpper>, std::string> resolved =
        ud::ResolveSourceUpper(source);
    MT_EXPECT_TRUE(resolved.ok());
    MT_EXPECT_TRUE(resolved.value().has_value());
    MT_EXPECT_EQ(resolved.value()->data_path, dir.Join("src.data"));
    MT_EXPECT_TRUE(resolved.value()->index_path.has_value());
    MT_EXPECT_EQ(*resolved.value()->index_path, dir.Join("src.index"));
    // Empty is unset, not "the config's own directory": resolving "" would
    // produce a path to a file nothing created.
    MT_EXPECT_TRUE(!resolved.value()->target_path.has_value());
    MT_EXPECT_TRUE(!resolved.value()->gzip_index_path.has_value());
}

MT_TEST(a_source_with_a_half_declared_upper_is_refused) {
    TempDir dir;
    const std::string source = dir.Join("source-image.json");
    // Data without index: not "no upper", because the caller would then lay a
    // second upper over storage the source is already writing to.
    WriteFile(source, "{\"lowers\":[],\"upper\":{\"data\":\"./src.data\"},\"resultFile\":\"\"}");
    const core::Expected<core::Optional<ud::ResolvedSourceUpper>, std::string> resolved =
        ud::ResolveSourceUpper(source);
    MT_EXPECT_TRUE(!resolved.ok());
}

MT_TEST(upper_index_paths_exist_only_for_the_indexed_layouts) {
    MT_EXPECT_TRUE(!ud::RuntimeUpperIndexPath("/r", obd::UpperMode::Sparse).has_value());
    MT_EXPECT_TRUE(ud::RuntimeUpperIndexPath("/r", obd::UpperMode::LogStructured).has_value());
    MT_EXPECT_EQ(*ud::RuntimeUpperIndexPath("/r", obd::UpperMode::LogStructured),
                 std::string("/r/upper.index"));
    MT_EXPECT_TRUE(
        ud::RuntimeUpperIndexPath("/r", obd::UpperMode::HybridLogStructured).has_value());
}

MT_TEST(every_upper_mode_maps_to_its_own_rw_layout) {
    // A wrong mapping here produces a file the loader opens with the wrong
    // layout, which reads as corruption rather than as a configuration error.
    MT_EXPECT_TRUE(ud::RwLayoutFromUpperMode(obd::UpperMode::Sparse) == obd::kRwSparse);
    MT_EXPECT_TRUE(ud::RwLayoutFromUpperMode(obd::UpperMode::LogStructured) ==
                   obd::kRwLogStructured);
    MT_EXPECT_TRUE(ud::RwLayoutFromUpperMode(obd::UpperMode::HybridLogStructured) ==
                   obd::kRwHybridLogStructured);
}

// ---------------------------------------------------------------------------
// materialize_overlaybd_image_config
// ---------------------------------------------------------------------------

MT_TEST(an_absent_upper_serializes_as_an_empty_object) {
    TempDir dir;
    const std::string source = WriteLowerOnlySource(dir);
    const core::Expected<core::Json, std::string> image = ud::MaterializeOverlaybdImageConfig(
        source, dir.Join("runtime"), ud::ResolvedUpperMode::Absent());
    MT_EXPECT_TRUE(image.ok());
    MT_EXPECT_TRUE(Field(image.value(), "upper").as_object().empty());
    MT_EXPECT_EQ(StringField(image.value(), "resultFile"), std::string("./result.txt"));
}

MT_TEST(a_created_log_structured_upper_names_both_files_relatively) {
    TempDir dir;
    const std::string source = WriteLowerOnlySource(dir);
    const core::Expected<core::Json, std::string> image = ud::MaterializeOverlaybdImageConfig(
        source, dir.Join("runtime"),
        ud::ResolvedUpperMode::Create(obd::UpperMode::LogStructured));
    MT_EXPECT_TRUE(image.ok());
    const core::Json& upper = Field(image.value(), "upper");
    MT_EXPECT_EQ(StringField(upper, "mode"), std::string("logStructured"));
    MT_EXPECT_EQ(StringField(upper, "data"), std::string("./upper.data"));
    MT_EXPECT_EQ(StringField(upper, "index"), std::string("./upper.index"));
    MT_EXPECT_EQ(StringField(upper, "target"), std::string(""));
    MT_EXPECT_EQ(StringField(upper, "gzipIndex"), std::string(""));
}

MT_TEST(a_created_sparse_upper_leaves_the_index_empty) {
    TempDir dir;
    const std::string source = WriteLowerOnlySource(dir);
    const core::Expected<core::Json, std::string> image = ud::MaterializeOverlaybdImageConfig(
        source, dir.Join("runtime"), ud::ResolvedUpperMode::Create(obd::UpperMode::Sparse));
    MT_EXPECT_TRUE(image.ok());
    const core::Json& upper = Field(image.value(), "upper");
    MT_EXPECT_EQ(StringField(upper, "mode"), std::string("sparse"));
    // Empty, not "./upper.index": a sparse upper has no index file, and the
    // loader would try to open the one this names.
    MT_EXPECT_EQ(StringField(upper, "index"), std::string(""));
}

MT_TEST(lower_paths_are_retargeted_at_the_runtime_dir) {
    TempDir dir;
    const std::string source = dir.Join("source-image.json");
    // Written relative to the source dir, as a produced config is.
    WriteFile(source, "{\"lowers\":[{\"file\":\"./lower.data\"}],\"upper\":{},"
                      "\"resultFile\":\"\"}");
    const std::string runtime_dir = dir.Join("runtime");
    const core::Expected<core::Json, std::string> image = ud::MaterializeOverlaybdImageConfig(
        source, runtime_dir, ud::ResolvedUpperMode::Absent());
    MT_EXPECT_TRUE(image.ok());

    const core::Json& lowers = Field(image.value(), "lowers");
    MT_EXPECT_EQ(lowers.as_array().size(), static_cast<std::size_t>(1));
    // Must resolve to the *source's* lower from the runtime dir. Left alone it
    // would name `runtime/lower.data`, which does not exist.
    const std::string file = StringField(lowers.as_array()[0], "file");
    MT_EXPECT_EQ(file, std::string("../lower.data"));
}

MT_TEST(an_existing_upper_is_rewritten_relative_to_the_runtime_dir) {
    TempDir dir;
    const std::string source = dir.Join("source-image.json");
    WriteFile(source,
              "{\"lowers\":[],\"upper\":{\"mode\":\"sparse\",\"data\":\"./src.data\","
              "\"index\":\"./src.index\",\"target\":\"\",\"gzipIndex\":\"\"},"
              "\"resultFile\":\"\"}");
    const core::Expected<core::Optional<ud::ResolvedSourceUpper>, std::string> existing =
        ud::ResolveSourceUpper(source);
    MT_EXPECT_TRUE(existing.ok() && existing.value().has_value());

    const core::Expected<core::Json, std::string> image = ud::MaterializeOverlaybdImageConfig(
        source, dir.Join("runtime"), ud::ResolvedUpperMode::Existing(*existing.value()));
    MT_EXPECT_TRUE(image.ok());
    const core::Json& upper = Field(image.value(), "upper");
    // The source's own mode is preserved, not replaced by the request's
    // runtime mode: the files already exist in that layout.
    MT_EXPECT_EQ(StringField(upper, "mode"), std::string("sparse"));
    MT_EXPECT_EQ(StringField(upper, "data"), std::string("../src.data"));
    MT_EXPECT_EQ(StringField(upper, "index"), std::string("../src.index"));
    MT_EXPECT_EQ(StringField(upper, "target"), std::string(""));
}

// ---------------------------------------------------------------------------
// materialize_overlaybd_runtime
// ---------------------------------------------------------------------------

// Rust `materialize_read_only_runtime_writes_empty_upper`.
MT_TEST(a_read_only_runtime_writes_an_empty_upper_and_no_upper_files) {
    TempDir dir;
    ud::ImageServiceCache cache(NoP2pPublishUrl());
    const std::string runtime_dir = dir.Join("runtime");
    ud::MaterializeOverlaybdRuntimeRequest request =
        TestRequest(&cache, WriteLowerOnlySource(dir), dir.Join("global.json"), runtime_dir);
    request.read_only = true;
    // Supplied so nothing has to open the image; the Rust test resolves it
    // from a real sealed lower instead.
    request.known_source_virtual_size = core::Optional<uint64_t>(4ull * ud::kGib);

    core::Expected<ud::MaterializedOverlaybdRuntime, ud::RuntimeError> runtime =
        ud::MaterializeOverlaybdRuntime(request);
    MT_EXPECT_TRUE(runtime.ok());
    MT_EXPECT_EQ(runtime.value().actual_virtual_size, 4ull * ud::kGib);
    MT_EXPECT_EQ(runtime.value().runtime_image_config_path,
                 core::fs::Join(runtime_dir, "image.json"));

    MT_EXPECT_TRUE(
        ParsedFile(runtime.value().runtime_image_config_path).as_object().count("upper") == 1);
    MT_EXPECT_TRUE(
        Field(ParsedFile(runtime.value().runtime_image_config_path), "upper").as_object().empty());
    // No upper files, because there is no upper. Creating them anyway would
    // make a read-only device look writable to anything inspecting the dir.
    MT_EXPECT_TRUE(!core::fs::Exists(core::fs::Join(runtime_dir, "upper.data")));
    MT_EXPECT_TRUE(!core::fs::Exists(core::fs::Join(runtime_dir, "upper.index")));
}

// Rust `materialize_writable_lower_only_runtime_covers_upper_layouts`.
MT_TEST(a_writable_runtime_lays_down_the_upper_files_for_its_layout) {
    TempDir dir;
    ud::ImageServiceCache cache(NoP2pPublishUrl());

    const std::string ls_dir = dir.Join("runtime-ls");
    ud::MaterializeOverlaybdRuntimeRequest ls =
        TestRequest(&cache, WriteLowerOnlySource(dir), dir.Join("global.json"), ls_dir);
    ls.known_source_virtual_size = core::Optional<uint64_t>(2ull * ud::kGib);
    ls.runtime_upper_mode = obd::UpperMode::LogStructured;
    MT_EXPECT_TRUE(ud::MaterializeOverlaybdRuntime(ls).ok());
    MT_EXPECT_TRUE(core::fs::Exists(core::fs::Join(ls_dir, "upper.data")));
    MT_EXPECT_TRUE(core::fs::Exists(core::fs::Join(ls_dir, "upper.index")));

    const std::string sparse_dir = dir.Join("runtime-sparse");
    ud::MaterializeOverlaybdRuntimeRequest sparse =
        TestRequest(&cache, WriteLowerOnlySource(dir), dir.Join("global.json"), sparse_dir);
    sparse.known_source_virtual_size = core::Optional<uint64_t>(2ull * ud::kGib);
    sparse.runtime_upper_mode = obd::UpperMode::Sparse;
    MT_EXPECT_TRUE(ud::MaterializeOverlaybdRuntime(sparse).ok());
    MT_EXPECT_TRUE(core::fs::Exists(core::fs::Join(sparse_dir, "upper.data")));
    MT_EXPECT_TRUE(!core::fs::Exists(core::fs::Join(sparse_dir, "upper.index")));
}

MT_TEST(the_upper_is_created_at_the_base_size_not_the_requested_one) {
    TempDir dir;
    ud::ImageServiceCache cache(NoP2pPublishUrl());
    const std::string runtime_dir = dir.Join("runtime");
    ud::MaterializeOverlaybdRuntimeRequest request =
        TestRequest(&cache, WriteLowerOnlySource(dir), dir.Join("global.json"), runtime_dir);
    request.known_source_virtual_size = core::Optional<uint64_t>(2ull * ud::kGib);
    MT_EXPECT_TRUE(ud::MaterializeOverlaybdRuntime(request).ok());

    // The header must declare the *base* size: the resize tool grows it
    // afterwards, and a header already at the target would leave the lowers
    // describing a smaller device than the upper claims.
    MT_EXPECT_TRUE(obd::ValidateRwHeaderPairPaths(
                       core::fs::Join(runtime_dir, "upper.data"),
                       core::Optional<std::string>(core::fs::Join(runtime_dir, "upper.index")),
                       2ull * ud::kGib, obd::kRwLogStructured)
                       .ok());
}

MT_TEST(an_existing_runtime_dir_is_refused_rather_than_adopted) {
    TempDir dir;
    ud::ImageServiceCache cache(NoP2pPublishUrl());
    const std::string runtime_dir = dir.Join("runtime");
    MT_EXPECT_TRUE(core::fs::CreateDirAll(runtime_dir).ok());
    // Something is already in there — plausibly a live device's upper.
    WriteFile(core::fs::Join(runtime_dir, "upper.data"), "in use");

    ud::MaterializeOverlaybdRuntimeRequest request =
        TestRequest(&cache, WriteLowerOnlySource(dir), dir.Join("global.json"), runtime_dir);
    request.known_source_virtual_size = core::Optional<uint64_t>(2ull * ud::kGib);

    const core::Expected<ud::MaterializedOverlaybdRuntime, ud::RuntimeError> runtime =
        ud::MaterializeOverlaybdRuntime(request);
    MT_EXPECT_TRUE(!runtime.ok());
    MT_EXPECT_TRUE(runtime.error().message.find("must not already exist") != std::string::npos);
    // And the existing contents are untouched: a failed claim that cleaned up
    // would destroy the device it collided with.
    MT_EXPECT_EQ(ReadFile(core::fs::Join(runtime_dir, "upper.data")), std::string("in use"));
}

MT_TEST(a_failed_materialization_removes_the_directory_it_claimed) {
    TempDir dir;
    ud::ImageServiceCache cache(NoP2pPublishUrl());
    const std::string runtime_dir = dir.Join("runtime");
    // A resize with no tool configured: rejected after the dir is claimed and
    // the upper is laid down, which is exactly the partial state that must not
    // survive.
    ud::MaterializeOverlaybdRuntimeRequest request =
        TestRequest(&cache, WriteLowerOnlySource(dir), dir.Join("global.json"), runtime_dir);
    request.known_source_virtual_size = core::Optional<uint64_t>(2ull * ud::kGib);
    request.requested_virtual_size = core::Optional<uint64_t>(4ull * ud::kGib);
    request.resize_tool = 0;

    const core::Expected<ud::MaterializedOverlaybdRuntime, ud::RuntimeError> runtime =
        ud::MaterializeOverlaybdRuntime(request);
    MT_EXPECT_TRUE(!runtime.ok());
    MT_EXPECT_TRUE(runtime.error().invalid_request);
    MT_EXPECT_TRUE(runtime.error().message.find("resize tool is required") != std::string::npos);
    // Gone, or the next attempt at this path would fail with "already
    // exists" and the caller could never make progress.
    MT_EXPECT_TRUE(!core::fs::Exists(runtime_dir));
}

MT_TEST(resizing_an_existing_upper_is_an_invalid_request) {
    TempDir dir;
    ud::ImageServiceCache cache(NoP2pPublishUrl());
    const std::string source = dir.Join("source-image.json");
    WriteFile(source,
              "{\"lowers\":[],\"upper\":{\"mode\":\"logStructured\","
              "\"data\":\"./src.data\",\"index\":\"./src.index\"},\"resultFile\":\"\"}");

    ud::MaterializeOverlaybdRuntimeRequest request =
        TestRequest(&cache, source, dir.Join("global.json"), dir.Join("runtime"));
    request.known_source_virtual_size = core::Optional<uint64_t>(2ull * ud::kGib);
    request.requested_virtual_size = core::Optional<uint64_t>(4ull * ud::kGib);

    const core::Expected<ud::MaterializedOverlaybdRuntime, ud::RuntimeError> runtime =
        ud::MaterializeOverlaybdRuntime(request);
    MT_EXPECT_TRUE(!runtime.ok());
    // The caller's fault, not ours: growing a shared upper would corrupt
    // whatever else is writing to it, so the request has to come back as a
    // rejection the client will not retry.
    MT_EXPECT_TRUE(runtime.error().invalid_request);
    MT_EXPECT_TRUE(runtime.error().message.find("fresh writable OverlayBD upper") !=
                   std::string::npos);
}

MT_TEST(a_zero_base_virtual_size_is_refused_as_internal_not_invalid) {
    TempDir dir;
    ud::ImageServiceCache cache(NoP2pPublishUrl());
    ud::MaterializeOverlaybdRuntimeRequest request =
        TestRequest(&cache, WriteLowerOnlySource(dir), dir.Join("global.json"),
                    dir.Join("runtime"));
    request.known_source_virtual_size = core::Optional<uint64_t>(0);

    const core::Expected<ud::MaterializedOverlaybdRuntime, ud::RuntimeError> runtime =
        ud::MaterializeOverlaybdRuntime(request);
    MT_EXPECT_TRUE(!runtime.ok());
    // Not an invalid request: the size came from the image, not the client,
    // so blaming the caller would send them hunting for a parameter they
    // never set.
    MT_EXPECT_TRUE(!runtime.error().invalid_request);
    MT_EXPECT_TRUE(runtime.error().message.find("non-zero") != std::string::npos);
}

// Rust `cleanup_runtime_dir_removes_all_daemon_owned_contents`.
MT_TEST(rollback_removes_the_claim_and_is_idempotent) {
    TempDir dir;
    ud::ImageServiceCache cache(NoP2pPublishUrl());
    const std::string runtime_dir = dir.Join("runtime");
    ud::MaterializeOverlaybdRuntimeRequest request =
        TestRequest(&cache, WriteLowerOnlySource(dir), dir.Join("global.json"), runtime_dir);
    request.known_source_virtual_size = core::Optional<uint64_t>(2ull * ud::kGib);

    core::Expected<ud::MaterializedOverlaybdRuntime, ud::RuntimeError> runtime =
        ud::MaterializeOverlaybdRuntime(request);
    MT_EXPECT_TRUE(runtime.ok());
    ud::MaterializedOverlaybdRuntime owned = runtime.value();
    MT_EXPECT_EQ(owned.claimed_runtime_dir(), runtime_dir);

    owned.Rollback();
    MT_EXPECT_TRUE(!core::fs::Exists(runtime_dir));
    // The claim is forgotten, so a second rollback cannot delete a directory
    // that has since been re-claimed for a different device.
    MT_EXPECT_TRUE(owned.claimed_runtime_dir().empty());
    MT_EXPECT_TRUE(core::fs::CreateDirAll(runtime_dir).ok());
    WriteFile(core::fs::Join(runtime_dir, "someone-elses"), "x");
    owned.Rollback();
    MT_EXPECT_TRUE(core::fs::Exists(core::fs::Join(runtime_dir, "someone-elses")));
}

// ---------------------------------------------------------------------------
// Not covered here
// ---------------------------------------------------------------------------
//
// Rust's `materialize_writable_lower_only_runtime_resolves_base_size_when_unknown`,
// `resolve_overlaybd_virtual_size_falls_back_to_opening_image` and
// `materialize_resize_runs_fake_tool_and_reopens_target_size` all build a real
// sealed overlaybd lower and then open the image. They exercise
// `ResolveOverlaybdVirtualSize`'s trailer/open fallback and the post-resize
// reopen, neither of which the cases above reach — every test here either
// supplies `known_source_virtual_size` or stops before the resize. Those two
// paths are therefore unverified by this file; see `test_storage_overlaybd` and
// `test_lsmt` for coverage of the layer primitives they rest on.

MT_MAIN
