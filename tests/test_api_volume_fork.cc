// SPDX-License-Identifier: MIT
// Rust: src/api/impls/sandbox.rs :: prepare_volume_fork_specs,
//       cleanup_fork_volume_children
//
// Separate from `test_api_sandbox_fork.cc` because the volume fake and the
// orchestrator fake each declare a `Fixture` in `agentenv::testing`, so the
// two headers cannot be included together. These tests need the real
// `VolumeManager` over the repository fake; the handler-level ones need the
// orchestrator.
#include "microtest.h"

#include <algorithm>
#include <string>
#include <vector>

#include "agentenv/api/volumes.h"
#include "volume_fakes.h"

using namespace agentenv;
using namespace agentenv::api;
using agentenv::testing::Fixture;
using agentenv::volume::VolumeMode;
using agentenv::volume::VolumeRecord;

namespace {

/// A source sandbox record holding `mounts`, which is all
/// `PrepareVolumeForkSpecs` reads from it.
orchestrator::SandboxMetadata SourceWith(
    const std::vector<std::pair<std::string, std::string> >& mounts) {
    orchestrator::SandboxMetadata m;
    m.id    = core::SandboxId::Fresh();
    m.state = orchestrator::SandboxState::Running;
    for (std::size_t i = 0; i < mounts.size(); ++i) {
        m.volume_mounts[mounts[i].first] = mounts[i].second;
    }
    return m;
}

bool HasVolume(Fixture& f, const std::string& id) {
    return f.manager->Get(id).ok();
}

}  // namespace

MT_TEST(an_exclusive_volume_is_cloned_per_child) {
    Fixture f;
    const VolumeRecord parent = f.PublishedVolume();
    MT_EXPECT_TRUE(parent.mode == VolumeMode::Exclusive);

    std::vector<std::pair<std::string, std::string> > mounts;
    mounts.push_back(std::make_pair(std::string("/data"), parent.id));
    const orchestrator::SandboxMetadata source = SourceWith(mounts);

    const core::Expected<PreparedVolumeForkSpecs, VolumeApiError> prepared =
        PrepareVolumeForkSpecs(f.manager.get(), source, 2);
    MT_EXPECT_TRUE(prepared.ok());
    if (!prepared.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(prepared.value().specs.size()), 2);

    std::vector<std::string> clone_ids;
    for (std::size_t i = 0; i < prepared.value().specs.size(); ++i) {
        const orchestrator::SandboxForkChildSpec& spec = prepared.value().specs[i];

        // The child mounts its clone, not the parent's volume. An exclusive
        // volume has exactly one writer by definition, so sharing it would put
        // the parent and this child on one image.
        MT_EXPECT_EQ(static_cast<int>(spec.volume_mounts.size()), 1);
        const std::string mounted = spec.volume_mounts.find("/data")->second;
        MT_EXPECT_TRUE(mounted != parent.id);
        clone_ids.push_back(mounted);

        // Registered as a drive replacement as well. The child resumes from the
        // parent's capture, whose config still names the parent's volume in
        // that slot -- without this the child would mount it regardless of what
        // the metadata says.
        MT_EXPECT_EQ(static_cast<int>(spec.replace_drive_ids.size()), 1);
        if (!spec.replace_drive_ids.empty()) {
            MT_EXPECT_EQ(spec.replace_drive_ids[0].first, parent.id);
            MT_EXPECT_EQ(spec.replace_drive_ids[0].second, mounted);
        }
    }

    // One clone each, not one shared between them.
    MT_EXPECT_TRUE(clone_ids[0] != clone_ids[1]);
    MT_EXPECT_TRUE(HasVolume(f, clone_ids[0]));
    MT_EXPECT_TRUE(HasVolume(f, clone_ids[1]));

    // The clone inherits the parent's size: a child that resumes the parent's
    // capture reads the same block device geometry.
    const agentenv::volume::VolumeResult<VolumeRecord> clone =
        f.manager->Get(clone_ids[0]);
    MT_EXPECT_TRUE(clone.ok());
    if (clone.ok()) {
        MT_EXPECT_EQ(clone.value().size_mb, parent.size_mb);
        MT_EXPECT_TRUE(clone.value().mode == VolumeMode::Exclusive);
    }
}

