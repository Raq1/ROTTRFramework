// DLAA: DLSS at the screen's own resolution, an entry of Options > Display > DLSS (dlaa.h).
#include "dlaa.h"

#include "game_build.h"
#include "log.h"

#include <MinHook.h>
#include <Windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace Dlaa {
namespace {
// --- retail code (checked before hooking) ---
constexpr std::uintptr_t kRvaConvertScreenToRenderRes = 0x00CB52C0ull;
constexpr std::uint8_t kConvertSig[] = {0x4C, 0x89, 0x4C, 0x24, 0x20, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41,
                                        0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x68, 0x4C, 0x8D, 0x79, 0x70,
                                        0x48, 0x8B, 0xF1, 0x49, 0x8B, 0xCF, 0x49, 0x8B, 0xD9, 0x4D, 0x8B, 0xE0};
constexpr std::uintptr_t kRvaGetDisplaySettings = 0x00C82660ull;   // NxAppImpl::GetDisplaySettings
constexpr std::uint8_t kGetDisplaySettingsSig[] = {0x48, 0x8D, 0x81, 0x1C, 0x05, 0x00, 0x00, 0xC3};
constexpr std::uintptr_t kRvaUpdateState = 0x00CB7140ull;          // NxDLSSImpl::UpdateState
constexpr std::uint8_t kUpdateStateSig[] = {0x44, 0x89, 0x4C, 0x24, 0x20, 0x44, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54,
                                            0x24, 0x10, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x83,
                                            0xEC, 0x58, 0x48, 0x8D, 0x41, 0x70};
constexpr std::uintptr_t kRvaInitDlss = 0x00CB59C0ull;             // NxDLSSImpl::InitDLSS (checked: its read below)
constexpr std::uint8_t kInitDlssSig[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x78, 0x20, 0x41, 0x54, 0x41, 0x56, 0x41,
                                         0x57, 0x48, 0x83, 0xEC, 0x50, 0x80, 0x79, 0x09, 0x00, 0x45, 0x8B, 0xF9};
// the two reads of the setting that pick NGX's quality: the instructions after their GetDisplaySettings calls
constexpr std::uintptr_t kRvaUpdateStateRead = 0x00CB71B3ull;      // mov edx, [rax+90h]
constexpr std::uint8_t kUpdateStateReadSig[] = {0x8B, 0x90, 0x90, 0x00, 0x00, 0x00};
constexpr std::uintptr_t kRvaInitDlssRead = 0x00CB5A05ull;         // xor ebp, ebp; mov esi, 1; mov r9d, [rax+90h]
constexpr std::uint8_t kInitDlssReadSig[] = {0x33, 0xED, 0xBE, 0x01, 0x00, 0x00, 0x00, 0x44, 0x8B, 0x88, 0x90, 0x00, 0x00, 0x00};
constexpr std::uintptr_t kRvaSupportsRead = 0x00CB69D4ull;         // ResolutionSupportsDLSS's: mov r10d, [rax+90h]
constexpr std::uint8_t kSupportsReadSig[] = {0x44, 0x8B, 0x90, 0x90, 0x00, 0x00, 0x00};
constexpr std::uintptr_t kRvaNxAppGetApp = 0x00C5FD20ull;          // NxApp_GetApp
constexpr std::uint8_t kNxAppGetAppSig[] = {0x48, 0x8B, 0x05, 0x49, 0xC2, 0x20, 0x02, 0x48, 0x8B, 0x00, 0xC3};
constexpr std::uintptr_t kRvaFillDlssOption = 0x008EA410ull;       // UiMenuStateNixxesOptionsScreenDisplay::FillDLSSOption
constexpr std::uint8_t kFillDlssOptionSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89,
                                               0x7C, 0x24, 0x18, 0x4C, 0x89, 0x74, 0x24, 0x20, 0x55, 0x48, 0x8D, 0x6C,
                                               0x24, 0xA9, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00};
constexpr std::uintptr_t kRvaFillDlssIndexWrite = 0x008EA65Bull;   // mov eax, [rsi+90h]; mov [r14+40h], eax
constexpr std::uint8_t kFillDlssIndexWriteSig[] = {0x8B, 0x86, 0x90, 0x00, 0x00, 0x00, 0x41, 0x89, 0x46, 0x40};
constexpr std::uintptr_t kRvaStepperChanged = 0x00926AC0ull;       // ...ScreenDisplay::OnOptionStepperValueChanged
constexpr std::uint8_t kStepperChangedSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x01,
                                               0x48, 0x8B, 0xDA, 0xBA, 0x02, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF9, 0xFF, 0x90,
                                               0xC0, 0x00, 0x00, 0x00, 0x48, 0x8B};
