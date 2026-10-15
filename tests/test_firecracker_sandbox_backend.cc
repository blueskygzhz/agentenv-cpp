// SPDX-License-Identifier: MIT
// Rust: src/sandbox/firecracker/sandbox.rs — `impl SandboxBackend for
// FirecrackerSandbox`, plus `recover_capture_failure`.
//
// The assertions are about the capture-error contract, which is the part a
// caller acts on: `Recoverable` is a *promise* that the sandbox is still
// runnable, so it may only be reported after the VM has actually been resumed.
// Getting that wrong strands a paused VM that the orchestrator believes is
// live — a failure that shows up later, as a sandbox that answers nothing.
#include "microtest.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/fs.h"
#include "agentenv/sandbox/firecracker/sandbox_backend.h"

using namespace agentenv;
using namespace agentenv::sandbox;
using namespace agentenv::sandbox::firecracker;

namespace {

/// Fails the nominated endpoints; records every call.
class ScriptedSocket : public Socket {
 public:
    ScriptedSocket(std::vector<std::string>* log, const std::string& fail_state)
        : log_(log), fail_state_(fail_state) {}

    core::Expected<std::string, std::string> Request(const std::string& method,
                                                     const std::string& path,
                                                     const std::string& body) override {
        log_->push_back(method + " " + path + " " + body);
        // `PATCH /vm` carries the target state in its body, so pause and resume
        // share an endpoint and can only be told apart by it.
        if (!fail_state_.empty() && body.find(fail_state_) != std::string::npos) {
            return core::make_unexpected(std::string("HTTP 400: ") + fail_state_ + " refused");
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
    std::vector<std::string>* log_;
    std::string fail_state_;
};

class ScriptedConnector : public Connector {
 public:
    explicit ScriptedConnector(const std::string& fail_state = std::string())
        : fail_state_(fail_state) {}

    core::Expected<std::shared_ptr<Socket>, std::string>
    Connect(const std::string& /*path*/) override {
        return std::shared_ptr<Socket>(new ScriptedSocket(&calls, fail_state_));
    }

    std::vector<std::string> calls;

 private:
    std::string fail_state_;
};

/// A backend over an instance with no spawned child: the API still flows
/// through the connector, which is all these assertions need.
std::unique_ptr<FirecrackerSandboxBackend> MakeBackend(ScriptedConnector* conn) {
    std::unique_ptr<FirecrackerInstance> instance(
        new FirecrackerInstance("/tmp/aenv-sbx-test", std::shared_ptr<Connector>(conn)));
    SnapshotConfig cfg;
    return std::unique_ptr<FirecrackerSandboxBackend>(
        new FirecrackerSandboxBackend(std::move(instance), cfg));
}

/// `SandboxId` is a UUID, so a readable label like "sbx-start-0001" does not
/// parse and `.value()` on the failure yields an unspecified id -- which the
/// managed-snapshot path is derived from. A real one, fixed so assertions can
/// name the directory it produces.
const char* const kTestSandboxIdText = "0192f3a1-4c5d-7e80-9abc-def012345678";

core::SandboxId TestSandboxId() {
    const core::Expected<core::SandboxId, std::string> parsed =
        core::SandboxId::Parse(kTestSandboxIdText);
    return parsed.ok() ? parsed.value() : core::SandboxId();
}

/// A spec whose paths exist, so a start reaches the VMM stand-in.
FreshStartSpec SpecIn(const std::string& dir) {
    { std::ofstream f((dir + "/vmlinux").c_str()); f << "k"; }
    FreshStartSpec spec;
    spec.firecracker_bin = AGENTENV_FAKE_FIRECRACKER;
    spec.chroot_base_dir = dir + "/chroot";
    spec.sandbox_id = TestSandboxId();
    // Keeps `Snapshot`'s managed-snapshot base inside the test's temp dir
    // instead of the shared `/tmp/aenv`, where concurrent runs would collide.
    spec.firecracker_work_dir = core::Optional<std::string>(dir + "/fc-work");
    spec.socket_timeout_ms = 300;
    spec.socket_poll_ms = 5;
    spec.envd_timeout_ms = 60;
    spec.envd_poll_ms = 5;
    spec.configure.mem_size_mib = 512;
    spec.configure.vcpu_count = 1;
    spec.configure.kernel_image = dir + "/vmlinux";
    return spec;
}

std::string MakeStartDir() {
    char tmpl[] = "/tmp/aenv-start-XXXXXX";
    char* p = ::mkdtemp(tmpl);
    return p ? std::string(p) : std::string();
}

/// Records the order in which capture steps ran, so the ordering rules can be
/// asserted rather than inferred from side effects.
struct CaptureTrace {
    std::vector<std::string> events;
    int64_t pid_seen = 0;
    uint64_t memory_size_seen = 0;
};

bool PathExists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

/// A spec whose callbacks succeed and log themselves.
PauseToDirSpec TracingSpec(const std::string& dir, CaptureTrace* trace) {
    PauseToDirSpec spec;
    spec.snapshot_dir = dir + "/capture";
    spec.flush_volumes = [trace]() -> core::Expected<core::Unit, std::string> {
        trace->events.push_back("flush");
        return core::Unit{};
    };
    spec.on_pid_read = [trace](int64_t pid) {
        trace->events.push_back("pid");
        trace->pid_seen = pid;
    };
    spec.write_memory_layer =
        [trace](const std::string& path,
                const std::vector<storage::overlaybd::lsmt::SegmentMapping>&,
                uint64_t memory_size, int64_t pid)
        -> core::Expected<core::Unit, std::string> {
        trace->events.push_back("write_layer");
        trace->pid_seen = pid;
        trace->memory_size_seen = memory_size;
        std::ofstream f(path.c_str());
        f << "layer";
        return core::Unit{};
    };
    return spec;
}

/// A connector that answers the dirty-ranges GET with a usable reply.
class CaptureConnector : public Connector {
 public:
    CaptureConnector(CaptureTrace* trace, const std::string& fail_path)
        : trace_(trace), fail_path_(fail_path) {}

    core::Expected<std::shared_ptr<Socket>, std::string>
    Connect(const std::string&) override {
        class S : public Socket {
         public:
            S(CaptureTrace* t, const std::string& fp) : t_(t), fp_(fp) {}
            core::Expected<std::string, std::string>
            Request(const std::string& m, const std::string& p,
                    const std::string& b) override {
                if (p == "/vm" && b.find("Paused") != std::string::npos) {
                    t_->events.push_back("pause");
                } else if (p == "/vm" && b.find("Resumed") != std::string::npos) {
                    t_->events.push_back("resume");
                } else if (p == "/snapshot/create") {
                    t_->events.push_back("create_snapshot");
                } else if (p == "/vm/dirty-memory-ranges") {
                    t_->events.push_back("dirty_ranges");
                }
                if (!fp_.empty() && p == fp_) {
                    return core::make_unexpected(std::string("HTTP 400: ") + fp_ + " refused");
                }
                if (p == "/vm/dirty-memory-ranges") {
                    return std::string(
                        "{\"page_size\":4096,\"memory_size\":8192,\"ranges\":["
                        "{\"base_host_virt_addr\":4096,\"image_offset\":0,"
                        "\"length\":8192}]}");
                }
                return std::string();
            }
            std::future<core::Expected<std::string, std::string> >
            RequestAsync(const std::string& m, const std::string& p,
                         const std::string& b) override {
                std::promise<core::Expected<std::string, std::string> > pr;
                pr.set_value(Request(m, p, b));
                return pr.get_future();
            }
         private:
            CaptureTrace* t_;
            std::string fp_;
        };
        return std::shared_ptr<Socket>(new S(trace_, fail_path_));
    }

 private:
    CaptureTrace* trace_;
    std::string fail_path_;
};

/// A started backend over the fake VMM, so Pid() returns a real child.
std::unique_ptr<FirecrackerSandboxBackend> StartedBackend(const std::string& dir,
                                                          CaptureTrace* trace,
                                                          const std::string& fail_path) {
    SnapshotConfig cfg;
    std::unique_ptr<FirecrackerSandboxBackend> backend(new FirecrackerSandboxBackend(
        SpecIn(dir), cfg,
        std::shared_ptr<Connector>(new CaptureConnector(trace, fail_path))));
    if (!backend->StartNowait().ok()) return std::unique_ptr<FirecrackerSandboxBackend>();
    trace->events.clear();  // drop the boot calls
    return backend;
}

int IndexIn(const std::vector<std::string>& v, const std::string& what) {
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] == what) return static_cast<int>(i);
    }
    return -1;
}

