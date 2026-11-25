// Where the game stops presenting frames (stall_sampler.h).
#include "stall_sampler.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "log.h"

namespace StallSampler {
namespace {
constexpr ULONGLONG kStallMs = 250;          // no frame for this long: a stall
constexpr DWORD kSampleEveryMs = 25;
constexpr ULONGLONG kSampleForMs = 30000;    // longer: a pause (debugger, sleep), stop sampling
constexpr int kMaxReports = 30;
constexpr int kDepth = 32;
constexpr int kMaxSamples = 1200;            // 30 s of both threads
constexpr int kMaxModules = 512;
// (v0.9.470) A stall that never ends (a hang: the black screen at startup) was never reported (the report waits for
// frames to come back). One that began with the game in front and is still on after kOngoingReportMs is reported
// then, with the samples so far; after kHangDumpMs a minidump of every thread is written next to the game
// (ROTTRFramework_hang.dmp, once a session), as long as the game is still in front.
constexpr ULONGLONG kOngoingReportMs = 10000;
constexpr ULONGLONG kHangDumpMs = 15000;
// (v0.9.592) The main menu's hang (0.9.566, 0.9.569, 0.9.591): the game's tick thread waits in
// CommonRenderDevice::BeginFrame (retail 0x297620) for the previous frame's end (+0x130, set by DoFrameStateSwitch
// 0x2A13E0 at its end) while the Present thread waits for work; the frame goes through more threads than those two
// (EndFrame 0x2AF310 -> a job, 0x2B1100 -> +0x140 counted down by 0x2B2B50 -> the thread waiting at +0x138 -> the
// flush thread 0x2B0EC0, waiting at +0x148, whose command 1 runs 0x2B0680 -> DoFrameStateSwitch). So the ongoing
// report now also has every thread's stack once and the render device's frame state, and the dump is written once a
// hang was reported, whether or not the game is still in front.
constexpr int kCensusMax = 256;
constexpr std::uintptr_t kRvaRenderDevice = 0x01A53E40ull;   // cdc::CommonRenderDevice* (the PC DX12 device)

std::atomic<ULONGLONG> g_lastPresentMs{0};
std::atomic<DWORD> g_gameThread{0};
std::atomic<DWORD> g_presentThread{0};
std::atomic<bool> g_started{false};

ULONGLONG NowMs() {
    static LARGE_INTEGER frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<ULONGLONG>(counter.QuadPart * 1000 / frequency.QuadPart);
}

// ---- modules and their unwind data (built before a thread is suspended) ----
struct Module {
    std::uintptr_t base = 0, end = 0;
    const RUNTIME_FUNCTION* functions = nullptr;
    DWORD functionCount = 0;
    bool app = false;   // the game or the framework (else: Windows, drivers)
    char name[48] = {};
};
Module g_modules[kMaxModules];
int g_moduleCount = 0;
std::uintptr_t g_frameworkBase = 0;

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
        if (handles[i] == self) g_frameworkBase = m.base;
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

// ---- sampling (the thread is suspended: static data only) ----
struct Sample {
    std::uint8_t present = 0;   // 0 the game's tick thread, 1 the Present thread
    std::uint8_t depth = 0;
    std::uint64_t pc[kDepth];
};
Sample g_samples[kMaxSamples];
int g_sampleCount = 0;

int Walk(CONTEXT& context, std::uint64_t* out) {
    int n = 0;
    __try {
        while (n < kDepth) {
            const DWORD64 pc = context.Rip;
            if (pc < 0x10000) break;
            out[n++] = pc;
            const Module* m = FindModule(static_cast<std::uintptr_t>(pc));
            // (a return address can be just past its function's end: look its call up)
            const RUNTIME_FUNCTION* f =
                m ? FindFunction(*m, static_cast<DWORD>(pc - m->base - (n > 1 ? 1 : 0))) : nullptr;
            if (!f) {
                if (!m && n > 1) break;   // code no module owns past the first frame: give up
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

void SampleThread(HANDLE thread, std::uint8_t present) {
    if (!thread || g_sampleCount >= kMaxSamples) return;
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) return;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(thread, &context)) {
        Sample& s = g_samples[g_sampleCount];
        s.present = present;
        s.depth = static_cast<std::uint8_t>(Walk(context, s.pc));
        if (s.depth) ++g_sampleCount;
    }
    ResumeThread(thread);
}

HANDLE OpenSampled(DWORD id, DWORD& openId, HANDLE& handle) {
    if (id == openId) return handle;
    if (handle) CloseHandle(handle);
    handle = id ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id) : nullptr;
    openId = id;
    return handle;
}

// ---- the report (no thread suspended) ----
using SymFromAddrFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
SymFromAddrFn g_symFromAddr = nullptr;

void LoadFrameworkSymbols() {
    static bool tried = false;
    if (tried || !g_frameworkBase) return;
    tried = true;
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (!dbghelp) return;
    using SetOptionsFn = DWORD(WINAPI*)(DWORD);
    using InitializeFn = BOOL(WINAPI*)(HANDLE, PCWSTR, BOOL);
    using LoadModuleFn = DWORD64(WINAPI*)(HANDLE, HANDLE, PCWSTR, PCWSTR, DWORD64, DWORD, PMODLOAD_DATA, DWORD);
    using ModuleInfoFn = BOOL(WINAPI*)(HANDLE, DWORD64, PIMAGEHLP_MODULEW64);
    auto setOptions = reinterpret_cast<SetOptionsFn>(GetProcAddress(dbghelp, "SymSetOptions"));
    auto initialize = reinterpret_cast<InitializeFn>(GetProcAddress(dbghelp, "SymInitializeW"));
    auto loadModule = reinterpret_cast<LoadModuleFn>(GetProcAddress(dbghelp, "SymLoadModuleExW"));
    auto moduleInfo = reinterpret_cast<ModuleInfoFn>(GetProcAddress(dbghelp, "SymGetModuleInfoW64"));
    auto fromAddr = reinterpret_cast<SymFromAddrFn>(GetProcAddress(dbghelp, "SymFromAddr"));
    if (!setOptions || !initialize || !loadModule || !moduleInfo || !fromAddr) return;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(reinterpret_cast<HMODULE>(g_frameworkBase), path, MAX_PATH)) return;
    std::wstring folder = path;
    folder.resize(folder.find_last_of(L"\\/") == std::wstring::npos ? 0 : folder.find_last_of(L"\\/"));
    setOptions(SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    if (!initialize(GetCurrentProcess(), folder.c_str(), FALSE)) return;
    const Module* m = FindModule(g_frameworkBase);
    if (!m || !loadModule(GetCurrentProcess(), nullptr, path, nullptr, g_frameworkBase,
                          static_cast<DWORD>(m->end - m->base), nullptr, 0))
        return;
    IMAGEHLP_MODULEW64 info{};
    info.SizeOfStruct = sizeof(info);
    if (moduleInfo(GetCurrentProcess(), g_frameworkBase, &info) && info.SymType == SymPdb) g_symFromAddr = fromAddr;
}

std::string Name(std::uint64_t pc) {
    char text[160];
    const Module* m = FindModule(static_cast<std::uintptr_t>(pc));
    if (!m) {
        std::snprintf(text, sizeof(text), "%llX", static_cast<unsigned long long>(pc));
        return text;
    }
    if (m->base == g_frameworkBase && g_symFromAddr) {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 128] = {};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 127;
        DWORD64 displacement = 0;
        if (g_symFromAddr(GetCurrentProcess(), pc, &displacement, symbol)) {
            std::snprintf(text, sizeof(text), "%s!%s+0x%llX", m->name, symbol->Name,
                          static_cast<unsigned long long>(displacement));
            return text;
        }
    }
    std::snprintf(text, sizeof(text), "%s+0x%llX", m->name, static_cast<unsigned long long>(pc - m->base));
    return text;
}

bool IsApp(std::uint64_t pc) {
    const Module* m = FindModule(static_cast<std::uintptr_t>(pc));
    return m && m->app;
}

// A stack's key: its first 4 frames in the game or the framework (where it waits or works, and who asked).
std::string Describe(const Sample& s, std::uint64_t* key) {
    int first = 0;
    while (first < s.depth && !IsApp(s.pc[first])) ++first;
    std::uint64_t hash = 1469598103934665603ull;
    for (int i = first; i < s.depth && i < first + 4; ++i) hash = (hash ^ s.pc[i]) * 1099511628211ull;
    *key = hash ^ (first == s.depth ? 0x5A5A : 0);
    std::string text;
    if (first > 0) {   // the Windows frames above it, by module only (a wait, a file read, the driver)
        text += "[";
        const Module* last = nullptr;
        for (int i = 0; i < first; ++i) {
            const Module* m = FindModule(static_cast<std::uintptr_t>(s.pc[i]));
            if (m == last) continue;
            if (last) text += " < ";
            text += m ? Name(s.pc[i]) : "?";
            last = m;
        }
        text += "] ";
    }
    for (int i = first, shown = 0; i < s.depth && shown < 10; ++i, ++shown) {
        if (shown) text += " < ";
        text += Name(s.pc[i]);
    }
    return text;
}

void Report(ULONGLONG lengthMs, long long atMs, DWORD gameId, DWORD presentId, bool ongoing = false) {
    static int reports = 0;
    if (reports >= kMaxReports) return;
    ++reports;
    LoadFrameworkSymbols();
    // One line per thread, then its 3 most frequent stacks a line each.
    const auto summarize = [&](std::uint8_t present, const char* thread, DWORD id) {
        struct Group {
            std::uint64_t key;
            int count;
            int sample;
        };
        std::vector<Group> groups;
        int total = 0;
        for (int i = 0; i < g_sampleCount; ++i) {
            if (g_samples[i].present != present) continue;
            ++total;
            std::uint64_t key = 0;
            (void)Describe(g_samples[i], &key);
            auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.key == key; });
            if (it == groups.end()) groups.push_back({key, 1, i});
            else ++it->count;
        }
        std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.count > b.count; });
        Log::Writef("Stall:   %s thread %lu: %d samples, %zu different stacks", thread, static_cast<unsigned long>(id),
                    total, groups.size());
        for (std::size_t g = 0; g < groups.size() && g < 3; ++g) {
            std::uint64_t key = 0;
            const std::string line = "Stall:     " + std::to_string(groups[g].count) + "x " +
                                     Describe(g_samples[groups[g].sample], &key);
            Log::Write(line.substr(0, 1000).c_str());
        }
    };
    if (ongoing)
        Log::Writef("Stall: no frame for %llu ms so far, none since (the game may be stuck), at %lld ms from process start",
                    static_cast<unsigned long long>(lengthMs), atMs);
    else
        Log::Writef("Stall: no frame for %llu ms, at %lld ms from process start", static_cast<unsigned long long>(lengthMs),
                    atMs);
    summarize(0, "game tick", gameId);
    if (presentId && presentId != gameId) summarize(1, "Present", presentId);
}