constexpr std::uintptr_t kRvaStepperDlssCase = 0x00926C60ull;      // mov rax, [rdi+1F8h]; mov rcx, rdi; mov [rax+90h], r8d
constexpr std::uint8_t kStepperDlssCaseSig[] = {0x48, 0x8B, 0x87, 0xF8, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xCF,
                                                0x44, 0x89, 0x80, 0x90, 0x00, 0x00, 0x00};
constexpr std::uintptr_t kRvaItemValuePushBack = 0x00969770ull;    // cdc::ArrayImpl<ItemValue>::push_back
constexpr std::uint8_t kItemValuePushBackSig[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57,
                                                  0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0xF2, 0x8B, 0x09};

constexpr std::size_t kSettingsDlss = 0x90, kSettingsSize = 0xB4;   // nx::DisplaySettings
constexpr std::size_t kDlssQualityStored = 0x1C;                   // NxDLSSImpl: NGX quality of the feature made
constexpr std::size_t kPendingSettings = 0x1F8;                    // the options screen: its DisplaySettings*
constexpr std::size_t kItemSelected = 0x40, kItemValues = 0x48;    // OptionStepperItemData
constexpr int kDlssOptionId = 5;
constexpr std::uint32_t kDlssQuality = 3;

struct GameArray {
    std::uint32_t count;
    std::uint32_t capacity;
    void* data;
};
struct GameUtf8String {
    std::uint32_t chars;
    std::uint32_t pad;
    GameArray bytes;
};
struct ItemValue {
    GameUtf8String label;
    std::int32_t value;
    std::uint8_t flag;
    std::uint8_t pad[3];
};
static_assert(sizeof(ItemValue) == 0x20, "OptionStepperItemData::ItemValue");

// The list: index -> the setting, the setting -> index (Off, DLAA, Quality, Balanced, Performance, Ultra Performance)
constexpr std::uint32_t kIndexToValue[6] = {0, kDlssDlaa, 3, 2, 1, 4};
constexpr int kValueToIndex[6] = {0, 4, 3, 2, 5, 1};
// the game's five labels, in the order it adds them (the values 0..4)
const char* const kGameLabels[5] = {"$OptionsMenu_Quality_Off", "$OptionsMenu_Performance", "$OptionsMenu_Balanced",
                                    "$OptionsMenu_Quality", "$OptionsMenu_Ultra_Performance"};
char g_dlaaLabel[] = "DLAA";

using ConvertFn = void(__fastcall*)(void*, std::uint32_t*, std::uint32_t*, std::uint32_t*, std::uint32_t*, std::uint32_t*,
                                    std::uint32_t*, int);
using GetDisplaySettingsFn = const std::uint8_t*(__fastcall*)(void* app);
using UpdateStateFn = void(__fastcall*)(void* self, void* device, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                        bool, std::uint32_t);
using NxAppGetAppFn = void*(__fastcall*)();
using FillDlssOptionFn = void(__fastcall*)(void* screen, std::uint8_t* item, std::uint8_t* settings);
using StepperChangedFn = void(__fastcall*)(void* screen, const std::uint8_t* params);
using PushBackFn = void(__fastcall*)(void* array, const ItemValue* value);

ConvertFn g_originalConvert = nullptr;
GetDisplaySettingsFn g_originalGetDisplaySettings = nullptr;
UpdateStateFn g_originalUpdateState = nullptr;
NxAppGetAppFn g_nxAppGetApp = nullptr;
FillDlssOptionFn g_originalFillDlssOption = nullptr;
StepperChangedFn g_originalStepperChanged = nullptr;
PushBackFn g_pushBack = nullptr;
std::atomic_bool g_available{false};
std::atomic_bool g_listInstalled{false};
std::uintptr_t g_updateStateRead = 0, g_initDlssRead = 0, g_supportsRead = 0;
std::mutex g_lastMutex;
LastSetup g_last;
int g_pendingIndex = -1;   // (the UI thread) a DLSS step's index, turned into its value by the next list build

