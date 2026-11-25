// DirectX 12 root signature changes with tessellation (see dx12_root_fix.h). Addresses: the retail Steam ROTTR.exe
// (GameBuild::kSupportedTimestamp), found from TR2_x64_final's functions of the same names; each is checked against the
// bytes the fix relies on.
#include "dx12_root_fix.h"

#include "game_build.h"
#include "log.h"

#include <MinHook.h>
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace Dx12RootFix {
namespace {
constexpr std::uintptr_t kRvaPreDraw = 0x00310860;            // PCDX12RenderContext::PreDraw (TR2 0x333B10)
constexpr std::uintptr_t kRvaPreDrawSetRootSig = 0x003108DD;   // its call [rax + 0xF0] (SetGraphicsRootSignature)
constexpr std::uintptr_t kRvaUpdateCommandList = 0x00316CC0;   // PCDX12RenderContext::UpdateCommandList (TR2 0x339F10)
constexpr std::uintptr_t kRvaTransferCommandList = 0x00315850; // PCDX12RenderContext::TransferCommandList (TR2 0x338AA0)
constexpr std::uintptr_t kRvaDeviceManager = 0x01A76B90;       // PCDX12DeviceManager::s_pInstance (root signatures +0x20..)
constexpr std::size_t kCtxRootSignature = 0x34F0, kCtxRootSignatureIndex = 0x3810, kCtxDescriptorCache = 0x3490;
constexpr std::size_t kManagerRootSignatures = 0x20;
constexpr std::size_t kCacheDirtySrv = 0x1C, kCacheDirtyCb = 0x20, kCacheDirtySampler = 0x24, kCacheDirtyUav = 0x28;

struct Check {
    std::uintptr_t rva;
    std::uint8_t bytes[40];
    std::size_t size;
    const char* what;
};
const Check kChecks[] = {
    {kRvaPreDraw, {0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x89, 0x5C, 0x24, 0x40, 0x48, 0x8B, 0xF9}, 14, "PCDX12RenderContext::PreDraw"},
    // lea rbx, [rdi + 0x34F0] (the pipeline state: +0x28 domain, +0x38 hull, +0x48 geometry shader)
    {0x310878, {0x48, 0x8D, 0x9F, 0xF0, 0x34, 0x00, 0x00, 0x48, 0x83, 0x7B, 0x38, 0x00}, 12, "PreDraw: the pipeline state (+0x34F0)"},
    // mov r8, [rax + rdx * 8 + 0x20]; mov rax, [rbx]; cmp rax, r8; je; mov [rbx + 0x320], edx; mov rdx, r8; mov [rbx], r8;
    // mov rcx, [rdi + 0x3488]; mov rax, [rcx]; call [rax + 0xF0]; mov rcx, rbx
    {0x3108BA, {0x4C, 0x8B, 0x44, 0xD0, 0x20, 0x48, 0x8B, 0x03, 0x49, 0x3B, 0xC0, 0x74, 0x1C, 0x89, 0x93, 0x20, 0x03, 0x00, 0x00, 0x49,
                0x8B, 0xD0, 0x4C, 0x89, 0x03, 0x48, 0x8B, 0x8F, 0x88, 0x34, 0x00, 0x00, 0x48, 0x8B, 0x01, 0xFF, 0x90, 0xF0, 0x00, 0x00},
     40, "PreDraw: the root signature change"},
    {0x3108E1, {0x00, 0x00, 0x48, 0x8B, 0xCB}, 5, "PreDraw: after SetGraphicsRootSignature"},
    // mov rax, [rdi + 0x3490]; or dword [rax + 0x2C], 1 (the descriptor cache, m_activeStages)
    {0x310925, {0x48, 0x8B, 0x87, 0x90, 0x34, 0x00, 0x00, 0x48, 0x89, 0x6C, 0x24, 0x48, 0x83, 0x48, 0x2C, 0x01}, 16,
     "PreDraw: the descriptor cache (+0x3490)"},
    // RestoreRootSignature: mov [rbx + 0x34F0], rdx; mov [rbx + 0x3810], r8d ... the dirty masks it sets
    {0x3127E6, {0x48, 0x89, 0x93, 0xF0, 0x34, 0x00, 0x00, 0x44, 0x89, 0x83, 0x10, 0x38, 0x00, 0x00}, 14,
     "RestoreRootSignature: the root signature and its index"},
    {0x31281F, {0x48, 0x8B, 0x83, 0x90, 0x34, 0x00, 0x00, 0xC7, 0x40, 0x20, 0x3F, 0x00, 0x00, 0x00, 0xC7, 0x40, 0x1C, 0x3F, 0x00, 0x00,
                0x00, 0xC7, 0x40, 0x24, 0x3F, 0x00, 0x00, 0x00, 0xC7, 0x40, 0x28, 0x30, 0x00, 0x00, 0x00},
     35, "RestoreRootSignature: the dirty stages"},
    {kRvaUpdateCommandList, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF9}, 16,
     "PCDX12RenderContext::UpdateCommandList"},
    {kRvaTransferCommandList, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x81, 0x80, 0x34, 0x00}, 16,
     "PCDX12RenderContext::TransferCommandList"},
};
struct RelCheck {
    std::uintptr_t rva;
    std::size_t operand, length;
    std::uintptr_t target;
    const char* what;
};
const RelCheck kRelChecks[] = {
    {0x3108B3, 3, 7, kRvaDeviceManager, "PreDraw's device manager"},
    {0x3127DA, 3, 7, kRvaDeviceManager, "RestoreRootSignature's device manager"},
    {0x316D34, 3, 7, kRvaDeviceManager, "UpdateCommandList's device manager"},
};
// UpdateCommandList: mov rdx, [devmgr]; mov rax, [rcx]; mov rdx, [rdx + 0x20]; call [rax + 0xF0] (the first root signature)
constexpr std::uintptr_t kRvaUpdateRootSig = 0x316D6E;
constexpr std::uint8_t kUpdateRootSig[] = {0x48, 0x8B, 0x01, 0x48, 0x8B, 0x52, 0x20, 0xFF, 0x90, 0xF0, 0x00, 0x00, 0x00};