// (v0.9.470) Every thread's stack (and the memory they point at) into ROTTRFramework_hang.dmp, next to the framework.
void WriteHangDump() {
    using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                              PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    auto write = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    wchar_t path[MAX_PATH] = {};
    if (!write || !g_frameworkBase || !GetModuleFileNameW(reinterpret_cast<HMODULE>(g_frameworkBase), path, MAX_PATH)) {
        Log::Write("Stall: no hang dump (DbgHelp or the framework's path not found)");
        return;
    }
    std::wstring file = path;
    const std::size_t slash = file.find_last_of(L"\\/");
    file.resize(slash == std::wstring::npos ? 0 : slash + 1);
    // (v0.9.591) into ROTTRFramework\CrashDumps beside the framework, as the crash files
    file += L"ROTTRFramework";
    CreateDirectoryW(file.c_str(), nullptr);
    file += L"\\CrashDumps";
    CreateDirectoryW(file.c_str(), nullptr);
    file += L"\\ROTTRFramework_hang.dmp";
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        Log::Writef("Stall: the hang dump could not be created (Win32 %lu)", GetLastError());
        return;
    }
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                                                 MiniDumpWithUnloadedModules);
    const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), handle, type, nullptr, nullptr, nullptr);
    const DWORD error = ok ? 0 : GetLastError();
    LARGE_INTEGER size{};
    GetFileSizeEx(handle, &size);
    CloseHandle(handle);
    if (ok)
        Log::Writef("Stall: every thread's stack written to ROTTRFramework\\CrashDumps\\ROTTRFramework_hang.dmp (%lld KB)",
                    static_cast<long long>(size.QuadPart / 1024));
    else
        Log::Writef("Stall: the hang dump failed (Win32 %lu)", error);
}

