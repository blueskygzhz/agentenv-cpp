// SPDX-License-Identifier: MIT
// Rust: crates/linux-cap/src/lib.rs
#include "agentenv/core/capability.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "agentenv/core/fs.h"

namespace agentenv {
namespace core {
namespace cap {
namespace {

/// Rust `PR_CAP_AMBIENT*` / `LINUX_CAPABILITY_VERSION_3`. Spelled out rather
/// than taken from <sys/prctl.h> and <linux/capability.h> because the values
/// are stable kernel ABI and the headers are not always present.
const int kPrCapAmbient = 47;
const unsigned long kPrCapAmbientRaise = 2;
const unsigned long kPrCapAmbientClearAll = 4;
const uint32_t kLinuxCapabilityVersion3 = 0x20080522u;

struct CapabilityHeader {
    uint32_t version;
    int32_t pid;
};

struct CapabilityData {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
};

/// Rust `invalid_capability()`.
std::string InvalidCapability() { return std::strerror(EINVAL); }

std::string LastOsError(const char* op) {
    std::ostringstream oss;
    oss << op << ": " << std::strerror(errno);
    return oss.str();
}

/// Rust `capability_mask` — a single 64-bit mask, used for queries.
Expected<uint64_t, std::string> CapabilityMask(int capability) {
    if (capability < 0 || static_cast<std::size_t>(capability) >= kMaxCapability) {
        return make_unexpected(InvalidCapability());
    }
    return static_cast<uint64_t>(1) << capability;
}

/// Rust `field(status, name)` inside `from_proc_status`.
Expected<uint64_t, std::string> ParseStatusField(const std::string& status, const char* name) {
    const std::size_t name_len = std::strlen(name);
    std::size_t line_begin = 0;

    while (line_begin <= status.size()) {
        std::size_t line_end = status.find('\n', line_begin);
        if (line_end == std::string::npos) line_end = status.size();

        if (line_end - line_begin >= name_len &&
            status.compare(line_begin, name_len, name) == 0) {
            std::string value = status.substr(line_begin + name_len, line_end - line_begin - name_len);
            // Rust `.trim()` before `from_str_radix`.
            std::size_t begin = 0;
            while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
                ++begin;
            }
            std::size_t end = value.size();
            while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
                --end;
            }
            value = value.substr(begin, end - begin);

            if (value.empty()) {
                std::ostringstream oss;
                oss << name << " is not valid hexadecimal";
                return make_unexpected(oss.str());
            }
            // strtoull would accept "0x" prefixes and leading signs; the field
            // is plain hex, so it is validated explicitly.
            uint64_t parsed = 0;
            for (std::size_t i = 0; i < value.size(); ++i) {
                const char c = value[i];
                int digit;
                if (c >= '0' && c <= '9') {
                    digit = c - '0';
                } else if (c >= 'a' && c <= 'f') {
                    digit = c - 'a' + 10;
                } else if (c >= 'A' && c <= 'F') {
                    digit = c - 'A' + 10;
                } else {
                    std::ostringstream oss;
                    oss << name << " is not valid hexadecimal";
                    return make_unexpected(oss.str());
                }
                parsed = (parsed << 4) | static_cast<uint64_t>(digit);
            }
            return parsed;
        }

        if (line_end == status.size()) break;
        line_begin = line_end + 1;
    }

