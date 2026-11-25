
















using u32 = unsigned int;
using usize = unsigned long long;
using i32 = int;

#define WINAPI __stdcall
#define DLL_PROCESS_ATTACH 1u
#define TRUE_VALUE 1
#define MAX_PATH_ 260u
#define PAGE_READWRITE_ 0x04u
#define PAGE_EXECUTE_READWRITE_ 0x40u
#define MEM_COMMIT_ 0x00001000u
#define MEM_RESERVE_ 0x00002000u

using HANDLE_ = void*;
using HMODULE_ = void*;
using FARPROC_ = void*;
using BOOL_ = int;
using DWORD_ = u32;
using ThreadProc_ = DWORD_ (WINAPI*)(void*);
using ReportFaultFn_ = i32 (WINAPI*)(void*, DWORD_);

extern "C" int _fltused = 0;



extern "C" void* memcpy(void* dst, const void* src, usize n) {
    auto* d = static_cast<volatile unsigned char*>(dst);
    auto* s = static_cast<const volatile unsigned char*>(src);
    for (usize i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}
extern "C" void* memset(void* dst, int value, usize n) {
    auto* p = static_cast<volatile unsigned char*>(dst);
    const unsigned char v = static_cast<unsigned char>(value);
    for (usize i = 0; i < n; ++i) p[i] = v;
    return dst;
}

extern "C" {
__declspec(dllimport) HMODULE_ WINAPI LoadLibraryW(const wchar_t* path);
__declspec(dllimport) HMODULE_ WINAPI GetModuleHandleW(const wchar_t* name);
__declspec(dllimport) FARPROC_ WINAPI GetProcAddress(HMODULE_ module, const char* name);
__declspec(dllimport) DWORD_ WINAPI GetSystemDirectoryW(wchar_t* buffer, DWORD_ size);
__declspec(dllimport) DWORD_ WINAPI GetModuleFileNameW(HMODULE_ module, wchar_t* path, DWORD_ size);
__declspec(dllimport) BOOL_ WINAPI DisableThreadLibraryCalls(HMODULE_ module);
__declspec(dllimport) HANDLE_ WINAPI CreateThread(void*, usize, ThreadProc_, void*, DWORD_, DWORD_*);
__declspec(dllimport) BOOL_ WINAPI CloseHandle(HANDLE_ handle);
__declspec(dllimport) int WINAPI GetPrivateProfileIntW(const wchar_t*, const wchar_t*, int, const wchar_t*);
__declspec(dllimport) BOOL_ WINAPI VirtualProtect(void*, usize, DWORD_, DWORD_*);
__declspec(dllimport) void* WINAPI VirtualAlloc(void*, usize, DWORD_, DWORD_);
__declspec(dllimport) BOOL_ WINAPI FlushInstructionCache(HANDLE_, const void*, usize);
__declspec(dllimport) HANDLE_ WINAPI GetCurrentProcess();
__declspec(dllimport) unsigned long long WINAPI GetTickCount64();
__declspec(dllimport) DWORD_ WINAPI GetCurrentThreadId();
}

namespace {
HMODULE_ g_self = nullptr;
HMODULE_ g_realFaultrep = nullptr;
ReportFaultFn_ g_reportFault = nullptr;




bool AppendWide(wchar_t* dst, u32 capacity, const wchar_t* suffix) {
    u32 n = 0;
    while (n < capacity && dst[n]) ++n;
    if (n >= capacity) return false;
    for (u32 i = 0; suffix[i]; ++i) {
        if (n + 1u >= capacity) return false;
        dst[n++] = suffix[i];
    }
    dst[n] = 0;
    return true;
}

bool ReplaceFilename(wchar_t* path, u32 capacity, const wchar_t* filename) {
    u32 n = 0;
    while (n < capacity && path[n]) ++n;
    if (n >= capacity) return false;
    i32 slash = -1;
    for (u32 i = 0; i < n; ++i)
        if (path[i] == L'\\' || path[i] == L'/') slash = static_cast<i32>(i);
    u32 pos = slash >= 0 ? static_cast<u32>(slash + 1) : 0u;
    for (u32 i = 0; filename[i]; ++i) {
        if (pos + 1u >= capacity) return false;
        path[pos++] = filename[i];
    }
    path[pos] = 0;
    return true;
}

bool BytesEqual(const unsigned char* a, const unsigned char* b, usize n) {
    if (!a || !b) return false;
    for (usize i = 0; i < n; ++i)
        if (a[i] != b[i]) return false;
    return true;
}

bool WritePointerSlot(void** slot, void* value) {
    if (!slot) return false;
    DWORD_ oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE_, &oldProtect)) return false;
    *slot = value;
    DWORD_ ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

void WriteAbsJump(unsigned char* dst, const void* target) {
    dst[0] = 0xFF; dst[1] = 0x25;
    dst[2] = 0; dst[3] = 0; dst[4] = 0; dst[5] = 0;
    *reinterpret_cast<usize*>(dst + 6) = reinterpret_cast<usize>(target);
}

bool LoadRealFaultrep() {
    if (g_reportFault) return true;
    wchar_t path[MAX_PATH_]{};
    const DWORD_ n = GetSystemDirectoryW(path, MAX_PATH_);
    if (!n || n >= MAX_PATH_ || !AppendWide(path, MAX_PATH_, L"\\faultrep.dll")) return false;
    g_realFaultrep = LoadLibraryW(path);
    if (!g_realFaultrep) return false;
    g_reportFault = reinterpret_cast<ReportFaultFn_>(GetProcAddress(g_realFaultrep, "ReportFault"));
    return g_reportFault != nullptr;
}





volatile bool g_skipRequested = false;
volatile bool g_legalProcessArmed = false;
volatile bool g_legalProcessFired = false;
volatile bool g_legalProcessSignatureMismatch = false;
volatile bool g_legalVisualSuppressionActive = false;
volatile bool g_legalExitWatchArmed = false;
volatile bool g_legalExitWatchFired = false;
volatile bool g_legalExitWatchSignatureMismatch = false;
volatile bool g_legalGotoMainMenuSignatureMismatch = false;
volatile bool g_legalForcedNativeExit = false;



volatile unsigned long long g_timingLegalFirstProcessMs = 0;
volatile unsigned long long g_timingForcedGotoMs = 0;
volatile unsigned long long g_timingProfileLoadRequestMs = 0;
volatile unsigned long long g_timingLegalPreExitMs = 0;
volatile unsigned long long g_timingLegalOnExitMs = 0;
volatile unsigned long long g_timingProfileLoadOnEnterMs = 0;
volatile unsigned long long g_timingGotoReturnedMs = 0;
// v0.9.297 startup timing (RottrStartupTimingQuery 7..10)
volatile unsigned long long g_timingAttachMs = 0;
volatile unsigned long long g_timingLegalOnEnterMs = 0;
volatile unsigned long long g_timingProfileLoadOnExitMs = 0;
volatile unsigned long long g_timingStartupUiReleaseMs = 0;

constexpr usize kRvaUiMenuStateLegalProcess = 0x0093EDA0ull;
constexpr usize kUiMenuStateLegalTimerOffset = 0x150ull;
constexpr usize kLegalProcessPrologueSize = 14ull;
constexpr unsigned char kLegalProcessPrologue[kLegalProcessPrologueSize] = {
    0x40,0x57,0x48,0x83,0xEC,0x20,0xF3,0x0F,0x10,0x91,0x50,0x01,0x00,0x00
};
using LegalProcessFn_ = void(__fastcall*)(void*, float);
LegalProcessFn_ g_legalProcessTrampoline = nullptr;
unsigned char* g_legalProcessTarget = nullptr;

constexpr usize kRvaUiMenuStateLegalGotoMainMenu = 0x008EECF0ull;
constexpr usize kLegalGotoMainMenuSigSize = 16ull;
constexpr unsigned char kLegalGotoMainMenuSig[kLegalGotoMainMenuSigSize] = {
    0x40,0x53,0x48,0x83,0xEC,0x20,0xE8,0xA5,0x2A,0xC9,0xFF,0xBA,0x02,0x00,0x00,0x00
};
using LegalGotoMainMenuFn_ = void(__fastcall*)(void*);
LegalGotoMainMenuFn_ g_legalGotoMainMenu = nullptr;

constexpr usize kRvaUiMenuStateLegalVtable = 0x00DB4448ull;
constexpr usize kUiMenuStateLegalTransitionSlot = 8ull;
constexpr usize kRvaUiMenuStateLegalTransition = 0x00913ED0ull;
using LegalTransitionFn_ = bool(__fastcall*)(void*, const void*, const int*);
LegalTransitionFn_ g_originalLegalTransition = nullptr;
void** g_legalTransitionSlot = nullptr;

constexpr usize kUiMenuStateLegalPreExitSlot = 7ull;
constexpr usize kRvaUiMenuStateLegalPreExit = 0x008BBE10ull;
using LegalPreExitFn_ = void(__fastcall*)(void*, const void*, const int*, float);
LegalPreExitFn_ g_originalLegalPreExit = nullptr;
void** g_legalPreExitSlot = nullptr;
volatile bool g_legalPreExitWatchArmed = false;

void RestoreLegalProcessEntry() {
    if (!g_legalProcessTarget || !g_legalProcessArmed) return;
    DWORD_ oldProtect = 0;
    if (!VirtualProtect(g_legalProcessTarget, kLegalProcessPrologueSize, PAGE_EXECUTE_READWRITE_, &oldProtect)) return;
    for (usize i = 0; i < kLegalProcessPrologueSize; ++i) g_legalProcessTarget[i] = kLegalProcessPrologue[i];
    DWORD_ ignored = 0;
    VirtualProtect(g_legalProcessTarget, kLegalProcessPrologueSize, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), g_legalProcessTarget, kLegalProcessPrologueSize);
    g_legalProcessArmed = false;
}

