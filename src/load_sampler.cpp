// Where a load's time goes, every thread of the game's (load_sampler.h).
#include "load_sampler.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "log.h"

namespace LoadSampler {
namespace {
constexpr DWORD kEveryMs = 10;            // a round of every thread
constexpr ULONGLONG kThreadListEveryMs = 1000;   // a system-wide thread snapshot (about 70 ms of CPU each)
constexpr ULONGLONG kCpuEveryMs = 250;          // the known threads' CPU (which ones are sampled)
constexpr ULONGLONG kModuleListEveryMs = 2000;
constexpr int kDepth = 48;   // (deep enough for the loading thread's game loop to its waits)
constexpr int kMaxModules = 512;
constexpr std::size_t kMaxThreads = 512;
constexpr std::size_t kMaxPlacesPerThread = 512;
constexpr std::size_t kMaxHotSpots = 4096;
constexpr int kThreadsReported = 10;
constexpr int kRunningPlacesPerThread = 4, kWaitingPlacesPerThread = 2;
constexpr int kHotSpotsReported = 20;
constexpr ULONGLONG kActiveCpu = 10000;   // a thread with at least 1 ms of CPU over the last 250 ms is sampled
constexpr int kTimelineThreads = 6;

// ---- modules and their unwind data (this module's own table: only the sampler thread and End read it) ----
struct Module {
    std::uintptr_t base = 0, end = 0;
    const RUNTIME_FUNCTION* functions = nullptr;
    DWORD functionCount = 0;
    bool app = false;   // the game or the framework
    char name[48] = {};
};
Module g_modules[kMaxModules];
int g_moduleCount = 0;

void RefreshModules() {
    static HMODULE handles[kMaxModules];
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &needed)) return;
    const int count = static_cast<int>(std::min<DWORD>(needed / sizeof(HMODULE), kMaxModules));
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&RefreshModules), &self);
    const HMODULE game = GetModuleHandleW(nullptr);
    int filled = 0;
    for (int i = 0; i < count; ++i) {
        MODULEINFO info{};
        if (!K32GetModuleInformation(GetCurrentProcess(), handles[i], &info, sizeof(info))) continue;
        Module m;
        m.base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
        m.end = m.base + info.SizeOfImage;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m.base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) continue;
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (dir.VirtualAddress && dir.Size >= sizeof(RUNTIME_FUNCTION)) {
            m.functions = reinterpret_cast<const RUNTIME_FUNCTION*>(m.base + dir.VirtualAddress);
            m.functionCount = dir.Size / sizeof(RUNTIME_FUNCTION);
        }
        m.app = handles[i] == game || handles[i] == self;
        if (!K32GetModuleBaseNameA(GetCurrentProcess(), handles[i], m.name, sizeof(m.name))) m.name[0] = '?';
        g_modules[filled++] = m;
    }
    std::sort(g_modules, g_modules + filled, [](const Module& a, const Module& b) { return a.base < b.base; });
    g_moduleCount = filled;
}

const Module* FindModule(std::uintptr_t address) {
    int lo = 0, hi = g_moduleCount - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const Module& m = g_modules[mid];
        if (address < m.base) hi = mid - 1;
        else if (address >= m.end) lo = mid + 1;
        else return &m;
    }
    return nullptr;
}

const RUNTIME_FUNCTION* FindFunction(const Module& m, DWORD rva) {
    if (!m.functions) return nullptr;
    std::size_t lo = 0, hi = m.functionCount;
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        const RUNTIME_FUNCTION& f = m.functions[mid];
        if (rva < f.BeginAddress) hi = mid;
        else if (rva >= f.EndAddress) lo = mid + 1;
        else return &f;
    }
    return nullptr;
}

