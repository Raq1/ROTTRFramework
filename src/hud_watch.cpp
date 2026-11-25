// (v0.9.541) Why the HUD stops answering after some loads: logging only (see hud_watch.h). Retail RVAs of the Steam
// ROTTR.exe (GameBuild::kSupportedTimestamp), found from TR2_x64_final's functions of the same names, each checked
// against its first bytes.
//
// The weapon keys are not read from the player's controller (as TAB and Escape are, _ProcessPauseModeChanges) but sent
// to the HUD's weapon select widget as UI input: UiWidgetBase::ProcessInput (TR2 0x140090350, retail 0x7DBF0) hands a
// widget its actions only while the world pause byte (retail 0x2D57965) is not 1, it has input devices (+0x50), it is
// visible (or +0x2D is set), +0x28 is below 0, it has actions (+0x30) and its vfunc +0x60 agrees. Then
// UiWidgetWeaponSelect::OnInputActionPressed (TR2 0x1409E1800, retail 0x99C770) does nothing when
// Player::GetOverrideWeaponSelectBlocked (TR2 0x1408676C0, retail 0x82AFD0: the player's MovesetProfileComponent does
// not permit the move hash at PlayerComponent +0x2B8 -> +0x1B10) answers true, sets +0x1C8 bit 3 and goes on only if
// PassesAccessCheck (TR2 0x1409E90D0, retail 0x9A3F90: no menu state 0x4E current, no 0x52 / 0x7B / 0x91 in the
// navigation stack, not in the status menus, the world pause byte not 2, no pause block or request, a player with an
// Instance, no cinematic or movie, player +0x369C and +0x36A8 both -1, +0x1C8 bit 3, an item in menu slot 3, 7, 1 or 5)
// passes. Logged:
// - OnInputActionPressed hooked: the action, whether the moveset blocks it, and the access check's answer (called as the
//   game calls it, with bit 3 set for the call and the byte put back): the first 2 calls, then the refused ones (12).
// - A number key 1..9 pressed in play (the game window in front, no menu current or next, not the framework's menu, not
//   loading) that reached no OnInputActionPressed in 0.3 s: the weapon select widget's input gates as ProcessInput
//   reads them (its vfunc +0x60 not called: its RVA logged) and a census (6 at most).
// - The census (LogCensus, also called by the button bar's guard and TAB's fallback): the menu navigation stack
//   (UiMenuStateManager +0x9A8, 32 states; current +0xA28, next +0xA2C) and every visible widget of the widget manager
//   (its hash map: +0x08 buckets, +0x18 count, +0x20 bucket count; nodes {next, +8 id, +0x10 widget}) by class (RTTI),
//   which names the icon that stays.
// (v0.9.587) 0.9.586's run: after a load the saving icon (UiWidgetSavingIcon 27) stayed, the number keys reached no
// weapon select input although every gate above passed, and TAB's map never opened (next state 0x60 waiting). Two more
// gates of ProcessInput: its vfunc +0x60, UiWidgetBase::IsInputAllowed (retail 0x7B500, TR2 0x14008D7D0; every widget's,
// the weapon select's too), refuses any widget input while NotificationDisplayManagerBase::Get() (retail 0x401F60: the
// pointer at 0x2D30AA8) +0x28 (the system message on display: the Yes/No and message dialogs) is set, or while the
// exclusive input widget id (retail 0xFB3AF4, TR2 0x14101BC28; RequestExclusiveInput, only the front end's tutorial and
// the PC invite / join dialogs set it) is another widget's; and per action, the widget's +0x48 (no input before this
// TIMER_GetTimeMS time, retail 0x3C8DA0) and +0x40 (the same for held actions). Logged now with the gates: the answer of
// IsInputAllowed, the system message (its type +0x04, id +0x08, flags +0x0C, +0x20, the dialog widget type +0x1834,
// its text +0x22, or wide at +0x422 when +0x21 is set), the manager's queue (+0x20), timer (+0x30), +0x36 and dialog
// widget (+0x4C: its class, visible or not), the exclusive id, the time gates and the saving icon's timer (+0x170). The
// census lists the hidden widgets too.
// - Recovery (TickGameThread): in play (a player, no menu state current), a system message on display whose dialog widget
//   is missing or not visible for 4 s in a row (nothing the player can see or answer: every HUD input refused for good)
//   is cleared with the game's NotificationDisplayManagerBase::ForceClearMessage(false) (retail 0x401560, TR2
//   0x14042C820: ClearMessage(true, false) through the vtable, unless the message's +0x20 is clear and its flags have
//   0x40; the queue kept), logged with what it was; and an exclusive input id whose widget no longer exists is put back
//   to -1. Each at most 8 times a run.
// (v0.9.588) 0.9.587's run: the main menu never came (black): the framework asked for it ("the main menu comes after
// 375 ms", UiMenuStateManager::SetState(6, 0.25)) and "the main menu entered (53 -> 6)" never followed, as TAB's map
// (next 0x60) never opened in 0.9.586's. UiMenuStateManager::ChangeState (retail 0x8AEED0, TR2 0x1408F0A20) switches
// current (+0xA28) to the requested state (+0xA2C) only while the delay +0xA98 (TR2 +0xAA0) is not above 0 (comiss: a
// NaN never passes); UiMenuStateManager::Process (retail 0x8BD3F0, TR2 0x1408FF520) counts that delay down, and runs
// its input, only when its re-entry count +0xAA4 (TR2 +0xAAC: incremented at its start, decremented at its end) is 1,
// so one Process left without its decrement (an exception swallowed further up) stops both for good. SetState(state,
// delay) (TR2 0x140905190) takes, for a delay below 0, the current state's movie data +0x5C ([state +0x10] +0x10): a
// freed movie gives it any value. Watched now (RecoverMenu): a requested state not entered for 3 s is logged (current,
// requested, the delay and its raw bits, the re-entry count and the lowest one seen, a census and the system message);
// a delay that is not finite or above 30 s is set to 0, and a re-entry count above the lowest one seen (the framework's
// tick is always at the same place against Process) is set back to it, both logged. Each at most 8 times a run.
#include "hud_watch.h"

#include "features.h"
#include "game_build.h"
#include "log.h"
#include "overlay.h"
#include "settings.h"

#include <MinHook.h>
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace HudWatch {
namespace {
constexpr std::uintptr_t kRvaWidgetManagerGet = 0x008B35E0ull, kRvaWidgetIsVisible = 0x0007B820ull,
                         kRvaStateManager = 0x02E1E758ull, kRvaWorldPauseByte = 0x02D57965ull,
                         kRvaWeaponSelectPressed = 0x0099C770ull, kRvaWeaponSelectBlocked = 0x0082AFD0ull,
                         kRvaWeaponSelectAccess = 0x009A3F90ull, kRvaWidgetProcessInput = 0x0007DBF0ull;
// mov rax, [rip + the manager]; ret
constexpr std::uint8_t kWidgetManagerGetSig[] = {0x48, 0x8B, 0x05, 0xE9, 0x2F, 0x84, 0x00, 0xC3};
constexpr std::uint8_t kWidgetIsVisibleSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x83, 0x79, 0x24, 0x00, 0x48, 0x8B, 0xD9};
// mov [rsp+8], rbx; push rdi; sub rsp, 20h; mov rbx, rcx; movsxd rdi, edx; mov rcx, [rcx+170h]; test rcx, rcx; je
constexpr std::uint8_t kWeaponSelectPressedSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9,
                                                    0x48, 0x63, 0xFA, 0x48, 0x8B, 0x89, 0x70, 0x01, 0x00, 0x00, 0x48, 0x85, 0xC9,
                                                    0x74, 0x0D};
constexpr std::uint8_t kWeaponSelectBlockedSig[] = {0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8B, 0x81, 0xA8, 0x31, 0x00, 0x00, 0xB2, 0x01,
                                                    0x88, 0x54, 0x24, 0x30};
// ... cmp dword [rcx+0A28h], 4Eh (the current menu state)
constexpr std::uint8_t kWeaponSelectAccessSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x0D, 0xB8,
                                                   0xA7, 0x47, 0x02, 0x83, 0xB9, 0x28, 0x0A, 0x00, 0x00, 0x4E};
// the input gates' reads: the pause byte == 1, [rcx+50h], [rcx+2Dh], IsVisible, comiss 0, [rbx+28h], [rbx+30h]
constexpr std::uint8_t kWidgetProcessInputSig[] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x80, 0x3D, 0x68, 0x9D, 0xCD, 0x02, 0x01, 0x48, 0x8B, 0xD9, 0x0F, 0x84,
    0x32, 0x02, 0x00, 0x00, 0x83, 0x79, 0x50, 0x00, 0x0F, 0x84, 0x28, 0x02, 0x00, 0x00, 0x80, 0x79, 0x2D, 0x00,
    0x75, 0x0D, 0xE8, 0x05, 0xDC, 0xFF, 0xFF, 0x84, 0xC0, 0x0F, 0x84, 0x15, 0x02, 0x00, 0x00, 0x0F, 0x57, 0xC0,
    0x0F, 0x2F, 0x43, 0x28, 0x0F, 0x86, 0x08, 0x02, 0x00, 0x00, 0x83, 0x7B, 0x30, 0x00, 0x0F, 0x84, 0xFE, 0x01};
