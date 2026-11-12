#!/usr/bin/env python3
"""Mutation harness for the auto-evict task (Rust `start_auto_evict_task`).

`evict_expired_sandboxes` was ported in full and had two tests, and
`auto_evict_interval_ms` was parsed and defaulted to 1000 with a test of its
own. What nothing had was a *caller*: `Orchestrator::New` built the
orchestrator and returned, where upstream's `new_inner_with_volumes` ends by
spawning the sweep. So no sandbox ever reached its timeout, `timeout_action` --
the field a caller sets to choose between pause and delete -- never ran, and a
leaked sandbox held its VM, network slot, drives and image refs until the
process died.

That is the shape of defect this file guards: not a wrong answer, but a
complete, tested, configured component with nothing driving it. A mutation that
unhooks the driver produces no error anywhere, and the config keeps claiming
eviction is enabled.

The lifecycle mutations are the other half. A background thread that reads the
store, the handle map and the deletion locks must stop before those members
die, and must stop when the node drains rather than keep pausing and deleting
sandboxes on the way down. Upstream gets both from `Weak<Self>` plus a
`watch` channel; here it is a join, so the failure mode is a use-after-free
rather than a no-op.

One mutation is deliberately absent, because no test here can catch it:
replacing the idempotence guard in `StartAutoEvictTask` with
`auto_evict_thread_.detach()`. The detached thread still observes the stop flag
and exits, so every behavioural assertion passes; what it leaves behind is a
thread touching `auto_evict_mu_` and `auto_evict_cv_` after the destructor has
returned. That is a use-after-free, not a wrong value, and catching it needs a
sanitizer build rather than an assertion.

Status is decided **by exit code only**: the suites call `exit(1)` on the first
failing assertion, so the summary line is not printed on failure and scraping
stdout would misreport a caught mutation as a survivor.
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
SERVICE = ROOT / "src/orchestrator/service.cc"

# Both suites run for every mutation: the driver's wiring is pinned in the
# persistence suite (which builds through `Orchestrator::New`), its behaviour
# and lifecycle in the lifecycle suite.
TARGETS = ["test_orchestrator_lifecycle", "test_orchestrator_persistence"]

# The call that was missing. Anchored with its comment, since `New` ends with
# a run of similar `orchestrator->Set*` lines.
NEW_WIRING = """    orchestrator->StartAutoEvictTask(config.orchestrator.auto_evict_interval_ms);
    return orchestrator;"""

THREAD_BODY = """            {
                std::unique_lock<std::mutex> lock(auto_evict_mu_);"""

SWEEP_CALL = """            const OrchestratorResult<std::vector<core::SandboxId> > evicted =
                EvictExpiredSandboxes();
            if (!evicted.ok()) {"""

MUTATIONS = [
    # ---- the wiring that was missing ---------------------------------------
    #
    # Exactly the code that was there before: an orchestrator that is built,
    # configured, and never sweeps. Nothing fails, and `ListExpired` keeps
    # answering with sandboxes nobody collects.
    (
        "new_builds_an_orchestrator_that_never_sweeps",
        NEW_WIRING,
        "    return orchestrator;",
    ),
    # The interval is read from somewhere else, so configuring it does
    # nothing. Upstream's default is 1000ms; a hard-coded value would make a
    # node tuned for faster or slower eviction behave as though it were not.
    (
        "the_configured_interval_is_ignored",
        NEW_WIRING,
        """    orchestrator->StartAutoEvictTask(1000);
    return orchestrator;""",
    ),
    # ---- the task itself ---------------------------------------------------
    #
    # The thread is never created. Same end state as the original defect,
    # reached one level down.
    (
        "starting_the_task_does_not_start_a_thread",
        "    if (auto_evict_thread_.joinable()) return;",
        "    if (true) return;",
    ),
    # The sweep runs once and the thread exits. Eviction then works for
    # exactly one tick after startup, which looks right in any test that only
    # checks the first sandbox.
    (
        "the_task_sweeps_once_and_stops",
        SWEEP_CALL,
        """            const OrchestratorResult<std::vector<core::SandboxId> > evicted =
                EvictExpiredSandboxes();
            if (true) return;
            if (!evicted.ok()) {""",
    ),
    # A failed cycle ends the loop. Rust logs and keeps going: one transient
    # store error would otherwise disable eviction for the life of the
    # process, silently, because the task has no caller to report to.
    (
        "a_failed_cycle_ends_the_task",
        """                AGENTENV_WARN("auto-evict task failed: " << evicted.error().Message());""",
        """                AGENTENV_WARN("auto-evict task failed: " << evicted.error().Message());
                return;""",
    ),
    # ---- stopping ----------------------------------------------------------
    #
    # Shutdown no longer stops the sweep. The task then keeps pausing and
    # deleting sandboxes while the node drains -- at the same time as
    # `EnsureAcceptingLifecycleOperations` is refusing every explicit request.
    # (`EvictExpiredSandboxes` checks `IsShuttingDown` itself, so the sweep
    # goes quiet; what survives is the unjoined thread.)
    (
        "shutdown_leaves_the_task_running",
        """    StopAutoEvictTask();

    OrchestratorResult<core::Unit> result = core::Unit{};""",
        "    OrchestratorResult<core::Unit> result = core::Unit{};",
    ),
    # The destructor does not join. The thread then reads the store, the
    # handle map and the deletion locks after they have been destroyed.
    (
        "the_destructor_does_not_join_the_task",
        """    StopAutoEvictTask();
}

// Rust `start_auto_evict_task`.""",
        """}

// Rust `start_auto_evict_task`.""",
    ),
    # Stop sets the flag but never wakes the sleeper, so a shutdown waits out
    # the remainder of a tick -- a second on a default configuration, and
    # unbounded for anyone who configured a slower sweep.
    (
        "stopping_does_not_wake_the_sleeping_task",
        "    auto_evict_cv_.notify_all();",
        "    // notify elided",
    ),
    # The flag is never set, so the predicate stays false and the thread sleeps
    # out every tick before noticing. The join then takes an interval.
    (
        "stopping_does_not_raise_the_stop_flag",
        """    {
        std::lock_guard<std::mutex> g(auto_evict_mu_);
        auto_evict_stop_ = true;
    }
    auto_evict_cv_.notify_all();""",
        "    auto_evict_cv_.notify_all();",
    ),
    # The wait becomes an uninterruptible sleep of the full interval: the
    # predicate is dropped, so a stop raised during the wait is not seen until
    # the timeout expires.
    (
        "the_task_sleeps_instead_of_waiting_on_the_flag",
        """                auto_evict_cv_.wait_for(lock, std::chrono::milliseconds(interval_ms),
                                        [this]() { return auto_evict_stop_; });""",
        """                auto_evict_cv_.wait_for(lock, std::chrono::milliseconds(interval_ms));""",
    ),
    # The stop flag is checked only before the wait, never after it. A stop
    # that arrives while the thread is waiting runs one more full sweep --
    # pausing or deleting sandboxes after the shutdown was requested.
    (
        "a_stop_during_the_wait_still_runs_one_more_sweep",
        "                if (auto_evict_stop_) return;",
        "                if (false) return;",
    ),
]