void __fastcall HookConvert(void* self, std::uint32_t* width, std::uint32_t* height, std::uint32_t* maxWidth,
                            std::uint32_t* maxHeight, std::uint32_t* minWidth, std::uint32_t* minHeight, int quality) {
    const std::uint32_t screenWidth = width ? *width : 0, screenHeight = height ? *height : 0;
    const bool dlaa = static_cast<std::uint32_t>(quality) == kDlssDlaa;
    g_originalConvert(self, width, height, maxWidth, maxHeight, minWidth, minHeight,
                      dlaa ? static_cast<int>(kDlssQuality) : quality);
    if (!width || !height || !maxWidth || !maxHeight || !minWidth || !minHeight || !screenWidth || !screenHeight) return;
    const std::uint32_t dlssWidth = *width, dlssHeight = *height;
    if (dlaa) {
        *width = *maxWidth = *minWidth = screenWidth;
        *height = *maxHeight = *minHeight = screenHeight;
    }
    LastSetup last;
    last.seen = true;
    last.dlaa = dlaa;
    last.screenWidth = screenWidth;
    last.screenHeight = screenHeight;
    last.renderWidth = *width;
    last.renderHeight = *height;
    last.quality = quality;
    {
        std::lock_guard<std::mutex> lock(g_lastMutex);
        g_last = last;
    }
    static const char* const kNames[] = {"?", "Performance", "Balanced", "Quality", "Ultra Performance", "DLAA"};
    if (dlaa)
        Log::Writef("DLAA: DLSS set up for %ux%u as DLAA: rendered at %ux%u (Quality would render %ux%u), DLSS anti-aliases only",
                    screenWidth, screenHeight, *width, *height, dlssWidth, dlssHeight);
    else
        Log::Writef("DLAA: DLSS set up for %ux%u (%s): rendered at %ux%u", screenWidth, screenHeight,
                    kNames[quality >= 1 && quality <= 5 ? quality : 0], *width, *height);
}

// NGX's quality for DLAA: the two reads that map the setting see Quality (a copy; the game's own settings unchanged)
const std::uint8_t* __fastcall HookGetDisplaySettings(void* app) {
    const std::uint8_t* settings = g_originalGetDisplaySettings(app);
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if ((caller == g_updateStateRead || caller == g_initDlssRead || caller == g_supportsRead) && settings &&
        *reinterpret_cast<const std::uint32_t*>(settings + kSettingsDlss) == kDlssDlaa) {
        thread_local std::uint8_t copy[kSettingsSize];
        std::memcpy(copy, settings, kSettingsSize);
        *reinterpret_cast<std::uint32_t*>(copy + kSettingsDlss) = kDlssQuality;
        return copy;
    }
    return settings;
}

// To or from DLAA: the feature made again (its input size is the render size, which UpdateState does not compare)
void __fastcall HookUpdateState(void* self, void* device, std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d,
                                bool e, std::uint32_t f) {
    static std::uint32_t s_last = 0xFFFFFFFFu;
    void* app = g_nxAppGetApp ? g_nxAppGetApp() : nullptr;
    const std::uint8_t* settings = app ? g_originalGetDisplaySettings(app) : nullptr;
    if (settings && self) {
        const std::uint32_t value = *reinterpret_cast<const std::uint32_t*>(settings + kSettingsDlss);
        if (value != s_last) {
            if (s_last != 0xFFFFFFFFu && (value == kDlssDlaa || s_last == kDlssDlaa)) {
                *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(self) + kDlssQualityStored) = 0xFFFFFFFFu;
                Log::Writef("DLAA: DLSS %s DLAA: the DLSS feature is made again for the new render size",
                            value == kDlssDlaa ? "set to" : "changed from");
            }
            s_last = value;
        }
    }
    g_originalUpdateState(self, device, a, b, c, d, e, f);
}

bool LabelIs(const ItemValue& v, const char* text) {
    const auto* data = static_cast<const char*>(v.label.bytes.data);
    return data && v.label.bytes.count == std::strlen(text) + 1 && std::memcmp(data, text, v.label.bytes.count) == 0;
}

