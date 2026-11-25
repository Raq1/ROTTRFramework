#include "cutscene_viewer.h"
#include "cutscenes.h"
#include "ui.h"
#include "features.h"
#include "hud_watch.h"
#include "custom_outfits.h"
#include "instance_anim.h"
#include "settings.h"
#include "script_engine.h"
#include "game_build.h"
#include "bone_patch.h"
#include "overlay.h"
#include "dx12_overlay.h"
#include "eight_weight_patch.h"
#include "swim_breath.h"
#include "combat_air.h"
#include "acro.h"
#include "grapple.h"
#include "enemy_ai.h"
#include "brawl.h"
#include "suite_link.h"
#include "save_dialog.h"
#include "material_names.h"
#include "hair_layout.h"
#include "glow_color.h"
#include "silencer.h"
#include "dx12_root_fix.h"
#include "dlaa.h"
#include "render_tweaks.h"
#include "shadow_blend.h"
#include "rt_probe.h"
#include "larafm.h"
#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <iterator>
#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <limits>
#include <cmath>
#include <cmath>
#include <limits>
#include <array>

namespace {
bool g_showDiagnostics = false;
bool g_showAbout = false;
bool g_showHairEditor = false;
bool g_showInstanceBrowser = false;
bool g_showLightEditor = false;
bool g_showMaterialEditor = false;



bool g_instanceClothTabActiveThisFrame = false;
bool g_instanceCollisionTabActiveThisFrame = false;
bool g_instanceMarkerTabActiveThisFrame = false;
char g_unitSearch[128]{};
std::vector<Features::UnitInfo> g_unitCache;
std::vector<int> g_unitFiltered;
std::uint64_t g_unitCacheVersion = ~std::uint64_t{0};
bool g_unitFilterDirty = true;
char g_materialSearch[128]{};
char g_lastMaterialSearch[128]{};
std::vector<Features::MaterialInfo> g_materialCache;
std::vector<int> g_materialFiltered;
std::uint64_t g_materialCacheVersion = ~std::uint64_t{0};
std::uintptr_t g_selectedMaterialAddress = 0;
Features::MaterialDetails g_materialDetailsCache{};
std::vector<Features::MaterialTextureInfo> g_materialTextureCache;
std::vector<Features::MaterialConstantInfo> g_materialConstantCache;
std::vector<Features::KnownTextureInfo> g_materialKnownTextureCache;
std::uint64_t g_materialDetailVersion = ~std::uint64_t{0};
bool g_materialHoverHighlight = true;
std::vector<Features::LightInfo> g_lightCache;
std::uint64_t g_lightCacheVersion = ~std::uint64_t{0};
std::vector<Features::GameLightInfo> g_gameLightCache;
std::uint64_t g_gameLightCacheVersion = ~std::uint64_t{0};
enum class LightSelectionKind : int { None=0, Game, Custom };
LightSelectionKind g_lightSelectionKind = LightSelectionKind::None;
std::uint32_t g_selectedLightId = 0;
std::uint64_t g_selectedGameLightToken = 0;
int g_selectedGameLightCacheIndex = -1;
Features::LightSettings g_lightEdit{};
bool g_lightEditLoaded = false;
int g_selectedClothGroup = 0;
std::uint32_t g_loadedClothInstanceId = 0;
int g_loadedClothConfig = -999;
unsigned int g_loadedClothGlobalGroup = 0xFFFFFFFFu;
Features::ClothStripGroupSettings g_clothEdit{};
Features::PlayerTransform g_manualTarget{};
bool g_manualTargetInitialized = false;
bool g_manualPositionFieldActive = false;
bool g_manualRotationFieldActive = false;
char g_playerStatus[128]{};
std::vector<Features::SkillInfo> g_skillCache;
std::vector<std::string> g_skillDisplayCache;
std::uint64_t g_skillCacheVersion = ~std::uint64_t{0};
char g_skillUiStatus[128]{};
char g_instanceSearch[128]{};
char g_outfitSearch[64]{};   // the Outfits menu's search (v0.9.439)
char g_lastInstanceSearch[128]{};
std::vector<Features::InstanceInfo> g_instanceCache;
std::vector<int> g_instanceFiltered;
std::uint64_t g_instanceCacheVersion = ~std::uint64_t{0};
std::uint32_t g_selectedInstanceId = 0;
std::uint64_t g_selectedInstanceComponentId = 0;
Features::PlayerTransform g_instanceEdit{};
bool g_instanceEditLoaded = false;
// Live transform (v0.9.291): the fields follow the game-thread sample unless that field is being dragged; after an
// edit is queued, samples taken before the game thread applied it are skipped (they would snap the field back).
bool g_instancePositionFieldActive = false;
bool g_instanceRotationFieldActive = false;
std::uint64_t g_instanceLiveAcceptSequence = 0;
void MarkInstanceTransformQueued() {
    g_instanceLiveAcceptSequence = Features::InstanceTransformWatchSequence() + 2u;
}
char g_instanceUiStatus[128]{};
struct InstanceOriginalKey {
    std::uint32_t instanceId{};
    std::uint64_t componentId{};
    bool operator==(const InstanceOriginalKey& other) const {
        return instanceId == other.instanceId && componentId == other.componentId;
    }
};
struct InstanceOriginalKeyHash {
    std::size_t operator()(const InstanceOriginalKey& key) const noexcept {
        std::size_t h = std::hash<std::uint64_t>{}(key.componentId);
        h ^= std::hash<std::uint32_t>{}(key.instanceId) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};
std::unordered_map<InstanceOriginalKey, Features::PlayerTransform, InstanceOriginalKeyHash> g_instanceOriginalTransforms;
char g_clothUiStatus[160]{};
std::vector<Features::CapsuleCollisionInfo> g_capsuleCollisionCache;
std::uint64_t g_capsuleCollisionCacheVersion = ~std::uint64_t{0};
int g_selectedCapsuleCollision = 0;
Features::CapsuleCollisionInfo g_capsuleCollisionEdit{};
bool g_capsuleCollisionEditLoaded = false;
char g_capsuleCollisionUiStatus[160]{};
bool g_drawSelectedCapsuleCollision = true;
std::vector<Features::CapsuleCollisionDrawInfo> g_capsuleCollisionDrawCache;
std::uint64_t g_capsuleCollisionDrawCacheVersion = ~std::uint64_t{0};
char g_hairUiStatus[192]{};
int g_selectedHairPhysicsGroup = 0;
int g_selectedHairRenderGroup = 0;
int g_selectedHairConstantGroup = 0;
int g_loadedHairPhysicsGroup = -1;
int g_loadedHairRenderGroup = -1;
int g_loadedHairConstantGroup = -1;
unsigned int g_loadedHairSimProfile = 0xFFFFFFFFu;
unsigned int g_loadedHairRenderProfile = 0xFFFFFFFFu;
unsigned int g_loadedHairRenderGroupProfile = 0xFFFFFFFFu;
std::uint16_t g_loadedHairPlayerObject = 0;
Features::HairPhysicsGroupSettings g_hairPhysicsEdit{};
Features::HairRenderGroupSettings g_hairRenderGroupEdit{};
Features::HairConstantGroupInfo g_hairConstantEdit{};
Features::HairRenderingProfileInfo g_hairRenderingEdit{};
Features::HairAdvancedSettings g_hairAdvancedEdit{};
bool g_hairAdvancedLoaded = false;
float g_hairProfileTransition = 0.15f;
char g_birthObjectName[128]{};
char g_lastBirthSearch[128]{};
std::vector<Features::ZoneSuggestion> g_birthSuggestions;
bool g_showBirthSuggestions = false;
// Instances window, Animation tab
char g_animSearch[128]{};
char g_animLastSearch[128]{};
std::vector<InstanceAnim::AnimListEntry> g_animSuggestions;
std::uint64_t g_animSuggestionsVersion = ~std::uint64_t{0};
InstanceAnim::AnimListEntry g_animSelected{};
bool g_animHasSelection = false;
bool g_animAllLibraries = false;
bool g_animLoop = true;
float g_animSpeed = 1.0f;
std::uint32_t g_animListInstance = 0;
std::uint64_t g_animListComponent = 0;
bool g_animListAllRequested = false;
bool g_animListRequested = false;
std::vector<Features::CollisionMarkerInfo> g_markerCache;
std::uint64_t g_markerCacheVersion = ~std::uint64_t{0};
char g_markerSearch[128]{};
char g_lastMarkerSearch[128]{};
std::vector<int> g_markerFiltered;
struct MarkerEditState {
    bool initialized{};
    float matrix[16]{};
    int boneSegment{};
};
std::unordered_map<std::uint64_t, MarkerEditState> g_markerEdits;

bool g_drawInstances = false;
bool g_drawTriggers = false;
bool g_drawMarkup = false;

float UiScale() {
    return std::clamp(Settings::Get().uiScale, 1.0f, 2.5f);
}

float UiPx(float value) {
    return value > 0.0f ? value * UiScale() : value;
}

ImVec2 UiSize(float x, float y) {
    return ImVec2(UiPx(x), UiPx(y));
}




std::uint32_t g_lastInstanceHighlightId = 0;
std::uint64_t g_lastInstanceHighlightComponentId = 0;
// Hover highlight (v0.9.292): the row under the mouse this frame, and the last one with a short grace period so
// moving across the gaps between rows does not flicker the highlight off and on.
std::uint32_t g_instanceHoverId = 0;
std::uint64_t g_instanceHoverComponentId = 0;
std::uint32_t g_instanceHoverHeldId = 0;
std::uint64_t g_instanceHoverHeldComponentId = 0;
double g_instanceHoverHeldUntil = 0.0;
bool g_instanceHoverSent = false;

std::vector<Features::DrawInstanceLabel> g_drawInstanceCache;
std::vector<Features::DrawTriggerBox> g_drawTriggerCache;
std::vector<Features::DrawLine3D> g_drawMarkupCache;
std::uint64_t g_drawInstanceCacheVersion = ~std::uint64_t{0};
std::uint64_t g_drawTriggerCacheVersion = ~std::uint64_t{0};
std::uint64_t g_drawMarkupCacheVersion = ~std::uint64_t{0};
std::array<int, 26> g_drawMarkupTypeCounts{};   // boxes a type in the cache
// Draw menu options (this session)
int g_drawInstanceLabelLimit = 150;
bool g_drawInstanceDetails = true;
bool g_drawTriggerLabels = true;
bool g_drawMarkupLabels = true;
bool g_drawMarkupLegend = true;

ImU32 MarkupTypeColor(int type) {
    switch (type) {
        case 1: return IM_COL32(255, 215, 60, 240);    // LedgeGrab
        case 2: return IM_COL32(150, 240, 120, 235);   // JumpLanding
        case 3: return IM_COL32(170, 170, 170, 220);   // EnemyDropdown
        case 4: case 5: case 6: return IM_COL32(255, 140, 40, 240);   // poles
        case 7: case 11: return IM_COL32(240, 90, 240, 240);          // Zipline, Rope
        case 8: return IM_COL32(255, 120, 170, 240);   // RailSlide
        case 9: return IM_COL32(80, 160, 255, 240);    // Ladder
        case 10: return IM_COL32(60, 120, 255, 200);   // Water
        case 12: case 13: return IM_COL32(180, 150, 230, 210);        // cover
        case 14: return IM_COL32(255, 70, 70, 240);    // Perch
        case 15: return IM_COL32(190, 255, 60, 240);   // BalanceBeam
        case 16: return IM_COL32(60, 220, 200, 240);   // Pillar
        case 17: return IM_COL32(255, 255, 255, 230);  // VehicleJumpTarget
        case 18: return IM_COL32(190, 140, 90, 230);   // ConstrainedGround
        case 19: return IM_COL32(200, 40, 40, 230);    // EdgeStop
        case 23: case 24: return IM_COL32(90, 210, 90, 240);          // trees
        default: return IM_COL32(70, 210, 255, 235);
    }
}

void DrawShadowedText(ImDrawList* draw, ImVec2 at, ImU32 color, const char* text) {
    draw->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, (color >> IM_COL32_A_SHIFT) & 0xFF), text);
    draw->AddText(at, color, text);
}

// The markup types present, with their colors (a small window at the screen's lower left).
void DrawMarkupLegend() {
    int types = 0;
    for (const int count : g_drawMarkupTypeCounts) types += count > 0 ? 1 : 0;
    if (!types) return;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(UiPx(12.0f), display.y - UiPx(12.0f)), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##markup_legend", nullptr, flags)) {
        ImGui::TextUnformatted("Markup");
        for (int type = 0; type < static_cast<int>(g_drawMarkupTypeCounts.size()); ++type) {
            if (!g_drawMarkupTypeCounts[type]) continue;
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(MarkupTypeColor(type)), "%s  %d",
                               Features::DrawMarkupTypeName(type), g_drawMarkupTypeCounts[type]);
        }
    }
    ImGui::End();
}

struct DrawCameraBasis {
    float pos[3]{};
    float right[3]{};
    float forward[3]{};
    float up[3]{};
    float tanHalfFov{};
    float aspect{};
    float width{};
    float height{};
};

bool BuildDrawCameraBasis(const Features::DrawCameraState& camera, DrawCameraBasis& out) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (!camera.valid || display.x <= 1.0f || display.y <= 1.0f) return false;

    float qx = camera.quaternion[0], qy = camera.quaternion[1];
    float qz = camera.quaternion[2], qw = camera.quaternion[3];
    const float lenSq = qx*qx + qy*qy + qz*qz + qw*qw;
    if (!(lenSq > 1.0e-10f) || !std::isfinite(lenSq)) return false;
    const float invLen = 1.0f / std::sqrt(lenSq);
    qx *= invLen; qy *= invLen; qz *= invLen; qw *= invLen;

    out.pos[0] = camera.position[0]; out.pos[1] = camera.position[1]; out.pos[2] = camera.position[2];
    
    out.right[0] = 1.0f - 2.0f * (qy*qy + qz*qz);
    out.right[1] = 2.0f * (qx*qy + qz*qw);
    out.right[2] = 2.0f * (qx*qz - qy*qw);
    out.forward[0] = 2.0f * (qx*qy - qz*qw);
    out.forward[1] = 1.0f - 2.0f * (qx*qx + qz*qz);
    out.forward[2] = 2.0f * (qy*qz + qx*qw);
    out.up[0] = 2.0f * (qx*qz + qy*qw);
    out.up[1] = 2.0f * (qy*qz - qx*qw);
    out.up[2] = 1.0f - 2.0f * (qx*qx + qy*qy);

    float fov = camera.fov;
    if (fov > 3.2f) fov *= 0.01745329251994329577f;
    fov = std::clamp(fov, 0.05f, 3.05f);
    out.tanHalfFov = std::tan(fov * 0.5f);
    out.aspect = display.x / display.y;
    out.width = display.x;
    out.height = display.y;
    return std::isfinite(out.tanHalfFov) && out.tanHalfFov > 0.001f;
}

float DrawDepth(const DrawCameraBasis& camera, const float point[3]) {
    const float dx = point[0] - camera.pos[0];
    const float dy = point[1] - camera.pos[1];
    const float dz = point[2] - camera.pos[2];
    return dx * camera.forward[0] + dy * camera.forward[1] + dz * camera.forward[2];
}

bool ProjectDrawPoint(const DrawCameraBasis& camera, const float point[3], ImVec2& screen, bool requireOnScreen = true) {
    const float dx = point[0] - camera.pos[0];
    const float dy = point[1] - camera.pos[1];
    const float dz = point[2] - camera.pos[2];
    const float depth = dx * camera.forward[0] + dy * camera.forward[1] + dz * camera.forward[2];
    if (depth <= 0.05f) return false;
    const float x = dx * camera.right[0] + dy * camera.right[1] + dz * camera.right[2];
    const float z = dx * camera.up[0] + dy * camera.up[1] + dz * camera.up[2];
    const float ndcX = x / (depth * camera.tanHalfFov * camera.aspect);
    const float ndcY = z / (depth * camera.tanHalfFov);
    if (!std::isfinite(ndcX) || !std::isfinite(ndcY)) return false;
    screen.x = camera.width * (0.5f + 0.5f * ndcX);
    screen.y = camera.height * (0.5f - 0.5f * ndcY);
    if (requireOnScreen && (screen.x < -64.0f || screen.x > camera.width + 64.0f ||
                            screen.y < -32.0f || screen.y > camera.height + 32.0f))
        return false;
    return true;
}

bool ProjectDrawLine(const DrawCameraBasis& camera, const float aIn[3], const float bIn[3], ImVec2& aOut, ImVec2& bOut) {
    float a[3]{aIn[0], aIn[1], aIn[2]};
    float b[3]{bIn[0], bIn[1], bIn[2]};
    float da = DrawDepth(camera, a);
    float db = DrawDepth(camera, b);
    constexpr float nearDepth = 0.05f;
    if (da <= nearDepth && db <= nearDepth) return false;
    if (da <= nearDepth || db <= nearDepth) {
        const float denom = db - da;
        if (std::fabs(denom) < 1.0e-7f) return false;
        const float t = (nearDepth - da) / denom;
        float clipped[3]{};
        for (int i = 0; i < 3; ++i) clipped[i] = a[i] + (b[i] - a[i]) * t;
        if (da <= nearDepth) { for (int i = 0; i < 3; ++i) a[i] = clipped[i]; }
        else { for (int i = 0; i < 3; ++i) b[i] = clipped[i]; }
    }
    if (!ProjectDrawPoint(camera, a, aOut, false) || !ProjectDrawPoint(camera, b, bOut, false)) return false;
    
    if ((aOut.x < 0.0f && bOut.x < 0.0f) || (aOut.x > camera.width && bOut.x > camera.width) ||
        (aOut.y < 0.0f && bOut.y < 0.0f) || (aOut.y > camera.height && bOut.y > camera.height))
        return false;
    return true;
}



void DrawWorldLine(ImDrawList* draw, const DrawCameraBasis& camera,
                   const float a[3], const float b[3], ImU32 color, float thickness) {
    if (!draw) return;
    ImVec2 sa{}, sb{};
    if (ProjectDrawLine(camera, a, b, sa, sb))
        draw->AddLine(sa, sb, color, thickness);
}

bool Normalize3(float v[3]) {
    const float lenSq = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
    if (!(lenSq > 1.0e-10f) || !std::isfinite(lenSq)) return false;
    const float inv = 1.0f / std::sqrt(lenSq);
    v[0] *= inv; v[1] *= inv; v[2] *= inv;
    return true;
}

void Cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1]*b[2] - a[2]*b[1];
    out[1] = a[2]*b[0] - a[0]*b[2];
    out[2] = a[0]*b[1] - a[1]*b[0];
}

void DrawCapsuleCollisionWireframe(ImDrawList* draw, const DrawCameraBasis& camera,
                                   const Features::CapsuleCollisionDrawInfo& capsule,
                                   ImU32 color) {
    if (!draw || !std::isfinite(capsule.radius) || capsule.radius <= 0.0f) return;
    constexpr int kSegments = 20;
    constexpr float kTwoPi = 6.28318530717958647692f;
    constexpr float kPi = 3.14159265358979323846f;

    float axis[3]{
        capsule.position2[0] - capsule.position1[0],
        capsule.position2[1] - capsule.position1[1],
        capsule.position2[2] - capsule.position1[2]
    };
    const bool hasAxis = Normalize3(axis);
    if (!hasAxis) { axis[0] = 0.0f; axis[1] = 0.0f; axis[2] = 1.0f; }

    const float helper[3] = {0.0f, std::fabs(axis[2]) > 0.90f ? 1.0f : 0.0f,
                            std::fabs(axis[2]) > 0.90f ? 0.0f : 1.0f};
    float u[3]{}; Cross3(axis, helper, u);
    if (!Normalize3(u)) { u[0] = 1.0f; u[1] = 0.0f; u[2] = 0.0f; }
    float v[3]{}; Cross3(axis, u, v);
    Normalize3(v);

    auto ringPoint = [&](const float center[3], float angle, float out[3]) {
        const float c = std::cos(angle), s = std::sin(angle);
        for (int k = 0; k < 3; ++k)
            out[k] = center[k] + capsule.radius * (c*u[k] + s*v[k]);
    };

    
    for (int end = 0; end < 2; ++end) {
        const float* center = end == 0 ? capsule.position1 : capsule.position2;
        float prev[3]{}, cur[3]{};
        ringPoint(center, 0.0f, prev);
        for (int i = 1; i <= kSegments; ++i) {
            ringPoint(center, kTwoPi * static_cast<float>(i) / static_cast<float>(kSegments), cur);
            DrawWorldLine(draw, camera, prev, cur, color, 1.8f);
            for (int k = 0; k < 3; ++k) prev[k] = cur[k];
        }
    }

    
    for (int rail = 0; rail < 4; ++rail) {
        const float angle = 0.5f * kPi * static_cast<float>(rail);
        float a[3]{}, b[3]{};
        ringPoint(capsule.position1, angle, a);
        ringPoint(capsule.position2, angle, b);
        DrawWorldLine(draw, camera, a, b, color, 1.8f);
    }

    
    
    const float* radialAxes[2] = {u, v};
    for (const float* radial : radialAxes) {
        for (int end = 0; end < 2; ++end) {
            const float* center = end == 0 ? capsule.position1 : capsule.position2;
            const float sign = end == 0 ? -1.0f : 1.0f;
            float prev[3]{};
            for (int k = 0; k < 3; ++k) prev[k] = center[k] + capsule.radius * radial[k];
            for (int i = 1; i <= kSegments / 2; ++i) {
                const float t = kPi * static_cast<float>(i) / static_cast<float>(kSegments / 2);
                const float c = std::cos(t), s = std::sin(t);
                float cur[3]{};
                for (int k = 0; k < 3; ++k)
                    cur[k] = center[k] + capsule.radius * (c*radial[k] + sign*s*axis[k]);
                DrawWorldLine(draw, camera, prev, cur, color, 1.8f);
                for (int k = 0; k < 3; ++k) prev[k] = cur[k];
            }
        }
    }

    
    DrawWorldLine(draw, camera, capsule.position1, capsule.position2, color, 1.0f);
}

void RefreshDrawOverlayCaches() {
    if (g_drawInstances) {
        const auto version = Features::DrawInstanceVersion();
        if (version != g_drawInstanceCacheVersion) {
            const int count = Features::DrawInstanceCount();
            g_drawInstanceCache.resize(std::max(0, count));
            const int copied = count > 0 ? Features::CopyDrawInstances(g_drawInstanceCache.data(), count) : 0;
            g_drawInstanceCache.resize(std::max(0, copied));
            g_drawInstanceCacheVersion = version;
        }
    }
    if (g_drawTriggers) {
        const auto version = Features::DrawTriggerVersion();
        if (version != g_drawTriggerCacheVersion) {
            const int count = Features::DrawTriggerCount();
            g_drawTriggerCache.resize(std::max(0, count));
            const int copied = count > 0 ? Features::CopyDrawTriggers(g_drawTriggerCache.data(), count) : 0;
            g_drawTriggerCache.resize(std::max(0, copied));
            g_drawTriggerCacheVersion = version;
        }
    }
    if (g_drawMarkup) {
        const auto version = Features::DrawMarkupVersion();
        if (version != g_drawMarkupCacheVersion) {
            const int count = Features::DrawMarkupLineCount();
            g_drawMarkupCache.resize(std::max(0, count));
            const int copied = count > 0 ? Features::CopyDrawMarkupLines(g_drawMarkupCache.data(), count) : 0;
            g_drawMarkupCache.resize(std::max(0, copied));
            g_drawMarkupCacheVersion = version;
            g_drawMarkupTypeCounts.fill(0);
            std::uint32_t lastBox = ~0u;
            for (const auto& line : g_drawMarkupCache) {
                if (line.box == lastBox) continue;
                lastBox = line.box;
                if (line.type < g_drawMarkupTypeCounts.size()) ++g_drawMarkupTypeCounts[line.type];
            }
        }
    }
}

void SetPlayerStatus(const char* text) {
    strncpy_s(g_playerStatus, text ? text : "", _TRUNCATE);
}

// (v0.9.519) Enhancements > Graphics (out of the General tab): render_tweaks.h, shadow_blend.h.
void DrawGraphicsSection() {
    // (v0.9.487) render_tweaks.h: past the game's own graphics options
    auto slider = [](const char* label, float value, bool available, void (*set)(float), float Settings::State::*field,
                     const char* tip, float low = 0.5f, float high = 4.0f) {
        if (!available) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(UiPx(170.0f));
        if (ImGui::SliderFloat(label, &value, low, high, "x%.2f", ImGuiSliderFlags_AlwaysClamp)) {
            set(value);
            Settings::Get().*field = value;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) Settings::Save();
        if (!available) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s%s", tip, available ? "" : "\nUnavailable: see ROTTRFramework.log (RenderTweaks).");
    };
    slider("LOD distance", RenderTweaks::LodDistance(), RenderTweaks::LodAvailable(), &RenderTweaks::SetLodDistance,
           &Settings::State::lodDistance,
           "How far detailed models are kept before the game switches to simpler ones (x1 = the game's own, from\n"
           "its quality options). Higher: more detail in the distance, more GPU time. Up to x16. Takes effect at\n"
           "once. Foliage has its own (below).",
           0.5f, 16.0f);
    slider("Foliage LOD distance", RenderTweaks::FoliageLodDistance(), RenderTweaks::LodAvailable(),
           &RenderTweaks::SetFoliageLodDistance, &Settings::State::foliageLodDistance,
           "How far the grass keeps its blades (then flat billboards, then nothing), and the same for the\n"
           "models the game never lets its LOD option push out (it caps them at x1). Higher: grass and plants\n"
           "full farther away, more GPU time. Up to x16. Takes effect at once.",
           0.5f, 16.0f);
    slider("Shadow distance", RenderTweaks::ShadowDistance(), RenderTweaks::ShadowAvailable(),
           &RenderTweaks::SetShadowDistance, &Settings::State::shadowDistance,
           "How far the sun casts shadows (x1 = the level's own): its last shadow cascade's end, and how far\n"
           "each object keeps casting its shadow (each has its own range too). Higher keeps far shadows, but\n"
           "the far cascades cover more ground, so their shadows are softer. Up to x16. Takes effect at once.",
           0.5f, 16.0f);
    slider("Shadow sharpness", RenderTweaks::ShadowSharpness(), RenderTweaks::ShadowAvailable(),
           &RenderTweaks::SetShadowSharpness, &Settings::State::shadowSharpness,
           "Sharper sun shadows close to the camera (x1 = the level's own): the nearer cascades cover\n"
           "less ground, so their pixels are smaller (the nearest never under 4.5 m, so Lara stays in it).\n"
           "Works best with Shadow distance raised too. Takes effect at once.",
           1.0f);
    // (v0.9.523) each light's own shadow map size (render_tweaks.h)
    {
        const bool available = RenderTweaks::ShadowMapSizeAvailable();
        static const char* const kSizes[] = {"x1 (game)", "x2", "x4", "x8"};   // (v0.9.524: x8)
        const int size = RenderTweaks::ShadowMapSize();
        int index = size >= 8 ? 3 : (size >= 4 ? 2 : (size >= 2 ? 1 : 0));
        if (!available) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(UiPx(170.0f));
        if (ImGui::Combo("Shadow map size", &index, kSizes, IM_ARRAYSIZE(kSizes))) {
            const int chosen = index == 3 ? 8 : (index == 2 ? 4 : (index == 1 ? 2 : 1));
            RenderTweaks::SetShadowMapSize(chosen);
            Settings::Get().shadowMapSize = chosen;
            Settings::Save();
        }
        if (!available) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Each shadowed light's own shadow map made larger (the sun's cascades, lamps, torches):\n"
                              "finer shadows up close, such as the hair's on Lara's neck. Only while all of the frame's\n"
                              "maps still fit the shadow atlas; a light already at the largest a map can be stays as it is\n"
                              "(ROTTRFramework.log lists what each light got). x8 changes only the lights that ask for\n"
                              "small maps (most are at the largest with x4). More GPU time for shadows. Takes effect at\n"
                              "once.%s",
                              available ? "" : "\nUnavailable: see ROTTRFramework.log (RenderTweaks).");
    }
    // (v0.9.525) the lamps' shadow filter (shadow_blend.h)
    {
        static const char* const kSoftness[] = {"Off (game)", "Soft", "Softer", "Softest"};
        int softness = ShadowBlend::LampShadowSoftness();
        ImGui::SetNextItemWidth(UiPx(170.0f));
        if (ImGui::Combo("Lamp shadow softness", &softness, kSoftness, IM_ARRAYSIZE(kSoftness))) {
            ShadowBlend::SetLampShadowSoftness(softness);
            Settings::Get().lampShadowSoftness = softness;
            Settings::Save();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Smoother edges for the shadows of lamps, torches and light shafts (the spot lights): the\n"
                              "game looks up 2 x 2 points of the shadow map, which leaves its pixels as stair steps (the\n"
                              "hair's shadow on Lara's neck); these look up 4 x 4, 1 (Soft), 1.5 (Softer) or 2 (Softest)\n"
                              "pixels apart: softer edges, a little more GPU time. The sun's shadows are not changed.\n"
                              "Takes effect at once (the lights' pass is made again in the background).");
    }
    // (v0.9.526) contact shadows in the lights' pass (shadow_blend.h)
    {
        static const char* const kLevels[] = {"Off", "Light", "Medium", "Strong"};
        int level = ShadowBlend::ContactShadows();
        ImGui::SetNextItemWidth(UiPx(170.0f));
        if (ImGui::Combo("Contact shadows", &level, kLevels, IM_ARRAYSIZE(kLevels))) {
            ShadowBlend::SetContactShadows(level);
            Settings::Get().contactShadows = level;
            Settings::Save();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Small shadows the shadow maps miss: for the sun, lamps and the bulbs and glows that cast no\n"
                              "shadow at all, a short ray from each point toward the light looks for anything just in\n"
                              "front of it on screen (lips over the inside of the mouth, the nose, the collar, hair on skin,\n"
                              "objects on the ground). Only what is on screen counts. A little more GPU time.\n"
                              "Takes effect at once (the lights' pass is made again in the background).");
        // (v0.9.527: applied when the slider is let go: each length is a pipeline of its own)
        float length = Settings::Get().contactShadowLength;
        if (level == 0) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(UiPx(170.0f));
        if (ImGui::SliderFloat("Contact shadow length", &length, 4.0f, 30.0f, "%.0f cm", ImGuiSliderFlags_AlwaysClamp))
            Settings::Get().contactShadowLength = length;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            ShadowBlend::SetContactShadowLength(Settings::Get().contactShadowLength);
            Settings::Save();
        }
        // (v0.9.541: the debug view's "Show contact shadows only" taken out; ShadowBlend::SetContactShadowsDebug stays)
        if (level == 0) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("How far each ray looks toward the light. Shorter: only the closest contact (creases, the\n"
                              "mouth); longer: larger shapes too, with more chance of halos at edges. Takes effect when\n"
                              "the slider is let go.");
    }
    {   // (v0.9.561) ray-traced lamp shadows: the ray tracing probe's mask read by the lights' pass
        bool rt = Settings::Get().rtLampShadows;
        if (ImGui::Checkbox("Ray-traced lamp shadows", &rt)) {
            Settings::Get().rtLampShadows = rt;
            RtProbe::SetLampShadows(rt);   // (v0.9.562: it switches the lights' pass's reading)
            Settings::Save();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Real shadows for the three lamps or fires nearest the camera: a ray from each point toward the\n"
                              "lamp through the whole scene (Lara, people, leaves and all), so lamps that cast no shadow in\n"
                              "the game now do. Needs DirectX Raytracing; takes GPU time (about 2 ms at 1440p). Takes\n"
                              "effect within a moment: the lighting pass reads the shadows once the framework has checked\n"
                              "that they reach it.");
        if (rt) ImGui::TextDisabled("    %s", RtProbe::LampShadowsState());   // (v0.9.562)
        // (v0.9.570) ray-traced sun shadows: the probe's sun mask read by the sun's shadow pass
        bool sun = Settings::Get().rtSunShadows;
        if (ImGui::Checkbox("Ray-traced sun shadows", &sun)) {
            Settings::Get().rtSunShadows = sun;
            RtProbe::SetSunShadows(sun);
            Settings::Save();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The sun's (or moon's) shadows by rays instead of the shadow maps, up to 250 m from the camera:\n"
                              "sharp and in the right place at any distance, small things included; past 250 m, and where\n"
                              "the ray-traced scene does not match what is drawn, the game's own shadow maps. Needs DirectX\n"
                              "Raytracing; takes GPU time. Takes effect within a moment, once the framework has checked\n"
                              "that they reach the sun's shadow pass.");
        if (sun) ImGui::TextDisabled("    %s", RtProbe::SunShadowsState());
        float softness = Settings::Get().rtLampSoftness;   // (v0.9.564)
        if (!rt && !sun) ImGui::BeginDisabled();   // (v0.9.570: the sun's too)
        ImGui::SetNextItemWidth(UiPx(170.0f));
        // (v0.9.568: its own label; it was "Lamp shadow softness", the same as the shadow maps' combo above: one ID)
        if (ImGui::SliderFloat("Ray-traced shadow softness", &softness, 0.0f, 3.0f, softness > 0.0f ? "%.2f" : "sharp",
                               ImGuiSliderFlags_AlwaysClamp)) {
            Settings::Get().rtLampSoftness = softness;
            RtProbe::SetLampSoftness(softness);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) Settings::Save();
        if (!rt && !sun) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("How large the lamps, fires and the sun are taken to be. Larger: softer shadow edges, widening\n"
                              "away from what casts them; 0: sharp (one ray each, cheaper). 1 sizes each lamp from its\n"
                              "reach (a lantern a few cm, a big fire up to 30 cm) and the sun at 0.5 degree. Takes effect\n"
                              "at once.");
    }
    // (v0.9.543) the ray tracing probe (rt_probe.h; not saved)
    {
        int probe = RtProbe::Mode();
        const char* probeModes[] = {"Off", "Full screen", "Split screen", "Overlay"};
        ImGui::SetNextItemWidth(UiPx(170.0f));
        if (ImGui::Combo("Ray tracing probe", &probe, probeModes, 4)) RtProbe::SetMode(probe);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A test, not an effect: the scene the framework builds for ray tracing from the game's own\n"
                              "draws, shown by tracing a ray per pixel (each object its own colour, lit from the camera).\n"
                              "Split screen: the right half ray traced beside the game's image; Overlay: over it, half\n"
                              "see-through (to check they line up). Lara and the characters are posed every frame as the\n"
                              "game poses them (not 8-weight custom outfits yet); grass and the sky are not in it. Uses GPU\n"
                              "time and up to about 2 GB of video memory while on. Not saved.");
        if (probe) {
            bool kinds = RtProbe::KindView();   // (v0.9.548)
            if (ImGui::Checkbox("Colour by kind", &kinds)) RtProbe::SetKindView(kinds);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Each object coloured by how the probe took it instead of its own colour: grey static,\n"
                                  "green instanced, yellow a skinned model drawn without its bones, red skinned (four\n"
                                  "weights), blue skinned (one bone), cyan tessellated, orange skinned and shared by several\n"
                                  "characters, pink skinned and drawn only without depth. Not saved.");
            bool mask = RtProbe::ShadowMask();   // (v0.9.560)
            if (ImGui::Checkbox("Lamp shadow mask", &mask)) RtProbe::SetShadowMask(mask);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Ray-traced shadows of the three lamps nearest the camera, over the view: where a lamp's\n"
                                  "light is blocked, its colour channel is taken away (red, green, blue: lamps 1 to 3).\n"
                                  "A first step: the game's lighting does not use it yet. Not saved.");
            ImGui::TextDisabled("  %s", RtProbe::Status());
        }
    }
    slider("Fog distance", RenderTweaks::FogDistance(), RenderTweaks::FogAvailable(), &RenderTweaks::SetFogDistance,
           &Settings::State::fogDistance,
           "Where the distance fog starts and ends (x1 = the game's own). Higher: clearer views far away.\n"
           "Takes effect at once. The low height fog in valleys is left as the levels set it.");

    // (v0.9.518) the hair's self-shadow, in its lighting pass (shadow_blend.h); its shadow on her is left whole
    float hairDistance = ShadowBlend::HairSelfShadowDistance();
    ImGui::SetNextItemWidth(UiPx(170.0f));
    if (ImGui::SliderFloat("Hair self-shadow distance", &hairDistance, 1.0f, 40.0f,
                           hairDistance <= 1.05f ? "1 cm (game)" : "%.0f cm", ImGuiSliderFlags_AlwaysClamp)) {
        ShadowBlend::SetHairSelfShadowDistance(hairDistance);
        Settings::Get().hairSelfShadowDistance = hairDistance;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) Settings::Save();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The hair takes everything between it and the light as more hair to shine through, its own\n"
                          "strands too, so it shadows itself (and flickers). Within this distance in front of it, it\n"
                          "ignores what is there: no shadow on itself, while her head, her body and the scenery still\n"
                          "shadow it, and the shadow it casts on her and the ground stays as it is. 1 cm = the game's.\n"
                          "Takes effect the next time the game starts.");
    // (v0.9.607) "Hair shadows" removed: the hair always casts its shadow, as in the game
    // (v0.9.540) the skin's subsurface scattering in play (render_tweaks.h; v0.9.607: the list's last)
    {
        const bool available = RenderTweaks::SkinScatteringAvailable();
        if (!available) ImGui::BeginDisabled();
        bool skin = RenderTweaks::SkinScattering();
        if (ImGui::Checkbox("Skin scattering", &skin)) {
            RenderTweaks::SetSkinScattering(skin);
            Settings::Get().skinScattering = skin;
            Settings::Save();
        }
        if (!available) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The game's subsurface scattering for skin: light spreading under the skin of faces and\n"
                              "hands, which softens pores and the edges of shadows, and light shining through Lara's ears\n"
                              "from behind (their warm glow when backlit). The game draws both in its cutscenes only;\n"
                              "on: in play too. Off: as the game's scripts set them. Takes effect at once.%s",
                              available ? "" : "\nUnavailable: see ROTTRFramework.log (RenderTweaks).");
    }
}

