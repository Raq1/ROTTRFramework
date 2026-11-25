// Draw distances and shadow detail past the game's own settings (render_tweaks.h).
#include "render_tweaks.h"

#include "game_build.h"
#include "log.h"

#include <MinHook.h>
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <mutex>

namespace RenderTweaks {
namespace {
constexpr std::uintptr_t kRvaApplyLodDistanceScale = 0x00296C90ull;
// sub rsp, 18h; movss xmm4, [g_settingsLODScale_InUse] (rip 0xFC1F58)
constexpr std::uint8_t kApplyLodDistanceScaleSig[] = {0x48, 0x83, 0xEC, 0x18, 0xF3, 0x0F, 0x10, 0x25};
// (v0.9.490) the sun's cascades: CommonScene::SetPSSMDistances, and its call in TERRAIN_CalculateFogging (the main view;
// the other one, ReflectionRender::RenderReflectionBuffer at 0x877A4D, is left as it is)
constexpr std::uintptr_t kRvaSetPssmDistances = 0x002C0590ull;
constexpr std::uint8_t kSetPssmDistancesSig[] = {0x0F, 0x57, 0xC9, 0x0F, 0x28, 0xC2, 0x0F, 0x57,
                                                 0xED, 0xB8, 0x04, 0x00, 0x00, 0x00};
constexpr std::uintptr_t kRvaFoggingPssmCall = 0x0042CADCull;
constexpr std::uint8_t kFoggingPssmCallSig[] = {0xE8, 0xAF, 0x3A, 0xE9, 0xFF};   // call SetPSSMDistances
constexpr std::uintptr_t kRvaFoggingPssmReturn = 0x0042CAE1ull;
constexpr std::uintptr_t kRvaSetFog = 0x00879010ull;
constexpr std::uint8_t kSetFogSig[] = {0x45, 0x0F, 0xB6, 0xD8, 0x4C, 0x8B, 0xD1, 0x83, 0xFA, 0xFF, 0x7F, 0x18,
                                       0x48, 0x63, 0x81, 0xF0, 0xCD, 0x00, 0x00, 0x83, 0xF8, 0x09, 0x7D, 0x20};

std::atomic_bool g_hairOffsetAvailable{false};
std::atomic<float> g_hairOffset{0.0f};   // (v0.9.518: no longer set: shadow_blend.h's self-shadow distance instead)

// (v0.9.490) The hair's self-shadow (see render_tweaks.h): the hair shadow pass sets RenderHairData's no-cull rasterizer
// state (retail 0xFF4450: depth bias 0, slope 0), so the strands are drawn into the shadow map with no bias at all. Its
// one SetRasterizerState call there (retail 0x2D5D79, returning to 0x2D5D7E) is given a copy with a depth bias and a
// slope bias of the light's own (what RenderShadowMapDirectional / Spot set once per light for every caster: the
// context's +0xCB0 and +0xCB8 just before) times the offset. The state is shared with the hair's own drawing, so it is
// not changed itself. DrawHairSM restores only the bias clamp and one flag when it is done, so in the game every caster
// drawn after the hair in the same light keeps the hair's state (no bias); wrapped: the context's whole rasterizer state
// from before the hair is put back after it (and the next hair pass reads the light's bias, not ours).
constexpr std::uintptr_t kRvaDrawHairSM = 0x002D5D20ull;
constexpr std::uint8_t kDrawHairSMSig[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48,
                                           0x89, 0x70, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xB0};
using DrawHairSMFn = void(__fastcall*)(void* self, void* a, void* b, void* c);
DrawHairSMFn g_originalDrawHairSM = nullptr;
constexpr std::size_t kContextRasterizerState = 0xCA8;
constexpr std::uintptr_t kRvaSetRasterizerState = 0x00281240ull;
constexpr std::uint8_t kSetRasterizerStateSig[] = {0x8B, 0x02, 0x39, 0x81, 0xA8, 0x0C, 0x00, 0x00, 0x75, 0x5E, 0x8B, 0x42,
                                                   0x04, 0x39, 0x81, 0xAC};
constexpr std::uintptr_t kRvaHairPassSetState = 0x002D5D6Cull;   // lea rdx, [state]; mov rcx, rbx; movaps xmm6, xmm0; call
constexpr std::uint8_t kHairPassSetStateSig[] = {0x48, 0x8D, 0x15, 0xDD, 0xE6, 0xD1, 0x00, 0x48, 0x8B, 0xCB,
                                                 0x0F, 0x28, 0xF0, 0xE8, 0xC2, 0xB4, 0xFA, 0xFF};
constexpr std::uintptr_t kRvaHairPassReturn = 0x002D5D7Eull;
constexpr std::size_t kContextDepthBias = 0xCB0, kContextDepthBiasSlope = 0xCB8;
constexpr std::size_t kStateSize = 0x18, kStateDepthBias = 0x08, kStateDepthBiasSlope = 0x10;
using SetRasterizerStateFn = void(__fastcall*)(void* context, const std::uint8_t* state);
SetRasterizerStateFn g_originalSetRasterizerState = nullptr;
std::uintptr_t g_hairPassReturn = 0;
// (the render thread's hair pass in flight: the context and its rasterizer state from before it)
thread_local void* t_hairContext = nullptr;
thread_local std::uint8_t t_hairSavedState[kStateSize];

void __fastcall HookDrawHairSM(void* self, void* a, void* b, void* c) {
    t_hairContext = nullptr;
    g_originalDrawHairSM(self, a, b, c);
    if (void* context = t_hairContext) {
        t_hairContext = nullptr;
        g_originalSetRasterizerState(context, t_hairSavedState);
    }
}

void __fastcall HookSetRasterizerState(void* context, const std::uint8_t* state) {
    const float offset = g_hairOffset.load(std::memory_order_relaxed);
    if (reinterpret_cast<std::uintptr_t>(_ReturnAddress()) != g_hairPassReturn || offset <= 0.0f || !context || !state) {
        g_originalSetRasterizerState(context, state);
        return;
    }
    std::uint8_t copy[kStateSize];
    std::memcpy(copy, state, kStateSize);
    const auto* ctx = static_cast<const std::uint8_t*>(context);
    std::memcpy(t_hairSavedState, ctx + kContextRasterizerState, kStateSize);
    t_hairContext = context;
    const std::int32_t lightBias = *reinterpret_cast<const std::int32_t*>(ctx + kContextDepthBias);
    const float lightSlope = *reinterpret_cast<const float*>(ctx + kContextDepthBiasSlope);
    // (signed as the light has them: the depth direction is the light's; a light with none gives the hair none either)
    const std::int32_t bias = static_cast<std::int32_t>(std::clamp(static_cast<float>(lightBias) * offset, -2.0e9f, 2.0e9f));
    const float slope = lightSlope * offset;
    *reinterpret_cast<std::int32_t*>(copy + kStateDepthBias) = bias;
    *reinterpret_cast<float*>(copy + kStateDepthBiasSlope) = slope;
    static int s_logs = 0;
    if (s_logs++ < 6)
        Log::Writef("RenderTweaks: the hair's shadow pass: the light's bias %d, slope %.3f; the hair's now %d, %.3f (x%.1f)",
                    lightBias, lightSlope, bias, slope, offset);
    g_originalSetRasterizerState(context, copy);
}

using SetPssmDistancesFn = void(__fastcall*)(void* scene, std::uint32_t splits, float d1, float d2, float d3, float d4);
using SetFogFn = void(__fastcall*)(void* render, int screen, bool enabled, const float* color, float nearDistance,
                                   float farDistance);
SetPssmDistancesFn g_originalSetPssmDistances = nullptr;
std::uintptr_t g_foggingPssmReturn = 0;
// (v0.9.488) the LOD distance through ApplyLODDistanceScale itself (see render_tweaks.h)
using ApplyLodDistanceScaleFn = float(__fastcall*)(float distance, float scale, std::uint8_t flags);
ApplyLodDistanceScaleFn g_originalApplyLodDistanceScale = nullptr;
SetFogFn g_originalSetFog = nullptr;
std::atomic_bool g_lodAvailable{false}, g_shadowAvailable{false}, g_fogAvailable{false};
std::atomic<float> g_lod{1.0f}, g_shadowDistance{1.0f}, g_shadowSharpness{1.0f}, g_fog{1.0f};
// (v0.9.519) The models the game keeps from LOD scaling up (ApplyLODDistanceScale's flag bit 0: the game's own LOD
// option is capped at x1 for them, and "LOD distance" left them so too): its foliage. Their own multiplier.
std::atomic<float> g_foliageLod{1.0f};
std::atomic<bool> g_foliageSeen{false};

// (v0.9.496) DrawableInfo::ShadowMaskAndDistanceCull (TR2 0x37BF80, retail 0x359480): a shadow caster's distance,
// through ApplyLODDistanceScale (its call at retail 0x35956A, returning to 0x35956F), against the drawable's own shadow
// range (+0x7C .. +0x84): past it the object casts no shadow, however far the cascades reach.
constexpr std::uintptr_t kRvaShadowCullScaleCall = 0x0035956Aull;
constexpr std::uint8_t kShadowCullScaleCallSig[] = {0xE8, 0x21, 0xD7, 0xF3, 0xFF};   // call ApplyLODDistanceScale
constexpr std::uintptr_t kRvaShadowCullScaleReturn = 0x0035956Full;
std::uintptr_t g_shadowCullReturn = 0;

// (v0.9.520) The grass (the user, on 0.9.519's Foliage LOD: no change; their video: the blades near Lara, a flat green
// a few metres on). It is the GrassManager's, not models: Scene::DrawGrass (TR2 0x14038AB10) gives each grass patch's
// RenderGrass::Draw (TR2 0x1402C49A0, retail 0x2A5680: this, scene, matrix, float distance) the patch's distance from
// the camera, and its grass volume's data (this +0x18, dtp::GrassVolumeComponent) decides from it: past +0x0C nothing,
// blades fading from +0x14 to +0x10, flat billboards from there to +0x0C. The distance it is given is divided by
// "Foliage LOD distance": all three pushed out together (blades and billboards farther; more GPU time).
constexpr std::uintptr_t kRvaRenderGrassDraw = 0x002A5680ull;
constexpr std::uint8_t kRenderGrassDrawSig[] = {0x48, 0x8B, 0xC4, 0x53, 0x56, 0x57, 0x41, 0x56, 0x48, 0x81, 0xEC, 0xE8,
                                                0x00, 0x00, 0x00, 0x80, 0xB9, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x49, 0x8B};
using RenderGrassDrawFn = void(__fastcall*)(void* self, void* scene, const void* matrix, float distance);
RenderGrassDrawFn g_originalRenderGrassDraw = nullptr;
std::atomic<bool> g_grassSeen{false};
void __fastcall HookRenderGrassDraw(void* self, void* scene, const void* matrix, float distance) {
    const float foliage = g_foliageLod.load(std::memory_order_relaxed);
    if (foliage > 0.0f && std::fabs(foliage - 1.0f) >= 0.001f) {
        if (!g_grassSeen.exchange(true))
            Log::Writef("RenderTweaks: the grass drawn as if x%.2f closer (Foliage LOD distance): its blades and billboards farther out", foliage);
        distance /= foliage;
    }
    g_originalRenderGrassDraw(self, scene, matrix, distance);
}

float __fastcall HookApplyLodDistanceScale(float distance, float scale, std::uint8_t flags) {
    // (flags bit 0: the game keeps this one from scaling up: left so)
    const float lod = g_lod.load(std::memory_order_relaxed);
    // (v0.9.489) the scale multiplies the object's distance for the LOD choice: a larger one makes it look farther, so
    // coarser (the user: lower values gave more detail). The setting's "more detail farther" divides it.
    if (!(flags & 1) && std::fabs(lod - 1.0f) >= 0.001f) scale /= lod;
    // (v0.9.519) the ones the game keeps from scaling up (its foliage): "Foliage LOD distance" (their model LOD and how
    // far they are drawn: the distance cull goes through here too)
    if (flags & 1) {
        const float foliage = g_foliageLod.load(std::memory_order_relaxed);
        if (std::fabs(foliage - 1.0f) >= 0.001f) {
            scale /= foliage;
            if (!g_foliageSeen.exchange(true))
                Log::Writef("RenderTweaks: the models the game keeps from LOD scaling (flag 1, its foliage) now x%.2f", foliage);
        }
    }
    // (v0.9.496) a shadow caster's own range goes as far out as the shadow distance takes the cascades (only farther)
    if (reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == g_shadowCullReturn) {
        const float shadow = g_shadowDistance.load(std::memory_order_relaxed);
        if (shadow > 1.001f) scale /= shadow;
    }
    return g_originalApplyLodDistanceScale(distance, scale, flags);
}

// (v0.9.490) Every frame, the level's cascade ends for the main view (see render_tweaks.h): the nearest one divided by
// the sharpness, the farthest times the distance, the ones between on the same curve (in log space).
void __fastcall HookSetPssmDistances(void* scene, std::uint32_t splits, float d1, float d2, float d3, float d4) {
    const float distance = g_shadowDistance.load(std::memory_order_relaxed);
    const float sharpness = g_shadowSharpness.load(std::memory_order_relaxed);
    const std::uint32_t n = (std::min)(splits, 4u);
    if (reinterpret_cast<std::uintptr_t>(_ReturnAddress()) != g_foggingPssmReturn || n == 0 ||
        (std::fabs(distance - 1.0f) < 0.001f && std::fabs(sharpness - 1.0f) < 0.001f)) {
        g_originalSetPssmDistances(scene, splits, d1, d2, d3, d4);
        return;
    }
    float d[4] = {d1, d2, d3, d4};
    // (v0.9.496) The two ends moved: the nearest never below 4.5 m (where Lara stands from the camera: she stays in the
    // sharpest cascade), the farthest at most 4 km (unless the level's own is farther). The ones between: v0.9.515,
    // below (0.9.496 kept them where the level puts them relative to its own ends, in log space).
    if (n == 1) {
        d[0] *= distance;
    } else {
        const float first = (std::max)(d[0] / sharpness, (std::min)(d[0], 450.0f));
        const float last = (std::max)(d[n - 1], (std::min)(d[n - 1] * distance, 400000.0f));
        // (v0.9.515) How sharp a cascade's shadows look goes with the ratio of its far end to its near end (its map covers
        // ground about as wide as its far end, seen from as near as its near end). 0.9.496 stretched all of them alike
        // in log space: at distance x8 the level's 7 / 20 / 35 / 130 m became 4.5 / 32 / 90 / 1040 m, the second cascade
        // 4.5 .. 32 m (a ratio of 7.1 where the level has 2.9): the user's shadows sharp only right by Lara, coarse a few
        // metres ahead (their video). Now each cascade keeps the level's own ratio and the extra span (the new ends'
        // ratio over the level's) is shared out mostly to the farthest: weights 0 / 0.3 / 0.7 (4 cascades; 0.3 / 0.7
        // for 3, all of it for 2). At x8: 4.5 / 12.9 / 48 / 1040 m, the second cascade as sharp as the level's, the third
        // as its last was, and only shadows past about 50 m coarser (the level has none past 130 m).
        static const float kWeights[3][3] = {{1.0f, 0.0f, 0.0f}, {0.3f, 0.7f, 0.0f}, {0.0f, 0.3f, 0.7f}};
        const float* weights = kWeights[n - 2];
        float ratio[3];
        float levelSpan = 0.0f;
        for (std::uint32_t i = 1; i < n; ++i) {
            ratio[i - 1] = (std::max)(d[i] / (std::max)(d[i - 1], 1.0f), 1.05f);
            levelSpan += std::log(ratio[i - 1]);
        }
        const float extra = std::log(last / first) - levelSpan;   // (log space; below 0 when the new ends are closer)
        float out[4];
        out[0] = first;
        for (std::uint32_t i = 1; i < n; ++i) {
            out[i] = out[i - 1] * std::exp(std::log(ratio[i - 1]) + extra * weights[i - 1]);
            out[i] = (std::max)(out[i], out[i - 1] * 1.05f);
        }
        out[n - 1] = (std::max)(last, out[n - 2] * 1.05f);
        for (std::uint32_t i = 0; i < n; ++i) d[i] = out[i];
    }
    for (std::uint32_t i = n; i < 4; ++i) d[i] = (std::max)(d[i], d[n - 1]);
    static float s_last[4] = {};
    static int s_logs = 0;
    if (s_logs < 10 && (std::fabs(s_last[0] - d1) > 1.0f || std::fabs(s_last[3] - d4) > 1.0f)) {
        ++s_logs;
        s_last[0] = d1;
        s_last[3] = d4;
        Log::Writef("RenderTweaks: the sun's %u cascades end at %.0f, %.0f, %.0f, %.0f; now %.0f, %.0f, %.0f, %.0f "
                    "(distance x%.2f, sharpness x%.2f)",
                    n, d1, d2, d3, d4, d[0], d[1], d[2], d[3], distance, sharpness);
    }
    g_originalSetPssmDistances(scene, splits, d[0], d[1], d[2], d[3]);
}

void __fastcall HookSetFog(void* render, int screen, bool enabled, const float* color, float nearDistance, float farDistance) {
    const float scale = g_fog.load(std::memory_order_relaxed);
    g_originalSetFog(render, screen, enabled, color, nearDistance * scale, farDistance * scale);
}

// (v0.9.523) Shadow map size (render_tweaks.h): each shadowed light's map made larger while the shadow atlas has room.
// CommonLightManager::SetupShadowMapAllocationsAndTransforms (TR2 0x2E7130) fills a ShadowMapPool each frame: a budget of
// 8192 x 4096 (+0x204 the area left, +0x200 the maps placed, at most 16), the sun first, then the other lights; then,
// per view, the pool put back to that point (+0x210 / +0x218 keep it; both 0 before) and the view's lights again.
// ShadowMapPool::AddMap (TR2 0x2B3DF0, retail 0x2953B0) asks CommonDeferredLight::GetShadowMapSize (TR2 0x2D5E80, retail
// 0x2B4F40: the light's own size, the sun at least 1024, others halved with distance), caps it at 2048 and takes faces
// (+0x794: the sun's cascades, a point light's six) x size^2 of the budget. A light that does not fit makes the whole
// allocation start again (its second attempt drops the sun's 1024 minimum: the bool both take), then without shadows.
// ComputeAllocation packs the maps into the atlas, scaled to it (x2 at the top Shadow quality: 2048 there is 4096).
// The size a light asks for is multiplied (x2 / x4 / x8, less where that would not fit) only while the frame's allocation
// can take it: the area already placed, this map, and what the lights still to come asked for in the previous frame
// (their own sizes), within the budget less a 1/32 margin. If the game still has to try again in a frame where a map
// was made larger (a light that was not there the frame before), none is made larger for 3 s.
constexpr std::uintptr_t kRvaGetShadowMapSize = 0x002B4F40ull;
constexpr std::uint8_t kGetShadowMapSizeSig[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x81, 0xEC,
                                                 0x80, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x41, 0x08, 0x48, 0x8B, 0xF1};
constexpr std::uintptr_t kRvaShadowPoolAddMap = 0x002953B0ull;
constexpr std::uint8_t kShadowPoolAddMapSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48,
                                                 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x54,
                                                 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0xF2,
                                                 0x48, 0x8B, 0xD9, 0x49, 0x8B, 0xCE, 0x41, 0x0F, 0xB6, 0xD0};
constexpr std::size_t kPoolMaps = 0x200, kPoolAreaLeft = 0x204, kPoolShift = 0x20C, kPoolSavedMaps = 0x210,
                      kPoolSavedArea = 0x218;
constexpr std::size_t kLightFaces = 0x794, kLightData = 0x08, kLightDataType = 0x140;
constexpr std::uint64_t kPoolArea = 0x2000000ull, kPoolMapCap = 0x800ull, kPoolMargin = kPoolArea / 32;
using GetShadowMapSizeFn = std::uint32_t(__fastcall*)(void* light, bool reduced);
using ShadowPoolAddMapFn = bool(__fastcall*)(void* pool, void* light, bool reduced);
GetShadowMapSizeFn g_originalGetShadowMapSize = nullptr;
ShadowPoolAddMapFn g_originalShadowPoolAddMap = nullptr;
std::atomic<int> g_shadowMapScale{1};
std::atomic<bool> g_shadowMapAvailable{false};
thread_local void* t_shadowPool = nullptr;   // (the AddMap in flight on this thread)
struct PoolDemand {
    const void* pool = nullptr;
    std::uint64_t others = 0, view = 0, viewMax = 0;   // this frame: the area asked by the lights before the views, this
                                                       // view's, the largest view's (their own sizes)
    std::uint64_t lastOthers = 0, lastViewMax = 0;     // the frame before's
    bool larger = false;                               // a map made larger this frame
};
std::mutex g_poolLock;
PoolDemand g_pools[4];
ULONGLONG g_largerPausedUntil = 0;
struct ShadowMapSeen {
    int type;
    std::uint32_t faces, own, given;
};
ShadowMapSeen g_shadowMapSeen[24];
int g_shadowMapSeenCount = 0, g_shadowMapPauseLogs = 0;

PoolDemand& DemandOf(const void* pool) {
    for (auto& d : g_pools)
        if (d.pool == pool) return d;
    for (auto& d : g_pools)
        if (!d.pool) {
            d.pool = pool;
            return d;
        }
    g_pools[0] = PoolDemand{};
    g_pools[0].pool = pool;
    return g_pools[0];
}

void LogShadowMap(int type, std::uint32_t faces, std::uint64_t own, std::uint64_t given, const char* why) {
    for (int i = 0; i < g_shadowMapSeenCount; ++i) {
        const ShadowMapSeen& seen = g_shadowMapSeen[i];
        if (seen.type == type && seen.faces == faces && seen.own == own && seen.given == given) return;
    }
    if (g_shadowMapSeenCount >= int(sizeof(g_shadowMapSeen) / sizeof(g_shadowMapSeen[0]))) return;
    g_shadowMapSeen[g_shadowMapSeenCount++] = {type, faces, std::uint32_t(own), std::uint32_t(given)};
    Log::Writef("RenderTweaks: a shadowed light (%s, %u map%s) asks for %llu in the shadow atlas; %llu given%s",
                type == 5 ? "the sun" : (type == 2 ? "a spot light" : "a light"), faces, faces == 1 ? "" : "s",
                static_cast<unsigned long long>(own), static_cast<unsigned long long>(given), why);
}

std::uint32_t ShadowMapSizeFor(const std::uint8_t* pool, const std::uint8_t* light, bool reduced, std::uint32_t size) {
    const std::uint32_t maps = *reinterpret_cast<const std::uint32_t*>(pool + kPoolMaps);
    const std::uint32_t left = *reinterpret_cast<const std::uint32_t*>(pool + kPoolAreaLeft);
    const std::uint32_t shift = *reinterpret_cast<const std::uint32_t*>(pool + kPoolShift);
    const std::uint32_t savedMaps = *reinterpret_cast<const std::uint32_t*>(pool + kPoolSavedMaps);
    const std::uint32_t savedLeft = *reinterpret_cast<const std::uint32_t*>(pool + kPoolSavedArea);
    const std::uint32_t faces = *reinterpret_cast<const std::uint32_t*>(light + kLightFaces);
    const auto* data = *reinterpret_cast<const std::uint8_t* const*>(light + kLightData);
    const int type = data ? data[kLightDataType] : -1;
    if (shift >= 16 || faces == 0 || faces > 16 || left > kPoolArea) return size;
    auto placed = [shift](std::uint64_t s) { return (std::min)(s >> shift, kPoolMapCap); };
    const std::uint64_t own = placed(size), ownArea = std::uint64_t(faces) * own * own;
    const bool views = savedLeft != 0;   // (the views' part: the pool put back to where the others left it)
    std::lock_guard<std::mutex> lock(g_poolLock);
    PoolDemand& d = DemandOf(pool);
    if (reduced) {   // (the game's next attempt: the one before did not fit)
        if (d.larger) {
            d.larger = false;
            g_largerPausedUntil = GetTickCount64() + 3000;
            if (g_shadowMapPauseLogs++ < 8)
                Log::Write("RenderTweaks: the shadow atlas did not fit this frame with larger maps (a new light?): the "
                           "game's own sizes for 3 s");
        }
        return size;
    }
    if (!views && maps == 0 && left == kPoolArea) {   // a frame's allocation starts
        d.lastOthers = d.others;
        d.lastViewMax = (std::max)(d.viewMax, d.view);
        d.others = d.view = d.viewMax = 0;
        d.larger = false;
    }
    if (views && maps == savedMaps && left == savedLeft) {   // a view's lights start
        d.viewMax = (std::max)(d.viewMax, d.view);
        d.view = 0;
    }
    (views ? d.view : d.others) += ownArea;
    const int scale = g_shadowMapScale.load(std::memory_order_relaxed);
    if (scale <= 1) return size;
    if (own >= kPoolMapCap) {
        LogShadowMap(type, faces, own, own, " (already the largest a map can be)");
        return size;
    }
    if (GetTickCount64() < g_largerPausedUntil) return size;
    const std::uint64_t used = kPoolArea - left;
    const std::uint64_t later = views ? (d.lastViewMax > d.view ? d.lastViewMax - d.view : 0)
                                      : (d.lastOthers > d.others ? d.lastOthers - d.others : 0) + d.lastViewMax;
    for (int k = scale; k > 1; k /= 2) {
        const std::uint64_t s = placed(std::uint64_t(size) * std::uint64_t(k));
        if (s <= own) continue;
        const std::uint64_t area = std::uint64_t(faces) * s * s;
        if (used + area + later + kPoolMargin > kPoolArea) continue;
        d.larger = true;
        LogShadowMap(type, faces, own, s, "");
        return size * std::uint32_t(k);
    }
    LogShadowMap(type, faces, own, own, " (no room for a larger one)");
    return size;
}

bool __fastcall HookShadowPoolAddMap(void* pool, void* light, bool reduced) {
    void* const outer = t_shadowPool;
    t_shadowPool = pool;
    const bool fits = g_originalShadowPoolAddMap(pool, light, reduced);
    t_shadowPool = outer;
    return fits;
}

std::uint32_t __fastcall HookGetShadowMapSize(void* light, bool reduced) {
    const std::uint32_t size = g_originalGetShadowMapSize(light, reduced);
    if (!t_shadowPool || !light || size == 0) return size;
    return ShadowMapSizeFor(static_cast<const std::uint8_t*>(t_shadowPool), static_cast<const std::uint8_t*>(light), reduced,
                            size);
}

// (v0.9.528) Ear light transmission (render_tweaks.h). SSSRender::Draw (TR2 0x1408CD420, retail 0x88C860) blurs the
// skin's light when SSSRender::sm_bSSSEnabled (retail 0x10B17CD, .data: 1 at start) is set, and after it draws the
// ears' transmission (render targets "SSS Ear Tmp", "SSS Ear Temp DB") when sm_bEarTransEnabled (retail 0x2E1BFA7,
// .bss: 0 at start) is set. Only the level scripts' action graph nodes write them: AGToggleSubSurfaceScatteringNode::
// ProcessMsg (retail 0x732CD0) and AGToggleEarTransmissionNode::ProcessMsg (retail 0x739930): on message 2, input
// bit 0 sets the flag, bit 1 clears it. So the ears' transmission is off in play unless a script turns it on (the
// cutscenes'). Both nodes hooked: after the game's own, what the script chose is kept (and logged when it changes) and,
// with "Ear light transmission" on, the ears' flag is set again; off, the script's choice is put back.
// (v0.9.540) That was not all: GameRender::BeginScene (retail 0x871257..0x87128A, TR2 0x1408AFC47..) calls
// SSSRender::Draw only when DebugDrawFlags_GetMdlView (retail 0x3D4BA0: a debug flag's bit 1) or cdc::CinematicHandler::
// IsCinematicPlaying (retail 0x169D30) answers true: the skin's scattering (and so the ears' transmission, drawn inside
// it) only in the cutscenes, never in play (found for the model viewer in v0.9.430; 0.9.528 missed it). Skin scattering
// (render_tweaks.h): DebugDrawFlags_GetMdlView hooked (moved here from model_viewer.cpp), answering 1 when called from
// that place (its return address, retail 0x87125C) while chosen or while the model viewer asks; every other call and
// answer the game's. And sm_bSSSEnabled kept set while chosen (a script clearing it: set again, logged).
// (v0.9.541) One switch for both (the user: separate settings made no sense): Skin scattering sets the ears' flag too.
constexpr std::uintptr_t kRvaModelViewFlag = 0x003D4BA0ull, kRvaBeginSceneSssCall = 0x00871257ull;
// mov eax, [rip + the debug flags]; shr eax, 1; and eax, 1; ret
constexpr std::uint8_t kModelViewFlagSig[] = {0x8B, 0x05, 0xCA, 0xCF, 0x10, 0x02, 0xD1, 0xE8, 0x83, 0xE0, 0x01, 0xC3};
// call DebugDrawFlags_GetMdlView; test eax, eax; jne (SSSRender::Draw's call); mov rcx, [rip + the cinematic handler];
// test; je; xor edx, edx; call IsCinematicPlaying; test al, al; je (past it); ...; call SSSRender::Draw
constexpr std::uint8_t kBeginSceneSssSig[] = {0xE8, 0x44, 0x39, 0xB6, 0xFF, 0x85, 0xC0, 0x75, 0x17, 0x48, 0x8B, 0x0D, 0x71,
                                              0x67, 0x4E, 0x02, 0x48, 0x85, 0xC9, 0x74, 0x23, 0x33, 0xD2, 0xE8, 0xBD, 0x8A,
                                              0x8F, 0xFF, 0x84, 0xC0, 0x74, 0x18, 0x48, 0x8B, 0x06, 0x48, 0x8B, 0xCE, 0xFF,
                                              0x50, 0x68, 0x49, 0x8B, 0x8F, 0x08, 0xCE, 0x00, 0x00, 0x48, 0x8B, 0xD0, 0xE8,
                                              0xD1, 0xB5, 0x01, 0x00};
constexpr std::uintptr_t kRvaEarFlag = 0x02E1BFA7ull, kRvaSssFlag = 0x010B17CDull;
constexpr std::uintptr_t kRvaEarNode = 0x00739930ull, kRvaSssNode = 0x00732CD0ull;
// cmp edx, 2; jne; test byte [rcx+14h], 1; je; mov byte [rip + flag], 1 (its displacement: the flag's address)
constexpr std::uint8_t kEarNodeSig[] = {0x83, 0xFA, 0x02, 0x75, 0x2A, 0xF6, 0x41, 0x14, 0x01,
                                        0x74, 0x08, 0xC6, 0x05, 0x65, 0x26, 0x6E, 0x02, 0x01};
constexpr std::uint8_t kSssNodeSig[] = {0x83, 0xFA, 0x02, 0x75, 0x2A, 0xF6, 0x41, 0x14, 0x01,
                                        0x74, 0x08, 0xC6, 0x05, 0xEB, 0xEA, 0x97, 0x00, 0x01};
using AgProcessMsgFn = void(__fastcall*)(void* node, int message, void* data);
AgProcessMsgFn g_originalEarNode = nullptr, g_originalSssNode = nullptr;
std::atomic<bool> g_earAvailable{false}, g_earForced{true};   // (v0.9.541: g_earForced follows Skin scattering)
std::atomic<bool> g_scriptEar{false}, g_scriptSss{true};   // (what the scripts chose last)
std::atomic<int> g_skinLogs{0};
using ModelViewFlagFn = unsigned(__fastcall*)();
ModelViewFlagFn g_originalModelViewFlag = nullptr;
std::uintptr_t g_beginSceneSssReturn = 0;
std::atomic<bool> g_sssAvailable{false}, g_sssChosen{true}, g_sssViewer{false};   // (v0.9.540)
std::atomic<std::uint32_t> g_sssAsked{0};

volatile std::uint8_t* SkinFlag(std::uintptr_t rva) {
    return reinterpret_cast<volatile std::uint8_t*>(GameBuild::Base() + rva);
}

void __fastcall HookEarNode(void* node, int message, void* data) {
    g_originalEarNode(node, message, data);
    if (message != 2) return;
    const bool chosen = *SkinFlag(kRvaEarFlag) != 0;
    const bool forced = g_earForced.load();
    if (chosen != g_scriptEar.exchange(chosen) && g_skinLogs.fetch_add(1) < 32)
        Log::Writef("RenderTweaks: the game's scripts switched ear light transmission %s%s", chosen ? "on" : "off",
                    forced && !chosen ? " (kept on: Skin scattering)" : "");
    if (forced) *SkinFlag(kRvaEarFlag) = 1;
}

void __fastcall HookSssNode(void* node, int message, void* data) {
    g_originalSssNode(node, message, data);
    if (message != 2) return;
    const bool chosen = *SkinFlag(kRvaSssFlag) != 0;
    const bool forced = g_sssAvailable.load() && (g_sssChosen.load() || g_sssViewer.load());
    if (chosen != g_scriptSss.exchange(chosen) && g_skinLogs.fetch_add(1) < 32)
        Log::Writef("RenderTweaks: the game's scripts switched skin scattering %s%s", chosen ? "on" : "off",
                    forced && !chosen ? " (kept on: Skin scattering)" : "");
    if (forced) *SkinFlag(kRvaSssFlag) = 1;
}

// (v0.9.540) BeginScene's question "draw the skin's scattering?": yes while chosen or while the model viewer asks
unsigned __fastcall HookModelViewFlag() {
    if (reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == g_beginSceneSssReturn &&
        (g_sssChosen.load(std::memory_order_relaxed) || g_sssViewer.load(std::memory_order_acquire))) {
        g_sssAsked.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    return g_originalModelViewFlag();
}

void ApplySkinScattering() {
    if (!g_sssAvailable.load()) return;
    *SkinFlag(kRvaSssFlag) = (g_sssChosen.load() || g_sssViewer.load() || g_scriptSss.load()) ? 1 : 0;
}

void ApplyEarTransmission() {
    if (!g_earAvailable.load()) return;
    *SkinFlag(kRvaEarFlag) = g_earForced.load() ? 1 : (g_scriptEar.load() ? 1 : 0);
}

bool Hook(std::uintptr_t rva, const std::uint8_t* sig, std::size_t size, void* detour, void** original, const char* what) {
    if (!GameBuild::VerifyBytes(rva, sig, size)) {
        Log::Writef("RenderTweaks: %s does not match this build", what);
        return false;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(GameBuild::Base() + rva), detour, original) != MH_OK) {
        Log::Writef("RenderTweaks: the hook on %s could not be created", what);
        return false;
    }
    return true;
}
}  // namespace

