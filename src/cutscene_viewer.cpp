#include "cutscene_viewer.h"
#include "steam_save_cache.h"

#include <MinHook.h>
#include <intrin.h>
#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "custom_outfits.h"
#include "cutscenes.h"
#include "features.h"
#include "game_build.h"
#include "hud_watch.h"
#include "image_load.h"
#include "imgui.h"
#include "log.h"
#include "model_viewer.h"
#include "picture_frame.h"
#include "viewer_audio.h"
#include "viewer_text.h"

namespace CutsceneViewer {
namespace {

// ---- The main menu (retail RVAs; TR2 PDB names) ---------------------------------------------------------------------
// UiMenuStateMainMenu::OnEnter (TR2 0x94F900) sets the option count (+0x38) to 12. OnUiSelect (TR2 0x9750C0) plays the
// select sound (vfunc +0xC0, 3) and switches on the selected option (+0x2C): 9 Quit, 10 Credits, 11 Start Benchmark
// (UiMenuStateManager::SetState(-4) + PCBenchmark::Start). The movie's SetButtonText(number, string key) names an option
// (retail OnEnter: SetButtonText(9, "Menu_QuitLabel"); a string that is not a key is shown as it is).
// 0.9.342 tried a 13th option (the TR2 dev mode's SetDevMode): it shows a dev selector (with its own value and arrows,
// over the button legend) whose clicks are not "MenuLabel" events, so OnUiSelect never came. Now Start Benchmark (the
// last option, under Quit Game and Credits) is EXTRAS (0.9.359: the Extras menu; the benchmark is not offered any more).
// 0.9.397: EXTRAS above QUIT GAME: the options keep their places, their texts and actions are swapped (option 9 reads
// EXTRAS and opens the Extras menu, option 11 reads the game's "Menu_QuitLabel" and quits as option 9 does).
// The movie (State_MainMenu) has 13 options: the 13th (12, the dev mode's) is hidden when it loads (DisableDevMode ->
// HideOption: alpha 0, its mouse hitbox removed), but its OnEnter makes every option a mouse button again
// (CreateMouseButton for all 13: a hitbox under the last option). A click there reaches OnUiSelect with 12, which plays
// the select sound and does nothing (its switch stops at 11): HideOption(12) again after OnEnter removes the hitbox.
constexpr std::uintptr_t kRvaMainMenuOnEnter = 0x0090C640ull;
constexpr std::uint8_t kMainMenuOnEnterSig[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10,
                                                0x48, 0x89, 0x78, 0x18, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
                                                0x41, 0x57, 0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC, 0x90};
constexpr std::uintptr_t kRvaMainMenuOnUiSelect = 0x009311F0ull;
constexpr std::uint8_t kMainMenuOnUiSelectSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x01, 0xBA,
                                                   0x03, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0xFF, 0x90, 0xC0};
constexpr std::uintptr_t kRvaInvokeStateMovie = 0x008B9750ull;   // UiMenuStateBase::InvokeStateMovie
constexpr std::uint8_t kInvokeStateMovieSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                                 0x24, 0x10, 0x57, 0x48, 0x81, 0xEC, 0x40, 0x01};
constexpr std::size_t kStateSelected = 0x2C, kStateNavCount = 0x38, kStateVtablePlaySound = 0xC0;
constexpr int kRetailOptionCount = 12, kLeaderboardsOption = 6, kOptionsOption = 7, kQuitOption = 9, kCreditsOption = 10,
              kBenchmarkOption = 11, kDevOption = 12, kSelectSound = 3;
// The widgets under the options (0.9.438; TR2 PDB names, retail OnEnter checked): the username (UiWidgetUsername, widget
// type 0x4D: OnEnter 0x90C8EF lea edx, [r8 + 0x4E] (r8 = -1); UiWidgetManager::CreateWidget; mov [rdi + 0x1F0], eax:
// m_usernameWidgetID) and the news ticker (UiWidgetInfocast, type 0x22, made by GameStateMenuing::OnEnter into
// m_InfocastWidgetID, +0x78: OnEnter 0x90C740 NetManagerPC::Get -> vfunc +0x70 (2), 0x90C76D mov ebx, [r14 + 0x78];
// GetWidget; 0x90C798 vfunc +0x30, Show). Only the main menu's OnEnter shows them, its OnPreExit / OnExit hide them
// (UiWidgetInfocast::Process and RefreshInfocast fill the ticker, and stop while it is hidden; UiWidgetUsername's
// Process only names it). Hidden (vfunc +0x38, as GameStateMenuing::HideLiveInfoWidget) after OnEnter.
constexpr std::size_t kMainMenuUsernameWidget = 0x1F0, kGsmInfocastWidget = 0x78;
constexpr std::uintptr_t kRvaMainMenuUsernameStore = 0x0090C8EFull, kRvaMainMenuNetManager = 0x0090C740ull,
                         kRvaMainMenuInfocastRead = 0x0090C76Dull, kRvaMainMenuInfocastShow = 0x0090C798ull;
constexpr std::uint8_t kMainMenuUsernameStoreSig[] = {0x41, 0x8D, 0x50, 0x4E, 0x41, 0xFF, 0x52, 0x10,
                                                      0x8B, 0xD8, 0x89, 0x87, 0xF0, 0x01, 0x00, 0x00};
constexpr std::uint8_t kMainMenuNetManagerSig[] = {0xE8, 0x5B, 0x50, 0xC7, 0xFF, 0x8B, 0xD6, 0x48, 0x8B,
                                                   0xC8, 0x4C, 0x8B, 0x00, 0x41, 0xFF, 0x50, 0x70};
constexpr std::uint8_t kMainMenuInfocastReadSig[] = {0x41, 0x8B, 0x5E, 0x78, 0xE8, 0x6A, 0x6E, 0xFA, 0xFF, 0x8B, 0xD3, 0x48,
                                                     0x8B, 0xC8, 0xE8, 0xE0, 0xDD, 0x76, 0xFF, 0x48, 0x8B, 0xD8, 0x48, 0x85};
constexpr std::uint8_t kMainMenuInfocastShowSig[] = {0x48, 0x8B, 0x13, 0x48, 0x8B, 0xCB, 0xF3, 0x0F, 0x10,
                                                     0x0D, 0x5E, 0x44, 0x40, 0x00, 0xFF, 0x52, 0x30};
bool g_menuWidgetsReady = false;
// The pause menu's online widgets (0.9.455): UiMenuStatePause::OnEnter (retail 0x90D530; TR2 0x950DD0) makes and shows
// UiWidgetLiveLevelStats (type 0x7B, m_... +0x244: the player's name and the level's online stats, when the profile's live
// notifications are on, outside game modes 5 and 10) and UiWidgetInfocast (type 0x22, +0x24C: the Square Enix online
// services' notice, unless in VR). Both through OpenAndShowAsync (vfunc +0x28: shown when their movies have loaded) or,
// made already, Show (+0x30), so hiding them after OnEnter would not last: each block is jumped over instead (the
// first's mov ebx, [rdi + 0x244] becomes jmp 0x90E39D, the second's jne 0x90E410 after IsVrGamePlayActive a jmp). Their
// IDs stay -1; OnPreExit / CloseWidgets skip widgets that are not there.
constexpr std::uintptr_t kRvaPauseLiveStats = 0x0090E333ull, kRvaPauseInfocast = 0x0090E3A4ull;
constexpr std::uint8_t kPauseLiveStatsSig[] = {0x8B, 0x9F, 0x44, 0x02, 0x00, 0x00, 0xE8};     // mov ebx, [rdi + 0x244]; call
constexpr std::uint8_t kPauseLiveStatsGateSig[] = {0x83, 0xF8, 0x0A, 0x74, 0x6A};             // cmp eax, 0xA; je 0x90E39D
constexpr std::uint8_t kPauseInfocastSig[] = {0x84, 0xC0, 0x75, 0x6A, 0x8B, 0x9F, 0x4C, 0x02, 0x00, 0x00};   // (from +0x90E3A2)
constexpr std::uint8_t kPauseLiveStatsPatch[] = {0xEB, 0x68, 0x90, 0x90, 0x90, 0x90};         // jmp 0x90E39D
constexpr std::uint8_t kPauseInfocastPatch[] = {0xEB, 0x6A};                                  // jmp 0x90E410
bool WritePauseCode(std::uintptr_t rva, const std::uint8_t* bytes, std::size_t size) {
    auto* at = reinterpret_cast<std::uint8_t*>(GameBuild::Base() + rva);
    DWORD old = 0;
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(at, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    DWORD ignored = 0;
    VirtualProtect(at, size, old, &ignored);
    return true;
}
void RemovePauseOnlineWidgets() {
    if (!GameBuild::VerifyBytes(kRvaPauseLiveStats, kPauseLiveStatsSig, sizeof(kPauseLiveStatsSig)) ||
        !GameBuild::VerifyBytes(kRvaPauseLiveStats - 5, kPauseLiveStatsGateSig, sizeof(kPauseLiveStatsGateSig)) ||
        !GameBuild::VerifyBytes(kRvaPauseInfocast - 2, kPauseInfocastSig, sizeof(kPauseInfocastSig))) {
        Log::Write("CutsceneViewer: the pause menu's widget code does not match; its online stats and notice stay");
        return;
    }
    const bool ok = WritePauseCode(kRvaPauseLiveStats, kPauseLiveStatsPatch, sizeof(kPauseLiveStatsPatch)) &&
                    WritePauseCode(kRvaPauseInfocast, kPauseInfocastPatch, sizeof(kPauseInfocastPatch));
    Log::Detail(ok ? "CutsceneViewer: the pause menu shows no username, online stats or online services notice"
                   : "CutsceneViewer: the pause menu's widget code could not be changed");
}
constexpr int kExtrasShownAt = kQuitOption, kQuitShownAt = kBenchmarkOption;   // (EXTRAS above QUIT GAME)

// The user's sound volumes (SOUND_GetUser/SetUser Music, Sfx, Speech volume: retail 0x3B7AB0 / 0x3B7AD0 / 0x3B7AF0 read
// *(float*)global * 128; the setters store the float and clear the next one). The fades scale the floats and put them back.
constexpr std::uintptr_t kRvaVolumeGlobals[3] = {0x02400E48ull, 0x02458BD8ull, 0x02458BE0ull};
constexpr std::uintptr_t kRvaVolumeGetters[3] = {0x003B7AB0ull, 0x003B7AD0ull, 0x003B7AF0ull};

struct GfxValue {   // Scaleform GFx 3 GFxValue
    void* iface = nullptr;
    std::uint32_t type = 0;   // 3 number, 4 string
    std::uint32_t pad = 0;
    union {
        double number;
        const char* string;
        void* data;
    };
    GfxValue() : data(nullptr) {}
};

using OnEnterFn = bool(__fastcall*)(void*, void*, void*);
using OnUiSelectFn = void(__fastcall*)(void*, char);
using InvokeStateMovieFn = bool(__fastcall*)(void*, const char*, GfxValue*, const GfxValue*, unsigned int);
OnEnterFn g_origOnEnter = nullptr;
OnUiSelectFn g_origOnUiSelect = nullptr;
InvokeStateMovieFn g_invokeStateMovie = nullptr;
bool g_hooksReady = false;
std::atomic<int> g_menuOption{0};          // 0 not yet, 1 EXTRA shown
std::atomic<std::uint32_t> g_menuEnters{0};
std::atomic<void*> g_mainMenu{nullptr};      // the main menu's state (UiMenuStateMainMenu)

template <typename T>
T& At(void* base, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(base) + offset);
}


std::atomic<WORD> g_padHeld[4]{};
std::atomic<bool> g_abortRequest{false};

// ---- Playback (game thread) ------------------------------------------------------------------------------------------
enum class Play : int { None, FadeOut, Waiting, FadeIn, Showing, FadeOutEnd, Returning, MenuSettle, MenuOpening, MenuFadeIn };
std::atomic<Play> g_play{Play::None};
std::atomic<float> g_black{0.0f};
std::mutex g_requestMutex;
struct PlayRequest {
    bool pending = false;
    std::string id, title;
    int outfit = -1;   // Features available-outfit index; -1 the story's
} g_request;
std::string g_playId, g_playTitle;
int g_playOutfit = -1;
double g_t0 = 0.0, g_fadeLength = 0.5;
std::uint32_t g_entersAtStart = 0;
bool g_abortPending = false, g_abortSent = false, g_menuForced = false;
float g_savedVolumes[3] = {1.0f, 1.0f, 1.0f};
bool g_volumesSaved = false;
int g_savedProfile = -1;
bool g_swapAfterLoad = false;   // the outfit is swapped to after the load (a node-started cutscene)
double g_swapStart = 0.0, g_swapDoneAt = 0.0;
std::uint16_t g_wantedObject = 0;
unsigned int g_wantedRaw = 0;
bool g_swapTried = false, g_outfitApplied = false;
std::mutex g_messageMutex;
std::string g_message;   // shown in the viewer when it comes back (an error)
constexpr double kFade = 0.5;

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void SetMessage(const std::string& text) {
    std::lock_guard lock(g_messageMutex);
    g_message = text;
    if (!text.empty()) Log::Writef("CutsceneViewer: %s", text.c_str());
}

float* VolumeSlot(int i) {
    static const bool verified = [] {
        // each getter: mov rax,[rip+global]; movss xmm0,[rax]
        for (int k = 0; k < 3; ++k) {
            const auto* code = reinterpret_cast<const std::uint8_t*>(GameBuild::Base() + kRvaVolumeGetters[k]);
            if (code[0] != 0x48 || code[1] != 0x8B || code[2] != 0x05) return false;
            const std::int32_t rel = *reinterpret_cast<const std::int32_t*>(code + 3);
            if (kRvaVolumeGetters[k] + 7 + static_cast<std::intptr_t>(rel) != kRvaVolumeGlobals[k]) return false;
        }
        return true;
    }();
    if (!verified) return nullptr;
    return *reinterpret_cast<float**>(GameBuild::Base() + kRvaVolumeGlobals[i]);
}

// While the fades hold the volumes, the getters (the options screen, the profile's save) answer the user's own values.
using VolumeGetterFn = int(__fastcall*)();
VolumeGetterFn g_origVolumeGetters[3] = {};
std::atomic<bool> g_volumesHeld{false};
float g_heldVolumes[3] = {1.0f, 1.0f, 1.0f};
template <int I>
int __fastcall HookVolumeGetter() {
    if (g_volumesHeld.load()) return int(g_heldVolumes[I] * 128.0f);
    return g_origVolumeGetters[I]();
}

void SaveVolumes() {
    for (int i = 0; i < 3; ++i) {
        float* slot = VolumeSlot(i);
        g_savedVolumes[i] = slot ? *slot : 1.0f;
        g_heldVolumes[i] = g_savedVolumes[i];
    }
    g_volumesSaved = VolumeSlot(0) != nullptr;
    g_volumesHeld = g_volumesSaved;
}

void ApplyGain(float gain) {
    if (!g_volumesSaved) return;
    gain = std::clamp(gain, 0.0f, 1.0f);
    for (int i = 0; i < 3; ++i)
        if (float* slot = VolumeSlot(i)) {
            slot[0] = g_savedVolumes[i] * gain;
            slot[1] = 0.0f;   // as SOUND_SetUser*Volume
        }
}

void RestoreVolumes() {
    if (!g_volumesSaved) return;
    ApplyGain(1.0f);
    g_volumesSaved = false;
    g_volumesHeld = false;
}

// NetPlayerInfo::SetCharacterProfileIndex (retail 0x58F3B0): while a chosen outfit plays, the chapter's own profile
// (set while it loads) is replaced by the chosen one, so Lara spawns in it.
constexpr std::uintptr_t kRvaSetProfileIndex = 0x0058F3B0ull;
constexpr std::uint8_t kSetProfileIndexSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57,
                                                0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x01, 0x41, 0x0F, 0xB6, 0xF0};
using SetProfileIndexFn = void(__fastcall*)(void*, char, bool);
SetProfileIndexFn g_origSetProfileIndex = nullptr;
std::atomic<int> g_forcedProfile{-1};
std::atomic<int> g_profileOverrides{0};
std::atomic<int> g_gameProfile{-1};   // the profile the chapter asked for while ours was forced
void __fastcall HookSetProfileIndex(void* info, char profile, bool flag) {
    const int forced = g_forcedProfile.load();
    if (forced >= 0 && profile != static_cast<char>(forced)) {
        g_gameProfile = int(profile);
        if (g_profileOverrides.fetch_add(1) < 4)
            Log::Writef("CutsceneViewer: the game sets character profile %d; %d (the chosen outfit) instead", int(profile), forced);
        profile = static_cast<char>(forced);
    }
    g_origSetProfileIndex(info, profile, flag);
}

// Lara's starting items come from the object she spawns as (InventoryComponent::LoadStartupInventory, TR2 0x63B400: the
// object's own InventoryData unless its ObjectFlags have 0x20000, then the character profile's): spawned in the chosen
// outfit she lacked the chapter's pistol / bow / glowstick, and the cutscenes that use one waited forever. A respawn
// keeps the inventory (NetPlayerInfo::RespawnAuthoritativePlayer adds references to it before destroying the player),
// so for a cutscene the level does not play while it loads (Cutscenes::ViewerTrigger not "load": "node", or not known
// yet, or one that comes by way of another, as Execution) the chapter loads in the story's outfit, the outfit is
// swapped after the load (EnsureOutfit), and the node waits until Lara wears it. (0.9.361 swapped only for "node":
// Execution, its trigger never learned, loaded in the chosen outfit and The Reckoning on its way waited for actor -102,
// the player's inventory item of type 2, until the viewer gave up.) A "load" cutscene whose trigger was not known yet
// played in the story's outfit the first time (0.9.363: Cathedral Ambush, Aftermath); the triggers seen in game are known
// from the start since 0.9.364 (Cutscenes' KnownTrigger).

// While a cutscene plays from the viewer (from its load to the menu):
// - DLCLicenseCheck (an action graph node, retail ProcessMsg 0x756DE0; TR2 0x766200): with +0x22 set it asks a yes/no
//   ("buy it?") or a confirmation through NotificationDisplayManager after PauseQueue::RequestPause; its callbacks (OkCB,
//   ShowInGameStoreCB) only unpause. +0x22 is cleared around the call: the node still sends whether the feature is usable.
// - PauseQueue::RequestPause (retail 0x41DAE0: queue, callback, group, debug name): logged, to see what stops a cutscene.
// - UiWidgetSavingIcon::Show (retail 0x9C8C90, its vtable's Show): not shown (the chapter's saves showed it over the
//   cutscene; its 3 s timer runs on game time, which stands while the game is paused).
constexpr std::uintptr_t kRvaDlcCheckProcess = 0x00756DE0ull, kRvaRequestPause = 0x0041DAE0ull, kRvaSavingIconShow = 0x009C8C90ull;
constexpr std::uint8_t kDlcCheckProcessSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57,
                                                0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x50};
constexpr std::uint8_t kRequestPauseSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89,
                                             0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xF9};
constexpr std::uint8_t kSavingIconShowSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xF3, 0x0F, 0x10, 0x0D, 0x4A, 0x40,
                                               0x34, 0x00, 0x48, 0x8B, 0xD9, 0xE8};
using NodeProcessFn = void(__fastcall*)(void*, int, void*);
using RequestPauseFn = void(__fastcall*)(void*, void*, int, const char*);
using SavingIconShowFn = bool(__fastcall*)(void*, float);
NodeProcessFn g_origDlcCheckProcess = nullptr;
RequestPauseFn g_origRequestPause = nullptr;
SavingIconShowFn g_origSavingIconShow = nullptr;
std::atomic<int> g_pauseLogs{0}, g_dlcLogs{0};
std::atomic<bool> g_savingIconLogged{false};

bool PlaybackActive() { return g_play.load() != Play::None; }

void __fastcall HookDlcCheckProcess(void* node, int msg, void* data) {
    if (!PlaybackActive()) return g_origDlcCheckProcess(node, msg, data);
    auto* n = static_cast<std::uint8_t*>(node);
    const std::uint8_t show = n[0x22];
    if (show && msg == 2 && g_dlcLogs.fetch_add(1) < 6)
        Log::Writef("CutsceneViewer: a DLC check's message left out (feature %u)", *reinterpret_cast<std::uint32_t*>(n + 0x24));
    n[0x22] = 0;
    g_origDlcCheckProcess(node, msg, data);
    n[0x22] = show;
}

void __fastcall HookRequestPause(void* queue, void* callback, int group, const char* name) {
    if (PlaybackActive() && g_pauseLogs.fetch_add(1) < 24)
        Log::Writef("CutsceneViewer: the game asks for a pause (group %d, \"%.60s\", from ROTTR.exe+0x%llX)", group,
                    name ? name : "", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(_ReturnAddress()) -
                                                                      GameBuild::Base()));
    g_origRequestPause(queue, callback, group, name);
}

// UIUtils::StartPauseGameWorld / EndPauseGameWorld (retail 0x8C3FB0 / 0x8B2E20; TR2 0x906520 / 0x8F4EB0): the sound
// groups paused, PauseQueue::BlockPause, a pause group byte. GameTutorialStep (ProcessMsgDefaultMode: group 8) pauses
// the world for a card until the card is answered (EndTutorial -> EndPauseGameWorld), AGFreezeGame too. The DLC card in
// Cathedral Ambush / The Race froze the cutscene that way (0.9.351: frame 71 / 67, the game "running", no RequestPause):
// with the card left out nothing ended the pause. While the viewer plays, the world is not paused; the matching end is
// skipped as well.
constexpr std::uintptr_t kRvaStartPauseWorld = 0x008C3FB0ull, kRvaEndPauseWorld = 0x008B2E20ull;
constexpr std::uint8_t kStartPauseWorldSig[] = {0x48, 0x83, 0xEC, 0x28, 0xB2, 0x01, 0xE8, 0xA5, 0x3E, 0xAF, 0xFF, 0xE8};
constexpr std::uint8_t kEndPauseWorldSig[] = {0x48, 0x83, 0xEC, 0x28, 0x33, 0xD2, 0xE8, 0x35, 0x50, 0xB0, 0xFF, 0xE8};
using PauseWorldFn = void(__fastcall*)(int);
PauseWorldFn g_origStartPauseWorld = nullptr, g_origEndPauseWorld = nullptr;
std::atomic<int> g_skippedWorldPauses{0};

void __fastcall HookStartPauseWorld(int group) {
    if (PlaybackActive()) {
        if (g_skippedWorldPauses.fetch_add(1) < 6)
            Log::Writef("CutsceneViewer: the game's world pause (group %d) left out while a cutscene plays", group);
        return;
    }
    g_origStartPauseWorld(group);
}

void __fastcall HookEndPauseWorld(int group) {
    int skipped = g_skippedWorldPauses.load();
    while (skipped > 0)
        if (g_skippedWorldPauses.compare_exchange_weak(skipped, skipped - 1)) {   // (its start was left out)
            if (!PlaybackActive())
                Log::Writef("CutsceneViewer: the end of a world pause (group %d) taken for one left out during the cutscene", group);
            return;
        }
    g_origEndPauseWorld(group);
}

// Full-screen videos the level plays before the cutscene (The Race: two .bik movies after the Gulag): skipped as the
// game's own skip does it, MOVIE_ExternallySkipped (retail 0x1EBB70: a flag MOVIE_PlaySharedInternalUpdate reads),
// while MOVIE_IsPlaying (retail 0x1EBE00).
// The game takes that skip only once the level's streams are all in (MOVIE_PlaySharedInternalUpdate, TR2 0x207040: the
// video's AllowSkip and STREAM_AreAllStreamsLoadedAndPlacementsBirthed): The Race's first video is the loading's own (its
// Chapter Select entry names it: Gameloop::InitLoadingDisplay plays it while the level loads) and played on in 0.9.360.
// So the video's end flag is set too (retail 0x165AFC0: MOVIE_Play clears it, retail 0x1EBF74; MOVIE_PlayInternalUpdate
// ends the video when it is set, 0x1EC1CE, as the skip's fade does after 0.75 s): the video ends at its next frame, and
// when the load is not over Gameloop::LoadingDisplay (TR2 0x486160) goes on with the game's loading screen
// (MOVIE_PlayEnd, state 0x4F), as when a loading video ends before the load.
constexpr std::uintptr_t kRvaMovieEndCheck = 0x001EC1CEull, kRvaMoviePlayClear = 0x001EBF74ull,
                         kRvaMovieEndFlag = 0x0165AFC0ull;
constexpr std::uint8_t kMovieEndCheckSig[] = {0x0F, 0xB6, 0x05, 0xEB, 0xED, 0x46, 0x01};   // movzx eax, byte [end flag]
constexpr std::uint8_t kMoviePlayClearSig[] = {0xC6, 0x05, 0x45, 0xF0, 0x46, 0x01, 0x00};  // mov byte [end flag], 0
constexpr std::uintptr_t kRvaMovieSkip = 0x001EBB70ull, kRvaMovieIsPlaying = 0x001EBE00ull;
constexpr std::uint8_t kMovieSkipSig[] = {0xC6, 0x05, 0xFF, 0xF3, 0x46, 0x01, 0x01, 0xC3};
constexpr std::uint8_t kMovieIsPlayingSig[] = {0x48, 0x83, 0x3D, 0x90, 0xF1, 0x46, 0x01, 0x00, 0x0F, 0x95, 0xC0, 0xC3};
bool g_movieFnsReady = false, g_movieEndReady = false;
double g_movieSkipAt = 0.0;
int g_moviesSkipped = 0;

void SkipMovies(double now) {
    if (!g_movieFnsReady || now < g_movieSkipAt) return;
    if (!reinterpret_cast<bool(__fastcall*)()>(GameBuild::Base() + kRvaMovieIsPlaying)()) return;
    g_movieSkipAt = now + 0.5;   // (again while it still plays)
    reinterpret_cast<void(__fastcall*)()>(GameBuild::Base() + kRvaMovieSkip)();
    if (g_movieEndReady) *reinterpret_cast<volatile std::uint8_t*>(GameBuild::Base() + kRvaMovieEndFlag) = 1;
    if (g_moviesSkipped++ < 8)
        Log::Writef("CutsceneViewer: a video the level plays before the cutscene skipped%s",
                    g_movieEndReady ? " (ended at its next frame)" : "");
}

// While the viewer plays a cutscene (from its load to the menu) no team is the enemy of another: GameTeam::
// TeamsAreEnemies(a, b) (retail 0x9F7790, TR2 0xA3DF90) answers false. It is what the AI asks about everyone (45 TR2
// callers: NpcComponent::IsEnemyOf, its sensing (SensedInstance_CheckAdd, NpcEvent_Detected), targets
// (Target_AddInfo, Target_CheckAwarenessForCombat, Target_ComputeValue), ResetEnemyAwareness, turrets (Goal_UseTurret),
// NotifyNpcHitNpc / NotifyNpcHitPlayer; NpcComponent::TakeDamage only uses it to know whether the attacker was an enemy,
// the damage is dealt either way). Retreat's start (PCAP: 41 Retreat: the tower's mini-boss checkpoint) keeps a fight
// going around the cutscene: 0.9.361 kept the NPCs off Lara (NpcComponent::IsEnemyOf answered false for her), and the
// shooting and explosions went on all the same (presumably among the level's teams). The cutscene's actors are driven
// by the cutscene, not by their AI.
constexpr std::uintptr_t kRvaTeamsAreEnemies = 0x009F7790ull;
constexpr std::uint8_t kTeamsAreEnemiesSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B,
                                                0xF9, 0x8B, 0xDA, 0x48, 0x8B, 0x0D, 0xAB, 0x43, 0x39, 0x02};
using TeamsAreEnemiesFn = bool(__fastcall*)(int, int);
TeamsAreEnemiesFn g_origTeamsAreEnemies = nullptr;
std::atomic<bool> g_enemyIgnoreLogged{false};

bool __fastcall HookTeamsAreEnemies(int a, int b) {
    if (PlaybackActive()) {
        if (!g_enemyIgnoreLogged.exchange(true))
            Log::Write("CutsceneViewer: no team counts another as its enemy while the cutscene plays (the level's fights stop)");
        return false;
    }
    return g_origTeamsAreEnemies(a, b);
}

// Gameplay dialogue and barks while the viewer's cutscene plays (0.9.363's To the Tower: the level's people went on
// talking behind it). DialogueManager::Process(manager, dt) (retail 0x55A650, TR2 0x58DA40: called each frame by the game
// loop, retail 0x4807FC, TR2 0x4B0A31, then BarkComponentManager::Process) is skipped while the cutscene plays, so that no
// dialogue starts or goes on to its next line, and the dialogues in its pipelines then (DialogueManager +0x348, retail;
// TR2 +0x358: three arrays of Dialogue*, 10, 10 and 12 slots, by dialogue type, as AreAnyDialoguesPlaying, retail
// 0x553700, reads them) have their sounds stopped (Dialogue::StopAllSounds, retail 0x55D380, TR2 0x5906E0: what
// Dialogue::AbortDialogue calls). BarkComponentManager::Process (retail 0x3ACA50, TR2 0x3D0680) is skipped too: no new
// barks. A cutscene's own lines are in its stream. Both go on as before between two cutscenes of a chain.
constexpr std::uintptr_t kRvaDialogueProcess = 0x0055A650ull, kRvaDialogueStopSounds = 0x0055D380ull,
                         kRvaBarkProcess = 0x003ACA50ull;
constexpr std::uint8_t kDialogueProcessSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x80, 0xB9, 0x3C, 0x03,
                                                0x00, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0x0F, 0x29, 0x74, 0x24};
constexpr std::uint8_t kDialogueStopSoundsSig[] = {0x40, 0x55, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x83, 0x79, 0x58,
                                                   0x00, 0x48, 0x8B, 0xE9, 0x0F, 0x84, 0xA3, 0x00, 0x00, 0x00};
constexpr std::uint8_t kBarkProcessSig[] = {0x40, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8D,
                                            0x6C, 0x24, 0x20, 0x48, 0x89, 0x5D, 0x40, 0x48, 0x8B, 0x1D};
constexpr std::size_t kDialoguePipelines = 0x348;
constexpr int kDialoguePipelineSlots[3] = {10, 10, 12};
using DialogueProcessFn = void(__fastcall*)(void*, float);
using BarkProcessFn = void(__fastcall*)();
DialogueProcessFn g_origDialogueProcess = nullptr;
BarkProcessFn g_origBarkProcess = nullptr;
bool g_dialogueStopReady = false;
std::atomic<bool> g_dialoguesStopped{false}, g_barksLogged{false};

bool CutscenePlayingNow() { return PlaybackActive() && Cutscenes::CurrentStage() == Cutscenes::Stage::Playing; }

// The dialogues in the manager's pipelines, their sounds stopped. Their count (-1: not readable). SEH only.
int StopPipelineDialogues(void* manager, int* byType) noexcept {
    __try {
        int stopped = 0;
        auto* m = static_cast<std::uint8_t*>(manager);
        for (int type = 0; type < 3; ++type) {
            byType[type] = 0;
            auto** list = *reinterpret_cast<void***>(m + kDialoguePipelines + std::size_t(type) * 8);
            if (!list) continue;
            for (int i = 0; i < kDialoguePipelineSlots[type]; ++i) {
                if (!list[i]) continue;
                if (g_dialogueStopReady)
                    reinterpret_cast<void(__fastcall*)(void*)>(GameBuild::Base() + kRvaDialogueStopSounds)(list[i]);
                ++byType[type];
                ++stopped;
            }
        }
        return stopped;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void __fastcall HookDialogueProcess(void* manager, float dt) {
    if (!CutscenePlayingNow()) {
        g_dialoguesStopped = false;
        g_origDialogueProcess(manager, dt);
        return;
    }
    if (!g_dialoguesStopped.exchange(true) && manager) {
        int byType[3] = {};
        const int stopped = StopPipelineDialogues(manager, byType);
        Log::Writef("CutsceneViewer: the level's dialogue held while the cutscene plays; %d dialogue%s playing then (by type: "
                    "%d, %d, %d)%s", stopped, stopped == 1 ? "" : "s", byType[0], byType[1], byType[2],
                    stopped > 0 ? (g_dialogueStopReady ? ", their sounds stopped" : ", not stopped (code does not match)") : "");
    }
}

void __fastcall HookBarkProcess() {
    if (CutscenePlayingNow()) {
        if (!g_barksLogged.exchange(true)) Log::Write("CutsceneViewer: no barks while the cutscene plays");
        return;
    }
    g_origBarkProcess();
}

bool __fastcall HookSavingIconShow(void* widget, float time) {
    if (PlaybackActive()) {
        if (!g_savingIconLogged.exchange(true)) Log::Write("CutsceneViewer: the saving icon is not shown while a cutscene plays");
        return false;
    }
    return g_origSavingIconShow(widget, time);
}

void ApplyOutfitBeforeLoad() {
    g_outfitApplied = false;
    g_swapTried = false;
    g_wantedObject = 0;
    g_savedProfile = -1;
    if (g_playOutfit < 0) return;
    Features::OutfitInfo info;
    if (!Features::GetOutfitInfo(g_playOutfit, info)) {
        Log::Writef("CutsceneViewer: outfit %d is not available; the story's outfit is kept", g_playOutfit);
        return;
    }
    g_wantedObject = info.playerObjectId;
    g_wantedRaw = info.outfitIndex;
    g_savedProfile = Features::LocalPlayerCharacterProfile();
    g_gameProfile = -1;
    g_forcedProfile = info.characterProfileIndex;
    g_profileOverrides = 0;
    Features::SetCutsceneViewerOutfitLock(true);
    g_outfitApplied = true;
    g_swapAfterLoad = Cutscenes::ViewerTrigger(g_playId) != "load";
    if (g_swapAfterLoad) {   // (the chapter's kit comes with the story's outfit; the swap keeps it)
        g_forcedProfile = -1;
        g_swapDoneAt = 0.0;
        g_swapStart = Now();
        Cutscenes::HoldNodeFiring(true);
        Log::Writef("CutsceneViewer: outfit %s (profile %d, object %u): swapped to after the load (the chapter's items "
                    "come with the story's outfit); the story's outfit changes are held", info.objectName,
                    info.characterProfileIndex, unsigned(info.playerObjectId));
        return;
    }
    const bool set = Features::SetLocalPlayerCharacterProfile(info.characterProfileIndex);
    Log::Writef("CutsceneViewer: outfit %s (profile %d, object %u): %s; the story's outfit changes are held", info.objectName,
                info.characterProfileIndex, unsigned(info.playerObjectId),
                set ? "set for the next spawn" : "the player info is not there yet (a swap after the load)");
}

void RestoreOutfit() {
    g_forcedProfile = -1;
    Cutscenes::HoldNodeFiring(false);
    g_swapAfterLoad = false;
    if (!g_outfitApplied) return;
    Features::SetCutsceneViewerOutfitLock(false);
    if (g_savedProfile >= 0) Features::SetLocalPlayerCharacterProfile(g_savedProfile);
    g_outfitApplied = false;
}

// While the chapter loads: the chosen profile kept (the game may write it directly too).
void KeepProfile() {
    const int forced = g_forcedProfile.load();
    if (!g_outfitApplied || forced < 0) return;
    const int now = Features::LocalPlayerCharacterProfile();
    if (now >= 0 && now != forced) Features::SetLocalPlayerCharacterProfile(forced);
}

// After the load, before the cutscene: Lara in the chosen outfit if the spawn did not take it.
void EnsureOutfit() {
    if (g_swapAfterLoad && g_swapTried) {   // the node waits until she wears it (0.5 s more; 1 s up to 0.9.363), 25 s at most
        const bool worn = Features::CurrentOutfitObjectId() == g_wantedObject && !Features::OutfitSwapInProgress();
        if (worn && g_swapDoneAt == 0.0) g_swapDoneAt = Now();
        if ((worn && Now() - g_swapDoneAt >= 0.5) || Now() - g_swapStart > 25.0) {
            if (!worn) Log::Write("CutsceneViewer: the outfit swap did not finish; the cutscene goes on");
            g_swapAfterLoad = false;
            Cutscenes::HoldNodeFiring(false);
        }
        return;
    }
    if (!g_outfitApplied || g_swapTried || !g_wantedObject) return;
    const std::uint16_t current = Features::CurrentOutfitObjectId();
    if (!current) return;
    if (current == g_wantedObject) {
        g_swapTried = true;
        return;
    }
    if (Features::OutfitSwapInProgress() || Cutscenes::AnyCutscenePlaying()) return;
    g_swapTried = true;
    g_swapStart = Now();
    const bool ok = Features::RequestOutfitSwap(g_wantedRaw);
    if (!ok && g_swapAfterLoad) {
        g_swapAfterLoad = false;
        Cutscenes::HoldNodeFiring(false);
    }
    Log::Writef("CutsceneViewer: Lara wears object %u after the load; swap to %u %s", unsigned(current),
                unsigned(g_wantedObject), ok ? "requested" : "refused");
}

void Finish() {
    // (v0.9.471) a world pause left out while the cutscene played whose end never came (a tutorial card left out:
    // nothing ends it) must not take the end of the next world pause after the viewer: that one stayed (PauseQueue
    // blocked: no pause menu, no map; the world-pause byte set)
    if (const int owed = g_skippedWorldPauses.exchange(0))
        Log::Writef("CutsceneViewer: %d world pause(s) left out during the cutscene never ended; forgotten (the next world "
                    "pause's end is the game's)", owed);
    RestoreVolumes();
    RestoreOutfit();
    Features::SetCutsceneViewerNoTutorials(false);
    Cutscenes::EndViewerMode();
    g_black = 0.0f;
    g_play = Play::None;
}

float Progress(double now, double length) { return float(std::clamp((now - g_t0) / std::max(0.05, length), 0.0, 1.0)); }

// ---- The Extras menu and the Cutscene Viewer screen ------------------------------------------------------------------
// EXTRAS opens the Extras menu: the game's Croft Manor menu (UiMenuStateCroftManor, state 0x93; movie tr2_croftmanor,
// class State_CroftManor) driven by us, with CUTSCENE VIEWER, MODEL VIEWER (greyed out) and CREDITS (the game's credits,
// state 0x0B, as the main menu's Credits opens them). CUTSCENE VIEWER opens the Cutscene Viewer screen: the game's
// Expeditions screen (UiMenuStateLiveSelectionCombo, state 0x82) driven by us. Each is the game's own state object with a
// copy of its vtable whose entries we need are ours; UiMenuStateManager::SetState opens it. The manager's history (+0x9B0,
// 32 states, SetStatePrevious pops it) brings Back from the viewer (or the credits) to the Extras menu and from there to the
// main menu. ChangeState calls the current state's OnExit(current, next) when the next state has +0xD5 bit 1 (the
// UiMenuStateBase default: the combo, Croft Manor and the credits have it), then the next one's OnEnter(previous, current).
//
// The Extras menu (retail UiMenuStateCroftManor: vtable 0xDBCD80, OnUiSelect 0x930F90, OnEnter 0x909010,
// OnInputActionPressed 0x91BCB0, Process 0x93E1F0, PushHighlightIndexToMovie 0x943750) as its OnEnter builds it:
// UiMenuStateGeneric::OnEnter, the movie's GetNumNavItems (10 slots), the option count (+0x38) and the slot count (+0x160),
// per entry the movie's SetButtonText(i, text) (the native SetItemText, retail 0x948FE0, passes a localized key to it) and
// PushEnable/PushDisableIndexToMovie, HideOption(i) + PushDisableIndexToMovie for the other slots, left/right off
// (NavigationDisableInputDirection), RegisterNavItems, the selection kept if accessible (else the first accessible one),
// PushHighlightIndexToMovie, InitializeContextbarWidget (its OnContextbarInitialized: Select, Back). The highlight writes the
// entry's description to "<state clip path>.text_txt.text" (UiMenuStateBase::SetVariable; the path at +0x88). The title
// ("$MultiMenu_CroftManor") is an unnamed Component_BasicLabel placed at depth 502 of State_CroftManor:
// getInstanceAtDepth(502 - 16384) (timeline depths are -16384 based), SetByCodeUsage, SetText("EXTRAS", false); put back
// when the menu is left for the main menu. Cancel: sound 5 and SetStatePrevious(true, 0.25) as the game's. Its Process (the
// VR headset's availability) and its list (+0x150) are not used.
//
// The Cutscene Viewer screen: the combo's OnEnter / input / mouse / OnUiSelect / Process / OnExit are ours (the rest: the
// combo's or UiMenuStateGeneric's). The screen's movie (live_menucomboselection, class State_MenuComboSelection) is filled
// through its own functions: ShowReplayModeSettings (Chapter Replay's layout: rows 0..2, row 3 hidden by its alpha,
// Continue at nav index 4), nav_list_item_N_mc.SetText (row labels), ChangeSpinnerValue(row, text, dir), the dots (that
// layout: mode = row 0, level = row 1, difficulty = row 2), SetContinueText, OnHighlight / OnDisable / OnEnable. The header
// widget (live_widgetheader: SetTitle, ClearAllRibbonItems, AddRibbonItem, SetActiveRibbon) and the info widget
// (live_widgetinfo: SetFrame("Info"), SetMode, SetLevel, SetDifficultyDescription, PlayAnimation...) are
// GameStateMenuing's (ShowLiveHeaderWidget / ShowLiveInfoWidget).
constexpr std::uintptr_t kRvaStateManager = 0x02E1E758ull;      // UiMenuStateManager* (main menu OnUiSelect, 0x9312E9)
constexpr std::uintptr_t kRvaSetState = 0x008C2D70ull;          // UiMenuStateManager::SetState(const MENU_STATE&, float)
constexpr std::uintptr_t kRvaGetStateObjectPtr = 0x008B4C90ull; // UiMenuStateManager::GetStateObjectPtr (+0x328 + id*8)
constexpr std::uintptr_t kRvaComboVtable = 0x00DB9090ull;       // UiMenuStateLiveSelectionCombo vftable
constexpr std::uintptr_t kRvaComboOnEnter = 0x0090BBF0ull, kRvaComboPressed = 0x0091D1D0ull, kRvaComboOnExit = 0x009143F0ull;
constexpr std::uintptr_t kRvaSetAllInputBlockTimer = 0x008C15E0ull, kRvaSetStatePrevious = 0x008C2E10ull;  // (the combo's Back)
// UiMenuStateManager::IsInputBlocked (retail 0x8B9CF0, TR2 0x8FBE20: the block timers +0xA90 / +0xA88 against
// TIMER_GetTimeMS). The combo's left / right (ProcessLeftRightSelectionInput, TR2 0x9857E0) blocks all input for 0.3 s
// (SetAllInputBlockTimer) before CycleOption: the row's text slides for 0.25 s (MenuSpinner.ChangeSpinnerValue tweens
// textMain_mc / textHidden_mc), and a change asked for while neither stands in place is dropped (the row showed another
// cutscene than the info panel). UiMenuStateManager::ProcessInput (TR2 0x8FF9F1) holds the keys while blocked; its
// OnMouseDown checks IsInputBlocked itself before a spinner's arrow.
constexpr std::uintptr_t kRvaIsInputBlocked = 0x008B9CF0ull;
constexpr float kCycleBlockSeconds = 0.3f;
constexpr std::uintptr_t kRvaGenericOnEnter = 0x008BB0D0ull;    // UiMenuStateGeneric::OnEnter
// UiMenuStateBase::IsSelectionAccessible (its nav items' accessible flags, +0x90), SetSelectionIndex,
// PushHighlightIndexToMovie; a ret 0 (the vtables' empty entries)
constexpr std::uintptr_t kRvaGenericAccessible = 0x008B9ED0ull, kRvaGenericSetSelection = 0x008C27F0ull,
                         kRvaGenericPushHighlight = 0x008BE430ull, kRvaNop = 0x009F98A0ull;
constexpr std::uintptr_t kRvaRegisterNavItems = 0x008BEC80ull, kRvaInitContextbar = 0x008B8BD0ull,
                         kRvaGetContextbar = 0x008B38C0ull, kRvaContextbarClear = 0x009799D0ull,
                         kRvaContextbarAdd = 0x00973000ull, kRvaIsCurrentState = 0x008B9A80ull;
constexpr std::uintptr_t kRvaNetManagerGet = 0x005817A0ull;     // NetManagerPC::Get (vfunc +0x70 (2): GameStateMenuing)
constexpr std::uintptr_t kRvaShowLiveHeader = 0x00590C60ull, kRvaShowLiveInfo = 0x00590D70ull,
                         kRvaSetCreditsVisible = 0x0058F7A0ull;
constexpr std::uintptr_t kRvaWidgetManagerGet = 0x008B35E0ull, kRvaGetWidget = 0x0007A560ull,
                         kRvaInvokeAS = 0x00995D80ull;          // UiWidgetScaleform::InvokeASFunction
constexpr std::uintptr_t kRvaObjGetMember = 0x00AF4A70ull, kRvaObjInvoke = 0x00AF4F00ull, kRvaObjRelease = 0x00AF5700ull,
                         kRvaObjSetMember = 0x00AF6A10ull;   // (GFxValue::ObjectInterface::SetMember, as CustomOutfits')
// The Extras menu's: UiMenuStateCroftManor's vtable and own entries (checked in it), UiMenuStateGeneric::OnExit /
// OnInputActionPressed, UiMenuStateBase::SetVariable / PushEnableIndexToMovie / PushDisableIndexToMovie /
// NavigationDisableInputDirection (TR2 0x8FD9B0 / 0x8FDE40 / 0x905D50 / 0x9004B0 / 0x900400 / 0x8FCB80, matched by code)
constexpr std::uintptr_t kRvaCroftVtable = 0x00DBCD80ull;
constexpr std::uintptr_t kRvaCroftUiSelect = 0x00930F90ull, kRvaCroftOnEnter = 0x00909010ull, kRvaCroftPressed = 0x0091BCB0ull,
                         kRvaCroftProcess = 0x0093E1F0ull, kRvaCroftHighlight = 0x00943750ull;
constexpr std::uintptr_t kRvaGenericOnExit = 0x008BB880ull, kRvaGenericPressed = 0x008BBD10ull,
                         kRvaSetVariable = 0x008C37E0ull, kRvaPushEnable = 0x008BE370ull, kRvaPushDisable = 0x008BE2C0ull,
                         kRvaNavDisableDirection = 0x008BAA50ull;
constexpr int kStateCombo = 0x82, kStateExtras = 0x93, kStateCredits = 0x0B;
constexpr int kStateList = 0x5C;   // (v0.9.497) UiMenuStateCollectibleListRelics: the Cutscene Viewer's list
std::atomic<bool> g_listMode{false};   // the Relics list is the Cutscene Viewer screen (its vtable is our copy)
constexpr std::size_t kVtableEntries = 30;
constexpr std::size_t kStateClip = 0x60;          // the state's movie clip (GFxValue: iface, type, data)
constexpr std::size_t kStateClipPath = 0x88;      // const char*: the state's clip path (SetVariable's prefix)
constexpr std::size_t kStateInputFlags = 0xD4;    // bit 3: input on
constexpr std::size_t kCroftSlotCount = 0x160;    // UiMenuStateCroftManor: the movie's option slots
constexpr std::size_t kGsmInfoWidget = 0x84, kGsmHeaderWidget = 0x88;
// The combo's own members (retail = TR2 + 8 from here): its widget callback object (CallbackBase1, TR2 +0x170), the EXP
// widget's ID (TR2 +0x4B4).
constexpr std::size_t kComboWidgetCallback = 0x178, kComboExpWidget = 0x4BC;
// input actions (dtp::INPUTACTION)
constexpr int kActCancel = 0x53, kActUp = 0x13B, kActUpHeld = 0x13C, kActDown = 0x141, kActDownHeld = 0x142,
              kActLeft = 0x147, kActLeftHeld = 0x148, kActRight = 0x14D, kActRightHeld = 0x14E, kActSelect = 0x153,
              kActLB = 0x15A, kActRB = 0x15B;
constexpr int kMouseContinue = 0x7A4, kMouseSpinner = 0x5E2;   // Mouse_Button IDs of the movie
// The rows (0.9.390: no GROUP row; Remnant Resistance's layout, rows 0 and 1, rows 2 and 3 hidden by the movie, PLAY at
// nav index 4)
constexpr int kRowCutscene = 0, kRowOutfit = 1, kRowPlay = 4;
constexpr int kBackSound = 5;
// The title label's depth in State_CroftManor (tr2_croftmanor: PlaceObject depth 502, AS depths are -16384 based)
constexpr double kExtrasTitleDepth = 502.0 - 16384.0;

struct Sig {
    std::uintptr_t rva;
    std::uint8_t bytes[8];
};
const Sig kNativeSigs[] = {
    {kRvaSetState, {0x48, 0x83, 0xEC, 0x48, 0x4C, 0x8D, 0x4C, 0x24}},
    {kRvaGetStateObjectPtr, {0x48, 0x63, 0x02, 0x3D, 0xCF, 0x00, 0x00, 0x00}},
    {kRvaComboPressed, {0x40, 0x53, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC}},
    {kRvaComboOnExit, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74}},
    {kRvaGenericOnEnter, {0x40, 0x55, 0x56, 0x41, 0x54, 0x41, 0x56, 0x41}},
    {kRvaRegisterNavItems, {0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x10, 0x49}},
    {kRvaInitContextbar, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83}},
    {kRvaGetContextbar, {0x8B, 0x91, 0x1C, 0x01, 0x00, 0x00, 0x83, 0xFA}},
    {kRvaContextbarClear, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B}},
    {kRvaContextbarAdd, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C}},
    {kRvaNetManagerGet, {0x48, 0x8B, 0x05, 0xA9, 0xA3, 0x80, 0x02, 0xC3}},
    {kRvaShowLiveHeader, {0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x48, 0x83}},
    {kRvaShowLiveInfo, {0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x48, 0x83}},
    {kRvaSetCreditsVisible, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C}},
    {kRvaWidgetManagerGet, {0x48, 0x8B, 0x05, 0xE9, 0x2F, 0x84, 0x00, 0xC3}},
    {kRvaGetWidget, {0x48, 0x83, 0x79, 0x18, 0x00, 0x4C, 0x63, 0xC2}},
    {kRvaInvokeAS, {0x48, 0x85, 0xD2, 0x0F, 0x84, 0xC0, 0x00, 0x00}},
    {kRvaObjGetMember, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x57, 0x41}},
    {kRvaObjInvoke, {0x40, 0x53, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57}},
    {kRvaObjRelease, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x42}},
    {kRvaIsCurrentState, {0x48, 0x8B, 0x05, 0xD1, 0x4C, 0x56, 0x02, 0x8B}},
    {kRvaGenericSetSelection, {0x85, 0xD2, 0x78, 0x0B, 0x3B, 0x51, 0x38, 0x7F}},
    {kRvaGenericPushHighlight, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x41}},
    {kRvaGenericAccessible, {0x4C, 0x8B, 0x81, 0x90, 0x00, 0x00, 0x00, 0x4D}},
    {kRvaNop, {0xC2, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC}},
    {kRvaSetAllInputBlockTimer, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x0F, 0x29}},
    {kRvaIsInputBlocked, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B}},
    {kRvaSetStatePrevious, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C}},
};
const Sig kExtrasSigs[] = {
    {kRvaGenericOnExit, {0x48, 0x8B, 0xC4, 0x55, 0x56, 0x41, 0x56, 0x41}},
    {kRvaGenericPressed, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83}},
    {kRvaSetVariable, {0x48, 0x8B, 0x49, 0x10, 0x48, 0x85, 0xC9, 0x75}},
    {kRvaPushEnable, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8B}},
    {kRvaPushDisable, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83}},
    {kRvaNavDisableDirection, {0x89, 0x54, 0x24, 0x10, 0x48, 0x83, 0xEC, 0x48}},
    {kRvaCroftOnEnter, {0x40, 0x55, 0x53, 0x41, 0x55, 0x48, 0x8B, 0xEC}},
    {kRvaCroftUiSelect, {0x8B, 0x41, 0x2C, 0x48, 0x8D, 0x14, 0x80, 0x48}},
};
bool g_nativeReady = false;   // the Cutscene Viewer screen
bool g_extrasReady = false;   // the Extras menu (without it EXTRAS opens the Cutscene Viewer screen)

std::uintptr_t Fn(std::uintptr_t rva) { return GameBuild::Base() + rva; }

std::uintptr_t* VtableOf(void* object) { return *reinterpret_cast<std::uintptr_t**>(object); }
void* StateManager() { return *reinterpret_cast<void**>(Fn(kRvaStateManager)); }
void* StateObject(int id) {
    void* mgr = StateManager();
    return mgr ? reinterpret_cast<void*(__fastcall*)(void*, const int*)>(Fn(kRvaGetStateObjectPtr))(mgr, &id) : nullptr;
}
void* ComboState() { return StateObject(kStateCombo); }
bool SetMenuState(int id, float time) {
    void* mgr = StateManager();
    return mgr && reinterpret_cast<bool(__fastcall*)(void*, const int*, float)>(Fn(kRvaSetState))(mgr, &id, time);
}
bool IsCurrentState(void* state) {
    return state && reinterpret_cast<bool(__fastcall*)(void*)>(Fn(kRvaIsCurrentState))(state);
}

// The story's end credits (UIMenuStateCredits, state 0x0B: after A New Beginning; Execution, the scene after them, stood
// at frame 23 under them in 0.9.360: their OnEnter asks PauseQueue for pause group 4, "UiMenuStateLC2Credits"), closed
// while the viewer plays as their own end closes them (ScaleformMovieInstance::DefaultCallback, retail 0x9E0D2B: the
// current state 0x0B -> GetStateObjectPtr(0x0B) -> UIMenuStateCredits::ExitState(state, false); retail 0x8E9570, TR2
// 0x92C300: in game it pops the state, SetStatePrevious, and OnExit unpauses group 4).
constexpr std::uintptr_t kRvaCreditsExitState = 0x008E9570ull;
constexpr std::uint8_t kCreditsExitStateSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x0D, 0xD3, 0x25, 0x4A, 0x02,
                                                 0x0F, 0xB6, 0xDA, 0x48, 0x8B, 0x01, 0xFF, 0x90, 0x80, 0x00, 0x00, 0x00};
bool g_creditsExitReady = false;
double g_creditsCloseAt = 0.0;
int g_creditsClosed = 0;

void CloseStoryCredits(double now) {
    if (!g_creditsExitReady || now < g_creditsCloseAt) return;
    void* credits = StateObject(kStateCredits);
    if (!IsCurrentState(credits)) return;
    g_creditsCloseAt = now + 0.5;   // (again while they are still the current state)
    reinterpret_cast<void(__fastcall*)(void*, bool)>(Fn(kRvaCreditsExitState))(credits, false);
    if (g_creditsClosed++ < 3) Log::Write("CutsceneViewer: the story's end credits closed (the cutscene comes after them)");
}
void* GameStateMenuing() {
    void* net = reinterpret_cast<void*(__fastcall*)()>(Fn(kRvaNetManagerGet))();
    if (!net) return nullptr;
    auto vfunc = reinterpret_cast<void*(__fastcall*)(void*, int)>((*reinterpret_cast<std::uintptr_t**>(net))[0x70 / 8]);
    return vfunc(net, 2);
}
void* Widget(std::uint32_t id) {
    if (id == 0xFFFFFFFFu) return nullptr;
    void* manager = reinterpret_cast<void*(__fastcall*)()>(Fn(kRvaWidgetManagerGet))();
    return manager ? reinterpret_cast<void*(__fastcall*)(void*, std::uint32_t)>(Fn(kRvaGetWidget))(manager, id) : nullptr;
}

GfxValue Num(double v) { GfxValue g; g.type = 3; g.number = v; return g; }
GfxValue Str(const char* s) { GfxValue g; g.type = 4; g.string = s; return g; }
GfxValue Bool(bool b) { GfxValue g; g.type = 2; g.data = nullptr; *reinterpret_cast<bool*>(&g.data) = b; return g; }

void Movie(void* state, const char* fn, std::initializer_list<GfxValue> args = {}) {
    g_invokeStateMovie(state, fn, nullptr, args.size() ? args.begin() : nullptr, unsigned(args.size()));
}
bool g_ourWidgetCall = false, g_allowWidgetCalls = false;
using InvokeASFn = void(__fastcall*)(void*, const char*, GfxValue*, const GfxValue*, unsigned);
InvokeASFn g_origInvokeAS = nullptr;
void WidgetCall(void* widget, const char* fn, std::initializer_list<GfxValue> args = {}) {
    if (!widget) return;
    g_ourWidgetCall = true;
    reinterpret_cast<InvokeASFn>(Fn(kRvaInvokeAS))(widget, fn, nullptr, args.size() ? args.begin() : nullptr,
                                                    unsigned(args.size()));
    g_ourWidgetCall = false;
}

using GetMemberFn = bool(__fastcall*)(void*, void*, const char*, GfxValue*, bool);
using ObjInvokeFn = bool(__fastcall*)(void*, void*, GfxValue*, const char*, const GfxValue*, std::size_t, bool);
using ReleaseFn = void(__fastcall*)(void*, GfxValue*, void*);
void ReleaseValue(GfxValue& value) {
    if ((value.type & 0x40) && value.iface) reinterpret_cast<ReleaseFn>(Fn(kRvaObjRelease))(value.iface, &value, value.data);
    value.iface = nullptr;
    value.type = 0;
    value.data = nullptr;
}
// An AS object's method: object.fn(args)
void ObjectCall(const GfxValue& object, const char* fn, std::initializer_list<GfxValue> args) {
    if (!object.iface || (object.type & 0x8F) < 6) return;
    reinterpret_cast<ObjInvokeFn>(Fn(kRvaObjInvoke))(object.iface, object.data, nullptr, fn,
                                                     args.size() ? args.begin() : nullptr, args.size(),
                                                     (object.type & 0x8F) == 8);
}

// A member clip's function: clip.member.fn(args) (the state's clip as GFxValue, or its path when it has none).
void MemberCall(void* state, const char* member, const char* fn, std::initializer_list<GfxValue> args) {
    auto* clip = reinterpret_cast<GfxValue*>(static_cast<std::uint8_t*>(state) + kStateClip);
    if ((clip->type & 0x8F) == 1 || !clip->iface) {   // undefined: InvokeStateMovie goes by the clip's path
        char path[128];
        std::snprintf(path, sizeof(path), "%s.%s", member, fn);
        Movie(state, path, args);
        return;
    }
    const bool isObject = (clip->type & 0x8F) == 8;
    GfxValue out;
    if (!reinterpret_cast<GetMemberFn>(Fn(kRvaObjGetMember))(clip->iface, clip->data, member, &out, isObject)) return;
    ObjectCall(out, fn, args);
    ReleaseValue(out);
}

// ---- Screen data (game thread) ----
// The categories: cutscenes.json's (English, as written there): the list's order (the story's, then the DLCs'; each in
// the game's order).
const char* const kCategories[] = {"Story", "Baba Yaga", "Blood Ties", "Cold Darkness"};
constexpr int kCategoryCount = 4;
std::vector<Cutscenes::ViewerItem> g_items;
int g_pick = 0, g_row = kRowPlay;
struct OutfitChoice {
    int index;
    std::string name;
};
std::vector<OutfitChoice> g_outfits;
int g_outfitPick = 0;
std::atomic<bool> g_viewerMode{false};   // the combo is the Cutscene Viewer screen (its vtable is our copy)
void* g_combo = nullptr;
std::uintptr_t g_cloneVtable[kVtableEntries + 2] = {};
std::uintptr_t* g_comboVtable = nullptr;
bool g_infoReady = false, g_headerReady = false;
std::atomic<bool> g_enterRequest{false};
double g_viewerEnterTime = 0.0;

// The Extras menu's entries (their text in the game's language: viewer_text.cpp)
struct ExtrasEntry {
    ViewerText::Id label;
    ViewerText::Id description;
    bool enabled;
};
const ExtrasEntry kExtras[] = {
    {ViewerText::CutsceneViewer, ViewerText::AboutCutsceneViewer, true},
    {ViewerText::ModelViewer, ViewerText::AboutModelViewer, true},   // (when the Model Viewer is available)
    {ViewerText::Credits, ViewerText::AboutCredits, true},
};
constexpr int kExtrasCount = int(sizeof(kExtras) / sizeof(kExtras[0]));
constexpr int kExtrasViewer = 0, kExtrasModels = 1, kExtrasCredits = 2;
bool g_modelScreensReady = false;   // the Model Viewer's screens (the game's Collectibles list and relic viewer)
bool EntryEnabled(int i) { return i == kExtrasModels ? g_modelScreensReady && ModelViewer::Available() : kExtras[i].enabled; }
std::atomic<bool> g_extrasMode{false};   // the Croft Manor menu is the Extras menu (its vtable is our copy)
void* g_extrasState = nullptr;
std::uintptr_t* g_croftVtable = nullptr;
std::uintptr_t g_extrasCloneStore[kVtableEntries + 2] = {};   // [0]: the RTTI locator; the vtable from [1]
std::uintptr_t* ExtrasClone() { return &g_extrasCloneStore[1]; }
double g_extrasEnterTime = 0.0;
int g_extrasTitleTries = 0;
bool g_extrasTitleLogged = false;
std::atomic<bool> g_reopenViewer{false};   // back from a cutscene: the Extras menu opens the viewer again
bool g_waitViewer = false;                  // (the fade-in waits for it)

int CategoryRank(const std::string& c) {
    for (int i = 0; i < kCategoryCount; ++i)
        if (_stricmp(c.c_str(), kCategories[i]) == 0) return i;
    return kCategoryCount;
}

void LoadItems() {
    Cutscenes::ReloadUserText();
    g_items = Cutscenes::PlayableItems();
    std::stable_sort(g_items.begin(), g_items.end(), [](const Cutscenes::ViewerItem& a, const Cutscenes::ViewerItem& b) {
        return CategoryRank(a.category) < CategoryRank(b.category);
    });
    if (g_pick >= int(g_items.size())) g_pick = 0;
    const std::string keep = g_outfitPick > 0 && g_outfitPick < int(g_outfits.size()) ? g_outfits[g_outfitPick].name : "";
    g_outfits.clear();
    g_outfits.push_back({-1, ViewerText::Get(ViewerText::StoryOutfit)});
    for (int i = 0, n = Features::OutfitCount(); i < n; ++i) {
        char name[160];
        if (Features::OutfitDisplayName(i, name, sizeof(name)) && name[0]) g_outfits.push_back({i, name});
    }
    g_outfitPick = 0;
    for (int i = 1; i < int(g_outfits.size()); ++i)
        if (g_outfits[i].name == keep) g_outfitPick = i;
}

const Cutscenes::ViewerItem* PickedItem() {
    return g_pick >= 0 && g_pick < int(g_items.size()) ? &g_items[g_pick] : nullptr;
}

// (v0.9.496: no OUTFIT row; the outfit is chosen in the grid on the right, OutfitGrid below)
bool RowEnabled(int row) { return row == kRowCutscene || row == kRowPlay; }

std::string g_textCutscene, g_textOutfit;   // (the movie copies the strings)

// (the dots beside each row: the movie's mode dots beside row 0, its level dots beside row 1)
void PushRow(void* state, int row, const char* dir) {
    switch (row) {
        case kRowCutscene: {
            const auto* item = PickedItem();
            g_textCutscene = item ? ViewerText::Localized(item->titles, item->title)
                                  : std::string(ViewerText::Get(ViewerText::NoneYet));
            Movie(state, "ChangeSpinnerValue", {Num(row), Str(g_textCutscene.c_str()), Str(dir)});
            Movie(state, "SetModeDotSelected", {Num(g_pick)});
            break;
        }
        case kRowOutfit:
            g_textOutfit = g_outfits.empty() ? std::string("-") : g_outfits[g_outfitPick].name;
            Movie(state, "ChangeSpinnerValue", {Num(row), Str(g_textOutfit.c_str()), Str(dir)});
            Movie(state, "SetLevelDotSelected", {Num(g_outfitPick)});
            break;
        default:
            break;
    }
}

void PushDots(void* state) {
    Movie(state, "ResetDots");
    Movie(state, "SetNumModeDots", {Num(std::min<int>(100, int(g_items.size())))});
    Movie(state, "SetNumLevelDots", {Num(0)});   // (v0.9.496: the OUTFIT row's dots, with the row gone)
}

// ---- The cutscenes' pictures in the info panel ------------------------------------------------------------------------
// The viewer screen's info panel (live_widgetinfo, class Widget_LiveInfo) has a picture: Info_mc's image_loader_mc (frame
// "Info": placed at (-564, -242.2), the panel's top-left). Widget_LiveInfo.SetImageHandle(handle), then LoadImage(handle,
// id, x, y) load "img://<id>" into it, unscaled (ImageLoader NO_SCALE) and moved by x, y, as UiWidgetLiveInfo::
// InternalImageLoadedCB (TR2 0x9DA270) does for Chapter Replay's level pictures (512x256, at 0, 0). The game's
// CustomImageLoader::LoadImage (retail 0x9EC5F0) makes the number a texture: ResolveSection::s_pSection[5] (retail
// 0x1AA05D8, the textures), vtable +0xA8 GetBasePointer(id) (RenderResourceSection::GetBasePointer, retail 0x343240), then
// a ScaleformTexture and a GImageInfo(texture, width, height) of the texture's own size (TextureMap vtable +0x38, +0x40;
// GImageInfo's ctor retail 0xB0F0D0 keeps them at +0x30, +0x34). That size is the image's in the movie: GImageInfo::
// GetWidth, GetHeight (its vtable retail 0xE08748, +8, +0x10) return them, GFxShapeWithStyles::SetToImage makes the
// loaded image a rectangle of GetRect (0, 0, GetWidth, GetHeight), and GFxFillStyle::GetImageFillTexture (the code at
// retail 0xC114E8) maps that rectangle to the whole texture by 1 / GetWidth, 1 / GetHeight ("img://" loads smoothed,
// ProcessLoadMovieClip). So our pictures' GImageInfo is set to the panel size (cutscenes.json "pictures" width, height:
// 512x256, the game's own), and its ScaleformTexture's (+0x18; +0x50, +0x54) too, as CustomOutfits does for its larger
// campsite pictures; their texture, 4x that, fills it. MinHook makes one hook a function: when custom outfits are
// configured CustomOutfits has LoadImage hooked and calls ours after its own (0.9.366 made its own and failed then).
// The textures: cutscenes\images\<image> decoded (stb_image, in a thread), cropped from the middle to the texture's
// shape and scaled (Lanczos), given cutscenes\frame.png's edges when the file has no alpha (picture_frame.h), its mips
// made and each one BC3 (stb_dxt). PCDX12RenderDevice::CreateTexture (RenderDevice::s_pcInstance retail 0x1A53E40,
// vtable +0x308, retail 0x307960; type 0, a cdc::RenderFormat, width, height, depth 0, 1 face, the mips, the face's
// data with its mips one after another, each GetMipLevelSize long, as D3DBitmapTexture::ASyncCreate reads them) makes
// it and waits (RenderAsyncCreateResource::CreateNow; PCDX12Texture::CreateD3DResource copies the data to an upload
// buffer, ExecuteUpload), so ours is freed after. It goes under an ID of ours (from 0x7EED0000) that our GetBasePointer
// hook answers. cdc::RenderFormat is DXGI's numbering up to 66 and three less from BC1 on (g_renderFormats' DXGI
// families; GetMipLevelSize and GetFormatPitch: 68, 69 8-byte blocks, 71 to 75 16-byte): 75 is BC3_UNORM_SRGB (sRGB, as
// 0.9.365's R8G8B8A8_UNORM_SRGB, 29, was).
constexpr std::uintptr_t kRvaRenderDevice = 0x01A53E40ull, kRvaCreateTexture = 0x00307960ull,
                         kRvaGetBasePointer = 0x00343240ull, kRvaTextureSection = 0x01AA05D8ull,
                         kRvaLoadImage = 0x009EC5F0ull, kRvaImageInfoVtable = 0x00E08748ull;
constexpr std::uint8_t kCreateTextureSig[] = {0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89,
                                              0x7C, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x50, 0x4C, 0x8B, 0xF1};
constexpr std::uint8_t kGetBasePointerSig[] = {0x48, 0x83, 0x79, 0x18, 0x00, 0x44, 0x8B, 0xC2, 0x74, 0x24,
                                               0x33, 0xD2, 0x41, 0x8B, 0xC0, 0x48, 0xF7, 0x71, 0x20};
constexpr std::uint8_t kLoadImageSig[] = {0x40, 0x53, 0x55, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0xDA,
                                          0x48, 0x8B, 0xE9, 0x48, 0x8B, 0xCB, 0x48, 0x8D, 0x15};
constexpr std::size_t kDeviceCreateTextureSlot = 0x308, kSectionGetBasePointerSlot = 0xA8;
constexpr std::size_t kImageInfoWidth = 0x30, kImageInfoTexture = 0x18, kScaleformTextureWidth = 0x50;
constexpr std::uint32_t kPictureIdBase = 0x7EED0000u;
constexpr int kMaxPictureTextures = 1024, kFormatBc3Srgb = 75;
using CreateTextureFn = void*(__fastcall*)(void* device, int type, int format, std::uint16_t width, std::uint16_t height,
                                           std::uint16_t depth, std::uint16_t faces, std::uint16_t mips,
                                           const std::uint8_t* const* data, std::uint16_t, int tileMode, const char* name,
                                           bool, bool);
using GetBasePointerFn = void*(__fastcall*)(void*, unsigned);
using LoadImageFn = void*(__fastcall*)(void* loader, const char* url);
GetBasePointerFn g_origGetBasePointer = nullptr;
LoadImageFn g_origLoadImage = nullptr;
std::atomic<void*> g_pictureTextures[kMaxPictureTextures];
std::atomic<std::uint32_t> g_pictureShown[kMaxPictureTextures];   // the size the movie shows it at (width << 16 | height)
int g_pictureTextureCount = 0;
int g_picturesState = 0;                        // 0 the hook not made, 1 ready, -1 not usable (logged)
bool g_loadImageHooked = false;                 // (without it the textures are made at the panel size)
std::atomic<bool> g_imageInfoMismatch{false};   // our picture's GImageInfo was not the one expected (logged once)

void* __fastcall HookGetBasePointer(void* section, unsigned id) {
    if (id - kPictureIdBase < unsigned(kMaxPictureTextures))
        if (void* texture = g_pictureTextures[id - kPictureIdBase].load()) return texture;
    return g_origGetBasePointer(section, id);
}

// "img://<id>" (or "IMG://", "img://#<id>", as the game's LoadImage reads it) one of our textures.
bool IsPictureUrl(const char* url) noexcept {
    if (!url || (std::strncmp(url, "img://", 6) != 0 && std::strncmp(url, "IMG://", 6) != 0)) return false;
    const unsigned long id = std::strtoul(url + 6 + (url[6] == '#'), nullptr, 10);
    return id - kPictureIdBase < unsigned(kMaxPictureTextures) && g_pictureTextures[id - kPictureIdBase].load();
}

// The GImageInfo the game made for our texture (and its ScaleformTexture, when it says the same size) given the panel
// size. SEH only.
bool SizeImageInfo(void* info, std::uint32_t width, std::uint32_t height) noexcept {
    __try {
        auto* image = static_cast<std::uint8_t*>(info);
        if (*reinterpret_cast<std::uintptr_t*>(image) != Fn(kRvaImageInfoVtable)) return false;
        auto* size = reinterpret_cast<std::uint32_t*>(image + kImageInfoWidth);
        const std::uint32_t was[2] = {size[0], size[1]};
        size[0] = width;
        size[1] = height;
        if (auto* texture = *reinterpret_cast<std::uint8_t**>(image + kImageInfoTexture)) {
            auto* textureSize = reinterpret_cast<std::uint32_t*>(texture + kScaleformTextureWidth);
            if (textureSize[0] == was[0] && textureSize[1] == was[1]) {
                textureSize[0] = width;
                textureSize[1] = height;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Each image the game's LoadImage made (the GFx loader's thread or the game's): ours given the size it is shown at (the
// info panel's for the cutscenes' pictures, the grid tile's for the Model Viewer's).
void OnImageLoaded(const char* url, void* info) {
    if (info && IsPictureUrl(url)) {
        const unsigned long id = std::strtoul(url + 6 + (url[6] == '#'), nullptr, 10);
        const std::uint32_t shown = g_pictureShown[id - kPictureIdBase].load();
        if (!SizeImageInfo(info, shown >> 16, shown & 0xFFFF)) g_imageInfoMismatch = true;
    }
}

void* __fastcall HookLoadImage(void* loader, const char* url) {
    void* info = g_origLoadImage(loader, url);
    OnImageLoaded(url, info);
    return info;
}

// The game's texture section answers through our hook, and the render device makes textures where we expect. SEH only.
bool PictureFunctionsMatch() noexcept {
    __try {
        void* section = *reinterpret_cast<void**>(Fn(kRvaTextureSection));
        void* device = *reinterpret_cast<void**>(Fn(kRvaRenderDevice));
        if (!section || !device) return false;
        // (the section's vtable entry is the function we hooked; the device's is CreateTexture)
        return VtableOf(section)[kSectionGetBasePointerSlot / 8] == Fn(kRvaGetBasePointer) &&
               VtableOf(device)[kDeviceCreateTextureSlot / 8] == Fn(kRvaCreateTexture);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A BC3 sRGB texture of the mips in data (one after another, largest first). SEH only.
void* CreatePictureTexture(const std::uint8_t* data, int width, int height, int mips) noexcept {
    __try {
        void* device = *reinterpret_cast<void**>(Fn(kRvaRenderDevice));
        if (!device) return nullptr;
        const std::uint8_t* faces[1] = {data};
        return reinterpret_cast<CreateTextureFn>(Fn(kRvaCreateTexture))(device, 0, kFormatBc3Srgb, std::uint16_t(width),
                                                                       std::uint16_t(height), 0, 1, std::uint16_t(mips),
                                                                       faces, 0, 0, nullptr, false, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

struct PictureMake {                 // what a picture's texture is made for
    int width = 0, height = 0;       // its size
    bool frame = false;              // a file without alpha given the frame's edges
    long long frameStamp = 0;        // cutscenes\frame.png's last write (0: none)
    bool contain = false;            // scaled into it whole (transparent around), else cropped to its shape
    int shownWidth = 0, shownHeight = 0;   // the size the movie shows it at
    bool operator==(const PictureMake&) const = default;
};
struct Picture {
    std::wstring file;                // the file it was made from
    long long stamp = 0;              // its last write (a replaced picture is made again)
    PictureMake make;
    int state = 0;                    // 1 decoding, 2 texture, -1 no file or not usable
    std::uint32_t textureId = 0;
};
std::map<std::string, Picture> g_pictures;   // by image name (game thread)
struct PictureJob {
    std::string key;
    std::wstring path, framePath;
    PictureMake make;
    long long stamp = 0;
    std::vector<std::uint8_t> data;   // (the result: the mips' BC3 blocks, one after another)
    int mips = 0;
    bool ok = false, framed = false;
};
std::mutex g_pictureJobMutex;
std::condition_variable g_pictureJobWake;
std::deque<PictureJob> g_pictureJobs, g_pictureResults;
bool g_pictureWorker = false;
int g_pictureImageHandle = 0;
bool g_frameMissingLogged = false;

// Mips down to the one whose shorter side is 1 (CreateTexture's mips are the size >> level: none may be 0).
int PictureMips(int width, int height) {
    int mips = 1;
    while ((width >> mips) >= 1 && (height >> mips) >= 1) ++mips;
    return mips;
}

void PictureWorker() {
    PictureFrame::Image frame;   // cutscenes\frame.png at the textures' size (this thread's)
    long long frameStamp = -1;
    int frameWidth = 0, frameHeight = 0;
    for (;;) {
        PictureJob job;
        {
            std::unique_lock lock(g_pictureJobMutex);
            g_pictureJobWake.wait(lock, [] { return !g_pictureJobs.empty(); });
            job = std::move(g_pictureJobs.front());
            g_pictureJobs.pop_front();
        }
        PictureFrame::Image picture;
        if (ImageLoad::DecodeFile(job.path, picture.rgba, picture.width, picture.height)) {
            const bool opaque = PictureFrame::Opaque(picture);
            picture = job.make.contain ? PictureFrame::Contain(picture, job.make.width, job.make.height)
                                       : PictureFrame::Fit(picture, job.make.width, job.make.height);
            if (opaque && job.make.frame && job.make.frameStamp) {
                if (frameStamp != job.make.frameStamp || frameWidth != job.make.width || frameHeight != job.make.height) {
                    frame = {};
                    PictureFrame::Image file;
                    if (ImageLoad::DecodeFile(job.framePath, file.rgba, file.width, file.height))
                        frame = PictureFrame::Resize(file, job.make.width, job.make.height);
                    frameStamp = job.make.frameStamp;
                    frameWidth = job.make.width;
                    frameHeight = job.make.height;
                }
                if (frame.width) {
                    PictureFrame::ApplyFrame(picture, frame);
                    job.framed = true;
                }
            }
            const std::vector<PictureFrame::Image> chain =
                PictureFrame::MipChain(picture, PictureMips(job.make.width, job.make.height));
            std::size_t size = 0;
            for (const auto& level : chain) size += ImageLoad::Bc3Size(level.width, level.height);
            job.data.resize(size);
            std::uint8_t* out = job.data.data();
            for (const auto& level : chain) {
                ImageLoad::CompressBc3(level.rgba.data(), level.width, level.height, out);
                out += ImageLoad::Bc3Size(level.width, level.height);
            }
            job.mips = int(chain.size());
            job.ok = !chain.empty();
        }
        std::lock_guard lock(g_pictureJobMutex);
        g_pictureResults.push_back(std::move(job));
    }
}

long long FileStamp(const std::wstring& path) {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    return ec ? 0 : static_cast<long long>(time.time_since_epoch().count());
}

std::wstring FramePath() {
    return (std::filesystem::path(Cutscenes::ImagesDirectory()).parent_path() / L"frame.png").wstring();
}

// The cutscene's picture file: its image or, of the same name, .png, .jpg or .jpeg, the newest (a framed PNG put
// beside the JPEG the json names is used once it is there). Empty: none.
std::wstring PictureFile(const std::string& image, long long& stamp) {
    stamp = 0;
    std::wstring best;
    const std::filesystem::path named = std::filesystem::path(Cutscenes::ImagesDirectory()) / std::filesystem::path(image);
    const auto consider = [&](const std::filesystem::path& path) {
        const long long time = FileStamp(path.wstring());
        if (time > stamp) {
            stamp = time;
            best = path.wstring();
        }
    };
    consider(named);
    for (const wchar_t* extension : {L".png", L".jpg", L".jpeg"}) {
        std::filesystem::path other = named;
        other.replace_extension(extension);
        if (other != named) consider(other);
    }
    return best;
}

PictureMake CurrentPictureMake() {
    const Cutscenes::PictureLayout layout = Cutscenes::CurrentPictureLayout();
    PictureMake make;
    // (shown at its texture's size without the LoadImage hook: made at the panel's then; BC3 sides are 4s)
    make.width = g_loadImageHooked ? layout.textureWidth : (layout.width + 3) / 4 * 4;
    make.height = g_loadImageHooked ? layout.textureHeight : (layout.height + 3) / 4 * 4;
    make.frame = layout.frame;
    make.frameStamp = layout.frame ? FileStamp(FramePath()) : 0;
    make.shownWidth = layout.width;
    make.shownHeight = layout.height;
    return make;
}

// The picture of the cutscene asked for (made in the background); its texture ID when it is there (0: not yet, or none).
// The picture of that key from that file (made in the background); its texture ID when it is there (0: not yet, or
// none).
std::uint32_t PictureFromFile(const std::string& key, const std::wstring& path, long long stamp, const PictureMake& make) {
    if (g_picturesState <= 0 || key.empty()) return 0;
    Picture& picture = g_pictures[key];
    const bool same = picture.file == path && picture.stamp == stamp && picture.make == make;
    if (same && picture.state != 0) return picture.state == 2 ? picture.textureId : 0;
    picture.file = path;
    picture.stamp = stamp;
    picture.make = make;
    if (!stamp) {   // (no file)
        picture.state = -1;
        return 0;
    }
    if (make.frame && !make.frameStamp && !g_frameMissingLogged) {
        g_frameMissingLogged = true;
        Log::Write("CutsceneViewer: cutscenes\\frame.png not found; screenshots are shown without the game's picture edges");
    }
    picture.state = 1;
    {
        std::lock_guard lock(g_pictureJobMutex);
        PictureJob job;
        job.key = key;
        job.path = path;
        job.framePath = FramePath();
        job.make = make;
        job.stamp = stamp;
        g_pictureJobs.push_back(std::move(job));
        if (!g_pictureWorker) {
            g_pictureWorker = true;
            std::thread(PictureWorker).detach();
        }
    }
    g_pictureJobWake.notify_one();
    return 0;
}

// The picture of the cutscene asked for (made in the background); its texture ID when it is there (0: not yet, or none).
std::uint32_t PictureFor(const Cutscenes::ViewerItem& item) {
    if (g_picturesState <= 0 || item.image.empty()) return 0;
    long long stamp = 0;
    const std::wstring path = PictureFile(item.image, stamp);
    return PictureFromFile(item.image, path, stamp, CurrentPictureMake());
}

// The pictures made in the background made textures (game thread). True when one of them is the picked cutscene's.
bool PumpPictures(const std::string& pickedImage) {
    if (g_imageInfoMismatch.exchange(false)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log::Write("CutsceneViewer: a picture's GImageInfo is not the game's GImageInfo; it shows at its texture's size");
        }
    }
    std::deque<PictureJob> done;
    {
        std::lock_guard lock(g_pictureJobMutex);
        done.swap(g_pictureResults);
    }
    bool picked = false;
    for (PictureJob& job : done) {
        Picture& picture = g_pictures[job.key];
        if (picture.file != job.path || picture.stamp != job.stamp || !(picture.make == job.make)) continue;   // (stale)
        if (!job.ok) {
            picture.state = -1;
            Log::Writef("CutsceneViewer: picture %s could not be read (JPEG or PNG)", job.key.c_str());
            continue;
        }
        if (g_picturesState == 1 && !PictureFunctionsMatch()) {
            g_picturesState = -1;
            Log::Write("CutsceneViewer: the game's texture functions are not where expected; no pictures");
        }
        if (g_picturesState <= 0 || g_pictureTextureCount >= kMaxPictureTextures) {
            picture.state = -1;
            continue;
        }
        // (made, its data copied: job.data goes with the job)
        void* texture = CreatePictureTexture(job.data.data(), job.make.width, job.make.height, job.mips);
        if (!texture) {
            picture.state = -1;
            Log::Writef("CutsceneViewer: picture %s: the game did not make its texture", job.key.c_str());
            continue;
        }
        const int slot = g_pictureTextureCount++;
        g_pictureShown[slot] = std::uint32_t(job.make.shownWidth) << 16 | std::uint32_t(job.make.shownHeight & 0xFFFF);
        g_pictureTextures[slot] = texture;
        picture.textureId = kPictureIdBase + std::uint32_t(slot);
        picture.state = 2;
        std::string file(job.key);   // (the file used: the image named, or its .png, .jpg or .jpeg)
        if (const std::size_t dot = job.path.find_last_of(L'.'); dot != std::wstring::npos) {
            std::string extension;
            for (std::size_t i = dot; i < job.path.size(); ++i) extension += char(job.path[i] < 128 ? job.path[i] : '?');
            file = file.substr(0, file.find_last_of('.')) + extension;
        }
        if (job.key.rfind("icon:", 0) == 0)   // (the Model Viewer's grid pictures)
            Log::Detailf("ModelViewer: the picture of %s made a %dx%d texture (BC3, %d mips; ID %u)", job.key.c_str() + 5,
                         job.make.width, job.make.height, job.mips, picture.textureId);
        else
            Log::Writef("CutsceneViewer: picture %s made a %dx%d texture (BC3, %d mips%s; ID %u)", file.c_str(),
                        job.make.width, job.make.height, job.mips, job.framed ? ", framed" : "", picture.textureId);
        picked = picked || job.key == pickedImage;
    }
    return picked;
}

// The picture into the info panel's slot (after its SetFrame: the slot is empty until then).
void PushPicture(void* info, std::uint32_t textureId) {
    if (!info || !textureId) return;
    const Cutscenes::PictureLayout layout = Cutscenes::CurrentPictureLayout();
    const int handle = ++g_pictureImageHandle;
    WidgetCall(info, "SetImageHandle", {Num(handle)});
    WidgetCall(info, "LoadImage", {Num(handle), Num(double(textureId)), Num(layout.x), Num(layout.y)});
}

void UpdateInfo() {
    void* gsm = GameStateMenuing();
    if (!gsm || !g_infoReady) return;
    void* info = Widget(At<std::uint32_t>(gsm, kGsmInfoWidget));
    if (!info) return;
    static std::string title, desc;
    const auto* item = PickedItem();
    title = item ? ViewerText::Localized(item->titles, item->title)
                 : std::string(ViewerText::Get(ViewerText::NoCutscenes));
    desc = item ? ViewerText::Localized(item->descriptions, item->description) : std::string();
    if (g_listMode.load() && g_outfitPick >= 0 && g_outfitPick < int(g_outfits.size())) {   // (v0.9.500: the outfit chosen)
        const std::string outfit = std::string(ViewerText::Upper(ViewerText::Outfit)) + ": " +
                                   ViewerText::ToUpper(g_outfits[std::size_t(g_outfitPick)].name);
        desc = outfit;   // (v0.9.501: the outfit alone on the picture's lower edge: a description ran over the grid under it)
    }
    // Reset hides what the combo put there (its high score); single buffered, both buffers the same clip
    WidgetCall(info, "Reset");
    WidgetCall(info, "SetDoubleBuffered", {Bool(true)});
    WidgetCall(info, "SetDoubleBuffered", {Bool(false)});
    WidgetCall(info, "SetFrame", {Str("Info")});
    WidgetCall(info, "HideAllChallenges");
    WidgetCall(info, "HideParTime");
    WidgetCall(info, "HideDifficulty");
    WidgetCall(info, "HideCardBonus");
    WidgetCall(info, "HideMedal");
    WidgetCall(info, "HideMedalScores");
    // The title over the picture: mode_mc, the small top line (size 20, 475 wide, Widget_LiveInfo.SetMode: in
    // capitals); level_mc (the big line under it, size 30) hidden. (0.9.400: the category was on top, the title under.)
    WidgetCall(info, "SetMode", {Str(title.c_str())});
    WidgetCall(info, "HideLevel");
    if (desc.empty()) WidgetCall(info, "HideDifficultyDescription");
    else WidgetCall(info, "SetDifficultyDescription", {Str(desc.c_str())});
    if (item) {
        PushPicture(info, PictureFor(*item));
        // (the ones beside it decoded too: the next turn shows its picture at once)
        const int n = int(g_items.size()), p = g_pick;
        if (n > 1) {
            PictureFor(g_items[(p + 1) % n]);
            PictureFor(g_items[(p + n - 1) % n]);
        }
    }
    WidgetCall(info, "PlayAnimation");
}

void UpdateHeader() {
    void* gsm = GameStateMenuing();
    if (!gsm || !g_headerReady) return;
    void* header = Widget(At<std::uint32_t>(gsm, kGsmHeaderWidget));
    if (!header) return;
    WidgetCall(header, "SetTitle", {Str(ViewerText::Get(ViewerText::Extras))});
    WidgetCall(header, "ClearAllRibbonItems");
    WidgetCall(header, "AddRibbonItem", {Str(ViewerText::Get(kExtras[kExtrasViewer].label))});
    WidgetCall(header, "SetActiveRibbon", {Num(0)});
}

void Highlight(void* state, int row) {
    g_row = row;
    At<std::int32_t>(state, 0x2C) = row;
    At<std::int32_t>(state, 0x30) = row;
    // (State_MenuComboSelection.OnHighlight returns at once for the index it has: -1 first clears it)
    Movie(state, "OnHighlight", {Num(-1)});
    Movie(state, "OnHighlight", {Num(row)});
}

// The state clip's number property (its _alpha), -1 when it cannot be read.
double ClipNumber(void* state, const char* name) {
    auto* clip = reinterpret_cast<GfxValue*>(static_cast<std::uint8_t*>(state) + kStateClip);
    if (!clip->iface || (clip->type & 0x8F) < 6) return -1.0;
    GfxValue out;
    if (!reinterpret_cast<GetMemberFn>(Fn(kRvaObjGetMember))(clip->iface, clip->data, name, &out, (clip->type & 0x8F) == 8))
        return -1.0;
    return (out.type & 0x8F) == 3 ? out.number : -1.0;
}
// The state clip's number property set; false when it cannot be.
using SetMemberFn = bool(__fastcall*)(void*, void*, const char*, const GfxValue*, bool);
bool SetClipNumber(void* state, const char* name, double value) {
    auto* clip = reinterpret_cast<GfxValue*>(static_cast<std::uint8_t*>(state) + kStateClip);
    if (!clip->iface || (clip->type & 0x8F) < 6) return false;
    const GfxValue v = Num(value);
    return reinterpret_cast<SetMemberFn>(Fn(kRvaObjSetMember))(clip->iface, clip->data, name, &v, (clip->type & 0x8F) == 8);
}
// A member clip's number property (clip.member.name), NAN when it cannot be read; and set.
double MemberNumber(void* state, const char* member, const char* name) {
    auto* clip = reinterpret_cast<GfxValue*>(static_cast<std::uint8_t*>(state) + kStateClip);
    if (!clip->iface || (clip->type & 0x8F) < 6) return NAN;
    GfxValue child;
    if (!reinterpret_cast<GetMemberFn>(Fn(kRvaObjGetMember))(clip->iface, clip->data, member, &child, (clip->type & 0x8F) == 8))
        return NAN;
    double result = NAN;
    if (child.iface && (child.type & 0x8F) >= 6) {
        GfxValue out;
        if (reinterpret_cast<GetMemberFn>(Fn(kRvaObjGetMember))(child.iface, child.data, name, &out, (child.type & 0x8F) == 8) &&
            (out.type & 0x8F) == 3)
            result = out.number;
    }
    ReleaseValue(child);
    return result;
}
bool SetMemberNumber(void* state, const char* member, const char* name, double value) {
    auto* clip = reinterpret_cast<GfxValue*>(static_cast<std::uint8_t*>(state) + kStateClip);
    if (!clip->iface || (clip->type & 0x8F) < 6) return false;
    GfxValue child;
    if (!reinterpret_cast<GetMemberFn>(Fn(kRvaObjGetMember))(clip->iface, clip->data, member, &child, (clip->type & 0x8F) == 8))
        return false;
    bool ok = false;
    if (child.iface && (child.type & 0x8F) >= 6) {
        const GfxValue v = Num(value);
        ok = reinterpret_cast<SetMemberFn>(Fn(kRvaObjSetMember))(child.iface, child.data, name, &v, (child.type & 0x8F) == 8);
    }
    ReleaseValue(child);
    return ok;
}
double g_enterTime = 0.0;
bool g_alphaChecked = false;
int g_creditsHides = 0;

void PlaySoundId(void* state, int id) {
    reinterpret_cast<void(__fastcall*)(void*, int)>((*reinterpret_cast<std::uintptr_t**>(state))[0xC0 / 8])(state, id);
}

void RefreshAll(void* state) {
    PushDots(state);
    PushRow(state, kRowCutscene, "");
    Movie(state, "SetContinueText", {Str(ViewerText::Upper(ViewerText::Play))});
    UpdateInfo();
    UpdateHeader();
}

// The combo's widget callback object (CallbackBase1<void, unsigned>: +0 operator(), +8 Exec(widget ID), +0x10 deleting
// destructor) gets our vtable while the viewer screen is up: the combo's own callback runs first (it attaches the info
// widget to the screen's 3D movie data and fills the header as Expeditions), then ours puts our content in. Its content
// calls to the info widget are dropped (HookInvokeAS) even there: it filled one buffer (Expeditions' Score Attack card,
// Info_mc's tutorial frame) and PlayAnimation started a 0.3 s Tweener fade of that buffer to _alpha 100; our Reset set both
// buffers' _alpha to 0 and drew into the other one, and the fade still running brought the Score Attack card back over
// ours until the next UpdateInfo (the user, 0.9.391: on entering the viewer, gone on changing the cutscene).
void** g_origCallbackVtable = nullptr;
bool IsInfoWidget(std::uint32_t id) {
    void* gsm = GameStateMenuing();
    return gsm && id == At<std::uint32_t>(gsm, kGsmInfoWidget);
}
void OurWidgetContent(std::uint32_t id) {
    void* gsm = GameStateMenuing();
    if (!gsm || !g_viewerMode.load()) return;
    if (id == At<std::uint32_t>(gsm, kGsmInfoWidget)) {
        g_infoReady = true;
        UpdateInfo();
    } else if (id == At<std::uint32_t>(gsm, kGsmHeaderWidget)) {
        g_headerReady = true;
        UpdateHeader();
    }
}
void __fastcall WidgetCallbackOp(void* self, std::uint32_t id) {
    if (g_origCallbackVtable) {
        g_allowWidgetCalls = !IsInfoWidget(id);
        reinterpret_cast<void(__fastcall*)(void*, std::uint32_t)>(g_origCallbackVtable[0])(self, id);
        g_allowWidgetCalls = false;
    }
    OurWidgetContent(id);
}
void __fastcall WidgetCallbackExec(void* self, std::uint32_t id) {
    if (g_origCallbackVtable) {
        g_allowWidgetCalls = !IsInfoWidget(id);
        reinterpret_cast<void(__fastcall*)(void*, std::uint32_t)>(g_origCallbackVtable[1])(self, id);
        g_allowWidgetCalls = false;
    }
    OurWidgetContent(id);
}
void* __fastcall WidgetCallbackDtor(void* self, unsigned) { return self; }
void* g_widgetCallbackVtable[3] = {reinterpret_cast<void*>(&WidgetCallbackOp), reinterpret_cast<void*>(&WidgetCallbackExec),
                                   reinterpret_cast<void*>(&WidgetCallbackDtor)};

// UiWidgetScaleform::InvokeASFunction while the viewer screen is up: the combo's later content calls to the header and the
// info widget (its leaderboard replies, its info animations) are dropped; ours and the widgets' own go through.
bool ContentCall(const char* fn) {
    static const char* const kContent[] = {
        "SetFrame", "SetMode", "HideMode", "SetLevel", "HideLevel", "SetParTime", "HideParTime", "SetDifficulty",
        "HideDifficulty", "SetDifficultyDescription", "HideDifficultyDescription", "SetCardBonus", "SetCardBonusPositive",
        "HideCardBonus", "AddChallenge", "HideAllChallenges", "SetMedal", "HideMedal", "SetMedalScores", "HideMedalScores",
        "SetHighScores", "PlayAnimation", "ShowInfo", "LoadImage", "SetImageHandle", "SetTitle", "AddRibbonItem",
        "ClearAllRibbonItems", "SetActiveRibbon"};
    for (const char* name : kContent)
        if (std::strcmp(fn, name) == 0) return true;
    return false;
}
void __fastcall HookInvokeAS(void* widget, const char* fn, GfxValue* result, const GfxValue* args, unsigned count) {
    if (g_viewerMode.load() && !g_ourWidgetCall && !g_allowWidgetCalls && widget && fn && ContentCall(fn)) {
        if (void* gsm = GameStateMenuing())
            if (widget == Widget(At<std::uint32_t>(gsm, kGsmInfoWidget)) ||
                widget == Widget(At<std::uint32_t>(gsm, kGsmHeaderWidget)))
                return;
    }
    g_origInvokeAS(widget, fn, result, args, count);
}

void Activate(void* state) {
    const auto* item = PickedItem();
    if (!item || g_play.load() != Play::None) return;
    PlaySoundId(state, kSelectSound);
    std::lock_guard lock(g_requestMutex);
    g_request.pending = true;
    g_request.id = item->id;
    g_request.title = item->title;
    g_request.outfit = g_outfitPick > 0 && g_outfitPick < int(g_outfits.size()) ? g_outfits[g_outfitPick].index : -1;
}

void Move(void* state, int delta) {
    int row = g_row;
    for (int i = 0; i < 5; ++i) {
        row = (row + delta + 5) % 5;
        if (RowEnabled(row)) break;
    }
    if (row == g_row) return;
    Highlight(state, row);
    PlaySoundId(state, 0);
}

// The rows changed lately: written again without their slide once it is over (a change the movie dropped shows then).
unsigned g_settleRows = 0;
double g_settleAt = 0.0;

bool InputBlocked() {
    void* mgr = StateManager();
    return mgr && reinterpret_cast<bool(__fastcall*)(void*)>(Fn(kRvaIsInputBlocked))(mgr);
}
void BlockInputForCycle(int row) {
    if (void* mgr = StateManager())
        reinterpret_cast<void(__fastcall*)(void*, float)>(Fn(kRvaSetAllInputBlockTimer))(mgr, kCycleBlockSeconds);
    g_settleRows |= 1u << row;
    g_settleAt = Now() + kCycleBlockSeconds + 0.05;
}

void Cycle(void* state, int row, int delta) {
    const char* dir = delta < 0 ? "Left" : "Right";
    switch (row) {
        case kRowCutscene: {
            if (g_items.empty()) return;
            const int n = int(g_items.size());
            g_pick = (g_pick + delta + n) % n;
            PushRow(state, kRowCutscene, dir);
            BlockInputForCycle(kRowCutscene);
            UpdateInfo();
            break;
        }
        case kRowOutfit: {
            if (g_outfits.size() < 2) return;
            const int n = int(g_outfits.size());
            g_outfitPick = (g_outfitPick + delta + n) % n;
            PushRow(state, kRowOutfit, dir);
            BlockInputForCycle(kRowOutfit);
            break;
        }
        default:
            return;
    }
    PlaySoundId(state, 0);
}

// (v0.9.496) The outfit grid on the right of the Cutscene Viewer screen (OutfitGrid, with the HUD's helpers below).
// (v0.9.497) its layout, set by the screen it is made on (the Expeditions screen's: six by six on the right; the list's:
// nine by four under the picture); in the stage's 1280 x 720
int g_ogColumns = 6, g_ogRows = 6;
double g_ogScale = 0.5, g_ogRight = 1240.0, g_ogTop = 62.0;
bool g_ogInList = false;   // made on the Relics list screen (else the Expeditions screen)
void OutfitGridMake(void* state);
void OutfitGridProcess(void* state);
void OutfitGridRemove(void* state);
void OutfitGridFocus(void* state, bool on);
bool OutfitGridPressed(void* state, int action);
bool OutfitGridHeld(void* state, int action);
void OutfitGridLeave();
void HideOutfitRow(void* state);

// The screen is set up by the combo's own OnEnter (as Expeditions: its movie, widgets, 3D placement), then ours on top:
// the EXP panel and the credits hidden, Chapter Replay's three rows, our labels, values, button bar.
bool __fastcall ExtraOnEnter(void* state, void* prev, void* cur) {
    g_infoReady = g_headerReady = false;
    g_settleRows = 0;
    LoadItems();
    auto** callbackObject = reinterpret_cast<void***>(static_cast<std::uint8_t*>(state) + kComboWidgetCallback);
    g_origCallbackVtable = *callbackObject;
    *callbackObject = g_widgetCallbackVtable;
    const bool entered = reinterpret_cast<bool(__fastcall*)(void*, void*, void*)>(Fn(kRvaComboOnEnter))(state, prev, cur);
    if (!entered) {
        *callbackObject = g_origCallbackVtable;
        g_origCallbackVtable = nullptr;
        return entered;
    }
    const std::uint32_t exp = At<std::uint32_t>(state, kComboExpWidget);
    if (void* panel = Widget(exp)) {
        reinterpret_cast<void(__fastcall*)(void*, float, bool)>((*reinterpret_cast<std::uintptr_t**>(panel))[0x38 / 8])(
            panel, 0.0f, true);
        At<std::uint32_t>(state, kComboExpWidget) = 0xFFFFFFFFu;
    }
    // (State_MenuComboSelection.ShowRemnantResistanceSettings: every row visible, then SetNavItemVisibility's rows 2 and
    // 3 at alpha 0; GetNumNavItems 4 as Chapter Replay's)
    Movie(state, "ShowRemnantResistanceSettings");
    // (Component_MenuLabel.SetText keeps a text only after SetByCodeUsage: a row showed its own label)
    MemberCall(state, "nav_list_item_0_mc", "SetByCodeUsage", {});
    MemberCall(state, "nav_list_item_0_mc", "SetText", {Str(ViewerText::Upper(ViewerText::Cutscene)), Bool(false)});
    Movie(state, "OnEnable", {Num(kRowCutscene)});
    for (int row : {1, 2, 3}) Movie(state, "OnDisable", {Num(row)});   // (the hidden rows: not reachable with the mouse either)
    HideOutfitRow(state);   // (v0.9.496: the OUTFIT row too; the grid on the right instead)
    At<std::int32_t>(state, 0x38) = 5;
    reinterpret_cast<void(__fastcall*)(void*)>(Fn(kRvaRegisterNavItems))(state);
    if (void* gsm = GameStateMenuing())
        reinterpret_cast<void(__fastcall*)(void*, bool, bool)>(Fn(kRvaSetCreditsVisible))(gsm, false, false);
    if (void* bar = reinterpret_cast<void*(__fastcall*)(void*)>(Fn(kRvaGetContextbar))(state)) {
        using AddFn = void(__fastcall*)(void*, int, const char*, std::uint64_t, bool, bool);
        reinterpret_cast<void(__fastcall*)(void*)>(Fn(kRvaContextbarClear))(bar);
        reinterpret_cast<AddFn>(Fn(kRvaContextbarAdd))(bar, kActSelect, "BL_Select", 2, true, true);
        reinterpret_cast<AddFn>(Fn(kRvaContextbarAdd))(bar, kActCancel, "Menu_Back", 3, true, true);
    }
    RefreshAll(state);
    Highlight(state, RowEnabled(g_row) ? g_row : kRowPlay);
    g_enterTime = g_viewerEnterTime = Now();
    g_alphaChecked = false;
    g_creditsHides = 0;
    g_ogColumns = 6, g_ogRows = 6, g_ogScale = 0.5, g_ogRight = 1240.0, g_ogTop = 62.0, g_ogInList = false;
    OutfitGridMake(state);
    Log::Writef("CutsceneViewer: Cutscene Viewer screen (%zu playable cutscenes, %zu outfits)", g_items.size(),
                g_outfits.size());
    return entered;
}

void __fastcall ExtraPressed(void* state, int action, char) {
    if (!(At<std::uint8_t>(state, kStateInputFlags) & 8) || g_play.load() != Play::None) return;
    if (OutfitGridPressed(state, action)) return;   // (v0.9.496) the grid's, while it has the focus
    if (action == kActRight && g_row == kRowPlay) {   // (PLAY has no values: Right goes into the grid)
        OutfitGridFocus(state, true);
        return;
    }
    switch (action) {
        case kActUp: Move(state, -1); break;
        case kActDown: Move(state, +1); break;
        case kActLeft: Cycle(state, g_row, -1); break;
        case kActRight: Cycle(state, g_row, +1); break;
        case kActLB: Cycle(state, kRowCutscene, -1); break;   // (LB / RB: the cutscene from any row)
        case kActRB: Cycle(state, kRowCutscene, +1); break;
        case kActSelect: Activate(state); break;
        case kActCancel:   // as the combo: input blocked 0.3 s, the previous state (the Extras menu)
            if (void* mgr = StateManager()) {
                reinterpret_cast<void(__fastcall*)(void*, float)>(Fn(kRvaSetAllInputBlockTimer))(mgr, 0.3f);
                reinterpret_cast<bool(__fastcall*)(void*, bool, float)>(Fn(kRvaSetStatePrevious))(mgr, true, -1.0f);
            }
            break;
        default: break;
    }
}

void __fastcall ExtraOn(void* state, int action, char) {
    if (!(At<std::uint8_t>(state, kStateInputFlags) & 8) || g_play.load() != Play::None) return;
    if (OutfitGridHeld(state, action)) return;   // (v0.9.496)
    switch (action) {
        case kActUpHeld: Move(state, -1); break;
        case kActDownHeld: Move(state, +1); break;
        case kActLeftHeld: Cycle(state, g_row, -1); break;
        case kActRightHeld: Cycle(state, g_row, +1); break;
        default: break;
    }
}

void __fastcall ExtraUiSelect(void* state, char) {
    if (At<std::int32_t>(state, 0x2C) == kRowPlay) Activate(state);
    else if (RowEnabled(At<std::int32_t>(state, 0x2C))) g_row = At<std::int32_t>(state, 0x2C);
}

void __fastcall ExtraMouseOver(void* state, int id, int param, const char*) {
    if (g_play.load() != Play::None) return;
    OutfitGridLeave();   // (v0.9.496: the mouse on the rows: the grid's keyboard focus ends)
    int row = -1;
    if (id == kMouseContinue) row = kRowPlay;
    else if (id == kMouseSpinner) row = param / 1000 - 1;
    if (row >= 0 && row <= kRowPlay && row != g_row && RowEnabled(row)) Highlight(state, row);
}

void __fastcall ExtraMouseDown(void* state, int id, int param, const char*) {
    if (!(At<std::uint8_t>(state, kStateInputFlags) & 8) || g_play.load() != Play::None) return;
    if (id == kMouseContinue) {
        Highlight(state, kRowPlay);
        Activate(state);
    } else if (id == kMouseSpinner) {
        if (InputBlocked()) return;   // (as the combo's OnMouseDown: the last change still slides)
        const int row = param / 1000 - 1;
        if (row < 0 || row > kRowOutfit || !RowEnabled(row)) return;
        if (row != g_row) Highlight(state, row);
        Cycle(state, row, param % 1000 == 1 ? +1 : -1);
    }
}

// 0.8 s after the screen opened: its movie should be fully shown (it looked dimmed); logged, and faded in if not.
// The credits widget the combo shows (GameStateMenuing +0x80, shown flag +0x12): hidden at once by SetCreditsVisible, it
// still came up once its movie had opened; hidden again 0.3, 0.8, 1.6 and 3 s after the screen opened.
void HideCredits() {
    void* gsm = GameStateMenuing();
    void* credits = gsm ? Widget(At<std::uint32_t>(gsm, 0x80)) : nullptr;
    if (!credits) {
        if (g_creditsHides == 1) Log::Write("CutsceneViewer: no credits widget to hide yet");
        return;
    }
    reinterpret_cast<void(__fastcall*)(void*, float, bool)>((*reinterpret_cast<std::uintptr_t**>(credits))[0x38 / 8])(
        credits, 0.0f, false);
    At<std::uint8_t>(gsm, 0x12) = 0;
}
void __fastcall ExtraProcess(void* state, float) {
    const double since = Now() - g_enterTime;
    // the credits movie loads after the combo's OnEnter asked for it, and shows itself once loaded: hidden a few times
    static constexpr double kCreditsHideTimes[] = {0.3, 0.8, 1.6, 3.0};
    if (g_creditsHides < 4 && since >= kCreditsHideTimes[g_creditsHides]) {
        ++g_creditsHides;
        HideCredits();
    }
    // the button's text again every 0.5 s: the movie's own OnEnter, which can come after ours, writes "CONTINUE" there
    // ($MainMenu_ContinueGame; 0.9.369 showed it)
    static double continueAt = 0.0;
    if (Now() >= continueAt) {
        continueAt = Now() + 0.5;
        Movie(state, "SetContinueText", {Str(ViewerText::Upper(ViewerText::Play))});
        HideOutfitRow(state);   // (v0.9.496: kept hidden should the movie show it again)
    }
    if (g_settleRows && Now() >= g_settleAt) {
        for (int row = kRowCutscene; row <= kRowOutfit; ++row)
            if (g_settleRows & (1u << row)) PushRow(state, row, "");
        g_settleRows = 0;
    }
    OutfitGridProcess(state);   // (v0.9.496)
    if (g_alphaChecked || since < 0.8) return;
    g_alphaChecked = true;
    const double alpha = ClipNumber(state, "_alpha");
    if (alpha >= 0.0 && alpha < 99.0) Movie(state, "FadeIn");
}

bool __fastcall ExtraOnExit(void* state, void* a, void* b) {
    OutfitGridRemove(state);   // (v0.9.496)
    const bool result = reinterpret_cast<bool(__fastcall*)(void*, void*, void*)>(Fn(kRvaComboOnExit))(state, a, b);
    if (g_origCallbackVtable) {
        *reinterpret_cast<void***>(static_cast<std::uint8_t*>(state) + kComboWidgetCallback) = g_origCallbackVtable;
        g_origCallbackVtable = nullptr;
    }
    if (g_comboVtable) *reinterpret_cast<std::uintptr_t**>(state) = g_comboVtable;
    g_viewerMode = false;
    g_infoReady = g_headerReady = false;
    return result;
}

// CUTSCENE VIEWER: the combo state as the Cutscene Viewer screen (v0.9.497: when the Relics list cannot be: OpenViewer).
bool OpenComboViewer() {
    if (!g_nativeReady || g_viewerMode.load()) return false;
    void* state = ComboState();
    if (!state || !StateManager()) return false;
    auto* vtable = VtableOf(state);
    if (reinterpret_cast<std::uintptr_t>(vtable) != Fn(kRvaComboVtable)) {
        Log::Write("CutsceneViewer: the Expeditions screen's object is not the expected one; the Cutscene Viewer not opened");
        return false;
    }
    g_comboVtable = vtable;
    std::memcpy(g_cloneVtable, vtable, kVtableEntries * sizeof(std::uintptr_t));
    g_cloneVtable[0x08 / 8] = reinterpret_cast<std::uintptr_t>(&ExtraUiSelect);
    g_cloneVtable[0x18 / 8] = reinterpret_cast<std::uintptr_t>(&ExtraOnEnter);
    g_cloneVtable[0x20 / 8] = reinterpret_cast<std::uintptr_t>(&ExtraPressed);
    g_cloneVtable[0x28 / 8] = reinterpret_cast<std::uintptr_t>(&ExtraOn);
    g_cloneVtable[0x30 / 8] = reinterpret_cast<std::uintptr_t>(&ExtraProcess);
    g_cloneVtable[0x40 / 8] = reinterpret_cast<std::uintptr_t>(&ExtraOnExit);
    g_cloneVtable[0x78 / 8] = Fn(kRvaGenericAccessible);
    g_cloneVtable[0x80 / 8] = Fn(kRvaGenericSetSelection);
    g_cloneVtable[0x88 / 8] = Fn(kRvaGenericPushHighlight);
    g_cloneVtable[0xA0 / 8] = reinterpret_cast<std::uintptr_t>(&ExtraMouseDown);
    g_cloneVtable[0xB0 / 8] = reinterpret_cast<std::uintptr_t>(&ExtraMouseOver);
    *reinterpret_cast<std::uintptr_t**>(state) = g_cloneVtable;
    g_combo = state;
    g_viewerMode = true;
    g_viewerEnterTime = 0.0;
    if (!SetMenuState(kStateCombo, 0.25f)) {
        *reinterpret_cast<std::uintptr_t**>(state) = g_comboVtable;
        g_viewerMode = false;
        Log::Write("CutsceneViewer: the game refused to open the Cutscene Viewer screen");
        return false;
    }
    return true;
}

bool OpenViewer();   // (v0.9.497: the Relics list, else OpenComboViewer; below)

// ---- The Model Viewer's screens (0.9.418) ----------------------------------------------------------------------------
// MODEL VIEWER opens the campsite's Outfits grid (UiMenuStateCampsiteGear, state 0x59; its movie
// menu_basecamp_gear_select, AS2 class Campsite_GearSelect, in its outfit state) with the viewer's categories in turn
// (LB / RB) and their models as its tiles, each with its picture: the outfits' the game's own (their campsite pictures),
// the others' the Modding Suite's (ROTTRFramework\model_viewer\icons\<object>.png); the gear screen's information widget
// (Widget_Equipment_Loadout_Information, its outfit_info_mc) names the model. Examine (A, Enter, a click) opens the relic
// viewer (UiMenuStateCollectibleViewerRelic, state 0x5E; movie collectible_object_viewer, class Object_Viewer) over the
// model. ModelViewer draws the model in the game's 3D view behind both (on the grid on the right, turning; on the view
// alone, the controls its own).
// - The movies: the relic viewer's is in the in-game menu collection, the grid's and the widget's in the campsite's
//   ("campsite_menus": its ObjectManager ID by its name, from the object names cdc::ObjectManager::LoadObject reads,
//   retail 0x23C189: [[0x1A50B08] +0x28] = {count, names[]}, ID - 1 the name's index), none of which the main menu loads
//   (TR2: UiCollectionManager::GetScaleformMovieData finds a state's movie in the loaded collections by the state's ID,
//   a widget's by its type: retail 0x8B4980, the call at 0x94BC69; UIMenuCollection: count +8, entries +0x10 of 0x210
//   bytes, the menu ID at +0). GameStatePlaying::OnEnter (retail 0x586210) loads the in-game one with
//   UiCollectionManager::Load(manager [0x2E1E760], ID, true, nullptr, 3) (retail 0x8BA050; an object of the global data
//   [0x2D57430]: +0x2814 with a NetSessionBase [0x2D8BB58], else +0x281E in game mode type 9 (0x448470), else +0x2816);
//   the campsite's action graphs load theirs (UICollectionLoadOrUnload: the same Load). The viewer loads what is not there
//   (UiCollectionManager::IsLoaded(menu ID), retail 0x8B9D50), waits for it (the manager's entries: count +0, array +8 of
//   {ID +0, callback +8, collection +0x10}), tries the other in-game ones when one came without the movie, and unloads
//   what it loaded when the main menu comes back (UiCollectionManager::Unload, retail 0x8C5850).
// - Both states' objects get a copy of their vtable (as the Extras menu). The grid's (retail 0xDB6618, 45 entries):
//   our OnEnter, input, Process, OnPreExit, OnExit, PushHighlightIndexToMovie, mouse; UiMenuStateGeneric's
//   SetSelectionIndex; nothing for its own campsite work (OnUiSelect, MenuStateSetup and the rest from +0xF0: the
//   player's inventory, its outfits, the campsite's navigation and widgets). Its OnEnter: UiMenuStateGeneric::OnEnter (the
//   movie), then the gear screen's own SetupInformationWidget (retail 0x94BBE0: the two information widgets, types 0x62
//   and 0x63 of the movie data 0x62, weapon_info_mc and outfit_info_mc, attached to the parent at +0x110 (the campsite
//   menu's movie: none here, see GridOnEnter), their IDs at +0x150 and +0x230; the outfit one shown and the movie's
//   SetInternalMenuState(1) when the category, +0x1F4, is the outfits' (index 5 of the category types at 0xDB4088: {4,
//   7, 6, 5, 8, 14})); its OnPreExit hides them (vfunc
//   +0x38, time -1, removed but on the way to the view) as the gear screen's own does (TR2 0x14096BCD0); the names:
//   UiWidgetGearLoadoutInformation::SendOutfitName / SendOutfitDescription (retail 0x9B9A30 / 0x9B9940: the widget's
//   SetOutfitName / SetOutfitDescription, htmlText).
// - The movie in its outfit state (as UiMenuStateCampsiteGear::PopulateSkills, TR2 0x14097D350): Reset,
//   AddOutfitItem(icon, "Complete", base) a tile (5 a row, 2 rows shown, scrolled by OnIndexSelected), RegisterNavListItems,
//   OnHighlight(index, false); the tiles' Mouse_Button ID 1338 (param: the tile). A tile's picture:
//   LoadOutfitImage(tile, texture ID, ...) loads "img://<ID>" unscaled; the game's are 128x256 (theirs: the outfit's
//   dtp::ImagePath, +0x38 of its OutfitsData, through UiImageLoaderManager ([0x2E1E718], read at 0x8FE031 by
//   LoadOutfitImages): LoadImageW(manager, path, callback, name) (retail 0x8BA530) asks for it, the callback object's
//   vfunc +8 (callback, request, texture ID, image ID) answers (UiImageLoaderManager::BirthImage, retail 0x8AE2C0, from
//   its Process), UnloadImage(manager, &request, callback) (retail 0x8C5AC0) lets it go; ours: the Cutscene Viewer's
//   textures, 256x512, shown at 128x256).
// - Back from the viewer the grid is not entered again (the viewer is not a full screen: see ModelGridReturn).
constexpr int kStateModelGrid = 0x59, kStateModelView = 0x5E;
constexpr std::uintptr_t kRvaModelGridVtable = 0x00DB6618ull, kRvaModelViewVtable = 0x00DB8000ull;
constexpr std::size_t kModelGridEntries = 45, kModelViewEntries = 32;
constexpr std::uintptr_t kRvaGearOnUiSelect = 0x00930020ull, kRvaGearOnEnter = 0x00907730ull, kRvaGearPressed = 0x0091A640ull,
                         kRvaGearProcess = 0x0093C140ull, kRvaGearOnExit = 0x00912610ull, kRvaGearSetupInfo = 0x0094BBE0ull,
                         kRvaSkillsSetSelection = 0x0094A7A0ull, kRvaGearHighlight = 0x00942B70ull;
constexpr std::uintptr_t kRvaViewOwnOnEnter = 0x00908DF0ull, kRvaViewOwnProcess = 0x0093E190ull,
                         kRvaGenericOnEnterAsync = 0x008BB6A0ull, kRvaGenericOnPreExit = 0x008BBE10ull;
constexpr std::uintptr_t kRvaCollectionLoad = 0x008BA050ull, kRvaCollectionUnload = 0x008C5850ull,
                         kRvaMovieAvailable = 0x008B9D50ull, kRvaPlayingCollections = 0x005862A0ull,
                         kRvaWidgetMovieData = 0x008B4980ull, kRvaWidgetMovieDataCall = 0x0094BC61ull,
                         kRvaObjectNamesRead = 0x0023C189ull;
constexpr std::uintptr_t kRvaSendOutfitName = 0x009B9A30ull, kRvaSendOutfitDescription = 0x009B9940ull,
                         kRvaWidgetIsVisible = 0x0007B820ull;
constexpr std::uintptr_t kRvaImageLoaderRead = 0x008FE031ull, kRvaLoadImageW = 0x008BA530ull, kRvaUnloadImage = 0x008C5AC0ull,
                         kRvaGearCategoryTypes = 0x00DB4088ull;
constexpr std::size_t kGearCategory = 0x1F4, kGearWeaponInfo = 0x150, kGearOutfitInfo = 0x230;
constexpr int kGearOutfitsCategory = 5, kInfoWidgetType = 0x62;
constexpr std::size_t kOutfitPicture = 0x38, kImagePathFile = 0x08, kImagePathOffset = 0x10, kImagePathAction = 0x20;
// UiMenuStateManager::SetHeldInputBlockTimer (retail 0x8C2310: the held actions' block, +0xA80 / +0xA88; TR2 0x904730):
// UiMenuStateGeneric's navigation sets 0.3 s after a move; the grid's held moves set less
constexpr std::uintptr_t kRvaSetHeldInputBlockTimer = 0x008C2310ull;
constexpr float kGridFirstRepeat = 0.3f, kGridRepeat = 0.12f;
// GameStatePlaying::OnEnter's choice of the collection (retail 0x5862A0..0x586309): the NetSessionBase test, the game
// mode type, the manager, the global data and its three IDs, the call to UiCollectionManager::Load
constexpr std::uint8_t kPlayingCollectionsSig[] = {
    0x48, 0x39, 0x35, 0xB1, 0x58, 0x80, 0x02, 0x75, 0x38, 0xE8, 0xC2, 0x21, 0xEC, 0xFF, 0x48, 0x8B, 0x0D, 0xAB, 0x84, 0x89, 0x02,
    0x45, 0x33, 0xC9, 0x83, 0xF8, 0x09, 0xC7, 0x44, 0x24, 0x20, 0x03, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05, 0xA6, 0x11, 0x7D, 0x02,
    0x41, 0xB0, 0x01, 0x75, 0x09, 0x0F, 0xB7, 0x90, 0x1E, 0x28, 0x00, 0x00, 0xEB, 0x2C, 0x0F, 0xB7, 0x90, 0x16, 0x28, 0x00, 0x00,
    0xEB, 0x23, 0x48, 0x8B, 0x05, 0x88, 0x11, 0x7D, 0x02, 0x45, 0x33, 0xC9, 0x48, 0x8B, 0x0D, 0x6E, 0x84, 0x89, 0x02, 0x41, 0xB0,
    0x01, 0xC7, 0x44, 0x24, 0x20, 0x03, 0x00, 0x00, 0x00, 0x0F, 0xB7, 0x90, 0x14, 0x28, 0x00, 0x00, 0xE8, 0x47, 0x3D, 0x33, 0x00};
constexpr std::size_t kCollectionOnline = 0x2814, kCollectionStory = 0x2816, kCollectionMode9 = 0x281E;
// the code the globals and the calls are read from: mov edx, 0x62; mov rsi, rax; call GetScaleformMovieData (widget);
// mov rdx, [rip + ObjectManager]; mov rax, [rdx + 0x28]; cmp ebx, [rax]; mov rcx, [rip + UiImageLoaderManager];
// lea r9, [name]; add rdx, 0x38; call LoadImageW
constexpr std::uint8_t kWidgetMovieDataCallSig[] = {0xBA, 0x62, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF0, 0xE8, 0x12, 0x8D, 0xF6, 0xFF};
constexpr std::uint8_t kObjectNamesReadSig[] = {0x48, 0x8B, 0x15, 0x78, 0x49, 0x81, 0x01, 0x48, 0x8B, 0x42, 0x28, 0x3B, 0x18};
constexpr std::uint8_t kImageLoaderReadSig[] = {0x48, 0x8B, 0x0D, 0xE0, 0x06, 0x52, 0x02, 0x4C, 0x8D, 0x0D, 0x29, 0x91, 0x4B, 0x00,
                                                0x48, 0x83, 0xC2, 0x38, 0xE8, 0xE8, 0xC4, 0xFB, 0xFF};
constexpr std::uint8_t kUnloadImageCallSig[] = {0x48, 0x8B, 0x0D, 0x9D, 0xA4, 0x4C, 0x02, 0x4C, 0x8D, 0x83, 0x98, 0x02, 0x00, 0x00,
                                                0x48, 0xC1, 0xE0, 0x05, 0x48, 0x03, 0xD0, 0xE8, 0x32, 0x18, 0xF7, 0xFF};
constexpr std::uint8_t kSendOutfitNameText[] = {0x4C, 0x8D, 0x0D, 0x94, 0x2E, 0x41, 0x00};          // lea r9, "SetOutfitName"
constexpr std::uint8_t kSendOutfitDescriptionText[] = {0x4C, 0x8D, 0x0D, 0x94, 0x2F, 0x41, 0x00};   // "SetOutfitDescription"
const Sig kModelSigs[] = {
    {kRvaCollectionLoad, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C}},
    {kRvaCollectionUnload, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C}},
    {kRvaMovieAvailable, {0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8D, 0x05, 0x59}},
    {kRvaGenericOnPreExit, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48}},
    {kRvaGenericOnEnterAsync, {0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x63}},
    {kRvaSetHeldInputBlockTimer, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xF3, 0x0F}},
    {kRvaGearSetupInfo, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C}},
    {kRvaSendOutfitName, {0x4C, 0x8B, 0xDC, 0x48, 0x81, 0xEC, 0x58, 0x01}},
    {kRvaSendOutfitDescription, {0x4C, 0x8B, 0xDC, 0x48, 0x81, 0xEC, 0x58, 0x01}},
    {kRvaWidgetMovieData, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C}},
    {kRvaWidgetIsVisible, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x83, 0x79}},
    {kRvaObjSetMember, {0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41}},
    {kRvaLoadImageW, {0xE9, 0xAB, 0xFD, 0xFF, 0xFF, 0xCC, 0xCC, 0xCC}},
    {kRvaUnloadImage, {0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83}},
};
constexpr int kActMenuSelect = 0x51, kActMenuClose = 0x154, kActLT = 0x15E, kActRT = 0x161, kActRS = 0x163,
              kActRStick = 0x16A;
constexpr int kMouseGridItem = 1338;   // the grid movie's tiles' Mouse_Button ID (param: the tile; the gear screen's own,
                                       // OnMouseDown TR2 0x140969060; RegisterNavListItems adds the generic 1337 too)
int g_gridColumns = 5;   // the grid's movie's NUM_OUTFITS_PER_ROW (GridLayout: 6, v0.9.431)
constexpr int kMoveSound = 0;
std::uintptr_t g_netSessionGlobal = 0, g_collectionManagerGlobal = 0, g_globalDataGlobal = 0, g_gameModeTypeFn = 0,
               g_objectManagerGlobal = 0, g_imageLoaderGlobal = 0;

// The menu collections the screens need (game thread): the relic viewer's (the in-game ones), the grid's (the
// campsite's), in that order.
struct ModelUi {
    bool waiting = false;
    int need = 0;                            // 0 the relic viewer's movie, 1 the grid's and its widget's
    std::vector<std::uint16_t> candidates;   // the IDs still to try for it
    std::uint16_t asked = 0;                 // the one asked for
    double askedAt = 0.0, openedAt = 0.0;
    std::vector<std::uint16_t> loaded;       // the ones the viewer loaded (unloaded when the main menu comes back)
    int target = 0;                          // (v0.9.497) 0 the Model Viewer's grid, 1 the Cutscene Viewer's list
    int needs = 2;                           // the needs asked for (the list: the in-game one alone)
};
ModelUi g_modelUi;
constexpr int kNeeds = 2;
const char* const kNeedNames[kNeeds] = {"the relic viewer's movie (an in-game menu collection)",
                                        "the grid's movie and its information widget (the campsite's menu collection)"};

void* CollectionManager() {
    return g_collectionManagerGlobal ? *reinterpret_cast<void**>(g_collectionManagerGlobal) : nullptr;
}
bool MovieAvailable(int menuState) {
    void* manager = CollectionManager();
    return manager && reinterpret_cast<bool(__fastcall*)(void*, int)>(Fn(kRvaMovieAvailable))(manager, menuState);
}
bool WidgetMovieAvailable(int widgetType) {
    void* manager = CollectionManager();
    return manager && reinterpret_cast<void*(__fastcall*)(void*, int)>(Fn(kRvaWidgetMovieData))(manager, widgetType);
}
bool NeedMet(int need) {
    return need == 0 ? MovieAvailable(kStateModelView) : MovieAvailable(kStateModelGrid) && WidgetMovieAvailable(kInfoWidgetType);
}
bool ModelMoviesThere() { return NeedMet(0) && NeedMet(1); }
std::uint16_t CollectionId(std::size_t offset) {
    auto* data = g_globalDataGlobal ? *reinterpret_cast<std::uint8_t**>(g_globalDataGlobal) : nullptr;
    return data ? *reinterpret_cast<std::uint16_t*>(data + offset) : 0;
}
// The ObjectManager ID of the object of that name (0: none); the names with "menu" in them into nearby. SEH only.
std::uint16_t ObjectIdByName(const char* name, char* nearby, std::size_t nearSize) noexcept {
    if (nearSize) nearby[0] = '\0';
    __try {
        auto* manager = g_objectManagerGlobal ? *reinterpret_cast<std::uint8_t**>(g_objectManagerGlobal) : nullptr;
        auto* list = manager ? *reinterpret_cast<std::uint8_t**>(manager + 0x28) : nullptr;
        if (!list) return 0;
        const int count = *reinterpret_cast<const std::int32_t*>(list);
        auto* const* names = reinterpret_cast<const char* const*>(list + 8);
        std::size_t used = 0;
        for (int i = 0; i < count && i < 0xFFFF; ++i) {
            const char* n = names[i];
            if (!n) continue;
            if (_stricmp(n, name) == 0) return std::uint16_t(i + 1);
            if (used + 40 < nearSize && (std::strstr(n, "menu") || std::strstr(n, "Menu"))) {
                used += std::size_t(std::snprintf(nearby + used, nearSize - used, "%s%s (%d)", used ? ", " : "", n, i + 1));
            }
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
// The manager's entry for the ID: -1 none, 0 asked (its collection not there yet), 1 loaded. SEH only.
int CollectionEntry(std::uint16_t id) noexcept {
    __try {
        auto* manager = static_cast<std::uint8_t*>(CollectionManager());
        if (!manager) return -1;
        const std::uint32_t count = *reinterpret_cast<std::uint32_t*>(manager);
        auto** entries = *reinterpret_cast<std::uint8_t***>(manager + 8);
        int found = -1;
        for (std::uint32_t i = 0; i < count && i < 256; ++i) {
            const std::uint8_t* entry = entries[i];
            if (!entry || *reinterpret_cast<const std::int32_t*>(entry) != int(id)) continue;
            found = std::max(found, *reinterpret_cast<void* const*>(entry + 0x10) ? 1 : 0);
        }
        return found;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
void UnloadCollection(std::uint16_t id) {
    if (void* manager = CollectionManager())
        reinterpret_cast<void(__fastcall*)(void*, int)>(Fn(kRvaCollectionUnload))(manager, int(id));
}
bool AskNextCollection() {
    void* manager = CollectionManager();
    while (manager && !g_modelUi.candidates.empty()) {
        const std::uint16_t id = g_modelUi.candidates.front();
        g_modelUi.candidates.erase(g_modelUi.candidates.begin());
        if (!id) continue;
        const bool asked =
            reinterpret_cast<bool(__fastcall*)(void*, int, bool, void*, int)>(Fn(kRvaCollectionLoad))(manager, int(id), true, nullptr, 3);
        Log::Detailf("ModelViewer: the menu collection %u asked for, for %s (UiCollectionManager::Load %s)", unsigned(id),
                    kNeedNames[g_modelUi.need], asked ? "took it" : "refused it");
        if (!asked) continue;
        g_modelUi.asked = id;
        g_modelUi.askedAt = Now();
        if (std::find(g_modelUi.loaded.begin(), g_modelUi.loaded.end(), id) == g_modelUi.loaded.end())
            g_modelUi.loaded.push_back(id);
        return true;
    }
    return false;
}
// The collections to try for the need.
void PrepareNeed(int need) {
    g_modelUi.need = need;
    g_modelUi.candidates.clear();
    if (need == 0) {
        const bool online = g_netSessionGlobal && *reinterpret_cast<void**>(g_netSessionGlobal);
        const int mode = g_gameModeTypeFn ? reinterpret_cast<int(__fastcall*)()>(g_gameModeTypeFn)() : 0;
        const std::size_t first = online ? kCollectionOnline : mode == 9 ? kCollectionMode9 : kCollectionStory;
        for (const std::size_t offset : {first, kCollectionStory, kCollectionOnline, kCollectionMode9}) {
            const std::uint16_t id = CollectionId(offset);
            if (id && std::find(g_modelUi.candidates.begin(), g_modelUi.candidates.end(), id) == g_modelUi.candidates.end())
                g_modelUi.candidates.push_back(id);
        }
        Log::Writef("ModelViewer: %s is not loaded: the in-game menu collections (online %u, story %u, mode 9 %u; a net session "
                    "%s, game mode type %d) asked for in turn", kNeedNames[0], unsigned(CollectionId(kCollectionOnline)),
                    unsigned(CollectionId(kCollectionStory)), unsigned(CollectionId(kCollectionMode9)),
                    online ? "there" : "not there", mode);
    } else {
        char nearby[1024];
        const std::uint16_t id = ObjectIdByName("campsite_menus", nearby, sizeof(nearby));
        if (id) g_modelUi.candidates.push_back(id);
        if (id) Log::Writef("ModelViewer: %s is not loaded: campsite_menus (object %u) asked for", kNeedNames[1], unsigned(id));
        else Log::Writef("ModelViewer: %s is not loaded, and no object is named campsite_menus (the objects with \"menu\": %s)",
                         kNeedNames[1], nearby[0] ? nearby : "none");
    }
}
// (the main menu's OnEnter) what the viewer loaded, unloaded (nothing when the game unloaded it itself)
void UnloadModelUi() {
    if (g_modelUi.waiting || g_modelUi.loaded.empty()) return;
    for (const std::uint16_t id : g_modelUi.loaded) {
        const int entry = CollectionEntry(id);
        UnloadCollection(id);
        Log::Detailf("ModelViewer: the menu collection %u unloaded%s", unsigned(id), entry < 0 ? " (it was not there)" : "");
    }
    g_modelUi.loaded.clear();
}

// The screens' state (game thread)
// (v0.9.438) A tile shows a model of the category (the search leaves some out); the pictures are the
// category's models', kept while the tiles change.
struct GridPicture {
    int request = -1;            // the game's picture asked for (UiImageLoaderManager)
    std::string icon;            // ours (the Cutscene Viewer's pictures' key) once asked for
    std::uint32_t texture = 0;   // its texture ID once there (the game's or ours)
};
struct GridTile {
    int model = 0;               // the category's model it shows
    bool sent = false;           // its picture given to the movie
};
std::vector<GridTile> g_gridTiles;
std::vector<GridPicture> g_gridPictures;   // the category's models' (by model)
int g_gridPicturesCategory = -1;
int g_gridCategory = 0;
std::wstring g_gridSearch;                 // the grid's search (v0.9.438 the outfits'; v0.9.440 every category's)
std::atomic<bool> g_searchFocus{false};    // typing in it (the window's keys kept from the game)
std::atomic<bool> g_searchHere{false};     // (game thread) the grid's input on: Ctrl+F types in its search
std::atomic<ULONGLONG> g_searchLeftAt{0};  // (GetTickCount64) when the typing ended: the keys let go kept a while
std::mutex g_searchMutex;
std::wstring g_searchEdits;                // (the window's thread) the characters typed, for the grid's Process
int g_gridHighlighted = -1;                // the tile highlighted last (its check mark hidden again after the next)
int TileModel(int tile) {
    return tile >= 0 && tile < int(g_gridTiles.size()) ? g_gridTiles[std::size_t(tile)].model : -1;
}
int ModelTile(int model) {
    for (std::size_t i = 0; i < g_gridTiles.size(); ++i)
        if (g_gridTiles[i].model == model) return int(i);
    return -1;
}
std::string g_gridName, g_gridText, g_gridStatus, g_viewSub, g_viewName, g_viewText;
std::atomic<bool> g_modelGridMode{false}, g_modelViewMode{false};
void* g_modelGridState = nullptr;
void* g_modelViewState = nullptr;
std::uintptr_t* g_modelGridVtable = nullptr;
std::uintptr_t* g_modelViewVtable = nullptr;
std::uintptr_t g_modelGridStore[kModelGridEntries + 1] = {};   // [0]: the RTTI locator; the vtable from [1]
std::uintptr_t g_modelViewStore[kModelViewEntries + 1] = {};
std::int32_t g_gearCategoryBefore = 0;      // the gear screen's category (+0x1F4) before ours
double g_modelBackAt = -10.0;               // (one Back a press: the key's and the menu's actions, a frame apart)
double g_modelGridEnteredAt = 0.0;
bool g_modelGridLogged = false;
unsigned g_viewSerial = 0;
std::string g_viewStatus;
bool g_viewLoadedSent = false;

unsigned __fastcall ReturnZero(void*, int, void*) { return 0; }

// The game's pictures' callback object (see UiImageLoaderManager): its vfunc +8 told the picture is there.
struct ImageCallback {
    const std::uintptr_t* vtable;
};
void __fastcall ImageCallbackNothing(void*, unsigned, unsigned, unsigned) {}
void OutfitGridPictureLoaded(unsigned request, unsigned texture);   // (v0.9.496, below)
void __fastcall ImageCallbackLoaded(void*, unsigned request, unsigned texture, unsigned) {
    if (texture == 0xFFFFFFFFu) return;
    OutfitGridPictureLoaded(request, texture);
    for (std::size_t m = 0; m < g_gridPictures.size(); ++m) {
        if (g_gridPictures[m].request != int(request)) continue;
        g_gridPictures[m].texture = texture;
        for (GridTile& tile : g_gridTiles)
            if (tile.model == int(m)) tile.sent = false;
    }
}
std::uintptr_t g_imageCallbackVtable[4] = {};
ImageCallback g_imageCallback{g_imageCallbackVtable};
void* ImageLoaderManager() { return g_imageLoaderGlobal ? *reinterpret_cast<void**>(g_imageLoaderGlobal) : nullptr; }
// The outfit's picture path when it has one (its file named, not "null"). SEH only.
const void* OutfitPicture(const void* outfit) noexcept {
    __try {
        auto* path = static_cast<const std::uint8_t*>(outfit) + kOutfitPicture;
        const char* file = *reinterpret_cast<const char* const*>(path + kImagePathFile);
        return file && file[0] && std::strcmp(file, "null") != 0 ? path : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
int AskOutfitPicture(const void* outfit) {
    void* manager = ImageLoaderManager();
    const void* path = outfit ? OutfitPicture(outfit) : nullptr;
    if (!manager || !path) return -1;
    using LoadFn = int(__fastcall*)(void*, const void*, void*, const char*);
    return reinterpret_cast<LoadFn>(Fn(kRvaLoadImageW))(manager, path, &g_imageCallback, "ModelViewer");
}
void ReleaseGridPictures() {
    void* manager = ImageLoaderManager();
    for (GridPicture& picture : g_gridPictures)
        if (picture.request >= 0 && manager)
            reinterpret_cast<void(__fastcall*)(void*, int*, void*)>(Fn(kRvaUnloadImage))(manager, &picture.request,
                                                                                         &g_imageCallback);
    g_gridPictures.clear();
    g_gridPicturesCategory = -1;
    g_gridTiles.clear();
}
// The picture's place in the tile, as the game gives its own (dtp::ImagePath OffSetX, OffSetY, Width, Height, Action;
// the movie's LoadOutfitImage keeps the action alone). SEH only.
bool PictureLayoutOf(const void* outfit, int out[4], const char*& action) noexcept {
    action = "";
    out[0] = out[1] = 0, out[2] = 128, out[3] = 256;
    if (!outfit) return true;
    __try {
        auto* path = static_cast<const std::uint8_t*>(outfit) + kOutfitPicture;
        std::memcpy(out, path + kImagePathOffset, sizeof(int) * 4);
        const char* a = *reinterpret_cast<const char* const*>(path + kImagePathAction);
        action = a ? a : "";
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RestoreModelGrid() {
    if (g_modelGridState && g_modelGridVtable && VtableOf(g_modelGridState) == &g_modelGridStore[1]) {
        *reinterpret_cast<std::uintptr_t**>(g_modelGridState) = g_modelGridVtable;
        At<std::int32_t>(g_modelGridState, kGearCategory) = g_gearCategoryBefore;
    }
    ReleaseGridPictures();
    g_modelGridMode = false;
}
void RestoreModelView() {
    if (g_modelViewState && g_modelViewVtable && VtableOf(g_modelViewState) == &g_modelViewStore[1])
        *reinterpret_cast<std::uintptr_t**>(g_modelViewState) = g_modelViewVtable;
    g_modelViewMode = false;
}

void AddLegend(void* state, std::initializer_list<std::tuple<int, const char*, int>> items) {
    reinterpret_cast<void(__fastcall*)(void*)>(Fn(kRvaInitContextbar))(state);
    void* bar = reinterpret_cast<void*(__fastcall*)(void*)>(Fn(kRvaGetContextbar))(state);
    if (!bar) return;
    using AddFn = void(__fastcall*)(void*, int, const char*, std::uint64_t, bool, bool);
    reinterpret_cast<void(__fastcall*)(void*)>(Fn(kRvaContextbarClear))(bar);
    for (const auto& [action, label, slot] : items)
        reinterpret_cast<AddFn>(Fn(kRvaContextbarAdd))(bar, action, label, std::uint64_t(slot), true, true);
}

bool ModelBackAllowed() {
    const double now = Now();
    if (now - g_modelBackAt < 0.4) return false;
    g_modelBackAt = now;
    return true;
}
// (time: the screen's fade out before the change, UiMenuStateManager::SetStatePrevious; below 0 the movie's own pre-hide
// time. sound: the back sound, ours and SetStatePrevious's own)
void ModelGoBack(void* state, float time = -1.0f, bool sound = true) {
    if (sound) PlaySoundId(state, kBackSound);
    if (void* mgr = StateManager()) {
        reinterpret_cast<void(__fastcall*)(void*, float)>(Fn(kRvaSetAllInputBlockTimer))(mgr, sound ? 0.3f : 0.15f);
        const bool done = reinterpret_cast<bool(__fastcall*)(void*, bool, float)>(Fn(kRvaSetStatePrevious))(mgr, sound, time);
        if (!done) Log::Write("ModelViewer: Back: UiMenuStateManager::SetStatePrevious refused");
    }
}

// Text for the widget's htmlText.
std::string Html(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '&') out += "&amp;";
        else if (c == '\n') out += "<br>";
        else out += c;
    }
    return out;
}

// -- The grid --
void* GridWidget(void* state, std::size_t at) { return Widget(At<std::uint32_t>(state, at)); }

// The information widget's name and text: the model's name; its category, its place in it, the game's description,
// its object, what the viewer says about it.
void GridTexts(void* state, bool changed) {
    void* widget = GridWidget(state, kGearOutfitInfo);
    if (!widget) return;
    int category = 0, index = 0;
    ModelViewer::Current(category, index);
    g_gridStatus = ModelViewer::Status();
    if (changed) WidgetCall(widget, "OnHighlight");   // (the widget's change: the texts before go)
    g_gridName = Html(ModelViewer::ModelLabel(category, index));
    char where[160];
    snprintf(where, sizeof(where), "%s   %d / %d", ViewerText::ToUpper(ModelViewer::CategoryName(category)).c_str(), index + 1,
             ModelViewer::ModelCount(category));
    std::string text = where;
    const std::string description = ModelViewer::ModelDescription(category, index);
    if (!description.empty()) text += "\n\n" + description;
    if (!g_gridStatus.empty()) text += "\n\n" + g_gridStatus;
    g_gridText = Html(text);
    reinterpret_cast<void(__fastcall*)(void*, const char*)>(Fn(kRvaSendOutfitName))(widget, g_gridName.c_str());
    reinterpret_cast<void(__fastcall*)(void*, const char*)>(Fn(kRvaSendOutfitDescription))(widget, g_gridText.c_str());
}

// The tiles' pictures: asked for (the outfits' the game's, the others' ours), given to the movie once there.
void GridPictures(void* state) {
    const int n = int(g_gridTiles.size());
    for (int i = 0; i < n; ++i) {
        GridTile& tile = g_gridTiles[std::size_t(i)];
        if (tile.model < 0 || tile.model >= int(g_gridPictures.size())) continue;
        GridPicture& picture = g_gridPictures[std::size_t(tile.model)];
        const void* outfit = ModelViewer::ModelOutfit(g_gridCategory, tile.model);
        if (!outfit && !picture.texture) {
            if (picture.icon.empty()) {   // (asked for once: its file looked at once a category's fill)
                const std::wstring icon = ModelViewer::ModelIcon(g_gridCategory, tile.model);
                PictureMake make;
                make.width = g_loadImageHooked ? 256 : 128;
                make.height = g_loadImageHooked ? 512 : 256;
                make.contain = true;
                make.shownWidth = 128, make.shownHeight = 256;
                picture.icon = "icon:" + ModelViewer::ModelObject(g_gridCategory, tile.model);
                picture.texture = PictureFromFile(picture.icon, icon, FileStamp(icon), make);
            } else if (const auto it = g_pictures.find(picture.icon); it != g_pictures.end() && it->second.state == 2) {
                picture.texture = it->second.textureId;
            }
        }
        if (!picture.texture || tile.sent) continue;
        tile.sent = true;
        int layout[4];
        const char* action = "";
        PictureLayoutOf(outfit, layout, action);
        Movie(state, "LoadOutfitImage", {Num(i), Num(double(picture.texture)), Num(layout[0]), Num(layout[1]), Num(layout[2]),
                                         Num(layout[3]), Str(action)});
    }
}

GfxValue* StateClip(void* state);
bool GfxChild(const GfxValue& parent, const char* name, GfxValue& out);
bool GfxSet(const GfxValue& object, const char* name, const GfxValue& value);
// (v0.9.438) A tile's check mark hidden: the campsite's mark of the outfit worn (the tile, navListItemCraft:
// checkmark_mc at its depth 13, placed on its first frame for all of its 20; frames OFF, an empty box, and ON),
// which UpdateItemState (each highlight, RegisterNavListItems) only moves to a frame.
void HideTileCheckmark(void* state, int tile) {
    char name[48];
    std::snprintf(name, sizeof(name), "nav_list_item_%d_mc", tile);
    GfxValue tiles, item, mark;
    if (GfxChild(*StateClip(state), "campsite_outfit_select_mc", tiles) && GfxChild(tiles, name, item) &&
        GfxChild(item, "checkmark_mc", mark))
        GfxSet(mark, "_visible", Bool(false));
    ReleaseValue(mark);
    ReleaseValue(item);
    ReleaseValue(tiles);
}

// The search (v0.9.438; every category's from v0.9.440): a model whose name (or object) has the text, either in capitals.
bool SearchMatches(int category, int model, const std::string& upper) {
    if (upper.empty()) return true;
    if (ViewerText::ToUpper(ModelViewer::ModelLabel(category, model)).find(upper) != std::string::npos) return true;
    return ViewerText::ToUpper(ModelViewer::ModelObject(category, model)).find(upper) != std::string::npos;
}
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(std::size_t(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}
bool GridSearches(int) { return !g_gridSearch.empty(); }

// The category's tiles (as PopulateSkills fills the outfits): its models, those the search finds. (v0.9.440) Another
// category: its own models, the search emptied and its typing ended.
void GridFill(void* state, int category) {
    if (category != g_gridCategory && (!g_gridSearch.empty() || g_searchFocus.load())) {
        g_gridSearch.clear();
        if (g_searchFocus.exchange(false)) g_searchLeftAt = GetTickCount64();
    }
    if (category != g_gridPicturesCategory) {
        ReleaseGridPictures();
        g_gridPicturesCategory = category;
        g_gridPictures.assign(std::size_t(std::max(0, ModelViewer::ModelCount(category))), GridPicture{});
        for (int m = 0; m < int(g_gridPictures.size()); ++m)
            if (const void* outfit = ModelViewer::ModelOutfit(category, m)) g_gridPictures[std::size_t(m)].request = AskOutfitPicture(outfit);
    }
    Movie(state, "ResetAllImages");
    Movie(state, "Reset");
    g_gridCategory = category;
    g_gridTiles.clear();
    const std::string upper = GridSearches(category) ? ViewerText::ToUpper(Utf8(g_gridSearch)) : std::string();
    for (int m = 0; m < int(g_gridPictures.size()); ++m)
        if (SearchMatches(category, m, upper)) g_gridTiles.push_back(GridTile{m, false});
    const int n = int(g_gridTiles.size());
    for (int i = 0; i < n; ++i) Movie(state, "AddOutfitItem", {Str(""), Str("Complete"), Bool(true)});
    Movie(state, "RegisterNavListItems");
    for (int i = 0; i < n; ++i) HideTileCheckmark(state, i);
    At<std::int32_t>(state, kStateNavCount) = std::int32_t(n);
    At<std::int32_t>(state, kStateSelected) = -1;
    g_gridHighlighted = -1;
    GridPictures(state);
}

void __fastcall GridHighlight(void* state, int index, int) {
    const int n = int(g_gridTiles.size());
    if (index < 0 || index >= n) return;
    const int before = g_gridHighlighted;
    Movie(state, "OnHighlight", {Num(index), Bool(false)});
    Movie(state, "OnIndexSelected", {Num(index)});   // (it scrolls the grid to the tile)
    HideTileCheckmark(state, index);   // (the two tiles OnHighlight's UpdateItemState moved to a frame)
    if (before != index && before >= 0 && before < n) HideTileCheckmark(state, before);
    g_gridHighlighted = index;
    ModelViewer::Choose(g_gridCategory, TileModel(index));
    GridTexts(state, true);
}

void GridSelect(void* state, int index, bool sound) {
    const int n = int(g_gridTiles.size());
    if (!n) return;
    index = std::clamp(index, 0, n - 1);
    if (index == At<std::int32_t>(state, kStateSelected)) return;
    reinterpret_cast<void(__fastcall*)(void*, int)>(Fn(kRvaGenericSetSelection))(state, index);
    GridHighlight(state, index, 0);
    if (sound) PlaySoundId(state, kMoveSound);
}
// A model of the category selected: its tile (none found by the search: the search cleared first).
void GridSelectModel(void* state, int model) {
    int tile = ModelTile(model);
    if (tile < 0 && GridSearches(g_gridCategory)) {
        g_gridSearch.clear();
        GridFill(state, g_gridCategory);
        tile = ModelTile(model);
    }
    GridSelect(state, std::max(tile, 0), false);
}

// The arrows: left / right the tile before / after (in the row's order), up / down the row above / below (from the
// last row's tiles, the last tile when the row below is short of it).
void GridMove(void* state, int dx, int dy, bool held) {
    const int n = int(g_gridTiles.size());
    if (!n) return;
    const int at = std::clamp(At<std::int32_t>(state, kStateSelected), 0, n - 1);
    int to = at + dx;
    if (dy) {
        to = at + dy * g_gridColumns;
        if (to >= n) to = at / g_gridColumns < (n - 1) / g_gridColumns ? n - 1 : at;
        if (to < 0) to = at;
    }
    GridSelect(state, std::clamp(to, 0, n - 1), true);
    if (void* mgr = StateManager())
        reinterpret_cast<void(__fastcall*)(void*, float)>(Fn(kRvaSetHeldInputBlockTimer))(mgr, held ? kGridRepeat : kGridFirstRepeat);
}

// A category (its model where it was): LB / RB, LT / RT the previous / next one, a tab's click that one (v0.9.431).
void GridCategoryJump(void* state, int target) {
    if (target < 0 || target >= ModelViewer::CategoryCount() || target == g_gridCategory) return;
    ModelViewer::ChooseCategory(target);
    int category = 0, index = 0;
    ModelViewer::Current(category, index);   // (the category's own place)
    GridFill(state, target);
    GridSelectModel(state, index);
    PlaySoundId(state, kMoveSound);
}
void GridCategoryStep(void* state, int delta) {
    const int categories = ModelViewer::CategoryCount();
    if (categories < 2) return;
    GridCategoryJump(state, ((g_gridCategory + delta) % categories + categories) % categories);
}

bool OpenModelView(void* state);

void __fastcall GridPressed(void* state, int action, char) {
    if (!(At<std::uint8_t>(state, kStateInputFlags) & 8)) return;
    switch (action) {
        case kActUp: GridMove(state, 0, -1, false); break;
        case kActDown: GridMove(state, 0, +1, false); break;
        case kActLeft: GridMove(state, -1, 0, false); break;
        case kActRight: GridMove(state, +1, 0, false); break;
        case kActLB:
        case kActLT: GridCategoryStep(state, -1); break;
        case kActRB:
        case kActRT: GridCategoryStep(state, +1); break;
        case kActSelect: OpenModelView(state); break;
        case kActCancel:
        case kActMenuClose:
        case kActMenuSelect:
            if (ModelBackAllowed()) ModelGoBack(state);
            break;
        default: break;
    }
}

void __fastcall GridOn(void* state, int action, char) {
    if (!(At<std::uint8_t>(state, kStateInputFlags) & 8)) return;
    if (action == kActUpHeld) GridMove(state, 0, -1, true);
    else if (action == kActDownHeld) GridMove(state, 0, +1, true);
    else if (action == kActLeftHeld) GridMove(state, -1, 0, true);
    else if (action == kActRightHeld) GridMove(state, +1, 0, true);
}

void __fastcall GridMouseOver(void* state, int id, int param, const char*) {
    if (id == kMouseGridItem && param >= 0 && param < int(g_gridTiles.size())) GridSelect(state, param, false);
}

void __fastcall GridMouseDown(void* state, int id, int param, const char*) {
    if (!(At<std::uint8_t>(state, kStateInputFlags) & 8)) return;
    if (id != kMouseGridItem || param < 0 || param >= int(g_gridTiles.size())) return;
    GridSelect(state, param, false);
    OpenModelView(state);
}

void GridLegend(void* state) {
    AddLegend(state, {{kActSelect, "StatusMenu_ExamineRelic", 2}, {kActRB, "StatusMenu_NextSet", 2},
                      {kActLB, "StatusMenu_PrevSet", 2}, {kActMenuClose, "Menu_Back", 3}});
}

void ShowWidget(void* widget, bool show, bool remove) {
    if (!widget) return;
    const bool visible = reinterpret_cast<bool(__fastcall*)(void*)>(Fn(kRvaWidgetIsVisible))(widget);
    if (show && !visible) reinterpret_cast<void(__fastcall*)(void*, float)>(VtableOf(widget)[0x30 / 8])(widget, -1.0f);
    else if (!show && (visible || remove))
        reinterpret_cast<void(__fastcall*)(void*, float, bool)>(VtableOf(widget)[0x38 / 8])(widget, -1.0f, remove);
}

// The grid's layout (v0.9.423 a third row; v0.9.431 six a row, four rows, half the size, on the left of the screen;
// v0.9.433 five rows, 0.55 of the size:
// the model on the right, ModelViewer; its title, tabs and name about it, GridHud's).
// Campsite_GearSelect (the campsite's gear movie, GearNavButtons: the state's clip, campsite_nav_select_mc at 607.1,
// -55.05 on the 1280 x 720 stage) puts its tiles (campsite_outfit_select_mc's children: that clip at 332.2, 68.5, its
// first tile at -304.5, so 27.7, 137 in the state's clip) NUM_OUTFITS_PER_ROW (5) a row, 125 apart, m_outfitDeltaY
// (180) a row (AddOutfitItem), and shows NUM_ROW_OUTFITS_VISIBLE (2) rows under mask_mc (depth 59, clip depth 63: the
// outfits at depth 61; its shape's y 425.75 to 751.35, the clip at _y -360.05, _yscale 114.77: 128.6 to 502.3 in the
// state's clip, x -95.8 to 921.3), the scroll bar's track scrollbar_background_mc (y 0 to 341.6 at 140.4) and the
// backdrop OutfitVeil_mc (a centred 512 tall shape, _y 298.5, x 1.0625; x -262 to 878); SetCursorPosition,
// OnIndexSelected and UpdateOutfitScrollbar scroll and size the bar by NUM_ROW_OUTFITS_VISIBLE (the movie's AS). So,
// before the tiles are added: NUM_OUTFITS_PER_ROW 6 (the mask and the backdrop are wide enough for the tiles' 27.7 to
// 777.7), NUM_ROW_OUTFITS_VISIBLE 5, the mask, the track and the backdrop three rows taller (their tops kept); then the
// clip scaled by [ModelViewer] GridScale and placed so that the tiles' left edge is at GridLeft and the mask's top at
// GridTop on the stage (its m_OrigX too: State_Base.AnimateIn slides it there). Each read checked against those values
// (the movie's own, from the game's .swf): anything else, the grid stays the game's (five a row, two rows, on the right).
// The place is checked four times a second (AnimateIn, AnimateOut move the clip).
constexpr double kMaskShapeTop = 425.75, kMaskShapeBottom = 751.35, kTilesLeft = 27.7;
constexpr int kGridColumnsOurs = 6, kGridRowsOurs = 5;
double g_gridTargetX = NAN, g_gridTargetY = NAN, g_gridTargetScale = NAN, g_gridPlacedAt = -10.0;
bool Near(double value, double expected, double within) { return std::isfinite(value) && std::fabs(value - expected) <= within; }
void PlaceGrid(void* state) {
    if (std::isnan(g_gridTargetX)) return;
    g_gridPlacedAt = Now();
    if (std::fabs(ClipNumber(state, "_xscale") - g_gridTargetScale) > 0.05) {
        SetClipNumber(state, "_xscale", g_gridTargetScale);
        SetClipNumber(state, "_yscale", g_gridTargetScale);
    }
    if (std::fabs(ClipNumber(state, "_x") - g_gridTargetX) > 0.5) SetClipNumber(state, "_x", g_gridTargetX);
    if (std::fabs(ClipNumber(state, "_y") - g_gridTargetY) > 0.5) SetClipNumber(state, "_y", g_gridTargetY);
}
// (v0.9.500) the Cutscene Viewer's grid over its list: its own place and size (the same six by five)
bool g_gridLayoutOverride = false;
double g_gridLayoutScale = 0.38, g_gridLayoutLeft = 780.0, g_gridLayoutTop = 330.0;
int g_gridLayoutColumns = 6;   // (v0.9.504: the Cutscene Viewer's eight)
// (v0.9.504) menu_basecamp_gear_select's own: OutfitVeil_mc (the dark backdrop, shape 65: a 1024 x 512 bitmap) at x
// 307.95, 111.33 %, over the scroll bar (x -5.95) and five tiles (to 27.7 + 5 x 125); mask_mc (shape 67, x 20.9 ..
// 452.6) at x -144.95, 235.6 %: to x 921.4, seven tiles. For more columns both made wider (the backdrop by the tiles
// added, its middle moved half as much; the mask to the last tile and 15 more).
constexpr double kVeilX = 307.95, kVeilScaleX = 111.328125, kVeilSpan = 652.7 + 5.95, kMaskScaleX = 235.601806640625,
                 kMaskShapeRight = 452.6, kGridTileStep = 125.0;
// (v0.9.505) and its height: 106.25 % (544) about y 298.5, the movie's two rows from y 128.59 (mask_mc's top) to 488.59
constexpr double kVeilScaleY = 106.25, kVeilY = 298.5, kVeilAreaX = (652.7 - 5.95) * 0.5, kVeilAreaY = 128.59 + 180.0,
                 kVeilMargin = 1.08;
void GridLayout(void* state) {
    g_gridTargetX = g_gridTargetY = g_gridTargetScale = NAN;
    g_gridColumns = 5;
    // (the class's own values, 5, 2 and 180, may not be readable from the clip: its prototype's; once set, they are the
    // clip's: this movie's already, its mask taken as it is)
    const double columns = ClipNumber(state, "NUM_OUTFITS_PER_ROW"), rows = ClipNumber(state, "NUM_ROW_OUTFITS_VISIBLE");
    const double readHeight = ClipNumber(state, "m_outfitDeltaY"), rowHeight = readHeight == -1.0 ? 180.0 : readHeight;
    const double maskY = MemberNumber(state, "mask_mc", "_y"), maskScale = MemberNumber(state, "mask_mc", "_yscale");
    const double outfitsX = MemberNumber(state, "campsite_outfit_select_mc", "_x");
    const int wantColumns = g_gridLayoutOverride ? g_gridLayoutColumns : kGridColumnsOurs;
    const bool already = columns == double(wantColumns) && rows == double(kGridRowsOurs);
    if (!already) {
        const double trackY = MemberNumber(state, "scrollbar_background_mc", "_y"),
                     trackHeight = MemberNumber(state, "scrollbar_background_mc", "_height");
        const double veilY = MemberNumber(state, "OutfitVeil_mc", "_y"), veilHeight = MemberNumber(state, "OutfitVeil_mc", "_height");
        if ((columns != 5.0 && columns != -1.0) || (rows != 2.0 && rows != -1.0) || !Near(rowHeight, 180.0, 0.01) ||
            !Near(maskY, -360.05, 1.0) || !Near(maskScale, 114.77, 0.5) || !Near(outfitsX, 332.2, 1.0) || !Near(trackY, 140.4, 1.0) ||
            !Near(trackHeight, 341.6, 1.0) || !Near(veilY, 298.5, 1.0) || !Near(veilHeight, 544.0, 2.0)) {
            Log::Writef("ModelViewer: the grid's movie is not as expected (%.0f a row, rows %.0f, row height %.1f, mask y %.2f scale "
                        "%.2f, tiles' clip x %.1f, track y %.1f height %.1f, backdrop y %.1f height %.1f): the game's grid",
                        columns, rows, rowHeight, maskY, maskScale, outfitsX, trackY, trackHeight, veilY, veilHeight);
            return;
        }
        const double k = maskScale / 100.0, top = maskY + k * kMaskShapeTop, bottom = maskY + k * kMaskShapeBottom;
        const double added = rowHeight * double(kGridRowsOurs - 2);
        const double kTall = (bottom + added - top) / (kMaskShapeBottom - kMaskShapeTop);
        const bool ok = SetMemberNumber(state, "mask_mc", "_yscale", kTall * 100.0) &&
                        SetMemberNumber(state, "mask_mc", "_y", top - kTall * kMaskShapeTop) &&
                        SetMemberNumber(state, "scrollbar_background_mc", "_height", trackHeight + added) &&
                        SetMemberNumber(state, "OutfitVeil_mc", "_height", veilHeight + added) &&
                        SetMemberNumber(state, "OutfitVeil_mc", "_y", veilY + added * 0.5) &&
                        SetClipNumber(state, "NUM_OUTFITS_PER_ROW", double(wantColumns)) &&
                        SetClipNumber(state, "NUM_ROW_OUTFITS_VISIBLE", double(kGridRowsOurs));
        if (!ok) {
            Log::Write("ModelViewer: the grid's movie could not be set (six a row, five rows): the game's grid");
            return;
        }
    }
    const double maskTop = MemberNumber(state, "mask_mc", "_y") + MemberNumber(state, "mask_mc", "_yscale") / 100.0 * kMaskShapeTop;
    const double scale = g_gridLayoutOverride ? g_gridLayoutScale : double(ModelViewer::GridScale()),
                 left = g_gridLayoutOverride ? g_gridLayoutLeft : double(ModelViewer::GridLeft()),
                 top = g_gridLayoutOverride ? g_gridLayoutTop : double(ModelViewer::GridTop());
    if (!std::isfinite(maskTop)) return;
    g_gridColumns = wantColumns;
    if (wantColumns > 5) {   // (v0.9.504: for the columns past the movie's five)
        const double added = kGridTileStep * double(wantColumns - 5);
        if (wantColumns != kGridColumnsOurs) {   // (the Model Viewer's six as before)
            // (v0.9.505) the backdrop over all of the tiles, as the movie's own over its five by two: sized and placed to
            // the tiles' area (the scroll bar to the last column, mask_mc's top to the last row) as the movie's is to
            // its own, 8 % more each way (its edges are ragged). 0.9.504 widened it alone: the first row stood above it.
            const double spanX = kVeilSpan + added, spanY = rowHeight * double(kGridRowsOurs);
            const double fx = spanX / kVeilSpan * kVeilMargin, fy = spanY / (rowHeight * 2.0) * kVeilMargin;
            const double areaX = (kTilesLeft + kGridTileStep * double(wantColumns) - 5.95) * 0.5, areaY = maskTop + spanY * 0.5;
            SetMemberNumber(state, "OutfitVeil_mc", "_xscale", kVeilScaleX * fx);
            SetMemberNumber(state, "OutfitVeil_mc", "_yscale", kVeilScaleY * fy);
            SetMemberNumber(state, "OutfitVeil_mc", "_x", areaX + (kVeilX - kVeilAreaX) * fx);
            SetMemberNumber(state, "OutfitVeil_mc", "_y", areaY + (kVeilY - kVeilAreaY) * fy);
        }
        const double maskX = MemberNumber(state, "mask_mc", "_x");
        const double maskRight = kTilesLeft + kGridTileStep * double(wantColumns) + 15.0;
        if (std::isfinite(maskX))
            SetMemberNumber(state, "mask_mc", "_xscale", std::max(kMaskScaleX, (maskRight - maskX) / kMaskShapeRight * 100.0));
    }
    g_gridTargetScale = scale * 100.0;
    g_gridTargetX = left - scale * kTilesLeft;
    g_gridTargetY = top - scale * maskTop;
    SetClipNumber(state, "m_OrigX", g_gridTargetX);
    PlaceGrid(state);
    Log::Detailf("ModelViewer: the grid %s: %d a row, %d rows, at %.0f%% (its clip at %.1f, %.1f; the tiles from %.0f, %.0f on the "
                "1280 x 720 stage)", already ? "as before" : "made", wantColumns, kGridRowsOurs, scale * 100.0, g_gridTargetX,
                g_gridTargetY, left, top);
}

// ---- The grid's title, tabs and name (v0.9.432: the grid movie's own, no longer the overlay's) ----
// An empty clip of ours in the state's clip (createEmptyMovieClip at its getNextHighestDepth: above mask_mc's depths,
// 59 to 63), scaled back to the stage's units (the state's clip is at GridScale) and put at the stage's origin. So it
// fades in with the movie (OnEnter's tween of _root), slides with the grid (State_Base.AnimateIn / AnimateOut move the
// state's clip) and goes with the movie. In it the movie's own text fields (createTextField; html, embedFonts) in the
// game's font, "$DINFont_24" (what the game's movies import from gfxfontlib.swf, 226 of them: GFx's font map gives each
// text language its font, the font library has it; a text field's font is looked for there too), checked by the
// title's textWidth; and the drawing API's lines (as the movie's Mouse_Button draws its hit boxes). The mouse: the clip's
// _xmouse / _ymouse (the movie's own place of it) against the tabs' fields, each frame on the game thread; a click on a
// tab (the window's message, kept from the game) asks for that category, which the grid's Process takes.
constexpr const char* kHudClip = "fw_model_viewer_hud_mc";
constexpr const char* const kHudFonts[] = {"$DINFont_24", "DIN-Bold"};   // (the second: the face 89 fields name)
constexpr std::uint32_t kHudWhite = 0xFFFFFF, kHudHover = 0xE1E1E1, kHudGrey = 0x969696;
constexpr double kHudTitleSize = 26.0, kHudTabSize = 14.0, kHudNameSize = 17.0, kHudTabGap = 20.0;
constexpr double kHudTitleAbove = 88.0, kHudTabsAbove = 46.0, kHudLineAbove = 14.0, kHudNameBelow = 14.0;
constexpr double kHudGutter = 2.0;   // (a text field's own, each side: its text 2 in from its edges)
constexpr double kTileStep = 125.0, kRowStep = 180.0;   // (AddOutfitItem's, the state's clip's units)
constexpr double kStageWidth = 1280.0;
struct GridHud {
    bool made = false;
    const char* font = nullptr;
    std::vector<std::array<double, 4>> tabs;   // each tab's field: x, y, width, height (the stage's units)
    std::vector<double> tabText;               // its text's width
    int active = -2, hover = -2;
    int nameCategory = -2, nameIndex = -2;
    bool goneLogged = false;
    // (v0.9.438) under the grid on its right: ALL UPGRADES (the weapons'); (v0.9.445) the search right of the tabs
    bool upgradesMade = false, searchMade = false;
    std::array<double, 4> upgrades{}, search{};   // where a click takes them: x, y, width, height
    std::array<double, 2> checkAt{};              // the box's top left
    int upgradesDrawn = -1;                       // (0 off, 1 on, +2 under the mouse) as drawn
    int shownKind = -1;                           // the category's kind they were shown for
    std::wstring searchDrawn;
    int searchStateDrawn = -1;                    // (1 typing, +2 under the mouse) as drawn
    std::size_t searchFound = 0;
    // (v0.9.445) the name's fields' widths: the grid's (name_txt), and on the weapons the grid's less ALL UPGRADES'
    // (namew_txt); which one shows the name (0 / 1, -1: none yet)
    double nameWidthAll = 0.0, nameWidthWeapons = 0.0;
    int nameField = -1;
};
constexpr int kHudUpgradesId = 1000, kHudSearchId = 1001;   // (g_tabUnderMouse / g_tabClicked, past the tabs)
constexpr double kCheckSize = 12.0, kCheckGap = 8.0, kSearchWidth = 150.0, kSearchHeight = 22.0, kSlotGap = 16.0;
constexpr std::size_t kSearchMax = 40;
GridHud g_hud;
std::atomic<int> g_tabUnderMouse{-1};   // (game thread) the tab under the mouse; read by the window's thread
std::atomic<int> g_tabClicked{-1};      // (the window's thread) a tab clicked; the grid's Process takes it
std::atomic<bool> g_tabPressKept{false};

// CSCCF_Status_IsMouseKeyboard (retail 0x66A660) reads the flag InputDevice::Update keeps: mouse & keyboard in use (the
// hover shown only then: with a pad the cursor is hidden where it was left).
constexpr std::uintptr_t kRvaIsMouseKeyboardStatus = 0x0066A660ull, kRvaUsingMouseKeyboard = 0x00FB6F80ull;
constexpr std::uint8_t kIsMouseKeyboardStatusSig[] = {0x41, 0x8B, 0x10, 0x0F, 0xB6, 0x0D, 0x16, 0xC9, 0x94, 0x00};
int g_mouseKeyboardKnown = 0;   // 1 the reader as expected, -1 not
bool MouseKeyboardInUse() {
    if (!g_mouseKeyboardKnown)
        g_mouseKeyboardKnown = GameBuild::VerifyBytes(kRvaIsMouseKeyboardStatus, kIsMouseKeyboardStatusSig,
                                                      sizeof(kIsMouseKeyboardStatusSig)) ? 1 : -1;
    return g_mouseKeyboardKnown < 0 || *reinterpret_cast<const std::uint8_t*>(Fn(kRvaUsingMouseKeyboard)) != 0;
}

GfxValue* StateClip(void* state) { return reinterpret_cast<GfxValue*>(static_cast<std::uint8_t*>(state) + kStateClip); }
bool GfxIsObject(const GfxValue& v) { return v.iface && (v.type & 0x8F) >= 6; }
bool GfxGet(const GfxValue& object, const char* name, GfxValue& out) {
    if (!GfxIsObject(object)) return false;
    return reinterpret_cast<GetMemberFn>(Fn(kRvaObjGetMember))(object.iface, object.data, name, &out, (object.type & 0x8F) == 8);
}
bool GfxSet(const GfxValue& object, const char* name, const GfxValue& value) {
    if (!GfxIsObject(object)) return false;
    return reinterpret_cast<SetMemberFn>(Fn(kRvaObjSetMember))(object.iface, object.data, name, &value, (object.type & 0x8F) == 8);
}
bool GfxCall(const GfxValue& object, const char* fn, std::initializer_list<GfxValue> args, GfxValue* result = nullptr) {
    if (!GfxIsObject(object)) return false;
    return reinterpret_cast<ObjInvokeFn>(Fn(kRvaObjInvoke))(object.iface, object.data, result, fn,
                                                            args.size() ? args.begin() : nullptr, args.size(),
                                                            (object.type & 0x8F) == 8);
}
double GfxNumber(const GfxValue& object, const char* name) {
    GfxValue value;
    const double number = GfxGet(object, name, value) && (value.type & 0x8F) == 3 ? value.number : NAN;
    ReleaseValue(value);
    return number;
}
// A member that is an object (a clip, a text field), released by the caller.
bool GfxChild(const GfxValue& parent, const char* name, GfxValue& out) {
    if (GfxGet(parent, name, out) && GfxIsObject(out)) return true;
    ReleaseValue(out);
    return false;
}

// A text field of ours in the clip: the game's way (html, the font embedded, not selectable), as wide as its text
// (autoSize), or wrapped at its width.
bool HudField(const GfxValue& hud, const char* name, int depth, double x, double y, double width, bool wrap, GfxValue& field) {
    GfxValue made;
    GfxCall(hud, "createTextField", {Str(name), Num(depth), Num(x), Num(y), Num(width), Num(32.0)}, &made);
    ReleaseValue(made);   // (taken by its name: the clip's member)
    if (!GfxChild(hud, name, field)) return false;
    GfxSet(field, "embedFonts", Bool(true));
    GfxSet(field, "selectable", Bool(false));
    GfxSet(field, "html", Bool(true));
    GfxSet(field, "multiline", Bool(wrap));
    GfxSet(field, "wordWrap", Bool(wrap));
    GfxSet(field, "autoSize", Str("left"));
    return true;
}
void HudText(const GfxValue& field, const char* font, const std::string& text, double size, std::uint32_t color) {
    char open[160];
    std::snprintf(open, sizeof(open), "<font face=\"%s\" size=\"%ld\" color=\"#%06X\" kerning=\"1\">", font, std::lround(size),
                  unsigned(color));
    const std::string html = open + Html(text) + "</font>";
    GfxSet(field, "htmlText", Str(html.c_str()));
}
// (v0.9.443) The model's name in its field: lines broken between words only (the field broke a word wider than it,
// SPARROWHA / WK), its size smaller when its widest word is wider than the field. (v0.9.444) The words are measured in a
// hidden field of their own: 0.9.443 measured them in the name's, whose autoSize then made it as narrow as the last word
// (DE / SE / RT / TA).
void HudName(const GfxValue& hud, const GfxValue& field, const std::string& name, double width) {
    double widest = 0.0;
    GfxValue measure;
    if (GfxChild(hud, "namesize_txt", measure))
        for (std::size_t at = 0; at < name.size();) {
            const std::size_t end = std::min(name.find(' ', at), name.size());
            if (end > at) {
                HudText(measure, g_hud.font, name.substr(at, end - at), kHudNameSize, kHudWhite);
                const double w = GfxNumber(measure, "textWidth");
                if (std::isfinite(w)) widest = std::max(widest, w);
            }
            at = end + 1;
        }
    ReleaseValue(measure);
    const double room = width - 2.0 * kHudGutter - 4.0;
    const double size = widest > room && room > 0.0 ? std::max(kHudNameSize * 0.55, kHudNameSize * room / widest) : kHudNameSize;
    HudText(field, g_hud.font, name, size, kHudWhite);
}
// A filled box drawn in a clip (the drawing API).
void HudBox(const GfxValue& clip, double x0, double y0, double x1, double y1, std::uint32_t color, double alpha) {
    GfxCall(clip, "beginFill", {Num(double(color)), Num(alpha)});
    GfxCall(clip, "moveTo", {Num(x0), Num(y0)});
    GfxCall(clip, "lineTo", {Num(x1), Num(y0)});
    GfxCall(clip, "lineTo", {Num(x1), Num(y1)});
    GfxCall(clip, "lineTo", {Num(x0), Num(y1)});
    GfxCall(clip, "lineTo", {Num(x0), Num(y0)});
    GfxCall(clip, "endFill", {});
}
std::string HudTabName(int i) {
    char name[32];
    std::snprintf(name, sizeof(name), "tab%d_txt", i);
    return name;
}

void GridHudRemove(void* state) {
    GfxValue hud;
    if (state && GfxChild(*StateClip(state), kHudClip, hud)) GfxCall(hud, "removeMovieClip", {});
    ReleaseValue(hud);
    g_hud = GridHud{};
    g_tabUnderMouse = -1;
    g_tabClicked = -1;
}

// (game thread, v0.9.438) The slot under the grid: ALL UPGRADES on the weapons (its box ticked while on), the search
// (v0.9.440 on every category: its text, else SEARCH in grey; its box lighter under the mouse or while typing; no caret).
void GridSlotUpdate(const GfxValue& hud, ModelViewer::Kind kind, int hover) {
    const bool upgradesShown = g_hud.upgradesMade && kind == ModelViewer::Kind::Weapons;
    const bool searchShown = g_hud.searchMade;
    if (int(kind) != g_hud.shownKind) {
        g_hud.shownKind = int(kind);
        for (const char* name : {"upgrades_mc", "upgrades_txt"}) {
            GfxValue v;
            if (GfxChild(hud, name, v)) GfxSet(v, "_visible", Bool(upgradesShown));
            ReleaseValue(v);
        }
        for (const char* name : {"search_mc", "search_txt"}) {
            GfxValue v;
            if (GfxChild(hud, name, v)) GfxSet(v, "_visible", Bool(searchShown));
            ReleaseValue(v);
        }
    }
    if (upgradesShown) {
        const int drawn = (ModelViewer::AllUpgrades() ? 1 : 0) + (hover == kHudUpgradesId ? 2 : 0);
        if (drawn != g_hud.upgradesDrawn) {
            g_hud.upgradesDrawn = drawn;
            const std::uint32_t color = drawn & 2 ? kHudHover : drawn & 1 ? kHudWhite : kHudGrey;
            GfxValue box, label;
            if (GfxChild(hud, "upgrades_mc", box)) {
                GfxCall(box, "clear", {});
                const double x = g_hud.checkAt[0], y = g_hud.checkAt[1], e = kCheckSize;
                HudBox(box, x, y, x + e, y + 1.0, color, 100.0);   // (its outline)
                HudBox(box, x, y + e - 1.0, x + e, y + e, color, 100.0);
                HudBox(box, x, y, x + 1.0, y + e, color, 100.0);
                HudBox(box, x + e - 1.0, y, x + e, y + e, color, 100.0);
                if (drawn & 1) HudBox(box, x + 3.0, y + 3.0, x + e - 3.0, y + e - 3.0, color, 100.0);   // (ticked)
            }
            if (GfxChild(hud, "upgrades_txt", label)) GfxSet(label, "textColor", Num(double(color)));
            ReleaseValue(label);
            ReleaseValue(box);
        }
    }
    if (searchShown) {
        const bool typing = g_searchFocus.load();
        const int drawn = (typing ? 1 : 0) + (hover == kHudSearchId ? 2 : 0);
        if (drawn != g_hud.searchStateDrawn || g_gridSearch != g_hud.searchDrawn || g_gridTiles.size() != g_hud.searchFound) {
            const bool boxChanged = drawn != g_hud.searchStateDrawn;
            g_hud.searchStateDrawn = drawn;
            g_hud.searchDrawn = g_gridSearch;
            g_hud.searchFound = g_gridTiles.size();
            GfxValue box, field;
            if (boxChanged && GfxChild(hud, "search_mc", box)) {
                GfxCall(box, "clear", {});
                const auto& r = g_hud.search;
                const std::uint32_t color = drawn ? kHudWhite : kHudGrey;
                const double alpha = drawn ? 70.0 : 40.0;
                HudBox(box, r[0], r[1], r[0] + r[2], r[1] + r[3], 0x000000, 35.0);   // (its backdrop)
                HudBox(box, r[0], r[1], r[0] + r[2], r[1] + 1.0, color, alpha);
                HudBox(box, r[0], r[1] + r[3] - 1.0, r[0] + r[2], r[1] + r[3], color, alpha);
                HudBox(box, r[0], r[1], r[0] + 1.0, r[1] + r[3], color, alpha);
                HudBox(box, r[0] + r[2] - 1.0, r[1], r[0] + r[2], r[1] + r[3], color, alpha);
            }
            if (GfxChild(hud, "search_txt", field)) {
                // (the text's end shown when it is long: the field is as wide as its text)
                std::wstring shown = g_gridSearch;
                if (shown.size() > 12) shown = L"..." + shown.substr(shown.size() - 12);
                if (g_gridSearch.empty()) {   // (typing too: the box lighter says so)
                    HudText(field, g_hud.font, ViewerText::Upper(ViewerText::Search), kHudTabSize, kHudGrey);
                } else {   // (v0.9.443: without the count of the models found; grey when none)
                    HudText(field, g_hud.font, Utf8(shown), kHudTabSize, g_gridTiles.empty() ? kHudGrey : kHudWhite);
                }
            }
            ReleaseValue(field);
            ReleaseValue(box);
        }
    }
}

// (game thread) The tabs' colours (the category's white, underlined; the one under the mouse lighter), the model's name.
void GridHudUpdate(void* state) {
    if (!g_hud.made) return;
    GfxValue hud;
    if (!GfxChild(*StateClip(state), kHudClip, hud)) {
        if (!g_hud.goneLogged) Log::Write("ModelViewer: the grid's title, tabs and name are gone from its movie");
        g_hud.goneLogged = true;
        g_tabUnderMouse = -1;
        return;
    }
    int under = -1;
    const bool inputOn = !ModelViewer::OwnsInput() && (At<std::uint8_t>(state, kStateInputFlags) & 8);   // (on the grid)
    const ModelViewer::Kind kind = ModelViewer::CategoryKind(g_gridCategory);
    const bool upgradesShown = g_hud.upgradesMade && kind == ModelViewer::Kind::Weapons;
    const bool searchShown = g_hud.searchMade;
    if (inputOn) {
        const double x = GfxNumber(hud, "_xmouse"), y = GfxNumber(hud, "_ymouse");
        for (std::size_t i = 0; i < g_hud.tabs.size() && std::isfinite(x) && std::isfinite(y); ++i) {
            const auto& r = g_hud.tabs[i];
            if (x >= r[0] - 2.0 && x < r[0] + r[2] + 2.0 && y >= r[1] - 4.0 && y < r[1] + r[3] + 6.0) under = int(i);
        }
        const auto inside = [x, y](const std::array<double, 4>& r) {
            return std::isfinite(x) && std::isfinite(y) && x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3];
        };
        if (upgradesShown && inside(g_hud.upgrades)) under = kHudUpgradesId;
        if (searchShown && inside(g_hud.search)) under = kHudSearchId;
    }
    g_tabUnderMouse = under;
    g_searchHere = inputOn && searchShown;
    if (g_searchFocus.load() && !(inputOn && searchShown)) {   // (the grid left: no typing)
        g_searchFocus = false;
        g_searchLeftAt = GetTickCount64();
    }
    GridSlotUpdate(hud, kind, MouseKeyboardInUse() ? under : -1);
    const int hover = MouseKeyboardInUse() ? under : -1;
    const int active = g_gridCategory;
    if (active != g_hud.active || hover != g_hud.hover) {
        for (int i = 0; i < int(g_hud.tabs.size()); ++i) {
            GfxValue field;
            if (GfxChild(hud, HudTabName(i).c_str(), field))
                GfxSet(field, "textColor", Num(double(i == active ? kHudWhite : i == hover ? kHudHover : kHudGrey)));
            ReleaseValue(field);
        }
        GfxValue line;
        if (active != g_hud.active && GfxChild(hud, "underline_mc", line)) {
            GfxCall(line, "clear", {});
            if (active >= 0 && active < int(g_hud.tabs.size())) {
                const auto& r = g_hud.tabs[std::size_t(active)];
                const double x = r[0] + kHudGutter, y = r[1] + r[3] + 1.0;
                HudBox(line, x, y, x + g_hud.tabText[std::size_t(active)], y + 2.0, kHudWhite, 100.0);
            }
        }
        ReleaseValue(line);
        g_hud.active = active;
        g_hud.hover = hover;
    }
    int category = 0, index = 0;
    ModelViewer::Current(category, index);
    const int nameField = upgradesShown ? 1 : 0;   // (v0.9.445: the weapons' field leaves room for ALL UPGRADES)
    if (category != g_hud.nameCategory || index != g_hud.nameIndex || nameField != g_hud.nameField) {
        GfxValue field, other;
        if (nameField != g_hud.nameField && GfxChild(hud, nameField ? "name_txt" : "namew_txt", other))
            HudText(other, g_hud.font, "", kHudNameSize, kHudWhite);
        if (GfxChild(hud, nameField ? "namew_txt" : "name_txt", field))
            HudName(hud, field, ModelViewer::ModelLabel(category, index), nameField ? g_hud.nameWidthWeapons : g_hud.nameWidthAll);
        ReleaseValue(other);
        ReleaseValue(field);
        g_hud.nameCategory = category;
        g_hud.nameIndex = index;
        g_hud.nameField = nameField;
    }
    ReleaseValue(hud);
}

// (game thread, the grid entered once its layout is ours) MODEL VIEWER, the tabs, a line under them, the name's field.
void GridHudMake(void* state) {
    GridHudRemove(state);   // (one of before, when the movie stayed)
    if (std::isnan(g_gridTargetX) || !(g_gridTargetScale > 0.0)) return;   // (the game's grid: none)
    GfxValue* clip = StateClip(state);
    if (!GfxIsObject(*clip)) {
        Log::Write("ModelViewer: the grid's clip is not there: no title, tabs or name");
        return;
    }
    GfxValue next, made, hud;
    GfxCall(*clip, "getNextHighestDepth", {}, &next);
    const double depth = (next.type & 0x8F) == 3 && next.number >= 0.0 && next.number < 16384.0 ? next.number : 4096.0;
    ReleaseValue(next);
    GfxCall(*clip, "createEmptyMovieClip", {Str(kHudClip), Num(depth)}, &made);
    ReleaseValue(made);
    if (!GfxChild(*clip, kHudClip, hud)) {
        Log::Write("ModelViewer: the grid's movie made no clip for its title, tabs and name: none");
        return;
    }
    const double s = g_gridTargetScale / 100.0;
    GfxSet(hud, "_xscale", Num(100.0 / s));
    GfxSet(hud, "_yscale", Num(100.0 / s));
    GfxSet(hud, "_x", Num(-g_gridTargetX / s));
    GfxSet(hud, "_y", Num(-g_gridTargetY / s));
    const double left = ModelViewer::GridLeft(), top = ModelViewer::GridTop(), scale = ModelViewer::GridScale();
    const double gridWidth = kTileStep * kGridColumnsOurs * scale, gridHeight = kRowStep * kGridRowsOurs * scale;
    int at = 1;
    // MODEL VIEWER, and the font: the first of the game's whose text has a width
    GfxValue title;
    double titleWidth = 0.0;
    if (HudField(hud, "title_txt", at++, left - kHudGutter, top - kHudTitleAbove - kHudGutter, 600.0, false, title)) {
        const std::string text = ViewerText::Upper(ViewerText::ModelViewer);
        for (const char* font : kHudFonts) {
            HudText(title, font, text, kHudTitleSize, kHudWhite);
            titleWidth = GfxNumber(title, "textWidth");
            if (titleWidth > 0.0) {
                g_hud.font = font;
                break;
            }
        }
    }
    ReleaseValue(title);
    if (!g_hud.font) {
        GfxCall(hud, "removeMovieClip", {});
        ReleaseValue(hud);
        Log::Writef("ModelViewer: the grid's title has no width in the game's font (%s, %s: not found by the movie): no title, "
                    "tabs or name (LB / RB still change the category)", kHudFonts[0], kHudFonts[1]);
        return;
    }
    // the tabs: in a row, smaller when they would pass the stage's middle
    const int count = ModelViewer::CategoryCount();
    std::vector<std::string> names;
    for (int i = 0; i < count; ++i) names.push_back(ViewerText::ToUpper(ModelViewer::CategoryName(i)));
    double size = kHudTabSize, gap = kHudTabGap;
    // (v0.9.445: the search right of them, in their row)
    const double room = std::max(100.0, kStageWidth * 0.5 - left - kSearchWidth - kHudTabGap);
    const double tabsY = top - kHudTabsAbove - kHudGutter;
    for (int pass = 0; pass < 2; ++pass) {
        g_hud.tabs.clear();
        g_hud.tabText.clear();
        double x = left, total = 0.0;
        for (int i = 0; i < count; ++i) {
            GfxValue field;
            const std::string name = HudTabName(i);
            const bool got = pass == 0 ? HudField(hud, name.c_str(), at++, x - kHudGutter, tabsY, 300.0, false, field)
                                       : GfxChild(hud, name.c_str(), field);
            if (got) {
                GfxSet(field, "_x", Num(x - kHudGutter));
                HudText(field, g_hud.font, names[std::size_t(i)], size, kHudGrey);
            }
            const double textWidth = got ? GfxNumber(field, "textWidth") : NAN;
            const double width = got ? GfxNumber(field, "_width") : NAN, height = got ? GfxNumber(field, "_height") : NAN;
            ReleaseValue(field);
            const double w = std::isfinite(textWidth) ? textWidth : 0.0;
            g_hud.tabs.push_back({x - kHudGutter, tabsY, std::isfinite(width) ? width : w + 2.0 * kHudGutter,
                                  std::isfinite(height) ? height : size + 2.0 * kHudGutter});
            g_hud.tabText.push_back(w);
            total += w + (i + 1 < count ? gap : 0.0);
            x += w + gap;
        }
        if (pass == 0 && total > room) {
            const double f = std::max(0.6, room / total);
            size *= f;
            gap *= f;
            continue;
        }
        break;
    }
    // the line under the tabs, the active tab's underline, the model's name under the grid
    GfxValue line, underline, name;
    GfxCall(hud, "createEmptyMovieClip", {Str("line_mc"), Num(double(at++))}, &made);
    ReleaseValue(made);
    if (GfxChild(hud, "line_mc", line)) HudBox(line, left, top - kHudLineAbove, left + gridWidth, top - kHudLineAbove + 1.0, kHudWhite, 30.0);
    ReleaseValue(line);
    GfxCall(hud, "createEmptyMovieClip", {Str("underline_mc"), Num(double(at++))}, &made);
    ReleaseValue(made);
    const bool underlined = GfxChild(hud, "underline_mc", underline);
    ReleaseValue(underline);
    // (v0.9.438) under the grid on its right: ALL UPGRADES (the weapons': a box and its label); (v0.9.445) the search
    // (a box with its text in it, every category's) right of the tabs, in their row (it narrowed the name's field)
    const double slotY = top + gridHeight + kHudNameBelow, gridRight = left + gridWidth;
    g_hud.nameWidthAll = gridWidth + 2.0 * kHudGutter;
    g_hud.nameWidthWeapons = g_hud.nameWidthAll;
    g_hud.nameField = -1;
    if (ModelViewer::WeaponUpgradesAvailable()) {
        GfxValue label;
        GfxCall(hud, "createEmptyMovieClip", {Str("upgrades_mc"), Num(double(at++))}, &made);
        ReleaseValue(made);
        if (HudField(hud, "upgrades_txt", at++, 0.0, slotY - kHudGutter, 400.0, false, label)) {
            HudText(label, g_hud.font, ViewerText::Upper(ViewerText::AllUpgrades), kHudTabSize, kHudGrey);
            const double textWidth = GfxNumber(label, "textWidth"), fieldHeight = GfxNumber(label, "_height");
            const double w = std::isfinite(textWidth) && textWidth > 0.0 ? textWidth : 100.0;
            const double h = std::isfinite(fieldHeight) && fieldHeight > 0.0 ? fieldHeight : kHudTabSize + 2.0 * kHudGutter;
            const double total = kCheckSize + kCheckGap + w, x = gridRight - total;
            GfxSet(label, "_x", Num(x + kCheckSize + kCheckGap - kHudGutter));
            g_hud.checkAt = {x, slotY - kHudGutter + (h - kCheckSize) * 0.5};
            g_hud.upgrades = {x - 4.0, slotY - 4.0, total + 8.0, h + 4.0};
            g_hud.upgradesMade = true;
            g_hud.nameWidthWeapons = std::max(120.0, g_hud.nameWidthAll - total - kSlotGap);
        }
        ReleaseValue(label);
    }
    {
        GfxValue field;
        GfxCall(hud, "createEmptyMovieClip", {Str("search_mc"), Num(double(at++))}, &made);
        ReleaseValue(made);
        double tabsRight = left, tabsHeight = size + 2.0 * kHudGutter;
        if (!g_hud.tabs.empty()) {
            tabsRight = g_hud.tabs.back()[0] + kHudGutter + g_hud.tabText.back();
            tabsHeight = g_hud.tabs.back()[3];
        }
        const double x = tabsRight + gap, y = tabsY + (tabsHeight - kSearchHeight) * 0.5;
        if (HudField(hud, "search_txt", at++, x + 6.0 - kHudGutter, y + 1.0, kSearchWidth - 12.0, false, field)) {
            HudText(field, g_hud.font, ViewerText::Upper(ViewerText::Search), kHudTabSize, kHudGrey);
            g_hud.search = {x, y, kSearchWidth, kSearchHeight};
            g_hud.searchMade = true;
        }
        ReleaseValue(field);
    }
    const bool named = HudField(hud, "name_txt", at++, left - kHudGutter, slotY - kHudGutter, g_hud.nameWidthAll, true, name);
    ReleaseValue(name);
    {   // (v0.9.445) the weapons' name, beside ALL UPGRADES
        GfxValue weaponsName;
        HudField(hud, "namew_txt", at++, left - kHudGutter, slotY - kHudGutter, g_hud.nameWidthWeapons, true, weaponsName);
        ReleaseValue(weaponsName);
    }
    {   // (v0.9.444) the name's words measured here (HudName), never shown
        GfxValue measure;
        if (HudField(hud, "namesize_txt", at++, left - kHudGutter, slotY - kHudGutter, 10.0, false, measure))
            GfxSet(measure, "_visible", Bool(false));
        ReleaseValue(measure);
    }
    ReleaseValue(hud);
    g_hud.made = true;
    GridHudUpdate(state);
    Log::Writef("ModelViewer: the grid's title, tabs and name: the movie's own text fields in %s (the title %.0f wide; %d tabs at "
                "%.0f%% of their size%s%s%s%s; at depth %.0f of the grid's clip)", g_hud.font, titleWidth, count,
                size / kHudTabSize * 100.0, underlined ? "" : ", no underline", named ? "" : ", no name",
                g_hud.upgradesMade ? ", ALL UPGRADES" : "", g_hud.searchMade ? ", the search" : "", depth);
}

// (the window's thread, v0.9.438) The search's keys: Ctrl+F on the grid types in it; while typing every key
// is kept from the game (its characters, WM_CHAR, go to the search; Enter and Esc end the typing), and for 0.6 s after,
// the keys let go (the game did not see them go down).
constexpr ULONGLONG kSearchKeysKeptMs = 600;
bool SearchKeepsKeys() {
    return g_searchFocus.load() || GetTickCount64() - g_searchLeftAt.load() < kSearchKeysKeptMs;
}
bool GridSearchKey(UINT msg, WPARAM wParam) {
    const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN, up = msg == WM_KEYUP || msg == WM_SYSKEYUP;
    if (!g_searchFocus.load()) {
        if (msg == WM_KEYDOWN && wParam == 'F' && (GetKeyState(VK_CONTROL) & 0x8000) && g_searchHere.load()) {
            g_searchFocus = true;
            return true;
        }
        // (v0.9.443) V on the grid: the highlighted character's line (its press and release kept from the game)
        static bool lineKeyKept = false;
        if (wParam == 'V' && (down || up)) {
            if (down && g_searchHere.load() && !(GetKeyState(VK_CONTROL) & 0x8000)) {
                if (!lineKeyKept) ModelViewer::RequestLine();   // (not again while held: the key's repeats)
                lineKeyKept = true;
                return true;
            }
            if (up && lineKeyKept) {
                lineKeyKept = false;
                return true;
            }
        }
        return up && GetTickCount64() - g_searchLeftAt.load() < kSearchKeysKeptMs;
    }
    if (msg == WM_CHAR) {
        {
            std::lock_guard lock(g_searchMutex);
            if (g_searchEdits.size() < 256) g_searchEdits.push_back(wchar_t(wParam));
        }
        if (wParam == L'\r' || wParam == 27) {
            g_searchFocus = false;
            g_searchLeftAt = GetTickCount64();
        }
        return true;
    }
    return down || up;
}

// (the window's thread) A press on a tab: its category asked for; the press and its release kept from the game.
bool GridTabMessage(UINT msg) {
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
        const int tab = g_tabUnderMouse.load();
        if (tab != kHudSearchId && g_searchFocus.exchange(false)) g_searchLeftAt = GetTickCount64();   // (a click elsewhere)
        if (tab < 0 || !ModelViewer::IsOpen() || ModelViewer::OwnsInput()) return false;
        g_tabClicked = tab;
        g_tabPressKept = true;
        return true;
    }
    return msg == WM_LBUTTONUP && g_tabPressKept.exchange(false);
}

// Back from the viewer: UiMenuStateManager::ChangeState (retail 0x8AEED0, TR2 0x8F0A20) calls the state left's OnExit
// only when the next state has +0xD5 bit 1 (a full screen) and the next state's OnEnter only when the state left has
// it: the relic viewer has not, so on its way back the grid is not entered again (and on the way there it was not left:
// its movie and its Process flag, +0xD6 bit 2, stay). The game's viewer calls the list it came from back from its
// OnExit (TR2 0x140956DE9); here the viewer's OnExit calls this.
bool g_modelGridReturned = true;
void ModelGridReturn(void* state) {
    if (g_modelGridReturned) return;
    g_modelGridReturned = true;
    ModelViewer::SetScreen(ModelViewer::Screen::Grid);
    GridLegend(state);
    Movie(state, "AnimateIn", {Num(0.01)});
    PlaceGrid(state);
    ShowWidget(GridWidget(state, kGearOutfitInfo), false, false);   // (v0.9.496: hidden, as on entering)
    int category = 0, index = 0;
    ModelViewer::Current(category, index);   // (the view may have gone to another model, another category)
    if (category != g_gridCategory) GridFill(state, category);
    At<std::int32_t>(state, kStateSelected) = -1;
    GridSelectModel(state, index);
    Log::Write("ModelViewer: back on the grid");
}

bool __fastcall GridOnEnter(void* state, void* prev, void* cur) {
    const int from = prev ? *static_cast<const int*>(prev) : -1;
    if (from == kStateModelView) {   // (not called by the game: see ModelGridReturn)
        ModelGridReturn(state);
        return true;
    }
    At<std::int32_t>(state, kGearCategory) = kGearOutfitsCategory;   // (the outfits' grid: SetupInformationWidget)
    const bool entered = reinterpret_cast<bool(__fastcall*)(void*, void*, void*)>(Fn(kRvaGenericOnEnter))(state, prev, cur);
    if (!entered) return entered;
    // (+0x110, the parent the information widgets are attached to (0x9766C0 keeps it at the widget's +0x150), is the
    // campsite menu's movie: UiMenuState 0x52 +0x180, set by the gear's own OnEnter at 0x907786, which is not called
    // here: at the main menu there is none, and they are attached to none)
    // the information widgets made for this movie (IDs from another visit may name widgets gone with their level)
    At<std::uint32_t>(state, kGearWeaponInfo) = At<std::uint32_t>(state, kGearOutfitInfo) = 0xFFFFFFFFu;
    reinterpret_cast<void(__fastcall*)(void*)>(Fn(kRvaGearSetupInfo))(state);   // (the information widgets, the outfit grid)
    if (!GridWidget(state, kGearOutfitInfo)) Log::Write("ModelViewer: the grid's information widget was not made");
    ShowWidget(GridWidget(state, kGearWeaponInfo), false, false);
    // (SetupInformationWidget shows the outfit one unless the menu manager's +0xA28 is 0x5A, 0x94BD50. v0.9.496: hidden:
    // since 0.9.422 the model stands on the right, under it, and its name is the HUD's under the grid)
    ShowWidget(GridWidget(state, kGearOutfitInfo), false, false);
    GridLegend(state);
    GridLayout(state);   // (before its tiles: they are put a row's count a row, the scroll bar sized as they come)
    int category = 0, index = 0;
    ModelViewer::Current(category, index);
    g_gridSearch.clear();   // (each visit without a search)
    g_searchFocus = false;
    GridFill(state, category);
    GridSelectModel(state, index);
    ModelViewer::SetScreen(ModelViewer::Screen::Grid);
    GridHudMake(state);   // (its title, tabs and name: once the layout is ours)
    g_modelGridEnteredAt = Now();
    g_modelGridLogged = false;
    Log::Writef("ModelViewer: the grid (%d categories; %s: %zu models)", ModelViewer::CategoryCount(),
                ModelViewer::CategoryName(category).c_str(), g_gridTiles.size());
    return entered;
}

// (v0.9.438) The search: what was typed since the last frame (Backspace one character off, Enter the typing
// ended, Esc the search emptied and the typing ended); the tiles made again with the models it finds, the model shown
// kept when it is one of them (else the first).
void GridSearchTick(void* state) {
    std::wstring edits;
    {
        std::lock_guard lock(g_searchMutex);
        edits.swap(g_searchEdits);
    }
    if (edits.empty()) return;
    const std::wstring before = g_gridSearch;
    for (const wchar_t c : edits) {
        if (c == L'\b') {
            if (!g_gridSearch.empty()) g_gridSearch.pop_back();
        } else if (c == 27) {
            g_gridSearch.clear();
        } else if (c >= 32 && c != 127 && (c < 0xD800 || c > 0xDFFF) && g_gridSearch.size() < kSearchMax) {
            g_gridSearch.push_back(c);
        }
    }
    if (g_gridSearch == before) return;
    int category = 0, index = 0;
    ModelViewer::Current(category, index);
    GridFill(state, g_gridCategory);
    const int tile = ModelTile(index);
    if (!g_gridTiles.empty()) GridSelect(state, tile >= 0 ? tile : 0, false);
    Log::Detailf("ModelViewer: the search \"%s\" in %s: %zu of %d", Utf8(g_gridSearch).c_str(),
                 ModelViewer::CategoryName(g_gridCategory).c_str(), g_gridTiles.size(), ModelViewer::ModelCount(g_gridCategory));
}

// (v0.9.602) The Model Viewer's tick (run in the menu state's Process, inside the game's own tick), into the 30 s
// FrameTime summary's sections.
void TimedModelViewerTick() {
    LARGE_INTEGER start, end, frequency;
    QueryPerformanceCounter(&start);
    ModelViewer::TickGameThread();
    QueryPerformanceCounter(&end);
    QueryPerformanceFrequency(&frequency);
    Features::AddFrameSection("ModelViewer", double(end.QuadPart - start.QuadPart) * 1000.0 / double(frequency.QuadPart));
}

void __fastcall GridProcess(void* state, float) {
    TimedModelViewerTick();
    if (const int tab = g_tabClicked.exchange(-1); tab >= 0 && (At<std::uint8_t>(state, kStateInputFlags) & 8)) {
        if (tab == kHudUpgradesId) {   // (ALL UPGRADES clicked: every weapon with all its upgrades, or none)
            ModelViewer::SetAllUpgrades(!ModelViewer::AllUpgrades());
            PlaySoundId(state, kMoveSound);
        } else if (tab == kHudSearchId) {   // (the search clicked: typing in it)
            g_searchFocus = true;
        } else {
            GridCategoryJump(state, tab);   // (a tab clicked)
        }
    }
    GridSearchTick(state);
    if (Now() - g_gridPlacedAt >= 0.25) PlaceGrid(state);
    GridHudUpdate(state);
    PumpPictures("");
    GridPictures(state);
    if (ModelViewer::Status() != g_gridStatus) GridTexts(state, false);
    ShowWidget(GridWidget(state, kGearOutfitInfo), false, false);   // (v0.9.496: kept hidden should the movie show it)
    if (!g_modelGridLogged && Now() - g_modelGridEnteredAt > 1.0) {   // (where the grid stands: the model's place beside it)
        g_modelGridLogged = true;
        int shown = 0, game = 0;
        for (std::size_t i = 0; i < g_gridTiles.size(); ++i) {
            shown += g_gridTiles[i].sent;
            const int m = g_gridTiles[i].model;
            game += m >= 0 && m < int(g_gridPictures.size()) && g_gridPictures[std::size_t(m)].request >= 0;
        }
        void* info = GridWidget(state, kGearOutfitInfo);
        const char* infoState = !info ? "not made"
                                : reinterpret_cast<bool(__fastcall*)(void*)>(Fn(kRvaWidgetIsVisible))(info) ? "shown" : "hidden";
        Log::Writef("ModelViewer: the grid's clip at x %.0f, y %.0f, %.0f x %.0f, alpha %.0f; %d of %zu tiles with a picture after "
                    "1 s (%d of the game's asked for); the information widget %s", ClipNumber(state, "_x"),
                    ClipNumber(state, "_y"), ClipNumber(state, "_width"), ClipNumber(state, "_height"),
                    ClipNumber(state, "_alpha"), shown, g_gridTiles.size(), game, infoState);
    }
}

void __fastcall GridPreExit(void* state, void* current, void* next, float time) {
    const int to = next ? *static_cast<const int*>(next) : -1;
    reinterpret_cast<void(__fastcall*)(void*, void*, void*, float)>(Fn(kRvaGenericOnPreExit))(state, current, next, time);
    const bool view = to == kStateModelView;
    g_tabUnderMouse = -1;
    if (g_searchFocus.exchange(false)) g_searchLeftAt = GetTickCount64();
    g_searchHere = false;
    if (!view) GridHudRemove(state);   // (to the view: they slide out with the grid, AnimateOut)
    for (const std::size_t at : {kGearWeaponInfo, kGearOutfitInfo}) {
        ShowWidget(GridWidget(state, at), false, !view);
        if (!view) At<std::uint32_t>(state, at) = 0xFFFFFFFFu;
    }
    Movie(state, "AnimateOut", {Num(0.01)});
}

bool __fastcall GridOnExit(void* state, void* current, void* next) {
    const int to = next ? *static_cast<const int*>(next) : -1;
    const bool result = reinterpret_cast<bool(__fastcall*)(void*, void*, void*)>(Fn(kRvaGenericOnExit))(state, current, next);
    if (to != kStateModelView) {
        RestoreModelGrid();
        g_hud = GridHud{};   // (gone with the movie)
        g_tabUnderMouse = -1;
        ModelViewer::Close();
    }
    return result;
}

// -- The view --
// Its text box (collectible_object_viewer's Object_Viewer_MC, the state's clip: veil_large and veil_small, its
// backdrops, the unnamed glow placed at depth 5 (blend add), desc the text) shown or not (v0.9.431: none for the
// characters and the animals, the game has no description of theirs).
constexpr double kViewGlowDepth = 5.0 - 16384.0;
bool SetMemberBool(void* state, const char* member, const char* name, bool value) {
    auto* clip = reinterpret_cast<GfxValue*>(static_cast<std::uint8_t*>(state) + kStateClip);
    if (!clip->iface || (clip->type & 0x8F) < 6) return false;
    GfxValue child;
    if (!reinterpret_cast<GetMemberFn>(Fn(kRvaObjGetMember))(clip->iface, clip->data, member, &child, (clip->type & 0x8F) == 8))
        return false;
    bool ok = false;
    if (child.iface && (child.type & 0x8F) >= 6) {
        const GfxValue v = Bool(value);
        ok = reinterpret_cast<SetMemberFn>(Fn(kRvaObjSetMember))(child.iface, child.data, name, &v, (child.type & 0x8F) == 8);
    }
    ReleaseValue(child);
    return ok;
}
bool g_viewBoxShown = true;
void ShowViewTextBox(void* state, bool shown) {
    int set = 0;
    for (const char* member : {"veil_large", "veil_small", "desc"}) set += SetMemberBool(state, member, "_visible", shown);
    for (const double at : {kViewGlowDepth, kViewGlowDepth + 16384.0}) {   // (the placed depth as it is: a fallback)
        GfxValue glow;
        GfxValue depth = Num(at);
        g_invokeStateMovie(state, "getInstanceAtDepth", &glow, &depth, 1);
        const unsigned kind = glow.type & 0x8F;
        const bool found = glow.iface && (kind == 6 || kind == 8);
        if (found) {
            const GfxValue v = Bool(shown);
            set += reinterpret_cast<SetMemberFn>(Fn(kRvaObjSetMember))(glow.iface, glow.data, "_visible", &v, kind == 8);
        }
        ReleaseValue(glow);
        if (found) break;
    }
    if (shown != g_viewBoxShown || set != 4)
        Log::Detailf("ModelViewer: the view's text box %s (%d of its 4 clips set)", shown ? "shown" : "hidden", set);
    g_viewBoxShown = shown;
}

void ModelViewTexts(void* state, bool show) {
    int category = 0, index = 0;
    ModelViewer::Current(category, index);
    if (show) {
        char sub[160];
        snprintf(sub, sizeof(sub), "%s   %d / %d", ViewerText::ToUpper(ModelViewer::CategoryName(category)).c_str(), index + 1,
                 ModelViewer::ModelCount(category));
        g_viewSub = sub;
        g_viewName = ModelViewer::ModelLabel(category, index);
        Movie(state, "ShowObjectViewer", {Str(ViewerText::Upper(ViewerText::ModelViewer)), Str(g_viewSub.c_str())});
        g_viewLoadedSent = false;
    }
    g_viewStatus = ModelViewer::Status();
    const ModelViewer::Kind kind = ModelViewer::CategoryKind(category);
    const bool box = kind != ModelViewer::Kind::Characters && kind != ModelViewer::Kind::Animals;
    // (the characters and the animals: no box at all, not even the viewer's status; the log has it)
    g_viewText = box ? ModelViewer::ModelDescription(category, index) : std::string();
    if (box && !g_viewStatus.empty()) g_viewText += (g_viewText.empty() ? "" : "\n\n") + g_viewStatus;
    Movie(state, "SetHeadlineAndDescriptionText", {Str(g_viewName.c_str()), Str(g_viewText.c_str())});
    ShowViewTextBox(state, !g_viewText.empty());
}

// H on the view (v0.9.436): its UI hidden, or shown again: the state's clip (collectible_object_viewer's
// Object_Viewer_MC: the title, the category and count, the text box) and the context bar (a widget of its own,
// UiMenuStateBase::GetContextbar: the buttons' legend). Shown again on each entry to the view.
bool g_viewUiHidden = false;
void ModelViewLegend(void* state) {
    AddLegend(state, {{kActRS, "Menu_Reset", 3}, {kActRStick, "Menu_RotateLabel", 2}, {kActLT, "StatusMenu_ZoomOut", 2},
                      {kActRT, "StatusMenu_ZoomIn", 2}, {kActMenuClose, "Menu_CloseLabel", 3}});
}
// The clip's _visible as wanted; true when it was set.
bool ApplyViewUi(void* state) { return GfxSet(*StateClip(state), "_visible", Bool(!g_viewUiHidden)); }
void SetViewUiHidden(void* state, bool hidden) {
    g_viewUiHidden = hidden;
    const bool clip = ApplyViewUi(state);
    void* bar = reinterpret_cast<void*(__fastcall*)(void*)>(Fn(kRvaGetContextbar))(state);
    ShowWidget(bar, !hidden, false);
    Log::Writef("ModelViewer: the view's UI %s (H; its clip %s, its legend %s)", hidden ? "hidden" : "shown",
                clip ? "set" : "not set", bar ? "set" : "not there");
}

bool __fastcall ModelViewOnEnter(void* state, void* prev, void* cur) {
    const bool entered = reinterpret_cast<bool(__fastcall*)(void*, void*, void*)>(Fn(kRvaGenericOnEnter))(state, prev, cur);
    if (!entered) return entered;
    ModelViewer::SetScreen(ModelViewer::Screen::View);
    g_viewUiHidden = false;
    ModelViewLegend(state);
    g_viewSerial = ModelViewer::ChoiceSerial();
    ModelViewTexts(state, true);
    int category = 0, index = 0;
    ModelViewer::Current(category, index);
    Log::Writef("ModelViewer: the relic viewer on %s", ModelViewer::ModelObject(category, index).c_str());
    return entered;
}

// (the button bar's clicks come here as their actions: Back, Reset, v0.9.422)
void __fastcall ModelViewPressed(void* state, int action, char) {
    if (action == kActCancel || action == kActMenuClose || action == kActMenuSelect)
        if (ModelBackAllowed()) ModelGoBack(state);
    if (action == kActRS) ModelViewer::RequestReset();
}

void __fastcall ModelViewProcess(void* state, float) {
    TimedModelViewerTick();
    if (ModelViewer::TakeBack()) {
        if (ModelBackAllowed()) ModelGoBack(state);
        return;
    }
    if (ModelViewer::TakeUiHideToggle()) SetViewUiHidden(state, !g_viewUiHidden);
    if (const unsigned serial = ModelViewer::ChoiceSerial(); serial != g_viewSerial) {   // (the view's own keys)
        g_viewSerial = serial;
        ModelViewTexts(state, true);
    } else if (ModelViewer::Status() != g_viewStatus) {
        ModelViewTexts(state, false);
    }
    if (g_viewUiHidden) ApplyViewUi(state);   // (kept hidden: the movie's own calls do not show it again)
    if (!g_viewLoadedSent && !ModelViewer::ModelLoading()) {
        g_viewLoadedSent = true;
        Movie(state, "ObjectLoaded");
    }
}

void __fastcall ModelViewPreExit(void* state, void* current, void* next, float time) {
    if (g_viewUiHidden) SetViewUiHidden(state, false);   // (the context bar is the next state's too)
    reinterpret_cast<void(__fastcall*)(void*, void*, void*, float)>(Fn(kRvaGenericOnPreExit))(state, current, next, time);
}

bool __fastcall ModelViewOnExit(void* state, void* current, void* next) {
    const int to = next ? *static_cast<const int*>(next) : -1;
    const bool result = reinterpret_cast<bool(__fastcall*)(void*, void*, void*)>(Fn(kRvaGenericOnExit))(state, current, next);
    if (!result) return result;   // (ChangeState then stays: UiMenuStateGeneric::OnExit refuses for a next state other
                                  // than the previous one)
    RestoreModelView();
    if (to == kStateModelGrid && g_modelGridState && g_modelGridMode.load()) {
        ModelGridReturn(g_modelGridState);
    } else {   // (not back to the grid: the grid goes too)
        RestoreModelGrid();
        ModelViewer::Close();
    }
    return result;
}

bool OpenModelView(void* state) {
    if (g_modelViewMode.load() || g_gridTiles.empty()) return false;
    void* view = StateObject(kStateModelView);
    if (!view || !StateManager()) return false;
    std::uintptr_t* vtable = VtableOf(view);
    if (vtable != &g_modelViewStore[1]) {
        if (reinterpret_cast<std::uintptr_t>(vtable) != Fn(kRvaModelViewVtable)) {
            Log::Write("ModelViewer: the relic viewer's object is not the expected one; not opened");
            return false;
        }
        g_modelViewVtable = vtable;
        g_modelViewStore[0] = vtable[-1];
        std::uintptr_t* clone = &g_modelViewStore[1];
        std::memcpy(clone, vtable, kModelViewEntries * sizeof(std::uintptr_t));
        clone[0x10 / 8] = Fn(kRvaGenericOnEnterAsync);
        clone[0x18 / 8] = reinterpret_cast<std::uintptr_t>(&ModelViewOnEnter);
        clone[0x20 / 8] = reinterpret_cast<std::uintptr_t>(&ModelViewPressed);
        clone[0x28 / 8] = Fn(kRvaNop);
        clone[0x30 / 8] = reinterpret_cast<std::uintptr_t>(&ModelViewProcess);
        clone[0x38 / 8] = reinterpret_cast<std::uintptr_t>(&ModelViewPreExit);
        clone[0x40 / 8] = reinterpret_cast<std::uintptr_t>(&ModelViewOnExit);
        *reinterpret_cast<std::uintptr_t**>(view) = clone;
    }
    g_modelViewState = view;
    g_modelViewMode = true;
    g_modelGridReturned = false;
    PlaySoundId(state, kSelectSound);
    if (!SetMenuState(kStateModelView, -1.0f)) {
        RestoreModelView();
        g_modelGridReturned = true;
        Log::Write("ModelViewer: the game refused to open the relic viewer");
        return false;
    }
    return true;
}

// The gear screen's state as the Model Viewer's grid (its movies there).
bool EnterModelGrid() {
    void* grid = StateObject(kStateModelGrid);
    if (!grid || !StateManager()) return false;
    std::uintptr_t* vtable = VtableOf(grid);
    if (vtable != &g_modelGridStore[1] && reinterpret_cast<std::uintptr_t>(vtable) != Fn(kRvaModelGridVtable)) {
        Log::Write("ModelViewer: the campsite gear screen's object is not the expected one; not opened");
        return false;
    }
    if (!ModelViewer::Open()) return false;
    if (vtable != &g_modelGridStore[1]) {
        g_modelGridVtable = vtable;
        g_modelGridStore[0] = vtable[-1];
        std::uintptr_t* clone = &g_modelGridStore[1];
        std::memcpy(clone, vtable, kModelGridEntries * sizeof(std::uintptr_t));
        clone[0x08 / 8] = Fn(kRvaNop);   // (OnUiSelect: the outfit worn)
        clone[0x18 / 8] = reinterpret_cast<std::uintptr_t>(&GridOnEnter);
        clone[0x20 / 8] = reinterpret_cast<std::uintptr_t>(&GridPressed);
        clone[0x28 / 8] = reinterpret_cast<std::uintptr_t>(&GridOn);
        clone[0x30 / 8] = reinterpret_cast<std::uintptr_t>(&GridProcess);
        clone[0x38 / 8] = reinterpret_cast<std::uintptr_t>(&GridPreExit);
        clone[0x40 / 8] = reinterpret_cast<std::uintptr_t>(&GridOnExit);
        clone[0x80 / 8] = Fn(kRvaGenericSetSelection);
        clone[0x88 / 8] = reinterpret_cast<std::uintptr_t>(&GridHighlight);
        clone[0xA0 / 8] = reinterpret_cast<std::uintptr_t>(&GridMouseDown);
        clone[0xB0 / 8] = reinterpret_cast<std::uintptr_t>(&GridMouseOver);
        clone[0xB8 / 8] = Fn(kRvaNop);   // (OnMouseOut)
        for (std::size_t slot = 0xF0; slot < kModelGridEntries * 8; slot += 8)   // (the campsite's own work)
            clone[slot / 8] = reinterpret_cast<std::uintptr_t>(&ReturnZero);
        *reinterpret_cast<std::uintptr_t**>(grid) = clone;
        g_gearCategoryBefore = At<std::int32_t>(grid, kGearCategory);
    }
    g_modelGridState = grid;
    g_modelGridMode = true;
    if (!SetMenuState(kStateModelGrid, 0.25f)) {
        RestoreModelGrid();
        ModelViewer::Close();
        Log::Write("ModelViewer: the game refused to open the grid");
        return false;
    }
    return true;
}

// The next need not met asked for; false: none to ask (all met: met is true then).
bool AskNextNeed(bool& met) {
    met = false;
    while (g_modelUi.need < g_modelUi.needs && NeedMet(g_modelUi.need)) ++g_modelUi.need;
    if (g_modelUi.need >= g_modelUi.needs) {
        met = true;
        return false;
    }
    PrepareNeed(g_modelUi.need);
    g_modelUi.askedAt = Now();
    return AskNextCollection();
}

// MODEL VIEWER (the Extras menu): the grid at once when its movies are there, else once the collections are.
void OpenModels() {
    if (g_modelUi.waiting || g_modelGridMode.load()) return;
    g_modelUi.target = 0;
    g_modelUi.needs = kNeeds;
    g_modelUi.need = 0;
    g_modelUi.openedAt = Now();
    bool met = false;
    g_modelUi.waiting = AskNextNeed(met);
    if (met) {
        if (!EnterModelGrid()) Log::Write("ModelViewer: not opened (see above)");
        return;
    }
    if (!g_modelUi.waiting) Log::Writef("ModelViewer: %s could not be asked for; not opened", kNeedNames[g_modelUi.need]);
}

void ListCollectionsReady();   // (v0.9.497: the Cutscene Viewer's list, below)

// (the Extras menu's Process) waiting for the screens' movies
void PumpModelUi() {
    if (!g_modelUi.waiting) return;
    const double now = Now();
    if (NeedMet(g_modelUi.need)) {
        Log::Detailf("ModelViewer: %s is there (collection %u, %.1f s)", kNeedNames[g_modelUi.need], unsigned(g_modelUi.asked),
                    now - g_modelUi.openedAt);
        bool met = false;
        g_modelUi.waiting = AskNextNeed(met);
        if (met && g_modelUi.target == 1) ListCollectionsReady();
        else if (met && !EnterModelGrid()) Log::Write("ModelViewer: not opened (see above)");
        if (!met && !g_modelUi.waiting) Log::Writef("ModelViewer: %s could not be asked for; not opened", kNeedNames[g_modelUi.need]);
        if (!met && !g_modelUi.waiting && g_modelUi.target == 1) OpenComboViewer();   // (v0.9.497: the Cutscene Viewer anyway)
        return;
    }
    const int entry = CollectionEntry(g_modelUi.asked);
    const bool loadedWithout = entry == 1, gone = entry < 0, tooLong = now - g_modelUi.askedAt > 20.0;
    if (!loadedWithout && !gone && !tooLong) return;
    Log::Writef("ModelViewer: the menu collection %u %s", unsigned(g_modelUi.asked),
                loadedWithout ? "came without the movies needed" : gone ? "is not in the manager" : "did not come in 20 s");
    if (!gone) {
        UnloadCollection(g_modelUi.asked);
        g_modelUi.loaded.erase(std::remove(g_modelUi.loaded.begin(), g_modelUi.loaded.end(), g_modelUi.asked),
                               g_modelUi.loaded.end());
    }
    g_modelUi.waiting = AskNextCollection();
    if (!g_modelUi.waiting) Log::Writef("ModelViewer: %s was in none of the collections tried; not opened", kNeedNames[g_modelUi.need]);
    if (!g_modelUi.waiting && g_modelUi.target == 1) OpenComboViewer();   // (v0.9.497: the Cutscene Viewer anyway)
}

void CancelModelUi() {
    if (!g_modelUi.waiting) return;
    g_modelUi.waiting = false;
    Log::Write("ModelViewer: the wait for the screens' movies cancelled (Back)");
}

bool CheckModelScreens() {
    for (const Sig& s : kModelSigs)
        if (!GameBuild::VerifyBytes(s.rva, s.bytes, sizeof(s.bytes))) {
            Log::Writef("ModelViewer: the screens' function at 0x%llX does not match; no Model Viewer",
                        static_cast<unsigned long long>(s.rva));
            return false;
        }
    if (!GameBuild::VerifyBytes(kRvaPlayingCollections, kPlayingCollectionsSig, sizeof(kPlayingCollectionsSig)) ||
        !GameBuild::VerifyBytes(kRvaWidgetMovieDataCall, kWidgetMovieDataCallSig, sizeof(kWidgetMovieDataCallSig)) ||
        !GameBuild::VerifyBytes(kRvaObjectNamesRead, kObjectNamesReadSig, sizeof(kObjectNamesReadSig)) ||
        !GameBuild::VerifyBytes(kRvaImageLoaderRead, kImageLoaderReadSig, sizeof(kImageLoaderReadSig)) ||
        !GameBuild::VerifyBytes(0x00954274ull, kUnloadImageCallSig, sizeof(kUnloadImageCallSig)) ||
        !GameBuild::VerifyBytes(kRvaSendOutfitName + 0x4D, kSendOutfitNameText, sizeof(kSendOutfitNameText)) ||
        !GameBuild::VerifyBytes(kRvaSendOutfitDescription + 0x4D, kSendOutfitDescriptionText, sizeof(kSendOutfitDescriptionText))) {
        Log::Write("ModelViewer: the menu collections', the pictures' or the widget's code is not the code expected; no Model Viewer");
        return false;
    }
    const auto* grid = reinterpret_cast<const std::uintptr_t*>(Fn(kRvaModelGridVtable));
    const auto* view = reinterpret_cast<const std::uintptr_t*>(Fn(kRvaModelViewVtable));
    const auto* types = reinterpret_cast<const std::int32_t*>(Fn(kRvaGearCategoryTypes));
    if (!(grid[1] == Fn(kRvaGearOnUiSelect) && grid[2] == Fn(kRvaGenericOnEnterAsync) && grid[3] == Fn(kRvaGearOnEnter) &&
          grid[4] == Fn(kRvaGearPressed) && grid[6] == Fn(kRvaGearProcess) && grid[8] == Fn(kRvaGearOnExit) &&
          grid[16] == Fn(kRvaSkillsSetSelection) && grid[17] == Fn(kRvaGearHighlight) && grid[37] == Fn(kRvaGearSetupInfo) &&
          grid[44] == Fn(kRvaNop) && types[kGearOutfitsCategory] == 14 && view[0x18 / 8] == Fn(kRvaViewOwnOnEnter) &&
          view[0x30 / 8] == Fn(kRvaViewOwnProcess) && view[0x80 / 8] == Fn(kRvaGenericSetSelection) &&
          view[0x88 / 8] == Fn(kRvaGenericPushHighlight))) {
        Log::Write("ModelViewer: the campsite gear screen's or the relic viewer's vtable does not match; no Model Viewer");
        return false;
    }
    const auto rip = [](std::uintptr_t rva, std::size_t length) {   // (disp32 at +3)
        return Fn(rva) + length + std::intptr_t(*reinterpret_cast<const std::int32_t*>(Fn(rva) + 3));
    };
    g_netSessionGlobal = rip(kRvaPlayingCollections + 0x00, 7);     // cmp [rip + session], rsi
    g_collectionManagerGlobal = rip(kRvaPlayingCollections + 0x0E, 7);   // mov rcx, [rip + manager]
    g_globalDataGlobal = rip(kRvaPlayingCollections + 0x23, 7);     // mov rax, [rip + global data]
    g_gameModeTypeFn = Fn(kRvaPlayingCollections + 0x09) + 5 +
                       std::intptr_t(*reinterpret_cast<const std::int32_t*>(Fn(kRvaPlayingCollections + 0x0A)));
    g_objectManagerGlobal = rip(kRvaObjectNamesRead, 7);           // mov rdx, [rip + ObjectManager]
    g_imageLoaderGlobal = rip(kRvaImageLoaderRead, 7);             // mov rcx, [rip + UiImageLoaderManager]
    g_imageCallbackVtable[0] = reinterpret_cast<std::uintptr_t>(&ImageCallbackNothing);
    g_imageCallbackVtable[1] = reinterpret_cast<std::uintptr_t>(&ImageCallbackLoaded);
    g_imageCallbackVtable[2] = g_imageCallbackVtable[3] = reinterpret_cast<std::uintptr_t>(&ImageCallbackNothing);
    return true;
}

// ---- The Extras menu ----
// Its title label: "EXTRAS" (or the Croft Manor menu's own text back).
bool SetExtrasTitle(void* state, bool ours) {
    for (const double at : {kExtrasTitleDepth, kExtrasTitleDepth + 16384.0}) {   // (the placed depth as it is: a fallback)
        GfxValue label;
        GfxValue depth = Num(at);
        g_invokeStateMovie(state, "getInstanceAtDepth", &label, &depth, 1);
        const unsigned kind = label.type & 0x8F;
        const bool found = label.iface && (kind == 6 || kind == 8);
        if (found) {
            ObjectCall(label, "SetByCodeUsage", {});
            if (ours) ObjectCall(label, "SetText", {Str(ViewerText::Get(ViewerText::Extras)), Bool(false)});
            else ObjectCall(label, "SetText", {Str("$MultiMenu_CroftManor"), Bool(true)});
        }
        ReleaseValue(label);
        if (found) return true;
    }
    return false;
}

void RestoreExtras() {
    void* state = g_extrasState;
    if (state && g_croftVtable && VtableOf(state) == ExtrasClone()) *reinterpret_cast<std::uintptr_t**>(state) = g_croftVtable;
    g_extrasMode = false;
    g_reopenViewer = false;
}

void __fastcall ExtrasHighlight(void* state, int index, int flag) {
    reinterpret_cast<void(__fastcall*)(void*, int, int)>(Fn(kRvaGenericPushHighlight))(state, index, flag);
    const GfxValue text = Str(index >= 0 && index < kExtrasCount ? ViewerText::Get(kExtras[index].description) : "");
    reinterpret_cast<bool(__fastcall*)(void*, const char*, const GfxValue*, const char*)>(Fn(kRvaSetVariable))(
        state, ".text_txt.text", &text, At<const char*>(state, kStateClipPath));
}

bool __fastcall ExtrasOnEnter(void* state, void* prev, void* cur) {
    const bool entered = reinterpret_cast<bool(__fastcall*)(void*, void*, void*)>(Fn(kRvaGenericOnEnter))(state, prev, cur);
    if (!entered) return entered;
    int slots = 10;
    {
        GfxValue result;
        g_invokeStateMovie(state, "GetNumNavItems", &result, nullptr, 0);
        if ((result.type & 0x8F) == 3) slots = int(result.number);
        ReleaseValue(result);
    }
    slots = std::clamp(slots, kExtrasCount, 32);
    At<std::int32_t>(state, kStateNavCount) = kExtrasCount;
    At<std::int32_t>(state, kCroftSlotCount) = slots;
    using IndexFn = void(__fastcall*)(void*, int);
    for (int i = 0; i < slots; ++i) {
        if (i < kExtrasCount) {
            Movie(state, "SetButtonText", {Num(i), Str(ViewerText::Get(kExtras[i].label))});
            reinterpret_cast<IndexFn>(Fn(EntryEnabled(i) ? kRvaPushEnable : kRvaPushDisable))(state, i);
        } else {
            Movie(state, "HideOption", {Num(i)});
            reinterpret_cast<IndexFn>(Fn(kRvaPushDisable))(state, i);
        }
    }
    for (int action : {kActLeft, kActLeftHeld, kActRight, kActRightHeld})
        reinterpret_cast<IndexFn>(Fn(kRvaNavDisableDirection))(state, action);
    reinterpret_cast<void(__fastcall*)(void*)>(Fn(kRvaRegisterNavItems))(state);
    using AccessibleFn = bool(__fastcall*)(void*, int);
    const auto accessible = [state](int i) {
        return i >= 0 && i < kExtrasCount && reinterpret_cast<AccessibleFn>(VtableOf(state)[0x78 / 8])(state, i);
    };
    bool any = accessible(At<std::int32_t>(state, kStateSelected));
    for (int i = 0; !any && i < kExtrasCount; ++i)
        if (accessible(i)) {
            reinterpret_cast<IndexFn>(VtableOf(state)[0x80 / 8])(state, i);
            any = true;
        }
    if (any)
        reinterpret_cast<void(__fastcall*)(void*, int, int)>(VtableOf(state)[0x88 / 8])(
            state, At<std::int32_t>(state, kStateSelected), 0);
    reinterpret_cast<void(__fastcall*)(void*)>(Fn(kRvaInitContextbar))(state);
    g_extrasTitleTries = SetExtrasTitle(state, true) ? 3 : 0;
    g_extrasEnterTime = Now();
    Log::Detailf("CutsceneViewer: Extras menu%s", g_reopenViewer.load() ? " (the Cutscene Viewer opens next)" : "");
    return entered;
}

void ExtrasActivate(void* state, bool sound) {
    const int i = At<std::int32_t>(state, kStateSelected);
    if (i < 0 || i >= kExtrasCount || !EntryEnabled(i) || g_play.load() != Play::None || g_modelUi.waiting) return;
    if (sound) PlaySoundId(state, kSelectSound);
    if (i == kExtrasViewer) {
        if (!OpenViewer()) Log::Write("CutsceneViewer: the Cutscene Viewer screen did not open (see above)");
    } else if (i == kExtrasModels) {
        OpenModels();
    } else if (i == kExtrasCredits) {
        if (!SetMenuState(kStateCredits, 0.25f)) Log::Write("CutsceneViewer: the game refused to open the credits");
    }
}

void __fastcall ExtrasUiSelect(void* state, char) {
    ExtrasActivate(state, false);
}

void __fastcall ExtrasPressed(void* state, int action, char c) {
    if (g_play.load() != Play::None) return;
    if (g_modelUi.waiting) {   // (the Model Viewer's movies on their way: Back stops waiting)
        if (action == kActCancel) CancelModelUi();
        return;
    }
    switch (action) {
        case kActCancel:   // as the Croft Manor menu: its sound, the previous state (the main menu)
            if (!(At<std::uint8_t>(state, kStateInputFlags) & 8)) return;
            PlaySoundId(state, kBackSound);
            if (void* mgr = StateManager())
                reinterpret_cast<bool(__fastcall*)(void*, bool, float)>(Fn(kRvaSetStatePrevious))(mgr, true, 0.25f);
            return;
        case kActSelect:
            if (!(At<std::uint8_t>(state, kStateInputFlags) & 8)) return;
            ExtrasActivate(state, true);
            return;
        case kActLeft:
        case kActLeftHeld:
        case kActRight:
        case kActRightHeld:
            return;
        default:   // up / down: the game's navigation
            reinterpret_cast<void(__fastcall*)(void*, int, char)>(Fn(kRvaGenericPressed))(state, action, c);
            return;
    }
}

void __fastcall ExtrasProcess(void* state, float) {
    PumpModelUi();
    const double since = Now() - g_extrasEnterTime;
    // the title, again a little later if the label was not there yet at OnEnter
    static constexpr double kTitleTimes[] = {0.15, 0.6, 1.5};
    if (g_extrasTitleTries < 3 && since >= kTitleTimes[g_extrasTitleTries]) {
        const bool set = SetExtrasTitle(state, true);
        ++g_extrasTitleTries;
        if (set) g_extrasTitleTries = 3;
        else if (g_extrasTitleTries == 3 && !g_extrasTitleLogged) {
            g_extrasTitleLogged = true;
            Log::Write("CutsceneViewer: the Extras menu's title label was not found (getInstanceAtDepth); its title is "
                       "the Croft Manor menu's");
        }
    }
    if (g_reopenViewer.load() && since >= 0.35 && IsCurrentState(state)) {
        g_reopenViewer = false;
        if (!OpenViewer()) Log::Write("CutsceneViewer: the Cutscene Viewer screen did not open again after the cutscene");
    }
}

bool __fastcall ExtrasOnExit(void* state, void* current, void* next) {
    CancelModelUi();
    const int to = next ? *static_cast<const int*>(next) : -1;
    const bool comingBack = to == kStateCombo || to == kStateCredits || to == kStateModelGrid ||
                            to == kStateList;   // (SetStatePrevious returns to it; v0.9.497 the Cutscene Viewer's list)
    if (!comingBack) SetExtrasTitle(state, false);
    const bool result = reinterpret_cast<bool(__fastcall*)(void*, void*, void*)>(Fn(kRvaGenericOnExit))(state, current, next);
    if (!comingBack) RestoreExtras();
    return result;
}

// EXTRAS: the Croft Manor menu as the Extras menu (the Cutscene Viewer screen at once without it).
bool EnterExtras() {
    if (!g_extrasReady) return OpenViewer();
    void* state = StateObject(kStateExtras);
    if (!state || !StateManager()) return false;
    std::uintptr_t* vtable = VtableOf(state);
    if (vtable != ExtrasClone()) {
        if (reinterpret_cast<std::uintptr_t>(vtable) != Fn(kRvaCroftVtable)) {
            Log::Write("CutsceneViewer: the Croft Manor menu's object is not the expected one; the Cutscene Viewer opens");
            return OpenViewer();
        }
        g_croftVtable = vtable;
        g_extrasCloneStore[0] = vtable[-1];   // (its RTTI locator)
        std::memcpy(ExtrasClone(), vtable, kVtableEntries * sizeof(std::uintptr_t));
        std::uintptr_t* clone = ExtrasClone();
        clone[0x08 / 8] = reinterpret_cast<std::uintptr_t>(&ExtrasUiSelect);
        clone[0x18 / 8] = reinterpret_cast<std::uintptr_t>(&ExtrasOnEnter);
        clone[0x20 / 8] = reinterpret_cast<std::uintptr_t>(&ExtrasPressed);
        clone[0x30 / 8] = reinterpret_cast<std::uintptr_t>(&ExtrasProcess);
        clone[0x40 / 8] = reinterpret_cast<std::uintptr_t>(&ExtrasOnExit);
        clone[0x88 / 8] = reinterpret_cast<std::uintptr_t>(&ExtrasHighlight);
        *reinterpret_cast<std::uintptr_t**>(state) = clone;
    }
    g_extrasState = state;
    g_extrasMode = true;
    if (!SetMenuState(kStateExtras, 0.25f)) {
        RestoreExtras();
        Log::Write("CutsceneViewer: the game refused to open the Extras menu");
        return false;
    }
    return true;
}

bool MainMenuIsCurrent() {
    void* menu = g_mainMenu.load();
    return menu && IsCurrentState(menu);
}

// ---- Main menu hooks ------------------------------------------------------------------------------------------------
void AddExtraOption(void* state) {
    if (!g_nativeReady || At<std::int32_t>(state, kStateNavCount) != kRetailOptionCount) return;
    GfxValue args[2];
    args[0].type = 3;
    args[0].number = kExtrasShownAt;
    args[1].type = 4;
    args[1].string = ViewerText::Get(ViewerText::Extras);   // (kept for the session: the movie may read it later)
    g_invokeStateMovie(state, "SetButtonText", nullptr, args, 2);
    args[0].number = kQuitShownAt;
    args[1].string = "Menu_QuitLabel";   // (a localization key: the label translates it, as the game's own option 9)
    g_invokeStateMovie(state, "SetButtonText", nullptr, args, 2);
    if (g_menuOption.exchange(1) != 1)
        Log::Write("CutsceneViewer: the main menu shows EXTRAS above QUIT GAME (options 9 and 11: Quit Game's and Start Benchmark's)");
}

// The dev mode's hidden option (12): its mouse hitbox, made again by the movie's OnEnter, removed.
void RemoveDevOptionHitbox(void* state) {
    Movie(state, "HideOption", {Num(kDevOption)});
}

// Credits are in the Extras menu, and Leaderboards (option 6: HandleLeaderboards, SetState(0x6E)) are gone with the
// game's online services (0.9.438): the main menu's own options hidden as its OnEnter hides Multiplayer (option 4:
// PushDisableIndexToMovie, the movie's HideOption, which moves the options under it up), then RegisterNavItems again
// (it only reads the options' accessibility and positions back from the movie), the selection moved off them.
bool g_optionsHiddenLogged = false;
void RemoveMenuOptions(void* state) {
    if (!g_extrasReady || At<std::int32_t>(state, kStateNavCount) != kRetailOptionCount) return;
    for (const int option : {kLeaderboardsOption, kCreditsOption}) {
        reinterpret_cast<void(__fastcall*)(void*, int)>(Fn(kRvaPushDisable))(state, option);
        Movie(state, "HideOption", {Num(option)});
    }
    reinterpret_cast<void(__fastcall*)(void*)>(Fn(kRvaRegisterNavItems))(state);
    const std::int32_t selected = At<std::int32_t>(state, kStateSelected);
    if (selected == kCreditsOption || selected == kLeaderboardsOption) {
        const int to = selected == kCreditsOption ? kExtrasShownAt : kOptionsOption;
        reinterpret_cast<void(__fastcall*)(void*, int)>(VtableOf(state)[0x80 / 8])(state, to);
        reinterpret_cast<void(__fastcall*)(void*, int, int)>(VtableOf(state)[0x88 / 8])(state, to, 0);
    }
    if (!g_optionsHiddenLogged) {
        g_optionsHiddenLogged = true;
        Log::Write("CutsceneViewer: the main menu's Leaderboards and Credits options hidden (the credits are in EXTRAS)");
    }
}

// The username and the news ticker under the options (0.9.438), hidden right after the main menu's OnEnter shows them.
bool g_menuWidgetsLogged = false;
void HideMenuWidgets(void* state) {
    if (!g_menuWidgetsReady) return;
    void* username = Widget(At<std::uint32_t>(state, kMainMenuUsernameWidget));
    void* menuing = GameStateMenuing();
    void* news = menuing ? Widget(At<std::uint32_t>(menuing, kGsmInfocastWidget)) : nullptr;
    for (void* widget : {username, news})
        if (widget && reinterpret_cast<bool(__fastcall*)(void*)>(Fn(kRvaWidgetIsVisible))(widget))
            reinterpret_cast<void(__fastcall*)(void*, float, bool)>(VtableOf(widget)[0x38 / 8])(widget, -1.0f, false);
    if (!g_menuWidgetsLogged) {
        g_menuWidgetsLogged = true;
        Log::Writef("CutsceneViewer: the main menu's username and news widgets hidden (%s, %s)", username ? "found" : "none",
                    news ? "found" : "none");
    }
}

// (v0.9.475) The main menu's 3D scene missed its "main menu entered". The menu level's graph learns the menu state only
// from evMenuStateChanged (UiMenuStateGeneric::OnEnter / OnPreExit / OnExit send it with SendEventNoPost, retail
// 0x7F9C10: UiMenuStateEventData {type 0 enter / 1 exit, from, to, the state's +0x2C}, 16 bytes) through its
// UiMenuStateNode, which subscribes in its Init (SetupListener, retail 0x7BA340; TR2 0x7F48E0) and never asks for the
// current state. With the faster loading the main menu is entered before the menu level's graph has started, so the
// event comes before any node listens and the scene stays black until the next menu change (the user's "it loads once
// I click on a menu element"). Now a UiMenuStateNode that starts listening while the main menu is current gets the
// main menu's own enter event again, on the next game tick (sent to every listener, as the game does).
constexpr std::uintptr_t kRvaMenuNodeSetupListener = 0x007BA340ull;
constexpr std::uint8_t kMenuNodeSetupListenerSig[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B,
                                                      0xD9, 0x48, 0x8B, 0x49, 0x20, 0x48, 0x85, 0xC9, 0x74, 0x12, 0x48, 0x8B};
constexpr std::uintptr_t kRvaMenuNodeEventName = 0x007BA381ull;   // lea r8, ["evMenuStateChanged"]
constexpr std::uintptr_t kRvaMenuStateChangedName = 0x00D6ACE8ull;
constexpr std::uintptr_t kRvaSendEventNoPost = 0x007F9C10ull;     // EventSystem::SendEventNoPost(char*, Instance*, int, void*, u64)
constexpr std::uint8_t kSendEventNoPostSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48,
                                                0x83, 0xEC, 0x40, 0x49, 0x8B, 0xF8, 0x48, 0x8B, 0xF1, 0x4C, 0x8B, 0xC2};
constexpr std::uintptr_t kRvaGenericEnterSystem = 0x008BB43Full;  // UiMenuStateGeneric::OnEnter: mov rcx, [EventSystem::s_instance]
constexpr std::uintptr_t kRvaGenericEnterSend = 0x008BB470ull;    // ...: call SendEventNoPost (its evMenuStateChanged)
constexpr std::uintptr_t kRvaEventSystemInstance = 0x02E1ACE8ull;
constexpr std::size_t kStateEventSub = 0x2C;
struct MenuStateEvent {
    std::int32_t type, from, to, sub;
};
static_assert(sizeof(MenuStateEvent) == 16, "UiMenuStateEventData is 16 bytes");
using SetupListenerFn = void(__fastcall*)(void* node);
using SendEventNoPostFn = void(__fastcall*)(void* system, const char* name, void* instance, int size, void* data, std::uint64_t extra);
SetupListenerFn g_origSetupListener = nullptr;
SendEventNoPostFn g_sendEventNoPost = nullptr;
MenuStateEvent g_menuEnterEvent{};
std::atomic_bool g_menuEnterKnown{false};
std::atomic_bool g_menuEnterResend{false};
std::atomic<int> g_lateMenuListeners{0};
int g_menuResends = 0;

std::atomic<int> g_menuListenerSetups{0};        // every UiMenuStateNode subscription
std::atomic<ULONGLONG> g_lastMenuSubscriptionMs{0};

void __fastcall HookMenuNodeSetupListener(void* node) {
    g_origSetupListener(node);
    g_menuListenerSetups.fetch_add(1);
    g_lastMenuSubscriptionMs.store(GetTickCount64());
    if (g_menuEnterKnown.load()) g_lateMenuListeners.fetch_add(1);   // (v0.9.478: counted for the log, not re-sent)
}

// (v0.9.478) The main menu held until its scene listens. 0.9.477's log: "the main menu entered (53 -> 6); 0 menu scene
// node subscription(s) so far", then 59 subscriptions; sending them the enter event again (0.9.475) did not bring the
// scene, and the menu then took no input. So the game's order is put back instead: state 53 is UiMenuStateCalibration
// (TR2 0x949130; retail OnEnter 0x906120), whose OnEnter only asks for the main menu (SetState(6, 0.25)) and sets its
// +0x38 to 2 when 0, with no movie of its own. Its OnEnter is hooked: unless a menu scene node subscribed in the last
// 5 s (the scene is up), it does the same without the SetState; the game tick then asks for the main menu once the
// scene's nodes have subscribed and none more for 0.3 s, or after 6 s whatever happens (logged either way), and only
// while Calibration is still the current state.
constexpr std::uintptr_t kRvaCalibrationOnEnter = 0x00906120ull;
constexpr std::uint8_t kCalibrationOnEnterSig[] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xF3, 0x0F, 0x10, 0x15, 0xD6, 0xAA, 0x40, 0x00, 0x48, 0x8D, 0x54, 0x24, 0x30, 0x48, 0x8B,
    0xD9, 0xC7, 0x44, 0x24, 0x30, 0x06, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x0D, 0x13, 0x86, 0x51, 0x02, 0xE8, 0x26, 0xCC, 0xFB, 0xFF,
    0x83, 0x7B, 0x38, 0x00, 0xB0, 0x01, 0x75, 0x07, 0xC7, 0x43, 0x38, 0x02, 0x00, 0x00, 0x00};
constexpr int kStateCalibration = 0x35, kStateMainMenu = 6;
constexpr std::size_t kCalibrationFlag = 0x38;
constexpr std::size_t kMgrCurrentState = 0xA28, kMgrNextState = 0xA2C;   // (UiMenuStateManager, retail)
constexpr ULONGLONG kSceneQuietMs = 300, kSceneWaitMs = 6000, kSceneRecentMs = 5000;
using CalibrationOnEnterFn = bool(__fastcall*)(void*, void*, void*);
CalibrationOnEnterFn g_origCalibrationOnEnter = nullptr;
std::atomic_bool g_calibrationHold{false};
ULONGLONG g_calibrationHoldSince = 0;
int g_calibrationHoldSubs = 0;

bool __fastcall HookCalibrationOnEnter(void* state, void* from, void* to) {
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG last = g_lastMenuSubscriptionMs.load();
    if (!state || (last && now - last < kSceneRecentMs)) return g_origCalibrationOnEnter(state, from, to);
    if (At<std::int32_t>(state, kCalibrationFlag) == 0) At<std::int32_t>(state, kCalibrationFlag) = 2;
    g_calibrationHoldSince = now;
    g_calibrationHoldSubs = g_menuListenerSetups.load();
    g_calibrationHold.store(true);
    Log::Write("CutsceneViewer: the main menu waits for its scene (no menu scene node subscribed yet)");
    return true;
}

void CalibrationHoldTick() {
    if (!g_calibrationHold.load()) return;
    const ULONGLONG now = GetTickCount64();
    const int subs = g_menuListenerSetups.load() - g_calibrationHoldSubs;
    const bool quiet = subs > 0 && now - g_lastMenuSubscriptionMs.load() >= kSceneQuietMs;
    const bool timedOut = now - g_calibrationHoldSince >= kSceneWaitMs;
    if (!quiet && !timedOut) return;
    g_calibrationHold.store(false);
    void* mgr = StateManager();
    const std::int32_t current = mgr ? At<std::int32_t>(mgr, kMgrCurrentState) : -1;
    const std::int32_t next = mgr ? At<std::int32_t>(mgr, kMgrNextState) : -1;
    if (current != kStateCalibration || next != -1) {
        Log::Writef("CutsceneViewer: the main menu's wait ended with the menu elsewhere (state %d, next %d): left as it is", current, next);
        return;
    }
    const bool asked = SetMenuState(kStateMainMenu, 0.25f);
    Log::Writef("CutsceneViewer: the main menu %s after %llu ms (%d menu scene node subscription(s)%s)",
                asked ? "comes" : "could not be asked for", static_cast<unsigned long long>(now - g_calibrationHoldSince), subs,
                timedOut && !quiet ? "; waited the longest" : "");
}

bool SendMenuEvent(MenuStateEvent* event) {
    __try {
        void* system = *reinterpret_cast<void* const*>(GameBuild::Base() + kRvaEventSystemInstance);
        if (!system) return false;
        g_sendEventNoPost(system, "evMenuStateChanged", nullptr, static_cast<int>(sizeof(*event)), event, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// game thread: a scene node that began listening after the main menu came gets its enter event
void ResendMenuEnterTick() {
    CalibrationHoldTick();   // (v0.9.478)
    if (!g_menuEnterKnown.load()) return;
    if (!MainMenuIsCurrent()) {
        g_menuEnterKnown.store(false);
        g_menuEnterResend.store(false);
        return;
    }
    if (!g_menuEnterResend.exchange(false)) return;
    MenuStateEvent event = g_menuEnterEvent;
    const int late = g_lateMenuListeners.exchange(0);
    const bool sent = SendMenuEvent(&event);
    if (g_menuResends++ < 16)
        Log::Writef("CutsceneViewer: %d menu scene node(s) began listening after the main menu came: its evMenuStateChanged "
                    "(enter, %d -> %d) sent again%s",
                    late, event.from, event.to, sent ? "" : " (failed)");
}

bool __fastcall HookOnEnter(void* state, void* a, void* b) {
    // (the Croft Manor menu is its own again when the main menu comes back without passing through the Extras menu's
    // exit: after a cutscene, after the credits went elsewhere)
    if (g_extrasMode.load() && !IsCurrentState(g_extrasState)) RestoreExtras();
    if (g_modelViewMode.load()) RestoreModelView();   // (the Model Viewer's screens left some other way)
    if (g_modelGridMode.load()) RestoreModelGrid();
    ModelViewer::Close();
    UnloadModelUi();
    const bool result = g_origOnEnter(state, a, b);
    g_menuEnters.fetch_add(1);
    if (state) {
        g_mainMenu = state;
        Features::ReleaseOutfitPinIfLanguageChanged();   // (0.9.455)
        AddExtraOption(state);
        if (result && g_menuOption.load() == 1) RemoveMenuOptions(state);
        if (result && g_nativeReady && At<std::int32_t>(state, kStateNavCount) == kRetailOptionCount) RemoveDevOptionHitbox(state);
        if (result) HideMenuWidgets(state);
    }
    SteamSaveCache::OnMainMenuEntered();   // (v0.9.473) its missed save data read again
    if (result && state && g_sendEventNoPost) {   // (v0.9.475) the enter event, for scene nodes that start listening later
        g_menuEnterEvent = MenuStateEvent{0, a ? *static_cast<const std::int32_t*>(a) : -1,
                                          b ? *static_cast<const std::int32_t*>(b) : -1, At<std::int32_t>(state, kStateEventSub)};
        g_lateMenuListeners.store(0);
        g_menuEnterResend.store(false);
        g_menuEnterKnown.store(true);
        static int s_enterLogs = 0;
        if (s_enterLogs++ < 4)
            Log::Detailf("CutsceneViewer: the main menu entered (%d -> %d); %d menu scene node subscription(s) so far",
                        g_menuEnterEvent.from, g_menuEnterEvent.to, g_menuListenerSetups.load());
    }
    return result;
}

void __fastcall HookOnUiSelect(void* state, char c) {
    if (g_play.load() != Play::None) return;
    if (!state || g_menuOption.load() != 1 || !g_nativeReady) return g_origOnUiSelect(state, c);
    std::int32_t& selected = At<std::int32_t>(state, kStateSelected);
    // options not shown (Credits, in EXTRAS; Leaderboards; the dev mode's): nothing, not even the select sound
    if (selected >= kRetailOptionCount ||
        ((selected == kCreditsOption || selected == kLeaderboardsOption) && g_extrasReady))
        return;
    if (selected == kExtrasShownAt) {
        PlaySoundId(state, kSelectSound);
        EnterExtras();
        return;
    }
    if (selected == kQuitShownAt) {   // Quit Game, in Start Benchmark's place: the game's option 9 (its quit prompt)
        selected = kQuitOption;
        g_origOnUiSelect(state, c);
        if (selected == kQuitOption) selected = kQuitShownAt;   // (the highlight stays on QUIT GAME)
        return;
    }
    g_origOnUiSelect(state, c);
}

// ---- The main menu's level (0.9.386) ----
// GameStateMenuing::Tick's first call runs GameStateMenuing::InitMainMenu (retail 0x583400 by its "trx_main_menu"
// reference, TR2 0x5B78D0): the level name "trx_main_menu" into the game shell, MAIN_DoLoadLevel (retail 0x458170, TR2
// 0x4875C0: InitLoadingDisplay, Gameloop::LevelLoadAndInit -> InitiateLevelLoad ... CompleteLevelLoadAndInit),
// CloseLoadingDisplay, then the menu states (UiMenuStateManager::SetState(6, -1) and the rest by its menu stack, and
// UiMenuStateManager::Process: the main menu's OnEnter). On the viewer's way back its call is timed and profiled as the
// game's GameStateLoading loads are (Features::BeginOtherLoadProfile: its LoadProf line), and the game ticks inside it
// counted (0: it blocks the game loop).
constexpr std::uintptr_t kRvaInitMainMenu = 0x00583400ull;
constexpr std::uint8_t kInitMainMenuSig[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x48, 0x8B, 0xEC, 0x48,
                                             0x83, 0xEC, 0x70, 0x80, 0x79, 0x10, 0x00, 0x48, 0x8B, 0xD9};
using InitMainMenuFn = void(__fastcall*)(void*);
InitMainMenuFn g_origInitMainMenu = nullptr;
void __fastcall HookInitMainMenu(void* menuing);   // (with the way back's timing, below)
// UiMenuStateManager (retail; TR2 +8): the current state +0xA28, the next +0xA2C (retail ChangeState 0x8AEED0)
constexpr std::size_t kSmCurrentState = 0xA28, kSmNextState = 0xA2C;

// ---- (v0.9.496) The outfit grid on the right of the Cutscene Viewer screen --------------------------------------------
// The user: stepping through 30-odd outfits with the arrows to find one was a chore; a grid as the Model Viewer's, on
// the screen's right, instead of the OUTFIT row. The Cutscene Viewer is one game screen (the combo, 0x82) and the Model
// Viewer's grid another (0x59, the campsite's): one of them at a time. So the grid is ours, in the combo's own movie, as
// the Model Viewer's title, tabs and search are in the grid's: a clip (fw_outfit_grid_mc) placed in the stage's
// coordinates; a tile per choice (the story's outfit first, then Features' outfits in the OUTFIT row's order), six a
// row, at half the campsite's tile (125 x 180); each outfit's campsite picture (its dtp::OutfitsData ImagePath through
// UiImageLoaderManager, as the Model Viewer's tiles) loaded into the tile with loadMovie("img://#<texture>") (the game's
// CustomImageLoader takes it in any movie), placed by its ImagePath offsets and masked to the tile; a frame (thin, white
// under the mouse or the keys, thick for the outfit chosen) and the name under the grid (the one under the mouse, else
// the chosen one). Mouse: a click chooses; the wheel scrolls past six rows. Keys / pad: Right on PLAY goes into the
// grid, the arrows move, Select chooses (and goes back to PLAY), Back or Left from the first column go back.
constexpr const char* kOgClip = "fw_outfit_grid_mc";
constexpr double kOgTileW = 125.0, kOgTileH = 180.0;
constexpr double kOgPictureX = 1.9, kOgPictureY = 10.65, kOgPictureScaleX = 97.08, kOgPictureScaleY = 97.07;
// (v0.9.497: its layout, g_ogColumns and the rest, with the forward declarations above ExtraOnEnter)
void ListRefocus(void* state);   // (the list's highlight back from the grid, below)
struct OutfitTile {
    int choice = 0;          // its g_outfits index
    const void* data = nullptr;
    int request = -1;        // the image loader's request
    unsigned texture = 0;
    bool sent = false;       // given to its clip
    int layout[4] = {0, 0, 128, 256};
};
std::vector<OutfitTile> g_ogTiles;
bool g_ogMade = false;
int g_ogMakeTries = 0;
double g_ogMadeAt = 0.0;
bool g_ogPicturesLogged = false;
const char* g_ogFont = nullptr;
int g_ogCursor = -1;                     // the keys' tile (-1: the rows have them)
int g_ogFirstRow = 0, g_ogDrawnFirstRow = -1;
std::vector<int> g_ogDrawn;              // each tile's frame as drawn
std::string g_ogNameShown;
std::atomic<int> g_ogUnderMouse{-1};     // (game thread; read by the window's)
std::atomic<bool> g_ogOverGrid{false};
std::atomic<int> g_ogClicked{-1};        // (the window's thread; taken by the game thread)
std::atomic<int> g_ogWheel{0};
std::atomic<bool> g_ogPressKept{false};
std::atomic<bool> g_ogLeave{false};

double OgLeft() { return g_ogRight - g_ogColumns * kOgTileW * g_ogScale; }
int OgRowsTotal() { return (int(g_ogTiles.size()) + g_ogColumns - 1) / g_ogColumns; }
std::string OgTileName(int i) {
    char name[24];
    std::snprintf(name, sizeof(name), "tile%d_mc", i);
    return name;
}
int OgChosenTile() {
    for (int i = 0; i < int(g_ogTiles.size()); ++i)
        if (g_ogTiles[std::size_t(i)].choice == g_outfitPick) return i;
    return 0;
}

// (v0.9.498) One of our clips in the stage's coordinates, whatever the screen's clip is moved and scaled by now (the
// list's PlayEnterAnimation slides it in from the right: 0.9.497's grid, placed then, stayed 208 to the left).
void KeepOnStage(void* state, const char* name) {
    GfxValue* clip = StateClip(state);
    GfxValue ours;
    if (!GfxChild(*clip, name, ours)) return;
    const double sx = GfxNumber(*clip, "_xscale"), sy = GfxNumber(*clip, "_yscale");
    const double cx = GfxNumber(*clip, "_x"), cy = GfxNumber(*clip, "_y");
    const double fx = std::isfinite(sx) && sx > 1.0 ? sx / 100.0 : 1.0, fy = std::isfinite(sy) && sy > 1.0 ? sy / 100.0 : 1.0;
    const double want[4] = {100.0 / fx, 100.0 / fy, -(std::isfinite(cx) ? cx : 0.0) / fx, -(std::isfinite(cy) ? cy : 0.0) / fy};
    const char* const props[4] = {"_xscale", "_yscale", "_x", "_y"};
    for (int k = 0; k < 4; ++k) {
        const double now = GfxNumber(ours, props[k]);
        if (!std::isfinite(now) || std::fabs(now - want[k]) > 0.01) GfxSet(ours, props[k], Num(want[k]));
    }
    ReleaseValue(ours);
}

void HideOutfitRow(void* state) {
    GfxValue row;
    if (GfxChild(*StateClip(state), "nav_list_item_1_mc", row)) {
        GfxSet(row, "_visible", Bool(false));
        GfxSet(row, "_alpha", Num(0.0));
    }
    ReleaseValue(row);
}

void OutfitGridPictureLoaded(unsigned request, unsigned texture) {
    for (OutfitTile& tile : g_ogTiles)
        if (tile.request == int(request)) {
            tile.texture = texture;
            tile.sent = false;
        }
}

void OgReleasePictures() {
    void* manager = ImageLoaderManager();
    for (OutfitTile& tile : g_ogTiles)
        if (tile.request >= 0 && manager)
            reinterpret_cast<void(__fastcall*)(void*, int*, void*)>(Fn(kRvaUnloadImage))(manager, &tile.request, &g_imageCallback);
    g_ogTiles.clear();
}

void OutfitGridRemove(void* state) {
    GfxValue og;
    if (state && GfxChild(*StateClip(state), kOgClip, og)) GfxCall(og, "removeMovieClip", {});
    ReleaseValue(og);
    OgReleasePictures();
    g_ogMade = false;
    g_ogMakeTries = 0;
    g_ogCursor = -1;
    g_ogDrawn.clear();
    g_ogDrawnFirstRow = -1;
    g_ogNameShown.clear();
    g_ogUnderMouse = -1;
    g_ogOverGrid = false;
    g_ogClicked = -1;
    g_ogWheel = 0;
}

// A tile's frame: 0 the plain one, 1 under the mouse or the keys, 2 the outfit chosen, 3 both.
void OgDrawFrame(const GfxValue& frame, int kind) {
    GfxCall(frame, "clear", {});
    const double t = kind >= 2 ? 4.0 : 2.0;   // (the tile's own units: half that on the screen)
    const double alpha = kind >= 2 ? 100.0 : kind == 1 ? 70.0 : 22.0;
    const std::uint32_t color = kHudWhite;
    HudBox(frame, 0.0, 0.0, kOgTileW, t, color, alpha);
    HudBox(frame, 0.0, kOgTileH - t, kOgTileW, kOgTileH, color, alpha);
    HudBox(frame, 0.0, t, t, kOgTileH - t, color, alpha);
    HudBox(frame, kOgTileW - t, t, kOgTileW, kOgTileH - t, color, alpha);
    if (kind == 1 || kind == 3) HudBox(frame, t, t, kOgTileW - t, kOgTileH - t, color, 10.0);   // (a light veil)
}

void OutfitGridMake(void* state) {
    OutfitGridRemove(state);
    if (!GfxIsObject(*StateClip(state))) return;
    // the choices: the story's outfit, then each outfit with its record (its picture)
    for (int i = 0; i < int(g_outfits.size()); ++i) {
        OutfitTile tile;
        tile.choice = i;
        if (g_outfits[std::size_t(i)].index >= 0) {
            tile.data = Features::OutfitData(g_outfits[std::size_t(i)].index);
            const char* action = "";
            if (tile.data) PictureLayoutOf(tile.data, tile.layout, action);
            tile.request = tile.data ? AskOutfitPicture(tile.data) : -1;
        }
        g_ogTiles.push_back(tile);
    }
    GfxValue* clip = StateClip(state);
    GfxValue next, made, og;
    GfxCall(*clip, "getNextHighestDepth", {}, &next);
    const double depth = (next.type & 0x8F) == 3 && next.number >= 0.0 && next.number < 16384.0 ? next.number : 4096.0;
    ReleaseValue(next);
    GfxCall(*clip, "createEmptyMovieClip", {Str(kOgClip), Num(depth)}, &made);
    ReleaseValue(made);
    if (!GfxChild(*clip, kOgClip, og)) {
        Log::Write("CutsceneViewer: the screen's movie made no clip for the outfit grid");
        return;
    }
    // (in the stage's coordinates whatever the screen's clip is scaled and placed by)
    const double sx = GfxNumber(*clip, "_xscale"), sy = GfxNumber(*clip, "_yscale");
    const double cx = GfxNumber(*clip, "_x"), cy = GfxNumber(*clip, "_y");
    const double fx = std::isfinite(sx) && sx > 1.0 ? sx / 100.0 : 1.0, fy = std::isfinite(sy) && sy > 1.0 ? sy / 100.0 : 1.0;
    GfxSet(og, "_xscale", Num(100.0 / fx));
    GfxSet(og, "_yscale", Num(100.0 / fy));
    GfxSet(og, "_x", Num(-(std::isfinite(cx) ? cx : 0.0) / fx));
    GfxSet(og, "_y", Num(-(std::isfinite(cy) ? cy : 0.0) / fy));
    int at = 1;
    for (int i = 0; i < int(g_ogTiles.size()); ++i) {
        GfxValue tile, part;
        const std::string name = OgTileName(i);
        GfxCall(og, "createEmptyMovieClip", {Str(name.c_str()), Num(double(at++))}, &made);
        ReleaseValue(made);
        if (!GfxChild(og, name.c_str(), tile)) continue;
        GfxSet(tile, "_xscale", Num(g_ogScale * 100.0));
        GfxSet(tile, "_yscale", Num(g_ogScale * 100.0));
        GfxCall(tile, "createEmptyMovieClip", {Str("bg_mc"), Num(1.0)}, &made);
        ReleaseValue(made);
        if (GfxChild(tile, "bg_mc", part)) HudBox(part, 0.0, 0.0, kOgTileW, kOgTileH, 0x000000, 45.0);
        ReleaseValue(part);
        GfxCall(tile, "createEmptyMovieClip", {Str("holder_mc"), Num(2.0)}, &made);
        ReleaseValue(made);
        GfxValue holder, mask;
        if (GfxChild(tile, "holder_mc", holder)) {
            // (v0.9.499) As the game's tile shows it (menu_basecamp_gear_select's own placements, read from the movie):
            // the tile (navListItemCraft) has Icons_mc at (1.9, 10.65) at 97.08 / 97.07 %; the ImageLoader component in it
            // (ImageLoader_mc, 91.76 / 60.16 %) loads with NO_SCALE into a container of its own at the picture's own
            // shape (0.9.498 took its squash for the picture's: the user saw them stretched). 0.9.497: from the top left.
            GfxSet(holder, "_x", Num(kOgPictureX));
            GfxSet(holder, "_y", Num(kOgPictureY));
            GfxSet(holder, "_xscale", Num(kOgPictureScaleX));
            GfxSet(holder, "_yscale", Num(kOgPictureScaleY));
            GfxCall(holder, "createEmptyMovieClip", {Str("pic_mc"), Num(1.0)}, &made);
            ReleaseValue(made);
            GfxCall(tile, "createEmptyMovieClip", {Str("mask_mc"), Num(3.0)}, &made);
            ReleaseValue(made);
            if (GfxChild(tile, "mask_mc", mask)) {
                HudBox(mask, 2.0, 2.0, kOgTileW - 2.0, kOgTileH - 2.0, 0xFFFFFF, 100.0);
                GfxCall(holder, "setMask", {mask});
            }
        }
        ReleaseValue(mask);
        ReleaseValue(holder);
        if (g_ogTiles[std::size_t(i)].choice == 0) {   // the story's outfit: its words in the tile
            GfxValue label;
            if (HudField(tile, "label_txt", 4, 8.0, 64.0, kOgTileW - 16.0, true, label)) {
                GfxSet(label, "autoSize", Str("center"));
                for (const char* font : kHudFonts) {
                    HudText(label, font, ViewerText::ToUpper(g_outfits[0].name), 30.0, kHudGrey);
                    if (GfxNumber(label, "textWidth") > 0.0) {
                        g_ogFont = font;
                        break;
                    }
                }
            }
            ReleaseValue(label);
        }
        GfxCall(tile, "createEmptyMovieClip", {Str("frame_mc"), Num(5.0)}, &made);
        ReleaseValue(made);
        ReleaseValue(tile);
    }
    // the name under the grid
    GfxValue field;
    const double gridHeight = g_ogRows * kOgTileH * g_ogScale;
    if (HudField(og, "name_txt", at++, OgLeft() - kHudGutter, g_ogTop + gridHeight + 10.0,
                 g_ogColumns * kOgTileW * g_ogScale + 2.0 * kHudGutter, true, field) && !g_ogFont)
        for (const char* font : kHudFonts) {
            HudText(field, font, "X", kHudNameSize, kHudWhite);
            if (GfxNumber(field, "textWidth") > 0.0) {
                g_ogFont = font;
                break;
            }
        }
    ReleaseValue(field);
    ReleaseValue(og);
    g_ogMade = true;
    g_ogMadeAt = Now();
    g_ogPicturesLogged = false;
    g_ogFirstRow = std::max(0, OgChosenTile() / g_ogColumns - (g_ogRows - 1));
    Log::Writef("CutsceneViewer: the outfit grid (%zu tiles, %d rows of %d; font %s)", g_ogTiles.size(), OgRowsTotal(),
                g_ogColumns, g_ogFont ? g_ogFont : "none found");
}

void OgKeepVisible(int tile) {
    const int row = tile / g_ogColumns;
    if (row < g_ogFirstRow) g_ogFirstRow = row;
    if (row >= g_ogFirstRow + g_ogRows) g_ogFirstRow = row - g_ogRows + 1;
}

void OgChoose(void* state, int tile) {
    if (tile < 0 || tile >= int(g_ogTiles.size())) return;
    g_outfitPick = g_ogTiles[std::size_t(tile)].choice;
    PlaySoundId(state, kSelectSound);
}

void OutfitGridFocus(void* state, bool on) {
    if (on) {
        if (!g_ogMade || g_ogTiles.empty()) return;
        g_ogCursor = OgChosenTile();
        OgKeepVisible(g_ogCursor);
        if (!g_ogInList) Movie(state, "OnHighlight", {Num(-1)});   // (no row lit while the grid has the keys)
        PlaySoundId(state, 0);
    } else if (g_ogCursor >= 0) {
        g_ogCursor = -1;
        if (g_ogInList) ListRefocus(state);
        else Highlight(state, kRowPlay);
        PlaySoundId(state, 0);
    }
}

void OutfitGridLeave() { g_ogLeave = true; }

bool OgMove(void* state, int action) {
    const int n = int(g_ogTiles.size()), col = g_ogCursor % g_ogColumns;
    int to = g_ogCursor;
    if (action == kActUp || action == kActUpHeld) to -= g_ogColumns;
    else if (action == kActDown || action == kActDownHeld) to = std::min(n - 1, to + g_ogColumns);
    else if (action == kActRight || action == kActRightHeld) to = col + 1 < g_ogColumns ? std::min(n - 1, to + 1) : to;
    else if (action == kActLeft || action == kActLeftHeld) {
        if (col == 0) {   // (the first column: back to the rows)
            if (action == kActLeft) OutfitGridFocus(state, false);
            return true;
        }
        --to;
    } else {
        return false;
    }
    if (to >= 0 && to < n && to != g_ogCursor) {
        g_ogCursor = to;
        OgKeepVisible(to);
        PlaySoundId(state, 0);
    }
    return true;
}

bool OutfitGridPressed(void* state, int action) {
    if (g_ogCursor < 0 || !g_ogMade) return false;
    if (action == kActSelect) {
        OgChoose(state, g_ogCursor);
        OutfitGridFocus(state, false);   // (chosen: PLAY next)
        return true;
    }
    if (action == kActCancel) {
        OutfitGridFocus(state, false);
        return true;
    }
    return OgMove(state, action);   // (LB / RB not: they still change the cutscene)
}

bool OutfitGridHeld(void* state, int action) {
    if (g_ogCursor < 0 || !g_ogMade) return false;
    return OgMove(state, action);
}

// Every frame on the screen: the mouse, a click, the wheel, the frames, the pictures, the name.
void OutfitGridProcess(void* state) {
    if (!g_ogMade) {
        if (g_ogMakeTries < 5 && GfxIsObject(*StateClip(state))) {
            ++g_ogMakeTries;
            const int tries = g_ogMakeTries;
            OutfitGridMake(state);
            g_ogMakeTries = tries;
        }
        return;
    }
    KeepOnStage(state, kOgClip);   // (v0.9.498)
    GfxValue og;
    if (!GfxChild(*StateClip(state), kOgClip, og)) {
        g_ogMade = false;
        return;
    }
    if (g_ogLeave.exchange(false) && g_ogCursor >= 0) g_ogCursor = -1;   // (the mouse went to the rows)
    const int n = int(g_ogTiles.size());
    // the mouse (with mouse and keyboard in use, the input on)
    int under = -1;
    bool over = false;
    const double left = OgLeft(), stepX = kOgTileW * g_ogScale, stepY = kOgTileH * g_ogScale;
    if ((At<std::uint8_t>(state, kStateInputFlags) & 8) && MouseKeyboardInUse()) {
        const double x = GfxNumber(og, "_xmouse"), y = GfxNumber(og, "_ymouse");
        if (std::isfinite(x) && std::isfinite(y) && x >= left && x < g_ogRight && y >= g_ogTop && y < g_ogTop + g_ogRows * stepY) {
            over = true;
            const int col = int((x - left) / stepX), row = int((y - g_ogTop) / stepY) + g_ogFirstRow;
            const int i = row * g_ogColumns + col;
            if (col >= 0 && col < g_ogColumns && i >= 0 && i < n) under = i;
        }
    }
    g_ogUnderMouse = under;
    g_ogOverGrid = over;
    if (const int clicked = g_ogClicked.exchange(-1); clicked >= 0 && clicked < n) {
        OgChoose(state, clicked);
        if (g_ogCursor >= 0) g_ogCursor = clicked;
    }
    if (const int wheel = g_ogWheel.exchange(0); wheel != 0) {
        const int maxFirst = std::max(0, OgRowsTotal() - g_ogRows);
        g_ogFirstRow = std::clamp(g_ogFirstRow + (wheel > 0 ? -1 : 1), 0, maxFirst);
    }
    // the tiles: placed by the rows scrolled, framed, their pictures given once there
    if (int(g_ogDrawn.size()) != n) g_ogDrawn.assign(std::size_t(n), -1);
    const bool placed = g_ogDrawnFirstRow == g_ogFirstRow;
    const int chosen = OgChosenTile();
    for (int i = 0; i < n; ++i) {
        OutfitTile& t = g_ogTiles[std::size_t(i)];
        const int row = i / g_ogColumns - g_ogFirstRow;
        const bool visible = row >= 0 && row < g_ogRows;
        const int kind = (i == chosen ? 2 : 0) + (i == under || i == g_ogCursor ? 1 : 0);
        const bool pictureDue = t.texture && !t.sent;
        if (placed && g_ogDrawn[std::size_t(i)] == kind && !pictureDue) continue;
        GfxValue tile;
        if (!GfxChild(og, OgTileName(i).c_str(), tile)) continue;
        if (!placed) {
            GfxSet(tile, "_visible", Bool(visible));
            GfxSet(tile, "_x", Num(left + (i % g_ogColumns) * stepX));
            GfxSet(tile, "_y", Num(g_ogTop + row * stepY));
        }
        if (g_ogDrawn[std::size_t(i)] != kind) {
            GfxValue frame;
            if (GfxChild(tile, "frame_mc", frame)) OgDrawFrame(frame, kind);
            ReleaseValue(frame);
            g_ogDrawn[std::size_t(i)] = kind;
        }
        if (pictureDue) {
            GfxValue holder, picture;
            if (GfxChild(tile, "holder_mc", holder) && GfxChild(holder, "pic_mc", picture)) {
                char url[40];
                std::snprintf(url, sizeof(url), "img://#%u", t.texture);
                GfxCall(picture, "loadMovie", {Str(url)});
            }
            ReleaseValue(picture);
            ReleaseValue(holder);
            t.sent = true;
        }
        ReleaseValue(tile);
    }
    g_ogDrawnFirstRow = g_ogFirstRow;
    // the name: the tile under the mouse or the keys, else the chosen one
    const int named = under >= 0 ? under : g_ogCursor >= 0 ? g_ogCursor : chosen;
    const std::string name = named >= 0 && named < n && g_ogTiles[std::size_t(named)].choice < int(g_outfits.size())
                                 ? ViewerText::ToUpper(g_outfits[std::size_t(g_ogTiles[std::size_t(named)].choice)].name)
                                 : std::string();
    if (name != g_ogNameShown && g_ogFont) {
        GfxValue field;
        if (GfxChild(og, "name_txt", field)) HudText(field, g_ogFont, name, kHudNameSize, kHudWhite);
        ReleaseValue(field);
        g_ogNameShown = name;
    }
    if (!g_ogPicturesLogged && Now() - g_ogMadeAt > 2.0) {   // (whether the pictures came: logged once)
        g_ogPicturesLogged = true;
        int asked = 0, came = 0, drawn = 0;
        for (int i = 0; i < n; ++i) {
            const OutfitTile& t = g_ogTiles[std::size_t(i)];
            asked += t.request >= 0;
            came += t.texture != 0;
            GfxValue tile, holder;
            if (t.sent && GfxChild(og, OgTileName(i).c_str(), tile) && GfxChild(tile, "holder_mc", holder) &&
                GfxNumber(holder, "_width") > 1.0)
                ++drawn;
            ReleaseValue(holder);
            ReleaseValue(tile);
        }
        Log::Writef("CutsceneViewer: the outfit grid's pictures after 2 s: %d asked for, %d there, %d drawn", asked, came, drawn);
    }
    ReleaseValue(og);
}

// (the window's thread) A click on a tile: that outfit; the wheel over the grid: its rows. Kept from the game.
bool OutfitGridMessage(UINT msg, WPARAM wParam) {
    if (!g_ogMade || g_play.load() != Play::None) return false;
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
        const int tile = g_ogUnderMouse.load();
        if (tile < 0) return false;
        g_ogClicked = tile;
        g_ogPressKept = true;
        return true;
    }
    if (msg == WM_LBUTTONUP) return g_ogPressKept.exchange(false);
    if (msg == WM_MOUSEWHEEL && g_ogOverGrid.load()) {
        g_ogWheel += GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1 : -1;
        return true;
    }
    return false;
}

// ---- (v0.9.497; v0.9.500 side by side, the game's own UI only) The Cutscene Viewer on the game's Relics list ----------
// The user: the cutscene picked as the pause menu's RELICS list picks a relic, its picture on the top right and the outfit
// grid under it, with nothing but the game's own UI (0.9.497 - 499 drew the picture, the grid and a band themselves).
// - The list: the game's Relics list (UiMenuStateCollectibleListRelics, state 0x5C; retail vtable 0xDB7ED8, 36 entries,
//   checked; its movie trx_collectible_list, class TRX_Collectibles_List, in an in-game menu collection) with a copy of its
//   vtable: OnEnter, input, Process, OnPreExit, OnExit, PushHighlightIndexToMovie, the mouse ours; UiMenuStateGeneric's
//   SetSelectionIndex; nothing for its own work from +0xF0. UiMenuStateGeneric::OnEnter (the movie), then the movie's Init,
//   SetHeaderTitle(CUTSCENE VIEWER), AddCategory(the category), AddNavItemRelic("None", title) a cutscene,
//   PlayEnterAnimation(false), SetNavigationEnabled(true); a highlight: OnHighlight(index, "", 0), SetHeaderSub("CATEGORY
//   i / n"). One category at a time (LB / RB, LT / RT), as the game's sets.
// - The picture: the main menu's own live information panel (Widget_LiveInfo, the one the Expeditions screen and 0.9.345's
//   Cutscene Viewer show): GameStateMenuing::ShowLiveInfoWidget(callback) (TR2 0x5C5970, retail 0x590D70: makes it, widget
//   type 0x77, at its +0x84, when there is none; shows it in 0.25 s; the callback is called with its ID once it is there)
//   with a callback of ours; filled as before (UpdateInfo: the title, the picture; v0.9.501 the outfit's name in the
//   description's place) and moved to the screen's top right (its clip _root.<name>, through
//   UiWidgetScaleform::GetVariable: v0.9.501 at the game's own size, its picture's frame at x 744, y 28 of the stage;
//   again every 0.25 s while shown). Hidden when the list goes (its vfunc +0x38, GameStateMenuing +0x13 cleared).
// - The outfit grid: the campsite's own (UiMenuStateCampsiteGear, state 0x59, menu_basecamp_gear_select, the Model
//   Viewer's), opened over the list as the relic viewer is opened over the Model Viewer's grid: its +0xD5 bit 1 (a full
//   screen) cleared while it is up, so UiMenuStateManager::ChangeState leaves the list as it is (its OnExit not called:
//   its movie and its Process stay) and calls the grid's OnEnter; on the way back the list is not entered again. The grid
//   (a copy of its vtable, as the Model Viewer's: ours at +0x18 .. +0x40, +0x88, +0xA0, +0xB0; nothing for its campsite
//   work) has a tile per choice (the story's outfit first, then Features' outfits) with each outfit's own campsite picture
//   (LoadOutfitImage), the tile of the outfit chosen highlighted and its check mark shown (the campsite's mark of the outfit
//   worn), six a row, five rows, at 43 % under the picture (GridLayout, as the Model Viewer's); its information widgets
//   hidden (the outfit's name is in the picture's panel). v0.9.501: what the game checks before it draws the grid's
//   movie checked and put right while it is over the list (PickerShow, below).
// - Input (the grid is the current screen: the game gives it the input): Up / Down the cutscene (the list's movie), Left /
//   Right the outfit (held: again after 0.3 s, then every 0.12 s), LB / RB and LT / RT the category, Select plays, Back the
//   Extras menu (the grid back to the list, then the list's own Back). The mouse: a tile under it highlighted, a click on
//   it chooses that outfit (v0.9.503; its check mark, the movie's SetCheckMarkVisible); a list entry under it the
//   cutscene, a click on it plays (v0.9.501: hit by us, PickerListMouse: the game gives the mouse to the current screen's
//   movie only). The wheel (v0.9.503, PickerWheelMessage): on the list's side of the screen the list, else the grid's rows.
// - The dark top (v0.9.508): the main menu's room left out of the drawing while the list is up: the list's own background
//   on black, as the Relics list in game; v0.9.509 faded out as the list comes, in as it goes (ListBandStart, BackdropTick,
//   below); the panel's and the grid's movies kept drawn after the list's.
// - When the list cannot be opened (its object, its movie's collection) the Expeditions screen as before
//   (OpenComboViewer); without the grid's movies, the list alone (Select plays with the outfit chosen before).
constexpr std::uintptr_t kRvaListVtable = 0x00DB7ED8ull;
constexpr std::size_t kListEntries = 36;
constexpr int kMouseListItem = 1442;   // the list movie's entries' Mouse_Button ID (param: the entry)
std::uintptr_t g_listStore[kListEntries + 2] = {};   // [0]: the RTTI locator; the vtable from [1]
std::uintptr_t* g_listVtable = nullptr;              // the game's
void* g_listState = nullptr;
// (v0.9.513) The list's movie as ListFill filled it (its ScaleformMovieInstance and the movie in it): the list was seen
// empty after a cutscene, "No Documents Found" over "No Relics Found" (the movie as it loads, nothing put in it), the
// outfit grid over it: ListOnEnter's way back from the grid (v0.9.504) left the list as it was while its movie was a new
// one. Now that way is only for the movie filled, and the list's Process fills a new one.
void* g_listFilledInstance = nullptr;
void* g_listFilledMovie = nullptr;
int g_listRefills = 0;
void* StateMovieInstance(void* state);
std::vector<int> g_listItems;   // the g_items of the category shown
int g_listCategory = -1;        // its CategoryRank
int g_listIndex = 0;            // the entry highlighted
int g_listUnknownMouse = 0, g_listMouseLogs = 0;
bool g_listWaitLogged = false;
bool g_listLeave = false;       // Back on the grid: the list goes back too once it is the screen again
bool g_pickerTried = false;     // the grid opened over this visit's list once
// the live information panel (the picture)
constexpr std::uintptr_t kRvaShowLiveInfoWidget = 0x00590D70ull;
constexpr std::uint8_t kShowLiveInfoWidgetSig[] = {0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x80, 0x79, 0x13};
constexpr std::size_t kGsmInfoShown = 0x13;
constexpr std::uintptr_t kRvaWidgetGetVariable = 0x0098A680ull;   // UiWidgetScaleform::GetVariable(out, path, "")
constexpr std::size_t kWidgetMovieOffset = 0x98, kWidgetNameOffset = 0xA8;
// (v0.9.501) The panel at the game's own size, its picture's frame (Info_mc.image_loader_mc.guide_mc, 512 x 256: the
// title and the text lie on it) with its top left at kInfoLeft, kInfoTop of the stage. Widget_LiveInfo's own places (its
// GFX's PlaceObject tags): Widget_mc at (12.5, 377.35) on its stage, Info1_mc / Info2_mc at (552.55, 288.3) in it,
// image_loader_mc at (-564, -242.2) in those: the frame at (-11.45, 46.1) from Widget_mc's origin. 0.9.500 fitted the
// clip's whole bounds (1196 x 339 with its hidden score and challenge fields) into 460: the picture at a third of its size.
constexpr double kInfoLeft = 744.0, kInfoTop = 28.0, kInfoWidth = 512.0, kInfoHeight = 256.0;
constexpr double kInfoFrameX = 552.55 - 564.0, kInfoFrameY = 288.3 - 242.2;
double g_infoPlacedAt = 0.0;
int g_infoShowLogs = 0;
int g_infoPlaceLogs = 0;
void __fastcall ListInfoCallbackOp(void*, std::uint32_t id);
void* __fastcall ListInfoCallbackDtor(void* self, unsigned) { return self; }
void* g_listInfoCallbackVtable[3] = {reinterpret_cast<void*>(&ListInfoCallbackOp), reinterpret_cast<void*>(&ListInfoCallbackOp),
                                     reinterpret_cast<void*>(&ListInfoCallbackDtor)};
struct ListInfoCallback {
    void** vtable;
} g_listInfoCallback{g_listInfoCallbackVtable};
// the grid over the list
std::uintptr_t g_pickerStore[kModelGridEntries + 2] = {};
std::uintptr_t* g_pickerVtable = nullptr;   // the game's
void* g_pickerState = nullptr;
std::atomic<bool> g_pickerMode{false};
std::uint8_t g_pickerScreenByte = 0;        // its +0xD5 before
bool g_pickerByteSaved = false;
int g_pickerTile = -1;     // the tile of the outfit chosen
int g_pickerCursor = -1;   // (v0.9.503) the tile highlighted: the mouse's, the keys'
constexpr std::size_t kStateScreenFlags = 0xD5;   // bit 1: a full screen (UiMenuStateManager::ChangeState)
// (v0.9.504) eight a row at 40 % (400 x 360 on the stage) from x 778, under the panel, left of the legend's BACK (1186)
constexpr double kPickerScale = 0.40, kPickerTop = kInfoTop + kInfoHeight + 20.0, kPickerLeft = 778.0;
constexpr int kPickerColumns = 8;

std::vector<int> ListCategories() {
    std::vector<int> out;
    for (const auto& item : g_items) {
        const int rank = CategoryRank(item.category);
        if (std::find(out.begin(), out.end(), rank) == out.end()) out.push_back(rank);
    }
    return out;
}
std::string ListCategoryName(int rank) {
    for (const auto& item : g_items)
        if (CategoryRank(item.category) == rank) return ViewerText::ToUpper(item.category);
    return rank >= 0 && rank < kCategoryCount ? ViewerText::ToUpper(kCategories[rank]) : std::string();
}

void ListRestore() {
    if (g_listState && g_listVtable && VtableOf(g_listState) == &g_listStore[1])
        *reinterpret_cast<std::uintptr_t**>(g_listState) = g_listVtable;
    g_listMode = false;
}
void PickerMovieRestore();
void ListMouseClear();
// (v0.9.504) The grid's +0xD5 put back once it is neither the menu's screen nor the next one. Not from its OnExit:
// UiMenuStateManager::ChangeState (TR2 0x8F0B6C .. 0x8F0BC0) calls the left screen's OnExit, then the next screen's
// OnEnter when the left one (its +0xA38) has bit 1 set: put back there, it made the game enter the list again on Back
// (ListOnEnter made it anew and opened the grid over it again: Back went round, the UI piled up).
void PickerByteRestore() {
    if (!g_pickerByteSaved || !g_pickerState) return;
    if (void* mgr = StateManager()) {
        const std::int32_t current = At<std::int32_t>(mgr, kSmCurrentState), next = At<std::int32_t>(mgr, kSmNextState);
        if (current == kStateModelGrid || next == kStateModelGrid) return;
    }
    At<std::uint8_t>(g_pickerState, kStateScreenFlags) = g_pickerScreenByte;
    g_pickerByteSaved = false;
}
void PickerRestore(bool byteLater = false) {
    PickerMovieRestore();   // (v0.9.501)
    ListMouseClear();
    if (g_pickerState && g_pickerVtable && VtableOf(g_pickerState) == &g_pickerStore[1])
        *reinterpret_cast<std::uintptr_t**>(g_pickerState) = g_pickerVtable;
    if (!byteLater) {
        if (g_pickerState && g_pickerByteSaved) At<std::uint8_t>(g_pickerState, kStateScreenFlags) = g_pickerScreenByte;
        g_pickerByteSaved = false;
    }
    g_pickerMode = false;
    OgReleasePictures();
}

// -- the live information panel --
void __fastcall ListInfoCallbackOp(void*, std::uint32_t) {
    if (!g_listMode.load()) return;
    g_infoReady = true;
    g_infoPlacedAt = 0.0;
    UpdateInfo();
}
void* LiveInfoWidget() {
    void* gsm = GameStateMenuing();
    return gsm ? Widget(At<std::uint32_t>(gsm, kGsmInfoWidget)) : nullptr;
}
void ListShowInfo() {
    void* gsm = GameStateMenuing();
    if (!gsm || !GameBuild::VerifyBytes(kRvaShowLiveInfoWidget, kShowLiveInfoWidgetSig, sizeof(kShowLiveInfoWidgetSig))) {
        Log::Write("CutsceneViewer: no live information panel for the list (GameStateMenuing or its ShowLiveInfoWidget not there)");
        return;
    }
    g_infoReady = false;
    At<std::uint8_t>(gsm, kGsmInfoShown) = 0;   // (shown again: it returns at once while this is set)
    reinterpret_cast<void(__fastcall*)(void*, void*)>(Fn(kRvaShowLiveInfoWidget))(gsm, &g_listInfoCallback);
}
void ListHideInfo() {
    if (void* info = LiveInfoWidget())
        reinterpret_cast<void(__fastcall*)(void*, float, bool)>(VtableOf(info)[0x38 / 8])(info, 0.0f, false);
    if (void* gsm = GameStateMenuing()) At<std::uint8_t>(gsm, kGsmInfoShown) = 0;
    g_infoReady = false;
}
// The panel's clip (Widget_mc) at the game's own size, its picture's frame at (kInfoLeft, kInfoTop); logged where the
// frame is then.
void ListPlaceInfo() {
    void* info = LiveInfoWidget();
    if (!info || !g_infoReady || !At<void*>(info, kWidgetMovieOffset)) return;
    const char* name = static_cast<const char*>(info) + kWidgetNameOffset;
    if (!std::memchr(name, 0, 128) || !*name) return;
    using GetVariableFn = bool(__fastcall*)(void*, GfxValue*, const char*, const char*);
    const auto getVariable = reinterpret_cast<GetVariableFn>(Fn(kRvaWidgetGetVariable));
    char path[160];
    std::snprintf(path, sizeof(path), "_root.%s", name);
    GfxValue clip;
    if (!getVariable(info, &clip, path, "") || !GfxIsObject(clip)) {
        if (g_infoPlaceLogs++ < 3) Log::Writef("CutsceneViewer: the live information panel's clip (%s) not found", path);
        ReleaseValue(clip);
        return;
    }
    // (v0.9.505) shown again should anything have hidden it (UiWidgetBase vfunc +0x30, Show(time), as
    // GameStateMenuing::ShowLiveInfoWidget does with a panel already made)
    if (!reinterpret_cast<bool(__fastcall*)(void*)>(Fn(kRvaWidgetIsVisible))(info)) {
        reinterpret_cast<void(__fastcall*)(void*, float)>(VtableOf(info)[0x30 / 8])(info, 0.25f);
        if (g_infoShowLogs++ < 3) Log::Write("CutsceneViewer: the live information panel was hidden on the list's screen: shown again");
    }
    const double want[4] = {100.0, 100.0, kInfoLeft - kInfoFrameX, kInfoTop - kInfoFrameY};
    const char* const props[4] = {"_xscale", "_yscale", "_x", "_y"};
    double was[4];
    bool moved = false;
    for (int k = 0; k < 4; ++k) {
        was[k] = GfxNumber(clip, props[k]);
        if (!std::isfinite(was[k]) || std::fabs(was[k] - want[k]) > 0.05) {
            GfxSet(clip, props[k], Num(want[k]));
            moved = true;
        }
    }
    if (moved && g_infoPlaceLogs++ < 3) {
        GfxValue root, frame, bounds;
        double x0 = NAN, y0 = NAN, x1 = NAN, y1 = NAN;
        char framePath[220];
        std::snprintf(framePath, sizeof(framePath), "%s.Info1_mc.image_loader_mc.guide_mc", path);
        if (getVariable(info, &root, "_root", "") && GfxIsObject(root) && getVariable(info, &frame, framePath, "") &&
            GfxIsObject(frame) && GfxCall(frame, "getBounds", {root}, &bounds) && GfxIsObject(bounds)) {
            x0 = GfxNumber(bounds, "xMin"), y0 = GfxNumber(bounds, "yMin"), x1 = GfxNumber(bounds, "xMax"), y1 = GfxNumber(bounds, "yMax");
        }
        Log::Writef("CutsceneViewer: the live information panel (%s) was at %.1f, %.1f at %.0f %%; now at %.1f, %.1f at 100 %% "
                    "(its picture's frame at %.0f, %.0f to %.0f, %.0f on the stage)", path, was[2], was[3], was[0], want[2], want[3],
                    x0, y0, x1, y1);
        ReleaseValue(bounds);
        ReleaseValue(frame);
        ReleaseValue(root);
    }
    ReleaseValue(clip);
}

void ListHeaderSub(void* state) {
    char sub[160];
    std::snprintf(sub, sizeof(sub), "%s   %d / %d", ListCategoryName(g_listCategory).c_str(),
                  g_listItems.empty() ? 0 : g_listIndex + 1, int(g_listItems.size()));
    Movie(state, "SetHeaderSub", {Str(sub)});
}

void ListHighlight(void* state, int index, bool sound) {
    if (g_listItems.empty()) return;
    index = std::clamp(index, 0, int(g_listItems.size()) - 1);
    const bool moved = index != g_listIndex;
    g_listIndex = index;
    At<std::int32_t>(state, kStateSelected) = index;
    At<std::int32_t>(state, 0x30) = index;
    Movie(state, "OnHighlight", {Num(index), Str(""), Num(0.0)});
    g_pick = g_listItems[std::size_t(index)];
    ListHeaderSub(state);
    if (moved || !sound) UpdateInfo();
    if (sound && moved) PlaySoundId(state, kMoveSound);
    // (the ones beside it decoded too: the next one's picture at once)
    const int n = int(g_listItems.size());
    if (n > 1) {
        PictureFor(g_items[std::size_t(g_listItems[std::size_t((index + 1) % n)])]);
        PictureFor(g_items[std::size_t(g_listItems[std::size_t((index + n - 1) % n)])]);
    }
}

void ListRefocus(void* state) { ListHighlight(state, g_listIndex, false); }   // (the drawn grid's way back: unused on the list now)

// A category's entries (its first, or the cutscene picked when it is in it, highlighted).
void ListFill(void* state, int category) {
    g_listCategory = category;
    g_listItems.clear();
    for (int i = 0; i < int(g_items.size()); ++i)
        if (CategoryRank(g_items[std::size_t(i)].category) == category) g_listItems.push_back(i);
    Movie(state, "Init");
    Movie(state, "SetHeaderTitle", {Str(ViewerText::Upper(ViewerText::CutsceneViewer))});
    Movie(state, "AddCategory", {Str(ListCategoryName(category).c_str())});
    static std::vector<std::string> titles;   // (the movie copies them)
    titles.clear();
    for (const int i : g_listItems) {
        const auto& item = g_items[std::size_t(i)];
        titles.push_back(ViewerText::Localized(item.titles, item.title));
    }
    if (g_listItems.empty()) titles.push_back(ViewerText::Get(ViewerText::NoCutscenes));
    for (const std::string& title : titles) Movie(state, "AddNavItemRelic", {Str("None"), Str(title.c_str())});
    At<std::int32_t>(state, kStateNavCount) = int(titles.size());
    g_listFilledInstance = StateMovieInstance(state);   // (v0.9.513)
    g_listFilledMovie = g_listFilledInstance ? At<void*>(g_listFilledInstance, 0x08) : nullptr;   // (kInstanceMovie)
    int at = 0;
    for (int k = 0; k < int(g_listItems.size()); ++k)
        if (g_listItems[std::size_t(k)] == g_pick) at = k;
    g_listIndex = -1;
    ListHighlight(state, at, false);
}

void ListCategoryStep(void* state, int delta) {
    const std::vector<int> categories = ListCategories();
    if (categories.size() < 2) return;
    int at = 0;
    for (int k = 0; k < int(categories.size()); ++k)
        if (categories[std::size_t(k)] == g_listCategory) at = k;
    const int n = int(categories.size());
    const int next = categories[std::size_t((at + delta + n) % n)];
    for (int i = 0; i < int(g_items.size()); ++i)   // (its first cutscene)
        if (CategoryRank(g_items[std::size_t(i)].category) == next) {
            g_pick = i;
            break;
        }
    ListFill(state, next);
    PlaySoundId(state, kMoveSound);
}

void ListMove(void* state, int delta, bool held) {
    if (g_listItems.empty()) return;
    const int to = std::clamp(g_listIndex + delta, 0, int(g_listItems.size()) - 1);
    ListHighlight(state, to, true);
    if (void* mgr = StateManager())
        reinterpret_cast<void(__fastcall*)(void*, float)>(Fn(kRvaSetHeldInputBlockTimer))(mgr, held ? 0.06f : 0.3f);
}

void ListLegend(void* state) {
    AddLegend(state, {{kActSelect, "BL_Select", 2}, {kActRB, "StatusMenu_NextSet", 2}, {kActLB, "StatusMenu_PrevSet", 2},
                      {kActMenuClose, "Menu_Back", 3}});
}

bool EnterPicker();   // (the grid over the list, below)
void PickerByteRestore();
void ListBandStart();    // (v0.9.502: the dark top, below)
void ListBandTick();
void ListBandRemove(void* list);

bool __fastcall ListOnEnter(void* state, void* prev, void* cur) {
    // (v0.9.504) back from the grid over it: the list was not left (its OnExit not called), nothing to make again
    // (v0.9.513: only while its movie is the one filled; a new one is filled again below)
    void* instance = StateMovieInstance(state);
    const bool sameMovie = instance && instance == g_listFilledInstance && At<void*>(instance, 0x08) == g_listFilledMovie;
    if (prev && *static_cast<const int*>(prev) == kStateModelGrid && g_listMode.load() && g_listState == state && sameMovie) {
        Log::Write("CutsceneViewer: the list entered again on the way back from the outfit grid: left as it is");
        return true;
    }
    LoadItems();
    const bool entered = reinterpret_cast<bool(__fastcall*)(void*, void*, void*)>(Fn(kRvaGenericOnEnter))(state, prev, cur);
    if (!entered) return entered;
    if (g_pick < 0 || g_pick >= int(g_items.size())) g_pick = 0;
    const int category = g_items.empty() ? 0 : CategoryRank(g_items[std::size_t(g_pick)].category);
    ListFill(state, category);
    Movie(state, "PlayEnterAnimation", {Bool(false)});
    Movie(state, "SetNavigationEnabled", {Bool(true)});
    ListLegend(state);
    ListShowInfo();
    ListBandStart();   // (v0.9.502)
    g_viewerEnterTime = Now();
    g_listUnknownMouse = g_listMouseLogs = 0;
    g_infoPlaceLogs = 0;
    g_pickerTried = false;
    g_listLeave = false;
    Log::Writef("CutsceneViewer: Cutscene Viewer screen on the Relics list (%zu playable cutscenes in %zu categories, %zu outfits)",
                g_items.size(), ListCategories().size(), g_outfits.size());
    return entered;
}

void __fastcall ListPressed(void* state, int action, char) {
    if (!(At<std::uint8_t>(state, kStateInputFlags) & 8) || g_play.load() != Play::None) return;
    if (g_pickerMode.load()) return;   // (the grid over it has the keys: they move the list from there)
    switch (action) {
        case kActUp: ListMove(state, -1, false); break;
        case kActDown: ListMove(state, +1, false); break;
        case kActLB:
        case kActLT: ListCategoryStep(state, -1); break;
        case kActRB:
        case kActRT: ListCategoryStep(state, +1); break;
        case kActSelect: Activate(state); break;
        case kActCancel:
        case kActMenuClose:
        case kActMenuSelect:
            ModelGoBack(state);   // (the Extras menu: input blocked 0.3 s, SetStatePrevious)
            break;
        default: break;
    }
}

void __fastcall ListOn(void* state, int action, char) {
    if (!(At<std::uint8_t>(state, kStateInputFlags) & 8) || g_play.load() != Play::None) return;
    if (g_pickerMode.load()) return;
    if (action == kActUpHeld) ListMove(state, -1, true);
    else if (action == kActDownHeld) ListMove(state, +1, true);
}

void __fastcall ListPushHighlight(void* state, int index, int) {
    if (index >= 0 && index < int(g_listItems.size()) && index != g_listIndex) ListHighlight(state, index, false);
}

// (the list's movie's mouse; under the grid too when the game sends it there: logged a few times to see)
void __fastcall ListMouseOver(void* state, int id, int param, const char*) {
    if (g_play.load() != Play::None) return;
    if (g_pickerMode.load() && g_listMouseLogs++ < 3) Log::Writef("CutsceneViewer: the list's mouse under the grid: %d (%d)", id, param);
    if (id == kMouseListItem && param >= 0 && param < int(g_listItems.size()) && param != g_listIndex)
        ListHighlight(state, param, true);
}

void __fastcall ListMouseDown(void* state, int id, int param, const char*) {
    if (g_play.load() != Play::None) return;
    if (!g_pickerMode.load() && !(At<std::uint8_t>(state, kStateInputFlags) & 8)) return;
    if (id == kMouseListItem && param >= 0 && param < int(g_listItems.size())) {
        ListHighlight(state, param, false);
        Activate(state);
    } else if (g_listUnknownMouse++ < 6) {
        Log::Writef("CutsceneViewer: the list's movie sent Mouse_Button %d (param %d): not ours", id, param);
    }
}

void __fastcall ListProcess(void* state, float) {
    const double now = Now();
    ListBandTick();   // (v0.9.502)
    // (v0.9.513) its movie made again since it was filled (seen empty after a cutscene, the grid over it): filled again
    if (g_listMode.load() && g_listState == state && g_listFilledInstance) {
        void* instance = StateMovieInstance(state);
        if (instance && (instance != g_listFilledInstance || At<void*>(instance, 0x08) != g_listFilledMovie)) {
            ListFill(state, g_listCategory);
            Movie(state, "PlayEnterAnimation", {Bool(false)});
            Movie(state, "SetNavigationEnabled", {Bool(true)});
            ListLegend(state);
            if (g_listRefills++ < 5) Log::Write("CutsceneViewer: the list's movie was a new one (empty): filled again");
        }
    }
    if (g_infoReady && now - g_infoPlacedAt >= 0.25) {
        g_infoPlacedAt = now;
        ListPlaceInfo();
    }
    if (!IsCurrentState(state)) return;   // (under the grid: the grid's Process does the rest)
    if (g_listLeave) {   // (Back on the grid: the grid is gone, the list goes too)
        g_listLeave = false;
        ModelGoBack(state, 0.1f, false);   // (v0.9.505: quick, the sound was the grid's)
        return;
    }
    if (!g_pickerTried && g_viewerEnterTime > 0.0 && now - g_viewerEnterTime >= 0.3) {
        g_pickerTried = true;
        if (!EnterPicker()) Log::Write("CutsceneViewer: the outfit grid is not over the list (see above): Select plays with the "
                                       "outfit chosen before");
    }
}

void __fastcall ListPreExit(void* state, void* current, void* next, float time) {
    // (on the way under the grid nothing: UiMenuStateManager::ChangeState calls it even for a screen that is not a full
    // one, and UiMenuStateGeneric::OnPreExit would fade the list out, as the Model Viewer's grid under its view)
    if (next && *static_cast<const int*>(next) == kStateModelGrid && g_pickerMode.load()) return;
    reinterpret_cast<void(__fastcall*)(void*, void*, void*, float)>(Fn(kRvaGenericOnPreExit))(state, current, next, time);
    ListHideInfo();
}

bool __fastcall ListOnExit(void* state, void* current, void* next) {
    ListBandRemove(state);   // (v0.9.502; the movie is still there: ChangeState clears it after the next screen's OnEnter)
    const bool result = reinterpret_cast<bool(__fastcall*)(void*, void*, void*)>(Fn(kRvaGenericOnExit))(state, current, next);
    ListHideInfo();
    ListRestore();
    return result;
}

// -- the outfit grid over the list --
// (v0.9.501) The grid's movie drawn: 0.9.500's grid took the input but nothing of it was drawn over the list. What the
// game checks before it draws a movie, read in retail: ScaleformManager::Render (0x9EE741) leaves out a movie whose layer
// is disabled (the manager's +0x20 + layer * 4; the movie's +0x1C0 its layer, +0x1C4 drawn anyway);
// ScaleformMovieInstance::Render draws an instance shown (+0x2C8, 0x9EEB25; SetVisible 0x9F0060) and, of a pausable
// movie (+0x154, 0x9EEB66), nothing while the PauseQueue is paused (its +0x20; PauseQueue::Get 0x401F70). And the movie's
// own ActionScript: State_Base.onLoad hides the state's clip, its OnEnter shows it (and fades it in);
// Campsite_GearSelect.OnEnter fades _root in from 0 at _z 3000 in 0.5 s, a tween its frames run. Each checked every
// 0.25 s while the grid is over the list and put right (the fades once they had their time, 0.8 s), the movie's flags
// put back after; what was found logged once, after 1 s.
constexpr std::uintptr_t kRvaMovieInstanceVtable = 0x00DD4B70ull;   // ScaleformMovieInstance's vftable
constexpr std::uintptr_t kRvaMovieSetVisible = 0x009F0060ull, kRvaPauseQueueGet = 0x00401F70ull,
                         kRvaScaleformManagerGet = 0x0014A590ull;
constexpr std::uint8_t kMovieSetVisibleSig[] = {0x38, 0x91, 0xC8, 0x02, 0x00, 0x00, 0x74, 0x03, 0x88,
                                                0x51, 0x18, 0x88, 0x91, 0xC8, 0x02, 0x00, 0x00, 0xE9};
constexpr std::uint8_t kPauseQueueGetSig[] = {0x48, 0x8B, 0x05, 0xB1, 0x3B, 0x93, 0x02, 0xC3};
constexpr std::uint8_t kScaleformManagerGetSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x65, 0x48,
                                                    0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00};
constexpr std::size_t kStateMovieInstance = 0x10, kInstanceMovie = 0x08, kInstanceShown = 0x2C8;
constexpr std::size_t kMoviePausable = 0x154, kMovieLayer = 0x1C0, kMovieLayerExempt = 0x1C4;
constexpr std::size_t kManagerLayerFlags = 0x20, kPauseQueuePaused = 0x20;
constexpr int kLayerCount = 16;
struct MovieDraw {
    void* instance = nullptr;
    void* movie = nullptr;
    int shown = -1, pausable = -1, paused = -1, layer = -1, exempt = -1;
    std::uint32_t layerOff = 0;
};
struct MovieDrawCalls {
    bool setVisible = false, pauseQueue = false, manager = false;
};
MovieDrawCalls DrawCallsChecked() {   // (its own function: a static, not beside __try)
    static const MovieDrawCalls calls = [] {
        MovieDrawCalls c;
        c.setVisible = GameBuild::VerifyBytes(kRvaMovieSetVisible, kMovieSetVisibleSig, sizeof(kMovieSetVisibleSig));
        c.pauseQueue = GameBuild::VerifyBytes(kRvaPauseQueueGet, kPauseQueueGetSig, sizeof(kPauseQueueGetSig));
        c.manager = GameBuild::VerifyBytes(kRvaScaleformManagerGet, kScaleformManagerGetSig, sizeof(kScaleformManagerGetSig));
        if (!c.setVisible || !c.pauseQueue || !c.manager)
            Log::Writef("CutsceneViewer: the game's movie checks not all as expected (SetVisible %d, PauseQueue %d, "
                        "ScaleformManager %d): those not looked at", c.setVisible, c.pauseQueue, c.manager);
        return c;
    }();
    return calls;
}
void* StateMovieInstance(void* state) {
    void* instance = state ? At<void*>(state, kStateMovieInstance) : nullptr;
    return instance && reinterpret_cast<std::uintptr_t>(VtableOf(instance)) == Fn(kRvaMovieInstanceVtable) ? instance : nullptr;
}
// The state's movie's _root (ScaleformMovieInstance::GetVariable, its vfunc +0xA0: the prefix then the name).
bool StateMovieRoot(void* state, GfxValue& out) {
    void* instance = StateMovieInstance(state);
    if (!instance) return false;
    using GetVariableFn = bool(__fastcall*)(void*, GfxValue*, const char*, const char*);
    if (reinterpret_cast<GetVariableFn>(VtableOf(instance)[0xA0 / 8])(instance, &out, "_root", "") && GfxIsObject(out)) return true;
    ReleaseValue(out);
    return false;
}
bool ReadMovieDraw(void* instance, const MovieDrawCalls& calls, MovieDraw& out) {
    __try {
        auto* inst = static_cast<std::uint8_t*>(instance);
        out.instance = instance;
        out.shown = inst[kInstanceShown];
        auto* movie = *reinterpret_cast<std::uint8_t**>(inst + kInstanceMovie);
        if (!movie) return false;
        out.movie = movie;
        out.pausable = movie[kMoviePausable];
        out.layer = *reinterpret_cast<std::int32_t*>(movie + kMovieLayer);
        out.exempt = movie[kMovieLayerExempt];
        if (calls.pauseQueue) {
            auto* queue = reinterpret_cast<std::uint8_t*(__fastcall*)()>(Fn(kRvaPauseQueueGet))();
            out.paused = queue ? queue[kPauseQueuePaused] : -1;
        }
        if (calls.manager && out.layer >= 0 && out.layer < kLayerCount) {
            auto* manager = reinterpret_cast<std::uint8_t*(__fastcall*)()>(Fn(kRvaScaleformManagerGet))();
            out.layerOff = manager ? *reinterpret_cast<std::uint32_t*>(manager + kManagerLayerFlags + std::size_t(out.layer) * 4) : 0u;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SetMovieByte(void* movie, std::size_t offset, std::uint8_t value) {
    __try {
        static_cast<std::uint8_t*>(movie)[offset] = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool ShowMovieInstance(void* instance) {
    __try {
        reinterpret_cast<void(__fastcall*)(void*, bool, bool)>(Fn(kRvaMovieSetVisible))(instance, true, true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
int GfxBoolean(const GfxValue& object, const char* name) {
    GfxValue value;
    const int result = GfxGet(object, name, value) && (value.type & 0x8F) == 2 ? (*reinterpret_cast<const bool*>(&value.data) ? 1 : 0)
                                                                                : -1;
    ReleaseValue(value);
    return result;
}
// the grid's movie's flags changed by us (put back by PickerRestore)
void* g_pickerMovie = nullptr;
int g_pickerPausableWas = -1, g_pickerExemptWas = -1;
double g_pickerEnteredAt = 0.0, g_pickerCheckedAt = 0.0;
bool g_pickerDrawLogged = false;
unsigned g_pickerFixes = 0;   // what was put right (bits: the instance shown, pausable, layer, _root's fade, its clip)
int g_pickerLateFixLogs = 0;
std::string PickerFixesText(unsigned fixes) {
    std::string text;
    const char* const names[] = {"the movie instance shown (SetVisible)", "its pausable flag cleared (the game is paused)",
                                 "drawn though its layer is disabled", "_root's fade-in ended (alpha 100, _z 0)",
                                 "the state's clip shown", "the state's clip's alpha 100"};
    for (int k = 0; k < 6; ++k)
        if (fixes & (1u << k)) text += std::string(text.empty() ? "" : "; ") + names[k];
    return text.empty() ? "nothing put right" : text;
}
void PickerShow(void* state) {
    const double now = Now();
    if (now - g_pickerCheckedAt < 0.25) return;
    g_pickerCheckedAt = now;
    const double since = now - g_pickerEnteredAt;
    const unsigned fixesBefore = g_pickerFixes;
    const MovieDrawCalls calls = DrawCallsChecked();
    MovieDraw draw;
    void* instance = StateMovieInstance(state);
    const bool read = instance && ReadMovieDraw(instance, calls, draw);
    if (read) {
        if (draw.shown == 0 && calls.setVisible && ShowMovieInstance(instance)) g_pickerFixes |= 1u;
        if (draw.pausable == 1 && draw.paused == 1) {
            if (!g_pickerMovie || g_pickerMovie == draw.movie) {
                g_pickerMovie = draw.movie;
                if (g_pickerPausableWas < 0) g_pickerPausableWas = 1;
                if (SetMovieByte(draw.movie, kMoviePausable, 0)) g_pickerFixes |= 2u;
            }
        }
        if (draw.layerOff != 0 && draw.exempt == 0) {
            if (!g_pickerMovie || g_pickerMovie == draw.movie) {
                g_pickerMovie = draw.movie;
                if (g_pickerExemptWas < 0) g_pickerExemptWas = 0;
                if (SetMovieByte(draw.movie, kMovieLayerExempt, 1)) g_pickerFixes |= 4u;
            }
        }
    }
    GfxValue root;
    const bool haveRoot = StateMovieRoot(state, root);
    const double rootAlpha = haveRoot ? GfxNumber(root, "_alpha") : NAN, rootZ = haveRoot ? GfxNumber(root, "_z") : NAN;
    const int rootShown = haveRoot ? GfxBoolean(root, "_visible") : -1;
    GfxValue* clip = StateClip(state);
    const double clipAlpha = GfxNumber(*clip, "_alpha");
    const int clipShown = GfxBoolean(*clip, "_visible");
    if (since >= 0.8 && since < 2.5) {   // (the fades had their time: 0.5 s and the movie's show time; not its fade out)
        if (haveRoot && (!(rootAlpha >= 99.0) || (std::isfinite(rootZ) && std::fabs(rootZ) > 0.5) || rootShown == 0)) {
            GfxSet(root, "_alpha", Num(100.0));
            if (std::isfinite(rootZ)) GfxSet(root, "_z", Num(0.0));
            GfxSet(root, "_visible", Bool(true));
            g_pickerFixes |= 8u;
        }
        if (clipShown == 0) {
            GfxSet(*clip, "_visible", Bool(true));
            g_pickerFixes |= 16u;
        }
        if (std::isfinite(clipAlpha) && clipAlpha < 99.0) {
            GfxSet(*clip, "_alpha", Num(100.0));
            g_pickerFixes |= 32u;
        }
    }
    if (!g_pickerDrawLogged && since >= 1.0) {
        g_pickerDrawLogged = true;
        Log::Writef("CutsceneViewer: the outfit grid's movie after 1 s: %s: shown %d, pausable %d (the game paused %d), layer %d "
                    "(disabled 0x%X, drawn anyway %d); _root alpha %.0f, _z %.0f, visible %d; its clip alpha %.0f, visible %d, at "
                    "%.1f, %.1f, %.0f %%; %s",
                    instance ? (read ? "its instance read" : "its instance not readable")
                             : "its movie instance not the expected one (not checked)",
                    draw.shown, draw.pausable, draw.paused, draw.layer, draw.layerOff, draw.exempt, rootAlpha, rootZ, rootShown,
                    clipAlpha, clipShown, ClipNumber(state, "_x"), ClipNumber(state, "_y"), ClipNumber(state, "_xscale"),
                    PickerFixesText(g_pickerFixes).c_str());
    } else if (g_pickerDrawLogged && g_pickerFixes != fixesBefore && g_pickerLateFixLogs++ < 4) {
        Log::Writef("CutsceneViewer: the outfit grid's movie after %.1f s: %s", since, PickerFixesText(g_pickerFixes & ~fixesBefore).c_str());
    }
    ReleaseValue(root);
}
void PickerMovieRestore() {
    if (g_pickerMovie) {
        if (g_pickerPausableWas >= 0) SetMovieByte(g_pickerMovie, kMoviePausable, std::uint8_t(g_pickerPausableWas));
        if (g_pickerExemptWas >= 0) SetMovieByte(g_pickerMovie, kMovieLayerExempt, std::uint8_t(g_pickerExemptWas));
    }
    g_pickerMovie = nullptr;
    g_pickerPausableWas = g_pickerExemptWas = -1;
}

// (v0.9.501) The list under the grid: the game gives the mouse to the current screen's movie only (0.9.500: no
// Mouse_Button of the list's came while the grid was the screen), so its entries are hit here, on the game thread: the
// mouse's place on the grid's stage (its _root._xmouse / _ymouse; both movies are on the 1280 x 720 stage) against each
// entry's bounds on the list's stage (Entry_Container_mc.nav_list_item_<i>_mc, made by the movie's AddNavItemRelic),
// within the list's mask (ListContainer's, depth 17: x -475.8 .. 118.1, y -205.5 .. 271 in the list's clip). The entry
// under the mouse highlighted when the mouse moves; a click on it plays it, the wheel over the list moves its highlight
// (the window's messages, kept from the game).
constexpr double kListMaskX0 = -475.8, kListMaskX1 = 118.1, kListMaskY0 = -205.5, kListMaskY1 = 271.0;
std::atomic<int> g_listUnderMouse{-1};
std::atomic<bool> g_listMouseOver{false};
std::atomic<int> g_listClicked{-1};
std::atomic<int> g_listWheel{0};
std::atomic<bool> g_listPressKept{false};
double g_listMouseX = NAN, g_listMouseY = NAN;
// (v0.9.503) The wheel: the game reads the mouse by raw input (NxInputImpl::ProcessMouse, TR2 PDB) and makes the wheel
// the menus' Up / Down, which the grid's input gives the list (0.9.502: the wheel over the grid moved the list). Its raw
// input kept from the game while the grid is over the list; each notch to the list when the mouse is on the list's side
// of the stage (left of x 725, halfway between the list's right edge and the grid's left), else to the grid's rows (the
// movie's own MoveCursor, as its onMouseWheel: the rows seen move, the outfit chosen stays).
constexpr double kWheelSplitX = 725.0;
std::atomic<int> g_pickerMouseX{-1};   // the mouse's x on the stage (the grid's _root._xmouse), -1 not known
std::atomic<int> g_gridWheel{0};
std::atomic<bool> g_rawWheelSeen{false};
int g_listHitLogs = 0;
bool BoundsOf(const GfxValue& clip, const GfxValue& space, double out[4]) {
    GfxValue bounds;
    const bool got = GfxCall(clip, "getBounds", {space}, &bounds) && GfxIsObject(bounds);
    if (got) {
        out[0] = GfxNumber(bounds, "xMin"), out[1] = GfxNumber(bounds, "yMin");
        out[2] = GfxNumber(bounds, "xMax"), out[3] = GfxNumber(bounds, "yMax");
    }
    ReleaseValue(bounds);
    return got && std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]) && std::isfinite(out[3]);
}
void ListMouseClear() {
    g_listUnderMouse = -1;
    g_listMouseOver = false;
    g_listClicked = -1;
    g_listWheel = 0;
    g_gridWheel = 0;
    g_listMouseX = g_listMouseY = NAN;
}
void PickerListMouse(void* grid) {
    void* list = g_listState;
    if (!list || g_play.load() != Play::None) return;
    if (const int clicked = g_listClicked.exchange(-1); clicked >= 0 && clicked < int(g_listItems.size())) {
        ListHighlight(list, clicked, false);
        Activate(grid);
        return;
    }
    if (const int wheel = g_listWheel.exchange(0); wheel != 0) ListMove(list, wheel > 0 ? -1 : +1, false);
    if (const int wheel = g_gridWheel.exchange(0); wheel != 0) Movie(grid, "MoveCursor", {Num(double(-wheel))});   // (v0.9.503)
    GfxValue gridRoot, listRoot;
    if (!StateMovieRoot(grid, gridRoot) || !StateMovieRoot(list, listRoot)) {
        ReleaseValue(listRoot);
        ReleaseValue(gridRoot);
        return;
    }
    const double mx = GfxNumber(gridRoot, "_xmouse"), my = GfxNumber(gridRoot, "_ymouse");
    if (std::isfinite(mx) && std::isfinite(my) && (mx != g_listMouseX || my != g_listMouseY)) {
        g_listMouseX = mx;
        g_listMouseY = my;
        g_pickerMouseX = int(mx);
        // the list's clip on its stage (a scale and a place): its bounds in itself and on the stage
        GfxValue* clip = StateClip(list);
        double local[4], stage[4];
        bool over = false;
        int hit = -1, entries = 0;
        if (BoundsOf(*clip, *clip, local) && BoundsOf(*clip, listRoot, stage) && local[2] - local[0] > 1.0 && local[3] - local[1] > 1.0) {
            const double sx = (stage[2] - stage[0]) / (local[2] - local[0]), sy = (stage[3] - stage[1]) / (local[3] - local[1]);
            const double tx = stage[0] - local[0] * sx, ty = stage[1] - local[1] * sy;
            over = mx >= tx + kListMaskX0 * sx && mx < tx + kListMaskX1 * sx && my >= ty + kListMaskY0 * sy && my < ty + kListMaskY1 * sy;
            GfxValue container;
            if (over && GfxChild(*clip, "Entry_Container_mc", container)) {
                for (int i = 0; i < int(g_listItems.size()) && hit < 0; ++i) {
                    char name[48];
                    std::snprintf(name, sizeof(name), "nav_list_item_%d_mc", i);
                    GfxValue entry;
                    double b[4];
                    if (GfxChild(container, name, entry)) {
                        ++entries;
                        if (BoundsOf(entry, listRoot, b) && mx >= b[0] && mx < b[2] && my >= b[1] && my < b[3]) hit = i;
                    }
                    ReleaseValue(entry);
                }
            }
            ReleaseValue(container);
            if (over && g_listHitLogs < 2) {
                ++g_listHitLogs;
                Log::Detailf("CutsceneViewer: the mouse at %.0f, %.0f over the list under the grid (its clip at %.0f %% x %.0f %% from "
                            "%.1f, %.1f): entry %d (%d entries looked at)", mx, my, sx * 100.0, sy * 100.0, tx, ty, hit, entries);
            }
        }
        g_listMouseOver = over;
        g_listUnderMouse = hit;
        if (hit >= 0 && hit != g_listIndex) ListHighlight(list, hit, true);
    }
    ReleaseValue(listRoot);
    ReleaseValue(gridRoot);
}
// (the window's thread) A click on the list's entry under the mouse: that cutscene played; the wheel over the list.
bool ListMouseMessage(UINT msg) {
    if (!g_pickerMode.load() || g_play.load() != Play::None) return false;
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
        const int entry = g_listUnderMouse.load();
        if (entry < 0) return false;
        g_listClicked = entry;
        g_listPressKept = true;
        return true;
    }
    if (msg == WM_LBUTTONUP) return g_listPressKept.exchange(false);
    return false;
}

// (v0.9.503, the window's thread) The wheel while the grid is over the list: its raw input (a packet with the wheel
// alone) kept from the game, each notch given to the list or the grid by the mouse's side; WM_MOUSEWHEEL kept too
// (counted only when no raw wheel came).
void RouteWheel(int delta) {
    if (delta == 0) return;
    const int step = delta > 0 ? 1 : -1;
    const int x = g_pickerMouseX.load();
    if (x >= 0 && x < int(kWheelSplitX)) g_listWheel += step;
    else g_gridWheel += step;
}
bool PickerWheelMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!g_pickerMode.load() || g_play.load() != Play::None) return false;
    if (msg == WM_INPUT) {
        RAWINPUT raw{};
        UINT size = sizeof(raw);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == UINT(-1) ||
            raw.header.dwType != RIM_TYPEMOUSE || !(raw.data.mouse.usButtonFlags & RI_MOUSE_WHEEL))
            return false;
        g_rawWheelSeen = true;
        RouteWheel(static_cast<short>(raw.data.mouse.usButtonData));
        return raw.data.mouse.usButtonFlags == RI_MOUSE_WHEEL && raw.data.mouse.lLastX == 0 && raw.data.mouse.lLastY == 0;
    }
    if (msg == WM_MOUSEWHEEL) {
        if (!g_rawWheelSeen.load()) RouteWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return true;
    }
    return false;
}

// (v0.9.508) The dark top the user asked for: the list on black as the game's Relics list in game, where it is drawn over
// the pause screen with nothing of the world behind it: its own background (trx_collectible_list's bg_mc: the band, a 50 %
// gradient from black to #182C2E down from y 362 of the stage, "the blue one at the bottom"; the map and the crosshairs,
// adding their lines) then shows as there: black above, its blue below, the lines on it. So the main menu's room is left
// out of the drawing while the list is up (ModelViewer::SetBackdropHidden: the hooks the Model Viewer's black background
// is made with, Scene::DrawEntity / DrawStaticMeshes / DrawGrass, every scene) and the list's movie is left as the game
// made it. 0.9.502 - 507 worked on the band instead: copies of it (duplicateMovieClip; made, in place, shown, never drawn),
// then the band itself over the whole screen (at 50 % it only dimmed the room; at _alpha 200 the game's renderer drew it
// past opaque, the room showing through as in a negative).
// The game's movies drawn over the list's (the panel's, the grid's) kept drawn after it: ScaleformManager's movies (its
// array +8, count +0, its lock +0x230; retail 0x9EE705 .. 0x9EE726) are drawn in the array's order, which is sorted when a
// movie comes (ScaleformManager::Process) by MoviePriorityCompare (retail 0x9ECB20: +0x50, a word; v0.9.505: the lower
// first, as the game's sorts showed: 0.9.502 - 504 gave a movie the list's priority less one, which put it back under the
// list at each sort); one drawn before the list's is moved after it and given the list's priority and one (put back when
// the list goes).
// (v0.9.509) Not at once: the room fades to black as the list comes (ModelViewer::SetBackdropDarkness: the tone mapping's
// exposure, the 3D frame alone, the menus drawn over it as they are), then is left out; on the way back it is drawn
// again at black and fades in. kBackdropFade each way, eased; driven every tick (BackdropTick, from TickGameThread: the
// way back goes on after the list's screen is gone). Not the game's screen wipe (GAMELOOP_SetScreenWipe): its quad is
// drawn with the UI primitives after the Scaleform menus (GameRender::EndScene, KickUIPrims after
// ScaleformManager::Render), over them.
constexpr std::size_t kManagerMovieCount = 0x00, kManagerMovies = 0x08, kManagerLock = 0x230, kMoviePriority = 0x50;
constexpr double kBackdropFade = 0.4;
enum class BackdropState { Shown, FadingOut, Hidden, FadingIn };
BackdropState g_backdropState = BackdropState::Shown;
double g_backdropFadeAt = 0.0;   // (the fade's start, set back so that it goes on from where the other left off)
float g_backdropDark = 0.0f;
double g_orderAt = 0.0, g_backdropAt = 0.0;
bool g_backdropHidden = false, g_backdropLogged = false;
int g_orderLogs = 0;
bool g_orderSeen[2] = {false, false};
struct PriorityWas {
    void* movie;
    std::int16_t priority;
};
std::vector<PriorityWas> g_priorityWas;

// The movie moved after the list's in the manager's array (drawn later: over it); its priority made the list's less
// one so that the game's next sort keeps it there. 1 moved, 0 already after, -1 not found (or not readable).
int KeepDrawnAfterLocked(std::uint8_t* manager, void* listMovie, void* movie, std::int16_t& was, int& from, int& to) {
    const std::uint32_t count = *reinterpret_cast<std::uint32_t*>(manager + kManagerMovieCount);
    void** movies = *reinterpret_cast<void***>(manager + kManagerMovies);
    int list = -1, other = -1;
    for (std::uint32_t i = 0; movies && i < count && i < 1024; ++i) {
        if (movies[i] == listMovie) list = int(i);
        if (movies[i] == movie) other = int(i);
    }
    from = other;
    to = list;
    if (list < 0 || other < 0) return -1;
    if (other > list) return 0;
    auto* priority = reinterpret_cast<std::int16_t*>(static_cast<std::uint8_t*>(movie) + kMoviePriority);
    const std::int16_t listPriority = *reinterpret_cast<std::int16_t*>(static_cast<std::uint8_t*>(listMovie) + kMoviePriority);
    was = *priority;
    if (*priority <= listPriority && listPriority < INT16_MAX) *priority = std::int16_t(listPriority + 1);   // (v0.9.505)
    void* moving = movies[other];
    for (int k = other; k < list; ++k) movies[k] = movies[k + 1];
    movies[list] = moving;
    return 1;
}
int KeepDrawnAfter(void* listMovie, void* movie, std::int16_t& was, int& from, int& to) {
    __try {
        auto* manager = reinterpret_cast<std::uint8_t*(__fastcall*)()>(Fn(kRvaScaleformManagerGet))();
        if (!manager) return -1;
        auto* lock = reinterpret_cast<CRITICAL_SECTION*>(manager + kManagerLock);
        EnterCriticalSection(lock);
        int result = -1;
        __try {
            result = KeepDrawnAfterLocked(manager, listMovie, movie, was, from, to);
        } __finally {
            LeaveCriticalSection(lock);
        }
        return result;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
bool SetMoviePriority(void* movie, std::int16_t priority) {
    __try {
        *reinterpret_cast<std::int16_t*>(static_cast<std::uint8_t*>(movie) + kMoviePriority) = priority;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void* InstanceMovie(void* instance) {
    return instance && reinterpret_cast<std::uintptr_t>(VtableOf(instance)) == Fn(kRvaMovieInstanceVtable)
               ? At<void*>(instance, kInstanceMovie)
               : nullptr;
}
void ListDrawOrder() {
    if (!DrawCallsChecked().manager) return;
    void* listMovie = InstanceMovie(StateMovieInstance(g_listState));
    if (!listMovie) return;
    void* info = LiveInfoWidget();
    void* const movies[2] = {info ? InstanceMovie(At<void*>(info, kWidgetMovieOffset)) : nullptr,
                             g_pickerMode.load() ? InstanceMovie(StateMovieInstance(g_pickerState)) : nullptr};
    const char* const names[2] = {"the picture's panel", "the outfit grid"};
    for (int k = 0; k < 2; ++k) {
        if (!movies[k] || movies[k] == listMovie) continue;
        std::int16_t was = 0;
        int from = -1, to = -1;
        const int result = KeepDrawnAfter(listMovie, movies[k], was, from, to);
        if (result == 1) {
            bool kept = false;
            for (const PriorityWas& p : g_priorityWas) kept |= p.movie == movies[k];
            if (!kept) g_priorityWas.push_back({movies[k], was});
            if (g_orderLogs++ < 6)
                Log::Detailf("CutsceneViewer: %s's movie was drawn before the list's (%d of the movies, the list's %d; priority %d): "
                            "moved after it", names[k], from, to, int(was));
        } else if (!g_orderSeen[k]) {
            g_orderSeen[k] = true;
            Log::Detailf("CutsceneViewer: %s's movie %s", names[k],
                        result == 0 ? "is drawn after the list's" : "or the list's not among the game's movies (not moved)");
        }
    }
}
void PriorityRestore() {
    for (const PriorityWas& p : g_priorityWas) SetMoviePriority(p.movie, p.priority);
    g_priorityWas.clear();
}

// (v0.9.509) The room's fade, every tick.
float Eased(double t) {
    const double x = std::clamp(t, 0.0, 1.0);
    return float(x * x * (3.0 - 2.0 * x));
}
void BackdropTick() {
    const double now = Now();
    switch (g_backdropState) {
        case BackdropState::FadingOut: {
            const double t = (now - g_backdropFadeAt) / kBackdropFade;
            g_backdropDark = Eased(t);
            ModelViewer::SetBackdropDarkness(g_backdropDark);
            if (t >= 1.0) {
                g_backdropHidden = ModelViewer::SetBackdropHidden(true);
                g_backdropState = BackdropState::Hidden;
                g_backdropAt = now;
                g_backdropLogged = false;
                ModelViewer::TakeBackdropHiddenDraws();
                Log::Writef("CutsceneViewer: the main menu's room faded out behind the list (%u frames dimmed by the tone "
                            "mapping), then left out%s", ModelViewer::TakeBackdropDimmed(),
                            g_backdropHidden ? "" : " (not: the Model Viewer's scene hooks are not there)");
            }
            break;
        }
        case BackdropState::FadingIn: {
            const double t = (now - g_backdropFadeAt) / kBackdropFade;
            g_backdropDark = 1.0f - Eased(t);
            ModelViewer::SetBackdropDarkness(g_backdropDark);
            if (t >= 1.0) {
                g_backdropDark = 0.0f;
                ModelViewer::SetBackdropDarkness(0.0f);
                g_backdropState = BackdropState::Shown;
                Log::Writef("CutsceneViewer: the main menu's room faded in again (%u frames dimmed)", ModelViewer::TakeBackdropDimmed());
            }
            break;
        }
        case BackdropState::Hidden:
            if (g_backdropHidden && !g_backdropLogged && now - g_backdropAt >= 2.0) {   // (that it is so)
                g_backdropLogged = true;
                Log::Writef("CutsceneViewer: the dark top: the main menu's room left out behind the list (%u of its entities and "
                            "mesh passes left out in 2 s)", ModelViewer::TakeBackdropHiddenDraws());
            }
            break;
        default:
            break;
    }
}

void ListBandStart() {
    g_orderSeen[0] = g_orderSeen[1] = false;
    g_orderLogs = 0;   // (v0.9.505: each visit's)
    g_orderAt = 0.0;
    // (v0.9.509) the room fades out from where it is (a fade in under way turned round)
    if (g_backdropState == BackdropState::Shown || g_backdropState == BackdropState::FadingIn) {
        ModelViewer::TakeBackdropDimmed();
        g_backdropFadeAt = Now() - kBackdropFade * double(g_backdropDark);
        g_backdropState = BackdropState::FadingOut;
    }
    ListBandTick();
}
void ListBandTick() {
    void* list = g_listState;
    if (!list || !g_listMode.load()) return;
    const double now = Now();
    if (now - g_orderAt < 0.25) return;
    g_orderAt = now;
    ListDrawOrder();
}
void ListBandRemove(void*) {
    // (v0.9.509) the room drawn again, at black, and faded in
    if (g_backdropState == BackdropState::Hidden || g_backdropState == BackdropState::FadingOut) {
        if (g_backdropHidden) ModelViewer::SetBackdropHidden(false);
        g_backdropHidden = false;
        ModelViewer::TakeBackdropDimmed();
        g_backdropFadeAt = Now() - kBackdropFade * double(1.0f - g_backdropDark);
        g_backdropState = BackdropState::FadingIn;
    }
    PriorityRestore();
}

void PickerLegend(void* state) {
    AddLegend(state, {{kActSelect, "BL_Select", 2}, {kActRB, "StatusMenu_NextSet", 2}, {kActLB, "StatusMenu_PrevSet", 2},
                      {kActMenuClose, "Menu_Back", 3}});
}

// A tile's check mark (the campsite's mark of the outfit worn): on the outfit chosen only.
void PickerMarks(void* state) {
    GfxValue tiles;
    if (!GfxChild(*StateClip(state), "campsite_outfit_select_mc", tiles)) return;
    for (int i = 0; i < int(g_ogTiles.size()); ++i) {
        char name[48];
        std::snprintf(name, sizeof(name), "nav_list_item_%d_mc", i);
        GfxValue item, mark;
        if (GfxChild(tiles, name, item) && GfxChild(item, "checkmark_mc", mark)) {
            const bool chosen = g_ogTiles[std::size_t(i)].choice == g_outfitPick;
            GfxSet(mark, "_visible", Bool(chosen));
            if (chosen) GfxCall(mark, "gotoAndStop", {Str("ON")});   // (v0.9.503: checked)
        }
        ReleaseValue(mark);
        ReleaseValue(item);
    }
    ReleaseValue(tiles);
}

// (v0.9.503) The tile highlighted (the mouse over it, the game's highlight): its look only, the outfit chosen stays
// (0.9.500 - 502 chose the outfit under the mouse).
void PickerCursor(void* state, int tile, bool sound) {
    const int n = int(g_ogTiles.size());
    if (!n) return;
    tile = std::clamp(tile, 0, n - 1);
    if (tile == g_pickerCursor) return;
    g_pickerCursor = tile;   // (first: SetSelectionIndex calls PushHighlightIndexToMovie, PickerHighlight, back here)
    reinterpret_cast<void(__fastcall*)(void*, int)>(Fn(kRvaGenericSetSelection))(state, tile);
    Movie(state, "OnHighlight", {Num(tile), Bool(false)});
    Movie(state, "OnIndexSelected", {Num(tile)});   // (it scrolls the grid to the tile)
    if (sound) PlaySoundId(state, kMoveSound);
}

// The outfit chosen (a click on its tile, Left / Right): highlighted too, its check mark checked (the movie's own
// SetCheckMarkVisible: the tile's item state checkmark, UpdateItemState puts checkmark_mc at its frame ON; the mark of
// the one before back to OFF), the panel names it.
void PickerSelect(void* state, int tile, bool sound) {
    const int n = int(g_ogTiles.size());
    if (!n) return;
    tile = std::clamp(tile, 0, n - 1);
    PickerCursor(state, tile, false);
    if (tile == g_pickerTile) return;
    g_pickerTile = tile;
    g_outfitPick = g_ogTiles[std::size_t(tile)].choice;
    Movie(state, "SetCheckMarkVisible", {Num(tile), Bool(true)});
    PickerMarks(state);
    UpdateInfo();   // (the panel names the outfit)
    if (sound) PlaySoundId(state, kSelectSound);
}

void PickerStep(void* state, int delta, bool held) {
    PickerSelect(state, std::max(0, g_pickerTile) + delta, true);
    if (void* mgr = StateManager())
        reinterpret_cast<void(__fastcall*)(void*, float)>(Fn(kRvaSetHeldInputBlockTimer))(mgr, held ? kGridRepeat : kGridFirstRepeat);
}

void PickerFill(void* state) {
    OgReleasePictures();
    for (int i = 0; i < int(g_outfits.size()); ++i) {
        OutfitTile tile;
        tile.choice = i;
        if (g_outfits[std::size_t(i)].index >= 0) {
            tile.data = Features::OutfitData(g_outfits[std::size_t(i)].index);
            const char* action = "";
            if (tile.data) PictureLayoutOf(tile.data, tile.layout, action);
            tile.request = tile.data ? AskOutfitPicture(tile.data) : -1;
        }
        g_ogTiles.push_back(tile);
    }
    Movie(state, "ResetAllImages");
    Movie(state, "Reset");
    const int n = int(g_ogTiles.size());
    for (int i = 0; i < n; ++i) Movie(state, "AddOutfitItem", {Str(""), Str("Complete"), Bool(true)});
    Movie(state, "RegisterNavListItems");
    At<std::int32_t>(state, kStateNavCount) = std::int32_t(n);
    At<std::int32_t>(state, kStateSelected) = -1;
    g_pickerTile = -1;
    g_pickerCursor = -1;
}

void PickerPictures(void* state) {
    for (int i = 0; i < int(g_ogTiles.size()); ++i) {
        OutfitTile& t = g_ogTiles[std::size_t(i)];
        if (!t.texture || t.sent) continue;
        t.sent = true;
        const char* action = "";
        int layout[4];
        PictureLayoutOf(t.data, layout, action);
        Movie(state, "LoadOutfitImage", {Num(i), Num(double(t.texture)), Num(layout[0]), Num(layout[1]), Num(layout[2]),
                                         Num(layout[3]), Str(action)});
    }
}

void PickerHideWidgets(void* state, bool remove) {
    for (const std::size_t at : {kGearWeaponInfo, kGearOutfitInfo}) {
        ShowWidget(GridWidget(state, at), false, remove);
        if (remove) At<std::uint32_t>(state, at) = 0xFFFFFFFFu;
    }
}

bool __fastcall PickerOnEnter(void* state, void* prev, void* cur) {
    At<std::int32_t>(state, kGearCategory) = kGearOutfitsCategory;   // (the outfits' grid: SetupInformationWidget)
    const bool entered = reinterpret_cast<bool(__fastcall*)(void*, void*, void*)>(Fn(kRvaGenericOnEnter))(state, prev, cur);
    if (!entered) return entered;
    At<std::uint32_t>(state, kGearWeaponInfo) = At<std::uint32_t>(state, kGearOutfitInfo) = 0xFFFFFFFFu;
    reinterpret_cast<void(__fastcall*)(void*)>(Fn(kRvaGearSetupInfo))(state);   // (the movie in its outfit state)
    PickerHideWidgets(state, false);
    PickerLegend(state);
    g_gridLayoutOverride = true;   // (under the picture: GridLayout's place and size ours, its six by five as the Model Viewer's)
    g_gridLayoutScale = kPickerScale;
    g_gridLayoutLeft = kPickerLeft;
    g_gridLayoutColumns = kPickerColumns;
    g_gridLayoutTop = kPickerTop;
    GridLayout(state);
    g_gridLayoutOverride = false;
    PickerFill(state);
    PickerSelect(state, std::clamp(g_outfitPick, 0, std::max(0, int(g_ogTiles.size()) - 1)), false);
    g_pickerEnteredAt = Now();   // (v0.9.501)
    g_pickerCheckedAt = 0.0;
    g_pickerDrawLogged = false;
    g_pickerFixes = 0;
    g_pickerLateFixLogs = 0;
    g_listHitLogs = 0;
    ListMouseClear();
    PickerShow(state);
    Log::Writef("CutsceneViewer: the campsite's outfit grid over the list (%zu outfits, the chosen one %d)", g_ogTiles.size(),
                g_outfitPick);
    return entered;
}

void PickerLeave(void* state) {
    g_listLeave = true;   // (the list goes back too, once it is the screen again)
    ModelGoBack(state, 0.0f);   // (v0.9.505: at once, no fade: the list goes right after it)
}

void __fastcall PickerPressed(void* state, int action, char) {
    if (!(At<std::uint8_t>(state, kStateInputFlags) & 8) || g_play.load() != Play::None) return;
    switch (action) {
        case kActUp: if (g_listState) ListMove(g_listState, -1, false); break;
        case kActDown: if (g_listState) ListMove(g_listState, +1, false); break;
        case kActLeft: PickerStep(state, -1, false); break;
        case kActRight: PickerStep(state, +1, false); break;
        case kActLB:
        case kActLT: if (g_listState) ListCategoryStep(g_listState, -1); break;
        case kActRB:
        case kActRT: if (g_listState) ListCategoryStep(g_listState, +1); break;
        case kActSelect: Activate(state); break;
        case kActCancel:
        case kActMenuClose:
        case kActMenuSelect:
            PickerLeave(state);
            break;
        default: break;
    }
}

void __fastcall PickerOn(void* state, int action, char) {
    if (!(At<std::uint8_t>(state, kStateInputFlags) & 8) || g_play.load() != Play::None) return;
    if (action == kActUpHeld && g_listState) ListMove(g_listState, -1, true);
    else if (action == kActDownHeld && g_listState) ListMove(g_listState, +1, true);
    else if (action == kActLeftHeld) PickerStep(state, -1, true);
    else if (action == kActRightHeld) PickerStep(state, +1, true);
}

void __fastcall PickerHighlight(void* state, int index, int) {
    if (index >= 0 && index < int(g_ogTiles.size())) PickerCursor(state, index, false);
}

void __fastcall PickerMouseOver(void* state, int id, int param, const char*) {
    if (g_play.load() != Play::None) return;
    if (id == kMouseGridItem && param >= 0 && param < int(g_ogTiles.size())) PickerCursor(state, param, true);   // (v0.9.503)
}

void __fastcall PickerMouseDown(void* state, int id, int param, const char*) {
    if (!(At<std::uint8_t>(state, kStateInputFlags) & 8) || g_play.load() != Play::None) return;
    if (id == kMouseGridItem && param >= 0 && param < int(g_ogTiles.size())) PickerSelect(state, param, true);
}

void __fastcall PickerProcess(void* state, float) {
    PumpPictures("");
    PickerPictures(state);
    const double now = Now();
    if (now - g_gridPlacedAt >= 0.25) {
        PlaceGrid(state);
        PickerHideWidgets(state, false);   // (kept hidden should the movie show them)
        PickerMarks(state);                // (OnHighlight's item states may have moved a mark)
    }
    PickerShow(state);   // (v0.9.501: drawn)
    // (v0.9.501) the panel kept in place: the game calls the Process of the current screen alone, the list's not now
    if (g_infoReady && now - g_infoPlacedAt >= 0.25) {
        g_infoPlacedAt = now;
        ListPlaceInfo();
    }
    PickerListMouse(state);   // (v0.9.501: the list's entries under the mouse)
    ListBandTick();           // (v0.9.502: the list's Process is not called under the grid)
}

void __fastcall PickerPreExit(void* state, void* current, void* next, float time) {
    reinterpret_cast<void(__fastcall*)(void*, void*, void*, float)>(Fn(kRvaGenericOnPreExit))(state, current, next, time);
    PickerHideWidgets(state, true);
    Movie(state, "AnimateOut", {Num(0.01)});
}

bool __fastcall PickerOnExit(void* state, void* current, void* next) {
    const bool result = reinterpret_cast<bool(__fastcall*)(void*, void*, void*)>(Fn(kRvaGenericOnExit))(state, current, next);
    PickerRestore(true);   // (v0.9.504: its full-screen bit after the state change: PickerByteRestore)
    return result;
}

bool EnterPicker() {
    if (!MovieAvailable(kStateModelGrid) || !WidgetMovieAvailable(kInfoWidgetType)) {
        Log::Write("CutsceneViewer: the campsite's outfit grid's movie is not there");
        return false;
    }
    if (g_modelGridMode.load()) return false;
    void* grid = StateObject(kStateModelGrid);
    if (!grid || !StateManager()) return false;
    std::uintptr_t* vtable = VtableOf(grid);
    if (vtable != &g_pickerStore[1] && reinterpret_cast<std::uintptr_t>(vtable) != Fn(kRvaModelGridVtable)) {
        Log::Write("CutsceneViewer: the campsite gear screen's object is not the expected one");
        return false;
    }
    if (vtable != &g_pickerStore[1]) {
        g_pickerVtable = vtable;
        g_pickerStore[0] = vtable[-1];
        std::uintptr_t* clone = &g_pickerStore[1];
        std::memcpy(clone, vtable, kModelGridEntries * sizeof(std::uintptr_t));
        clone[0x08 / 8] = Fn(kRvaNop);   // (OnUiSelect: the outfit worn)
        clone[0x18 / 8] = reinterpret_cast<std::uintptr_t>(&PickerOnEnter);
        clone[0x20 / 8] = reinterpret_cast<std::uintptr_t>(&PickerPressed);
        clone[0x28 / 8] = reinterpret_cast<std::uintptr_t>(&PickerOn);
        clone[0x30 / 8] = reinterpret_cast<std::uintptr_t>(&PickerProcess);
        clone[0x38 / 8] = reinterpret_cast<std::uintptr_t>(&PickerPreExit);
        clone[0x40 / 8] = reinterpret_cast<std::uintptr_t>(&PickerOnExit);
        clone[0x80 / 8] = Fn(kRvaGenericSetSelection);
        clone[0x88 / 8] = reinterpret_cast<std::uintptr_t>(&PickerHighlight);
        clone[0xA0 / 8] = reinterpret_cast<std::uintptr_t>(&PickerMouseDown);
        clone[0xB0 / 8] = reinterpret_cast<std::uintptr_t>(&PickerMouseOver);
        clone[0xB8 / 8] = Fn(kRvaNop);   // (OnMouseOut)
        for (std::size_t slot = 0xF0; slot < kModelGridEntries * 8; slot += 8)   // (the campsite's own work)
            clone[slot / 8] = reinterpret_cast<std::uintptr_t>(&ReturnZero);
        *reinterpret_cast<std::uintptr_t**>(grid) = clone;
    }
    g_pickerState = grid;
    if (!g_pickerByteSaved) {   // (v0.9.504: not while the one before is still to be put back)
        g_pickerScreenByte = At<std::uint8_t>(grid, kStateScreenFlags);
        g_pickerByteSaved = true;
    }
    At<std::uint8_t>(grid, kStateScreenFlags) = std::uint8_t(g_pickerScreenByte & ~2u);   // (not a full screen: the list stays)
    g_pickerMode = true;
    if (!SetMenuState(kStateModelGrid, 0.25f)) {
        PickerRestore();
        Log::Write("CutsceneViewer: the game refused to open the outfit grid over the list");
        return false;
    }
    return true;
}

// (game thread, every tick) Neither screen the current one any more (a cutscene's level loads, the main menu went):
// their vtables and the grid's full-screen bit put back, so that the game's own Relics list and campsite grid are theirs.
void ListSafetyRestore() {
    PickerByteRestore();   // (v0.9.504)
    if (!g_listMode.load() && !g_pickerMode.load()) return;
    void* mgr = StateManager();
    if (!mgr) return;
    const std::int32_t current = At<std::int32_t>(mgr, kSmCurrentState), next = At<std::int32_t>(mgr, kSmNextState);
    const bool ours = current == kStateList || current == kStateModelGrid || next == kStateList || next == kStateModelGrid;
    if (ours) return;
    if (g_pickerMode.load()) PickerRestore();
    if (g_listMode.load()) ListRestore();
    ListBandRemove(nullptr);   // (v0.9.502: the movies' priorities put back)
    Log::Writef("CutsceneViewer: the list and the outfit grid put back as the game's (the menu's state %d)", current);
}

bool EnterCutsceneList() {
    void* list = StateObject(kStateList);
    if (!list || !StateManager()) return false;
    std::uintptr_t* vtable = VtableOf(list);
    if (vtable != &g_listStore[1] && reinterpret_cast<std::uintptr_t>(vtable) != Fn(kRvaListVtable)) {
        Log::Write("CutsceneViewer: the Relics list's object is not the expected one; the Expeditions screen instead");
        return false;
    }
    if (vtable != &g_listStore[1]) {
        g_listVtable = vtable;
        g_listStore[0] = vtable[-1];
        std::uintptr_t* clone = &g_listStore[1];
        std::memcpy(clone, vtable, kListEntries * sizeof(std::uintptr_t));
        clone[0x18 / 8] = reinterpret_cast<std::uintptr_t>(&ListOnEnter);
        clone[0x20 / 8] = reinterpret_cast<std::uintptr_t>(&ListPressed);
        clone[0x28 / 8] = reinterpret_cast<std::uintptr_t>(&ListOn);
        clone[0x30 / 8] = reinterpret_cast<std::uintptr_t>(&ListProcess);
        clone[0x38 / 8] = reinterpret_cast<std::uintptr_t>(&ListPreExit);
        clone[0x40 / 8] = reinterpret_cast<std::uintptr_t>(&ListOnExit);
        clone[0x80 / 8] = Fn(kRvaGenericSetSelection);
        clone[0x88 / 8] = reinterpret_cast<std::uintptr_t>(&ListPushHighlight);
        clone[0xA0 / 8] = reinterpret_cast<std::uintptr_t>(&ListMouseDown);
        clone[0xB0 / 8] = reinterpret_cast<std::uintptr_t>(&ListMouseOver);
        clone[0xB8 / 8] = Fn(kRvaNop);   // (OnMouseOut)
        for (const std::size_t slot : {0xF0, 0x100, 0x108, 0x110})   // (the relics' own work: nothing)
            clone[slot / 8] = Fn(kRvaNop);
        clone[0x118 / 8] = reinterpret_cast<std::uintptr_t>(&ReturnZero);   // (AddCollectiblesOfType)
        *reinterpret_cast<std::uintptr_t**>(list) = clone;
    }
    g_listState = list;
    g_listMode = true;
    g_viewerEnterTime = 0.0;
    if (!SetMenuState(kStateList, 0.25f)) {
        ListRestore();
        Log::Write("CutsceneViewer: the game refused to open the Relics list; the Expeditions screen instead");
        return false;
    }
    return true;
}

// The in-game menu collection there (the list's movie with it): the list, else the Expeditions screen.
void ListCollectionsReady() {
    if (MovieAvailable(kStateList) && EnterCutsceneList()) return;
    if (!MovieAvailable(kStateList)) Log::Write("CutsceneViewer: the Relics list's movie did not come; the Expeditions screen instead");
    if (!OpenComboViewer()) Log::Write("CutsceneViewer: the Cutscene Viewer screen did not open (see above)");
}

bool OpenViewer() {
    if (g_viewerMode.load() || g_listMode.load()) return false;
    if (g_modelUi.waiting) return true;   // (on its way)
    if (g_modelScreensReady && g_nativeReady) {
        if (MovieAvailable(kStateList) && MovieAvailable(kStateModelGrid) && WidgetMovieAvailable(kInfoWidgetType)) {
            if (EnterCutsceneList()) return true;
        } else {
            // the in-game menu collection (the list's movie) and the campsite's (the outfit grid's) asked for, as the
            // Model Viewer's are: PumpModelUi, from the Extras menu's Process
            g_modelUi.target = 1;
            g_modelUi.needs = kNeeds;
            g_modelUi.need = 0;
            g_modelUi.openedAt = Now();
            bool met = false;
            g_modelUi.waiting = AskNextNeed(met);
            if (met) {
                ListCollectionsReady();
                return true;
            }
            if (g_modelUi.waiting) {
                if (!g_listWaitLogged) Log::Write("CutsceneViewer: the Relics list's and the outfit grid's movies asked for (menu collections)");
                g_listWaitLogged = true;
                return true;
            }
        }
    }
    return OpenComboViewer();
}

}  // namespace

// ---- Public -------------------------------------------------------------------------------------------------------
bool CreateHooks() {
    const struct {
        std::uintptr_t rva;
        const std::uint8_t* sig;
        std::size_t size;
        const char* name;
    } checks[] = {
        {kRvaMainMenuOnEnter, kMainMenuOnEnterSig, sizeof(kMainMenuOnEnterSig), "UiMenuStateMainMenu::OnEnter"},
        {kRvaMainMenuOnUiSelect, kMainMenuOnUiSelectSig, sizeof(kMainMenuOnUiSelectSig), "UiMenuStateMainMenu::OnUiSelect"},
        {kRvaInvokeStateMovie, kInvokeStateMovieSig, sizeof(kInvokeStateMovieSig), "UiMenuStateBase::InvokeStateMovie"},
    };
    for (const auto& c : checks)
        if (!GameBuild::VerifyBytes(c.rva, c.sig, c.size)) {
            Log::Writef("CutsceneViewer: %s signature mismatch; no EXTRAS option", c.name);
            return false;
        }
    g_nativeReady = true;
    for (const Sig& s : kNativeSigs)
        if (!GameBuild::VerifyBytes(s.rva, s.bytes, sizeof(s.bytes))) {
            Log::Writef("CutsceneViewer: Cutscene Viewer screen function at 0x%llX does not match; the main menu keeps "
                        "Start Benchmark", static_cast<unsigned long long>(s.rva));
            g_nativeReady = false;
            break;
        }
    if (g_nativeReady) {   // the combo's vtable as expected
        const auto* vt = reinterpret_cast<const std::uintptr_t*>(Fn(kRvaComboVtable));
        g_nativeReady = vt[0x18 / 8] == Fn(kRvaComboOnEnter) && vt[0x20 / 8] == Fn(kRvaComboPressed) &&
                        vt[0x40 / 8] == Fn(kRvaComboOnExit);
        if (!g_nativeReady) Log::Write("CutsceneViewer: the Expeditions screen's vtable does not match; no Cutscene Viewer");
    }
    if (g_nativeReady) {   // the Extras menu: the functions it calls, the Croft Manor menu's vtable as expected
        g_extrasReady = true;
        for (const Sig& s : kExtrasSigs)
            if (!GameBuild::VerifyBytes(s.rva, s.bytes, sizeof(s.bytes))) {
                Log::Writef("CutsceneViewer: Extras menu function at 0x%llX does not match; EXTRAS opens the Cutscene "
                            "Viewer", static_cast<unsigned long long>(s.rva));
                g_extrasReady = false;
                break;
            }
        const auto* vt = reinterpret_cast<const std::uintptr_t*>(Fn(kRvaCroftVtable));
        if (g_extrasReady &&
            !(vt[0x08 / 8] == Fn(kRvaCroftUiSelect) && vt[0x18 / 8] == Fn(kRvaCroftOnEnter) &&
              vt[0x20 / 8] == Fn(kRvaCroftPressed) && vt[0x30 / 8] == Fn(kRvaCroftProcess) &&
              vt[0x40 / 8] == Fn(kRvaGenericOnExit) && vt[0x78 / 8] == Fn(kRvaGenericAccessible) &&
              vt[0x80 / 8] == Fn(kRvaGenericSetSelection) && vt[0x88 / 8] == Fn(kRvaCroftHighlight))) {
            Log::Write("CutsceneViewer: the Croft Manor menu's vtable does not match; EXTRAS opens the Cutscene Viewer");
            g_extrasReady = false;
        }
    }
    RemovePauseOnlineWidgets();   // (0.9.455)
    {   // the main menu's username and news widgets (0.9.438)
        static constexpr std::uint8_t kIsVisibleSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x83, 0x79};
        g_menuWidgetsReady =
            g_nativeReady &&
            GameBuild::VerifyBytes(kRvaMainMenuUsernameStore, kMainMenuUsernameStoreSig, sizeof(kMainMenuUsernameStoreSig)) &&
            GameBuild::VerifyBytes(kRvaMainMenuNetManager, kMainMenuNetManagerSig, sizeof(kMainMenuNetManagerSig)) &&
            GameBuild::VerifyBytes(kRvaMainMenuInfocastRead, kMainMenuInfocastReadSig, sizeof(kMainMenuInfocastReadSig)) &&
            GameBuild::VerifyBytes(kRvaMainMenuInfocastShow, kMainMenuInfocastShowSig, sizeof(kMainMenuInfocastShowSig)) &&
            GameBuild::VerifyBytes(kRvaWidgetIsVisible, kIsVisibleSig, sizeof(kIsVisibleSig));
        if (!g_menuWidgetsReady) Log::Write("CutsceneViewer: the main menu's widget code does not match; its username and news stay");
    }
    const std::uintptr_t base = GameBuild::Base();
    g_invokeStateMovie = reinterpret_cast<InvokeStateMovieFn>(base + kRvaInvokeStateMovie);   // (CustomOutfits' hook passes on)
    if (MH_CreateHook(reinterpret_cast<void*>(base + kRvaMainMenuOnEnter), reinterpret_cast<void*>(&HookOnEnter),
                      reinterpret_cast<void**>(&g_origOnEnter)) != MH_OK)
        return false;
    if (MH_CreateHook(reinterpret_cast<void*>(base + kRvaMainMenuOnUiSelect), reinterpret_cast<void*>(&HookOnUiSelect),
                      reinterpret_cast<void**>(&g_origOnUiSelect)) != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(base + kRvaMainMenuOnEnter));
        return false;
    }
    if (g_nativeReady && MH_CreateHook(reinterpret_cast<void*>(base + kRvaInvokeAS), reinterpret_cast<void*>(&HookInvokeAS),
                                       reinterpret_cast<void**>(&g_origInvokeAS)) != MH_OK) {
        Log::Write("CutsceneViewer: InvokeASFunction hook not created; no Cutscene Viewer");
        g_nativeReady = false;
        g_extrasReady = false;
    }
    {   // (v0.9.475) the main menu's scene: its enter event for nodes that listen late
        auto target = [&](std::uintptr_t rva, std::size_t length, std::size_t disp) {
            return rva + length + static_cast<std::intptr_t>(*reinterpret_cast<const std::int32_t*>(base + rva + disp));
        };
        if (GameBuild::VerifyBytes(kRvaMenuNodeSetupListener, kMenuNodeSetupListenerSig, sizeof(kMenuNodeSetupListenerSig)) &&
            target(kRvaMenuNodeEventName, 7, 3) == kRvaMenuStateChangedName &&
            GameBuild::VerifyBytes(kRvaSendEventNoPost, kSendEventNoPostSig, sizeof(kSendEventNoPostSig)) &&
            target(kRvaGenericEnterSend, 5, 1) == kRvaSendEventNoPost && target(kRvaGenericEnterSystem, 7, 3) == kRvaEventSystemInstance &&
            MH_CreateHook(reinterpret_cast<void*>(base + kRvaMenuNodeSetupListener), reinterpret_cast<void*>(&HookMenuNodeSetupListener),
                          reinterpret_cast<void**>(&g_origSetupListener)) == MH_OK) {
            g_sendEventNoPost = reinterpret_cast<SendEventNoPostFn>(base + kRvaSendEventNoPost);
            if (!GameBuild::VerifyBytes(kRvaCalibrationOnEnter, kCalibrationOnEnterSig, sizeof(kCalibrationOnEnterSig)) ||
                MH_CreateHook(reinterpret_cast<void*>(base + kRvaCalibrationOnEnter), reinterpret_cast<void*>(&HookCalibrationOnEnter),
                              reinterpret_cast<void**>(&g_origCalibrationOnEnter)) != MH_OK)
                Log::Write("CutsceneViewer: UiMenuStateCalibration::OnEnter does not match; the main menu does not wait for its scene");
        } else {
            Log::Write("CutsceneViewer: UiMenuStateNode / evMenuStateChanged code does not match; the main menu's scene may stay black after a fast start");
        }
    }
    if (GameBuild::VerifyBytes(kRvaSetProfileIndex, kSetProfileIndexSig, sizeof(kSetProfileIndexSig)) &&
        MH_CreateHook(reinterpret_cast<void*>(base + kRvaSetProfileIndex), reinterpret_cast<void*>(&HookSetProfileIndex),
                      reinterpret_cast<void**>(&g_origSetProfileIndex)) == MH_OK) {
    } else {
        Log::Write("CutsceneViewer: NetPlayerInfo::SetCharacterProfileIndex hook not created; the chosen outfit may not apply");
    }
    const struct {
        std::uintptr_t rva;
        const std::uint8_t* sig;
        std::size_t size;
        void* hook;
        void** original;
        const char* name;
    } playbackHooks[] = {
        {kRvaDlcCheckProcess, kDlcCheckProcessSig, sizeof(kDlcCheckProcessSig), reinterpret_cast<void*>(&HookDlcCheckProcess),
         reinterpret_cast<void**>(&g_origDlcCheckProcess), "DLCLicenseCheck::ProcessMsg"},
        {kRvaRequestPause, kRequestPauseSig, sizeof(kRequestPauseSig), reinterpret_cast<void*>(&HookRequestPause),
         reinterpret_cast<void**>(&g_origRequestPause), "PauseQueue::RequestPause"},
        {kRvaSavingIconShow, kSavingIconShowSig, sizeof(kSavingIconShowSig), reinterpret_cast<void*>(&HookSavingIconShow),
         reinterpret_cast<void**>(&g_origSavingIconShow), "UiWidgetSavingIcon::Show"},
        {kRvaStartPauseWorld, kStartPauseWorldSig, sizeof(kStartPauseWorldSig), reinterpret_cast<void*>(&HookStartPauseWorld),
         reinterpret_cast<void**>(&g_origStartPauseWorld), "UIUtils::StartPauseGameWorld"},
        {kRvaEndPauseWorld, kEndPauseWorldSig, sizeof(kEndPauseWorldSig), reinterpret_cast<void*>(&HookEndPauseWorld),
         reinterpret_cast<void**>(&g_origEndPauseWorld), "UIUtils::EndPauseGameWorld"},
    };
    g_movieFnsReady = GameBuild::VerifyBytes(kRvaMovieSkip, kMovieSkipSig, sizeof(kMovieSkipSig)) &&
                      GameBuild::VerifyBytes(kRvaMovieIsPlaying, kMovieIsPlayingSig, sizeof(kMovieIsPlayingSig));
    g_movieEndReady = g_movieFnsReady && GameBuild::VerifyBytes(kRvaMovieEndCheck, kMovieEndCheckSig, sizeof(kMovieEndCheckSig)) &&
                      GameBuild::VerifyBytes(kRvaMoviePlayClear, kMoviePlayClearSig, sizeof(kMoviePlayClearSig));
    if (g_movieFnsReady && !g_movieEndReady) Log::Write("CutsceneViewer: the video end flag does not match; videos are skipped as the player would");
    g_creditsExitReady = GameBuild::VerifyBytes(kRvaCreditsExitState, kCreditsExitStateSig, sizeof(kCreditsExitStateSig));
    if (!g_creditsExitReady) Log::Write("CutsceneViewer: UIMenuStateCredits::ExitState does not match; the story's end credits stay");
    for (const auto& h : playbackHooks)
        if (!GameBuild::VerifyBytes(h.rva, h.sig, h.size) ||
            MH_CreateHook(reinterpret_cast<void*>(base + h.rva), h.hook, h.original) != MH_OK)
            Log::Writef("CutsceneViewer: %s hook not created", h.name);
    if (!GameBuild::VerifyBytes(kRvaTeamsAreEnemies, kTeamsAreEnemiesSig, sizeof(kTeamsAreEnemiesSig)) ||
        MH_CreateHook(reinterpret_cast<void*>(base + kRvaTeamsAreEnemies), reinterpret_cast<void*>(&HookTeamsAreEnemies),
                      reinterpret_cast<void**>(&g_origTeamsAreEnemies)) != MH_OK)
        Log::Write("CutsceneViewer: GameTeam::TeamsAreEnemies hook not created; the level's fights go on during cutscenes");
    g_dialogueStopReady = GameBuild::VerifyBytes(kRvaDialogueStopSounds, kDialogueStopSoundsSig, sizeof(kDialogueStopSoundsSig));
    if (!GameBuild::VerifyBytes(kRvaDialogueProcess, kDialogueProcessSig, sizeof(kDialogueProcessSig)) ||
        MH_CreateHook(reinterpret_cast<void*>(base + kRvaDialogueProcess), reinterpret_cast<void*>(&HookDialogueProcess),
                      reinterpret_cast<void**>(&g_origDialogueProcess)) != MH_OK)
        Log::Write("CutsceneViewer: DialogueManager::Process hook not created; the level's dialogue goes on during cutscenes");
    if (!GameBuild::VerifyBytes(kRvaBarkProcess, kBarkProcessSig, sizeof(kBarkProcessSig)) ||
        MH_CreateHook(reinterpret_cast<void*>(base + kRvaBarkProcess), reinterpret_cast<void*>(&HookBarkProcess),
                      reinterpret_cast<void**>(&g_origBarkProcess)) != MH_OK)
        Log::Write("CutsceneViewer: BarkComponentManager::Process hook not created; barks go on during cutscenes");
    if (GameBuild::VerifyBytes(kRvaCreateTexture, kCreateTextureSig, sizeof(kCreateTextureSig)) &&
        GameBuild::VerifyBytes(kRvaGetBasePointer, kGetBasePointerSig, sizeof(kGetBasePointerSig)) &&
        MH_CreateHook(reinterpret_cast<void*>(base + kRvaGetBasePointer), reinterpret_cast<void*>(&HookGetBasePointer),
                      reinterpret_cast<void**>(&g_origGetBasePointer)) == MH_OK) {
        g_picturesState = 1;
        if (CustomOutfits::HooksImageLoad()) {   // (its hook: ours is called from it)
            CustomOutfits::SetImageLoadedListener(&OnImageLoaded);
            g_loadImageHooked = true;
        } else if (!GameBuild::VerifyBytes(kRvaLoadImage, kLoadImageSig, sizeof(kLoadImageSig))) {
            Log::Write("CutsceneViewer: CustomImageLoader::LoadImage is not the code expected; the pictures' textures "
                       "are made at the panel's size");
        } else if (const MH_STATUS status = MH_CreateHook(reinterpret_cast<void*>(base + kRvaLoadImage),
                                                          reinterpret_cast<void*>(&HookLoadImage),
                                                          reinterpret_cast<void**>(&g_origLoadImage));
                   status != MH_OK) {
            Log::Writef("CutsceneViewer: CustomImageLoader::LoadImage hook not created (MinHook %d); the pictures' "
                        "textures are made at the panel's size", int(status));
        } else {
            g_loadImageHooked = true;
        }
    } else {
        g_picturesState = -1;
        Log::Write("CutsceneViewer: the texture functions do not match; the viewer shows no pictures");
    }
    if (!GameBuild::VerifyBytes(kRvaInitMainMenu, kInitMainMenuSig, sizeof(kInitMainMenuSig)) ||
        MH_CreateHook(reinterpret_cast<void*>(base + kRvaInitMainMenu), reinterpret_cast<void*>(&HookInitMainMenu),
                      reinterpret_cast<void**>(&g_origInitMainMenu)) != MH_OK)
        Log::Write("CutsceneViewer: GameStateMenuing::InitMainMenu hook not created; the main menu's level load is not timed");
    if (g_extrasReady && CheckModelScreens()) {   // (the Extras menu's MODEL VIEWER)
        g_modelScreensReady = true;
        ModelViewer::CreateHooks();
    }
    g_hooksReady = true;
    Log::Writef("CutsceneViewer: main menu hooks created (EXTRAS in place of Start Benchmark; Extras menu %s, Cutscene "
                "Viewer %s)", g_extrasReady ? "ready" : "unavailable", g_nativeReady ? "ready" : "unavailable");
    if (VolumeSlot(0)) {   // (the getters' code checked there)
        void* const hooks[3] = {reinterpret_cast<void*>(&HookVolumeGetter<0>), reinterpret_cast<void*>(&HookVolumeGetter<1>),
                                reinterpret_cast<void*>(&HookVolumeGetter<2>)};
        int made = 0;
        for (int i = 0; i < 3; ++i)
            made += MH_CreateHook(reinterpret_cast<void*>(base + kRvaVolumeGetters[i]), hooks[i],
                                  reinterpret_cast<void**>(&g_origVolumeGetters[i])) == MH_OK;
        if (made != 3) Log::Writef("CutsceneViewer: %d of 3 volume getter hooks", made);
    }
    return true;
}

void AddFonts() {}

void Open() {
    g_enterRequest = true;
}

const char* MenuStatus() {
    if (!g_hooksReady) return "the main menu hooks are not there (see the log)";
    if (!g_nativeReady) return "the Cutscene Viewer is unavailable (see the log)";
    if (!g_extrasReady) return "the Extras menu is unavailable: EXTRAS opens the Cutscene Viewer (see the log)";
    return g_menuOption.load() == 1 ? "EXTRAS is in the main menu (in place of Start Benchmark)"
                                    : "EXTRAS is set when the main menu opens";
}

bool WantsInput() { return ModelViewer::OwnsInput(); }
bool InUse() { return g_play.load() != Play::None || ModelViewer::IsOpen(); }   // (v0.9.602)

// The game's window messages kept from it: all of its input while a cutscene plays from the viewer; on the Model
// Viewer's view its keys and its mouse, but the mouse over the view's button bar (v0.9.422: the game's legend takes
// the clicks there, its Reset and Back buttons; raw input too, the mouse's alone).
bool BlocksGameMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_play.load() != Play::None) return true;
    if (GridTabMessage(msg)) return true;   // (the grid's tabs, v0.9.432)
    if (OutfitGridMessage(msg, wParam)) return true;   // (v0.9.496: the Cutscene Viewer's outfit grid)
    if (PickerWheelMessage(msg, wParam, lParam)) return true;   // (v0.9.503: the wheel, by the mouse's side)
    if (ListMouseMessage(msg)) return true;    // (v0.9.501: the list under the grid)
    if (msg == WM_INPUT && SearchKeepsKeys()) {   // (the keyboard's raw input while the search is typed in)
        RAWINPUTHEADER header{};
        UINT size = sizeof(header);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_HEADER, &header, &size, sizeof(header)) != UINT(-1) &&
            header.dwType == RIM_TYPEKEYBOARD)
            return true;
    }
    if (!ModelViewer::OwnsInput()) return false;
    const bool mouse = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && msg != WM_MOUSEWHEEL && msg != WM_MOUSEHWHEEL);
    if (msg == WM_INPUT) {
        RAWINPUTHEADER header{};
        UINT size = sizeof(header);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_HEADER, &header, &size, sizeof(header)) == UINT(-1) ||
            header.dwType != RIM_TYPEMOUSE)
            return true;
    } else if (!mouse) {
        return true;
    }
    POINT p{};
    RECT client{};
    if (!GetCursorPos(&p) || !ScreenToClient(hwnd, &p) || !GetClientRect(hwnd, &client)) return true;
    return !ModelViewer::InButtonBar(float(p.x), float(p.y), float(client.right), float(client.bottom));
}

bool HandleKey(UINT msg, WPARAM wParam) {
    if (GridSearchKey(msg, wParam)) return true;   // (the grid's search, v0.9.438)
    if (ModelViewer::HandleKey(msg, wParam)) return true;
    if (g_play.load() == Play::None || wParam != VK_ESCAPE) return false;
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        g_abortRequest = true;
        return true;
    }
    return msg == WM_KEYUP || msg == WM_SYSKEYUP;
}

bool FilterPad(unsigned long user, void* xinputState) {
    if (ModelViewer::FilterPad(user, xinputState)) return true;
    if (user >= 4 || !xinputState || g_play.load() == Play::None) {
        if (user < 4 && xinputState) g_padHeld[user] = static_cast<XINPUT_STATE*>(xinputState)->Gamepad.wButtons;
        return false;
    }
    auto* state = static_cast<XINPUT_STATE*>(xinputState);
    const WORD buttons = state->Gamepad.wButtons;
    const WORD previous = g_padHeld[user].exchange(buttons);
    const WORD pressed = static_cast<WORD>(buttons & ~previous);
    if (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK)) g_abortRequest = true;
    std::memset(&state->Gamepad, 0, sizeof(state->Gamepad));
    return true;
}

bool NeedsFrame() { return g_black.load() > 0.0f || ModelViewer::WantsFrame(); }
bool FullBlack() { return g_black.load() >= 1.0f; }

namespace {
// ---- The way back to the viewer, timed (0.9.385) ----
// From the skip (or the fade-out before the cutscene's end) to the viewer's picture back, one line: what happened when
// (the game's states, Cutscenes::GameStates: NetManager::ReturnToMainMenu sets QUITTING, whose OnEnter dumps the
// levels and sets MENUING; the main menu's screen, its UiMenuStateMainMenu::OnEnter; the Extras menu; the viewer
// screen), the viewer's own waits, and the frames on the way (the longest gap between two game ticks).
struct BackMark {
    double at;
    std::string what;
};
bool g_backOn = false;
double g_backStart = 0.0, g_backLastTick = 0.0, g_backLongest = 0.0, g_backSlow = 0.0;
int g_backFrames = 0, g_backGame = -2, g_backNet = -2;
std::uint32_t g_backEnters = 0;
bool g_backMenuSeen = false, g_backExtrasSeen = false, g_backViewerSeen = false, g_backIdleSeen = false;
std::string g_backWhy;
std::vector<BackMark> g_backMarks;
// the units (StreamTracker slots) and the menu state on the way: their changes, a second line
Cutscenes::UnitSlot g_backSlots[32];
int g_backSlotCount = 0;
std::vector<std::pair<std::string, std::string>> g_backUnits;   // unit name, its changes
int g_backUiState = -2;
bool g_inMainMenuLoad = false;   // InitMainMenu running (game thread)
int g_mainMenuLoadTicks = 0;

const char* GameStateName(int id) {
    static constexpr const char* kNames[] = {"default",  "booting",           "menuing",  "loading",      "waiting",
                                             "playing",  "resetting intros",  "quitting", "post-playing", "object viewer"};
    return id >= 0 && id < 10 ? kNames[id] : "?";
}
const char* UnitStateName(int state) {
    static constexpr const char* kNames[] = {"gone", "being loaded", "loaded", "not wanted", "dumping"};
    return state >= 0 && state < 5 ? kNames[state] : "?";
}
int UiStateNow(int* next) {
    *next = -1;
    void* manager = StateManager();
    if (!manager) return -1;
    *next = At<std::int32_t>(manager, kSmNextState);
    return At<std::int32_t>(manager, kSmCurrentState);
}
void BackUnitChange(double now, const char* name, const char* what) {
    if (!name[0]) return;
    auto it = std::find_if(g_backUnits.begin(), g_backUnits.end(), [&](const auto& u) { return u.first == name; });
    if (it == g_backUnits.end()) {
        if (g_backUnits.size() >= 24) return;
        g_backUnits.emplace_back(name, std::string());
        it = g_backUnits.end() - 1;
    }
    char one[64];
    snprintf(one, sizeof(one), "%s%s at %.2f s", it->second.empty() ? "" : ", ", what, now - g_backStart);
    if (it->second.size() < 200) it->second += one;
}

const char* NetStateName(int id) {
    static constexpr const char* kNames[] = {"idle",     "hosting (system link)", "joining (system link)", "creating",
                                             "joining",  "searching",             "connecting",            "in a session",
                                             "leaving"};
    return id >= 0 && id < 9 ? kNames[id] : "?";
}

void BackMarkNow(double now, const std::string& what) {
    if (g_backOn && g_backMarks.size() < 40) g_backMarks.push_back({now - g_backStart, what});
}

void BackBegin(double now, const char* why) {
    if (g_backOn) return;
    g_backOn = true;
    g_backStart = g_backLastTick = now;
    g_backLongest = g_backSlow = 0.0;
    g_backFrames = 0;
    Cutscenes::GameStates(g_backGame, g_backNet);
    g_backEnters = g_menuEnters.load();
    g_backMenuSeen = g_backExtrasSeen = g_backViewerSeen = g_backIdleSeen = false;
    g_backWhy = why;
    g_backMarks.clear();
    g_backUnits.clear();
    g_backSlotCount = Cutscenes::StreamUnitSlots(g_backSlots, 32);
    int next = -1;
    g_backUiState = UiStateNow(&next);
    char text[96];
    snprintf(text, sizeof(text), "the game %s, net state %s", GameStateName(g_backGame), NetStateName(g_backNet));
    BackMarkNow(now, text);
}

// every tick on the way back
void BackTick(double now) {
    if (!g_backOn) return;
    const double gap = now - g_backLastTick;
    g_backLastTick = now;
    ++g_backFrames;
    g_backLongest = std::max(g_backLongest, gap);
    if (gap > 0.1) g_backSlow += gap;
    if (g_inMainMenuLoad) ++g_mainMenuLoadTicks;
    {   // the units' changes
        Cutscenes::UnitSlot slots[32];
        const int count = Cutscenes::StreamUnitSlots(slots, 32);
        for (int i = 0; i < count && i < g_backSlotCount; ++i) {
            const Cutscenes::UnitSlot& was = g_backSlots[i];
            const Cutscenes::UnitSlot& is = slots[i];
            if (was.id == is.id && was.state == is.state) continue;
            if (is.state == 0) {
                if (was.state != 0) BackUnitChange(now, was.name, "gone");
            } else {
                if (was.state != 0 && (was.id != is.id || std::strcmp(was.name, is.name) != 0)) BackUnitChange(now, was.name, "gone");
                BackUnitChange(now, is.name, UnitStateName(is.state));
            }
            g_backSlots[i] = is;
        }
    }
    {   // the menu state
        int next = -1;
        const int ui = UiStateNow(&next);
        if (ui != g_backUiState) {
            char text[64];
            snprintf(text, sizeof(text), "the menu state 0x%X", unsigned(ui));
            BackMarkNow(now, text);
            g_backUiState = ui;
        }
    }
    int game = -1, net = -1;
    Cutscenes::GameStates(game, net);
    if (game != g_backGame) {
        BackMarkNow(now, std::string("the game ") + GameStateName(game));
        g_backGame = game;
    }
    if (net != g_backNet) {
        BackMarkNow(now, std::string("net state ") + NetStateName(net));
        g_backNet = net;
    }
    if (!g_backIdleSeen && Cutscenes::CurrentStage() == Cutscenes::Stage::Idle) {
        g_backIdleSeen = true;
        BackMarkNow(now, "the viewer's cutscene over");
    }
    if (!g_backMenuSeen && g_menuEnters.load() != g_backEnters) {
        g_backMenuSeen = true;
        BackMarkNow(now, "the main menu's screen (its OnEnter)");
    }
    if (!g_backExtrasSeen && g_extrasEnterTime > g_backStart) {
        g_backExtrasSeen = true;
        BackMarkNow(g_extrasEnterTime, "the Extras menu's screen");
    }
    if (!g_backViewerSeen && g_viewerEnterTime > g_backStart) {
        g_backViewerSeen = true;
        BackMarkNow(g_viewerEnterTime, "the Cutscene Viewer's screen");
    }
}

void BackEnd(double now) {
    if (!g_backOn) return;
    BackMarkNow(now, "the picture back");
    std::stable_sort(g_backMarks.begin(), g_backMarks.end(), [](const BackMark& a, const BackMark& b) { return a.at < b.at; });
    std::string list;
    for (const BackMark& m : g_backMarks) {
        char one[160];
        snprintf(one, sizeof(one), "%s%s at %.2f s", list.empty() ? "" : "; ", m.what.c_str(), m.at);
        list += one;
    }
    if (list.size() > 820) list = list.substr(0, 820) + " ...";
    Log::Writef("CutsceneViewer: back in the viewer %.1f s after %s: %s (%d frames, the longest %.0f ms, %.1f s in frames "
                "over 100 ms)", now - g_backStart, g_backWhy.c_str(), list.c_str(), g_backFrames, g_backLongest * 1000.0, g_backSlow);
    std::string units;
    for (const auto& u : g_backUnits) {
        if (units.size() > 820) {
            units += "; ...";
            break;
        }
        units += (units.empty() ? "" : "; ") + u.first + ": " + u.second;
    }
    Log::Writef("CutsceneViewer: the units on that way back: %s", units.empty() ? "no change" : units.c_str());
    g_backOn = false;
}

// (v0.9.606) Every load of the main menu's level (the game's start, Quit to the main menu, a level's end) gets the
// faster loading as the viewer's way back has had since 0.9.387 (Features::BeginOtherLoadProfile: the poll sleeps
// shortened, the renderer's multicore mode off, presents without vsync and without waiting; its LoadProf line), and its
// time in the log: "the main menu's level loaded in N s (at T ms from the process start; M game ticks in it)".
int g_mainMenuLoads = 0;
void __fastcall HookInitMainMenu(void* menuing) {
    const bool loads = !g_inMainMenuLoad && menuing && !*(static_cast<std::uint8_t*>(menuing) + 0x10);
    const bool timed = g_backOn && loads;
    if (!timed && !loads) {
        g_origInitMainMenu(menuing);
        return;
    }
    if (!timed) {
        const double start = Now();
        const ULONGLONG startTick = GetTickCount64();
        g_inMainMenuLoad = true;
        g_mainMenuLoadTicks = 0;
        Features::BeginOtherLoadProfile();
        g_origInitMainMenu(menuing);
        Features::EndOtherLoadProfile();
        g_inMainMenuLoad = false;
        FILETIME created{}, exited{}, kernel{}, user{};
        long long sinceStart = -1;
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
            FILETIME nowFt{};
            GetSystemTimeAsFileTime(&nowFt);
            const auto u64 = [](const FILETIME& f) { return (ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
            sinceStart = static_cast<long long>((u64(nowFt) - u64(created)) / 10000ull) -
                         static_cast<long long>(GetTickCount64() - startTick);
        }
        Log::Writef("CutsceneViewer: the main menu's level loaded in %.2f s (load %d of the session, begun at %lld ms from the "
                    "process start; %d game ticks in it; faster loading %s)",
                    Now() - start, ++g_mainMenuLoads, sinceStart, g_mainMenuLoadTicks,
                    Features::FastLoadingEnabled() ? "on" : "off");
        return;
    }
    ++g_mainMenuLoads;
    const double start = Now();
    BackMarkNow(start, "the main menu's level load begun (InitMainMenu)");
    g_inMainMenuLoad = true;
    g_mainMenuLoadTicks = 0;
    Features::BeginOtherLoadProfile();
    g_origInitMainMenu(menuing);
    Features::EndOtherLoadProfile();
    g_inMainMenuLoad = false;
    const double end = Now();
    char text[96];
    snprintf(text, sizeof(text), "the main menu's level loaded (%.1f s, %d game ticks in it)", end - start, g_mainMenuLoadTicks);
    BackMarkNow(end, text);
}
}  // namespace

// (v0.9.513) The game's button bar (UiWidgetContextbar: "BACK" at the bottom right) seen during play. Its first
// reports came after the viewer (a cutscene's level loaded from the list: "UiWidgetContextbar still held its movie ...
// when the movie was destroyed (loading)"), but it was also there in play right after Continue, the viewer not opened
// at all. UiWidgetContextbar belongs to the menu screens (UiMenuStateGeneric::GetContextbarWidget; the status screens
// have their own legend, UiWidgetSSButtonLegend): with no menu screen up none should show. So every 0.5 s, with no menu
// screen up (the state manager's current and next screens -1) and nothing loading, every UiWidgetContextbar of the widget
// manager that is shown is hidden (each logged, the first few times).
// UiWidgetManagerBase keeps its widgets in a hash map by ID (TR2 GetWidget 0x14008C830; retail 0x7A560, the same code):
// +0x08 the buckets, +0x18 the count, +0x20 the bucket count; a node {+0 the next, +8 the ID, +0x10 the widget}.
constexpr std::uintptr_t kRvaContextbarWidgetVtable = 0x00DCAB00ull;   // UiWidgetContextbar's vftable (features.cpp's)
constexpr std::size_t kWmBuckets = 0x08, kWmCount = 0x18, kWmBucketCount = 0x20, kWmNodeId = 0x08, kWmNodeWidget = 0x10;
int g_barsHidden = 0;
// The shown button bars of the widget manager (at most `max`), with their IDs.
int ShownContextbars(void** bars, std::uint32_t* ids, int max) {
    int n = 0;
    __try {
        void* manager = reinterpret_cast<void*(__fastcall*)()>(Fn(kRvaWidgetManagerGet))();
        if (!manager) return 0;
        auto* const* buckets = At<void**>(manager, kWmBuckets);
        const std::uint64_t count = At<std::uint64_t>(manager, kWmCount), bucketCount = At<std::uint64_t>(manager, kWmBucketCount);
        if (!buckets || !count || !bucketCount || bucketCount > 65536) return 0;
        const std::uintptr_t vtable = Fn(kRvaContextbarWidgetVtable);
        int walked = 0;
        for (std::uint64_t b = 0; b < bucketCount && n < max; ++b)
            for (void* node = buckets[b]; node && n < max && walked < 16384; node = At<void*>(node, 0), ++walked) {
                void* widget = At<void*>(node, kWmNodeWidget);
                if (!widget || reinterpret_cast<std::uintptr_t>(VtableOf(widget)) != vtable) continue;
                if (std::find(bars, bars + n, widget) != bars + n) continue;   // (a chain met again)
                if (!reinterpret_cast<bool(__fastcall*)(void*)>(Fn(kRvaWidgetIsVisible))(widget)) continue;
                bars[n] = widget;
                ids[n] = At<std::uint32_t>(node, kWmNodeId);
                ++n;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}
bool HideBar(void* bar) {
    __try {
        ShowWidget(bar, false, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void BarSafetyTick() {
    if (!g_hooksReady || !g_nativeReady || Features::LoadingProfilerSessionActive()) return;
    static double s_checkedAt = 0.0;
    const double now = Now();
    if (now - s_checkedAt < 0.5) return;
    s_checkedAt = now;
    void* mgr = StateManager();
    if (!mgr) return;
    const std::int32_t current = At<std::int32_t>(mgr, kSmCurrentState), next = At<std::int32_t>(mgr, kSmNextState);
    if (current != -1 || next != -1) return;   // (a menu screen is up, or on its way: its bar is its own)
    void* bars[8];
    std::uint32_t ids[8];
    const int shown = ShownContextbars(bars, ids, 8);
    for (int i = 0; i < shown; ++i) {
        const bool hidden = HideBar(bars[i]);
        if (g_barsHidden++ < 8)
            Log::Writef("UI: the button bar (UiWidgetContextbar, widget %u) was shown with no menu screen up: %s", ids[i],
                        hidden ? "hidden" : "could not be hidden");
        if (g_barsHidden <= 2) HudWatch::LogCensus("the button bar was shown with no menu screen up");   // (v0.9.541)
    }
}

void TickGameThread() {
    ResendMenuEnterTick();   // (v0.9.475)
    ListSafetyRestore();     // (v0.9.500)
    BarSafetyTick();         // (v0.9.513)
    BackdropTick();          // (v0.9.509: the room's fade behind the list)
    ViewerAudio::Tick();   // (the Model Viewer's music and the level's sounds: their fades, after it closes too)
    {   // (the pictures decoded in the background made textures; the picked one shown when it is new)
        const Cutscenes::ViewerItem* item = (g_viewerMode.load() || g_listMode.load()) && g_infoReady ? PickedItem() : nullptr;
        if (PumpPictures(item ? item->image : std::string()) && item)
            if (void* gsm = GameStateMenuing()) PushPicture(Widget(At<std::uint32_t>(gsm, kGsmInfoWidget)), PictureFor(*item));
    }
    if (g_enterRequest.exchange(false)) {
        if (MainMenuIsCurrent()) EnterExtras();
        else Log::Write("CutsceneViewer: the Extras menu opens from the main menu");
    }
    const double now = Now();
    const Play play = g_play.load();
    const bool abort = g_abortRequest.exchange(false);
    BackTick(now);
    switch (play) {
        case Play::None: {
            PlayRequest request;
            {
                std::lock_guard lock(g_requestMutex);
                request = g_request;
                g_request.pending = false;
            }
            if (!request.pending) break;
            g_playId = request.id;
            g_playTitle = request.title;
            g_playOutfit = request.outfit;
            g_entersAtStart = g_menuEnters.load();
            g_pauseLogs = 0;
            g_moviesSkipped = 0;
            g_movieSkipAt = 0.0;
            g_creditsClosed = 0;
            g_creditsCloseAt = 0.0;
            g_enemyIgnoreLogged = false;
            g_dialoguesStopped = false;
            g_barksLogged = false;
            g_skippedWorldPauses = 0;
            g_dlcLogs = 0;
            g_savingIconLogged = false;
            g_abortPending = g_abortSent = g_menuForced = false;
            SaveVolumes();
            g_t0 = now;
            g_play = Play::FadeOut;
            Log::Writef("CutsceneViewer: %s (%s), outfit %d", g_playTitle.c_str(), g_playId.c_str(), g_playOutfit);
            break;
        }
        case Play::FadeOut: {
            const float k = Progress(now, kFade);
            g_black = k;
            ApplyGain(1.0f - k);
            if (k < 1.0f) break;
            ApplyOutfitBeforeLoad();
            Features::SetCutsceneViewerNoTutorials(true);   // (a card pauses the game: the cutscene would not come)
            if (!Cutscenes::PlayForViewer(g_playId)) {
                SetMessage("This cutscene could not be started (see the log).");
                RestoreOutfit();
                Features::SetCutsceneViewerNoTutorials(false);
                g_t0 = now;
                g_play = Play::MenuFadeIn;
                break;
            }
            g_t0 = now;
            g_play = Play::Waiting;
            break;
        }
        case Play::Waiting: {
            ApplyGain(0.0f);
            SkipMovies(now);
            CloseStoryCredits(now);
            const Cutscenes::Stage stage = Cutscenes::CurrentStage();
            if (abort) g_abortPending = true;
            KeepProfile();
            // (the game's loading screen is shown as it is: the overlay is not drawn on it reliably)
            g_black = Features::LoadingProfilerSessionActive() ? 0.0f : 1.0f;
            if (stage == Cutscenes::Stage::PreRoll) EnsureOutfit();
            if (g_abortPending && stage != Cutscenes::Stage::Loading && stage != Cutscenes::Stage::Idle) {
                BackBegin(now, "the skip (before it played)");
                Cutscenes::AbortToMainMenu();
                g_abortSent = true;
                g_t0 = now;
                g_play = Play::Returning;
            } else if (stage == Cutscenes::Stage::Playing && !g_abortPending) {
                if (g_outfitApplied)
                    Log::Writef("CutsceneViewer: the cutscene starts; Lara wears object %u (chosen %u), profile %d",
                                unsigned(Features::CurrentOutfitObjectId()), unsigned(g_wantedObject),
                                Features::LocalPlayerCharacterProfile());
                g_black = 1.0f;
                g_t0 = now;
                g_play = Play::FadeIn;
            } else if (stage == Cutscenes::Stage::Idle && now - g_t0 > 3.0) {
                if (Cutscenes::LastViewerPlayFailed()) SetMessage("The cutscene did not start after the load.");
                BackBegin(now, "it did not come");
                g_t0 = now;
                g_play = Play::Returning;
            }
            break;
        }
        case Play::FadeIn:
        case Play::Showing: {
            CloseStoryCredits(now);
            const Cutscenes::Stage stage = Cutscenes::CurrentStage();
            if (stage == Cutscenes::Stage::PreRoll && !abort) {   // the next of its chain is waited for: black until it
                g_black = 1.0f;                                    // plays, then the fade-in
                ApplyGain(0.0f);
                g_t0 = now;
                g_play = Play::Waiting;
                break;
            }
            if (stage != Cutscenes::Stage::Playing) {   // over (or skipped) before the fade
                BackBegin(now, "its end");
                g_black = 1.0f;
                ApplyGain(0.0f);
                g_t0 = now;
                g_play = Play::Returning;
                break;
            }
            const float left = Cutscenes::TargetSecondsLeft();
            if (abort || (left >= 0.0f && left <= kFade)) {
                BackBegin(now, abort ? "the skip" : "the fade-out before its end");
                g_abortPending = abort;
                g_fadeLength = abort ? 0.35 : std::max(0.1, double(left));
                // from the current darkness
                g_t0 = now - double(g_black.load()) * g_fadeLength;   // from the current darkness
                g_play = Play::FadeOutEnd;
                break;
            }
            if (play == Play::FadeIn) {
                const float k = Progress(now, kFade);
                g_black = 1.0f - k;
                ApplyGain(k);
                if (k >= 1.0f) g_play = Play::Showing;
            } else {
                g_black = 0.0f;
                ApplyGain(1.0f);
            }
            break;
        }
        case Play::FadeOutEnd: {
            const float k = Progress(now, g_fadeLength);
            g_black = k;
            ApplyGain(1.0f - k);
            if (abort) g_abortPending = true;
            if (k >= 1.0f || Cutscenes::CurrentStage() != Cutscenes::Stage::Playing) {
                g_black = 1.0f;
                ApplyGain(0.0f);
                BackMarkNow(now, "black");
                if (g_abortPending && Cutscenes::CurrentStage() != Cutscenes::Stage::Idle) {
                    Cutscenes::AbortToMainMenu();
                    g_abortSent = true;
                    BackMarkNow(now, "the main menu asked for");
                }
                g_t0 = now;
                g_play = Play::Returning;
            }
            break;
        }
        case Play::Returning: {
            // (the main menu's level load, profiled since 0.9.386, stays black as before)
            g_black = Features::LoadingProfilerSessionActive() && !g_inMainMenuLoad ? 0.0f : 1.0f;
            ApplyGain(0.0f);
            const Cutscenes::Stage stage = Cutscenes::CurrentStage();
            if (abort && stage != Cutscenes::Stage::Idle && stage != Cutscenes::Stage::Loading && !g_abortSent) {
                Cutscenes::AbortToMainMenu();
                g_abortSent = true;
                BackMarkNow(now, "the main menu asked for");
            }
            if (g_menuEnters.load() != g_entersAtStart && stage == Cutscenes::Stage::Idle) {
                g_t0 = now;
                g_play = Play::MenuSettle;
            } else if (stage == Cutscenes::Stage::Idle && now - g_t0 > 15.0 && !g_menuForced) {
                // the story went on without the way back (or it was refused): back to the menu
                Log::Write("CutsceneViewer: no main menu 15 s after the cutscene; returning to it");
                Cutscenes::AbortToMainMenu();
                g_menuForced = true;
                g_t0 = now;
            } else if (now - g_t0 > 90.0) {
                Log::Write("CutsceneViewer: still no main menu after 90 s; the picture and sound come back");
                BackEnd(now);
                Finish();
            }
            break;
        }
        case Play::MenuSettle:   // (0.6 s until 0.9.386: the main menu's OnEnter has run, its options set by ours)
            g_black = 1.0f;
            ApplyGain(0.0f);
            if (now - g_t0 < 0.25) break;
            RestoreOutfit();
            Features::SetCutsceneViewerNoTutorials(false);
            BackMarkNow(now, "the viewer's 0.25 s on the main menu over");
            // back to the Cutscene Viewer screen with the same choices, through the Extras menu (so that Back leads to it)
            g_waitViewer = false;
            if (!g_viewerMode.load() && !g_listMode.load() && !g_extrasMode.load()) {
                if (g_extrasReady) {
                    g_reopenViewer = true;
                    g_waitViewer = EnterExtras();
                    if (!g_waitViewer) g_reopenViewer = false;
                } else {
                    g_waitViewer = OpenViewer();
                }
            }
            g_t0 = now;
            g_play = Play::MenuOpening;
            break;
        case Play::MenuOpening:   // black until the viewer screen is up (4 s at most)
            g_black = 1.0f;
            ApplyGain(0.0f);
            if (!g_waitViewer ||
                ((g_viewerMode.load() || g_listMode.load()) && g_viewerEnterTime > 0.0 && now - g_viewerEnterTime >= 0.3) ||
                now - g_t0 > 6.0) {
                if (g_waitViewer && !g_viewerMode.load() && !g_listMode.load())
                    Log::Write("CutsceneViewer: the Cutscene Viewer screen did not come back");
                g_reopenViewer = false;
                BackMarkNow(now, "the fade-in");
                g_t0 = now;
                g_play = Play::MenuFadeIn;
            }
            break;
        case Play::MenuFadeIn: {
            const float k = Progress(now, kFade);
            g_black = 1.0f - k;
            ApplyGain(k);
            if (k >= 1.0f) {
                BackEnd(now);
                Finish();
            }
            break;
        }
    }
}

void Draw() {
    ModelViewer::Draw();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 size = io.DisplaySize;
    const float black = g_black.load();
    if (size.x <= 0 || size.y <= 0 || black <= 0.0f) return;
    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), size,
                                                  ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, std::min(1.0f, black))));
}

}  // namespace CutsceneViewer