// The stub PreDraw calls instead of [rax + 0xF0] (rcx the command list, rdx the root signature, rax its vtable, rdi the
// context): the call, then (the switch on) the descriptor cache's every stage dirty.
//   sub rsp, 0x28 / call [rax + 0xF0] / cmp byte [rip + flag], 0 / je done / mov rax, [rdi + 0x3490] / test rax, rax /
//   je done / or dword [rax + 0x1C], 0x3F / or dword [rax + 0x20], 0x3F / or dword [rax + 0x24], 0x3F /
//   or dword [rax + 0x28], 0x30 / done: add rsp, 0x28 / ret
constexpr std::size_t kStubFlagAt = 0x40;
constexpr std::uint8_t kStub[] = {
    0x48, 0x83, 0xEC, 0x28,                          // 00 sub rsp, 0x28
    0xFF, 0x90, 0xF0, 0x00, 0x00, 0x00,              // 04 call [rax + 0xF0]
    0x80, 0x3D, 0x2F, 0x00, 0x00, 0x00, 0x00,        // 0A cmp byte [rip + 0x2F] (0x11 + 0x2F = 0x40), 0
    0x74, 0x1C,                                      // 11 je 0x2F
    0x48, 0x8B, 0x87, 0x90, 0x34, 0x00, 0x00,        // 13 mov rax, [rdi + 0x3490]
    0x48, 0x85, 0xC0,                                // 1A test rax, rax
    0x74, 0x10,                                      // 1D je 0x2F
    0x83, 0x48, 0x1C, 0x3F,                          // 1F or dword [rax + 0x1C], 0x3F
    0x83, 0x48, 0x20, 0x3F,                          // 23 or dword [rax + 0x20], 0x3F
    0x83, 0x48, 0x24, 0x3F,                          // 27 or dword [rax + 0x24], 0x3F
    0x83, 0x48, 0x28, 0x30,                          // 2B or dword [rax + 0x28], 0x30
    0x48, 0x83, 0xC4, 0x28,                          // 2F add rsp, 0x28
    0xC3,                                            // 33 ret
};
static_assert(sizeof(kStub) <= kStubFlagAt, "the stub runs into its flag");

