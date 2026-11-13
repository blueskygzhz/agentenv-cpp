// SPDX-License-Identifier: MIT
// Mirrors services/scheduler/internal/cpu_template_test.go, case for case.
#include "microtest.h"

#include <string>
#include <vector>

#include "agentenv/core/json.h"
#include "services/scheduler/cpu_template.h"

using agentenv::core::Json;
using agentenv::services::scheduler::IntersectCpuConfigs;

// Go: bm32 / bm64 — a Firecracker-style bitmap literal.
static std::string Bm(uint64_t v, int bits) {
    std::string digits(static_cast<std::size_t>(bits), '0');
    for (int i = bits - 1; i >= 0; --i) {
        digits[static_cast<std::size_t>(i)] = (v & 1) ? '1' : '0';
        v >>= 1;
    }
    return "0b" + digits;
}
static std::string Bm32(uint32_t v) { return Bm(v, 32); }
static std::string Bm64(uint64_t v) { return Bm(v, 64); }

struct Reg {
    std::string reg;
    std::string bitmap;
    Reg(const std::string& r, const std::string& b) : reg(r), bitmap(b) {}
};
struct Cpuid {
    std::string leaf;
    std::string subleaf;
    long long flags;
    std::vector<Reg> mods;
    Cpuid(const std::string& l, const std::string& s, const std::vector<Reg>& m)
        : leaf(l), subleaf(s), flags(0), mods(m) {}
};
struct Msr {
    std::string addr;
    std::string bitmap;
    Msr(const std::string& a, const std::string& b) : addr(a), bitmap(b) {}
};

static std::string Q(const std::string& s) { return "\"" + s + "\""; }

/// Go: buildConfig — `json.Marshal(cpuConfig{...})`, i.e. struct declaration
/// order with nil slices normalised to `[]`. Written out by hand so the
/// expected bytes in the string-equality cases below come from the upstream
/// layout rather than from the implementation under test.
static std::string BuildConfig(const std::vector<std::string>& caps,
                               const std::vector<Cpuid>& cpuid,
                               const std::vector<Msr>& msr) {
    std::string out = "{\"kvm_capabilities\":[";
    for (std::size_t i = 0; i < caps.size(); ++i) {
        if (i) out += ",";
        out += Q(caps[i]);
    }
    out += "],\"cpuid_modifiers\":[";
    for (std::size_t i = 0; i < cpuid.size(); ++i) {
        if (i) out += ",";
        out += "{\"leaf\":" + Q(cpuid[i].leaf) + ",\"subleaf\":" + Q(cpuid[i].subleaf) +
               ",\"flags\":0,\"modifiers\":[";
        for (std::size_t j = 0; j < cpuid[i].mods.size(); ++j) {
            if (j) out += ",";
            out += "{\"register\":" + Q(cpuid[i].mods[j].reg) + ",\"bitmap\":" +
                   Q(cpuid[i].mods[j].bitmap) + "}";
        }
        out += "]}";
    }
    out += "],\"msr_modifiers\":[";
    for (std::size_t i = 0; i < msr.size(); ++i) {
        if (i) out += ",";
        out += "{\"addr\":" + Q(msr[i].addr) + ",\"bitmap\":" + Q(msr[i].bitmap) + "}";
    }
    out += "]}";
    return out;
}

static std::vector<std::string> NoCaps() { return std::vector<std::string>(); }
static std::vector<Cpuid> NoCpuid() { return std::vector<Cpuid>(); }
static std::vector<Msr> NoMsr() { return std::vector<Msr>(); }

static std::vector<std::string> Jsons2(const std::string& a, const std::string& b) {
    std::vector<std::string> v;
    v.push_back(a);
    v.push_back(b);
    return v;
}

// Go: parsedResult — intersect, then decode so fields can be inspected.
static Json ParsedResult(const std::vector<std::string>& jsons) {
    const agentenv::core::Expected<std::string, std::string> out = IntersectCpuConfigs(jsons);
    MT_EXPECT_TRUE(out.ok());
    const agentenv::core::Expected<Json, agentenv::core::AnyError> parsed =
        Json::Parse(out.value());
    MT_EXPECT_TRUE(parsed.ok());
    return parsed.value();
}

static const Json& Field(const Json& obj, const char* key) { return obj[key]; }

MT_TEST(cpu_template_empty) {
    const agentenv::core::Expected<std::string, std::string> out =
        IntersectCpuConfigs(std::vector<std::string>());
    MT_EXPECT_TRUE(out.ok());
    MT_EXPECT_TRUE(out.value() == "");
}