void RestoreLegalTransitionSlot() {
    if (!g_legalTransitionSlot || !g_legalExitWatchArmed || !g_originalLegalTransition) return;
    WritePointerSlot(g_legalTransitionSlot, reinterpret_cast<void*>(g_originalLegalTransition));
    g_legalExitWatchArmed = false;
}

void RestoreLegalPreExitSlot() {
    if (!g_legalPreExitSlot || !g_legalPreExitWatchArmed || !g_originalLegalPreExit) return;
    WritePointerSlot(g_legalPreExitSlot, reinterpret_cast<void*>(g_originalLegalPreExit));
    g_legalPreExitWatchArmed = false;
}

void __fastcall EarlyUiMenuStateLegalPreExit(void* self, const void* fromState, const int* toState, float fade) {
    if (toState && *toState == 3 && !g_timingLegalPreExitMs) {
        g_timingLegalPreExitMs = GetTickCount64();
        RestoreLegalPreExitSlot();
    }
    if (g_originalLegalPreExit) g_originalLegalPreExit(self, fromState, toState, fade);
}

bool __fastcall EarlyUiMenuStateLegalTransition(void* self, const void* fromState, const int* toState) {
    if (toState && *toState == 3) {
        if (!g_timingLegalOnExitMs) g_timingLegalOnExitMs = GetTickCount64();
        g_legalVisualSuppressionActive = false;
        g_legalExitWatchFired = true;
        RestoreLegalTransitionSlot();
    }
    return g_originalLegalTransition ? g_originalLegalTransition(self, fromState, toState) : true;
}

bool ArmLegalExitWatch(unsigned char* base) {
    if (!base) return false;
    auto** slot = reinterpret_cast<void**>(base + kRvaUiMenuStateLegalVtable + kUiMenuStateLegalTransitionSlot * sizeof(void*));
    if (*slot != reinterpret_cast<void*>(base + kRvaUiMenuStateLegalTransition)) {
        g_legalExitWatchSignatureMismatch = true;
        return false;
    }
    g_originalLegalTransition = reinterpret_cast<LegalTransitionFn_>(*slot);
    if (!WritePointerSlot(slot, reinterpret_cast<void*>(&EarlyUiMenuStateLegalTransition))) return false;
    g_legalTransitionSlot = slot;
    g_legalExitWatchArmed = true;
    return true;
}

bool ArmLegalPreExitWatch(unsigned char* base) {
    if (!base) return false;
    auto** slot = reinterpret_cast<void**>(base + kRvaUiMenuStateLegalVtable + kUiMenuStateLegalPreExitSlot * sizeof(void*));
    if (*slot != reinterpret_cast<void*>(base + kRvaUiMenuStateLegalPreExit)) return false;
    g_originalLegalPreExit = reinterpret_cast<LegalPreExitFn_>(*slot);
    if (!WritePointerSlot(slot, reinterpret_cast<void*>(&EarlyUiMenuStateLegalPreExit))) return false;
    g_legalPreExitSlot = slot;
    g_legalPreExitWatchArmed = true;
    return true;
}

