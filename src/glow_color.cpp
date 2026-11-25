#include "glow_color.h"
#include "game_build.h"
#include "log.h"
#include "zone_list.h"
#include <Windows.h>
#include <MinHook.h>
#include <imgui.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Retail addresses (Steam build 2022-09-23), each checked by its bytes at start.
namespace GlowColor {
namespace {

// AGMiscSetColorNode::ProcessMsg(AGMessageType, void*): on Init (when m_outputOnInit, +0x24) or on Activate with its Set
// input (+0x14 bit 0) it outputs m_In (+0x20, u32 colour, R in the low byte). The node copies m_In from its
// dtp::SetColorNode when the graph is created, so the value is swapped in the node itself.
constexpr std::uintptr_t kRvaSetColorProcessMsg = 0x00741260ull;
constexpr std::uint8_t kSetColorProcessMsgSig[] = {0x85, 0xD2, 0x75, 0x07, 0x38, 0x51, 0x24, 0x75, 0x0D, 0xEB, 0x05,
                                                   0x83, 0xFA, 0x02, 0x75, 0x17, 0xF6, 0x41, 0x14, 0x01, 0x74, 0x11,
                                                   0x41, 0xB9, 0x06, 0x00, 0x00, 0x00, 0x4C, 0x8D, 0x41, 0x20};
constexpr std::size_t kSetColorIn = 0x20;
constexpr std::uint32_t kGameBlue = 0x4BFB741Du;    // RGB 29, 116, 251
constexpr std::uint32_t kGameAmber = 0x4B206DFBu;   // RGB 251, 109, 32

// Lara's glowstick: her InventoryType 72 item
constexpr std::uintptr_t kRvaLocalPlayer = 0x00403BF0ull;         // GetLocalPlayerInstance(u8)
constexpr std::uintptr_t kRvaGetItemByType = 0x005E2570ull;       // InventoryComponent::GetItemByType(type, notReady)
constexpr std::uintptr_t kRvaInventoryVtableLea = 0x005A43DEull;  // InventoryComponent ctor: lea rax,[vtable]
constexpr std::uintptr_t kRvaInventoryVtable = 0x00D5A958ull;
constexpr std::uint8_t kLocalPlayerSig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0xB6, 0xD9};
constexpr std::uint8_t kGetItemByTypeSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89,
                                              0x74, 0x24, 0x18, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr std::uint8_t kLeaRaxRipSig[] = {0x48, 0x8D, 0x05};
constexpr std::size_t kInstanceComponentCount = 0xD0, kInstanceComponents = 0xD8;
constexpr int kMaxComponents = 256;
constexpr int kGlowstickType = 72;

// ObjectNetDataComponent (the glowstick's state variables): items count u32 +0x2D8, item array +0x2E0 (CheckValue
// 0x5BCA10); NetDataItem_Float32 getter virtual +0x40; SetValue 0x528F60 takes dtp::ObjectNetData_Set
// {u32 index +0, u8 dataType +4, data +8}.
constexpr std::uintptr_t kRvaNetDataSetValue = 0x00528F60ull, kRvaNetDataCheckValue = 0x005BCA41ull;
constexpr std::uint8_t kSetValueSig[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41,
                                         0x56, 0x48, 0x83, 0xEC, 0x70, 0x48, 0x8B, 0xF1, 0x48, 0x8B, 0xDA};
constexpr std::uint8_t kSetValueItemsSig[] = {0x48, 0x8B, 0x86, 0xE0, 0x02, 0x00, 0x00};
constexpr std::uint8_t kCheckCountSig[] = {0x8B, 0x4F, 0x04, 0x3B, 0x8E, 0xD8, 0x02, 0x00, 0x00};
constexpr std::size_t kNetDataCount = 0x2D8, kNetDataItems = 0x2E0, kGetFloat = 0x40;
constexpr std::uint32_t kIntensityState = 0;   // net data 0: the light's intensity (its colour graph runs on a change)
constexpr std::uint32_t kTargetState = 3;      // net data 3: the intensity it fades to (evGlowstickIntensity's value)

constexpr ULONGLONG kRefreshEveryMs = 120;
constexpr ULONGLONG kBrightCheckMs = 500, kBrightResendMs = 1500;
constexpr std::size_t kMaxTracked = 4096;
constexpr const char* kIniSection = "GlowstickColour";

// brightness: evGlowstickIntensity with an AGData_Any float, sent by name:
// EventSystem::SendEvent(char*, Instance*, int size, void* data, u64)
constexpr std::uintptr_t kRvaSendEvent = 0x007F94D0ull;
constexpr std::uintptr_t kRvaEventSystemLoad = 0x005D1703ull;   // mov rcx,[EventSystem::s_instance] (VisionMode::Enable)
constexpr std::uintptr_t kRvaSendEventCall = 0x005D1717ull;     // call SendEvent (same function)
constexpr std::uint8_t kSendEventSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48,
                                          0x83, 0xEC, 0x40, 0x49, 0x8B, 0xF8, 0x48, 0x8B, 0xF1, 0x4C, 0x8B, 0xC2};
constexpr std::uint8_t kMovRcxRipSig[] = {0x48, 0x8B, 0x0D};
constexpr std::uint8_t kCallSig[] = {0xE8};

// The glowstick in her hand, on her belt, let go (TR2 PDB build names; retail found by their code). v0.9.474: by the
// glowstick item's object id (her InventoryType 72 item, Instance +0x80 -> +8 -> +0x50), the key of the inventory's
// items: InventoryComponent::Activate(object id) (TR2 0x5E1430; retail 0x5AB480) = Item::MakeReady(inv, false, true)
// (state 3, her hand); Deactivate(object id, stow) (TR2 0x5FE330; retail 0x5C7FF0) = Item::PutAway(inv, 0, stow), with
// stow false the item's away marker (marker_GlowstickAway, her belt). (v0.9.473 used ToggleTorch, which takes
// GetItemByPrimaryType(20), the "torch" type: the glowstick is not one, so nothing was found: "she has no glowstick".)
// DropItem(object id) (TR2 0x6072D0): the item's Item cleared with ItemActionType 1 (its owner let go: it falls where it
// is), the item erased from the inventory. LoadItem(object id, count, slot, bool, bool) (TR2 0x63B1B0): the game gives a
// missing item that way (1, -1, false, false; the item comes once its object has loaded).
constexpr std::uintptr_t kRvaActivate = 0x005AB480ull;     // InventoryComponent::Activate(u32 object id)
constexpr std::uint8_t kActivateSig[] = {0x44, 0x8B, 0xC2, 0x4C, 0x8B, 0xD1, 0x85, 0xD2, 0x74, 0x4C, 0x48, 0x83,
                                         0xB9, 0xC8, 0x02, 0x00, 0x00, 0x00, 0x74, 0x42, 0x33, 0xD2, 0x41, 0x8B};