// The stack of a suspended thread (static data only: no allocation, no lock).
int Walk(CONTEXT& context, std::uint64_t* out) {
    int n = 0;
    __try {
        while (n < kDepth) {
            const DWORD64 pc = context.Rip;
            if (pc < 0x10000) break;
            out[n++] = pc;
            const Module* m = FindModule(static_cast<std::uintptr_t>(pc));
            const RUNTIME_FUNCTION* f =
                m ? FindFunction(*m, static_cast<DWORD>(pc - m->base - (n > 1 ? 1 : 0))) : nullptr;
            if (!f) {
                if (!m && n > 1) break;
                context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);   // a leaf function
                context.Rsp += 8;
            } else {
                PVOID handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, m->base, pc, const_cast<PRUNTIME_FUNCTION>(f), &context,
                                 &handlerData, &establisher, nullptr);
            }
            if (!context.Rsp || (context.Rsp & 7)) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

// The start of the function a frame is in (a return address looked up one byte back); the address itself when its
// module has no unwind data for it.
std::uint64_t FunctionStart(std::uint64_t pc, bool returnAddress) {
    const Module* m = FindModule(static_cast<std::uintptr_t>(pc));
    if (!m) return pc;
    const RUNTIME_FUNCTION* f = FindFunction(*m, static_cast<DWORD>(pc - m->base - (returnAddress ? 1 : 0)));
    return f ? m->base + f->BeginAddress : pc;
}

bool IsApp(std::uint64_t pc) {
    const Module* m = FindModule(static_cast<std::uintptr_t>(pc));
    return m && m->app;
}

std::string Name(std::uint64_t address) {
    char text[96];
    const Module* m = FindModule(static_cast<std::uintptr_t>(address));
    if (!m) std::snprintf(text, sizeof(text), "%llX", static_cast<unsigned long long>(address));
    else std::snprintf(text, sizeof(text), "%s+0x%llX", m->name, static_cast<unsigned long long>(address - m->base));
    return text;
}

// ---- what is kept ----
struct Place {
    std::uint64_t inner = 0, app = 0, caller = 0;   // function starts: where it is, the game's first, the game's next
    bool syscall = false;
    int count = 0;
};
struct ThreadStats {
    DWORD id = 0;
    HANDLE handle = nullptr;
    std::string name;
    ULONGLONG cpuFirst = 0, cpuLast = 0;   // kernel + user, 100 ns
    bool gone = false;
    bool active = true;          // sampled in the rounds (CPU in the last list refresh, or the loading thread)
    ULONGLONG cpuAtRefresh = 0;
    std::vector<float> cpuPerSecond;   // its CPU (s) in each second of the load, from the list refreshes
    int samples = 0, syscalls = 0, otherPlaces = 0;
    std::vector<Place> places;
    std::unordered_map<std::uint64_t, std::size_t> placeIndex;
    // the loading thread only: each of the game's functions on its stack, the samples it was in (all / running)
    std::unordered_map<std::uint64_t, std::pair<int, int>> inclusive;
};
struct HotSpot {
    std::uint64_t inner = 0, app = 0;
    int count = 0;
};

std::vector<ThreadStats> g_threads;
std::unordered_map<std::uint64_t, HotSpot> g_hot;
int g_otherHot = 0;
std::uint64_t g_rounds = 0;
double g_sampleSeconds = 0.0;   // the rounds' own time
ULONGLONG g_samplerCpu = 0;
ULONGLONG g_beganMs = 0;
DWORD g_loadingThread = 0;
DWORD g_samplerThreadId = 0;
HANDLE g_samplerThread = nullptr;
HANDLE g_stop = nullptr;
std::uint64_t g_session = 0;
bool g_running = false;
bool g_stuck = false;   // a sampler that did not stop: no other is started

using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
GetThreadDescriptionFn g_getThreadDescription = nullptr;

ULONGLONG ThreadCpu(HANDLE thread) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) return 0;
    const auto u64 = [](const FILETIME& f) { return (ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return u64(kernel) + u64(user);
}

std::string ThreadName(HANDLE thread) {
    if (!g_getThreadDescription) return {};
    PWSTR wide = nullptr;
    if (FAILED(g_getThreadDescription(thread, &wide)) || !wide) return {};
    char text[96] = {};
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, sizeof(text) - 1, nullptr, nullptr);
    LocalFree(wide);
    return text;
}

void RefreshThreads() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD process = GetCurrentProcessId();
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == g_samplerThreadId) continue;
        const auto known = std::find_if(g_threads.begin(), g_threads.end(),
                                        [&](const ThreadStats& t) { return t.id == entry.th32ThreadID && !t.gone; });
        if (known != g_threads.end() || g_threads.size() >= kMaxThreads) continue;
        const HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION |
                                         THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (!handle) continue;
        ThreadStats t;
        t.id = entry.th32ThreadID;
        t.handle = handle;
        t.name = ThreadName(handle);
        t.cpuFirst = t.cpuLast = t.cpuAtRefresh = ThreadCpu(handle);
        g_threads.push_back(std::move(t));
    }
    CloseHandle(snapshot);
}