std::size_t CountWithState(const std::vector<std::string>& calls, const std::string& state) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < calls.size(); ++i) {
        if (calls[i].find(state) != std::string::npos) ++n;
    }
    return n;
}

}  // namespace

// ============================================================
// pause
// ============================================================

MT_TEST(pause_yields_a_paused_state_carrying_the_capture_descriptor) {
    ScriptedConnector* conn = new ScriptedConnector();
    std::unique_ptr<FirecrackerSandboxBackend> backend = MakeBackend(conn);

    SandboxCaptureResult<std::shared_ptr<PausedSandboxState> > r =
        backend->Pause(core::nullopt);
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    MT_EXPECT_TRUE(r.value().get() != NULL);
    // One PATCH, to Paused. The state is what a later resume decodes, so the
    // descriptor must survive the round trip.
    MT_EXPECT_EQ(conn->calls.size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(conn->calls[0].find("PATCH /vm") != std::string::npos);
    MT_EXPECT_TRUE(conn->calls[0].find("Paused") != std::string::npos);
    MT_EXPECT_TRUE(!r.value()->Encode().empty());
}

MT_TEST(a_failed_pause_resumes_the_vm_before_reporting_recoverable) {
    // This is the whole point of the variant. `Recoverable` promises the caller
    // the sandbox is still runnable, so the resume must have happened by the
    // time the error is returned -- otherwise the orchestrator keeps scheduling
    // onto a VM that is paused and answers nothing.
    ScriptedConnector* conn = new ScriptedConnector("Paused");
    std::unique_ptr<FirecrackerSandboxBackend> backend = MakeBackend(conn);

    SandboxCaptureResult<std::shared_ptr<PausedSandboxState> > r =
        backend->Pause(core::nullopt);
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_TRUE(!r.error().IsTerminal());
    MT_EXPECT_TRUE(r.error().message.find("Paused refused") != std::string::npos);
    // The recovery resume was actually issued.
    MT_EXPECT_EQ(CountWithState(conn->calls, "Resumed"), static_cast<std::size_t>(1));
}

MT_TEST(a_pause_whose_recovery_also_fails_becomes_terminal) {
    // The promise cannot be kept, so the error is upgraded. Reporting
    // Recoverable here would be a lie the caller acts on.
    // Both states must fail, and `ScriptedSocket` keys off the body, so a
    // connector that refuses every request is used instead.
    class RefuseAll : public Connector {
     public:
        explicit RefuseAll(std::vector<std::string>* log) : log_(log) {}
        core::Expected<std::shared_ptr<Socket>, std::string>
        Connect(const std::string&) override {
            class S : public Socket {
             public:
                explicit S(std::vector<std::string>* l) : l_(l) {}
                core::Expected<std::string, std::string>
                Request(const std::string& m, const std::string& p,
                        const std::string& b) override {
                    l_->push_back(m + " " + p + " " + b);
                    return core::make_unexpected(std::string("HTTP 500: vmm wedged"));
                }
                std::future<core::Expected<std::string, std::string> >
                RequestAsync(const std::string& m, const std::string& p,
                             const std::string& b) override {
                    std::promise<core::Expected<std::string, std::string> > pr;
                    pr.set_value(Request(m, p, b));
                    return pr.get_future();
                }
             private:
                std::vector<std::string>* l_;
            };
            return std::shared_ptr<Socket>(new S(log_));
        }
     private:
        std::vector<std::string>* log_;
    };
    std::vector<std::string> calls;
    std::unique_ptr<FirecrackerInstance> instance(
        new FirecrackerInstance("/tmp/aenv-sbx-test",
                                std::shared_ptr<Connector>(new RefuseAll(&calls))));
    SnapshotConfig cfg;
    FirecrackerSandboxBackend backend(std::move(instance), cfg);

    SandboxCaptureResult<std::shared_ptr<PausedSandboxState> > r =
        backend.Pause(core::nullopt);
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_TRUE(r.error().IsTerminal());
    // Both faults are named: the original is what an operator diagnoses, the
    // resume failure is what makes it unrecoverable.
    MT_EXPECT_TRUE(r.error().message.find("could not be resumed") != std::string::npos);
    MT_EXPECT_TRUE(r.error().message.find("vmm wedged") != std::string::npos);
    MT_EXPECT_EQ(CountWithState(calls, "Resumed"), static_cast<std::size_t>(1));
}

MT_TEST(an_unported_artifact_root_is_refused_rather_than_ignored) {
    // Honouring the pause but dropping the artifacts would hand back a paused
    // state whose backing files were never written -- discovered only at the
    // resume, on another node.
    ScriptedConnector* conn = new ScriptedConnector();
    std::unique_ptr<FirecrackerSandboxBackend> backend = MakeBackend(conn);

    SandboxCaptureResult<std::shared_ptr<PausedSandboxState> > r =
        backend->Pause(core::Optional<std::string>("/var/lib/agentenv/captures"));
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    // Recoverable: nothing was touched, so the sandbox is still runnable.
    MT_EXPECT_TRUE(!r.error().IsTerminal());
    // And nothing was: the VM was never paused.
    MT_EXPECT_EQ(conn->calls.size(), static_cast<std::size_t>(0));
}

// ============================================================
// resume
// ============================================================

MT_TEST(resume_is_idempotent) {
    // The trait requires it and Firecracker provides it: a resume on a running
    // VM is accepted. The call is therefore forwarded unconditionally rather
    // than gated on a local flag, which could only drift from the VMM.
    ScriptedConnector* conn = new ScriptedConnector();
    std::unique_ptr<FirecrackerSandboxBackend> backend = MakeBackend(conn);

    MT_EXPECT_TRUE(backend->Resume().ok());
    MT_EXPECT_TRUE(backend->Resume().ok());
    MT_EXPECT_TRUE(backend->Resume().ok());
    MT_EXPECT_EQ(CountWithState(conn->calls, "Resumed"), static_cast<std::size_t>(3));
}

MT_TEST(a_refused_resume_is_an_error) {
    ScriptedConnector* conn = new ScriptedConnector("Resumed");
    std::unique_ptr<FirecrackerSandboxBackend> backend = MakeBackend(conn);

    core::Expected<core::Unit, core::AnyError> r = backend->Resume();
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().chain().find("Resumed refused") != std::string::npos);
}