bool Install() {
    if (!GameBuild::IsSupportedRetail()) {
        Log::Write("RenderTweaks: unavailable (not the supported game build)");
        return false;
    }
    if (GameBuild::VerifyBytes(kRvaShadowCullScaleCall, kShadowCullScaleCallSig, sizeof(kShadowCullScaleCallSig)))
        g_shadowCullReturn = GameBuild::Base() + kRvaShadowCullScaleReturn;
    else
        Log::Write("RenderTweaks: the shadow casters' range check does not match this build; it stays the game's");
    g_lodAvailable.store(Hook(kRvaApplyLodDistanceScale, kApplyLodDistanceScaleSig, sizeof(kApplyLodDistanceScaleSig),
                              reinterpret_cast<void*>(&HookApplyLodDistanceScale),
                              reinterpret_cast<void**>(&g_originalApplyLodDistanceScale), "CommonRenderDevice::ApplyLODDistanceScale"));
    if (GameBuild::VerifyBytes(kRvaFoggingPssmCall, kFoggingPssmCallSig, sizeof(kFoggingPssmCallSig))) {
        g_foggingPssmReturn = GameBuild::Base() + kRvaFoggingPssmReturn;
        g_shadowAvailable.store(Hook(kRvaSetPssmDistances, kSetPssmDistancesSig, sizeof(kSetPssmDistancesSig),
                                     reinterpret_cast<void*>(&HookSetPssmDistances),
                                     reinterpret_cast<void**>(&g_originalSetPssmDistances), "CommonScene::SetPSSMDistances"));
    } else {
        Log::Write("RenderTweaks: TERRAIN_CalculateFogging's SetPSSMDistances call does not match this build");
    }
    g_fogAvailable.store(Hook(kRvaSetFog, kSetFogSig, sizeof(kSetFogSig), reinterpret_cast<void*>(&HookSetFog),
                              reinterpret_cast<void**>(&g_originalSetFog), "GameRender::SetFog"));
    // (v0.9.520) the grass's distance (Foliage LOD distance)
    Hook(kRvaRenderGrassDraw, kRenderGrassDrawSig, sizeof(kRenderGrassDrawSig), reinterpret_cast<void*>(&HookRenderGrassDraw),
         reinterpret_cast<void**>(&g_originalRenderGrassDraw), "RenderGrass::Draw");
    // (v0.9.523) Shadow map size: both or neither
    if (Hook(kRvaShadowPoolAddMap, kShadowPoolAddMapSig, sizeof(kShadowPoolAddMapSig),
             reinterpret_cast<void*>(&HookShadowPoolAddMap), reinterpret_cast<void**>(&g_originalShadowPoolAddMap),
             "ShadowMapPool::AddMap")) {
        const bool size = Hook(kRvaGetShadowMapSize, kGetShadowMapSizeSig, sizeof(kGetShadowMapSizeSig),
                               reinterpret_cast<void*>(&HookGetShadowMapSize),
                               reinterpret_cast<void**>(&g_originalGetShadowMapSize), "CommonDeferredLight::GetShadowMapSize");
        if (!size) MH_RemoveHook(reinterpret_cast<void*>(GameBuild::Base() + kRvaShadowPoolAddMap));
        g_shadowMapAvailable.store(size);
    }
    // (v0.9.528) the skin's two switches (ear transmission forced on when chosen; both logged)
    if (Hook(kRvaEarNode, kEarNodeSig, sizeof(kEarNodeSig), reinterpret_cast<void*>(&HookEarNode),
             reinterpret_cast<void**>(&g_originalEarNode), "AGToggleEarTransmissionNode::ProcessMsg")) {
        g_scriptEar.store(*SkinFlag(kRvaEarFlag) != 0);
        g_earAvailable.store(true);
        ApplyEarTransmission();
    }
    const bool sssNode = Hook(kRvaSssNode, kSssNodeSig, sizeof(kSssNodeSig), reinterpret_cast<void*>(&HookSssNode),
                              reinterpret_cast<void**>(&g_originalSssNode), "AGToggleSubSurfaceScatteringNode::ProcessMsg");
    if (sssNode) g_scriptSss.store(*SkinFlag(kRvaSssFlag) != 0);
    // (v0.9.540) the skin's scattering in play: BeginScene's call checked, then the flag's function hooked
    if (sssNode && GameBuild::VerifyBytes(kRvaBeginSceneSssCall, kBeginSceneSssSig, sizeof(kBeginSceneSssSig))) {
        g_beginSceneSssReturn = GameBuild::Base() + kRvaBeginSceneSssCall + 5;
        g_sssAvailable.store(Hook(kRvaModelViewFlag, kModelViewFlagSig, sizeof(kModelViewFlagSig),
                                  reinterpret_cast<void*>(&HookModelViewFlag),
                                  reinterpret_cast<void**>(&g_originalModelViewFlag), "DebugDrawFlags_GetMdlView"));
        ApplySkinScattering();
    } else {
        Log::Write("RenderTweaks: GameRender::BeginScene's skin scattering call does not match this build; the skin's "
                   "scattering stays in the cutscenes only");
    }
    // (v0.9.607) HairShadowDrawable's constructor no longer hooked: the hair shadows switch removed
    if (GameBuild::VerifyBytes(kRvaHairPassSetState, kHairPassSetStateSig, sizeof(kHairPassSetStateSig))) {
        g_hairPassReturn = GameBuild::Base() + kRvaHairPassReturn;
        // (both or neither: the offset without the restore would leave it on the casters after the hair)
        if (Hook(kRvaDrawHairSM, kDrawHairSMSig, sizeof(kDrawHairSMSig), reinterpret_cast<void*>(&HookDrawHairSM),
                 reinterpret_cast<void**>(&g_originalDrawHairSM), "DrawHairSM")) {
            const bool set = Hook(kRvaSetRasterizerState, kSetRasterizerStateSig, sizeof(kSetRasterizerStateSig),
                                  reinterpret_cast<void*>(&HookSetRasterizerState),
                                  reinterpret_cast<void**>(&g_originalSetRasterizerState), "RenderContext::SetRasterizerState");
            if (!set) MH_RemoveHook(reinterpret_cast<void*>(GameBuild::Base() + kRvaDrawHairSM));
            g_hairOffsetAvailable.store(set);
        }
    } else {
        Log::Write("RenderTweaks: the hair shadow pass's rasterizer state call does not match; no hair self-shadow offset");
    }
    Log::Writef("RenderTweaks: LOD distance %s, sun shadow cascades %s, shadow map size %s, fog distance %s, hair "
                "self-shadow offset %s (LOD x%.2f, shadow distance x%.2f, sharpness x%.2f, map size x%d, fog x%.2f, hair x%.1f)",
                g_lodAvailable.load() ? "ready" : "unavailable", g_shadowAvailable.load() ? "ready" : "unavailable",
                g_shadowMapAvailable.load() ? "ready" : "unavailable", g_fogAvailable.load() ? "ready" : "unavailable",
                g_hairOffsetAvailable.load() ? "ready" : "unavailable", g_lod.load(), g_shadowDistance.load(),
                g_shadowSharpness.load(), g_shadowMapScale.load(), g_fog.load(), g_hairOffset.load());
    return g_lodAvailable.load() || g_shadowAvailable.load() || g_fogAvailable.load() || g_hairOffsetAvailable.load();
}

