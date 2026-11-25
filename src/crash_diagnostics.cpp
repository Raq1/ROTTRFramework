#include "crash_diagnostics.h"
#include "log.h"

#include <DbgHelp.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {
using MiniDumpWriteDumpFn = BOOL (WINAPI*)(
    HANDLE,
    DWORD,
    HANDLE,
    MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION,
    PMINIDUMP_USER_STREAM_INFORMATION,
    PMINIDUMP_CALLBACK_INFORMATION);

HMODULE g_trainerModule = nullptr;
std::uint64_t g_trainerBegin = 0, g_trainerEnd = 0;   // its image (a fault in there: one of its guarded reads)
HMODULE g_dbgHelpModule = nullptr;
MiniDumpWriteDumpFn g_miniDumpWriteDump = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_previousUnhandledFilter = nullptr;
std::atomic_bool g_installed{false};
volatile LONG g_reportInProgress = 0;

std::atomic<std::uint32_t> g_breadcrumbSequence{0};
char g_breadcrumb[384]{};

char g_crashLogPath[MAX_PATH]{};
char g_lastFault[2048]{};   // the last first-chance fault outside the framework (see FirstChanceHandler)
char g_dumpPath[MAX_PATH]{};

// The game's folder (with its last slash; empty if not found).
void GameFolder(char (&out)[MAX_PATH]) {
    out[0] = '\0';
    const DWORD len = GetModuleFileNameA(nullptr, out, MAX_PATH);
    if (!len || len >= MAX_PATH) {
        out[0] = '\0';
        return;
    }
    char* filePart = out;
    for (char* p = out; *p; ++p) {
        if (*p == '\\' || *p == '/') filePart = p + 1;
    }
    *filePart = '\0';
}

// (v0.9.591) The crash files go to <game>\ROTTRFramework\CrashDumps (made when missing; the game's folder if it cannot
// be made).
void BuildSiblingPath(const char* fileName, char (&out)[MAX_PATH]) {
    GameFolder(out);
    if (!out[0]) {
        strncpy_s(out, fileName, _TRUNCATE);
        return;
    }
    char dir[MAX_PATH];
    strncpy_s(dir, out, _TRUNCATE);
    strncat_s(dir, MAX_PATH, "ROTTRFramework", _TRUNCATE);
    CreateDirectoryA(dir, nullptr);
    strncat_s(dir, MAX_PATH, "\\CrashDumps", _TRUNCATE);
    const BOOL made = CreateDirectoryA(dir, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
    if (made) {
        strncpy_s(out, dir, _TRUNCATE);
        strncat_s(out, MAX_PATH, "\\", _TRUNCATE);
    }
    strncat_s(out, MAX_PATH, fileName, _TRUNCATE);
}

// (v0.9.591) The crash and hang files earlier versions left in the game's folder moved into CrashDumps.
void MoveOldCrashFiles() {
    char root[MAX_PATH];
    GameFolder(root);
    if (!root[0]) return;
    for (const char* pattern : {"ROTTRFramework_crash*.log", "ROTTRFramework_crash*.dmp", "ROTTRFramework_hang.dmp"}) {
        char search[MAX_PATH];
        strncpy_s(search, root, _TRUNCATE);
        strncat_s(search, MAX_PATH, pattern, _TRUNCATE);
        WIN32_FIND_DATAA found{};
        const HANDLE find = FindFirstFileA(search, &found);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            char from[MAX_PATH], to[MAX_PATH];
            strncpy_s(from, root, _TRUNCATE);
            strncat_s(from, MAX_PATH, found.cFileName, _TRUNCATE);
            BuildSiblingPath(found.cFileName, to);
            if (std::strcmp(from, to) != 0) MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
        } while (FindNextFileA(find, &found));
        FindClose(find);
    }
}

