// SPDX-License-Identifier: MIT
// Rust: src/template/build_spec.rs
//
// Porting note on the builder style: Rust takes `mut self` and returns `Self`,
// which is a move-based chain. C++11 cannot express that cheaply, so each
// setter mutates and returns `*this` by reference. Chaining still reads the
// same (`spec.Run("a").Env("K", "v")`), but the object is modified in place
// rather than threaded through — so a `TemplateBuildSpec` must not be shared
// while being configured.
#ifndef AGENTENV_TEMPLATE_BUILD_SPEC_H_
#define AGENTENV_TEMPLATE_BUILD_SPEC_H_

#include <cstddef>
#include <ostream>
#include <string>
#include <vector>

#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/sandbox/types.h"
#include "agentenv/snapshot/types.h"
#include "agentenv/template/errors.h"

namespace agentenv {
namespace tpl {

/// Rust struct `ImageConfigEntry` (src/types/image_configs.rs).
struct ImageConfigEntry {
    /// Unset for the rootfs; set to the Firecracker drive id for a volume.
    core::Optional<std::string> drive_id;
    std::string mount_path;
    core::Json config;

    bool operator==(const ImageConfigEntry& o) const;
    bool operator!=(const ImageConfigEntry& o) const { return !(*this == o); }
};

/// Rust newtype `ImageConfigs(Vec<ImageConfigEntry>)`, `#[serde(transparent)]`.
class ImageConfigs {
 public:
    ImageConfigs() {}

    /// Rust `ImageConfigs::add`. Pass an unset `drive_id` for the rootfs.
    void Add(const core::Optional<std::string>& drive_id, const std::string& mount_path,
             const core::Json& config);

    bool IsEmpty() const { return entries_.empty(); }
    std::size_t size() const { return entries_.size(); }
    const std::vector<ImageConfigEntry>& entries() const { return entries_; }

    bool operator==(const ImageConfigs& o) const { return entries_ == o.entries_; }
    bool operator!=(const ImageConfigs& o) const { return !(*this == o); }

 private:
    std::vector<ImageConfigEntry> entries_;
};

/// Rust enum `TemplateBuildRootfsBase`.
struct TemplateBuildRootfsBase {
    enum class Kind { Ext4, Overlaybd };

    Kind kind = Kind::Ext4;
    /// `Ext4 { image_path }`.
    std::string image_path;
    /// `Overlaybd { image_config_path, image_configs }`.
    std::string image_config_path;
    ImageConfigs image_configs;

    static TemplateBuildRootfsBase Ext4(const std::string& image_path);
    static TemplateBuildRootfsBase Overlaybd(const std::string& image_config_path,
                                             const ImageConfigs& image_configs);

    bool is_ext4() const { return kind == Kind::Ext4; }
    bool is_overlaybd() const { return kind == Kind::Overlaybd; }
};

/// Rust enum `TemplateBuildStepKind`.
enum class TemplateBuildStepKind {
    Run,
    Env,
    Workdir,
    User,
    ExposedPort,
    Volume,
    Label,
};

/// Rust struct `TemplateBuildStep`.
struct TemplateBuildStep {
    TemplateBuildStepKind kind = TemplateBuildStepKind::Run;
    /// `Run { cmd }` / `Workdir { path }` / `User { value }` /
    /// `ExposedPort { port }` / `Volume { path }` all carry a single string;
    /// `Env`/`Label` carry a pair. Flattened rather than one struct per kind.
    std::string value;
    std::string key;

    /// Rust `source_step`: 1-based position of the client-visible step this
    /// came from. The e2b front-end expands one request step (a multi-pair
    /// ENV/LABEL) into several internal steps, so failed-step reporting must
    /// not count internal positions. Unset means the step maps 1:1 to its
    /// position, which holds for every other front-end.
    core::Optional<std::size_t> source_step;

    static TemplateBuildStep Run(const std::string& cmd);
    static TemplateBuildStep Env(const std::string& key, const std::string& value);
    static TemplateBuildStep Workdir(const std::string& path);
    static TemplateBuildStep User(const std::string& value);
    static TemplateBuildStep ExposedPort(const std::string& port);
    static TemplateBuildStep Volume(const std::string& path);
    static TemplateBuildStep Label(const std::string& key, const std::string& value);

    bool operator==(const TemplateBuildStep& o) const;
    bool operator!=(const TemplateBuildStep& o) const { return !(*this == o); }
};

std::ostream& operator<<(std::ostream& os, TemplateBuildStepKind kind);
std::ostream& operator<<(std::ostream& os, const TemplateBuildStep& step);

/// Rust struct `TemplateBuildSpec` — describes the desired template build
/// independently from storage and runtime details.
class TemplateBuildSpec {
 public:
    /// Rust `TemplateBuildSpec::new` / `Default`.
    TemplateBuildSpec() {}