using UpdateCommandListFn = void(__fastcall*)(std::uint8_t* context, void* commandList, bool newList);
using TransferCommandListFn = void(__fastcall*)(std::uint8_t* from, std::uint8_t* to);
UpdateCommandListFn g_origUpdateCommandList = nullptr;
TransferCommandListFn g_origTransferCommandList = nullptr;
std::uintptr_t g_base = 0;
std::uint8_t* g_stub = nullptr;
bool g_installed = false;
std::atomic<bool> g_enabled{true};

void* AllocNearGame(std::size_t size) {   // executable memory within +-2 GB of the game's code (a rel32 call)
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const std::uintptr_t gran = std::max<std::uintptr_t>(si.dwAllocationGranularity, 0x10000u);
    for (std::uintptr_t cursor = (g_base + 0x04000000ull + gran - 1) & ~(gran - 1); cursor < g_base + 0x60000000ull;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi))) break;
        const std::uintptr_t regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::uintptr_t regionEnd = regionBase + mbi.RegionSize;
        if (mbi.State == MEM_FREE) {
            const std::uintptr_t candidate = (std::max(cursor, regionBase) + gran - 1) & ~(gran - 1);
            if (candidate < regionEnd && regionEnd - candidate >= size)
                if (void* p = VirtualAlloc(reinterpret_cast<void*>(candidate), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
                    return p;
        }
        if (regionEnd <= cursor) break;
        cursor = (regionEnd + gran - 1) & ~(gran - 1);
    }
    return nullptr;
}