def build(target):
    return subprocess.run(
        ["cmake", "--build", "build", "-j", "8", "--target", target],
        cwd=ROOT, capture_output=True, text=True,
    ).returncode == 0


def run(target):
    # Timed out rather than waited on indefinitely: several mutations here
    # leave a thread that never stops, and a harness that blocks on the join
    # would hang instead of reporting the mutation.
    try:
        return subprocess.run([str(ROOT / "build/bin" / target)],
                              cwd=ROOT, capture_output=True, text=True,
                              timeout=180).returncode
    except subprocess.TimeoutExpired:
        return 124


def build_all():
    return all(build(t) for t in TARGETS)


def failing_targets():
    """Targets whose suite reported a failure, i.e. noticed the mutation."""
    return [t for t in TARGETS if run(t) != 0]


def main():
    # Refuses if a previous run was killed mid-mutation: taking a baseline
    # from a tree that still holds one would copy the mutation into the
    # backup and lose the original. See tests/mutation_residue.py.
    mutation_residue.refuse_if_residue()
    # A full pass rebuilds once per mutation and routinely outlives a
    # command timeout; without this, SIGTERM skips the restore below.
    mutation_residue.restore_on_signal()

    original = SERVICE.read_text()
    backup = SERVICE.with_suffix(SERVICE.suffix + ".mutation-backup")
    shutil.copy(SERVICE, backup)

    if not build_all():
        print("BASELINE BUILD FAILED")
        return 2
    red = failing_targets()
    if red:
        print(f"BASELINE TESTS FAILED: {', '.join(red)}")
        return 2
    print("baseline: build ok, tests pass\n")

    caught, survived, broken, noop = [], [], [], []
    try:
        for name, old, new in MUTATIONS:
            SERVICE.write_text(original)
            assert original.count(old) == 1, \
                f"anchor for {name} matched {original.count(old)} times"
            if old == new:
                noop.append(name)
                print(f"  NO-OP    {name} (mutation is identity)")
                continue
            SERVICE.write_text(original.replace(old, new, 1))

            if not build_all():
                broken.append(name)
                print(f"  BROKEN   {name} (did not compile)")
                continue
            red = failing_targets()
            if red:
                caught.append(name)
                print(f"  caught   {name} ({', '.join(red)})")
            else:
                survived.append(name)
                print(f"  SURVIVED {name}")
    finally:
        shutil.copy(backup, SERVICE)
        backup.unlink()
        build_all()

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