// v0.9.296: the Legal (logo) screen is a Scaleform movie that was opened and drawn for a frame or two before the forced
// GotoMainMenu switch took effect, and the framework's black overlay mask comes up too late to cover it (the overlay
// only exists after the framework's startup delay). Instead, the game's own Scaleform renderer is told not to draw any
// UI layer from the moment the Legal state is entered until PROFILE_LOAD exits, so the main menu is the first UI frame.
// It uses ScaleformManager's per-layer disable masks (the same ones the framework's HUD hide uses) with a reason bit of
// its own, which only stops drawing (the movies keep updating). Released by PROFILE_LOAD's OnExit, by a 10 s safety
// timeout, or by the framework (RottrStartupUiRelease).
constexpr usize kRvaScaleformManagerGet = 0x0014A590ull;
constexpr usize kScaleformManagerGetSigSize = 15ull;
constexpr unsigned char kScaleformManagerGetSig[kScaleformManagerGetSigSize] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00
};
constexpr usize kScaleformManagerLayerMasksOffset = 0x20ull;
constexpr u32 kScaleformLayerCount = 16u;
constexpr u32 kStartupUiDisableReason = 0x40000000u;   // the framework's HUD hide uses 0x80000000
// (v0.9.339: up to the end of MOUNTING_DLC, whose synchronous DLC reads take seconds while the menu streams in)
constexpr unsigned long long kStartupUiHideSafetyMs = 20000ull;

constexpr usize kUiMenuStateLegalOnEnterAsyncSlot = 2ull;
constexpr usize kUiMenuStateLegalOnEnterSlot = 3ull;
constexpr usize kRvaUiMenuStateGenericOnEnterAsync = 0x008BB6A0ull;
constexpr usize kRvaUiMenuStateLegalOnEnter = 0x0090A8A0ull;
using ScaleformManagerGetFn_ = void*(__fastcall*)();
using LegalOnEnterFn_ = bool(__fastcall*)(void*, const void*, const void*);
ScaleformManagerGetFn_ g_scaleformManagerGet = nullptr;
LegalOnEnterFn_ g_originalLegalOnEnterAsync = nullptr;
LegalOnEnterFn_ g_originalLegalOnEnter = nullptr;
void** g_legalOnEnterAsyncSlot = nullptr;
void** g_legalOnEnterSlot = nullptr;
volatile bool g_startupUiHideArmed = false;
volatile bool g_startupUiHidden = false;
volatile bool g_startupUiHideDone = false;
volatile unsigned long long g_startupUiHiddenAtMs = 0;
// v0.9.339: PROFILE_LOAD has exited; the hide is kept until MOUNTING_DLC exits (or the framework releases it when that
// state never comes).
volatile bool g_startupUiWaitingForDlcMount = false;
volatile bool g_startupUiDlcMountEntered = false;
volatile bool g_dlcMountHooksArmed = false;

void SetStartupUiLayersDisabled(bool disabled) {
    if (!g_scaleformManagerGet) return;
    void* manager = g_scaleformManagerGet();
    if (!manager) return;
    auto* masks = reinterpret_cast<u32*>(reinterpret_cast<unsigned char*>(manager) + kScaleformManagerLayerMasksOffset);
    for (u32 layer = 0; layer < kScaleformLayerCount; ++layer) {
        if (disabled) masks[layer] |= kStartupUiDisableReason;
        else masks[layer] &= ~kStartupUiDisableReason;
    }
}

void HideStartupUi() {
    if (!g_startupUiHideArmed || g_startupUiHidden || g_startupUiHideDone) return;
    SetStartupUiLayersDisabled(true);
    g_startupUiHidden = true;
    g_startupUiHiddenAtMs = GetTickCount64();
}

void ReleaseStartupUi() {
    g_startupUiWaitingForDlcMount = false;
    if (!g_timingStartupUiReleaseMs) g_timingStartupUiReleaseMs = GetTickCount64();
    if (!g_startupUiHidden) { g_startupUiHideDone = true; return; }
    SetStartupUiLayersDisabled(false);
    g_startupUiHidden = false;
    g_startupUiHideDone = true;
}

void ReleaseStartupUiIfExpired() {
    if (g_startupUiHidden && GetTickCount64() - g_startupUiHiddenAtMs > kStartupUiHideSafetyMs)
        ReleaseStartupUi();
}

void RestoreLegalOnEnterSlots() {
    if (g_legalOnEnterAsyncSlot && g_originalLegalOnEnterAsync)
        WritePointerSlot(g_legalOnEnterAsyncSlot, reinterpret_cast<void*>(g_originalLegalOnEnterAsync));
    if (g_legalOnEnterSlot && g_originalLegalOnEnter)
        WritePointerSlot(g_legalOnEnterSlot, reinterpret_cast<void*>(g_originalLegalOnEnter));
    g_legalOnEnterAsyncSlot = nullptr;
    g_legalOnEnterSlot = nullptr;
}

bool __fastcall EarlyUiMenuStateLegalOnEnterAsync(void* self, const void* fromState, const void* toState) {
    if (!g_timingLegalOnEnterMs) g_timingLegalOnEnterMs = GetTickCount64();
    HideStartupUi();
    return g_originalLegalOnEnterAsync ? g_originalLegalOnEnterAsync(self, fromState, toState) : true;
}

bool __fastcall EarlyUiMenuStateLegalOnEnter(void* self, const void* fromState, const void* toState) {
    if (!g_timingLegalOnEnterMs) g_timingLegalOnEnterMs = GetTickCount64();
    HideStartupUi();
    const bool result = g_originalLegalOnEnter ? g_originalLegalOnEnter(self, fromState, toState) : true;
    if (result) RestoreLegalOnEnterSlots();   // the Legal screen is entered once per boot
    return result;
}