MT_TEST(a_read_only_volume_is_shared_not_cloned) {
    Fixture f;
    // Seeded from a published parent rather than created empty: an empty
    // volume needs `mkfs.ext4` plus the overlaybd packer, which this port does
    // not have. Rust's own tests build a read-only volume the same way.
    const VolumeRecord seed = f.PublishedVolume();
    const agentenv::volume::VolumeResult<VolumeRecord> shared = f.manager->Create(
        "shared", VolumeMode::ReadOnly, core::Optional<std::string>(seed.id),
        core::Optional<std::string>(), seed.size_mb);
    MT_EXPECT_TRUE(shared.ok());
    if (!shared.ok()) return;

    std::vector<std::pair<std::string, std::string> > mounts;
    mounts.push_back(std::make_pair(std::string("/ro"), shared.value().id));
    const orchestrator::SandboxMetadata source = SourceWith(mounts);

    const core::Expected<PreparedVolumeForkSpecs, VolumeApiError> prepared =
        PrepareVolumeForkSpecs(f.manager.get(), source, 2);
    MT_EXPECT_TRUE(prepared.ok());
    if (!prepared.ok()) return;

    for (std::size_t i = 0; i < prepared.value().specs.size(); ++i) {
        const orchestrator::SandboxForkChildSpec& spec = prepared.value().specs[i];
        // The same volume id, shared. A read-only volume has no writer, so
        // there is nothing for parent and child to race on -- and cloning it
        // would copy data that cannot change.
        MT_EXPECT_EQ(spec.volume_mounts.find("/ro")->second, shared.value().id);
        // No substitution either: the captured slot already names the volume
        // the child wants.
        MT_EXPECT_TRUE(spec.replace_drive_ids.empty());
    }

    // Nothing was created, so cleanup has nothing to undo. This is what keeps
    // a failed fork from deleting a volume the parent still mounts.
    for (ForkVolumeChildren::const_iterator it = prepared.value().children.begin();
         it != prepared.value().children.end(); ++it) {
        MT_EXPECT_TRUE(it->second.empty());
    }
}

MT_TEST(a_mixed_source_clones_only_the_exclusive_volume) {
    Fixture f;
    const VolumeRecord exclusive = f.PublishedVolume();
    // Seeded from the same published volume rather than created empty: an
    // empty volume needs `mkfs.ext4` plus the overlaybd packer, which this
    // port does not have. Rust's own tests build a read-only volume this way.
    // `PublishedVolume` always names its volume "parent", so it is called once
    // and reused here instead of twice.
    const agentenv::volume::VolumeResult<VolumeRecord> shared = f.manager->Create(
        "shared", VolumeMode::ReadOnly, core::Optional<std::string>(exclusive.id),
        core::Optional<std::string>(), exclusive.size_mb);
    MT_EXPECT_TRUE(shared.ok());
    if (!shared.ok()) return;

    std::vector<std::pair<std::string, std::string> > mounts;
    mounts.push_back(std::make_pair(std::string("/rw"), exclusive.id));
    mounts.push_back(std::make_pair(std::string("/ro"), shared.value().id));
    const orchestrator::SandboxMetadata source = SourceWith(mounts);

    const core::Expected<PreparedVolumeForkSpecs, VolumeApiError> prepared =
        PrepareVolumeForkSpecs(f.manager.get(), source, 1);
    MT_EXPECT_TRUE(prepared.ok());
    if (!prepared.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(prepared.value().specs.size()), 1);

    const orchestrator::SandboxForkChildSpec& spec = prepared.value().specs[0];
    MT_EXPECT_EQ(static_cast<int>(spec.volume_mounts.size()), 2);
    // The decision is per volume, on its mode -- not per sandbox.
    MT_EXPECT_TRUE(spec.volume_mounts.find("/rw")->second != exclusive.id);
    MT_EXPECT_EQ(spec.volume_mounts.find("/ro")->second, shared.value().id);
    // One replacement, for the one volume that moved.
    MT_EXPECT_EQ(static_cast<int>(spec.replace_drive_ids.size()), 1);

    // Both mounts still produce a drive: the child has to attach the shared
    // volume too, it just does not own it.
    MT_EXPECT_EQ(static_cast<int>(spec.extra_drives.size()), 2);
}