MT_TEST(pause_then_resume_round_trips) {
    ScriptedConnector* conn = new ScriptedConnector();
    std::unique_ptr<FirecrackerSandboxBackend> backend = MakeBackend(conn);

    MT_EXPECT_TRUE(backend->Pause(core::nullopt).ok());
    MT_EXPECT_TRUE(backend->Resume().ok());
    MT_EXPECT_EQ(conn->calls.size(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(conn->calls[0].find("Paused") != std::string::npos);
    MT_EXPECT_TRUE(conn->calls[1].find("Resumed") != std::string::npos);
}

// ============================================================
// stop
// ============================================================

MT_TEST(stop_is_idempotent) {
    // The trait requires it. Nothing to stop is a success, not an error:
    // a caller that must call Stop() after Pause() cannot be made to track
    // whether a previous Stop() already ran.
    ScriptedConnector* conn = new ScriptedConnector();
    std::unique_ptr<FirecrackerSandboxBackend> backend = MakeBackend(conn);

    MT_EXPECT_TRUE(backend->Stop().ok());
    MT_EXPECT_TRUE(backend->Stop().ok());
}

MT_TEST(stop_succeeds_when_there_is_no_instance_at_all) {
    // The trait tells callers to Stop() after Pause(), and a caller cannot be
    // made to track whether an earlier Stop() already ran -- so "nothing to
    // stop" has to be a success. A backend constructed without an instance is
    // the state a moved-from or already-released one is left in.
    SnapshotConfig cfg;
    FirecrackerSandboxBackend backend(std::unique_ptr<FirecrackerInstance>(), cfg);
    MT_EXPECT_TRUE(backend.Stop().ok());
    MT_EXPECT_TRUE(backend.Stop().ok());
    // Pause and Resume, by contrast, need a live VM and must say so.
    MT_EXPECT_TRUE(!backend.Resume().ok());
    MT_EXPECT_TRUE(!backend.Pause(core::nullopt).ok());
}

// ============================================================
// unported methods fail rather than lie
// ============================================================

MT_TEST(unported_capture_methods_report_failure) {
    // A caller told that a snapshot succeeded will later resume from a capture
    // that was never written, and the fault then appears on a different node
    // as a corrupt record. Failing here keeps the blame where the cause is.
    ScriptedConnector* conn = new ScriptedConnector();
    std::unique_ptr<FirecrackerSandboxBackend> backend = MakeBackend(conn);

    // `Snapshot` is no longer on this list: it runs the real capture sequence
    // now, so it is covered by its own tests below rather than by asserting
    // it refuses. Leaving it here would have kept passing for the wrong
    // reason -- it still fails without a spawned child, but from the Pid()
    // read rather than from being unported.
    MT_EXPECT_TRUE(!backend->SnapshotVolumes().ok());
    MT_EXPECT_TRUE(!backend->FreezeAndSnapshotVolumes().ok());
    MT_EXPECT_TRUE(!backend->ThawVolumes().ok());
    MT_EXPECT_TRUE(!backend->Start().ok());
    MT_EXPECT_TRUE(!backend->StartNowait().ok());
    MT_EXPECT_TRUE(!backend->WaitForReady().ok());
    MT_EXPECT_TRUE(!backend->Fork(std::vector<SandboxForkSpec>()).ok());
    MT_EXPECT_TRUE(!backend->UpdateNetworkPolicy(core::nullopt).ok());
    // No API call was made by any of them.
    MT_EXPECT_EQ(conn->calls.size(), static_cast<std::size_t>(0));
}

MT_TEST(runtime_info_leaves_an_unrecorded_rootfs_size_absent) {
    // Rust caches this from the live block device during `start()` and treats
    // its absence as "snapshot not allowed yet". This port never records it,
    // so it must stay absent rather than be guessed -- a wrong size would make
    // a snapshot silently short.
    ScriptedConnector* conn = new ScriptedConnector();
    std::unique_ptr<FirecrackerSandboxBackend> backend = MakeBackend(conn);

    const SandboxRuntimeInfo info = backend->RuntimeInfoOf();
    MT_EXPECT_TRUE(!info.rootfs_virtual_size.has_value());
    MT_EXPECT_TRUE(!backend->HostInteractionIp().has_value());
}


// ============================================================
// start / start_nowait / wait_for_ready
// ============================================================

MT_TEST(start_nowait_runs_the_fresh_sequence) {
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    ScriptedConnector* conn = new ScriptedConnector();
    SnapshotConfig cfg;
    FirecrackerSandboxBackend backend(SpecIn(dir), cfg,
                                      std::shared_ptr<Connector>(conn));

    MT_EXPECT_TRUE(backend.StartNowait().ok());
    // The same sequence the boot path issues, ending in InstanceStart.
    MT_EXPECT_TRUE(conn->calls.size() >= static_cast<std::size_t>(5));
    MT_EXPECT_TRUE(conn->calls[0].find("/machine-config") != std::string::npos);
    MT_EXPECT_TRUE(conn->calls.back().find("InstanceStart") != std::string::npos);
    MT_EXPECT_TRUE(backend.Stop().ok());
}

MT_TEST(a_second_start_is_refused_rather_than_spawning_another_vmm) {
    // Spawning again would leak the first VMM and put two processes on one
    // socket path, where a request reaches whichever bound it.
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    ScriptedConnector* conn = new ScriptedConnector();
    SnapshotConfig cfg;
    FirecrackerSandboxBackend backend(SpecIn(dir), cfg,
                                      std::shared_ptr<Connector>(conn));

    MT_EXPECT_TRUE(backend.StartNowait().ok());
    const std::size_t after_first = conn->calls.size();

    core::Expected<core::Unit, core::AnyError> again = backend.StartNowait();
    MT_EXPECT_TRUE(!again.ok());
    MT_EXPECT_TRUE(again.error().chain().find("already started") != std::string::npos);
    // Nothing further was issued.
    MT_EXPECT_EQ(conn->calls.size(), after_first);
    MT_EXPECT_TRUE(backend.Stop().ok());
}

MT_TEST(a_backend_given_a_booted_instance_cannot_be_started) {
    // There is nothing left to start, and a start would spawn a second VMM
    // over an instance this backend did not create.
    ScriptedConnector* conn = new ScriptedConnector();
    std::unique_ptr<FirecrackerSandboxBackend> backend = MakeBackend(conn);

    core::Expected<core::Unit, core::AnyError> r = backend->StartNowait();
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().chain().find("already-booted") != std::string::npos);
    MT_EXPECT_EQ(conn->calls.size(), static_cast<std::size_t>(0));
}

MT_TEST(wait_for_ready_waits_for_envd_not_the_api_socket) {
    // The API socket is already up by the time StartNowait returns. This is
    // the readiness the workload depends on, so it must probe envd.
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    SnapshotConfig cfg;
    FirecrackerSandboxBackend backend(SpecIn(dir), cfg,
                                      std::shared_ptr<Connector>(new ScriptedConnector()));

    int probes = 0;
    backend.SetEnvdProbeForTesting([&probes](int32_t) -> bool {
        ++probes;
        return probes >= 3;  // healthy on the third attempt
    });

    MT_EXPECT_TRUE(backend.StartNowait().ok());
    MT_EXPECT_TRUE(backend.WaitForReady().ok());
    MT_EXPECT_EQ(probes, 3);
    MT_EXPECT_TRUE(backend.Stop().ok());
}

MT_TEST(wait_for_ready_refuses_to_claim_readiness_it_never_observed) {
    // Rust errors when no envd instance exists. Reporting readiness without
    // having confirmed it hands the orchestrator a sandbox that accepts work
    // and answers nothing -- the one answer that cannot be recovered from.
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    SnapshotConfig cfg;
    FirecrackerSandboxBackend backend(SpecIn(dir), cfg,
                                      std::shared_ptr<Connector>(new ScriptedConnector()));

    MT_EXPECT_TRUE(backend.StartNowait().ok());
    core::Expected<core::Unit, core::AnyError> r = backend.WaitForReady();
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().chain().find("envd instance not initialized") !=
                   std::string::npos);
    MT_EXPECT_TRUE(backend.Stop().ok());
}

