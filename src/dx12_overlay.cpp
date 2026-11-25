#include "dx12_overlay.h"
#include "log.h"
#include "eight_weight_patch.h"
#include "rt_probe.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <MinHook.h>
#include <backends/imgui_impl_dx12.h>

#include <atomic>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cstdio>

namespace {
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

ExecuteCommandListsFn g_originalExecuteCommandListsPrimary = nullptr;
ExecuteCommandListsFn g_originalExecuteCommandListsAlt = nullptr;
bool g_primaryQueueHookCreated = false;
bool g_queueCaptureHookCreated = false;
bool g_queueCaptureHooksRetired = false;
void* g_executeHookTargetPrimary = nullptr;
void* g_executeHookTargetAlt = nullptr;
bool g_swapDeviceTargetProbed = false;
std::atomic<ID3D12CommandQueue*> g_capturedQueue{nullptr};
SRWLOCK g_queueCaptureLock = SRWLOCK_INIT;
ID3D12Device* g_expectedDevice = nullptr; 
bool g_loggedQueueCapture = false;
bool g_loggedQueueDeviceMismatch = false;

ID3D12Device* g_device = nullptr;
ID3D12CommandQueue* g_queue = nullptr;
IDXGISwapChain3* g_swapChain3 = nullptr;
ID3D12DescriptorHeap* g_rtvHeap = nullptr;
ID3D12DescriptorHeap* g_srvHeap = nullptr;
ID3D12GraphicsCommandList* g_commandList = nullptr;
ID3D12Fence* g_fence = nullptr;
HANDLE g_fenceEvent = nullptr;
UINT64 g_lastFenceValue = 0;
UINT g_rtvDescriptorSize = 0;
DXGI_FORMAT g_rtvFormat = DXGI_FORMAT_UNKNOWN;
bool g_rtvIsSrgb = false;
bool g_backendInitialized = false;
bool g_ready = false;

struct FrameContext {
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12Resource* backBuffer = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    UINT64 fenceValue = 0;
};
std::vector<FrameContext> g_frames;

struct SrvDescriptorAllocator {
    ID3D12DescriptorHeap* heap = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuStart{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuStart{};
    UINT increment = 0;
    UINT capacity = 0;
    std::vector<UINT> freeIndices;

    void Reset() {
        heap = nullptr;
        cpuStart.ptr = 0;
        gpuStart.ptr = 0;
        increment = 0;
        capacity = 0;
        freeIndices.clear();
    }

    void Initialize(ID3D12Device* device, ID3D12DescriptorHeap* descriptorHeap, UINT descriptorCount) {
        Reset();
        heap = descriptorHeap;
        capacity = descriptorCount;
        increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        cpuStart = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        gpuStart = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
        freeIndices.reserve(descriptorCount);
        
        for (UINT i = descriptorCount; i > 0; --i)
            freeIndices.push_back(i - 1);
    }

    void Allocate(D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
        if (!outCpu || !outGpu) return;
        outCpu->ptr = 0;
        outGpu->ptr = 0;
        if (!heap || freeIndices.empty() || increment == 0) {
            Log::Write("Overlay: DX12 ImGui SRV descriptor heap exhausted");
            return;
        }
        const UINT index = freeIndices.back();
        freeIndices.pop_back();
        outCpu->ptr = cpuStart.ptr + static_cast<SIZE_T>(index) * increment;
        outGpu->ptr = gpuStart.ptr + static_cast<UINT64>(index) * increment;
    }

    void Free(D3D12_CPU_DESCRIPTOR_HANDLE cpu) {
        if (!heap || increment == 0 || cpu.ptr < cpuStart.ptr) return;
        const SIZE_T delta = cpu.ptr - cpuStart.ptr;
        if (delta % increment != 0) return;
        const UINT index = static_cast<UINT>(delta / increment);
        if (index >= capacity) return;
        if (std::find(freeIndices.begin(), freeIndices.end(), index) == freeIndices.end())
            freeIndices.push_back(index);
    }
};
SrvDescriptorAllocator g_srvAllocator;
constexpr UINT kSrvDescriptorCount = 256;





struct TexturePreviewSlot {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    std::uintptr_t source = 0;
};
struct TexturePreviewFrameSlots {
    std::vector<TexturePreviewSlot> slots;
    std::size_t used = 0;
    bool writable = false;
};
std::vector<TexturePreviewFrameSlots> g_texturePreviewFrames;

bool ReadableRange(const void* ptr, std::size_t bytes) {
    if (!ptr || bytes == 0) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(ptr);
    const auto end = begin + bytes;
    if (end < begin) return false;
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return end <= regionEnd;
}




void* TryGetTextureShaderResourceView(std::uintptr_t textureAddress) noexcept {
    if (!textureAddress) return nullptr;
    void* texture = reinterpret_cast<void*>(textureAddress);
    if (!ReadableRange(texture, sizeof(void*))) return nullptr;

    using GetSrvFn = void*(__fastcall*)(void*);
    void* srv = nullptr;
#if defined(_MSC_VER)
    __try {
#endif
        auto** vtable = *reinterpret_cast<void***>(texture);
        if (!vtable || !ReadableRange(vtable + 14, sizeof(void*))) return nullptr;
        auto getSrv = reinterpret_cast<GetSrvFn>(vtable[14]);
        if (!getSrv) return nullptr;
        srv = getSrv(texture);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
#endif
    return srv;
}

bool TryReadTextureSrvDescriptor(void* srv, D3D12_CPU_DESCRIPTOR_HANDLE* out) noexcept {
    if (out) out->ptr = 0;
    if (!out || !srv ||
        !ReadableRange(reinterpret_cast<const std::uint8_t*>(srv) + 0x68, sizeof(SIZE_T)))
        return false;

#if defined(_MSC_VER)
    __try {
#endif
        out->ptr = *reinterpret_cast<const SIZE_T*>(reinterpret_cast<const std::uint8_t*>(srv) + 0x68);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->ptr = 0;
        return false;
    }
#endif
    return out->ptr != 0;
}

bool IsSrgbFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

bool SetExpectedDevice(ID3D12Device* device) {
    if (!device) return false;
    bool changed = false;
    AcquireSRWLockExclusive(&g_queueCaptureLock);
    if (g_expectedDevice != device) {
        device->AddRef();
        ID3D12Device* old = g_expectedDevice;
        g_expectedDevice = device;
        if (old) old->Release();

        
        
        
        
        
        ID3D12CommandQueue* stale = g_capturedQueue.exchange(nullptr, std::memory_order_acq_rel);
        if (stale) stale->Release();
        g_swapDeviceTargetProbed = false;
        g_queueCaptureHookCreated = g_primaryQueueHookCreated;
        g_loggedQueueCapture = false;
        changed = true;
    }
    ReleaseSRWLockExclusive(&g_queueCaptureLock);
    return changed;
}

ID3D12CommandQueue* AcquireCapturedQueue() {
    ID3D12CommandQueue* result = nullptr;
    AcquireSRWLockShared(&g_queueCaptureLock);
    result = g_capturedQueue.load(std::memory_order_acquire);
    if (result) result->AddRef();
    ReleaseSRWLockShared(&g_queueCaptureLock);
    return result;
}

bool QueueBelongsToExpectedDevice(ID3D12CommandQueue* queue) {
    if (!queue) return false;
    ID3D12Device* expected = nullptr;
    AcquireSRWLockShared(&g_queueCaptureLock);
    expected = g_expectedDevice;
    if (expected) expected->AddRef();
    ReleaseSRWLockShared(&g_queueCaptureLock);
    if (!expected) return true;

    ID3D12Device* queueDevice = nullptr;
    const HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&queueDevice));
    const bool matches = SUCCEEDED(hr) && queueDevice == expected;
    if (queueDevice) queueDevice->Release();
    expected->Release();
    return matches;
}

void CaptureQueue(ID3D12CommandQueue* queue) {
    if (!queue) return;

    
    
    
    ID3D12CommandQueue* current = g_capturedQueue.load(std::memory_order_acquire);
    if (current == queue) return;

    const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    if (current && !QueueBelongsToExpectedDevice(queue)) return;

    AcquireSRWLockExclusive(&g_queueCaptureLock);
    current = g_capturedQueue.load(std::memory_order_relaxed);
    bool accept = (current == nullptr);
    if (!accept && current != queue && g_expectedDevice) {
        ID3D12Device* queueDevice = nullptr;
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) && queueDevice == g_expectedDevice)
            accept = true;
        if (queueDevice) queueDevice->Release();
    }
    if (accept && current != queue) {
        queue->AddRef();
        g_capturedQueue.store(queue, std::memory_order_release);
        if (current) current->Release();
        if (!g_loggedQueueCapture) {
            g_loggedQueueCapture = true;
            Log::Detailf("Overlay: DX12 DIRECT command queue captured at %p", queue);
        }
    }
    ReleaseSRWLockExclusive(&g_queueCaptureLock);
}

