// The game's "does this save exist" without asking Steam once per Steam Cloud file (steam_save_cache.h).
#include "steam_save_cache.h"

#include <MinHook.h>
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "game_build.h"
#include "log.h"

namespace SteamSaveCache {
namespace {
// SteamSave::SaveExists(self, slot): slot's high 32 bits are the save enum; the name comes from
// SteamSave::BuildFileName(self, buffer, saveEnum, false) (0x8A7EE0) and is compared with strcmp.
constexpr std::uintptr_t kRvaSaveExists = 0x008A9160ull;
constexpr std::uint8_t kSaveExistsSig[] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10, 0x57, 0x48,
                                           0x83, 0xEC, 0x60, 0x48, 0x8B, 0xD9, 0x48, 0x8D, 0x0D, 0x67, 0x8F, 0x70,
                                           0x00, 0xFF, 0x15, 0xC9};
constexpr std::uintptr_t kRvaBuildFileName = 0x008A7EE0ull;
constexpr std::uintptr_t kRvaSteamContextInitIat = 0x00D02D48ull;   // SteamInternal_ContextInit
constexpr std::uintptr_t kRvaSteamContextInitData = 0x00FB20E0ull;
constexpr std::size_t kContextRemoteStorageOffset = 0x48;
constexpr std::size_t kGetFileCountSlot = 0x90 / sizeof(void*);
constexpr std::size_t kGetFileNameAndSizeSlot = 0x98 / sizeof(void*);
constexpr ULONGLONG kKeepMs = 2000;

using SaveExistsFn = bool(__fastcall*)(void*, std::uint64_t);
using BuildFileNameFn = bool(__fastcall*)(void*, char*, std::uint32_t, bool);
using ContextInitFn = void*(__cdecl*)(void*);
using GetFileCountFn = int(__fastcall*)(void*);
using GetFileNameAndSizeFn = const char*(__fastcall*)(void*, int, int*);

SaveExistsFn g_origSaveExists = nullptr;
std::mutex g_mutex;
std::vector<std::string> g_names;
int g_count = -1;
ULONGLONG g_readAt = 0;
unsigned g_reads = 0;

void* RemoteStorage() {
    const std::uintptr_t base = GameBuild::Base();
    auto init = *reinterpret_cast<ContextInitFn*>(base + kRvaSteamContextInitIat);
    void* context = init ? init(reinterpret_cast<void*>(base + kRvaSteamContextInitData)) : nullptr;
    return context ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(context) + kContextRemoteStorageOffset)
                   : nullptr;
}

bool __fastcall HookSaveExists(void* self, std::uint64_t slot) {
    void* storage = RemoteStorage();
    if (!storage) return false;   // (as the game: no remote storage, no save)
    void** vtable = *static_cast<void***>(storage);
    auto getCount = reinterpret_cast<GetFileCountFn>(vtable[kGetFileCountSlot]);
    auto getName = reinterpret_cast<GetFileNameAndSizeFn>(vtable[kGetFileNameAndSizeSlot]);
    const int count = getCount(storage);
    if (count <= 0) return false;
    char wanted[512] = {};
    auto build = reinterpret_cast<BuildFileNameFn>(GameBuild::Base() + kRvaBuildFileName);
    build(self, wanted, static_cast<std::uint32_t>(slot >> 32), false);

    std::scoped_lock lock(g_mutex);
    const ULONGLONG now = GetTickCount64();
    if (count != g_count || now - g_readAt > kKeepMs) {
        g_names.clear();
        g_names.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            int size = 0;
            if (const char* name = getName(storage, i, &size)) g_names.emplace_back(name);
        }
        g_count = count;
        g_readAt = now;
        if (++g_reads == 1) Log::Detailf("SteamSave: %d Steam Cloud file(s) listed once for the save checks", count);
    }
    for (const std::string& name : g_names)
        if (std::strcmp(name.c_str(), wanted) == 0) return true;
    return false;
}

