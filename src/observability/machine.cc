// SPDX-License-Identifier: MIT
// Rust: src/observability/machine.rs
#include "agentenv/observability/machine.h"

#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>

namespace agentenv {
namespace observability {

core::Optional<std::string> FirstCpuinfoValue(const std::string& cpuinfo,
                                              const std::vector<std::string>& keys) {
    std::istringstream ss(cpuinfo);
    std::string line;
    while (std::getline(ss, line)) {
        std::string::size_type colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string raw_key = line.substr(0, colon);
        // trim key
        std::string::size_type ks = raw_key.find_first_not_of(" \t");
        std::string::size_type ke = raw_key.find_last_not_of(" \t");
        if (ks == std::string::npos) continue;
        std::string key = raw_key.substr(ks, ke - ks + 1);

        bool matched = false;
        for (const auto& candidate : keys) {
            if (key == candidate) { matched = true; break; }
        }
        if (!matched) continue;

        std::string raw_val = line.substr(colon + 1);
        std::string::size_type vs = raw_val.find_first_not_of(" \t");
        if (vs == std::string::npos) continue;
        std::string::size_type ve = raw_val.find_last_not_of(" \t\r\n");
        if (ve == std::string::npos) continue;
        std::string value = raw_val.substr(vs, ve - vs + 1);
        if (value.empty()) continue;
        return value;
    }
    return core::nullopt;
}

MachineInfo DetectMachineInfo() {
    std::ifstream f("/proc/cpuinfo");
    std::ostringstream ss;
    if (f) ss << f.rdbuf();
    const std::string cpuinfo = ss.str();

    // Rust: first_cpuinfo_value(&cpuinfo, &["cpu family", "CPU architecture"])
    std::vector<std::string> family_keys;
    family_keys.push_back("cpu family");
    family_keys.push_back("CPU architecture");

    std::vector<std::string> model_keys;
    model_keys.push_back("model");
    model_keys.push_back("CPU part");

    std::vector<std::string> name_keys;
    name_keys.push_back("model name");
    name_keys.push_back("Hardware");

    MachineInfo m;
    core::Optional<std::string> family = FirstCpuinfoValue(cpuinfo, family_keys);
    m.cpu_family = family ? *family : "unknown";

    core::Optional<std::string> model = FirstCpuinfoValue(cpuinfo, model_keys);
    m.cpu_model = model ? *model : "unknown";

    core::Optional<std::string> name = FirstCpuinfoValue(cpuinfo, name_keys);
    m.cpu_model_name = name ? *name : "unknown";

    // Rust: std::env::consts::ARCH — use compile-time macro equivalents.
#if defined(__x86_64__) || defined(_M_X64)
    m.cpu_architecture = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    m.cpu_architecture = "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
    m.cpu_architecture = "arm";
#elif defined(__riscv)
    m.cpu_architecture = "riscv64";
#else
    m.cpu_architecture = "unknown";
#endif

    // cpu_config_json left unset — populated out-of-band by the reporter.
    return m;
}

core::Optional<std::string> DumpCpuConfig(const std::string& path) {
    // Rust: `tokio::task::spawn_blocking` → plain blocking call here.
    // argv: <path> template dump -o /dev/stdout
    std::string cmd = path + " template dump -o /dev/stdout 2>/dev/null";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return core::nullopt;

    std::string result;
    char buf[4096];
    while (::fgets(buf, sizeof(buf), pipe)) result += buf;
    int rc = ::pclose(pipe);
    if (rc != 0 || result.empty()) return core::nullopt;
    return result;
}

}  // namespace observability
}  // namespace agentenv
