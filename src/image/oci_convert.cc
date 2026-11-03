// SPDX-License-Identifier: MIT
// Rust: src/image/oci_image.rs — the layer conversion pipeline.
#include <sys/stat.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/image/commit_index.h"
#include "agentenv/image/oci_image.h"
#include "agentenv/storage/overlaybd/tools_oci.h"

namespace agentenv {
namespace image {

namespace {

namespace overlaybd = storage::overlaybd::tools;

const char* const kConvertedLayerP2pProtocol = "agentenv-oci-layer-v1";
const char* const kConvertedLayerP2pKeyPrefix = "oci-layer/v1";
const int64_t kOciLayerBlobPollIntervalMs = 50;
const std::size_t kMaxBuilderLayers = 1024;

std::string TrimAscii(const std::string& text) {
    const char* const kSpace = " \t\n\r\v\f";
    const std::size_t begin = text.find_first_not_of(kSpace);
    if (begin == std::string::npos) return std::string();
    return text.substr(begin, text.find_last_not_of(kSpace) - begin + 1);
}

bool IsUtf8(const std::string& bytes) { return core::FromUtf8Lossy(bytes) == bytes; }

/// Rust `layer_blob_is_ready`.
core::Expected<bool, std::string> LayerBlobIsReady(const std::string& path,
                                                   uint64_t expected_size) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) return false;
        return core::make_unexpected("stat OCI layer blob " + path + ": " + std::strerror(errno));
    }
    return S_ISREG(st.st_mode) && static_cast<uint64_t>(st.st_size) == expected_size;
}

/// Rust `OverlaybdLayerConverter`.
class OverlaybdLayerConverter : public LayerConverter {
 public:
    OverlaybdLayerConverter(const OverlaybdConversionEnv& conversion, uint64_t virtual_size_gib)
        : tools_(overlaybd::OverlaybdTools::FromOverlaybdInstallRoot(conversion.install_root)),
          global_config_path_(conversion.global_config),
          virtual_size_gib_(virtual_size_gib) {}

    core::Expected<std::string, std::string> ConvertLayer(
        const LayerConversionRequest& request) override {
        AGENTENV_DEBUG("converting OCI layer to overlaybd: idx="
                       << request.idx << " digest=" << request.layer.digest
                       << " size=" << request.layer.size
                       << " media_type=" << request.layer.media_type);
        const core::Expected<core::Unit, std::string> created =
            core::fs::CreateDirAll(request.layer_work);
        if (!created.ok()) {
            return core::make_unexpected("create layer work dir " + request.layer_work + ": " +
                                         created.error());
        }

        overlaybd::ConvertLayerRequest convert;
        convert.work_dir = request.layer_work;
        convert.input_layer_path = request.blob_path;
        convert.global_config_path = global_config_path_;
        convert.virtual_size_gib = virtual_size_gib_;
        convert.mkfs = request.mkfs;
        convert.lower_layers = request.lower_paths;
        convert.uuid = request.layer_uuid.ToString();
        if (request.parent_uuid.has_value()) convert.parent_uuid = request.parent_uuid->ToString();

        const core::Expected<overlaybd::ConvertLayerResult, overlaybd::ToolExecutionError> result =
            tools_.ConvertLocalOciLayerToOverlaybd(convert);
        if (!result.ok()) {
            std::ostringstream os;
            os << "convert layer " << request.idx << " (" << request.layer.digest
               << "): " << result.error().message;
            return core::make_unexpected(os.str());
        }
        return result.value().layer_commit;
    }

 private:
    overlaybd::OverlaybdTools tools_;
    std::string global_config_path_;
    uint64_t virtual_size_gib_;
};

/// Rust `ContentLayerSource`.
class ContentLayerSource : public LayerBlobSource {
 public:
    ContentLayerSource(const BuildkitContent& content, const std::string& work)
        : content_(content), work_(work) {}

    core::Expected<std::string, ImageError> WaitLayerBlob(
        std::size_t idx, const OciLayerDescriptor& layer) override {
        const std::string path = core::fs::Join(work_, "blob-" + std::to_string(idx));
        const core::Expected<core::Unit, std::string> downloaded =
            content_.Download(layer.digest, layer.size, path);
        if (!downloaded.ok()) return core::make_unexpected(OtherError(downloaded.error()));
        return path;
    }

