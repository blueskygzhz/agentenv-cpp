// SPDX-License-Identifier: MIT
// Rust: src/virtualization.rs
#include "agentenv/core/virtualization.h"

namespace agentenv {
namespace core {
namespace {

std::string TrimAsciiLowercase(const std::string& raw) {
    std::size_t begin = 0;
    std::size_t end = raw.size();
    while (begin < end && (raw[begin] == ' ' || raw[begin] == '\t' || raw[begin] == '\r' ||
                           raw[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (raw[end - 1] == ' ' || raw[end - 1] == '\t' || raw[end - 1] == '\r' ||
                           raw[end - 1] == '\n')) {
        --end;
    }
    std::string out = raw.substr(begin, end - begin);
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = static_cast<char>(out[i] - 'A' + 'a');
    }
    return out;
}

}  // namespace

const char* VirtualizationModeToString(VirtualizationMode mode) {
    switch (mode) {
        case VirtualizationMode::Kvm:
            return "kvm";
        case VirtualizationMode::Pvm:
            return "pvm";
    }
    return "kvm";
}

Expected<VirtualizationMode, std::string> VirtualizationModeParse(const std::string& raw) {
    const std::string normalized = TrimAsciiLowercase(raw);
    if (normalized == "kvm") return VirtualizationMode::Kvm;
    if (normalized == "pvm") return VirtualizationMode::Pvm;
    // Mirrors the Rust message, including the quoted `other` (Rust `{other:?}`).
    return make_unexpected(std::string("unsupported virtualization mode \"") + normalized +
                           "\"; expected \"kvm\" or \"pvm\"");
}

}  // namespace core
}  // namespace agentenv
