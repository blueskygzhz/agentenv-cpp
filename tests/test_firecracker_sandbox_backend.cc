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

#include <cstdlib>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <vector>

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

/// A spec whose paths exist, so a start reaches the VMM stand-in.
FreshStartSpec SpecIn(const std::string& dir) {
    { std::ofstream f((dir + "/vmlinux").c_str()); f << "k"; }
    FreshStartSpec spec;
    spec.firecracker_bin = AGENTENV_FAKE_FIRECRACKER;
    spec.chroot_base_dir = dir + "/chroot";
    spec.sandbox_id = core::SandboxId::Parse("sbx-start-0001").value();
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

    MT_EXPECT_TRUE(!backend->Snapshot().ok());
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

MT_MAIN