void Count(ThreadStats& t, const std::uint64_t* pc, int depth, bool syscall) {
    ++t.samples;
    if (syscall) ++t.syscalls;
    if (t.id == g_loadingThread) {   // every game function on the stack, once a sample
        std::uint64_t seen[kDepth];
        int seenCount = 0;
        for (int i = 0; i < depth; ++i) {
            if (!IsApp(pc[i])) continue;
            const std::uint64_t f = FunctionStart(pc[i], i > 0);
            bool again = false;
            for (int k = 0; k < seenCount && !again; ++k) again = seen[k] == f;
            if (again) continue;
            seen[seenCount++] = f;
            auto& counts = t.inclusive[f];
            ++counts.first;
            if (!syscall) ++counts.second;
        }
    }
    // the place: the function it is in, the game's first function on the stack and the game's one that called it
    int first = 0;
    while (first < depth && !IsApp(pc[first])) ++first;
    Place place;
    place.syscall = syscall;
    place.inner = depth ? FunctionStart(pc[0], false) : 0;
    place.app = first < depth ? FunctionStart(pc[first], first > 0) : 0;
    place.caller = first + 1 < depth ? FunctionStart(pc[first + 1], true) : 0;
    const std::uint64_t key = (place.inner * 0x9E3779B97F4A7C15ull) ^ (place.app * 0xC2B2AE3D27D4EB4Full) ^
                              (place.caller * 0x165667B19E3779F9ull) ^ (syscall ? 1u : 0u);
    const auto found = t.placeIndex.find(key);
    if (found != t.placeIndex.end()) {
        ++t.places[found->second].count;
    } else if (t.places.size() < kMaxPlacesPerThread) {
        place.count = 1;
        t.placeIndex.emplace(key, t.places.size());
        t.places.push_back(place);
    } else {
        ++t.otherPlaces;
    }
    if (syscall) return;
    // over every thread: where the CPU is (the function it is in, the game's first function on the stack)
    const std::uint64_t hotKey = (place.inner * 0x9E3779B97F4A7C15ull) ^ (place.app * 0xC2B2AE3D27D4EB4Full);
    auto hot = g_hot.find(hotKey);
    if (hot != g_hot.end()) ++hot->second.count;
    else if (g_hot.size() < kMaxHotSpots) g_hot.emplace(hotKey, HotSpot{place.inner, place.app, 1});
    else ++g_otherHot;
}

void Round() {
    for (ThreadStats& t : g_threads) {
        if (t.gone || !t.active) continue;
        if (SuspendThread(t.handle) == static_cast<DWORD>(-1)) {
            t.gone = true;
            continue;
        }
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        std::uint64_t pc[kDepth];
        int depth = 0;
        bool syscall = false;
        if (GetThreadContext(t.handle, &context)) {
            const DWORD64 rip = context.Rip;
            __try {   // in a system call: the instruction before its place is "syscall" (0F 05)
                syscall = rip > 2 && reinterpret_cast<const std::uint8_t*>(rip)[-2] == 0x0F &&
                          reinterpret_cast<const std::uint8_t*>(rip)[-1] == 0x05;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                syscall = false;
            }
            depth = Walk(context, pc);
        }
        ResumeThread(t.handle);
        if (depth) Count(t, pc, depth, syscall);
    }
}

LARGE_INTEGER Qpc() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now;
}

DWORD WINAPI SamplerMain(void*) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    ULONGLONG threadsAt = 0, cpuAt = 0, modulesAt = GetTickCount64();
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now - threadsAt >= kThreadListEveryMs) {
            RefreshThreads();
            threadsAt = now;
        }
        if (now - cpuAt >= kCpuEveryMs) {
            const std::size_t second = static_cast<std::size_t>((now - g_beganMs) / 1000);
            for (ThreadStats& t : g_threads) {
                if (t.gone) continue;
                t.cpuLast = std::max(t.cpuLast, ThreadCpu(t.handle));
                const ULONGLONG delta = t.cpuLast - t.cpuAtRefresh;
                t.cpuAtRefresh = t.cpuLast;
                t.active = delta >= kActiveCpu || t.id == g_loadingThread;   // (idle threads: not suspended for nothing)
                if (second < 600) {
                    if (t.cpuPerSecond.size() <= second) t.cpuPerSecond.resize(second + 1, 0.0f);
                    t.cpuPerSecond[second] += static_cast<float>(double(delta) / 1e7);
                }
            }
            cpuAt = now;
        }
        if (now - modulesAt >= kModuleListEveryMs) {   // (a driver's DLL loaded on the way)
            RefreshModules();
            modulesAt = now;
        }
        const LARGE_INTEGER begin = Qpc();
        Round();
        ++g_rounds;
        g_sampleSeconds += double(Qpc().QuadPart - begin.QuadPart) / double(frequency.QuadPart);
        if (WaitForSingleObject(g_stop, kEveryMs) == WAIT_OBJECT_0) break;
    }
    g_samplerCpu = ThreadCpu(GetCurrentThread());
    return 0;
}