void TickGameThread() {
    // (v0.9.488: nothing any more; g_globalLODScale is not written: VrManager::Process sets it back to 1 every frame
    // outside VR, and 0.9.487's writes made the LODs alternate frame by frame)
}

bool LodAvailable() { return g_lodAvailable.load(); }
bool ShadowAvailable() { return g_shadowAvailable.load(); }
bool FogAvailable() { return g_fogAvailable.load(); }
float LodDistance() { return g_lod.load(); }
float ShadowDistance() { return g_shadowDistance.load(); }
float ShadowSharpness() { return g_shadowSharpness.load(); }
bool ShadowMapSizeAvailable() { return g_shadowMapAvailable.load(); }   // (v0.9.523)
int ShadowMapSize() { return g_shadowMapScale.load(); }
void SetShadowMapSize(int scale) {   // (v0.9.524: x8 too)
    g_shadowMapScale.store(scale >= 8 ? 8 : (scale >= 4 ? 4 : (scale >= 2 ? 2 : 1)));
}
float FogDistance() { return g_fog.load(); }
void SetLodDistance(float value) { g_lod.store(std::clamp(value, 0.5f, 16.0f)); }   // (v0.9.496: to 8; v0.9.519: 16)
void SetShadowDistance(float value) { g_shadowDistance.store(std::clamp(value, 0.5f, 16.0f)); }
float FoliageLodDistance() { return g_foliageLod.load(); }
void SetFoliageLodDistance(float value) { g_foliageLod.store(std::clamp(value, 0.5f, 16.0f)); }
bool SkinScatteringAvailable() { return g_sssAvailable.load(); }   // (v0.9.540)
bool SkinScattering() { return g_sssChosen.load(); }
void SetSkinScattering(bool value) {
    g_sssChosen.store(value);
    g_earForced.store(value);   // (v0.9.541: the ears' transmission with it)
    ApplySkinScattering();
    ApplyEarTransmission();
}
void SetViewerSkinScattering(bool on) {
    g_sssViewer.store(on, std::memory_order_release);
    ApplySkinScattering();
}
std::uint32_t TakeSkinScatteringAsked() { return g_sssAsked.exchange(0); }
void SetShadowSharpness(float value) { g_shadowSharpness.store(std::clamp(value, 1.0f, 4.0f)); }
void SetFogDistance(float value) { g_fog.store(std::clamp(value, 0.5f, 4.0f)); }
bool HairOffsetAvailable() { return g_hairOffsetAvailable.load(); }
float HairSelfShadowOffset() { return g_hairOffset.load(); }
void SetHairSelfShadowOffset(float value) { g_hairOffset.store(std::clamp(value, 0.0f, 64.0f)); }
}  // namespace RenderTweaks