// (v0.9.586) Enhancements > Gameplay: the hints and discovery notifications, weapon switching without a draw, Lara's
// gear (the Modding Suite's Hide Lower / Upper Gear for every outfit, her body gear and quiver strap) and the campsites
// near enemies (out of the General tab).
void GameplayCheckbox(const char* label, bool available, bool value, void (*set)(bool), const char* tip,
                      const char* unavailable) {
    if (!available) ImGui::BeginDisabled();
    if (ImGui::Checkbox(label, &value)) {
        set(value);
        Settings::Save();
    }
    if (!available) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s%s%s", tip, available ? "" : "\n", available ? "" : unavailable);
}

void DrawGameplaySection() {
    GameplayCheckbox("Hide hints and discovery notifications", Features::HideHintsAvailable(), Features::HideHintsEnabled(),
                     &Features::SetHideHintsEnabled,
                     "No tutorial hints or cards, and no \"New basecamp discovered\", \"Tomb discovered\" or\n"
                     "\"Tomb nearby\" notifications. The game carries on as if each hint had been shown as often\n"
                     "as it allows. Pickups, experience, objectives and the region names still show.",
                     "The game's tutorial and notification code did not match this executable.");
    GameplayCheckbox("Don't draw the weapon switched to", Features::NoDrawOnWeaponSwitchAvailable(),
                     Features::NoDrawOnWeaponSwitchEnabled(), &Features::SetNoDrawOnWeaponSwitchEnabled,
                     "Switching weapons (the weapon select, the number keys) only chooses the weapon: Lara puts it\n"
                     "on her back or in its holster instead of taking it in her hands. She draws it when you aim.\n"
                     "The key of the weapon chosen already does not draw it either.\n"
                     "With a weapon already in her hands, she swaps to the new one as usual. Her quiver is\n"
                     "drawn only while the bow is the chosen weapon.",
                     "The game's weapon switch code did not match this executable.");
    bool allowCampsitesNearEnemies = Features::AllowCampsitesNearEnemiesEnabled();
    const bool allowCampsitesNearEnemiesAvailable = Features::AllowCampsitesNearEnemiesAvailable();
    if (!allowCampsitesNearEnemiesAvailable) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Allow campsites near enemies", &allowCampsitesNearEnemies)) {
        Features::SetAllowCampsitesNearEnemiesEnabled(allowCampsitesNearEnemies);
        Settings::Save();
    }
    if (!allowCampsitesNearEnemiesAvailable) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Allows campsite/basecamp interactions even while nearby enemies would normally trigger\n"
            "the UNSAFE TO CAMP restriction. Other campsite requirements and unrelated combat-\n"
            "restricted interactions remain unchanged.%s",
            allowCampsitesNearEnemiesAvailable ? "" : "\nThe required interaction-profile hook is unavailable for this executable.");
    }
    // (v0.9.591) under the campsites
    GameplayCheckbox("Weapon select at the bottom left", HudWatch::WeaponSelectLayoutAvailable(),   // (v0.9.589)
                     HudWatch::WeaponSelectBottomLeft(), &HudWatch::SetWeaponSelectBottomLeft,
                     "The HUD's weapon select (the weapons and their ammo, shown when you switch) at the bottom\n"
                     "left of the screen instead of the bottom right, at the size below.",
                     "The weapon select's movie did not match this executable.");
    if (HudWatch::WeaponSelectBottomLeft()) {   // (v0.9.590)
        int size = HudWatch::WeaponSelectSize();
        ImGui::Indent(UiPx(18.0f));
        ImGui::SetNextItemWidth(UiPx(170.0f));
        if (ImGui::SliderInt("Weapon select size", &size, 30, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp))
            HudWatch::SetWeaponSelectSize(size);
        if (ImGui::IsItemDeactivatedAfterEdit()) Settings::Save();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Its size at the bottom left, as a share of the game's own size. At once.");
        ImGui::Unindent(UiPx(18.0f));
    }

}

// (v0.9.591) Lara's gear hiding (0.9.586's, out of Enhancements > Gameplay): in the Player tab.
void DrawLarasGearSection() {
    const bool gear = Features::PlayerGearHidingAvailable();
    auto& s = Settings::Get();
    if (!gear) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Hide Lower Gear", &s.hideLowerGear)) Settings::Save();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Her radio, holstered pistols, climbing axe, glowstick, knife and sheath, rope and belt\n"
                          "pouches are not drawn on her (in her hands they are), as the Modding Suite's Hide Lower\n"
                          "Gear does for one outfit.");
    ImGui::SameLine();
    if (ImGui::Checkbox("Hide Upper Gear", &s.hideUpperGear)) Settings::Save();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The bow, rifle and shotgun on her back are not drawn (in her hands they are), as the\n"
                          "Modding Suite's Hide Upper Gear does for one outfit.");
    if (!gear) ImGui::EndDisabled();
    const bool groups = Features::PlayerDrawGroupHidingAvailable();
    GameplayCheckbox("Hide Body Gear", groups, Features::HideBodyGearEnabled(), &Features::SetHideBodyGearEnabled,
                     "The bandolier, rucksack and satchel her upgrades add are not drawn (draw groups 46, 47, 48 of\n"
                     "her model).",
                     "The player tools are unavailable for this executable.");
    ImGui::SameLine();
    GameplayCheckbox("Hide Quiver Strap", groups, Features::HideQuiverStrapEnabled(), &Features::SetHideQuiverStrapEnabled,
                     "The quiver strap across her chest is not drawn (draw group 12 of her model).",
                     "The player tools are unavailable for this executable.");
}

// (v0.9.479) The gameplay modules' settings, out of the Player tab: swimming, air aim & slow motion (with the aim camera),
// acrobatics, the grapple kick, hand-to-hand and the enemies' behaviour.
// (v0.9.519) Each a collapsing header of its own (the tab was one long page), Graphics first.
void DrawEnhancementsTab() {
    struct Section {
        const char* title;
        void (*draw)();
    };
    static const Section kSections[] = {
        {"Graphics", &DrawGraphicsSection},
        {"Gameplay", &DrawGameplaySection},   // (v0.9.586)
        {"Swimming", &SwimBreath::DrawPlayerSection},
        {"Air aim & slow motion", &CombatAir::DrawPlayerSection},
        {"Acrobatics (experimental)", &Acro::DrawPlayerSection},
        {"Grapple kick (Black Widow, experimental)", &Grapple::DrawPlayerSection},
        {"Hand-to-hand (Black Widow, experimental)", &Brawl::DrawPlayerSection},
        {"Enemy behaviour (close combat, experimental)", &EnemyAI::DrawPlayerSection},
    };
    for (const Section& section : kSections) {
        if (!ImGui::CollapsingHeader(section.title)) continue;
        ImGui::PushID(section.title);
        ImGui::Indent(UiPx(6.0f));
        section.draw();
        ImGui::Unindent(UiPx(6.0f));
        ImGui::PopID();
        ImGui::Spacing();
    }
}

void DrawPlayerTab() {
    const bool playerTools = Features::PlayerToolsAvailable();
    Features::PlayerTransform live{};
    const bool hasPlayer = playerTools && Features::GetPlayerTransform(live);

    ImGui::TextUnformatted("Misc");
    ImGui::Separator();

    bool invuln = Features::Invulnerability();
    if (!Features::InvulnerabilityAvailable()) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Invulnerability", &invuln))
        Features::SetInvulnerability(invuln);
    if (!Features::InvulnerabilityAvailable()) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Uses ROTTR's native invincibility state. Damage/hit reactions may be suppressed.");

    bool infiniteHealth = Features::InfiniteHealth();
    if (!Features::InfiniteHealthAvailable()) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Infinite Health", &infiniteHealth))
        Features::SetInfiniteHealth(infiniteHealth);
    if (!Features::InfiniteHealthAvailable()) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Normal damage handling stays active so Lara can still be hit; health is restored immediately.");

    bool infiniteAmmo = Features::InfiniteAmmo();
    if (!Features::InfiniteAmmoAvailable()) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Infinite Ammo", &infiniteAmmo))
        Features::SetInfiniteAmmo(infiniteAmmo);
    if (!Features::InfiniteAmmoAvailable()) ImGui::EndDisabled();

    bool infiniteResources = Features::InfiniteResources();
    if (!Features::InfiniteResourcesAvailable()) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Infinite Resources", &infiniteResources))
        Features::SetInfiniteResources(infiniteResources);
    if (!Features::InfiniteResourcesAvailable()) ImGui::EndDisabled();

    bool moonJump = Features::MoonJump();
    if (!Features::MoonJumpAvailable()) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Moon jump", &moonJump))
        Features::SetMoonJump(moonJump);
    if (!Features::MoonJumpAvailable()) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Boost Lara upward when a jump starts, allowing much higher jumps.");

    bool oneHitKill = Features::OneHitKill();
    if (!Features::OneHitKillAvailable()) ImGui::BeginDisabled();
    if (ImGui::Checkbox("One Hit Kill", &oneHitKill))
        Features::SetOneHitKill(oneHitKill);
    if (!Features::OneHitKillAvailable()) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Makes positive damage to non-player health components lethal. Lara is excluded.");


    ImGui::Spacing();
    ImGui::TextUnformatted("Lara's gear (every outfit)");   // (v0.9.591)
    ImGui::Separator();
    DrawLarasGearSection();

    ImGui::Spacing();
    ImGui::TextUnformatted("Death");
    ImGui::Separator();

    const bool killOff = !Features::PlayerKillAvailable() || Features::CutscenePlaying();   // (v0.9.440)
    if (killOff) ImGui::BeginDisabled();
    if (ImGui::Button("Kill Lara")) {
        if (!Features::RequestKillPlayer())
            SetPlayerStatus("Native InstantKill is unavailable for this executable build.");
        else
            SetPlayerStatus("");
    }
    if (killOff) ImGui::EndDisabled();
    if (Features::CutscenePlaying() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Not during a cutscene (F11 too): the game has no death inside one.");
    // (v1.0.0) its "Ready" status beside it taken out (the Diagnostics window keeps the F11 kill state)

    float deathDelay = Features::DeathReloadDelay();
    if (!Features::DeathTimingAvailable()) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(UiPx(145.0f));
    if (ImGui::InputFloat("Deathfade delay", &deathDelay, 0.25f, 1.0f, "%.2f"))
        Features::SetDeathReloadDelay(deathDelay);
    if (ImGui::IsItemDeactivatedAfterEdit())
        Settings::Save();
    if (!Features::DeathTimingAvailable()) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Total time from entering ROTTR's death state until its normal reload.\n"
            "0 = use the game's native timing. Values are clamped to 0-60 seconds.");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("0 = game default");

    ImGui::Spacing();
    ImGui::TextUnformatted("Player Position");
    ImGui::Separator();

    if (!playerTools) {
        ImGui::TextDisabled("Player transform functions are unavailable for this executable build.");
    } else if (!hasPlayer) {
        ImGui::TextDisabled("Player instance is not currently available.");
    }

    
    
    
    if (hasPlayer) {
        if (!g_manualTargetInitialized) {
            g_manualTarget = live;
            g_manualTargetInitialized = true;
        } else {
            if (!g_manualPositionFieldActive)
                std::copy_n(live.position, 3, g_manualTarget.position);
            if (!g_manualRotationFieldActive)
                std::copy_n(live.rotation, 3, g_manualTarget.rotation);
        }
    } else {
        g_manualPositionFieldActive = false;
        g_manualRotationFieldActive = false;
    }

    if (!hasPlayer || !Features::TeleportAvailable()) ImGui::BeginDisabled();
    bool manualChanged = false;
    const float transformFieldWidth = std::min(UiPx(420.0f), ImGui::GetContentRegionAvail().x);
    ImGui::SetNextItemWidth(transformFieldWidth);
    manualChanged |= ImGui::DragFloat3("Position##manual", g_manualTarget.position,
                                       1.0f, 0.0f, 0.0f, "%.3f");
    const bool positionFieldActiveNow = ImGui::IsItemActive();
    ImGui::SetNextItemWidth(transformFieldWidth);
    manualChanged |= ImGui::DragFloat3("Rotation##manual", g_manualTarget.rotation,
                                       0.0025f, -3.14159265f, 3.14159265f, "%.5f",
                                       ImGuiSliderFlags_AlwaysClamp);
    const bool rotationFieldActiveNow = ImGui::IsItemActive();
    if (!hasPlayer || !Features::TeleportAvailable()) ImGui::EndDisabled();
    g_manualPositionFieldActive = hasPlayer && positionFieldActiveNow;
    g_manualRotationFieldActive = hasPlayer && rotationFieldActiveNow;

    if (manualChanged) {
        if (Features::QueuePlayerTransform(g_manualTarget))
            SetPlayerStatus("");
        else
            SetPlayerStatus("Live transform update was not queued (player unavailable or outfit switching).");
    }

    if (g_playerStatus[0]) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", g_playerStatus);
    }
}

std::string SkillDisplayName(const Features::SkillInfo& skill);

void RefreshSkillUiCache() {
    const std::uint64_t version = Features::SkillListVersion();
    if (version == g_skillCacheVersion) return;
    const int count = Features::SkillCount();
    g_skillCache.resize(static_cast<std::size_t>(std::max(0, count)));
    if (count > 0) {
        const int copied = Features::CopySkills(g_skillCache.data(), count);
        g_skillCache.resize(static_cast<std::size_t>(std::max(0, copied)));
    }
    g_skillDisplayCache.clear();
    g_skillDisplayCache.reserve(g_skillCache.size());
    for (const auto& skill : g_skillCache)
        g_skillDisplayCache.push_back(SkillDisplayName(skill));
    g_skillCacheVersion = version;
}

std::string SkillDisplayName(const Features::SkillInfo& skill) {
    std::string name = skill.name[0] ? skill.name : ("Skill " + std::to_string(skill.traitIndex));
    static constexpr const char* kPrefixes[] = {
        "PlayerTrait_", "playertrait_", "Trait_", "trait_", "Skill_", "skill_"
    };
    for (const char* prefix : kPrefixes) {
        const std::size_t length = std::strlen(prefix);
        if (name.size() >= length && name.compare(0, length, prefix) == 0) {
            name.erase(0, length);
            break;
        }
    }
    std::replace(name.begin(), name.end(), '_', ' ');
    if (name.empty())
        name = "Skill " + std::to_string(skill.traitIndex);
    return name;
}

void QueueSkillGroup(int category, bool enabled) {
    bool rejected = false;
    for (auto& skill : g_skillCache) {
        if (category >= 0 && skill.category != category) continue;
        if (skill.enabled == enabled) continue;
        if (Features::QueueSkillEnabled(skill.traitIndex, enabled)) {
            
            
            skill.enabled = enabled;
        } else {
            rejected = true;
        }
    }

    if (rejected)
        strncpy_s(g_skillUiStatus, "One or more skill updates were rejected; Lara or the skill table may have changed.", _TRUNCATE);
    else
        g_skillUiStatus[0] = '\0';
}

void DrawSkillCategory(const char* title, int category) {
    
    
    if (!ImGui::CollapsingHeader(title)) return;

    const bool any = std::any_of(g_skillCache.begin(), g_skillCache.end(),
                                 [category](const Features::SkillInfo& skill) {
                                     return skill.category == category;
                                 });

    ImGui::Indent();
    if (!any) {
        ImGui::TextDisabled("No skills currently available in this category.");
        ImGui::Unindent();
        return;
    }

    ImGui::PushID(category);
    if (ImGui::Button("Give all skills", UiSize(120.0f, 0.0f)))
        QueueSkillGroup(category, true);
    ImGui::SameLine();
    if (ImGui::Button("Remove all skills", UiSize(130.0f, 0.0f)))
        QueueSkillGroup(category, false);
    ImGui::Spacing();

    for (std::size_t skillIndex = 0; skillIndex < g_skillCache.size(); ++skillIndex) {
        auto& skill = g_skillCache[skillIndex];
        if (skill.category != category) continue;
        bool enabled = skill.enabled;
        const char* displayName = skillIndex < g_skillDisplayCache.size()
            ? g_skillDisplayCache[skillIndex].c_str() : skill.name;
        ImGui::PushID(static_cast<int>(skill.traitIndex));
        if (ImGui::Checkbox(displayName, &enabled)) {
            if (Features::QueueSkillEnabled(skill.traitIndex, enabled)) {
                skill.enabled = enabled;
                g_skillUiStatus[0] = '\0';
            } else {
                strncpy_s(g_skillUiStatus, "Skill update rejected; Lara or the skill table may have changed.", _TRUNCATE);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\nTrait index: %u", skill.name,
                              static_cast<unsigned int>(skill.traitIndex));
        }
        ImGui::PopID();
    }
    ImGui::PopID();
    ImGui::Unindent();
}

void DrawSkillsTab() {
    if (!Features::SkillControlsAvailable()) {
        ImGui::TextDisabled("Skill controls are unavailable for this executable build.");
        return;
    }

    RefreshSkillUiCache();
    const int points = Features::SkillPointCount();
    if (points >= 0)
        ImGui::Text("Skill points: %d", points);
    else
        ImGui::TextDisabled("Skill points: --");

    const bool hasPlayer = points >= 0;
    if (!hasPlayer) ImGui::BeginDisabled();
    if (ImGui::Button("Give Skill Point", UiSize(135.0f, 0.0f))) {
        if (!Features::RequestGiveSkillPoint())
            strncpy_s(g_skillUiStatus, "Could not queue a skill-point increase.", _TRUNCATE);
        else
            g_skillUiStatus[0] = '\0';
    }
    ImGui::SameLine();
    if (points <= 0) ImGui::BeginDisabled();
    if (ImGui::Button("Remove Skill Point", UiSize(145.0f, 0.0f))) {
        if (!Features::RequestRemoveSkillPoint())
            strncpy_s(g_skillUiStatus, "Could not queue a skill-point decrease.", _TRUNCATE);
        else
            g_skillUiStatus[0] = '\0';
    }
    if (points <= 0) ImGui::EndDisabled();
    if (!hasPlayer) ImGui::EndDisabled();

    const char* status = Features::SkillStatus();
    if (std::strcmp(status, "Ready") != 0)
        ImGui::TextDisabled("%s", status);
    if (g_skillUiStatus[0])
        ImGui::TextDisabled("%s", g_skillUiStatus);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (g_skillCache.empty()) {
        ImGui::TextDisabled("Skill list is not available yet.");
        return;
    }

    if (!hasPlayer) ImGui::BeginDisabled();
    if (ImGui::Button("Give all skills", UiSize(120.0f, 0.0f)))
        QueueSkillGroup(-1, true);
    ImGui::SameLine();
    if (ImGui::Button("Remove all skills", UiSize(130.0f, 0.0f)))
        QueueSkillGroup(-1, false);
    if (!hasPlayer) ImGui::EndDisabled();
    ImGui::Spacing();

    
    
    
    DrawSkillCategory("Brawler", 3);
    DrawSkillCategory("Hunter", 2);
    DrawSkillCategory("Survivor", 1);
}

// (v1.0.0) in the General tab (its own tab until then)
void DrawWeatherSection() {
    ImGui::TextUnformatted("Weather");
    ImGui::Separator();

    Features::WeatherState weather{};
    if (!Features::WeatherControlsAvailable() || !Features::GetWeatherState(weather)) {
        ImGui::Spacing();
        ImGui::TextWrapped(
            "ROTTRFramework uses ROTTR's verified WeatherManager singleton plus the authored GlobalWeather catalog. "
            "Precipitation and canonical time-of-day states are resolved by Rise's own list indices; no "
            "process-memory scan is performed.");
        return;
    }

    static const char* kTimeOfDayNames[] = {"Night", "Dawn", "Day", "Dusk"};
    int timeOfDay = weather.timeOfDay;
    const char* timeOfDayPreview =
        (timeOfDay >= 0 && timeOfDay < IM_ARRAYSIZE(kTimeOfDayNames))
            ? kTimeOfDayNames[timeOfDay]
            : (weather.timeOfDayName[0] ? weather.timeOfDayName : "<other authored state>");

    ImGui::SetNextItemWidth(UiPx(180.0f));
    if (ImGui::BeginCombo("Time of day", timeOfDayPreview)) {
        for (int i = 0; i < IM_ARRAYSIZE(kTimeOfDayNames); ++i) {
            const bool selected = timeOfDay == i;
            if (ImGui::Selectable(kTimeOfDayNames[i], selected)) {
                timeOfDay = i;
                Features::SetTimeOfDayOverride(i);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    // (v1.0.0) the "override" / "live" labels and the "Changing a control pins..." line taken out
    if (ImGui::IsItemHovered())   // (v0.9.522: no checkpoint needed since 0.9.521)
        ImGui::SetTooltip("Changes the time of day at once: the sky, the level's time-of-day events and each area's lighting "
                          "and effects layers, as a checkpoint reload would give them.");

    ImGui::Spacing();
    
    
    
    // (v1.0.0) no "Precipitation active" checkbox: the precipitation is on whenever the intensity is above 0
    float precipitationIntensity =
        weather.rainActive || weather.snowActive ? std::max(weather.rainIntensity, weather.snowIntensity) : 0.0f;
    ImGui::SetNextItemWidth(UiPx(260.0f));
    if (ImGui::SliderFloat("Precipitation intensity", &precipitationIntensity, 0.0f, 1.0f, "%.2f",
                           ImGuiSliderFlags_AlwaysClamp))
        Features::SetPrecipitationOverride(precipitationIntensity > 0.0f, precipitationIntensity);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Drives ROTTR's authored Rain and Snow slots together; the current area determines the visible "
                          "precipitation type. 0: none.");

    ImGui::Spacing();
    if (ImGui::Button("Use game weather"))
        Features::ClearWeatherOverrides();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Stops pinning precipitation and time-of-day values so ROTTR can drive them normally again.");
}

// (v0.9.598) Inventory > Equipment: the game's Equipment items (its Equipment crafting recipes: quivers, pouches,
// satchel, rucksack, bandolier, rebreather, ascender, lock pick, ...), given or taken away (features.h EquipmentInfo).
void DrawEquipmentTab() {
    ImGui::TextUnformatted("Equipment");
    ImGui::Separator();
    if (!Features::EquipmentControlsAvailable()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Equipment controls are unavailable for this executable build.");
        return;
    }
    static std::vector<Features::EquipmentInfo> cache;
    static std::uint64_t cachedVersion = ~std::uint64_t{0};
    const std::uint64_t version = Features::EquipmentListVersion();
    if (version != cachedVersion) {
        cache.resize(256);
        cache.resize(static_cast<std::size_t>(Features::CopyEquipment(cache.data(), 256)));
        cachedVersion = version;
    }
    int owned = 0;
    for (const auto& info : cache)
        if (info.owned) ++owned;
    const char* status = Features::EquipmentStatus();
    if (cache.empty()) {
        ImGui::TextDisabled("%s", std::strcmp(status, "Ready") == 0 ? "No Equipment items in the game's crafting data." : status);
        return;
    }
    ImGui::Text("%d/%d owned", owned, static_cast<int>(cache.size()));
    if (std::strcmp(status, "Ready") != 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", status);
    }
    if (ImGui::Button("Give all", UiSize(110.0f, 0.0f))) Features::QueueEquipment(-1, true);
    ImGui::SameLine();
    if (ImGui::Button("Remove all", UiSize(110.0f, 0.0f))) Features::QueueEquipment(-1, false);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Every Equipment item taken away, as if never crafted or found (the story's gear too:\nthe rebreather, the ascender, the lock pick, ...).");
    ImGui::Spacing();
    ImGui::TextDisabled("Checked: Lara has it. Hover an item for what it is.");
    // (v0.9.600) two lists: the gear found on the way (the campsite shows it as ACQUIRED) and what is crafted there
    auto drawList = [&](const char* id, bool found) {
        if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchSame)) return;
        for (const auto& info : cache) {
            if (info.found != found) continue;
            ImGui::TableNextColumn();
            ImGui::PushID(info.recipe);
            bool value = info.owned;
            if (ImGui::Checkbox(info.name, &value)) Features::QueueEquipment(info.recipe, value);
            if (ImGui::IsItemHovered()) {
                char item[128] = "";
                if (info.itemObject)
                    std::snprintf(item, sizeof(item), "\nIts item: object %u (%s)", info.itemObject,
                                  info.itemHeld ? "in her inventory" : "not in her inventory");
                char campsite[192] = "";
                if (!found && !info.known)
                    std::snprintf(campsite, sizeof(campsite),
                                  "\n\nThe campsite does not offer it yet: it appears there once Lara has\ntrait %u (%s).",
                                  static_cast<unsigned>(info.knownTrait), info.knownTraitName);
                ImGui::SetTooltip("%s%s\n\nTrait %u %s (%s)%s\nRecipe %d \"%s\"", info.description, campsite,
                                  static_cast<unsigned>(info.craftedTrait), info.traitName,
                                  info.transient ? "an ability on Lara" : "saved with her progress", item, info.recipe,
                                  info.debugName);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    };
    ImGui::SeparatorText("Found on the way");
    drawList("##equipment_found", true);
    ImGui::SeparatorText("Crafted at a campsite");
    drawList("##equipment_crafted", false);
}

void DrawWeaponsTab() {
    ImGui::TextUnformatted("Weapons");
    ImGui::Separator();
    {
        bool dual = Features::DualWieldExperimentalEnabled();
        const bool available = Features::DualWieldExperimentalAvailable();
        if (!available) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Dual wield pistols", &dual)) {
            Features::SetDualWieldExperimentalEnabled(dual);
            Settings::Get().dualWieldPistols = dual;  // remembered: on at the next start unless unchecked
            Settings::Save();
        }
        if (!available) ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Stage 37: the offhand pistol is created while the pistol is holstered, so draw it after enabling (holster once if it was already out). With two pistols in the Item the game's own dual-pistol animation branches run (draw, aim, fire, lowered idle/walk/run, holster); their unfinished parts are filled with the shipped Hosted_DualPistol clips, and each shot plays the one-arm recoil of the gun that fired. The offhand pistol holsters on Lara's left thigh and has its own muzzle FX.\nTest: draw, idle, walk, run, single shots (left/right alternate), then wait for the auto-holster.");
        // (v0.9.519) "Offhand fires", "Only the pistol that fires animates" and "Legacy: force two-handed combat flag"
        // gone (test options): the offhand fires, only the gun that fires animates, the legacy flag stays off.
        if (dual) ImGui::TextWrapped("%s", Features::DualWieldStatus());
        // (v0.9.607) "Lowered pistols: forward lean" removed: the lowered clips keep their shipped pose
        ImGui::Separator();
    }
    {
        // (v0.9.456) the silencer off and on again with the weapon's key (silencer.h)
        bool silencer = Silencer::Enabled();
        const bool available = Silencer::Available();
        if (!available) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Weapon key again: silencer off / on", &silencer)) {
            Silencer::SetEnabled(silencer);
            Settings::Get().silencerToggle = silencer;
            Settings::Save();
        }
        if (!available) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("As in Tomb Raider (2013): with a silenced weapon in hand, pressing its key again takes the silencer off; pressing again puts it back on and goes on to the next ammo type.\nA weapon with more ammo types goes: A silenced, A, B silenced, B, ...\nWeapons without a silencer (or before it is crafted) keep the game's ammo cycle.\n%s",
                              Silencer::Status());
        // (v0.9.598) the toggle without Lara's reload
        bool noReload = Silencer::NoReload();
        const bool noReloadAvailable = available && silencer && Silencer::NoReloadAvailable();
        ImGui::Indent();
        if (!noReloadAvailable) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Without the reload", &noReload)) {
            Silencer::SetNoReload(noReload);
            Settings::Get().silencerNoReload = noReload;
            Settings::Save();
        }
        if (!noReloadAvailable) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The silencer comes off and goes back on at once: Lara does not play the reload the game plays when\nthe ammo type changes. The clip stays as it is (the silenced and unsilenced ammo are the same).\nGoing on to another ammo type still reloads.%s",
                              Silencer::NoReloadAvailable() ? "" : "\nUnavailable: the game's code differs (see the log).");
        ImGui::Unindent();
        ImGui::Separator();
    }
    {
        // (v0.9.606) the weapons drawn as before their first upgrade (silencer.h)
        bool plain = Silencer::PlainLooks();
        const bool available = Silencer::PlainLooksAvailable();
        if (!available) ImGui::BeginDisabled();
        if (ImGui::Checkbox("No upgrade looks (weapons as they come)", &plain)) {
            Silencer::SetPlainLooks(plain);
            Settings::Get().noUpgradeLooks = plain;
            Settings::Save();
        }
        if (!available) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Every weapon keeps its default look, as before its first upgrade: the campsite's upgrades (grip wraps,\n"
                              "barrels, magazines, stocks, the axe's handles...) are not drawn on it, in her hands, on her back or in\n"
                              "the campsite's menus. The upgrades still work (damage, magazine size, ...).\n"
                              "Attachments keep their looks: the suppressors, the laser sight, the grenade launcher.%s",
                              available ? "" : "\nUnavailable: the game's code differs (see the log).");
        ImGui::Separator();
    }
    if (!Features::WeaponControlsAvailable()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Weapon inventory controls are unavailable for this executable build.");
        return;
    }

    static constexpr Features::WeaponClass kClasses[] = {
        Features::WeaponClass::Bow,
        Features::WeaponClass::Pistol,
        Features::WeaponClass::Smg,
        Features::WeaponClass::Shotgun,
    };
    static constexpr const char* kLabels[] = {"Bow", "Pistol", "SMG", "Shotgun"};

    // (v0.9.606: open; v0.9.607: closed) the four weapon lists closed whenever the page is shown (the menu opened, the
    // Inventory tab or its Weapons page chosen): ImGui would keep a list open for the rest of the session. Opening one
    // still works while the page stays up.
    static int s_lastDrawnFrame = -2;
    const int frame = ImGui::GetFrameCount();
    const bool pageAppearing = frame - s_lastDrawnFrame > 1;
    s_lastDrawnFrame = frame;

    for (int wi = 0; wi < 4; ++wi) {
        std::vector<Features::WeaponVariantInfo> variants;
        const unsigned int variantCount = Features::WeaponVariantCount(kClasses[wi]);
        variants.reserve(variantCount);
        unsigned int ownedCount = 0;
        for (unsigned int vi = 0; vi < variantCount; ++vi) {
            Features::WeaponVariantInfo info{};
            if (!Features::GetWeaponVariantInfo(kClasses[wi], vi, info)) continue;
            if (info.owned) ++ownedCount;
            variants.push_back(info);
        }

        char header[128]{};
        std::snprintf(header, sizeof(header), "%s  [%u/%u owned]###weapon_class_%d",
                      kLabels[wi], ownedCount, static_cast<unsigned int>(variants.size()), wi);
        if (pageAppearing) ImGui::SetNextItemOpen(false, ImGuiCond_Always);
        if (!ImGui::CollapsingHeader(header))
            continue;

        if (variants.empty()) {
            ImGui::TextDisabled("No authored variants resolved for this weapon class.");
            continue;
        }

        for (std::size_t vi = 0; vi < variants.size(); ++vi) {
            const auto& info = variants[vi];
            ImGui::PushID(static_cast<int>(info.objectZoneId));

            ImGui::TextUnformatted(info.displayName[0] ? info.displayName : info.internalName);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("ObjectZone %u\n%s",
                                  info.objectZoneId,
                                  info.internalName[0] ? info.internalName : "<unnamed>");
            }
            ImGui::SameLine();
            if (info.selected)
                ImGui::TextDisabled("[Equipped]");
            else if (info.owned)
                ImGui::TextDisabled("[Owned]");
            else
                ImGui::TextDisabled("[Locked]");

            const bool canEquip = info.owned && !info.selected && info.inventoryItemId != 0;
            if (!canEquip) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Equip"))
                Features::RequestEquipWeaponVariant(info.objectZoneId);
            if (!canEquip) ImGui::EndDisabled();

            ImGui::SameLine();
            const bool canUnlock = !info.owned && info.objectZoneId != 0;
            if (!canUnlock) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Unlock / add"))
                Features::RequestUnlockWeaponVariant(info.objectZoneId);
            if (!canUnlock) ImGui::EndDisabled();

            ImGui::SameLine();
            const bool canRemove = info.owned && info.inventoryItemId != 0;
            if (!canRemove) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Remove"))
                Features::RequestRemoveWeaponVariant(info.objectZoneId);
            if (!canRemove) ImGui::EndDisabled();

            if (vi + 1 != variants.size()) ImGui::Separator();
            ImGui::PopID();
        }
    }
}


const char* CardRarityLabel(Features::CardRarity rarity) {
    switch (rarity) {
        case Features::CardRarity::Common: return "Common";
        case Features::CardRarity::Uncommon: return "Uncommon";
        case Features::CardRarity::Rare: return "Rare";
        case Features::CardRarity::Heroic: return "Heroic";
        case Features::CardRarity::Legendary: return "Legendary";
    }
    return "Unknown";
}