constexpr std::uintptr_t kRvaDeactivate = 0x005C7FF0ull;   // InventoryComponent::Deactivate(u32 object id, bool stow)
constexpr std::uint8_t kDeactivateSig[] = {0x85, 0xD2, 0x0F, 0x84, 0xDD, 0x00, 0x00, 0x00, 0x48, 0x89, 0x5C, 0x24,
                                           0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48};
// (v0.9.475) "On her belt" keeps it there: 0.9.474's Deactivate put it on her belt (state 3 -> 2) and the game took it
// back into her hand a second later (its dark places want it out). InventoryMemberComponent::MakeReady(member, inv,
// bool) (retail 0x605050, TR2 0x63B540; member +8 = the item instance) is hooked: while the stick is kept on her belt the
// game's MakeReady of it is refused (logged with its caller), and should it come back to state 3 some other way it is
// put on the belt again. "In her hand" and "Let go of it" end the keeping, and so does a new stick.
constexpr std::uintptr_t kRvaMemberMakeReady = 0x00605050ull;
constexpr std::uint8_t kMemberMakeReadySig[] = {0x40, 0x57, 0x41, 0x54, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x4C, 0x8B,
                                                0xFA, 0x45, 0x0F, 0xB6, 0xE0, 0x8B, 0x51, 0x28, 0x48, 0x8B, 0xF9};
constexpr std::size_t kMemberInstance = 0x08;
constexpr ULONGLONG kBeltRetryMs = 500;
constexpr std::uintptr_t kRvaToggleTorchMemberId = 0x00641AA3ull;   // mov ecx,[ComponentArrayID<InventoryMemberComponent>::s_id]
constexpr std::uintptr_t kRvaMemberComponentId = 0x01101D04ull;
constexpr std::uint8_t kMovEcxRipSig[] = {0x8B, 0x0D};
constexpr std::uintptr_t kRvaDropItem = 0x005C89F0ull;
constexpr std::uint8_t kDropItemSig[] = {0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x83,
                                         0xB9, 0xC8, 0x02, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF1, 0x8B, 0xFA};
constexpr std::uintptr_t kRvaLoadItem = 0x00604CC0ull;
constexpr std::uint8_t kLoadItemSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89,
                                         0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x41};
// an item's object id: Instance +0x80 -> +8 -> +0x50 (NsPlayerBase::Inv_DropItem reads it so for DropItem)
constexpr std::size_t kItemObjectRef = 0x80, kObjectRefData = 0x08, kObjectDataId = 0x50;
constexpr std::size_t kMemberState = 0x28;   // InventoryMemberComponent m_state: 1 stowed, 2 away (her belt), 3 ready (her hand)
constexpr ULONGLONG kGiveWaitMs = 10000, kLitCheckMs = 1500, kHoldSnapshotMs = 250;
struct AgDataAny {   // AGData_Any (0x20): +0 type (5 float), +8 string, +0x10 the value
    std::uint32_t type;
    std::uint32_t pad0;
    void* text;
    union {
        float f;
        std::uint64_t raw;
    } value;
    std::uint64_t pad1;
};
static_assert(sizeof(AgDataAny) == 0x20, "the game's Message_Send sends 0x20 bytes");

struct SetArgs {   // dtp::ObjectNetData_Set
    std::uint32_t index;
    std::uint8_t dataType;
    std::uint8_t pad[3];
    union {
        float f;
        std::uint64_t raw[4];
    } data;
};

using ProcessMsgFn = void(__fastcall*)(std::uint8_t* node, int message, void* data);
using LocalPlayerFn = void*(__fastcall*)(std::uint8_t);
using GetItemByTypeFn = void*(__fastcall*)(void* inventory, int type, bool notReady);
using SendEventFn = void(__fastcall*)(void* system, const char* name, void* instance, int size, void* data, std::uint64_t extra);
using SetValueFn = void(__fastcall*)(void* component, const void* set);
using GetterFn = void(__fastcall*)(void* item, void* out);
using ActivateFn = void(__fastcall*)(void* inventory, std::uint32_t objectId);
using DeactivateFn = void(__fastcall*)(void* inventory, std::uint32_t objectId, bool stow);
using DropItemFn = void(__fastcall*)(void* inventory, std::uint32_t objectId);
using LoadItemFn = bool(__fastcall*)(void* inventory, std::uint32_t objectId, std::uint32_t count, int slot, bool a, bool b);

std::uintptr_t g_base = 0;
ProcessMsgFn g_original = nullptr;
LocalPlayerFn g_localPlayer = nullptr;
GetItemByTypeFn g_itemByType = nullptr;
const void* g_inventoryVtable = nullptr;
SendEventFn g_sendEvent = nullptr;
void* const* g_eventSystem = nullptr;
SetValueFn g_setValue = nullptr;
bool g_available = false;
bool g_canRefresh = false;
bool g_canNetData = false;
bool g_canBrighten = false;
std::string g_status = "not started";