MT_TEST(cpu_template_single_drops_kvm_read_only_leaves) {
    std::vector<Reg> eax1;
    eax1.push_back(Reg("eax", Bm32(1)));
    std::vector<Reg> eax2;
    eax2.push_back(Reg("eax", Bm32(2)));

    const Cpuid safe("0x1", "0x0", eax1);
    std::vector<Cpuid> input;
    input.push_back(safe);
    input.push_back(Cpuid("0xb", "0x1", eax2));   // KVM-owned
    input.push_back(Cpuid("0x1f", "0x1", eax2));  // KVM-owned

    std::vector<Cpuid> want_mods;
    want_mods.push_back(safe);

    std::vector<std::string> jsons;
    jsons.push_back(BuildConfig(NoCaps(), input, NoMsr()));

    const agentenv::core::Expected<std::string, std::string> out = IntersectCpuConfigs(jsons);
    MT_EXPECT_TRUE(out.ok());
    // Byte equality, as upstream asserts: this pins the declaration-order
    // field layout as well as the filtering.
    MT_EXPECT_TRUE(out.value() == BuildConfig(NoCaps(), want_mods, NoMsr()));
}

MT_TEST(cpu_template_cpuid_bitmap_and) {
    std::vector<Reg> a;
    a.push_back(Reg("eax", Bm32(0xFFFFFFFFu)));
    std::vector<Reg> b;
    b.push_back(Reg("eax", Bm32(0x0F0F0F0Fu)));
    std::vector<Cpuid> ca;
    ca.push_back(Cpuid("0x1", "0x0", a));
    std::vector<Cpuid> cb;
    cb.push_back(Cpuid("0x1", "0x0", b));

    const Json result = ParsedResult(Jsons2(BuildConfig(NoCaps(), ca, NoMsr()),
                                            BuildConfig(NoCaps(), cb, NoMsr())));
    const Json& mods = Field(result, "cpuid_modifiers");
    MT_EXPECT_TRUE(mods.as_array().size() == 1);
    MT_EXPECT_TRUE(Field(mods.as_array()[0], "modifiers").as_array()[0]["bitmap"].as_string() ==
                   Bm32(0x0F0F0F0Fu));
}

MT_TEST(cpu_template_cpuid_extra_leaf_dropped) {
    std::vector<Reg> eax;
    eax.push_back(Reg("eax", Bm32(0xFFFFu)));
    std::vector<Reg> ebx;
    ebx.push_back(Reg("ebx", Bm32(0xABCDu)));
    const Cpuid leaf1("0x1", "0x0", eax);
    const Cpuid leaf7("0x7", "0x0", ebx);

    std::vector<Cpuid> ca;
    ca.push_back(leaf1);
    ca.push_back(leaf7);
    std::vector<Cpuid> cb;
    cb.push_back(leaf1);

    const Json result = ParsedResult(Jsons2(BuildConfig(NoCaps(), ca, NoMsr()),
                                            BuildConfig(NoCaps(), cb, NoMsr())));
    const agentenv::core::JsonArray& mods = Field(result, "cpuid_modifiers").as_array();
    MT_EXPECT_TRUE(mods.size() == 1);
    for (std::size_t i = 0; i < mods.size(); ++i) {
        MT_EXPECT_TRUE(mods[i]["leaf"].as_string() != "0x7");
    }
}

MT_TEST(cpu_template_cpuid_extra_register_dropped) {
    std::vector<Reg> a;
    a.push_back(Reg("eax", Bm32(0xFu)));
    a.push_back(Reg("ecx", Bm32(0xFu)));
    std::vector<Reg> b;
    b.push_back(Reg("eax", Bm32(0x3u)));
    std::vector<Cpuid> ca;
    ca.push_back(Cpuid("0x1", "0x0", a));
    std::vector<Cpuid> cb;
    cb.push_back(Cpuid("0x1", "0x0", b));

    const Json result = ParsedResult(Jsons2(BuildConfig(NoCaps(), ca, NoMsr()),
                                            BuildConfig(NoCaps(), cb, NoMsr())));
    const agentenv::core::JsonArray& mods = Field(result, "cpuid_modifiers").as_array();
    MT_EXPECT_TRUE(mods.size() == 1);
    const agentenv::core::JsonArray& regs = mods[0]["modifiers"].as_array();
    MT_EXPECT_TRUE(regs.size() == 1);
    MT_EXPECT_TRUE(regs[0]["register"].as_string() == "eax");
    MT_EXPECT_TRUE(regs[0]["bitmap"].as_string() == Bm32(0x3u));
}

