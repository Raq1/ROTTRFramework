// The sun's shadow cascades blended (see shadow_blend.h).
#include "shadow_blend.h"

#include "dxbc_eight_weight_rewriter.h"
#include "log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace ShadowBlend {
namespace {
struct ShadowBlendShader {
    std::uint32_t original[4];   // the original's DXBC checksum
    const unsigned char* data;   // the patched shader
    std::size_t size;
    const char* name;            // its file offset in the retail ROTTR.exe
    int kernel;                  // its filter kernel's instruction count
    std::size_t rt;              // (v0.9.570) the byte offset of its sun mask marker (0 in data: not read)
};
#include "shadow_blend_shaders.inc"
// (v0.9.525) the tiled lighting shaders with the spot lights' shadows over 4 x 4 taps; (v0.9.526) and contact shadows
// (tools/shadow_blend/light_patch.py)
struct LightPassShader {
    std::uint32_t original[4];   // the original's DXBC checksum
    const unsigned char* data;   // the patched shader (its four values: markers)
    std::size_t size;
    const char* name;            // its file offset in the retail ROTTR.exe
    std::size_t values[6];       // the byte offsets of: contact strength, contact length (cm), lamp inner, centre steps,
                                 // (v0.9.537) the debug view, (v0.9.561) the probe's lamp shadows
};
#include "light_pass_shaders.inc"

constexpr std::size_t kCount = sizeof(kShadowBlendShaders) / sizeof(kShadowBlendShaders[0]);
D3D12_SHADER_BYTECODE g_bytecode[kCount];
std::atomic<bool> g_used[kCount];
std::atomic<int> g_refusals{0};

// (v0.9.518) The hair resolve (ROTTR.exe 0x137C4AC, 57876 bytes): its DXBC checksum and the byte offsets of its four
// `add rX, rX, l(1.0)` (the occluder's depth + 1, after its linearisation; tools/shadow_blend/hair_offsets.py)
constexpr std::uint32_t kHairChecksum[4] = {0xBB482914u, 0x42333F74u, 0x6CB801ECu, 0x403A69C1u};
constexpr std::size_t kHairSize = 57876;
constexpr std::size_t kHairBias[4] = {0x454C, 0x6794, 0x99C8, 0xBC10};
std::atomic<float> g_hairDistance{10.0f};
std::mutex g_hairLock;
std::deque<std::vector<unsigned char>> g_hairShaders;   // (g_hairLock: every one made, kept for the device)
std::deque<D3D12_SHADER_BYTECODE> g_hairBytecode;
std::atomic<int> g_hairLogs{0};

// (v0.9.525) Lamp shadow softness: 0 the game's (2 x 2 taps), 1 / 2 / 3 a 4 x 4 grid of taps 1 / 1.5 / 2 texels apart.
// (v0.9.526) Contact shadows: 0 off, 1 / 2 / 3 light / medium / strong; their length in cm. One patched shader for both:
// lamp softness off is its inner step 1 and centre step 0 (four copies of the game's own taps), contact shadows off a
// strength of 0 (no trace); both off: the game's own shader.
constexpr std::size_t kLightCount = sizeof(kLightPassShaders) / sizeof(kLightPassShaders[0]);
std::atomic<int> g_lampSoftness{1};
std::atomic<int> g_contactShadows{2};
std::atomic<float> g_contactLength{12.0f};
std::atomic<bool> g_contactDebug{false};   // (v0.9.537) the debug view: the light white, the contact shadows red
std::atomic<bool> g_rtLampShadows{false};  // (v0.9.561) the ray tracing probe's lamp shadow mask read (t45, t46)
std::mutex g_lightLock;
std::deque<std::vector<unsigned char>> g_lightShaders;   // (g_lightLock: every one made, kept for the device)
std::deque<D3D12_SHADER_BYTECODE> g_lightBytecode;
std::atomic<bool> g_lightUsed[kLightCount];

float LampSpacing(int softness) { return softness >= 3 ? 2.0f : (softness == 2 ? 1.5f : 1.0f); }
float ContactStrength(int level) { return level >= 3 ? 1.0f : (level == 2 ? 0.7f : (level == 1 ? 0.4f : 0.0f)); }

// (v0.9.527) The settings as one key (0: both off, the game's own shader): softness | contact << 2 | (v0.9.537) the
// debug view << 4 | length (cm) << 5; the length and the debug view only when contact shadows are on. The patched
// shader for any key, so the lights' pass can be made again while the game runs (eight_weight_patch.cpp, LightPassRecord).
// (v0.9.561) | the probe's lamp shadows << 10
std::uint32_t KeyOf(int softness, int contact, bool debug, float length, bool rt = false) {
    if (softness <= 0 && contact <= 0 && !rt) return 0;
    const std::uint32_t cm = contact > 0 ? static_cast<std::uint32_t>(std::lround(std::clamp(length, 4.0f, 30.0f))) : 0u;
    return static_cast<std::uint32_t>(softness) | (static_cast<std::uint32_t>(contact) << 2) |
           ((debug && contact > 0 ? 1u : 0u) << 4) | (cm << 5) | ((rt ? 1u : 0u) << 10);
}

const D3D12_SHADER_BYTECODE* LightPassForKey(std::size_t index, std::uint32_t key) {
    if (index >= kLightCount || key == 0) return nullptr;
    const int softness = static_cast<int>(key & 3), contact = static_cast<int>((key >> 2) & 3);
    const bool debug = ((key >> 4) & 1) != 0;
    const bool rt = ((key >> 10) & 1) != 0;   // (v0.9.561)
    const float length = contact > 0 ? static_cast<float>((key >> 5) & 31) : 12.0f;
    const LightPassShader& pass = kLightPassShaders[index];
    const float values[6] = {ContactStrength(contact), length, softness > 0 ? LampSpacing(softness) : 1.0f,
                             softness > 0 ? LampSpacing(softness) : 0.0f, debug ? 1.0f : 0.0f, rt ? 1.0f : 0.0f};
    for (const std::size_t at : pass.values)
        if (at < 20 || at + 4 > pass.size) return nullptr;
    std::lock_guard<std::mutex> lock(g_lightLock);
    for (std::size_t i = 0; i < g_lightShaders.size(); ++i) {   // (one copy per variant and values)
        const std::vector<unsigned char>& made = g_lightShaders[i];
        if (made.size() != pass.size) continue;
        bool same = true;
        for (int k = 0; k < 6 && same; ++k) same = std::memcmp(made.data() + pass.values[k], &values[k], 4) == 0;
        if (same && std::memcmp(made.data() + 20, pass.data + 20, pass.values[0] - 20) == 0) return &g_lightBytecode[i];
    }
    std::vector<unsigned char> copy(pass.data, pass.data + pass.size);
    for (int k = 0; k < 6; ++k) std::memcpy(copy.data() + pass.values[k], &values[k], 4);
    std::uint8_t sum[16];
    if (!DxbcEightWeight::Checksum(copy.data(), copy.size(), sum)) return nullptr;
    std::memcpy(copy.data() + 4, sum, 16);
    g_lightShaders.push_back(std::move(copy));
    D3D12_SHADER_BYTECODE code;
    code.pShaderBytecode = g_lightShaders.back().data();
    code.BytecodeLength = g_lightShaders.back().size();
    g_lightBytecode.push_back(code);
    static std::atomic<int> s_logs{0};
    if (!g_lightUsed[index].exchange(true) || s_logs.fetch_add(1) < 16)
        Log::Writef("RenderTweaks: the lights' pass (shader %s of ROTTR.exe) patched: lamp shadows over %s, contact "
                    "shadows %s (strength %.1f, %.0f cm)%s",
                    pass.name, softness > 0 ? (softness >= 3 ? "4 x 4 taps 2 texels apart" : (softness == 2 ? "4 x 4 taps 1.5 texels apart" : "4 x 4 taps 1 texel apart")) : "the game's 2 x 2",
                    contact > 0 ? "on" : "off", values[0], values[1], rt ? ", ray-traced lamp shadows read" : "");
    return &g_lightBytecode.back();
}

const D3D12_SHADER_BYTECODE* LightPassReplacement(std::size_t index) {
    return LightPassForKey(index, KeyOf(g_lampSoftness.load(), g_contactShadows.load(), g_contactDebug.load(),
                                        g_contactLength.load(), g_rtLampShadows.load()));
}

const D3D12_SHADER_BYTECODE* HairReplacement(const unsigned char* bytes, std::size_t size) {
    const float distance = g_hairDistance.load();
    if (std::fabs(distance - 1.0f) < 0.001f) return nullptr;   // (the game's own)
    for (const std::size_t at : kHairBias) {
        std::uint32_t word;
        std::memcpy(&word, bytes + at, 4);
        if (word != 0x3F800000u) {   // (not 1.0 where it should be: another shader with that checksum? left alone)
            if (g_hairLogs.fetch_add(1) < 4) Log::Write("RenderTweaks: the hair's lighting pass is not as expected; its self-shadow left as the game has it");
            return nullptr;
        }
    }
    std::lock_guard<std::mutex> lock(g_hairLock);
    std::vector<unsigned char> copy(bytes, bytes + size);
    for (const std::size_t at : kHairBias) std::memcpy(copy.data() + at, &distance, 4);
    std::uint8_t sum[16];
    if (!DxbcEightWeight::Checksum(copy.data(), copy.size(), sum)) return nullptr;
    std::memcpy(copy.data() + 4, sum, 16);
    g_hairShaders.push_back(std::move(copy));
    D3D12_SHADER_BYTECODE code;
    code.pShaderBytecode = g_hairShaders.back().data();
    code.BytecodeLength = g_hairShaders.back().size();
    g_hairBytecode.push_back(code);
    if (g_hairLogs.fetch_add(1) < 8)
        Log::Detailf("RenderTweaks: the hair's lighting pass made with a self-shadow distance of %.1f cm (the game's: 1): the "
                    "hair does not shadow itself within it",
                    distance);
    return &g_hairBytecode.back();
}
}  // namespace