// (v0.9.513) A crash file from before renamed after its last write time (ROTTRFramework_crash_<yyyymmdd-hhmmss>.<ext>,
// beside it), and only the five newest of those kept (the names sort by time).
void KeepPreviousCrashFile(const char* path, const char* ext) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!path[0] || !GetFileAttributesExA(path, GetFileExInfoStandard, &data)) return;
    FILETIME local{};
    SYSTEMTIME t{};
    FileTimeToLocalFileTime(&data.ftLastWriteTime, &local);
    FileTimeToSystemTime(&local, &t);
    char stamp[64];
    std::snprintf(stamp, sizeof(stamp), "ROTTRFramework_crash_%04u%02u%02u-%02u%02u%02u.%s", t.wYear, t.wMonth, t.wDay, t.wHour,
                  t.wMinute, t.wSecond, ext);
    char kept[MAX_PATH];
    BuildSiblingPath(stamp, kept);
    if (!MoveFileExA(path, kept, MOVEFILE_REPLACE_EXISTING)) DeleteFileA(path);
    // the older ones beyond five
    char pattern[64];
    std::snprintf(pattern, sizeof(pattern), "ROTTRFramework_crash_*.%s", ext);
    char search[MAX_PATH];
    BuildSiblingPath(pattern, search);
    std::vector<std::string> names;   // (at start-up, before any crash: the heap is fine here)
    WIN32_FIND_DATAA found{};
    const HANDLE find = FindFirstFileA(search, &found);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.emplace_back(found.cFileName);
    } while (FindNextFileA(find, &found));
    FindClose(find);
    std::sort(names.begin(), names.end(), std::greater<std::string>());   // (newest first)
    for (std::size_t i = 5; i < names.size(); ++i) {
        char old[MAX_PATH];
        BuildSiblingPath(names[i].c_str(), old);
        DeleteFileA(old);
    }
}

void WriteBytes(HANDLE file, const char* text, std::size_t len) {
    if (file == INVALID_HANDLE_VALUE || !text || !len) return;
    while (len) {
        const DWORD chunk = static_cast<DWORD>(len > 0x7FFFFFFFu ? 0x7FFFFFFFu : len);
        DWORD written = 0;
        if (!WriteFile(file, text, chunk, &written, nullptr) || written == 0) return;
        text += written;
        len -= written;
    }
}

void WriteText(HANDLE file, const char* text) {
    if (!text) return;
    WriteBytes(file, text, std::strlen(text));
}

void WriteLine(HANDLE file, const char* text) {
    WriteText(file, text ? text : "");
    WriteText(file, "\r\n");
}

void WriteFmt(HANDLE file, const char* fmt, ...) {
    char buffer[1024]{};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, ap);
    va_end(ap);
    WriteLine(file, buffer);
}

bool ReadBreadcrumb(char (&out)[384]) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::uint32_t before = g_breadcrumbSequence.load(std::memory_order_acquire);
        if (before & 1u) continue;
        std::memcpy(out, g_breadcrumb, sizeof(out));
        out[sizeof(out) - 1] = '\0';
        const std::uint32_t after = g_breadcrumbSequence.load(std::memory_order_acquire);
        if (before == after && !(after & 1u)) return true;
    }
    strncpy_s(out, "<breadcrumb update in progress>", _TRUNCATE);
    return false;
}

const char* ExceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:   return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:              return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:   return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:      return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION:     return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:           return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:      return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_PRIV_INSTRUCTION:        return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW:          return "EXCEPTION_STACK_OVERFLOW";
    default:                                return "UNKNOWN_EXCEPTION";
    }
}

void DescribeAddress(HANDLE file, const char* label, const void* address) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!address || VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi) || !mbi.AllocationBase) {
        WriteFmt(file, "%s: %p (module unavailable)", label, address);
        return;
    }

    const auto module = reinterpret_cast<HMODULE>(mbi.AllocationBase);
    char path[MAX_PATH]{};
    const DWORD len = GetModuleFileNameA(module, path, MAX_PATH);
    const auto rva = reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module);
    if (len && len < MAX_PATH)
        WriteFmt(file, "%s: %p | module=%s | RVA=0x%llX", label, address, path, static_cast<unsigned long long>(rva));
    else
        WriteFmt(file, "%s: %p | moduleBase=%p | RVA=0x%llX", label, address, module, static_cast<unsigned long long>(rva));
}