MT_TEST(wait_for_ready_before_start_is_an_error) {
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    SnapshotConfig cfg;
    FirecrackerSandboxBackend backend(SpecIn(dir), cfg,
                                      std::shared_ptr<Connector>(new ScriptedConnector()));
    backend.SetEnvdProbeForTesting([](int32_t) -> bool { return true; });

    core::Expected<core::Unit, core::AnyError> r = backend.WaitForReady();
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().chain().find("not started") != std::string::npos);
}

MT_TEST(start_is_start_nowait_then_wait_for_ready) {
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    ScriptedConnector* conn = new ScriptedConnector();
    SnapshotConfig cfg;
    FirecrackerSandboxBackend backend(SpecIn(dir), cfg,
                                      std::shared_ptr<Connector>(conn));

    bool probed = false;
    backend.SetEnvdProbeForTesting([&probed](int32_t) -> bool {
        probed = true;
        return true;
    });

    MT_EXPECT_TRUE(backend.Start().ok());
    // Both halves ran: the VM was configured and started, and envd was probed.
    MT_EXPECT_TRUE(conn->calls.back().find("InstanceStart") != std::string::npos);
    MT_EXPECT_TRUE(probed);
    MT_EXPECT_TRUE(backend.Stop().ok());
}

MT_TEST(start_does_not_probe_envd_when_the_vm_never_started) {
    // A failed start_nowait must short-circuit: probing envd on a VM that was
    // never configured would spend the whole envd timeout before reporting the
    // real cause.
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    FreshStartSpec spec = SpecIn(dir);
    spec.configure.physical_extra_drive_count = 3;  // more than prepared: permanent

    SnapshotConfig cfg;
    FirecrackerSandboxBackend backend(spec, cfg,
                                      std::shared_ptr<Connector>(new ScriptedConnector()));
    bool probed = false;
    backend.SetEnvdProbeForTesting([&probed](int32_t) -> bool {
        probed = true;
        return true;
    });

    core::Expected<core::Unit, core::AnyError> r = backend.Start();
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(r.error().chain().find("physical extra drives") != std::string::npos);
    MT_EXPECT_TRUE(!probed);
}