constexpr std::size_t kWmBuckets = 0x08, kWmCount = 0x18, kWmBucketCount = 0x20, kWmNodeId = 0x08, kWmNodeWidget = 0x10;
constexpr std::size_t kSmNavStack = 0x9A8, kSmCurrent = 0xA28, kSmNext = 0xA2C;
constexpr int kSmNavDepth = 32;
constexpr std::size_t kWInputTimer = 0x28, kWAlwaysInput = 0x2D, kWActions = 0x30, kWDevices = 0x50;
constexpr std::size_t kWsPlayer = 0x170, kWsFlags = 0x1C8, kPlayerInstance = 0x31A8, kPlayerSlotA = 0x369C,
                      kPlayerSlotB = 0x36A8;
constexpr int kMaxWidgets = 512;
// (v0.9.587) the system messages and the other input gates
constexpr std::uintptr_t kRvaNotifyGet = 0x00401F60ull, kRvaWidgetIsInputAllowed = 0x0007B500ull,
                         kRvaExclusiveWidget = 0x00FB3AF4ull, kRvaNotifyForceClear = 0x00401560ull,
                         kRvaTimerGetTimeMs = 0x003C8DA0ull, kRvaNotifyProcess = 0x008BCE10ull;
// mov rax, [rip + the manager (0x2D30AA8)]; ret
constexpr std::uint8_t kNotifyGetSig[] = {0x48, 0x8B, 0x05, 0x41, 0xEB, 0x92, 0x02, 0xC3};
// push rbx; sub rsp, 20h; mov rbx, rcx; call Get; cmp qword [rax+28h], 0; jne; mov edx, [the exclusive id]; cmp edx, -1;
// je; cmp edx, [rbx+18h]; jne
constexpr std::uint8_t kWidgetIsInputAllowedSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xE8, 0x52, 0x6A,
                                                     0x38, 0x00, 0x48, 0x83, 0x78, 0x28, 0x00, 0x75, 0x18, 0x8B, 0x15, 0xD9,
                                                     0x85, 0xF3, 0x00, 0x83, 0xFA, 0xFF, 0x74, 0x05, 0x3B, 0x53, 0x18, 0x75};
// ... mov rax, [rcx+28h]; ...; test rax, rax; je; cmp [rax+20h], bpl; jne; test byte [rax+0Ch], 40h; jne
constexpr std::uint8_t kNotifyForceClearSig[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x56, 0x48,
                                                 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x41, 0x28, 0x40, 0x32, 0xED, 0x0F, 0xB6,
                                                 0xDA, 0x48, 0x8B, 0xF1, 0x48, 0x85, 0xC0, 0x74, 0x1A, 0x40, 0x38, 0x68,
                                                 0x20, 0x75, 0x06, 0xF6, 0x40, 0x0C, 0x40, 0x75};
constexpr std::uint8_t kTimerGetTimeMsSig[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xFF, 0x15};
// NotificationDisplayManager::Process: the base's Process, cmp byte [rbx+36h], 0 (its layout: +0x28, +0x36, +0x4C)
constexpr std::uint8_t kNotifyProcessSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0xD9, 0xE8, 0xD2, 0x75, 0xB5,
                                              0xFF, 0x80, 0x7B, 0x36, 0x00, 0x0F, 0x84};
constexpr std::size_t kNmQueue = 0x20, kNmCurrent = 0x28, kNmTimer = 0x30, kNmUiOn = 0x36, kNmDialog = 0x4C;
constexpr std::size_t kMsgType = 0x04, kMsgId = 0x08, kMsgFlags = 0x0C, kMsgTimed = 0x20, kMsgWide = 0x21,
                      kMsgText = 0x22, kMsgWideText = 0x422, kMsgWidgetType = 0x1834;
constexpr std::size_t kWNoHeldBefore = 0x40, kWNoInputBefore = 0x48, kWId = 0x18, kSavingIconTimer = 0x170;
// (v0.9.588) the menu state manager's change
constexpr std::uintptr_t kRvaMenuProcess = 0x008BD3F0ull, kRvaMenuChangeState = 0x008AEED0ull;
// mov [rsp+18h], rbx; push rbp; sub rsp, 40h; inc dword [rcx+0AA4h]; mov rbx, rcx
constexpr std::uint8_t kMenuProcessSig[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x48, 0x83, 0xEC, 0x40,
                                            0xFF, 0x81, 0xA4, 0x0A, 0x00, 0x00, 0x48, 0x8B, 0xD9};
// ... lea rsi, [rcx+0A28h]; ...; lea rbx, [rcx+0A2Ch]; ...; xorps xmm0, xmm0; comiss xmm0, [rcx+0A98h]; jb
constexpr std::uint8_t kMenuChangeStateSig[] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8D, 0xB1, 0x28, 0x0A,
    0x00, 0x00, 0x48, 0x8B, 0xF9, 0x48, 0x63, 0x16, 0x48, 0x8D, 0x99, 0x2C, 0x0A, 0x00, 0x00, 0x8B, 0x03, 0x3B, 0xC2, 0x0F,
    0x84, 0x84, 0x00, 0x00, 0x00, 0x83, 0xF8, 0xFF, 0x74, 0x7F, 0x0F, 0x57, 0xC0, 0x0F, 0x2F, 0x81, 0x98, 0x0A, 0x00, 0x00,
    0x72};
constexpr std::size_t kSmDelay = 0xA98, kSmDepth = 0xAA4;

using PressedFn = void(__fastcall*)(void* widget, int action, char value);
using BoolOfFn = bool(__fastcall*)(void*);
using ForceClearFn = bool(__fastcall*)(void* manager, bool queue);
using TimeMsFn = std::uint64_t(__cdecl*)();
PressedFn g_originalPressed = nullptr;
bool g_ready = false, g_gatesReady = false;
bool g_notifyReady = false;   // (v0.9.587) the system messages' reads and ForceClearMessage checked
bool g_menuReady = false;     // (v0.9.588) the menu state manager's change checked
std::atomic<ULONGLONG> g_lastPressedAt{0};
std::atomic<int> g_pressedLogs{0}, g_pressedPlain{0}, g_censusLogs{0};

std::uintptr_t Fn(std::uintptr_t rva) { return GameBuild::Base() + rva; }

// The object's class name from its RTTI (".?AVUiWidgetWeaponSelect@@" -> "UiWidgetWeaponSelect").
bool ClassName(const void* object, char* out, std::size_t size) noexcept {
    out[0] = '\0';
    __try {
        const std::uintptr_t base = GameBuild::Base();
        const auto vtable = *reinterpret_cast<const std::uintptr_t*>(object);
        if (vtable < base) return false;
        const auto locator = *reinterpret_cast<const std::uintptr_t*>(vtable - 8);
        if (!locator || *reinterpret_cast<const std::uint32_t*>(locator) != 1) return false;
        if (base + static_cast<std::uintptr_t>(*reinterpret_cast<const std::int32_t*>(locator + 0x14)) != locator) return false;
        const char* name = reinterpret_cast<const char*>(
            base + static_cast<std::uintptr_t>(*reinterpret_cast<const std::int32_t*>(locator + 0x0C)) + 0x10);
        if (std::strncmp(name, ".?AV", 4) == 0) name += 4;
        std::size_t i = 0;
        for (; name[i] && !(name[i] == '@' && name[i + 1] == '@') && i + 1 < size; ++i) out[i] = name[i];
        out[i] = '\0';
        return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
        return false;
    }
}

