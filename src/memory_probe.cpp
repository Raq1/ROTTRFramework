// Memory checks on the game's data that do not stall the game thread (memory_probe.h).
#include "memory_probe.h"

#include <windows.h>

#include <cstdint>

namespace MemoryProbe {
namespace {
constexpr std::uintptr_t kPage = 0x1000;
constexpr std::size_t kProbeLimit = 1u << 20;   // (larger ranges: VirtualQuery)

// The filter of the guarded reads: any fault means "not readable". A guard page the read tripped (its guard is gone
// once the exception is raised) gets it back: it is the end of some thread's stack, which grows through it.
int ProbeFilter(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info ? info->ExceptionRecord : nullptr;
    if (record && record->ExceptionCode == STATUS_GUARD_PAGE_VIOLATION && record->NumberParameters >= 2) {
        void* page = reinterpret_cast<void*>(record->ExceptionInformation[1] & ~(kPage - 1));
        MEMORY_BASIC_INFORMATION mbi{};
        DWORD old = 0;
        if (VirtualQuery(page, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD))
            VirtualProtect(page, kPage, mbi.Protect | PAGE_GUARD, &old);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

bool ReadableProtection(DWORD protect) {
    if (protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const DWORD p = protect & 0xFFu;
    return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READ ||
           p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

bool WritableProtection(DWORD protect) {
    if (protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const DWORD p = protect & 0xFFu;
    return p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

bool QueryReadable(std::uintptr_t current, std::uintptr_t end) {
    while (current < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(current), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || !ReadableProtection(mbi.Protect)) return false;
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= current) return false;
        current = regionEnd < end ? regionEnd : end;
    }
    return true;
}

// (no objects with destructors in the guarded functions)
bool ProbeRange(std::uintptr_t begin, std::uintptr_t end) {
    __try {
        std::uintptr_t page = begin;
        for (;;) {
            (void)*reinterpret_cast<const volatile std::uint8_t*>(page);
            const std::uintptr_t next = (page & ~(kPage - 1)) + kPage;
            if (next >= end) break;
            page = next;
        }
        (void)*reinterpret_cast<const volatile std::uint8_t*>(end - 1);
        return true;
    } __except (ProbeFilter(GetExceptionInformation())) {
        return false;
    }
}

std::size_t ProbeCopy(const char* source, char* destination, std::size_t capacity) {
    std::size_t written = 0;
    __try {
        while (written + 1 < capacity) {
            const char c = *reinterpret_cast<const volatile char*>(source + written);
            if (!c) break;
            destination[written++] = c;
        }
    } __except (ProbeFilter(GetExceptionInformation())) {
    }
    return written;
}

// The heap regions found writable lately (private memory only), kept kKeepMs.
constexpr ULONGLONG kKeepMs = 1000;
struct Region {
    std::uintptr_t begin = 0, end = 0;
    ULONGLONG until = 0;
};
constexpr std::size_t kRegions = 32;
Region g_regions[kRegions];
std::size_t g_nextRegion = 0;
SRWLOCK g_regionLock = SRWLOCK_INIT;

bool KnownWritable(std::uintptr_t begin, std::uintptr_t end, ULONGLONG now) {
    AcquireSRWLockShared(&g_regionLock);
    bool found = false;
    for (const Region& r : g_regions)
        if (now < r.until && begin >= r.begin && end <= r.end) {
            found = true;
            break;
        }
    ReleaseSRWLockShared(&g_regionLock);
    return found;
}

void Remember(std::uintptr_t begin, std::uintptr_t end, ULONGLONG now) {
    AcquireSRWLockExclusive(&g_regionLock);
    Region* slot = nullptr;
    for (Region& r : g_regions)
        if (r.begin == begin && r.end == end) slot = &r;
    if (!slot) slot = &g_regions[g_nextRegion++ % kRegions];
    *slot = Region{begin, end, now + kKeepMs};
    ReleaseSRWLockExclusive(&g_regionLock);
}
}  // namespace

bool Readable(const void* address, std::size_t bytes) {
    if (!address || bytes == 0) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto end = begin + bytes;
    if (end < begin || begin < 0x10000u) return false;
    if (bytes > kProbeLimit) return QueryReadable(begin, end);
    return ProbeRange(begin, end);
}

bool CopyCString(const char* source, char* destination, std::size_t capacity) {
    if (!destination || capacity == 0) return false;
    destination[0] = '\0';
    if (!source || capacity == 1 || reinterpret_cast<std::uintptr_t>(source) < 0x10000u) return false;
    const std::size_t written = ProbeCopy(source, destination, capacity);
    destination[written] = '\0';
    return written != 0;
}

bool Writable(const void* address, std::size_t bytes) {
    if (!address || bytes == 0) return false;
    auto current = reinterpret_cast<std::uintptr_t>(address);
    const auto end = current + bytes;
    if (end < current) return false;
    const ULONGLONG now = GetTickCount64();
    if (KnownWritable(current, end, now)) return true;
    while (current < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(current), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || !WritableProtection(mbi.Protect)) return false;
        const auto regionBegin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto regionEnd = regionBegin + mbi.RegionSize;
        if (regionEnd <= current) return false;
        if (mbi.Type == MEM_PRIVATE) Remember(regionBegin, regionEnd, now);
        current = regionEnd < end ? regionEnd : end;
    }
    return true;
}

bool MakeWritable(void* address, std::size_t bytes) {
    if (Writable(address, bytes)) return true;
    DWORD old = 0;
    return VirtualProtect(address, bytes, PAGE_READWRITE, &old) != 0;
}

void Forget() {
    AcquireSRWLockExclusive(&g_regionLock);
    for (Region& r : g_regions) r = Region{};
    ReleaseSRWLockExclusive(&g_regionLock);
}
}  // namespace MemoryProbe