// Armed only together with the PROFILE_LOAD hooks (their OnExit is the normal release point).
bool ArmStartupUiHide(unsigned char* base) {
    if (!base) return false;
    auto* get = base + kRvaScaleformManagerGet;
    if (!BytesEqual(get, kScaleformManagerGetSig, kScaleformManagerGetSigSize)) return false;
    auto** asyncSlot = reinterpret_cast<void**>(base + kRvaUiMenuStateLegalVtable + kUiMenuStateLegalOnEnterAsyncSlot * sizeof(void*));
    auto** enterSlot = reinterpret_cast<void**>(base + kRvaUiMenuStateLegalVtable + kUiMenuStateLegalOnEnterSlot * sizeof(void*));
    if (*asyncSlot != reinterpret_cast<void*>(base + kRvaUiMenuStateGenericOnEnterAsync) ||
        *enterSlot != reinterpret_cast<void*>(base + kRvaUiMenuStateLegalOnEnter))
        return false;

    g_scaleformManagerGet = reinterpret_cast<ScaleformManagerGetFn_>(get);
    g_originalLegalOnEnterAsync = reinterpret_cast<LegalOnEnterFn_>(*asyncSlot);
    g_originalLegalOnEnter = reinterpret_cast<LegalOnEnterFn_>(*enterSlot);
    if (!WritePointerSlot(asyncSlot, reinterpret_cast<void*>(&EarlyUiMenuStateLegalOnEnterAsync))) return false;
    if (!WritePointerSlot(enterSlot, reinterpret_cast<void*>(&EarlyUiMenuStateLegalOnEnter))) {
        WritePointerSlot(asyncSlot, reinterpret_cast<void*>(g_originalLegalOnEnterAsync));
        return false;
    }
    g_legalOnEnterAsyncSlot = asyncSlot;
    g_legalOnEnterSlot = enterSlot;
    g_startupUiHideArmed = true;
    return true;
}




constexpr usize kRvaStartupSteamSaveGlobal = 0x02E1E4A0ull;
constexpr usize kWinSaveStatusOffset = 0x08ull;
constexpr usize kRvaSteamSaveProcess = 0x008A87D0ull;
constexpr usize kRvaSteamSaveCompletionDone = 0x02E1E4F4ull;
constexpr usize kRvaSteamSaveCompletionSuccess = 0x02E1E4F5ull;
constexpr usize kRvaSteamSaveSaveExists = 0x008A9160ull;
using SteamSaveProcessFn_ = void(__fastcall*)(void*, long long);
using StartupSaveExistsFn_ = bool(__fastcall*)(void*, unsigned long long);

SteamSaveProcessFn_ g_profileEarlyPumpProcess = nullptr;
void* g_profileEarlyPumpSteamSave = nullptr;
volatile unsigned char* g_profileEarlyPumpDone = nullptr;
volatile unsigned char* g_profileEarlyPumpSuccess = nullptr;
volatile bool g_profileEarlyPumpVerified = false;
DWORD_ g_startupThreadId = 0;

StartupSaveExistsFn_ g_startupSaveExistsOriginal = nullptr;
void** g_startupSaveExistsSlot = nullptr;
volatile bool g_startupSaveExistsHookArmed = false;
volatile bool g_startupSaveCacheActive = false;

constexpr usize kRvaSteamInternalContextInitIat = 0x00D02D48ull;
constexpr usize kRvaSteamContextInitData = 0x00FB20E0ull;
constexpr usize kRvaSteamSaveBuildFileName = 0x008A7EE0ull;
constexpr usize kSteamContextRemoteStorageOffset = 0x48ull;
constexpr usize kSteamRemoteGetFileCountVslot = 0x90ull / sizeof(void*);
constexpr usize kSteamRemoteGetFileNameAndSizeVslot = 0x98ull / sizeof(void*);
constexpr u32 kStartupRemoteFileLimit = 4096u;
constexpr u32 kStartupRemoteFileNameStride = 512u;
using SteamInternalContextInitFn_ = void*(__fastcall*)(void*);
using SteamRemoteGetFileCountFn_ = int(__fastcall*)(void*);
using SteamRemoteGetFileNameAndSizeFn_ = const char*(__fastcall*)(void*, int, int*);
using SteamSaveBuildFileNameFn_ = bool(__fastcall*)(void*, char*, u32, bool);
SteamSaveBuildFileNameFn_ g_startupBuildSaveFileName = nullptr;
char* g_startupRemoteFileNames = nullptr;
volatile bool g_startupRemoteFileCacheReady = false;
volatile bool g_startupRemoteFileCacheFailed = false;
volatile u32 g_startupRemoteFileCount = 0;

u32 BoundedAnsiLength(const char* s, u32 limit) {
    if (!s) return limit;
    u32 n = 0;
    while (n < limit && s[n]) ++n;
    return n;
}

bool CopyBoundedAnsi(char* dst, u32 capacity, const char* src) {
    if (!dst || !capacity || !src) return false;
    const u32 n = BoundedAnsiLength(src, capacity);
    if (n >= capacity) return false;
    for (u32 i = 0; i <= n; ++i) dst[i] = src[i];
    return true;
}

bool AnsiStringsEqual(const char* a, const char* b) {
    if (!a || !b) return false;
    for (u32 i = 0;; ++i) {
        if (a[i] != b[i]) return false;
        if (!a[i]) return true;
    }
}

bool EnsureStartupRemoteFileCache() {
    if (g_startupRemoteFileCacheReady) return true;
    if (g_startupRemoteFileCacheFailed) return false;

    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!base) { g_startupRemoteFileCacheFailed = true; return false; }

    auto contextInit = reinterpret_cast<SteamInternalContextInitFn_>(
        *reinterpret_cast<void**>(base + kRvaSteamInternalContextInitIat));
    if (!contextInit) { g_startupRemoteFileCacheFailed = true; return false; }

    void* steamContext = contextInit(base + kRvaSteamContextInitData);
    if (!steamContext) { g_startupRemoteFileCacheFailed = true; return false; }
    void* remoteStorage = *reinterpret_cast<void**>(
        reinterpret_cast<unsigned char*>(steamContext) + kSteamContextRemoteStorageOffset);
    if (!remoteStorage) { g_startupRemoteFileCacheFailed = true; return false; }
    void** vtable = *reinterpret_cast<void***>(remoteStorage);
    if (!vtable) { g_startupRemoteFileCacheFailed = true; return false; }

    auto getFileCount = reinterpret_cast<SteamRemoteGetFileCountFn_>(vtable[kSteamRemoteGetFileCountVslot]);
    auto getFileNameAndSize = reinterpret_cast<SteamRemoteGetFileNameAndSizeFn_>(vtable[kSteamRemoteGetFileNameAndSizeVslot]);
    if (!getFileCount || !getFileNameAndSize) { g_startupRemoteFileCacheFailed = true; return false; }

    const int sourceCount = getFileCount(remoteStorage);
    if (sourceCount < 0 || static_cast<u32>(sourceCount) > kStartupRemoteFileLimit) {
        g_startupRemoteFileCacheFailed = true;
        return false;
    }
    if (!sourceCount) {
        g_startupRemoteFileCount = 0;
        g_startupRemoteFileCacheReady = true;
        return true;
    }

    const usize bytes = static_cast<usize>(sourceCount) * kStartupRemoteFileNameStride;
    char* cache = static_cast<char*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT_ | MEM_RESERVE_, PAGE_READWRITE_));
    if (!cache) { g_startupRemoteFileCacheFailed = true; return false; }

    u32 stored = 0;
    for (int i = 0; i < sourceCount; ++i) {
        int fileSize = 0;
        const char* name = getFileNameAndSize(remoteStorage, i, &fileSize);
        if (!name) continue;
        char* dst = cache + static_cast<usize>(stored) * kStartupRemoteFileNameStride;
        if (!CopyBoundedAnsi(dst, kStartupRemoteFileNameStride, name)) continue;
        ++stored;
    }
    g_startupRemoteFileNames = cache;
    g_startupRemoteFileCount = stored;
    g_startupRemoteFileCacheReady = true;
    return true;
}