MT_TEST(a_source_without_mounts_yields_plain_specs) {
    Fixture f;
    const orchestrator::SandboxMetadata source =
        SourceWith(std::vector<std::pair<std::string, std::string> >());

    const core::Expected<PreparedVolumeForkSpecs, VolumeApiError> prepared =
        PrepareVolumeForkSpecs(f.manager.get(), source, 3);
    MT_EXPECT_TRUE(prepared.ok());
    if (!prepared.ok()) return;
    MT_EXPECT_EQ(static_cast<int>(prepared.value().specs.size()), 3);

    std::vector<std::string> ids;
    for (std::size_t i = 0; i < prepared.value().specs.size(); ++i) {
        MT_EXPECT_TRUE(prepared.value().specs[i].volume_mounts.empty());
        MT_EXPECT_TRUE(prepared.value().specs[i].replace_drive_ids.empty());
        ids.push_back(prepared.value().specs[i].sandbox_id.ToString());
    }
    // Each child still gets a fresh id, which is the one thing the spec always
    // carries.
    std::sort(ids.begin(), ids.end());
    MT_EXPECT_TRUE(std::unique(ids.begin(), ids.end()) == ids.end());
}

MT_TEST(an_unknown_source_volume_is_reported_not_skipped) {
    Fixture f;
    std::vector<std::pair<std::string, std::string> > mounts;
    mounts.push_back(std::make_pair(std::string("/data"), std::string("vol-missing")));
    const orchestrator::SandboxMetadata source = SourceWith(mounts);

    // Reported rather than dropped. Skipping the mount would launch the child
    // without a volume it was recorded as having, and the guest would find an
    // empty mount point instead of its data.
    const core::Expected<PreparedVolumeForkSpecs, VolumeApiError> prepared =
        PrepareVolumeForkSpecs(f.manager.get(), source, 1);
    MT_EXPECT_TRUE(!prepared.ok());
    if (!prepared.ok()) MT_EXPECT_EQ(prepared.error().code, 404);
}

MT_TEST(a_failure_partway_through_removes_every_clone_made_so_far) {
    Fixture f;
    const VolumeRecord parent = f.PublishedVolume();
    std::vector<std::pair<std::string, std::string> > mounts;
    mounts.push_back(std::make_pair(std::string("/data"), parent.id));
    const orchestrator::SandboxMetadata source = SourceWith(mounts);

    // `PublishedVolume` already created one, so refusing after two lets the
    // first child's clone through and fails the second. The failure has to
    // land *between* children -- that is the only arrangement where cleanup
    // has something to undo.
    f.repository->fail_create_volume_after = 2;

    const core::Expected<PreparedVolumeForkSpecs, VolumeApiError> prepared =
        PrepareVolumeForkSpecs(f.manager.get(), source, 2);
    MT_EXPECT_TRUE(!prepared.ok());

    // The first child's clone did get created. It must not survive the
    // failure: no sandbox exists to hold its lease or ever release it, so it
    // would sit in the catalog forever consuming its parent's layers.
    f.repository->fail_create_volume_after = -1;
    const agentenv::volume::VolumeResult<agentenv::volume::VolumePage> page =
        f.manager->ListPage(core::Optional<std::string>(), 100);
    MT_EXPECT_TRUE(page.ok());
    if (!page.ok()) return;
    int fork_clones = 0;
    for (std::size_t i = 0; i < page.value().records.size(); ++i) {
        if (page.value().records[i].name.find("volume-fork-") == 0) ++fork_clones;
    }
    MT_EXPECT_EQ(fork_clones, 0);
}

