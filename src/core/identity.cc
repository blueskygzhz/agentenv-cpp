// SPDX-License-Identifier: MIT
#include "agentenv/core/identity.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

#include "agentenv/core/logging.h"

// Build-time injected identity metadata. Mirrors the Rust build using
// `option_env!("AENV_GIT_COMMIT")` and `env!("CARGO_PKG_VERSION")`.
// These are normally passed by the build system via -D flags.
#ifndef AENV_GIT_COMMIT
#define AENV_GIT_COMMIT ""
#endif
#ifndef AENV_VERSION
#define AENV_VERSION "0.0.0"
#endif

namespace agentenv {
namespace core {

// Parse "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx".
bool Uuid::Parse(const std::string& s, Uuid* out) {
    if (s.size() != 36) return false;
    // Positions of dashes: 8, 13, 18, 23.
    if (s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') return false;
    uint8_t bytes[16] = {};
    int bi = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        int hi, lo;
        char c1 = s[i], c2 = (i + 1 < s.size()) ? s[i + 1] : 0;
        auto hex = [](char c, int* v) {
            if (c >= '0' && c <= '9') { *v = c - '0'; return true; }
            if (c >= 'a' && c <= 'f') { *v = c - 'a' + 10; return true; }
            if (c >= 'A' && c <= 'F') { *v = c - 'A' + 10; return true; }
            return false;
        };
        if (!hex(c1, &hi) || !hex(c2, &lo)) return false;
        bytes[bi++] = static_cast<uint8_t>((hi << 4) | lo);
        ++i;  // consumed 2 chars
    }
    *out = Uuid(bytes);
    return true;
}

std::string Uuid::ToString() const {
    char buf[37];
    static const char* kHex = "0123456789abcdef";
    int p = 0;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) buf[p++] = '-';
        buf[p++] = kHex[bytes_[i] >> 4];
        buf[p++] = kHex[bytes_[i] & 0xf];
    }
    buf[36] = 0;
    return std::string(buf);
}

// UUID v7 per RFC 9562 §5.7. Non-cryptographic randomness suffices for our IDs.
Uuid Uuid::GenV7() {
    using namespace std::chrono;
    // 48-bit unix ms.
    uint64_t ms = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();

    // Thread-local RNG.
    thread_local std::mt19937_64 rng{
        static_cast<uint64_t>(steady_clock::now().time_since_epoch().count())};
    uint64_t rand_a = rng() & 0x0fffull;         // 12 bits
    uint64_t rand_b = rng() & 0x3fffffffffffffffull;  // 62 bits

    uint8_t bytes[16] = {};
    bytes[0] = static_cast<uint8_t>((ms >> 40) & 0xff);
    bytes[1] = static_cast<uint8_t>((ms >> 32) & 0xff);
    bytes[2] = static_cast<uint8_t>((ms >> 24) & 0xff);
    bytes[3] = static_cast<uint8_t>((ms >> 16) & 0xff);
    bytes[4] = static_cast<uint8_t>((ms >>  8) & 0xff);
    bytes[5] = static_cast<uint8_t>((ms >>  0) & 0xff);
    // ver = 7 in high nibble of bytes[6], rand_a in the rest.
    bytes[6] = static_cast<uint8_t>(0x70 | ((rand_a >> 8) & 0x0f));
    bytes[7] = static_cast<uint8_t>(rand_a & 0xff);
    // var = 10b in high 2 bits of bytes[8], rand_b in the rest.
    bytes[8]  = static_cast<uint8_t>(0x80 | ((rand_b >> 56) & 0x3f));
    bytes[9]  = static_cast<uint8_t>((rand_b >> 48) & 0xff);
    bytes[10] = static_cast<uint8_t>((rand_b >> 40) & 0xff);
    bytes[11] = static_cast<uint8_t>((rand_b >> 32) & 0xff);
    bytes[12] = static_cast<uint8_t>((rand_b >> 24) & 0xff);
    bytes[13] = static_cast<uint8_t>((rand_b >> 16) & 0xff);
    bytes[14] = static_cast<uint8_t>((rand_b >>  8) & 0xff);
    bytes[15] = static_cast<uint8_t>((rand_b >>  0) & 0xff);
    return Uuid(bytes);
}