MT_TEST(a_failed_start_leaves_no_instance_to_pause) {
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    FreshStartSpec spec = SpecIn(dir);
    spec.firecracker_bin = dir + "/no-such-binary";

    SnapshotConfig cfg;
    FirecrackerSandboxBackend backend(spec, cfg,
                                      std::shared_ptr<Connector>(new ScriptedConnector()));
    MT_EXPECT_TRUE(!backend.StartNowait().ok());
    // StartFreshInstance assigns its out-param only on success, so a failed
    // start must leave the backend with no instance at all. Handing one back
    // alongside an error would make the caller own a VMM it was told not to
    // expect -- and Pause would then appear to work on it.
    MT_EXPECT_TRUE(!backend.Pause(core::nullopt).ok());
    MT_EXPECT_TRUE(!backend.Resume().ok());
    MT_EXPECT_TRUE(backend.Stop().ok());
}

MT_TEST(a_started_sandbox_can_be_paused_and_resumed) {
    // The point of merging the start path into this interface: one object now
    // covers the whole lifecycle, where before a VM could only be booted
    // through the legacy shim and paused through this one.
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    ScriptedConnector* conn = new ScriptedConnector();
    SnapshotConfig cfg;
    FirecrackerSandboxBackend backend(SpecIn(dir), cfg,
                                      std::shared_ptr<Connector>(conn));

    MT_EXPECT_TRUE(backend.StartNowait().ok());
    MT_EXPECT_TRUE(backend.Pause(core::nullopt).ok());
    MT_EXPECT_TRUE(backend.Resume().ok());
    MT_EXPECT_EQ(CountWithState(conn->calls, "Paused"), static_cast<std::size_t>(1));
    MT_EXPECT_EQ(CountWithState(conn->calls, "Resumed"), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(backend.Stop().ok());
}

MT_TEST(start_fresh_instance_does_not_hand_back_an_instance_on_failure) {
    // Ownership is all-or-nothing: the out-param is assigned only after
    // InstanceStart succeeds. Anything earlier returns with the unique_ptr
    // still local, so unwinding stops the VMM -- assigning it on the way out
    // would both leak the process and contradict the error.
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    FreshStartSpec spec = SpecIn(dir);
    // Socket never appears: the stand-in VMM is replaced by one that exits at
    // once, so WaitForReady fails partway through the sequence.
    spec.firecracker_bin = "/bin/true";
    spec.socket_timeout_ms = 100;

    std::unique_ptr<FirecrackerInstance> out;
    const core::Expected<core::Unit, std::string> r =
        StartFreshInstance(spec, std::shared_ptr<Connector>(new ScriptedConnector()), &out);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(out.get() == NULL);
}

// ============================================================
// pause_to_dir: the ordering rules
// ============================================================

MT_TEST(capture_flushes_writable_volumes_before_pausing) {
    // The flush runs `busybox sync` inside the guest through envd, and envd
    // cannot be reached once the VM is paused. Reversed, the capture takes
    // volumes with dirty page cache -- the resumed sandbox then sees a stale
    // filesystem, not merely an old one.
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    CaptureTrace trace;
    std::unique_ptr<FirecrackerSandboxBackend> backend =
        StartedBackend(dir, &trace, std::string());
    if (!backend) return;

    PauseToDirSpec spec = TracingSpec(dir, &trace);
    spec.has_writable_persistent_volumes = true;

    SandboxCaptureResult<PauseToDirResult> r = backend->PauseToDir(spec);
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    MT_EXPECT_TRUE(IndexIn(trace.events, "flush") >= 0);
    MT_EXPECT_TRUE(IndexIn(trace.events, "flush") < IndexIn(trace.events, "pause"));
    MT_EXPECT_TRUE(backend->Stop().ok());
}

MT_TEST(capture_skips_the_flush_when_no_volume_needs_it) {
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    CaptureTrace trace;
    std::unique_ptr<FirecrackerSandboxBackend> backend =
        StartedBackend(dir, &trace, std::string());
    if (!backend) return;

    PauseToDirSpec spec = TracingSpec(dir, &trace);
    spec.has_writable_persistent_volumes = false;

    MT_EXPECT_TRUE(backend->PauseToDir(spec).ok());
    MT_EXPECT_EQ(IndexIn(trace.events, "flush"), -1);
    MT_EXPECT_TRUE(backend->Stop().ok());
}

MT_TEST(a_writable_volume_without_envd_is_refused_before_the_pause) {
    // Rust fails with this exact reason rather than skipping the flush: a
    // capture that silently omits it produces the stale filesystem above.
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    CaptureTrace trace;
    std::unique_ptr<FirecrackerSandboxBackend> backend =
        StartedBackend(dir, &trace, std::string());
    if (!backend) return;

    PauseToDirSpec spec = TracingSpec(dir, &trace);
    spec.has_writable_persistent_volumes = true;
    spec.flush_volumes = NULL;  // envd not running

    SandboxCaptureResult<PauseToDirResult> r = backend->PauseToDir(spec);
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_TRUE(r.error().message.find("envd is not running") != std::string::npos);
    // Recoverable, and genuinely so: the VM was never paused.
    MT_EXPECT_TRUE(!r.error().IsTerminal());
    MT_EXPECT_EQ(IndexIn(trace.events, "pause"), -1);
    MT_EXPECT_TRUE(backend->Stop().ok());
}

MT_TEST(capture_reads_the_pid_before_taking_the_state_snapshot) {
    // Guest memory is pulled from the live VMM with process_vm_readv, so the
    // capture needs a pid that is still valid. Reading it afterwards would
    // race a VMM that died during the snapshot and blame the wrong step.
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    CaptureTrace trace;
    std::unique_ptr<FirecrackerSandboxBackend> backend =
        StartedBackend(dir, &trace, std::string());
    if (!backend) return;

    // The pid is read through the instance, so the only way to observe *when*
    // is to kill the VMM between the pause and the snapshot: a capture that
    // reads the pid first still has it, while one that reads it later does
    // not. The value alone proves nothing -- it is the same number either way.
    SandboxCaptureResult<PauseToDirResult> r =
        backend->PauseToDir(TracingSpec(dir, &trace));
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    MT_EXPECT_TRUE(trace.pid_seen > 0);
    // Ordering, asserted directly: the pid must already be known by the time
    // the state snapshot is requested.
    MT_EXPECT_TRUE(IndexIn(trace.events, "pid") >= 0);
    MT_EXPECT_TRUE(IndexIn(trace.events, "pid") <
                   IndexIn(trace.events, "create_snapshot"));
    MT_EXPECT_TRUE(backend->Stop().ok());
}

MT_TEST(dirty_ranges_are_read_after_the_state_snapshot) {
    // vm_state.bin must describe the same instant as the ranges. Reading the
    // ranges first would describe memory as of before the state was written.
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    CaptureTrace trace;
    std::unique_ptr<FirecrackerSandboxBackend> backend =
        StartedBackend(dir, &trace, std::string());
    if (!backend) return;

    MT_EXPECT_TRUE(backend->PauseToDir(TracingSpec(dir, &trace)).ok());
    const int snap = IndexIn(trace.events, "create_snapshot");
    const int ranges = IndexIn(trace.events, "dirty_ranges");
    const int layer = IndexIn(trace.events, "write_layer");
    MT_EXPECT_TRUE(snap >= 0 && ranges >= 0 && layer >= 0);
    MT_EXPECT_TRUE(snap < ranges);
    MT_EXPECT_TRUE(ranges < layer);
    MT_EXPECT_TRUE(backend->Stop().ok());
}

MT_TEST(a_successful_capture_names_its_artifacts) {
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    CaptureTrace trace;
    std::unique_ptr<FirecrackerSandboxBackend> backend =
        StartedBackend(dir, &trace, std::string());
    if (!backend) return;

    SandboxCaptureResult<PauseToDirResult> r =
        backend->PauseToDir(TracingSpec(dir, &trace));
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) return;
    MT_EXPECT_TRUE(r.value().vm_state_path.find("vm_state") != std::string::npos);
    MT_EXPECT_TRUE(r.value().memory_layer_path.find("mem_overlaybd/overlaybd.commit") !=
                   std::string::npos);
    // The declared memory size comes from the API reply, not from the layer.
    MT_EXPECT_EQ(r.value().memory_size, static_cast<uint64_t>(8192));
    MT_EXPECT_TRUE(PathExists(r.value().memory_layer_path));
    MT_EXPECT_TRUE(backend->Stop().ok());
}