void DrawCardsTab() {
    ImGui::TextUnformatted("Expedition Cards");
    ImGui::Separator();
    if (!Features::CardControlsAvailable()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Expedition card controls are unavailable for this executable build.");
        return;
    }

    static char search[128]{};
    static std::vector<Features::CardInfo> cache;
    static std::uint64_t cachedVersion = ~std::uint64_t{0};
    
    
    
    
    
    
    
    
    
    
    
    
    
    static std::vector<int> filtered;
    static unsigned int filteredActive = 0;
    static bool filterDirty = true;
    const std::uint64_t version = Features::CardSnapshotVersion();
    if (version != cachedVersion) {
        cache.clear();
        const unsigned int count = Features::CardCount();
        cache.reserve(count);
        for (unsigned int i = 0; i < count; ++i) {
            Features::CardInfo info{};
            if (Features::GetCardInfo(i, info)) cache.push_back(info);
        }
        cachedVersion = version;
        filterDirty = true;
    }

    ImGui::SetNextItemWidth(UiPx(300.0f));
    if (ImGui::InputTextWithHint("##card_search", "Search cards...", search, sizeof(search)))
        filterDirty = true;

    auto containsNoCaseNoAlloc = [](const char* text, const char* needle) {
        if (!needle || !*needle) return true;
        if (!text) return false;
        for (const char* start = text; *start; ++start) {
            const char* a = start;
            const char* b = needle;
            while (*a && *b &&
                   std::tolower(static_cast<unsigned char>(*a)) ==
                       std::tolower(static_cast<unsigned char>(*b))) {
                ++a;
                ++b;
            }
            if (!*b) return true;
        }
        return false;
    };
    auto passesSearch = [&](const Features::CardInfo& info) {
        if (!search[0]) return true;
        if (containsNoCaseNoAlloc(info.name, search)) return true;
        char hashText[16]{};
        std::snprintf(hashText, sizeof(hashText), "%08X", info.cardHash);
        return containsNoCaseNoAlloc(hashText, search);
    };

    if (filterDirty) {
        filtered.clear();
        filtered.reserve(cache.size());
        filteredActive = 0;
        for (std::size_t i = 0; i < cache.size(); ++i) {
            const auto& info = cache[i];
            if (!passesSearch(info)) continue;
            filtered.push_back(static_cast<int>(i));
            if (info.active) ++filteredActive;
        }
        filterDirty = false;
    }

    ImGui::SameLine();
    ImGui::TextDisabled("%u shown | %u active | %u total",
                        static_cast<unsigned int>(filtered.size()), filteredActive,
                        static_cast<unsigned int>(cache.size()));
    ImGui::Separator();

    if (cache.empty()) {
        ImGui::TextDisabled("Waiting for ROTTR's Expedition card database...");
        return;
    }

    if (ImGui::BeginChild("cards_list", ImVec2(0.0f, 0.0f), false,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(filtered.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto& info = cache[static_cast<std::size_t>(filtered[static_cast<std::size_t>(row)])];
                ImGui::PushID(static_cast<int>(info.cardHash));
                bool enabled = info.active;
                if (ImGui::Checkbox("##enabled", &enabled)) {
                    if (!Features::RequestSetCardEnabled(info.cardHash, enabled))
                        enabled = info.active;
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(info.name[0] ? info.name : "<unnamed card>");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "Card hash: 0x%08X\nCard type: 0x%08X\nRarity: %s\nModifier: %.3f\nXP modifier: %.3f\nConsumable: %s",
                        info.cardHash, info.cardTypeHash, CardRarityLabel(info.rarity),
                        info.modifier, info.xpModifier, info.consumable ? "yes" : "no");
                }
                ImGui::SameLine();
                ImGui::TextDisabled("[%s]  Mod %.2f  XP %.2f%s",
                                    CardRarityLabel(info.rarity), info.modifier, info.xpModifier,
                                    info.consumable ? "  Consumable" : "");
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
}

void DrawClothEditorContents() {
    if (!Features::ClothControlsAvailable()) {
        ImGui::TextDisabled("Live cloth editing is unavailable for this executable build.");
        return;
    }

    Features::ClothRuntimeInfo runtime{};
    const bool hasRuntime = Features::GetClothRuntimeInfo(runtime);
    const int groupCount = Features::ClothStripGroupCount();
    // (v1.0.0) the "Instance | Config | Active StripGroups | Total groups" line beside the tabs taken out (the tab bar
    // drew over it)
    ImGui::Separator();

    
    
    
    if (hasRuntime && runtime.isPlayer) {
        ImGui::TextUnformatted("Wet Cloth water switching");
        bool wetUnderwater = Features::ClothWetConfigUnderwaterEnabled();
        if (!Features::ClothWaterBehaviorAvailable()) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Use Wet Cloth config underwater", &wetUnderwater))
            Features::SetClothWetConfigUnderwaterEnabled(wetUnderwater);
        if (!Features::ClothWaterBehaviorAvailable()) ImGui::EndDisabled();
        Features::ClothWaterRuntimeInfo waterInfo{};
        if (Features::GetClothWaterRuntimeInfo(waterInfo)) {
            ImGui::TextDisabled("Cloth configs: current %d | default %d | wet %d%s",
                                waterInfo.currentConfigIndex, waterInfo.defaultConfigIndex,
                                waterInfo.wetConfigIndex,
                                waterInfo.wetConfigApplied ? " | framework Wet active" : "");
        }
        ImGui::Separator();
    }

    if (!hasRuntime || groupCount <= 0) {
        ImGui::TextDisabled("This Instance has no editable StripGroups in its active cloth configuration.");
        return;
    }

    if (g_selectedClothGroup < 0 || g_selectedClothGroup >= groupCount)
        g_selectedClothGroup = 0;

    const float listWidth = UiPx(225.0f);
    ImGui::BeginChild("cloth_stripgroup_list", UiSize(listWidth, UiPx(520.0f)), true);
    ImGui::TextUnformatted("StripGroups");
    ImGui::Separator();
    for (int i = 0; i < groupCount; ++i) {
        Features::ClothStripGroupInfo info{};
        if (!Features::GetClothStripGroupInfo(i, info)) continue;
        char label[96];
        std::snprintf(label, sizeof(label), "StripGroup %d  [group %u]", info.activeIndex, info.groupIndex);
        if (ImGui::Selectable(label, g_selectedClothGroup == i)) {
            g_selectedClothGroup = i;
            g_loadedClothConfig = -999;
            g_loadedClothGlobalGroup = 0xFFFFFFFFu;
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginGroup();
    Features::ClothStripGroupInfo selected{};
    if (!Features::GetClothStripGroupInfo(g_selectedClothGroup, selected)) {
        ImGui::TextDisabled("Selected StripGroup is no longer available.");
        ImGui::EndGroup();
        return;
    }

    const bool selectionChanged =
        g_loadedClothInstanceId != runtime.instanceId ||
        g_loadedClothConfig != runtime.currentConfigIndex ||
        g_loadedClothGlobalGroup != selected.groupIndex;
    if (selectionChanged) {
        g_clothEdit = selected.settings;
        g_loadedClothInstanceId = runtime.instanceId;
        g_loadedClothConfig = runtime.currentConfigIndex;
        g_loadedClothGlobalGroup = selected.groupIndex;
    }

    ImGui::Text("StripGroup %d", selected.activeIndex);
    ImGui::SameLine();
    ImGui::TextDisabled("global group %u | %u strip%s", selected.groupIndex, selected.stripCount,
                        selected.stripCount == 1 ? "" : "s");
    ImGui::Separator();

    const float fieldWidth = UiPx(330.0f);
    bool clothChanged = false;
    ImGui::SetNextItemWidth(fieldWidth); clothChanged |= ImGui::DragFloat("gravity", &g_clothEdit.gravity, 0.01f, 0.0f, 0.0f, "%.6g");
    ImGui::SetNextItemWidth(fieldWidth); clothChanged |= ImGui::DragFloat("drag", &g_clothEdit.drag, 0.001f, 0.0f, 0.0f, "%.6g");
    ImGui::SetNextItemWidth(fieldWidth); clothChanged |= ImGui::DragInt("maxVelocityUpdateIterations", &g_clothEdit.maxVelocityUpdateIterations, 0.10f, 0, 0, "%d");
    ImGui::SetNextItemWidth(fieldWidth); clothChanged |= ImGui::DragInt("maxPositionUpdateIterations", &g_clothEdit.maxPositionUpdateIterations, 0.10f, 0, 0, "%d");
    ImGui::SetNextItemWidth(fieldWidth); clothChanged |= ImGui::DragInt("relaxationIterations", &g_clothEdit.relaxationIterations, 0.10f, 0, 0, "%d");
    ImGui::SetNextItemWidth(fieldWidth); clothChanged |= ImGui::DragInt("subStepCount", &g_clothEdit.subStepCount, 0.10f, 0, 0, "%d");
    ImGui::SetNextItemWidth(fieldWidth); clothChanged |= ImGui::DragFloat("windFactor", &g_clothEdit.windFactor, 0.001f, 0.0f, 0.0f, "%.6g");
    clothChanged |= ImGui::Checkbox("windOnConstraints", &g_clothEdit.windOnConstraints);
    ImGui::SetNextItemWidth(fieldWidth); clothChanged |= ImGui::DragFloat("maxMassBounceBackStrength", &g_clothEdit.maxMassBounceBackStrength, 0.001f, 0.0f, 0.0f, "%.6g");
    ImGui::SetNextItemWidth(fieldWidth); clothChanged |= ImGui::DragInt("transformType", &g_clothEdit.transformType, 0.05f, 0, 0, "%d");
    ImGui::SetNextItemWidth(fieldWidth); clothChanged |= ImGui::DragInt("springStretchinessDefaultPercentage", &g_clothEdit.springStretchinessDefaultPercentage, 0.10f, 0, 0, "%d");
    ImGui::SetNextItemWidth(fieldWidth); clothChanged |= ImGui::DragInt("springStretchinessLowerPercentage", &g_clothEdit.springStretchinessLowerPercentage, 0.10f, 0, 0, "%d");
    ImGui::SetNextItemWidth(fieldWidth); clothChanged |= ImGui::DragInt("rigidityPercentage", &g_clothEdit.rigidityPercentage, 0.10f, 0, 0, "%d");
    ImGui::SetNextItemWidth(fieldWidth); clothChanged |= ImGui::DragFloat("accelerationDivider", &g_clothEdit.accelerationDivider, 0.001f, 0.0f, 0.0f, "%.6g");
    ImGui::SetNextItemWidth(fieldWidth); clothChanged |= ImGui::DragFloat("timeDeltaScale", &g_clothEdit.timeDeltaScale, 0.001f, 0.0f, 0.0f, "%.6g");
    clothChanged |= ImGui::Checkbox("Collision", &g_clothEdit.collisionEnabled);
    clothChanged |= ImGui::Checkbox("collideWithDynamicHair", &g_clothEdit.collideWithDynamicHair);

    if (clothChanged) {
        if (Features::QueueClothStripGroupUpdate(g_selectedClothGroup, g_clothEdit))
            g_clothUiStatus[0] = '\0';
        else
            strncpy_s(g_clothUiStatus,
                      "Live cloth update rejected: the selected Instance/config changed or a non-finite float was entered.",
                      _TRUNCATE);
    }

    ImGui::Spacing();
    if (!selected.hasOriginalSettings) ImGui::BeginDisabled();
    if (ImGui::Button("Restore original")) {
        Features::ClothStripGroupSettings restored{};
        if (Features::RestoreClothStripGroupOriginal(g_selectedClothGroup, &restored)) {
            g_clothEdit = restored;
            g_clothUiStatus[0] = '\0';
        } else {
            strncpy_s(g_clothUiStatus, "Could not restore the original StripGroup values.", _TRUNCATE);
        }
    }
    if (!selected.hasOriginalSettings) ImGui::EndDisabled();
    if (g_clothUiStatus[0]) ImGui::TextDisabled("%s", g_clothUiStatus);
    ImGui::EndGroup();
}

void DrawCapsuleCollisionEditorContents() {
    if (!Features::CapsuleCollisionControlsAvailable()) {
        Features::SetCapsuleCollisionInteractiveEditActive(false);
        Features::SetCapsuleCollisionDrawSelection(0, 0, false);
        ImGui::TextDisabled("GENERICCAPSULESHAPELIST editing is unavailable for this executable build.");
        return;
    }

    const auto version = Features::CapsuleCollisionListVersion();
    if (version != g_capsuleCollisionCacheVersion) {
        const bool hadPreviousSelection =
            g_selectedCapsuleCollision >= 0 &&
            g_selectedCapsuleCollision < static_cast<int>(g_capsuleCollisionCache.size());
        const unsigned int previousListIndex = hadPreviousSelection
            ? g_capsuleCollisionCache[static_cast<std::size_t>(g_selectedCapsuleCollision)].listIndex
            : ~0u;
        const std::uint32_t previousItemId = hadPreviousSelection
            ? g_capsuleCollisionCache[static_cast<std::size_t>(g_selectedCapsuleCollision)].itemId
            : 0u;

        const int count = Features::CapsuleCollisionCount();
        g_capsuleCollisionCache.resize(static_cast<std::size_t>(std::max(0, count)));
        const int copied = count > 0
            ? Features::CopyCapsuleCollisions(g_capsuleCollisionCache.data(), count)
            : 0;
        g_capsuleCollisionCache.resize(static_cast<std::size_t>(std::max(0, copied)));
        g_capsuleCollisionCacheVersion = version;

        
        
        
        int restoredSelection = -1;
        if (hadPreviousSelection) {
            for (int i = 0; i < copied; ++i) {
                const auto& candidate = g_capsuleCollisionCache[static_cast<std::size_t>(i)];
                if (candidate.listIndex == previousListIndex && candidate.itemId == previousItemId) {
                    restoredSelection = i;
                    break;
                }
            }
        }
        if (restoredSelection >= 0)
            g_selectedCapsuleCollision = restoredSelection;
        else if (g_selectedCapsuleCollision >= copied)
            g_selectedCapsuleCollision = std::max(0, copied - 1);

        const bool hasNewSelection =
            g_selectedCapsuleCollision >= 0 && g_selectedCapsuleCollision < copied;
        const unsigned int newListIndex = hasNewSelection
            ? g_capsuleCollisionCache[static_cast<std::size_t>(g_selectedCapsuleCollision)].listIndex
            : ~0u;
        const std::uint32_t newItemId = hasNewSelection
            ? g_capsuleCollisionCache[static_cast<std::size_t>(g_selectedCapsuleCollision)].itemId
            : 0u;
        if (!g_capsuleCollisionEditLoaded ||
            previousListIndex != newListIndex || previousItemId != newItemId)
            g_capsuleCollisionEditLoaded = false;
    }
    ImGui::Checkbox("Highlight selected capsule", &g_drawSelectedCapsuleCollision);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Draw the live world-space capsule used by ClothState. The overlay follows animated bones and stays visible through the model for editing.");
    if (g_drawSelectedCapsuleCollision) {
        const int visualCopies = Features::CapsuleCollisionDrawCount();
        if (visualCopies > 0) {
            ImGui::TextDisabled("Visualizer: %d live ClothState capsule%s", visualCopies, visualCopies == 1 ? "" : "s");
        } else {
            const int keysScanned = Features::CapsuleCollisionDrawKeysScanned();
            const int keyMatches = Features::CapsuleCollisionDrawKeyMatches();
            const int runtimeCandidates = Features::CapsuleCollisionDrawRuntimeCandidates();
            const int runtimeCapsules = Features::CapsuleCollisionDrawRuntimeCapsules();
            if (keysScanned == 0)
                ImGui::TextDisabled("Visualizer: waiting for active Cloth collision keys...");
            else if (keyMatches == 0)
                ImGui::TextDisabled("Visualizer: 0/%d ClothTune keys match this capsule", keysScanned);
            else if (runtimeCandidates == 0)
                ImGui::TextDisabled("Visualizer: %d ClothTune key match%s, but no live slot at that index",
                                    keyMatches, keyMatches == 1 ? "" : "es");
            else if (runtimeCapsules == 0)
                ImGui::TextDisabled("Visualizer: %d live candidate slot%s found, but type is not capsule",
                                    runtimeCandidates, runtimeCandidates == 1 ? "" : "s");
            else
                ImGui::TextDisabled("Visualizer: resolving %d live capsule slot%s...",
                                    runtimeCapsules, runtimeCapsules == 1 ? "" : "s");
        }
    }
    ImGui::Separator();
    if (g_capsuleCollisionCache.empty()) {
        Features::SetCapsuleCollisionInteractiveEditActive(false);
        Features::SetCapsuleCollisionDrawSelection(0, 0, false);
        ImGui::TextDisabled("No editable GENERICCAPSULESHAPELIST capsules were found on this Instance.");
        return;
    }

    ImGui::BeginChild("instance_capsule_list", UiSize(UiPx(240.0f), UiPx(520.0f)), true);
    for (int i = 0; i < static_cast<int>(g_capsuleCollisionCache.size()); ++i) {
        const auto& item = g_capsuleCollisionCache[static_cast<std::size_t>(i)];
        char label[150]{};
        std::snprintf(label, sizeof(label), "%s  [id %u]##capsule_%u",
                      item.name[0] ? item.name : "Capsule", item.itemId, item.listIndex);
        if (ImGui::Selectable(label, g_selectedCapsuleCollision == i)) {
            g_selectedCapsuleCollision = i;
            g_capsuleCollisionEditLoaded = false;
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    if (g_selectedCapsuleCollision < 0 ||
        g_selectedCapsuleCollision >= static_cast<int>(g_capsuleCollisionCache.size())) {
        Features::SetCapsuleCollisionInteractiveEditActive(false);
        Features::SetCapsuleCollisionDrawSelection(0, 0, false);
        return;
    }
    const auto& selected = g_capsuleCollisionCache[static_cast<std::size_t>(g_selectedCapsuleCollision)];
    Features::SetCapsuleCollisionDrawSelection(
        selected.listIndex, selected.itemId, g_drawSelectedCapsuleCollision);
    if (!g_capsuleCollisionEditLoaded ||
        g_capsuleCollisionEdit.itemId != selected.itemId ||
        g_capsuleCollisionEdit.listIndex != selected.listIndex) {
        g_capsuleCollisionEdit = selected;
        g_capsuleCollisionEditLoaded = true;
    }

    ImGui::BeginGroup();
    ImGui::Text("%s", selected.name[0] ? selected.name : "Capsule");
    ImGui::TextDisabled("scene item %u | list slot %u", selected.itemId, selected.listIndex);
    ImGui::Separator();

    bool changed = false;
    bool interactiveEditActive = false;
    const float width = std::min(UiPx(430.0f), ImGui::GetContentRegionAvail().x);
    ImGui::SetNextItemWidth(width);
    changed |= ImGui::DragFloat("Radius", &g_capsuleCollisionEdit.radius, 0.01f, 0.0f, 0.0f, "%.6g");
    interactiveEditActive |= ImGui::IsItemActive();
    ImGui::SetNextItemWidth(width);
    changed |= ImGui::DragFloat("Length", &g_capsuleCollisionEdit.length, 0.01f, 0.0f, 0.0f, "%.6g");
    interactiveEditActive |= ImGui::IsItemActive();
    ImGui::SetNextItemWidth(width);
    changed |= ImGui::DragInt("Segment", &g_capsuleCollisionEdit.segment, 0.10f, 0, 255, "%d",
                              ImGuiSliderFlags_AlwaysClamp);
    interactiveEditActive |= ImGui::IsItemActive();

    ImGui::Spacing();
    ImGui::TextUnformatted("Scene-item transform");
    for (int row = 0; row < 4; ++row) {
        char label[24]{};
        std::snprintf(label, sizeof(label), "Row %d##capsule_matrix", row);
        ImGui::SetNextItemWidth(width);
        changed |= ImGui::DragFloat4(label, &g_capsuleCollisionEdit.matrix[row * 4], 0.0025f,
                                     0.0f, 0.0f, "%.6g");
        interactiveEditActive |= ImGui::IsItemActive();
    }

    
    
    
    Features::SetCapsuleCollisionInteractiveEditActive(interactiveEditActive);

    if (changed) {
        if (Features::QueueCapsuleCollisionUpdate(g_capsuleCollisionEdit.listIndex,
                                                  g_capsuleCollisionEdit.itemId,
                                                  g_capsuleCollisionEdit.matrix,
                                                  g_capsuleCollisionEdit.radius,
                                                  g_capsuleCollisionEdit.length,
                                                  g_capsuleCollisionEdit.segment)) {
            g_capsuleCollisionUiStatus[0] = '\0';
        } else {
            strncpy_s(g_capsuleCollisionUiStatus,
                      "Collision update rejected: the selected Instance/capsule changed or a non-finite value was entered.",
                      _TRUNCATE);
        }
    }

    if (ImGui::Button("Restore original")) {
        std::memcpy(g_capsuleCollisionEdit.matrix, selected.originalMatrix, sizeof(selected.originalMatrix));
        g_capsuleCollisionEdit.radius = selected.originalRadius;
        g_capsuleCollisionEdit.length = selected.originalLength;
        g_capsuleCollisionEdit.segment = selected.originalSegment;
        if (!Features::QueueCapsuleCollisionUpdate(g_capsuleCollisionEdit.listIndex,
                                                   g_capsuleCollisionEdit.itemId,
                                                   g_capsuleCollisionEdit.matrix,
                                                   g_capsuleCollisionEdit.radius,
                                                   g_capsuleCollisionEdit.length,
                                                   g_capsuleCollisionEdit.segment)) {
            strncpy_s(g_capsuleCollisionUiStatus, "Could not restore the original capsule values.", _TRUNCATE);
        } else {
            g_capsuleCollisionUiStatus[0] = '\0';
        }
    }
    if (g_capsuleCollisionUiStatus[0]) ImGui::TextDisabled("%s", g_capsuleCollisionUiStatus);
    ImGui::EndGroup();
}


// ---- PureHair editor ---------------------------------------------------------------------------------------------
// Every edit applies as it is made: shading and motion values are read by the game every frame (the edits write the
// profiles it reads), the advanced values once the slider is let go (with a simulation reset), and strand data
// (thickness, curves, colors), which the game bakes into the hair when it creates it, rebuilds the hair a moment
// after the last edit.
struct HairEditState {
    bool changed = false;    // a value changed this frame
    bool released = false;   // a value's edit ended this frame (the slider let go, the text entered)
    void Track(bool edited) {
        changed |= edited;
        released |= ImGui::IsItemDeactivatedAfterEdit();
    }
};

std::unordered_map<std::uint64_t, Features::HairConstantGroupInfo> g_hairConstantOriginals;   // (object, group)
std::unordered_map<std::uint64_t, Features::HairRenderingProfileInfo> g_hairDirtOriginals;    // (object, profile)
std::unordered_map<std::uint16_t, Features::HairAdvancedSettings> g_hairAdvancedOriginals;     // object
double g_hairLastResync = -10.0;

std::uint64_t HairKey(std::uint16_t object, unsigned int a, unsigned int b = 0) {
    return (static_cast<std::uint64_t>(object) << 48) ^ (static_cast<std::uint64_t>(a) << 16) ^ b;
}

void HairTip(const char* text) {
    if (!text || !ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

float HairFieldWidth() { return std::min(UiPx(300.0f), ImGui::GetContentRegionAvail().x * 0.55f); }

// A value field: drag it (no limits: the usual range only sets the drag speed) or Ctrl+click / double-click to type
// one. Values are kept as they are (no rounding to the shown digits), so the game's own values are never changed by
// looking at them.
float HairDragSpeed(float v, float lo, float hi) {
    const float span = std::isfinite(hi - lo) && hi > lo ? hi - lo : 1.0f;
    const float magnitude = std::isfinite(v) ? std::fabs(v) : 0.0f;
    return magnitude > 0.0f ? std::max(magnitude / 100.0f, span / 20000.0f) : span / 1000.0f;
}

bool HairSlider(const char* label, float* v, float lo, float hi, const char* tip, HairEditState& st,
                const char* format = "%.6g") {
    (void)format;
    ImGui::SetNextItemWidth(HairFieldWidth());
    const bool edited = ImGui::DragFloat(label, v, HairDragSpeed(*v, lo, hi), 0.0f, 0.0f, "%.6g",
                                         ImGuiSliderFlags_NoRoundToFormat);
    st.Track(edited);
    HairTip(tip);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && !tip) ImGui::SetTooltip("%.9g", *v);
    return edited;
}

bool HairColor(const char* label, float rgb[3], const char* tip, HairEditState& st) {
    const bool edited = ImGui::ColorEdit3(label, rgb, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
    st.Track(edited);
    HairTip(tip);
    return edited;
}

// A curve: one vertical slider a sample, root (left) to tip (right). Its range starts at the usual one widened to the
// game's values (with room above) and only grows: pulling a sample to the top widens it, and "Numbers" types them.
bool HairCurve(const char* label, float* values, int count, float lo, float hi, const char* tip, HairEditState& st) {
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    HairTip(tip);
    struct CurveRange {
        float lo, hi;
        bool grow;   // a sample was pulled to the top: widen when the drag ends
    };
    static std::unordered_map<ImGuiID, CurveRange> ranges;
    const ImGuiID id = ImGui::GetID("range");
    float vmin = lo, vmax = hi;
    for (int i = 0; i < count; ++i)
        if (std::isfinite(values[i])) {
            vmin = std::min(vmin, values[i]);
            vmax = std::max(vmax, values[i]);
        }
    auto found = ranges.find(id);
    if (found == ranges.end())
        found = ranges.emplace(id, CurveRange{std::min(lo, vmin), std::max(hi, vmax > hi ? vmax * 1.5f : hi), false}).first;
    auto& range = found->second;
    if (range.grow && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        range.hi += (range.hi - range.lo) * 0.5f;
        range.grow = false;
    }
    range.lo = std::min(range.lo, vmin);
    range.hi = std::max(range.hi, vmax);
    const float l = range.lo, h = range.hi;
    const float gap = UiPx(3.0f);
    const float avail = std::min(ImGui::GetContentRegionAvail().x, UiPx(count > 10 ? 560.0f : 320.0f));
    const float w = std::clamp((avail - gap * static_cast<float>(count - 1)) / static_cast<float>(count), UiPx(10.0f), UiPx(34.0f));
    bool edited = false, atTop = false;
    for (int i = 0; i < count; ++i) {
        if (i) ImGui::SameLine(0.0f, gap);
        ImGui::PushID(i);
        const bool e = ImGui::VSliderFloat("##sample", ImVec2(w, UiPx(84.0f)), &values[i], l, h, "",
                                           ImGuiSliderFlags_NoRoundToFormat);
        st.Track(e);
        edited |= e;
        if (e && values[i] >= h - (h - l) * 0.01f) atTop = true;
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetTooltip("Sample %d of %d (%s): %.6g", i + 1, count, i == 0 ? "root" : i == count - 1 ? "tip" : "along the strand", values[i]);
        ImGui::PopID();
    }
    if (atTop) range.grow = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Numbers")) ImGui::OpenPopup("numbers");
    HairTip("Type the samples' values (no limits).");
    if (ImGui::BeginPopup("numbers")) {
        for (int i = 0; i < count; ++i) {
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(UiPx(110.0f));
            char name[32];
            std::snprintf(name, sizeof(name), "%d%s", i + 1, i == 0 ? " (root)" : i == count - 1 ? " (tip)" : "");
            const bool e = ImGui::DragFloat(name, &values[i], HairDragSpeed(values[i], lo, hi), 0.0f, 0.0f, "%.6g",
                                            ImGuiSliderFlags_NoRoundToFormat);
            st.Track(e);
            edited |= e;
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return edited;
}

const char* HairGroupLabel(char* out, std::size_t size, int index, unsigned int hash) {
    std::snprintf(out, size, "Group %d##g%d", index + 1, index);
    (void)hash;
    return out;
}

// A group picker (hair models are split into groups, e.g. bangs / ponytail / main hair).
bool HairGroupCombo(const char* label, int* selected, int count, unsigned int (*hashOf)(int)) {
    char preview[64];
    std::snprintf(preview, sizeof(preview), "Group %d of %d  (0x%08X)", *selected + 1, count, hashOf(*selected));
    ImGui::SetNextItemWidth(HairFieldWidth());
    bool changed = false;
    if (ImGui::BeginCombo(label, preview)) {
        for (int i = 0; i < count; ++i) {
            char item[64];
            std::snprintf(item, sizeof(item), "Group %d  (0x%08X)##hg%d", i + 1, hashOf(i), i);
            if (ImGui::Selectable(item, i == *selected)) {
                *selected = i;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    HairTip("The hair model is made of groups (e.g. the main hair, the bangs, a braid); each has its own values. "
            "The number after it is the group's name hash in the game's data.");
    return changed;
}

unsigned int HairPhysicsHash(int i) {
    Features::HairPhysicsGroupInfo info{};
    return Features::GetHairPhysicsGroupInfo(i, info) ? info.groupNameHash : 0u;
}
unsigned int HairRenderHash(int i) {
    Features::HairRenderGroupInfo info{};
    return Features::GetHairRenderGroupInfo(i, info) ? info.groupNameHash : 0u;
}
unsigned int HairConstantHash(int i) {
    Features::HairConstantGroupInfo info{};
    return Features::GetHairConstantGroupInfo(i, info) ? info.groupNameHash : 0u;
}

void HairSectionTitle(const char* text) {
    ImGui::Spacing();
    ImGui::SeparatorText(text);
}

void DrawHairProfileCombo(bool simulation, const Features::HairRuntimeInfo& rt) {
    const int count = simulation ? Features::HairSimulationProfileCount() : Features::HairRenderingProfileCount();
    const unsigned int current = simulation ? rt.currentSimulationProfile : rt.currentRenderingProfile;
    char preview[128] = "<unavailable>";
    for (int i = 0; i < count; ++i) {
        if (static_cast<unsigned int>(i) != current) continue;
        if (simulation) {
            Features::HairProfileInfo info{};
            if (Features::GetHairSimulationProfileInfo(i, info)) std::snprintf(preview, sizeof(preview), "%s", info.name);
        } else {
            Features::HairRenderingProfileInfo info{};
            if (Features::GetHairRenderingProfileInfo(i, info)) std::snprintf(preview, sizeof(preview), "%s", info.name);
        }
    }
    ImGui::SetNextItemWidth(HairFieldWidth());
    if (ImGui::BeginCombo(simulation ? "Motion profile" : "Look profile", preview)) {
        for (int i = 0; i < count; ++i) {
            char name[96]{};
            bool isDefault = false;
            if (simulation) {
                Features::HairProfileInfo info{};
                if (!Features::GetHairSimulationProfileInfo(i, info)) continue;
                std::snprintf(name, sizeof(name), "%s", info.name);
                isDefault = info.defaultSetting;
            } else {
                Features::HairRenderingProfileInfo info{};
                if (!Features::GetHairRenderingProfileInfo(i, info)) continue;
                std::snprintf(name, sizeof(name), "%s", info.name);
                isDefault = info.defaultSetting;
            }
            char label[160];
            std::snprintf(label, sizeof(label), "%s%s##hp%d", name, isDefault ? "  (default)" : "", i);
            const bool selected = static_cast<unsigned int>(i) == current;
            if (ImGui::Selectable(label, selected)) {
                // (v0.9.586) a locked motion profile moves to the one picked
                if (simulation && !Features::HairLockedMotionProfile().empty()) {
                    Features::SetHairLockedMotionProfile(name);
                    Settings::Save();
                }
                const bool ok = simulation ? Features::QueueHairSimulationProfile(static_cast<unsigned int>(i), g_hairProfileTransition)
                                           : Features::QueueHairRenderingProfile(static_cast<unsigned int>(i), g_hairProfileTransition);
                if (!ok) strncpy_s(g_hairUiStatus, "The profile change was not accepted (the hair changed).", _TRUNCATE);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    HairTip(simulation ? "The game switches between motion profiles by situation (e.g. walking, swimming, cutscenes). "
                         "Pick one to switch to it now; the game may switch again later. The values below are the "
                         "active profile's."
                       : "The game switches between look profiles by situation (e.g. dry, wet, snow). Pick one to "
                         "switch to it now; the game may switch again later. The values below are the active "
                         "profile's.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(UiPx(90.0f));
    ImGui::PushID(simulation ? "simblend" : "renderblend");
    ImGui::DragFloat("Blend (s)", &g_hairProfileTransition, 0.01f, 0.0f, 60.0f, "%.2f");
    ImGui::PopID();
    HairTip("How long the switch to a picked profile blends (0: at once).");
    // (v0.9.586) the motion profile locked: the game's own switches answered with it
    if (simulation && Features::HairProfileLockAvailable()) {
        const std::string locked = Features::HairLockedMotionProfile();
        bool lock = !locked.empty();
        const bool canLock = lock || std::strcmp(preview, "<unavailable>") != 0;
        if (!canLock) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Always use this profile##hairlock", &lock)) {
            Features::SetHairLockedMotionProfile(lock ? preview : "");
            Settings::Save();
        }
        if (!canLock) ImGui::EndDisabled();
        HairTip("Keeps Lara's hair on this motion profile whatever she does (running, swimming, cutscenes...): every "
                "switch the game makes is answered with this one, and a new hair (another outfit, a load) is put on it "
                "too. Picking another profile above moves the lock to it. Kept for the next game (by its name).");
        if (!locked.empty()) {
            if (Features::HairLockedProfileMissing()) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Locked to \"%s\", which this hair does not have: the game chooses.",
                                   locked.c_str());
            } else {
                Features::HairProfileInfo asked{};
                const int askedIndex = Features::HairLockLastGameRequest();
                if (askedIndex >= 0 && Features::GetHairSimulationProfileInfo(askedIndex, asked))
                    ImGui::TextDisabled("Locked to \"%s\" (the game last asked for \"%s\")", locked.c_str(), asked.name);
                else
                    ImGui::TextDisabled("Locked to \"%s\"", locked.c_str());
            }
        }
    }
}

void DrawHairLookTab(const Features::HairRuntimeInfo& rt) {
    DrawHairProfileCombo(false, rt);
    const bool stale = rt.renderMappingProfile != rt.currentRenderingProfile;

    // the active look profile's dirt
    Features::HairRenderingProfileInfo profile{};
    if (Features::GetHairRenderingProfileInfo(static_cast<int>(rt.currentRenderingProfile), profile)) {
        if (g_loadedHairRenderProfile != rt.currentRenderingProfile) {
            g_hairRenderingEdit = profile;
            g_loadedHairRenderProfile = rt.currentRenderingProfile;
        }
        const std::uint64_t key = HairKey(rt.playerObjectId, rt.currentRenderingProfile);
        if (!g_hairDirtOriginals.count(key)) g_hairDirtOriginals[key] = profile;
        HairSectionTitle("Dirt");
        HairEditState st;
        HairColor("Dirt color", g_hairRenderingEdit.dirtColor, "The color dirt tints the hair with.", st);
        HairSlider("Dirt amount", &g_hairRenderingEdit.dirtStrength, 0.0f, 1.0f,
                   "How strongly the dirt color shows (0: clean hair).", st);
        if (st.changed &&
            !Features::QueueHairRenderingProfileUpdate(static_cast<int>(rt.currentRenderingProfile), g_hairRenderingEdit))
            strncpy_s(g_hairUiStatus, "The dirt change was not accepted (the hair changed).", _TRUNCATE);
        if (ImGui::SmallButton("Restore dirt")) {
            const auto& original = g_hairDirtOriginals[key];
            std::copy(std::begin(original.dirtColor), std::end(original.dirtColor), std::begin(g_hairRenderingEdit.dirtColor));
            g_hairRenderingEdit.dirtStrength = original.dirtStrength;
            Features::QueueHairRenderingProfileUpdate(static_cast<int>(rt.currentRenderingProfile), g_hairRenderingEdit);
        }
        HairTip("Back to the values the profile had when the editor first saw it.");
    }

    HairSectionTitle("Shading");
    const int count = Features::HairRenderGroupCount();
    if (stale) {
        ImGui::TextDisabled("Reading the new look profile...");
        return;
    }
    if (count <= 0) {
        ImGui::TextDisabled("The active look profile has no hair groups to shade.");
        return;
    }
    g_selectedHairRenderGroup = std::clamp(g_selectedHairRenderGroup, 0, count - 1);
    if (HairGroupCombo("Group##render", &g_selectedHairRenderGroup, count, &HairRenderHash)) g_loadedHairRenderGroup = -1;
    Features::HairRenderGroupInfo group{};
    if (!Features::GetHairRenderGroupInfo(g_selectedHairRenderGroup, group)) return;
    if (g_loadedHairRenderGroup != g_selectedHairRenderGroup || g_loadedHairRenderGroupProfile != rt.renderMappingProfile) {
        g_hairRenderGroupEdit = group.settings;
        g_loadedHairRenderGroup = g_selectedHairRenderGroup;
        g_loadedHairRenderGroupProfile = rt.renderMappingProfile;
    }
    auto& e = g_hairRenderGroupEdit;
    HairEditState st;
    HairSlider("Ambient", &e.ambientStrength, 0.0f, 2.0f, "How much of the surrounding (ambient) light the hair picks up.", st);
    HairSlider("Diffuse", &e.diffuseStrength, 0.0f, 2.0f, "How much direct light brightens the hair.", st);
    HairSlider("Color shift", &e.colorShift, -1.0f, 1.0f, "Shifts the hair's color tone.", st);
    ImGui::Spacing();
    HairSlider("Primary shine", &e.specStrength1, 0.0f, 2.0f, "The strength of the main (white) highlight band.", st);
    HairSlider("Primary sharpness", &e.specExponent1, 1.0f, 200.0f,
               "How tight the main highlight is: higher is a thinner, sharper band.", st, "%.1f");
    HairSlider("Secondary shine", &e.specStrength2, 0.0f, 2.0f,
               "The strength of the second (tinted) highlight band, seen further along the strands.", st);
    HairSlider("Secondary sharpness", &e.specExponent2, 1.0f, 200.0f,
               "How tight the second highlight is.", st, "%.1f");
    HairSlider("Highlight shift", &e.highlightShift, -1.0f, 1.0f,
               "Moves the highlight bands toward the roots or the tips.", st);
    ImGui::Spacing();
    HairColor("Color override", e.colorOverride, "A color that replaces the hair's own, mixed in by the amount below.", st);
    HairSlider("Override amount", &e.colorOverrideStrength, 0.0f, 1.0f, "0: the hair's own colors; 1: the override color.", st);
    if (st.changed && !Features::QueueHairRenderGroupUpdate(g_selectedHairRenderGroup, e))
        strncpy_s(g_hairUiStatus, "The shading change was not accepted (the hair changed).", _TRUNCATE);
    if (group.hasOriginalSettings) {
        if (ImGui::SmallButton("Restore group shading")) {
            Features::HairRenderGroupSettings restored{};
            if (Features::RestoreHairRenderGroupOriginal(g_selectedHairRenderGroup, &restored)) e = restored;
        }
        HairTip("Back to the game's values for this group.");
    }
    ImGui::TextDisabled("Strand fade distances: %.2f (master %.2f to %.2f)", group.interpMaxDistance,
                        group.interpMinMasterDistance, group.interpMaxMasterDistance);
}

void DrawHairMotionTab(const Features::HairRuntimeInfo& rt) {
    DrawHairProfileCombo(true, rt);
    if (rt.physicsMappingProfile != rt.currentSimulationProfile) {
        ImGui::TextDisabled("Reading the new motion profile...");
        return;
    }
    const int count = Features::HairPhysicsGroupCount();
    if (count <= 0) {
        ImGui::TextDisabled("The active motion profile has no hair groups.");
        return;
    }
    g_selectedHairPhysicsGroup = std::clamp(g_selectedHairPhysicsGroup, 0, count - 1);
    if (HairGroupCombo("Group##physics", &g_selectedHairPhysicsGroup, count, &HairPhysicsHash)) g_loadedHairPhysicsGroup = -1;
    Features::HairPhysicsGroupInfo group{};
    if (!Features::GetHairPhysicsGroupInfo(g_selectedHairPhysicsGroup, group)) return;
    if (g_loadedHairPhysicsGroup != g_selectedHairPhysicsGroup || g_loadedHairSimProfile != rt.physicsMappingProfile) {
        g_hairPhysicsEdit = group.settings;
        g_loadedHairPhysicsGroup = g_selectedHairPhysicsGroup;
        g_loadedHairSimProfile = rt.physicsMappingProfile;
    }
    auto& e = g_hairPhysicsEdit;
    HairEditState st;
    HairSectionTitle("Forces");
    HairSlider("Gravity", &e.gravityMagnitude, 0.0f, 20.0f, "How strongly the hair is pulled down.", st);
    HairSlider("Wind", &e.windScale, 0.0f, 5.0f, "How much wind moves the hair.", st);
    HairSlider("Body motion", &e.rootMotionScale, 0.0f, 2.0f,
               "How much Lara's own movement swings the hair (0: the hair ignores her motion).", st);
    HairSlider("Body motion as wind", &e.rootMotionWindScale, 0.0f, 2.0f,
               "How much Lara's movement acts like wind blowing through the hair.", st);
    HairSlider("Sideways share", &e.rootMotionSidewaysPercentage, 0.0f, 1.0f,
               "How much of Lara's sideways movement is passed to the hair.", st);
    HairSectionTitle("Response");
    HairSlider("Damping", &e.damping, 0.0f, 1.0f, "How quickly the hair's swinging dies down: higher is calmer.", st);
    HairSlider("Simulation speed", &e.timeStepScale, 0.0f, 2.0f, "Speeds up or slows down the hair's motion.", st);
    HairSectionTitle("Collision");
    {
        const bool edited = ImGui::Checkbox("Collide with the body", &e.enableCollision);
        st.Track(edited);
        HairTip("Keeps the hair from going through Lara's head and shoulders (her collision spheres).");
    }
    HairSlider("Bounce", &e.restitutionScale, 0.0f, 1.0f, "How much the hair bounces off the body.", st);
    HairSlider("Friction", &e.frictionScale, 0.0f, 1.0f, "How much the hair sticks when sliding over the body.", st);
    HairSectionTitle("Stiffness");
    {
        int iterations = static_cast<int>(e.numLocalShapeMatchingIterations);
        ImGui::SetNextItemWidth(HairFieldWidth());
        const bool edited = ImGui::DragInt("Shape passes", &iterations, 0.1f, 0, 1000);
        st.Track(edited);
        if (edited) e.numLocalShapeMatchingIterations = static_cast<unsigned int>(std::max(0, iterations));
        HairTip("How many times a frame the strands are pulled back to their styled shape: more is stiffer (and costs more).");
    }
    HairCurve("Hold the style (along the strand)", e.localShapeMatchingStiffness, 8, 0.0f, 1.0f,
              "How strongly each part of a strand keeps its styled shape relative to its neighbours, root (left) to tip (right).", st);
    HairCurve("Hold the head's shape (along the strand)", e.worldShapeMatchingStiffness, 8, 0.0f, 1.0f,
              "How strongly each part of a strand is pulled back to where it sits on the head, root (left) to tip (right).", st);
    HairCurve("Hold the head's shape by speed", e.velocityWorldShapeMatchingScale, 20, 0.0f, 1.0f,
              "Scales the pull above by how fast Lara moves, slow (left) to fast (right).", st);
    if (st.changed && !Features::QueueHairPhysicsGroupUpdate(g_selectedHairPhysicsGroup, e, false))
        strncpy_s(g_hairUiStatus, "The motion change was not accepted (the hair changed).", _TRUNCATE);
    ImGui::Spacing();
    if (group.hasOriginalSettings) {
        if (ImGui::SmallButton("Restore group motion")) {
            Features::HairPhysicsGroupSettings restored{};
            if (Features::RestoreHairPhysicsGroupOriginal(g_selectedHairPhysicsGroup, &restored)) e = restored;
        }
        HairTip("Back to the game's values for this group.");
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("Settle the hair")) Features::QueueHairResetSimulation();
    HairTip("Puts the hair back in its rest pose (if it got tangled while trying values).");
}

void DrawHairStrandsTab(const Features::HairRuntimeInfo& rt) {
    const int count = Features::HairConstantGroupCount();
    if (count <= 0) {
        ImGui::TextDisabled("This hair has no strand data to edit.");
        return;
    }
    g_selectedHairConstantGroup = std::clamp(g_selectedHairConstantGroup, 0, count - 1);
    if (HairGroupCombo("Group##strands", &g_selectedHairConstantGroup, count, &HairConstantHash)) g_loadedHairConstantGroup = -1;
    Features::HairConstantGroupInfo group{};
    if (!Features::GetHairConstantGroupInfo(g_selectedHairConstantGroup, group)) return;
    if (g_loadedHairConstantGroup != g_selectedHairConstantGroup) {
        g_hairConstantEdit = group;
        g_loadedHairConstantGroup = g_selectedHairConstantGroup;
    }
    const std::uint64_t key = HairKey(rt.playerObjectId, static_cast<unsigned int>(g_selectedHairConstantGroup), group.groupNameHash);
    if (!g_hairConstantOriginals.count(key)) g_hairConstantOriginals[key] = group;
    ImGui::TextDisabled("The game builds these into the hair when it creates it: the hair is rebuilt a moment after "
                        "you stop changing them (its motion starts over).");
    auto& e = g_hairConstantEdit;
    HairEditState st;
    HairSectionTitle("Strands");
    HairSlider("Thickness", &e.thickness, 0.0f, 0.5f, "How thick each strand is drawn.", st, "%.4f");
    HairSlider("Frizz scale", &e.noiseFrequency, 0.0f, 10.0f, "How fine the random waviness along the strands is.", st);
    HairSlider("Frizz", &e.noiseStrength, 0.0f, 1.0f, "How much random waviness the strands get.", st);
    HairCurve("Thickness along the strand", e.shapeCurve, 8, 0.0f, 1.0f,
              "Scales the thickness along each strand, root (left) to tip (right): lower toward the tips makes them taper.", st);
    HairCurve("Opacity along the strand", e.alphaCurve, 8, 0.0f, 1.0f,
              "How opaque each part of a strand is, root (left) to tip (right): lower toward the tips softens them.", st);
    if (e.shownStrandColorCount > 0) {
        HairSectionTitle("Colors");
        if (e.totalStrandColorCount > e.shownStrandColorCount)
            ImGui::TextDisabled("%d color sets; the first %d can be edited.", e.totalStrandColorCount, e.shownStrandColorCount);
        if (ImGui::BeginTable("##haircolors", 6, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Set");
            ImGui::TableSetupColumn("Root A");
            ImGui::TableSetupColumn("Tip A");
            ImGui::TableSetupColumn("Root B");
            ImGui::TableSetupColumn("Tip B");
            ImGui::TableSetupColumn("Mix");
            ImGui::TableHeadersRow();
            for (int i = 0; i < e.shownStrandColorCount; ++i) {
                auto& c = e.strandColors[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%d", i + 1);
                const ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR;
                ImGui::TableNextColumn(); st.Track(ImGui::ColorEdit3("##r0", c.rootColor0, flags));
                ImGui::TableNextColumn(); st.Track(ImGui::ColorEdit3("##t0", c.tipColor0, flags));
                ImGui::TableNextColumn(); st.Track(ImGui::ColorEdit3("##r1", c.rootColor1, flags));
                ImGui::TableNextColumn(); st.Track(ImGui::ColorEdit3("##t1", c.tipColor1, flags));
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(UiPx(110.0f));
                st.Track(ImGui::DragFloat("##freq", &c.frequency, HairDragSpeed(c.frequency, 0.0f, 1.0f), 0.0f, 0.0f, "%.6g",
                                          ImGuiSliderFlags_NoRoundToFormat));
                HairTip("How often strands take colors A or B in this set (each strand blends from its root color to its tip color).");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    if (st.changed && !Features::QueueHairConstantGroupUpdate(g_selectedHairConstantGroup, e, true))
        strncpy_s(g_hairUiStatus, "The strand change was not accepted (the hair changed).", _TRUNCATE);
    ImGui::Spacing();
    if (ImGui::SmallButton("Restore group strands")) {
        const auto& original = g_hairConstantOriginals[key];
        const int shown = e.shownStrandColorCount;
        e = original;
        e.shownStrandColorCount = std::min(shown, original.shownStrandColorCount);
        Features::QueueHairConstantGroupUpdate(g_selectedHairConstantGroup, e, true);
    }
    HairTip("Back to the values this group had when the editor first saw it (then the hair is rebuilt).");
}

void DrawHairLayoutSetting();

void DrawHairAdvancedTab(const Features::HairRuntimeInfo& rt) {
    if (!g_hairAdvancedLoaded) {
        g_hairAdvancedEdit.componentRootMotionScale = rt.componentRootMotionScale;
        g_hairAdvancedEdit.underwaterGravity = rt.underwaterGravity;
        g_hairAdvancedEdit.underwaterDamping = rt.underwaterDamping;
        g_hairAdvancedEdit.targetUpdateRate = rt.targetUpdateRate;
        g_hairAdvancedEdit.multiStepUpdateRateThreshold = rt.multiStepUpdateRateThreshold;
        g_hairAdvancedEdit.resetRelaxSteps = rt.resetRelaxSteps;
        g_hairAdvancedEdit.lodProfile = rt.lodProfile;
        g_hairAdvancedEdit.cinematicLodProfile = rt.cinematicLodProfile;
        g_hairAdvancedEdit.combatLodProfile = rt.combatLodProfile;
        g_hairAdvancedEdit.headRadius = rt.headRadius;
        g_hairAdvancedEdit.headFalloff = rt.headFalloff;
        std::copy(std::begin(rt.headOffset), std::end(rt.headOffset), std::begin(g_hairAdvancedEdit.headOffset));
        g_hairAdvancedLoaded = true;
        if (!g_hairAdvancedOriginals.count(rt.playerObjectId)) g_hairAdvancedOriginals[rt.playerObjectId] = g_hairAdvancedEdit;
    }
    auto& e = g_hairAdvancedEdit;
    HairEditState st;
    HairSectionTitle("Head collision");
    HairSlider("Head radius", &e.headRadius, 0.0f, 0.5f, "The size of the sphere around the head that the hair rests on.", st, "%.4f");
    HairSlider("Head softness", &e.headFalloff, 0.0f, 0.5f, "How gradually the head sphere pushes the hair out.", st, "%.4f");
    {
        float lo = -0.5f, hi = 0.5f;
        for (float v : e.headOffset)
            if (std::isfinite(v)) {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        ImGui::SetNextItemWidth(HairFieldWidth());
        st.Track(ImGui::DragFloat3("Head offset", e.headOffset, HairDragSpeed(0.0f, lo, hi), 0.0f, 0.0f, "%.6g",
                                   ImGuiSliderFlags_NoRoundToFormat));
        HairTip("Moves the head sphere (x, y, z) relative to the head.");
    }
    HairSectionTitle("Movement and water");
    HairSlider("Body motion (overall)", &e.componentRootMotionScale, 0.0f, 2.0f,
               "Scales how much Lara's movement moves the hair, for every group and profile.", st);
    HairSlider("Gravity underwater", &e.underwaterGravity, -2.0f, 2.0f, "Gravity on the hair while underwater (negative: it floats up).", st);
    HairSlider("Damping underwater", &e.underwaterDamping, 0.0f, 1.0f, "How calm the hair is underwater.", st);
    HairSectionTitle("Update rate");
    HairSlider("Updates per second", &e.targetUpdateRate, 0.0f, 120.0f, "How often the hair simulation runs.", st, "%.1f");
    HairSlider("Extra steps below", &e.multiStepUpdateRateThreshold, 0.0f, 120.0f,
               "Below this frame rate the simulation runs several steps a frame to stay smooth.", st, "%.1f");
    {
        int steps = static_cast<int>(e.resetRelaxSteps);
        ImGui::SetNextItemWidth(HairFieldWidth());
        const bool edited = ImGui::DragInt("Settle steps", &steps, 0.2f, 0, 100000);
        st.Track(edited);
        if (edited) e.resetRelaxSteps = static_cast<unsigned int>(std::max(0, steps));
        HairTip("How many steps the hair takes to settle after it is reset.");
    }
    HairSectionTitle("Detail level");
    auto lodField = [&](const char* label, unsigned int* v, const char* tip) {
        int value = static_cast<int>(*v);
        ImGui::SetNextItemWidth(HairFieldWidth());
        const bool edited = ImGui::InputInt(label, &value);
        st.Track(edited);
        if (edited) *v = static_cast<unsigned int>(std::max(0, value));
        HairTip(tip);
    };
    lodField("Gameplay", &e.lodProfile, "The hair's level of detail profile during gameplay.");
    lodField("Cutscenes", &e.cinematicLodProfile, "The level of detail profile in cutscenes.");
    lodField("Combat", &e.combatLodProfile, "The level of detail profile in combat.");
    // live while dragging; once let go, applied again with a simulation reset (some of these are only read then)
    if (st.released) {
        if (!Features::QueueHairAdvancedUpdate(e, true))
            strncpy_s(g_hairUiStatus, "The change was not accepted (the hair changed).", _TRUNCATE);
    } else if (st.changed && !Features::QueueHairAdvancedUpdate(e, false)) {
        strncpy_s(g_hairUiStatus, "The change was not accepted (the hair changed).", _TRUNCATE);
    }
    ImGui::Spacing();
    if (ImGui::SmallButton("Restore advanced")) {
        auto it = g_hairAdvancedOriginals.find(rt.playerObjectId);
        if (it != g_hairAdvancedOriginals.end()) {
            e = it->second;
            Features::QueueHairAdvancedUpdate(e, true);
        }
    }
    HairTip("Back to the values the hair had when the editor first saw it.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Rebuild the hair")) {
        if (!Features::QueueHairRebuild()) strncpy_s(g_hairUiStatus, "The rebuild could not be queued.", _TRUNCATE);
    }
    HairTip("Creates the hair again from its data. Not needed normally (edits apply by themselves); for when it looks stuck.");
    DrawHairLayoutSetting();
}

// (v0.9.440) PureHair's strand layout ([Hair] StrandLayout): put into memory at the game's start (hair_layout.cpp).
void DrawHairLayoutSetting() {
    HairSectionTitle("Strand layout");
    auto& settings = Settings::Get();
    const int wanted = HairLayout::Parse(settings.hairStrandLayout);
    const char* const items[] = {"Off (the game's own)", "Stock (32 points)", "PH64 (64 points)", "PH128 (128 points)",
                                 "PH256 (256 points)"};
    int index = wanted + 1;
    ImGui::SetNextItemWidth(HairFieldWidth());
    if (ImGui::Combo("Layout at startup", &index, items, IM_ARRAYSIZE(items))) {
        strcpy_s(settings.hairStrandLayout, index == 0 ? "" : HairLayout::Name(index - 1));
        Settings::Save();
    }
    HairTip("How many points a hair strand can have: what rottr_hair.py patches into ROTTR.exe, done in memory when the "
            "game starts instead (the exe is not changed). Every hair the game loads must be exported in that layout, "
            "Lara's stock hair too. Takes effect at the next start.");
    ImGui::TextDisabled("This session: %s", HairLayout::Status());
    if (wanted >= 0 && wanted != HairLayout::Current())
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Restart the game for %s.", HairLayout::Name(wanted));
}

void DrawHairEditorWindow() {
    if (!g_showHairEditor) return;
    ImGui::SetNextWindowSize(UiSize(640.0f, 720.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("PureHair Editor", &g_showHairEditor)) {
        ImGui::End();
        return;
    }
    Features::HairRuntimeInfo rt{};
    const bool haveHair = Features::GetHairRuntimeInfo(rt);
    if (!haveHair) {
        ImGui::TextWrapped("Lara's current outfit has no PureHair loaded. The editor fills in as soon as an outfit with "
                           "dynamic hair is worn.");
        ImGui::TextDisabled("%s", Features::HairStatus());
        DrawHairLayoutSetting();
        ImGui::End();
        return;
    }
    if (g_loadedHairPlayerObject != rt.playerObjectId) {
        g_loadedHairPlayerObject = rt.playerObjectId;
        g_loadedHairPhysicsGroup = -1;
        g_loadedHairRenderGroup = -1;
        g_loadedHairConstantGroup = -1;
        g_loadedHairSimProfile = 0xFFFFFFFFu;
        g_loadedHairRenderProfile = 0xFFFFFFFFu;
        g_loadedHairRenderGroupProfile = 0xFFFFFFFFu;
        g_hairAdvancedLoaded = false;
        g_hairUiStatus[0] = '\0';
    }
    // the game switched profiles on its own: read the new one's groups (only while nothing is being dragged)
    const bool stale = rt.renderMappingProfile != rt.currentRenderingProfile ||
                       rt.physicsMappingProfile != rt.currentSimulationProfile;
    if (stale && !ImGui::IsAnyItemActive() && ImGui::GetTime() - g_hairLastResync > 1.0) {
        g_hairLastResync = ImGui::GetTime();
        Features::RequestHairRefresh();
    }

    ImGui::TextDisabled("Object %u  |  %d group%s", static_cast<unsigned int>(rt.playerObjectId), rt.physicsGroupCount,
                        rt.physicsGroupCount == 1 ? "" : "s");
    ImGui::SameLine();
    if (Features::HairRebuildPending())
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "  Rebuilding the hair...");
    else if (Features::OutfitSwapInProgress())
        ImGui::TextDisabled("  (changing outfit: edits wait)");
    ImGui::TextDisabled("Changes apply as you make them. Hover a value for what it does.");

    if (ImGui::BeginTabBar("##hair_tabs")) {
        if (ImGui::BeginTabItem("Look")) {
            DrawHairLookTab(rt);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Motion")) {
            DrawHairMotionTab(rt);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Strands")) {
            DrawHairStrandsTab(rt);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Advanced")) {
            DrawHairAdvancedTab(rt);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (g_hairUiStatus[0]) {
        ImGui::Separator();
        ImGui::TextDisabled("%s", g_hairUiStatus);
    }
    ImGui::End();
}

bool ContainsNoCase(const char* text, const char* needle) {
    if (!needle || !needle[0]) return true;
    if (!text) return false;
    for (const char* start = text; *start; ++start) {
        const char* a = start;
        const char* b = needle;
        while (*a && *b &&
               std::tolower(static_cast<unsigned char>(*a)) ==
               std::tolower(static_cast<unsigned char>(*b))) {
            ++a;
            ++b;
        }
        if (!*b) return true;
    }
    return false;
}

void RebuildInstanceFilter() {
    g_instanceFiltered.clear();
    g_instanceFiltered.reserve(g_instanceCache.size());
    for (int i = 0; i < static_cast<int>(g_instanceCache.size()); ++i) {
        const auto& info = g_instanceCache[static_cast<std::size_t>(i)];
        char idText[32];
        std::snprintf(idText, sizeof(idText), "%u", info.instanceId);
        if (ContainsNoCase(info.name, g_instanceSearch) || ContainsNoCase(idText, g_instanceSearch))
            g_instanceFiltered.push_back(i);
    }
    strncpy_s(g_lastInstanceSearch, g_instanceSearch, _TRUNCATE);
}

void RefreshInstanceUiCache() {
    if (!Features::InstanceToolsAvailable()) {
        g_instanceCache.clear();
        g_instanceFiltered.clear();
        return;
    }

    const std::uint64_t version = Features::InstanceListVersion();
    if (version != g_instanceCacheVersion) {
        const int count = Features::InstanceCount();
        g_instanceCache.resize(count > 0 ? static_cast<std::size_t>(count) : 0u);
        int copied = 0;
        if (!g_instanceCache.empty())
            copied = Features::CopyInstances(g_instanceCache.data(), static_cast<int>(g_instanceCache.size()));
        if (copied < 0) copied = 0;
        g_instanceCache.resize(static_cast<std::size_t>(copied));
        std::sort(g_instanceCache.begin(), g_instanceCache.end(),
                  [](const Features::InstanceInfo& a, const Features::InstanceInfo& b) {
                      return a.instanceId > b.instanceId;
                  });
        g_instanceCacheVersion = version;
        RebuildInstanceFilter();
    } else if (std::strcmp(g_instanceSearch, g_lastInstanceSearch) != 0) {
        RebuildInstanceFilter();
    }
}

const Features::InstanceInfo* SelectedInstance() {
    if (!g_selectedInstanceId || !g_selectedInstanceComponentId) return nullptr;
    for (const auto& info : g_instanceCache) {
        if (info.instanceId == g_selectedInstanceId && info.componentId == g_selectedInstanceComponentId)
            return &info;
    }
    return nullptr;
}

void SelectInstance(const Features::InstanceInfo& info) {
    if (g_selectedInstanceId != info.instanceId || g_selectedInstanceComponentId != info.componentId) {
        g_markerCache.clear();
        g_markerFiltered.clear();
        g_markerEdits.clear();
        g_markerCacheVersion = ~std::uint64_t{0};
    }
    g_selectedInstanceId = info.instanceId;
    g_selectedInstanceComponentId = info.componentId;
    g_instanceEdit = info.transform;
    g_instanceEditLoaded = info.detailsReady;
    g_instancePositionFieldActive = false;
    g_instanceRotationFieldActive = false;
    g_instanceLiveAcceptSequence = 0;
    if (info.detailsReady) {
        const InstanceOriginalKey originalKey{info.instanceId, info.componentId};
        g_instanceOriginalTransforms.try_emplace(originalKey, info.transform);
    } else {
        Features::RequestInstanceDetails(info.instanceId, info.componentId);
    }
    g_instanceUiStatus[0] = '\0';
}

void SyncInstanceHoverHighlight() {
    std::uint32_t id = 0;
    std::uint64_t componentId = 0;
    if (g_showInstanceBrowser && Features::InstanceNativeHighlightAvailable()) {
        const double now = ImGui::GetTime();
        if (g_instanceHoverId) {
            g_instanceHoverHeldId = g_instanceHoverId;
            g_instanceHoverHeldComponentId = g_instanceHoverComponentId;
            g_instanceHoverHeldUntil = now + 0.12;
        }
        if (g_instanceHoverHeldId && now <= g_instanceHoverHeldUntil) {
            id = g_instanceHoverHeldId;
            componentId = g_instanceHoverHeldComponentId;
        }
    }
    g_instanceHoverId = 0;
    g_instanceHoverComponentId = 0;

    // Sent every frame while something is hovered (the game side drops a hover not refreshed for 300 ms),
    // and once when it ends.
    if (id) {
        Features::QueueInstanceHoverHighlight(id, componentId);
        g_instanceHoverSent = true;
    } else if (g_instanceHoverSent) {
        if (Features::QueueInstanceHoverHighlight(0, 0))
            g_instanceHoverSent = false;
        g_instanceHoverHeldId = 0;
        g_instanceHoverHeldComponentId = 0;
    }
}

void SyncInstanceSelectionHighlight() {
    std::uint32_t id = 0;
    std::uint64_t componentId = 0;
    if (g_showInstanceBrowser && Features::InstanceNativeHighlightAvailable()) {
        if (const auto* selected = SelectedInstance()) {
            id = selected->instanceId;
            componentId = selected->componentId;
        }
    }

    if (id == g_lastInstanceHighlightId && componentId == g_lastInstanceHighlightComponentId)
        return;
    if (Features::QueueInstanceSelectionHighlight(id, componentId)) {
        g_lastInstanceHighlightId = id;
        g_lastInstanceHighlightComponentId = componentId;
    }
}

void RebuildBirthSuggestions() {
    strncpy_s(g_lastBirthSearch, g_birthObjectName, _TRUNCATE);
    g_birthSuggestions.clear();
    if (!g_birthObjectName[0]) {
        g_showBirthSuggestions = false;
        return;
    }

    
    
    
    
    constexpr int kMaxSuggestions = 10000;
    g_birthSuggestions.resize(kMaxSuggestions);
    const int found = Features::FindZoneSuggestions(g_birthObjectName, g_birthSuggestions.data(), kMaxSuggestions);
    g_birthSuggestions.resize(found > 0 ? static_cast<std::size_t>(found) : 0u);
    g_showBirthSuggestions = !g_birthSuggestions.empty();
}

// (v0.9.607) Draw > Draw wireframe and its options (eight_weight_patch.h). The colours apply when the colour control is
// let go: each change makes the lines' pipelines again.
void DrawWireframeOptions() {
    const bool available = EightWeightPatch::WireframeAvailable();
    bool on = EightWeightPatch::WireframeEnabled();
    if (!available) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Draw wireframe", &on)) EightWeightPatch::SetWireframeEnabled(on);
    if (!available) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The edges of the triangles of everything the game draws solid: the level, Lara, the characters,\n"
                          "objects (not glass, particles, water or the shadows). The first frames after switching it on\n"
                          "make the lines' pipelines: a short hitch. Not saved; the options below are.%s",
                          available ? "" : "\nUnavailable: DirectX 12 only (see ROTTRFramework.log).");
    if (!on) return;

    static EightWeightPatch::WireframeOptions s_edit{};
    static bool s_editing = false;   // a colour being changed (applied when let go)
    EightWeightPatch::WireframeOptions options = s_editing ? s_edit : EightWeightPatch::GetWireframeOptions();
    bool apply = false;
    ImGui::Indent();
    static const char* const kStyles[] = {"Over the picture", "Lines only"};
    ImGui::SetNextItemWidth(UiPx(160.0f));
    if (ImGui::Combo("Style##wireframe", &options.style, kStyles, IM_ARRAYSIZE(kStyles))) apply = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Over the picture: the game's image as it is, with the lines on it.\n"
                          "Lines only: the surfaces in one flat colour (Surfaces, below) and the lines on them: no\n"
                          "textures, lighting or shadows on what gets lines.");
    const ImGuiColorEditFlags colorFlags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR;
    ImGui::SetNextItemWidth(UiPx(220.0f));
    if (ImGui::ColorEdit4("Lines##wireframe", options.lineColor, colorFlags | ImGuiColorEditFlags_AlphaBar))
        s_editing = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The lines' colour and opacity (A). The game's exposure and tone mapping apply to it as to the\n"
                          "rest of its picture: past 1 for brighter lines in dark places.");
    if (options.style == 1) {
        ImGui::SetNextItemWidth(UiPx(220.0f));
        if (ImGui::ColorEdit3("Surfaces##wireframe", options.fillColor, colorFlags)) s_editing = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Lines only: the colour the surfaces are drawn in, under the lines (the game's exposure and\n"
                              "tone mapping apply to it too).");
    }
    if (ImGui::Checkbox("See through##wireframe", &options.seeThrough)) apply = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The lines of what is hidden behind other surfaces too (everything drawn on screen, whatever is in\n"
                          "front of it).");
    ImGui::SameLine();
    if (ImGui::Checkbox("Back faces##wireframe", &options.backFaces)) apply = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The edges of the triangles facing away from the camera too (the game leaves most of them out).\n"
                          "With See through: both sides of every model.");
    if (s_editing) {
        s_edit = options;
        if (!ImGui::IsAnyItemActive()) apply = true;   // the colour let go
    }
    if (apply) {
        EightWeightPatch::SetWireframeOptions(options);
        s_editing = false;
        auto& s = Settings::Get();
        s.wireframeStyle = options.style;
        std::memcpy(s.wireframeColor, options.lineColor, sizeof(s.wireframeColor));
        std::memcpy(s.wireframeBackground, options.fillColor, sizeof(s.wireframeBackground));
        s.wireframeSeeThrough = options.seeThrough;
        s.wireframeBackFaces = options.backFaces;
        Settings::Save();
    }
    ImGui::TextDisabled("%s", EightWeightPatch::WireframeStatus());
    ImGui::Unindent();
}

// (v1.0.0) The Camera tab's offsets and field of view, now in the General tab. "Enable gameplay camera offsets" removed:
// the offsets apply whenever one of them is not zero (features.cpp ApplyGameplayCameraOffsets); depth of field is in
// the General part above; the native camera's position / quaternion / FOV readouts left out.
void DrawCameraSection() {
    const bool available = Features::GameplayCameraAvailable();
    ImGui::TextUnformatted("Camera");
    ImGui::Separator();

    float position[3]{};
    float rotation[3]{};
    Features::GetGameplayCameraOffsets(position, rotation);
    if (!available) ImGui::BeginDisabled();
    const float fieldWidth = std::min(UiPx(430.0f), ImGui::GetContentRegionAvail().x);
    ImGui::SetNextItemWidth(fieldWidth);
    if (ImGui::DragFloat3("Position offset (right / forward / up)", position, 1.0f, -5000.0f, 5000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp))
        Features::SetGameplayCameraOffsets(position, rotation);
    ImGui::SetNextItemWidth(fieldWidth);
    if (ImGui::DragFloat3("Rotation offset (pitch / yaw / roll)", rotation, 0.10f, -180.0f, 180.0f, "%.2f deg", ImGuiSliderFlags_AlwaysClamp))
        Features::SetGameplayCameraOffsets(position, rotation);
    if (ImGui::Button("Reset camera offsets"))
        Features::ResetGameplayCameraOffsets();
    if (!available) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The gameplay camera's position and rotation offsets back to zero (the game's own camera).");

    float mul = Features::FovMultiplier();
    ImGui::SetNextItemWidth(UiPx(260.0f));
    if (ImGui::SliderFloat("FOV multiplier", &mul, 0.50f, 2.00f, "%.2fx"))
        Features::SetFovMultiplier(mul);
    if (ImGui::IsItemDeactivatedAfterEdit()) Settings::Save();
    ImGui::SameLine();
    if (ImGui::Button("Reset##gameplay_fov")) {
        Features::SetFovMultiplier(1.0f);
        Settings::Save();
    }
    if (Features::FreeCameraEnabled())
        ImGui::TextDisabled("The free camera is on: it takes priority over the camera offsets.");
}

void DrawGeneralTab() {
    
    
    const float kBirthFieldWidth = UiPx(345.0f);
    const float kBirthButtonWidth = UiPx(94.0f);

    ImGui::TextUnformatted("Birth Instance");
    ImGui::Separator();

    ImGui::SetNextItemWidth(kBirthFieldWidth);
    const bool birthInputChanged = ImGui::InputTextWithHint("##birth_instance_name", "Object name or zone ID...",
                                                            g_birthObjectName, sizeof(g_birthObjectName));
    if (birthInputChanged || std::strcmp(g_birthObjectName, g_lastBirthSearch) != 0)
        RebuildBirthSuggestions();

    if (g_showBirthSuggestions && !g_birthSuggestions.empty()) {
        ImGui::TextDisabled("%d matching ObjectZones", static_cast<int>(g_birthSuggestions.size()));
        const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
        const float listHeight = std::min(8.0f, static_cast<float>(g_birthSuggestions.size())) * rowHeight + UiPx(8.0f);
        ImGui::BeginChild("birth_autocomplete", ImVec2(kBirthFieldWidth, listHeight), true);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(g_birthSuggestions.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto& suggestion = g_birthSuggestions[static_cast<std::size_t>(row)];
                char label[180]{};
                std::snprintf(label, sizeof(label), "%5u  %s##birth_zone_%u", suggestion.zoneId, suggestion.name, suggestion.zoneId);
                if (ImGui::Selectable(label, false)) {
                    strncpy_s(g_birthObjectName, suggestion.name, _TRUNCATE);
                    strncpy_s(g_lastBirthSearch, g_birthObjectName, _TRUNCATE);
                    g_showBirthSuggestions = false;
                }
            }
        }
        ImGui::EndChild();
    }

    const bool busy = Features::BirthInstanceBusy();
    const bool canBirth = Features::InstanceToolsAvailable() && !busy && g_birthObjectName[0] != '\0';
    if (!canBirth) ImGui::BeginDisabled();
    if (ImGui::Button("Birth", ImVec2(kBirthButtonWidth, 0.0f)))
        Features::RequestBirthInstance(g_birthObjectName);
    if (!canBirth) ImGui::EndDisabled();

    const char* birthStatus = Features::BirthInstanceStatus();   // (v1.0.0: empty while ready, 3 s after a success)
    if (std::strncmp(birthStatus, "Birth cancelled:", 16) == 0)
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.25f, 1.0f), "%s", birthStatus);
    else if (birthStatus[0])
        ImGui::TextDisabled("%s", birthStatus);
    // (v0.9.607) its DRM path and "Resolved ObjectZone" lines taken out (the log keeps both)

    // (v0.9.486) The Draw distances are under their checkboxes in the Draw menu.

    // (v1.0.0) one "General" part (Loading, Cutscene skeleton and Physics until then), then the Camera tab's and the
    // Weather tab's controls
    ImGui::Spacing();
    ImGui::TextUnformatted("General");
    ImGui::Separator();

    // (v0.9.459) Faster loading: the old Fast startup and Faster loading times as one option, on by default.
    bool fasterLoading = Features::FastLoadingEnabled();
    if (ImGui::Checkbox("Faster loading", &fasterLoading)) {
        Features::SetFastLoadingEnabled(fasterLoading);
        Settings::Save();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Startup: skips the Legal/logo presentation, caches the startup Steam save-name sweep,\n"
            "consumes completed PROFILE_LOAD work promptly and advances UiMenuStateMountingDLC's native\n"
            "manager queues (from the next launch; faultrep.dll reads this option too).\n"
            "Level loads: during START_LOADING removes known short polling delays, forces Present sync\n"
            "interval 0, requests ROTTR's native single-thread renderer mode when the verified hook is\n"
            "available and uses non-blocking Present for loading-screen refreshes (from the next load).\n"
            "Retail readiness/finalization and render completion events are never bypassed; the original\n"
            "renderer mode is restored after loading.");
    }


    // (v0.9.486) The DX12 tessellation fix is always on (no checkbox); DLAA lives in the game's DLSS list (no line here).

    // (v0.9.519) The graphics options (LOD, shadows, fog, the hair's shadow) are in Enhancements > Graphics.

    bool doNotPauseOnFocusLoss = Features::DoNotPauseOnFocusLossEnabled();
    const bool focusLossAvailable = Features::DoNotPauseOnFocusLossAvailable();
    if (!focusLossAvailable) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Do not pause on focus loss", &doNotPauseOnFocusLoss)) {
        Features::SetDoNotPauseOnFocusLossEnabled(doNotPauseOnFocusLoss);
        Settings::Save();
    }
    if (!focusLossAvailable) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Keep ROTTR's game-window-active check true while this option is enabled,\n"
            "so gameplay continues when Alt-Tabbing or focusing another window.%s",
            focusLossAvailable ? "" : "\nThe required game-window hook is unavailable for this executable.");
    }

    // (v0.9.586) Allow campsites near enemies: in Enhancements > Gameplay

    bool preserveModelBonePositions = BonePatch::PreserveModelBonePositionsEnabled();
    if (!BonePatch::Installed()) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Fix cutscene bone positions", &preserveModelBonePositions)) {
        BonePatch::SetPreserveModelBonePositionsEnabled(preserveModelBonePositions);
        Settings::Get().fixCutsceneBonePositions = preserveModelBonePositions;
        Settings::Save();
    }
    if (!BonePatch::Installed()) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Prevents cutscenes from overriding a model's own skeleton transforms,\n"
            "avoiding bone deformation or shifting when custom bone positions are used.\n"
            "Applies to Lara and to characters (objects with a DRAWABLE component);\n"
            "props have none and keep the places the cutscene gives their pieces.\n"
            "The correction is only applied while ROTTR's cinematic camera stack is active;\n"
            "normal gameplay and death animations remain untouched.");
    }

    bool clothLevelCollision = Features::ClothLevelCollisionEnabled();
    const bool clothLevelCollisionAvailable = Features::ClothLevelCollisionAvailable();
    if (!clothLevelCollisionAvailable) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Cloth level collision", &clothLevelCollision)) {
        Features::SetClothLevelCollisionEnabled(clothLevelCollision);
        Settings::Save();
    }
    if (!clothLevelCollisionAvailable) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Experimental global Cloth option. Tests free cloth knots against ROTTR's\n"
            "loaded level collision after the native cloth solve. The current build uses\n"
            "full 512-knot coverage, two-sided probes and a 6-unit contact clearance.%s",
            clothLevelCollisionAvailable ? "" : "\nThe required Cloth/CollisionScene hooks are unavailable for this executable.");
    }

    // (v1.0.0, from the Camera tab)
    bool depthOfField = Features::DepthOfFieldEnabled();
    const bool depthOfFieldAvailable = Features::DepthOfFieldAvailable();
    if (!depthOfFieldAvailable) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Enable depth of field (F7)", &depthOfField))
        Features::SetDepthOfFieldEnabled(depthOfField);
    if (!depthOfFieldAvailable) ImGui::EndDisabled();

    // (v0.9.607) Wireframe moved to the Draw menu (Draw wireframe). (v1.0.0) The editor link's, the Modding Suite
    // link's and the scene builds' status lines removed.
    ImGui::Spacing();
    DrawCameraSection();
    ImGui::Spacing();
    DrawWeatherSection();
}

void DrawPersistentMenuWindow() {
    ImGui::SetNextWindowPos(UiSize(18.0f, 48.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(UiSize(610.0f, 610.0f), ImGuiCond_FirstUseEver);
    bool skillsTabActive = false;
    bool weaponsTabActive = false;
    bool equipmentTabActive = false;
    bool cardsTabActive = false;
    if (ImGui::Begin("Menu", nullptr)) {
        if (ImGui::BeginTabBar("menu_tabs")) {
            if (ImGui::BeginTabItem("General")) {
                DrawGeneralTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Player")) {
                DrawPlayerTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Skills")) {
                skillsTabActive = true;
                DrawSkillsTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Inventory")) {   // (v0.9.598: "Weapons" until then; its Weapons and Equipment pages)
                if (ImGui::BeginTabBar("inventory_tabs")) {
                    if (ImGui::BeginTabItem("Weapons")) {
                        weaponsTabActive = true;
                        DrawWeaponsTab();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Equipment")) {
                        equipmentTabActive = true;
                        DrawEquipmentTab();
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Cards")) {
                cardsTabActive = true;
                DrawCardsTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Glowstick")) {   // (v0.9.473: its own tab; the Cutscenes tab removed)
                GlowColor::DrawTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("LaraFM")) {   // (v0.9.491)
                LaraFM::DrawTab();
                ImGui::EndTabItem();
            }
            ScriptEngine::DrawMenuTabs();
            if (ImGui::BeginTabItem("Enhancements")) {   // (v0.9.479; v0.9.486: the last tab)
                DrawEnhancementsTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    Features::SetSkillEditorActive(skillsTabActive);
    Features::SetWeaponMenuActive(weaponsTabActive);
    Features::SetEquipmentMenuActive(equipmentTabActive);
    Features::SetCardMenuActive(cardsTabActive);
    ImGui::End();
}

void DrawMarkerEditorContents();

// Instances window, Animation tab: a clip from the Instance's animation libraries played on that Instance only.
void DrawInstanceAnimationTab(const Features::InstanceInfo& instance) {
    if (!InstanceAnim::Available()) {
        ImGui::TextDisabled("Animation playback is unavailable: %s", InstanceAnim::UnavailableReason());
        return;
    }
    // One list per Instance (and per "all libraries" choice), built on the game thread.
    if (!g_animListRequested || instance.instanceId != g_animListInstance || instance.componentId != g_animListComponent ||
        g_animListAllRequested != g_animAllLibraries) {
        InstanceAnim::RequestList(instance.instanceId, instance.componentId, g_animAllLibraries);
        if (instance.instanceId != g_animListInstance || instance.componentId != g_animListComponent)
            g_animHasSelection = false;
        g_animListInstance = instance.instanceId;
        g_animListComponent = instance.componentId;
        g_animListAllRequested = g_animAllLibraries;
        g_animListRequested = true;
        g_animSuggestionsVersion = ~std::uint64_t{0};
    }

    int entries = 0, libraries = 0;
    bool listAll = false;
    const bool listReady = InstanceAnim::GetListInfo(instance.instanceId, instance.componentId, entries, libraries, listAll) &&
                           listAll == g_animAllLibraries;
    if (ImGui::Checkbox("All loaded libraries##anim_all", &g_animAllLibraries)) g_animHasSelection = false;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Off: only the libraries of the animation graphs this Instance runs (clips made for its skeleton).\n"
                          "On: every animation library loaded right now (clips for other skeletons can look broken).");
    ImGui::SameLine();
    if (ImGui::Button("Refresh##anim_refresh")) {
        InstanceAnim::RequestList(instance.instanceId, instance.componentId, g_animAllLibraries);
        g_animSuggestionsVersion = ~std::uint64_t{0};
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Read the animation libraries again (they load and unload with the level).");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", listReady ? InstanceAnim::ListStatus() : "Reading animation libraries...");

    const float fieldWidth = std::min(UiPx(345.0f), ImGui::GetContentRegionAvail().x);
    ImGui::SetNextItemWidth(fieldWidth);
    const bool searchChanged = ImGui::InputTextWithHint("##anim_search", "Animation name or ID...", g_animSearch,
                                                        sizeof(g_animSearch));
    const std::uint64_t listVersion = InstanceAnim::ListVersion();
    if (searchChanged || std::strcmp(g_animSearch, g_animLastSearch) != 0 || listVersion != g_animSuggestionsVersion) {
        strncpy_s(g_animLastSearch, g_animSearch, _TRUNCATE);
        g_animSuggestionsVersion = listVersion;
        g_animSuggestions.resize(listReady ? static_cast<std::size_t>(std::max(entries, 0)) : 0u);
        const int found = g_animSuggestions.empty()
            ? 0
            : InstanceAnim::FindAnimations(g_animSearch, g_animSuggestions.data(), static_cast<int>(g_animSuggestions.size()));
        g_animSuggestions.resize(found > 0 ? static_cast<std::size_t>(found) : 0u);
    }

    if (listReady && !g_animSuggestions.empty()) {
        ImGui::TextDisabled("%d matching animations", static_cast<int>(g_animSuggestions.size()));
        const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
        const float listHeight = std::min(10.0f, static_cast<float>(g_animSuggestions.size())) * rowHeight + UiPx(8.0f);
        ImGui::BeginChild("anim_autocomplete", ImVec2(fieldWidth, listHeight), true);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(g_animSuggestions.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto& entry = g_animSuggestions[static_cast<std::size_t>(row)];
                char label[160]{};
                std::snprintf(label, sizeof(label), "%s (%d)##anim_%u_%u", entry.name, static_cast<int>(entry.animId),
                              entry.libraryKey, static_cast<unsigned>(entry.index));
                const bool isSelected = g_animHasSelection && g_animSelected.libraryKey == entry.libraryKey &&
                                        g_animSelected.index == entry.index;
                if (ImGui::Selectable(label, isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
                    g_animSelected = entry;
                    g_animHasSelection = true;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        InstanceAnim::RequestPlay(instance.instanceId, instance.componentId, entry, g_animLoop, g_animSpeed);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\nLibrary: %s (key %u, entry %u)\n%u frames, %.2f s\nDouble-click to play.",
                                      entry.name, entry.library[0] ? entry.library : "?", entry.libraryKey,
                                      static_cast<unsigned>(entry.index), static_cast<unsigned>(entry.keys),
                                      static_cast<double>(entry.keys) * entry.timePerKey / 3000.0);
            }
        }
        ImGui::EndChild();
    } else if (listReady && entries > 0) {
        ImGui::TextDisabled("No animation matches.");
    }

    // Play the selected entry, or the best match of what was typed.
    const InstanceAnim::AnimListEntry* toPlay = g_animHasSelection ? &g_animSelected
        : (g_animSearch[0] && !g_animSuggestions.empty() ? &g_animSuggestions.front() : nullptr);
    if (toPlay)
        ImGui::Text("Selected: %s (%d)", toPlay->name, static_cast<int>(toPlay->animId));
    else
        ImGui::TextDisabled("Selected: none (click an animation in the list)");

    const float buttonWidth = UiPx(94.0f);
    if (!toPlay) ImGui::BeginDisabled();
    if (ImGui::Button("Play##anim_play", ImVec2(buttonWidth, 0.0f)) && toPlay)
        InstanceAnim::RequestPlay(instance.instanceId, instance.componentId, *toPlay, g_animLoop, g_animSpeed);
    if (!toPlay) ImGui::EndDisabled();

    InstanceAnim::PlaybackInfo playback{};
    const bool playing = InstanceAnim::GetPlayback(instance.instanceId, instance.componentId, playback);
    ImGui::SameLine();
    if (!playing) ImGui::BeginDisabled();
    if (ImGui::Button(playback.paused ? "Resume##anim_pause" : "Pause##anim_pause", ImVec2(buttonWidth, 0.0f)))
        InstanceAnim::SetPaused(instance.instanceId, instance.componentId, !playback.paused);
    ImGui::SameLine();
    if (ImGui::Button("Stop##anim_stop", ImVec2(buttonWidth, 0.0f)))
        InstanceAnim::Stop(instance.instanceId, instance.componentId);
    if (!playing) ImGui::EndDisabled();

    if (ImGui::Checkbox("Loop##anim_loop", &g_animLoop) && playing)
        InstanceAnim::SetLoop(instance.instanceId, instance.componentId, g_animLoop);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: the clip plays once and holds its last frame until Stop.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(UiPx(180.0f));
    if (ImGui::SliderFloat("Speed##anim_speed", &g_animSpeed, 0.05f, 4.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp) && playing)
        InstanceAnim::SetSpeed(instance.instanceId, instance.componentId, g_animSpeed);

    if (playing) {
        ImGui::Text("Playing: %s (%d)%s", playback.name, static_cast<int>(playback.animId),
                    playback.paused ? "  [paused]" : "");
        float frame = static_cast<float>(playback.frame);
        const int lastFrame = std::max(playback.frames - 1, 0);
        char format[96]{};
        std::snprintf(format, sizeof(format), "frame %%.0f / %d  (%.2f / %.2f s)", lastFrame,
                      static_cast<double>(playback.time), static_cast<double>(playback.duration));
        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::SliderFloat("##anim_frame", &frame, 0.0f, static_cast<float>(lastFrame), format))
            InstanceAnim::SeekFrame(instance.instanceId, instance.componentId, frame);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to scrub (pauses on that frame).");
    }
    ImGui::TextDisabled("%s", InstanceAnim::PlayStatus());
}

void DrawInstanceBrowserWindow() {
    if (!g_showInstanceBrowser) return;
    ImGui::SetNextWindowSize(UiSize(920.0f, 650.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Instances", &g_showInstanceBrowser)) {
        ImGui::End();
        return;
    }

    if (!Features::InstanceToolsAvailable()) {
        ImGui::TextDisabled("Live Instance access is unavailable for this executable build.");
        ImGui::End();
        return;
    }

    RefreshInstanceUiCache();

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##instance_search", "Search instances by name or ID...",
                             g_instanceSearch, sizeof(g_instanceSearch));
    if (std::strcmp(g_instanceSearch, g_lastInstanceSearch) != 0)
        RebuildInstanceFilter();
    ImGui::TextDisabled("%d shown / %d currently spawned", static_cast<int>(g_instanceFiltered.size()),
                        static_cast<int>(g_instanceCache.size()));
    if (Features::InstanceNativeHighlightAvailable())
    {
        bool highlightSelected = Settings::Get().instanceHighlightSelected;
        if (ImGui::Checkbox("Highlight selected Instance (yellow)", &highlightSelected)) {
            Settings::Get().instanceHighlightSelected = highlightSelected;
            Features::SetInstanceSelectionTintEnabled(highlightSelected);
            Settings::Save();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The hovered Instance is tinted green, the selected one yellow while this is on.\n"
                              "The tint is applied to the materials of its model, so other copies drawn\n"
                              "with the same materials light up too.");
    }
    ImGui::Separator();

    const float listWidth = UiPx(355.0f);
    ImGui::BeginChild("instance_list", ImVec2(listWidth, 0.0f), true);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(g_instanceFiltered.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const int cacheIndex = g_instanceFiltered[static_cast<std::size_t>(row)];
            if (cacheIndex < 0 || cacheIndex >= static_cast<int>(g_instanceCache.size())) continue;
            const auto& info = g_instanceCache[static_cast<std::size_t>(cacheIndex)];
            const bool selected = info.instanceId == g_selectedInstanceId &&
                                  info.componentId == g_selectedInstanceComponentId;
            char label[180];
            char fallbackName[32]{};
            const char* displayName = info.name;
            if (!displayName[0]) {
                std::snprintf(fallbackName, sizeof(fallbackName), "instance_%u", info.instanceId);
                displayName = fallbackName;
            }
            std::snprintf(label, sizeof(label), "%5u  %s##inst_%u_%016llX",
                          info.instanceId, displayName, info.instanceId,
                          static_cast<unsigned long long>(info.componentId));
            if (selected) {
                
                
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.16f, 0.42f, 0.72f, 0.82f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.20f, 0.50f, 0.82f, 0.90f));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.22f, 0.55f, 0.90f, 0.96f));
            }
            const bool clicked = ImGui::Selectable(label, selected);
            if (ImGui::IsItemHovered()) {
                g_instanceHoverId = info.instanceId;
                g_instanceHoverComponentId = info.componentId;
            }
            if (selected) ImGui::PopStyleColor(3);
            if (clicked)
                SelectInstance(info);
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("instance_details", ImVec2(0.0f, 0.0f), true);
    const Features::InstanceInfo* selected = SelectedInstance();
    Features::SetInstanceTransformWatch(selected ? selected->instanceId : 0u, selected ? selected->componentId : 0u);
    if (!selected) {
        if (g_selectedInstanceId) {
            ImGui::TextDisabled("The selected Instance is no longer spawned (or its pool slot was reused).");
            if (ImGui::Button("Clear selection")) {
                g_selectedInstanceId = 0;
                g_selectedInstanceComponentId = 0;
                g_instanceEditLoaded = false;
                g_markerCache.clear();
                g_markerFiltered.clear();
                g_markerEdits.clear();
                g_markerCacheVersion = ~std::uint64_t{0};
                Features::SetMarkerEditorTarget(0, 0);
            }
        } else {
            ImGui::TextDisabled("Select a spawned Instance from the list.");
        }
        ImGui::EndChild();
        ImGui::End();
        return;
    }

    if (!selected->detailsReady) {
        Features::RequestInstanceDetails(selected->instanceId, selected->componentId);
    } else if (!g_instanceEditLoaded) {
        g_instanceEdit = selected->transform;
        g_instanceEditLoaded = true;
        const InstanceOriginalKey originalKey{selected->instanceId, selected->componentId};
        g_instanceOriginalTransforms.try_emplace(originalKey, selected->transform);
    }

    char selectedFallbackName[32]{};
    const char* selectedDisplayName = selected->name;
    if (!selectedDisplayName[0]) {
        std::snprintf(selectedFallbackName, sizeof(selectedFallbackName), "instance_%u", selected->instanceId);
        selectedDisplayName = selectedFallbackName;
    }
    ImGui::TextUnformatted(selectedDisplayName);
    ImGui::Separator();
    ImGui::Text("Position: %.5f  %.5f  %.5f", selected->transform.position[0],
                selected->transform.position[1], selected->transform.position[2]);
    if (selected->detailsReady) {
        ImGui::Text("Rotation: %.6f  %.6f  %.6f", selected->transform.rotation[0],
                    selected->transform.rotation[1], selected->transform.rotation[2]);
    } else {
        ImGui::TextDisabled("Rotation: loading...");
    }
    ImGui::Text("Instance ID: %u", selected->instanceId);
    ImGui::Text("Address: %p", reinterpret_cast<void*>(selected->address));
    ImGui::Text("Placement hash: 0x%016llX", static_cast<unsigned long long>(selected->placementHash));
    ImGui::Text("Component ID: 0x%016llX", static_cast<unsigned long long>(selected->componentId));
    ImGui::Text("Stream unit: %u", static_cast<unsigned int>(selected->streamUnitId));

    if (ImGui::Button("Goto")) {
        Features::PlayerTransform player{};
        if (Features::GetPlayerTransform(player)) {
            std::copy_n(selected->transform.position, 3, player.position);
            if (Features::QueuePlayerTransform(player))
                g_instanceUiStatus[0] = '\0';
            else
                strncpy_s(g_instanceUiStatus, "Goto rejected: Lara is unavailable or a player transform cannot be queued right now.", _TRUNCATE);
        } else {
            strncpy_s(g_instanceUiStatus, "Goto rejected: Lara is not currently available.", _TRUNCATE);
        }
    }
    ImGui::SameLine();
    if (!selected->detailsReady) ImGui::BeginDisabled();
    if (ImGui::Button("Bring")) {
        Features::PlayerTransform player{};
        if (Features::GetPlayerTransform(player)) {
            Features::PlayerTransform target = selected->transform;
            std::copy_n(player.position, 3, target.position);
            if (Features::QueueInstanceTransform(selected->instanceId, selected->componentId, target)) {
                MarkInstanceTransformQueued();
                g_instanceEdit = target;
                g_instanceEditLoaded = true;
                for (auto& info : g_instanceCache) {
                    if (info.instanceId == selected->instanceId && info.componentId == selected->componentId) {
                        info.transform = target;
                        break;
                    }
                }
                g_instanceUiStatus[0] = '\0';
            } else {
                strncpy_s(g_instanceUiStatus, "Bring rejected: the Instance may have despawned or its slot changed.", _TRUNCATE);
            }
        } else {
            strncpy_s(g_instanceUiStatus, "Bring rejected: Lara is not currently available.", _TRUNCATE);
        }
    }
    if (!selected->detailsReady) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        if (Features::QueueInstanceDelete(selected->instanceId, selected->componentId))
            g_instanceUiStatus[0] = '\0';
        else
            strncpy_s(g_instanceUiStatus, "Delete rejected: the Instance may have despawned or a delete is already pending.", _TRUNCATE);
    }
    const char* deleteStatus = Features::InstanceDeleteStatus();
    if (std::strcmp(deleteStatus, "Ready") != 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", deleteStatus);
    }
    ImGui::Spacing();

    if (ImGui::BeginTabBar("instance_detail_tabs")) {
        if (ImGui::BeginTabItem("Transforms")) {
            if (!selected->detailsReady) {
                ImGui::TextDisabled("Loading selected Instance details...");
            } else {
                if (!g_instanceEditLoaded) {
                    g_instanceEdit = selected->transform;
                    g_instanceEditLoaded = true;
                }

                // Follow the Instance live (like Player Position), except a field that is being dragged.
                Features::PlayerTransform live{};
                std::uint64_t liveSequence = 0;
                if (Features::GetWatchedInstanceTransform(selected->instanceId, selected->componentId, live,
                                                          &liveSequence) &&
                    liveSequence >= g_instanceLiveAcceptSequence) {
                    if (!g_instancePositionFieldActive)
                        std::copy_n(live.position, 3, g_instanceEdit.position);
                    if (!g_instanceRotationFieldActive)
                        std::copy_n(live.rotation, 3, g_instanceEdit.rotation);
                    for (auto& info : g_instanceCache) {
                        if (info.instanceId == selected->instanceId && info.componentId == selected->componentId) {
                            info.transform = live;
                            break;
                        }
                    }
                }

                bool instanceTransformChanged = false;
                const float instanceTransformFieldWidth = std::min(UiPx(420.0f), ImGui::GetContentRegionAvail().x);
                ImGui::SetNextItemWidth(instanceTransformFieldWidth);
                instanceTransformChanged |= ImGui::DragFloat3("Position##instance_transform", g_instanceEdit.position,
                                                              1.0f, 0.0f, 0.0f, "%.5f");
                g_instancePositionFieldActive = ImGui::IsItemActive();
                ImGui::SetNextItemWidth(instanceTransformFieldWidth);
                instanceTransformChanged |= ImGui::DragFloat3("Rotation##instance_transform", g_instanceEdit.rotation,
                                                              0.0025f, -3.14159265f, 3.14159265f, "%.6f",
                                                              ImGuiSliderFlags_AlwaysClamp);
                g_instanceRotationFieldActive = ImGui::IsItemActive();

                if (instanceTransformChanged) {
                    if (Features::QueueInstanceTransform(selected->instanceId, selected->componentId, g_instanceEdit)) {
                        MarkInstanceTransformQueued();
                        g_instanceUiStatus[0] = '\0';
                        for (auto& info : g_instanceCache) {
                            if (info.instanceId == selected->instanceId && info.componentId == selected->componentId) {
                                info.transform = g_instanceEdit;
                                break;
                            }
                        }
                    } else {
                        strncpy_s(g_instanceUiStatus,
                                  "Live transform update rejected: the Instance may have despawned or its slot changed.",
                                  _TRUNCATE);
                    }
                }

                ImGui::Spacing();
                if (ImGui::Button("Restore original")) {
                    const InstanceOriginalKey originalKey{selected->instanceId, selected->componentId};
                    const auto original = g_instanceOriginalTransforms.find(originalKey);
                    if (original != g_instanceOriginalTransforms.end()) {
                        g_instanceEdit = original->second;
                        if (Features::QueueInstanceTransform(selected->instanceId, selected->componentId, g_instanceEdit)) {
                            MarkInstanceTransformQueued();
                            for (auto& info : g_instanceCache) {
                                if (info.instanceId == selected->instanceId && info.componentId == selected->componentId) {
                                    info.transform = g_instanceEdit;
                                    break;
                                }
                            }
                            g_instanceUiStatus[0] = '\0';
                        } else {
                            strncpy_s(g_instanceUiStatus,
                                      "Restore rejected: the Instance may have despawned or its slot changed.",
                                      _TRUNCATE);
                        }
                    }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Restore the transform captured when this Instance was first selected during this session.");

                const char* transformStatus = Features::InstanceTransformStatus();
                if (std::strcmp(transformStatus, "Ready") != 0)
                    ImGui::TextDisabled("%s", transformStatus);
                if (g_instanceUiStatus[0])
                    ImGui::TextDisabled("%s", g_instanceUiStatus);
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Drawgroups")) {
            if (!selected->detailsReady) {
                ImGui::TextDisabled("Loading selected Instance details...");
            } else if (!selected->hasDrawableComponent) {
                ImGui::TextDisabled("This Instance has no DrawableComponent runtime drawgroup mask.");
            } else {
                
                
                
                for (int group = 0; group < 64; ++group) {
                    const std::uint64_t bit = std::uint64_t{1} << static_cast<unsigned int>(group);
                    bool visible = (selected->noDrawGroupsMask & bit) == 0;
                    char label[24]{};
                    std::snprintf(label, sizeof(label), "%d##drawgroup_%d", group, group);
                    if (ImGui::Checkbox(label, &visible)) {
                        std::uint64_t desiredMask = selected->noDrawGroupsMask;
                        if (visible)
                            desiredMask &= ~bit;
                        else
                            desiredMask |= bit;

                        if (Features::QueueInstanceDrawGroupMask(selected->instanceId, selected->componentId, desiredMask)) {
                            
                            
                            for (auto& info : g_instanceCache) {
                                if (info.instanceId == selected->instanceId && info.componentId == selected->componentId) {
                                    info.noDrawGroupsMask = desiredMask;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Cloth")) {
            g_instanceClothTabActiveThisFrame = true;
            Features::SetClothEditorTarget(selected->instanceId, selected->componentId);
            DrawClothEditorContents();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Collisions")) {
            g_instanceCollisionTabActiveThisFrame = true;
            Features::SetCapsuleCollisionTarget(selected->instanceId, selected->componentId);
            DrawCapsuleCollisionEditorContents();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Markers")) {
            g_instanceMarkerTabActiveThisFrame = true;
            Features::SetMarkerEditorTarget(selected->instanceId, selected->componentId);
            DrawMarkerEditorContents();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Animation")) {
            DrawInstanceAnimationTab(*selected);
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::EndChild();
    ImGui::End();
}

std::string NormalizeFuzzyText(const char* text) {
    std::string out;
    if (!text) return out;
    out.reserve(std::strlen(text));
    for (const unsigned char ch : std::string(text)) {
        if (std::isalnum(ch))
            out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

int FuzzyMarkerScore(const char* text, const char* query) {
    const std::string haystack = NormalizeFuzzyText(text);
    const std::string needle = NormalizeFuzzyText(query);
    if (needle.empty()) return 0;
    if (haystack.empty()) return -1;

    
    
    const std::size_t exact = haystack.find(needle);
    if (exact != std::string::npos) {
        int score = 100000;
        if (exact == 0) score += 10000;
        if (haystack.size() == needle.size()) score += 10000;
        score -= static_cast<int>(std::min<std::size_t>(exact, 500)) * 25;
        score -= static_cast<int>(std::min<std::size_t>(haystack.size() - needle.size(), 500));
        return score;
    }

    
    
    
    int score = 0;
    int previous = -2;
    int first = -1;
    std::size_t searchFrom = 0;
    for (const char wanted : needle) {
        const std::size_t found = haystack.find(wanted, searchFrom);
        if (found == std::string::npos) return -1;
        const int current = static_cast<int>(found);
        if (first < 0) first = current;
        score += 1000;
        if (current == previous + 1)
            score += 450;
        else if (previous >= 0)
            score -= std::min(250, (current - previous - 1) * 18);
        previous = current;
        searchFrom = found + 1;
    }
    score -= std::max(0, first) * 12;
    score -= static_cast<int>(std::min<std::size_t>(haystack.size() - needle.size(), 500)) * 2;
    return score;
}

void RebuildMarkerFilter() {
    struct RankedMarker {
        int index{};
        int score{};
    };

    g_markerFiltered.clear();
    g_markerFiltered.reserve(g_markerCache.size());

    if (!g_markerSearch[0]) {
        for (int i = 0; i < static_cast<int>(g_markerCache.size()); ++i)
            g_markerFiltered.push_back(i);
        strncpy_s(g_lastMarkerSearch, g_markerSearch, _TRUNCATE);
        return;
    }

    std::vector<RankedMarker> ranked;
    ranked.reserve(g_markerCache.size());
    for (int i = 0; i < static_cast<int>(g_markerCache.size()); ++i) {
        const auto& marker = g_markerCache[static_cast<std::size_t>(i)];
        int score = FuzzyMarkerScore(marker.name, g_markerSearch);

        
        
        char idText[64];
        std::snprintf(idText, sizeof(idText), "%u %u", marker.itemId, marker.listIndex);
        if (ContainsNoCase(idText, g_markerSearch))
            score = std::max(score, 120000);

        if (score >= 0) ranked.push_back({i, score});
    }

    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const RankedMarker& a, const RankedMarker& b) {
                         return a.score > b.score;
                     });
    for (const auto& match : ranked) g_markerFiltered.push_back(match.index);
    strncpy_s(g_lastMarkerSearch, g_markerSearch, _TRUNCATE);
}

void RefreshMarkerUiCache() {
    const std::uint64_t version = Features::MarkerListVersion();
    if (version != g_markerCacheVersion) {
        const int count = Features::MarkerCount();
        g_markerCache.resize(static_cast<std::size_t>(std::max(0, count)));
        if (count > 0) {
            const int copied = Features::CopyMarkers(g_markerCache.data(), count);
            g_markerCache.resize(static_cast<std::size_t>(std::max(0, copied)));
        }
        g_markerCacheVersion = version;
        RebuildMarkerFilter();
    } else if (std::strcmp(g_markerSearch, g_lastMarkerSearch) != 0) {
        RebuildMarkerFilter();
    }
}

const char* MaterialStageLabel(int stage) {
    switch (stage) {
        case Features::MaterialShaderStage_Pixel: return "Pixel";
        case Features::MaterialShaderStage_Vertex: return "Vertex";
        case Features::MaterialShaderStage_Hull: return "Hull";
        case Features::MaterialShaderStage_Domain: return "Domain";
        default: return "Unknown";
    }
}

// Materials window, Save button (v0.9.307).
bool g_materialSavePending = false;
std::string g_materialSaveStatus;
std::uint64_t g_materialSaveDialogVersion = 0;

const char* MaterialLeafName(const char* path) {
    if (!path || !path[0]) return "<unnamed>";
    const char* slash = std::strrchr(path, '/');
    const char* backslash = std::strrchr(path, '\\');
    const char* last = nullptr;
    if (slash && backslash)
        last = slash > backslash ? slash : backslash;
    else
        last = slash ? slash : backslash;
    return (last && last[1]) ? last + 1 : path;
}

// The Material Editor (v0.9.335 overhaul): the list keeps itself current (no Refresh), can be limited to Lara's or
// the selected Instance's materials, and the constants are shown by what they do (MaterialNames: the Modding Suite's
// analysis of the pixel shaders), with every raw float4 still under "All constants".
int g_materialScope = 0;                              // 0 all loaded, 1 Lara, 2 the selected Instance
std::uint64_t g_materialScopeCacheVersion = ~std::uint64_t{0};
std::vector<std::uintptr_t> g_materialScopeCache;     // sorted
char g_materialParamFilter[96]{};
MaterialNames::Result g_materialNames;
std::string g_materialNamesSignature;                 // what was asked for: the material and its shaders / slots
ULONGLONG g_materialDetailsReadMs = 0;                // the selected material's details were last asked for
std::unordered_map<std::uint64_t, std::size_t> g_materialPixelConstantIndex;   // (pass, register) -> constant cache row

void SelectMaterial(std::uintptr_t address) {
    g_selectedMaterialAddress = address;
    g_materialDetailsCache = {};
    g_materialTextureCache.clear();
    g_materialConstantCache.clear();
    g_materialPixelConstantIndex.clear();
    g_materialDetailVersion = ~std::uint64_t{0};
    g_materialNames = {};
    g_materialNamesSignature.clear();
    g_materialDetailsReadMs = GetTickCount64();
    if (address) Features::RequestMaterialDetails(address);
}

bool MaterialInScope(std::uintptr_t address) {
    return g_materialScope == 0 || std::binary_search(g_materialScopeCache.begin(), g_materialScopeCache.end(), address);
}

void RebuildMaterialFilter() {
    struct RankedMaterial { int index{}; int score{}; };
    g_materialFiltered.clear();
    g_materialFiltered.reserve(g_materialCache.size());
    if (!g_materialSearch[0]) {
        for (int i = 0; i < static_cast<int>(g_materialCache.size()); ++i)
            if (MaterialInScope(g_materialCache[static_cast<std::size_t>(i)].address)) g_materialFiltered.push_back(i);
        strncpy_s(g_lastMaterialSearch, g_materialSearch, _TRUNCATE);
        return;
    }

    std::vector<RankedMaterial> ranked;
    ranked.reserve(g_materialCache.size());
    for (int i = 0; i < static_cast<int>(g_materialCache.size()); ++i) {
        const auto& mat = g_materialCache[static_cast<std::size_t>(i)];
        if (!MaterialInScope(mat.address)) continue;
        int score = std::max({FuzzyMarkerScore(MaterialLeafName(mat.resourceName), g_materialSearch),
                              FuzzyMarkerScore(mat.resourceName, g_materialSearch),
                              FuzzyMarkerScore(mat.materialTypeName, g_materialSearch)});
        char idText[96]{};
        std::snprintf(idText, sizeof(idText), "%d 0x%08X %llX", mat.id,
                      static_cast<unsigned int>(mat.id),
                      static_cast<unsigned long long>(mat.address));
        if (ContainsNoCase(idText, g_materialSearch)) score = std::max(score, 125000);
        if (score >= 0) ranked.push_back({i, score});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const RankedMaterial& a, const RankedMaterial& b) {
        return a.score > b.score;
    });
    for (const auto& m : ranked) g_materialFiltered.push_back(m.index);
    strncpy_s(g_lastMaterialSearch, g_materialSearch, _TRUNCATE);
}

// What the names were made from: when it is unchanged, a newer read of the same material keeps its names.
std::string MaterialNamesSignature(const Features::MaterialAnalysisInput& input) {
    std::string signature = std::to_string(input.address);
    for (int p = 0; p < 10; ++p) {
        const auto& pass = input.passes[p];
        if (!pass.present) continue;
        signature += "|" + std::to_string(p) + ":" + std::to_string(pass.shader) + ":" + std::to_string(pass.constants.size());
        for (const auto& slot : pass.textures) signature += "," + std::to_string(slot.texture) + "/" + std::to_string(slot.reg);
    }
    return signature;
}

void RefreshMaterialUiCache() {
    const std::uint64_t scopeVersion = Features::MaterialListScopeVersion();
    bool refilter = false;
    if (scopeVersion != g_materialScopeCacheVersion) {
        g_materialScopeCacheVersion = scopeVersion;
        g_materialScopeCache = Features::MaterialListScope();
        refilter = true;
    }
    const std::uint64_t version = Features::MaterialListVersion();
    if (version != g_materialCacheVersion) {
        const int count = Features::MaterialCount();
        g_materialCache.resize(static_cast<std::size_t>(std::max(0, count)));
        if (count > 0) {
            const int copied = Features::CopyMaterials(g_materialCache.data(), count);
            g_materialCache.resize(static_cast<std::size_t>(std::max(0, copied)));
        }
        g_materialCacheVersion = version;
        refilter = true;

        const int texCount = Features::KnownTextureCount();
        g_materialKnownTextureCache.resize(static_cast<std::size_t>(std::max(0, texCount)));
        if (texCount > 0) {
            const int copied = Features::CopyKnownTextures(g_materialKnownTextureCache.data(), texCount);
            g_materialKnownTextureCache.resize(static_cast<std::size_t>(std::max(0, copied)));
        }

        if (g_selectedMaterialAddress) {
            const auto it = std::find_if(g_materialCache.begin(), g_materialCache.end(), [](const auto& info) {
                return info.address == g_selectedMaterialAddress;
            });
            if (it == g_materialCache.end()) SelectMaterial(0);   // (unloaded)
        }
    }
    if (refilter || std::strcmp(g_materialSearch, g_lastMaterialSearch) != 0) RebuildMaterialFilter();

    // The selected material is read again every 3 s while nothing is being edited (other edits, scripts, the game).
    const ULONGLONG now = GetTickCount64();
    if (g_selectedMaterialAddress && !ImGui::IsAnyItemActive() && now - g_materialDetailsReadMs >= 3000) {
        g_materialDetailsReadMs = now;
        Features::RequestMaterialDetails(g_selectedMaterialAddress);
    }

    const std::uint64_t detailVersion = Features::MaterialDetailVersion();
    if (g_selectedMaterialAddress && detailVersion != g_materialDetailVersion && !ImGui::IsAnyItemActive()) {
        Features::MaterialDetails details{};
        if (Features::GetMaterialDetails(details) && details.address == g_selectedMaterialAddress) {
            g_materialDetailsCache = details;
            const int texCount = Features::MaterialTextureCount();
            g_materialTextureCache.resize(static_cast<std::size_t>(std::max(0, texCount)));
            if (texCount > 0) {
                const int copied = Features::CopyMaterialTextures(g_materialTextureCache.data(), texCount);
                g_materialTextureCache.resize(static_cast<std::size_t>(std::max(0, copied)));
            }
            const int constantCount = Features::MaterialConstantCount();
            g_materialConstantCache.resize(static_cast<std::size_t>(std::max(0, constantCount)));
            if (constantCount > 0) {
                const int copied = Features::CopyMaterialConstants(g_materialConstantCache.data(), constantCount);
                g_materialConstantCache.resize(static_cast<std::size_t>(std::max(0, copied)));
            }
            g_materialPixelConstantIndex.clear();
            for (std::size_t i = 0; i < g_materialConstantCache.size(); ++i) {
                const auto& c = g_materialConstantCache[i];
                if (c.stage == Features::MaterialShaderStage_Pixel)
                    g_materialPixelConstantIndex[(static_cast<std::uint64_t>(c.passIndex) << 32) | c.index] = i;
            }
            g_materialDetailVersion = detailVersion;

            // names: asked for again only when the material's shaders or slots changed
            Features::MaterialAnalysisInput input;
            if (Features::CopyMaterialAnalysisInput(input) && input.address == g_selectedMaterialAddress) {
                std::string signature = MaterialNamesSignature(input);
                if (signature != g_materialNamesSignature) {
                    g_materialNamesSignature = std::move(signature);
                    MaterialNames::Request(input);
                }
            }
        }
    }
    MaterialNames::Result names;
    if (MaterialNames::Poll(names) && names.address == g_selectedMaterialAddress &&
        (names.address != g_materialNames.address || names.detailVersion != g_materialNames.detailVersion))
        g_materialNames = std::move(names);
}

void MaterialTooltip(const std::string& text) {
    if (text.empty() || !ImGui::BeginItemTooltip()) return;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void DrawMaterialTextureEditor() {
    if (g_materialTextureCache.empty()) {
        ImGui::TextDisabled("No material texture entries were found in the active passes.");
        return;
    }
    ImGui::TextDisabled("Texture choices are the textures of the materials opened in this editor.");
    ImGui::Separator();

    int previousPass = -1;
    int previousStage = -1;
    for (std::size_t i = 0; i < g_materialTextureCache.size(); ++i) {
        auto& texture = g_materialTextureCache[i];
        if (texture.passIndex != previousPass || texture.stage != previousStage) {
            if (i) ImGui::Spacing();
            ImGui::SeparatorText((std::string(MaterialNames::PassName(texture.passIndex)) + " / " +
                                  MaterialStageLabel(texture.stage) + " shader##" + std::to_string(i)).c_str());
            previousPass = texture.passIndex;
            previousStage = texture.stage;
        }

        ImGui::PushID(static_cast<int>(i));
        const char* currentName = texture.textureAddress ? texture.resourceName : "<none>";
        // what the slot is for, measured from the material's shaders (MaterialNames: the Suite's TextureRoles)
        const MaterialNames::SlotRole* role = nullptr;
        if (texture.stage == Features::MaterialShaderStage_Pixel && g_materialNames.address == g_selectedMaterialAddress)
            for (const auto& r : g_materialNames.slotRoles)
                if (r.pass == static_cast<int>(texture.passIndex) && r.reg == static_cast<int>(texture.textureSlot)) {
                    role = &r;
                    break;
                }
        ImGui::AlignTextToFramePadding();
        if (role) {
            if (role->measured) ImGui::TextUnformatted(role->role.c_str());
            else ImGui::TextDisabled("%s", role->role.c_str());
            MaterialTooltip((role->measured ? "Measured from the material's shaders: " : "From the texture's format: ") + role->why);
            ImGui::SameLine();
        }
        ImGui::TextDisabled("t%u%s%s", texture.textureSlot, texture.width > 0 ? "  " : "",
                            texture.width > 0 ? (std::to_string(texture.width) + " x " + std::to_string(texture.height)).c_str() : "");
        if (!role) ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        if (!texture.writable) ImGui::BeginDisabled();
        if (ImGui::BeginCombo("##texture", currentName && currentName[0] ? currentName : "<unnamed>")) {
            const bool noneSelected = texture.textureAddress == 0;
            if (ImGui::Selectable("<none>", noneSelected)) {
                if (Features::QueueMaterialTextureUpdate(g_selectedMaterialAddress, texture.passIndex,
                                                         texture.stage, texture.entryIndex, 0)) {
                    texture.textureAddress = 0;
                    texture.resourceId = 0;
                    texture.hasResourceId = false;
                    texture.width = texture.height = 0;
                    strncpy_s(texture.resourceName, "<none>", _TRUNCATE);
                }
            }
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(g_materialKnownTextureCache.size()));
            while (clipper.Step()) {
                for (int n = clipper.DisplayStart; n < clipper.DisplayEnd; ++n) {
                    const auto& candidate = g_materialKnownTextureCache[static_cast<std::size_t>(n)];
                    const bool selected = texture.textureAddress == candidate.address;
                    char label[256]{};
                    std::snprintf(label, sizeof(label), "%s  (%dx%d)##tex_%llX",
                                  candidate.resourceName[0] ? candidate.resourceName : "texture_unresolved",
                                  candidate.width, candidate.height,
                                  static_cast<unsigned long long>(candidate.address));
                    if (ImGui::Selectable(label, selected)) {
                        if (Features::QueueMaterialTextureUpdate(g_selectedMaterialAddress, texture.passIndex,
                                                                 texture.stage, texture.entryIndex,
                                                                 candidate.address)) {
                            texture.textureAddress = candidate.address;
                            texture.resourceId = candidate.resourceId;
                            texture.hasResourceId = candidate.hasResourceId;
                            texture.width = candidate.width;
                            texture.height = candidate.height;
                            strncpy_s(texture.resourceName, candidate.resourceName, _TRUNCATE);
                        }
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        if (!texture.writable) ImGui::EndDisabled();

        if (texture.textureAddress) {
            const float maxSide = UiPx(160.0f);
            float drawW = maxSide;
            float drawH = maxSide;
            if (texture.width > 0 && texture.height > 0) {
                if (texture.width >= texture.height)
                    drawH = std::max(UiPx(24.0f), maxSide * static_cast<float>(texture.height) / static_cast<float>(texture.width));
                else
                    drawW = std::max(UiPx(24.0f), maxSide * static_cast<float>(texture.width) / static_cast<float>(texture.height));
            }
            const ImVec2 previewSize(drawW, drawH);
            if (ImGui::IsRectVisible(previewSize)) {
                std::uint64_t previewId = 0;
                if (Dx12Overlay::GetTexturePreviewId(texture.textureAddress, &previewId) && previewId)
                    ImGui::Image(ImTextureRef(static_cast<ImTextureID>(previewId)), previewSize);
                else
                    ImGui::Dummy(previewSize);
            } else {
                ImGui::Dummy(previewSize);
            }
        }
        ImGui::PopID();
    }
}

// A named parameter's value in the constant cache (null: not there).
Features::MaterialConstantInfo* MaterialPixelConstant(int pass, int index, int& component) {
    component = index & 3;
    const auto it = g_materialPixelConstantIndex.find((static_cast<std::uint64_t>(pass) << 32) |
                                                      static_cast<std::uint32_t>(index >> 2));
    if (it == g_materialPixelConstantIndex.end() || it->second >= g_materialConstantCache.size()) return nullptr;
    return &g_materialConstantCache[it->second];
}

float MaterialParamValue(const MaterialNames::Param& p, int c, bool* mixed = nullptr, bool* writable = nullptr) {
    float first = 0.0f;
    bool have = false;
    if (mixed) *mixed = false;
    if (writable) *writable = !p.where[c].empty();
    for (const auto& l : p.where[c]) {
        int component = 0;
        auto* constant = MaterialPixelConstant(l.pass, l.index, component);
        if (!constant) {
            if (writable) *writable = false;
            continue;
        }
        if (writable && !constant->writable) *writable = false;
        const float v = constant->values[component];
        if (!have) {
            first = v;
            have = true;
        } else if (mixed && std::fabs(v - first) > 1e-6f) {
            *mixed = true;
        }
    }
    return first;
}

// Writes a component everywhere it is stored (finalize: the drag ended).
void SetMaterialParamValue(const MaterialNames::Param& p, int c, float value, bool finalize) {
    for (const auto& l : p.where[c]) {
        int component = 0;
        auto* constant = MaterialPixelConstant(l.pass, l.index, component);
        if (!constant) continue;
        constant->values[component] = value;
        Features::QueueMaterialConstantUpdate(g_selectedMaterialAddress, l.pass, Features::MaterialShaderStage_Pixel,
                                              static_cast<std::uint32_t>(l.index), value, finalize);
    }
}

void DrawMaterialParam(const MaterialNames::Param& p, int id) {
    using MaterialNames::Widget;
    ImGui::PushID(id);
    float v[4]{};
    bool mixed = false, writable = true;
    for (int c = 0; c < p.components; ++c) {
        bool m = false, w = true;
        v[c] = MaterialParamValue(p, c, &m, &w);
        mixed = mixed || m;
        if (!p.where[c].empty()) writable = writable && w;
    }
    const float width = ImGui::GetContentRegionAvail().x * 0.6f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(p.label.c_str());
    std::string tip = p.help.empty() ? p.note : p.help + "\n\n" + p.note;
    if (!p.slotName.empty()) tip += "\nshader texture: " + p.slotName;
    if (mixed) tip += "\n(the passes hold different values: editing sets them all)";
    MaterialTooltip(tip);
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - width));
    ImGui::SetNextItemWidth(width);
    if (!writable) ImGui::BeginDisabled();

    // one ImGui item edits component c; its value is sent while dragging and once more when it is let go
    auto commit = [&](int c, bool changed) {
        if (changed) SetMaterialParamValue(p, c, v[c], false);
        if (ImGui::IsItemDeactivatedAfterEdit()) SetMaterialParamValue(p, c, v[c], true);
    };
    switch (p.widget) {
        case Widget::Color: {
            const float before[3] = {v[0], v[1], v[2]};
            const bool changed = ImGui::ColorEdit3("##v", v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
            const bool done = ImGui::IsItemDeactivatedAfterEdit();
            for (int c = 0; c < 3; ++c) {
                if (p.where[c].empty()) continue;
                if (changed && v[c] != before[c]) SetMaterialParamValue(p, c, v[c], false);
                if (done) SetMaterialParamValue(p, c, v[c], true);
            }
            break;
        }
        case Widget::Pair: {
            if (!p.where[0].empty() && !p.where[1].empty()) {
                const float before[2] = {v[0], v[1]};
                const bool changed = ImGui::DragFloat2("##v", v, 0.01f, 0.0f, 0.0f, "%.4g");
                const bool done = ImGui::IsItemDeactivatedAfterEdit();
                for (int c = 0; c < 2; ++c) {
                    if (changed && v[c] != before[c]) SetMaterialParamValue(p, c, v[c], false);
                    if (done) SetMaterialParamValue(p, c, v[c], true);
                }
            } else {
                const int c = p.where[0].empty() ? 1 : 0;
                commit(c, ImGui::DragFloat("##v", &v[c], 0.01f, 0.0f, 0.0f, c ? "v %.4g" : "u %.4g"));
            }
            break;
        }
        case Widget::Distance: {   // the squared distance in centimetres, shown in metres
            float metres[2];
            for (int c = 0; c < 2; ++c) metres[c] = std::sqrt(std::max(0.0f, v[c])) / 100.0f;
            const float each = (width - ImGui::GetStyle().ItemInnerSpacing.x) * 0.5f;
            for (int c = 0; c < 2; ++c) {
                if (c) ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
                ImGui::PushID(c);
                ImGui::SetNextItemWidth(each);
                const bool changed = ImGui::DragFloat("##m", &metres[c], 0.01f, 0.0f, 1000.0f, c ? "to %.2f m" : "from %.2f m",
                                                      ImGuiSliderFlags_AlwaysClamp);
                v[c] = (metres[c] * 100.0f) * (metres[c] * 100.0f);
                if (!p.where[c].empty()) commit(c, changed);
                ImGui::PopID();
            }
            break;
        }
        case Widget::Highlights: {
            static const char* const names[4] = {"primary strength", "primary sharpness", "secondary strength",
                                                 "secondary sharpness"};
            ImGui::NewLine();
            for (int c = 0; c < 4; ++c) {
                if (p.where[c].empty()) continue;
                ImGui::PushID(c);
                ImGui::TextDisabled("  %s", names[c]);
                ImGui::SameLine();
                ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - width * 0.6f));
                ImGui::SetNextItemWidth(width * 0.6f);
                commit(c, ImGui::DragFloat("##c", &v[c], c % 2 ? 0.5f : 0.005f, 0.0f, 0.0f, "%.4g"));
                ImGui::PopID();
            }
            break;
        }
        case Widget::Values:
        default: {
            int used = 0;
            for (int c = 0; c < p.components; ++c) used += p.where[c].empty() ? 0 : 1;
            const float each = (width - ImGui::GetStyle().ItemInnerSpacing.x * static_cast<float>(std::max(0, used - 1))) /
                               static_cast<float>(std::max(1, used));
            bool first = true;
            for (int c = 0; c < p.components; ++c) {
                if (p.where[c].empty()) continue;
                if (!first) ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
                first = false;
                ImGui::PushID(c);
                ImGui::SetNextItemWidth(each);
                char format[48];
                if (!p.componentNames[c].empty())
                    std::snprintf(format, sizeof(format), "%s %%.4g",
                                  p.componentNames[c] == " " ? "" : p.componentNames[c].c_str());
                else if (p.plainNumbers || used == 1)
                    std::snprintf(format, sizeof(format), "%%.4g");
                else
                    std::snprintf(format, sizeof(format), "%c %%.4g", "RGBA"[c]);
                commit(c, ImGui::DragFloat("##c", &v[c], 0.005f, 0.0f, 0.0f, format));
                ImGui::PopID();
            }
            break;
        }
    }
    if (!writable) ImGui::EndDisabled();
    if (mixed) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "*");
    }
    ImGui::PopID();
}

void DrawMaterialParameterEditor() {
    const auto& names = g_materialNames;
    if (names.address != g_selectedMaterialAddress || !names.done) {
        ImGui::TextDisabled("Reading what this material's values do from its shaders...");
        return;
    }
    if (names.params.empty()) {
        ImGui::TextDisabled("%s", names.problem.empty()
                                      ? "No named parameters were found in this material's shaders."
                                      : names.problem.c_str());
        ImGui::TextDisabled("Its values are under All constants.");
        return;
    }
    ImGui::SetNextItemWidth(UiPx(260.0f));
    ImGui::InputTextWithHint("##material_param_filter", "Filter parameters...", g_materialParamFilter,
                             sizeof(g_materialParamFilter));
    ImGui::SameLine();
    ImGui::TextDisabled("%u parameters from %d pixel shader%s", static_cast<unsigned>(names.params.size()), names.shaders,
                        names.shaders == 1 ? "" : "s");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Read from the material's pixel shaders (the Modding Suite's analysis). A value several passes\n"
                          "use is one entry: editing it sets them all. Every raw value is under All constants.");
    ImGui::BeginChild("##material_params", ImVec2(0.0f, 0.0f), false);
    std::string group;
    bool open = true;
    for (std::size_t i = 0; i < names.params.size(); ++i) {
        const auto& p = names.params[i];
        if (g_materialParamFilter[0] && !ContainsNoCase(p.label.c_str(), g_materialParamFilter) &&
            !ContainsNoCase(p.group.c_str(), g_materialParamFilter))
            continue;
        if (p.group != group || i == 0) {
            group = p.group;
            open = ImGui::CollapsingHeader((group + "##group" + std::to_string(i)).c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        }
        if (open) DrawMaterialParam(p, static_cast<int>(i));
    }
    ImGui::EndChild();
}

void DrawMaterialConstantEditor() {
    if (g_materialConstantCache.empty()) {
        ImGui::TextDisabled("No raw material constants were found in the active passes.");
        return;
    }
    ImGui::TextDisabled("Every constant register (float4) of every pass and shader stage, as the game stores them.");
    ImGui::TextDisabled("Changes apply live and affect every user of this material resource.");
    ImGui::Separator();

    const float constantComponentWidth = UiPx(112.0f);
    const float constantLabelWidth = UiPx(92.0f);

    int previousPass = -1;
    int previousStage = -1;
    bool tableOpen = false;

    auto endConstantTable = [&]() {
        if (tableOpen) {
            ImGui::EndTable();
            tableOpen = false;
        }
    };

    auto beginConstantTable = [&](int passIndex, int stage) {
        char tableId[64]{};
        std::snprintf(tableId, sizeof(tableId), "##material_constants_p%d_s%d", passIndex, stage);
        const ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit |
                                      ImGuiTableFlags_NoSavedSettings |
                                      ImGuiTableFlags_PadOuterX;
        tableOpen = ImGui::BeginTable(tableId, 5, flags);
        if (!tableOpen) return;
        ImGui::TableSetupColumn("Register", ImGuiTableColumnFlags_WidthFixed, constantLabelWidth);
        ImGui::TableSetupColumn("X", ImGuiTableColumnFlags_WidthFixed, constantComponentWidth);
        ImGui::TableSetupColumn("Y", ImGuiTableColumnFlags_WidthFixed, constantComponentWidth);
        ImGui::TableSetupColumn("Z", ImGuiTableColumnFlags_WidthFixed, constantComponentWidth);
        ImGui::TableSetupColumn("W", ImGuiTableColumnFlags_WidthFixed, constantComponentWidth);
        ImGui::TableHeadersRow();
    };

    auto drawConstantRow = [&](std::size_t i) {
        auto& constant = g_materialConstantCache[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("[%u]", constant.index);

        for (int component = 0; component < 4; ++component) {
            ImGui::TableSetColumnIndex(component + 1);
            ImGui::PushID(component);
            ImGui::SetNextItemWidth(constantComponentWidth - UiPx(8.0f));
            if (!constant.writable) ImGui::BeginDisabled();

            float value = constant.values[component];
            const bool valueChanged = ImGui::DragFloat("##value", &value, 0.01f, 0.0f, 0.0f, "%.6g");
            const std::uint32_t scalarIndex = constant.index * 4u + static_cast<std::uint32_t>(component);
            if (valueChanged) {
                constant.values[component] = value;
                Features::QueueMaterialConstantUpdate(g_selectedMaterialAddress, constant.passIndex,
                                                      constant.stage, scalarIndex, value, false);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                Features::QueueMaterialConstantUpdate(g_selectedMaterialAddress, constant.passIndex,
                                                      constant.stage, scalarIndex,
                                                      constant.values[component], true);
            }
            if (!constant.writable) ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::PopID();
    };

    const std::size_t constantTotal = g_materialConstantCache.size();
    std::size_t groupStart = 0;
    while (groupStart < constantTotal) {
        previousPass = g_materialConstantCache[groupStart].passIndex;
        previousStage = g_materialConstantCache[groupStart].stage;
        std::size_t groupEnd = groupStart;
        while (groupEnd < constantTotal && g_materialConstantCache[groupEnd].passIndex == previousPass &&
               g_materialConstantCache[groupEnd].stage == previousStage)
            ++groupEnd;

        if (groupStart) ImGui::Spacing();
        ImGui::Text("%s / %s shader", MaterialNames::PassName(previousPass), MaterialStageLabel(previousStage));
        beginConstantTable(previousPass, previousStage);
        if (tableOpen) {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(groupEnd - groupStart));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                    drawConstantRow(groupStart + static_cast<std::size_t>(row));
            }
            endConstantTable();
        }
        groupStart = groupEnd;
    }
}

void DrawMaterialEditorWindow() {
    if (!g_showMaterialEditor) return;
    ImGui::SetNextWindowSize(UiSize(1040.0f, 720.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Material Editor", &g_showMaterialEditor)) {
        ImGui::End();
        return;
    }
    if (!Features::MaterialEditorAvailable()) {
        ImGui::TextDisabled("Rendered-material editing is unavailable for this executable build.");
        ImGui::End();
        return;
    }

    Features::SetMaterialListScope(g_materialScope);
    RefreshMaterialUiCache();

    // the list: what it shows, the search
    ImGui::SetNextItemWidth(UiPx(150.0f));
    static const char* const kScopes[] = {"All loaded", "Lara", "Selected Instance"};
    if (ImGui::Combo("##material_scope", &g_materialScope, kScopes, IM_ARRAYSIZE(kScopes))) {
        Features::SetMaterialListScope(g_materialScope);
        RebuildMaterialFilter();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("All loaded: every material the game has loaded.\nLara: the materials of Lara's model.\n"
                          "Selected Instance: those of the Instance selected in the Instances window.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(UiPx(360.0f));
    if (ImGui::InputTextWithHint("##material_search", "Search materials by name, type or ID...",
                                 g_materialSearch, sizeof(g_materialSearch)))
        RebuildMaterialFilter();
    ImGui::SameLine();
    ImGui::TextDisabled("%d of %d | %s", static_cast<int>(g_materialFiltered.size()),
                        static_cast<int>(g_materialCache.size()), Features::MaterialEditorStatus());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The list follows the game by itself: materials loaded or unloaded show up within a second or so.");
    ImGui::SameLine();
    ImGui::Checkbox("Highlight hovered", &g_materialHoverHighlight);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Tints the material under the mouse in the list bright green (on the GPU only;\n"
                          "the material's values are not changed).");
    ImGui::Separator();

    std::uintptr_t hoveredMaterialAddress = 0;
    ImGui::BeginChild("##material_list", UiSize(330.0f, 0.0f), true);
    if (g_materialFiltered.empty()) {
        if (g_materialCache.empty())
            ImGui::TextDisabled("Building the material list...");
        else if (g_materialScope != 0 && g_materialScopeCache.empty())
            ImGui::TextDisabled(g_materialScope == 1 ? "Lara's materials are not available right now."
                                                     : "Select an Instance in the Instances window.");
        else
            ImGui::TextDisabled("No materials match the search.");
    } else {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(g_materialFiltered.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const int idx = g_materialFiltered[static_cast<std::size_t>(row)];
                if (idx < 0 || idx >= static_cast<int>(g_materialCache.size())) continue;
                const auto& mat = g_materialCache[static_cast<std::size_t>(idx)];
                char label[320]{};
                const bool rowHidden =
                    mat.id > 0 && Features::IsMaterialResourceHidden(static_cast<std::uint32_t>(mat.id));
                std::snprintf(label, sizeof(label), "%s%s##mat_%llX", MaterialLeafName(mat.resourceName),
                              rowHidden ? "  [hidden]" : "", static_cast<unsigned long long>(mat.address));
                const bool selected = g_selectedMaterialAddress == mat.address;
                const float rowStart = ImGui::GetCursorPosX();
                const float rowWidth = ImGui::GetContentRegionAvail().x;
                if (ImGui::Selectable(label, selected)) SelectMaterial(mat.address);
                if (ImGui::IsItemHovered()) {
                    hoveredMaterialAddress = mat.address;
                    ImGui::SetTooltip("ID %d\n%s\nType: %s\nPasses %u, textures %u, constant registers %u",
                                      mat.id, mat.resourceName,
                                      mat.materialTypeName[0] ? mat.materialTypeName : "<unknown>",
                                      mat.passCount, mat.textureCount, mat.constantCount);
                }
                char idText[16];
                std::snprintf(idText, sizeof(idText), "%d", mat.id);
                ImGui::SameLine(rowStart + std::max(0.0f, rowWidth - ImGui::CalcTextSize(idText).x));
                ImGui::TextDisabled("%s", idText);
            }
        }
    }
    ImGui::EndChild();

    Features::SetMaterialHoverHighlight(g_materialHoverHighlight ? hoveredMaterialAddress : 0);

    ImGui::SameLine();

    ImGui::BeginChild("##material_details", ImVec2(0.0f, 0.0f), false);
    if (!g_selectedMaterialAddress) {
        ImGui::TextDisabled("Select a material from the list.");
        ImGui::EndChild();
        ImGui::End();
        return;
    }
    if (g_materialDetailsCache.address != g_selectedMaterialAddress) {
        ImGui::TextDisabled("Reading the material...");
        ImGui::EndChild();
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted(MaterialLeafName(g_materialDetailsCache.resourceName));
    ImGui::SameLine();
    ImGui::TextDisabled("ID %d | %s", g_materialDetailsCache.id,
                        g_materialDetailsCache.materialTypeName[0] ? g_materialDetailsCache.materialTypeName : "<unknown type>");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\nAddress 0x%llX\nPasses %u, textures %u, constant registers %u\nFlags 0x%08X, blend mode 0x%X",
                          g_materialDetailsCache.resourceName,
                          static_cast<unsigned long long>(g_materialDetailsCache.address), g_materialDetailsCache.passCount,
                          g_materialDetailsCache.textureCount, g_materialDetailsCache.constantCount,
                          g_materialDetailsCache.flags, g_materialDetailsCache.blendMode);

    if (ImGui::Button("Restore"))
        Features::RequestMaterialRestore(g_selectedMaterialAddress);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Put back the constants and textures changed in this session.");

    // Hide / Show: the same resource-ID hide scripts use (rottr.material.set_hidden).
    ImGui::SameLine();
    const int selectedMaterialId = g_materialDetailsCache.id;
    const bool visibilityAvailable = Features::MaterialResourceVisibilityAvailable() && selectedMaterialId > 0;
    const bool materialHidden =
        visibilityAvailable && Features::IsMaterialResourceHidden(static_cast<std::uint32_t>(selectedMaterialId));
    if (!visibilityAvailable) ImGui::BeginDisabled();
    if (ImGui::Button(materialHidden ? "Show##material_visibility" : "Hide##material_visibility"))
        Features::SetMaterialResourceHidden(static_cast<std::uint32_t>(selectedMaterialId), !materialHidden);
    if (!visibilityAvailable) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (!visibilityAvailable)
            ImGui::SetTooltip("Material visibility is unavailable for this material or executable build.");
        else if (materialHidden)
            ImGui::SetTooltip("Material %d is hidden on every model that uses it.", selectedMaterialId);
        else
            ImGui::SetTooltip("Hide material %d on every model that uses it.\nIt stays hidden through deaths, checkpoint "
                              "reloads and outfit changes until shown again\n(or until the game is restarted).",
                              selectedMaterialId);
    }

    // Save: the material's .tr10material with this session's texture and constant changes.
    ImGui::SameLine();
    const bool saveAvailable = Features::MaterialSaveAvailable();
    const bool saveBusy = g_materialSavePending || SaveDialog::Busy();
    if (!saveAvailable || saveBusy) ImGui::BeginDisabled();
    if (ImGui::Button("Save...##material_save")) {
        if (Features::RequestMaterialSave(g_selectedMaterialAddress)) {
            g_materialSavePending = true;
            g_materialSaveStatus = "Preparing the material file...";
        } else {
            g_materialSaveStatus = "Save failed: the request was rejected.";
        }
    }
    if (!saveAvailable || saveBusy) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(saveAvailable
                              ? "Save this material as a .tr10material file (the resource as the game loaded it,\n"
                                "with the texture assignments and constants changed in this session)."
                              : "Material saving is unavailable for this executable build.");

    Features::MaterialSaveResult saveResult{};
    if (g_materialSavePending && Features::TakeMaterialSaveResult(saveResult)) {
        g_materialSavePending = false;
        if (!saveResult.ok) {
            g_materialSaveStatus = "Save failed: " + saveResult.message;
        } else {
            const std::string fileName = saveResult.fileName;
            g_materialSaveStatus = SaveDialog::Start(std::move(saveResult.bytes), fileName, L"TR10 material",
                                                     L"tr10material")
                                       ? "Choose where to save " + fileName + "..."
                                       : std::string("Save failed: a save dialog is already open.");
        }
    }
    std::uint64_t dialogStatusVersion = 0;
    std::string dialogStatus = SaveDialog::Status(&dialogStatusVersion);
    if (dialogStatusVersion != g_materialSaveDialogVersion) {
        g_materialSaveDialogVersion = dialogStatusVersion;
        if (!dialogStatus.empty()) g_materialSaveStatus = std::move(dialogStatus);
    }
    if (!g_materialSaveStatus.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", g_materialSaveStatus.c_str());
    }

    ImGui::Separator();

    if (ImGui::BeginTabBar("##material_tabs")) {
        if (ImGui::BeginTabItem("Parameters")) {
            DrawMaterialParameterEditor();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Textures")) {
            DrawMaterialTextureEditor();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("All constants")) {
            DrawMaterialConstantEditor();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::End();
}

void DrawMarkerEditorContents() {
    if (!Features::MarkerControlsAvailable()) {
        ImGui::TextDisabled("COLLISIONMARKER editing is unavailable for this executable build.");
        return;
    }

    RefreshMarkerUiCache();

    
    
    
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##marker_search", "Fuzzy search markers by name or ID...",
                                 g_markerSearch, sizeof(g_markerSearch)))
        RebuildMarkerFilter();

    const char* markerState = Features::MarkerStatus();
    if (g_markerSearch[0])
        ImGui::TextDisabled("%d shown / %d COLLISIONMARKER%s on selected Instance",
                            static_cast<int>(g_markerFiltered.size()),
                            static_cast<int>(g_markerCache.size()), g_markerCache.size() == 1 ? "" : "s");
    else
        ImGui::TextDisabled("%d COLLISIONMARKER%s on selected Instance",
                            static_cast<int>(g_markerCache.size()), g_markerCache.size() == 1 ? "" : "s");
    ImGui::Separator();

    if (g_markerCache.empty()) {
        if (std::strcmp(markerState, "Loading markers...") == 0)
            ImGui::TextDisabled("Loading collision markers...");
        else if (std::strcmp(markerState, "Ready") != 0)
            ImGui::TextDisabled("%s", markerState);
        else
            ImGui::TextDisabled("No collision markers are currently available.");
        return;
    }
    if (g_markerFiltered.empty()) {
        ImGui::TextDisabled("No markers match \"%s\".", g_markerSearch);
        return;
    }

    for (const int markerIndex : g_markerFiltered) {
        if (markerIndex < 0 || markerIndex >= static_cast<int>(g_markerCache.size())) continue;
        const auto& marker = g_markerCache[static_cast<std::size_t>(markerIndex)];
        const std::uint64_t key = (static_cast<std::uint64_t>(marker.itemId) << 32) | marker.listIndex;
        char header[160];
        std::snprintf(header, sizeof(header), "%s##marker_%u_%u", marker.name, marker.listIndex, marker.itemId);
        if (!ImGui::CollapsingHeader(header)) continue;

        auto& edit = g_markerEdits[key];
        if (!edit.initialized) {
            std::memcpy(edit.matrix, marker.matrix, sizeof(edit.matrix));
            edit.boneSegment = marker.boneSegment;
            edit.initialized = true;
        }

        ImGui::PushID(static_cast<int>(marker.listIndex));
        ImGui::TextDisabled("Item ID: %u | Marker index: %u", marker.itemId, marker.listIndex);
        bool changed = false;
        const int maxBone = std::max(0, std::min(255, marker.boneCount - 1));
        ImGui::SetNextItemWidth(UiPx(220.0f));
        changed |= ImGui::DragInt("Bone / segment", &edit.boneSegment, 1.0f, 0, maxBone, "%d", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine();
        const std::uint16_t liveBoneId = Features::MarkerBoneIdForSegment(edit.boneSegment);
        if (liveBoneId != 0xFFFFu)
            ImGui::TextDisabled("Bone ID %u | valid segments 0-%d", static_cast<unsigned int>(liveBoneId), maxBone);
        else
            ImGui::TextDisabled("valid segments 0-%d", maxBone);
        ImGui::Spacing();
        ImGui::TextUnformatted("Transform matrix");
        const float matrixWidth = std::min(UiPx(560.0f), ImGui::GetContentRegionAvail().x);
        for (int row = 0; row < 4; ++row) {
            char rowLabel[32];
            std::snprintf(rowLabel, sizeof(rowLabel), "Row %d", row);
            ImGui::SetNextItemWidth(matrixWidth);
            changed |= ImGui::DragFloat4(rowLabel, edit.matrix + row * 4, 0.0025f, 0.0f, 0.0f, "%.6f");
        }

        bool restored = false;
        if (ImGui::Button("Restore original")) {
            std::memcpy(edit.matrix, marker.originalMatrix, sizeof(edit.matrix));
            edit.boneSegment = marker.originalBoneSegment;
            restored = true;
            if (!Features::QueueMarkerUpdate(marker.listIndex, marker.itemId, edit.matrix, edit.boneSegment))
                ImGui::TextDisabled("Restore rejected; the selected Instance may have changed.");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Restore this COLLISIONMARKER's authored bone attachment and full 4x4 matrix.");

        if (changed && !restored) {
            if (!Features::QueueMarkerUpdate(marker.listIndex, marker.itemId, edit.matrix, edit.boneSegment))
                ImGui::TextDisabled("Update rejected; the selected Instance may have changed.");
        }
        ImGui::PopID();
        ImGui::Spacing();
    }
}

const char* LightTypeName(int type) {
    static const char* names[] = {"Point", "Capsule", "Spot", "Capsule Spot", "Box", "Directional", "Directional Fill"};
    return (type >= 0 && type < static_cast<int>(std::size(names))) ? names[type] : "Unknown";
}

void RefreshLightCache() {
    const auto customVersion = Features::LightListVersion();
    if (customVersion != g_lightCacheVersion) {
        g_lightCacheVersion = customVersion;
        const int count = Features::LightCount();
        g_lightCache.resize(static_cast<std::size_t>(std::max(0, count)));
        if (count > 0) {
            const int copied = Features::CopyLights(g_lightCache.data(), count);
            g_lightCache.resize(static_cast<std::size_t>(std::max(0, copied)));
        }
        if (g_lightSelectionKind == LightSelectionKind::Custom && g_selectedLightId) {
            const bool exists = std::any_of(g_lightCache.begin(), g_lightCache.end(),
                                            [](const auto& x){ return x.id == g_selectedLightId; });
            if (!exists) {
                g_lightSelectionKind = LightSelectionKind::None;
                g_selectedLightId = 0;
                g_lightEditLoaded = false;
            }
        }
    }

    const auto gameVersion = Features::GameLightListVersion();
    if (gameVersion != g_gameLightCacheVersion) {
        g_gameLightCacheVersion = gameVersion;
        const int count = Features::GameLightCount();
        g_gameLightCache.resize(static_cast<std::size_t>(std::max(0, count)));
        if (count > 0) {
            const int copied = Features::CopyGameLights(g_gameLightCache.data(), count);
            g_gameLightCache.resize(static_cast<std::size_t>(std::max(0, copied)));
        }

        
        
        g_selectedGameLightCacheIndex = -1;
        if (g_lightSelectionKind == LightSelectionKind::Game && g_selectedGameLightToken) {
            auto it = std::find_if(g_gameLightCache.begin(), g_gameLightCache.end(),
                                   [](const auto& x){ return x.token == g_selectedGameLightToken; });
            if (it != g_gameLightCache.end()) {
                g_selectedGameLightCacheIndex = static_cast<int>(std::distance(g_gameLightCache.begin(), it));
            } else {
                g_lightSelectionKind = LightSelectionKind::None;
                g_selectedGameLightToken = 0;
                g_selectedGameLightCacheIndex = -1;
                g_lightEditLoaded = false;
            }
        }
    }
}

// ---- Light Editor ----------------------------------------------------------------------------------------------------
// The list: the scene's lights (read by the framework a few hundred a tick, kept up to date while the window is open)
// and the framework's own, filtered by name / type / state / distance and sorted by distance to the camera (or Lara),
// name, type or scene index. The chosen light is edited live and marked in the world (its range as three rings).
struct LightListRow {
    bool custom{};
    int index{};          // into g_gameLightCache / g_lightCache
    float distance{};     // world units (100 a metre); infinity: unknown
};
std::vector<LightListRow> g_lightRows;
char g_lightNameFilter[96]{};
int g_lightTypeFilter = -1;          // -1: every type
bool g_lightOnlyEnabled = false;
bool g_lightOnlyEdited = false;
float g_lightMaxDistanceM = 0.0f;    // 0: any distance
int g_lightSortMode = 0;             // 0 distance, 1 name, 2 type, 3 scene index
bool g_lightDistanceFromLara = false;
bool g_lightShowMarker = true;
std::uint64_t g_lightRowsGameVersion = ~std::uint64_t{0};
std::uint64_t g_lightRowsCustomVersion = ~std::uint64_t{0};
double g_lightRowsBuiltAt = -1.0;
bool g_lightRowsDirty = true;
float g_lightRefPosition[3]{};
bool g_lightRefValid = false;

bool LightNameMatches(const char* name, const char* filter) {
    if (!filter[0]) return true;
    if (!name) return false;
    const std::size_t n = std::strlen(name), f = std::strlen(filter);
    for (std::size_t i = 0; i + f <= n; ++i) {
        std::size_t j = 0;
        while (j < f && std::tolower(static_cast<unsigned char>(name[i + j])) ==
                            std::tolower(static_cast<unsigned char>(filter[j])))
            ++j;
        if (j == f) return true;
    }
    return false;
}

// Where distances are measured from: the camera (the framework's snapshot of it), or Lara.
void UpdateLightReference() {
    g_lightRefValid = false;
    if (!g_lightDistanceFromLara) {
        Features::DrawCameraState camera{};
        if (Features::GetDrawCameraState(camera) && camera.valid) {
            std::copy(std::begin(camera.position), std::end(camera.position), std::begin(g_lightRefPosition));
            g_lightRefValid = true;
            return;
        }
    }
    Features::PlayerTransform player{};
    if (Features::GetPlayerTransform(player)) {
        std::copy(std::begin(player.position), std::end(player.position), std::begin(g_lightRefPosition));
        g_lightRefValid = true;
    }
}

float LightDistance(const float p[3]) {
    if (!g_lightRefValid) return std::numeric_limits<float>::infinity();
    const float dx = p[0] - g_lightRefPosition[0], dy = p[1] - g_lightRefPosition[1], dz = p[2] - g_lightRefPosition[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void BuildLightRows() {
    UpdateLightReference();
    g_lightRows.clear();
    g_lightRows.reserve(g_gameLightCache.size() + g_lightCache.size());
    const float maxDistance = g_lightMaxDistanceM > 0.0f ? g_lightMaxDistanceM * 100.0f : 0.0f;
    for (int i = 0; i < static_cast<int>(g_lightCache.size()); ++i) {
        const auto& light = g_lightCache[static_cast<std::size_t>(i)];
        char name[32];
        std::snprintf(name, sizeof(name), "Custom %u", light.id);
        if (!LightNameMatches(name, g_lightNameFilter)) continue;
        if (g_lightTypeFilter >= 0 && light.settings.type != g_lightTypeFilter) continue;
        if (g_lightOnlyEnabled && !light.settings.enabled) continue;
        const float distance = LightDistance(light.settings.position);
        if (maxDistance > 0.0f && !(distance <= maxDistance)) continue;
        g_lightRows.push_back({true, i, distance});
    }
    for (int i = 0; i < static_cast<int>(g_gameLightCache.size()); ++i) {
        const auto& light = g_gameLightCache[static_cast<std::size_t>(i)];
        if (!LightNameMatches(light.name, g_lightNameFilter)) continue;
        if (g_lightTypeFilter >= 0 && light.settings.type != g_lightTypeFilter) continue;
        const bool enabled = light.detailsLoaded ? light.settings.enabled : light.enabled;
        if (g_lightOnlyEnabled && !enabled) continue;
        if (g_lightOnlyEdited && !light.modified) continue;
        // (the roster's place is read again every pass: moving lights; an edited light's too)
        const float* position = light.hasPosition ? light.position : light.settings.position;
        const float distance = (light.hasPosition || light.detailsLoaded) ? LightDistance(position)
                                                                          : std::numeric_limits<float>::infinity();
        if (maxDistance > 0.0f && !(distance <= maxDistance)) continue;
        g_lightRows.push_back({false, i, distance});
    }
    auto nameOf = [](const LightListRow& r) -> const char* {
        return r.custom ? "" : g_gameLightCache[static_cast<std::size_t>(r.index)].name;
    };
    auto typeOf = [](const LightListRow& r) {
        return r.custom ? g_lightCache[static_cast<std::size_t>(r.index)].settings.type
                        : g_gameLightCache[static_cast<std::size_t>(r.index)].settings.type;
    };
    auto indexOf = [](const LightListRow& r) {
        return r.custom ? -1 : static_cast<int>(g_gameLightCache[static_cast<std::size_t>(r.index)].sceneIndex);
    };
    std::stable_sort(g_lightRows.begin(), g_lightRows.end(), [&](const LightListRow& a, const LightListRow& b) {
        if (a.custom != b.custom) return a.custom;   // (the framework's lights first)
        switch (g_lightSortMode) {
            case 1: { const int c = std::strcmp(nameOf(a), nameOf(b)); if (c) return c < 0; break; }
            case 2: if (typeOf(a) != typeOf(b)) return typeOf(a) < typeOf(b); break;
            case 3: return indexOf(a) < indexOf(b);
            default: break;
        }
        if (a.distance != b.distance) return a.distance < b.distance;
        return indexOf(a) < indexOf(b);
    });
    g_lightRowsBuiltAt = ImGui::GetTime();
    g_lightRowsDirty = false;
    g_lightRowsGameVersion = g_gameLightCacheVersion;
    g_lightRowsCustomVersion = g_lightCacheVersion;
}

void SelectGameLight(const Features::GameLightInfo& light, int cacheIndex) {
    g_lightSelectionKind = LightSelectionKind::Game;
    g_selectedGameLightToken = light.token;
    g_selectedGameLightCacheIndex = cacheIndex;
    g_selectedLightId = 0;
    g_lightEditLoaded = false;
    Features::RequestGameLightDetails(light.token);
}

void SelectCustomLight(const Features::LightInfo& light) {
    g_lightSelectionKind = LightSelectionKind::Custom;
    g_selectedLightId = light.id;
    g_selectedGameLightToken = 0;
    g_selectedGameLightCacheIndex = -1;
    g_lightEdit = light.settings;
    g_lightEditLoaded = true;
}

const Features::GameLightInfo* SelectedGameLight() {
    if (g_lightSelectionKind != LightSelectionKind::Game) return nullptr;
    if (g_selectedGameLightCacheIndex < 0 || g_selectedGameLightCacheIndex >= static_cast<int>(g_gameLightCache.size()))
        return nullptr;
    const auto& light = g_gameLightCache[static_cast<std::size_t>(g_selectedGameLightCacheIndex)];
    return light.token == g_selectedGameLightToken ? &light : nullptr;
}

const Features::LightInfo* SelectedCustomLight() {
    if (g_lightSelectionKind != LightSelectionKind::Custom) return nullptr;
    auto it = std::find_if(g_lightCache.begin(), g_lightCache.end(), [](const auto& x) { return x.id == g_selectedLightId; });
    return it != g_lightCache.end() ? &*it : nullptr;
}

void LightTooltip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text);
}

void FormatLightDistance(char* out, std::size_t size, float distance) {
    if (!std::isfinite(distance)) std::snprintf(out, size, "-");
    else if (distance < 10000.0f) std::snprintf(out, size, "%.1f m", distance / 100.0f);
    else std::snprintf(out, size, "%.0f m", distance / 100.0f);
}

// The selected light in the world: a cross at its place, its range as three rings, its name.
bool LightMarkerActive() {
    return g_showLightEditor && g_lightShowMarker && (SelectedGameLight() || SelectedCustomLight());
}

void DrawLightMarker() {
    if (!LightMarkerActive()) return;
    Features::DrawCameraState camera{};
    if (!Features::GetDrawCameraState(camera)) return;
    DrawCameraBasis basis{};
    if (!BuildDrawCameraBasis(camera, basis)) return;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    if (!draw) return;
    float position[3]{};
    float color[3]{1.0f, 1.0f, 1.0f};
    float range = 0.0f;
    const char* name = "";
    char customName[32];
    if (const auto* game = SelectedGameLight()) {
        const bool edited = game->detailsLoaded && g_lightEditLoaded;
        if (!edited && !game->hasPosition) return;
        std::copy_n(edited ? g_lightEdit.position : game->position, 3, position);
        std::copy_n(edited ? g_lightEdit.color : game->color, 3, color);
        range = edited ? g_lightEdit.range : game->range;
        name = game->name;
    } else if (SelectedCustomLight()) {
        std::copy_n(g_lightEdit.position, 3, position);
        std::copy_n(g_lightEdit.color, 3, color);
        range = g_lightEdit.range;
        std::snprintf(customName, sizeof(customName), "Custom %u", g_selectedLightId);
        name = customName;
    } else {
        return;
    }
    const float maxC = std::max({color[0], color[1], color[2], 0.001f});
    const ImU32 lineColor = IM_COL32(static_cast<int>(120 + 135 * color[0] / maxC), static_cast<int>(120 + 135 * color[1] / maxC),
                                     static_cast<int>(120 + 135 * color[2] / maxC), 235);
    const float cross = 30.0f;
    for (int axis = 0; axis < 3; ++axis) {
        float a[3]{position[0], position[1], position[2]}, b[3]{position[0], position[1], position[2]};
        a[axis] -= cross;
        b[axis] += cross;
        DrawWorldLine(draw, basis, a, b, IM_COL32(255, 255, 255, 240), 2.0f);
    }
    if (std::isfinite(range) && range > 1.0f && range < 200000.0f) {
        constexpr int kSegments = 48;
        for (int plane = 0; plane < 3; ++plane) {
            float prev[3]{};
            for (int i = 0; i <= kSegments; ++i) {
                const float t = 6.28318530718f * static_cast<float>(i) / kSegments;
                const float c = std::cos(t) * range, s = std::sin(t) * range;
                float p[3]{position[0], position[1], position[2]};
                if (plane == 0) { p[0] += c; p[1] += s; }
                else if (plane == 1) { p[0] += c; p[2] += s; }
                else { p[1] += c; p[2] += s; }
                if (i) DrawWorldLine(draw, basis, prev, p, lineColor, 1.5f);
                std::copy_n(p, 3, prev);
            }
        }
    }
    ImVec2 at{};
    if (ProjectDrawPoint(basis, position, at)) {
        char label[160];
        char distance[32];
        const float dx = position[0] - basis.pos[0], dy = position[1] - basis.pos[1], dz = position[2] - basis.pos[2];
        FormatLightDistance(distance, sizeof(distance), std::sqrt(dx * dx + dy * dy + dz * dz));
        std::snprintf(label, sizeof(label), "%s  (%s)", name, distance);
        DrawShadowedText(draw, ImVec2(at.x + 8.0f, at.y - 18.0f), IM_COL32(255, 255, 255, 255), label);
    }
}

void DrawLightList() {
    // filters
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##light_filter", "Search by name", g_lightNameFilter, sizeof(g_lightNameFilter)))
        g_lightRowsDirty = true;
    ImGui::SetNextItemWidth(UiPx(130.0f));
    if (ImGui::BeginCombo("##light_type", g_lightTypeFilter < 0 ? "Every type" : LightTypeName(g_lightTypeFilter))) {
        if (ImGui::Selectable("Every type", g_lightTypeFilter < 0)) { g_lightTypeFilter = -1; g_lightRowsDirty = true; }
        for (int i = 0; i < 7; ++i)
            if (ImGui::Selectable(LightTypeName(i), g_lightTypeFilter == i)) { g_lightTypeFilter = i; g_lightRowsDirty = true; }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##light_distance", &g_lightMaxDistanceM, 0.0f, 200.0f,
                           g_lightMaxDistanceM > 0.0f ? "within %.0f m" : "any distance"))
        g_lightRowsDirty = true;
    LightTooltip("Only the lights within this distance (0: any). Ctrl+click to type a value.");
    if (ImGui::Checkbox("On only", &g_lightOnlyEnabled)) g_lightRowsDirty = true;
    LightTooltip("Only the lights that are switched on.");
    ImGui::SameLine();
    if (ImGui::Checkbox("Edited only", &g_lightOnlyEdited)) g_lightRowsDirty = true;
    LightTooltip("Only the game's lights you changed.");
    ImGui::SetNextItemWidth(UiPx(130.0f));
    static const char* const kSorts[] = {"Nearest first", "By name", "By type", "Scene order"};
    if (ImGui::Combo("##light_sort", &g_lightSortMode, kSorts, IM_ARRAYSIZE(kSorts))) g_lightRowsDirty = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("From Lara", &g_lightDistanceFromLara)) g_lightRowsDirty = true;
    LightTooltip("Distances from Lara instead of the camera.");

    if (g_lightRowsDirty || g_lightRowsGameVersion != g_gameLightCacheVersion ||
        g_lightRowsCustomVersion != g_lightCacheVersion || ImGui::GetTime() - g_lightRowsBuiltAt > 0.5)
        BuildLightRows();

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##light_table", 4, flags, ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Light", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(g_lightRows.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto& r = g_lightRows[static_cast<std::size_t>(row)];
                char name[96];
                const float* color = nullptr;
                int type = 0;
                bool on = true, edited = false, shadows = false, selected = false;
                if (r.custom) {
                    const auto& light = g_lightCache[static_cast<std::size_t>(r.index)];
                    std::snprintf(name, sizeof(name), "Custom %u", light.id);
                    color = light.settings.color;
                    type = light.settings.type;
                    on = light.settings.enabled;
                    shadows = light.settings.shadowEnabled;
                    selected = g_lightSelectionKind == LightSelectionKind::Custom && g_selectedLightId == light.id;
                } else {
                    const auto& light = g_gameLightCache[static_cast<std::size_t>(r.index)];
                    std::snprintf(name, sizeof(name), "%s", light.name[0] ? light.name : "Game light");
                    color = light.detailsLoaded ? light.settings.color : light.color;
                    type = light.settings.type;
                    on = light.detailsLoaded ? light.settings.enabled : light.enabled;
                    shadows = light.detailsLoaded ? light.settings.shadowEnabled : light.castsShadows;
                    edited = light.modified;
                    selected = g_lightSelectionKind == LightSelectionKind::Game && g_selectedGameLightToken == light.token;
                }
                ImGui::PushID(row);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::ColorButton("##c", ImVec4(color[0], color[1], color[2], 1.0f),
                                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop | ImGuiColorEditFlags_NoBorder,
                                   ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()));
                ImGui::SameLine();
                if (!on) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                const bool clicked = ImGui::Selectable(name, selected, ImGuiSelectableFlags_SpanAllColumns);
                if (!on) ImGui::PopStyleColor();
                if (clicked) {
                    if (r.custom) SelectCustomLight(g_lightCache[static_cast<std::size_t>(r.index)]);
                    else SelectGameLight(g_gameLightCache[static_cast<std::size_t>(r.index)], r.index);
                }
                if (!r.custom && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                    ImGui::SetTooltip("Scene index %u", g_gameLightCache[static_cast<std::size_t>(r.index)].sceneIndex);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(LightTypeName(type));
                ImGui::TableNextColumn();
                char distance[32];
                FormatLightDistance(distance, sizeof(distance), r.distance);
                ImGui::TextUnformatted(distance);
                ImGui::TableNextColumn();
                char state[48];
                std::snprintf(state, sizeof(state), "%s%s%s%s", on ? "on" : "off", shadows ? ", shadows" : "",
                              edited ? ", edited" : "", r.custom ? ", custom" : "");
                ImGui::TextDisabled("%s", state);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

void DrawLightEditorWindow() {
    if (!g_showLightEditor) return;
    Features::SetLightEditorActive(true);
    RefreshLightCache();

    ImGui::SetNextWindowSize(UiSize(1040.0f, 700.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Light Editor", &g_showLightEditor)) {
        ImGui::End();
        return;
    }
    if (!Features::LightControlsAvailable()) {
        ImGui::TextDisabled("Native light editing is unavailable for this executable build.");
        ImGui::End();
        return;
    }

    // toolbar
    if (ImGui::Button("New light")) Features::RequestCreateLight();
    LightTooltip("A point light of the framework's at Lara's place (it lasts until the scene changes).");
    ImGui::SameLine();
    int edited = 0;
    for (const auto& light : g_gameLightCache) edited += light.modified ? 1 : 0;
    ImGui::BeginDisabled(edited == 0);
    char restoreLabel[48];
    std::snprintf(restoreLabel, sizeof(restoreLabel), "Restore edited (%d)", edited);
    if (ImGui::Button(restoreLabel)) {
        Features::QueueRestoreAllGameLights();
        if (const auto* game = SelectedGameLight(); game && game->detailsLoaded) {
            g_lightEdit = game->originalSettings;   // (the chosen light shows the game's values again)
            g_lightEditLoaded = true;
        }
    }
    ImGui::EndDisabled();
    LightTooltip("Put every game light you changed back as the game had it.");
    ImGui::SameLine();
    ImGui::BeginDisabled(g_lightCache.empty());
    if (ImGui::Button("Delete my lights")) {
        Features::QueueDeleteAllLights();
        if (g_lightSelectionKind == LightSelectionKind::Custom) {
            g_lightSelectionKind = LightSelectionKind::None;
            g_selectedLightId = 0;
            g_lightEditLoaded = false;
        }
    }
    ImGui::EndDisabled();
    LightTooltip("Delete every light the framework made.");
    ImGui::SameLine();
    if (ImGui::Button("Read again")) Features::RequestGameLightRefresh();
    LightTooltip("Read the scene's lights now (the list is also kept up to date every couple of seconds while this "
                 "window is open). Your edits are kept.");
    ImGui::SameLine();
    ImGui::Checkbox("Mark the chosen light", &g_lightShowMarker);
    LightTooltip("Draw the chosen light in the world: a cross at its place, its range as three rings, its name.");
    int scanned = 0, total = 0;
    bool scanning = false;
    Features::GameLightScanProgress(scanned, total, scanning);
    ImGui::SameLine();
    if (scanning && total > 0)
        ImGui::TextDisabled("Reading lights %d / %d...", scanned, total);
    else
        ImGui::TextDisabled("%d game light%s, %d of yours", static_cast<int>(g_gameLightCache.size()),
                            g_gameLightCache.size() == 1 ? "" : "s", static_cast<int>(g_lightCache.size()));
    ImGui::Separator();

    ImGui::BeginChild("##light_list", UiSize(430.0f, 0.0f), true);
    DrawLightList();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##light_editor", ImVec2(0.0f, 0.0f), false);

    const Features::LightInfo* selectedCustom = SelectedCustomLight();
    const Features::GameLightInfo* selectedGame = SelectedGameLight();
    if (!selectedCustom && !selectedGame) {
        ImGui::TextDisabled("Choose a light in the list, or make a new one.");
        ImGui::Spacing();
        ImGui::TextWrapped("The game's lights are edited live and can be put back (Restore). The framework never "
                           "deletes them; to change what only a light's resource holds (its type, shadow map, lighting "
                           "weights), copy it as a light of yours.");
        ImGui::EndChild();
        ImGui::End();
        return;
    }

    const bool isGameLight = selectedGame != nullptr;
    char distance[32];
    if (isGameLight) {
        ImGui::Text("%s", selectedGame->name[0] ? selectedGame->name : "Game light");
        ImGui::SameLine();
        ImGui::TextDisabled("%s, scene index %u%s", LightTypeName(selectedGame->settings.type), selectedGame->sceneIndex,
                            selectedGame->modified ? ", edited" : "");
    } else {
        ImGui::Text("Custom %u", selectedCustom->id);
        ImGui::SameLine();
        ImGui::TextDisabled("%s, made by the framework", LightTypeName(selectedCustom->settings.type));
    }

    if (isGameLight && !selectedGame->detailsLoaded) {
        ImGui::Separator();
        ImGui::TextDisabled("Reading this light's settings...");
        ImGui::ColorButton("##quick", ImVec4(selectedGame->color[0], selectedGame->color[1], selectedGame->color[2], 1.0f),
                           ImGuiColorEditFlags_NoTooltip);
        ImGui::SameLine();
        ImGui::Text("Intensity %.2f, range %.1f m, %s", selectedGame->intensity, selectedGame->range / 100.0f,
                    selectedGame->enabled ? "on" : "off");
        if (ImGui::Button("Try again")) Features::RequestGameLightDetails(selectedGame->token);
        ImGui::EndChild();
        ImGui::End();
        return;
    }
    if (!g_lightEditLoaded) {
        g_lightEdit = isGameLight ? selectedGame->settings : selectedCustom->settings;
        g_lightEditLoaded = true;
    }
    {
        UpdateLightReference();
        FormatLightDistance(distance, sizeof(distance), LightDistance(g_lightEdit.position));
        ImGui::TextDisabled("%s from %s", distance, g_lightDistanceFromLara ? "Lara" : "the camera");
    }

    // actions
    if (isGameLight) {
        ImGui::BeginDisabled(!selectedGame->modified);
        if (ImGui::Button("Restore")) {
            Features::QueueRestoreGameLight(selectedGame->token);
            g_lightEdit = selectedGame->originalSettings;
            g_lightEditLoaded = true;
        }
        ImGui::EndDisabled();
        LightTooltip("Put this light back as the game had it.");
        ImGui::SameLine();
        if (ImGui::Button("Copy as my light")) Features::QueueDuplicateGameLight(selectedGame->token);
        LightTooltip("A light of the framework's made from this one (its resource as the template): all of its "
                     "settings can be changed, its type and shadows too.");
    } else {
        if (ImGui::Button("Delete")) {
            Features::QueueLightDelete(g_selectedLightId);
            g_lightSelectionKind = LightSelectionKind::None;
            g_selectedLightId = 0;
            g_lightEditLoaded = false;
            ImGui::EndChild();
            ImGui::End();
            return;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Take Lara there")) {
        Features::PlayerTransform player{};
        if (Features::GetPlayerTransform(player)) {
            std::copy(std::begin(g_lightEdit.position), std::end(g_lightEdit.position), std::begin(player.position));
            Features::TeleportPlayer(player);
        }
    }
    LightTooltip("Teleport Lara to the light's place (she keeps facing the same way).");
    ImGui::Separator();

    bool liveChanged = false;
    bool resourceCommit = false;
    auto ResourceDragFloat = [&](const char* label, float* value, float speed, float minv, float maxv, const char* fmt,
                                 const char* help) {
        ImGui::DragFloat(label, value, speed, minv, maxv, fmt);
        if (ImGui::IsItemDeactivatedAfterEdit()) resourceCommit = true;
        LightTooltip(help);
    };

    if (ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen)) {
        liveChanged |= ImGui::Checkbox("On", &g_lightEdit.enabled);
        LightTooltip("Whether the light shines.");
        if (!isGameLight) {
            ImGui::SetNextItemWidth(UiPx(220.0f));
            if (ImGui::BeginCombo("Type", LightTypeName(g_lightEdit.type))) {
                for (int i = 0; i < 7; ++i) {
                    if (ImGui::Selectable(LightTypeName(i), g_lightEdit.type == i) && g_lightEdit.type != i) {
                        g_lightEdit.type = i;
                        if ((i == 1 || i == 3) && g_lightEdit.nearRadius < 1.0f) g_lightEdit.nearRadius = 100.0f;
                        resourceCommit = true;
                    }
                }
                ImGui::EndCombo();
            }
            LightTooltip("Point: all around. Spot: a cone (Umbra / Penumbra). Capsule: along a line (Near radius). "
                         "Changing it rebuilds the light.");
        }
        liveChanged |= ImGui::ColorEdit3("Color", g_lightEdit.color, ImGuiColorEditFlags_Float);
        liveChanged |= ImGui::DragFloat("Intensity", &g_lightEdit.intensity, 0.02f, 0.0f, 10000.0f, "%.3f");
        LightTooltip("Brightness (the color is multiplied by it).");
        float rangeM = g_lightEdit.range / 100.0f;
        if (ImGui::DragFloat("Range", &rangeM, 0.05f, 0.0001f, 1000.0f, "%.2f m")) {
            g_lightEdit.range = rangeM * 100.0f;
            liveChanged = true;
        }
        LightTooltip("How far the light reaches (the rings drawn in the world).");
        const float nearMin = (g_lightEdit.type == 1 || g_lightEdit.type == 3) ? 0.0001f : 0.0f;
        float nearM = g_lightEdit.nearRadius / 100.0f;
        if (ImGui::DragFloat("Near radius", &nearM, 0.01f, nearMin, 100.0f, "%.2f m")) {
            g_lightEdit.nearRadius = nearM * 100.0f;
            liveChanged = true;
        }
        LightTooltip("The light's own size: a capsule light's length, a soft start for the others.");
        if (g_lightEdit.type == 2 || g_lightEdit.type == 3) {
            liveChanged |= ImGui::DragFloat("Umbra", &g_lightEdit.umbra, 0.002f, -10.0f, 10.0f, "%.4f");
            LightTooltip("A spot's inner cone (full brightness inside).");
            liveChanged |= ImGui::DragFloat("Penumbra", &g_lightEdit.penumbra, 0.002f, -10.0f, 10.0f, "%.4f");
            LightTooltip("A spot's outer cone (fading to dark at its edge).");
        }
        liveChanged |= ImGui::Checkbox("Indirect lighting", &g_lightEdit.indirectLit);
        LightTooltip("Whether the light also feeds the bounced (indirect) lighting.");
    }

    if (ImGui::CollapsingHeader("Place", ImGuiTreeNodeFlags_DefaultOpen)) {
        float positionM[3]{g_lightEdit.position[0] / 100.0f, g_lightEdit.position[1] / 100.0f, g_lightEdit.position[2] / 100.0f};
        ImGui::SetNextItemWidth(UiPx(360.0f));
        if (ImGui::DragFloat3("Position", positionM, 0.01f, 0.0f, 0.0f, "%.2f m")) {
            for (int i = 0; i < 3; ++i) g_lightEdit.position[i] = positionM[i] * 100.0f;
            liveChanged = true;
        }
        LightTooltip("World position, in metres (x, y, z; z is up).");
        constexpr float kDeg = 57.2957795131f;
        float rotationDeg[3]{g_lightEdit.rotation[0] * kDeg, g_lightEdit.rotation[1] * kDeg, g_lightEdit.rotation[2] * kDeg};
        ImGui::SetNextItemWidth(UiPx(360.0f));
        if (ImGui::DragFloat3("Rotation", rotationDeg, 0.25f, -360.0f, 360.0f, "%.1f deg")) {
            for (int i = 0; i < 3; ++i) g_lightEdit.rotation[i] = rotationDeg[i] / kDeg;
            liveChanged = true;
        }
        LightTooltip("Where a spot light points (x, y, z rotation).");
        if (ImGui::Button("To Lara")) {
            Features::PlayerTransform player{};
            if (Features::GetPlayerTransform(player)) {
                std::copy(std::begin(player.position), std::end(player.position), std::begin(g_lightEdit.position));
                liveChanged = true;
            }
        }
        LightTooltip("Move the light to Lara's place.");
        ImGui::SameLine();
        if (ImGui::Button("To the camera")) {
            Features::DrawCameraState camera{};
            if (Features::GetDrawCameraState(camera) && camera.valid) {
                std::copy(std::begin(camera.position), std::end(camera.position), std::begin(g_lightEdit.position));
                liveChanged = true;
            }
        }
        LightTooltip("Move the light to the camera's place.");
        ImGui::SameLine();
        if (ImGui::Button("No rotation")) {
            g_lightEdit.rotation[0] = g_lightEdit.rotation[1] = g_lightEdit.rotation[2] = 0.0f;
            liveChanged = true;
        }
    }

    if (ImGui::CollapsingHeader("Shading")) {
        if (isGameLight && selectedGame->hasResourceProperties) {
            // (v0.9.529) written into the light's resource, which the game reads every frame: at once, for every light
            // on the same resource
            if (selectedGame->resourceShared > 1)
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                                   "Shared: these change all %d lights on this light's resource.", selectedGame->resourceShared);
            auto LiveDragFloat = [&](const char* label, float* value, float speed, float minv, float maxv, const char* help) {
                liveChanged |= ImGui::DragFloat(label, value, speed, minv, maxv, "%.3f");
                LightTooltip(help);
            };
            LiveDragFloat("Probe priority", &g_lightEdit.probePriority, 0.01f, -1000.0f, 1000.0f,
                          "Which light the lighting probes prefer.");
            LiveDragFloat("Indirect intensity", &g_lightEdit.indirectIntensity, 0.01f, -1000.0f, 1000.0f,
                          "The bounced light's brightness.");
            LiveDragFloat("Indirect strength", &g_lightEdit.indirectStrength, 0.01f, -1000.0f, 1000.0f,
                          "How much the bounced light counts.");
            LiveDragFloat("Specular strength", &g_lightEdit.specularStrength, 0.01f, -1000.0f, 1000.0f,
                          "Its highlights' strength.");
            LiveDragFloat("Lambert weight", &g_lightEdit.lambertWeight, 0.01f, -100.0f, 100.0f,
                          "Plain (Lambert) diffuse share: hard falloff toward the unlit side.");
            LiveDragFloat("Half-Lambert weight", &g_lightEdit.halfLambertWeight, 0.01f, -100.0f, 100.0f,
                          "Wrapped (half-Lambert) diffuse share: light that wraps softly around the form.");
            LiveDragFloat("Ambient weight", &g_lightEdit.ambientWeight, 0.01f, -100.0f, 100.0f,
                          "Flat (ambient) share: the same from every side.");
            LiveDragFloat("Minimum shadow specular", &g_lightEdit.minShadowSpecular, 0.01f, -100.0f, 100.0f,
                          "Highlights kept in its shadow.");
            LiveDragFloat("Range scale", &g_lightEdit.rangeScale, 0.01f, -100.0f, 100.0f, "The range's multiplier.");
            ImGui::TextDisabled("Restore puts the game's values back.");
        } else if (isGameLight) {
            ImGui::TextDisabled("This light's resource could not be read.");
        } else {
            ResourceDragFloat("Probe priority", &g_lightEdit.probePriority, 0.01f, -1000.0f, 1000.0f, "%.3f",
                              "Which light the lighting probes prefer.");
            ResourceDragFloat("Indirect intensity", &g_lightEdit.indirectIntensity, 0.01f, -1000.0f, 1000.0f, "%.3f",
                              "The bounced light's brightness.");
            ResourceDragFloat("Indirect strength", &g_lightEdit.indirectStrength, 0.01f, -1000.0f, 1000.0f, "%.3f",
                              "How much the bounced light counts.");
            ResourceDragFloat("Specular strength", &g_lightEdit.specularStrength, 0.01f, -1000.0f, 1000.0f, "%.3f",
                              "Its highlights' strength.");
            ResourceDragFloat("Lambert weight", &g_lightEdit.lambertWeight, 0.01f, -100.0f, 100.0f, "%.3f",
                              "Plain (Lambert) diffuse share.");
            ResourceDragFloat("Half-Lambert weight", &g_lightEdit.halfLambertWeight, 0.01f, -100.0f, 100.0f, "%.3f",
                              "Wrapped (half-Lambert) diffuse share.");
            ResourceDragFloat("Ambient weight", &g_lightEdit.ambientWeight, 0.01f, -100.0f, 100.0f, "%.3f",
                              "Flat (ambient) share.");
            ResourceDragFloat("Minimum shadow specular", &g_lightEdit.minShadowSpecular, 0.01f, -100.0f, 100.0f, "%.3f",
                              "Highlights kept in its shadow.");
            ResourceDragFloat("Range scale", &g_lightEdit.rangeScale, 0.01f, -100.0f, 100.0f, "%.3f",
                              "The range's multiplier.");
            ImGui::TextDisabled("These rebuild the light once, when you let go.");
        }
    }

    if (ImGui::CollapsingHeader("Shadows")) {
        liveChanged |= ImGui::Checkbox("Casts shadows", &g_lightEdit.shadowEnabled);
        LightTooltip("Whether the light casts shadows (costly for many lights).");
        if (isGameLight) {
            ImGui::TextDisabled("The game's resource holds these (copy the light as yours to change them):");
            ImGui::BulletText("Shadow map: %d", g_lightEdit.shadowMapSize);
            ImGui::BulletText("Bias: %.6f", g_lightEdit.shadowMapBias);
            ImGui::BulletText("Soft: %s (radius %.3f, %d samples)", g_lightEdit.softShadow ? "yes" : "no",
                              g_lightEdit.softShadowRadius, g_lightEdit.softShadowSamples);
            ImGui::BulletText("Soft depth bias / exponent: %.6f / %.3f", g_lightEdit.softShadowDepthBias,
                              g_lightEdit.softShadowExponent);
        } else {
            static const int kShadowSizes[] = {128, 256, 512, 1024, 2048, 4096};
            char current[32];
            std::snprintf(current, sizeof(current), "%d", g_lightEdit.shadowMapSize);
            ImGui::SetNextItemWidth(UiPx(160.0f));
            if (ImGui::BeginCombo("Shadow map", current)) {
                for (int size : kShadowSizes) {
                    char label[32];
                    std::snprintf(label, sizeof(label), "%d", size);
                    if (ImGui::Selectable(label, g_lightEdit.shadowMapSize == size) && g_lightEdit.shadowMapSize != size) {
                        g_lightEdit.shadowMapSize = size;
                        resourceCommit = true;
                    }
                }
                ImGui::EndCombo();
            }
            LightTooltip("The shadow texture's size: sharper and costlier when larger.");
            ResourceDragFloat("Bias", &g_lightEdit.shadowMapBias, 0.00001f, -1.0f, 1.0f, "%.6f",
                              "Pushes the shadow off the surfaces (too small: speckles; too large: gaps).");
            if (ImGui::Checkbox("Soft shadows", &g_lightEdit.softShadow)) resourceCommit = true;
            LightTooltip("Blurred shadow edges.");
            ResourceDragFloat("Soft radius", &g_lightEdit.softShadowRadius, 0.005f, 0.0f, 100.0f, "%.3f",
                              "How blurred the edges are.");
            ImGui::DragInt("Soft samples", &g_lightEdit.softShadowSamples, 1.0f, 1, 64);
            if (ImGui::IsItemDeactivatedAfterEdit()) resourceCommit = true;
            LightTooltip("More samples: smoother, costlier.");
            ResourceDragFloat("Soft depth bias", &g_lightEdit.softShadowDepthBias, 0.00001f, -1.0f, 1.0f, "%.6f",
                              "The soft shadows' own bias.");
            ResourceDragFloat("Soft exponent", &g_lightEdit.softShadowExponent, 0.01f, -100.0f, 100.0f, "%.3f",
                              "The soft shadows' falloff.");
        }
    }

    if (isGameLight && ImGui::CollapsingHeader("Game details")) {
        ImGui::TextDisabled("Native light: %p", reinterpret_cast<void*>(selectedGame->address));
        ImGui::TextDisabled("Light mask 0x%08X, shadow mask 0x%08X", selectedGame->lightMask, selectedGame->shadowMask);
        ImGui::TextDisabled("Portal culling %s, object culling %s, resource %s", selectedGame->portalCulling ? "on" : "off",
                            selectedGame->objectCulling ? "on" : "off",
                            selectedGame->hasResourceProperties ? "read" : "not read");
    }

    if (isGameLight) {
        if (liveChanged) Features::QueueGameLightUpdate(selectedGame->token, g_lightEdit);
    } else if (liveChanged || resourceCommit) {
        Features::QueueLightUpdate(selectedCustom->id, g_lightEdit, resourceCommit);
    }
    ImGui::EndChild();
    ImGui::End();
}

void DrawDiagnosticsWindow() {
    if (!g_showDiagnostics) return;

    ImGui::SetNextWindowSize(UiSize(620.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Diagnostics", &g_showDiagnostics)) {
        ImGuiIO& io = ImGui::GetIO();
        const float fps = io.Framerate;
        ImGui::Text("FPS: %.1f", fps);
        ImGui::Text("Frame time: %.3f ms", fps > 0.0f ? 1000.0f / fps : 0.0f);
        ImGui::Separator();
        ImGui::Text("Retail build: %s", GameBuild::IsSupportedRetail() ? "Supported" : "Unsupported");
        ImGui::Text("ROTTR base: 0x%p", reinterpret_cast<void*>(GameBuild::Base()));
        ImGui::Text("Feature module: %s", Features::HooksCreated() ? "Initialized" : "Not initialized");
        ImGui::Text("Renderer: %s", Overlay::RendererName());
        ImGui::Text("Player transform access: %s", Features::PlayerToolsAvailable() ? "Available" : "Unavailable");
        ImGui::Text("Teleport access: %s", Features::TeleportAvailable() ? "Available" : "Unavailable");
        ImGui::Text("Invulnerability access: %s", Features::InvulnerabilityAvailable() ? "Available" : "Unavailable");
        ImGui::Text("Infinite Health access: %s", Features::InfiniteHealthAvailable() ? "Available" : "Unavailable");
        ImGui::Text("Infinite Ammo/Resources access: %s",
                    (Features::InfiniteAmmoAvailable() && Features::InfiniteResourcesAvailable()) ? "Available" : "Unavailable");
        ImGui::Text("F11 native InstantKill path: %s", Features::PlayerKillAvailable() ? "Available" : "Unavailable");
        if (Features::PlayerKillAvailable())
            ImGui::Text("F11 kill state: %s", Features::PlayerKillStatus());
        ImGui::Text("Death timing override: %s", Features::DeathTimingAvailable() ? "Available" : "Unavailable");
        if (Features::DeathTimingAvailable())
            ImGui::Text("Deathfade delay: %.2f s%s", Features::DeathReloadDelay(),
                        Features::DeathReloadDelay() <= 0.0f ? " (native)" : "");
        ImGui::Text("Outfit swap path: %s", Features::OutfitControlsAvailable() ? "Available" : "Unavailable");
        if (Features::OutfitControlsAvailable()) {
            if (!Features::OutfitSwapInProgress())
                ImGui::Text("Current outfit object: %u", static_cast<unsigned int>(Features::CurrentOutfitObjectId()));
            else
                ImGui::TextDisabled("Current outfit object: changing...");
        }
        ImGui::Text("Cloth editor path: %s", Features::ClothControlsAvailable() ? "Available" : "Unavailable");
        if (Features::ClothControlsAvailable()) {
            Features::ClothRuntimeInfo cloth{};
            if (Features::GetClothRuntimeInfo(cloth))
                ImGui::Text("Cloth config/groups: %d | %d active / %d total",
                            cloth.currentConfigIndex, cloth.activeStripGroupCount, cloth.totalStripGroupCount);
        }
        ImGui::Text("PureHair editor: %s", Features::HairControlsAvailable() ? Features::HairStatus() : "Unavailable");
        if (Features::HairControlsAvailable()) {
            Features::HairRuntimeInfo hair{};
            if (Features::GetHairRuntimeInfo(hair))
                ImGui::Text("PureHair profiles/groups: sim %u/%d | render %u/%d | runtime groups %d",
                            hair.currentSimulationProfile, hair.simulationProfileCount,
                            hair.currentRenderingProfile, hair.renderingProfileCount,
                            hair.physicsGroupCount);
        }
        ImGui::Text("COLLISIONMARKER editor: %s", Features::MarkerControlsAvailable() ? Features::MarkerStatus() : "Unavailable");
        ImGui::Text("World Draw overlay: %s", Features::DrawToolsStatus());
        ImGui::Text("Instance browser/birth path: %s", Features::InstanceToolsAvailable() ? "Available" : "Unavailable");
        if (Features::InstanceToolsAvailable()) {
            ImGui::Text("Cached Instance snapshot: %d", Features::InstanceCount());
            ImGui::Text("Rendered selection highlight: %s",
                        Features::InstanceNativeHighlightAvailable() ? "Available" : "Unavailable");
        }
        ImGui::Text("Native Light Editor: %s (%s)", Features::LightControlsAvailable() ? "Available" : "Unavailable", Features::LightStatus());
        ImGui::Separator();
        ImGui::Text("Loading profiler: %s", Features::LoadingProfilerAvailable() ? "Available" : "Unavailable");
        ImGui::Text("Faster loading: %s", Features::FastLoadingEnabled() ? "Enabled" : "Disabled");
        if (Features::LoadingProfilerAvailable()) {
            Features::LoadingProfile load{};
            if (Features::GetLoadingProfile(load) && (load.active || load.completed)) {
                ImGui::Text("Load session %llu: %s | %.3f s",
                            static_cast<unsigned long long>(load.sessionId),
                            load.active ? "ACTIVE" : "last completed", load.elapsedSeconds);
                ImGui::Text("WORLD_LoadUnit: %llu calls | aggregate %.3f s | max %.2f ms",
                            static_cast<unsigned long long>(load.worldLoadUnitCalls),
                            load.worldLoadUnitAggregateSeconds, load.worldLoadUnitMaxMs);
                ImGui::Text("ResolveReceiver: %llu chunks | %.1f MiB | aggregate %.3f s | max %.2f ms | done %llu",
                            static_cast<unsigned long long>(load.resolveDataCalls),
                            static_cast<double>(load.resolveDataBytes) / (1024.0 * 1024.0),
                            load.resolveDataAggregateSeconds, load.resolveDataMaxMs,
                            static_cast<unsigned long long>(load.resolveDoneCalls));
                ImGui::Text("GameStateLoading::Tick: wall %.3f s | thread CPU %.3f s | max %.2f ms",
                            load.loadingTickAggregateSeconds, load.loadingTickThreadCpuSeconds, load.loadingTickMaxMs);
                static const char* const loadStateNames[6] = {
                    "SYNC_BEGIN", "SYNC_PROGRESS", "WAIT_FOR_DATA", "START_LOADING", "LOADING", "DONE"
                };
                for (int state = 0; state < 6; ++state) {
                    ImGui::Text("  %s: wall %.3f s | tick wall %.3f s | thread CPU %.3f s | ticks %llu | first %.3f s",
                                loadStateNames[state], load.loadingStateWallSeconds[state],
                                load.loadingStateTickSeconds[state], load.loadingStateThreadCpuSeconds[state],
                                static_cast<unsigned long long>(load.loadingStateTickCalls[state]),
                                load.loadingStateFirstSeenSeconds[state]);
                }
                if (load.completed) {
                    ImGui::Text("Process I/O: %llu reads | %.1f MiB read | %.1f MiB written",
                                static_cast<unsigned long long>(load.processReadOperations),
                                static_cast<double>(load.processReadBytes) / (1024.0 * 1024.0),
                                static_cast<double>(load.processWriteBytes) / (1024.0 * 1024.0));
                    ImGui::Text("Process CPU: user %.3f s | kernel %.3f s | %.2f core-equivalent",
                                load.processUserCpuSeconds, load.processKernelCpuSeconds, load.cpuCoreEquivalent);
                    ImGui::Text("Fast Loading session: %s | shortened sleeps %llu | requested delay removed %llu ms",
                                load.fastLoadingEnabledForSession ? "ON" : "OFF",
                                static_cast<unsigned long long>(load.fastLoadingSleepShortenedCalls),
                                static_cast<unsigned long long>(load.fastLoadingSleepRequestedMs));
                    ImGui::Text("Aggressive renderer: %s | BeginFrame %llu | mode %d/%d -> %d/%d | writes %llu/%llu",
                                load.fastLoadingAggressiveRendererForSession ? "ON" : "OFF",
                                static_cast<unsigned long long>(load.fastLoadingRendererBeginFrameCalls),
                                load.fastLoadingRendererOriginalSetting,
                                load.fastLoadingRendererOriginalMode,
                                load.fastLoadingRendererLastSetting,
                                load.fastLoadingRendererLastMode,
                                static_cast<unsigned long long>(load.fastLoadingRendererOverrideWrites),
                                static_cast<unsigned long long>(load.fastLoadingRendererRestoreWrites));
                    ImGui::Text("DXGI Present: %llu calls | requested sync 0/1/other = %llu/%llu/%llu | forced 0 = %llu | %.3f s",
                                static_cast<unsigned long long>(load.loadingPresentCalls),
                                static_cast<unsigned long long>(load.loadingPresentSync0Calls),
                                static_cast<unsigned long long>(load.loadingPresentSync1Calls),
                                static_cast<unsigned long long>(load.loadingPresentSyncOtherCalls),
                                static_cast<unsigned long long>(load.loadingPresentForcedNoSyncCalls),
                                load.loadingPresentAggregateSeconds);
                    ImGui::Text("Non-blocking Present: %s | DO_NOT_WAIT %llu | success %llu | dropped busy %llu | failures %llu",
                                load.fastLoadingNonBlockingPresentForSession ? "ON" : "OFF",
                                static_cast<unsigned long long>(load.loadingPresentDoNotWaitCalls),
                                static_cast<unsigned long long>(load.loadingPresentDoNotWaitSuccessCalls),
                                static_cast<unsigned long long>(load.loadingPresentWouldBlockDrops),
                                static_cast<unsigned long long>(load.loadingPresentOtherFailures));
                    ImGui::Separator();
                    ImGui::Text("START_LOADING helper: %llu calls | wall %.3f s | thread CPU %.3f s | max %.2f ms",
                                static_cast<unsigned long long>(load.startLoadingHelperCalls),
                                load.startLoadingHelperSeconds, load.startLoadingHelperThreadCpuSeconds,
                                load.startLoadingHelperMaxMs);
                    ImGui::Text("Blocking load pump: %llu calls | wall %.3f s | thread CPU %.3f s | max %.2f ms",
                                static_cast<unsigned long long>(load.blockingLoadPumpCalls),
                                load.blockingLoadPumpSeconds, load.blockingLoadPumpThreadCpuSeconds,
                                load.blockingLoadPumpMaxMs);
                    const double avgBudget = load.loadWorkPumpCalls
                        ? static_cast<double>(load.loadWorkPumpBudgetSum) / static_cast<double>(load.loadWorkPumpCalls) : 0.0;
                    ImGui::Text("Load work pump: %llu calls | wall %.3f s | CPU %.3f s | budget %.1f (%d..%d)",
                                static_cast<unsigned long long>(load.loadWorkPumpCalls), load.loadWorkPumpSeconds,
                                load.loadWorkPumpThreadCpuSeconds, avgBudget,
                                load.loadWorkPumpBudgetMin, load.loadWorkPumpBudgetMax);
                    ImGui::Text("Pump gate: %llu calls / %llu true | %.3f s | busy flag %llu / %llu true",
                                static_cast<unsigned long long>(load.loadPumpGateCalls),
                                static_cast<unsigned long long>(load.loadPumpGateTrueCalls), load.loadPumpGateSeconds,
                                static_cast<unsigned long long>(load.loadPumpBusyChecks),
                                static_cast<unsigned long long>(load.loadPumpBusyTrue));

                    if (load.readApiAvailable) {
                        ImGui::Text("Win32 ReadFile: %llu calls (%llu sync / %llu overlapped) | ReadFileEx %llu",
                                    static_cast<unsigned long long>(load.win32ReadFileCalls),
                                    static_cast<unsigned long long>(load.win32ReadFileSyncCalls),
                                    static_cast<unsigned long long>(load.win32ReadFileOverlappedCalls),
                                    static_cast<unsigned long long>(load.win32ReadFileExCalls));
                        ImGui::Text("Win32 reads: %.1f MiB requested | %.1f MiB completed | aggregate %.3f s | max %.2f ms | concurrency %llu",
                                    static_cast<double>(load.win32ReadRequestedBytes) / (1024.0 * 1024.0),
                                    static_cast<double>(load.win32ReadCompletedBytes) / (1024.0 * 1024.0),
                                    load.win32ReadAggregateSeconds, load.win32ReadMaxMs,
                                    static_cast<unsigned long long>(load.win32ReadMaxConcurrent));
                        ImGui::Text("Read sizes: <=4K %llu | <=64K %llu | <=256K %llu | <=1M %llu | >1M %llu",
                                    static_cast<unsigned long long>(load.win32ReadBucket4K),
                                    static_cast<unsigned long long>(load.win32ReadBucket64K),
                                    static_cast<unsigned long long>(load.win32ReadBucket256K),
                                    static_cast<unsigned long long>(load.win32ReadBucket1M),
                                    static_cast<unsigned long long>(load.win32ReadBucketLarge));
                        ImGui::Text("Inter-read gaps: %llu | aggregate %.3f s | max %.2f ms",
                                    static_cast<unsigned long long>(load.readGapCount),
                                    load.readGapAggregateSeconds, load.readGapMaxMs);
                        ImGui::Text("Gap sizes: <=0.1ms %llu | <=1ms %llu | <=10ms %llu | <=100ms %llu | >100ms %llu",
                                    static_cast<unsigned long long>(load.readGapBucket100us),
                                    static_cast<unsigned long long>(load.readGapBucket1ms),
                                    static_cast<unsigned long long>(load.readGapBucket10ms),
                                    static_cast<unsigned long long>(load.readGapBucket100ms),
                                    static_cast<unsigned long long>(load.readGapBucketLarge));
                    } else {
                        ImGui::TextDisabled("Win32 ReadFile diagnostics unavailable on this run.");
                    }

                    if (load.waitApiAvailable) {
                        ImGui::Separator();
                        ImGui::Text("Loading-thread waits: %llu calls | aggregate %.3f s | max %.2f ms | thread %lu",
                                    static_cast<unsigned long long>(load.waitCalls), load.waitAggregateSeconds,
                                    load.waitMaxMs, static_cast<unsigned long>(load.loadingThreadId));
                        ImGui::Text("Wait timeouts: infinite %llu | zero %llu | finite %llu | returned timeout %llu",
                                    static_cast<unsigned long long>(load.waitInfiniteCalls),
                                    static_cast<unsigned long long>(load.waitZeroTimeoutCalls),
                                    static_cast<unsigned long long>(load.waitFiniteTimeoutCalls),
                                    static_cast<unsigned long long>(load.waitTimeoutResults));
                        static const char* const waitApiNames[7] = {
                            "Sleep", "SleepEx", "WaitForSingleObject", "WaitForSingleObjectEx",
                            "WaitForMultipleObjects", "WaitForMultipleObjectsEx", "WaitMessage"
                        };
                        for (int api = 0; api < 7; ++api) {
                            if (!load.waitApiCalls[api])
                                continue;
                            ImGui::Text("  %s: %llu calls | %.3f s | max %.2f ms | timeouts %llu",
                                        waitApiNames[api], static_cast<unsigned long long>(load.waitApiCalls[api]),
                                        load.waitApiSeconds[api], load.waitApiMaxMs[api],
                                        static_cast<unsigned long long>(load.waitApiTimeoutResults[api]));
                        }
                        if (load.waitCallsiteCount) {
                            ImGui::TextDisabled("Top wait callers (retail executable RVAs):");
                            for (std::uint32_t siteIndex = 0; siteIndex < load.waitCallsiteCount; ++siteIndex) {
                                const auto& site = load.waitCallsites[siteIndex];
                                const char* apiName = site.apiKind < 7 ? waitApiNames[site.apiKind] : "Unknown";
                                ImGui::Text("  %s +0x%llX: %llu calls | %.3f s | max %.2f ms",
                                            apiName, static_cast<unsigned long long>(site.callerRva),
                                            static_cast<unsigned long long>(site.calls), site.seconds, site.maxMs);
                            }
                        }
                    } else {
                        ImGui::TextDisabled("Loading-thread wait diagnostics unavailable on this run.");
                    }
                } else {
                    ImGui::TextDisabled("Process I/O/CPU totals are finalized when loading exits.");
                }
                ImGui::TextDisabled("Aggregate stage time may exceed wall time when worker threads overlap.");
            } else {
                ImGui::TextDisabled("No GameStateLoading session captured yet.");
            }
        }
        ImGui::Text("Time-scale access: %s", Features::TimeControlsAvailable() ? "Available" : "Unavailable");
        ImGui::Text("Weather controls: %s", Features::WeatherStatus());
        ImGui::Text("Pause access: %s", Features::PauseAvailable() ? "Available" : "Unavailable");
        ImGui::Text("HUD layer access: %s", Features::HudControlsAvailable() ? "Available" : "Unavailable");
        ImGui::Text("Free-camera path: %s", Features::FreeCameraAvailable() ? "Available (direct camera override)" : "Unavailable");
    }
    ImGui::End();
}

// (v1.0.0) the hotkeys, one a line (About and the Help menu; was one line with "|" between them)
void DrawHotkeyLines(bool faint) {
    static const char* const kKeys[] = {"F2: fly", "F3: pause", "F4: freecam", "F5: player input", "F6: UI", "F7: DOF",
                                        "F8: menu", "Shift+F8: visible pass-through", "F9: next outfit", "F11: kill Lara"};
    for (const char* key : kKeys) {
        if (faint) ImGui::TextDisabled("%s", key);
        else ImGui::TextUnformatted(key);
    }
}

void DrawAboutWindow() {
    if (!g_showAbout) return;

    ImGui::SetNextWindowSize(UiSize(360.0f, 360.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("About ROTTRFramework", &g_showAbout)) {
        ImGui::Text("ROTTRFramework v1.0.0");
        ImGui::TextDisabled("Lua 5.4 scripting");   // (v1.0.0: the old "Native core: v0.9.169" label out)
        ImGui::Separator();
        // (v1.0.0) one key a line; the "Extended skeleton support" part below it taken out
        DrawHotkeyLines(false);
        ImGui::Spacing();
        ImGui::Text("Renderer: %s", Overlay::RendererName());
    }
    ImGui::End();
}


}

namespace UI {
void Draw() {
    bool unitSelectorOpen = false;
    g_instanceClothTabActiveThisFrame = false;
    g_instanceCollisionTabActiveThisFrame = false;
    g_instanceMarkerTabActiveThisFrame = false;
    Features::SetInstanceBrowserActive(g_showInstanceBrowser);
    Features::SetHairEditorActive(g_showHairEditor);
    Features::SetLightEditorActive(g_showLightEditor);
    Features::SetMaterialEditorActive(g_showMaterialEditor || SuiteLink::WantsMaterials());
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::MenuItem("Instance")) {
            g_showInstanceBrowser = true;
            Features::SetInstanceBrowserActive(true);
        }


        if (ImGui::MenuItem("Lights")) {
            g_showLightEditor = true;
            Features::SetLightEditorActive(true);
        }

        if (ImGui::MenuItem("Materials")) {
            g_showMaterialEditor = true;
            Features::SetMaterialEditorActive(true);
        }

        if (ImGui::BeginMenu("Scripts")) {
            ImGui::TextDisabled("%s", ScriptEngine::Status());
            ImGui::Separator();
            if (ImGui::MenuItem("Reload Lua scripts"))
                ScriptEngine::RequestReload();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Unit")) {
            unitSelectorOpen = true;
            if (!Features::UnitControlsAvailable()) {
                ImGui::TextDisabled("Unit loading is unavailable for this executable build.");
            } else {

                const std::uint64_t version = Features::UnitListVersion();
                if (version != g_unitCacheVersion) {
                    const int count = Features::UnitCount();
                    g_unitCache.resize(count > 0 ? static_cast<std::size_t>(count) : 0u);
                    int copied = 0;
                    if (!g_unitCache.empty())
                        copied = Features::CopyUnits(g_unitCache.data(), static_cast<int>(g_unitCache.size()));
                    g_unitCache.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0u);
                    g_unitCacheVersion = version;
                    g_unitFilterDirty = true;
                }

                ImGui::SetNextItemWidth(UiPx(420.0f));
                if (ImGui::InputTextWithHint("##unit_search", "Search Chapter Select name, Unit ID or Runnable...",
                                             g_unitSearch, sizeof(g_unitSearch))) {
                    g_unitFilterDirty = true;
                }
                ImGui::Separator();

                if (g_unitCache.empty()) {
                    ImGui::TextDisabled("Chapter Select catalog is being cached incrementally. The menu can remain open.");
                } else {
                    
                    
                    
                    
                    if (g_unitFilterDirty) {
                        g_unitFiltered.clear();
                        if (g_unitSearch[0]) {
                            g_unitFiltered.reserve(g_unitCache.size());
                            for (std::size_t i = 0; i < g_unitCache.size(); ++i) {
                                const auto& info = g_unitCache[i];
                                char idText[32]{};
                                std::snprintf(idText, sizeof(idText), "%u", static_cast<unsigned int>(info.unitId));
                                if (ContainsNoCase(info.releaseName, g_unitSearch) ||
                                    ContainsNoCase(info.sprintName, g_unitSearch) ||
                                    ContainsNoCase(info.storyName, g_unitSearch) ||
                                    ContainsNoCase(info.unitName, g_unitSearch) ||
                                    ContainsNoCase(info.runnableName, g_unitSearch) ||
                                    ContainsNoCase(idText, g_unitSearch)) {
                                    g_unitFiltered.push_back(static_cast<int>(i));
                                }
                            }
                        }
                        g_unitFilterDirty = false;
                    }

                    const bool filtered = g_unitSearch[0] != '\0';
                    const int visible = filtered
                        ? static_cast<int>(g_unitFiltered.size())
                        : static_cast<int>(g_unitCache.size());

                    ImGui::BeginChild("##unit_list", UiSize(500.0f, 420.0f), true);
                    if (visible == 0) {
                        ImGui::TextDisabled("No Units match \"%s\".", g_unitSearch);
                    } else {
                        ImGuiListClipper clipper;
                        clipper.Begin(visible);
                        while (clipper.Step()) {
                            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                                const std::size_t i = filtered
                                    ? static_cast<std::size_t>(g_unitFiltered[static_cast<std::size_t>(row)])
                                    : static_cast<std::size_t>(row);
                                const auto& info = g_unitCache[i];
                                char label[320]{};
                                const char* displayName =
                                    (info.runnableName[0] && _stricmp(info.runnableName, "null") != 0)
                                        ? info.runnableName
                                        : (info.storyName[0] ? info.storyName
                                                             : (info.unitName[0] ? info.unitName : "<unnamed>"));
                                std::snprintf(label, sizeof(label), "%s  [Unit %u]##unit_%zu",
                                              displayName,
                                              static_cast<unsigned int>(info.unitId), i);
                                if (ImGui::Selectable(label, false)) {
                                    if (Features::RequestUnitLoad(info.unitId, info.unitName, info.runnableName))
                                        ImGui::CloseCurrentPopup();
                                }

                                if (ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip(
                                        "Release: %s\nSprint: %s\nStory: %s\nUnit ID: %u\nUnit: %s\nRunnable ID: %u\nRunnable: %s",
                                        info.releaseName[0] ? info.releaseName : "<unnamed>",
                                        info.sprintName[0] ? info.sprintName : "<unnamed>",
                                        info.storyName[0] ? info.storyName : "<unnamed>",
                                        static_cast<unsigned int>(info.unitId),
                                        info.unitName[0] ? info.unitName : "<resolved when loading>",
                                        info.runnableId,
                                        info.runnableName[0] ? info.runnableName : "<none>");
                                }
                            }
                        }
                    }
                    ImGui::EndChild();
                    ImGui::TextDisabled("%d launch target%s", visible, visible == 1 ? "" : "s");
                }
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Draw")) {
            if (!Features::DrawToolsAvailable()) {
                ImGui::TextDisabled("World debug drawing is unavailable for this executable build.");
            } else {
                auto statusLine = [](const char* text) {
                    if (text && text[0]) {
                        ImGui::Indent();
                        ImGui::TextDisabled("%s", text);
                        ImGui::Unindent();
                    }
                };
                // (v0.9.486, from Menu > General) the distances, under the checkboxes that use them
                auto instancesDistance = [] {
                    float distance = Features::DrawInstancesDistance();
                    ImGui::SetNextItemWidth(UiPx(120.0f));
                    if (ImGui::InputFloat("Distance##instances", &distance, 5.0f, 25.0f, "%.1f"))
                        Features::SetDrawInstancesDistance(distance);
                    if (ImGui::IsItemDeactivatedAfterEdit()) Settings::Save();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("How far from the camera Instance names and trigger volumes are drawn.\n"
                                          "0 = unlimited. Lower values reduce clutter.");
                };
                auto markupDistance = [] {   // (v0.9.597: the markup's own; Draw collision, which shared it, removed)
                    float distance = Features::DrawMarkupDistance();
                    ImGui::SetNextItemWidth(UiPx(120.0f));
                    if (ImGui::InputFloat("Distance##markup", &distance, 25.0f, 100.0f, "%.1f"))
                        Features::SetDrawMarkupDistance(distance);
                    if (ImGui::IsItemDeactivatedAfterEdit()) Settings::Save();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("How far from the camera markup is drawn.\n"
                                          "0 = unlimited. Large values can make a dense overlay.");
                };
                if (ImGui::Checkbox("Draw instances", &g_drawInstances))
                    Features::SetDrawInstances(g_drawInstances);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Draw the name of each Instance near the camera (the nearest ones first; farther "
                                      "ones fainter). Close ones also show their Instance ID, object ID, stream unit "
                                      "and distance. The distance is set below it.");
                if (g_drawInstances) {
                    ImGui::Indent();
                    instancesDistance();
                    ImGui::SetNextItemWidth(UiPx(120.0f));
                    ImGui::SliderInt("Labels at most", &g_drawInstanceLabelLimit, 10, 1000);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("How many Instance names are drawn: the nearest ones. Fewer is faster and "
                                          "easier to read.");
                    ImGui::Checkbox("Details close by", &g_drawInstanceDetails);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Instances within 10 m also show their Instance ID (#), object ID, stream "
                                          "unit and distance.");
                    ImGui::Unindent();
                    statusLine(Features::DrawInstancesStatus());
                }

                if (ImGui::Checkbox("Draw triggers", &g_drawTriggers))
                    Features::SetDrawTriggers(g_drawTriggers);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Draw the volume of each TriggerComponent within the Draw instances distance: "
                                      "orange enabled, gray disabled, green while Lara is inside. Close ones show "
                                      "their Instance, size and state.");
                if (g_drawTriggers) {
                    ImGui::Indent();
                    if (!g_drawInstances) instancesDistance();   // (the same distance as the Instance names)
                    ImGui::Checkbox("Trigger labels", &g_drawTriggerLabels);
                    ImGui::Unindent();
                    statusLine(Features::DrawTriggersStatus());
                }

                if (!Features::DrawMarkupAvailable()) ImGui::BeginDisabled();
                if (ImGui::Checkbox("Draw markup", &g_drawMarkup)) {
                    Features::SetDrawMarkup(g_drawMarkup);
                    g_drawMarkup = Features::DrawMarkup();
                }
                if (!Features::DrawMarkupAvailable()) ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(
                        "Draw ROTTR's live traversal markup (ledges, perches, ladders, balance beams, poles, ropes, "
                        "ziplines, cover and the rest), one color a type, faint when the game has it disabled. Close "
                        "ones are labeled with their type. The distance is set below it.");
                if (g_drawMarkup) {
                    ImGui::Indent();
                    markupDistance();
                    ImGui::Checkbox("Type labels", &g_drawMarkupLabels);
                    ImGui::SameLine();
                    ImGui::Checkbox("Legend", &g_drawMarkupLegend);
                    ImGui::Unindent();
                    statusLine(Features::DrawMarkupStatus());
                }

                if (!Features::DrawMarkupAvailable()) {
                    ImGui::Separator();
                    ImGui::TextDisabled("%s", Features::DrawToolsStatus());
                }
            }
            DrawWireframeOptions();   // (v0.9.607: from General > Graphics; DirectX 12's, not the draw tools'; v1.0.0: no separator)
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Outfits")) {
            if (!Features::OutfitControlsAvailable()) {
                ImGui::TextDisabled("Outfit switching is unavailable for this executable build.");
            } else {
                const bool inCutscene = Features::CutscenePlaying();   // (v0.9.440: no switch while one plays)
                const bool busy = Features::OutfitSwapInProgress() || inCutscene;
                const std::uint16_t currentObject = Features::OutfitSwapInProgress() ? 0 : Features::CurrentOutfitObjectId();
                const int outfitCount = Features::OutfitCount();
                if (currentObject)
                    ImGui::TextDisabled("Current player object: %u", static_cast<unsigned int>(currentObject));
                else
                    ImGui::TextDisabled("Current player object: unavailable");
                if (inCutscene)
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                                       "Not during a cutscene (F9 too): switching respawns Lara, which ends it.");
                ImGui::Separator();

                bool noCutsceneOutfitChanges = Features::NoCutsceneOutfitChangesEnabled();
                const bool noCutsceneOutfitChangesAvailable = Features::NoCutsceneOutfitChangesAvailable();
                if (!noCutsceneOutfitChangesAvailable) ImGui::BeginDisabled();
                if (ImGui::Checkbox("No cutscene outfit changes", &noCutsceneOutfitChanges)) {
                    Features::SetNoCutsceneOutfitChangesEnabled(noCutsceneOutfitChanges);
                    Settings::Save();
                }
                if (!noCutsceneOutfitChangesAvailable) ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip(
                        "Lara keeps the outfit you chose when the story would change it (the level scripts that put\n"
                        "her in another outfit around cutscenes). Outfits chosen at a campsite or here still apply.%s",
                        noCutsceneOutfitChangesAvailable ? "" : "\nThe required level-script hook is unavailable for this executable.");
                }
                ImGui::Separator();

                if (outfitCount <= 0) {
                    ImGui::TextDisabled("Outfit data is not currently loaded.");
                } else {
                    // (v0.9.439) the list's search: the outfits whose object name (or object ID) has the text, any
                    // case; the field has the keyboard when the menu opens, and keeps its text for the session
                    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                    ImGui::SetNextItemWidth(UiPx(260.0f));
                    ImGui::InputTextWithHint("##outfit_search", "Search by object name or ID...", g_outfitSearch,
                                             sizeof(g_outfitSearch));
                    if (g_outfitSearch[0]) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Clear##outfit_search_clear")) g_outfitSearch[0] = '\0';
                    }
                    int shown = 0;
                    for (int i = 0; i < outfitCount; ++i) {
                        Features::OutfitInfo info{};
                        if (!Features::GetOutfitInfo(i, info)) continue;
                        if (g_outfitSearch[0]) {
                            char idText[16];
                            std::snprintf(idText, sizeof(idText), "%u", static_cast<unsigned int>(info.playerObjectId));
                            if (!ContainsNoCase(info.objectName, g_outfitSearch) && !ContainsNoCase(idText, g_outfitSearch))
                                continue;
                        }
                        ++shown;

                        const bool selected = currentObject != 0 && currentObject == info.playerObjectId;
                        const bool enabled = !busy && !selected;
                        char label[160];
                        std::snprintf(label, sizeof(label), "%s##outfit_%u", info.objectName, info.outfitIndex);
                        if (ImGui::MenuItem(label, nullptr, selected, enabled))
                            Features::RequestOutfitSwap(info.outfitIndex);

                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                            ImGui::SetTooltip(
                                "Object: %s\nObject ID: %u\nOutfit table index: %u\nCharacter profile: %d%s",
                                info.objectName,
                                static_cast<unsigned int>(info.playerObjectId),
                                info.outfitIndex,
                                info.characterProfileIndex,
                                selected ? "\nCurrently equipped" : "");
                        }
                    }
                    if (!shown) ImGui::TextDisabled("No outfit's object name or ID has \"%s\".", g_outfitSearch);
                }

                // [Toggles] of the worn custom outfit (outfit.ini).
                if (const char* outfitName = CustomOutfits::WornOutfitName()) {
                    const int toggles = CustomOutfits::WornToggleCount();
                    if (toggles > 0) {
                        ImGui::Separator();
                        ImGui::TextDisabled("%s", outfitName);
                        for (int i = 0; i < toggles; ++i) {
                            CustomOutfits::ToggleInfo toggle{};
                            if (!CustomOutfits::GetWornToggle(i, toggle)) continue;
                            char label[160];
                            std::snprintf(label, sizeof(label), "%s##outfit_toggle_%d", toggle.name, i);
                            bool on = toggle.on;
                            if (ImGui::Checkbox(label, &on)) CustomOutfits::SetWornToggle(i, on);
                            if (!toggle.hides.empty() && ImGui::IsItemHovered())   // (v0.9.454)
                                ImGui::SetTooltip("Never shown with %s: showing this hides %s.", toggle.hides.c_str(),
                                                  toggle.hides.find(", ") == std::string::npos ? "it" : "them");
                        }
                    }
                }
            }
            ImGui::EndMenu();
        }



        // Hair: opens the PureHair editor
        {
            const bool hairAvailable = Features::HairControlsAvailable();
            if (ImGui::MenuItem("Hair", nullptr, g_showHairEditor, hairAvailable)) {
                g_showHairEditor = !g_showHairEditor;
                Features::SetHairEditorActive(g_showHairEditor);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", hairAvailable ? "Open the PureHair editor (Lara's hair look and motion)."
                                                      : "PureHair editing is unavailable for this executable build.");
        }

        if (ImGui::BeginMenu("Camera")) {
            bool freeCam = Features::FreeCameraEnabled();
            if (!Features::FreeCameraAvailable()) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Free camera", nullptr, freeCam)) {
                Overlay::SetFreeCameraEnabled(!freeCam);
            }
            if (!Features::FreeCameraAvailable()) ImGui::EndDisabled();
            float freeCamSpeed = Features::FreeCameraMoveSpeed();
            if (!Features::FreeCameraAvailable()) ImGui::BeginDisabled();
            ImGui::SetNextItemWidth(UiPx(190.0f));
            if (ImGui::SliderFloat("Freecam speed", &freeCamSpeed, 0.5f, 1024.0f, "%.1f"))
                Features::SetFreeCameraMoveSpeed(freeCamSpeed);

            float freeCamSensitivity = Features::FreeCameraMouseSensitivity() * 1000.0f;
            ImGui::SetNextItemWidth(UiPx(190.0f));
            if (ImGui::SliderFloat("Mouse sensitivity", &freeCamSensitivity, 0.5f, 8.0f, "%.2f"))
                Features::SetFreeCameraMouseSensitivity(freeCamSensitivity / 1000.0f);
            if (!Features::FreeCameraAvailable()) ImGui::EndDisabled();
            ImGui::TextDisabled("Freecam FOV: %.2fx", Features::FreeCameraFovMultiplier());
            ImGui::TextDisabled("Player input: %s",
                                Overlay::PlayerInputLocked() ? "LOCKED" : "UNLOCKED");
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("World")) {
            bool paused = Features::GamePaused();
            if (!Features::PauseAvailable()) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Pause game", nullptr, paused))
                Features::SetGamePaused(!paused);
            if (!Features::PauseAvailable()) ImGui::EndDisabled();

            float timeScale = Features::TimeScale();
            if (!Features::TimeControlsAvailable()) ImGui::BeginDisabled();
            ImGui::SetNextItemWidth(UiPx(190.0f));
            if (ImGui::SliderFloat("Time scale", &timeScale, 0.05f, 4.00f, "%.2fx"))
                Features::SetTimeScale(timeScale);
            if (ImGui::MenuItem("Reset time scale"))
                Features::SetTimeScale(1.0f);
            if (!Features::TimeControlsAvailable()) ImGui::EndDisabled();

            ImGui::Separator();
            bool hidden = Features::HudHidden();
            if (!Features::HudControlsAvailable()) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Hide UI", "F6", hidden))
                Features::SetHudHidden(!hidden);
            if (!Features::HudControlsAvailable()) ImGui::EndDisabled();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Settings")) {
            if (ImGui::BeginMenu("UI scale")) {
                struct UiScaleOption { const char* label; float value; };
                static constexpr UiScaleOption kUiScaleOptions[] = {
                    {"100%", 1.00f},
                    {"125%", 1.25f},
                    {"150%", 1.50f},
                    {"175%", 1.75f},
                    {"200%", 2.00f},
                    {"250%", 2.50f},
                };
                for (const auto& option : kUiScaleOptions) {
                    const bool selected = std::fabs(Settings::Get().uiScale - option.value) < 0.001f;
                    if (ImGui::MenuItem(option.label, nullptr, selected)) {
                        Settings::Get().uiScale = option.value;
                        Settings::Get().uiScaleSaved = true;   // (v1.0.0: the user's choice from now on)
                        Settings::Save();
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Theme")) {
                static constexpr const char* kThemeNames[] = {"Dark", "Light", "Classic"};
                for (int theme = 0; theme < 3; ++theme) {
                    if (ImGui::MenuItem(kThemeNames[theme], nullptr, Settings::Get().uiTheme == theme)) {
                        Settings::Get().uiTheme = theme;
                        Settings::Save();
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Debug")) {
            ImGui::MenuItem("Diagnostics", nullptr, &g_showDiagnostics);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            ImGui::MenuItem("About", nullptr, &g_showAbout);
            ImGui::Separator();
            DrawHotkeyLines(true);   // (v1.0.0; F10, the Foundation Engine editor's freeze, no longer listed)
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
    Features::SetUnitSelectorActive(unitSelectorOpen);

    DrawPersistentMenuWindow();
    DrawHairEditorWindow();
    DrawLightEditorWindow();
    DrawMaterialEditorWindow();
    DrawInstanceBrowserWindow();
    SyncInstanceSelectionHighlight();
    SyncInstanceHoverHighlight();
    Features::SetClothEditorActive(g_showInstanceBrowser && g_instanceClothTabActiveThisFrame);
    Features::SetCapsuleCollisionEditorActive(g_showInstanceBrowser && g_instanceCollisionTabActiveThisFrame);
    Features::SetMarkerEditorActive(g_showInstanceBrowser && g_instanceMarkerTabActiveThisFrame);
    Features::SetHairEditorActive(g_showHairEditor);
    Features::SetLightEditorActive(g_showLightEditor);
    Features::SetMaterialEditorActive(g_showMaterialEditor || SuiteLink::WantsMaterials());
    Features::SetInstanceBrowserActive(g_showInstanceBrowser);
    DrawDiagnosticsWindow();
    DrawAboutWindow();
}

bool WorldOverlayActive() {
    if (Grapple::OverlayActive()) return true;
    const bool drawSelectedCapsule =
        g_showInstanceBrowser && g_instanceCollisionTabActiveThisFrame && g_drawSelectedCapsuleCollision;
    return g_drawInstances || g_drawTriggers || g_drawMarkup || drawSelectedCapsule ||
           LightMarkerActive() || RtProbe::Active();
}

void DrawWorldOverlay() {
    if (const std::uint64_t view = RtProbe::ViewTexture()) {   // (v0.9.543) the ray tracing probe's view, under the rest
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        if (RtProbe::Mode() == 2) {
            draw->AddImage(static_cast<ImTextureID>(view), ImVec2(size.x * 0.5f, 0.0f), size, ImVec2(0.5f, 0.0f), ImVec2(1.0f, 1.0f));
            draw->AddLine(ImVec2(size.x * 0.5f, 0.0f), ImVec2(size.x * 0.5f, size.y), IM_COL32(255, 255, 255, 160), 2.0f);
        } else if (RtProbe::Mode() == 3) {   // (v0.9.544) over the game's image, half see-through
            draw->AddImage(static_cast<ImTextureID>(view), ImVec2(0.0f, 0.0f), size, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                           IM_COL32(255, 255, 255, 128));
        } else {
            draw->AddImage(static_cast<ImTextureID>(view), ImVec2(0.0f, 0.0f), size);
        }
    }
    Grapple::DrawOverlay();  // the grapple rope and the target marker (its own camera snapshot)
    DrawLightMarker();   // (the Light Editor's chosen light)
    const bool drawSelectedCapsule =
        g_showInstanceBrowser && g_instanceCollisionTabActiveThisFrame && g_drawSelectedCapsuleCollision;
    if (!g_drawInstances && !g_drawTriggers && !g_drawMarkup && !drawSelectedCapsule) return;
    if (!Features::DrawToolsAvailable()) return;

    Features::DrawCameraState camera{};
    if (!Features::GetDrawCameraState(camera)) return;
    DrawCameraBasis basis{};
    if (!BuildDrawCameraBasis(camera, basis)) return;

    RefreshDrawOverlayCaches();
    if (drawSelectedCapsule) {
        const auto version = Features::CapsuleCollisionDrawVersion();
        if (version != g_capsuleCollisionDrawCacheVersion) {
            const int count = Features::CapsuleCollisionDrawCount();
            g_capsuleCollisionDrawCache.resize(static_cast<std::size_t>(std::max(0, count)));
            const int copied = count > 0
                ? Features::CopyCapsuleCollisionDraw(g_capsuleCollisionDrawCache.data(), count)
                : 0;
            g_capsuleCollisionDrawCache.resize(static_cast<std::size_t>(std::max(0, copied)));
            g_capsuleCollisionDrawCacheVersion = version;
        }
    } else if (!g_capsuleCollisionDrawCache.empty()) {
        g_capsuleCollisionDrawCache.clear();
        g_capsuleCollisionDrawCacheVersion = ~std::uint64_t{0};
    }

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    if (!draw) return;

    if (drawSelectedCapsule) {
        const ImU32 capsuleColor = IM_COL32(255, 220, 70, 245);
        for (const auto& capsule : g_capsuleCollisionDrawCache)
            DrawCapsuleCollisionWireframe(draw, basis, capsule, capsuleColor);
    }

    if (g_drawMarkup) {
        // one color a markup type; disabled boxes faint; the type's name over the nearest boxes
        std::uint32_t lastBox = ~0u;
        int labels = 0;
        for (const auto& line : g_drawMarkupCache) {
            ImU32 color = MarkupTypeColor(line.type);
            if (line.flags & Features::kDrawMarkupDisabled) color = (color & 0x00FFFFFFu) | (80u << 24);
            ImVec2 a{}, b{};
            const bool visible = ProjectDrawLine(basis, line.a, line.b, a, b);
            if (visible) draw->AddLine(a, b, color, 2.0f);
            if (line.box != lastBox) {
                lastBox = line.box;
                if (g_drawMarkupLabels && visible && labels < 80) {
                    const float dx = line.a[0] - basis.pos[0], dy = line.a[1] - basis.pos[1], dz = line.a[2] - basis.pos[2];
                    if (dx * dx + dy * dy + dz * dz < 1200.0f * 1200.0f) {
                        char text[64];
                        std::snprintf(text, sizeof(text), "%s%s%s", Features::DrawMarkupTypeName(line.type),
                                      (line.flags & Features::kDrawMarkupDynamic) ? " (moving)" : "",
                                      (line.flags & Features::kDrawMarkupDisabled) ? " (off)" : "");
                        DrawShadowedText(draw, ImVec2(a.x + 4.0f, a.y - 16.0f), color | 0xFF000000u, text);
                        ++labels;
                    }
                }
            }
        }
        if (g_drawMarkupLegend) DrawMarkupLegend();
    }

    if (g_drawTriggers) {
        static constexpr int edges[12][2] = {
            {0,1},{1,3},{3,2},{2,0},
            {4,5},{5,7},{7,6},{6,4},
            {0,4},{1,5},{2,6},{3,7}
        };
        Features::PlayerTransform player{};
        const bool havePlayer = Features::GetPlayerTransform(player);
        const float maxDistance = Features::DrawInstancesDistance();
        int labels = 0;
        for (const auto& box : g_drawTriggerCache) {
            float distSq = 0.0f;
            for (int i = 0; i < 3; ++i) {
                const float p = basis.pos[i];
                const float d = p < box.min[i] ? box.min[i] - p : (p > box.max[i] ? p - box.max[i] : 0.0f);
                distSq += d * d;
            }
            if (maxDistance > 0.0f && distSq > maxDistance * maxDistance) continue;
            const bool inside = havePlayer &&
                player.position[0] >= box.min[0] && player.position[0] <= box.max[0] &&
                player.position[1] >= box.min[1] && player.position[1] <= box.max[1] &&
                player.position[2] >= box.min[2] && player.position[2] <= box.max[2];
            const ImU32 color = inside ? IM_COL32(90, 255, 120, 240)
                              : box.enabled ? IM_COL32(255, 170, 40, 225)
                                            : IM_COL32(150, 150, 150, 150);
            float corners[8][3]{};
            for (int i = 0; i < 8; ++i) {
                corners[i][0] = (i & 1) ? box.max[0] : box.min[0];
                corners[i][1] = (i & 2) ? box.max[1] : box.min[1];
                corners[i][2] = (i & 4) ? box.max[2] : box.min[2];
            }
            for (const auto& edge : edges) {
                ImVec2 a{}, b{};
                if (ProjectDrawLine(basis, corners[edge[0]], corners[edge[1]], a, b))
                    draw->AddLine(a, b, color, inside ? 2.5f : 1.5f);
            }
            if (g_drawTriggerLabels && labels < 40 && distSq < 1500.0f * 1500.0f) {
                const float top[3]{(box.min[0] + box.max[0]) * 0.5f, (box.min[1] + box.max[1]) * 0.5f, box.max[2]};
                ImVec2 at{};
                if (ProjectDrawPoint(basis, top, at, true)) {
                    char text[192];
                    std::snprintf(text, sizeof(text), "%s  #%u\n%.1f x %.1f x %.1f m  %s", box.owner[0] ? box.owner : "?",
                                  box.slot, (box.max[0] - box.min[0]) / 100.0f, (box.max[1] - box.min[1]) / 100.0f,
                                  (box.max[2] - box.min[2]) / 100.0f,
                                  inside ? "Lara inside" : box.enabled ? "enabled" : "disabled");
                    DrawShadowedText(draw, ImVec2(at.x + 4.0f, at.y), color | 0xFF000000u, text);
                    ++labels;
                }
            }
        }
    }

    if (g_drawInstances) {
        // the nearest labels only (the limit), farther ones fainter, details (ID, object, unit, distance) close by
        struct Candidate {
            float distSq;
            const Features::DrawInstanceLabel* label;
            ImVec2 screen;
        };
        static std::vector<Candidate> candidates;
        candidates.clear();
        for (const auto& label : g_drawInstanceCache) {
            ImVec2 pos{};
            if (!ProjectDrawPoint(basis, label.position, pos, true)) continue;
            const float dx = label.position[0] - basis.pos[0], dy = label.position[1] - basis.pos[1],
                        dz = label.position[2] - basis.pos[2];
            candidates.push_back({dx * dx + dy * dy + dz * dz, &label, pos});
        }
        const std::size_t limit = std::min<std::size_t>(candidates.size(), static_cast<std::size_t>(std::max(1, g_drawInstanceLabelLimit)));
        std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(limit), candidates.end(),
                          [](const Candidate& a, const Candidate& b) { return a.distSq < b.distSq; });
        const float maxDistance = std::max(1.0f, Features::DrawInstancesDistance());
        for (std::size_t i = limit; i-- > 0;) {   // (farthest first: the nearest on top)
            const auto& c = candidates[i];
            const float distance = std::sqrt(c.distSq);
            const float fade = std::clamp(1.0f - distance / maxDistance, 0.0f, 1.0f);
            const auto alpha = static_cast<unsigned int>(110.0f + 145.0f * fade);
            const ImU32 color = IM_COL32(255, 255, 255, alpha);
            DrawShadowedText(draw, c.screen, color, c.label->name);
            if (g_drawInstanceDetails && distance < 1000.0f) {
                char text[128];
                std::snprintf(text, sizeof(text), "#%u  object %u  unit %u  %.1f m", c.label->slot, c.label->objectId,
                              static_cast<unsigned int>(c.label->streamUnit), distance / 100.0f);
                DrawShadowedText(draw, ImVec2(c.screen.x, c.screen.y + ImGui::GetTextLineHeight()),
                                 IM_COL32(190, 220, 255, alpha), text);
            }
        }
    }
}
}