// The context holds the root signature its new list has (the first) and binds every table again.
void Resync(std::uint8_t* context) {
    if (!context || !g_enabled.load(std::memory_order_relaxed)) return;
    __try {
        const std::uint8_t* manager = *reinterpret_cast<const std::uint8_t* const*>(g_base + kRvaDeviceManager);
        if (!manager) return;
        *reinterpret_cast<void**>(context + kCtxRootSignature) = *reinterpret_cast<void* const*>(manager + kManagerRootSignatures);
        *reinterpret_cast<std::uint32_t*>(context + kCtxRootSignatureIndex) = 0;
        if (auto* cache = *reinterpret_cast<std::uint8_t**>(context + kCtxDescriptorCache)) {
            *reinterpret_cast<std::uint32_t*>(cache + kCacheDirtySrv) |= 0x3F;
            *reinterpret_cast<std::uint32_t*>(cache + kCacheDirtyCb) |= 0x3F;
            *reinterpret_cast<std::uint32_t*>(cache + kCacheDirtySampler) |= 0x3F;
            *reinterpret_cast<std::uint32_t*>(cache + kCacheDirtyUav) |= 0x30;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __fastcall HookUpdateCommandList(std::uint8_t* context, void* commandList, bool newList) {
    g_origUpdateCommandList(context, commandList, newList);
    Resync(context);
}

void __fastcall HookTransferCommandList(std::uint8_t* from, std::uint8_t* to) {
    g_origTransferCommandList(from, to);
    Resync(to);
    Resync(from);
}

bool Verify() {
    for (const Check& c : kChecks)
        if (!GameBuild::VerifyBytes(c.rva, c.bytes, c.size)) {
            Log::Writef("Dx12RootFix: %s is not where this framework expects it (0x%llX): the DX12 tessellation fix is off", c.what,
                        static_cast<unsigned long long>(c.rva));
            return false;
        }
    for (const RelCheck& c : kRelChecks) {
        std::int32_t displacement = 0;
        std::memcpy(&displacement, reinterpret_cast<const void*>(g_base + c.rva + c.operand), sizeof(displacement));
        if (c.rva + c.length + std::intptr_t(displacement) != c.target) {
            Log::Writef("Dx12RootFix: %s differs (0x%llX): the DX12 tessellation fix is off", c.what, static_cast<unsigned long long>(c.rva));
            return false;
        }
    }
    if (!GameBuild::VerifyBytes(kRvaUpdateRootSig, kUpdateRootSig, sizeof(kUpdateRootSig))) {
        Log::Write("Dx12RootFix: UpdateCommandList's root signature is not where this framework expects it: the DX12 tessellation fix is off");
        return false;
    }
    return true;
}
}  // namespace

bool Install() {
    if (g_installed) return true;
    g_base = GameBuild::Base();
    if (!g_base || !Verify()) return false;
    g_stub = static_cast<std::uint8_t*>(AllocNearGame(0x1000));
    const std::intptr_t rel = g_stub ? reinterpret_cast<std::intptr_t>(g_stub) - std::intptr_t(g_base + kRvaPreDrawSetRootSig + 5) : 0;
    if (!g_stub || rel < INT32_MIN || rel > INT32_MAX) {
        if (g_stub) VirtualFree(g_stub, 0, MEM_RELEASE);
        g_stub = nullptr;
        Log::Write("Dx12RootFix: no memory near the game's code: the DX12 tessellation fix is off");
        return false;
    }
    std::memcpy(g_stub, kStub, sizeof(kStub));
    g_stub[kStubFlagAt] = g_enabled.load() ? 1 : 0;
    FlushInstructionCache(GetCurrentProcess(), g_stub, kStubFlagAt + 1);

    // the hooks first: nothing patched when they cannot be made
    if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaUpdateCommandList), reinterpret_cast<void*>(&HookUpdateCommandList),
                      reinterpret_cast<void**>(&g_origUpdateCommandList)) != MH_OK) {
        VirtualFree(g_stub, 0, MEM_RELEASE);
        g_stub = nullptr;
        Log::Write("Dx12RootFix: the UpdateCommandList hook could not be made: the DX12 tessellation fix is off");
        return false;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaTransferCommandList), reinterpret_cast<void*>(&HookTransferCommandList),
                      reinterpret_cast<void**>(&g_origTransferCommandList)) != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(g_base + kRvaUpdateCommandList));
        VirtualFree(g_stub, 0, MEM_RELEASE);
        g_stub = nullptr;
        Log::Write("Dx12RootFix: the TransferCommandList hook could not be made: the DX12 tessellation fix is off");
        return false;
    }

    // call stub; nop (in place of call [rax + 0xF0], 6 bytes)
    std::uint8_t call[6] = {0xE8, 0, 0, 0, 0, 0x90};
    const std::int32_t rel32 = static_cast<std::int32_t>(rel);
    std::memcpy(call + 1, &rel32, sizeof(rel32));
    auto* at = reinterpret_cast<std::uint8_t*>(g_base + kRvaPreDrawSetRootSig);
    DWORD old = 0;
    if (!VirtualProtect(at, sizeof(call), PAGE_EXECUTE_READWRITE, &old)) {
        MH_RemoveHook(reinterpret_cast<void*>(g_base + kRvaUpdateCommandList));
        MH_RemoveHook(reinterpret_cast<void*>(g_base + kRvaTransferCommandList));
        VirtualFree(g_stub, 0, MEM_RELEASE);
        g_stub = nullptr;
        Log::Write("Dx12RootFix: PreDraw could not be patched: the DX12 tessellation fix is off");
        return false;
    }
    std::memcpy(at, call, sizeof(call));
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(call));
    VirtualProtect(at, sizeof(call), old, &old);
    g_installed = true;
    Log::Writef("Dx12RootFix: DX12 root signature changes bind every descriptor table again (tessellation's black textures fix: %s)",
                g_enabled.load() ? "on" : "off ([Rendering] Dx12TessellationFix=0)");
    return true;
}

bool Available() {
    return g_installed;
}

bool Enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

void SetEnabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_relaxed);
    if (g_stub) {
        reinterpret_cast<volatile std::uint8_t*>(g_stub)[kStubFlagAt] = enabled ? 1 : 0;
    }
}
}  // namespace Dx12RootFix