// ============================================================
// pause_to_dir: failure cleanup
// ============================================================

MT_TEST(a_failed_capture_removes_the_partial_directory) {
    // A half-written snapshot directory cannot be told from a complete one by
    // name, so the resume path would later load a vm_state.bin whose memory
    // layer was never finished.
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    CaptureTrace trace;
    std::unique_ptr<FirecrackerSandboxBackend> backend =
        StartedBackend(dir, &trace, std::string());
    if (!backend) return;

    PauseToDirSpec spec = TracingSpec(dir, &trace);
    spec.write_memory_layer =
        [](const std::string& path,
           const std::vector<storage::overlaybd::lsmt::SegmentMapping>&, uint64_t,
           int64_t) -> core::Expected<core::Unit, std::string> {
        // Write something, then fail: this is the half-written case.
        std::ofstream f(path.c_str());
        f << "partial";
        return core::make_unexpected(std::string("layer write failed"));
    };

    SandboxCaptureResult<PauseToDirResult> r = backend->PauseToDir(spec);
    MT_EXPECT_TRUE(!r.ok());
    MT_EXPECT_TRUE(!PathExists(spec.snapshot_dir));
    MT_EXPECT_TRUE(backend->Stop().ok());
}

MT_TEST(a_failed_capture_resumes_the_vm_before_reporting_recoverable) {
    // The VM is already paused by the time the capture fails, so the recovery
    // resume matters more here than in a plain Pause, not less: Recoverable
    // still promises the caller a runnable sandbox.
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    CaptureTrace trace;
    std::unique_ptr<FirecrackerSandboxBackend> backend =
        StartedBackend(dir, &trace, std::string());
    if (!backend) return;

    PauseToDirSpec spec = TracingSpec(dir, &trace);
    spec.write_memory_layer =
        [](const std::string&, const std::vector<storage::overlaybd::lsmt::SegmentMapping>&,
           uint64_t, int64_t) -> core::Expected<core::Unit, std::string> {
        return core::make_unexpected(std::string("layer write failed"));
    };

    SandboxCaptureResult<PauseToDirResult> r = backend->PauseToDir(spec);
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_TRUE(!r.error().IsTerminal());
    MT_EXPECT_TRUE(IndexIn(trace.events, "resume") >= 0);
    MT_EXPECT_TRUE(IndexIn(trace.events, "pause") < IndexIn(trace.events, "resume"));
    MT_EXPECT_TRUE(backend->Stop().ok());
}

