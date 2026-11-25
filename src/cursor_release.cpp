#include "cursor_release.h"
#include "game_build.h"
#include "log.h"
#include <Windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>

// See cursor_release.h. nx::NxGameStateImpl::UpdateCursor(state, hwnd, clipAllowed) (retail 0xC6BB30, decompiled in
// re\levels\retail_cursor.c): picks the wanted visibility / confinement from the state's cursor requests, calls
// ShowCursor until the count flips, ClipCursor(client rect or null) every call, and keeps what it did at state +0x90
// (shown) and +0x91 (confined). Skipping it while the cursor is released and writing "shown, not confined" there makes
// the game hide and confine it again by itself once it is not released any more.
namespace CursorRelease {
namespace {
constexpr std::uintptr_t kRvaUpdateCursor = 0x00C6BB30ull;
constexpr std::size_t kStateShown = 0x90, kStateClipped = 0x91;
// prologue up to "mov rax, [__security_cookie]" (its rip-relative operand is not compared)
constexpr std::uint8_t kUpdateCursorSig[] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0x05,
};
constexpr std::uint8_t kUpdateCursorArgsSig[] = {0x45, 0x0F, 0xB6, 0xF8, 0x4C, 0x8B, 0xF2, 0x48, 0x8B, 0xF9};  // +0x1F

using UpdateCursorFn = void(__fastcall*)(std::uint8_t* state, HWND hwnd, char clipAllowed);
UpdateCursorFn g_origUpdateCursor = nullptr;
std::atomic<int> g_cursorHolds{0};      // framework windows of its own (a Save As dialog) that need the cursor
std::atomic_bool g_released{false};   // what the hook did last: the cursor was left free

bool GameInFront(HWND hwnd) {
    HWND front = GetForegroundWindow();
    if (!front) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(front, &pid);
    return pid == GetCurrentProcessId() || front == hwnd;
}

void __fastcall HookUpdateCursor(std::uint8_t* state, HWND hwnd, char clipAllowed) {
    const bool release = hwnd && (g_cursorHolds.load(std::memory_order_relaxed) > 0 || !GameInFront(hwnd));
    if (!release) {
        g_released.store(false, std::memory_order_relaxed);
        g_origUpdateCursor(state, hwnd, clipAllowed);
        return;
    }
    if (!g_released.exchange(true, std::memory_order_relaxed) || (state && state[kStateClipped])) ClipCursor(nullptr);
    if (state && !state[kStateShown]) {
        int guard = 0;
        while (ShowCursor(TRUE) < 0 && ++guard < 64) {
        }
        state[kStateShown] = 1;
    }
    if (state) state[kStateClipped] = 0;
}
}  // namespace

bool Initialize() {
    if (!GameBuild::IsSupportedRetail()) return false;
    if (!GameBuild::VerifyBytes(kRvaUpdateCursor, kUpdateCursorSig, sizeof(kUpdateCursorSig)) ||
        !GameBuild::VerifyBytes(kRvaUpdateCursor + 0x1F, kUpdateCursorArgsSig, sizeof(kUpdateCursorArgsSig))) {
        Log::Write("CursorRelease: nx::NxGameStateImpl::UpdateCursor signature mismatch: the cursor stays the game's");
        return false;
    }
    void* target = reinterpret_cast<void*>(GameBuild::Base() + kRvaUpdateCursor);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookUpdateCursor), reinterpret_cast<void**>(&g_origUpdateCursor)) != MH_OK) {
        Log::Write("CursorRelease: the UpdateCursor hook could not be created");
        return false;
    }
    Log::Detail("CursorRelease: ready (the cursor is free while another window is in front or a Save As dialog is open)");
    return true;
}

void HoldCursor(bool on) {
    const int holds = on ? g_cursorHolds.fetch_add(1, std::memory_order_relaxed) + 1
                         : g_cursorHolds.fetch_sub(1, std::memory_order_relaxed) - 1;
    if (holds < 0) g_cursorHolds.store(0, std::memory_order_relaxed);
}
}  // namespace CursorRelease