void STDMETHODCALLTYPE HookExecuteCommandListsPrimary(ID3D12CommandQueue* self, UINT count,
                                                       ID3D12CommandList* const* lists) {
    CaptureQueue(self);
    if (g_originalExecuteCommandListsPrimary)
        g_originalExecuteCommandListsPrimary(self, count, lists);
}

void STDMETHODCALLTYPE HookExecuteCommandListsAlt(ID3D12CommandQueue* self, UINT count,
                                                   ID3D12CommandList* const* lists) {
    CaptureQueue(self);
    if (g_originalExecuteCommandListsAlt)
        g_originalExecuteCommandListsAlt(self, count, lists);
}

void ReleaseFrameResources() {
    for (FrameContext& frame : g_frames) {
        if (frame.backBuffer) { frame.backBuffer->Release(); frame.backBuffer = nullptr; }
        if (frame.allocator) { frame.allocator->Release(); frame.allocator = nullptr; }
        frame.fenceValue = 0;
        frame.rtv.ptr = 0;
    }
    g_frames.clear();
    if (g_commandList) { g_commandList->Release(); g_commandList = nullptr; }
    if (g_rtvHeap) { g_rtvHeap->Release(); g_rtvHeap = nullptr; }
    if (g_swapChain3) { g_swapChain3->Release(); g_swapChain3 = nullptr; }
    g_rtvDescriptorSize = 0;
}