bool ReorderListUnsafe(std::uint8_t* item, std::uint32_t value) {
    auto* values = reinterpret_cast<GameArray*>(item + kItemValues);
    if (values->count != 5 || !values->data) return false;
    auto* entries = static_cast<ItemValue*>(values->data);
    for (int i = 0; i < 5; ++i)
        if (!LabelIs(entries[i], kGameLabels[i])) return false;
    ItemValue dlaa{};
    dlaa.label.chars = 4;
    dlaa.label.bytes.count = sizeof(g_dlaaLabel);   // (with its 0; capacity 0: not ours to free, push_back copies it)
    dlaa.label.bytes.data = g_dlaaLabel;
    dlaa.value = -1;
    g_pushBack(values, &dlaa);
    if (values->count != 6 || !values->data) return false;
    entries = static_cast<ItemValue*>(values->data);
    ItemValue old[6];
    std::memcpy(old, entries, sizeof(old));
    // old: Off, Performance, Balanced, Quality, Ultra Performance, DLAA -> Off, DLAA, Quality, Balanced, Performance, Ultra
    const int from[6] = {0, 5, 3, 2, 1, 4};
    for (int i = 0; i < 6; ++i) std::memcpy(&entries[i], &old[from[i]], sizeof(ItemValue));
    *reinterpret_cast<std::int32_t*>(item + kItemSelected) = value < 6 ? kValueToIndex[value] : static_cast<std::int32_t>(value);
    return true;
}