void WriteExceptionDetails(HANDLE file, EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord) {
        WriteLine(file, "Exception information unavailable.");
        return;
    }

    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    WriteFmt(file, "Exception: 0x%08lX (%s)", record->ExceptionCode, ExceptionName(record->ExceptionCode));
    WriteFmt(file, "Flags: 0x%08lX", record->ExceptionFlags);
    WriteFmt(file, "Thread ID: %lu", GetCurrentThreadId());
    DescribeAddress(file, "Exception address", record->ExceptionAddress);

    if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        record->NumberParameters >= 2) {
        const ULONG_PTR op = record->ExceptionInformation[0];
        const char* operation = op == 0 ? "read" : (op == 1 ? "write" : (op == 8 ? "execute" : "unknown"));
        WriteFmt(file, "Memory access: %s at %p", operation, reinterpret_cast<void*>(record->ExceptionInformation[1]));
        if (record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR && record->NumberParameters >= 3)
            WriteFmt(file, "In-page NTSTATUS: 0x%08llX", static_cast<unsigned long long>(record->ExceptionInformation[2]));
    }

#if defined(_M_X64)
    if (info->ContextRecord) {
        const CONTEXT* c = info->ContextRecord;
        WriteFmt(file, "RIP=%016llX RSP=%016llX RBP=%016llX",
                 static_cast<unsigned long long>(c->Rip),
                 static_cast<unsigned long long>(c->Rsp),
                 static_cast<unsigned long long>(c->Rbp));
        WriteFmt(file, "RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX",
                 static_cast<unsigned long long>(c->Rax),
                 static_cast<unsigned long long>(c->Rbx),
                 static_cast<unsigned long long>(c->Rcx),
                 static_cast<unsigned long long>(c->Rdx));
        WriteFmt(file, "RSI=%016llX RDI=%016llX R8 =%016llX R9 =%016llX",
                 static_cast<unsigned long long>(c->Rsi),
                 static_cast<unsigned long long>(c->Rdi),
                 static_cast<unsigned long long>(c->R8),
                 static_cast<unsigned long long>(c->R9));
        WriteFmt(file, "R10=%016llX R11=%016llX R12=%016llX R13=%016llX R14=%016llX R15=%016llX",
                 static_cast<unsigned long long>(c->R10),
                 static_cast<unsigned long long>(c->R11),
                 static_cast<unsigned long long>(c->R12),
                 static_cast<unsigned long long>(c->R13),
                 static_cast<unsigned long long>(c->R14),
                 static_cast<unsigned long long>(c->R15));
    }
#endif
}

