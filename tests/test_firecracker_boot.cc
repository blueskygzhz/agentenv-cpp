// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/sandbox.rs :: FirecrackerSandbox::start_fresh
//   (spawn_with_netns -> wait_for_ready -> configure_microvm -> start)
//
// This is the first test that executes anything past the spawn. A fake
// connector stands in for the VMM, and `/bin/sh` stands in for the firecracker
// binary, so the whole sequence runs on a host with no KVM: the ordering, the
// abort-on-failure rule, and the cleanup that must happen when a later step
// fails are all otherwise unreachable without hardware.
#include "microtest.h"

#include <cerrno>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "agentenv/sandbox/firecracker/instance.h"
#include "agentenv/sandbox/firecracker/sandbox.h"

using namespace agentenv;
using namespace agentenv::sandbox;
using namespace agentenv::sandbox::firecracker;

namespace {

/// Records every request, and can be told to fail one of them.
class RecordingSocket : public Socket {
 public:
    struct Call {
        std::string method;
        std::string path;
        std::string body;
    };

    RecordingSocket(std::vector<Call>* log, const std::string& fail_path)
        : log_(log), fail_path_(fail_path) {}

    core::Expected<std::string, std::string> Request(const std::string& method,
                                                     const std::string& path,
                                                     const std::string& body) override {
        Call call;
        call.method = method;
        call.path = path;
        call.body = body;
        log_->push_back(call);
        if (!fail_path_.empty() && path == fail_path_) {
            return core::make_unexpected(std::string("HTTP 400: fault injected"));
        }
        return std::string();
    }

    std::future<core::Expected<std::string, std::string> >
    RequestAsync(const std::string& method, const std::string& path,
                 const std::string& body) override {
        std::promise<core::Expected<std::string, std::string> > p;
        p.set_value(Request(method, path, body));
        return p.get_future();
    }

 private:
    std::vector<Call>* log_;
    std::string fail_path_;
};

class RecordingConnector : public Connector {
 public:
    explicit RecordingConnector(const std::string& fail_path = std::string())
        : fail_path_(fail_path) {}

    core::Expected<std::shared_ptr<Socket>, std::string>
    Connect(const std::string& /*api_socket_path*/) override {
        return std::shared_ptr<Socket>(new RecordingSocket(&calls, fail_path_));
    }

    std::vector<RecordingSocket::Call> calls;

 private:
    std::string fail_path_;
};

/// Notes the spawned VMM's pid on first connect, so a test can ask whether the
/// process survived a failed boot. The pid is read from the work directory's
/// parent process list rather than returned by the API, because a failed Boot
/// hands back no handle.
class PidCapturingConnector : public Connector {
 public:
    explicit PidCapturingConnector(const std::string& fail_path)
        : vmm_pid(0), fail_path_(fail_path) {}

    core::Expected<std::shared_ptr<Socket>, std::string>
    Connect(const std::string& /*api_socket_path*/) override {
        if (vmm_pid == 0) vmm_pid = FindFakeVmmChild();
        return std::shared_ptr<Socket>(new RecordingSocket(&calls, fail_path_));
    }

    std::vector<RecordingSocket::Call> calls;
    pid_t vmm_pid;

 private:
    /// The only child of this test process is the stand-in VMM.
    static pid_t FindFakeVmmChild() {
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/task/%d/children",
                      static_cast<int>(::getpid()), static_cast<int>(::getpid()));
        std::ifstream f(path);
        pid_t pid = 0;
        if (f) f >> pid;
        return pid;
    }

    std::string fail_path_;
};

/// Counts `agentenv-fc-*` work directories under a chroot base. The work
/// directory is created immediately before the spawn, so its absence proves
/// nothing was started -- which an already-reaped child cannot show.
std::size_t CountWorkDirs(const std::string& chroot_base) {
    std::size_t n = 0;
    std::string cmd = "find " + chroot_base + " -maxdepth 3 -name 'agentenv-fc-*' -type d "
                      "2>/dev/null | wc -l";
    std::FILE* p = ::popen(cmd.c_str(), "r");
    if (p == NULL) return 0;
    if (std::fscanf(p, "%zu", &n) != 1) n = 0;
    ::pclose(p);
    return n;
}

