#include "blendshape_patch.h"
#include "game_build.h"
#include "log.h"
#include "settings.h"

#include <Windows.h>
#include <MinHook.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

#pragma intrinsic(_ReturnAddress)

namespace {
// LoadSharedResources: mov r8d, 0xA00000 / mov rdx, rbx / mov rcx, rax / call TransientGPUBuffer::TransientGPUBuffer
// followed by the store to CommonRenderDevice+0x1FB98 (the blendshape output buffer).
constexpr std::uintptr_t kRvaBlendShapeBufferSize = 0x002B80B9ull;
constexpr std::uint8_t kBlendShapeBufferSizeBytes[] = {
    0x41, 0xB8, 0x00, 0x00, 0xA0, 0x00,   // mov r8d, 0xA00000
    0x48, 0x8B, 0xD3,                     // mov rdx, rbx
    0x48, 0x8B, 0xC8,                     // mov rcx, rax
    0xE8, 0x36, 0xAA, 0xFD, 0xFF          // call 0x292B00 (TransientGPUBuffer ctor)
};
constexpr std::uintptr_t kRvaBlendShapeBufferStore = 0x002B80D4ull;
constexpr std::uint8_t kBlendShapeBufferStoreBytes[] = {
    0x48, 0x89, 0x83, 0x98, 0xFB, 0x01, 0x00   // mov [rbx+0x1FB98], rax
};

// TransientGPUBuffer::Alloc(uint32_t* outOffset, uint32_t size) -> bool
constexpr std::uintptr_t kRvaTransientAlloc = 0x00296170ull;
constexpr std::uint8_t kTransientAllocBytes[] = {
    0x4C, 0x8B, 0xCA,                 // mov r9, rdx
    0x41, 0x8B, 0xC0,                 // mov eax, r8d
    0xF0, 0x0F, 0xC1, 0x41, 0x10,     // lock xadd [rcx+0x10], eax
    0x0F, 0x1F, 0x44, 0x00, 0x00,     // nop
    0x8B, 0x41, 0x0C,                 // mov eax, [rcx+0xC]
    0x42, 0x8D, 0x14, 0x00,           // lea edx, [rax+r8]
    0x3B, 0x51, 0x08                  // cmp edx, [rcx+8]
};

// CommonRenderModelInstance::DrawEx per-mesh blendshape allocation (return address after the call).
constexpr std::uintptr_t kRvaDrawExAllocCall = 0x00278839ull;
constexpr std::uint8_t kDrawExAllocCallBytes[] = {0xE8, 0x32, 0xD9, 0x01, 0x00};
constexpr std::uintptr_t kRvaDrawExAllocReturn = kRvaDrawExAllocCall + 5u;

// Destruction guard (v0.9.304). A CommonRenderModel whose asynchronous creation is still queued when it is destroyed
// has it cancelled: ~CommonRenderModel (0x293690) calls the render device's queue removal (0x2BC750(queue, &model->
// +0x18), true when the request was still queued) and then skips the wait for the creation. Its mesh loop then frees
// each mesh's blendshape set through mesh+0x38 - but that field only becomes a pointer in CreateBlendshapes (0x29CB80,
// run by the creation: +0x38 = u32 offset + model data base +0x50). A model unloaded before its creation ran (an outfit
// swap loading and dropping a model quickly) still holds the raw offset there and the game crashed reading it. After a
// successful removal from the destructor, the meshes whose +0x38 is still an offset (below the data base) get 0, which
// the loop skips: nothing was created for them.
constexpr std::uintptr_t kRvaRenderQueueRemove = 0x002BC750ull;
constexpr std::uint8_t kRenderQueueRemoveSig[] = {
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48,0x8D,0x99,0x00,0x00,0x01,0x00,
    0x48,0x8B,0xF1,0x48,0x8B,0xCB,0x48,0x8B,0xFA};
constexpr std::uintptr_t kRvaModelDtorRemoveCall = 0x002936D5ull;  // lea rdx,[r14+18h] / call remove / test al,al
constexpr std::uint8_t kModelDtorRemoveCallSig[] = {0x49,0x8D,0x56,0x18,0xE8,0x72,0x90,0x02,0x00,0x84,0xC0};
constexpr std::uintptr_t kRvaModelDtorRemoveReturn = 0x002936DEull;
constexpr std::uintptr_t kRvaModelDtorMeshLoop = 0x002937A2ull;    // mov rax,[rdi+38h] / mov [rdi+18h],r15 / test / je
constexpr std::uint8_t kModelDtorMeshLoopSig[] = {0x48,0x8B,0x47,0x38,0x4C,0x89,0x7F,0x18,0x48,0x85,0xC0,0x74,0x43};
constexpr std::size_t kModelDataBase = 0x50, kModelMeshCount = 0x68, kModelMeshes = 0x70;
constexpr std::size_t kMeshStride = 0x50, kMeshBlendShapes = 0x38;

constexpr std::uint32_t kVanillaBytes = 0x00A00000u;   // 10 MB
constexpr std::uint32_t kStrideVertBS = 24u;           // cdc::g_iStrideVertBS

// TransientGPUBuffer layout (retail): +0x00 IRenderBuffer*, +0x08 capacity, +0x0C used,
// +0x10 requested this frame, +0x14 peak requested (both reset/updated once per frame).
struct TransientGPUBuffer {
    void* buffer;
    std::uint32_t capacity;
    std::uint32_t used;
    std::uint32_t requestedThisFrame;
    std::uint32_t peakRequested;
};

using AllocFn = bool (__fastcall*)(TransientGPUBuffer*, std::uint32_t*, std::uint32_t);
AllocFn g_realAlloc = nullptr;
std::uintptr_t g_drawExReturn = 0;
std::uint32_t g_wantedBytes = kVanillaBytes;

std::atomic_bool g_installed{false};
std::atomic_bool g_capacityLogged{false};
std::atomic_uint32_t g_capacity{0};
std::atomic_uint32_t g_peak{0};
std::atomic_uint32_t g_failures{0};
std::atomic_uint32_t g_nextPeakReportPct{50};

using RenderQueueRemoveFn = bool(__fastcall*)(void* queue, void* request);
RenderQueueRemoveFn g_realQueueRemove = nullptr;
std::uintptr_t g_modelDtorRemoveReturn = 0;
std::atomic_uint32_t g_uncreatedModelsGuarded{0};

// The meshes of a model whose creation was just cancelled: +0x38 offsets that were never turned into pointers -> 0.
unsigned int ClearUncreatedBlendShapes(std::uint8_t* model) {
    unsigned int cleared = 0;
    __try {
        const auto base = *reinterpret_cast<const std::uintptr_t*>(model + kModelDataBase);
        const std::uint16_t count = *reinterpret_cast<const std::uint16_t*>(model + kModelMeshCount);
        auto* meshes = *reinterpret_cast<std::uint8_t* const*>(model + kModelMeshes);
        if (!base || !meshes || count > 4096) return 0;
        for (std::uint16_t i = 0; i < count; ++i) {
            auto& field = *reinterpret_cast<std::uintptr_t*>(meshes + static_cast<std::size_t>(i) * kMeshStride + kMeshBlendShapes);
            if (field != 0 && field < base) {
                field = 0;
                ++cleared;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return cleared;
}

bool __fastcall HookQueueRemove(void* queue, void* request) {
    const bool removed = g_realQueueRemove(queue, request);
    if (removed && request && reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == g_modelDtorRemoveReturn) {
        auto* model = static_cast<std::uint8_t*>(request) - 0x18;
        if (const unsigned int cleared = ClearUncreatedBlendShapes(model)) {
            const std::uint32_t n = g_uncreatedModelsGuarded.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 20u)
                Log::Detailf("BlendShapes: render model %p destroyed before its creation ran; %u uncreated blendshape "
                            "set(s) skipped (#%u)", static_cast<void*>(model), cleared, n);
        }
    }
    return removed;
}

bool PatchImm32(std::uintptr_t address, std::uint32_t value) {
    DWORD oldProtect = 0;
    auto* at = reinterpret_cast<void*>(address);
    if (!VirtualProtect(at, sizeof(value), PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    std::memcpy(at, &value, sizeof(value));
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(value));
    DWORD ignored = 0;
    VirtualProtect(at, sizeof(value), oldProtect, &ignored);
    return true;
}

void ReportPeak(std::uint32_t demand, std::uint32_t capacity) {
    std::uint32_t seen = g_peak.load(std::memory_order_relaxed);
    while (demand > seen && !g_peak.compare_exchange_weak(seen, demand, std::memory_order_relaxed)) {}
    if (demand <= seen || capacity == 0)
        return;

    const std::uint32_t pct = static_cast<std::uint32_t>((static_cast<std::uint64_t>(demand) * 100u) / capacity);
    std::uint32_t next = g_nextPeakReportPct.load(std::memory_order_relaxed);
    if (pct < next || next > 100u)
        return;
    if (!g_nextPeakReportPct.compare_exchange_strong(next, (pct / 25u + 1u) * 25u))
        return;
    Log::Detailf("BlendShapes: per-frame demand reached %.2f MB of %.2f MB (%u%%, ~%u vertices)",
                demand / 1048576.0, capacity / 1048576.0, pct, demand / kStrideVertBS);
}

bool __fastcall HookAlloc(TransientGPUBuffer* self, std::uint32_t* outOffset, std::uint32_t size) {
    const bool ok = g_realAlloc(self, outOffset, size);
    if (reinterpret_cast<std::uintptr_t>(_ReturnAddress()) != g_drawExReturn || !self)
        return ok;

    const std::uint32_t capacity = self->capacity;
    if (!g_capacityLogged.exchange(true)) {
        g_capacity.store(capacity);
        if (capacity < g_wantedBytes)
            Log::Writef("BlendShapes: WARNING buffer was created at %.2f MB before the size patch applied (wanted %u MB)",
                        capacity / 1048576.0, g_wantedBytes >> 20);
        else
            Log::Detailf("BlendShapes: output buffer active at %.2f MB (vanilla 10 MB)", capacity / 1048576.0);
    }

    ReportPeak(self->requestedThisFrame, capacity);

    if (!ok && outOffset) {
        // Point this mesh past the end of the buffer (rounded up to a whole 24-byte vertex): D3D drops
        // out-of-range UAV writes and returns 0 for out-of-range reads, so the mesh renders without
        // morph deltas this frame instead of with another mesh's (or stale) data.
        *outOffset = ((capacity + kStrideVertBS - 1u) / kStrideVertBS) * kStrideVertBS;
        const std::uint32_t failures = g_failures.fetch_add(1u) + 1u;
        if (failures == 1u || (failures % 600u) == 0u) {
            Log::Writef("BlendShapes: buffer full, mesh of %u vertices skipped (frame demand %.2f MB > %.2f MB, %u skips so far). "
                        "Raise [Rendering] BlendShapeBufferMB in ROTTRFramework.ini.",
                        size / kStrideVertBS, self->requestedThisFrame / 1048576.0, capacity / 1048576.0, failures);
        }
    }
    return ok;
}
}

namespace BlendShapePatch {
bool Installed() { return g_installed.load(); }

Stats GetStats() {
    return Stats{g_capacity.load(), g_peak.load(), g_failures.load()};
}

bool Install() {
    const std::uintptr_t base = GameBuild::Base();
    if (!base) return false;

    const int wantedMb = std::clamp(Settings::Get().blendShapeBufferMB, 10, 512);
    g_wantedBytes = static_cast<std::uint32_t>(wantedMb) << 20;

    bool sizeOk = false;
    if (!GameBuild::VerifyBytes(kRvaBlendShapeBufferSize, kBlendShapeBufferSizeBytes, sizeof(kBlendShapeBufferSizeBytes)) ||
        !GameBuild::VerifyBytes(kRvaBlendShapeBufferStore, kBlendShapeBufferStoreBytes, sizeof(kBlendShapeBufferStoreBytes))) {
        Log::Write("BlendShapes: LoadSharedResources buffer-size signature mismatch; size not changed");
    } else if (g_wantedBytes == kVanillaBytes) {
        Log::Detail("BlendShapes: buffer size left at vanilla 10 MB ([Rendering] BlendShapeBufferMB=10)");
        sizeOk = true;
    } else if (PatchImm32(base + kRvaBlendShapeBufferSize + 2u, g_wantedBytes)) {
        Log::Detailf("BlendShapes: output buffer size patched 10 MB -> %d MB", wantedMb);
        sizeOk = true;
    } else {
        Log::Write("BlendShapes: VirtualProtect failed; buffer size not changed");
    }

    bool hookOk = false;
    if (!GameBuild::VerifyBytes(kRvaTransientAlloc, kTransientAllocBytes, sizeof(kTransientAllocBytes)) ||
        !GameBuild::VerifyBytes(kRvaDrawExAllocCall, kDrawExAllocCallBytes, sizeof(kDrawExAllocCallBytes))) {
        Log::Write("BlendShapes: TransientGPUBuffer::Alloc / DrawEx signature mismatch; overflow guard not installed");
    } else {
        g_drawExReturn = base + kRvaDrawExAllocReturn;
        void* target = reinterpret_cast<void*>(base + kRvaTransientAlloc);
        const MH_STATUS create = MH_CreateHook(target, reinterpret_cast<void*>(&HookAlloc),
                                               reinterpret_cast<void**>(&g_realAlloc));
        if (create != MH_OK) {
            Log::Writef("BlendShapes: MinHook Alloc creation failed (status=%d)", static_cast<int>(create));
        } else {
            const MH_STATUS enable = MH_EnableHook(target);
            if (enable != MH_OK)
                Log::Writef("BlendShapes: MinHook Alloc enable failed (status=%d)", static_cast<int>(enable));
            else
                hookOk = true;
        }
    }
    if (hookOk)
        Log::Detail("BlendShapes: overflow guard installed on TransientGPUBuffer::Alloc (DrawEx path)");

    if (!GameBuild::VerifyBytes(kRvaRenderQueueRemove, kRenderQueueRemoveSig, sizeof(kRenderQueueRemoveSig)) ||
        !GameBuild::VerifyBytes(kRvaModelDtorRemoveCall, kModelDtorRemoveCallSig, sizeof(kModelDtorRemoveCallSig)) ||
        !GameBuild::VerifyBytes(kRvaModelDtorMeshLoop, kModelDtorMeshLoopSig, sizeof(kModelDtorMeshLoopSig))) {
        Log::Write("BlendShapes: render model destructor signature mismatch; uncreated-model guard not installed");
    } else {
        g_modelDtorRemoveReturn = base + kRvaModelDtorRemoveReturn;
        void* target = reinterpret_cast<void*>(base + kRvaRenderQueueRemove);
        if (MH_CreateHook(target, reinterpret_cast<void*>(&HookQueueRemove), reinterpret_cast<void**>(&g_realQueueRemove)) == MH_OK &&
            MH_EnableHook(target) == MH_OK)
            Log::Detail("BlendShapes: guard for render models destroyed before their creation installed");
        else
            Log::Write("BlendShapes: could not hook the render queue removal; uncreated-model guard not installed");
    }

    g_installed.store(sizeOk || hookOk);
    return sizeOk && hookOk;
}
}