bool SafeReadQword(std::uint64_t address, std::uint64_t& value) {
    __try {
        value = *reinterpret_cast<const volatile std::uint64_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Hex dump of the heap memory register values point into (the minidump carries no heap), with
// qwords that point into ROTTR.exe annotated by RVA so neighbouring objects can be identified by
// their vtables.
void WriteRegisterMemory(HANDLE file, EXCEPTION_POINTERS* info) {
#if defined(_M_X64)
    if (!info || !info->ContextRecord) return;
    const CONTEXT* c = info->ContextRecord;
    const struct { const char* name; std::uint64_t value; } regs[] = {
        {"RBX", c->Rbx}, {"RCX", c->Rcx}, {"RDX", c->Rdx}, {"RSI", c->Rsi}, {"RDI", c->Rdi},
        {"RBP", c->Rbp}, {"R8", c->R8}, {"R9", c->R9}, {"R10", c->R10}, {"R11", c->R11},
        {"R12", c->R12}, {"R14", c->R14}, {"R15", c->R15},
    };
    const auto image = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
    std::uint64_t imageSize = 0;
    if (image) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
        imageSize = nt->OptionalHeader.SizeOfImage;
    }
    std::uint64_t dumped[16]{};
    int dumpedCount = 0;
    for (const auto& reg : regs) {
        if (reg.value < 0x10000ull || reg.value >= 0x00007FF000000000ull) continue;
        if (image && reg.value >= image && reg.value < image + imageSize) continue;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(reg.value), &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT || mbi.Type == MEM_IMAGE)
            continue;
        const std::uint64_t start = (reg.value & ~0xFull) - 0x100ull;
        bool overlaps = false;
        for (int i = 0; i < dumpedCount; ++i)
            if (start < dumped[i] + 0x200ull && dumped[i] < start + 0x200ull) overlaps = true;
        if (overlaps || dumpedCount >= 16) continue;
        dumped[dumpedCount++] = start;
        WriteFmt(file, "Memory around %s=%016llX:", reg.name, static_cast<unsigned long long>(reg.value));
        char notes[512]{};
        int notesLength = 0;
        for (std::uint64_t p = start; p < start + 0x200ull; p += 0x20ull) {
            char line[160]{};
            int n = sprintf_s(line, "  %016llX:", static_cast<unsigned long long>(p));
            for (int i = 0; i < 4; ++i) {
                std::uint64_t q = 0;
                if (SafeReadQword(p + i * 8ull, q)) {
                    n += sprintf_s(line + n, sizeof(line) - n, " %016llX", static_cast<unsigned long long>(q));
                    if (image && q >= image && q < image + imageSize && notesLength < 440)
                        notesLength += sprintf_s(notes + notesLength, sizeof(notes) - notesLength, " [%016llX]=ROTTR+0x%llX",
                                                 static_cast<unsigned long long>(p + i * 8ull),
                                                 static_cast<unsigned long long>(q - image));
                } else {
                    n += sprintf_s(line + n, sizeof(line) - n, " ????????????????");
                }
            }
            WriteLine(file, line);
        }
        if (notesLength) WriteFmt(file, "  image pointers:%s", notes);
    }
#else
    (void)file;
    (void)info;
#endif
}

void WriteMiniDump(HANDLE reportFile, EXCEPTION_POINTERS* info) {
    if (!g_miniDumpWriteDump) {
        WriteLine(reportFile, "Minidump: unavailable (DbgHelp/MiniDumpWriteDump not loaded)");
        return;
    }

    HANDLE dump = CreateFileA(g_dumpPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump == INVALID_HANDLE_VALUE) {
        WriteFmt(reportFile, "Minidump: CreateFile failed (Win32=%lu)", GetLastError());
        return;
    }

    MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
    exceptionInfo.ThreadId = GetCurrentThreadId();
    exceptionInfo.ExceptionPointers = info;
    exceptionInfo.ClientPointers = FALSE;

    // Indirectly referenced memory: the pages that registers and stack slots point into (heap
    // blocks around a corrupted allocator node), which a plain minidump leaves out.
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithDataSegs |
        MiniDumpWithThreadInfo |
        MiniDumpWithIndirectlyReferencedMemory);

    SetLastError(ERROR_SUCCESS);
    BOOL ok = g_miniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
                                  type, info ? &exceptionInfo : nullptr, nullptr, nullptr);
    DWORD error = GetLastError();

    
    
    if (!ok && error == ERROR_INVALID_PARAMETER) {
        SetFilePointer(dump, 0, nullptr, FILE_BEGIN);
        SetEndOfFile(dump);
        SetLastError(ERROR_SUCCESS);
        ok = g_miniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
                                 MiniDumpNormal, info ? &exceptionInfo : nullptr, nullptr, nullptr);
        error = GetLastError();
    }
    CloseHandle(dump);

    if (ok)
        WriteFmt(reportFile, "Minidump: %s", g_dumpPath);
    else
        WriteFmt(reportFile, "Minidump: MiniDumpWriteDump failed (Win32=%lu)", error);
}

