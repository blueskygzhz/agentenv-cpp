// SPDX-License-Identifier: MIT
// Go upstream: services/scheduler/internal/cpu_template.go.
#include "services/scheduler/cpu_template.h"

#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "agentenv/core/json.h"

namespace agentenv {
namespace services {
namespace scheduler {

namespace {

// ---- Go: the `cpuConfig` / `cpuidModifier` / `registerMod` / `msrModifier`
// structs. Field order here is the declaration order Go marshals in, and the
// serializer below depends on it.

struct RegisterMod {
    std::string register_name;  // json "register"
    std::string bitmap;         // json "bitmap"
};

struct CpuidModifier {
    std::string leaf;     // json "leaf"
    std::string subleaf;  // json "subleaf"
    int64_t flags;        // json "flags" (Go: int)
    std::vector<RegisterMod> modifiers;

    CpuidModifier() : flags(0) {}
};

struct MsrModifier {
    std::string addr;    // json "addr"
    std::string bitmap;  // json "bitmap"
};

struct CpuConfig {
    std::vector<std::string> kvm_capabilities;
    std::vector<CpuidModifier> cpuid_modifiers;
    std::vector<MsrModifier> msr_modifiers;
};

/// Go: `cpuidLeafKey`. Ordered so it can key a `std::map`; the ordering itself
/// is never observable because every output walks `configs[0]` in input order.
struct CpuidLeafKey {
    uint32_t leaf;
    uint32_t subleaf;
    int64_t flags;

    CpuidLeafKey() : leaf(0), subleaf(0), flags(0) {}
    CpuidLeafKey(uint32_t l, uint32_t s, int64_t f) : leaf(l), subleaf(s), flags(f) {}