// ---- (v0.9.592) every thread once, and the render device's frame state ----
struct CensusEntry {
    DWORD id = 0;
    Sample s{};
};
CensusEntry g_census[kCensusMax];
int g_censusCount = 0;

void TakeCensus() {
    g_censusCount = 0;
    DWORD ids[kCensusMax];
    int count = 0;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
    for (BOOL more = Thread32First(snapshot, &entry); more && count < kCensusMax; more = Thread32Next(snapshot, &entry))
        if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self) ids[count++] = entry.th32ThreadID;
    CloseHandle(snapshot);
    for (int i = 0; i < count; ++i) {
        const HANDLE thread =
            OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, ids[i]);
        if (!thread) continue;
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            CensusEntry& c = g_census[g_censusCount];
            if (GetThreadContext(thread, &context)) {
                c.id = ids[i];
                c.s.depth = static_cast<std::uint8_t>(Walk(context, c.s.pc));
                if (c.s.depth) ++g_censusCount;
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
}

void ReportCensus() {
    struct Group {
        std::uint64_t key;
        int sample;
        std::vector<DWORD> ids;
    };
    std::vector<Group> app, system;
    for (int i = 0; i < g_censusCount; ++i) {
        std::uint64_t key = 0;
        (void)Describe(g_census[i].s, &key);
        bool hasApp = false;
        for (int k = 0; k < g_census[i].s.depth && !hasApp; ++k) hasApp = IsApp(g_census[i].s.pc[k]);
        auto& groups = hasApp ? app : system;
        auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.key == key; });
        if (it == groups.end()) groups.push_back({key, i, {g_census[i].id}});
        else it->ids.push_back(g_census[i].id);
    }
    std::sort(app.begin(), app.end(), [](const Group& a, const Group& b) { return a.ids.size() > b.ids.size(); });
    int appThreads = 0;
    for (const Group& g : app) appThreads += static_cast<int>(g.ids.size());
    Log::Writef("Stall:   every thread once (%d): %d with the game's or the framework's code on their stack (%zu different "
                "stacks), %d in other code only", g_censusCount, appThreads, app.size(), g_censusCount - appThreads);
    int lines = 0;
    for (const Group& g : app) {
        if (++lines > 48) {
            Log::Write("Stall:     ... (more stacks left out)");
            break;
        }
        std::string ids;
        for (std::size_t k = 0; k < g.ids.size() && k < 6; ++k) ids += (k ? "," : "") + std::to_string(g.ids[k]);
        if (g.ids.size() > 6) ids += ",...";
        std::uint64_t key = 0;
        const std::string line = "Stall:     " + std::to_string(g.ids.size()) + "x (" + ids + ") " +
                                 Describe(g_census[g.sample].s, &key);
        Log::Write(line.substr(0, 1000).c_str());
    }
    std::string others;
    for (const Group& g : system) {
        if (others.size() > 700) {
            others += "; ...";
            break;
        }
        std::uint64_t key = 0;
        std::string top = Describe(g_census[g.sample].s, &key);
        top = top.substr(0, 120);
        others += (others.empty() ? "" : "; ") + std::to_string(g.ids.size()) + "x " + top;
    }
    if (!others.empty()) Log::Write(("Stall:     other code only: " + others).substr(0, 1000).c_str());
}

