// More threads for the game's background jobs (job_threads.h).
#include "job_threads.h"

#include "game_build.h"
#include "log.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace JobThreads {
namespace {
constexpr std::uintptr_t kRvaJobSystemInit = 0x001F0320, kRvaCapImmediate = 0x001F0357, kRvaSemaphoreCount = 0x001F0374;
constexpr std::uintptr_t kRvaHighCount = 0x00FC1404, kRvaNormalCount = 0x00FC1408, kRvaTotalCount = 0x0165B898;
constexpr unsigned kTableSize = 34;   // the threads' index table (0x165B7E0 .. 0x165B868)

struct Check {
    std::uintptr_t rva;
    std::uint8_t bytes[20];
    std::size_t size;
};
const Check kChecks[] = {
    {kRvaJobSystemInit, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x30}, 20},
    {0x001F0334, {0x41, 0xB8, 0x10, 0x00, 0x00, 0x00}, 6},          // mov r8d, 16 (the first two groups' cap)
    {kRvaCapImmediate, {0x41, 0xB8, 0x02, 0x00, 0x00, 0x00}, 6},    // mov r8d, 2 (the third group's cap)
    {0x001F0363, {0x41, 0x3B, 0xC8}, 3},                            // cmp ecx, r8d
    {0x001F036C, {0x41, 0x0F, 0x47, 0xC8}, 4},                      // cmova ecx, r8d
    {0x001F0370, {0x03, 0xC2, 0x03, 0xC1}, 4},                      // add eax, edx; add eax, ecx (the total)
    {kRvaSemaphoreCount, {0x41, 0x8D, 0x68, 0x01}, 4},              // lea ebp, [r8 + 1] (3 semaphores)
    {0x001F0378, {0x89, 0x05, 0x1A, 0xB5, 0x46, 0x01}, 6},          // mov [0x165B898], eax (the total)
    {0x001A1FD4, {0xB1, 0x01, 0xE8, 0xD5, 0xE2, 0x04, 0x00}, 7},    // the game's start: call 0x1F02B0
};

// What PatchEarly did (read by LogResult)
int g_outcome = 0;   // 0 not tried, 1 patched, 2 not wanted, 3 code differs, 4 too late, 5 no room, 6 write failed
unsigned g_processors = 0, g_cap = 0;

bool WriteCode(std::uintptr_t address, const void* bytes, std::size_t size) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(reinterpret_cast<void*>(address), bytes, size);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(address), size, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
    return true;
}

int IniInt(const char* section, const char* key, int fallback) {
    char ini[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    char* slash = std::strrchr(ini, '\\');
    if (slash) slash[1] = 0;
    else ini[0] = 0;
    strcat_s(ini, "ROTTRFramework.ini");
    return static_cast<int>(GetPrivateProfileIntA(section, key, fallback, ini));
}
}  // namespace

void PatchEarly() {
    if (!GameBuild::IsSupportedRetail()) return;
    const std::uintptr_t base = GameBuild::Base();
    // the processors as the game counts them (0x1F02B0: the process's affinity mask, its bits)
    DWORD_PTR processMask = 0, systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask))
        for (DWORD_PTR m = processMask; m; m >>= 1) g_processors += static_cast<unsigned>(m & 1);
    if (!g_processors) g_processors = 1;
    const int setting = IniInt("Loading", "BackgroundThreads", -1);
    int wanted = setting;
    if (setting < 0) wanted = IniInt("Loading", "FasterLoading", 1) ? static_cast<int>(std::max(2u, g_processors / 2)) : 0;
    if (wanted <= 2) {
        g_outcome = 2;
        return;
    }
    for (const Check& c : kChecks)
        if (!GameBuild::VerifyBytes(c.rva, c.bytes, c.size)) {
            g_outcome = 3;
            return;
        }
    if (*reinterpret_cast<const std::uint32_t*>(base + kRvaTotalCount) != 0) {   // (the job system already made)
        g_outcome = 4;
        return;
    }
    const unsigned others = 2 * std::min(g_processors, 16u);
    const unsigned room = kTableSize > others ? kTableSize - others : 0;
    g_cap = std::min(static_cast<unsigned>(wanted), room);
    if (g_cap <= std::min(g_processors, 2u)) {
        g_outcome = 5;
        return;
    }
    const std::uint8_t cap[4] = {static_cast<std::uint8_t>(g_cap), 0, 0, 0};
    const std::uint8_t semaphores[4] = {0x6A, 0x03, 0x5D, 0x90};   // push 3; pop rbp; nop
    if (!WriteCode(base + kRvaSemaphoreCount, semaphores, sizeof(semaphores)) ||
        !WriteCode(base + kRvaCapImmediate + 2, cap, sizeof(cap))) {
        g_outcome = 6;
        return;
    }
    g_outcome = 1;
}

void LogResult() {
    const std::uintptr_t base = GameBuild::Base();
    unsigned high = 0, normal = 0, total = 0;
    if (g_outcome != 0 && g_outcome != 3) {
        high = *reinterpret_cast<const std::uint32_t*>(base + kRvaHighCount);
        normal = *reinterpret_cast<const std::uint32_t*>(base + kRvaNormalCount);
        total = *reinterpret_cast<const std::uint32_t*>(base + kRvaTotalCount);
    }
    char groups[160] = "";
    if (total) std::snprintf(groups, sizeof(groups), "; the game's worker threads: %u above normal, %u normal, %u below normal (the background jobs)",
                             high, normal, total >= high + normal ? total - high - normal : 0);
    else std::snprintf(groups, sizeof(groups), "; the game's job system not made yet");
    switch (g_outcome) {
        case 1:
            Log::Writef("JobThreads: the background jobs' threads (the loads' Deflate decoding) capped at %u instead of 2 "
                        "(%u processors)%s", g_cap, g_processors, groups);
            break;
        case 2:
            Log::Writef("JobThreads: the game's 2 background job threads kept ([Loading] BackgroundThreads / FasterLoading)%s", groups);
            break;
        case 3:
            Log::Write("JobThreads: the job system's code differs: its background threads kept as the game makes them");
            break;
        case 4:
            Log::Writef("JobThreads: the job system was made before the framework loaded: its threads as the game made them%s", groups);
            break;
        case 5:
            Log::Writef("JobThreads: no room for more background threads (%u processors: the game's table of %u threads is full)%s",
                        g_processors, kTableSize, groups);
            break;
        case 6:
            Log::Write("JobThreads: the job system's code could not be written: its threads as the game makes them");
            break;
        default:
            break;
    }
}

}  // namespace JobThreads
