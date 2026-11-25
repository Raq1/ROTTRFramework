#include "zone_list.h"
#include "game_build.h"
#include "log.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>

namespace {
constexpr std::uintptr_t kRvaObjectZoneManagerPtr = 0x01A50B20ull;
constexpr std::uint32_t kOffsetMaxId = 0x140u;
constexpr std::uint32_t kOffsetNames = 0x148u;
constexpr std::uint32_t kSaneMaxId = 1u << 20;

// ObjectZoneManager::GetZoneName: test ecx,ecx / ... / mov rcx,[rip+singleton] / ... / mov rax,[rcx+0x148] / mov rax,[rax+rdx*8]
constexpr std::uintptr_t kRvaGetZoneName = 0x002378B0ull;
constexpr std::uint8_t kGetZoneNameSig[] = {
    0x85,0xC9,0x75,0x03,0x33,0xC0,0xC3,0x8D,0x41,0xFF,0x48,0x8B,0x0D,0x5F,0x92,0x81,0x01,
    0x48,0x63,0xD0,0x48,0x8B,0x81,0x48,0x01,0x00,0x00,0x48,0x8B,0x04,0xD0,0xC3
};
// zonelist.ids loader: mov [r13+0x140], esi (highest ID) and mov [r13+0x148], rax (name array)
constexpr std::uintptr_t kRvaLoaderStoreMaxId = 0x0024671Eull;
constexpr std::uint8_t kLoaderStoreMaxIdSig[] = {0x41,0x89,0xB5,0x40,0x01,0x00,0x00};
constexpr std::uintptr_t kRvaLoaderStoreNames = 0x00246783ull;
constexpr std::uint8_t kLoaderStoreNamesSig[] = {0x49,0x89,0x85,0x48,0x01,0x00,0x00};

enum : int { kUnchecked = 0, kLayoutOk = 1, kLayoutBad = 2 };
std::atomic_int g_layout{kUnchecked};
std::atomic<const char* const*> g_names{nullptr};
std::atomic_uint32_t g_maxId{0};

bool LayoutOk() {
    int state = g_layout.load(std::memory_order_acquire);
    if (state == kUnchecked) {
        const bool ok = GameBuild::Base() &&
            GameBuild::VerifyBytes(kRvaGetZoneName, kGetZoneNameSig, sizeof(kGetZoneNameSig)) &&
            GameBuild::VerifyBytes(kRvaLoaderStoreMaxId, kLoaderStoreMaxIdSig, sizeof(kLoaderStoreMaxIdSig)) &&
            GameBuild::VerifyBytes(kRvaLoaderStoreNames, kLoaderStoreNamesSig, sizeof(kLoaderStoreNamesSig));
        state = ok ? kLayoutOk : kLayoutBad;
        g_layout.store(state, std::memory_order_release);
        if (!ok) Log::Write("ZoneList: ObjectZoneManager signature mismatch; zone names unavailable (numeric zone IDs still work)");
    }
    return state == kLayoutOk;
}

// Reads the manager's table; SEH-guarded because the game may still be filling it in.
bool ReadTable(const char* const** names, std::uint32_t* maxId) {
    __try {
        const auto* slot = reinterpret_cast<std::uint8_t* const*>(GameBuild::Base() + kRvaObjectZoneManagerPtr);
        const std::uint8_t* manager = *slot;
        if (!manager) return false;
        const std::int32_t count = *reinterpret_cast<const std::int32_t*>(manager + kOffsetMaxId);
        const auto* array = *reinterpret_cast<const char* const* const*>(manager + kOffsetNames);
        if (count <= 0 || static_cast<std::uint32_t>(count) > kSaneMaxId || !array) return false;
        // The highest ID always has a name once the file is fully parsed (it came from the file),
        // so this also refuses the table while the game is still filling it in.
        const char* last = array[count - 1];
        if (!last || !last[0]) return false;
        *names = array;
        *maxId = static_cast<std::uint32_t>(count);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const char* SafeName(const char* const* names, std::uint32_t index) {
    __try {
        const char* name = names[index];
        if (!name || !name[0]) return nullptr;
        return name;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
}

namespace ZoneList {
bool Ready() {
    if (g_names.load(std::memory_order_acquire)) return true;
    if (!LayoutOk()) return false;

    const char* const* names = nullptr;
    std::uint32_t maxId = 0;
    if (!ReadTable(&names, &maxId)) return false;
    g_maxId.store(maxId, std::memory_order_relaxed);
    if (!g_names.exchange(names, std::memory_order_acq_rel))
        Log::Detailf("ZoneList: using the game's ObjectZone table (zone IDs 1..%u)", maxId);
    return true;
}

std::uint32_t MaxId() {
    return Ready() ? g_maxId.load(std::memory_order_relaxed) : 0u;
}

void Refresh() {
    if (!LayoutOk()) return;
    const char* const* names = nullptr;
    std::uint32_t maxId = 0;
    if (!ReadTable(&names, &maxId)) return;
    g_maxId.store(maxId, std::memory_order_relaxed);
    g_names.store(names, std::memory_order_release);
}

const char* Name(std::uint32_t zoneId) {
    if (!zoneId || !Ready() || zoneId > g_maxId.load(std::memory_order_relaxed)) return nullptr;
    return SafeName(g_names.load(std::memory_order_acquire), zoneId - 1u);
}
}