std::string MakeTempDir() {
    char tmpl[] = "/tmp/aenv-boot-XXXXXX";
    char* p = ::mkdtemp(tmpl);
    return p ? std::string(p) : std::string();
}

void WriteFile(const std::string& path, const std::string& text) {
    std::ofstream f(path.c_str());
    f << text;
}

/// A plan whose paths all exist, so validation passes and the sequence runs.
LaunchPlan PlanIn(const std::string& dir) {
    WriteFile(dir + "/vmlinux", "not really a kernel");
    WriteFile(dir + "/rootfs.img", "not really a rootfs");
    LaunchPlan plan;
    plan.sandbox_id = core::SandboxId::Parse("sbx-boot-0001").value();
    plan.kernel_path = dir + "/vmlinux";
    plan.rootfs_path = dir + "/rootfs.img";
    plan.vcpus = 2;
    plan.mem_bytes = 1024LL * 1024 * 1024;
    return plan;
}

/// `/bin/sh -c 'sleep ...'` would need argv control the Spawn API does not
/// take, so the stand-in VMM is a binary that simply stays alive long enough
/// for the socket check: `sleep` itself.
Config ConfigFor(const std::string& chroot_base) {
    Config cfg;
    cfg.firecracker_bin = AGENTENV_FAKE_FIRECRACKER;
    cfg.chroot_base_dir = chroot_base;
    cfg.use_jailer = false;
    cfg.capture_output = true;
    cfg.socket_timeout_ms = 300;
    cfg.socket_poll_ms = 5;
    return cfg;
}

std::vector<std::string> PathsOf(const std::vector<RecordingSocket::Call>& calls) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < calls.size(); ++i) out.push_back(calls[i].path);
    return out;
}

}  // namespace

// ============================================================
// validation happens before anything is created
// ============================================================

MT_TEST(boot_rejects_a_missing_kernel_before_spawning) {
    const std::string dir = MakeTempDir();
    MT_EXPECT_TRUE(!dir.empty());
    if (dir.empty()) return;

    FirecrackerBackend backend(ConfigFor(dir + "/chroot"));
    RecordingConnector* conn = new RecordingConnector();
    backend.SetConnectorForTesting(std::shared_ptr<Connector>(conn));

    LaunchPlan plan = PlanIn(dir);
    plan.kernel_path = dir + "/does-not-exist";
    core::Expected<Handle, core::AnyError> r = backend.Boot(plan);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().chain().find("kernel image not found") != std::string::npos);
    // Nothing was spawned and no request was issued.
    MT_EXPECT_EQ(conn->calls.size(), static_cast<std::size_t>(0));
}

MT_TEST(boot_rejects_a_missing_firecracker_binary) {
    const std::string dir = MakeTempDir();
    if (dir.empty()) return;
    Config cfg = ConfigFor(dir + "/chroot");
    cfg.firecracker_bin = dir + "/no-such-binary";

    FirecrackerBackend backend(cfg);
    backend.SetConnectorForTesting(std::shared_ptr<Connector>(new RecordingConnector()));
    core::Expected<Handle, core::AnyError> r = backend.Boot(PlanIn(dir));
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().chain().find("firecracker binary not found") != std::string::npos);
}

MT_TEST(a_permanent_misconfiguration_is_caught_before_the_vmm_starts) {
    // More volumes than reserved slots can never be fixed later, because
    // Firecracker cannot add a virtio-block device after a snapshot restore.
    // Finding it after a VMM is running means killing it for something that
    // was knowable up front, so the sequence is built before the spawn.
    const std::string dir = MakeTempDir();
    if (dir.empty()) return;
    Config cfg = ConfigFor(dir + "/chroot");
    cfg.physical_extra_drive_count = 5;  // more than the (empty) prepared list

    FirecrackerBackend backend(cfg);
    RecordingConnector* conn = new RecordingConnector();
    backend.SetConnectorForTesting(std::shared_ptr<Connector>(conn));

    core::Expected<Handle, core::AnyError> r = backend.Boot(PlanIn(dir));
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().chain().find("physical extra drives") != std::string::npos);
    MT_EXPECT_EQ(conn->calls.size(), static_cast<std::size_t>(0));
    // No request having been issued is not enough: that holds even if the
    // sequence were built after the spawn, since the failure still precedes
    // the first call. What must be true is that no VMM was ever started, and
    // the only observable trace of one is the work directory it is spawned
    // into -- CreateFirecrackerWorkDir runs just before the spawn.
    MT_EXPECT_EQ(CountWorkDirs(dir + "/chroot"), static_cast<std::size_t>(0));
}