 private:
    BuildkitContent content_;
    std::string work_;
};

/// Rust `convert_standard_oci_layers_pipeline`. The producer is aborted on both
/// paths; a failure to abort only warns, since the conversion's own outcome is
/// what the caller needs.
core::Expected<std::vector<LocalLayer>, ImageError> ConvertStandardOciLayersPipeline(
    const OciManifest& manifest, const std::string& image_ref, const std::string& work_root,
    const OverlaybdConversionEnv& conversion, ImageConversion* sink, LayerConverter* converter) {
    const std::string layout_dir = core::fs::Join(work_root, "oci");
    const core::Expected<std::shared_ptr<RegctlImageCopyProducer>, ImageError> producer =
        RegctlImageCopyProducer::Start(conversion.regctl_binary, image_ref, layout_dir,
                                       conversion.registry_config);
    if (!producer.ok()) return core::make_unexpected(OtherError(producer.error().reason));

    core::Expected<std::vector<LocalLayer>, ImageError> result =
        ConvertStandardOciLayersPipelineInner(manifest, image_ref, work_root, conversion, sink,
                                              converter, producer.value().get());

    const core::Expected<core::Unit, std::string> aborted = producer.value()->Abort();
    if (!aborted.ok()) {
        AGENTENV_WARN("failed to cleanly abort background regctl image copy after "
                      << (result.ok() ? "successful conversion" : "conversion failure")
                      << ": error=" << aborted.error());
    }
    return result;
}

}  // namespace

// ---- keys ----

std::string LayerConversionKey::ToJson() const {
    std::ostringstream os;
    os << "{\"sourceLayerDigest\":" << core::JsonQuote(source_layer_digest)
       << ",\"converterId\":" << core::JsonQuote(converter_id)
       << ",\"virtualSizeGib\":" << virtual_size_gib
       << ",\"mkfs\":" << (mkfs ? "true" : "false") << ",\"parentCommitDigest\":";
    if (parent_commit_digest.has_value()) {
        os << core::JsonQuote(*parent_commit_digest);
    } else {
        os << "null";
    }
    os << ",\"expectedLayerUuid\":" << core::JsonQuote(expected_layer_uuid.ToString()) << "}";
    return os.str();
}

p2p::P2pArtifactKey LayerConversionKey::P2pKey() const {
    return std::string(kConvertedLayerP2pKeyPrefix) + "/" + core::Sha256Hex(ToJson());
}

ConvertedLayerP2pMetadata ConvertedLayerP2pMetadata::FromLocalLayer(const LocalLayer& layer) {
    ConvertedLayerP2pMetadata metadata;
    metadata.protocol = kConvertedLayerP2pProtocol;
    metadata.commit_digest = layer.digest;
    metadata.size = layer.size;
    return metadata;
}

core::Expected<ConvertedLayerP2pMetadata, std::string> ConvertedLayerP2pMetadata::Parse(
    const core::Json& raw) {
    const std::string context = "parse converted image layer P2P metadata";
    if (raw.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(context + ": expected an object");
    }
    const core::Json& protocol = raw["protocol"];
    const core::Json& commit_digest = raw["commitDigest"];
    const core::Json& size = raw["size"];
    if (protocol.kind() != core::Json::Kind::String ||
        commit_digest.kind() != core::Json::Kind::String ||
        size.kind() != core::Json::Kind::Int || size.as_int() < 0) {
        return core::make_unexpected(context + ": missing or invalid field");
    }
    if (protocol.as_string() != kConvertedLayerP2pProtocol) {
        return core::make_unexpected(
            std::string("unexpected converted image layer P2P metadata protocol"));
    }
    ConvertedLayerP2pMetadata metadata;
    metadata.protocol = protocol.as_string();
    metadata.commit_digest = commit_digest.as_string();
    metadata.size = static_cast<uint64_t>(size.as_int());
    return metadata;
}

core::Json ConvertedLayerP2pMetadata::ToJson() const {
    core::Json out;
    out["protocol"] = core::Json(protocol);
    out["commitDigest"] = core::Json(commit_digest);
    out["size"] = core::Json(static_cast<int64_t>(size));
    return out;
}

