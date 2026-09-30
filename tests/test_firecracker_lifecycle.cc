// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/{process_vm_reader,pool,startup_pack,config}.rs
//
// `process_vm_readv` is exercised against a real forked child rather than
// mocked: the short-read loop and the HVA-vs-offset contract are exactly the
// things a mock cannot get wrong for you.
#include "agentenv/sandbox/firecracker/lifecycle.h"
#include "agentenv/sandbox/firecracker/process_vm.h"

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#include "agentenv/core/fs.h"
#include "agentenv/sandbox/extra_drive.h"
#include "agentenv/sandbox/firecracker/config.h"
#include "microtest.h"

using namespace agentenv;                        // NOLINT
using namespace agentenv::sandbox::firecracker;  // NOLINT

namespace {

struct TempRoot {
    std::string path;
    TempRoot() {
        const core::Expected<std::string, std::string> dir =
            core::fs::CreateTempDir("agentenv-fc-life-");
        path = dir.ok() ? dir.value() : std::string("/tmp/agentenv-fc-life-fallback");
        core::fs::CreateDirAll(path);
    }
    ~TempRoot() { core::fs::RemoveDirAll(path); }
};

/// A child process holding a known pattern in shared memory, so the parent can
/// read it back by host virtual address.
class MemoryDonor {
 public:
    explicit MemoryDonor(std::size_t bytes) : bytes_(bytes), pid_(-1), buffer_(NULL) {
        // Shared so the parent knows the address the child is using; the read
        // itself still goes through process_vm_readv against the child's pid.
        buffer_ = static_cast<unsigned char*>(::mmap(NULL, bytes_, PROT_READ | PROT_WRITE,
                                                     MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        MT_EXPECT_TRUE(buffer_ != MAP_FAILED);
        for (std::size_t i = 0; i < bytes_; ++i) {
            buffer_[i] = static_cast<unsigned char>(i & 0xFF);
        }

        stop_pipe_[0] = -1;
        stop_pipe_[1] = -1;
        if (::pipe(stop_pipe_) != 0) return;
        pid_ = ::fork();
        if (pid_ == 0) {
            // Child: hold the mapping until the parent closes the write end.
            ::close(stop_pipe_[1]);
            char sink;
            while (::read(stop_pipe_[0], &sink, 1) < 0) {
            }
            ::_exit(0);
        }
        // The write end stays open on purpose: closing it here would signal
        // EOF and the child would exit before it could be read from, making
        // every read race against the child's lifetime.
        ::close(stop_pipe_[0]);
        stop_pipe_[0] = -1;
    }

    ~MemoryDonor() {
        // Closing the write end is what tells the child to exit.
        if (stop_pipe_[1] >= 0) ::close(stop_pipe_[1]);
        if (pid_ > 0) {
            int status = 0;
            ::waitpid(pid_, &status, 0);
        }
        if (buffer_ != NULL && buffer_ != MAP_FAILED) ::munmap(buffer_, bytes_);
    }

    int64_t  pid() const { return pid_; }
    uint64_t address() const { return reinterpret_cast<uint64_t>(buffer_); }
    bool     ok() const { return pid_ > 0 && buffer_ != MAP_FAILED; }

 private:
    std::size_t    bytes_;
    pid_t          pid_;
    unsigned char* buffer_;
    int            stop_pipe_[2];
};

sandbox::firecracker::ExtraDriveSpec Drive(const std::string& id, const std::string& mount,
                                           const std::string& image) {
    sandbox::firecracker::ExtraDriveSpec spec;
    spec.drive_id          = id;
    spec.mount_path        = mount;
    spec.image_config_path = image;
    return spec;
}

std::vector<std::string> Failures(const std::string& a) {
    std::vector<std::string> out;
    out.push_back(a);
    return out;
}

std::vector<std::string> Failures(const std::string& a, const std::string& b) {
    std::vector<std::string> out;
    out.push_back(a);
    out.push_back(b);
    return out;
}

}  // namespace

// ---- process_vm_readv ------------------------------------------------------

MT_TEST(process_vm_reads_a_remote_pattern) {
    MemoryDonor donor(8192);
    MT_EXPECT_TRUE(donor.ok());

    const ProcessVmReader reader(donor.pid());
    const core::Expected<std::string, std::string> data = reader.ReadAt(donor.address(), 4096);
    MT_EXPECT_TRUE(data.ok());
    MT_EXPECT_EQ(data.value().size(), static_cast<std::size_t>(4096));
    // The offset is a host virtual address, not a file offset.
    for (std::size_t i = 0; i < 4096; ++i) {
        MT_EXPECT_EQ(static_cast<unsigned char>(data.value()[i]),
                     static_cast<unsigned char>(i & 0xFF));
    }
}

MT_TEST(process_vm_reads_from_a_nonzero_offset) {
    MemoryDonor donor(8192);
    MT_EXPECT_TRUE(donor.ok());

    const ProcessVmReader reader(donor.pid());
    // Reading mid-buffer must land on the pattern at that address, which is
    // what proves the address is used verbatim rather than as a base.
    const core::Expected<std::string, std::string> data =
        reader.ReadAt(donor.address() + 256, 16);
    MT_EXPECT_TRUE(data.ok());
    for (std::size_t i = 0; i < 16; ++i) {
        MT_EXPECT_EQ(static_cast<unsigned char>(data.value()[i]),
                     static_cast<unsigned char>((256 + i) & 0xFF));
    }
}

MT_TEST(process_vm_fills_a_large_buffer_completely) {
    // Large enough that a short read is plausible; the loop must still fill
    // every byte, because a partial read left as zeros is indistinguishable
    // from genuinely zeroed guest memory.
    const std::size_t bytes = 1024 * 1024;
    MemoryDonor donor(bytes);
    MT_EXPECT_TRUE(donor.ok());

    std::vector<unsigned char> sink(bytes, 0xEE);
    const ProcessVmReader reader(donor.pid());
    MT_EXPECT_TRUE(reader.ReadExactRemote(donor.address(), &sink[0], bytes).ok());
    for (std::size_t i = 0; i < bytes; i += 4093) {  // prime stride
        MT_EXPECT_EQ(sink[i], static_cast<unsigned char>(i & 0xFF));
    }
}

MT_TEST(process_vm_zero_length_read_is_a_noop) {
    const ProcessVmReader reader(::getpid());
    MT_EXPECT_TRUE(reader.ReadExactRemote(0, NULL, 0).ok());
    MT_EXPECT_TRUE(reader.ReadAt(0, 0).ok());
}

MT_TEST(process_vm_rejects_a_null_destination) {
    const ProcessVmReader reader(::getpid());
    MT_EXPECT_TRUE(!reader.ReadExactRemote(0x1000, NULL, 16).ok());
}

MT_TEST(process_vm_reports_an_unreadable_address) {
    MemoryDonor donor(4096);
    MT_EXPECT_TRUE(donor.ok());

    const ProcessVmReader reader(donor.pid());
    // Page zero is never mapped; the error must name the pid and the address
    // so an operator can tell which VM and which range failed.
    const core::Expected<std::string, std::string> data = reader.ReadAt(0, 16);
    MT_EXPECT_TRUE(!data.ok());
    MT_EXPECT_TRUE(data.error().find("process_vm_readv") != std::string::npos);
}

MT_TEST(process_vm_reports_a_dead_process) {
    const ProcessVmReader reader(0x7FFFFFFF);  // no such pid
    MT_EXPECT_TRUE(!reader.ReadAt(0x1000, 16).ok());
}

// ---- warm stdio paths ------------------------------------------------------

MT_TEST(lifecycle_warm_stdio_paths_are_both_or_neither) {
    core::Optional<std::string> out;
    core::Optional<std::string> err;

    WarmStdioPaths("/var/run/fc", true, &out, &err);
    MT_EXPECT_TRUE(out.has_value());
    MT_EXPECT_TRUE(err.has_value());
    MT_EXPECT_EQ(*out, std::string("/var/run/fc/firecracker-stdout.log"));
    MT_EXPECT_EQ(*err, std::string("/var/run/fc/firecracker-stderr.log"));

    // Returning one of the two would silently drop half the diagnostics.
    WarmStdioPaths("/var/run/fc", false, &out, &err);
    MT_EXPECT_TRUE(!out.has_value());
    MT_EXPECT_TRUE(!err.has_value());
}

// ---- pool cleanup aggregation ---------------------------------------------

MT_TEST(lifecycle_pool_cleanup_reports_every_failure) {
    MT_EXPECT_TRUE(FirecrackerPoolCleanupResult(std::vector<std::string>()).ok());

    const core::Expected<core::Unit, std::string> one =
        FirecrackerPoolCleanupResult(Failures("slot 1 stuck"));
    MT_EXPECT_TRUE(!one.ok());
    MT_EXPECT_TRUE(one.error().find("slot 1 stuck") != std::string::npos);

    // Both, joined: an operator needs to see every leaked entry, not just the
    // first one encountered.
    const core::Expected<core::Unit, std::string> two =
        FirecrackerPoolCleanupResult(Failures("slot 1 stuck", "slot 2 busy"));
    MT_EXPECT_TRUE(!two.ok());
    MT_EXPECT_TRUE(two.error().find("slot 1 stuck | slot 2 busy") != std::string::npos);
}

// ---- logging / recording predicates ---------------------------------------

MT_TEST(lifecycle_logging_enabled_treats_blank_as_off) {
    MT_EXPECT_TRUE(LoggingEnabled(core::Optional<std::string>(std::string("Info"))));
    MT_EXPECT_TRUE(!LoggingEnabled(core::Optional<std::string>()));
    // A blank config entry must not create an empty log file.
    MT_EXPECT_TRUE(!LoggingEnabled(core::Optional<std::string>(std::string(""))));
    MT_EXPECT_TRUE(!LoggingEnabled(core::Optional<std::string>(std::string("   "))));
    MT_EXPECT_TRUE(!LoggingEnabled(core::Optional<std::string>(std::string("\t\n"))));
}

MT_TEST(lifecycle_startup_pack_recording_needs_both_conditions) {
    MT_EXPECT_TRUE(StartupPackRecordingEnabled(true, true));
    // A POSIX backend resolves memory layers to plain file paths, so there is
    // no remote request chain for the pack to absorb.
    MT_EXPECT_TRUE(!StartupPackRecordingEnabled(true, false));
    MT_EXPECT_TRUE(!StartupPackRecordingEnabled(false, true));
    MT_EXPECT_TRUE(!StartupPackRecordingEnabled(false, false));
}

// ---- work dir --------------------------------------------------------------

MT_TEST(lifecycle_work_dir_without_a_parent_uses_tmp) {
    const core::Expected<std::string, std::string> dir =
        CreateFirecrackerWorkDir(core::Optional<std::string>());
    MT_EXPECT_TRUE(dir.ok());
    MT_EXPECT_TRUE(core::fs::IsDir(dir.value()));
    // The prefix is what identifies a leaked work directory as ours.
    MT_EXPECT_TRUE(dir.value().find("agentenv-fc-") != std::string::npos);
    core::fs::RemoveDirAll(dir.value());
}

MT_TEST(lifecycle_work_dir_creates_a_missing_parent) {
    TempRoot root;
    // A configured pool directory may not exist yet on a fresh node.
    const std::string parent = root.path + "/pool/nested";
    const core::Expected<std::string, std::string> dir =
        CreateFirecrackerWorkDir(core::Optional<std::string>(parent));
    MT_EXPECT_TRUE(dir.ok());
    MT_EXPECT_TRUE(core::fs::IsDir(dir.value()));
    // Inside the requested parent: putting it elsewhere would turn a later
    // hard-link into a cross-filesystem copy.
    MT_EXPECT_TRUE(dir.value().find(parent) == 0);
}

// ---- extra drive validation (the overlap fix) -----------------------------

MT_TEST(fc_extra_drives_reject_overlapping_mount_paths) {
    std::vector<sandbox::firecracker::ExtraDriveSpec> drives;
    drives.push_back(Drive("a", "/mnt/data", "/tmp/a.json"));
    drives.push_back(Drive("b", "/mnt/data/inner", "/tmp/b.json"));

    // Distinct strings, but the nested drive would be shadowed inside the
    // guest and therefore unreachable. A duplicate-only check would let this
    // through.
    const core::Expected<core::Unit, std::string> checked =
        ValidateExtraDriveSet(drives, false);
    MT_EXPECT_TRUE(!checked.ok());
    MT_EXPECT_TRUE(checked.error().find("overlapping") != std::string::npos);
}

MT_TEST(fc_extra_drives_allow_sibling_mount_paths) {
    std::vector<sandbox::firecracker::ExtraDriveSpec> drives;
    // A string-prefix check would wrongly reject these: `/mnt/ab` is not
    // inside `/mnt/a`.
    drives.push_back(Drive("a", "/mnt/a", "/tmp/a.json"));
    drives.push_back(Drive("b", "/mnt/ab", "/tmp/b.json"));
    MT_EXPECT_TRUE(ValidateExtraDriveSet(drives, false).ok());
}

MT_TEST(fc_extra_drives_normalize_before_comparing) {
    std::vector<sandbox::firecracker::ExtraDriveSpec> drives;
    // The same mount point spelled two ways must not be accepted twice.
    drives.push_back(Drive("a", "/mnt/data", "/tmp/a.json"));
    drives.push_back(Drive("b", "//mnt///data/.", "/tmp/b.json"));
    MT_EXPECT_TRUE(!ValidateExtraDriveSet(drives, false).ok());
}

MT_TEST(fc_extra_drives_still_reject_duplicate_ids_and_bad_input) {
    std::vector<sandbox::firecracker::ExtraDriveSpec> dup_ids;
    dup_ids.push_back(Drive("a", "/mnt/one", "/tmp/a.json"));
    dup_ids.push_back(Drive("a", "/mnt/two", "/tmp/b.json"));
    MT_EXPECT_TRUE(!ValidateExtraDriveSet(dup_ids, false).ok());

    std::vector<sandbox::firecracker::ExtraDriveSpec> bad_id;
    bad_id.push_back(Drive("bad id", "/mnt/one", "/tmp/a.json"));
    MT_EXPECT_TRUE(!ValidateExtraDriveSet(bad_id, false).ok());

    std::vector<sandbox::firecracker::ExtraDriveSpec> relative;
    relative.push_back(Drive("a", "relative/path", "/tmp/a.json"));
    MT_EXPECT_TRUE(!ValidateExtraDriveSet(relative, false).ok());

    std::vector<sandbox::firecracker::ExtraDriveSpec> zero_size;
    zero_size.push_back(Drive("a", "/mnt/one", "/tmp/a.json"));
    zero_size[0].has_virtual_size = true;
    zero_size[0].virtual_size     = 0;
    MT_EXPECT_TRUE(!ValidateExtraDriveSet(zero_size, false).ok());
}

MT_TEST(fc_extra_drives_enforce_the_hardware_ceiling) {
    std::vector<sandbox::firecracker::ExtraDriveSpec> drives;
    // /dev/vdc..vdz is 24 slots; the 25th has nowhere to attach.
    for (int i = 0; i < 25; ++i) {
        char id[16];
        char mount[32];
        std::snprintf(id, sizeof(id), "d%d", i);
        std::snprintf(mount, sizeof(mount), "/mnt/d%d", i);
        drives.push_back(Drive(id, mount, "/tmp/a.json"));
    }
    const core::Expected<core::Unit, std::string> checked =
        ValidateExtraDriveSet(drives, false);
    MT_EXPECT_TRUE(!checked.ok());
    MT_EXPECT_TRUE(checked.error().find("too many extra drives") != std::string::npos);
}

// ---- shared overlap rule ---------------------------------------------------

MT_TEST(mount_path_overlap_rule_is_shared) {
    // The sandbox layer owns the rule so volume mounts and extra drives cannot
    // disagree about what overlaps.
    MT_EXPECT_TRUE(sandbox::MountPathsOverlap("/mnt/a", "/mnt/a/b"));
    MT_EXPECT_TRUE(sandbox::MountPathsOverlap("/", "/mnt/a"));
    MT_EXPECT_TRUE(!sandbox::MountPathsOverlap("/mnt/a", "/mnt/ab"));
    MT_EXPECT_TRUE(!sandbox::MountPathsOverlap("/data", "/database"));
}

int main() { return microtest::RunAll(); }
