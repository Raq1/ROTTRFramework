#include "log.h"
#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {
std::mutex g_mutex;
char g_path[MAX_PATH]{};
// -1 until read from ROTTRFramework.ini [Log] Verbose (next to the exe, like the log).
std::atomic<int> g_verbose{-1};
HANDLE g_logHandle = INVALID_HANDLE_VALUE;

void BuildPath() {
    if (g_path[0]) return;
    GetModuleFileNameA(nullptr, g_path, MAX_PATH);
    char* slash = g_path;
    for (char* p = g_path; *p; ++p) {
        if (*p == '\\' || *p == '/') slash = p + 1;
    }
    *slash = '\0';
    lstrcatA(g_path, "ROTTRFramework.log");
}

void CloseLogHandle() {
    if (g_logHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_logHandle);
        g_logHandle = INVALID_HANDLE_VALUE;
    }
}

int ReadVerbose() {
    char ini[MAX_PATH]{};
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    char* slash = ini;
    for (char* p = ini; *p; ++p) {
        if (*p == '\\' || *p == '/') slash = p + 1;
    }
    *slash = '\0';
    lstrcatA(ini, "ROTTRFramework.ini");
    return GetPrivateProfileIntA("Log", "Verbose", 0, ini) != 0 ? 1 : 0;
}

bool EnsureLogHandle() {
    if (g_logHandle != INVALID_HANDLE_VALUE) return true;
    BuildPath();
    g_logHandle = CreateFileA(g_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return g_logHandle != INVALID_HANDLE_VALUE;
}

// The line itself, as it is (every way of writing comes here).
void Emit(std::string_view text) {
    std::scoped_lock lock(g_mutex);
    if (!EnsureLogHandle()) return;
    DWORD written = 0;
    WriteFile(g_logHandle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    static constexpr char nl[] = "\r\n";
    WriteFile(g_logHandle, nl, 2, &written, nullptr);
}

// (v1.0.0) Without [Log] Verbose each kind of line (a Writef's format string; a Write's text, its first 40 characters
// with the digits left out) is written kLinesPerKind times in a session at most, then one line says that the rest are
// left out: no call site that runs every tick, every stream event or every load fills the log. Verbose writes all.
constexpr std::uint32_t kLinesPerKind = 25;
std::mutex g_kindMutex;
std::unordered_map<std::uint64_t, std::uint32_t> g_kindCounts;

std::uint64_t TextKind(std::string_view text) {
    std::uint64_t hash = 1469598103934665603ull;
    std::size_t used = 0;
    for (const char c : text) {
        if (c >= '0' && c <= '9') continue;
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
        if (++used >= 40) break;
    }
    return hash | 1ull;   // odd: never the address of a format string
}

std::uint64_t FormatKind(const char* fmt) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(fmt)) & ~1ull;
}

// True: write the line. The first line past the budget is replaced by the note.
bool WithinBudget(std::uint64_t kind, std::string_view text) {
    std::uint32_t count = 0;
    {
        std::scoped_lock lock(g_kindMutex);
        count = ++g_kindCounts[kind];
    }
    if (count <= kLinesPerKind) return true;
    if (count == kLinesPerKind + 1) {
        const std::string start(text.substr(0, 60));
        char note[256]{};
        std::snprintf(note, sizeof(note),
                      "Log: lines like [%s%s] written %u times: the rest left out ([Log] Verbose=1 in ROTTRFramework.ini "
                      "writes every one)",
                      start.c_str(), text.size() > 60 ? "..." : "", kLinesPerKind);
        Emit(note);
    }
    return false;
}

void Format(char (&buf)[1024], const char* fmt, va_list ap) {
    vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
}
}  // namespace

namespace Log {
void Init() {
    g_verbose.store(ReadVerbose(), std::memory_order_relaxed);
    std::scoped_lock lock(g_mutex);
    CloseLogHandle();
    BuildPath();
    HANDLE h = CreateFileA(g_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

bool Verbose() {
    int v = g_verbose.load(std::memory_order_relaxed);
    if (v < 0) {
        v = ReadVerbose();
        g_verbose.store(v, std::memory_order_relaxed);
    }
    return v != 0;
}

void Write(std::string_view text) {
    if (!Verbose() && !WithinBudget(TextKind(text), text)) return;   // (v1.0.0)
    Emit(text);
}

void Detail(std::string_view text) {
    if (Verbose()) Emit(text);
}

void Detailf(const char* fmt, ...) {
    if (!Verbose()) return;
    char buf[1024]{};
    va_list ap;
    va_start(ap, fmt);
    Format(buf, fmt, ap);
    va_end(ap);
    Emit(buf);
}

void DetailOncef(const char* fmt, ...) {
    if (!Verbose()) return;
    {
        static std::mutex s_mutex;
        static std::unordered_set<const void*> s_seen;
        std::scoped_lock lock(s_mutex);
        if (!s_seen.insert(fmt).second) return;
    }
    char buf[1024]{};
    va_list ap;
    va_start(ap, fmt);
    Format(buf, fmt, ap);
    va_end(ap);
    Emit(buf);
}

void Writef(const char* fmt, ...) {
    char buf[1024]{};
    va_list ap;
    va_start(ap, fmt);
    Format(buf, fmt, ap);
    va_end(ap);
    if (!Verbose() && !WithinBudget(FormatKind(fmt), buf)) return;   // (v1.0.0)
    Emit(buf);
}

void WriteOncef(const char* fmt, ...) {
    {
        static std::mutex s_mutex;
        static std::unordered_set<const void*> s_seen;
        std::scoped_lock lock(s_mutex);
        if (!s_seen.insert(fmt).second) return;
    }
    char buf[1024]{};
    va_list ap;
    va_start(ap, fmt);
    Format(buf, fmt, ap);
    va_end(ap);
    Emit(buf);
}
}  // namespace Log