bool QueryStartupRemoteFileCache(void* self, unsigned long long slot, bool& result) {
    if (!EnsureStartupRemoteFileCache() || !g_startupBuildSaveFileName) return false;
    char wanted[kStartupRemoteFileNameStride]{};
    const u32 saveEnum = static_cast<u32>((slot >> 32) & 0xFFFFFFFFull);
    if (!g_startupBuildSaveFileName(self, wanted, saveEnum, false)) return false;

    result = false;
    for (u32 i = 0; i < g_startupRemoteFileCount; ++i) {
        const char* candidate = g_startupRemoteFileNames + static_cast<usize>(i) * kStartupRemoteFileNameStride;
        if (AnsiStringsEqual(candidate, wanted)) { result = true; break; }
    }
    return true;
}

bool __fastcall EarlyStartupSaveExists(void* self, unsigned long long slot) {
    if (g_startupSaveCacheActive) {
        bool result = false;
        if (QueryStartupRemoteFileCache(self, slot, result)) return result;
    }
    return g_startupSaveExistsOriginal ? g_startupSaveExistsOriginal(self, slot) : false;
}

void RestoreStartupSaveExistsHook() {
    if (g_startupSaveExistsHookArmed && g_startupSaveExistsSlot && g_startupSaveExistsOriginal)
        WritePointerSlot(g_startupSaveExistsSlot, reinterpret_cast<void*>(g_startupSaveExistsOriginal));
    g_startupSaveExistsHookArmed = false;
    g_startupSaveCacheActive = false;
}

bool ArmStartupSaveAcceleration(unsigned char* base) {
    if (!base) return false;
    void* steamSave = *reinterpret_cast<void**>(base + kRvaStartupSteamSaveGlobal);
    if (!steamSave) return false;
    void** vtable = *reinterpret_cast<void***>(steamSave);
    if (!vtable) return false;

    g_profileEarlyPumpSteamSave = steamSave;
    g_profileEarlyPumpDone = base + kRvaSteamSaveCompletionDone;
    g_profileEarlyPumpSuccess = base + kRvaSteamSaveCompletionSuccess;
    g_profileEarlyPumpProcess = reinterpret_cast<SteamSaveProcessFn_>(vtable[0x60u / sizeof(void*)]);
    g_profileEarlyPumpVerified =
        g_profileEarlyPumpProcess == reinterpret_cast<SteamSaveProcessFn_>(base + kRvaSteamSaveProcess);

    g_startupBuildSaveFileName = reinterpret_cast<SteamSaveBuildFileNameFn_>(base + kRvaSteamSaveBuildFileName);
    void** saveExistsSlot = vtable + (0x50u / sizeof(void*));
    if (*saveExistsSlot != reinterpret_cast<void*>(base + kRvaSteamSaveSaveExists)) return false;
    g_startupSaveExistsOriginal = reinterpret_cast<StartupSaveExistsFn_>(*saveExistsSlot);
    if (!WritePointerSlot(saveExistsSlot, reinterpret_cast<void*>(&EarlyStartupSaveExists))) return false;
    g_startupSaveExistsSlot = saveExistsSlot;
    g_startupSaveExistsHookArmed = true;
    g_startupSaveCacheActive = true;
    return true;
}

void TryEarlyProfileCompletionPump() {
    if (!g_profileEarlyPumpVerified || !g_profileEarlyPumpProcess || !g_profileEarlyPumpSteamSave ||
        !g_profileEarlyPumpDone || !g_profileEarlyPumpSuccess || GetCurrentThreadId() != g_startupThreadId)
        return;

    auto* saveBytes = reinterpret_cast<unsigned char*>(g_profileEarlyPumpSteamSave);
    const int status = *reinterpret_cast<volatile int*>(saveBytes + kWinSaveStatusOffset);
    if (status != 5 || !*g_profileEarlyPumpDone) return;

    
    
    g_profileEarlyPumpProcess(g_profileEarlyPumpSteamSave, 0);
}



constexpr usize kRvaUiMenuStateProfileLoadVtable = 0x00DB49A0ull;
constexpr usize kUiMenuStateOnEnterSlot = 3ull;
constexpr usize kUiMenuStateProcessSlot = 6ull;
constexpr usize kUiMenuStateOnExitSlot = 8ull;
constexpr usize kRvaUiMenuStateProfileLoadOnEnter = 0x0090E7B0ull;
constexpr usize kRvaUiMenuStateProfileLoadProcess = 0x00940650ull;
constexpr usize kRvaUiMenuStateProfileLoadOnExit = 0x00916570ull;
using UiStateOnEnterFn_ = bool(__fastcall*)(void*, const void*, const void*);
using UiStateProcessFn_ = void(__fastcall*)(void*, float);
using UiStateOnExitFn_ = bool(__fastcall*)(void*, const void*, const int*);
UiStateOnEnterFn_ g_originalProfileLoadOnEnter = nullptr;
UiStateProcessFn_ g_originalProfileLoadProcess = nullptr;
UiStateOnExitFn_ g_originalProfileLoadOnExit = nullptr;
void** g_profileLoadOnEnterSlot = nullptr;
void** g_profileLoadProcessSlot = nullptr;
void** g_profileLoadOnExitSlot = nullptr;
volatile bool g_profileLoadHooksArmed = false;