// ---------- the main menu's save data read again (v0.9.473) ----------
constexpr std::uintptr_t kRvaLoadSaveProgressData = 0x004903A0ull;   // SaveLoad::LoadSaveProgressData(SaveLoad*)
constexpr std::uint8_t kLoadSaveProgressDataSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89,
                                                     0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x40, 0x32, 0xF6, 0x48};
constexpr std::uintptr_t kRvaProgressCall = 0x004903C3ull;      // its call of SAVE_GetProgressDataForSlot
constexpr std::uintptr_t kRvaSaveManagerLoad = 0x004903DDull;   // its mov rcx,[SaveManager::s_ptr]
constexpr std::uintptr_t kRvaExistsCall = 0x004903E4ull;        // its call of SaveManager::SaveGameExists
constexpr std::uintptr_t kRvaGetProgressDataForSlot = 0x00350EF0ull;   // SAVE_GetProgressDataForSlot(slot)
constexpr std::uint8_t kGetProgressDataSig[] = {0x81, 0xF9, 0x93, 0x01, 0x00, 0x00, 0x75, 0x08, 0x48, 0x8D, 0x05};
constexpr std::uintptr_t kRvaSaveGameExists = 0x008A94D0ull;    // SaveManager::SaveGameExists(SaveManager*, slot)
constexpr std::uint8_t kSaveGameExistsSig[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x3D};
constexpr std::uintptr_t kRvaSaveManagerPtr = 0x02E1E4C8ull;
constexpr std::uintptr_t kRvaInitMainMenuTail = 0x008A8371ull;  // SteamSave::InitMainMenu: mov rcx,[SaveLoad::s_ptr]; ...; jmp
constexpr std::uintptr_t kRvaInitMainMenuJmp = 0x008A837Dull;
constexpr std::uintptr_t kRvaSaveLoadPtr = 0x02D58888ull;
constexpr std::uint8_t kMovRcxRip[] = {0x48, 0x8B, 0x0D};
constexpr std::uint32_t kContinueSlot = 0x193;   // the main menu's CONTINUE (SetContinueData)
constexpr std::uint32_t kLastSlot = 0xC8;        // slots 1..200 (OnSaveProgressDataUpdated's newest-save search)
constexpr std::size_t kProgressDate = 0x00, kProgressCorrupt = 0x20;   // SaveProgressData: SYSTEMTIME's first 8 bytes; bad
constexpr ULONGLONG kMenuSettleMs = 1500;
constexpr unsigned kMaxRereads = 3;

using LoadSaveProgressDataFn = void(__fastcall*)(void*);
using GetProgressDataFn = std::uint8_t*(__fastcall*)(std::uint32_t);
using SaveGameExistsFn = bool(__fastcall*)(void*, int);

bool g_rereadReady = false;
LoadSaveProgressDataFn g_loadSaveProgressData = nullptr;
GetProgressDataFn g_getProgressData = nullptr;
SaveGameExistsFn g_saveGameExists = nullptr;
void* const* g_saveManager = nullptr;
void* const* g_saveLoad = nullptr;
ULONGLONG g_menuEnteredAt = 0;
unsigned g_rereads = 0;

std::uintptr_t Target(std::uintptr_t rva, std::size_t length, std::size_t dispAt) {
    return rva + length + static_cast<std::intptr_t>(*reinterpret_cast<const std::int32_t*>(GameBuild::Base() + rva + dispAt));
}