// Every widget of the widget manager (at most max), with its id.
int Widgets(void** widgets, std::uint32_t* ids, int max) noexcept {
    int n = 0;
    __try {
        void* manager = reinterpret_cast<void*(__fastcall*)()>(Fn(kRvaWidgetManagerGet))();
        if (!manager) return 0;
        auto* m = static_cast<std::uint8_t*>(manager);
        auto* const* buckets = *reinterpret_cast<void** const*>(m + kWmBuckets);
        const std::uint64_t count = *reinterpret_cast<std::uint64_t*>(m + kWmCount);
        const std::uint64_t bucketCount = *reinterpret_cast<std::uint64_t*>(m + kWmBucketCount);
        if (!buckets || !count || !bucketCount || bucketCount > 65536) return 0;
        int walked = 0;
        for (std::uint64_t b = 0; b < bucketCount && n < max; ++b)
            for (void* node = buckets[b]; node && n < max && walked < 16384; node = *static_cast<void**>(node), ++walked) {
                void* widget = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(node) + kWmNodeWidget);
                if (!widget) continue;
                bool seen = false;
                for (int i = 0; i < n && !seen; ++i) seen = widgets[i] == widget;
                if (seen) continue;
                widgets[n] = widget;
                ids[n] = *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(node) + kWmNodeId);
                ++n;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

bool Visible(void* widget) noexcept {
    __try {
        return reinterpret_cast<BoolOfFn>(Fn(kRvaWidgetIsVisible))(widget);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool MenuStates(std::int32_t& current, std::int32_t& next, std::int32_t* stack) noexcept {
    __try {
        auto* mgr = *reinterpret_cast<std::uint8_t* const*>(Fn(kRvaStateManager));
        if (!mgr) return false;
        current = *reinterpret_cast<std::int32_t*>(mgr + kSmCurrent);
        next = *reinterpret_cast<std::int32_t*>(mgr + kSmNext);
        if (stack) std::memcpy(stack, mgr + kSmNavStack, sizeof(std::int32_t) * kSmNavDepth);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uint8_t PauseByte() noexcept {
    __try {
        return *reinterpret_cast<const std::uint8_t*>(Fn(kRvaWorldPauseByte));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xFF;
    }
}

void* FindWidget(const char* cls) {
    static void* widgets[kMaxWidgets];
    static std::uint32_t ids[kMaxWidgets];
    const int n = Widgets(widgets, ids, kMaxWidgets);
    char name[96];
    for (int i = 0; i < n; ++i)
        if (ClassName(widgets[i], name, sizeof(name)) && std::strcmp(name, cls) == 0) return widgets[i];
    return nullptr;
}

// (v0.9.587) The system message manager's state.
struct Notify {
    bool read = false;
    void* current = nullptr;
    std::int32_t type = 0, id = 0, flags = 0, widgetType = 0, dialog = -1, exclusive = -1;
    std::uint8_t timed = 0, uiOn = 0;
    float timer = 0.0f;
    int queued = 0;
    char text[96] = "";
};

bool ReadNotify(Notify& n) noexcept {
    __try {
        auto* m = *reinterpret_cast<std::uint8_t* const*>(Fn(0x02D30AA8ull));
        n.exclusive = *reinterpret_cast<const std::int32_t*>(Fn(kRvaExclusiveWidget));
        if (!m) return false;
        n.uiOn = m[kNmUiOn];
        n.timer = *reinterpret_cast<float*>(m + kNmTimer);
        n.dialog = *reinterpret_cast<std::int32_t*>(m + kNmDialog);
        // the queue: a ring of nodes {next, prev, message} from a head node (+0x20)
        auto* head = *reinterpret_cast<std::uint8_t* const*>(m + kNmQueue);
        if (head)
            for (auto* node = *reinterpret_cast<std::uint8_t* const*>(head); node && node != head && n.queued < 64;
                 node = *reinterpret_cast<std::uint8_t* const*>(node))
                ++n.queued;
        auto* msg = *reinterpret_cast<std::uint8_t* const*>(m + kNmCurrent);
        n.current = msg;
        if (msg) {
            n.type = *reinterpret_cast<std::int32_t*>(msg + kMsgType);
            n.id = *reinterpret_cast<std::int32_t*>(msg + kMsgId);
            n.flags = *reinterpret_cast<std::int32_t*>(msg + kMsgFlags);
            n.timed = msg[kMsgTimed];
            n.widgetType = *reinterpret_cast<std::int32_t*>(msg + kMsgWidgetType);
            std::size_t i = 0;
            if (msg[kMsgWide]) {
                const auto* w = reinterpret_cast<const wchar_t*>(msg + kMsgWideText);
                for (; i + 1 < sizeof(n.text) && w[i]; ++i) n.text[i] = w[i] < 0x80 ? static_cast<char>(w[i]) : '?';
            } else {
                const auto* c = reinterpret_cast<const char*>(msg + kMsgText);
                for (; i + 1 < sizeof(n.text) && c[i]; ++i) n.text[i] = c[i] == '\n' || c[i] == '\r' ? ' ' : c[i];
            }
            n.text[i] = '\0';
        }
        n.read = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float SavingIconTimer(void* icon) noexcept {
    __try {
        return *reinterpret_cast<float*>(static_cast<std::uint8_t*>(icon) + kSavingIconTimer);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1.0f;
    }
}

// A widget by its id (nullptr: none).
void* WidgetById(std::int32_t id) {
    if (id < 0) return nullptr;
    static void* widgets[kMaxWidgets];
    static std::uint32_t ids[kMaxWidgets];
    const int n = Widgets(widgets, ids, kMaxWidgets);
    for (int i = 0; i < n; ++i)
        if (ids[i] == static_cast<std::uint32_t>(id)) return widgets[i];
    return nullptr;
}

void LogNotify(const char* why) {
    Notify n;
    if (!g_notifyReady || !ReadNotify(n)) return;
    char dialog[96] = "none";
    if (n.dialog != -1) {
        void* w = WidgetById(n.dialog);
        char name[64];
        if (!w) std::snprintf(dialog, sizeof(dialog), "%d (no such widget)", n.dialog);
        else
            std::snprintf(dialog, sizeof(dialog), "%d %s (%s)", n.dialog, ClassName(w, name, sizeof(name)) ? name : "?",
                          Visible(w) ? "visible" : "hidden");
    }
    char exclusive[64] = "none (-1)";
    if (n.exclusive != -1)
        std::snprintf(exclusive, sizeof(exclusive), "%d%s", n.exclusive, WidgetById(n.exclusive) ? "" : " (no such widget)");
    if (n.current)
        Log::Writef("HUD: %s: the system message on display %p: type %d, id %d, flags 0x%X, +0x20 %u, dialog widget type "
                    "0x%X, \"%s\"; %d more queued, timer %.2f, +0x36 %u, its dialog %s; exclusive input widget %s", why,
                    n.current, n.type, n.id, n.flags, n.timed, n.widgetType, n.text, n.queued, n.timer, n.uiOn, dialog,
                    exclusive);
    else
        Log::Writef("HUD: %s: no system message on display (%d queued, +0x36 %u, dialog %s); exclusive input widget %s", why,
                    n.queued, n.uiOn, dialog, exclusive);
}

void Census(const char* why) {
    static void* widgets[kMaxWidgets];
    static std::uint32_t ids[kMaxWidgets];
    std::int32_t current = -2, next = -2, stack[kSmNavDepth];
    char menus[256] = "?";
    if (MenuStates(current, next, stack)) {
        int at = 0;
        menus[0] = '\0';
        for (int i = 0; i < kSmNavDepth && at < static_cast<int>(sizeof(menus)) - 12; ++i)
            if (stack[i] != -1) at += std::snprintf(menus + at, sizeof(menus) - at, "%s0x%X", at ? " " : "", stack[i]);
    }
    const int n = Widgets(widgets, ids, kMaxWidgets);
    char shown[2048], hidden[2048];
    int at = 0, hiddenAt = 0, visible = 0;
    shown[0] = hidden[0] = '\0';
    char name[96];
    for (int i = 0; i < n; ++i) {
        const bool isVisible = Visible(widgets[i]);
        if (isVisible) ++visible;
        if (!ClassName(widgets[i], name, sizeof(name))) std::snprintf(name, sizeof(name), "?");
        char* list = isVisible ? shown : hidden;
        int& pos = isVisible ? at : hiddenAt;
        if (pos < static_cast<int>(sizeof(shown)) - 120)
            pos += std::snprintf(list + pos, sizeof(shown) - pos, "%s%s %u", pos ? ", " : "", name, ids[i]);
    }
    Log::Writef("HUD: %s. Menus: current %d, next %d, navigation stack [%s]; world pause byte %u. Visible widgets (%d of "
                "%d): %s; hidden (v0.9.587): %s", why, current, next, menus, PauseByte(), visible, n, shown, hidden);
    LogNotify(why);   // (v0.9.587)
}

struct Gates {
    std::uint8_t pause = 0, always = 0, flags = 0;
    bool visible = false;
    std::uint32_t devices = 0, actions = 0;
    float timer = 0.0f;
    std::uintptr_t vfunc60 = 0;
    int inputAllowed = -1;                       // (v0.9.587) vfunc +0x60's answer (-1: not asked)
    std::uint64_t noInputBefore = 0, noHeldBefore = 0, now = 0;
    void* player = nullptr;
    void* instance = nullptr;
    std::int32_t slotA = 0, slotB = 0;
};

bool ReadGates(void* w, Gates& g) noexcept {
    __try {
        auto* p = static_cast<std::uint8_t*>(w);
        g.pause = PauseByte();
        g.devices = *reinterpret_cast<std::uint32_t*>(p + kWDevices);
        g.always = p[kWAlwaysInput];
        g.timer = *reinterpret_cast<float*>(p + kWInputTimer);
        g.actions = *reinterpret_cast<std::uint32_t*>(p + kWActions);
        g.flags = p[kWsFlags];
        g.vfunc60 = (*reinterpret_cast<std::uintptr_t**>(w))[0x60 / 8] - GameBuild::Base();
        if (g_notifyReady) {   // (v0.9.587) IsInputAllowed only reads; the timer is the one ProcessInput asks
            if (g.vfunc60 == kRvaWidgetIsInputAllowed) g.inputAllowed = reinterpret_cast<BoolOfFn>(Fn(kRvaWidgetIsInputAllowed))(w) ? 1 : 0;
            g.noInputBefore = *reinterpret_cast<std::uint64_t*>(p + kWNoInputBefore);
            g.noHeldBefore = *reinterpret_cast<std::uint64_t*>(p + kWNoHeldBefore);
            g.now = reinterpret_cast<TimeMsFn>(Fn(kRvaTimerGetTimeMs))();
        }
        g.player = *reinterpret_cast<void**>(p + kWsPlayer);
        if (g.player) {
            auto* pl = static_cast<std::uint8_t*>(g.player);
            g.instance = *reinterpret_cast<void**>(pl + kPlayerInstance);
            g.slotA = *reinterpret_cast<std::int32_t*>(pl + kPlayerSlotA);
            g.slotB = *reinterpret_cast<std::int32_t*>(pl + kPlayerSlotB);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// What OnInputActionPressed will decide: blocked by the moveset, and the access check (bit 3 set for the call, as the
// game sets it, the byte put back after).
bool Decide(void* w, bool& blocked, bool& access) noexcept {
    __try {
        auto* p = static_cast<std::uint8_t*>(w);
        void* player = *reinterpret_cast<void**>(p + kWsPlayer);
        blocked = player && reinterpret_cast<BoolOfFn>(Fn(kRvaWeaponSelectBlocked))(player);
        const std::uint8_t flags = p[kWsFlags];
        p[kWsFlags] = static_cast<std::uint8_t>(flags | 8);
        access = reinterpret_cast<BoolOfFn>(Fn(kRvaWeaponSelectAccess))(w);
        p[kWsFlags] = flags;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall HookPressed(void* widget, int action, char value) {
    g_lastPressedAt.store(GetTickCount64(), std::memory_order_relaxed);
    bool blocked = false, access = true;
    if (widget && Decide(widget, blocked, access)) {
        const bool refused = blocked || !access;
        const bool log = refused ? g_pressedLogs.fetch_add(1) < 12 : g_pressedPlain.fetch_add(1) < 2;
        if (log) {
            Gates g;
            ReadGates(widget, g);
            std::int32_t current = -2, next = -2;
            MenuStates(current, next, nullptr);
            Log::Detailf("HUD: the weapon select got input action %d (value %d): %s; menus current %d next %d, world pause "
                        "byte %u, player %p (Instance %p, +0x369C %d, +0x36A8 %d), flags 0x%02X", action, value,
                        blocked ? "refused: the player's moveset blocks weapon select (Player::GetOverrideWeaponSelectBlocked)"
                        : !access ? "refused by its access check (PassesAccessCheck)" : "accepted",
                        current, next, g.pause, g.player, g.instance, g.slotA, g.slotB, g.flags);
        }
    }
    g_originalPressed(widget, action, value);
}

bool ForceClearGuarded(void* manager, bool& cleared) noexcept {
    __try {
        cleared = reinterpret_cast<ForceClearFn>(Fn(kRvaNotifyForceClear))(manager, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* NotifyManager() noexcept {
    __try {
        return *reinterpret_cast<void* const*>(Fn(0x02D30AA8ull));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool ResetExclusive(std::int32_t expected) noexcept {
    __try {
        auto* id = reinterpret_cast<std::int32_t*>(Fn(kRvaExclusiveWidget));
        if (*id != expected) return false;
        *id = -1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// (v0.9.587) A system message on display that the player cannot see (its dialog widget gone or hidden) or an exclusive
// input id of a widget that no longer exists, 4 s in a row in play: put right (see the top).
void RecoverInput(ULONGLONG now) {
    static ULONGLONG s_next = 0, s_messageSince = 0, s_exclusiveSince = 0;
    static void* s_message = nullptr;
    static std::int32_t s_exclusive = -1;
    static int s_fixes = 0;
    if (!g_notifyReady || now < s_next || s_fixes >= 8) return;
    s_next = now + 250;
    std::int32_t current = -2, next = -2;
    Features::PlayerTransform lara{};
    const bool play = MenuStates(current, next, nullptr) && current == -1 && !Features::LoadingProfilerSessionActive() &&
                      Features::GetPlayerTransform(lara);
    Notify n;
    if (!play || !ReadNotify(n)) {
        s_messageSince = s_exclusiveSince = 0;
        return;
    }
    // the message: no dialog the player can see
    bool unseen = false;
    if (n.current) {
        void* dialog = WidgetById(n.dialog);
        unseen = !dialog || !Visible(dialog);
    }
    if (!unseen || n.current != s_message) {
        s_message = unseen ? n.current : nullptr;
        s_messageSince = unseen ? now : 0;
    } else if (s_messageSince && now - s_messageSince >= 4000) {
        s_messageSince = 0;
        ++s_fixes;
        LogNotify("a system message has had no dialog on screen for 4 s in play; every HUD input refused meanwhile");
        bool cleared = false;
        void* manager = NotifyManager();
        const bool called = manager && ForceClearGuarded(manager, cleared);
        Log::Writef("HUD: the system message %s", !called ? "could not be cleared (an exception)"
                                                  : cleared ? "cleared (ForceClearMessage): the HUD takes input again"
                                                            : "was not cleared: the game keeps it (flags 0x40)");
        Census("after clearing the system message");
    }
    // the exclusive input id: its widget gone
    const bool stale = n.exclusive != -1 && !WidgetById(n.exclusive);
    if (!stale || n.exclusive != s_exclusive) {
        s_exclusive = stale ? n.exclusive : -1;
        s_exclusiveSince = stale ? now : 0;
    } else if (s_exclusiveSince && now - s_exclusiveSince >= 4000) {
        s_exclusiveSince = 0;
        ++s_fixes;
        if (ResetExclusive(n.exclusive))
            Log::Writef("HUD: the exclusive input widget %d no longer exists: its claim released (every other widget's input "
                        "was refused)", n.exclusive);
    }
}

// (v0.9.588) The menu state manager's change: read, and put right (see the top).
struct MenuChange {
    std::int32_t current = -2, next = -2, depth = 0;
    float delay = 0.0f;
    std::uint32_t delayBits = 0;
};

bool ReadMenuChange(MenuChange& m) noexcept {
    __try {
        auto* mgr = *reinterpret_cast<std::uint8_t* const*>(Fn(kRvaStateManager));
        if (!mgr) return false;
        m.current = *reinterpret_cast<std::int32_t*>(mgr + kSmCurrent);
        m.next = *reinterpret_cast<std::int32_t*>(mgr + kSmNext);
        m.depth = *reinterpret_cast<std::int32_t*>(mgr + kSmDepth);
        std::memcpy(&m.delayBits, mgr + kSmDelay, sizeof(m.delayBits));
        std::memcpy(&m.delay, mgr + kSmDelay, sizeof(m.delay));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteMenuChange(const float* delay, const std::int32_t* depth) noexcept {
    __try {
        auto* mgr = *reinterpret_cast<std::uint8_t* const*>(Fn(kRvaStateManager));
        if (!mgr) return false;
        if (delay) std::memcpy(mgr + kSmDelay, delay, sizeof(*delay));
        if (depth) *reinterpret_cast<std::int32_t*>(mgr + kSmDepth) = *depth;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RecoverMenu(ULONGLONG now) {
    static ULONGLONG s_next = 0, s_since = 0;
    static std::int32_t s_wanted = -1, s_lowestDepth = 0x7FFFFFFF, s_depthAtStart = 0;
    static float s_delayAtStart = 0.0f;
    static int s_fixes = 0, s_logs = 0;
    if (!g_menuReady || now < s_next) return;
    s_next = now + 250;
    MenuChange m;
    if (!ReadMenuChange(m)) return;
    if (m.depth >= 0 && m.depth < s_lowestDepth) s_lowestDepth = m.depth;
    const bool pending = m.next != -1 && m.next != m.current && m.next >= 0;
    if (!pending || m.next != s_wanted) {
        s_wanted = pending ? m.next : -1;
        s_since = pending ? now : 0;
        s_delayAtStart = m.delay;
        s_depthAtStart = m.depth;
        return;
    }
    if (!s_since || now - s_since < 3000) return;
    const bool badDelay = !std::isfinite(m.delay) || m.delay > 30.0f;
    const bool stuckDelay = std::isfinite(m.delay) && m.delay > 0.0f && m.delay >= s_delayAtStart;   // (not counted down)
    // (raised all the while: the same count at the start of the 3 s and now)
    const bool badDepth = s_lowestDepth != 0x7FFFFFFF && m.depth > s_lowestDepth && m.depth == s_depthAtStart;
    if (s_logs < 8) {
        ++s_logs;
        Log::Writef("HUD: menu state 0x%X asked for %llu ms ago is not entered: current 0x%X, its delay %g (bits 0x%08X, %s), "
                    "Process's re-entry count %d (the lowest seen %d)", m.next, static_cast<unsigned long long>(now - s_since),
                    m.current, static_cast<double>(m.delay), m.delayBits,
                    badDelay ? "not a usable time" : stuckDelay ? "not counted down" : m.delay > 0.0f ? "counting down" : "over",
                    m.depth, s_lowestDepth == 0x7FFFFFFF ? m.depth : s_lowestDepth);
        Census("a menu change that does not come");
    }
    if (s_fixes < 8 && (badDelay || badDepth)) {
        ++s_fixes;
        const float zero = 0.0f;
        const std::int32_t depth = s_lowestDepth;
        if (WriteMenuChange(badDelay ? &zero : nullptr, badDepth ? &depth : nullptr))
            Log::Writef("HUD: the menu change put right:%s%s", badDelay ? " its delay set to 0" : "",
                        badDepth ? " Process's re-entry count set back to the lowest seen (one Process had not ended)" : "");
    }
    s_since = now;   // (the next look in 3 s)
    s_delayAtStart = m.delay;
    s_depthAtStart = m.depth;
}

// ---- (v0.9.589) Enhancements > Gameplay > "Weapon select at the bottom left" ---------------------------------------
// UiWidgetWeaponSelect (widget type 0x18) has a Scaleform movie of its own (+0x98; its clip under _root, +0xA8, is "":
// the widget is the movie's _root). Its Process (retail 0x9ABFA0, TR2 0x1409F1330) is hooked: after the game's, while
// the option is on, the movie's _root gets a position and scale of ours (GFxValue::ObjectInterface Get / SetMember,
// retail 0xAF4A70 / 0xAF6A10, as CustomOutfits'): its content's bounds in its own space (_root.getBounds(_root), read
// four times a second while the widget is visible, taken once two reads in a row agree: not during a slide), the
// stage (Stage.width / Stage.height,
// 1280 x 720 if they cannot be read) and the game's own _x, _y, _xscale, _yscale (taken again whenever they differ
// from ours: the game set them) give the content's place on the stage; it is put as far from the left edge as it was
// from the right one, its bottom where it was, at 80% of its size. Off: the game's values are put back once.
// (v0.9.590) 0.9.589's log: the game's values 0, 0, 100% (content 417.5..872.4 x 242.9..689.1 on the 1280 x 720 stage),
// then ours taken for the game's four times over (80% -> 64% -> 51% -> 41% -> 33%): the movie keeps _x / _y in twips
// (1/20) and the scales rounded, so the values read back were never exactly ours. Now ours are what the movie gives back
// after they are set, compared within 0.1. The mirrored margin (407.6: the movie's viewport is not the screen) is gone:
// the content's left edge goes 24 stage units from the stage's (where 0.9.589's last step put it, the place the user
// kept), its bottom where the game has it, at "Weapon select size" ([Gameplay] WeaponSelectSize, 30..100% of the
// game's, default 65: twice the 33% seen).
constexpr std::uintptr_t kRvaWeaponSelectProcess = 0x009ABFA0ull, kRvaWidgetGetVariable = 0x0098A680ull,
                         kRvaGfxGetMember = 0x00AF4A70ull, kRvaGfxSetMember = 0x00AF6A10ull,
                         kRvaGfxInvoke = 0x00AF4F00ull, kRvaGfxObjectRelease = 0x00AF5700ull;
// push rbx; sub rsp, 160h; movaps [rsp+150h], xmm6; mov rbx, rcx; movaps xmm6, xmm1; call (UiWidgetScaleform::Process)
constexpr std::uint8_t kWeaponSelectProcessSig[] = {0x40, 0x53, 0x48, 0x81, 0xEC, 0x60, 0x01, 0x00, 0x00, 0x0F, 0x29, 0xB4,
                                                    0x24, 0x50, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0x0F, 0x28, 0xF1, 0xE8};
constexpr std::uint8_t kWidgetGetVariableSig[] = {0x48, 0x8B, 0x89, 0x98, 0x00, 0x00, 0x00, 0x48,
                                                  0x85, 0xC9, 0x75, 0x03, 0x32, 0xC0, 0xC3, 0x48};
constexpr std::uint8_t kGfxGetMemberSig[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x57, 0x41,
                                             0x56, 0x48, 0x83, 0xEC, 0x40, 0x80, 0xBC, 0x24};
constexpr std::uint8_t kGfxSetMemberSig[] = {0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41,
                                             0x56, 0x48, 0x83, 0xEC, 0x50, 0x80, 0xBC, 0x24};
constexpr std::uint8_t kGfxInvokeSig[] = {0x40, 0x53, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57,
                                          0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00, 0x80};
constexpr std::uint8_t kGfxObjectReleaseSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x42,
                                                 0x08, 0x49, 0x8B, 0xD8, 0x25, 0x8F, 0x00, 0x00};
constexpr double kWeaponSelectLeft = 24.0;   // (v0.9.590) the content's left edge on the stage
constexpr double kDefaultStageWidth = 1280.0, kDefaultStageHeight = 720.0;

struct Gfx {   // Scaleform GFx 3 GFxValue
    void* iface = nullptr;
    std::uint32_t type = 0;   // 0 undefined, 2 bool, 3 number, 4 string, 8 display object; 0x40 managed
    std::uint32_t pad = 0;
    union {
        double number;
        void* data;
    };
    Gfx() : data(nullptr) {}
};
static_assert(sizeof(Gfx) == 24, "GFxValue");
using GfxGetMemberFn = bool(__fastcall*)(void*, void*, const char*, Gfx*, bool);
using GfxSetMemberFn = bool(__fastcall*)(void*, void*, const char*, const Gfx*, bool);
using GfxInvokeFn = bool(__fastcall*)(void*, void*, Gfx*, const char*, const Gfx*, std::size_t, bool);
using GfxReleaseFn = void(__fastcall*)(void*, Gfx*, void*);
using WidgetGetVariableFn = bool(__fastcall*)(void*, Gfx*, const char*, const char*);
using WidgetProcessFn = void(__fastcall*)(void*, float);

WidgetProcessFn g_originalWeaponSelectProcess = nullptr;
bool g_layoutReady = false;
std::atomic_bool g_weaponSelectBottomLeft{false};
std::atomic<int> g_weaponSelectSize{65};   // (v0.9.590) percent of the game's size

bool GfxDisplay(const Gfx& v) { return (v.type & 0x8F) == 8; }
bool GfxObject(const Gfx& v) { return v.iface && ((v.type & 0x8F) == 8 || (v.type & 0x8F) == 6); }
void GfxFree(Gfx& v) {
    if ((v.type & 0x40) && v.iface) reinterpret_cast<GfxReleaseFn>(Fn(kRvaGfxObjectRelease))(v.iface, &v, v.data);
    v.iface = nullptr;
    v.type = 0;
}
double GfxNum(const Gfx& object, const char* name, double fallback) {
    Gfx v;
    if (!object.iface ||
        !reinterpret_cast<GfxGetMemberFn>(Fn(kRvaGfxGetMember))(object.iface, object.data, name, &v, GfxDisplay(object)))
        return fallback;
    const double n = (v.type & 0x8F) == 3 ? v.number : fallback;
    GfxFree(v);
    return n;
}
bool GfxPut(const Gfx& object, const char* name, double number) {
    Gfx v;
    v.type = 3;
    v.number = number;
    return object.iface &&
           reinterpret_cast<GfxSetMemberFn>(Fn(kRvaGfxSetMember))(object.iface, object.data, name, &v, GfxDisplay(object));
}
bool MovieVariable(void* widget, const char* path, Gfx& out) {
    return reinterpret_cast<WidgetGetVariableFn>(Fn(kRvaWidgetGetVariable))(widget, &out, path, "");
}

struct WeaponSelectLayout {
    void* widget = nullptr;
    bool known = false;      // the game's values below
    bool applied = false;    // ours are on the movie
    double game[4] = {};     // _x, _y, _xscale, _yscale as the game set them
    double ours[4] = {};
    // xMin, yMin, xMax, yMax (the content in _root's space). (v0.9.591) Placed at once with the ones 0.9.589's log measured
    // (the weapon select's movie is the same everywhere), then measured again while it shows: if they differ, moved.
    double bounds[4] = {417.5, 242.9, 872.4, 689.1};
    bool haveBounds = true;
    bool measured = false;
    double previous[4] = {};   // the last read
    bool havePrevious = false;
    double stage[2] = {kDefaultStageWidth, kDefaultStageHeight};
    bool stageRead = false;
    ULONGLONG nextBounds = 0;
    bool stageTried = false;
    int size = 0;   // (v0.9.590) the size ours were made for
    int logs = 0, gameMoves = 0;
};
WeaponSelectLayout g_layout;

void ComputeWeaponSelectPlace(WeaponSelectLayout& l) {
    const double gx = l.game[0], gy = l.game[1], gsx = l.game[2] / 100.0, gsy = l.game[3] / 100.0;
    const double x0 = gx + gsx * l.bounds[0], x1 = gx + gsx * l.bounds[2], y1 = gy + gsy * l.bounds[3];
    l.size = std::clamp(g_weaponSelectSize.load(std::memory_order_relaxed), 30, 100);
    const double k = l.size / 100.0, margin = kWeaponSelectLeft;
    const double sx = gsx * k, sy = gsy * k;
    l.ours[2] = l.game[2] * k;
    l.ours[3] = l.game[3] * k;
    l.ours[0] = margin - sx * l.bounds[0];
    l.ours[1] = y1 - sy * l.bounds[3];
    if (l.logs < 6) {
        ++l.logs;
        Log::Detailf("HUD: the weapon select at the bottom left: its content %.1f..%.1f x %.1f..%.1f in its movie (on the "
                    "%.0f x %.0f stage%s: %.1f..%.1f, bottom %.1f, at %.1f, %.1f, %.0f%% x %.0f%%); now at %.1f, %.1f, "
                    "%.0f%% x %.0f%% (%.0f from the left edge)", l.bounds[0], l.bounds[2], l.bounds[1], l.bounds[3],
                    l.stage[0], l.stage[1], l.stageRead ? "" : ", not read: assumed", x0, x1, y1, l.game[0], l.game[1],
                    l.game[2], l.game[3], l.ours[0], l.ours[1], l.ours[2], l.ours[3], margin);
    }
}

void LayoutWeaponSelect(void* widget) {
    WeaponSelectLayout& l = g_layout;
    const bool on = g_weaponSelectBottomLeft.load(std::memory_order_relaxed);
    if (widget != l.widget) {   // (another widget: its movie is new)
        if (!on && !l.applied) return;
        l = WeaponSelectLayout{};
        l.widget = widget;
    }
    if (!on && !l.applied) return;
    Gfx root;
    if (!MovieVariable(widget, "_root", root) || !GfxObject(root)) {
        GfxFree(root);
        return;
    }
    static const char* const kProps[4] = {"_x", "_y", "_xscale", "_yscale"};
    double now[4];
    for (int k = 0; k < 4; ++k) now[k] = GfxNum(root, kProps[k], NAN);
    bool finite = true;
    for (double v : now) finite = finite && std::isfinite(v);
    if (!finite) {
        GfxFree(root);
        return;
    }
    auto same = [](const double* a, const double* b) {   // (v0.9.590: within 0.1, the movie rounds what it keeps)
        for (int k = 0; k < 4; ++k)
            if (std::fabs(a[k] - b[k]) > 0.1) return false;
        return true;
    };
    if (!on) {   // back to the game's values (unless the game has set its own meanwhile)
        if (l.applied && l.known && same(now, l.ours))
            for (int k = 0; k < 4; ++k) GfxPut(root, kProps[k], l.game[k]);
        l.applied = false;
        GfxFree(root);
        Log::Write("HUD: the weapon select back where the game puts it");
        return;
    }
    bool recompute = false;
    if (!l.known || !(l.applied && same(now, l.ours))) {   // the game's own values (first, or it set them again)
        if (l.known && l.applied && l.gameMoves++ < 4)
            Log::Detailf("HUD: the game moved the weapon select's movie again (%.1f, %.1f, %.0f%%)", now[0], now[1], now[2]);
        std::memcpy(l.game, now, sizeof(now));
        l.known = true;
        l.applied = false;
        recompute = true;
    }
    // the content's bounds while it is shown, taken once two reads in a row agree (not in the middle of a slide)
    const ULONGLONG tick = GetTickCount64();
    if (!l.measured && tick >= l.nextBounds && Visible(widget)) {
        l.nextBounds = tick + 250;
        Gfx bounds, space = root;
        if (reinterpret_cast<GfxInvokeFn>(Fn(kRvaGfxInvoke))(root.iface, root.data, &bounds, "getBounds", &space, 1,
                                                            GfxDisplay(root)) && GfxObject(bounds)) {
            const double b[4] = {GfxNum(bounds, "xMin", NAN), GfxNum(bounds, "yMin", NAN), GfxNum(bounds, "xMax", NAN),
                                 GfxNum(bounds, "yMax", NAN)};
            const bool ok = std::isfinite(b[0]) && std::isfinite(b[1]) && std::isfinite(b[2]) && std::isfinite(b[3]) &&
                            b[2] - b[0] > 4.0 && b[3] - b[1] > 4.0 && b[2] - b[0] < 20000.0;
            if (ok) {
                bool steady = l.havePrevious;
                for (int k = 0; k < 4 && steady; ++k) steady = std::fabs(b[k] - l.previous[k]) <= 0.5;
                std::memcpy(l.previous, b, sizeof(b));
                l.havePrevious = true;
                if (steady) {
                    l.measured = true;
                    bool differ = false;
                    for (int k = 0; k < 4; ++k) differ = differ || std::fabs(b[k] - l.bounds[k]) > 1.0;
                    if (differ) {
                        std::memcpy(l.bounds, b, sizeof(b));
                        recompute = true;
                    }
                    l.haveBounds = true;
                }
            }
        }
        GfxFree(bounds);
    }
    if (!l.stageRead && !l.stageTried) {
        l.stageTried = true;
        {
            Gfx w, h;
            if (MovieVariable(widget, "Stage.width", w) && MovieVariable(widget, "Stage.height", h) && (w.type & 0x8F) == 3 &&
                (h.type & 0x8F) == 3 && w.number > 100.0 && h.number > 100.0) {
                l.stage[0] = w.number;
                l.stage[1] = h.number;
                l.stageRead = true;
                recompute = true;
            }
            GfxFree(w);
            GfxFree(h);
        }
    }
    if (!l.haveBounds) {
        GfxFree(root);
        return;
    }
    if (recompute || l.size != std::clamp(g_weaponSelectSize.load(std::memory_order_relaxed), 30, 100))
        ComputeWeaponSelectPlace(l);
    if (!same(now, l.ours)) {
        for (int k = 0; k < 4; ++k) GfxPut(root, kProps[k], l.ours[k]);
        // (v0.9.590) ours as the movie keeps them (rounded), so the next frame knows them for ours
        for (int k = 0; k < 4; ++k) l.ours[k] = GfxNum(root, kProps[k], l.ours[k]);
    }
    l.applied = true;
    GfxFree(root);
}

void LayoutWeaponSelectGuarded(void* widget) noexcept {
    __try {
        LayoutWeaponSelect(widget);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int s_logs = 0;
        if (s_logs++ < 3) Log::Write("HUD: the weapon select's layout raised an exception: left as the game has it");
    }
}

void __fastcall HookWeaponSelectProcess(void* widget, float dt) {
    g_originalWeaponSelectProcess(widget, dt);
    if (widget && (g_weaponSelectBottomLeft.load(std::memory_order_relaxed) || g_layout.applied))
        LayoutWeaponSelectGuarded(widget);
}

bool GameInFront() {
    HWND front = GetForegroundWindow();
    DWORD pid = 0;
    if (front) GetWindowThreadProcessId(front, &pid);
    return pid == GetCurrentProcessId();
}

// ---- (v0.9.595) the widget manager's root ----
// The user, with 0.9.594 running: "the infinite loading bug" again (the saving icon up for good) and the graphics settings
// could not be saved. Read from the running game: the saving icon's timer (+0x170) stood at exactly 3.0 (Process takes
// the frame's time off it each frame); no save going on (WinSave 0x2E1E4A0 +8 status 2, SAVE_Process's request state
// 0x233B844 0, its countdown 0xFF6D70 -1); no menu (+0xA28 -1), the world not paused. cdc::UiWidgetManagerBase::Process
// (retail 0x7D800, TR2 0x14008FD70; the derived UiWidgetManager::Process 0x8BD600 jumps to it) processes the widgets
// only by ProcessRecursive (0x7DFC0) from the root widget, its +0x30 id: 1 there, and no widget 1 in its table (54
// widgets, ids 2..57). The root (a plain UiWidgetBase made once by UiWidgetManagerBase::Init, retail 0x7A6E0, and only
// when +0x30 is -1) had been destroyed, so no widget was processed any more: the saving icon never counted down, the
// button bars were never hidden, the weapon select took no input (0.9.586's report). Its children were the 13 layers
// whose ids UiWidgetManager keeps at +0x44 .. +0x74 (2 .. 14 there), every other widget under one of them.
// Put right: DestroyWidget(id&) (retail 0x78860, both managers' vtables) given the root's id is refused (the caller's id
// set to -1 as when it is destroyed, true returned) and logged with the caller's stack; and should the root be gone
// anyway, Process runs ProcessRecursive on each layer itself (the root's own work), logged once.
constexpr std::uintptr_t kRvaDestroyWidget = 0x00078860ull, kRvaManagerProcess = 0x0007D800ull,
                         kRvaProcessRecursive = 0x0007DFC0ull, kRvaGetWidgetById = 0x0007A560ull,
                         kRvaWidgetFrameScale = 0x02D579B0ull, kRvaWidgetTimeUnit = 0x00D10C00ull;
constexpr std::uint8_t kDestroyWidgetSig[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48,
                                              0x83, 0x79, 0x18, 0x00, 0x48, 0x8D, 0x59, 0x08, 0x4C, 0x63, 0x02};
// push rbx; sub rsp, 20h; cmp qword [rcx+18h], 0; mov rbx, rcx; movss xmm1, [frame scale]; mulss xmm1, [1 / 30];
// movsxd r8, dword [rcx+30h] (the root's id)
constexpr std::uint8_t kManagerProcessSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0x79, 0x18, 0x00, 0x48,
                                               0x8B, 0xD9, 0xF3, 0x0F, 0x10, 0x0D, 0x9A, 0xA1, 0xCD, 0x02, 0xF3, 0x0F,
                                               0x59, 0x0D, 0xE2, 0x33, 0xC9, 0x00, 0x4C, 0x63, 0x41, 0x30};
constexpr std::uint8_t kProcessRecursiveSig[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x0F, 0x29,
                                                 0x74, 0x24, 0x30, 0x48, 0x8B, 0xF9};
constexpr std::uint8_t kGetWidgetByIdSig[] = {0x48, 0x83, 0x79, 0x18, 0x00, 0x4C, 0x63, 0xC2, 0x74, 0x24};
constexpr std::size_t kWmRoot = 0x30, kWmLayers = 0x44;
constexpr int kWmLayerCount = 13;
using DestroyWidgetFn = bool(__fastcall*)(void* manager, std::int32_t* id);
using ManagerProcessFn = void(__fastcall*)(void* manager);
using ProcessRecursiveFn = void(__fastcall*)(void* widget, float dt);
using GetWidgetByIdFn = void*(__fastcall*)(void* manager, std::int32_t id);
DestroyWidgetFn g_originalDestroyWidget = nullptr;
ManagerProcessFn g_originalManagerProcess = nullptr;
bool g_rootGuardReady = false;
std::atomic<int> g_rootRefusals{0};
std::atomic<bool> g_rootLostSaid{false};
std::atomic<std::uint64_t> g_layerProcesses{0};

std::string CallerStack() {
    void* frames[16] = {};
    const USHORT n = RtlCaptureStackBackTrace(2, 16, frames, nullptr);
    const std::uintptr_t game = GameBuild::Base();
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&CallerStack), &self);
    std::string out;
    int shown = 0;
    for (USHORT i = 0; i < n && shown < 10; ++i) {
        const auto a = reinterpret_cast<std::uintptr_t>(frames[i]);
        char one[48];
        if (a >= game && a < game + 0x4000000)
            std::snprintf(one, sizeof(one), "%sROTTR.exe+0x%llX", shown ? " < " : "", static_cast<unsigned long long>(a - game));
        else if (self && a >= reinterpret_cast<std::uintptr_t>(self) && a < reinterpret_cast<std::uintptr_t>(self) + 0x2000000)
            std::snprintf(one, sizeof(one), "%sframework+0x%llX", shown ? " < " : "",
                          static_cast<unsigned long long>(a - reinterpret_cast<std::uintptr_t>(self)));
        else
            continue;
        out += one;
        ++shown;
    }
    return out;
}

void LogRootRefused(std::int32_t root) {
    Log::Writef("UI: the widget manager's root (widget %d) was to be destroyed (from %s): refused (every widget is "
                "processed from it)", root, CallerStack().c_str());
}

std::int32_t ManagerRoot(void* manager) noexcept {
    __try {
        return manager ? *reinterpret_cast<const std::int32_t*>(static_cast<std::uint8_t*>(manager) + kWmRoot) : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool __fastcall HookDestroyWidget(void* manager, std::int32_t* id) {
    const std::int32_t root = ManagerRoot(manager);
    if (root != -1 && id && *id == root) {
        if (g_rootRefusals.fetch_add(1, std::memory_order_relaxed) < 8) LogRootRefused(root);
        *id = -1;   // (as the game does once it is destroyed)
        return true;
    }
    return g_originalDestroyWidget(manager, id);
}

void ProcessLayersUnsafe(void* manager) {
    const std::uintptr_t base = GameBuild::Base();
    const float dt = *reinterpret_cast<const float*>(base + kRvaWidgetFrameScale) *
                     *reinterpret_cast<const float*>(base + kRvaWidgetTimeUnit);
    const auto get = reinterpret_cast<GetWidgetByIdFn>(base + kRvaGetWidgetById);
    const auto process = reinterpret_cast<ProcessRecursiveFn>(base + kRvaProcessRecursive);
    const auto* layers = reinterpret_cast<const std::int32_t*>(static_cast<std::uint8_t*>(manager) + kWmLayers);
    for (int i = 0; i < kWmLayerCount; ++i) {
        if (layers[i] <= 0) continue;
        if (void* layer = get(manager, layers[i])) process(layer, dt);
    }
}

void __fastcall HookManagerProcess(void* manager) {
    if (manager && Fn(kRvaWidgetManagerGet) &&
        manager == reinterpret_cast<void*(__fastcall*)()>(Fn(kRvaWidgetManagerGet))()) {
        bool rootGone = false;
        std::int32_t root = -1;
        __try {
            root = *reinterpret_cast<const std::int32_t*>(static_cast<std::uint8_t*>(manager) + kWmRoot);
            rootGone = root != -1 && *reinterpret_cast<const std::uint64_t*>(static_cast<std::uint8_t*>(manager) + 0x18) &&
                       !reinterpret_cast<GetWidgetByIdFn>(Fn(kRvaGetWidgetById))(manager, root);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            rootGone = false;
        }
        if (rootGone) {
            if (!g_rootLostSaid.exchange(true, std::memory_order_relaxed)) {
                const auto* layers = reinterpret_cast<const std::int32_t*>(static_cast<std::uint8_t*>(manager) + kWmLayers);
                Log::Writef("UI: the widget manager's root (widget %d) is gone: no widget would be processed (the saving "
                            "icon stays, the HUD takes no input); its %d layers (widgets %d .. %d) processed directly",
                            root, kWmLayerCount, layers[0], layers[kWmLayerCount - 1]);
            }
            __try {
                ProcessLayersUnsafe(manager);
                g_layerProcesses.fetch_add(1, std::memory_order_relaxed);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
    g_originalManagerProcess(manager);   // (the root found or not; then the widgets destroyed are finalized)
}
}  // namespace

bool Install() {
    if (!GameBuild::IsSupportedRetail()) return false;
    const bool widgets = GameBuild::VerifyBytes(kRvaWidgetManagerGet, kWidgetManagerGetSig, sizeof(kWidgetManagerGetSig)) &&
                         GameBuild::VerifyBytes(kRvaWidgetIsVisible, kWidgetIsVisibleSig, sizeof(kWidgetIsVisibleSig));
    g_gatesReady = widgets && GameBuild::VerifyBytes(kRvaWidgetProcessInput, kWidgetProcessInputSig, sizeof(kWidgetProcessInputSig));
    const bool weapon = GameBuild::VerifyBytes(kRvaWeaponSelectPressed, kWeaponSelectPressedSig, sizeof(kWeaponSelectPressedSig)) &&
                        GameBuild::VerifyBytes(kRvaWeaponSelectBlocked, kWeaponSelectBlockedSig, sizeof(kWeaponSelectBlockedSig)) &&
                        GameBuild::VerifyBytes(kRvaWeaponSelectAccess, kWeaponSelectAccessSig, sizeof(kWeaponSelectAccessSig));
    if (!widgets || !weapon) {
        Log::Write("HudWatch: the widget manager or the weapon select does not match this build; no HUD logging");
        return false;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(Fn(kRvaWeaponSelectPressed)), reinterpret_cast<void*>(&HookPressed),
                      reinterpret_cast<void**>(&g_originalPressed)) != MH_OK) {
        Log::Write("HudWatch: the weapon select's input hook could not be created; no HUD logging");
        return false;
    }
    g_ready = true;
    // (v0.9.587) the system messages: their reads, the input gate and the clear
    g_notifyReady = GameBuild::VerifyBytes(kRvaNotifyGet, kNotifyGetSig, sizeof(kNotifyGetSig)) &&
                    GameBuild::VerifyBytes(kRvaWidgetIsInputAllowed, kWidgetIsInputAllowedSig, sizeof(kWidgetIsInputAllowedSig)) &&
                    GameBuild::VerifyBytes(kRvaNotifyForceClear, kNotifyForceClearSig, sizeof(kNotifyForceClearSig)) &&
                    GameBuild::VerifyBytes(kRvaTimerGetTimeMs, kTimerGetTimeMsSig, sizeof(kTimerGetTimeMsSig)) &&
                    GameBuild::VerifyBytes(kRvaNotifyProcess, kNotifyProcessSig, sizeof(kNotifyProcessSig));
    if (!g_notifyReady) Log::Write("HudWatch: the system message manager does not match this build (not logged or put right)");
    // (v0.9.588) the menu state manager's change
    g_menuReady = GameBuild::VerifyBytes(kRvaMenuProcess, kMenuProcessSig, sizeof(kMenuProcessSig)) &&
                  GameBuild::VerifyBytes(kRvaMenuChangeState, kMenuChangeStateSig, sizeof(kMenuChangeStateSig));
    if (!g_menuReady) Log::Write("HudWatch: the menu state manager does not match this build (its changes not watched)");
    Log::Writef("HudWatch: ready (the weapon keys' path and the visible widgets are logged when the HUD stops answering%s)",
                g_notifyReady ? "; a system message no one can see is cleared after 4 s in play" : "");
    if (g_menuReady) Log::Write("HudWatch: menu changes watched (one not entered in 3 s is logged; a broken delay or a "
                                "Process that never ended put right)");
    // (v0.9.589) the weapon select at the bottom left
    g_layoutReady = GameBuild::VerifyBytes(kRvaWeaponSelectProcess, kWeaponSelectProcessSig, sizeof(kWeaponSelectProcessSig)) &&
                    GameBuild::VerifyBytes(kRvaWidgetGetVariable, kWidgetGetVariableSig, sizeof(kWidgetGetVariableSig)) &&
                    GameBuild::VerifyBytes(kRvaGfxGetMember, kGfxGetMemberSig, sizeof(kGfxGetMemberSig)) &&
                    GameBuild::VerifyBytes(kRvaGfxSetMember, kGfxSetMemberSig, sizeof(kGfxSetMemberSig)) &&
                    GameBuild::VerifyBytes(kRvaGfxInvoke, kGfxInvokeSig, sizeof(kGfxInvokeSig)) &&
                    GameBuild::VerifyBytes(kRvaGfxObjectRelease, kGfxObjectReleaseSig, sizeof(kGfxObjectReleaseSig));
    if (g_layoutReady && MH_CreateHook(reinterpret_cast<void*>(Fn(kRvaWeaponSelectProcess)),
                                       reinterpret_cast<void*>(&HookWeaponSelectProcess),
                                       reinterpret_cast<void**>(&g_originalWeaponSelectProcess)) != MH_OK) {
        g_originalWeaponSelectProcess = nullptr;
        g_layoutReady = false;
    }
    if (!g_layoutReady) Log::Write("HudWatch: the weapon select's movie does not match this build (it stays where the game puts it)");
    g_weaponSelectBottomLeft.store(g_layoutReady && Settings::Get().weaponSelectBottomLeft, std::memory_order_relaxed);
    g_weaponSelectSize.store(std::clamp(Settings::Get().weaponSelectSize, 30, 100), std::memory_order_relaxed);   // (v0.9.590)
    // (v0.9.595) the widget manager's root kept, and its layers processed should it be gone
    g_rootGuardReady = GameBuild::VerifyBytes(kRvaDestroyWidget, kDestroyWidgetSig, sizeof(kDestroyWidgetSig)) &&
                       GameBuild::VerifyBytes(kRvaManagerProcess, kManagerProcessSig, sizeof(kManagerProcessSig)) &&
                       GameBuild::VerifyBytes(kRvaProcessRecursive, kProcessRecursiveSig, sizeof(kProcessRecursiveSig)) &&
                       GameBuild::VerifyBytes(kRvaGetWidgetById, kGetWidgetByIdSig, sizeof(kGetWidgetByIdSig));
    if (g_rootGuardReady) {
        const bool destroy = MH_CreateHook(reinterpret_cast<void*>(Fn(kRvaDestroyWidget)), reinterpret_cast<void*>(&HookDestroyWidget),
                                           reinterpret_cast<void**>(&g_originalDestroyWidget)) == MH_OK;
        const bool process = MH_CreateHook(reinterpret_cast<void*>(Fn(kRvaManagerProcess)),
                                           reinterpret_cast<void*>(&HookManagerProcess),
                                           reinterpret_cast<void**>(&g_originalManagerProcess)) == MH_OK;
        if (destroy && !process) MH_RemoveHook(reinterpret_cast<void*>(Fn(kRvaDestroyWidget)));
        if (!destroy && process) MH_RemoveHook(reinterpret_cast<void*>(Fn(kRvaManagerProcess)));
        g_rootGuardReady = destroy && process;
    }
    Log::Write(g_rootGuardReady ? "HudWatch: the widget manager's root kept (its destruction refused and logged; its layers "
                                  "processed directly should it be gone)"
                                : "HudWatch: the widget manager does not match this build (its root not watched)");
    return true;
}

// (v0.9.589) Enhancements > Gameplay > Weapon select at the bottom left
bool WeaponSelectLayoutAvailable() { return g_layoutReady; }
bool WeaponSelectBottomLeft() { return g_weaponSelectBottomLeft.load(std::memory_order_relaxed); }
void SetWeaponSelectBottomLeft(bool on) {
    if (!g_layoutReady) on = false;
    g_weaponSelectBottomLeft.store(on, std::memory_order_relaxed);
    Settings::Get().weaponSelectBottomLeft = on;
}
int WeaponSelectSize() { return g_weaponSelectSize.load(std::memory_order_relaxed); }   // (v0.9.590)
void SetWeaponSelectSize(int percent) {
    percent = std::clamp(percent, 30, 100);
    g_weaponSelectSize.store(percent, std::memory_order_relaxed);
    Settings::Get().weaponSelectSize = percent;
}

void LogCensus(const char* why) {
    if (!g_ready || g_censusLogs.fetch_add(1) >= 8) return;
    Census(why);
}

void TickGameThread() {
    if (!g_ready) return;
    RecoverInput(GetTickCount64());   // (v0.9.587)
    RecoverMenu(GetTickCount64());    // (v0.9.588)
    static bool s_down[9] = {};
    static ULONGLONG s_pendingAt = 0;
    static int s_key = 0, s_logs = 0;
    const ULONGLONG now = GetTickCount64();
    const bool front = GameInFront();
    int pressed = -1;
    for (int k = 0; k < 9; ++k) {
        const bool down = front && (GetAsyncKeyState('1' + k) & 0x8000) != 0;
        if (down && !s_down[k]) pressed = k;
        s_down[k] = down;
    }
    if (pressed >= 0 && !s_pendingAt && s_logs < 6 && !Overlay::OwnsInput() && !Features::LoadingProfilerSessionActive()) {
        std::int32_t current = -2, next = -2;
        if (MenuStates(current, next, nullptr) && current == -1 && next == -1) {
            s_pendingAt = now;
            s_key = pressed + 1;
        }
    }
    if (!s_pendingAt || now - s_pendingAt < 300) return;
    const ULONGLONG pressedAt = s_pendingAt;
    s_pendingAt = 0;
    if (g_lastPressedAt.load(std::memory_order_relaxed) >= pressedAt) return;   // (it reached the weapon select)
    ++s_logs;
    void* w = FindWidget("UiWidgetWeaponSelect");
    Gates g;
    if (!w || !g_gatesReady || !ReadGates(w, g)) {
        Log::Writef("HUD: key %d reached no weapon select input in 0.3 s; the weapon select widget %s", s_key,
                    w ? "found (its gates not read)" : "not found");
    } else {
        Log::Writef("HUD: key %d reached no weapon select input in 0.3 s. Its input gates (UiWidgetBase::ProcessInput): world "
                    "pause byte %u (not 1), devices %u (not 0), visible %d or +0x2D %u, +0x28 %.3f (below 0), actions %u "
                    "(not 0), vfunc +0x60 ROTTR.exe+0x%llX answers %s; input from %llu, held from %llu (now %llu); flags "
                    "0x%02X, player %p", s_key, g.pause, g.devices, Visible(w) ? 1 : 0, g.always, g.timer, g.actions,
                    static_cast<unsigned long long>(g.vfunc60),
                    g.inputAllowed < 0 ? "(not asked)" : g.inputAllowed ? "yes" : "NO (IsInputAllowed)",
                    static_cast<unsigned long long>(g.noInputBefore), static_cast<unsigned long long>(g.noHeldBefore),
                    static_cast<unsigned long long>(g.now), g.flags, g.player);
        if (void* icon = FindWidget("UiWidgetSavingIcon"))
            Log::Writef("HUD: the saving icon: %s, its timer %.2f s", Visible(icon) ? "visible" : "hidden",
                        SavingIconTimer(icon));
    }
    LogCensus("a number key reached no weapon select input");
}
}  // namespace HudWatch