void WaitForFence(UINT64 value) {
    if (!value || !g_fence || !g_fenceEvent) return;
    if (g_fence->GetCompletedValue() >= value) return;
    if (SUCCEEDED(g_fence->SetEventOnCompletion(value, g_fenceEvent)))
        WaitForSingleObject(g_fenceEvent, INFINITE);
}

// (v0.9.475) the same, up to ms milliseconds: whether the value was reached
bool WaitForFenceFor(UINT64 value, DWORD ms) {
    if (!value || !g_fence || !g_fenceEvent) return true;
    if (g_fence->GetCompletedValue() >= value) return true;
    if (FAILED(g_fence->SetEventOnCompletion(value, g_fenceEvent))) return false;
    return WaitForSingleObject(g_fenceEvent, ms) == WAIT_OBJECT_0 || g_fence->GetCompletedValue() >= value;
}

void WaitForGpuIdle() {
    if (!g_queue || !g_fence || !g_fenceEvent) return;
    const UINT64 value = ++g_lastFenceValue;
    if (SUCCEEDED(g_queue->Signal(g_fence, value)))
        WaitForFence(value);
    for (FrameContext& frame : g_frames)
        frame.fenceValue = 0;
}

void ReleaseRendererResources(bool releaseDeviceAndQueue) {
    if (g_queue && g_fence)
        WaitForGpuIdle();
    RtProbe::OnOverlayReleased();   // (v0.9.543: the GPU idle; its descriptors go with the heap)

    if (g_backendInitialized) {
        ImGui_ImplDX12_Shutdown();
        g_backendInitialized = false;
    }

    ReleaseFrameResources();
    g_texturePreviewFrames.clear();
    g_srvAllocator.Reset();
    if (g_srvHeap) { g_srvHeap->Release(); g_srvHeap = nullptr; }
    if (g_fence) { g_fence->Release(); g_fence = nullptr; }
    if (g_fenceEvent) { CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
    g_lastFenceValue = 0;
    g_rtvFormat = DXGI_FORMAT_UNKNOWN;
    g_rtvIsSrgb = false;
    g_ready = false;

    if (releaseDeviceAndQueue) {
        if (g_queue) { g_queue->Release(); g_queue = nullptr; }
        if (g_device) { g_device->Release(); g_device = nullptr; }
    }
}

bool CreateGpuResources(IDXGISwapChain* swapChain) {
    if (!swapChain || !g_device || !g_queue) return false;

    IDXGISwapChain3* swap3 = nullptr;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swap3))) || !swap3) {
        Log::Write("Overlay: DX12 requires IDXGISwapChain3; QueryInterface failed");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc)) || desc.BufferCount == 0) {
        swap3->Release();
        Log::Write("Overlay: DX12 failed to query swap-chain description");
        return false;
    }

    g_swapChain3 = swap3;
    g_rtvFormat = desc.BufferDesc.Format;
    g_rtvIsSrgb = IsSrgbFormat(g_rtvFormat);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = desc.BufferCount;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_rtvHeap)))) {
        Log::Write("Overlay: DX12 failed to create RTV descriptor heap");
        return false;
    }
    g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = kSrvDescriptorCount;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_srvHeap)))) {
        Log::Write("Overlay: DX12 failed to create shader-visible SRV descriptor heap");
        return false;
    }
    g_srvAllocator.Initialize(g_device, g_srvHeap, kSrvDescriptorCount);

    g_frames.resize(desc.BufferCount);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < desc.BufferCount; ++i) {
        FrameContext& frame = g_frames[i];
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     IID_PPV_ARGS(&frame.allocator)))) {
            Log::Write("Overlay: DX12 failed to create command allocator");
            return false;
        }
        if (FAILED(swapChain->GetBuffer(i, IID_PPV_ARGS(&frame.backBuffer)))) {
            Log::Writef("Overlay: DX12 failed to acquire swap-chain backbuffer %u", i);
            return false;
        }
        frame.rtv = rtv;
        g_device->CreateRenderTargetView(frame.backBuffer, nullptr, frame.rtv);
        rtv.ptr += g_rtvDescriptorSize;
    }

    
    
    
    g_texturePreviewFrames.assign(g_frames.size(), TexturePreviewFrameSlots{});

    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            g_frames.front().allocator, nullptr,
                                            IID_PPV_ARGS(&g_commandList)))) {
        Log::Write("Overlay: DX12 failed to create graphics command list");
        return false;
    }
    
    
    EightWeightPatch::SetWireframeExcludedCommandList(g_commandList);

    
    if (FAILED(g_commandList->Close())) {
        Log::Write("Overlay: DX12 failed to close initial command list");
        return false;
    }

    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
        Log::Write("Overlay: DX12 failed to create synchronization fence");
        return false;
    }
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_fenceEvent) {
        Log::Write("Overlay: DX12 failed to create synchronization event");
        return false;
    }

    ImGui_ImplDX12_InitInfo initInfo{};
    initInfo.Device = g_device;
    initInfo.CommandQueue = g_queue;
    initInfo.NumFramesInFlight = static_cast<int>(g_frames.size());
    initInfo.RTVFormat = g_rtvFormat;
    initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
    initInfo.UserData = &g_srvAllocator;
    initInfo.SrvDescriptorHeap = g_srvHeap;
    initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info,
                                       D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
                                       D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
        auto* allocator = static_cast<SrvDescriptorAllocator*>(info->UserData);
        if (allocator) allocator->Allocate(outCpu, outGpu);
    };
    initInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* info,
                                      D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                      D3D12_GPU_DESCRIPTOR_HANDLE) {
        auto* allocator = static_cast<SrvDescriptorAllocator*>(info->UserData);
        if (allocator) allocator->Free(cpu);
    };

    if (!ImGui_ImplDX12_Init(&initInfo)) {
        Log::Write("Overlay: ImGui DX12 renderer initialization failed");
        return false;
    }
    g_backendInitialized = true;
    g_ready = true;
    Log::Detailf("Overlay: DX12 swap-chain resources initialized (%u buffers, RTV format=%u, sRGB=%s)",
                static_cast<unsigned>(g_frames.size()), static_cast<unsigned>(g_rtvFormat),
                g_rtvIsSrgb ? "yes" : "no");
    Log::Detail("Overlay: ImGui DX12 initialized");
    return true;
}