void RestoreProfileLoadHooks() {
    if (g_profileLoadOnEnterSlot && g_originalProfileLoadOnEnter)
        WritePointerSlot(g_profileLoadOnEnterSlot, reinterpret_cast<void*>(g_originalProfileLoadOnEnter));
    if (g_profileLoadProcessSlot && g_originalProfileLoadProcess)
        WritePointerSlot(g_profileLoadProcessSlot, reinterpret_cast<void*>(g_originalProfileLoadProcess));
    if (g_profileLoadOnExitSlot && g_originalProfileLoadOnExit)
        WritePointerSlot(g_profileLoadOnExitSlot, reinterpret_cast<void*>(g_originalProfileLoadOnExit));
    g_profileLoadHooksArmed = false;
}

bool __fastcall EarlyProfileLoadOnEnter(void* self, const void* fromState, const void* toState) {
    if (!g_timingProfileLoadOnEnterMs) g_timingProfileLoadOnEnterMs = GetTickCount64();
    return g_originalProfileLoadOnEnter ? g_originalProfileLoadOnEnter(self, fromState, toState) : true;
}

void __fastcall EarlyProfileLoadProcess(void* self, float dt) {
    ReleaseStartupUiIfExpired();
    TryEarlyProfileCompletionPump();
    if (g_originalProfileLoadProcess) g_originalProfileLoadProcess(self, dt);
}

bool __fastcall EarlyProfileLoadOnExit(void* self, const void* fromState, const int* toState) {
    const bool result = g_originalProfileLoadOnExit ? g_originalProfileLoadOnExit(self, fromState, toState) : true;
    if (!g_timingProfileLoadOnExitMs) g_timingProfileLoadOnExitMs = GetTickCount64();
    // v0.9.339: MOUNTING_DLC comes next and stops the game for seconds (DlcManager reads each DLC's files with a
    // synchronous request, 0x34B7F0, queued behind the menu's streaming: 4.3 s in the 0.9.339 log); the menu drawn
    // before it froze on screen. With its hooks in place the UI stays hidden until that state exits.
    if (g_dlcMountHooksArmed && g_startupUiHidden && !g_startupUiDlcMountEntered) g_startupUiWaitingForDlcMount = true;
    else ReleaseStartupUi();   // the next state (main menu) is the first UI drawn
    RestoreProfileLoadHooks();
    return result;
}

bool ArmProfileLoadHooks(unsigned char* base) {
    if (!base) return false;
    auto** onEnter = reinterpret_cast<void**>(base + kRvaUiMenuStateProfileLoadVtable + kUiMenuStateOnEnterSlot * sizeof(void*));
    auto** process = reinterpret_cast<void**>(base + kRvaUiMenuStateProfileLoadVtable + kUiMenuStateProcessSlot * sizeof(void*));
    auto** onExit = reinterpret_cast<void**>(base + kRvaUiMenuStateProfileLoadVtable + kUiMenuStateOnExitSlot * sizeof(void*));
    if (*onEnter != reinterpret_cast<void*>(base + kRvaUiMenuStateProfileLoadOnEnter) ||
        *process != reinterpret_cast<void*>(base + kRvaUiMenuStateProfileLoadProcess) ||
        *onExit != reinterpret_cast<void*>(base + kRvaUiMenuStateProfileLoadOnExit))
        return false;

    g_originalProfileLoadOnEnter = reinterpret_cast<UiStateOnEnterFn_>(*onEnter);
    g_originalProfileLoadProcess = reinterpret_cast<UiStateProcessFn_>(*process);
    g_originalProfileLoadOnExit = reinterpret_cast<UiStateOnExitFn_>(*onExit);
    if (!WritePointerSlot(onEnter, reinterpret_cast<void*>(&EarlyProfileLoadOnEnter))) return false;
    if (!WritePointerSlot(process, reinterpret_cast<void*>(&EarlyProfileLoadProcess))) {
        WritePointerSlot(onEnter, reinterpret_cast<void*>(g_originalProfileLoadOnEnter));
        return false;
    }
    if (!WritePointerSlot(onExit, reinterpret_cast<void*>(&EarlyProfileLoadOnExit))) {
        WritePointerSlot(process, reinterpret_cast<void*>(g_originalProfileLoadProcess));
        WritePointerSlot(onEnter, reinterpret_cast<void*>(g_originalProfileLoadOnEnter));
        return false;
    }
    g_profileLoadOnEnterSlot = onEnter;
    g_profileLoadProcessSlot = process;
    g_profileLoadOnExitSlot = onExit;
    g_profileLoadHooksArmed = true;
    return true;
}




constexpr usize kRvaUiMenuStateMountingDlcVtable = 0x00DB4A98ull;
constexpr usize kRvaUiMenuStateMountingDlcOnEnter = 0x0090CFA0ull;
constexpr usize kRvaUiMenuStateMountingDlcProcess = 0x0093FE80ull;
constexpr usize kRvaDlcManagerGet = 0x0054BF30ull;
constexpr usize kRvaDlcManagerPumpA = 0x0054D2A0ull;
constexpr usize kRvaDlcManagerPumpB = 0x0054D320ull;
using DlcManagerGetFn_ = void*(__fastcall*)();
using DlcManagerPumpFn_ = void(__fastcall*)(void*);
constexpr usize kRvaUiMenuStateMountingDlcOnExit = 0x009148D0ull;
UiStateOnEnterFn_ g_originalDlcMountOnEnter = nullptr;
UiStateProcessFn_ g_originalDlcMountProcess = nullptr;
UiStateOnExitFn_ g_originalDlcMountOnExit = nullptr;
void** g_dlcMountOnEnterSlot = nullptr;
void** g_dlcMountProcessSlot = nullptr;
void** g_dlcMountOnExitSlot = nullptr;

void PumpDlcManagerQueues() {
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!base) return;
    auto getManager = reinterpret_cast<DlcManagerGetFn_>(base + kRvaDlcManagerGet);
    void* manager = getManager ? getManager() : nullptr;
    if (!manager) return;
    auto pumpA = reinterpret_cast<DlcManagerPumpFn_>(base + kRvaDlcManagerPumpA);
    auto pumpB = reinterpret_cast<DlcManagerPumpFn_>(base + kRvaDlcManagerPumpB);
    if (pumpA) pumpA(manager);
    if (pumpB) pumpB(manager);
}