void SetHairSelfShadowDistance(float cm) { g_hairDistance.store(std::clamp(cm, 1.0f, 40.0f)); }
float HairSelfShadowDistance() { return g_hairDistance.load(); }
void SetLampShadowSoftness(int softness) { g_lampSoftness.store(std::clamp(softness, 0, 3)); }   // (v0.9.525)
int LampShadowSoftness() { return g_lampSoftness.load(); }
void SetContactShadows(int level) { g_contactShadows.store(std::clamp(level, 0, 3)); }   // (v0.9.526)
int ContactShadows() { return g_contactShadows.load(); }
void SetContactShadowLength(float cm) { g_contactLength.store(std::clamp(cm, 4.0f, 30.0f)); }
float ContactShadowLength() { return g_contactLength.load(); }
void SetContactShadowsDebug(bool on) { g_contactDebug.store(on); }   // (v0.9.537)
bool ContactShadowsDebug() { return g_contactDebug.load(); }
void SetRtLampShadows(bool on) { g_rtLampShadows.store(on); }   // (v0.9.561)
bool RtLampShadows() { return g_rtLampShadows.load(); }

// (v0.9.570) the sun's shadow pass: its patched shader with the sun mask's marker 0 (as made) or 1 (a copy, re-signed)
std::atomic<bool> g_rtSunShadows{false};
std::mutex g_sunLock;
std::vector<unsigned char> g_sunShaders[kCount];   // (g_sunLock) the marker-1 copies
D3D12_SHADER_BYTECODE g_sunBytecode[kCount][2];
void SetRtSunShadows(bool on) { g_rtSunShadows.store(on); }
bool RtSunShadows() { return g_rtSunShadows.load(); }
int SunPassVariant(const D3D12_SHADER_BYTECODE& ps) {
    if (!ps.pShaderBytecode || ps.BytecodeLength < 32) return -1;
    const auto* bytes = static_cast<const unsigned char*>(ps.pShaderBytecode);
    if (std::memcmp(bytes, "DXBC", 4) != 0) return -1;
    for (std::size_t i = 0; i < kCount; ++i)
        if (std::memcmp(bytes + 4, kShadowBlendShaders[i].original, 16) == 0) return static_cast<int>(i);
    return -1;
}
std::uint32_t SunPassKey() { return g_rtSunShadows.load() ? 1u : 0u; }
const D3D12_SHADER_BYTECODE* SunPassBytecode(int variant, std::uint32_t key) {
    if (variant < 0 || static_cast<std::size_t>(variant) >= kCount) return nullptr;
    const ShadowBlendShader& shader = kShadowBlendShaders[variant];
    if (shader.rt < 20 || shader.rt + 4 > shader.size) return nullptr;
    const bool rt = (key & 1) != 0;
    std::lock_guard<std::mutex> lock(g_sunLock);
    D3D12_SHADER_BYTECODE& code = g_sunBytecode[variant][rt ? 1 : 0];
    if (!rt) {
        code.pShaderBytecode = shader.data;
        code.BytecodeLength = shader.size;
    } else if (!code.pShaderBytecode) {
        std::vector<unsigned char>& copy = g_sunShaders[variant];
        copy.assign(shader.data, shader.data + shader.size);
        const float one = 1.0f;
        std::memcpy(copy.data() + shader.rt, &one, 4);
        std::uint8_t sum[16];
        if (!DxbcEightWeight::Checksum(copy.data(), copy.size(), sum)) {
            copy.clear();
            return nullptr;
        }
        std::memcpy(copy.data() + 4, sum, 16);
        code.pShaderBytecode = copy.data();
        code.BytecodeLength = copy.size();
    }
    if (!g_used[variant].exchange(true))
        Log::Writef("RenderTweaks: the sun's shadow pass (shader %s of ROTTR.exe, a filter of %d instructions) blends "
                    "its cascades: the patched shader used (%zu bytes)", shader.name, shader.kernel, shader.size);
    return &code;
}
const char* SunPassName(int variant) {
    return variant < 0 || static_cast<std::size_t>(variant) >= kCount ? "?" : kShadowBlendShaders[variant].name;
}