// ============================================================
// the full sequence, end to end
// ============================================================

MT_TEST(boot_issues_the_whole_sequence_in_order) {
    const std::string dir = MakeTempDir();
    if (dir.empty()) return;

    FirecrackerBackend backend(ConfigFor(dir + "/chroot"));
    RecordingConnector* conn = new RecordingConnector();
    backend.SetConnectorForTesting(std::shared_ptr<Connector>(conn));

    core::Expected<Handle, core::AnyError> r = backend.Boot(PlanIn(dir));
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) {
        std::printf("boot failed: %s\n", r.error().chain().c_str());
        return;
    }

    const std::vector<std::string> paths = PathsOf(conn->calls);
    // No logger (blank level), no cpu template, no network slot: what remains
    // is machine-config, boot-source, the two drives, and the balloon.
    MT_EXPECT_EQ(paths.size(), static_cast<std::size_t>(6));
    MT_EXPECT_EQ(paths[0], std::string("/machine-config"));
    MT_EXPECT_EQ(paths[1], std::string("/boot-source"));
    MT_EXPECT_EQ(paths[2], std::string("/drives/rootfs"));
    MT_EXPECT_EQ(paths[3], std::string("/drives/user_rootfs"));
    MT_EXPECT_EQ(paths[4], std::string("/balloon"));
    // InstanceStart comes last, after every configuration call.
    MT_EXPECT_EQ(paths[5], std::string("/actions"));
    MT_EXPECT_TRUE(conn->calls[5].body.find("InstanceStart") != std::string::npos);

    // The handle points at the API socket the VM is actually reachable on.
    MT_EXPECT_EQ(r.value().backend_kind, std::string("firecracker"));
    MT_EXPECT_TRUE(r.value().control_endpoint.find("firecracker.socket") != std::string::npos);
}

MT_TEST(machine_config_carries_the_plans_cpu_and_memory) {
    const std::string dir = MakeTempDir();
    if (dir.empty()) return;
    FirecrackerBackend backend(ConfigFor(dir + "/chroot"));
    RecordingConnector* conn = new RecordingConnector();
    backend.SetConnectorForTesting(std::shared_ptr<Connector>(conn));

    LaunchPlan plan = PlanIn(dir);
    plan.vcpus = 8;
    plan.mem_bytes = 2048LL * 1024 * 1024;
    core::Expected<Handle, core::AnyError> r = backend.Boot(plan);
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    // mem_bytes is converted to MiB, which is the unit the API takes.
    MT_EXPECT_TRUE(conn->calls[0].body.find("\"mem_size_mib\":2048") != std::string::npos);
    MT_EXPECT_TRUE(conn->calls[0].body.find("\"vcpu_count\":8") != std::string::npos);
}

MT_TEST(a_configured_log_level_adds_the_logger_first) {
    const std::string dir = MakeTempDir();
    if (dir.empty()) return;
    Config cfg = ConfigFor(dir + "/chroot");
    cfg.log_level = "Debug";

    FirecrackerBackend backend(cfg);
    RecordingConnector* conn = new RecordingConnector();
    backend.SetConnectorForTesting(std::shared_ptr<Connector>(conn));

    core::Expected<Handle, core::AnyError> r = backend.Boot(PlanIn(dir));
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    // The logger is accepted pre-boot and only once, so it precedes anything
    // that can fail -- a later failure is what the log was wanted for.
    MT_EXPECT_EQ(conn->calls[0].path, std::string("/logger"));
    MT_EXPECT_TRUE(conn->calls[0].body.find("\"level\":\"Debug\"") != std::string::npos);
}