bool __fastcall EarlyDlcMountOnEnter(void* self, const void* fromState, const void* toState) {
    g_startupUiDlcMountEntered = true;
    const bool result = g_originalDlcMountOnEnter ? g_originalDlcMountOnEnter(self, fromState, toState) : true;
    
    
    
    PumpDlcManagerQueues();
    return result;
}

void __fastcall EarlyDlcMountProcess(void* self, float dt) {
    ReleaseStartupUiIfExpired();
    
    
    
    PumpDlcManagerQueues();
    if (g_originalDlcMountProcess) g_originalDlcMountProcess(self, dt);
}

bool __fastcall EarlyDlcMountOnExit(void* self, const void* fromState, const int* toState) {
    const bool result = g_originalDlcMountOnExit ? g_originalDlcMountOnExit(self, fromState, toState) : true;
    if (g_startupUiWaitingForDlcMount) {
        g_startupUiWaitingForDlcMount = false;
        ReleaseStartupUi();   // the main menu, after the DLC reads, is the first UI drawn
    }
    return result;
}

bool ArmDlcMountHooks(unsigned char* base) {
    if (!base) return false;
    auto** onEnter = reinterpret_cast<void**>(base + kRvaUiMenuStateMountingDlcVtable + kUiMenuStateOnEnterSlot * sizeof(void*));
    auto** process = reinterpret_cast<void**>(base + kRvaUiMenuStateMountingDlcVtable + kUiMenuStateProcessSlot * sizeof(void*));
    auto** onExit = reinterpret_cast<void**>(base + kRvaUiMenuStateMountingDlcVtable + kUiMenuStateOnExitSlot * sizeof(void*));
    if (*onEnter != reinterpret_cast<void*>(base + kRvaUiMenuStateMountingDlcOnEnter) ||
        *process != reinterpret_cast<void*>(base + kRvaUiMenuStateMountingDlcProcess) ||
        *onExit != reinterpret_cast<void*>(base + kRvaUiMenuStateMountingDlcOnExit))
        return false;

    g_originalDlcMountOnEnter = reinterpret_cast<UiStateOnEnterFn_>(*onEnter);
    g_originalDlcMountProcess = reinterpret_cast<UiStateProcessFn_>(*process);
    if (!WritePointerSlot(onEnter, reinterpret_cast<void*>(&EarlyDlcMountOnEnter))) return false;
    if (!WritePointerSlot(process, reinterpret_cast<void*>(&EarlyDlcMountProcess))) {
        WritePointerSlot(onEnter, reinterpret_cast<void*>(g_originalDlcMountOnEnter));
        return false;
    }
    g_originalDlcMountOnExit = reinterpret_cast<UiStateOnExitFn_>(*onExit);
    if (!WritePointerSlot(onExit, reinterpret_cast<void*>(&EarlyDlcMountOnExit))) {
        WritePointerSlot(onEnter, reinterpret_cast<void*>(g_originalDlcMountOnEnter));
        WritePointerSlot(process, reinterpret_cast<void*>(g_originalDlcMountProcess));
        return false;
    }
    g_dlcMountOnEnterSlot = onEnter;
    g_dlcMountProcessSlot = process;
    g_dlcMountOnExitSlot = onExit;
    g_dlcMountHooksArmed = true;
    return true;
}




void __fastcall EarlyUiMenuStateLegalProcess(void* self, float dt) {
    if (self)
        *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(self) + kUiMenuStateLegalTimerOffset) = 0.0f;

    if (!g_timingLegalFirstProcessMs) g_timingLegalFirstProcessMs = GetTickCount64();
    g_legalProcessFired = true;
    g_legalVisualSuppressionActive = true;
    RestoreLegalProcessEntry();

    if (g_legalProcessTrampoline) g_legalProcessTrampoline(self, dt);

    
    
    if (self && !g_legalExitWatchFired && g_legalExitWatchArmed && g_legalGotoMainMenu) {
        g_legalForcedNativeExit = true;
        if (!g_timingForcedGotoMs) g_timingForcedGotoMs = GetTickCount64();
        g_startupThreadId = GetCurrentThreadId();
        auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        ArmStartupSaveAcceleration(base);
        g_legalGotoMainMenu(self);
        g_startupSaveCacheActive = false;
        RestoreStartupSaveExistsHook();
        if (!g_timingGotoReturnedMs) g_timingGotoReturnedMs = GetTickCount64();
    }
}

void ApplyEarlyStartupSkip() {
    // (v0.9.459) The startup skip is part of Faster loading: ROTTRFramework.ini [Loading] FasterLoading, on by default
    // (no ini, or no such key: on). It replaced [Startup] FastStartup / SkipIntroLogos, which are no longer read.
    wchar_t iniPath[MAX_PATH_]{};
    const DWORD_ n = GetModuleFileNameW(g_self, iniPath, MAX_PATH_);
    g_skipRequested = n && n < MAX_PATH_ && ReplaceFilename(iniPath, MAX_PATH_, L"ROTTRFramework.ini") &&
                      GetPrivateProfileIntW(L"Loading", L"FasterLoading", 1, iniPath) != 0;

    if (!g_skipRequested) return;

    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!base) {
        g_legalProcessSignatureMismatch = true;
        g_legalExitWatchSignatureMismatch = true;
        return;
    }

    ArmLegalExitWatch(base);
    ArmLegalPreExitWatch(base);
    if (ArmProfileLoadHooks(base))
        ArmStartupUiHide(base);

    auto* gotoTarget = base + kRvaUiMenuStateLegalGotoMainMenu;
    if (BytesEqual(gotoTarget, kLegalGotoMainMenuSig, kLegalGotoMainMenuSigSize))
        g_legalGotoMainMenu = reinterpret_cast<LegalGotoMainMenuFn_>(gotoTarget);
    else
        g_legalGotoMainMenuSignatureMismatch = true;

    g_legalProcessTarget = base + kRvaUiMenuStateLegalProcess;
    if (!BytesEqual(g_legalProcessTarget, kLegalProcessPrologue, kLegalProcessPrologueSize)) {
        g_legalProcessSignatureMismatch = true;
        return;
    }

    auto* trampoline = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, kLegalProcessPrologueSize + 14u,
                     MEM_COMMIT_ | MEM_RESERVE_, PAGE_EXECUTE_READWRITE_));
    if (!trampoline) return;
    for (usize i = 0; i < kLegalProcessPrologueSize; ++i) trampoline[i] = kLegalProcessPrologue[i];
    WriteAbsJump(trampoline + kLegalProcessPrologueSize,
                 g_legalProcessTarget + kLegalProcessPrologueSize);
    FlushInstructionCache(GetCurrentProcess(), trampoline, kLegalProcessPrologueSize + 14u);
    g_legalProcessTrampoline = reinterpret_cast<LegalProcessFn_>(trampoline);

    DWORD_ oldProtect = 0;
    if (!VirtualProtect(g_legalProcessTarget, kLegalProcessPrologueSize, PAGE_EXECUTE_READWRITE_, &oldProtect)) return;
    WriteAbsJump(g_legalProcessTarget, reinterpret_cast<const void*>(&EarlyUiMenuStateLegalProcess));
    DWORD_ ignored = 0;
    VirtualProtect(g_legalProcessTarget, kLegalProcessPrologueSize, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), g_legalProcessTarget, kLegalProcessPrologueSize);
    g_legalProcessArmed = true;
}