bool ValidateQueueForDevice(ID3D12CommandQueue* queue, ID3D12Device* device) {
    if (!queue || !device) return false;
    ID3D12Device* queueDevice = nullptr;
    const HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&queueDevice));
    const bool matches = SUCCEEDED(hr) && queueDevice == device;
    if (queueDevice) queueDevice->Release();
    return matches;
}

void DropCapturedQueueIfWrongDevice(ID3D12Device* device) {
    if (!device) return;
    AcquireSRWLockExclusive(&g_queueCaptureLock);
    ID3D12CommandQueue* current = g_capturedQueue.load(std::memory_order_relaxed);
    if (current) {
        ID3D12Device* queueDevice = nullptr;
        const bool matches = SUCCEEDED(current->GetDevice(IID_PPV_ARGS(&queueDevice))) && queueDevice == device;
        if (queueDevice) queueDevice->Release();
        if (!matches) {
            g_capturedQueue.store(nullptr, std::memory_order_release);
            current->Release();
        }
    }
    ReleaseSRWLockExclusive(&g_queueCaptureLock);
}

void RetireQueueCaptureHooksInternal() {
    if (g_queueCaptureHooksRetired) return;

    bool hadTarget = false;
    bool allRetired = true;
    auto retireOne = [&](void* target) {
        if (!target) return;
        hadTarget = true;
        const MH_STATUS st = MH_DisableHook(target);
        if (st != MH_OK && st != MH_ERROR_DISABLED)
            allRetired = false;
    };
    retireOne(g_executeHookTargetPrimary);
    if (g_executeHookTargetAlt != g_executeHookTargetPrimary)
        retireOne(g_executeHookTargetAlt);

    g_queueCaptureHooksRetired = allRetired;
    if (hadTarget && allRetired)
        Log::Detail("Overlay: DX12 queue-capture detour retired from ExecuteCommandLists hot path");
    else if (hadTarget && !allRetired)
        Log::Write("Overlay: warning - DX12 queue-capture detour could not be fully retired");
}
}