MT_TEST(a_mount_resolution_failure_also_removes_the_clones) {
    Fixture f;
    const VolumeRecord parent = f.PublishedVolume();
    const agentenv::volume::VolumeResult<VolumeRecord> second = f.manager->Create(
        "second", VolumeMode::Exclusive, core::Optional<std::string>(parent.id),
        core::Optional<std::string>(), parent.size_mb);
    MT_EXPECT_TRUE(second.ok());
    if (!second.ok()) return;

    // Overlapping mount paths. `ResolveVolumeMounts` rejects these because one
    // would shadow the other inside the guest -- and it runs *after* both
    // clones have been created, which is the case this covers: the clones are
    // this function's to undo, not the resolver's.
    std::vector<std::pair<std::string, std::string> > mounts;
    mounts.push_back(std::make_pair(std::string("/data"), parent.id));
    mounts.push_back(std::make_pair(std::string("/data/sub"), second.value().id));
    const orchestrator::SandboxMetadata source = SourceWith(mounts);

    const core::Expected<PreparedVolumeForkSpecs, VolumeApiError> prepared =
        PrepareVolumeForkSpecs(f.manager.get(), source, 1);
    MT_EXPECT_TRUE(!prepared.ok());

    // Neither clone survives. The resolver releases its own reservations, but
    // the volumes themselves would remain in the catalog leased to a sandbox
    // that never launched.
    const agentenv::volume::VolumeResult<agentenv::volume::VolumePage> page =
        f.manager->ListPage(core::Optional<std::string>(), 100);
    MT_EXPECT_TRUE(page.ok());
    if (!page.ok()) return;
    int fork_clones = 0;
    for (std::size_t i = 0; i < page.value().records.size(); ++i) {
        if (page.value().records[i].name.find("volume-fork-") == 0) ++fork_clones;
    }
    MT_EXPECT_EQ(fork_clones, 0);
}

MT_TEST(cleanup_releases_the_lease_before_deleting) {
    Fixture f;
    const VolumeRecord parent = f.PublishedVolume();
    std::vector<std::pair<std::string, std::string> > mounts;
    mounts.push_back(std::make_pair(std::string("/data"), parent.id));
    const orchestrator::SandboxMetadata source = SourceWith(mounts);

    const core::Expected<PreparedVolumeForkSpecs, VolumeApiError> prepared =
        PrepareVolumeForkSpecs(f.manager.get(), source, 1);
    MT_EXPECT_TRUE(prepared.ok());
    if (!prepared.ok()) return;

    const std::string clone_id =
        prepared.value().specs[0].volume_mounts.find("/data")->second;

    // The clone is leased to the child that was going to run, so a bare delete
    // is refused -- which is exactly why cleanup has to release first.
    MT_EXPECT_TRUE(!f.manager->Remove(clone_id).ok());

    CleanupForkVolumeChildren(f.manager.get(), prepared.value().children);
    MT_EXPECT_TRUE(!HasVolume(f, clone_id));
}

MT_TEST(cleanup_tolerates_a_null_manager_and_empty_input) {
    // Called on paths that are already failing, so it must not add a second
    // failure of its own.
    CleanupForkVolumeChildren(0, ForkVolumeChildren());
    Fixture f;
    CleanupForkVolumeChildren(f.manager.get(), ForkVolumeChildren());

    // An entry with no volumes is a child that cloned nothing, which is the
    // normal shape for a source holding only read-only volumes.
    ForkVolumeChildren empty_entry;
    empty_entry[core::SandboxId::Fresh().ToString()] = std::vector<std::string>();
    CleanupForkVolumeChildren(f.manager.get(), empty_entry);
}

