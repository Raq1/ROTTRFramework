#include "swim_breath.h"
#include "features.h"
#include "game_build.h"
#include "log.h"
#include "overlay.h"
#include "settings.h"
#include <Windows.h>
#include <Xinput.h>
#include <MinHook.h>
#include <imgui.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>

// Underwater swimming in ROTTR has no free depth: every swim state runs the activity
// PSCAF_P_Move_AdjustWadeSpeed, which pulls Lara to a target plane
// min(water surface, ceiling probe) - treadDepth, and while Dive is held (StateControl property
// Water_HoldBreath) only lowers that plane under obstacles ahead. The anim graph fakes the depth
// (-20 cm model offset + pitch from SwimTiltAngle). While submerged we skip that target-plane block and
// tilt the swim velocity along the camera instead (plus optional up/down keys). Releasing Dive normally
// sends the "Sub-State Modifiers" agent back to swim_surface (transition "NOT Requested Swim_Dive");
// while Lara is clearly under water we answer that condition with "held", so she only returns to surface
// swimming when she comes back up to the surface.
// The swim boost on PC is "Requested Swim_Forward" (B on a gamepad, primary states
// swim_surface_walk -> swim_surface_walk_boost); under water we answer it with RT instead.
// Her body pitch comes from the swim graph's "Pitch" curve (SwimTiltAngle, clamped to +-30 degrees), which
// we widen while swimming freely, and her turn speed from PlayerSteering::CalculateTurnRate.
// Breath lives in Player::m_breath (seconds); its maximum is PlayerProp dBreathDuration.

extern "C" {
// Shared with SwimVerticalStub (swim_breath_hooks.asm). Written on the game thread by
// HookAdjustWadeSpeed right before the original runs, cleared right after it returns.
volatile std::uint8_t g_swimOverrideActive = 0;
float g_swimSinPitch = 0.0f;
float g_swimCosMinusOne = 0.0f;
float g_swimCamFwdX = 0.0f;
float g_swimCamFwdY = 1.0f;
float g_swimVzExtra = 0.0f;
float g_swimVzMin = 0.0f;
float g_swimVzMax = 0.0f;
void* g_swimVerticalTrampoline = nullptr;
void* g_swimVerticalResume = nullptr;
void SwimVerticalStub();
}