LayerConversionKey MakeLayerConversionKey(const OciLayerDescriptor& layer,
                                          const std::string& converter_id, bool mkfs,
                                          const core::Optional<std::string>& parent_commit_digest,
                                          const core::Uuid& expected_layer_uuid) {
    LayerConversionKey key;
    key.source_layer_digest = layer.digest;
    key.converter_id = converter_id;
    key.virtual_size_gib = kLayerVirtualSizeGib;
    key.mkfs = mkfs;
    key.parent_commit_digest = parent_commit_digest;
    key.expected_layer_uuid = expected_layer_uuid;
    return key;
}

// ---- StagingDir ----

struct StagingDir::Holder {
    std::string path;
    ~Holder() {
        if (path.empty()) return;
        const core::Expected<core::Unit, std::string> removed = core::fs::RemoveDirAll(path);
        if (!removed.ok()) {
            AGENTENV_WARN("failed to remove staging dir " << path << ": " << removed.error());
        }
    }
};

StagingDir::StagingDir(const std::string& path) : holder_(std::make_shared<Holder>()) {
    holder_->path = path;
}

const std::string& StagingDir::Path() const { return holder_->path; }

// ---- pipeline ----

core::Expected<core::Optional<std::vector<LocalLayer> >, std::string> CachedStandardOciLowers(
    const OciManifest& manifest, const OverlaybdConversionEnv& conversion,
    ImageConversion* sink) {
    typedef core::Optional<std::vector<LocalLayer> > Result;
    std::vector<LocalLayer> lowers;
    lowers.reserve(manifest.layers.size());
    core::Optional<std::string> parent_commit_digest;
    for (std::size_t idx = 0; idx < manifest.layers.size(); ++idx) {
        const OciLayerDescriptor& layer = manifest.layers[idx];
        const LayerConversionKey key =
            MakeLayerConversionKey(layer, conversion.converter_id, idx == 0, parent_commit_digest,
                                   UuidFromLayerDigest(layer.digest));
        const core::Expected<core::Optional<LocalLayer>, std::string> cached =
            sink->LookupConvertedLayer(key);
        if (!cached.ok()) return core::make_unexpected(cached.error());
        if (!cached.value().has_value()) return Result();
        parent_commit_digest = core::Optional<std::string>(cached.value()->digest);
        lowers.push_back(*cached.value());
    }
    return Result(lowers);
}

core::Expected<std::vector<LocalLayer>, ImageError> ConvertStandardOciLayersPipelineInner(
    const OciManifest& manifest, const std::string& image_ref, const std::string& work_root,
    const OverlaybdConversionEnv& conversion, ImageConversion* sink, LayerConverter* converter,
    LayerBlobSource* producer) {
    std::vector<std::string> lower_paths;
    std::vector<LocalLayer> lowers;
    lower_paths.reserve(manifest.layers.size());
    lowers.reserve(manifest.layers.size());
    core::Optional<core::Uuid> parent_uuid;
    core::Optional<std::string> parent_commit_digest;

    for (std::size_t idx = 0; idx < manifest.layers.size(); ++idx) {
        const OciLayerDescriptor& layer = manifest.layers[idx];
        const core::Uuid layer_uuid = UuidFromLayerDigest(layer.digest);
        const LayerConversionKey key = MakeLayerConversionKey(
            layer, conversion.converter_id, idx == 0, parent_commit_digest, layer_uuid);

        const core::Expected<core::Optional<LocalLayer>, std::string> cached =
            sink->LookupConvertedLayer(key);
        if (!cached.ok()) return core::make_unexpected(OtherError(cached.error()));
        if (cached.value().has_value()) {
            const LocalLayer& hit = *cached.value();
            parent_commit_digest = core::Optional<std::string>(hit.digest);
            parent_uuid = core::Optional<core::Uuid>(layer_uuid);
            lower_paths.push_back(hit.path);
            lowers.push_back(hit);
            continue;
        }

        std::ostringstream label;
        label << idx << " (" << layer.digest << ")";

        // `anyhow::Context` in Rust, which erases the variant: this function
        // returns `anyhow::Result`, so even a 404 from the copy surfaces as
        // `Other` here. Only the manifest fetch, which runs first, reports 4xx.
        const core::Expected<std::string, ImageError> blob_path =
            producer->WaitLayerBlob(idx, layer);
        if (!blob_path.ok()) {
            return core::make_unexpected(OtherError("wait for copied layer " + label.str() + ": " +
                                                    blob_path.error().reason));
        }

        LayerConversionRequest request;
        request.idx = idx;
        request.layer = layer;
        request.blob_path = blob_path.value();
        request.layer_work = core::fs::Join(work_root, "layer-" + std::to_string(idx));
        request.mkfs = idx == 0;
        request.lower_paths = lower_paths;
        request.layer_uuid = layer_uuid;
        request.parent_uuid = parent_uuid;
        const core::Expected<std::string, std::string> layer_commit =
            converter->ConvertLayer(request);
        if (!layer_commit.ok()) {
            return core::make_unexpected(OtherError("convert layer " + label.str() + " for " +
                                                    image_ref + ": " + layer_commit.error()));
        }

        const core::Expected<LocalLayer, std::string> local_layer =
            sink->StoreConvertedLayer(key, layer_commit.value());
        if (!local_layer.ok()) return core::make_unexpected(OtherError(local_layer.error()));
        parent_commit_digest = core::Optional<std::string>(local_layer.value().digest);
        lower_paths.push_back(local_layer.value().path);
        lowers.push_back(local_layer.value());
        parent_uuid = core::Optional<core::Uuid>(layer_uuid);
    }
    return lowers;
}