DWORD_ WINAPI BootstrapThread(void*) {
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    
    
    
    if (g_skipRequested)
        ArmDlcMountHooks(base);

    wchar_t path[MAX_PATH_]{};
    const DWORD_ n = GetModuleFileNameW(g_self, path, MAX_PATH_);
    if (!n || n >= MAX_PATH_ || !ReplaceFilename(path, MAX_PATH_, L"ROTTRFramework.asi")) return 0;
    LoadLibraryW(path);
    return 0;
}
} 

extern "C" __declspec(dllexport) u32 WINAPI RottrStartupSkipState() {
    u32 state = 0;
    if (g_skipRequested) state |= 1u;
    if (g_legalProcessArmed) state |= 2u;
    if (g_legalProcessFired) state |= 4u;
    if (g_legalProcessSignatureMismatch) state |= 8u;
    if (g_legalVisualSuppressionActive) state |= 16u;
    if (g_legalExitWatchArmed) state |= 32u;
    if (g_legalExitWatchFired) state |= 64u;
    if (g_legalExitWatchSignatureMismatch) state |= 128u;
    if (g_legalGotoMainMenu) state |= 256u;
    if (g_legalGotoMainMenuSignatureMismatch) state |= 512u;
    if (g_legalForcedNativeExit) state |= 1024u;
    if (g_startupUiHideArmed) state |= 2048u;
    if (g_startupUiHidden) state |= 4096u;
    if (g_startupUiHideDone) state |= 8192u;
    if (g_startupUiWaitingForDlcMount) state |= 16384u;
    if (g_startupUiDlcMountEntered) state |= 32768u;
    return state;
}

// Framework-side safety net: release the startup UI hide (no-op when it is not active).
extern "C" __declspec(dllexport) void WINAPI RottrStartupUiRelease() {
    ReleaseStartupUi();
}

extern "C" __declspec(dllexport) unsigned long long WINAPI RottrStartupTimingQuery(u32 milestone) {
    switch (milestone) {
        case 0u: return g_timingLegalFirstProcessMs;
        case 1u: return g_timingForcedGotoMs;
        case 2u: return g_timingProfileLoadRequestMs; 
        case 3u: return g_timingLegalPreExitMs;
        case 4u: return g_timingLegalOnExitMs;
        case 5u: return g_timingProfileLoadOnEnterMs;
        case 6u: return g_timingGotoReturnedMs;
        case 7u: return g_timingAttachMs;
        case 8u: return g_timingLegalOnEnterMs;
        case 9u: return g_timingProfileLoadOnExitMs;
        case 10u: return g_timingStartupUiReleaseMs;
        default: return 0ull;
    }
}

extern "C" __declspec(dllexport) i32 WINAPI ReportFault(void* ep, DWORD_ options) {
    if (!g_reportFault && !LoadRealFaultrep()) return 0;
    return g_reportFault(ep, options);
}

// Launcher skip (v0.9.288, ROTTRFramework.ini [Startup] SkipLauncher=1): the Nixxes launcher window (Play /
// Options / ...) is shown by nx::NxAppImpl::Init (retail 0xC82E60) when NxAppImpl+9 is set ("-nolauncher" clears it in
// ParseCommandLine, which only sees the arguments). At 0xC8309B "test bl,bl; je +2D; call NxApp_GetLauncher" the je
// becomes a jmp: the game starts as if Play was pressed. Done here, before the game's WinMain runs.
volatile bool g_launcherSkipped = false;
volatile bool g_launcherSkipMismatch = false;

void ApplyEarlyLauncherSkip() {
    wchar_t iniPath[MAX_PATH_]{};
    const DWORD_ n = GetModuleFileNameW(g_self, iniPath, MAX_PATH_);
    if (!n || n >= MAX_PATH_ || !ReplaceFilename(iniPath, MAX_PATH_, L"ROTTRFramework.ini")) return;
    if (GetPrivateProfileIntW(L"Startup", L"SkipLauncher", 0, iniPath) == 0) return;
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!base) return;
    unsigned char* at = base + 0xC8309Bu;
    const unsigned char expected[5] = {0x84, 0xDB, 0x74, 0x2D, 0xE8};
    for (int i = 0; i < 5; ++i) {
        if (at[i] != expected[i]) {
            g_launcherSkipMismatch = true;
            return;
        }
    }
    DWORD_ oldProtect = 0;
    if (!VirtualProtect(at + 2, 1, PAGE_EXECUTE_READWRITE_, &oldProtect)) return;
    at[2] = 0xEB;
    DWORD_ ignored = 0;
    VirtualProtect(at + 2, 1, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at + 2, 1);
    g_launcherSkipped = true;
}

extern "C" __declspec(dllexport) u32 WINAPI RottrLauncherSkipState() {
    return (g_launcherSkipped ? 1u : 0u) | (g_launcherSkipMismatch ? 2u : 0u);
}

extern "C" BOOL_ WINAPI DllMain(HMODULE_ module, DWORD_ reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_timingAttachMs = GetTickCount64();
        g_self = module;
        DisableThreadLibraryCalls(module);
        ApplyEarlyStartupSkip();
        ApplyEarlyLauncherSkip();
        HANDLE_ thread = CreateThread(nullptr, 0, &BootstrapThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE_VALUE;
}