namespace {
constexpr std::uintptr_t kRvaAdjustWadeSpeed           = 0x0069FC20ull; // PSCAF_P_Move_AdjustWadeSpeed(SCAgent*)
constexpr std::uintptr_t kRvaAdjustWadeSpeedComponents = 0x0069FC68ull; // Player/PlayerProp from PlayerComponent
constexpr std::uintptr_t kRvaAdjustWadeSpeedWaterVol   = 0x0069FD88ull; // Player::currentWaterVolume
constexpr std::uintptr_t kRvaAdjustWadeSpeedSurfaceZ   = 0x0069FF0Cull; // msSwimData surface Z
constexpr std::uintptr_t kRvaAdjustWadeSpeedTread      = 0x0069FF50ull; // msSwimData treadDepth
constexpr std::uintptr_t kRvaAdjustWadeSpeedDesiredCopy = 0x006A0266ull; // movaps [rsp+60h],xmm6
constexpr std::uintptr_t kRvaAdjustWadeSpeedDeepBranch = 0x006A0289ull; // ja -> deep-water block
constexpr std::uintptr_t kRvaAdjustWadeSpeedDeepBlock  = 0x006A046Eull; // target-plane math (hooked)
constexpr std::uintptr_t kRvaAdjustWadeSpeedResume     = 0x006A0745ull; // stores xmm7 as desired.z
constexpr std::uintptr_t kRvaPlayerProcessSwimming     = 0x00842D10ull; // Player::ProcessSwimming(bool)
constexpr std::uintptr_t kRvaProcessSwimmingInstance   = 0x00842D2Dull;
constexpr std::uintptr_t kRvaProcessSwimmingProp       = 0x00842D4Bull;
constexpr std::uintptr_t kRvaSwimBreathEntryExtra      = 0x0070D195ull; // SwimBreathCheckEntry: +dExtraBreathDuration
constexpr std::uintptr_t kRvaSwimBreathProcessProps    = 0x0070D2E2ull; // dBreathDuration / dBreathRefillTime
constexpr std::uintptr_t kRvaSwimBreathDrain           = 0x0070D405ull; // m_breath -= deltaMult * dt
constexpr std::uintptr_t kRvaActionRequested          = 0x006A0A70ull; // CSCCF_ActionRequested (SC "Requested")
constexpr std::uintptr_t kRvaScalarCurveGetAnimData    = 0x0012CC50ull; // cdc::AnimScalarCurveNode::GetAnimData
constexpr std::uintptr_t kRvaCalculateTurnRate         = 0x0081AC50ull; // PlayerSteering::CalculateTurnRate
constexpr std::uintptr_t kRvaSteeringRateSetTurnRate   = 0x006BC0F2ull; // PSCPF_Turn_SteeringRate: Player+0x3430
constexpr std::uintptr_t kRvaSCAgentGetBoolProp        = 0x006DBD80ull;
constexpr std::uintptr_t kRvaGetLocalPlayerInstance    = 0x00403BF0ull;
constexpr std::uintptr_t kRvaPlayerComponentId         = 0x010FAA90ull; // ComponentArrayID<PlayerComponent>::s_id

constexpr std::uint8_t kAdjustWadeSpeedSig[] = {
    0x40,0x55,0x53,0x56,0x57,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xAC,0x24,0xE0,0xFE,0xFF,0xFF,
    0x48,0x81,0xEC,0x20,0x02,0x00,0x00,0x48,0x8B,0x71,0x10,0x48,0x8B,0xD9
};
constexpr std::uint8_t kAdjustWadeSpeedComponentsSig[] = {
    0x4D,0x8B,0xB8,0xA8,0x02,0x00,0x00,0x4D,0x8B,0xA8,0xB0,0x02,0x00,0x00
};
constexpr std::uint8_t kAdjustWadeSpeedWaterVolSig[] = {0x49,0x8B,0x87,0xE0,0x1A,0x00,0x00};
constexpr std::uint8_t kAdjustWadeSpeedSurfaceZSig[] = {0xF3,0x45,0x0F,0x10,0x87,0x2C,0x1B,0x00,0x00};
constexpr std::uint8_t kAdjustWadeSpeedTreadSig[] = {0x41,0x0F,0xBF,0x87,0x06,0x1B,0x00,0x00};
constexpr std::uint8_t kAdjustWadeSpeedDesiredCopySig[] = {0x0F,0x29,0x74,0x24,0x60};
constexpr std::uint8_t kAdjustWadeSpeedDeepBranchSig[] = {0x0F,0x87,0xC0,0x01,0x00,0x00};
constexpr std::uint8_t kAdjustWadeSpeedDeepBlockSig[] = {
    0xF3,0x45,0x0F,0x5C,0xC7,0x44,0x8B,0xC7,0x48,0x8D,0x54,0x24,0x40,
    0x49,0x8D,0x8D,0x50,0x33,0x00,0x00,0xF3,0x44,0x0F,0x5C,0x46,0x18
};
constexpr std::uint8_t kAdjustWadeSpeedResumeSig[] = {
    0x4C,0x8B,0xBD,0x78,0x01,0x00,0x00,0xF3,0x0F,0x11,0x7C,0x24,0x58,0x0F,0x28,0x7C,0x24,0x50
};
constexpr std::uint8_t kProcessSwimmingSig[] = {
    0x48,0x89,0x5C,0x24,0x18,0x57,0x48,0x83,0xEC,0x40,0xF6,0x81,0x80,0x00,0x00,0x00,0x08,
    0x0F,0xB6,0xDA,0x48,0x8B,0xF9
};
constexpr std::uint8_t kProcessSwimmingInstanceSig[] = {0x48,0x8B,0x89,0xA8,0x31,0x00,0x00};
constexpr std::uint8_t kProcessSwimmingPropSig[] = {0x49,0x8B,0xB0,0xB0,0x02,0x00,0x00};
constexpr std::uint8_t kSwimBreathEntryExtraSig[] = {0xF3,0x0F,0x58,0x87,0xCC,0x01,0x00,0x00};
constexpr std::uint8_t kSwimBreathProcessPropsSig[] = {
    0xF3,0x0F,0x10,0xB6,0xB8,0x01,0x00,0x00,0xF3,0x44,0x0F,0x10,0x86,0xBC,0x01,0x00,0x00
};
constexpr std::uint8_t kSwimBreathDrainSig[] = {
    0xF3,0x0F,0x10,0x8F,0x40,0x1B,0x00,0x00,0xF3,0x0F,0x59,0xC7,0xF3,0x0F,0x5C,0xC8,
    0xF3,0x0F,0x11,0x8F,0x40,0x1B,0x00,0x00
};
constexpr std::uint8_t kSCAgentGetBoolPropSig[] = {
    0x4D,0x8B,0xD0,0x4C,0x8B,0xC1,0x48,0x0F,0xBF,0xCA,0x48,0xC1,0xE1,0x07,0x66,0x90,
    0x42,0x80,0xBC,0x01,0x45,0x1C,0x00,0x00,0x00
};
constexpr std::uint8_t kGetLocalPlayerInstanceSig[] = {0x40,0x53,0x48,0x83,0xEC,0x20,0x0F,0xB6,0xD9};
constexpr std::uint8_t kActionRequestedSig[] = {
    0x40,0x53,0x48,0x83,0xEC,0x30,0x48,0x89,0x6C,0x24,0x48,0x32,0xDB,0x48,0x89,0x74,0x24,0x50,
    0xBD,0xFF,0xFF,0x00,0x00,0x48,0x89,0x7C,0x24,0x58,0x48,0x8B,0x79,0x10
};
constexpr std::uint8_t kScalarCurveGetAnimDataSig[] = {
    0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x40,0x48,0x8B,0x41,0x38,0x48,0x8B,0xDA,0x48,0x8B,0xF9,
    0xF3,0x0F,0x10,0x80,0x90,0x01,0x00,0x00,0x0F,0x2E,0x80,0x94,0x01,0x00,0x00
};
constexpr std::uint8_t kCalculateTurnRateSig[] = {
    0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x81,0xEC,0x80,0x00,0x00,0x00,0x0F,0x29,0x7C,0x24,0x60,0x48,0x8B,0xFA,
    0x44,0x0F,0x29,0x44,0x24,0x50,0x0F,0x57,0xFF,0x44,0x0F,0x28,0xC2,0x48,0x8B,0xD9
};
constexpr std::uint8_t kSteeringRateSetTurnRateSig[] = {0x48,0x8D,0x8D,0x30,0x34,0x00,0x00};

constexpr std::ptrdiff_t kAgentInstanceOffset          = 0x10;
constexpr std::ptrdiff_t kInstancePositionZOffset      = 0x18;
constexpr std::ptrdiff_t kInstanceComponentCountOffset = 0xD0;
constexpr std::ptrdiff_t kInstanceComponentsOffset     = 0xD8;
constexpr std::ptrdiff_t kPlayerComponentPlayerOffset  = 0x2A8;
constexpr std::ptrdiff_t kPlayerComponentPropOffset    = 0x2B0;
constexpr std::ptrdiff_t kPlayerInstanceOffset         = 0x31A8;
constexpr std::ptrdiff_t kPlayerWaterVolumeOffset      = 0x1AE0;
constexpr std::ptrdiff_t kPlayerSwimTreadDepthOffset   = 0x1B06; // msSwimData+0x06, s16 cm
constexpr std::ptrdiff_t kPlayerSwimSurfaceZOffset     = 0x1B2C; // msSwimData+0x2C
constexpr std::ptrdiff_t kPlayerBreathOffset           = 0x1B40; // m_breath, seconds
constexpr std::ptrdiff_t kPlayerSteeringOffset         = 0x3430; // Player::m_steering (PlayerSteering)
constexpr std::ptrdiff_t kAnimNodeDtpOffset            = 0x38;   // cdc::AnimScalarCurveNode::m_DTPProperties
constexpr std::ptrdiff_t kPropSwimTuneOffset           = 0x198;  // dtp::PlayerProp::swim_tune_data
constexpr std::ptrdiff_t kPropBreathDurationOffset     = 0x1B8;
constexpr std::ptrdiff_t kPropExtraBreathOffset        = 0x1CC;
constexpr short kSCPropertyWaterHoldBreath             = 0x01AA;
// dtp::SCConditionData: +0 context, +8 test property (u16 compare in the game), +0xC value, +0x10 bit0 NOT.
constexpr std::uint16_t kSCCondPropertyRequested       = 35;   // EnumSCCondProperty_Requested
constexpr std::int32_t kSCCondContextIndirect          = 0x28; // value resolved through GetSCCondInt
constexpr std::int32_t kInputActionSwimDive            = 253;  // INPUTACTION_Swim_Dive
constexpr std::int32_t kInputActionSwimForward         = 255;  // INPUTACTION_Swim_Forward (the boost, B)

constexpr float kPi = 3.14159265358979f;
constexpr float kHalfPi = kPi * 0.5f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kMaxSwimPitch = 80.0f * kDegToRad;
constexpr float kMaxVerticalSpeed = 600.0f;   // cm/s
constexpr float kAbovePlaneSlack = 2.0f;       // cm above the swim plane: let the game settle her back
constexpr float kAtPlaneSlack = 3.0f;          // cm below the swim plane still counts as "at the top"
constexpr float kKeepSubmergedDepth = 30.0f;   // cm below the swim plane: releasing Dive keeps her under
constexpr float kAutoDiveDepth = 60.0f;        // a dive from the surface first takes her this far down
constexpr float kAutoDiveSpeed = 150.0f;       // cm/s
constexpr ULONGLONG kAutoDiveTimeoutMs = 2000;
constexpr ULONGLONG kFreeSwimRecentMs = 300;   // free swim counts as active this long after its last frame
constexpr float kTriggerHeld = 0.2f;           // RT past the XInput threshold and 20% of its travel
constexpr float kStockTiltRange = 30.0f;       // the swim graph's "Pitch" curve: +-30 degrees
constexpr float kMaxBodyTilt = 85.0f;
constexpr float kMaxTurnMultiplier = 4.0f;

using AdjustWadeSpeedFn = void(__fastcall*)(void* agent);
using ActionRequestedFn = bool(__fastcall*)(void* agent, void* memory, const std::uint8_t* condition);
using ScalarCurveGetAnimDataFn = void*(__fastcall*)(void* node, void* out, void* arg3, void* arg4);
using CalculateTurnRateFn = float*(__fastcall*)(void* steering, float* out, float dt);
using ProcessSwimmingFn = void(__fastcall*)(void* player, bool flag);
using SCAgentGetBoolPropFn = void(__fastcall*)(void* agent, short property, bool* out, bool);
using GetLocalPlayerInstanceFn = void*(__fastcall*)(std::uint8_t);
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

std::uintptr_t g_base = 0;
AdjustWadeSpeedFn g_originalAdjustWadeSpeed = nullptr;
ActionRequestedFn g_originalActionRequested = nullptr;
ScalarCurveGetAnimDataFn g_originalScalarCurveGetAnimData = nullptr;
CalculateTurnRateFn g_originalCalculateTurnRate = nullptr;
ProcessSwimmingFn g_originalProcessSwimming = nullptr;
SCAgentGetBoolPropFn g_scAgentGetBoolProp = nullptr;
GetLocalPlayerInstanceFn g_getLocalPlayerInstance = nullptr;
bool g_tickAvailable = false;
bool g_breathAvailable = false;
bool g_freeSwimAvailable = false;
bool g_stayUnderAvailable = false;
bool g_bodyTiltAvailable = false;
bool g_turnAvailable = false;

std::atomic_bool g_freeSwimEnabled{false};   // (v1.0.0: off by default)
std::atomic_bool g_cameraSteering{true};
std::atomic<float> g_cameraDeadzoneDeg{5.0f};
std::atomic_bool g_keysEnabled{true};
std::atomic<int> g_keyUp{'Q'};
std::atomic<int> g_keyDown{'Z'};
std::atomic<float> g_verticalSpeed{250.0f};
std::atomic_bool g_rtBoost{false};   // (v1.0.0: off by default)
std::atomic_bool g_bodyTilt{true};
std::atomic<float> g_bodyTiltMax{80.0f};
std::atomic<float> g_turnMultiplier{2.0f};
std::atomic<float> g_breathMultiplier{1.0f};
std::atomic_bool g_infiniteBreath{false};

// Game thread only.
void* g_localPlayerInstance = nullptr;
XInputGetStateFn g_xinputGetState = nullptr;
bool g_xinputLoadTried = false;
DWORD g_padSlot = XUSER_MAX_COUNT;
ULONGLONG g_padNextScanMs = 0;
bool g_logRtBoostWas = false;
ULONGLONG g_logNextRtBoostMs = 0;
const std::uint8_t* g_loggedOddProp = nullptr;
bool g_logOverrideWasActive = false;
bool g_logSubmergedWas = false;
ULONGLONG g_logNextTraceMs = 0;
bool g_wasSubmerged = false;
bool g_autoDiveActive = false;
ULONGLONG g_autoDiveStartMs = 0;
// Answer "Requested Swim_Dive" with true for Lara. Set from the swim movement activity, which does not
// run in interact states, so the last decision is kept there; cleared out of water.
std::atomic_bool g_keepSubmerged{false};
// Gamepad snapshot, polled on the game thread while Lara is in water.
std::atomic_bool g_padConnected{false};
std::atomic<float> g_padRightTrigger{0.0f};
// Lara's PlayerSteering (Player+0x3430), for the turn-speed hook.
std::atomic<void*> g_localSteering{nullptr};
std::atomic_bool g_tiltCurveSeen{false};
std::atomic_bool g_tiltCurveWriteFailed{false};

// Snapshot for the menu (written on the game thread).
std::atomic<ULONGLONG> g_statusTickMs{0};
std::atomic<ULONGLONG> g_statusOverrideMs{0};
std::atomic_bool g_statusInWater{false};
std::atomic_bool g_statusSubmerged{false};
std::atomic<float> g_statusBreath{0.0f};
std::atomic<float> g_statusBreathMax{0.0f};
std::atomic<float> g_statusBreathOriginal{0.0f};
std::atomic<float> g_statusDepth{0.0f};

// PlayerProp breath values we changed, so a prop that comes back later is not scaled twice.
struct PropRecord {
    std::uint8_t* prop;
    float originalDuration;
    float originalExtra;
    float writtenDuration;
    float writtenExtra;
    bool scaleExtra;  // false when the extra-breath bonus holds no usable value
    bool logged;
};
PropRecord g_propRecords[8]{};
int g_propRecordNext = 0;

template <typename T>
T At(std::uintptr_t rva) {
    return reinterpret_cast<T>(g_base + rva);
}

bool Verify(std::uintptr_t rva, const std::uint8_t* sig, std::size_t size, const char* name) {
    if (GameBuild::VerifyBytes(rva, sig, size)) return true;
    Log::Writef("Swim: %s signature mismatch; related feature disabled", name);
    return false;
}

const char* IniPath() {
    static char path[MAX_PATH]{};
    if (path[0]) return path;
    char root[MAX_PATH]{};
    GetModuleFileNameA(nullptr, root, MAX_PATH);
    char* slash = root;
    for (char* p = root; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p + 1;
    *slash = '\0';
    strcpy_s(path, root);
    strcat_s(path, "ROTTRFramework.ini");
    return path;
}

float IniFloat(const char* key, float fallback) {
    char def[32]{};
    sprintf_s(def, "%.6f", fallback);
    char buf[64]{};
    GetPrivateProfileStringA("Swimming", key, def, buf, sizeof(buf), IniPath());
    const float value = static_cast<float>(std::atof(buf));
    return std::isfinite(value) ? value : fallback;
}

bool IniBool(const char* key, bool fallback) {
    return GetPrivateProfileIntA("Swimming", key, fallback ? 1 : 0, IniPath()) != 0;
}

void IniWriteFloat(const char* key, float value) {
    char buf[32]{};
    sprintf_s(buf, "%.7g", value);
    WritePrivateProfileStringA("Swimming", key, buf, IniPath());
}

void IniWriteInt(const char* key, int value) {
    char buf[32]{};
    sprintf_s(buf, "%d", value);
    WritePrivateProfileStringA("Swimming", key, buf, IniPath());
}

void LoadSettings() {
    g_freeSwimEnabled.store(IniBool("FreeDepthControl", false));   // (v1.0.0: default off)
    g_cameraSteering.store(IniBool("CameraSteering", true));
    g_cameraDeadzoneDeg.store(std::clamp(IniFloat("CameraDeadzone", 5.0f), 0.0f, 45.0f));
    g_keysEnabled.store(IniBool("Keys", true));
    const int up = static_cast<int>(GetPrivateProfileIntA("Swimming", "KeyUp", 'Q', IniPath()));
    const int down = static_cast<int>(GetPrivateProfileIntA("Swimming", "KeyDown", 'Z', IniPath()));
    g_keyUp.store(up > 0 && up < 256 ? up : 'Q');
    g_keyDown.store(down > 0 && down < 256 ? down : 'Z');
    g_verticalSpeed.store(std::clamp(IniFloat("VerticalSpeed", 250.0f), 20.0f, kMaxVerticalSpeed));
    g_rtBoost.store(IniBool("BoostOnRT", false));   // (v1.0.0: default off)
    g_bodyTilt.store(IniBool("BodyTilt", true));
    g_bodyTiltMax.store(std::clamp(IniFloat("BodyTiltMax", 80.0f), kStockTiltRange, kMaxBodyTilt));
    g_turnMultiplier.store(std::clamp(IniFloat("TurnSpeed", 2.0f), 1.0f, kMaxTurnMultiplier));
    g_breathMultiplier.store(std::clamp(IniFloat("BreathMultiplier", 1.0f), 1.0f, 10.0f));
    g_infiniteBreath.store(IniBool("InfiniteBreath", false));
}

void SaveSettings() {
    IniWriteInt("FreeDepthControl", g_freeSwimEnabled.load() ? 1 : 0);
    IniWriteInt("CameraSteering", g_cameraSteering.load() ? 1 : 0);
    IniWriteFloat("CameraDeadzone", g_cameraDeadzoneDeg.load());
    IniWriteInt("Keys", g_keysEnabled.load() ? 1 : 0);
    IniWriteInt("KeyUp", g_keyUp.load());
    IniWriteInt("KeyDown", g_keyDown.load());
    IniWriteFloat("VerticalSpeed", g_verticalSpeed.load());
    IniWriteInt("BoostOnRT", g_rtBoost.load() ? 1 : 0);
    IniWriteInt("BodyTilt", g_bodyTilt.load() ? 1 : 0);
    IniWriteFloat("BodyTiltMax", g_bodyTiltMax.load());
    IniWriteFloat("TurnSpeed", g_turnMultiplier.load());
    IniWriteFloat("BreathMultiplier", g_breathMultiplier.load());
    IniWriteInt("InfiniteBreath", g_infiniteBreath.load() ? 1 : 0);
}

std::uint8_t* PlayerComponentOf(std::uint8_t* instance) {
    const std::uint32_t id = *At<const std::uint32_t*>(kRvaPlayerComponentId);
    const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(instance + kInstanceComponentCountOffset);
    auto** components = *reinterpret_cast<std::uint8_t***>(instance + kInstanceComponentsOffset);
    if (!components || id >= count) return nullptr;
    return components[id];
}

bool PlausibleDuration(float duration) {
    return std::isfinite(duration) && duration > 0.0f && duration <= 1.0e6f;
}

bool PlausibleExtra(float extra) {
    return std::isfinite(extra) && extra >= 0.0f && extra <= 1.0e6f;
}

std::uint32_t FloatBits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

PropRecord* FindOrCaptureProp(std::uint8_t* prop) {
    const float duration = *reinterpret_cast<const float*>(prop + kPropBreathDurationOffset);
    const float extra = *reinterpret_cast<const float*>(prop + kPropExtraBreathOffset);
    for (auto& record : g_propRecords) {
        if (record.prop != prop) continue;
        if (duration == record.writtenDuration && (!record.scaleExtra || extra == record.writtenExtra))
            return &record;
        // Reloaded (or rewritten by the game) since our last write: these are the originals now.
        if (!PlausibleDuration(duration)) return nullptr;
        record.originalDuration = record.writtenDuration = duration;
        record.originalExtra = record.writtenExtra = extra;
        record.scaleExtra = PlausibleExtra(extra);
        return &record;
    }
    if (!PlausibleDuration(duration)) {
        if (g_loggedOddProp != prop) {
            g_loggedOddProp = prop;
            Log::Writef("Swim: PlayerProp %p breath duration %g (0x%08X), extra %g (0x%08X) look wrong; breath changes skipped for it",
                        prop, static_cast<double>(duration), FloatBits(duration), static_cast<double>(extra),
                        FloatBits(extra));
        }
        return nullptr;
    }
    PropRecord& record = g_propRecords[g_propRecordNext];
    g_propRecordNext = (g_propRecordNext + 1) % static_cast<int>(std::size(g_propRecords));
    record = {prop, duration, extra, duration, extra, PlausibleExtra(extra), false};
    return &record;
}

void LogSwimTune(const std::uint8_t* prop) {
    const std::uint8_t* s = prop + kPropSwimTuneOffset;
    auto sh = [s](int index) { return static_cast<int>(*reinterpret_cast<const std::int16_t*>(s + index * 2)); };
    auto fl = [s](int offset) { return static_cast<double>(*reinterpret_cast<const float*>(s + offset)); };
    auto u16 = [s](int offset) { return static_cast<unsigned int>(*reinterpret_cast<const std::uint16_t*>(s + offset)); };
    Log::DetailOncef("Swim: PlayerProp %p swim tune: tread level ocean/non-ocean %d/%d, depths wade %d swim %d tread %d dive %d deep %d, max under %d cm; sink rate %.2f, float rate %.2f, bobbing %u",
                prop, sh(0), sh(1), sh(2), sh(3), sh(4), sh(5), sh(6), sh(7), fl(0x10), fl(0x14),
                static_cast<unsigned int>(s[0x18]));
    Log::DetailOncef("Swim: PlayerProp %p breath %.2f s, refill %.2f s, drowning grace %.2f s, drowning damage %.2f/s, current x%.2f, extra breath trait %u adds %g s (0x%08X), probes ceiling %u / ahead %u / forward %u",
                prop, fl(0x20), fl(0x24), fl(0x28), fl(0x1C), fl(0x2C), u16(0x30), fl(0x34),
                FloatBits(*reinterpret_cast<const float*>(s + 0x34)), u16(0x38), u16(0x3A), u16(0x3C));
}

float TriggerValue(BYTE value) {
    constexpr int threshold = XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    if (value <= threshold) return 0.0f;
    return static_cast<float>(value - threshold) / static_cast<float>(255 - threshold);
}

// Game thread. Updates g_padConnected / g_padRightTrigger from the first connected XInput pad.
void PollGamepad() {
    if (!g_xinputLoadTried) {
        g_xinputLoadTried = true;
        for (const wchar_t* name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
            HMODULE module = LoadLibraryW(name);
            if (!module) continue;
            g_xinputGetState = reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState"));
            if (g_xinputGetState) break;
        }
        Log::Detailf("Swim: XInput %s for the RT boost", g_xinputGetState ? "loaded" : "unavailable");
    }
    XINPUT_STATE state{};
    bool connected = false;
    if (g_xinputGetState) {
        if (g_padSlot < XUSER_MAX_COUNT && g_xinputGetState(g_padSlot, &state) == ERROR_SUCCESS) {
            connected = true;
        } else {
            // Polling empty slots is slow, so rescan at most every 2 s.
            g_padSlot = XUSER_MAX_COUNT;
            const ULONGLONG now = GetTickCount64();
            if (now >= g_padNextScanMs) {
                g_padNextScanMs = now + 2000;
                for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
                    if (g_xinputGetState(slot, &state) == ERROR_SUCCESS) {
                        g_padSlot = slot;
                        connected = true;
                        break;
                    }
                }
            }
        }
    }
    g_padConnected.store(connected, std::memory_order_relaxed);
    g_padRightTrigger.store(connected ? TriggerValue(state.Gamepad.bRightTrigger) : 0.0f, std::memory_order_relaxed);
}

bool RtBoostHeld() {
    return g_padConnected.load(std::memory_order_relaxed) &&
           g_padRightTrigger.load(std::memory_order_relaxed) > kTriggerHeld && !Overlay::PlayerInputLocked();
}

}  // namespace