namespace Dx12Overlay {
bool CreateHookTargets(HWND dummyWindow, void** outPresent, void** outResizeBuffers) {
    if (outPresent) *outPresent = nullptr;
    if (outResizeBuffers) *outResizeBuffers = nullptr;

    ID3D12Device* device = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) || !device) {
        Log::Write("Overlay: D3D12 runtime unavailable; unified build will use DX11 only on this system");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) || !queue) {
        device->Release();
        Log::Write("Overlay: failed to create dummy D3D12 command queue");
        return false;
    }

    void** queueVtable = *reinterpret_cast<void***>(queue);
    void* executeCommandLists = queueVtable[10];
    const MH_STATUS queueHookStatus = MH_CreateHook(executeCommandLists,
                                                     reinterpret_cast<void*>(&HookExecuteCommandListsPrimary),
                                                     reinterpret_cast<void**>(&g_originalExecuteCommandListsPrimary));
    if (queueHookStatus == MH_OK) {
        g_primaryQueueHookCreated = true;
        g_queueCaptureHookCreated = true;
        g_queueCaptureHooksRetired = false;
        g_executeHookTargetPrimary = executeCommandLists;
        Log::Detail("Overlay: DX12 ExecuteCommandLists queue-capture hook created");
    } else {
        Log::Writef("Overlay: DX12 ExecuteCommandLists hook creation failed (MinHook status=%d)",
                    static_cast<int>(queueHookStatus));
    }

    
    
    
    
    IDXGIFactory4* factory = nullptr;
    IDXGISwapChain1* swap1 = nullptr;
    if (dummyWindow && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory) {
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 100;
        sd.Height = 100;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        if (SUCCEEDED(factory->CreateSwapChainForHwnd(queue, dummyWindow, &sd, nullptr, nullptr, &swap1)) && swap1) {
            IDXGISwapChain* swap = nullptr;
            if (SUCCEEDED(swap1->QueryInterface(IID_PPV_ARGS(&swap))) && swap) {
                void** swapVtable = *reinterpret_cast<void***>(swap);
                if (outPresent) *outPresent = swapVtable[8];
                if (outResizeBuffers) *outResizeBuffers = swapVtable[13];
                swap->Release();
            }
        }
    }

    if (swap1) swap1->Release();
    if (factory) factory->Release();
    queue->Release();
    device->Release();
    return g_queueCaptureHookCreated;
}