struct RenderDeviceState {
    bool read = false;
    std::uintptr_t device = 0, vtableRva = 0;
    int modeSetting = 0, mode = 0, flushPending = 0, framePending = 0;
    std::uint8_t inFrame = 0, begun = 0, switchPending = 0, flushing = 0, autoEnd = 0, autoEnding = 0;
    std::uint32_t frames = 0;
    std::uint64_t flushThread = 0, flushQueue = 0, kickQueue = 0;
};

RenderDeviceState ReadRenderDevice() {
    RenderDeviceState r{};
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    __try {
        const auto device = *reinterpret_cast<const std::uintptr_t*>(base + kRvaRenderDevice);
        r.device = device;
        if (!device) return r;
        const auto* b = reinterpret_cast<const std::uint8_t*>(device);
        r.vtableRva = *reinterpret_cast<const std::uintptr_t*>(b) - base;
        r.modeSetting = *reinterpret_cast<const int*>(b + 0x118);   // what BeginFrame makes the mode (0x29762A)
        r.mode = *reinterpret_cast<const int*>(b + 0x11C);          // EndFrame: 0 / 1 in place, 2 by the threads, else none
        r.flushPending = *reinterpret_cast<const int*>(b + 0x120);  // the flush thread's commands 2 / 3 not done (+0x1C0 at 0)
        r.inFrame = b[0x124];                                       // BeginFrame's end .. EndFrame's end
        r.framePending = *reinterpret_cast<const int*>(b + 0x140);  // a frame's parts not done (0x2B2B50: the last kicks)
        r.begun = b[0x201];
        r.switchPending = b[0x202];   // set by BeginFrame, cleared by DoFrameStateSwitch before its SetEvent(+0x130)
        r.flushing = b[0x203];        // set by the flush (0x2B0680)
        r.autoEnd = b[0x208];         // BeginFrame ends a frame not ended when this and +0x201 are set
        r.autoEnding = b[0x209];
        r.frames = *reinterpret_cast<const std::uint32_t*>(b + 0x1F900);
        r.flushThread = *reinterpret_cast<const std::uint64_t*>(b + 0x26FF8);   // 0x2B0F0B
        r.flushQueue = *reinterpret_cast<const std::uint64_t*>(b + 0xF0);       // the flush thread's list (0 = empty)
        r.kickQueue = *reinterpret_cast<const std::uint64_t*>(b + 0xB0);        // 0x2B2B94's list (+0x138's thread)
        r.read = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return r;
}

void ReportRenderDevice() {
    const RenderDeviceState r = ReadRenderDevice();
    if (!r.read) {
        Log::Writef("Stall:   the render device: %s", r.device ? "not readable" : "none yet");
        return;
    }
    Log::Writef("Stall:   the render device %p (vtable ROTTR.exe+0x%llX): mode %d (setting %d), in a frame %u, begun %u, "
                "its end pending %u, flushing %u, auto end %u/%u; frame parts pending %d, flush commands pending %d, frame %u; "
                "the flush thread %llu, its queue %s, the kick queue %s",
                reinterpret_cast<void*>(r.device), static_cast<unsigned long long>(r.vtableRva), r.mode, r.modeSetting,
                r.inFrame, r.begun, r.switchPending, r.flushing, r.autoEnd, r.autoEnding, r.framePending, r.flushPending,
                r.frames, static_cast<unsigned long long>(r.flushThread), r.flushQueue ? "holds commands" : "empty",
                r.kickQueue ? "holds commands" : "empty");
}

// ---- (v0.9.592) the render flush's lost pass event ----
// The hang caught in 0.9.592's test launches (1 of 4): the flush thread (the render device's, its id at +0x26FF8) in
// cdc::RenderPassData::ExecutePasses (retail 0x2B1310, TR2 0x1402D1880; from ExecuteFlush 0x2B0680 ->
// RenderDrawableList::Draw -> DrawDefault -> RenderPassInstanceDrawable::Draw) waiting in EventFlags32::WaitAny
// (0x3C91E0: WaitForMultipleObjects(1, the pass's event, any, INFINITE)) for one pass's bit, which the pass's
// JobPresortInstances job (ScheduleSortAndPrepareInstancingJob, on the post-collect job chain) sets at its end; every
// job and render worker idle, so nothing would ever set it. The game thread waits behind it in BeginFrame (+0x130),
// since the next frame's flush is queued after this one. A wait there that lasts 3 s while no frame comes is that
// hang: the event is set, the same as EventFlags32::Set, and the flush goes on.
// ExecutePasses: mov r11, rsp; mov [r11+18h], r8; mov [r11+8], rcx; push rbp; push r12; push r14; sub rsp, 70h
constexpr std::uintptr_t kRvaExecutePasses = 0x002B1310ull;
constexpr std::uint8_t kExecutePassesSig[] = {0x4C, 0x8B, 0xDC, 0x4D, 0x89, 0x43, 0x18, 0x49, 0x89, 0x4B,
                                              0x08, 0x55, 0x41, 0x54, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x70};
// mov rcx, [rsp+0C8h] (the IEventFlags); movsxd rdx, ebx (the pass's bit); mov rax, [rcx]; call [rax+18h] (WaitAny)
constexpr std::uintptr_t kRvaExecutePassesWait = 0x002B13C6ull, kRvaExecutePassesWaitReturn = 0x002B13D7ull;
constexpr std::uint8_t kExecutePassesWaitSig[] = {0x48, 0x8B, 0x8C, 0x24, 0xC8, 0x00, 0x00, 0x00, 0x48,
                                                  0x63, 0xD3, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x18};
// WaitAny: sub rsp, 148h; ...; or r9d, -1; lea rdx, [rsp+40h] (the events); xor r8d, r8d; mov ecx, r10d; call [WFMO]
constexpr std::uintptr_t kRvaWaitAny = 0x003C91E0ull, kRvaWaitAnyWait = 0x003C9226ull, kRvaWaitAnyReturn = 0x003C923Bull;
constexpr std::uint8_t kWaitAnySig[] = {0x48, 0x81, 0xEC, 0x48, 0x01, 0x00, 0x00};
constexpr std::uint8_t kWaitAnyWaitSig[] = {0x41, 0x83, 0xC9, 0xFF, 0x48, 0x8D, 0x54, 0x24, 0x40,
                                            0x45, 0x33, 0xC0, 0x41, 0x8B, 0xCA, 0xFF, 0x15};
constexpr std::uintptr_t kRvaEventFlags32Vtable = 0x00D422E0ull;   // cdc::EventFlags32 (+8: 32 events, +0x108 count)
constexpr ULONGLONG kLostPassMs = 3000;
constexpr int kMaxLostPassReleases = 32;

int g_lostPassCode = 0;   // 0 unchecked, 1 as known, -1 different
int g_lostPassReleases = 0;
std::uintptr_t g_lostPassSlot = 0;
std::uint64_t g_lostPassHandle = 0;
ULONGLONG g_lostPassSince = 0;

bool LostPassCodeMatches(std::uintptr_t base) {
    __try {
        return std::memcmp(reinterpret_cast<const void*>(base + kRvaExecutePasses), kExecutePassesSig,
                           sizeof(kExecutePassesSig)) == 0 &&
               std::memcmp(reinterpret_cast<const void*>(base + kRvaExecutePassesWait), kExecutePassesWaitSig,
                           sizeof(kExecutePassesWaitSig)) == 0 &&
               std::memcmp(reinterpret_cast<const void*>(base + kRvaWaitAny), kWaitAnySig, sizeof(kWaitAnySig)) == 0 &&
               std::memcmp(reinterpret_cast<const void*>(base + kRvaWaitAnyWait), kWaitAnyWaitSig,
                           sizeof(kWaitAnyWaitSig)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct LostPassSlot {
    std::uintptr_t slot = 0, flags = 0;
    std::uint64_t handle = 0;
    std::uint32_t bit = 0, count = 0;
};

// The flush thread's stack from its rsp: ExecutePasses' return address from WaitAny (slot S), WaitAny's own frame
// below it (S - 0x148: its bit numbers at +0x20, its events at +0x40; WFMO's return address at S - 0x150) and
// ExecutePasses' IEventFlags (its rsp + 0xC8 = S + 0xD0). The thread waits in the kernel: its stack does not move.
bool FindLostPassSlot(std::uintptr_t base, std::uintptr_t rsp, LostPassSlot& out) {
    __try {
        const std::uint64_t wanted = base + kRvaExecutePassesWaitReturn;
        for (std::uintptr_t a = (rsp + 7) & ~std::uintptr_t(7); a < rsp + 0x4000; a += 8) {
            if (*reinterpret_cast<const std::uint64_t*>(a) != wanted) continue;
            if (*reinterpret_cast<const std::uint64_t*>(a - 0x150) != base + kRvaWaitAnyReturn) return false;
            const std::uintptr_t flags = *reinterpret_cast<const std::uintptr_t*>(a + 0xD0);
            if (!flags || *reinterpret_cast<const std::uintptr_t*>(flags) != base + kRvaEventFlags32Vtable) return false;
            const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(flags + 0x108);
            const std::uint32_t bit = *reinterpret_cast<const std::uint8_t*>(a - 0x128);
            const std::uint64_t handle = *reinterpret_cast<const std::uint64_t*>(a - 0x108);
            if (count == 0 || count > 32 || bit >= count || !handle) return false;
            if (*reinterpret_cast<const std::uint64_t*>(flags + 8 + bit * 8) != handle) return false;
            out.slot = a;
            out.flags = flags;
            out.handle = handle;
            out.bit = bit;
            out.count = count;
            return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// While no frame comes (from the watch thread, every 500 ms): the flush thread in that wait for kLostPassMs with
// the same event: set it.
void CheckLostPassEvent(ULONGLONG now) {
    if (g_lostPassReleases >= kMaxLostPassReleases || g_lostPassCode < 0) return;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!g_lostPassCode) {
        g_lostPassCode = LostPassCodeMatches(base) ? 1 : -1;
        if (g_lostPassCode < 0) {
            Log::Write("Stall: the render flush's pass wait is not the known code: its lost event is not watched");
            return;
        }
    }
    const RenderDeviceState device = ReadRenderDevice();
    if (!device.read || !device.flushThread || device.flushThread > 0xFFFFFFFFull) return;
    const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                     static_cast<DWORD>(device.flushThread));
    if (!thread) return;
    Sample s{};
    std::uintptr_t rsp = 0;
    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(thread, &context)) {
            rsp = static_cast<std::uintptr_t>(context.Rsp);
            s.depth = static_cast<std::uint8_t>(Walk(context, s.pc));
        }
        ResumeThread(thread);
    }
    CloseHandle(thread);
    // in WaitAny's wait, called by ExecutePasses (only Windows and the framework's waits hook above it)
    bool waiting = false;
    for (int i = 0; i + 1 < s.depth && i < 10; ++i) {
        if (s.pc[i] == base + kRvaWaitAnyReturn) {
            waiting = s.pc[i + 1] == base + kRvaExecutePassesWaitReturn;
            break;
        }
        const Module* m = FindModule(static_cast<std::uintptr_t>(s.pc[i]));
        if (m && m->base == base) break;   // other game code first: not that wait
    }
    LostPassSlot slot{};
    if (!waiting || !rsp || !FindLostPassSlot(base, rsp, slot)) {
        g_lostPassSlot = 0;
        return;
    }
    if (slot.slot != g_lostPassSlot || slot.handle != g_lostPassHandle) {
        g_lostPassSlot = slot.slot;
        g_lostPassHandle = slot.handle;
        g_lostPassSince = now;
        return;
    }
    if (now < g_lostPassSince + kLostPassMs) return;
    const BOOL set = SetEvent(reinterpret_cast<HANDLE>(slot.handle));
    ++g_lostPassReleases;
    Log::Writef("Stall: the render flush (thread %llu) waited %llu ms in RenderPassData::ExecutePasses for its pass event %u "
                "(EventFlags32 %p, %u events), which no job will set: %s (frame parts pending %d, flush commands pending %d)",
                static_cast<unsigned long long>(device.flushThread),
                static_cast<unsigned long long>(now - g_lostPassSince), slot.bit, reinterpret_cast<void*>(slot.flags),
                slot.count, set ? "set" : "could not be set", device.framePending, device.flushPending);
    g_lostPassSlot = 0;
}

bool GameInForeground() {
    DWORD process = 0;
    HWND window = GetForegroundWindow();
    if (window) GetWindowThreadProcessId(window, &process);
    return process == GetCurrentProcessId();
}

ULONGLONG ProcessStartMs() {
    FILETIME created{}, exited{}, kernel{}, user{}, current{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    GetSystemTimeAsFileTime(&current);
    const auto toU64 = [](const FILETIME& ft) {
        return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    const ULONGLONG elapsed = (toU64(current) - toU64(created)) / 10000ull;
    const ULONGLONG now = NowMs();
    return now > elapsed ? now - elapsed : 0;
}

DWORD WINAPI WatchThread(void*) {
    const ULONGLONG processStart = ProcessStartMs();
    DWORD gameOpen = 0, presentOpen = 0;
    HANDLE gameHandle = nullptr, presentHandle = nullptr;
    bool stalled = false, background = false;
    bool ongoingReported = false;   // (v0.9.470)
    bool hangDumped = false;        // (v0.9.470, once a session)
    ULONGLONG stallFrame = 0;   // the last frame before the stall
    ULONGLONG lostPassCheck = 0;   // (v0.9.592)
    for (;;) {
        Sleep(stalled ? kSampleEveryMs : 50);
        const ULONGLONG lastFrame = g_lastPresentMs.load(std::memory_order_acquire);
        const ULONGLONG now = NowMs();
        if (!stalled) {
            if (!lastFrame || now < lastFrame + kStallMs) continue;
            stalled = true;
            ongoingReported = false;
            stallFrame = lastFrame;
            background = !GameInForeground();
            g_sampleCount = 0;
            RefreshModules();
        }
        if (lastFrame != stallFrame) {   // frames again
            stalled = false;
            if (!background && g_sampleCount)
                Report(lastFrame - stallFrame, static_cast<long long>(stallFrame - processStart),
                       g_gameThread.load(std::memory_order_relaxed), g_presentThread.load(std::memory_order_relaxed));
            continue;
        }
        if (!GameInForeground()) background = true;
        // (v0.9.470) still no frame, the game in front all along: reported now, a dump of every thread a little later
        if (!background && !ongoingReported && g_sampleCount && now >= stallFrame + kOngoingReportMs) {
            ongoingReported = true;
            Report(now - stallFrame, static_cast<long long>(stallFrame - processStart),
                   g_gameThread.load(std::memory_order_relaxed), g_presentThread.load(std::memory_order_relaxed), true);
            ReportRenderDevice();   // (v0.9.592)
            TakeCensus();
            ReportCensus();
        }
        // (v0.9.592) once a hang was reported, the dump is written even if another window came in front since
        if ((!background || ongoingReported) && !hangDumped && now >= stallFrame + kHangDumpMs) {
            hangDumped = true;
            WriteHangDump();
        }
        // (v0.9.592) the render flush's lost pass event, whether or not the game is in front
        if (now >= stallFrame + 1000 && now >= lostPassCheck + 500) {
            lostPassCheck = now;
            CheckLostPassEvent(now);
        }
        if (background || now > stallFrame + kSampleForMs) continue;
        const DWORD gameId = g_gameThread.load(std::memory_order_relaxed);
        const DWORD presentId = g_presentThread.load(std::memory_order_relaxed);
        SampleThread(OpenSampled(gameId, gameOpen, gameHandle), 0);
        if (presentId != gameId) SampleThread(OpenSampled(presentId, presentOpen, presentHandle), 1);
    }
}
}  // namespace

void GameTickBeat() {
    const DWORD id = GetCurrentThreadId();
    if (g_gameThread.load(std::memory_order_relaxed) != id) g_gameThread.store(id, std::memory_order_relaxed);
}

void PresentBeat() {
    const DWORD id = GetCurrentThreadId();
    if (g_presentThread.load(std::memory_order_relaxed) != id) g_presentThread.store(id, std::memory_order_relaxed);
    g_lastPresentMs.store(NowMs(), std::memory_order_release);
    if (!g_started.exchange(true, std::memory_order_acq_rel)) {
        if (HANDLE thread = CreateThread(nullptr, 0, &WatchThread, nullptr, 0, nullptr)) CloseHandle(thread);
    }
}
}  // namespace StallSampler