// (v0.9.586) LaraFM's muffle: her head under water (the hold-breath property the swim activity reads, in a water volume).
namespace SwimBreath {
bool LaraUnderwater() {
    if (!g_tickAvailable) return false;
    if (GetTickCount64() - g_statusTickMs.load(std::memory_order_relaxed) > 500) return false;
    return g_statusInWater.load(std::memory_order_relaxed) && g_statusSubmerged.load(std::memory_order_relaxed);
}
}  // namespace SwimBreath

namespace {
bool FreeSwimActiveRecently() {
    return GetTickCount64() - g_statusOverrideMs.load(std::memory_order_relaxed) < kFreeSwimRecentMs;
}

void SwimTickUnsafe(std::uint8_t* player) {
    auto* instance = *reinterpret_cast<std::uint8_t**>(player + kPlayerInstanceOffset);
    if (!instance) return;
    if (!g_getLocalPlayerInstance || g_getLocalPlayerInstance(0) != instance) return;
    g_localPlayerInstance = instance;

    std::uint8_t* component = PlayerComponentOf(instance);
    if (!component || *reinterpret_cast<std::uint8_t**>(component + kPlayerComponentPlayerOffset) != player)
        return;
    auto* prop = *reinterpret_cast<std::uint8_t**>(component + kPlayerComponentPropOffset);
    if (!prop) return;
    g_localSteering.store(player + kPlayerSteeringOffset, std::memory_order_relaxed);

    float* breath = reinterpret_cast<float*>(player + kPlayerBreathOffset);
    const bool inWater = *reinterpret_cast<void**>(player + kPlayerWaterVolumeOffset) != nullptr;
    g_statusInWater.store(inWater, std::memory_order_relaxed);
    if (!inWater) {
        g_keepSubmerged.store(false, std::memory_order_relaxed);
        g_autoDiveActive = false;
    }
    g_statusBreath.store(*breath, std::memory_order_relaxed);
    g_statusTickMs.store(GetTickCount64(), std::memory_order_relaxed);

    if (inWater && g_stayUnderAvailable && g_rtBoost.load(std::memory_order_relaxed)) {
        PollGamepad();
        const bool boosting = g_statusSubmerged.load(std::memory_order_relaxed) && RtBoostHeld();
        if (boosting && !g_logRtBoostWas && GetTickCount64() >= g_logNextRtBoostMs) {
            Log::Detail("Swim: RT boost under water");
            g_logNextRtBoostMs = GetTickCount64() + 5000;
        }
        g_logRtBoostWas = boosting;
    } else if (g_padConnected.load(std::memory_order_relaxed)) {
        g_padConnected.store(false, std::memory_order_relaxed);
        g_padRightTrigger.store(0.0f, std::memory_order_relaxed);
        g_logRtBoostWas = false;
    }
    if (!g_breathAvailable) return;

    PropRecord* record = FindOrCaptureProp(prop);
    if (!record) return;
    if (!record->logged) {
        record->logged = true;
        LogSwimTune(prop);
    }

    // Scaling the prop keeps the game's own rules: refill time, the 50% dive threshold and the
    // breath percentage all follow the new maximum. Multiplier 1 restores the shipped values.
    const float multiplier = std::clamp(g_breathMultiplier.load(std::memory_order_relaxed), 1.0f, 10.0f);
    const float duration = record->originalDuration * multiplier;
    const float extra = record->scaleExtra ? record->originalExtra * multiplier : record->originalExtra;
    auto* durationField = reinterpret_cast<float*>(prop + kPropBreathDurationOffset);
    auto* extraField = reinterpret_cast<float*>(prop + kPropExtraBreathOffset);
    if (*durationField != duration || (record->scaleExtra && *extraField != extra)) {
        *durationField = duration;
        if (record->scaleExtra) *extraField = extra;
        Log::Detailf("Swim: breath duration %.2f s -> %.2f s (x%.2f), extra-breath trait bonus %.2f s -> %.2f s%s",
                    record->originalDuration, duration, multiplier, record->originalExtra, extra,
                    record->scaleExtra ? "" : " (left as is)");
    }
    record->writtenDuration = duration;
    record->writtenExtra = extra;

    // Runs before StateControl drains this frame's dt, so the breath never runs out.
    if (g_infiniteBreath.load(std::memory_order_relaxed) && std::isfinite(*breath) && *breath < duration)
        *breath = duration;

    g_statusBreathMax.store(duration, std::memory_order_relaxed);
    g_statusBreathOriginal.store(record->originalDuration, std::memory_order_relaxed);
}

void SwimTickGuarded(void* player) {
    __try {
        SwimTickUnsafe(static_cast<std::uint8_t*>(player));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __fastcall HookProcessSwimming(void* player, bool flag) {
    g_originalProcessSwimming(player, flag);
    if (player) SwimTickGuarded(player);
}

bool GameWindowForeground() {
    HWND window = GetForegroundWindow();
    if (!window) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    return pid == GetCurrentProcessId();
}

float KeyboardVerticalInput() {
    if (!g_keysEnabled.load(std::memory_order_relaxed) || Overlay::PlayerInputLocked() || !GameWindowForeground())
        return 0.0f;
    float value = 0.0f;
    if ((GetAsyncKeyState(g_keyUp.load(std::memory_order_relaxed)) & 0x8000) != 0) value += 1.0f;
    if ((GetAsyncKeyState(g_keyDown.load(std::memory_order_relaxed)) & 0x8000) != 0) value -= 1.0f;
    return value;
}

// Camera pitch (radians, up positive; deadzone around level, rescaled to still reach 90 degrees) and
// the camera's horizontal forward direction. False when no camera snapshot is available.
bool CameraSwimDirection(float& pitchOut, float& forwardXOut, float& forwardYOut) {
    pitchOut = 0.0f;
    Features::GameplayCameraState camera{};
    if (!Features::GetGameplayCameraState(camera) || !camera.valid) return false;
    float qx = camera.quaternion[0], qy = camera.quaternion[1];
    float qz = camera.quaternion[2], qw = camera.quaternion[3];
    const float lengthSq = qx * qx + qy * qy + qz * qz + qw * qw;
    if (!(lengthSq > 1.0e-8f) || !std::isfinite(lengthSq)) return false;
    const float inverse = 1.0f / std::sqrt(lengthSq);
    qx *= inverse; qy *= inverse; qz *= inverse; qw *= inverse;
    // The camera's forward is its +Y axis.
    const float forwardX = 2.0f * (qx * qy - qz * qw);
    const float forwardY = 1.0f - 2.0f * (qx * qx + qz * qz);
    const float forwardZ = 2.0f * (qy * qz + qx * qw);
    const float horizontal = std::sqrt(forwardX * forwardX + forwardY * forwardY);
    if (!(horizontal > 1.0e-4f)) return false;
    forwardXOut = forwardX / horizontal;
    forwardYOut = forwardY / horizontal;
    const float pitch = std::atan2(forwardZ, horizontal);
    const float deadzone = std::clamp(g_cameraDeadzoneDeg.load(std::memory_order_relaxed), 0.0f, 45.0f) * kDegToRad;
    const float magnitude = std::fabs(pitch) - deadzone;
    if (magnitude > 0.0f)
        pitchOut = std::copysign(std::min(magnitude * (kHalfPi / (kHalfPi - deadzone)), kMaxSwimPitch), pitch);
    return true;
}

// Returns whether Lara should stay submerged when Dive is not held.
bool PrepareFreeSwimUnsafe(void* agent, bool freeDepth) {
    auto* instance = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(agent) + kAgentInstanceOffset);
    if (!instance || instance != g_localPlayerInstance) return g_keepSubmerged.load(std::memory_order_relaxed);

    bool holdBreath = false;
    g_scAgentGetBoolProp(agent, kSCPropertyWaterHoldBreath, &holdBreath, false);
    g_statusSubmerged.store(holdBreath, std::memory_order_relaxed);
    const bool diveStarted = holdBreath && !g_wasSubmerged;
    g_wasSubmerged = holdBreath;
    if (holdBreath != g_logSubmergedWas) {
        g_logSubmergedWas = holdBreath;
        Log::Detailf("Swim: %s", holdBreath ? "submerged" : "surface swimming");
    }
    if (!holdBreath || !freeDepth) {
        g_autoDiveActive = false;
        return false;
    }

    std::uint8_t* component = PlayerComponentOf(instance);
    if (!component) return false;
    auto* player = *reinterpret_cast<std::uint8_t**>(component + kPlayerComponentPlayerOffset);
    if (!player || !*reinterpret_cast<void**>(player + kPlayerWaterVolumeOffset)) return false;

    const float surfaceZ = *reinterpret_cast<const float*>(player + kPlayerSwimSurfaceZOffset);
    const float tread = static_cast<float>(*reinterpret_cast<const std::int16_t*>(player + kPlayerSwimTreadDepthOffset));
    const float z = *reinterpret_cast<const float*>(instance + kInstancePositionZOffset);
    if (!std::isfinite(surfaceZ) || !std::isfinite(z)) return false;
    const float plane = surfaceZ - tread;
    const float depth = plane - z;
    g_statusDepth.store(depth, std::memory_order_relaxed);

    const ULONGLONG now = GetTickCount64();
    // A dive from the surface first takes her under, so a short press is enough to go down.
    if (diveStarted && depth < kAutoDiveDepth) {
        g_autoDiveActive = true;
        g_autoDiveStartMs = now;
    }
    // Above the normal swim level: let the game settle her back down first.
    if (depth < -kAbovePlaneSlack) return g_autoDiveActive;
    const bool atPlane = depth <= kAtPlaneSlack;

    float pitch = 0.0f, forwardX = 0.0f, forwardY = 1.0f;
    if (g_cameraSteering.load(std::memory_order_relaxed))
        CameraSwimDirection(pitch, forwardX, forwardY);
    if (atPlane && pitch > 0.0f) pitch = 0.0f;
    const float speed = std::clamp(g_verticalSpeed.load(std::memory_order_relaxed), 20.0f, kMaxVerticalSpeed);
    const float input = KeyboardVerticalInput();
    float extra = input * speed;
    if (g_autoDiveActive) {
        if (depth >= kAutoDiveDepth || now - g_autoDiveStartMs > kAutoDiveTimeoutMs || input > 0.0f)
            g_autoDiveActive = false;
        else
            extra = std::min(extra, -kAutoDiveSpeed);
    }
    if (atPlane && extra > 0.0f) extra = 0.0f;

    g_swimSinPitch = std::sin(pitch);
    g_swimCosMinusOne = std::cos(pitch) - 1.0f;
    g_swimCamFwdX = forwardX;
    g_swimCamFwdY = forwardY;
    g_swimVzExtra = extra;
    g_swimVzMax = atPlane ? 0.0f : kMaxVerticalSpeed;
    g_swimVzMin = -kMaxVerticalSpeed;
    g_swimOverrideActive = 1;

    // Back near the surface she returns to surface swimming unless Dive is really held.
    const bool keepSubmerged = g_autoDiveActive || depth > kKeepSubmergedDepth;

    // One line when it takes over and then at most every 2 s; flicker at the top edge stays quiet.
    g_statusOverrideMs.store(now, std::memory_order_relaxed);
    if (now >= g_logNextTraceMs) {
        Log::Detailf("Swim: free swim %s: %.1f cm below the surface level (z %.1f, surface %.1f, tread %.0f), camera pitch %.1f deg, keys %.0f%s%s%s",
                    g_logOverrideWasActive ? "trace" : "active", depth, z, surfaceZ, tread,
                    pitch / kDegToRad, input, keepSubmerged ? ", staying under" : "",
                    g_autoDiveActive ? ", diving in" : "",
                    g_rtBoost.load(std::memory_order_relaxed) && RtBoostHeld() ? ", RT boost" : "");
        g_logNextTraceMs = now + 2000;
    }
    g_logOverrideWasActive = true;
    return keepSubmerged;
}

bool PrepareFreeSwimGuarded(void* agent, bool freeDepth) {
    __try {
        return PrepareFreeSwimUnsafe(agent, freeDepth);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_swimOverrideActive = 0;
        return false;
    }
}

void __fastcall HookAdjustWadeSpeed(void* agent) {
    g_swimOverrideActive = 0;
    if (agent) {
        const bool keep = PrepareFreeSwimGuarded(agent, g_freeSwimEnabled.load(std::memory_order_relaxed));
        if (keep != g_keepSubmerged.load(std::memory_order_relaxed)) {
            g_keepSubmerged.store(keep, std::memory_order_relaxed);
            Log::Detailf("Swim: %s", keep ? "staying under water without holding Dive"
                                          : "near the surface: Dive decides again");
        }
    }
    if (!g_swimOverrideActive && g_logOverrideWasActive) {
        g_logOverrideWasActive = false;
        if (GetTickCount64() >= g_logNextTraceMs) {
            Log::Detail("Swim: free swim released to the game");
            g_logNextTraceMs = GetTickCount64() + 2000;
        }
    }
    g_originalAdjustWadeSpeed(agent);
    g_swimOverrideActive = 0;
}

// StateControl "Requested <action>" condition, for Lara only:
// - Swim_Dive: while she is clearly under water it answers "held", so the hold-to-dive rule does not send
//   her back to the surface; everything else (drowning, shallow water, leaving the water) still changes
//   the state as usual.
// - Swim_Forward (the PC boost, B): under water with a gamepad connected it answers with RT instead.
bool __fastcall HookActionRequested(void* agent, void* memory, const std::uint8_t* condition) {
    const bool result = g_originalActionRequested(agent, memory, condition);
    if (!agent || !condition) return result;
    if (*reinterpret_cast<const std::uint16_t*>(condition + 8) != kSCCondPropertyRequested) return result;
    if (*reinterpret_cast<const std::int32_t*>(condition) == kSCCondContextIndirect) return result;
    const std::int32_t action = *reinterpret_cast<const std::int32_t*>(condition + 0x0C);
    if (action != kInputActionSwimDive && action != kInputActionSwimForward) return result;
    if (*reinterpret_cast<void* const*>(static_cast<const std::uint8_t*>(agent) + kAgentInstanceOffset) !=
        g_localPlayerInstance)
        return result;
    if (GetTickCount64() - g_statusTickMs.load(std::memory_order_relaxed) > 1000) return result;
    const bool negate = (condition[0x10] & 1) != 0;
    if (action == kInputActionSwimDive) {
        if (!g_keepSubmerged.load(std::memory_order_relaxed)) return result;
        return !negate;
    }
    if (!g_rtBoost.load(std::memory_order_relaxed) || !g_padConnected.load(std::memory_order_relaxed) ||
        !g_statusInWater.load(std::memory_order_relaxed) || !g_statusSubmerged.load(std::memory_order_relaxed))
        return result;
    return RtBoostHeld() != negate;
}

// The swim graph's "Pitch" curve (dtp::ScalarCurveNode: m_curve[100], xMin, xMax, yMin, yMax) maps
// SwimTiltAngle to the body pitch: a falling line over +-30 degrees in both x and y. Only this curve in
// Lara's graphs has that shape; our widened copies (+-30..85) are recognised as well.
bool IsSwimTiltCurve(const float* curve) {
    const float range = curve[101];
    if (!(range >= kStockTiltRange - 0.01f && range <= kMaxBodyTilt + 0.01f)) return false;
    if (curve[100] != -range || curve[102] != -range || curve[103] != range) return false;
    if (curve[0] != 1.0f || curve[99] != 0.0f) return false;
    return std::fabs(curve[50] - (1.0f - 50.0f / 99.0f)) < 0.01f;
}

float SwimTiltRangeWanted() {
    if (!g_bodyTilt.load(std::memory_order_relaxed) || !FreeSwimActiveRecently()) return kStockTiltRange;
    return std::clamp(g_bodyTiltMax.load(std::memory_order_relaxed), kStockTiltRange, kMaxBodyTilt);
}

void WriteTiltRange(float* curve, float range) {
    __try {
        curve[100] = -range;
        curve[101] = range;
        curve[102] = -range;
        curve[103] = range;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_tiltCurveWriteFailed.store(true, std::memory_order_relaxed);
    }
}

// Anim graph evaluation (any thread). The curve is Lara's graph data and is read fresh on every
// evaluation, so widening it in place while she swims freely lets her body follow the swim pitch.
void* __fastcall HookScalarCurveGetAnimData(void* node, void* out, void* arg3, void* arg4) {
    auto* curve = *reinterpret_cast<float**>(static_cast<std::uint8_t*>(node) + kAnimNodeDtpOffset);
    // Cheap test on xMin first; most curves use other ranges.
    if (curve && curve[100] <= -kStockTiltRange + 0.01f && curve[100] >= -kMaxBodyTilt - 0.01f &&
        !g_tiltCurveWriteFailed.load(std::memory_order_relaxed) && IsSwimTiltCurve(curve)) {
        if (!g_tiltCurveSeen.exchange(true, std::memory_order_relaxed))
            Log::Detailf("Swim: swim body pitch curve found at %p (+-%.0f deg)", static_cast<void*>(curve),
                        static_cast<double>(curve[101]));
        const float range = SwimTiltRangeWanted();
        if (curve[101] != range) WriteTiltRange(curve, range);
    }
    return g_originalScalarCurveGetAnimData(node, out, arg3, arg4);
}

// Per-axis turn rate for this frame. While Lara swims freely it is scaled, so she lines up with the
// steering direction (the camera) faster.
float* __fastcall HookCalculateTurnRate(void* steering, float* out, float dt) {
    float* result = g_originalCalculateTurnRate(steering, out, dt);
    if (steering && out && steering == g_localSteering.load(std::memory_order_relaxed)) {
        const float multiplier = g_turnMultiplier.load(std::memory_order_relaxed);
        if (multiplier > 1.001f && FreeSwimActiveRecently()) {
            out[0] *= multiplier;
            out[1] *= multiplier;
            out[2] *= multiplier;
        }
    }
    return result;
}

struct KeyChoice {
    int vk;
    const char* name;
};
constexpr KeyChoice kKeyChoices[] = {
    {'Q', "Q"}, {'E', "E"}, {'R', "R"}, {'F', "F"}, {'Z', "Z"}, {'X', "X"}, {'C', "C"}, {'V', "V"},
    {VK_SPACE, "Space"}, {VK_LSHIFT, "Left Shift"}, {VK_LCONTROL, "Left Ctrl"}, {VK_LMENU, "Left Alt"},
    {VK_XBUTTON1, "Mouse 4"}, {VK_XBUTTON2, "Mouse 5"}, {VK_PRIOR, "Page Up"}, {VK_NEXT, "Page Down"},
};

const char* KeyName(int vk) {
    for (const auto& choice : kKeyChoices)
        if (choice.vk == vk) return choice.name;
    return "?";
}

bool KeyCombo(const char* label, std::atomic<int>& key, float width) {
    bool changed = false;
    const int current = key.load();
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo(label, KeyName(current))) {
        for (const auto& choice : kKeyChoices) {
            const bool selected = choice.vk == current;
            if (ImGui::Selectable(choice.name, selected)) {
                key.store(choice.vk);
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}
}  // namespace

namespace SwimBreath {
bool CreateHooks() {
    if (!GameBuild::IsSupportedRetail()) return false;
    g_base = GameBuild::Base();
    LoadSettings();

    const bool common =
        Verify(kRvaSCAgentGetBoolProp, kSCAgentGetBoolPropSig, sizeof(kSCAgentGetBoolPropSig), "SCAgent::GetPropData(bool)") &&
        Verify(kRvaGetLocalPlayerInstance, kGetLocalPlayerInstanceSig, sizeof(kGetLocalPlayerInstanceSig), "GetLocalPlayerInstance") &&
        Verify(kRvaPlayerProcessSwimming, kProcessSwimmingSig, sizeof(kProcessSwimmingSig), "Player::ProcessSwimming") &&
        Verify(kRvaProcessSwimmingInstance, kProcessSwimmingInstanceSig, sizeof(kProcessSwimmingInstanceSig), "Player+0x31A8 instance") &&
        Verify(kRvaProcessSwimmingProp, kProcessSwimmingPropSig, sizeof(kProcessSwimmingPropSig), "PlayerComponent+0x2B0 PlayerProp");
    if (!common) {
        Log::Write("Swim: swimming/breath features unavailable");
        return false;
    }
    g_scAgentGetBoolProp = At<SCAgentGetBoolPropFn>(kRvaSCAgentGetBoolProp);
    g_getLocalPlayerInstance = At<GetLocalPlayerInstanceFn>(kRvaGetLocalPlayerInstance);

    if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaPlayerProcessSwimming),
                      reinterpret_cast<void*>(&HookProcessSwimming),
                      reinterpret_cast<void**>(&g_originalProcessSwimming)) != MH_OK) {
        Log::Write("Swim: failed to create Player::ProcessSwimming hook; swimming/breath features unavailable");
        return false;
    }
    g_tickAvailable = true;

    g_breathAvailable =
        Verify(kRvaSwimBreathProcessProps, kSwimBreathProcessPropsSig, sizeof(kSwimBreathProcessPropsSig), "SwimBreathCheckProcess PlayerProp breath") &&
        Verify(kRvaSwimBreathEntryExtra, kSwimBreathEntryExtraSig, sizeof(kSwimBreathEntryExtraSig), "SwimBreathCheckEntry extra breath") &&
        Verify(kRvaSwimBreathDrain, kSwimBreathDrainSig, sizeof(kSwimBreathDrainSig), "SwimBreathCheckProcess Player::m_breath");

    const bool swimSigs =
        Verify(kRvaAdjustWadeSpeed, kAdjustWadeSpeedSig, sizeof(kAdjustWadeSpeedSig), "PSCAF_P_Move_AdjustWadeSpeed") &&
        Verify(kRvaAdjustWadeSpeedComponents, kAdjustWadeSpeedComponentsSig, sizeof(kAdjustWadeSpeedComponentsSig), "AdjustWadeSpeed Player/PlayerProp") &&
        Verify(kRvaAdjustWadeSpeedWaterVol, kAdjustWadeSpeedWaterVolSig, sizeof(kAdjustWadeSpeedWaterVolSig), "AdjustWadeSpeed currentWaterVolume") &&
        Verify(kRvaAdjustWadeSpeedSurfaceZ, kAdjustWadeSpeedSurfaceZSig, sizeof(kAdjustWadeSpeedSurfaceZSig), "AdjustWadeSpeed surface Z") &&
        Verify(kRvaAdjustWadeSpeedTread, kAdjustWadeSpeedTreadSig, sizeof(kAdjustWadeSpeedTreadSig), "AdjustWadeSpeed tread depth") &&
        Verify(kRvaAdjustWadeSpeedDesiredCopy, kAdjustWadeSpeedDesiredCopySig, sizeof(kAdjustWadeSpeedDesiredCopySig), "AdjustWadeSpeed desired copy") &&
        Verify(kRvaAdjustWadeSpeedDeepBranch, kAdjustWadeSpeedDeepBranchSig, sizeof(kAdjustWadeSpeedDeepBranchSig), "AdjustWadeSpeed deep-water branch") &&
        Verify(kRvaAdjustWadeSpeedDeepBlock, kAdjustWadeSpeedDeepBlockSig, sizeof(kAdjustWadeSpeedDeepBlockSig), "AdjustWadeSpeed target-plane block") &&
        Verify(kRvaAdjustWadeSpeedResume, kAdjustWadeSpeedResumeSig, sizeof(kAdjustWadeSpeedResumeSig), "AdjustWadeSpeed desired.z store");
    if (swimSigs) {
        g_swimVerticalResume = reinterpret_cast<void*>(g_base + kRvaAdjustWadeSpeedResume);
        void* block = reinterpret_cast<void*>(g_base + kRvaAdjustWadeSpeedDeepBlock);
        void* entry = reinterpret_cast<void*>(g_base + kRvaAdjustWadeSpeed);
        if (MH_CreateHook(block, reinterpret_cast<void*>(&SwimVerticalStub), &g_swimVerticalTrampoline) != MH_OK) {
            Log::Write("Swim: failed to create the target-plane hook; free depth unavailable");
        } else if (MH_CreateHook(entry, reinterpret_cast<void*>(&HookAdjustWadeSpeed),
                                 reinterpret_cast<void**>(&g_originalAdjustWadeSpeed)) != MH_OK) {
            MH_RemoveHook(block);
            Log::Write("Swim: failed to create the AdjustWadeSpeed hook; free depth unavailable");
        } else {
            g_freeSwimAvailable = true;
        }
    }

    if (g_freeSwimAvailable &&
        Verify(kRvaActionRequested, kActionRequestedSig, sizeof(kActionRequestedSig), "CSCCF_ActionRequested")) {
        if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaActionRequested),
                          reinterpret_cast<void*>(&HookActionRequested),
                          reinterpret_cast<void**>(&g_originalActionRequested)) == MH_OK)
            g_stayUnderAvailable = true;
        else
            Log::Write("Swim: failed to create the Requested condition hook; releasing Dive will surface as usual");
    }

    if (g_freeSwimAvailable &&
        Verify(kRvaScalarCurveGetAnimData, kScalarCurveGetAnimDataSig, sizeof(kScalarCurveGetAnimDataSig),
               "AnimScalarCurveNode::GetAnimData")) {
        if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaScalarCurveGetAnimData),
                          reinterpret_cast<void*>(&HookScalarCurveGetAnimData),
                          reinterpret_cast<void**>(&g_originalScalarCurveGetAnimData)) == MH_OK)
            g_bodyTiltAvailable = true;
        else
            Log::Write("Swim: failed to create the anim curve hook; body pitch stays at the game's 30 degrees");
    }

    if (g_freeSwimAvailable &&
        Verify(kRvaCalculateTurnRate, kCalculateTurnRateSig, sizeof(kCalculateTurnRateSig),
               "PlayerSteering::CalculateTurnRate") &&
        Verify(kRvaSteeringRateSetTurnRate, kSteeringRateSetTurnRateSig, sizeof(kSteeringRateSetTurnRateSig),
               "Player+0x3430 steering")) {
        if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaCalculateTurnRate),
                          reinterpret_cast<void*>(&HookCalculateTurnRate),
                          reinterpret_cast<void**>(&g_originalCalculateTurnRate)) == MH_OK)
            g_turnAvailable = true;
        else
            Log::Write("Swim: failed to create the turn rate hook; turn speed stays at the game's value");
    }

    Log::Detailf("Swim: hooks created (free swim %s, stay under/RT boost %s, body pitch %s, turn speed %s, breath %s); settings: free swim %d camera %d deadzone %.0f keys %d (%s/%s) speed %.0f, RT boost %d, body pitch %d up to %.0f deg, turn x%.1f, breath x%.2f infinite %d",
                g_freeSwimAvailable ? "ready" : "unavailable", g_stayUnderAvailable ? "ready" : "unavailable",
                g_bodyTiltAvailable ? "ready" : "unavailable", g_turnAvailable ? "ready" : "unavailable",
                g_breathAvailable ? "ready" : "unavailable",
                g_freeSwimEnabled.load() ? 1 : 0, g_cameraSteering.load() ? 1 : 0, g_cameraDeadzoneDeg.load(),
                g_keysEnabled.load() ? 1 : 0, KeyName(g_keyUp.load()), KeyName(g_keyDown.load()),
                g_verticalSpeed.load(), g_rtBoost.load() ? 1 : 0, g_bodyTilt.load() ? 1 : 0, g_bodyTiltMax.load(),
                g_turnMultiplier.load(), g_breathMultiplier.load(), g_infiniteBreath.load() ? 1 : 0);
    return true;
}

