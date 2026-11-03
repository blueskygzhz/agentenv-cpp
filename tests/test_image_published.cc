// SPDX-License-Identifier: MIT
// Rust: src/image/mod.rs — `mod tests`.
#include "microtest.h"

#include <algorithm>
#include <string>
#include <vector>

#include "agentenv/core/digest.h"
#include "agentenv/core/json.h"
#include "agentenv/image/published_image.h"

using namespace agentenv;
using namespace agentenv::image;

namespace {

core::Json ParseJson(const std::string& text) {
    const core::Expected<core::Json, core::AnyError> value = core::Json::Parse(text);
    MT_EXPECT_TRUE(value.ok());
    return value.value();
}

/// Rust test helper `image`.
PublishedImage Image() {
    const char* const data[] = {"base", "delta"};
    std::vector<snapshot::OverlaybdLayerRef> layers;
    for (std::size_t i = 0; i < 2; ++i) {
        snapshot::ManagedLayer layer;
        layer.digest = core::Sha256Digest(std::string(data[i]));
        layer.size = std::string(data[i]).size();
        layers.push_back(snapshot::OverlaybdLayerRef::Managed(layer));
    }
    return PublishedImage::New(
        "amd64", layers,
        ParseJson("{\"Env\": [\"A=1\", \"B=2\"], \"Cmd\": [\"serve\"], "
                  "\"Labels\": {\"b\": \"2\", \"a\": \"1\"}}"));
}

std::string DigestOf(const PublishedImage& image) {
    const core::Expected<std::pair<std::string, std::string>, std::string> encoded =
        image.Encode();
    MT_EXPECT_TRUE(encoded.ok());
    return encoded.value().first;
}

}  // namespace

MT_TEST(identity_binds_ordered_layers_architecture_and_runtime_config) {
    const PublishedImage original = Image();
    const core::Expected<std::pair<std::string, std::string>, std::string> encoded =
        original.Encode();
    MT_EXPECT_TRUE(encoded.ok());
    const std::string digest = encoded.value().first;
    const std::string bytes = encoded.value().second;
    MT_EXPECT_TRUE(core::Sha256Digest(bytes) == digest);

    const core::Expected<PublishedImage, std::string> decoded =
        PublishedImage::Decode(digest, bytes);
    MT_EXPECT_TRUE(decoded.ok());
    MT_EXPECT_TRUE(DigestOf(decoded.value()) == digest);

    for (int change = 0; change < 4; ++change) {
        PublishedImage modified = original;
        switch (change) {
            case 0:
                std::reverse(modified.layers.begin(), modified.layers.end());
                break;
            case 1:
                modified.architecture = "arm64";
                break;
            case 2:
                modified.config["Cmd"] = ParseJson("[\"another-command\"]");
                break;
            default:
                modified.config["Env"] = ParseJson("[\"B=2\", \"A=1\"]");
                break;
        }
        MT_EXPECT_TRUE(DigestOf(modified) != digest);
    }

    PublishedImage reordered = original;
    reordered.config =
        ParseJson("{\"Labels\":{\"a\":\"1\",\"b\":\"2\"},\"Cmd\":[\"serve\"],\"Env\":[\"A=1\",\"B=2\"]}");
    MT_EXPECT_TRUE(DigestOf(reordered) == digest);

    MT_EXPECT_TRUE(!PublishedImage::Decode(digest, original.ToJson().ToStringPretty()).ok());
}

MT_TEST(rejects_invalid_and_node_local_descriptions) {
    core::Json value = Image().ToJson();
    value["localPath"] = core::Json("/node-a/image.json");
    const std::string bytes = value.ToString();
    MT_EXPECT_TRUE(!PublishedImage::Decode(core::Sha256Digest(bytes), bytes).ok());

    PublishedImage invalid = Image();
    invalid.schema_version = 2;
    MT_EXPECT_TRUE(!invalid.Encode().ok());
    invalid = Image();
    invalid.layers.clear();
    MT_EXPECT_TRUE(!invalid.Encode().ok());
}

// ---- C++-side checks on the wire shape ------------------------------------

MT_TEST(encoding_is_camel_case_with_sorted_keys_and_externally_tagged_layers) {
    snapshot::ManagedLayer layer;
    layer.digest = core::Sha256Digest(std::string("base"));
    layer.size = 4;
    const PublishedImage image = PublishedImage::New(
        "arm64", std::vector<snapshot::OverlaybdLayerRef>(1, snapshot::OverlaybdLayerRef::Managed(layer)),
        ParseJson("{\"b\":1,\"a\":2}"));
    const core::Expected<std::pair<std::string, std::string>, std::string> encoded =
        image.Encode();
    MT_EXPECT_TRUE(encoded.ok());
    MT_EXPECT_TRUE(encoded.value().second ==
                   "{\"architecture\":\"arm64\",\"config\":{\"a\":2,\"b\":1},\"layers\":[{"
                   "\"Managed\":{\"digest\":\"" +
                       layer.digest +
                       "\",\"size\":4}}],\"os\":\"linux\",\"schemaVersion\":1}");
}

MT_TEST(decode_rejects_bad_digest_missing_fields_and_external_layers) {
    const std::string bytes = Image().Encode().value().second;
    // Not a lowercase sha256 digest.
    MT_EXPECT_TRUE(!PublishedImage::Decode("sha256:ABC", bytes).ok());

    core::Json missing = Image().ToJson();
    core::JsonObject object = missing.as_object();
    object.erase("os");
    const std::string missing_bytes = core::Json(object).ToString();
    const core::Expected<PublishedImage, std::string> no_os =
        PublishedImage::Decode(core::Sha256Digest(missing_bytes), missing_bytes);
    MT_EXPECT_TRUE(!no_os.ok());
    MT_EXPECT_TRUE(no_os.error().find("missing field `os`") != std::string::npos);

    snapshot::ExternalLayer external;
    external.digest = core::Sha256Digest(std::string("x"));
    external.repo_blob_url = "https://registry.example.com/v2/x/blobs/" + external.digest;
    external.size = 1;
    PublishedImage foreign = Image();
    foreign.layers.push_back(snapshot::OverlaybdLayerRef::External(external));
    const core::Expected<std::pair<std::string, std::string>, std::string> refused =
        foreign.Encode();
    MT_EXPECT_TRUE(!refused.ok());
    MT_EXPECT_TRUE(refused.error() ==
                   "published images must own their layers in the repository");
}

MT_TEST(image_tag_prefix_matches_rust) {
    MT_EXPECT_TRUE(std::string(kImageTagPrefix) == "aenv/");
}

MT_MAIN