bool QueueCaptureHookCreated() { return g_queueCaptureHookCreated; }
bool QueueCaptured() {
    return g_capturedQueue.load(std::memory_order_acquire) != nullptr;
}

void RetireQueueCaptureHooks() {
    RetireQueueCaptureHooksInternal();
}

bool PrimeQueueCaptureForSwapDevice(IDXGISwapChain* swapChain) {
    if (!swapChain) return false;
    ID3D12Device* device = nullptr;
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))) || !device)
        return false;

    const bool deviceChanged = SetExpectedDevice(device);
    if (!deviceChanged)
        DropCapturedQueueIfWrongDevice(device);

    
    
    
    if (g_swapDeviceTargetProbed) {
        device->Release();
        return g_queueCaptureHookCreated;
    }

    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* probeQueue = nullptr;
    if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&probeQueue))) || !probeQueue) {
        device->Release();
        return false;
    }
    void** vtable = *reinterpret_cast<void***>(probeQueue);
    void* target = vtable[10];
    g_swapDeviceTargetProbed = true;

    bool ok = g_queueCaptureHookCreated;
    if (target && target != g_executeHookTargetPrimary && target != g_executeHookTargetAlt) {
        
        
        
        const MH_STATUS createStatus = MH_CreateHook(target, reinterpret_cast<void*>(&HookExecuteCommandListsAlt),
                                                      reinterpret_cast<void**>(&g_originalExecuteCommandListsAlt));
        if (createStatus == MH_OK) {
            const MH_STATUS enableStatus = MH_EnableHook(target);
            if (enableStatus == MH_OK) {
                g_executeHookTargetAlt = target;
                g_queueCaptureHookCreated = true;
                g_queueCaptureHooksRetired = false;
                ok = true;
                Log::Detail("Overlay: DX12 queue-capture hook added for ROTTR's swap-chain device");
            } else {
                MH_RemoveHook(target);
                g_originalExecuteCommandListsAlt = nullptr;
                g_queueCaptureHookCreated = false;
                ok = false;
                Log::Writef("Overlay: DX12 game-device queue hook enable failed (MinHook status=%d)",
                            static_cast<int>(enableStatus));
            }
        } else {
            g_queueCaptureHookCreated = false;
            ok = false;
            Log::Writef("Overlay: DX12 game-device queue hook creation failed (MinHook status=%d)",
                        static_cast<int>(createStatus));
        }
    }

    probeQueue->Release();
    device->Release();
    return ok;
}

bool TryInitialize(IDXGISwapChain* swapChain) {
    if (g_ready) return true;
    if (!swapChain) return false;

    ID3D12Device* swapDevice = nullptr;
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&swapDevice))) || !swapDevice)
        return false;

    SetExpectedDevice(swapDevice);
    ID3D12CommandQueue* queue = AcquireCapturedQueue();
    if (!queue || !ValidateQueueForDevice(queue, swapDevice)) {
        if (queue && !g_loggedQueueDeviceMismatch) {
            g_loggedQueueDeviceMismatch = true;
            Log::Detail("Overlay: captured DX12 queue belongs to a different device; waiting for ROTTR's DIRECT queue");
        }
        if (queue) queue->Release();
        swapDevice->Release();
        return false;
    }
    g_loggedQueueDeviceMismatch = false;

    if (g_device) g_device->Release();
    if (g_queue) g_queue->Release();
    g_device = swapDevice; 
    g_queue = queue;       

    if (!CreateGpuResources(swapChain)) {
        ReleaseRendererResources(true);
        return false;
    }

    
    
    
    
    RetireQueueCaptureHooksInternal();
    return true;
}