// ---- RegctlImageCopyProducer ----

core::Expected<std::shared_ptr<RegctlImageCopyProducer>, ImageError>
RegctlImageCopyProducer::Start(const std::string& regctl_binary, const std::string& image_ref,
                               const std::string& layout_dir,
                               const core::Optional<std::string>& registry_config) {
    const core::Expected<core::Unit, ImageError> ensured = EnsureRegctlBinary(regctl_binary);
    if (!ensured.ok()) return core::make_unexpected(ensured.error());

    std::shared_ptr<RegctlImageCopyProducer> producer(new RegctlImageCopyProducer());
    producer->regctl_binary_ = regctl_binary;
    producer->registry_config_ = registry_config;
    producer->image_ref_ = image_ref;
    producer->layout_dir_ = layout_dir;
    producer->backoff_ms_ = kRegctlRetryBaseDelayMs;
    const core::Expected<core::Unit, ImageError> spawned = producer->SpawnChild();
    if (!spawned.ok()) return core::make_unexpected(spawned.error());
    return producer;
}

core::Expected<core::Unit, ImageError> RegctlImageCopyProducer::SpawnChild() {
    RegctlCommand command = MakeRegctlCommand(regctl_binary_, registry_config_);
    command.argv.push_back("image");
    command.argv.push_back("copy");
    command.argv.push_back("--platform");
    command.argv.push_back("local");
    command.argv.push_back(image_ref_);
    command.argv.push_back("ocidir://" + layout_dir_ + ":latest");
    core::process::RunOptions options;
    options.env = command.env;

    const core::Expected<std::shared_ptr<core::process::Child>, std::string> child =
        core::process::Child::Spawn(command.argv, options);
    if (!child.ok()) {
        return core::make_unexpected(OtherError("spawn regctl image copy: " + child.error()));
    }
    child_ = child.value();
    ++attempts_started_;
    return core::Unit();
}

