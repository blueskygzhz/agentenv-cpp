// SPDX-License-Identifier: MIT
// Rust: the tests in src/image/buildkit.rs and src/image/buildkit/history.rs.
#include "agentenv/image/buildkit.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "microtest.h"

using namespace agentenv;
using namespace agentenv::image;

namespace {

template <typename T>
class ScriptedStream : public BuildkitStream<T> {
 public:
    typedef core::Expected<core::Optional<T>, BuildkitRpcStatus> Item;
    void Push(const T& value) { items_.push_back(Item(core::Optional<T>(value))); }
    void Fail(const BuildkitRpcStatus& status) {
        items_.push_back(Item(core::make_unexpected(status)));
    }
    Item Next() override {
        if (cursor_ >= items_.size()) return Item(core::Optional<T>());
        return items_[cursor_++];
    }

 private:
    std::vector<Item> items_;
    std::size_t cursor_ = 0;
};

class Store : public BuildkitContentWire {
 public:
    explicit Store(const std::vector<std::pair<int64_t, std::string> >& chunks) : chunks_(chunks) {}
    core::Expected<std::shared_ptr<BuildkitStream<ReadContentResponse> >, BuildkitRpcStatus> Read(
        const ReadContentRequest& request) override {
        MT_EXPECT_EQ(request.offset, int64_t(0));
        MT_EXPECT_EQ(request.size, int64_t(0));
        std::shared_ptr<ScriptedStream<ReadContentResponse> > stream(
            new ScriptedStream<ReadContentResponse>());
        for (std::size_t i = 0; i < chunks_.size(); ++i) {
            ReadContentResponse chunk;
            chunk.offset = chunks_[i].first;
            chunk.data = chunks_[i].second;
            stream->Push(chunk);
        }
        return std::shared_ptr<BuildkitStream<ReadContentResponse> >(stream);
    }