MT_TEST(kernel_cmdline_reaches_the_boot_source) {
    const std::string dir = MakeTempDir();
    if (dir.empty()) return;
    FirecrackerBackend backend(ConfigFor(dir + "/chroot"));
    RecordingConnector* conn = new RecordingConnector();
    backend.SetConnectorForTesting(std::shared_ptr<Connector>(conn));

    LaunchPlan plan = PlanIn(dir);
    plan.kernel_cmdline = "console=ttyS0 reboot=k panic=1";
    core::Expected<Handle, core::AnyError> r = backend.Boot(plan);
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    MT_EXPECT_EQ(conn->calls[1].path, std::string("/boot-source"));
    MT_EXPECT_TRUE(conn->calls[1].body.find("console=ttyS0 reboot=k panic=1") !=
                   std::string::npos);
}

// ============================================================
// failure aborts the rest and stops the VMM
// ============================================================

MT_TEST(a_rejected_step_aborts_the_sequence_and_names_the_call) {
    // These are pre-boot calls: a later one cannot repair an earlier
    // rejection, so continuing would only replace a precise error with a
    // confusing one from further down.
    const std::string dir = MakeTempDir();
    if (dir.empty()) return;

    FirecrackerBackend backend(ConfigFor(dir + "/chroot"));
    RecordingConnector* conn = new RecordingConnector("/drives/rootfs");
    backend.SetConnectorForTesting(std::shared_ptr<Connector>(conn));

    core::Expected<Handle, core::AnyError> r = backend.Boot(PlanIn(dir));
    MT_EXPECT_TRUE(!r.ok());
    // The error names the failing endpoint, not just the transport fault.
    MT_EXPECT_TRUE(r.error().chain().find("/drives/rootfs") != std::string::npos);
    MT_EXPECT_TRUE(r.error().chain().find("fault injected") != std::string::npos);

    // Stopped at the failing call: the user drive, the balloon and the start
    // action were never attempted.
    const std::vector<std::string> paths = PathsOf(conn->calls);
    MT_EXPECT_EQ(paths.size(), static_cast<std::size_t>(3));
    MT_EXPECT_EQ(paths[2], std::string("/drives/rootfs"));
    for (std::size_t i = 0; i < paths.size(); ++i) {
        MT_EXPECT_TRUE(paths[i] != std::string("/actions"));
    }
}

MT_TEST(a_failed_boot_does_not_leave_a_running_vmm) {
    // A returned error with a live Firecracker still holding the work
    // directory leaks both, and the next boot for this sandbox id collides
    // with it.
    const std::string dir = MakeTempDir();
    if (dir.empty()) return;

    FirecrackerBackend backend(ConfigFor(dir + "/chroot"));

    // Capture the VMM's pid from inside the boot, since a failed Boot returns
    // no handle to find it through.
    PidCapturingConnector* conn = new PidCapturingConnector("/balloon");
    backend.SetConnectorForTesting(std::shared_ptr<Connector>(conn));

    core::Expected<Handle, core::AnyError> r = backend.Boot(PlanIn(dir));
    MT_EXPECT_TRUE(!r.ok());

    // The stand-in VMM sleeps for 30s, so it would still be alive on its own.
    // `waitpid` cannot show this -- Stop() reaps the child itself, so it
    // returns <= 0 either way. Signal 0 asks whether the process still exists.
    MT_EXPECT_TRUE(conn->vmm_pid > 0);
    if (conn->vmm_pid > 0) {
        // ESRCH: the process is gone, not merely reaped.
        MT_EXPECT_TRUE(::kill(conn->vmm_pid, 0) != 0);
    }
}

MT_TEST(a_failed_start_action_is_reported_as_such) {
    const std::string dir = MakeTempDir();
    if (dir.empty()) return;
    FirecrackerBackend backend(ConfigFor(dir + "/chroot"));
    backend.SetConnectorForTesting(
        std::shared_ptr<Connector>(new RecordingConnector("/actions")));

    core::Expected<Handle, core::AnyError> r = backend.Boot(PlanIn(dir));
    MT_EXPECT_TRUE(!r.ok());
    // Distinguished from a configuration rejection: everything was accepted
    // and the VM still would not start.
    MT_EXPECT_TRUE(r.error().chain().find("InstanceStart") != std::string::npos);
}

MT_MAIN