void PrepareReread() {
    const bool ok = GameBuild::VerifyBytes(kRvaLoadSaveProgressData, kLoadSaveProgressDataSig, sizeof(kLoadSaveProgressDataSig)) &&
                    GameBuild::VerifyBytes(kRvaGetProgressDataForSlot, kGetProgressDataSig, sizeof(kGetProgressDataSig)) &&
                    GameBuild::VerifyBytes(kRvaSaveGameExists, kSaveGameExistsSig, sizeof(kSaveGameExistsSig)) &&
                    Target(kRvaProgressCall, 5, 1) == kRvaGetProgressDataForSlot && Target(kRvaExistsCall, 5, 1) == kRvaSaveGameExists &&
                    GameBuild::VerifyBytes(kRvaSaveManagerLoad, kMovRcxRip, sizeof(kMovRcxRip)) &&
                    Target(kRvaSaveManagerLoad, 7, 3) == kRvaSaveManagerPtr &&
                    GameBuild::VerifyBytes(kRvaInitMainMenuTail, kMovRcxRip, sizeof(kMovRcxRip)) &&
                    Target(kRvaInitMainMenuTail, 7, 3) == kRvaSaveLoadPtr && Target(kRvaInitMainMenuJmp, 5, 1) == kRvaLoadSaveProgressData;
    if (!ok) {
        Log::Write("SteamSave: unexpected code around SaveLoad::LoadSaveProgressData: the main menu's missed save data is not read again");
        return;
    }
    const std::uintptr_t base = GameBuild::Base();
    g_loadSaveProgressData = reinterpret_cast<LoadSaveProgressDataFn>(base + kRvaLoadSaveProgressData);
    g_getProgressData = reinterpret_cast<GetProgressDataFn>(base + kRvaGetProgressDataForSlot);
    g_saveGameExists = reinterpret_cast<SaveGameExistsFn>(base + kRvaSaveGameExists);
    g_saveManager = reinterpret_cast<void* const*>(base + kRvaSaveManagerPtr);
    g_saveLoad = reinterpret_cast<void* const*>(base + kRvaSaveLoadPtr);
    g_rereadReady = true;
}

// a save that exists with its progress data never read (-1: could not be checked)
int SlotMissed(void* manager, std::uint32_t slot) {
    __try {
        if (!g_saveGameExists(manager, static_cast<int>(slot))) return 0;
        const std::uint8_t* data = g_getProgressData(slot);
        if (!data) return -1;
        return *reinterpret_cast<const std::uint64_t*>(data + kProgressDate) == 0 && data[kProgressCorrupt] == 0 ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool CallLoadSaveProgressData(void* saveLoad) {
    __try {
        g_loadSaveProgressData(saveLoad);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

void OnMainMenuEntered() {
    if (g_rereadReady) g_menuEnteredAt = GetTickCount64();
}

void TickGameThread() {
    if (!g_menuEnteredAt || GetTickCount64() - g_menuEnteredAt < kMenuSettleMs) return;
    g_menuEnteredAt = 0;
    void* manager = *g_saveManager;
    void* saveLoad = *g_saveLoad;
    if (!manager || !saveLoad) return;
    int missed = 0;
    std::uint32_t first = 0;
    for (std::uint32_t slot = 1; slot <= kLastSlot + 1; ++slot) {
        const std::uint32_t s = slot <= kLastSlot ? slot : kContinueSlot;
        const int r = SlotMissed(manager, s);
        if (r < 0) return;
        if (r > 0 && !missed++) first = s;
    }
    if (!missed) return;
    if (g_rereads >= kMaxRereads) {
        Log::Writef("SteamSave: %d save(s) still without their progress data (slot %u first); not read again", missed, first);
        return;
    }
    ++g_rereads;
    const bool called = CallLoadSaveProgressData(saveLoad);
    Log::Writef("SteamSave: the main menu shows %d save(s) whose progress data was never read (slot %u first; CONTINUE "
                "\"EMPTY\"): SaveLoad::LoadSaveProgressData run again%s",
                missed, first, called ? "" : " (failed)");
}

bool CreateHook() {
    PrepareReread();
    if (!GameBuild::VerifyBytes(kRvaSaveExists, kSaveExistsSig, sizeof(kSaveExistsSig))) {
        Log::Write("SteamSave: SaveExists signature mismatch; the game's save checks stay as they are");
        return false;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(GameBuild::Base() + kRvaSaveExists),
                      reinterpret_cast<void*>(&HookSaveExists), reinterpret_cast<void**>(&g_origSaveExists)) != MH_OK) {
        Log::Write("SteamSave: SaveExists hook could not be created");
        return false;
    }
    return true;
}
}  // namespace SteamSaveCache