 private:
    std::vector<std::pair<int64_t, std::string> > chunks_;
};

BuildkitContent StoreOf(const std::vector<std::pair<int64_t, std::string> >& chunks) {
    return BuildkitContent(std::shared_ptr<BuildkitContentWire>(new Store(chunks)));
}

std::vector<std::pair<int64_t, std::string> > Chunks(
    std::initializer_list<std::pair<int64_t, std::string> > list) {
    return std::vector<std::pair<int64_t, std::string> >(list);
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

}  // namespace

MT_TEST(reads_verified_chunks_and_rejects_corrupt_truncated_or_oversized_content) {
    const std::string digest = core::Sha256Digest(std::string("abcdef"));
    BuildkitContent client = StoreOf(Chunks({{0, "abc"}, {3, "def"}}));
    auto metadata = client.Metadata(digest);
    MT_EXPECT_TRUE(metadata.ok());
    MT_EXPECT_EQ(metadata.value(), std::string("abcdef"));

    auto work = core::fs::CreateTempDir("aenv-buildkit-");
    MT_EXPECT_TRUE(work.ok());
    const std::string blob = work.value() + "/blob";
    MT_EXPECT_TRUE(client.Download(digest, 6, blob).ok());
    MT_EXPECT_EQ(core::fs::ReadToString(blob).value(), std::string("abcdef"));

    auto short_limit = client.Download(digest, 5, blob);
    MT_EXPECT_TRUE(!short_limit.ok());
    MT_EXPECT_TRUE(Contains(short_limit.error(), "byte limit"));
    auto long_size = client.Download(digest, 7, blob);
    MT_EXPECT_TRUE(!long_size.ok());
    MT_EXPECT_TRUE(Contains(long_size.error(), "size mismatch"));
    auto wrong = client.Metadata(core::Sha256Digest(std::string("wrong")));
    MT_EXPECT_TRUE(!wrong.ok());
    MT_EXPECT_TRUE(Contains(wrong.error(), "digest mismatch"));
    core::fs::RemoveDirAll(work.value());

    const std::vector<std::pair<int64_t, std::string> > corrupt[] = {
        Chunks({{0, ""}, {0, "abcdef"}}),
        Chunks({{0, "abc"}}),
        Chunks({{1, "abcdef"}}),
        Chunks({{-1, "abcdef"}}),
        Chunks({{0, "abc"}, {4, "def"}}),
    };
    for (std::size_t i = 0; i < sizeof(corrupt) / sizeof(corrupt[0]); ++i) {
        MT_EXPECT_TRUE(!StoreOf(corrupt[i]).Metadata(digest).ok());
    }
}

MT_TEST(download_rejects_layers_over_the_image_limit_before_touching_disk) {
    const std::string digest = core::Sha256Digest(std::string("x"));
    const std::string path = "/nonexistent-dir/blob";
    auto result = StoreOf(Chunks({})).Download(digest, kMaxImageBytes + 1, path);
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(Contains(result.error(), "exceeds 64 GiB"));
}

MT_TEST(content_digests_are_canonical_and_cannot_contain_paths) {
    std::string hex;
    for (int i = 0; i < 32; ++i) hex += "ab";
    MT_EXPECT_TRUE(ValidateDigest("sha256:" + hex).ok());
    std::string upper;
    for (int i = 0; i < 32; ++i) upper += "AB";
    const std::string bad[] = {"sha256:../file", "sha512:abc", "sha256:", "sha256:" + upper};
    for (std::size_t i = 0; i < 4; ++i) MT_EXPECT_TRUE(!ValidateDigest(bad[i]).ok());
}

MT_TEST(vertex_progress_does_not_regress_or_repeat_replayed_states) {
    const BuildkitTimestamp ts(1, 0);
    BuildkitVertexProgress progress;
    BuildkitVertex completed;
    completed.digest = "vertex";
    completed.name = "load .dockerignore";
    completed.started = core::Optional<BuildkitTimestamp>(ts);
    completed.completed = core::Optional<BuildkitTimestamp>(ts);

    auto first = VertexUpdates(completed, &progress);
    MT_EXPECT_EQ(static_cast<int>(first.size()), 2);
    MT_EXPECT_TRUE(first[0].first == core::LogLevel::Info);
    MT_EXPECT_EQ(first[0].second, std::string("started"));
    MT_EXPECT_EQ(first[1].second, std::string("completed"));

    BuildkitVertex replayed_start = completed;
    replayed_start.completed = core::Optional<BuildkitTimestamp>();
    MT_EXPECT_TRUE(VertexUpdates(replayed_start, &progress).empty());
    MT_EXPECT_TRUE(VertexUpdates(completed, &progress).empty());
}

namespace {

BuildkitHistoryEvent CompletedEvent(const std::string& name) {
    BuildkitExporter exporter;
    exporter.type = "image";
    exporter.attrs["name"] = name;
    BuildkitResultInfo result;
    result.results[0] = core::Sha256Digest(std::string("image"));
    BuildkitHistoryRecord record;
    record.ref = "solve-id";
    record.exporters.push_back(exporter);
    record.result = core::Optional<BuildkitResultInfo>(result);
    BuildkitHistoryEvent event;
    event.type = 1;
    event.record = core::Optional<BuildkitHistoryRecord>(record);
    return event;
}

}  // namespace

MT_TEST(only_successful_completion_of_the_expected_image_is_published) {
    const std::string name = BuildImageName("current");
    const std::string image_digest = core::Sha256Digest(std::string("image"));

    auto cached = CompletedImage(CompletedEvent("aenv-build:cached"), name);
    MT_EXPECT_TRUE(cached.ok() && !cached.value().has_value());

    const int kinds[] = {0, 2};
    for (int i = 0; i < 2; ++i) {
        BuildkitHistoryEvent event = CompletedEvent(name);
        event.type = kinds[i];
        auto result = CompletedImage(event, name);
        MT_EXPECT_TRUE(result.ok() && !result.value().has_value());
    }

    BuildkitHistoryEvent failed = CompletedEvent(name);
    BuildkitBuildError build_error;
    build_error.code = 2;
    build_error.message = "RUN failed";
    failed.record->error = core::Optional<BuildkitBuildError>(build_error);
    auto failure = CompletedImage(failed, name);
    MT_EXPECT_TRUE(!failure.ok());
    MT_EXPECT_TRUE(Contains(failure.error(), "RUN failed"));

    // No descriptor, and an invalid one.
    {
        BuildkitHistoryEvent event = CompletedEvent(name);
        event.record->result->results.clear();
        MT_EXPECT_TRUE(!CompletedImage(event, name).ok());
        event.record->result->results[0] = "sha256:invalid";
        MT_EXPECT_TRUE(!CompletedImage(event, name).ok());
    }

    // The descriptor is looked up by the image exporter's own index.
    {
        BuildkitHistoryEvent event = CompletedEvent(name);
        BuildkitExporter local;
        local.type = "local";
        event.record->exporters.insert(event.record->exporters.begin(), local);
        event.record->result->results[1] = image_digest;
        event.record->result->results[0] = core::Sha256Digest(std::string("other"));
        auto result = CompletedImage(event, name);
        MT_EXPECT_TRUE(result.ok() && result.value().has_value());
        MT_EXPECT_EQ(*result.value(), image_digest);
    }

    auto ok = CompletedImage(CompletedEvent(name), name);
    MT_EXPECT_TRUE(ok.ok() && ok.value().has_value());
    MT_EXPECT_EQ(*ok.value(), image_digest);
}

namespace {

class Entries : public core::BuildLogSink {
 public:
    void Log(core::LogLevel level, const std::string*, const std::string& message) override {
        std::lock_guard<std::mutex> lock(mu_);
        entries_.push_back(std::make_pair(level, message));
    }
    std::vector<std::pair<core::LogLevel, std::string> > Snapshot() {
        std::lock_guard<std::mutex> lock(mu_);
        return entries_;
    }