MT_TEST(a_refused_state_snapshot_cleans_up_and_recovers) {
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    CaptureTrace trace;
    std::unique_ptr<FirecrackerSandboxBackend> backend =
        StartedBackend(dir, &trace, "/snapshot/create");
    if (!backend) return;

    PauseToDirSpec spec = TracingSpec(dir, &trace);
    SandboxCaptureResult<PauseToDirResult> r = backend->PauseToDir(spec);
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_TRUE(r.error().message.find("/snapshot/create refused") != std::string::npos);
    MT_EXPECT_TRUE(!PathExists(spec.snapshot_dir));
    // The layer writer was never reached.
    MT_EXPECT_EQ(IndexIn(trace.events, "write_layer"), -1);
    MT_EXPECT_TRUE(backend->Stop().ok());
}

MT_TEST(no_injected_writer_uses_the_real_overlaybd_layer_writer) {
    // With no override the capture goes through PublishMemoryOverlaybdLayer,
    // which compacts the VM's memory into a sealed LSMT file. The fake VMM has
    // no guest memory at the captured addresses, so the read fails -- what
    // matters here is that the real writer ran at all, and that its failure is
    // handled like any other capture failure: directory removed, VM resumed.
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    CaptureTrace trace;
    std::unique_ptr<FirecrackerSandboxBackend> backend =
        StartedBackend(dir, &trace, std::string());
    if (!backend) return;

    PauseToDirSpec spec = TracingSpec(dir, &trace);
    spec.write_memory_layer = NULL;  // use the real one

    SandboxCaptureResult<PauseToDirResult> r = backend->PauseToDir(spec);
    // Either outcome is acceptable; the invariants below are not.
    if (!r.ok()) {
        MT_EXPECT_TRUE(!PathExists(spec.snapshot_dir));
        MT_EXPECT_TRUE(IndexIn(trace.events, "resume") >= 0);
    } else {
        MT_EXPECT_TRUE(PathExists(r.value().memory_layer_path));
        // The temp file never survives.
        MT_EXPECT_TRUE(!PathExists(r.value().memory_layer_path + ".tmp"));
    }
    MT_EXPECT_TRUE(backend->Stop().ok());
}

MT_TEST(capture_on_an_unstarted_backend_is_terminal) {
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    CaptureTrace trace;
    SnapshotConfig cfg;
    FirecrackerSandboxBackend backend(
        SpecIn(dir), cfg,
        std::shared_ptr<Connector>(new CaptureConnector(&trace, std::string())));

    SandboxCaptureResult<PauseToDirResult> r =
        backend.PauseToDir(TracingSpec(dir, &trace));
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    MT_EXPECT_TRUE(r.error().IsTerminal());
}

MT_TEST(cleanup_does_not_follow_a_symlink_out_of_the_snapshot_dir) {
    // The capture directory is attacker-adjacent: it holds paths derived from
    // image configs. If cleanup followed a symlink it found there, a link to
    // an unrelated directory would have that directory deleted instead.
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    CaptureTrace trace;
    std::unique_ptr<FirecrackerSandboxBackend> backend =
        StartedBackend(dir, &trace, std::string());
    if (!backend) return;

    // A directory outside the capture, with a file in it, pointed at by a
    // symlink the capture leaves behind.
    const std::string outside = dir + "/outside";
    ::mkdir(outside.c_str(), 0700);
    { std::ofstream f((outside + "/keep.txt").c_str()); f << "keep"; }

    PauseToDirSpec spec = TracingSpec(dir, &trace);
    const std::string link = spec.snapshot_dir + "/link";
    spec.write_memory_layer =
        [&link, &outside](const std::string&,
                          const std::vector<storage::overlaybd::lsmt::SegmentMapping>&,
                          uint64_t, int64_t) -> core::Expected<core::Unit, std::string> {
        ::symlink(outside.c_str(), link.c_str());
        return core::make_unexpected(std::string("layer write failed"));
    };

    MT_EXPECT_TRUE(!backend->PauseToDir(spec).ok());
    // The capture directory is gone...
    MT_EXPECT_TRUE(!PathExists(spec.snapshot_dir));
    // ...and the directory it linked to survived, contents intact.
    MT_EXPECT_TRUE(PathExists(outside));
    MT_EXPECT_TRUE(PathExists(outside + "/keep.txt"));
    MT_EXPECT_TRUE(backend->Stop().ok());
}

// ============================================================
// Rust `snapshot` — pause_to_dir into a managed dir, then resume
// ============================================================