core::Expected<std::string, ImageError> RegctlImageCopyProducer::WaitLayerBlob(
    std::size_t idx, const OciLayerDescriptor& layer) {
    const core::Expected<std::string, std::string> blob_path =
        BlobPathForDigest(layout_dir_, layer.digest);
    if (!blob_path.ok()) return core::make_unexpected(OtherError(blob_path.error()));

    for (;;) {
        const core::Expected<bool, std::string> ready =
            LayerBlobIsReady(blob_path.value(), layer.size);
        if (!ready.ok()) return core::make_unexpected(OtherError(ready.error()));
        if (ready.value()) {
            AGENTENV_DEBUG("OCI layer blob is ready in background image-copy layout: idx="
                           << idx << " digest=" << layer.digest
                           << " path=" << blob_path.value());
            return blob_path.value();
        }

        core::Optional<int> status;
        if (child_) {
            const core::Expected<core::Optional<int>, std::string> polled = child_->TryWait();
            if (!polled.ok()) {
                return core::make_unexpected(
                    OtherError("poll regctl image copy: " + polled.error()));
            }
            status = polled.value();
        }

        if (status.has_value()) {
            if (*status != 0) {
                const std::string stderr_text =
                    TrimAscii(core::FromUtf8Lossy(child_->StderrBytes()));
                if (RegctlStderrIsNotFound(stderr_text)) {
                    return core::make_unexpected(NotFound(
                        "regctl image copy reported the OCI resource does not exist: " +
                        stderr_text));
                }
                if (attempts_started_ < kRegctlRetryAttempts) {
                    AGENTENV_WARN(
                        "regctl image copy failed before needed layer was ready; retrying: "
                        "attempt="
                        << attempts_started_ << " idx=" << idx << " digest=" << layer.digest
                        << " error=" << stderr_text);
                    std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms_));
                    backoff_ms_ *= 2;
                    const core::Expected<core::Unit, ImageError> respawned = SpawnChild();
                    if (!respawned.ok()) return core::make_unexpected(respawned.error());
                    continue;
                }
                std::ostringstream os;
                os << "regctl image copy failed after " << kRegctlRetryAttempts
                   << " attempts with Some(" << *status << "): " << stderr_text;
                return core::make_unexpected(OtherError(os.str()));
            }

            const core::Expected<bool, std::string> final_ready =
                LayerBlobIsReady(blob_path.value(), layer.size);
            if (!final_ready.ok()) return core::make_unexpected(OtherError(final_ready.error()));
            if (final_ready.value()) return blob_path.value();
            std::ostringstream os;
            os << "regctl image copy completed but layer " << idx << " (" << layer.digest
               << ") is missing or has unexpected size at " << blob_path.value();
            return core::make_unexpected(OtherError(os.str()));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(kOciLayerBlobPollIntervalMs));
    }
}

core::Expected<core::Unit, std::string> RegctlImageCopyProducer::Abort() {
    if (!child_) return core::Unit();
    child_->StartKill();
    const core::Expected<int, std::string> status = child_->Wait();
    if (!status.ok()) {
        return core::make_unexpected("wait killed regctl image copy: " + status.error());
    }
    // Joins the pipe drain, so no thread outlives the abort.
    child_->StderrBytes();
    child_.reset();
    return core::Unit();
}

// ---- entry points ----