MT_TEST(cpu_template_msr_bitmap_and) {
    std::vector<Msr> a;
    a.push_back(Msr("0x10a", Bm64(0xFFFFFFFFFFFFFFFFull)));
    std::vector<Msr> b;
    b.push_back(Msr("0x10a", Bm64(0xFFull)));

    const Json result = ParsedResult(Jsons2(BuildConfig(NoCaps(), NoCpuid(), a),
                                            BuildConfig(NoCaps(), NoCpuid(), b)));
    const agentenv::core::JsonArray& mods = Field(result, "msr_modifiers").as_array();
    MT_EXPECT_TRUE(mods.size() == 1);
    MT_EXPECT_TRUE(mods[0]["bitmap"].as_string() == Bm64(0xFFull));
}

MT_TEST(cpu_template_msr_extra_addr_dropped) {
    const Msr common("0x10a", Bm64(0xFFull));
    const Msr extra("0x10b", Bm64(0xFFull));
    std::vector<Msr> a;
    a.push_back(common);
    a.push_back(extra);
    std::vector<Msr> b;
    b.push_back(common);

    const Json result = ParsedResult(Jsons2(BuildConfig(NoCaps(), NoCpuid(), a),
                                            BuildConfig(NoCaps(), NoCpuid(), b)));
    const agentenv::core::JsonArray& mods = Field(result, "msr_modifiers").as_array();
    MT_EXPECT_TRUE(mods.size() == 1);
    for (std::size_t i = 0; i < mods.size(); ++i) {
        MT_EXPECT_TRUE(mods[i]["addr"].as_string() != "0x10b");
    }
}

MT_TEST(cpu_template_kvm_capabilities) {
    std::vector<std::string> a;
    a.push_back("cap-a");
    a.push_back("cap-b");
    std::vector<std::string> b;
    b.push_back("cap-b");
    b.push_back("cap-c");

    const Json result = ParsedResult(Jsons2(BuildConfig(a, NoCpuid(), NoMsr()),
                                            BuildConfig(b, NoCpuid(), NoMsr())));
    const agentenv::core::JsonArray& caps = Field(result, "kvm_capabilities").as_array();
    MT_EXPECT_TRUE(caps.size() == 1);
    MT_EXPECT_TRUE(caps[0].as_string() == "cap-b");
}

MT_TEST(cpu_template_three_configs) {
    std::vector<std::string> jsons;
    const uint32_t vals[3] = {0xFFu, 0x0Fu, 0x03u};
    for (int i = 0; i < 3; ++i) {
        std::vector<Reg> r;
        r.push_back(Reg("eax", Bm32(vals[i])));
        std::vector<Cpuid> c;
        c.push_back(Cpuid("0x1", "0x0", r));
        jsons.push_back(BuildConfig(NoCaps(), c, NoMsr()));
    }
    const Json result = ParsedResult(jsons);
    MT_EXPECT_TRUE(Field(result, "cpuid_modifiers").as_array()[0]["modifiers"]
                       .as_array()[0]["bitmap"]
                       .as_string() == Bm32(0x03u));
}

MT_TEST(cpu_template_hex_normalization) {
    // "0x01" and "0x1" must identify the same leaf.
    std::vector<Reg> a;
    a.push_back(Reg("eax", Bm32(0xFFu)));
    std::vector<Reg> b;
    b.push_back(Reg("eax", Bm32(0x0Fu)));
    std::vector<Cpuid> ca;
    ca.push_back(Cpuid("0x01", "0x00", a));
    std::vector<Cpuid> cb;
    cb.push_back(Cpuid("0x1", "0x0", b));

    const Json result = ParsedResult(Jsons2(BuildConfig(NoCaps(), ca, NoMsr()),
                                            BuildConfig(NoCaps(), cb, NoMsr())));
    const agentenv::core::JsonArray& mods = Field(result, "cpuid_modifiers").as_array();
    MT_EXPECT_TRUE(mods.size() == 1);
    MT_EXPECT_TRUE(mods[0]["modifiers"].as_array()[0]["bitmap"].as_string() == Bm32(0x0Fu));
}

MT_TEST(cpu_template_identical_configs) {
    std::vector<Reg> r;
    r.push_back(Reg("eax", Bm32(0xABCD1234u)));
    std::vector<Cpuid> c;
    c.push_back(Cpuid("0x1", "0x0", r));
    const std::string cfg = BuildConfig(NoCaps(), c, NoMsr());

    const Json result = ParsedResult(Jsons2(cfg, cfg));
    MT_EXPECT_TRUE(Field(result, "cpuid_modifiers").as_array()[0]["modifiers"]
                       .as_array()[0]["bitmap"]
                       .as_string() == Bm32(0xABCD1234u));
}