namespace {

/// The layer writer `Snapshot` needs off a real VMM: a stand-in has no guest
/// memory, so the real `process_vm_readv` path cannot run here.
MemoryLayerWriter TracingWriter(CaptureTrace* trace) {
    return [trace](const std::string& path,
                   const std::vector<storage::overlaybd::lsmt::SegmentMapping>&,
                   uint64_t memory_size, int64_t pid)
        -> core::Expected<core::Unit, std::string> {
        trace->events.push_back("write_layer");
        trace->pid_seen = pid;
        trace->memory_size_seen = memory_size;
        std::ofstream f(path.c_str());
        f << "layer";
        return core::Unit{};
    };
}

bool StartsWithPath(const std::string& s, const std::string& prefix) {
    return s.size() > prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

MT_TEST(snapshot_captures_then_leaves_the_sandbox_running) {
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    CaptureTrace trace;
    std::unique_ptr<FirecrackerSandboxBackend> backend =
        StartedBackend(dir, &trace, std::string());
    if (!backend) return;
    backend->SetMemoryLayerWriterForTesting(TracingWriter(&trace));

    SandboxCaptureResult<std::string> r = backend->Snapshot();
    MT_EXPECT_TRUE(r.ok());
    if (!r.ok()) {
        MT_EXPECT_EQ(r.error().Message(), std::string(""));
        return;
    }

    // The resume is what separates `snapshot` from `pause`: the sandbox is
    // still the caller's to use afterwards, so a capture that left it paused
    // would be a silently broken sandbox rather than a failed call.
    const int resume_at = IndexIn(trace.events, "resume");
    const int create_at = IndexIn(trace.events, "create_snapshot");
    MT_EXPECT_TRUE(create_at >= 0);
    MT_EXPECT_TRUE(resume_at > create_at);

    // Artifacts are where the result says they are. Only the memory layer is
    // checked on disk: `vm_state.bin` is written by the VMM in response to
    // `/snapshot/create`, which the stand-in only records.
    MT_EXPECT_TRUE(PathExists(r.value()));
    MT_EXPECT_TRUE(PathExists(r.value() + "/mem_overlaybd/overlaybd.commit"));

    // Under the configured work dir, namespaced by sandbox id -- not in the
    // process's temp dir, which is where an unconfigured base would land.
    const std::string expected_root =
        dir + "/fc-work/managed-snapshots/" +
        TestSandboxId().ToString();
    MT_EXPECT_TRUE(StartsWithPath(r.value(), expected_root + "/"));

    MT_EXPECT_TRUE(backend->Stop().ok());
}

MT_TEST(two_snapshots_share_a_root_and_do_not_collide) {
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    CaptureTrace trace;
    std::unique_ptr<FirecrackerSandboxBackend> backend =
        StartedBackend(dir, &trace, std::string());
    if (!backend) return;
    backend->SetMemoryLayerWriterForTesting(TracingWriter(&trace));

    SandboxCaptureResult<std::string> first = backend->Snapshot();
    SandboxCaptureResult<std::string> second = backend->Snapshot();
    MT_EXPECT_TRUE(first.ok());
    MT_EXPECT_TRUE(second.ok());
    if (!first.ok() || !second.ok()) return;

    // Distinct directories: a reused name would have the second capture
    // overwrite the first, and both records would then point at one of them.
    MT_EXPECT_TRUE(first.value() != second.value());
    MT_EXPECT_TRUE(PathExists(first.value() + "/mem_overlaybd/overlaybd.commit"));
    MT_EXPECT_TRUE(PathExists(second.value() + "/mem_overlaybd/overlaybd.commit"));
    // ...but one root, so a sandbox's captures stay together.
    MT_EXPECT_EQ(first.value().substr(0, first.value().rfind('/')),
                 second.value().substr(0, second.value().rfind('/')));

    MT_EXPECT_TRUE(backend->Stop().ok());
}

MT_TEST(a_failed_snapshot_capture_resumes_and_leaves_no_directory) {
    const std::string dir = MakeStartDir();
    if (dir.empty()) return;
    CaptureTrace trace;
    // The capture's own call fails; the pause before it does not.
    std::unique_ptr<FirecrackerSandboxBackend> backend =
        StartedBackend(dir, &trace, "/snapshot/create");
    if (!backend) return;
    backend->SetMemoryLayerWriterForTesting(TracingWriter(&trace));

    SandboxCaptureResult<std::string> r = backend->Snapshot();
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    // Recoverable, and therefore a promise the sandbox is running again.
    MT_EXPECT_TRUE(!r.error().IsTerminal());
    MT_EXPECT_TRUE(IndexIn(trace.events, "resume") >= 0);

    // Nothing half-written was left under the root: a capture directory that
    // exists but is incomplete cannot be told from a good one by name.
    const std::string root =
        dir + "/fc-work/managed-snapshots/" +
        TestSandboxId().ToString();
    const core::Expected<std::vector<std::string>, std::string> entries =
        core::fs::ReadDir(root);
    MT_EXPECT_TRUE(entries.ok());
    if (entries.ok()) MT_EXPECT_EQ(entries.value().size(), static_cast<std::size_t>(0));

    MT_EXPECT_TRUE(backend->Stop().ok());
}

MT_TEST(snapshot_without_a_live_vm_is_terminal) {
    ScriptedConnector* conn = new ScriptedConnector();
    std::unique_ptr<FirecrackerSandboxBackend> backend(new FirecrackerSandboxBackend(
        FreshStartSpec(), SnapshotConfig(), std::shared_ptr<Connector>(conn)));

    SandboxCaptureResult<std::string> r = backend->Snapshot();
    MT_EXPECT_TRUE(!r.ok());
    if (r.ok()) return;
    // Terminal rather than recoverable: `Recoverable` promises a running
    // sandbox, and there is no sandbox at all to promise about.
    MT_EXPECT_TRUE(r.error().IsTerminal());
    MT_EXPECT_TRUE(r.error().Message().find("no live microVM") != std::string::npos);
    // Nothing was attempted against the VMM.
    MT_EXPECT_EQ(conn->calls.size(), static_cast<std::size_t>(0));
}

MT_TEST(managed_snapshot_base_follows_the_configured_work_dir) {
    // Captures are large; landing them on `/tmp` when the operator named a
    // filesystem for them is the kind of thing only noticed when it fills up.
    MT_EXPECT_EQ(ManagedSnapshotBase(core::Optional<std::string>("/data/fc")),
                 std::string("/data/fc/managed-snapshots"));
    // Unset falls back to the system temp dir, namespaced under `aenv`.
    const std::string fallback = ManagedSnapshotBase(core::Optional<std::string>());
    MT_EXPECT_TRUE(fallback.find("/aenv/managed-snapshots") != std::string::npos);
    // An empty string is a missing value, not a request to write at the root:
    // `/managed-snapshots` is both unwritable and not what was meant.
    MT_EXPECT_EQ(ManagedSnapshotBase(core::Optional<std::string>(std::string())), fallback);
}

MT_MAIN