namespace {

core::Expected<ResolvedImage, ImageError> ConvertFetched(
    const std::string& image_ref, const FetchedManifest& fetched,
    const OverlaybdConversionEnv& conversion, ImageConversion* sink,
    const std::string& host_arch, LayerConverter* converter_override) {
    // `FetchOciManifest` resolved the manifest for this host arch. Validated
    // again because the background copy uses `--platform local`; an unsupported
    // host architecture should fail before starting conversion work.
    const core::Expected<std::string, std::string> arch = HostArchToOci(host_arch);
    if (!arch.ok()) return core::make_unexpected(OtherError(arch.error()));
    const OciManifest& manifest = fetched.manifest;

    if (fetched.format == ImageFormat::OverlaybdNative) {
        const core::Expected<ParsedImageRef, std::string> parsed = ParseImageRef(image_ref);
        if (!parsed.ok()) return core::make_unexpected(OtherError(parsed.error()));
        ResolvedImage resolved;
        resolved.kind = ResolvedImage::kRemote;
        resolved.repo_blob_url =
            "https://" + parsed.value().host + "/v2/" + parsed.value().repository + "/blobs";
        AGENTENV_INFO("source image is overlaybd-native; skipping blob download -- runtime "
                      "registryfs_v2 will fetch on demand: image="
                      << image_ref << " layers=" << manifest.layers.size()
                      << " repo_blob_url=" << resolved.repo_blob_url);
        for (std::size_t i = 0; i < manifest.layers.size(); ++i) {
            RemoteLayer layer;
            layer.digest = manifest.layers[i].digest;
            layer.size = manifest.layers[i].size;
            layer.dir = sink->RemoteLayerDirHint(manifest.layers[i].digest);
            resolved.remote_layers.push_back(layer);
        }
        return resolved;
    }

    // Only reached when the digest-qualified image.json cache is missing or
    // stale. Cache-hit descriptors computed here are persisted into the
    // regenerated image.json, so later sandbox starts reuse that config instead
    // of hashing these commits again.
    const core::Expected<core::Optional<std::vector<LocalLayer> >, std::string> cached =
        CachedStandardOciLowers(manifest, conversion, sink);
    if (!cached.ok()) return core::make_unexpected(OtherError(cached.error()));
    ResolvedImage resolved;
    resolved.kind = ResolvedImage::kLocal;
    if (cached.value().has_value()) {
        AGENTENV_INFO("source image is standard OCI and all overlaybd layer commits are cached; "
                      "skipping blob download: image="
                      << image_ref << " layers=" << cached.value()->size());
        resolved.local_layers = *cached.value();
        return resolved;
    }

    AGENTENV_INFO("source image is standard OCI; starting background image copy because at "
                  "least one converted layer is missing: image="
                  << image_ref << " layers=" << manifest.layers.size());
    const core::Expected<StagingDir, std::string> work = sink->CreateTempStagingDir();
    if (!work.ok()) return core::make_unexpected(OtherError(work.error()));

    OverlaybdLayerConverter default_converter(conversion, kLayerVirtualSizeGib);
    LayerConverter* converter =
        converter_override != 0 ? converter_override : &default_converter;
    const core::Expected<std::vector<LocalLayer>, ImageError> lowers =
        ConvertStandardOciLayersPipeline(manifest, fetched.selected_image_ref,
                                         work.value().Path(), conversion, sink, converter);
    if (!lowers.ok()) return core::make_unexpected(lowers.error());
    resolved.local_layers = lowers.value();
    return resolved;
}

}  // namespace

core::Expected<ResolvedImage, ImageError> ConvertFetchedOciImageToOverlaybd(
    const std::string& image_ref, const FetchedManifest& fetched,
    const OverlaybdConversionEnv& conversion, ImageConversion* sink,
    const std::string& host_arch) {
    return ConvertFetched(image_ref, fetched, conversion, sink, host_arch, 0);
}

core::Expected<ResolvedImage, ImageError> ConvertFetchedOciImageToOverlaybdWith(
    const std::string& image_ref, const FetchedManifest& fetched,
    const OverlaybdConversionEnv& conversion, ImageConversion* sink,
    const std::string& host_arch, LayerConverter* converter) {
    return ConvertFetched(image_ref, fetched, conversion, sink, host_arch, converter);
}