void WriteCrashReport(EXCEPTION_POINTERS* info) {
    if (InterlockedCompareExchange(&g_reportInProgress, 1, 0) != 0) return;

    HANDLE report = CreateFileA(g_crashLogPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (report != INVALID_HANDLE_VALUE) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        WriteLine(report, "ROTTRFramework crash report");
        WriteFmt(report, "Time: %04u-%02u-%02u %02u:%02u:%02u.%03u",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        WriteFmt(report, "Process ID: %lu", GetCurrentProcessId());
        DescribeAddress(report, "ROTTR module", GetModuleHandleW(nullptr));
        DescribeAddress(report, "Framework module", g_trainerModule);

        char breadcrumb[384]{};
        ReadBreadcrumb(breadcrumb);
        WriteFmt(report, "Last framework breadcrumb: %s", breadcrumb[0] ? breadcrumb : "<none>");
        WriteLine(report, "");
        WriteExceptionDetails(report, info);
        WriteLine(report, "");
        if (g_lastFault[0]) {
            WriteFmt(report, "Last fault before this (it may have been caught by the game): %s", g_lastFault);
            WriteLine(report, "");
        }
        WriteRegisterMemory(report, info);
        WriteLine(report, "");
        WriteMiniDump(report, info);
        FlushFileBuffers(report);
        CloseHandle(report);
    }

    InterlockedExchange(&g_reportInProgress, 0);
}

// ---- first-chance exceptions ----------------------------------------------------------------------------------------
// The game catches some exceptions itself (e.g. ThreadedFileSystem::ReceiveDataChecked turns any exception while
// processing loaded data into "Error while reading game data" and a deliberate breakpoint), so the crash report only
// sees the breakpoint. The last real fault outside the framework (whose guarded reads fault on purpose) is kept here,
// written to the log (the first few), and added to the crash report.
volatile LONG g_faultsLogged = 0;
volatile LONG g_inFaultHandler = 0;

bool IsRealFault(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_IN_PAGE_ERROR: case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_STACK_OVERFLOW: case 0xC0000409u /* stack buffer overrun */:
        return true;
    default:
        return false;
    }
}

int FormatModuleAddress(char* out, std::size_t size, std::uint64_t address) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.AllocationBase &&
        mbi.Type == MEM_IMAGE) {
        char path[MAX_PATH]{};
        if (GetModuleFileNameA(reinterpret_cast<HMODULE>(mbi.AllocationBase), path, MAX_PATH)) {
            const char* name = path;
            for (const char* p = path; *p; ++p)
                if (*p == '\\' || *p == '/') name = p + 1;
            return std::snprintf(out, size, "%s+0x%llX", name,
                                 static_cast<unsigned long long>(address - reinterpret_cast<std::uint64_t>(mbi.AllocationBase)));
        }
    }
    return std::snprintf(out, size, "%016llX", static_cast<unsigned long long>(address));
}