// (v0.9.527) the lights' pass made again while the game runs
int LightPassVariant(const D3D12_SHADER_BYTECODE& ps) {
    if (!ps.pShaderBytecode || ps.BytecodeLength < 32) return -1;
    const auto* bytes = static_cast<const unsigned char*>(ps.pShaderBytecode);
    if (std::memcmp(bytes, "DXBC", 4) != 0) return -1;
    for (std::size_t i = 0; i < kLightCount; ++i)
        if (std::memcmp(bytes + 4, kLightPassShaders[i].original, 16) == 0) return static_cast<int>(i);
    return -1;
}
std::uint32_t LightPassKey() {
    return KeyOf(g_lampSoftness.load(), g_contactShadows.load(), g_contactDebug.load(), g_contactLength.load(),
                 g_rtLampShadows.load());
}
const D3D12_SHADER_BYTECODE* LightPassBytecode(int variant, std::uint32_t key) {
    return variant < 0 ? nullptr : LightPassForKey(static_cast<std::size_t>(variant), key);
}
const char* LightPassName(int variant) {
    return variant < 0 || static_cast<std::size_t>(variant) >= kLightCount ? "?" : kLightPassShaders[variant].name;
}

const D3D12_SHADER_BYTECODE* Replacement(const D3D12_SHADER_BYTECODE& ps) {
    if (!ps.pShaderBytecode || ps.BytecodeLength < 32) return nullptr;
    const auto* bytes = static_cast<const unsigned char*>(ps.pShaderBytecode);
    if (std::memcmp(bytes, "DXBC", 4) != 0) return nullptr;
    std::uint32_t checksum[4];
    std::memcpy(checksum, bytes + 4, sizeof(checksum));
    if (ps.BytecodeLength == kHairSize && std::memcmp(checksum, kHairChecksum, sizeof(checksum)) == 0)
        return HairReplacement(bytes, ps.BytecodeLength);
    for (std::size_t i = 0; i < kLightCount; ++i)   // (v0.9.525: lamp shadows; v0.9.526: contact shadows)
        if (std::memcmp(checksum, kLightPassShaders[i].original, sizeof(checksum)) == 0) return LightPassReplacement(i);
    for (std::size_t i = 0; i < kCount; ++i) {
        const ShadowBlendShader& s = kShadowBlendShaders[i];
        if (std::memcmp(checksum, s.original, sizeof(checksum)) != 0) continue;
        D3D12_SHADER_BYTECODE& code = g_bytecode[i];
        code.pShaderBytecode = s.data;
        code.BytecodeLength = s.size;
        if (!g_used[i].exchange(true))
            Log::Writef("RenderTweaks: the sun's shadow pass (shader %s of ROTTR.exe, a filter of %d instructions) blends "
                        "its cascades: the patched shader used (%zu -> %zu bytes)",
                        s.name, s.kernel, static_cast<std::size_t>(ps.BytecodeLength), s.size);
        return &code;
    }
    return nullptr;
}

void Refused(const D3D12_SHADER_BYTECODE& replacement, long hr) {
    const char* name = "the hair's lighting pass";
    for (std::size_t i = 0; i < kCount; ++i)
        if (kShadowBlendShaders[i].data == replacement.pShaderBytecode) name = kShadowBlendShaders[i].name;
    {
        std::lock_guard<std::mutex> lock(g_lightLock);
        for (const D3D12_SHADER_BYTECODE& code : g_lightBytecode)
            if (code.pShaderBytecode == replacement.pShaderBytecode)
                name = "the lights' pass (lamp shadows, contact shadows)";
    }
    if (g_refusals.fetch_add(1) < 8)
        Log::Writef("RenderTweaks: the device refused the blending shadow pass %s (hr 0x%08lX); the game's own used",
                    name, static_cast<unsigned long>(hr));
}
}  // namespace ShadowBlend