MT_TEST(preparing_without_a_manager_is_an_error_not_a_silent_share) {
    const orchestrator::SandboxMetadata source =
        SourceWith(std::vector<std::pair<std::string, std::string> >());
    // The handler refuses earlier than this, but the function is reachable on
    // its own and must not hand back specs that share the parent's volumes.
    const core::Expected<PreparedVolumeForkSpecs, VolumeApiError> prepared =
        PrepareVolumeForkSpecs(0, source, 1);
    MT_EXPECT_TRUE(!prepared.ok());
    if (!prepared.ok()) MT_EXPECT_EQ(prepared.error().code, 500);
}

MT_TEST(clone_names_are_unique_and_valid_volume_components) {
    Fixture f;
    const VolumeRecord parent = f.PublishedVolume();
    std::vector<std::pair<std::string, std::string> > mounts;
    mounts.push_back(std::make_pair(std::string("/data"), parent.id));
    const orchestrator::SandboxMetadata source = SourceWith(mounts);

    const core::Expected<PreparedVolumeForkSpecs, VolumeApiError> prepared =
        PrepareVolumeForkSpecs(f.manager.get(), source, 3);
    MT_EXPECT_TRUE(prepared.ok());
    if (!prepared.ok()) return;

    std::vector<std::string> names;
    for (std::size_t i = 0; i < prepared.value().specs.size(); ++i) {
        const std::string clone_id =
            prepared.value().specs[i].volume_mounts.find("/data")->second;
        const agentenv::volume::VolumeResult<VolumeRecord> clone = f.manager->Get(clone_id);
        MT_EXPECT_TRUE(clone.ok());
        if (!clone.ok()) continue;
        // `volume-fork-<32 hex>`. The uuid is rendered without hyphens so the
        // whole thing reads as one token, and the name has to pass the
        // component check -- a name the catalog rejects would fail the create.
        MT_EXPECT_TRUE(clone.value().name.find("volume-fork-") == 0);
        MT_EXPECT_TRUE(agentenv::volume::IsValidVolumeComponent(clone.value().name));
        names.push_back(clone.value().name);
    }
    // Distinct, or the second create would hit a name conflict and the fork
    // would fail for a reason that has nothing to do with the request.
    std::sort(names.begin(), names.end());
    MT_EXPECT_TRUE(std::unique(names.begin(), names.end()) == names.end());
}

MT_TEST(each_child_gets_its_own_lease_on_its_own_clone) {
    Fixture f;
    const VolumeRecord parent = f.PublishedVolume();
    std::vector<std::pair<std::string, std::string> > mounts;
    mounts.push_back(std::make_pair(std::string("/data"), parent.id));
    const orchestrator::SandboxMetadata source = SourceWith(mounts);

    const core::Expected<PreparedVolumeForkSpecs, VolumeApiError> prepared =
        PrepareVolumeForkSpecs(f.manager.get(), source, 2);
    MT_EXPECT_TRUE(prepared.ok());
    if (!prepared.ok()) return;

    for (std::size_t i = 0; i < prepared.value().specs.size(); ++i) {
        const orchestrator::SandboxForkChildSpec& spec = prepared.value().specs[i];
        const std::string clone_id = spec.volume_mounts.find("/data")->second;
        const agentenv::volume::VolumeResult<VolumeRecord> clone =
            f.manager->Get(clone_id);
        MT_EXPECT_TRUE(clone.ok());
        if (!clone.ok()) continue;
        // Reserved for the child that will mount it, not left unowned. An
        // unreserved exclusive volume could be handed to another sandbox
        // between here and the fork actually starting.
        MT_EXPECT_TRUE(clone.value().reserved_by_sandbox_id.has_value());
        if (clone.value().reserved_by_sandbox_id.has_value()) {
            MT_EXPECT_EQ(*clone.value().reserved_by_sandbox_id,
                         spec.sandbox_id.ToString());
        }
    }

    // The parent keeps its own volume throughout: forking must not move the
    // parent's lease to a child.
    const agentenv::volume::VolumeResult<VolumeRecord> after = f.manager->Get(parent.id);
    MT_EXPECT_TRUE(after.ok());
    if (after.ok()) {
        MT_EXPECT_TRUE(after.value().id == parent.id);
    }
}

MT_MAIN