LONG WINAPI FirstChanceHandler(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    if (!IsRealFault(record->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    const auto address = reinterpret_cast<std::uint64_t>(record->ExceptionAddress);
    if (address >= g_trainerBegin && address < g_trainerEnd)
        return EXCEPTION_CONTINUE_SEARCH;   // the framework's own guarded reads (MemoryProbe's included)
    if (InterlockedCompareExchange(&g_inFaultHandler, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;

    char text[sizeof(g_lastFault)];
    std::size_t at = 0;
    auto add = [&](int written) {
        if (written > 0) at = std::min(sizeof(text) - 1, at + static_cast<std::size_t>(written));
    };
    add(std::snprintf(text + at, sizeof(text) - at, "first-chance 0x%08lX (%s) thread %lu at ", record->ExceptionCode,
                      ExceptionName(record->ExceptionCode), GetCurrentThreadId()));
    add(FormatModuleAddress(text + at, sizeof(text) - at, address));
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        add(std::snprintf(text + at, sizeof(text) - at, " (%s %016llX)",
                          record->ExceptionInformation[0] == 1 ? "write" : record->ExceptionInformation[0] == 8 ? "execute" : "read",
                          static_cast<unsigned long long>(record->ExceptionInformation[1])));
#if defined(_M_X64)
    const CONTEXT* c = info->ContextRecord;
    add(std::snprintf(text + at, sizeof(text) - at,
                      "; RAX=%llX RBX=%llX RCX=%llX RDX=%llX RSI=%llX RDI=%llX RBP=%llX RSP=%llX R8=%llX R9=%llX "
                      "R10=%llX R11=%llX R12=%llX R13=%llX R14=%llX R15=%llX; stack:",
                      c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp, c->R8, c->R9, c->R10, c->R11,
                      c->R12, c->R13, c->R14, c->R15));
    // Return addresses on the stack (any module): a rough call chain.
    int found = 0;
    for (std::uint64_t slot = c->Rsp; slot < c->Rsp + 0x1000 && found < 16; slot += 8) {
        std::uint64_t value = 0;
        if (!SafeReadQword(slot, value)) break;
        MEMORY_BASIC_INFORMATION vm{};
        if (value < 0x10000 || VirtualQuery(reinterpret_cast<const void*>(value), &vm, sizeof(vm)) != sizeof(vm) ||
            vm.Type != MEM_IMAGE || !(vm.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE | PAGE_EXECUTE_READWRITE)))
            continue;
        add(std::snprintf(text + at, sizeof(text) - at, " "));
        add(FormatModuleAddress(text + at, sizeof(text) - at, value));
        ++found;
    }
#endif
    text[at] = '\0';
    std::memcpy(g_lastFault, text, at + 1);
    if (InterlockedIncrement(&g_faultsLogged) <= 25) Log::Writef("Crash diagnostics: %s", text);
    InterlockedExchange(&g_inFaultHandler, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI TrainerUnhandledExceptionFilter(EXCEPTION_POINTERS* info) {
    WriteCrashReport(info);

    
    
    if (g_previousUnhandledFilter && g_previousUnhandledFilter != &TrainerUnhandledExceptionFilter)
        return g_previousUnhandledFilter(info);
    return EXCEPTION_CONTINUE_SEARCH;
}
}

namespace CrashDiagnostics {
void Install(HMODULE trainerModule) {
    if (g_installed.exchange(true, std::memory_order_acq_rel)) return;

    g_trainerModule = trainerModule;
    if (trainerModule) {
        const auto* base = reinterpret_cast<const std::uint8_t*>(trainerModule);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        g_trainerBegin = reinterpret_cast<std::uint64_t>(base);
        g_trainerEnd = g_trainerBegin + nt->OptionalHeader.SizeOfImage;
    }
    MoveOldCrashFiles();   // (v0.9.591)
    BuildSiblingPath("ROTTRFramework_crash.log", g_crashLogPath);
    BuildSiblingPath("ROTTRFramework_crash.dmp", g_dumpPath);

    // (v0.9.513) The last crash's report and dump kept: they were deleted at every start, so a crash's files were gone
    // once the game was started again (the user's rope crash). Each is renamed after its time
    // (ROTTRFramework_crash_<yyyymmdd-hhmmss>.log / .dmp), the five newest of each kept.
    KeepPreviousCrashFile(g_crashLogPath, "log");
    KeepPreviousCrashFile(g_dumpPath, "dmp");

    g_dbgHelpModule = LoadLibraryW(L"dbghelp.dll");
    if (g_dbgHelpModule) {
        g_miniDumpWriteDump = reinterpret_cast<MiniDumpWriteDumpFn>(
            GetProcAddress(g_dbgHelpModule, "MiniDumpWriteDump"));
    }

    g_previousUnhandledFilter = SetUnhandledExceptionFilter(&TrainerUnhandledExceptionFilter);
    AddVectoredExceptionHandler(0, &FirstChanceHandler);
    SetBreadcrumb("framework initialization");
    Log::Detailf("Crash diagnostics: armed (log=%s dump=%s MiniDumpWriteDump=%s)",
                g_crashLogPath, g_dumpPath, g_miniDumpWriteDump ? "available" : "unavailable");
}

void SetBreadcrumb(const char* text) {
    g_breadcrumbSequence.fetch_add(1u, std::memory_order_acq_rel);
    if (text)
        strncpy_s(g_breadcrumb, text, _TRUNCATE);
    else
        g_breadcrumb[0] = '\0';
    g_breadcrumbSequence.fetch_add(1u, std::memory_order_release);
}

void SetBreadcrumbf(const char* fmt, ...) {
    char buffer[384]{};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, ap);
    va_end(ap);
    SetBreadcrumb(buffer);
}
}