    bool operator<(const CpuidLeafKey& o) const {
        if (leaf != o.leaf) return leaf < o.leaf;
        if (subleaf != o.subleaf) return subleaf < o.subleaf;
        return flags < o.flags;
    }
};

/// Go: `kvmReadOnlyLeafIDs` — leaves KVM refuses to let userspace override, so
/// emitting them would make Firecracker reject the whole template.
bool IsKvmReadOnlyLeaf(uint32_t leaf) { return leaf == 0xb || leaf == 0x1f; }

std::string ToLowerAscii(const std::string& s) {
    std::string out(s);
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = static_cast<char>(out[i] - 'A' + 'a');
    }
    return out;
}

/// Go `strings.TrimPrefix` — removes the prefix only when it is actually there.
std::string TrimPrefix(const std::string& s, const std::string& prefix) {
    if (s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0) {
        return s.substr(prefix.size());
    }
    return s;
}

/// `strconv.ParseUint(s, base, bits)` for the two bases this file uses.
///
/// Go's strictness is the point: an empty string, a sign, a stray separator or
/// an out-of-range value are all errors, never a silently truncated 0. A
/// bitmap that parsed as 0 by accident would AND away every feature and
/// produce a template that boots nothing.
bool ParseUintStrict(const std::string& s, int base, int bits, uint64_t* out) {
    if (s.empty()) return false;
    const uint64_t limit =
        bits >= 64 ? ~static_cast<uint64_t>(0)
                   : (static_cast<uint64_t>(1) << bits) - static_cast<uint64_t>(1);
    uint64_t acc = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        uint64_t digit;
        if (c >= '0' && c <= '9') {
            digit = static_cast<uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<uint64_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            // `ParseUint` accepts either case for hex digits.
            digit = static_cast<uint64_t>(c - 'A' + 10);
        } else {
            return false;
        }
        if (digit >= static_cast<uint64_t>(base)) return false;
        // Overflow check before multiplying, so a long literal cannot wrap.
        if (acc > (limit - digit) / static_cast<uint64_t>(base)) return false;
        acc = acc * static_cast<uint64_t>(base) + digit;
    }
    *out = acc;
    return true;
}

/// Go: `parseHexUint32` — lowercases *then* trims `0x`, so `0X1F` is accepted.
bool ParseHexUint32(const std::string& s, uint32_t* out) {
    uint64_t v = 0;
    if (!ParseUintStrict(TrimPrefix(ToLowerAscii(s), "0x"), 16, 32, &v)) return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

/// Go: `parseBitmapU32` — trims a *lowercase* `0b` only, without lowercasing
/// first. `0B101` therefore reaches `ParseUint` with the `B` still attached
/// and is rejected. Upstream behaves this way; matching it keeps both sides
/// rejecting the same inputs.
bool ParseBitmapU32(const std::string& s, uint32_t* out) {
    uint64_t v = 0;
    if (!ParseUintStrict(TrimPrefix(s, "0b"), 2, 32, &v)) return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

/// Go: `parseBitmapU64`.
bool ParseBitmapU64(const std::string& s, uint64_t* out) {
    return ParseUintStrict(TrimPrefix(s, "0b"), 2, 64, out);
}

/// Go: `formatBitmap` — `fmt.Sprintf("0b%0*b", bits, v)`.
std::string FormatBitmap(uint64_t v, int bits) {
    std::string digits(static_cast<std::size_t>(bits), '0');
    for (int i = bits - 1; i >= 0; --i) {
        digits[static_cast<std::size_t>(i)] = (v & 1) ? '1' : '0';
        v >>= 1;
    }
    return "0b" + digits;
}

bool MakeCpuidLeafKey(const CpuidModifier& mod, CpuidLeafKey* out, std::string* error) {
    uint32_t leaf = 0;
    if (!ParseHexUint32(mod.leaf, &leaf)) {
        *error = "parse leaf: invalid syntax";
        return false;
    }
    uint32_t subleaf = 0;
    if (!ParseHexUint32(mod.subleaf, &subleaf)) {
        *error = "parse subleaf: invalid syntax";
        return false;
    }
    *out = CpuidLeafKey(leaf, subleaf, mod.flags);
    return true;
}

std::string Itoa(std::size_t v) {
    std::ostringstream os;
    os << v;
    return os.str();
}

// ---- JSON decoding -------------------------------------------------------
// Go uses `json.Unmarshal` into the structs above: unknown fields are ignored,
// absent fields stay at their zero value (nil slices, read as empty), and a
// value of the wrong shape is an error.

bool JsonToString(const core::Json& v, std::string* out) {
    if (v.kind() != core::Json::Kind::String) return false;
    *out = v.as_string();
    return true;
}

/// Go unmarshals `flags` into an `int`: an integral JSON number is fine, a
/// fractional one is not.
bool JsonToInt(const core::Json& v, int64_t* out) {
    if (v.kind() == core::Json::Kind::Int) {
        *out = v.as_int();
        return true;
    }
    if (v.kind() == core::Json::Kind::Double) {
        const double d = v.as_double();
        const int64_t truncated = static_cast<int64_t>(d);
        if (static_cast<double>(truncated) != d) return false;
        *out = truncated;
        return true;
    }
    return false;
}

/// Reads an object member, treating both "absent" and "null" as the zero value
/// the way `encoding/json` does.
const core::Json* Member(const core::JsonObject& obj, const char* key) {
    const core::JsonObject::const_iterator it = obj.find(key);
    if (it == obj.end()) return NULL;
    if (it->second.kind() == core::Json::Kind::Null) return NULL;
    return &it->second;
}

bool DecodeCpuConfig(const core::Json& value, CpuConfig* out, std::string* error) {
    // `json.Unmarshal([]byte("null"), &struct)` succeeds and leaves the zero
    // value, so a literal null is an empty config rather than an error.
    if (value.kind() == core::Json::Kind::Null) return true;
    if (value.kind() != core::Json::Kind::Object) {
        *error = "cannot unmarshal non-object into cpuConfig";
        return false;
    }
    const core::JsonObject& obj = value.as_object();

    if (const core::Json* caps = Member(obj, "kvm_capabilities")) {
        if (caps->kind() != core::Json::Kind::Array) {
            *error = "cannot unmarshal kvm_capabilities into []string";
            return false;
        }
        const core::JsonArray& arr = caps->as_array();
        for (std::size_t i = 0; i < arr.size(); ++i) {
            std::string cap;
            if (!JsonToString(arr[i], &cap)) {
                *error = "cannot unmarshal kvm_capabilities element into string";
                return false;
            }
            out->kvm_capabilities.push_back(cap);
        }
    }

    if (const core::Json* mods = Member(obj, "cpuid_modifiers")) {
        if (mods->kind() != core::Json::Kind::Array) {
            *error = "cannot unmarshal cpuid_modifiers into []cpuidModifier";
            return false;
        }
        const core::JsonArray& arr = mods->as_array();
        for (std::size_t i = 0; i < arr.size(); ++i) {
            if (arr[i].kind() == core::Json::Kind::Null) {
                out->cpuid_modifiers.push_back(CpuidModifier());
                continue;
            }
            if (arr[i].kind() != core::Json::Kind::Object) {
                *error = "cannot unmarshal cpuid_modifiers element into cpuidModifier";
                return false;
            }
            const core::JsonObject& mo = arr[i].as_object();
            CpuidModifier mod;
            if (const core::Json* leaf = Member(mo, "leaf")) {
                if (!JsonToString(*leaf, &mod.leaf)) {
                    *error = "cannot unmarshal leaf into string";
                    return false;
                }
            }
            if (const core::Json* subleaf = Member(mo, "subleaf")) {
                if (!JsonToString(*subleaf, &mod.subleaf)) {
                    *error = "cannot unmarshal subleaf into string";
                    return false;
                }
            }
            if (const core::Json* flags = Member(mo, "flags")) {
                if (!JsonToInt(*flags, &mod.flags)) {
                    *error = "cannot unmarshal flags into int";
                    return false;
                }
            }
            if (const core::Json* regs = Member(mo, "modifiers")) {
                if (regs->kind() != core::Json::Kind::Array) {
                    *error = "cannot unmarshal modifiers into []registerMod";
                    return false;
                }
                const core::JsonArray& ra = regs->as_array();
                for (std::size_t j = 0; j < ra.size(); ++j) {
                    if (ra[j].kind() == core::Json::Kind::Null) {
                        // `json.Unmarshal` turns a null element into the zero
                        // value rather than skipping it.
                        mod.modifiers.push_back(RegisterMod());
                        continue;
                    }
                    if (ra[j].kind() != core::Json::Kind::Object) {
                        *error = "cannot unmarshal modifiers element into registerMod";
                        return false;
                    }
                    const core::JsonObject& ro = ra[j].as_object();
                    RegisterMod rm;
                    if (const core::Json* reg = Member(ro, "register")) {
                        if (!JsonToString(*reg, &rm.register_name)) {
                            *error = "cannot unmarshal register into string";
                            return false;
                        }
                    }
                    if (const core::Json* bm = Member(ro, "bitmap")) {
                        if (!JsonToString(*bm, &rm.bitmap)) {
                            *error = "cannot unmarshal bitmap into string";
                            return false;
                        }
                    }
                    mod.modifiers.push_back(rm);
                }
            }
            out->cpuid_modifiers.push_back(mod);
        }
    }

    if (const core::Json* mods = Member(obj, "msr_modifiers")) {
        if (mods->kind() != core::Json::Kind::Array) {
            *error = "cannot unmarshal msr_modifiers into []msrModifier";
            return false;
        }
        const core::JsonArray& arr = mods->as_array();
        for (std::size_t i = 0; i < arr.size(); ++i) {
            if (arr[i].kind() == core::Json::Kind::Null) {
                out->msr_modifiers.push_back(MsrModifier());
                continue;
            }
            if (arr[i].kind() != core::Json::Kind::Object) {
                *error = "cannot unmarshal msr_modifiers element into msrModifier";
                return false;
            }
            const core::JsonObject& mo = arr[i].as_object();
            MsrModifier mod;
            if (const core::Json* addr = Member(mo, "addr")) {
                if (!JsonToString(*addr, &mod.addr)) {
                    *error = "cannot unmarshal addr into string";
                    return false;
                }
            }
            if (const core::Json* bm = Member(mo, "bitmap")) {
                if (!JsonToString(*bm, &mod.bitmap)) {
                    *error = "cannot unmarshal bitmap into string";
                    return false;
                }
            }
            out->msr_modifiers.push_back(mod);
        }
    }

    return true;
}

// ---- the three intersections --------------------------------------------

/// Go: `intersectCpuidModifiers`.
bool IntersectCpuidModifiers(const std::vector<CpuConfig>& configs,
                             std::vector<CpuidModifier>* out, std::string* error) {
    const std::size_t n = configs.size();

    // Go: `perConfig[i][leafKey][register] = bitmap`. Duplicate leaf entries
    // inside one config merge their registers, and a duplicate register wins
    // with its last value -- Go map assignment, reproduced deliberately.
    std::vector<std::map<CpuidLeafKey, std::map<std::string, uint32_t> > > per_config(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::vector<CpuidModifier>& mods = configs[i].cpuid_modifiers;
        for (std::size_t m = 0; m < mods.size(); ++m) {
            CpuidLeafKey lk;
            std::string why;
            if (!MakeCpuidLeafKey(mods[m], &lk, &why)) {
                *error = "config " + Itoa(i) + " cpuid " + mods[m].leaf + "/" +
                         mods[m].subleaf + ": " + why;
                return false;
            }
            std::map<std::string, uint32_t>& regs = per_config[i][lk];
            for (std::size_t r = 0; r < mods[m].modifiers.size(); ++r) {
                const RegisterMod& rm = mods[m].modifiers[r];
                uint32_t v = 0;
                if (!ParseBitmapU32(rm.bitmap, &v)) {
                    *error = "config " + Itoa(i) + " cpuid " + mods[m].leaf + " " +
                             rm.register_name + " bitmap: invalid syntax";
                    return false;
                }
                regs[rm.register_name] = v;
            }
        }
    }

    // Go: how many configs contain each leaf key.
    std::map<CpuidLeafKey, std::size_t> leaf_count;
    for (std::size_t i = 0; i < n; ++i) {
        for (std::map<CpuidLeafKey, std::map<std::string, uint32_t> >::const_iterator it =
                 per_config[i].begin();
             it != per_config[i].end(); ++it) {
            ++leaf_count[it->first];
        }
    }

    // Walk `configs[0]` in input order so the output is deterministic.
    std::set<CpuidLeafKey> seen_leaf;
    const std::vector<CpuidModifier>& first = configs[0].cpuid_modifiers;
    for (std::size_t m = 0; m < first.size(); ++m) {
        CpuidLeafKey lk;
        std::string why;
        MakeCpuidLeafKey(first[m], &lk, &why);  // already validated above
        if (seen_leaf.count(lk) != 0) continue;
        seen_leaf.insert(lk);
        if (leaf_count[lk] < n) continue;      // not in every config
        if (IsKvmReadOnlyLeaf(lk.leaf)) continue;

        // How many configs carry each register for this leaf.
        std::map<std::string, std::size_t> reg_count;
        for (std::size_t i = 0; i < n; ++i) {
            const std::map<CpuidLeafKey, std::map<std::string, uint32_t> >::const_iterator
                lit = per_config[i].find(lk);
            if (lit == per_config[i].end()) continue;
            for (std::map<std::string, uint32_t>::const_iterator rit = lit->second.begin();
                 rit != lit->second.end(); ++rit) {
                ++reg_count[rit->first];
            }
        }

        // Candidates come from configs[0] only: see the note on
        // `IntersectCpuConfigs` about this being conservative on purpose.
        std::vector<RegisterMod> reg_mods;
        std::set<std::string> seen_reg;
        for (std::size_t r = 0; r < first[m].modifiers.size(); ++r) {
            const std::string& reg = first[m].modifiers[r].register_name;
            if (seen_reg.count(reg) != 0) continue;
            seen_reg.insert(reg);
            if (reg_count[reg] < n) continue;  // not in every config
            // AND across configs, starting from the identity (all ones).
            uint32_t and_val = ~static_cast<uint32_t>(0);
            for (std::size_t i = 0; i < n; ++i) {
                const std::map<CpuidLeafKey,
                               std::map<std::string, uint32_t> >::const_iterator lit =
                    per_config[i].find(lk);
                if (lit == per_config[i].end()) {
                    and_val = 0;  // Go's missing-key zero value
                    continue;
                }
                const std::map<std::string, uint32_t>::const_iterator rit =
                    lit->second.find(reg);
                and_val &= (rit == lit->second.end()) ? 0 : rit->second;
            }
            RegisterMod outm;
            outm.register_name = reg;
            outm.bitmap = FormatBitmap(static_cast<uint64_t>(and_val), 32);
            reg_mods.push_back(outm);
        }

        if (!reg_mods.empty()) {
            CpuidModifier outmod;
            outmod.leaf = first[m].leaf;
            outmod.subleaf = first[m].subleaf;
            outmod.flags = first[m].flags;
            outmod.modifiers = reg_mods;
            out->push_back(outmod);
        }
    }
    return true;
}

/// Go: `intersectMsrModifiers`.
bool IntersectMsrModifiers(const std::vector<CpuConfig>& configs,
                           std::vector<MsrModifier>* out, std::string* error) {
    const std::size_t n = configs.size();

    std::vector<std::map<uint32_t, uint64_t> > per_config(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::vector<MsrModifier>& mods = configs[i].msr_modifiers;
        for (std::size_t m = 0; m < mods.size(); ++m) {
            uint32_t addr = 0;
            if (!ParseHexUint32(mods[m].addr, &addr)) {
                *error = "config " + Itoa(i) + " msr addr \"" + mods[m].addr +
                         "\": invalid syntax";
                return false;
            }
            uint64_t v = 0;
            if (!ParseBitmapU64(mods[m].bitmap, &v)) {
                *error = "config " + Itoa(i) + " msr " + mods[m].addr +
                         " bitmap: invalid syntax";
                return false;
            }
            per_config[i][addr] = v;  // last duplicate wins, as in Go
        }
    }

    std::map<uint32_t, std::size_t> addr_count;
    for (std::size_t i = 0; i < n; ++i) {
        for (std::map<uint32_t, uint64_t>::const_iterator it = per_config[i].begin();
             it != per_config[i].end(); ++it) {
            ++addr_count[it->first];
        }
    }

    std::set<uint32_t> seen_addr;
    const std::vector<MsrModifier>& first = configs[0].msr_modifiers;
    for (std::size_t m = 0; m < first.size(); ++m) {
        uint32_t addr = 0;
        ParseHexUint32(first[m].addr, &addr);  // already validated above
        if (seen_addr.count(addr) != 0) continue;
        seen_addr.insert(addr);
        if (addr_count[addr] < n) continue;
        uint64_t and_val = ~static_cast<uint64_t>(0);
        for (std::size_t i = 0; i < n; ++i) {
            const std::map<uint32_t, uint64_t>::const_iterator it = per_config[i].find(addr);
            and_val &= (it == per_config[i].end()) ? 0 : it->second;
        }
        MsrModifier outmod;
        outmod.addr = first[m].addr;
        outmod.bitmap = FormatBitmap(and_val, 64);
        out->push_back(outmod);
    }
    return true;
}

/// Go: `intersectKvmCapabilities` — capabilities every config lists, sorted.
std::vector<std::string> IntersectKvmCapabilities(const std::vector<CpuConfig>& configs) {
    std::map<std::string, std::size_t> counts;
    for (std::size_t i = 0; i < configs.size(); ++i) {
        std::set<std::string> seen;
        const std::vector<std::string>& caps = configs[i].kvm_capabilities;
        for (std::size_t c = 0; c < caps.size(); ++c) {
            if (seen.insert(caps[c]).second) ++counts[caps[c]];
        }
    }
    // `counts` is already in sorted key order, which is what Go's
    // `sort.Strings` produces after collecting from its map.
    std::vector<std::string> result;
    for (std::map<std::string, std::size_t>::const_iterator it = counts.begin();
         it != counts.end(); ++it) {
        if (it->second == configs.size()) result.push_back(it->first);
    }
    return result;
}

// ---- serialization -------------------------------------------------------

/// `json.Marshal(cpuConfig)`: compact, and with each struct's fields in
/// *declaration* order. `core::JsonObject` is a sorted map and cannot express
/// that, so the writer is by hand -- the same reason `core::JsonQuote` exists.
std::string MarshalCpuConfig(const std::vector<std::string>& caps,
                             const std::vector<CpuidModifier>& cpuid,
                             const std::vector<MsrModifier>& msr) {
    std::ostringstream os;
    os << "{\"kvm_capabilities\":[";
    for (std::size_t i = 0; i < caps.size(); ++i) {
        if (i != 0) os << ",";
        os << core::JsonQuote(caps[i]);
    }
    os << "],\"cpuid_modifiers\":[";
    for (std::size_t i = 0; i < cpuid.size(); ++i) {
        if (i != 0) os << ",";
        os << "{\"leaf\":" << core::JsonQuote(cpuid[i].leaf)
           << ",\"subleaf\":" << core::JsonQuote(cpuid[i].subleaf)
           << ",\"flags\":" << cpuid[i].flags << ",\"modifiers\":[";
        for (std::size_t j = 0; j < cpuid[i].modifiers.size(); ++j) {
            if (j != 0) os << ",";
            os << "{\"register\":" << core::JsonQuote(cpuid[i].modifiers[j].register_name)
               << ",\"bitmap\":" << core::JsonQuote(cpuid[i].modifiers[j].bitmap) << "}";
        }
        os << "]}";
    }
    os << "],\"msr_modifiers\":[";
    for (std::size_t i = 0; i < msr.size(); ++i) {
        if (i != 0) os << ",";
        os << "{\"addr\":" << core::JsonQuote(msr[i].addr)
           << ",\"bitmap\":" << core::JsonQuote(msr[i].bitmap) << "}";
    }
    os << "]}";
    return os.str();
}

}  // namespace

core::Expected<std::string, std::string> IntersectCpuConfigs(
    const std::vector<std::string>& jsons) {
    // Go returns ("", nil) here: no inputs means no constraint was collected,
    // which callers must not confuse with an empty constraint set.
    if (jsons.empty()) return std::string();

    std::vector<CpuConfig> configs(jsons.size());
    for (std::size_t i = 0; i < jsons.size(); ++i) {
        const core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(jsons[i]);
        if (!parsed.ok()) {
            return core::make_unexpected("parse config " + Itoa(i) + ": " +
                                         parsed.error().chain());
        }
        std::string why;
        if (!DecodeCpuConfig(parsed.value(), &configs[i], &why)) {
            return core::make_unexpected("parse config " + Itoa(i) + ": " + why);
        }
    }

    std::vector<CpuidModifier> cpuid_mods;
    std::string error;
    if (!IntersectCpuidModifiers(configs, &cpuid_mods, &error)) {
        return core::make_unexpected(error);
    }
    std::vector<MsrModifier> msr_mods;
    if (!IntersectMsrModifiers(configs, &msr_mods, &error)) {
        return core::make_unexpected(error);
    }

    return MarshalCpuConfig(IntersectKvmCapabilities(configs), cpuid_mods, msr_mods);
}

}  // namespace scheduler
}  // namespace services
}  // namespace agentenv