// hand / belt / let go (v0.9.473)
ActivateFn g_activate = nullptr;
using MemberMakeReadyFn = void(__fastcall*)(std::uint8_t* member, void* inventory, bool flag);
MemberMakeReadyFn g_origMemberMakeReady = nullptr;
std::atomic<void*> g_beltItem{nullptr};   // the stick kept on her belt (null: none kept)
std::atomic<std::uint32_t> g_beltRefused{0};
ULONGLONG g_lastBeltRetryMs = 0;
DeactivateFn g_deactivate = nullptr;
DropItemFn g_dropItem = nullptr;
LoadItemFn g_loadItem = nullptr;
const std::uint32_t* g_memberComponentId = nullptr;
bool g_canHold = false;
std::atomic<int> g_holdRequest{0};
std::uint32_t g_lastStickId = 0;     // the object of the stick she held last (given again after a let go)
std::uint32_t g_glowstickId = 0;     // g_glowstick's object id (0: not looked up yet; ~0: not found)
ULONGLONG g_giveUntilMs = 0;         // a stick given: taken into her hand when it comes
ULONGLONG g_litCheckAtMs = 0;        // after taking it: lit by then?
void* g_litCheckItem = nullptr;
ULONGLONG g_lastHoldSnapshotMs = 0;
std::atomic<int> g_holdState{-1};    // her stick's m_state (-1 none, -2 not readable)
std::atomic_bool g_holdLit{false};
std::mutex g_holdStatusMutex;
std::string g_holdStatus;

std::atomic_bool g_brightChanged{false};
std::atomic_bool g_brightReset{false};
ULONGLONG g_lastBrightCheckMs = 0, g_lastBrightSentMs = 0;
void* g_brightSentItem = nullptr;
void* g_brightPrevItem = nullptr;
double g_brightPrevTarget = -1.0;

std::mutex g_mutex;   // the colour and the nodes changed (the hook may run off the game thread)
Colour g_colour;
struct Changed {
    std::uint32_t original;
    std::uint32_t written;
};
std::unordered_map<std::uint8_t*, Changed> g_nodes;
std::atomic<std::uint32_t> g_substituted{0};
std::atomic_bool g_refresh{false};
struct Restore {
    bool pending = false;
    void* item = nullptr;
    float value = 0.0f;
};
Restore g_restore;
ULONGLONG g_lastRefreshMs = 0;

const char* IniPath() {
    static char path[MAX_PATH]{};
    if (path[0]) return path;
    char dir[MAX_PATH]{};
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    char* slash = dir;
    for (char* p = dir; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p + 1;
    *slash = '\0';
    std::snprintf(path, sizeof(path), "%sROTTRFramework.ini", dir);
    return path;
}

void SaveIni(const Colour& c) {
    char buf[32]{};
    WritePrivateProfileStringA(kIniSection, "Custom", c.custom ? "1" : "0", IniPath());
    const std::pair<const char*, int> rgb[] = {{"R", c.r}, {"G", c.g}, {"B", c.b}};
    for (const auto& [key, value] : rgb) {
        std::snprintf(buf, sizeof(buf), "%d", value);
        WritePrivateProfileStringA(kIniSection, key, buf, IniPath());
    }
    WritePrivateProfileStringA(kIniSection, "OwnBrightness", c.ownBrightness ? "1" : "0", IniPath());
    std::snprintf(buf, sizeof(buf), "%.3f", c.brightness);
    WritePrivateProfileStringA(kIniSection, "Brightness", buf, IniPath());
}

Colour LoadIni() {
    Colour c;
    c.custom = GetPrivateProfileIntA(kIniSection, "Custom", 0, IniPath()) != 0;
    c.r = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(GetPrivateProfileIntA(kIniSection, "R", 29, IniPath())), 0, 255));
    c.g = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(GetPrivateProfileIntA(kIniSection, "G", 116, IniPath())), 0, 255));
    c.b = static_cast<std::uint8_t>(std::clamp<int>(static_cast<int>(GetPrivateProfileIntA(kIniSection, "B", 251, IniPath())), 0, 255));
    char buf[32]{};
    c.ownBrightness = GetPrivateProfileIntA(kIniSection, "OwnBrightness", 0, IniPath()) != 0;
    GetPrivateProfileStringA(kIniSection, "Brightness", "1", buf, sizeof(buf), IniPath());
    c.brightness = std::clamp(static_cast<float>(std::atof(buf)), kBrightnessMin, kBrightnessMax);
    return c;
}