std::ostream& operator<<(std::ostream& os, const Uuid& u) {
    return os << u.ToString();
}

// ---------------------------------------------------------------------------
// Node identity (Rust: src/identity.rs)
// ---------------------------------------------------------------------------

const char* BuildCommit() {
    // Rust: match option_env!("AENV_GIT_COMMIT") { Some(c) if !c.is_empty() => c, _ => "unknown" }
    const char* commit = AENV_GIT_COMMIT;
    if (commit != nullptr && commit[0] != '\0') {
        return commit;
    }
    return "unknown";
}

namespace {

// Trims ASCII whitespace from both ends. Mirrors Rust `str::trim`.
std::string TrimWhitespace(const std::string& s) {
    size_t begin = 0;
    size_t end = s.size();
    auto is_ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
               c == '\f' || c == '\v';
    };
    while (begin < end && is_ws(s[begin])) ++begin;
    while (end > begin && is_ws(s[end - 1])) --end;
    return s.substr(begin, end - begin);
}

// Reads a whole file and returns its trimmed content if non-empty.
// Mirrors `fs::read_to_string(path).ok().map(trim).filter(!is_empty)`.
Optional<std::string> ReadTrimmedFile(const char* path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        return nullopt;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string trimmed = TrimWhitespace(buf.str());
    if (trimmed.empty()) {
        return nullopt;
    }
    return Optional<std::string>(trimmed);
}

}  // namespace

Optional<std::string> ReadHostname() {
    // Rust: env HOSTNAME (non-empty) -> /proc/sys/kernel/hostname -> /etc/hostname
    const char* env_host = std::getenv("HOSTNAME");
    if (env_host != nullptr && env_host[0] != '\0') {
        return Optional<std::string>(std::string(env_host));
    }
    Optional<std::string> proc = ReadTrimmedFile("/proc/sys/kernel/hostname");
    if (proc.has_value()) {
        return proc;
    }
    return ReadTrimmedFile("/etc/hostname");
}

namespace {

// Rust: parse_uuid_with_fallback — parse or warn+nil.
Uuid ParseUuidWithFallback(const char* field,
                           const Optional<std::string>& config_value) {
    if (!config_value.has_value()) {
        return Uuid::Nil();
    }
    const std::string& raw = *config_value;
    Uuid parsed;
    if (Uuid::Parse(raw, &parsed)) {
        return parsed;
    }
    AGENTENV_WARN("invalid UUID for node identity: config_field=" << field
                  << " value=" << raw);
    return Uuid::Nil();
}

// Returns the inner string when present and non-empty. Mirrors Rust
// `opt.clone().filter(|v| !v.is_empty())`.
Optional<std::string> NonEmpty(const Optional<std::string>& v) {
    if (v.has_value() && !v->empty()) {
        return v;
    }
    return nullopt;
}

}  // namespace

NodeIdentity NodeIdentity::FromConfig(const NodeIdentityConfig& config) {
    std::string hostname = ReadHostname().value_or(std::string("unknown"));

    NodeIdentity out;
    // id: config.node_id (non-empty) else hostname.
    Optional<std::string> node_id = NonEmpty(config.node_id);
    out.id = node_id.has_value() ? *node_id : hostname;

    out.cluster_id =
        ParseUuidWithFallback("node_identity.cluster_id", config.cluster_id);

    // service_instance_id: config value (non-empty) else a fresh v7 uuid.
    Optional<std::string> sid = NonEmpty(config.service_instance_id);
    out.service_instance_id =
        sid.has_value() ? *sid : Uuid::GenV7().ToString();

    out.commit = std::string(BuildCommit());
    out.version = std::string(AENV_VERSION);
    return out;
}

}  // namespace core
}  // namespace agentenv