bool Ready() { return g_ready; }

bool GetTexturePreviewId(std::uintptr_t textureAddress, std::uint64_t* outTextureId) {
    if (outTextureId) *outTextureId = 0;
    if (!outTextureId || !g_ready || !g_device || !g_srvHeap || !g_swapChain3 ||
        !g_fence || !textureAddress || g_frames.empty())
        return false;

    const UINT frameIndex = g_swapChain3->GetCurrentBackBufferIndex();
    if (frameIndex >= g_frames.size()) return false;
    if (g_texturePreviewFrames.size() != g_frames.size())
        g_texturePreviewFrames.assign(g_frames.size(), TexturePreviewFrameSlots{});

    TexturePreviewFrameSlots& previews = g_texturePreviewFrames[frameIndex];
    if (!previews.writable) return false;

    
    
    
    for (std::size_t i = 0; i < previews.used && i < previews.slots.size(); ++i) {
        const TexturePreviewSlot& existing = previews.slots[i];
        if (existing.source == textureAddress && existing.gpu.ptr) {
            *outTextureId = static_cast<std::uint64_t>(existing.gpu.ptr);
            return true;
        }
    }

    const std::size_t slotIndex = previews.used;
    if (slotIndex >= previews.slots.size()) {
        TexturePreviewSlot slot{};
        g_srvAllocator.Allocate(&slot.cpu, &slot.gpu);
        if (!slot.cpu.ptr || !slot.gpu.ptr) return false;
        previews.slots.push_back(slot);
    }
    TexturePreviewSlot& preview = previews.slots[slotIndex];

    
    
    
    if (preview.source == textureAddress && preview.cpu.ptr && preview.gpu.ptr) {
        ++previews.used;
        *outTextureId = static_cast<std::uint64_t>(preview.gpu.ptr);
        return true;
    }

    
    
    
    
    
    void* srv = TryGetTextureShaderResourceView(textureAddress);
    if (!srv) return false;

    
    
    D3D12_CPU_DESCRIPTOR_HANDLE source{};
    if (!TryReadTextureSrvDescriptor(srv, &source)) return false;

    g_device->CopyDescriptorsSimple(1, preview.cpu, source,
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    preview.source = textureAddress;
    ++previews.used;
    *outTextureId = static_cast<std::uint64_t>(preview.gpu.ptr);
    return true;
}

ID3D12CommandQueue* Queue() { return g_queue; }   // (v0.9.561)

std::uint64_t TimestampFrequency() {   // (v0.9.555)
    UINT64 frequency = 0;
    if (!g_queue || FAILED(g_queue->GetTimestampFrequency(&frequency))) return 0;
    return frequency;
}

bool AllocateDescriptor(std::uint64_t* cpu, std::uint64_t* gpu) {   // (v0.9.543)
    if (!cpu || !gpu || !g_ready || !g_srvHeap) return false;
    D3D12_CPU_DESCRIPTOR_HANDLE c{};
    D3D12_GPU_DESCRIPTOR_HANDLE g{};
    g_srvAllocator.Allocate(&c, &g);
    *cpu = c.ptr;
    *gpu = g.ptr;
    return c.ptr && g.ptr;
}

void NewFrame() {
    if (!g_ready) return;

    
    
    
    if (g_swapChain3 && g_fence && !g_frames.empty()) {
        const UINT frameIndex = g_swapChain3->GetCurrentBackBufferIndex();
        if (frameIndex < g_frames.size()) {
            if (g_texturePreviewFrames.size() != g_frames.size())
                g_texturePreviewFrames.assign(g_frames.size(), TexturePreviewFrameSlots{});
            const FrameContext& frame = g_frames[frameIndex];
            auto& previews = g_texturePreviewFrames[frameIndex];
            previews.writable = !frame.fenceValue || g_fence->GetCompletedValue() >= frame.fenceValue;
            if (previews.writable) previews.used = 0;
        }
    }

    ImGui_ImplDX12_NewFrame();
}

bool Render(IDXGISwapChain* swapChain, bool clearBackBufferBlack) {
    if (!g_ready || !g_swapChain3 || !g_commandList || !g_queue || !g_fence || g_frames.empty())
        return false;
    if (!swapChain) return false;

    const UINT frameIndex = g_swapChain3->GetCurrentBackBufferIndex();
    if (frameIndex >= g_frames.size()) return false;
    FrameContext& frame = g_frames[frameIndex];
    if (!frame.allocator || !frame.backBuffer) return false;

    
    
    
    
    if (frame.fenceValue && g_fence->GetCompletedValue() < frame.fenceValue) {
        
        
        
        // (v0.9.475) waited for, up to 250 ms, instead of the frame going out without the overlay: during level loads
        // (frames presented without waiting, faster than the GPU gives them back) that skipped many frames and the F8
        // menu flickered on and off
        if (clearBackBufferBlack)
            WaitForFence(frame.fenceValue);
        else if (!WaitForFenceFor(frame.fenceValue, 250))
            return true;
    }
    frame.fenceValue = 0;

    if (FAILED(frame.allocator->Reset())) return false;
    if (FAILED(g_commandList->Reset(frame.allocator, nullptr))) return false;

    D3D12_RESOURCE_BARRIER toRender{};
    toRender.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRender.Transition.pResource = frame.backBuffer;
    toRender.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toRender.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRender.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_commandList->ResourceBarrier(1, &toRender);

    g_commandList->OMSetRenderTargets(1, &frame.rtv, FALSE, nullptr);
    if (clearBackBufferBlack) {
        const FLOAT black[4]{0.0f, 0.0f, 0.0f, 1.0f};
        g_commandList->ClearRenderTargetView(frame.rtv, black, 0, nullptr);
    }

    ID3D12DescriptorHeap* heaps[] = { g_srvHeap };
    g_commandList->SetDescriptorHeaps(1, heaps);
    {   // (v0.9.543) the ray tracing probe's builds and view, before ImGui draws it
        const D3D12_RESOURCE_DESC back = frame.backBuffer->GetDesc();
        RtProbe::RecordFrame(g_device, g_commandList, frameIndex, static_cast<unsigned>(back.Width), back.Height);
    }
    g_commandList->SetDescriptorHeaps(1, heaps);   // (v0.9.552: the probe binds its own heap for its view)
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_commandList);

    std::swap(toRender.Transition.StateBefore, toRender.Transition.StateAfter);
    g_commandList->ResourceBarrier(1, &toRender);
    if (FAILED(g_commandList->Close())) return false;

    ID3D12CommandList* lists[] = { g_commandList };
    g_queue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++g_lastFenceValue;
    if (FAILED(g_queue->Signal(g_fence, fenceValue))) return false;
    frame.fenceValue = fenceValue;
    return true;
}