// ---------- raw reads (SEH only in these) ----------
bool ReadIn(std::uint8_t* node, std::uint32_t& value) {
    __try {
        value = *reinterpret_cast<const std::uint32_t*>(node + kSetColorIn);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteIn(std::uint8_t* node, std::uint32_t value) {
    __try {
        *reinterpret_cast<std::uint32_t*>(node + kSetColorIn) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The decorated RTTI name of a polymorphic object (x64: the complete object locator points back at itself).
bool RttiName(const void* object, char* out, std::size_t size) {
    out[0] = '\0';
    __try {
        const auto vtable = *reinterpret_cast<const std::uintptr_t*>(object);
        if (vtable < g_base) return false;
        const auto locator = *reinterpret_cast<const std::uintptr_t*>(vtable - 8);
        if (!locator || *reinterpret_cast<const std::uint32_t*>(locator) != 1) return false;
        if (g_base + static_cast<std::uintptr_t>(*reinterpret_cast<const std::int32_t*>(locator + 0x14)) != locator) return false;
        const char* name = reinterpret_cast<const char*>(g_base + static_cast<std::uintptr_t>(*reinterpret_cast<const std::int32_t*>(locator + 0x0C)) + 0x10);
        std::size_t n = 0;
        for (; n + 1 < size && name[n]; ++n) out[n] = name[n];
        out[n] = '\0';
        return n > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
        return false;
    }
}

bool ComponentsOf(std::uint8_t* instance, int& count, void* const*& list) {
    __try {
        count = *reinterpret_cast<const int*>(instance + kInstanceComponentCount);
        list = *reinterpret_cast<void* const* const*>(instance + kInstanceComponents);
        return list && count > 0 && count <= kMaxComponents;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* PointerAt(void* const* list, int index) {
    __try {
        return list[index];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool NetItems(std::uint8_t* component, std::uint32_t& count, void* const*& items) {
    __try {
        count = *reinterpret_cast<const std::uint32_t*>(component + kNetDataCount);
        items = *reinterpret_cast<void* const* const*>(component + kNetDataItems);
        return items && count <= 256;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GetFloat(void* item, float& out) {
    __try {
        auto** vtable = *reinterpret_cast<void***>(item);
        reinterpret_cast<GetterFn>(vtable[kGetFloat / sizeof(void*)])(item, &out);
        return std::isfinite(out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallSetValue(void* component, const SetArgs* args) {
    __try {
        g_setValue(component, args);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Lara's InventoryComponent
void* LaraInventory() {
    __try {
        auto* lara = static_cast<std::uint8_t*>(g_localPlayer(0));
        if (!lara) return nullptr;
        int count = 0;
        void* const* components = nullptr;
        if (!ComponentsOf(lara, count, components)) return nullptr;
        for (int i = 0; i < count; ++i) {
            void* component = components[i];
            if (component && *static_cast<const void* const*>(component) == g_inventoryVtable) return component;
        }
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Lara's glowstick (or flare) instance
void* GlowstickItem() {
    void* inventory = LaraInventory();
    if (!inventory) return nullptr;
    __try {
        return g_itemByType(inventory, kGlowstickType, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ---------- hand / belt / let go (v0.9.473) ----------
void __fastcall HookMemberMakeReady(std::uint8_t* member, void* inventory, bool flag) {
    void* kept = g_beltItem.load(std::memory_order_relaxed);
    if (kept && member && *reinterpret_cast<void* const*>(member + kMemberInstance) == kept) {
        const std::uint32_t n = g_beltRefused.fetch_add(1) + 1;
        if (n <= 8 || n % 100 == 0)
            Log::Writef("GlowColor: the game's MakeReady of the glowstick kept on her belt refused (%u; caller ROTTR.exe+0x%llX)", n,
                        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_base));
        return;
    }
    g_origMemberMakeReady(member, inventory, flag);
}

// her glowstick item (InventoryType 72)
void* StickItem(void* inventory) {
    __try {
        return inventory ? g_itemByType(inventory, kGlowstickType, false) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool CallActivate(void* inventory, std::uint32_t objectId) {
    __try {
        g_activate(inventory, objectId);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallDeactivate(void* inventory, std::uint32_t objectId) {
    __try {
        g_deactivate(inventory, objectId, false);   // not stowed: on its away marker (her belt)
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallDropItem(void* inventory, std::uint32_t objectId) {
    __try {
        g_dropItem(inventory, objectId);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallLoadItem(void* inventory, std::uint32_t objectId) {
    __try {
        return g_loadItem(inventory, objectId, 1, -1, false, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uint32_t ObjectIdOf(void* item) {
    __try {
        auto* ref = *reinterpret_cast<std::uint8_t* const*>(static_cast<std::uint8_t*>(item) + kItemObjectRef);
        if (!ref) return 0;
        auto* data = *reinterpret_cast<std::uint8_t* const*>(ref + kObjectRefData);
        return data ? *reinterpret_cast<const std::uint32_t*>(data + kObjectDataId) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// the item's InventoryMemberComponent m_state (-2 when it cannot be read)
int MemberState(void* item) {
    __try {
        const std::uint32_t id = *g_memberComponentId;
        int count = 0;
        void* const* list = nullptr;
        if (!ComponentsOf(static_cast<std::uint8_t*>(item), count, list) || id >= static_cast<std::uint32_t>(count)) return -2;
        auto* member = static_cast<const std::uint8_t*>(list[id]);
        return member ? *reinterpret_cast<const int*>(member + kMemberState) : -2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

const char* StateName(int state) {
    switch (state) {
        case -1: return "she has none";
        case 1: return "stowed (not shown)";
        case 2: return "on her belt";
        case 3: return "in her hand";
        case 4: return "thrown";
        default: return "unknown state";
    }
}

void SetHoldStatus(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_holdStatusMutex);
    g_holdStatus = text;
}

// g_glowstick's object (the zone list's names are the objects' names)
std::uint32_t GlowstickObjectId() {
    if (g_glowstickId == 0 && ZoneList::Ready()) {
        g_glowstickId = ~0u;
        const std::uint32_t max = ZoneList::MaxId();
        for (std::uint32_t id = 1; id <= max; ++id) {
            const char* name = ZoneList::Name(id);
            if (name && _stricmp(name, "g_glowstick") == 0) {
                g_glowstickId = id;
                break;
            }
        }
        Log::Writef("GlowColor: g_glowstick is object %s", g_glowstickId == ~0u ? "(not found)" : std::to_string(g_glowstickId).c_str());
    }
    return g_glowstickId == ~0u ? 0 : g_glowstickId;
}

bool SendIntensity(void* instance, float value) {
    AgDataAny data{};
    data.type = 5;   // float
    data.value.f = value;
    __try {
        void* system = *g_eventSystem;
        if (!system || !instance) return false;
        g_sendEvent(system, "evGlowstickIntensity", instance, static_cast<int>(sizeof(data)), &data, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------- the item's net data ----------
void* NetDataOf(void* item) {
    int count = 0;
    void* const* list = nullptr;
    if (!item || !ComponentsOf(static_cast<std::uint8_t*>(item), count, list)) return nullptr;
    char name[96];
    for (int i = 0; i < count; ++i) {
        void* component = PointerAt(list, i);
        if (component && RttiName(component, name, sizeof(name)) && std::strcmp(name, ".?AVObjectNetDataComponent@@") == 0)
            return component;
    }
    return nullptr;
}

// a float state variable (false when it is missing or not a float)
bool ReadFloatState(void* item, std::uint32_t index, float& out) {
    void* component = NetDataOf(item);
    std::uint32_t count = 0;
    void* const* items = nullptr;
    if (!component || !NetItems(static_cast<std::uint8_t*>(component), count, items) || index >= count) return false;
    void* entry = PointerAt(items, static_cast<int>(index));
    char name[96];
    if (!entry || !RttiName(entry, name, sizeof(name)) || std::strcmp(name, ".?AUNetDataItem_Float32@@") != 0) return false;
    return GetFloat(entry, out);
}

bool WriteFloatState(void* item, std::uint32_t index, float value) {
    float current = 0.0f;
    if (!ReadFloatState(item, index, current)) return false;
    SetArgs args{};
    args.index = index;
    args.dataType = 4;   // float
    args.data.f = value;
    return CallSetValue(NetDataOf(item), &args);
}

// ---------- the hook ----------
// Before the node outputs: one of the glowstick's constants becomes the chosen colour, or goes back to the game's.
void Substitute(std::uint8_t* node) {
    std::uint32_t value = 0;
    if (!ReadIn(node, value)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::uint32_t original = value;
    auto it = g_nodes.find(node);
    if (it != g_nodes.end()) {
        if (it->second.written == value) original = it->second.original;
        else g_nodes.erase(it);   // a new node in that memory
    }
    if (original != kGameBlue && original != kGameAmber) return;
    const std::uint32_t wanted = g_colour.custom ? (original & 0xFF000000u) | (static_cast<std::uint32_t>(g_colour.b) << 16) |
                                                       (static_cast<std::uint32_t>(g_colour.g) << 8) | g_colour.r
                                                 : original;
    if (wanted != value && !WriteIn(node, wanted)) return;
    if (wanted == original) {
        g_nodes.erase(node);
        return;
    }
    if (g_nodes.size() >= kMaxTracked) g_nodes.clear();
    g_nodes[node] = Changed{original, wanted};
    g_substituted.fetch_add(1);
}

void __fastcall HookProcessMsg(std::uint8_t* node, int message, void* data) {
    if (node && (message == 0 || message == 2)) Substitute(node);   // kAGMessageType_Init / kAGMessageType_Activate
    g_original(node, message, data);
}

// a lit glowstick takes a new colour: a small change of its intensity runs its colour graph (NetData_OnChanged), the
// value comes back next tick
void TickColour(ULONGLONG now) {
    if (g_restore.pending) {
        g_restore.pending = false;
        if (GlowstickItem() == g_restore.item) WriteFloatState(g_restore.item, kIntensityState, g_restore.value);
        return;
    }
    if (!g_canNetData || !g_refresh.load() || now - g_lastRefreshMs < kRefreshEveryMs) return;
    g_refresh.store(false);
    g_lastRefreshMs = now;
    void* item = GlowstickItem();
    float intensity = 0.0f;
    if (!item || !ReadFloatState(item, kIntensityState, intensity) || intensity <= 0.001f) return;   // none / not lit
    if (WriteFloatState(item, kIntensityState, intensity * 0.98f)) g_restore = Restore{true, item, intensity};
}

// own brightness: sent to each new glowstick / flare and when the choice changes, and again when a level changed the
// target (net data 3, read twice the same, so a fade of the game's own is not cut); the game's: Normal (1) once
void TickBrightness(ULONGLONG now) {
    if (!g_canBrighten || now - g_lastBrightCheckMs < kBrightCheckMs) return;
    g_lastBrightCheckMs = now;
    void* item = GlowstickItem();
    if (!item) {
        g_brightPrevItem = nullptr;
        return;
    }
    const Colour c = Current();
    if (!c.ownBrightness) {
        if (g_brightReset.exchange(false)) {
            SendIntensity(item, 1.0f);
            Log::Write("GlowColor: the game's glowstick brightness (Normal, 1)");
        }
        g_brightChanged.store(false);
        g_brightSentItem = nullptr;
        return;
    }
    g_brightReset.store(false);
    float targetF = -1.0f;
    const double target = g_canNetData && ReadFloatState(item, kTargetState, targetF) ? targetF : -1.0;
    const bool stable = item == g_brightPrevItem && target >= 0.0 && std::fabs(target - g_brightPrevTarget) < 0.005;
    const bool levelChanged = stable && std::fabs(target - c.brightness) > 0.02 && now - g_lastBrightSentMs > kBrightResendMs;
    if (g_brightChanged.exchange(false) || item != g_brightSentItem || levelChanged) {
        if (SendIntensity(item, c.brightness)) {
            g_lastBrightSentMs = now;
            g_brightSentItem = item;
            if (levelChanged) Log::Writef("GlowColor: the level set the glowstick to %.2f; back to %.2f", target, c.brightness);
        }
    }
    g_brightPrevItem = item;
    g_brightPrevTarget = target;
}

// hand / belt / let go: done here, on the game thread
void TakeIntoHand(void* inventory, void* stick, ULONGLONG now) {
    g_beltItem.store(nullptr);   // (v0.9.475) no longer kept on her belt
    const std::uint32_t id = ObjectIdOf(stick);
    const int before = MemberState(stick);
    if (!id || !CallActivate(inventory, id)) {
        SetHoldStatus(id ? "The game's InventoryComponent::Activate failed" : "Its object could not be read");
        return;
    }
    g_lastStickId = id;
    g_litCheckAtMs = now + kLitCheckMs;
    g_litCheckItem = stick;
    const int state = MemberState(stick);
    SetHoldStatus("Taken into her hand");
    Log::Writef("GlowColor: the glowstick taken into her hand (Activate, object %u; state %d -> %d)", g_lastStickId, before, state);
}

void TickHold(ULONGLONG now) {
    if (!g_canHold) return;
    const int request = g_holdRequest.exchange(0);
    void* inventory = (request || g_giveUntilMs || g_litCheckAtMs || g_beltItem.load() ||
                       now - g_lastHoldSnapshotMs >= kHoldSnapshotMs)
                          ? LaraInventory()
                          : nullptr;
    if (!inventory) {
        if (request) SetHoldStatus("No Lara (in a level only)");
        if (now - g_lastHoldSnapshotMs >= kHoldSnapshotMs) {
            g_lastHoldSnapshotMs = now;
            g_holdState.store(-1);
            g_holdLit.store(false);
        }
        return;
    }
    void* stick = StickItem(inventory);
    if (request == static_cast<int>(HoldRequest::Hand)) {
        g_giveUntilMs = 0;
        if (stick) {
            TakeIntoHand(inventory, stick, now);
        } else {
            const std::uint32_t id = g_lastStickId ? g_lastStickId : GlowstickObjectId();
            if (!id) {
                SetHoldStatus("She has no glowstick, and g_glowstick is not known");
            } else if (CallLoadItem(inventory, id)) {
                g_giveUntilMs = now + kGiveWaitMs;
                SetHoldStatus("She had none: one is given...");
                Log::Writef("GlowColor: no glowstick: object %u given (LoadItem); taken into her hand when it comes", id);
            } else {
                SetHoldStatus("She has no glowstick and the game did not give one");
                Log::Writef("GlowColor: no glowstick, and LoadItem of object %u answered false", id);
            }
        }
    } else if (request == static_cast<int>(HoldRequest::Belt)) {
        g_giveUntilMs = 0;
        if (!stick) {
            SetHoldStatus("She has no glowstick");
        } else {
            const std::uint32_t id = ObjectIdOf(stick);
            const int before = MemberState(stick);
            if (id && CallDeactivate(inventory, id)) {
                g_lastStickId = id;
                g_litCheckAtMs = 0;
                if (g_origMemberMakeReady) g_beltItem.store(stick);   // (v0.9.475) kept there
                g_lastBeltRetryMs = now;
                SetHoldStatus(g_origMemberMakeReady ? "On her belt (kept there until In her hand)" : "Put on her belt");
                Log::Writef("GlowColor: the glowstick put on her belt (Deactivate, object %u; state %d -> %d)", id, before,
                            MemberState(stick));
            } else {
                SetHoldStatus(id ? "The game's InventoryComponent::Deactivate failed" : "Its object could not be read");
            }
        }
    } else if (request == static_cast<int>(HoldRequest::LetGo)) {
        g_giveUntilMs = 0;
        const std::uint32_t id = stick ? ObjectIdOf(stick) : 0;
        if (!stick) {
            SetHoldStatus("She has no glowstick");
        } else if (!id) {
            SetHoldStatus("Its object could not be read");
        } else if (g_beltItem.store(nullptr), CallDropItem(inventory, id)) {
            g_lastStickId = id;
            g_litCheckAtMs = 0;
            stick = nullptr;
            SetHoldStatus("Let go (In her hand gives her another)");
            Log::Writef("GlowColor: the glowstick let go (DropItem, object %u)", id);
        } else {
            SetHoldStatus("The game's DropItem failed");
        }
    }
    if (g_giveUntilMs) {
        if (stick) {
            g_giveUntilMs = 0;
            TakeIntoHand(inventory, stick, now);
        } else if (now > g_giveUntilMs) {
            g_giveUntilMs = 0;
            SetHoldStatus("The glowstick given did not come (10 s)");
            Log::Write("GlowColor: the glowstick given did not come within 10 s");
        }
    }
    // taken out but not lit (the game lights it in its dark places): its brightness sent to light it
    if (g_litCheckAtMs && now >= g_litCheckAtMs) {
        g_litCheckAtMs = 0;
        float intensity = 0.0f;
        const bool read = g_canNetData && stick == g_litCheckItem && ReadFloatState(stick, kIntensityState, intensity);
        const int state = stick == g_litCheckItem ? MemberState(stick) : -1;
        if (read && intensity <= 0.001f && state == 3 && g_canBrighten) {
            const Colour c = Current();
            const float value = c.ownBrightness ? c.brightness : 1.0f;
            SendIntensity(stick, value);
            Log::Writef("GlowColor: the glowstick in her hand is not lit: evGlowstickIntensity %.2f sent", value);
        } else {
            Log::Writef("GlowColor: %.1f s after taking it: state %d, intensity %s", kLitCheckMs / 1000.0, state,
                        read ? std::to_string(intensity).c_str() : "(not read)");
        }
    }
    // (v0.9.475) the stick kept on her belt: a new stick ends the keeping; back in her hand some other way: belt again
    if (void* kept = g_beltItem.load()) {
        if (kept != stick) {
            g_beltItem.store(nullptr);
            Log::Write("GlowColor: the glowstick kept on her belt is gone (a new one, or none): no longer kept");
        } else if (MemberState(stick) == 3 && now - g_lastBeltRetryMs >= kBeltRetryMs) {
            g_lastBeltRetryMs = now;
            const std::uint32_t id = ObjectIdOf(stick);
            if (id && CallDeactivate(inventory, id))
                Log::Writef("GlowColor: the glowstick kept on her belt was in her hand again (not by MakeReady): put back (state %d)",
                            MemberState(stick));
        }
    }
    if (now - g_lastHoldSnapshotMs >= kHoldSnapshotMs) {
        g_lastHoldSnapshotMs = now;
        float intensity = 0.0f;
        g_holdState.store(stick ? MemberState(stick) : -1);
        g_holdLit.store(stick && g_canNetData && ReadFloatState(stick, kIntensityState, intensity) && intensity > 0.001f);
    }
}

}  // namespace

bool Initialize() {
    if (g_available) return true;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_colour = LoadIni();
    }
    if (!GameBuild::IsSupportedRetail()) {
        g_status = "unsupported game build";
        Log::Write("GlowColor: unsupported game build: the glowstick colour is off");
        return false;
    }
    g_base = GameBuild::Base();
    if (!GameBuild::VerifyBytes(kRvaSetColorProcessMsg, kSetColorProcessMsgSig, sizeof(kSetColorProcessMsgSig))) {
        g_status = "unexpected code at AGMiscSetColorNode::ProcessMsg";
        Log::Write("GlowColor: unexpected code at AGMiscSetColorNode::ProcessMsg: the glowstick colour is off");
        return false;
    }
    void* target = reinterpret_cast<void*>(g_base + kRvaSetColorProcessMsg);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookProcessMsg), reinterpret_cast<void**>(&g_original)) != MH_OK) {
        g_status = "the hook could not be created";
        Log::Write("GlowColor: the AGMiscSetColorNode::ProcessMsg hook could not be created: the glowstick colour is off");
        return false;
    }
    // Lara's glowstick item
    const std::int32_t lea = *reinterpret_cast<const std::int32_t*>(g_base + kRvaInventoryVtableLea + 3);
    if (GameBuild::VerifyBytes(kRvaLocalPlayer, kLocalPlayerSig, sizeof(kLocalPlayerSig)) &&
        GameBuild::VerifyBytes(kRvaGetItemByType, kGetItemByTypeSig, sizeof(kGetItemByTypeSig)) &&
        GameBuild::VerifyBytes(kRvaInventoryVtableLea, kLeaRaxRipSig, sizeof(kLeaRaxRipSig)) &&
        kRvaInventoryVtableLea + 7 + static_cast<std::intptr_t>(lea) == kRvaInventoryVtable) {
        g_localPlayer = reinterpret_cast<LocalPlayerFn>(g_base + kRvaLocalPlayer);
        g_itemByType = reinterpret_cast<GetItemByTypeFn>(g_base + kRvaGetItemByType);
        g_inventoryVtable = reinterpret_cast<const void*>(g_base + kRvaInventoryVtable);
        g_canRefresh = true;
    } else {
        Log::Write("GlowColor: unexpected code around InventoryComponent::GetItemByType: a lit glowstick keeps its colour until the next one");
    }
    // its state variables (the lit stick's refresh, the level's brightness)
    if (g_canRefresh && GameBuild::VerifyBytes(kRvaNetDataSetValue, kSetValueSig, sizeof(kSetValueSig)) &&
        GameBuild::VerifyBytes(kRvaNetDataSetValue + 0x44, kSetValueItemsSig, sizeof(kSetValueItemsSig)) &&
        GameBuild::VerifyBytes(kRvaNetDataCheckValue, kCheckCountSig, sizeof(kCheckCountSig))) {
        g_setValue = reinterpret_cast<SetValueFn>(g_base + kRvaNetDataSetValue);
        g_canNetData = true;
    } else if (g_canRefresh) {
        Log::Write("GlowColor: unexpected code around ObjectNetDataComponent::SetValue: a lit glowstick keeps its colour until the next one");
    }
    // brightness: the game's evGlowstickIntensity sent by name
    if (g_canRefresh && GameBuild::VerifyBytes(kRvaSendEvent, kSendEventSig, sizeof(kSendEventSig)) &&
        GameBuild::VerifyBytes(kRvaEventSystemLoad, kMovRcxRipSig, sizeof(kMovRcxRipSig)) &&
        GameBuild::VerifyBytes(kRvaSendEventCall, kCallSig, sizeof(kCallSig)) &&
        kRvaSendEventCall + 5 + static_cast<std::intptr_t>(*reinterpret_cast<const std::int32_t*>(g_base + kRvaSendEventCall + 1)) == kRvaSendEvent) {
        g_sendEvent = reinterpret_cast<SendEventFn>(g_base + kRvaSendEvent);
        g_eventSystem = reinterpret_cast<void* const*>(
            g_base + kRvaEventSystemLoad + 7 + static_cast<std::intptr_t>(*reinterpret_cast<const std::int32_t*>(g_base + kRvaEventSystemLoad + 3)));
        g_canBrighten = true;
    } else {
        Log::Write("GlowColor: unexpected code around EventSystem::SendEvent: the glowstick brightness is off");
    }
    // hand / belt / let go (v0.9.473)
    if (g_canRefresh && GameBuild::VerifyBytes(kRvaActivate, kActivateSig, sizeof(kActivateSig)) &&
        GameBuild::VerifyBytes(kRvaDeactivate, kDeactivateSig, sizeof(kDeactivateSig)) &&
        GameBuild::VerifyBytes(kRvaToggleTorchMemberId, kMovEcxRipSig, sizeof(kMovEcxRipSig)) &&
        kRvaToggleTorchMemberId + 6 + static_cast<std::intptr_t>(*reinterpret_cast<const std::int32_t*>(g_base + kRvaToggleTorchMemberId + 2)) ==
            kRvaMemberComponentId &&
        GameBuild::VerifyBytes(kRvaDropItem, kDropItemSig, sizeof(kDropItemSig)) &&
        GameBuild::VerifyBytes(kRvaLoadItem, kLoadItemSig, sizeof(kLoadItemSig))) {
        g_activate = reinterpret_cast<ActivateFn>(g_base + kRvaActivate);
        if (!GameBuild::VerifyBytes(kRvaMemberMakeReady, kMemberMakeReadySig, sizeof(kMemberMakeReadySig)) ||
            MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaMemberMakeReady), reinterpret_cast<void*>(&HookMemberMakeReady),
                          reinterpret_cast<void**>(&g_origMemberMakeReady)) != MH_OK) {
            g_origMemberMakeReady = nullptr;
            Log::Write("GlowColor: InventoryMemberComponent::MakeReady hook not created: the game can take the glowstick off her belt again");
        }
        g_deactivate = reinterpret_cast<DeactivateFn>(g_base + kRvaDeactivate);
        g_dropItem = reinterpret_cast<DropItemFn>(g_base + kRvaDropItem);
        g_loadItem = reinterpret_cast<LoadItemFn>(g_base + kRvaLoadItem);
        g_memberComponentId = reinterpret_cast<const std::uint32_t*>(g_base + kRvaMemberComponentId);
        g_canHold = true;
    } else {
        Log::Write("GlowColor: unexpected code around InventoryComponent::Activate / Deactivate / DropItem / LoadItem: hand, belt and let go are off");
    }
    g_available = true;
    g_status = g_canNetData ? "ready" : "ready (new glowsticks only)";
    const Colour c = Current();
    Log::Writef("GlowColor: %s; colour %s (%d, %d, %d); brightness %s %.2f%s", g_status.c_str(), c.custom ? "of your own" : "of the game",
                c.r, c.g, c.b, c.ownBrightness ? "of your own" : "of the game", c.brightness, g_canBrighten ? "" : " (unavailable)");
    return true;
}

bool Available() {
    return g_available;
}

const char* Status() {
    return g_status.c_str();
}

Colour Current() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_colour;
}

void Set(const Colour& colour, bool save) {
    Colour c = colour;
    c.brightness = std::clamp(c.brightness, kBrightnessMin, kBrightnessMax);
    bool colourSame = false, brightSame = false, wasOwn = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        colourSame = g_colour.custom == c.custom && g_colour.r == c.r && g_colour.g == c.g && g_colour.b == c.b;
        brightSame = g_colour.ownBrightness == c.ownBrightness && std::fabs(g_colour.brightness - c.brightness) < 0.001f;
        wasOwn = g_colour.ownBrightness;
        g_colour = c;
        if (colourSame && brightSame && !save) return;
    }
    if (save) SaveIni(c);
    if (!colourSame) g_refresh.store(true);
    if (!brightSame) {
        g_brightChanged.store(true);
        if (wasOwn && !c.ownBrightness) g_brightReset.store(true);
    }
}

void TickGameThread() {
    if (!g_available || !g_canRefresh) return;
    const ULONGLONG now = GetTickCount64();
    TickColour(now);
    TickBrightness(now);
    TickHold(now);
}

bool HoldBackTransition(const char* name) {
    if (!name || !g_beltItem.load(std::memory_order_relaxed)) return false;
    if (std::strcmp(name, "default_to_glow_stick_equip") != 0 && std::strcmp(name, "default_to_glow_stick_spawned") != 0)
        return false;
    static std::atomic<std::uint32_t> s_held{0};
    const std::uint32_t n = s_held.fetch_add(1) + 1;
    if (n <= 4 || n % 200 == 0)
        Log::Writef("GlowColor: the glowstick is kept on her belt: '%s' held back (%u)", name, n);
    return true;
}

bool HoldAvailable() {
    return g_canHold;
}

void RequestHold(HoldRequest request) {
    g_holdRequest.store(static_cast<int>(request));
}

namespace {
void DrawHoldSection() {
    ImGui::TextUnformatted("Glowstick");
    ImGui::Separator();
    if (!g_canHold) {
        ImGui::TextDisabled("Unavailable: %s", g_available ? "unexpected game code" : g_status.c_str());
        return;
    }
    if (ImGui::Button("In her hand")) RequestHold(HoldRequest::Hand);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Lara takes her glowstick into her hand, anywhere (the game's own InventoryComponent::Activate).\n"
                          "If she has none (let go, or a chapter without one), she is given one first.");
    ImGui::SameLine();
    if (ImGui::Button("On her belt")) RequestHold(HoldRequest::Belt);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Lara hangs her glowstick on her belt (its away marker), and it stays there:\n"
                          "the game cannot take it out again until \"In her hand\" or \"Let go of it\".");
    ImGui::SameLine();
    if (ImGui::Button("Let go of it")) RequestHold(HoldRequest::LetGo);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Lara lets go of her glowstick: it leaves her inventory and stays where it falls.\n"
                          "\"In her hand\" gives her another.");
    const int state = g_holdState.load();
    ImGui::TextDisabled("Her glowstick: %s%s", StateName(state), state >= 0 && g_holdLit.load() ? ", lit" : "");
    std::string status;
    {
        std::lock_guard<std::mutex> lock(g_holdStatusMutex);
        status = g_holdStatus;
    }
    if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());
    ImGui::TextDisabled("The game can take it back or put it away again by itself (its dark places, climbing, cutscenes).");
}

void DrawColourSections() {
    ImGui::Spacing();
    ImGui::TextUnformatted("Glowstick colour");
    ImGui::Separator();
    if (!g_available) {
        ImGui::TextDisabled("Unavailable: %s", g_status.c_str());
        return;
    }
    Colour c = Current();
    bool custom = c.custom;
    if (ImGui::Checkbox("A colour of my own", &custom)) {
        c.custom = custom;
        Set(c, true);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Off: the game's own glowstick colours (blue, and amber later in the story).\n"
                          "On: every glowstick of Lara's glows in the colour below, its lights and the stick itself.\n"
                          "Kept in ROTTRFramework.ini [GlowstickColour].");
    float rgb[3] = {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f};
    if (!c.custom) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::ColorEdit3("Colour##glowstick", rgb)) {
        c.r = static_cast<std::uint8_t>(std::clamp(rgb[0], 0.0f, 1.0f) * 255.0f + 0.5f);
        c.g = static_cast<std::uint8_t>(std::clamp(rgb[1], 0.0f, 1.0f) * 255.0f + 0.5f);
        c.b = static_cast<std::uint8_t>(std::clamp(rgb[2], 0.0f, 1.0f) * 255.0f + 0.5f);
        Set(c, false);   // shown while dragging; kept when the edit ends
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) Set(Current(), true);
    if (!c.custom) ImGui::EndDisabled();
    struct Preset {
        const char* name;
        std::uint8_t r, g, b;
    };
    static const Preset kPresets[] = {{"Green", 40, 255, 60}, {"Red", 255, 30, 20},   {"Purple", 170, 60, 255},
                                      {"White", 255, 250, 235}, {"Pink", 255, 80, 190}, {"Amber", 251, 109, 32}};
    for (std::size_t i = 0; i < std::size(kPresets); ++i) {
        if (i > 0) ImGui::SameLine();
        if (ImGui::SmallButton(kPresets[i].name)) {
            Colour p = Current();
            p.custom = true;
            p.r = kPresets[i].r;
            p.g = kPresets[i].g;
            p.b = kPresets[i].b;
            Set(p, true);
        }
    }
    if (ImGui::Button("Back to the game's colour")) {
        c = Current();
        c.custom = false;
        Set(c, true);
    }
    ImGui::TextDisabled("%u glowstick colour outputs changed%s", g_substituted.load(),
                        g_canNetData ? "" : "; a lit glowstick changes with the next one");

    ImGui::Spacing();
    ImGui::TextUnformatted("Glowstick brightness");
    if (!g_canBrighten) {
        ImGui::TextDisabled("Unavailable (unexpected game code)");
        return;
    }
    c = Current();
    bool own = c.ownBrightness;
    if (ImGui::Checkbox("A brightness of my own", &own)) {
        c.ownBrightness = own;
        Set(c, true);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Off: the game's own brightness (its tombs dim or brighten the glowstick).\n"
                          "On: the glowstick (and the flare) keep the brightness below, even where a level changes it.\n"
                          "The game's scale: 0.5 Dim, 1 Normal, 3 Bright.");
    if (!c.ownBrightness) ImGui::BeginDisabled();
    float brightness = c.brightness;
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::SliderFloat("Brightness##glowstick", &brightness, kBrightnessMin, kBrightnessMax, "%.2f", ImGuiSliderFlags_Logarithmic)) {
        c.brightness = brightness;
        Set(c, false);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) Set(Current(), true);
    if (!c.ownBrightness) ImGui::EndDisabled();
    static const std::pair<const char*, float> kLevels[] = {{"Dim (0.5)", 0.5f}, {"Normal (1)", 1.0f}, {"Bright (3)", 3.0f}};
    for (std::size_t i = 0; i < std::size(kLevels); ++i) {
        if (i > 0) ImGui::SameLine();
        if (ImGui::SmallButton(kLevels[i].first)) {
            Colour p = Current();
            p.ownBrightness = true;
            p.brightness = kLevels[i].second;
            Set(p, true);
        }
    }
    if (ImGui::Button("Back to the game's brightness")) {
        c = Current();
        c.ownBrightness = false;
        Set(c, true);
    }
}
}  // namespace

void DrawTab() {
    DrawHoldSection();
    ImGui::Spacing();
    DrawColourSections();
}

}  // namespace GlowColor