    std::ostringstream oss;
    oss << name << " is missing from /proc/self/status";
    return make_unexpected(oss.str());
}

}  // namespace

Expected<std::vector<uint32_t>, std::string> CapabilityMasks(
    const std::vector<int>& capabilities) {
    std::vector<uint32_t> masks(kCapabilitySetCount, 0);
    for (std::size_t i = 0; i < capabilities.size(); ++i) {
        const int capability = capabilities[i];
        if (capability < 0 || static_cast<std::size_t>(capability) >= kMaxCapability) {
            return make_unexpected(InvalidCapability());
        }
        const std::size_t index = static_cast<std::size_t>(capability) / kCapabilitiesPerSet;
        const std::size_t bit = static_cast<std::size_t>(capability) % kCapabilitiesPerSet;
        masks[index] |= static_cast<uint32_t>(1) << bit;
    }
    return masks;
}

Expected<CapabilitySets, std::string> CapabilitySets::FromProcStatus(const std::string& status) {
    const Expected<uint64_t, std::string> inheritable = ParseStatusField(status, "CapInh:");
    if (!inheritable.ok()) return make_unexpected(inheritable.error());
    const Expected<uint64_t, std::string> permitted = ParseStatusField(status, "CapPrm:");
    if (!permitted.ok()) return make_unexpected(permitted.error());
    const Expected<uint64_t, std::string> effective = ParseStatusField(status, "CapEff:");
    if (!effective.ok()) return make_unexpected(effective.error());

    CapabilitySets sets;
    sets.inheritable_ = inheritable.value();
    sets.permitted_ = permitted.value();
    sets.effective_ = effective.value();
    return sets;
}

Expected<CapabilitySets, std::string> CapabilitySets::Current() {
    const Expected<std::string, std::string> status = fs::ReadToString("/proc/self/status");
    if (!status.ok()) return make_unexpected(status.error());
    return FromProcStatus(status.value());
}

Expected<bool, std::string> CapabilitySets::IsDelegable(int capability) const {
    const Expected<uint64_t, std::string> mask = CapabilityMask(capability);
    if (!mask.ok()) return make_unexpected(mask.error());
    const uint64_t bit = mask.value();
    return (inheritable_ & bit) != 0 && (permitted_ & bit) != 0 && (effective_ & bit) != 0;
}

Expected<bool, std::string> CapabilitySets::Effective(int capability) const {
    const Expected<uint64_t, std::string> mask = CapabilityMask(capability);
    if (!mask.ok()) return make_unexpected(mask.error());
    return (effective_ & mask.value()) != 0;
}

Expected<bool, std::string> HasCapabilities(const std::vector<int>& capabilities) {
    const Expected<CapabilitySets, std::string> sets = CapabilitySets::Current();
    if (!sets.ok()) return make_unexpected(sets.error());

    bool present = true;
    for (std::size_t i = 0; i < capabilities.size(); ++i) {
        const Expected<bool, std::string> delegable = sets.value().IsDelegable(capabilities[i]);
        if (!delegable.ok()) return make_unexpected(delegable.error());
        present = present && delegable.value();
    }
    return present;
}

Expected<bool, std::string> HasEffectiveCapabilities(const std::vector<int>& capabilities) {
    const Expected<CapabilitySets, std::string> sets = CapabilitySets::Current();
    if (!sets.ok()) return make_unexpected(sets.error());

    bool present = true;
    for (std::size_t i = 0; i < capabilities.size(); ++i) {
        const Expected<bool, std::string> effective = sets.value().Effective(capabilities[i]);
        if (!effective.ok()) return make_unexpected(effective.error());
        present = present && effective.value();
    }
    return present;
}

Expected<Unit, std::string> ClearAmbientCapabilities() {
    if (::prctl(kPrCapAmbient, kPrCapAmbientClearAll, 0, 0, 0) != 0) {
        return make_unexpected(LastOsError("clear ambient capabilities"));
    }
    return Unit();
}

namespace {

/// Rust `set_capability_sets`.
Expected<Unit, std::string> SetCapabilitySets(const std::vector<int>& capabilities) {
    const Expected<std::vector<uint32_t>, std::string> masks = CapabilityMasks(capabilities);
    if (!masks.ok()) return make_unexpected(masks.error());

    CapabilityHeader header;
    header.version = kLinuxCapabilityVersion3;
    header.pid = 0;  // 0 means "this thread"

    CapabilityData data[kCapabilitySetCount];
    for (std::size_t i = 0; i < kCapabilitySetCount; ++i) {
        // All three sets get the same mask: the thread keeps only what it was
        // asked for, and keeps it effective so it can act immediately.
        data[i].effective = masks.value()[i];
        data[i].permitted = masks.value()[i];
        data[i].inheritable = masks.value()[i];
    }

    if (::syscall(SYS_capset, &header, data) != 0) {
        return make_unexpected(LastOsError("capset"));
    }
    return Unit();
}

/// Rust `raise_ambient_capability`.
Expected<Unit, std::string> RaiseAmbientCapability(int capability) {
    const Expected<uint64_t, std::string> validated = CapabilityMask(capability);
    if (!validated.ok()) return make_unexpected(validated.error());

    if (::prctl(kPrCapAmbient, kPrCapAmbientRaise, static_cast<unsigned long>(capability), 0, 0) !=
        0) {
        return make_unexpected(LastOsError("raise ambient capability"));
    }
    return Unit();
}

}  // namespace

Expected<Unit, std::string> ConfigureCurrentProcessCapabilities(
    const std::vector<int>& capabilities) {
    // Validate before mutating anything, so a bad request cannot leave the
    // thread half-configured.
    const Expected<std::vector<uint32_t>, std::string> validated = CapabilityMasks(capabilities);
    if (!validated.ok()) return make_unexpected(validated.error());

    const Expected<Unit, std::string> cleared = ClearAmbientCapabilities();
    if (!cleared.ok()) return cleared;

    const Expected<Unit, std::string> configured = SetCapabilitySets(capabilities);
    if (!configured.ok()) return configured;

    for (std::size_t i = 0; i < capabilities.size(); ++i) {
        const Expected<Unit, std::string> raised = RaiseAmbientCapability(capabilities[i]);
        if (!raised.ok()) return raised;
    }
    return Unit();
}

const char* CapabilityName(int capability) {
    switch (capability) {
        case kCapNetAdmin:
            return "CAP_NET_ADMIN";
        case kCapSysAdmin:
            return "CAP_SYS_ADMIN";
        default:
            return "unknown capability";
    }
}

}  // namespace cap
}  // namespace core
}  // namespace agentenv