void DrawPlayerSection() {
    const float scale = std::clamp(Settings::Get().uiScale, 1.0f, 2.5f);
    bool save = false;

    // (v0.9.519: its title is the Enhancements tab's collapsing header)

    if (!g_freeSwimAvailable) ImGui::BeginDisabled();
    bool freeSwim = g_freeSwimEnabled.load();
    if (ImGui::Checkbox("Free underwater swimming", &freeSwim)) {
        g_freeSwimEnabled.store(freeSwim);
        save = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Dive takes Lara under water; after that she stays under without holding Dive and swims\n"
            "where the camera points. The game no longer picks her depth or follows tunnels for you.\n"
            "Swim back up to the surface to return to normal swimming and climb out.");
    }
    if (freeSwim) {
        ImGui::Indent();
        bool camera = g_cameraSteering.load();
        if (ImGui::Checkbox("Swim where the camera points", &camera)) {
            g_cameraSteering.store(camera);
            save = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Pushing forward follows the camera up or down; pushing back goes the other way.");
        if (camera) {
            float deadzone = g_cameraDeadzoneDeg.load();
            ImGui::SetNextItemWidth(145.0f * scale);
            if (ImGui::SliderFloat("Level deadzone", &deadzone, 0.0f, 30.0f, "%.0f deg", ImGuiSliderFlags_AlwaysClamp))
                g_cameraDeadzoneDeg.store(deadzone);
            if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Camera pitch within this angle of level swims straight ahead.");
        }

        bool keys = g_keysEnabled.load();
        if (ImGui::Checkbox("Up/down keys", &keys)) {
            g_keysEnabled.store(keys);
            save = true;
        }
        if (keys) {
            ImGui::SameLine();
            save |= KeyCombo("Up##swimKeyUp", g_keyUp, 95.0f * scale);
            ImGui::SameLine();
            save |= KeyCombo("Down##swimKeyDown", g_keyDown, 95.0f * scale);
        }

        float speed = g_verticalSpeed.load();
        ImGui::SetNextItemWidth(145.0f * scale);
        if (ImGui::SliderFloat("Up/down speed", &speed, 50.0f, kMaxVerticalSpeed, "%.0f cm/s", ImGuiSliderFlags_AlwaysClamp))
            g_verticalSpeed.store(speed);
        if (ImGui::IsItemDeactivatedAfterEdit()) save = true;

        if (!g_bodyTiltAvailable) ImGui::BeginDisabled();
        bool tilt = g_bodyTilt.load();
        if (ImGui::Checkbox("Body follows the swim direction", &tilt)) {
            g_bodyTilt.store(tilt);
            save = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Lara pitches her body up or down with the direction she swims in.\n"
                              "The game stops at 30 degrees.");
        if (tilt) {
            float tiltMax = g_bodyTiltMax.load();
            ImGui::SetNextItemWidth(145.0f * scale);
            if (ImGui::SliderFloat("Max body pitch", &tiltMax, kStockTiltRange, kMaxBodyTilt, "%.0f deg",
                                   ImGuiSliderFlags_AlwaysClamp))
                g_bodyTiltMax.store(tiltMax);
            if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
        }
        if (!g_bodyTiltAvailable) ImGui::EndDisabled();

        if (!g_turnAvailable) ImGui::BeginDisabled();
        float turn = g_turnMultiplier.load();
        ImGui::SetNextItemWidth(145.0f * scale);
        if (ImGui::SliderFloat("Turn speed", &turn, 1.0f, kMaxTurnMultiplier, "x%.1f", ImGuiSliderFlags_AlwaysClamp))
            g_turnMultiplier.store(turn);
        if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("How fast Lara turns toward the direction you steer (the camera) under water.\n"
                              "x1 is the game's own speed.");
        if (!g_turnAvailable) ImGui::EndDisabled();
        ImGui::Unindent();
    }

    if (!g_stayUnderAvailable) ImGui::BeginDisabled();
    bool rtBoost = g_rtBoost.load();
    if (ImGui::Checkbox("Swim boost on RT under water", &rtBoost)) {
        g_rtBoost.store(rtBoost);
        save = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("With a gamepad, hold RT under water to swim fast (B no longer does it there).\n"
                          "On the surface B still boosts; the keyboard is unchanged.");
    if (!g_stayUnderAvailable) ImGui::EndDisabled();
    if (!g_freeSwimAvailable) ImGui::EndDisabled();

    if (!g_breathAvailable) ImGui::BeginDisabled();
    float multiplier = g_breathMultiplier.load();
    ImGui::SetNextItemWidth(145.0f * scale);
    if (ImGui::SliderFloat("Breath duration", &multiplier, 1.0f, 10.0f, "x%.1f", ImGuiSliderFlags_AlwaysClamp))
        g_breathMultiplier.store(multiplier);
    if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Multiplies how long Lara can hold her breath. Refill time stays the same.");
    bool infinite = g_infiniteBreath.load();
    if (ImGui::Checkbox("Infinite breath", &infinite)) {
        g_infiniteBreath.store(infinite);
        save = true;
    }
    if (!g_breathAvailable) ImGui::EndDisabled();

    const ULONGLONG now = GetTickCount64();
    if (!g_tickAvailable) {
        ImGui::TextDisabled("Swimming hooks are unavailable for this executable build.");
    } else if (now - g_statusTickMs.load() > 1000) {
        ImGui::TextDisabled("Lara is not currently available.");
    } else if (!g_statusInWater.load()) {
        ImGui::TextDisabled("Not in water. Breath %.1f of %.1f s (game default %.1f s).",
                            g_statusBreath.load(), g_statusBreathMax.load(), g_statusBreathOriginal.load());
    } else {
        const bool submerged = g_statusSubmerged.load();
        const bool freeActive = now - g_statusOverrideMs.load() < 500;
        if (submerged)
            ImGui::TextDisabled("Under water, %.0f cm below the surface level%s%s. Breath %.1f of %.1f s.",
                                std::max(0.0f, g_statusDepth.load()), freeActive ? ", free swim" : "",
                                g_keepSubmerged.load() ? ", staying under" : "",
                                g_statusBreath.load(), g_statusBreathMax.load());
        else
            ImGui::TextDisabled("In water. Breath %.1f of %.1f s.", g_statusBreath.load(), g_statusBreathMax.load());
    }

    if (save) SaveSettings();
}
}  // namespace SwimBreath