bool Wanted() {
    char ini[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    char* slash = std::strrchr(ini, '\\');
    if (slash) slash[1] = 0;
    else ini[0] = 0;
    strcat_s(ini, "ROTTRFramework.ini");
    return GetPrivateProfileIntA("Loading", "SampleThreads", 0, ini) != 0;
}

std::string PlaceText(const Place& p) {
    std::string text = p.syscall ? "[in a system call] " : "";
    text += Name(p.inner);
    if (p.app && p.app != p.inner) text += " < " + Name(p.app);
    if (p.caller) text += " < " + Name(p.caller);
    return text;
}

void Report(double seconds) {
    // the threads' CPU over the load
    double total = 0.0;
    for (ThreadStats& t : g_threads) {
        if (!t.gone) t.cpuLast = std::max(t.cpuLast, ThreadCpu(t.handle));
        total += double(t.cpuLast - t.cpuFirst) / 1e7;
    }
    std::vector<const ThreadStats*> order;
    for (const ThreadStats& t : g_threads) order.push_back(&t);
    std::sort(order.begin(), order.end(), [](const ThreadStats* a, const ThreadStats* b) {
        return a->cpuLast - a->cpuFirst > b->cpuLast - b->cpuFirst;
    });
    Log::Writef("LoadSample: load %llu (%.2f s): %zu threads, %llu rounds every %lu ms (%.2f s of sampling, %.2f s of CPU on "
                "the sampler's thread); the threads' CPU over the load %.1f s",
                static_cast<unsigned long long>(g_session), seconds, g_threads.size(), static_cast<unsigned long long>(g_rounds),
                static_cast<unsigned long>(kEveryMs), g_sampleSeconds, double(g_samplerCpu) / 1e7, total);
    std::string list;
    for (std::size_t i = 0; i < order.size() && i < 16; ++i) {
        char one[160];
        std::snprintf(one, sizeof(one), "%s%lu%s%s%s %.2f", i ? ", " : "", static_cast<unsigned long>(order[i]->id),
                      order[i]->name.empty() ? "" : " \"", order[i]->name.c_str(), order[i]->name.empty() ? "" : "\"",
                      double(order[i]->cpuLast - order[i]->cpuFirst) / 1e7);
        list += one;
    }
    Log::Write(("LoadSample:   CPU by thread (s): " + list).substr(0, 1000).c_str());
    // the busiest threads and the loading thread: where they were
    int shown = 0;
    for (const ThreadStats* t : order) {
        const bool loading = t->id == g_loadingThread;
        if (shown >= kThreadsReported && !loading) continue;
        if (!t->samples) continue;
        ++shown;
        std::vector<const Place*> running, waiting;
        for (const Place& p : t->places) (p.syscall ? waiting : running).push_back(&p);
        const auto most = [](const Place* a, const Place* b) { return a->count > b->count; };
        std::sort(running.begin(), running.end(), most);
        std::sort(waiting.begin(), waiting.end(), most);
        char head[256];
        std::snprintf(head, sizeof(head), "LoadSample:   thread %lu%s%s%s%s: CPU %.2f s; %d samples, %d running, %d in system calls:",
                      static_cast<unsigned long>(t->id), t->name.empty() ? "" : " \"", t->name.c_str(),
                      t->name.empty() ? "" : "\"", loading ? " (the loading thread)" : "",
                      double(t->cpuLast - t->cpuFirst) / 1e7, t->samples, t->samples - t->syscalls, t->syscalls);
        std::string line = head;
        for (std::size_t k = 0; k < running.size() && int(k) < kRunningPlacesPerThread; ++k)
            line += (k ? "; " : " ") + std::to_string(running[k]->count) + "x " + PlaceText(*running[k]);
        for (std::size_t k = 0; k < waiting.size() && int(k) < kWaitingPlacesPerThread; ++k)
            line += (running.empty() && !k ? " " : "; ") + std::to_string(waiting[k]->count) + "x " + PlaceText(*waiting[k]);
        Log::Write(line.substr(0, 1000).c_str());
    }
    // over every thread: where the CPU went
    std::vector<const HotSpot*> hot;
    int running = g_otherHot;
    for (const auto& [key, h] : g_hot) {
        hot.push_back(&h);
        running += h.count;
    }
    std::sort(hot.begin(), hot.end(), [](const HotSpot* a, const HotSpot* b) { return a->count > b->count; });
    std::string spots;
    for (std::size_t k = 0; k < hot.size() && int(k) < kHotSpotsReported; ++k) {
        std::string one = std::to_string(hot[k]->count) + "x " + Name(hot[k]->inner);
        if (hot[k]->app && hot[k]->app != hot[k]->inner) one += " < " + Name(hot[k]->app);
        spots += (k ? "; " : "") + one;
    }
    Log::Write(("LoadSample:   the busiest places over every thread (" + std::to_string(running) + " running samples): " + spots)
                   .substr(0, 1000).c_str());
    // the loading thread's time by the game's functions on its stack (inclusive: a function and all it called)
    for (const ThreadStats& t : g_threads) {
        if (t.id != g_loadingThread || !t.samples) continue;
        std::vector<std::pair<std::uint64_t, std::pair<int, int>>> list(t.inclusive.begin(), t.inclusive.end());
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
        std::string line;
        int lines = 0;
        for (std::size_t k = 0; k < list.size() && k < 48; ++k) {
            char one[96];
            std::snprintf(one, sizeof(one), "%s %d%% (running %d%%)", Name(list[k].first).c_str(),
                          list[k].second.first * 100 / t.samples, list[k].second.second * 100 / t.samples);
            if (line.size() + std::strlen(one) > 900) {
                Log::Write(("LoadSample:   the loading thread's time by function (inclusive, of " + std::to_string(t.samples) +
                            " samples): " + line).c_str());
                line.clear();
                if (++lines >= 4) break;
            }
            line += (line.empty() ? "" : "; ") + std::string(one);
        }
        if (!line.empty() && lines < 4)
            Log::Write(("LoadSample:   the loading thread's time by function (inclusive, of " + std::to_string(t.samples) +
                        " samples): " + line).c_str());
    }
    // each second of the load: the busiest threads' CPU (% of one core), the loading thread among them
    std::string timeline;
    int lines = 0;
    for (const ThreadStats* t : order) {
        const bool loading = t->id == g_loadingThread;
        if (lines >= kTimelineThreads && !loading) continue;
        ++lines;
        std::string row = std::to_string(t->id) + (loading ? " (loading)" : "") + ":";
        for (const float cpu : t->cpuPerSecond) row += " " + std::to_string(static_cast<int>(cpu * 100.0f + 0.5f));
        Log::Write(("LoadSample:   CPU % each second, thread " + row).substr(0, 1000).c_str());
    }
}

void Reset() {
    for (ThreadStats& t : g_threads)
        if (t.handle) CloseHandle(t.handle);
    g_threads.clear();
    g_hot.clear();
    g_otherHot = 0;
    g_rounds = 0;
    g_sampleSeconds = 0.0;
    g_samplerCpu = 0;
}
}  // namespace