 private:
    std::mutex mu_;
    std::vector<std::pair<core::LogLevel, std::string> > entries_;
};

class History : public BuildkitControlWire {
 public:
    std::atomic<int> calls{0};
    std::atomic<int> status_calls{0};

    core::Expected<std::shared_ptr<BuildkitStream<BuildkitStatusResponse> >, BuildkitRpcStatus>
    Status(const std::string& ref) override {
        MT_EXPECT_EQ(ref, std::string("solve-id"));
        BuildkitVertexLog output;
        output.vertex = "vertex";
        output.timestamp = core::Optional<BuildkitTimestamp>(BuildkitTimestamp(1, 2));
        output.stream = 2;
        output.msg = "output\n";
        BuildkitStatusResponse first;
        first.logs.push_back(output);
        first.logs.push_back(output);

        std::shared_ptr<ScriptedStream<BuildkitStatusResponse> > stream(
            new ScriptedStream<BuildkitStatusResponse>());
        stream->Push(first);
        if (status_calls.fetch_add(1) == 0) {
            stream->Fail(BuildkitRpcStatus(BuildkitRpcStatus::kUnavailable, "status disconnected"));
        } else {
            BuildkitVertexLog last;
            last.vertex = "vertex";
            last.timestamp = core::Optional<BuildkitTimestamp>(BuildkitTimestamp(2, 0));
            last.stream = 1;
            last.msg = "last\n";
            BuildkitStatusResponse second;
            second.logs.push_back(last);
            stream->Push(second);
        }
        return std::shared_ptr<BuildkitStream<BuildkitStatusResponse> >(stream);
    }

    core::Expected<std::shared_ptr<BuildkitStream<BuildkitHistoryEvent> >, BuildkitRpcStatus>
    ListenBuildHistory(const BuildkitHistoryRequest& request) override {
        MT_EXPECT_TRUE(!request.active_only);
        MT_EXPECT_TRUE(!request.early_exit);
        std::shared_ptr<ScriptedStream<BuildkitHistoryEvent> > stream(
            new ScriptedStream<BuildkitHistoryEvent>());
        stream->Push(CompletedEvent("aenv-build:cached"));
        if (calls.fetch_add(1) == 0) {
            stream->Fail(BuildkitRpcStatus(BuildkitRpcStatus::kUnavailable, "connection lost"));
        } else {
            stream->Push(CompletedEvent(BuildImageName("current")));
        }
        return std::shared_ptr<BuildkitStream<BuildkitHistoryEvent> >(stream);
    }
};

/// A subscription that never produces anything until told to stop.
class Silent : public BuildkitControlWire {
 public:
    core::Expected<std::shared_ptr<BuildkitStream<BuildkitStatusResponse> >, BuildkitRpcStatus>
    Status(const std::string&) override {
        return core::make_unexpected(BuildkitRpcStatus(BuildkitRpcStatus::kUnavailable, "down"));
    }
    core::Expected<std::shared_ptr<BuildkitStream<BuildkitHistoryEvent> >, BuildkitRpcStatus>
    ListenBuildHistory(const BuildkitHistoryRequest&) override {
        return core::make_unexpected(BuildkitRpcStatus(BuildkitRpcStatus::kUnavailable, "down"));
    }
};

class NonRetryable : public Silent {
 public:
    core::Expected<std::shared_ptr<BuildkitStream<BuildkitHistoryEvent> >, BuildkitRpcStatus>
    ListenBuildHistory(const BuildkitHistoryRequest&) override {
        return core::make_unexpected(
            BuildkitRpcStatus(BuildkitRpcStatus::kOther, "permission denied"));
    }
};

}  // namespace

MT_TEST(reconnect_replays_completion_without_a_client_submission) {
    std::shared_ptr<History> wire(new History());
    BuildkitHistory client{std::shared_ptr<BuildkitControlWire>(wire)};
    std::shared_ptr<Entries> entries(new Entries());
    core::BuildLogger logger{std::shared_ptr<core::BuildLogSink>(entries)};

    auto result = client.WaitForImage("current", logger, core::CancelToken());
    MT_EXPECT_TRUE(result.ok());
    MT_EXPECT_EQ(result.value(), core::Sha256Digest(std::string("image")));

    const auto logged = entries->Snapshot();
    MT_EXPECT_EQ(static_cast<int>(logged.size()), 3);
    MT_EXPECT_TRUE(logged[0].first == core::LogLevel::Warn);
    MT_EXPECT_EQ(logged[0].second, std::string("output"));
    MT_EXPECT_TRUE(logged[1].first == core::LogLevel::Warn);
    MT_EXPECT_EQ(logged[1].second, std::string("output"));
    MT_EXPECT_TRUE(logged[2].first == core::LogLevel::Info);
    MT_EXPECT_EQ(logged[2].second, std::string("last"));
    MT_EXPECT_EQ(wire->status_calls.load(), 2);
    MT_EXPECT_EQ(wire->calls.load(), 2);
}

MT_TEST(wait_for_image_reconnect_is_bounded_by_cancellation) {
    BuildkitHistory client{std::shared_ptr<BuildkitControlWire>(new Silent())};
    core::CancelToken cancel;
    std::thread canceller([cancel]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cancel.Cancel();
    });
    auto result = client.WaitForImage("current", core::BuildLogger(), cancel);
    canceller.join();
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(Contains(result.error(), "cancelled"));
}