void BeforeResize() {
    if (!g_device && !g_queue) return;
    ReleaseRendererResources(false);
}

bool AfterResize(IDXGISwapChain* swapChain) {
    if (!g_device || !g_queue) return false;
    if (CreateGpuResources(swapChain)) return true;
    ReleaseRendererResources(false);
    return false;
}

void Shutdown() {
    ReleaseRendererResources(true);

    AcquireSRWLockExclusive(&g_queueCaptureLock);
    ID3D12CommandQueue* captured = g_capturedQueue.exchange(nullptr, std::memory_order_acq_rel);
    if (captured) captured->Release();
    if (g_expectedDevice) { g_expectedDevice->Release(); g_expectedDevice = nullptr; }
    ReleaseSRWLockExclusive(&g_queueCaptureLock);
}

DXGI_FORMAT RenderTargetFormat() { return g_rtvFormat; }
bool RenderTargetIsSrgb() { return g_rtvIsSrgb; }

const char* Status() {
    if (!g_queueCaptureHookCreated) return "DX12 queue-capture hook unavailable";
    if (!QueueCaptured()) return "waiting for DX12 DIRECT command queue";
    if (!g_ready) return "DX12 command queue captured; waiting for swap-chain initialization";
    return "DX12 ImGui renderer ready";
}
}