bool ReorderList(std::uint8_t* item, const std::uint8_t* settings) {
    __try {
        return ReorderListUnsafe(item, *reinterpret_cast<const std::uint32_t*>(settings + kSettingsDlss));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall HookFillDlssOption(void* screen, std::uint8_t* item, std::uint8_t* settings) {
    if (settings && g_pendingIndex >= 0) {   // (a step on the list: its index into its value)
        *reinterpret_cast<std::uint32_t*>(settings + kSettingsDlss) = kIndexToValue[g_pendingIndex];
        g_pendingIndex = -1;
    }
    g_originalFillDlssOption(screen, item, settings);
    if (!item || !settings) return;
    const bool done = ReorderList(item, settings);
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        if (done) Log::Write("DLAA: Options > Display > DLSS lists DLAA (Off, DLAA, Quality, Balanced, Performance, Ultra Performance)");
        else Log::Write("DLAA: the DLSS list is not the game's five entries; left as it is");
    }
}

void __fastcall HookStepperChanged(void* screen, const std::uint8_t* params) {
    int id = -1, index = -1;
    __try {
        const auto* args = *reinterpret_cast<const std::uint8_t* const*>(params + 0x20);
        id = static_cast<int>(*reinterpret_cast<const double*>(args + 0x10));
        index = static_cast<int>(*reinterpret_cast<const double*>(args + 0x28));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        id = -1;
    }
    const bool dlss = id == kDlssOptionId && index >= 0 && index < 6 && g_listInstalled.load();
    if (dlss) g_pendingIndex = index;
    g_originalStepperChanged(screen, params);
    if (dlss && g_pendingIndex >= 0) {   // (the list was not built again: the value set here)
        g_pendingIndex = -1;
        __try {
            auto* pending = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(screen) + kPendingSettings);
            if (pending) *reinterpret_cast<std::uint32_t*>(pending + kSettingsDlss) = kIndexToValue[index];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

bool Verify(std::uintptr_t rva, const std::uint8_t* sig, std::size_t size, const char* what) {
    if (GameBuild::VerifyBytes(rva, sig, size)) return true;
    Log::Writef("DLAA: %s does not match this build", what);
    return false;
}

bool Hook(std::uintptr_t rva, void* detour, void** original, const char* what) {
    if (MH_CreateHook(reinterpret_cast<void*>(GameBuild::Base() + rva), detour, original) == MH_OK) return true;
    Log::Writef("DLAA: the hook on %s could not be created", what);
    return false;
}
}  // namespace

bool Install() {
    if (g_available.load()) return true;
    if (!GameBuild::IsSupportedRetail() ||
        !Verify(kRvaConvertScreenToRenderRes, kConvertSig, sizeof(kConvertSig), "NxDLSSImpl::ConvertScreenToRenderRes") ||
        !Verify(kRvaGetDisplaySettings, kGetDisplaySettingsSig, sizeof(kGetDisplaySettingsSig), "NxAppImpl::GetDisplaySettings") ||
        !Verify(kRvaUpdateState, kUpdateStateSig, sizeof(kUpdateStateSig), "NxDLSSImpl::UpdateState") ||
        !Verify(kRvaInitDlss, kInitDlssSig, sizeof(kInitDlssSig), "NxDLSSImpl::InitDLSS") ||
        !Verify(kRvaUpdateStateRead, kUpdateStateReadSig, sizeof(kUpdateStateReadSig), "UpdateState's read of the DLSS setting") ||
        !Verify(kRvaInitDlssRead, kInitDlssReadSig, sizeof(kInitDlssReadSig), "InitDLSS's read of the DLSS setting") ||
        !Verify(kRvaNxAppGetApp, kNxAppGetAppSig, sizeof(kNxAppGetAppSig), "NxApp_GetApp")) {
        Log::Write("DLAA: unavailable");
        return false;
    }
    const std::uintptr_t base = GameBuild::Base();
    g_updateStateRead = base + kRvaUpdateStateRead;
    g_initDlssRead = base + kRvaInitDlssRead;
    g_nxAppGetApp = reinterpret_cast<NxAppGetAppFn>(base + kRvaNxAppGetApp);
    if (Verify(kRvaSupportsRead, kSupportsReadSig, sizeof(kSupportsReadSig), "ResolutionSupportsDLSS's read of the DLSS setting"))
        g_supportsRead = base + kRvaSupportsRead;   // (the display modes DLSS supports, checked as Quality for DLAA)
    if (!Hook(kRvaConvertScreenToRenderRes, reinterpret_cast<void*>(&HookConvert), reinterpret_cast<void**>(&g_originalConvert),
              "ConvertScreenToRenderRes") ||
        !Hook(kRvaGetDisplaySettings, reinterpret_cast<void*>(&HookGetDisplaySettings),
              reinterpret_cast<void**>(&g_originalGetDisplaySettings), "GetDisplaySettings") ||
        !Hook(kRvaUpdateState, reinterpret_cast<void*>(&HookUpdateState), reinterpret_cast<void**>(&g_originalUpdateState),
              "UpdateState")) {
        MH_RemoveHook(reinterpret_cast<void*>(base + kRvaConvertScreenToRenderRes));
        MH_RemoveHook(reinterpret_cast<void*>(base + kRvaGetDisplaySettings));
        MH_RemoveHook(reinterpret_cast<void*>(base + kRvaUpdateState));
        Log::Write("DLAA: unavailable");
        return false;
    }
    g_available.store(true);
    // the list in Options > Display (without it a saved DLAA still works)
    if (Verify(kRvaFillDlssOption, kFillDlssOptionSig, sizeof(kFillDlssOptionSig), "FillDLSSOption") &&
        Verify(kRvaFillDlssIndexWrite, kFillDlssIndexWriteSig, sizeof(kFillDlssIndexWriteSig), "FillDLSSOption's selected index") &&
        Verify(kRvaStepperChanged, kStepperChangedSig, sizeof(kStepperChangedSig), "OnOptionStepperValueChanged") &&
        Verify(kRvaStepperDlssCase, kStepperDlssCaseSig, sizeof(kStepperDlssCaseSig), "OnOptionStepperValueChanged's DLSS case") &&
        Verify(kRvaItemValuePushBack, kItemValuePushBackSig, sizeof(kItemValuePushBackSig), "ArrayImpl<ItemValue>::push_back")) {
        g_pushBack = reinterpret_cast<PushBackFn>(base + kRvaItemValuePushBack);
        const bool fill = Hook(kRvaFillDlssOption, reinterpret_cast<void*>(&HookFillDlssOption),
                               reinterpret_cast<void**>(&g_originalFillDlssOption), "FillDLSSOption");
        const bool step = fill && Hook(kRvaStepperChanged, reinterpret_cast<void*>(&HookStepperChanged),
                                       reinterpret_cast<void**>(&g_originalStepperChanged), "OnOptionStepperValueChanged");
        if (fill && !step) MH_RemoveHook(reinterpret_cast<void*>(base + kRvaFillDlssOption));
        g_listInstalled.store(fill && step);
    }
    Log::Writef("DLAA: hooks created (%s)", g_listInstalled.load() ? "DLAA is an entry of Options > Display > DLSS"
                                                                    : "the DLSS list unchanged: a saved DLAA setting still works");
    return true;
}

bool Available() {
    return g_available.load();
}

bool ListInstalled() {
    return g_listInstalled.load();
}

LastSetup Last() {
    std::lock_guard<std::mutex> lock(g_lastMutex);
    return g_last;
}
}  // namespace Dlaa