MT_TEST(cpu_template_invalid_json) {
    MT_EXPECT_TRUE(!IntersectCpuConfigs(Jsons2("{}", "not-json")).ok());
}

MT_TEST(cpu_template_invalid_bitmap) {
    std::vector<Reg> bad_r;
    bad_r.push_back(Reg("eax", "0bXXXX"));
    std::vector<Cpuid> bad_c;
    bad_c.push_back(Cpuid("0x1", "0x0", bad_r));
    std::vector<Reg> good_r;
    good_r.push_back(Reg("eax", Bm32(0xFFu)));
    std::vector<Cpuid> good_c;
    good_c.push_back(Cpuid("0x1", "0x0", good_r));

    MT_EXPECT_TRUE(!IntersectCpuConfigs(Jsons2(BuildConfig(NoCaps(), good_c, NoMsr()),
                                               BuildConfig(NoCaps(), bad_c, NoMsr())))
                        .ok());
}

MT_TEST(cpu_template_empty_slices_serialize_as_arrays) {
    const std::string cfg = BuildConfig(NoCaps(), NoCpuid(), NoMsr());
    const agentenv::core::Expected<std::string, std::string> out =
        IntersectCpuConfigs(Jsons2(cfg, cfg));
    MT_EXPECT_TRUE(out.ok());
    // Upstream guards against the three fields marshalling as `null`; here the
    // whole document is pinned, which also covers key order.
    MT_EXPECT_TRUE(out.value() ==
                   "{\"kvm_capabilities\":[],\"cpuid_modifiers\":[],\"msr_modifiers\":[]}");
}

// ---- Behaviour upstream leaves untested but that the port must not change --

MT_TEST(cpu_template_rejects_uppercase_binary_prefix) {
    // `parseBitmapU32` trims a lowercase "0b" only, so "0B..." reaches
    // ParseUint with the prefix attached and is rejected. Asserted so the
    // asymmetry with the hex parser stays deliberate.
    std::vector<Reg> r;
    r.push_back(Reg("eax", "0B1010"));
    std::vector<Cpuid> c;
    c.push_back(Cpuid("0x1", "0x0", r));
    std::vector<std::string> jsons;
    jsons.push_back(BuildConfig(NoCaps(), c, NoMsr()));
    MT_EXPECT_TRUE(!IntersectCpuConfigs(jsons).ok());
}

MT_TEST(cpu_template_accepts_uppercase_hex_leaf) {
    // `parseHexUint32` lowercases before trimming, so "0X1F" is a valid leaf
    // -- and 0x1f is KVM-owned, so it is then dropped.
    std::vector<Reg> r;
    r.push_back(Reg("eax", Bm32(1)));
    std::vector<Cpuid> c;
    c.push_back(Cpuid("0X1F", "0x0", r));
    std::vector<std::string> jsons;
    jsons.push_back(BuildConfig(NoCaps(), c, NoMsr()));
    const agentenv::core::Expected<std::string, std::string> out = IntersectCpuConfigs(jsons);
    MT_EXPECT_TRUE(out.ok());
    MT_EXPECT_TRUE(out.value() ==
                   "{\"kvm_capabilities\":[],\"cpuid_modifiers\":[],\"msr_modifiers\":[]}");
}

MT_TEST(cpu_template_null_config_is_empty_not_error) {
    // `json.Unmarshal([]byte("null"), &cpuConfig{})` succeeds with the zero
    // value, so a null config intersects to nothing rather than failing.
    std::vector<Reg> r;
    r.push_back(Reg("eax", Bm32(0xFFu)));
    std::vector<Cpuid> c;
    c.push_back(Cpuid("0x1", "0x0", r));
    const agentenv::core::Expected<std::string, std::string> out =
        IntersectCpuConfigs(Jsons2(BuildConfig(NoCaps(), c, NoMsr()), "null"));
    MT_EXPECT_TRUE(out.ok());
    MT_EXPECT_TRUE(out.value() ==
                   "{\"kvm_capabilities\":[],\"cpuid_modifiers\":[],\"msr_modifiers\":[]}");
}

MT_TEST(cpu_template_bitmap_overflow_rejected) {
    // 33 binary digits does not fit a CPUID register; ParseUint's range error
    // must surface instead of a silently truncated bitmap.
    std::vector<Reg> r;
    r.push_back(Reg("eax", "0b" + std::string(33, '1')));
    std::vector<Cpuid> c;
    c.push_back(Cpuid("0x1", "0x0", r));
    std::vector<std::string> jsons;
    jsons.push_back(BuildConfig(NoCaps(), c, NoMsr()));
    MT_EXPECT_TRUE(!IntersectCpuConfigs(jsons).ok());
}

MT_MAIN