void Begin(std::uint64_t session) {
    if (g_running || g_stuck || !Wanted()) return;
    if (!g_getThreadDescription) {
        if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll"))
            g_getThreadDescription = reinterpret_cast<GetThreadDescriptionFn>(GetProcAddress(kernel, "GetThreadDescription"));
    }
    if (!g_stop) g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop) return;
    Reset();
    ResetEvent(g_stop);
    RefreshModules();
    g_session = session;
    g_loadingThread = GetCurrentThreadId();
    g_beganMs = GetTickCount64();
    g_samplerThread = CreateThread(nullptr, 0, &SamplerMain, nullptr, CREATE_SUSPENDED, &g_samplerThreadId);
    if (!g_samplerThread) return;
    SetThreadPriority(g_samplerThread, THREAD_PRIORITY_HIGHEST);   // (its rounds on time, however busy the load)
    g_running = true;
    ResumeThread(g_samplerThread);
}

void End(std::uint64_t session, double seconds) {
    if (!g_running) return;
    g_running = false;
    SetEvent(g_stop);
    if (WaitForSingleObject(g_samplerThread, 3000) != WAIT_OBJECT_0) {
        Log::Writef("LoadSample: load %llu: the sampler did not stop in 3 s; no report", static_cast<unsigned long long>(session));
        g_stuck = true;   // (its data left as it is; no sampler again this session)
        return;
    }
    CloseHandle(g_samplerThread);
    g_samplerThread = nullptr;
    Report(seconds);
    Reset();
}

}  // namespace LoadSampler