// Only Unavailable, Cancelled and DeadlineExceeded reconnect.
MT_TEST(wait_for_image_fails_fast_on_other_rpc_errors) {
    BuildkitHistory client{std::shared_ptr<BuildkitControlWire>(new NonRetryable())};
    auto result = client.WaitForImage("current", core::BuildLogger(), core::CancelToken());
    MT_EXPECT_TRUE(!result.ok());
    MT_EXPECT_TRUE(Contains(result.error(), "permission denied"));
}

MT_TEST(connect_without_a_transport_is_an_explicit_error) {
    auto content = BuildkitContent::Connect("127.0.0.1", kBuildkitPort);
    MT_EXPECT_TRUE(!content.ok());
    MT_EXPECT_TRUE(Contains(content.error(), "connect to builder content store"));
    auto history = BuildkitHistory::Connect("127.0.0.1", kBuildkitPort);
    MT_EXPECT_TRUE(!history.ok());
    MT_EXPECT_TRUE(Contains(history.error(), "connect to builder history"));
}

MT_TEST(build_logger_output_splits_lines_like_str_lines) {
    std::shared_ptr<Entries> entries(new Entries());
    core::BuildLogger logger{std::shared_ptr<core::BuildLogSink>(entries)};
    logger.Output(0, false, std::string("a\r\nb\n\nc"));
    const auto logged = entries->Snapshot();
    MT_EXPECT_EQ(static_cast<int>(logged.size()), 4);
    MT_EXPECT_EQ(logged[0].second, std::string("a"));
    MT_EXPECT_EQ(logged[1].second, std::string("b"));
    MT_EXPECT_EQ(logged[2].second, std::string(""));
    MT_EXPECT_EQ(logged[3].second, std::string("c"));
    core::BuildLogger().Output(0, true, std::string("nowhere\n"));
}

MT_TEST(from_utf8_lossy_matches_rust_replacement_rules) {
    const std::string fffd = "\xEF\xBF\xBD";
    MT_EXPECT_EQ(core::FromUtf8Lossy("h\xC3\xA9llo"), std::string("h\xC3\xA9llo"));
    MT_EXPECT_EQ(core::FromUtf8Lossy(std::string("a\xFF" "b")), "a" + fffd + "b");
    // A truncated sequence is one replacement, not one per byte.
    MT_EXPECT_EQ(core::FromUtf8Lossy(std::string("a\xE2\x82")), "a" + fffd);
    // Overlong and surrogate encodings are invalid at their second byte.
    MT_EXPECT_EQ(core::FromUtf8Lossy(std::string("\xC0\x80")), fffd + fffd);
    MT_EXPECT_EQ(core::FromUtf8Lossy(std::string("\xED\xA0\x80")), fffd + fffd + fffd);
}

MT_TEST(trim_unicode_whitespace_matches_rust_str_trim) {
    MT_EXPECT_EQ(core::TrimUnicodeWhitespace(" \t\r\n\v\fabc \n"), std::string("abc"));
    // U+00A0, U+0085, U+3000 and U+2009 are Unicode White_Space.
    MT_EXPECT_EQ(core::TrimUnicodeWhitespace("\xC2\xA0\xC2\x85x y\xE3\x80\x80\xE2\x80\x89"),
                 std::string("x y"));
    // U+200B (zero width space) is not White_Space and survives.
    MT_EXPECT_EQ(core::TrimUnicodeWhitespace("\xE2\x80\x8Bz"), std::string("\xE2\x80\x8Bz"));
    MT_EXPECT_EQ(core::TrimUnicodeWhitespace("   "), std::string());
    MT_EXPECT_EQ(core::TrimUnicodeWhitespace(""), std::string());
}

MT_MAIN