core::Expected<std::pair<FetchedManifest, ImageResolutionMetadata>, std::string>
FetchContentManifest(const BuildkitContent& content, const std::string& digest_in,
                     const std::string& arch_in) {
    const core::Expected<std::string, std::string> arch_result = HostArchToOci(arch_in);
    if (!arch_result.ok()) return core::make_unexpected(arch_result.error());
    const std::string& arch = arch_result.value();

    std::string digest = digest_in;
    for (std::size_t depth = 0; depth < kMaxIndexResolutionDepth; ++depth) {
        const core::Expected<std::string, std::string> bytes = content.Metadata(digest);
        if (!bytes.ok()) return core::make_unexpected(bytes.error());
        if (!IsUtf8(bytes.value())) {
            return core::make_unexpected(std::string("builder manifest is not UTF-8"));
        }
        const core::Expected<core::Json, core::AnyError> value = core::Json::Parse(bytes.value());
        if (!value.ok()) return core::make_unexpected(value.error().chain());

        const bool has_manifests =
            value.value().kind() == core::Json::Kind::Object &&
            value.value().as_object().find("manifests") != value.value().as_object().end();
        if (has_manifests) {
            const core::Expected<OciIndex, std::string> index = ParseOciIndex(value.value());
            if (!index.ok()) return core::make_unexpected(index.error());
            const core::Expected<std::size_t, std::string> selected =
                SelectManifestForPlatform(index.value().manifests, arch, "linux");
            if (!selected.ok()) return core::make_unexpected(selected.error());
            digest = index.value().manifests[selected.value()].digest;
            continue;
        }

        const core::Json& schema_version = value.value()["schemaVersion"];
        if (schema_version.kind() != core::Json::Kind::Int || schema_version.as_int() != 2) {
            return core::make_unexpected(std::string("expected OCI schema version 2"));
        }
        const core::Expected<OciManifest, std::string> manifest = ParseOciManifest(value.value());
        if (!manifest.ok()) return core::make_unexpected(manifest.error());

        const core::Expected<ImageFormat, ImageError> format = ClassifyManifest(manifest.value());
        if (!format.ok()) return core::make_unexpected(format.error().reason);
        if (format.value() != ImageFormat::StandardOci) {
            return core::make_unexpected(std::string("builder must export standard OCI layers"));
        }
        if (manifest.value().layers.size() > kMaxBuilderLayers) {
            return core::make_unexpected(std::string("image exceeds 1024 layers"));
        }
        uint64_t total = 0;
        for (std::size_t i = 0; i < manifest.value().layers.size(); ++i) {
            const uint64_t size = manifest.value().layers[i].size;
            if (size > kMaxImageBytes - total) {
                return core::make_unexpected(
                    std::string("image exceeds 64 GiB of compressed layers"));
            }
            total += size;
        }
        for (std::size_t i = 0; i < manifest.value().layers.size(); ++i) {
            const core::Expected<core::Unit, std::string> valid =
                ValidateDigest(manifest.value().layers[i].digest);
            if (!valid.ok()) return core::make_unexpected(valid.error());
        }

        const core::Expected<std::string, std::string> config =
            content.Metadata(manifest.value().config.digest);
        if (!config.ok()) return core::make_unexpected(config.error());
        if (!IsUtf8(config.value())) {
            return core::make_unexpected(std::string("builder image config is not UTF-8"));
        }
        const core::Expected<core::Json, core::AnyError> config_value =
            core::Json::Parse(config.value());
        if (!config_value.ok()) return core::make_unexpected(config_value.error().chain());
        const core::Json& os_field = config_value.value()["os"];
        const core::Json& arch_field = config_value.value()["architecture"];
        if (os_field != core::Json("linux") || arch_field != core::Json(arch)) {
            return core::make_unexpected("built image must target linux/" + arch);
        }
        const core::Expected<ImageResolutionMetadata, std::string> metadata =
            ParseOciImageConfig(config.value());
        if (!metadata.ok()) return core::make_unexpected(metadata.error());

        FetchedManifest fetched;
        fetched.manifest_digest = digest;
        fetched.selected_image_ref = digest;
        fetched.format = ImageFormat::StandardOci;
        fetched.manifest = manifest.value();
        return std::make_pair(fetched, metadata.value());
    }
    return core::make_unexpected("builder image index nesting exceeds " +
                                 std::to_string(kMaxIndexResolutionDepth));
}

core::Expected<ResolvedImage, ImageError> ConvertContentImageWith(
    const BuildkitContent& content, const FetchedManifest& fetched,
    const OverlaybdConversionEnv& conversion, ImageConversion* sink, LayerConverter* converter) {
    const core::Expected<StagingDir, std::string> work = sink->CreateTempStagingDir();
    if (!work.ok()) return core::make_unexpected(OtherError(work.error()));

    ContentLayerSource producer(content, work.value().Path());
    const core::Expected<std::vector<LocalLayer>, ImageError> layers =
        ConvertStandardOciLayersPipelineInner(fetched.manifest, fetched.manifest_digest,
                                              work.value().Path(), conversion, sink, converter,
                                              &producer);
    if (!layers.ok()) return core::make_unexpected(layers.error());
    ResolvedImage resolved;
    resolved.kind = ResolvedImage::kLocal;
    resolved.local_layers = layers.value();
    return resolved;
}

core::Expected<ResolvedImage, ImageError> ConvertContentImage(
    const BuildkitContent& content, const FetchedManifest& fetched,
    const OverlaybdConversionEnv& conversion, ImageConversion* sink) {
    OverlaybdLayerConverter converter(conversion, kLayerVirtualSizeGib);
    return ConvertContentImageWith(content, fetched, conversion, sink, &converter);
}

}  // namespace image
}  // namespace agentenv
