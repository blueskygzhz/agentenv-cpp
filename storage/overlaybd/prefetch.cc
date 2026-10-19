// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/prefetch.rs
#include "agentenv/storage/overlaybd/prefetch.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>

#include "agentenv/storage/overlaybd/zfile.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

namespace {

void WriteU32Le(uint8_t* out, uint32_t v) {
    out[0] = static_cast<uint8_t>(v & 0xFF);
    out[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    out[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    out[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void WriteU64Le(uint8_t* out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

uint32_t ReadU32Le(const uint8_t* in) {
    return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

uint64_t ReadU64Le(const uint8_t* in) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | static_cast<uint64_t>(in[i]);
    return v;
}

/// Rust `TraceOp`.
///
/// `Write` is decodable but never produced: `PrefetchFile` passes writes
/// straight through without recording them, because replaying a write would
/// corrupt the layer it replayed onto. The variant exists so a trace written
/// by the C++ overlaybd, which does record writes, still loads.
enum TraceOp { kTraceRead, kTraceWrite };

uint8_t TraceOpAsByte(TraceOp op) { return op == kTraceRead ? 'R' : 'W'; }

bool TraceOpFromByte(uint8_t value, TraceOp* out) {
    if (value == 'R') {
        *out = kTraceRead;
        return true;
    }
    if (value == 'W') {
        *out = kTraceWrite;
        return true;
    }
    return false;
}

/// Rust `TraceRecord`. 24 bytes: op, 3 pad, layer u32, count u64, offset i64.
struct TraceRecord {
    TraceOp op;
    uint32_t layer_index;
    uint64_t count;
    /// Signed on the wire, matching the C++ overlaybd struct this format comes
    /// from. Only non-negative values are ever written, and a negative one is
    /// skipped on replay rather than rejected.
    int64_t offset;

    TraceRecord() : op(kTraceRead), layer_index(0), count(0), offset(0) {}
};

/// Rust `TraceRecord::new_read`.
bool MakeReadRecord(uint32_t layer_index, size_t count, uint64_t offset, TraceRecord* out) {
    // An offset past the signed range cannot be encoded, so the read is left
    // out of the trace instead of being written as a negative one.
    if (offset > static_cast<uint64_t>(INT64_MAX)) return false;
    out->op = kTraceRead;
    out->layer_index = layer_index;
    out->count = static_cast<uint64_t>(count);
    out->offset = static_cast<int64_t>(offset);
    return true;
}

void EncodeRecord(const TraceRecord& record, uint8_t* out) {
    std::memset(out, 0, kPrefetchTraceRecordSize);
    out[0] = TraceOpAsByte(record.op);
    WriteU32Le(out + 4, record.layer_index);
    WriteU64Le(out + 8, record.count);
    WriteU64Le(out + 16, static_cast<uint64_t>(record.offset));
}

bool DecodeRecord(const uint8_t* raw, TraceRecord* out) {
    if (!TraceOpFromByte(raw[0], &out->op)) return false;
    out->layer_index = ReadU32Le(raw + 4);
    out->count = ReadU64Le(raw + 8);
    out->offset = static_cast<int64_t>(ReadU64Le(raw + 16));
    return true;
}

/// Rust `TraceHeader`. 24 bytes: magic u32, 4 pad, data_size u64,
/// checksum u32, 4 pad.
struct TraceHeader {
    uint64_t data_size;
    uint32_t checksum;
};

void EncodeHeader(const TraceHeader& header, uint8_t* out) {
    std::memset(out, 0, kPrefetchTraceHeaderSize);
    WriteU32Le(out, kPrefetchTraceMagic);
    WriteU64Le(out + 8, header.data_size);
    WriteU32Le(out + 16, header.checksum);
}

core::Expected<TraceHeader, std::string> DecodeHeader(const uint8_t* raw, size_t len) {
    if (len != kPrefetchTraceHeaderSize) {
        return core::make_unexpected(std::string("invalid trace header size"));
    }
    if (ReadU32Le(raw) != kPrefetchTraceMagic) {
        return core::make_unexpected(std::string("prefetch trace magic mismatch"));
    }
    TraceHeader header;
    header.data_size = ReadU64Le(raw + 8);
    header.checksum = ReadU32Le(raw + 16);
    return header;
}

bool PathExists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

/// Create or truncate to zero length, which is how Rust's
/// `create(true).truncate(true).write(true)` is used here -- as a way to
/// *place a marker file*, not to write anything.
bool TouchTruncate(const std::string& path) {
    FILE* f = ::fopen(path.c_str(), "wb");
    if (f == 0) return false;
    ::fclose(f);
    return true;
}

}  // namespace

/// Rust `PrefetcherInner`.
struct PrefetcherInner {
    PrefetchMode mode;
    std::string trace_file_path;
    std::string lock_file_path;
    std::string ok_file_path;

    mutable std::mutex record_mu;
    std::vector<TraceRecord> record_array;

    mutable std::mutex replay_mu;
    std::deque<TraceRecord> replay_queue;

    mutable std::mutex src_mu;
    std::map<uint32_t, std::shared_ptr<VirtualFile> > src_files;

    std::atomic<bool> record_stopped;
    std::atomic<bool> replay_stopped;
    /// Guards the dump against running twice: the recorder thread and the
    /// destructor both try, and the second must not truncate the first's
    /// output.
    std::atomic<bool> dumped;
    std::atomic<bool> replay_started;
    size_t concurrency;

    PrefetcherInner(const std::string& path, PrefetchMode m, size_t workers)
        : mode(m),
          trace_file_path(path),
          lock_file_path(path + ".lock"),
          ok_file_path(path + ".ok"),
          record_stopped(false),
          replay_stopped(false),
          dumped(false),
          replay_started(false),
          concurrency(workers > 0 ? workers : 1) {}

    void RegisterSrcFile(uint32_t layer_index, const std::shared_ptr<VirtualFile>& src_file) {
        std::lock_guard<std::mutex> lk(src_mu);
        src_files[layer_index] = src_file;
    }

    void Record(const TraceRecord& record) {
        // Checked before taking the lock: once recording has stopped the
        // trace has been dumped, and a late read must not land in an array
        // nobody will write out.
        if (record_stopped.load()) return;
        std::lock_guard<std::mutex> lk(record_mu);
        record_array.push_back(record);
    }

    core::Expected<core::Unit, std::string> DumpRecords();

    static core::Expected<std::deque<TraceRecord>, std::string> LoadTraceFile(
        const std::string& path);
};

core::Expected<core::Unit, std::string> PrefetcherInner::DumpRecords() {
    if (mode != PrefetchMode::Record) return core::Unit();
    // Exchanged, not merely read: the recorder thread and the destructor race
    // here, and only one may write the file.
    if (dumped.exchange(true)) return core::Unit();

    std::vector<TraceRecord> records;
    {
        std::lock_guard<std::mutex> lk(record_mu);
        records = record_array;
    }

    const uint64_t data_size =
        static_cast<uint64_t>(records.size()) * kPrefetchTraceRecordSize;
    std::vector<uint8_t> payload(records.size() * kPrefetchTraceRecordSize);
    uint32_t checksum = 0;
    for (size_t i = 0; i < records.size(); ++i) {
        uint8_t* slot = &payload[i * kPrefetchTraceRecordSize];
        EncodeRecord(records[i], slot);
        // Chained while encoding, as Rust does, rather than a second pass over
        // the assembled payload.
        checksum = zfile::Crc32cAppend(checksum, slot, kPrefetchTraceRecordSize);
    }

    // The completion marker goes first, so a reader that sees `.ok` can trust
    // the trace beside it. Leaving a stale one during the rewrite would
    // advertise the previous boot's trace as describing this one.
    if (PathExists(ok_file_path)) ::unlink(ok_file_path.c_str());

    FILE* file = ::fopen(trace_file_path.c_str(), "w+b");
    if (file == 0) {
        return core::make_unexpected("open prefetch trace for write failed: " + trace_file_path);
    }

    // Written twice on purpose. The first header carries checksum 0, which no
    // non-empty trace can legitimately have, so a crash between here and the
    // rewrite leaves a file that fails verification instead of one that looks
    // valid and replays garbage. Rust carries a FIXME asking why the checksum
    // is not written directly; this is the reason, and changing it would
    // remove the only torn-write guard the format has.
    uint8_t header_bytes[kPrefetchTraceHeaderSize];
    TraceHeader header;
    header.data_size = data_size;
    header.checksum = 0;
    EncodeHeader(header, header_bytes);

    bool ok = ::fwrite(header_bytes, 1, kPrefetchTraceHeaderSize, file) ==
              kPrefetchTraceHeaderSize;
    if (ok && !payload.empty()) {
        ok = ::fwrite(&payload[0], 1, payload.size(), file) == payload.size();
    }
    if (ok) {
        header.checksum = checksum;
        EncodeHeader(header, header_bytes);
        ok = ::fseek(file, 0, SEEK_SET) == 0 &&
             ::fwrite(header_bytes, 1, kPrefetchTraceHeaderSize, file) ==
                 kPrefetchTraceHeaderSize;
    }
    if (ok) ok = ::fflush(file) == 0;
    ::fclose(file);
    if (!ok) {
        return core::make_unexpected("write prefetch trace failed: " + trace_file_path);
    }

    // Rust removes the lock file here even though the recorder only reaches
    // this point when the lock is already gone -- it also covers the
    // destructor path, where the lock is still present and would otherwise be
    // left behind to make the next boot think a recorder is live.
    ::unlink(lock_file_path.c_str());
    if (!TouchTruncate(ok_file_path)) {
        return core::make_unexpected("create prefetch trace ok marker failed: " + ok_file_path);
    }
    return core::Unit();
}

core::Expected<std::deque<TraceRecord>, std::string> PrefetcherInner::LoadTraceFile(
    const std::string& path) {
    FILE* file = ::fopen(path.c_str(), "rb");
    if (file == 0) {
        return core::make_unexpected("open prefetch trace failed: " + path);
    }
    std::vector<uint8_t> raw;
    uint8_t buffer[65536];
    for (;;) {
        const size_t got = ::fread(buffer, 1, sizeof(buffer), file);
        if (got > 0) raw.insert(raw.end(), buffer, buffer + got);
        if (got < sizeof(buffer)) break;
    }
    const bool read_failed = ::ferror(file) != 0;
    ::fclose(file);
    if (read_failed) {
        return core::make_unexpected("read prefetch trace failed: " + path);
    }

    if (raw.size() < kPrefetchTraceHeaderSize) {
        return core::make_unexpected(std::string("prefetch trace file is smaller than header"));
    }
    const core::Expected<TraceHeader, std::string> header =
        DecodeHeader(&raw[0], kPrefetchTraceHeaderSize);
    if (!header.ok()) return core::make_unexpected(header.error());

    // The declared size must match exactly, not merely fit: a file longer than
    // its header claims was appended to by something that did not update the
    // checksum, and a shorter one is truncated.
    const uint64_t expected_len = header.value().data_size + kPrefetchTraceHeaderSize;
    if (static_cast<uint64_t>(raw.size()) != expected_len) {
        return core::make_unexpected(std::string("prefetch trace file size mismatch"));
    }
    if (header.value().data_size % kPrefetchTraceRecordSize != 0) {
        return core::make_unexpected(
            std::string("prefetch trace payload is not record-aligned"));
    }

    uint32_t checksum = 0;
    std::deque<TraceRecord> queue;
    const size_t count =
        static_cast<size_t>(header.value().data_size / kPrefetchTraceRecordSize);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* slot =
            &raw[kPrefetchTraceHeaderSize + i * kPrefetchTraceRecordSize];
        checksum = zfile::Crc32cAppend(checksum, slot, kPrefetchTraceRecordSize);
        TraceRecord record;
        if (!DecodeRecord(slot, &record)) {
            std::ostringstream o;
            o << "invalid trace op byte: " << static_cast<unsigned>(slot[0]);
            return core::make_unexpected(o.str());
        }
        queue.push_back(record);
    }
    // Verified after decoding, so a corrupt trace is rejected rather than
    // partially replayed.
    if (checksum != header.value().checksum) {
        return core::make_unexpected(std::string("prefetch trace checksum mismatch"));
    }
    return queue;
}

namespace {

/// Rust `PrefetchFile` — a layer source that notes the reads passing through.
///
/// Records only *complete* reads. A short read is not reproducible: replaying
/// it would fetch a different span than the one the guest actually got, so it
/// is better left out of the trace than recorded wrong.
class PrefetchFile : public VirtualFile {
 public:
    PrefetchFile(const std::shared_ptr<VirtualFile>& src_file, uint32_t layer_index,
                 const std::shared_ptr<PrefetcherInner>& prefetcher)
        : src_file_(src_file), layer_index_(layer_index), prefetcher_(prefetcher) {}

    core::Expected<ByteBuffer, std::string> ReadAt(uint64_t offset, size_t len) {
        const core::Expected<ByteBuffer, std::string> data = src_file_->ReadAt(offset, len);
        if (!data.ok()) return data;
        NoteRead(data.value().size(), len, offset);
        return data;
    }

    core::Expected<size_t, std::string> ReadAtInto(uint64_t offset, uint8_t* dst, size_t len) {
        const core::Expected<size_t, std::string> n = src_file_->ReadAtInto(offset, dst, len);
        if (!n.ok()) return n;
        NoteRead(n.value(), len, offset);
        return n;
    }

    core::Expected<ByteBuffer, std::string> ReadAtWithCtx(const IoCtx& ctx, uint64_t offset,
                                                             size_t len) {
        // Forwarded with the context intact. Dropping it here would push the
        // whole stack below this wrapper back to synchronous I/O.
        const core::Expected<ByteBuffer, std::string> data =
            src_file_->ReadAtWithCtx(ctx, offset, len);
        if (!data.ok()) return data;
        NoteRead(data.value().size(), len, offset);
        return data;
    }

    core::Expected<size_t, std::string> ReadAtIntoWithCtx(const IoCtx& ctx, uint64_t offset,
                                                             uint8_t* dst, size_t len) {
        const core::Expected<size_t, std::string> n =
            src_file_->ReadAtIntoWithCtx(ctx, offset, dst, len);
        if (!n.ok()) return n;
        NoteRead(n.value(), len, offset);
        return n;
    }

    // Writes pass through unrecorded: a replayed write would overwrite the
    // layer it was replayed onto.
    core::Expected<size_t, std::string> WriteAt(uint64_t offset, const uint8_t* data,
                                                   size_t len) {
        return src_file_->WriteAt(offset, data, len);
    }

    core::Expected<size_t, std::string> WriteAtWithCtx(const IoCtx& ctx, uint64_t offset,
                                                          const uint8_t* data, size_t len) {
        return src_file_->WriteAtWithCtx(ctx, offset, data, len);
    }

    core::Expected<uint64_t, std::string> Size() { return src_file_->Size(); }

    core::Expected<core::Unit, std::string> Truncate(uint64_t size) {
        return src_file_->Truncate(size);
    }
    core::Expected<core::Unit, std::string> Sync() { return src_file_->Sync(); }
    core::Expected<core::Optional<uint64_t>, std::string> SeekData(uint64_t offset) {
        return src_file_->SeekData(offset);
    }
    core::Expected<core::Optional<uint64_t>, std::string> SeekHole(uint64_t offset) {
        return src_file_->SeekHole(offset);
    }
    core::Expected<core::Unit, std::string> EvictRange(uint64_t offset, uint64_t len) {
        return src_file_->EvictRange(offset, len);
    }
    core::Expected<core::Unit, std::string> EvictAll() { return src_file_->EvictAll(); }
    core::Expected<ByteBuffer, std::string> FGetXattr(const std::string& name) {
        return src_file_->FGetXattr(name);
    }
    core::Expected<std::vector<std::string>, std::string> FListXattr() {
        return src_file_->FListXattr();
    }
    core::Expected<core::Unit, std::string> FSetXattr(const std::string& name,
                                                         const uint8_t* value, size_t len,
                                                         int flags) {
        return src_file_->FSetXattr(name, value, len, flags);
    }
    core::Expected<core::Unit, std::string> FRemoveXattr(const std::string& name) {
        return src_file_->FRemoveXattr(name);
    }

    // `Discard` is deliberately not forwarded, matching Rust: the wrapper does
    // not implement it, so the base class's refusal stands. Punching a hole in
    // a layer being traced is not a prefetch operation.

 private:
    void NoteRead(size_t got, size_t wanted, uint64_t offset) {
        if (got != wanted) return;
        if (prefetcher_->mode != PrefetchMode::Record) return;
        TraceRecord record;
        // An unencodable offset drops the entry rather than failing the read:
        // the read itself succeeded, and the trace is best-effort.
        if (!MakeReadRecord(layer_index_, got, offset, &record)) return;
        prefetcher_->Record(record);
    }

    std::shared_ptr<VirtualFile> src_file_;
    uint32_t layer_index_;
    std::shared_ptr<PrefetcherInner> prefetcher_;
};

/// Rust `start_detect_lock_thread`'s body.
///
/// The lock file is the handshake: whoever asked for a recording removes it to
/// say "stop now and write the trace out". Polling a file rather than taking a
/// signal is what lets an unrelated process end the recording.
void DetectLockLoop(std::shared_ptr<PrefetcherInner> inner) {
    while (!inner->record_stopped.load()) {
        // Sleeps first, as Rust does, so a recording always lasts at least one
        // interval and a boot that removes the lock immediately still captures
        // its opening reads.
        std::this_thread::sleep_for(std::chrono::milliseconds(kPrefetchPollIntervalMs));
        if (inner->record_stopped.load()) break;
        if (!PathExists(inner->lock_file_path)) {
            inner->record_stopped.store(true);
            inner->DumpRecords();
            break;
        }
    }
}

/// Rust `replay_worker`.
void ReplayWorker(std::shared_ptr<PrefetcherInner> inner) {
    for (;;) {
        if (inner->replay_stopped.load()) return;

        TraceRecord record;
        {
            std::lock_guard<std::mutex> lk(inner->replay_mu);
            if (inner->replay_queue.empty()) return;
            record = inner->replay_queue.front();
            inner->replay_queue.pop_front();
        }

        std::shared_ptr<VirtualFile> src_file;
        {
            std::lock_guard<std::mutex> lk(inner->src_mu);
            const std::map<uint32_t, std::shared_ptr<VirtualFile> >::const_iterator found =
                inner->src_files.find(record.layer_index);
            if (found != inner->src_files.end()) src_file = found->second;
        }
        // Keeps draining rather than returning: a trace may name a layer this
        // boot does not have, and the remaining entries are still useful.
        if (!src_file) continue;

        if (record.op != kTraceRead) continue;
        if (record.offset < 0) continue;
        if (record.count > static_cast<uint64_t>(SIZE_MAX)) continue;

        // The result is discarded on purpose. A prefetch exists to populate a
        // cache; if it fails the foreground read will fetch the range itself,
        // so failing the replay would only add noise.
        src_file->ReadAt(static_cast<uint64_t>(record.offset),
                         static_cast<size_t>(record.count));
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Prefetcher
// ---------------------------------------------------------------------------

PrefetchMode Prefetcher::DetectMode(const std::string& trace_file_path) {
    struct stat st;
    if (::stat(trace_file_path.c_str(), &st) != 0) return PrefetchMode::Disabled;
    // Zero length is a request, not an absence: provisioning creates an empty
    // trace to ask this boot to record one.
    if (st.st_size == 0) return PrefetchMode::Record;
    return PrefetchMode::Replay;
}

PrefetchMode Prefetcher::Mode() const { return inner_->mode; }

size_t Prefetcher::RecordedCount() const {
    std::lock_guard<std::mutex> lk(inner_->record_mu);
    return inner_->record_array.size();
}

size_t Prefetcher::ReplayQueueSize() const {
    std::lock_guard<std::mutex> lk(inner_->replay_mu);
    return inner_->replay_queue.size();
}

std::shared_ptr<VirtualFile> Prefetcher::NewPrefetchFile(
    const std::shared_ptr<VirtualFile>& src_file, uint32_t layer_index) {
    // Registered only for replay. In record mode the workers never run, and
    // holding every layer's file alive would keep the whole stack open for the
    // life of the recording.
    if (inner_->mode == PrefetchMode::Replay) {
        inner_->RegisterSrcFile(layer_index, src_file);
    }
    return std::shared_ptr<VirtualFile>(new PrefetchFile(src_file, layer_index, inner_));
}

core::Expected<core::Unit, std::string> Prefetcher::Replay() {
    if (inner_->mode != PrefetchMode::Replay) return core::Unit();
    if (inner_->replay_started.exchange(true)) return core::Unit();

    // Nothing to replay, or nowhere to replay it to. The second case means the
    // caller has not wrapped any layer yet, which is a sequencing mistake
    // rather than an error: the trace simply goes unused.
    {
        std::lock_guard<std::mutex> lk(inner_->replay_mu);
        if (inner_->replay_queue.empty()) return core::Unit();
    }
    {
        std::lock_guard<std::mutex> lk(inner_->src_mu);
        if (inner_->src_files.empty()) return core::Unit();
    }

    // Deviation: Rust spawns one thread hosting a current-thread runtime with
    // `concurrency` tasks, which overlap because its reads are async. Reads
    // here block, so the same number of outstanding requests needs that many
    // OS threads. The observable effect -- `concurrency` reads in flight -- is
    // the same; the cost is `concurrency` short-lived threads.
    for (size_t i = 0; i < inner_->concurrency; ++i) {
        replay_threads_.push_back(
            std::shared_ptr<std::thread>(new std::thread(ReplayWorker, inner_)));
    }
    return core::Unit();
}

Prefetcher::~Prefetcher() {
    inner_->record_stopped.store(true);
    inner_->replay_stopped.store(true);

    if (detect_thread_ && detect_thread_->joinable()) detect_thread_->join();
    for (size_t i = 0; i < replay_threads_.size(); ++i) {
        if (replay_threads_[i]->joinable()) replay_threads_[i]->join();
    }

    // Last chance to persist a recording: the lock file may still exist, so
    // the detector never fired. The `dumped` flag makes this a no-op when it
    // already did.
    inner_->DumpRecords();
}

core::Expected<std::shared_ptr<Prefetcher>, std::string> NewPrefetcher(
    const std::string& trace_file_path, int32_t concurrency) {
    const PrefetchMode mode = Prefetcher::DetectMode(trace_file_path);
    const size_t workers =
        concurrency > 1 ? static_cast<size_t>(concurrency) : static_cast<size_t>(1);

    std::shared_ptr<PrefetcherInner> inner(
        new PrefetcherInner(trace_file_path, mode, workers));

    if (mode == PrefetchMode::Disabled) {
        // Refused rather than returning an inert prefetcher, so a wrong path
        // is reported at setup instead of costing every boot its prefetch
        // silently.
        return core::make_unexpected("prefetch trace `" + trace_file_path +
                                     "` does not exist");
    }

    std::shared_ptr<Prefetcher> prefetcher(new Prefetcher());
    prefetcher->inner_ = inner;

    if (mode == PrefetchMode::Record) {
        // Truncated even though it is already empty: `DetectMode` only saw a
        // zero-length file, and this is what claims it.
        if (!TouchTruncate(trace_file_path)) {
            return core::make_unexpected("create prefetch trace failed: " + trace_file_path);
        }
        // The lock file announces a live recorder. Its removal is how the
        // recording is ended from outside.
        if (!TouchTruncate(inner->lock_file_path)) {
            return core::make_unexpected("create prefetch trace lock failed: " +
                                         inner->lock_file_path);
        }
        prefetcher->detect_thread_ =
            std::shared_ptr<std::thread>(new std::thread(DetectLockLoop, inner));
        return prefetcher;
    }

    const core::Expected<std::deque<TraceRecord>, std::string> queue =
        PrefetcherInner::LoadTraceFile(trace_file_path);
    if (!queue.ok()) {
        // Rust attaches this context here. It reads oddly but is the message
        // operators have been seeing, so it is kept verbatim.
        return core::make_unexpected("dynamic prefetch list is not migrated in Rust yet: " +
                                     queue.error());
    }
    {
        std::lock_guard<std::mutex> lk(inner->replay_mu);
        inner->replay_queue = queue.value();
    }
    return prefetcher;
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