    // --- rootfs base --------------------------------------------------------

    /// Rust `from_existing_rootfs`.
    TemplateBuildSpec& FromExistingRootfs(const std::string& path);
    /// Rust `from_overlaybd_config` — equivalent to resolving with no configs.
    TemplateBuildSpec& FromOverlaybdConfig(const std::string& image_config_path);
    /// Rust `with_resolved_overlaybd_image`.
    TemplateBuildSpec& WithResolvedOverlaybdImage(const std::string& image_config_path,
                                                  const ImageConfigs& image_configs);

    // --- steps --------------------------------------------------------------

    TemplateBuildSpec& Run(const std::string& cmd);
    TemplateBuildSpec& Env(const std::string& key, const std::string& value);
    TemplateBuildSpec& Workdir(const std::string& path);
    TemplateBuildSpec& User(const std::string& value);
    TemplateBuildSpec& ExposedPort(const std::string& port);
    TemplateBuildSpec& Volume(const std::string& path);
    TemplateBuildSpec& Label(const std::string& key, const std::string& value);

    /// Rust `apt` — appends one `apt-get install` step for all packages, after
    /// dropping blank entries. Appends nothing when nothing survives filtering.
    TemplateBuildSpec& Apt(const std::vector<std::string>& packages);

    // --- metadata -----------------------------------------------------------

    /// Rust `alias` — stored unvalidated; `ParsedAlias` is what validates.
    TemplateBuildSpec& Alias(const std::string& alias);
    /// Rust `resources`. `disk_size_mib` stays 0: disk size is determined
    /// after the build.
    TemplateBuildSpec& Resources(uint32_t cpu_count, uint32_t memory_mib);
    /// Rust `start_cmd` — a blank command clears the override.
    TemplateBuildSpec& StartCmd(const std::string& cmd);
    /// Rust `ready_cmd` — a blank command clears the override.
    TemplateBuildSpec& ReadyCmd(const std::string& cmd);
    /// Rust `with_startup_shell`.
    TemplateBuildSpec& WithStartupShell(const std::string& shell);
    /// Rust `with_base_context`.
    TemplateBuildSpec& WithBaseContext(const snapshot::CommandContext& context);

    /// Rust `stamp_source_steps` — stamps the steps appended since `from` with
    /// the client step number they came from.
    TemplateBuildSpec& StampSourceSteps(std::size_t from, std::size_t source_step);

    // --- accessors ----------------------------------------------------------

    /// Rust `parsed_alias` — validates the alias, mapping a parse failure onto
    /// `InvalidInput`. An unset alias is a successful empty result.
    TemplateBuildResult<core::Optional<snapshot::SnapshotAlias> > ParsedAlias() const;

    /// Rust `rootfs_base_ref`.
    const core::Optional<TemplateBuildRootfsBase>& RootfsBase() const { return rootfs_base_; }
    /// Rust `step_count`.
    std::size_t StepCount() const { return steps_.size(); }
    /// Rust `steps`.
    const std::vector<TemplateBuildStep>& Steps() const { return steps_; }
    /// Rust `resources_ref`.
    const core::Optional<sandbox::SandboxResources>& ResourcesRef() const { return resources_; }
    /// Rust `start_cmd_ref`.
    const core::Optional<std::string>& StartCmdRef() const { return start_cmd_; }
    /// Rust `ready_cmd_ref`.
    const core::Optional<std::string>& ReadyCmdRef() const { return ready_cmd_; }
    /// Rust `startup_shell`.
    const core::Optional<std::string>& StartupShell() const { return startup_shell_; }
    /// Rust `base_context_ref`.
    const core::Optional<snapshot::CommandContext>& BaseContextRef() const {
        return base_context_;
    }
    /// Rust `overrides_startup`.
    bool OverridesStartup() const { return start_cmd_.has_value() || ready_cmd_.has_value(); }

 private:
    core::Optional<TemplateBuildRootfsBase> rootfs_base_;
    std::vector<TemplateBuildStep> steps_;
    core::Optional<std::string> alias_;
    core::Optional<sandbox::SandboxResources> resources_;
    core::Optional<std::string> start_cmd_;
    core::Optional<std::string> ready_cmd_;
    core::Optional<std::string> startup_shell_;
    core::Optional<snapshot::CommandContext> base_context_;
};

}  // namespace tpl
}  // namespace agentenv
#endif  // AGENTENV_TEMPLATE_BUILD_SPEC_H_
