#include "eight_weight_patch.h"
#include "dxbc_eight_weight_rewriter.h"
#include "log.h"
#include "rt_probe.h"
#include "shadow_blend.h"

#include <MinHook.h>

#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgiformat.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
constexpr std::uintptr_t kRvaVertexElemGetSize = 0x0027BF60ull;
constexpr std::uintptr_t kRvaRendererTypeToFormat = 0x0030F4E0ull;
constexpr std::uintptr_t kRvaIatGetProcAddress = 0x00D01888ull;
constexpr std::uint8_t kTypeUShort4EightWeights = 22u;
constexpr std::uint8_t kTypeWeightsUHalf4EightWeights = 23u;
constexpr std::size_t kCreateGraphicsPipelineStateSlot = 10u;
constexpr std::size_t kCreateCommandListSlot = 12u;
constexpr std::size_t kCommandListResetSlot = 10u;
constexpr std::size_t kSetPipelineStateSlot = 25u;

constexpr std::array<std::uint8_t, 16> kGetSizePrologue{
    0x0F,0xB6,0x41,0x06,0x83,0xF8,0x15,0x77,0x2B,0x48,0x8D,0x15,0x90,0x40,0xD8,0xFF
};
constexpr std::array<std::uint8_t, 15> kRendererMapPrologue{
    0x83,0xF9,0x14,0x77,0x6A,0x48,0x63,0xC1,0x48,0x8D,0x15,0x11,0x0B,0xCF,0xFF
};

using CreateGraphicsPipelineStateFn = HRESULT (STDMETHODCALLTYPE*)(
    ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using CreateCommandListFn = HRESULT (STDMETHODCALLTYPE*)(
    ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
    ID3D12PipelineState*, REFIID, void**);
using CommandListResetFn = HRESULT (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using SetPipelineStateFn = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using GetProcAddressFn = FARPROC (WINAPI*)(HMODULE, LPCSTR);
using D3D12CreateDeviceFn = HRESULT (WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

std::uintptr_t g_base = 0;
std::atomic_bool g_installed{false};
std::atomic_bool g_dx12Attached{false};
std::atomic_uint32_t g_seenType22{0};
std::atomic_uint32_t g_seenType23{0};
std::atomic_uint32_t g_seenRendererCustom{0};
std::atomic_uint32_t g_seenCustomPso{0};
CreateGraphicsPipelineStateFn g_realCreateGraphicsPipelineState = nullptr;
CreateCommandListFn g_realCreateCommandList = nullptr;
CommandListResetFn g_realCommandListReset = nullptr;
SetPipelineStateFn g_realSetPipelineState = nullptr;
GetProcAddressFn g_realGetProcAddress = nullptr;
D3D12CreateDeviceFn g_realD3D12CreateDevice = nullptr;
std::atomic_uint32_t g_seenEarlyD3D12Lookup{0};

std::atomic_bool g_wireframeEnabled{false};
std::atomic_bool g_wireframeCommandListHookAttached{false};
std::atomic<ID3D12GraphicsCommandList*> g_wireframeExcludedCommandList{nullptr};
// (v0.9.607) Draw wireframe: each of the game's geometry draws drawn again right after itself with its lines' pipeline
// (WireframeRecord::Lines, below). Its options (EightWeightPatch::WireframeOptions); the generation changes with every
// option the lines' and fills' pipelines are made with (all but the style), the epoch whenever the command lists' kept
// bindings must not be used any more (switched on, options changed).
std::atomic_bool g_wireframeDrawsHooked{false};
std::atomic<std::uint32_t> g_wireframeGeneration{1};
std::atomic<std::uint32_t> g_wireframeEpoch{1};
std::atomic_int g_wireframeStyle{0};   // 0 over the picture, 1 lines only (the surfaces flat)
std::atomic_bool g_wireframeSeeThrough{false}, g_wireframeBackFaces{false};
std::mutex g_wireframeOptionsMutex;   // the colours
float g_wireframeLineColor[4] = {0.35f, 0.85f, 1.0f, 1.0f};
float g_wireframeFillColor[3] = {0.08f, 0.08f, 0.09f};
std::atomic_uint32_t g_wireframeMirrorsCreated{0};   // lines' pipelines made this session
std::atomic_uint32_t g_wireframeFillsCreated{0};     // flat surfaces' pipelines (lines only)
std::atomic_uint32_t g_wireframeMirrorFailures{0};
std::atomic_uint32_t g_wireframeLinesThisTime{0};    // lines' pipelines made since it was switched on
std::atomic_uint64_t g_wireframeDrawsReplayed{0};    // draws drawn again with lines since it was switched on
std::atomic_uint32_t g_wireframeRecordsKept{0};
std::atomic_uint32_t g_wireframeRecordsLive{0};
std::atomic_uint32_t g_wireframeOverlayLive{0};      // of the live records: pipelines the lines are drawn for
// The game's depth convention, from its depth-writing pipelines: a LESS / LESS_EQUAL test (near = 0) or GREATER /
// GREATER_EQUAL (reversed depth, near = 1). The lines' test and bias follow the majority.
std::atomic_uint32_t g_depthLessPipelines{0}, g_depthGreaterPipelines{0};
std::atomic_uint64_t g_wireframeShaderBytesLive{0};
constexpr std::size_t kWireframeMaxShaderBytes = 16u * 1024u * 1024u;
constexpr UINT kWireframeMaxLayoutElements = 64u;
constexpr UINT kWireframeMaxStreamOutputEntries = 256u;

// Private-data key of the description kept on every game graphics PSO
const GUID kWireframeRecordGuid =
    {0x6b0e5c0b, 0x5c0d, 0x4f53, {0x9c, 0x57, 0x52, 0xd7, 0xa1, 0xe0, 0xb7, 0xc4}};

using WireframeShaderBlob = std::vector<std::uint8_t>;
struct WireframeShaderKey {
    std::uint64_t hash{};
    std::size_t size{};
    bool operator==(const WireframeShaderKey& other) const noexcept {
        return hash == other.hash && size == other.size;
    }
};
struct WireframeShaderKeyHash {
    std::size_t operator()(const WireframeShaderKey& key) const noexcept {
        return static_cast<std::size_t>(key.hash ^ (static_cast<std::uint64_t>(key.size) * 0x9E3779B97F4A7C15ull));
    }
};
std::mutex g_wireframeShaderMutex;
std::unordered_map<WireframeShaderKey, std::weak_ptr<const WireframeShaderBlob>, WireframeShaderKeyHash> g_wireframeShaders;
std::size_t g_wireframeShaderPurgeAt = 1024u;

// (v0.9.607) one lock per record's address stripe (was one lock for every wireframe pipeline: the render threads
// waited on each other's driver calls)
std::array<std::mutex, 64> g_wireframeCreateStripes;
std::mutex& WireframeCreateLock(const void* record) {
    return g_wireframeCreateStripes[(reinterpret_cast<std::uintptr_t>(record) >> 6) % g_wireframeCreateStripes.size()];
}
std::mutex g_wireframePendingMutex;
// (v0.9.607) with the time each was let go (GetTickCount64): released kPendingReleaseMs later
std::vector<std::pair<IUnknown*, ULONGLONG>> g_wireframePendingReleases;
constexpr ULONGLONG kPendingReleaseMs = 3000;
// Caller holds g_wireframePendingMutex.
void PendRelease(IUnknown* object) {
    if (!object) return;
    try {
        g_wireframePendingReleases.emplace_back(object, GetTickCount64());
    } catch (...) {
    }
}

std::atomic_flag g_shaderSeenLock = ATOMIC_FLAG_INIT;
std::array<std::uint64_t, 128> g_seenShaderHashes{};
std::size_t g_seenShaderHashCount = 0;

bool BytesEqual(const void* a, const void* b, std::size_t count) {
    return std::memcmp(a, b, count) == 0;
}

void WriteAbsoluteJump(std::uint8_t* dst, const void* target) {
    dst[0] = 0xFF;
    dst[1] = 0x25;
    dst[2] = dst[3] = dst[4] = dst[5] = 0;
    const auto value = reinterpret_cast<std::uint64_t>(target);
    std::memcpy(dst + 6, &value, sizeof(value));
}

bool InstallEntryRedirect(void* target, void* replacement, const void* expected, std::size_t patchLen) {
    if (!target || !replacement || !expected || patchLen < 14 || !BytesEqual(target, expected, patchLen))
        return false;

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, patchLen, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    auto* bytes = static_cast<std::uint8_t*>(target);
    WriteAbsoluteJump(bytes, replacement);
    for (std::size_t i = 14; i < patchLen; ++i)
        bytes[i] = 0x90;

    FlushInstructionCache(GetCurrentProcess(), target, patchLen);
    DWORD ignored = 0;
    VirtualProtect(target, patchLen, oldProtect, &ignored);
    return true;
}

bool PatchPointer(void** slot, void* replacement, void** original) {
    if (!slot || !replacement)
        return false;

    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
        return false;

    if (original)
        *original = *slot;
    *slot = replacement;

    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    return true;
}

std::uint64_t Fnv1a64(const void* data, std::size_t size);

// COM objects handed over by a dying description (or, v0.9.607, pipelines replaced by new wireframe options). They
// are released at the next PSO creation or wireframe switch once kPendingReleaseMs have passed since they were let go
// (command lists in flight may still use them), never inside the destructor of the game PSO.
void DrainWireframePendingReleases() {
    std::vector<IUnknown*> due;
    {
        std::lock_guard<std::mutex> guard(g_wireframePendingMutex);
        if (g_wireframePendingReleases.empty()) return;
        const ULONGLONG now = GetTickCount64();
        std::size_t count = 0;   // let go in order: the due ones come first
        while (count < g_wireframePendingReleases.size() && now - g_wireframePendingReleases[count].second >= kPendingReleaseMs)
            ++count;
        if (!count) return;
        try {
            due.reserve(count);
        } catch (...) {
            return;
        }
        for (std::size_t i = 0; i < count; ++i) due.push_back(g_wireframePendingReleases[i].first);
        g_wireframePendingReleases.erase(g_wireframePendingReleases.begin(),
                                         g_wireframePendingReleases.begin() + static_cast<std::ptrdiff_t>(count));
    }
    for (IUnknown* object : due)
        object->Release();
}

// (v0.9.606) A shader's key for the table below: a DXBC shader carries its own 16-byte checksum of the rest (bytes 4-19),
// folded into 64 bits; anything else hashed whole. (0.9.605 hashed every shader's bytes with FNV-1a, a byte at a time:
// most of the 1.28 s this hook added to a level load's 17591 pipelines.) A shared blob is still compared byte for byte.
std::uint64_t ShaderKeyHash(const std::uint8_t* bytes, std::size_t size) {
    if (size >= 32 && std::memcmp(bytes, "DXBC", 4) == 0) {
        std::uint64_t low = 0, high = 0;
        std::memcpy(&low, bytes + 4, sizeof(low));
        std::memcpy(&high, bytes + 12, sizeof(high));
        return low ^ (high * 0x9E3779B97F4A7C15ull) ^ static_cast<std::uint64_t>(size);
    }
    return Fnv1a64(bytes, size);
}

// (v0.9.606) RtCutout::Analyze's answer per vertex / pixel shader pair (it decodes the pixel shader's every instruction;
// a load's thousands of pipelines share far fewer pairs). Keyed by both DXBC checksums and sizes; the answer depends on
// those shaders' bytes alone.
struct CutoutKey {
    std::uint8_t vs[16]{}, ps[16]{};
    std::size_t vsSize = 0, psSize = 0;
    bool operator==(const CutoutKey& other) const noexcept {
        return vsSize == other.vsSize && psSize == other.psSize && !std::memcmp(vs, other.vs, 16) && !std::memcmp(ps, other.ps, 16);
    }
};
struct CutoutKeyHash {
    std::size_t operator()(const CutoutKey& key) const noexcept {
        std::uint64_t a = 0, b = 0;
        std::memcpy(&a, key.vs, sizeof(a));
        std::memcpy(&b, key.ps, sizeof(b));
        return static_cast<std::size_t>(a ^ (b * 0x9E3779B97F4A7C15ull) ^ (key.psSize << 20) ^ key.vsSize);
    }
};
std::mutex g_cutoutMutex;
std::unordered_map<CutoutKey, RtCutout::Program, CutoutKeyHash> g_cutouts;
constexpr std::size_t kMaxCutouts = 32768;

void AnalyzeCutout(const D3D12_SHADER_BYTECODE& vs, const D3D12_SHADER_BYTECODE& ps, RtCutout::Program& out) {
    const auto* vsBytes = static_cast<const std::uint8_t*>(vs.pShaderBytecode);
    const auto* psBytes = static_cast<const std::uint8_t*>(ps.pShaderBytecode);
    const bool keyed = vs.BytecodeLength >= 32 && ps.BytecodeLength >= 32 && !std::memcmp(vsBytes, "DXBC", 4) &&
                       !std::memcmp(psBytes, "DXBC", 4);
    if (!keyed) {
        RtCutout::Analyze(vs.pShaderBytecode, vs.BytecodeLength, ps.pShaderBytecode, ps.BytecodeLength, out);
        return;
    }
    CutoutKey key;
    std::memcpy(key.vs, vsBytes + 4, 16);
    std::memcpy(key.ps, psBytes + 4, 16);
    key.vsSize = vs.BytecodeLength;
    key.psSize = ps.BytecodeLength;
    {
        std::lock_guard<std::mutex> lock(g_cutoutMutex);
        const auto found = g_cutouts.find(key);
        if (found != g_cutouts.end()) {
            out = found->second;
            return;
        }
    }
    RtCutout::Analyze(vs.pShaderBytecode, vs.BytecodeLength, ps.pShaderBytecode, ps.BytecodeLength, out);
    std::lock_guard<std::mutex> lock(g_cutoutMutex);
    if (g_cutouts.size() >= kMaxCutouts) g_cutouts.clear();
    g_cutouts.emplace(key, out);
}

std::shared_ptr<const WireframeShaderBlob> InternWireframeShader(const D3D12_SHADER_BYTECODE& code) {
    const auto* bytes = static_cast<const std::uint8_t*>(code.pShaderBytecode);
    const auto size = static_cast<std::size_t>(code.BytecodeLength);
    const WireframeShaderKey key{ShaderKeyHash(bytes, size), size};

    std::lock_guard<std::mutex> guard(g_wireframeShaderMutex);
    bool share = true;
    const auto found = g_wireframeShaders.find(key);
    if (found != g_wireframeShaders.end()) {
        if (const auto live = found->second.lock()) {
            if (std::memcmp(live->data(), bytes, size) == 0)
                return live;
            share = false;
        }
    }

    std::shared_ptr<const WireframeShaderBlob> blob(
        new WireframeShaderBlob(bytes, bytes + size),
        [](const WireframeShaderBlob* dead) {
            g_wireframeShaderBytesLive.fetch_sub(dead->size(), std::memory_order_relaxed);
            delete dead;
        });
    g_wireframeShaderBytesLive.fetch_add(size, std::memory_order_relaxed);
    if (share) {
        if (g_wireframeShaders.size() >= g_wireframeShaderPurgeAt) {
            for (auto it = g_wireframeShaders.begin(); it != g_wireframeShaders.end();)
                it = it->second.expired() ? g_wireframeShaders.erase(it) : std::next(it);
            g_wireframeShaderPurgeAt = (std::max<std::size_t>)(1024u, g_wireframeShaders.size() * 2u);
        }
        g_wireframeShaders[key] = blob;
    }
    return blob;
}

// (v0.9.607) Draw wireframe's pixel shaders: one flat colour into render target 0 and zero into the others (as many as
// the pipeline has, 1 to 8), the lines' colour or the surfaces' (lines only). No input and no resources, so they go with
// any vertex shader and root signature of the game's. Compiled at run time (d3dcompiler_47.dll, loaded when first
// needed) for the options' generation; the blobs are shared with the pipeline creations that use them.
using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                      UINT, ID3DBlob**, ID3DBlob**);
D3DCompileFn LoadD3DCompile() {
    static const D3DCompileFn fn = [] {
        HMODULE module = LoadLibraryA("d3dcompiler_47.dll");
        return module ? reinterpret_cast<D3DCompileFn>(GetProcAddress(module, "D3DCompile")) : nullptr;
    }();
    return fn;
}
struct FlatShaders {
    std::uint32_t generation = 0;
    std::array<std::shared_ptr<const WireframeShaderBlob>, 8> lines{}, fill{};
};
std::mutex g_flatShaderMutex;
FlatShaders g_flatShaders;
std::atomic_uint32_t g_flatShaderFailures{0};

std::shared_ptr<const WireframeShaderBlob> FlatShader(UINT targets, bool fill, std::uint32_t generation) {
    if (targets < 1 || targets > 8) return nullptr;
    float color[4]{};
    {
        std::lock_guard<std::mutex> lock(g_wireframeOptionsMutex);
        if (fill) {
            std::memcpy(color, g_wireframeFillColor, sizeof(float) * 3);
            color[3] = 1.0f;
        } else {
            std::memcpy(color, g_wireframeLineColor, sizeof(color));
        }
    }
    std::lock_guard<std::mutex> lock(g_flatShaderMutex);
    if (g_flatShaders.generation != generation) g_flatShaders = FlatShaders{generation};
    auto& slot = (fill ? g_flatShaders.fill : g_flatShaders.lines)[targets - 1];
    if (slot) return slot;
    const D3DCompileFn compile = LoadD3DCompile();
    if (!compile) {
        if (g_flatShaderFailures.fetch_add(1, std::memory_order_relaxed) == 0)
            Log::Write("Wireframe: d3dcompiler_47.dll's D3DCompile not found: no lines");
        return nullptr;
    }
    std::string source = "struct O {";
    char line[160]{};
    for (UINT i = 0; i < targets; ++i) {
        std::snprintf(line, sizeof(line), " float4 t%u : SV_Target%u;", i, i);
        source += line;
    }
    std::snprintf(line, sizeof(line), " }; O main() { O o; o.t0 = float4(%.6f, %.6f, %.6f, %.6f);", color[0], color[1],
                  color[2], color[3]);
    source += line;
    for (UINT i = 1; i < targets; ++i) {
        std::snprintf(line, sizeof(line), " o.t%u = float4(0, 0, 0, 0);", i);
        source += line;
    }
    source += " return o; }";
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = compile(source.data(), source.size(), "wireframe_flat", nullptr, nullptr, "main", "ps_5_0",
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr) || !code) {
        if (g_flatShaderFailures.fetch_add(1, std::memory_order_relaxed) < 4)
            Log::Writef("Wireframe: the flat pixel shader (%u targets) did not compile (hr=0x%08X): %s", targets,
                        static_cast<unsigned>(hr),
                        errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
        if (errors) errors->Release();
        if (code) code->Release();
        return nullptr;
    }
    if (errors) errors->Release();
    try {
        const auto* bytes = static_cast<const std::uint8_t*>(code->GetBufferPointer());
        slot = std::make_shared<const WireframeShaderBlob>(bytes, bytes + code->GetBufferSize());
    } catch (...) {
        slot = nullptr;
    }
    code->Release();
    return slot;
}

bool IsIntegerFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT: case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT: case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT: case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SINT: case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT: case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SINT: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SINT:
        return true;
    default:
        return false;
    }
}

bool FloatDepthFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_D32_FLOAT || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

// (v0.9.607) the kinds of pipeline the lines were made for since Draw wireframe was switched on, for its log line
struct WireframeKind {
    UINT targets = 0;
    DXGI_FORMAT target0 = DXGI_FORMAT_UNKNOWN, depth = DXGI_FORMAT_UNKNOWN;
    D3D12_COMPARISON_FUNC test = D3D12_COMPARISON_FUNC_ALWAYS;
    bool writesDepth = false;
    std::uint32_t count = 0;
};
std::mutex g_wireframeKindMutex;
std::array<WireframeKind, 24> g_wireframeKinds{};
std::size_t g_wireframeKindCount = 0;
std::uint32_t g_wireframeKindOthers = 0;

void CountWireframeKind(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
    const bool writes = desc.DepthStencilState.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
    std::lock_guard<std::mutex> lock(g_wireframeKindMutex);
    for (std::size_t i = 0; i < g_wireframeKindCount; ++i) {
        WireframeKind& k = g_wireframeKinds[i];
        if (k.targets == desc.NumRenderTargets && k.target0 == desc.RTVFormats[0] && k.depth == desc.DSVFormat &&
            k.test == desc.DepthStencilState.DepthFunc && k.writesDepth == writes) {
            ++k.count;
            return;
        }
    }
    if (g_wireframeKindCount < g_wireframeKinds.size())
        g_wireframeKinds[g_wireframeKindCount++] = {desc.NumRenderTargets, desc.RTVFormats[0], desc.DSVFormat,
                                                    desc.DepthStencilState.DepthFunc, writes, 1};
    else
        ++g_wireframeKindOthers;
}

const char* DepthTestName(D3D12_COMPARISON_FUNC test) {
    switch (test) {
    case D3D12_COMPARISON_FUNC_NEVER: return "never";
    case D3D12_COMPARISON_FUNC_LESS: return "less";
    case D3D12_COMPARISON_FUNC_EQUAL: return "equal";
    case D3D12_COMPARISON_FUNC_LESS_EQUAL: return "less-equal";
    case D3D12_COMPARISON_FUNC_GREATER: return "greater";
    case D3D12_COMPARISON_FUNC_NOT_EQUAL: return "not-equal";
    case D3D12_COMPARISON_FUNC_GREATER_EQUAL: return "greater-equal";
    default: return "always";
    }
}

// Own copy of a game PSO description. It hangs on the game PSO as private data, so it lives exactly as long as that
// PSO. (v0.9.607) For a geometry pipeline (Overlay(): a vertex input, render targets, a depth test, render target 0
// written whole and not blended) Draw wireframe makes, the first time the game PSO is drawn while it is on, a copy that
// draws its triangles' edges in a flat colour (Lines: wireframe fill, the depth tested toward the camera with a small
// bias, nothing written but render target 0's colour, blended by the lines' opacity), and for Lines only one that draws
// its surfaces in the background colour (Fill: the game's states, the flat pixel shader). Every pass of the game still
// covers what it covers (0.9.606 and before swapped every pipeline for a wireframe one: the depth pre-pass, left solid,
// kept the colour passes' lines from covering the picture, so the last frame stayed on screen between them).
class WireframeRecord final : public IUnknown {
public:
    WireframeRecord() { g_wireframeRecordsLive.fetch_add(1, std::memory_order_relaxed); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualGUID(iid, __uuidof(IUnknown))) {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return refs_.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (!left) Retire();
        return left;
    }

    bool Capture(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& source);
    // (v0.9.607) the lines' / flat surfaces' pipeline for the options' generation (null: none, or it failed)
    ID3D12PipelineState* Lines(ID3D12PipelineState* original, std::uint32_t generation) {
        return Made(lines_, original, generation, false);
    }
    ID3D12PipelineState* Fill(ID3D12PipelineState* original, std::uint32_t generation) {
        return Made(fill_, original, generation, true);
    }
    bool Overlay() const { return overlay_; }
    void Geometry(EightWeightPatch::PipelineGeometry& out) const { out = geometry_; }   // (v0.9.543; v0.9.556: kept)

private:
    struct MadePipeline {
        std::atomic<ID3D12PipelineState*> pipeline{nullptr};
        std::atomic<std::uint32_t> generation{0};   // the options it was made (or failed) for
        std::atomic_bool failed{false};
    };
    ~WireframeRecord() = default;
    void Retire();
    ID3D12PipelineState* Made(MadePipeline& made, ID3D12PipelineState* original, std::uint32_t generation, bool fill);
    bool Describe(bool fill, std::uint32_t generation, D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                  std::shared_ptr<const WireframeShaderBlob>& shader) const;

    std::atomic<ULONG> refs_{1};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc_{};
    std::array<std::shared_ptr<const WireframeShaderBlob>, 5> shaders_{};
    std::vector<D3D12_INPUT_ELEMENT_DESC> layout_;
    std::vector<std::string> layoutNames_;
    std::vector<D3D12_SO_DECLARATION_ENTRY> streamOutput_;
    std::vector<std::string> streamOutputNames_;
    std::vector<UINT> streamOutputStrides_;
    MadePipeline lines_, fill_;   // (v0.9.607)
    bool overlay_{};
    RtCutout::Program cutout_{};   // (v0.9.552) the pipeline's alpha test, for the ray tracing probe
    // (v0.9.556) what the ray tracing probe reads of it, made once (it was made again at every SetPipelineState)
    void ComputeGeometry(EightWeightPatch::PipelineGeometry& out) const;
    EightWeightPatch::PipelineGeometry geometry_{};
};

bool WireframeRecord::Capture(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& source) {
    const auto& layout = source.InputLayout;
    const auto& stream = source.StreamOutput;
    if (layout.NumElements > kWireframeMaxLayoutElements || (layout.NumElements && !layout.pInputElementDescs))
        return false;
    if (stream.NumEntries > kWireframeMaxStreamOutputEntries || (stream.NumEntries && !stream.pSODeclaration) ||
        stream.NumStrides > D3D12_SO_BUFFER_SLOT_COUNT || (stream.NumStrides && !stream.pBufferStrides))
        return false;

    try {
        desc_ = source;
        desc_.pRootSignature = nullptr;
        desc_.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
        desc_.CachedPSO.pCachedBlob = nullptr;
        desc_.CachedPSO.CachedBlobSizeInBytes = 0;

        const D3D12_SHADER_BYTECODE* const sourceStages[5] = {
            &source.VS, &source.PS, &source.DS, &source.HS, &source.GS
        };
        D3D12_SHADER_BYTECODE* const keptStages[5] = {
            &desc_.VS, &desc_.PS, &desc_.DS, &desc_.HS, &desc_.GS
        };
        for (std::size_t i = 0; i < 5; ++i) {
            *keptStages[i] = {};
            if (!sourceStages[i]->pShaderBytecode || !sourceStages[i]->BytecodeLength) continue;
            if (sourceStages[i]->BytecodeLength > kWireframeMaxShaderBytes) return false;
            shaders_[i] = InternWireframeShader(*sourceStages[i]);
            keptStages[i]->pShaderBytecode = shaders_[i]->data();
            keptStages[i]->BytecodeLength = shaders_[i]->size();
        }

        if (layout.NumElements)
            layout_.assign(layout.pInputElementDescs, layout.pInputElementDescs + layout.NumElements);
        layoutNames_.reserve(layout_.size());
        for (const auto& element : layout_)
            layoutNames_.emplace_back(element.SemanticName ? element.SemanticName : "");
        for (std::size_t i = 0; i < layout_.size(); ++i)
            layout_[i].SemanticName = layoutNames_[i].c_str();
        desc_.InputLayout.pInputElementDescs = layout_.empty() ? nullptr : layout_.data();

        if (stream.NumEntries)
            streamOutput_.assign(stream.pSODeclaration, stream.pSODeclaration + stream.NumEntries);
        streamOutputNames_.reserve(streamOutput_.size());
        for (const auto& entry : streamOutput_)
            streamOutputNames_.emplace_back(entry.SemanticName ? entry.SemanticName : "");
        for (std::size_t i = 0; i < streamOutput_.size(); ++i) {
            if (streamOutput_[i].SemanticName)
                streamOutput_[i].SemanticName = streamOutputNames_[i].c_str();
        }
        if (stream.NumStrides)
            streamOutputStrides_.assign(stream.pBufferStrides, stream.pBufferStrides + stream.NumStrides);
        desc_.StreamOutput.pSODeclaration = streamOutput_.empty() ? nullptr : streamOutput_.data();
        desc_.StreamOutput.pBufferStrides = streamOutputStrides_.empty() ? nullptr : streamOutputStrides_.data();
        // (v0.9.552) its alpha test, once (the probe reads it at every SetPipelineState)
        if (desc_.PS.pShaderBytecode && desc_.VS.pShaderBytecode)
            AnalyzeCutout(desc_.VS, desc_.PS, cutout_);   // (v0.9.606: once per shader pair)
    } catch (...) {
        return false;
    }

    if (source.pRootSignature) {
        source.pRootSignature->AddRef();
        desc_.pRootSignature = source.pRootSignature;
    }
    // (v0.9.607) the game's depth convention, and whether Draw wireframe draws lines for this pipeline
    const auto& depth = source.DepthStencilState;
    const bool depthTested = depth.DepthEnable && source.DSVFormat != DXGI_FORMAT_UNKNOWN &&
                             depth.DepthFunc != D3D12_COMPARISON_FUNC_ALWAYS && depth.DepthFunc != D3D12_COMPARISON_FUNC_NEVER;
    if (depthTested && depth.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL) {
        if (depth.DepthFunc == D3D12_COMPARISON_FUNC_LESS || depth.DepthFunc == D3D12_COMPARISON_FUNC_LESS_EQUAL)
            g_depthLessPipelines.fetch_add(1, std::memory_order_relaxed);
        else if (depth.DepthFunc == D3D12_COMPARISON_FUNC_GREATER || depth.DepthFunc == D3D12_COMPARISON_FUNC_GREATER_EQUAL)
            g_depthGreaterPipelines.fetch_add(1, std::memory_order_relaxed);
    }
    const auto& target0 = source.BlendState.RenderTarget[0];
    overlay_ = layout.NumElements > 0 && source.NumRenderTargets >= 1 && source.NumRenderTargets <= 8 && depthTested &&
               !target0.BlendEnable && !target0.LogicOpEnable && (target0.RenderTargetWriteMask & 7) == 7 &&
               source.RTVFormats[0] != DXGI_FORMAT_UNKNOWN && !IsIntegerFormat(source.RTVFormats[0]) &&
               stream.NumEntries == 0 &&
               (source.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE ||
                source.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH);
    if (overlay_) g_wireframeOverlayLive.fetch_add(1, std::memory_order_relaxed);
    ComputeGeometry(geometry_);   // (v0.9.556)
    return true;
}

// (v0.9.607) The description of the lines' or the flat surfaces' pipeline (see the class).
bool WireframeRecord::Describe(bool fill, std::uint32_t generation, D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                               std::shared_ptr<const WireframeShaderBlob>& shader) const {
    shader = FlatShader(desc_.NumRenderTargets, fill, generation);
    if (!shader) return false;
    desc = desc_;
    desc.PS = {shader->data(), shader->size()};
    desc.CachedPSO = {};
    desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    if (fill) return true;   // the game's states: it covers what the game's own draw covers

    const bool reversed = g_depthGreaterPipelines.load(std::memory_order_relaxed) > g_depthLessPipelines.load(std::memory_order_relaxed);
    auto& raster = desc.RasterizerState;
    raster.FillMode = D3D12_FILL_MODE_WIREFRAME;
    if (g_wireframeBackFaces.load(std::memory_order_relaxed)) raster.CullMode = D3D12_CULL_MODE_NONE;
    raster.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    raster.ForcedSampleCount = 0;
    raster.AntialiasedLineEnable = FALSE;
    // toward the camera: enough for an edge's pixels to pass against its own surface's depth (the slope part, where the
    // line's and the triangle's interpolation differ), little enough that edges behind a surface stay hidden. With
    // reversed float depth a unit is relative to the depth (about 1e-4 of the distance here); with near = 0 it is an
    // absolute step, large far away, so few.
    const int units = reversed && FloatDepthFormat(desc_.DSVFormat) ? 800 : (desc_.DSVFormat == DXGI_FORMAT_D16_UNORM ? 2 : 16);
    raster.DepthBias = reversed ? units : -units;
    raster.SlopeScaledDepthBias = reversed ? 1.5f : -1.5f;
    raster.DepthBiasClamp = 0.0f;
    auto& depth = desc.DepthStencilState;
    depth.DepthEnable = g_wireframeSeeThrough.load(std::memory_order_relaxed) ? FALSE : TRUE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = reversed ? D3D12_COMPARISON_FUNC_GREATER_EQUAL : D3D12_COMPARISON_FUNC_LESS_EQUAL;
    depth.StencilEnable = FALSE;
    auto& blend = desc.BlendState;
    blend.AlphaToCoverageEnable = FALSE;
    blend.IndependentBlendEnable = TRUE;
    for (auto& target : blend.RenderTarget) {
        target = {};
        target.SrcBlend = D3D12_BLEND_ONE;
        target.DestBlend = D3D12_BLEND_ZERO;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlendAlpha = D3D12_BLEND_ZERO;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = 0;   // the other targets (normals, gloss, ...) left as the game wrote them
    }
    auto& lines = blend.RenderTarget[0];
    lines.BlendEnable = TRUE;
    lines.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    lines.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    lines.SrcBlendAlpha = D3D12_BLEND_ZERO;
    lines.DestBlendAlpha = D3D12_BLEND_ONE;
    lines.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    return true;
}

ID3D12PipelineState* WireframeRecord::Made(MadePipeline& made, ID3D12PipelineState* original, std::uint32_t generation,
                                           bool fill) {
    if (made.generation.load(std::memory_order_acquire) == generation)
        return made.failed.load(std::memory_order_relaxed) ? nullptr : made.pipeline.load(std::memory_order_acquire);
    if (!g_realCreateGraphicsPipelineState) return nullptr;

    std::lock_guard<std::mutex> guard(WireframeCreateLock(this));
    if (made.generation.load(std::memory_order_acquire) == generation)
        return made.failed.load(std::memory_order_relaxed) ? nullptr : made.pipeline.load(std::memory_order_acquire);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    std::shared_ptr<const WireframeShaderBlob> shader;
    HRESULT hr = E_FAIL;
    ID3D12PipelineState* pipeline = nullptr;
    if (Describe(fill, generation, desc, shader)) {
        ID3D12Device* device = nullptr;
        hr = original->GetDevice(IID_PPV_ARGS(&device));
        if (SUCCEEDED(hr) && device) {
            hr = g_realCreateGraphicsPipelineState(device, &desc, IID_PPV_ARGS(&pipeline));
            device->Release();
        }
    }
    if (FAILED(hr) || !pipeline) {
        made.failed.store(true, std::memory_order_relaxed);
        made.generation.store(generation, std::memory_order_release);
        if (shader && g_wireframeMirrorFailures.fetch_add(1, std::memory_order_relaxed) < 4)
            Log::Writef("Wireframe: the %s pipeline could not be made (hr=0x%08X; %u targets, target 0 format %u, depth %u)",
                        fill ? "flat surfaces'" : "lines'", static_cast<unsigned>(hr), desc_.NumRenderTargets,
                        static_cast<unsigned>(desc_.RTVFormats[0]), static_cast<unsigned>(desc_.DSVFormat));
        return nullptr;
    }
    ID3D12PipelineState* previous = made.pipeline.exchange(pipeline, std::memory_order_acq_rel);
    made.failed.store(false, std::memory_order_relaxed);
    made.generation.store(generation, std::memory_order_release);
    if (previous) {   // made for older options: still in command lists in flight
        std::lock_guard<std::mutex> pending(g_wireframePendingMutex);
        PendRelease(previous);
    }
    if (fill) {
        g_wireframeFillsCreated.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_wireframeLinesThisTime.fetch_add(1, std::memory_order_relaxed);
        if (g_wireframeMirrorsCreated.fetch_add(1, std::memory_order_relaxed) == 0)
            Log::Detail("Wireframe: the first lines' pipeline made");
        CountWireframeKind(desc_);
    }
    return pipeline;
}

std::atomic<std::uint32_t> g_pipelineGeneration{0};   // (v0.9.557) records let go (their pipelines gone)

void WireframeRecord::Retire() {
    g_pipelineGeneration.fetch_add(1, std::memory_order_release);
    IUnknown* lines = lines_.pipeline.exchange(nullptr, std::memory_order_acq_rel);
    IUnknown* fill = fill_.pipeline.exchange(nullptr, std::memory_order_acq_rel);
    IUnknown* rootSignature = desc_.pRootSignature;
    desc_.pRootSignature = nullptr;
    if (lines || fill || rootSignature) {
        std::lock_guard<std::mutex> guard(g_wireframePendingMutex);
        PendRelease(lines);
        PendRelease(fill);
        PendRelease(rootSignature);
    }
    if (overlay_) g_wireframeOverlayLive.fetch_sub(1, std::memory_order_relaxed);
    g_wireframeRecordsLive.fetch_sub(1, std::memory_order_relaxed);
    delete this;
}

// (v0.9.543) The bytes of a vertex element's format (the formats an input layout uses)
unsigned FormatBytes(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT:
            return 16;
        case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R32G32B32_SINT:
            return 12;
        case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SINT:
            return 8;
        case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_SNORM:
            return format == DXGI_FORMAT_R8G8_UNORM || format == DXGI_FORMAT_R8G8_SNORM ? 2 : 4;
        case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SINT:
            return 2;
        case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SINT:
            return 1;
        default:
            return 4;   // (the 32-bit and 8:8:8:8 / 10:10:10:2 / 11:11:10 ones)
    }
}

// (v0.9.547) Which components of BLENDWEIGHT 0 and BLENDINDICES 0 a vertex shader reads: its DXBC input signature
// (ISGN: a count, 8, then 24 bytes an element: name offset, semantic index, system value, component type, register,
// mask, read mask). The game's skinned pipelines do not all skin: some shaders read four weights and four indices,
// some only the first index (one bone), some neither (the object's world matrix, WorldBuffer).
bool VertexShaderBlendUse(const void* code, std::size_t size, unsigned& weightUse, unsigned& indexUse) {
    weightUse = indexUse = 0;
    const auto* b = static_cast<const std::uint8_t*>(code);
    if (!b || size < 32 || std::memcmp(b, "DXBC", 4) != 0) return false;
    std::uint32_t chunks = 0;
    std::memcpy(&chunks, b + 28, 4);
    for (std::uint32_t i = 0; i < chunks && 32 + 4 * i + 4 <= size; ++i) {
        std::uint32_t at = 0, length = 0;
        std::memcpy(&at, b + 32 + 4 * i, 4);
        if (at + 8 > size || std::memcmp(b + at, "ISGN", 4) != 0) continue;
        std::memcpy(&length, b + at + 4, 4);
        if (at + 8 + length > size || length < 8) return false;
        const std::uint8_t* c = b + at + 8;
        std::uint32_t count = 0;
        std::memcpy(&count, c, 4);
        for (std::uint32_t e = 0; e < count && 8 + 24 * (e + 1) <= length; ++e) {
            const std::uint8_t* element = c + 8 + 24 * e;
            std::uint32_t nameAt = 0, semantic = 0;
            std::memcpy(&nameAt, element, 4);
            std::memcpy(&semantic, element + 4, 4);
            if (nameAt >= length || semantic != 0) continue;
            const char* name = reinterpret_cast<const char*>(c + nameAt);
            if (!memchr(name, 0, length - nameAt)) continue;
            if (!_stricmp(name, "BLENDWEIGHT")) weightUse = element[21] & 0xF;
            else if (!_stricmp(name, "BLENDINDICES")) indexUse = element[21] & 0xF;
        }
        return true;
    }
    return false;
}

void WireframeRecord::ComputeGeometry(EightWeightPatch::PipelineGeometry& out) const {
    out = {};
    unsigned slotEnd[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]{};
    bool found = false;
    for (std::size_t i = 0; i < layout_.size(); ++i) {
        const auto& e = layout_[i];
        const char* name = layoutNames_[i].c_str();
        if (e.InputSlot >= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) continue;
        const unsigned offset = e.AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT ? slotEnd[e.InputSlot] : e.AlignedByteOffset;
        const unsigned size = FormatBytes(e.Format);
        slotEnd[e.InputSlot] = offset + size;
        if (!_stricmp(name, "BLENDINDICES") || !_stricmp(name, "BLENDWEIGHT")) {
            out.skinned = true;
            if (e.SemanticIndex > 0) out.eightWeights = true;   // (v0.9.545)
            else if (!_stricmp(name, "BLENDWEIGHT")) {
                out.weightFormat = static_cast<unsigned>(e.Format);
                out.weightSlot = e.InputSlot;
                out.weightOffset = offset;
            } else {
                out.blendIndexFormat = static_cast<unsigned>(e.Format);
                out.blendIndexSlot = e.InputSlot;
                out.blendIndexOffset = offset;
            }
        }
        if (!out.texcoordFound && cutout_.result == RtCutout::kFollowed && !_stricmp(name, "TEXCOORD") &&
            e.SemanticIndex == cutout_.texcoord && e.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA) {
            out.texcoordFound = true;   // (v0.9.552)
            out.texcoordFormat = static_cast<unsigned>(e.Format);
            out.texcoordSlot = e.InputSlot;
            out.texcoordOffset = offset;
        }
        if (!found && !_stricmp(name, "POSITION") && e.SemanticIndex == 0 && e.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA) {
            found = true;
            out.positionFormat = static_cast<unsigned>(e.Format);
            out.positionSlot = e.InputSlot;
            out.positionOffset = offset;
            out.positionSize = size;
        }
    }
    out.depthWrite = desc_.DepthStencilState.DepthEnable && desc_.DepthStencilState.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL &&
                     desc_.DSVFormat != DXGI_FORMAT_UNKNOWN;
    out.triangles = desc_.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    out.patches = desc_.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    out.renderTargets = desc_.NumRenderTargets;
    out.vertexShaderKnown = VertexShaderBlendUse(desc_.VS.pShaderBytecode, desc_.VS.BytecodeLength, out.weightUse,
                                                 out.blendIndexUse);   // (v0.9.547)
    out.cutout = cutout_;   // (v0.9.552)
    if (cutout_.result == RtCutout::kFollowed && !out.texcoordFound) {   // (v0.9.554) for the probe's log
        std::size_t at = 0;
        for (std::size_t i = 0; i < layout_.size() && at + 1 < sizeof(out.layoutSummary); ++i) {
            const auto& e = layout_[i];
            const int n = std::snprintf(out.layoutSummary + at, sizeof(out.layoutSummary) - at, "%s%s%u/%u%s/%u",
                                        i ? " " : "", layoutNames_[i].c_str(), e.SemanticIndex, e.InputSlot,
                                        e.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA ? "i" : "",
                                        static_cast<unsigned>(e.Format));
            if (n <= 0) break;
            at += static_cast<std::size_t>(n);
        }
    }
    out.valid = found;
}

// Returns the description attached to a game PSO with one reference added, or null.
WireframeRecord* AcquireWireframeRecord(ID3D12PipelineState* pipelineState) {
    IUnknown* attached = nullptr;
    UINT size = sizeof(attached);
    if (FAILED(pipelineState->GetPrivateData(kWireframeRecordGuid, &size, &attached)) || !attached)
        return nullptr;
    return static_cast<WireframeRecord*>(attached);
}

void KeepWireframeDescription(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& effectiveDesc, void* returnedState) {
    DrainWireframePendingReleases();
    if (!returnedState) return;

    ID3D12PipelineState* original = nullptr;
    if (FAILED(reinterpret_cast<IUnknown*>(returnedState)->QueryInterface(IID_PPV_ARGS(&original))) || !original)
        return;

    // D3D12 hands out the same object for an identical description: it already has its copy
    if (WireframeRecord* known = AcquireWireframeRecord(original)) {
        known->Release();
        original->Release();
        return;
    }

    if (auto* record = new (std::nothrow) WireframeRecord()) {
        if (record->Capture(effectiveDesc) &&
            SUCCEEDED(original->SetPrivateDataInterface(kWireframeRecordGuid, record))) {
            const auto kept = g_wireframeRecordsKept.fetch_add(1, std::memory_order_relaxed) + 1;
            if (kept == 1)
                Log::Detail("Wireframe: PSO descriptions are kept for on-demand wireframe (no wireframe PSO exists until wireframe is switched on)");
        }
        record->Release();
    }
    original->Release();
}

// (v0.9.527) The lights' pass made again while the game runs (the user: Lamp shadow softness and Contact shadows took
// effect only at the next start). The settings are values inside its patched pixel shader (shadow_blend.h), and a
// pipeline cannot change its shader; so each game pipeline whose pixel shader is a lights' pass variant keeps its
// description here (the game's own shader in it), and when the settings' key differs from the one it was made with,
// a pipeline with the shader for the new key is made on a thread of its own (no hitch) and bound in its place by
// Reset / SetPipelineState from then on (the game's own until it is ready). Each one made is kept until the game's own
// pipeline goes (command lists in flight may still use one). The few game pipelines are found by a table of their
// pointers (the hot path: a compare per slot).
const GUID kLightPassRecordGuid =
    {0x3f1d7a52, 0x0c4e, 0x4b8a, {0xa1, 0x6d, 0x27, 0x5e, 0x90, 0x3b, 0xc8, 0x14}};
// (v0.9.570) the sun's shadow pass's pipelines kept the same way (its records' sun_; 32 slots, was 16)
constexpr std::size_t kLightPassSlots = 32;

class LightPassRecord;
std::array<std::atomic<ID3D12PipelineState*>, kLightPassSlots> g_lightPassGame{};
std::array<std::atomic<LightPassRecord*>, kLightPassSlots> g_lightPassRecords{};
std::array<std::atomic<bool>, kLightPassSlots> g_lightPassSun{};   // (v0.9.570) the slot's record is the sun pass's
std::atomic_uint32_t g_lightPassLive{0};
std::atomic_uint32_t g_lightPassRebuilds{0};

class LightPassRecord final : public IUnknown {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualGUID(iid, __uuidof(IUnknown))) {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (!left) Retire();
        return left;
    }

    bool Capture(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& gameDesc, int variant, std::uint32_t madeKey, bool sun) {
        const auto& layout = gameDesc.InputLayout;
        if (layout.NumElements > kWireframeMaxLayoutElements || (layout.NumElements && !layout.pInputElementDescs) ||
            gameDesc.StreamOutput.NumEntries || !gameDesc.PS.pShaderBytecode)
            return false;
        try {
            desc_ = gameDesc;
            desc_.pRootSignature = nullptr;
            desc_.CachedPSO = {};
            const D3D12_SHADER_BYTECODE* const stages[5] = {&gameDesc.VS, &gameDesc.PS, &gameDesc.DS, &gameDesc.HS,
                                                             &gameDesc.GS};
            D3D12_SHADER_BYTECODE* const kept[5] = {&desc_.VS, &desc_.PS, &desc_.DS, &desc_.HS, &desc_.GS};
            for (std::size_t i = 0; i < 5; ++i) {
                *kept[i] = {};
                if (!stages[i]->pShaderBytecode || !stages[i]->BytecodeLength) continue;
                if (stages[i]->BytecodeLength > kWireframeMaxShaderBytes) return false;
                shaders_[i] = InternWireframeShader(*stages[i]);
                kept[i]->pShaderBytecode = shaders_[i]->data();
                kept[i]->BytecodeLength = shaders_[i]->size();
            }
            if (layout.NumElements) layout_.assign(layout.pInputElementDescs, layout.pInputElementDescs + layout.NumElements);
            for (const auto& element : layout_) layoutNames_.emplace_back(element.SemanticName ? element.SemanticName : "");
            for (std::size_t i = 0; i < layout_.size(); ++i) layout_[i].SemanticName = layoutNames_[i].c_str();
            desc_.InputLayout.pInputElementDescs = layout_.empty() ? nullptr : layout_.data();
        } catch (...) {
            return false;
        }
        if (gameDesc.pRootSignature) {
            gameDesc.pRootSignature->AddRef();
            desc_.pRootSignature = gameDesc.pRootSignature;
        }
        variant_ = variant;
        madeKey_ = madeKey;
        sun_ = sun;   // (v0.9.570)
        return true;
    }
    bool Sun() const { return sun_; }

    // The pipeline for the settings now: the game's own when they are the ones it was made with, else the one made
    // for them (or, while it is being made, the last one bound).
    ID3D12PipelineState* Select(ID3D12PipelineState* game) {
        const std::uint32_t key = sun_ ? ShadowBlend::SunPassKey() : ShadowBlend::LightPassKey();   // (v0.9.570)
        if (key == madeKey_) return game;
        {
            std::lock_guard<std::mutex> guard(lock_);
            for (const auto& made : made_)
                if (made.first == key) return made.second ? made.second : game;
            if (!building_ && !failed_) {
                building_ = true;
                ID3D12Device* device = nullptr;
                if (SUCCEEDED(game->GetDevice(IID_PPV_ARGS(&device))) && device) {
                    AddRef();
                    if (HANDLE thread = CreateThread(nullptr, 0, &BuildThread, new BuildJob{this, device, key}, 0, nullptr)) {
                        CloseHandle(thread);
                    } else {
                        building_ = false;
                        device->Release();
                        Release();
                    }
                } else {
                    building_ = false;
                }
            }
            if (last_) return last_;
        }
        return game;
    }

    void Remember(ID3D12PipelineState* game) {
        for (std::size_t i = 0; i < kLightPassSlots; ++i) {
            ID3D12PipelineState* expected = nullptr;
            if (g_lightPassGame[i].compare_exchange_strong(expected, game)) {
                g_lightPassSun[i].store(sun_, std::memory_order_relaxed);   // (v0.9.570)
                g_lightPassRecords[i].store(this, std::memory_order_release);
                slot_ = static_cast<int>(i);
                g_lightPassLive.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }

private:
    struct BuildJob {
        LightPassRecord* record;
        ID3D12Device* device;
        std::uint32_t key;
    };
    static DWORD WINAPI BuildThread(void* parameter) {
        auto* job = static_cast<BuildJob*>(parameter);
        job->record->Build(job->device, job->key);
        job->device->Release();
        job->record->Release();
        delete job;
        return 0;
    }
    void Build(ID3D12Device* device, std::uint32_t key) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = desc_;
        if (const D3D12_SHADER_BYTECODE* ps = sun_ ? ShadowBlend::SunPassBytecode(variant_, key)   // (v0.9.570)
                                                   : ShadowBlend::LightPassBytecode(variant_, key))
            desc.PS = *ps;
        ID3D12PipelineState* made = nullptr;
        const HRESULT hr = g_realCreateGraphicsPipelineState
            ? g_realCreateGraphicsPipelineState(device, &desc, IID_PPV_ARGS(&made)) : E_FAIL;
        std::lock_guard<std::mutex> guard(lock_);
        building_ = false;
        if (FAILED(hr) || !made) {
            failed_ = true;
            Log::Writef("RenderTweaks: the %s pass (shader %s) could not be made again for the new settings "
                        "(hr 0x%08X); the one it has stays", sun_ ? "sun's shadow" : "lights'",
                        sun_ ? ShadowBlend::SunPassName(variant_) : ShadowBlend::LightPassName(variant_),
                        static_cast<unsigned>(hr));
            return;
        }
        try {
            made_.emplace_back(key, made);
        } catch (...) {
            made->Release();
            return;
        }
        last_ = made;
        if (g_lightPassRebuilds.fetch_add(1, std::memory_order_relaxed) < 24)
            Log::Writef("RenderTweaks: the %s pass (shader %s) made again for the new settings (%s)",
                        sun_ ? "sun's shadow" : "lights'",
                        sun_ ? ShadowBlend::SunPassName(variant_) : ShadowBlend::LightPassName(variant_),
                        sun_ ? (key ? "the probe's sun mask read" : "the cascades alone")
                             : (key ? "patched" : "the game's own"));
    }
    void Retire() {
        if (slot_ >= 0) {
            g_lightPassRecords[slot_].store(nullptr, std::memory_order_release);
            g_lightPassGame[slot_].store(nullptr, std::memory_order_release);
            g_lightPassLive.fetch_sub(1, std::memory_order_relaxed);
        }
        {
            std::lock_guard<std::mutex> guard(g_wireframePendingMutex);
            for (const auto& made : made_)
                PendRelease(made.second);
            PendRelease(desc_.pRootSignature);
        }
        delete this;
    }
    ~LightPassRecord() = default;

    std::atomic<ULONG> refs_{1};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc_{};
    std::array<std::shared_ptr<const WireframeShaderBlob>, 5> shaders_{};
    std::vector<D3D12_INPUT_ELEMENT_DESC> layout_;
    std::vector<std::string> layoutNames_;
    int variant_ = -1;
    std::uint32_t madeKey_ = 0;
    bool sun_ = false;   // (v0.9.570) the sun's shadow pass's (else the lights' pass's)
    int slot_ = -1;
    std::mutex lock_;
    std::vector<std::pair<std::uint32_t, ID3D12PipelineState*>> made_;   // (lock_)
    ID3D12PipelineState* last_ = nullptr;                                 // (lock_)
    bool building_ = false, failed_ = false;                              // (lock_)
};

// After the game made a lights' pass pipeline: its description kept (with the game's own pixel shader).
void KeepLightPassDescription(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& gameDesc, int variant, std::uint32_t madeKey,
                              void* returnedState, bool sun = false) {
    if (variant < 0 || !returnedState) return;
    ID3D12PipelineState* game = nullptr;
    if (FAILED(reinterpret_cast<IUnknown*>(returnedState)->QueryInterface(IID_PPV_ARGS(&game))) || !game) return;
    IUnknown* attached = nullptr;
    UINT size = sizeof(attached);
    if (SUCCEEDED(game->GetPrivateData(kLightPassRecordGuid, &size, &attached)) && attached) {   // (the same object again)
        attached->Release();
        game->Release();
        return;
    }
    if (auto* record = new (std::nothrow) LightPassRecord()) {
        if (record->Capture(gameDesc, variant, madeKey, sun) &&
            SUCCEEDED(game->SetPrivateDataInterface(kLightPassRecordGuid, record)))
            record->Remember(game);
        record->Release();
    }
    game->Release();
}

ID3D12PipelineState* SelectLightPassPso(ID3D12PipelineState* pipelineState) {
    if (!pipelineState || !g_lightPassLive.load(std::memory_order_relaxed)) return pipelineState;
    for (std::size_t i = 0; i < kLightPassSlots; ++i) {
        if (g_lightPassGame[i].load(std::memory_order_relaxed) != pipelineState) continue;
        LightPassRecord* record = g_lightPassRecords[i].load(std::memory_order_acquire);
        return record ? record->Select(pipelineState) : pipelineState;
    }
    return pipelineState;
}

// The pipeline bound in the game's place (the lights' pass's own, or for Lines only the flat surfaces'), and through
// `lines` the lines' pipeline for its draws (null: none).
ID3D12PipelineState* SelectWireframePso(ID3D12GraphicsCommandList* commandList, ID3D12PipelineState* pipelineState,
                                        ID3D12PipelineState*& lines) {
    lines = nullptr;
    pipelineState = SelectLightPassPso(pipelineState);   // (v0.9.527)
    if (!g_wireframeEnabled.load(std::memory_order_relaxed) || !pipelineState ||
        commandList == g_wireframeExcludedCommandList.load(std::memory_order_relaxed))
        return pipelineState;

    WireframeRecord* record = AcquireWireframeRecord(pipelineState);
    if (!record) return pipelineState;

    ID3D12PipelineState* selected = pipelineState;
    if (record->Overlay()) {   // (v0.9.607)
        const std::uint32_t generation = g_wireframeGeneration.load(std::memory_order_acquire);
        lines = record->Lines(pipelineState, generation);
        if (lines && g_wireframeStyle.load(std::memory_order_relaxed) == 1)
            if (ID3D12PipelineState* fill = record->Fill(pipelineState, generation)) selected = fill;
    }
    record->Release();
    return selected;
}

// (v0.9.607) Each command list's bound pipeline and its lines' pipeline, for the draw hooks below: kept by the list's
// address in a table every thread shares (a list is recorded on one thread at a time, but not always the same one), with
// the epoch it was kept in (switching on and new options make every older entry stale).
struct ListLines {
    std::atomic<ID3D12GraphicsCommandList*> list{nullptr};
    std::atomic<ID3D12PipelineState*> bound{nullptr}, lines{nullptr};
    std::atomic<std::uint32_t> epoch{0};
};
constexpr std::size_t kListLinesSlots = 1024;   // a power of two
std::array<ListLines, kListLinesSlots> g_listLines;
std::atomic_bool g_listLinesFullLogged{false};

ListLines* ListLinesFor(ID3D12GraphicsCommandList* list, bool add) {
    const std::size_t start =
        static_cast<std::size_t>((reinterpret_cast<std::uintptr_t>(list) >> 4) * 0x9E3779B97F4A7C15ull >> 54);
    for (std::size_t i = 0; i < kListLinesSlots; ++i) {
        ListLines& entry = g_listLines[(start + i) & (kListLinesSlots - 1)];
        ID3D12GraphicsCommandList* at = entry.list.load(std::memory_order_acquire);
        if (at == list) return &entry;
        if (at) continue;
        if (!add) return nullptr;
        ID3D12GraphicsCommandList* expected = nullptr;
        if (entry.list.compare_exchange_strong(expected, list, std::memory_order_acq_rel) || expected == list) return &entry;
    }
    if (add && !g_listLinesFullLogged.exchange(true))
        Log::Write("Wireframe: more than 1024 command lists: the newer ones get no lines");
    return nullptr;
}

void KeepListLines(ID3D12GraphicsCommandList* list, ID3D12PipelineState* bound, ID3D12PipelineState* lines) {
    if (!g_wireframeEnabled.load(std::memory_order_relaxed)) return;
    ListLines* entry = ListLinesFor(list, lines != nullptr);
    if (!entry) return;
    entry->bound.store(bound, std::memory_order_relaxed);
    entry->lines.store(lines, std::memory_order_relaxed);
    entry->epoch.store(g_wireframeEpoch.load(std::memory_order_relaxed), std::memory_order_release);
}

HRESULT STDMETHODCALLTYPE HookCommandListReset(ID3D12GraphicsCommandList* commandList,
                                               ID3D12CommandAllocator* allocator,
                                               ID3D12PipelineState* initialState) {
    RtProbe::OnSetPipelineState(commandList, initialState);   // (v0.9.543)
    if (!g_realCommandListReset) return E_FAIL;
    ID3D12PipelineState* lines = nullptr;
    ID3D12PipelineState* selected = SelectWireframePso(commandList, initialState, lines);
    KeepListLines(commandList, selected, lines);
    return g_realCommandListReset(commandList, allocator, selected);
}

void STDMETHODCALLTYPE HookSetPipelineState(ID3D12GraphicsCommandList* commandList,
                                            ID3D12PipelineState* pipelineState) {
    RtProbe::OnSetPipelineState(commandList, pipelineState);   // (v0.9.543: the game's own)
    if (!g_realSetPipelineState) return;
    ID3D12PipelineState* lines = nullptr;
    ID3D12PipelineState* selected = SelectWireframePso(commandList, pipelineState, lines);
    KeepListLines(commandList, selected, lines);
    g_realSetPipelineState(commandList, selected);
}

// (v0.9.607) The draws: after each of the game's draws with a lines' pipeline kept for its list, the same draw again
// with the lines, then the game's pipeline put back. Hooked in the command lists' vtable before the ray tracing probe's
// hooks (RtProbe::AttachCommandList, after these, keeps them as its originals): the probe sees the game's draw once and
// never the lines'.
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
constexpr std::size_t kDrawInstancedSlot = 12u;
constexpr std::size_t kDrawIndexedInstancedSlot = 13u;
DrawInstancedFn g_realDrawInstanced = nullptr;
DrawIndexedInstancedFn g_realDrawIndexedInstanced = nullptr;
std::mutex g_drawHookMutex;
void** g_drawHookVtable = nullptr;

bool LinesForDraw(ID3D12GraphicsCommandList* list, ID3D12PipelineState*& bound, ID3D12PipelineState*& lines) {
    if (!g_wireframeEnabled.load(std::memory_order_relaxed) || !g_realSetPipelineState) return false;
    ListLines* entry = ListLinesFor(list, false);
    if (!entry || entry->epoch.load(std::memory_order_acquire) != g_wireframeEpoch.load(std::memory_order_relaxed))
        return false;
    lines = entry->lines.load(std::memory_order_relaxed);
    bound = entry->bound.load(std::memory_order_relaxed);
    return lines && bound;
}

void STDMETHODCALLTYPE HookDrawInstanced(ID3D12GraphicsCommandList* list, UINT vertices, UINT instances, UINT startVertex,
                                         UINT startInstance) {
    g_realDrawInstanced(list, vertices, instances, startVertex, startInstance);
    ID3D12PipelineState* bound = nullptr;
    ID3D12PipelineState* lines = nullptr;
    if (!LinesForDraw(list, bound, lines)) return;
    g_realSetPipelineState(list, lines);
    g_realDrawInstanced(list, vertices, instances, startVertex, startInstance);
    g_realSetPipelineState(list, bound);
    g_wireframeDrawsReplayed.fetch_add(1, std::memory_order_relaxed);
}

void STDMETHODCALLTYPE HookDrawIndexedInstanced(ID3D12GraphicsCommandList* list, UINT indices, UINT instances,
                                                UINT startIndex, INT baseVertex, UINT startInstance) {
    g_realDrawIndexedInstanced(list, indices, instances, startIndex, baseVertex, startInstance);
    ID3D12PipelineState* bound = nullptr;
    ID3D12PipelineState* lines = nullptr;
    if (!LinesForDraw(list, bound, lines)) return;
    g_realSetPipelineState(list, lines);
    g_realDrawIndexedInstanced(list, indices, instances, startIndex, baseVertex, startInstance);
    g_realSetPipelineState(list, bound);
    g_wireframeDrawsReplayed.fetch_add(1, std::memory_order_relaxed);
}

void AttachCommandListDraws(void** vtable) {
    std::lock_guard<std::mutex> lock(g_drawHookMutex);
    if (g_drawHookVtable) return;   // one vtable (the game's direct lists'); the first one seen
    void* indexed = nullptr;
    void* instanced = nullptr;
    if (!PatchPointer(&vtable[kDrawIndexedInstancedSlot], reinterpret_cast<void*>(&HookDrawIndexedInstanced), &indexed)) {
        Log::Write("Wireframe: the command lists' DrawIndexedInstanced could not be hooked (no lines)");
        g_drawHookVtable = vtable;
        return;
    }
    g_realDrawIndexedInstanced = reinterpret_cast<DrawIndexedInstancedFn>(indexed);
    if (!PatchPointer(&vtable[kDrawInstancedSlot], reinterpret_cast<void*>(&HookDrawInstanced), &instanced)) {
        PatchPointer(&vtable[kDrawIndexedInstancedSlot], indexed, nullptr);
        Log::Write("Wireframe: the command lists' DrawInstanced could not be hooked (no lines)");
        g_drawHookVtable = vtable;
        return;
    }
    g_realDrawInstanced = reinterpret_cast<DrawInstancedFn>(instanced);
    g_drawHookVtable = vtable;
    g_wireframeDrawsHooked.store(true, std::memory_order_release);
    Log::Detail("Wireframe: the command lists' draws hooked (Draw wireframe's lines)");
}

bool AttachCommandListSetPipelineState(ID3D12GraphicsCommandList* commandList) {
    if (!commandList) return false;
    void** vtable = *reinterpret_cast<void***>(commandList);
    if (!vtable) return false;

    bool resetReady = false;
    void** resetSlot = &vtable[kCommandListResetSlot];
    if (*resetSlot == reinterpret_cast<void*>(&HookCommandListReset)) {
        resetReady = true;
    } else if (!g_realCommandListReset || *resetSlot == reinterpret_cast<void*>(g_realCommandListReset)) {
        void* original = nullptr;
        if (PatchPointer(resetSlot, reinterpret_cast<void*>(&HookCommandListReset), &original)) {
            if (!g_realCommandListReset)
                g_realCommandListReset = reinterpret_cast<CommandListResetFn>(original);
            resetReady = true;
        }
    }

    bool setReady = false;
    void** setSlot = &vtable[kSetPipelineStateSlot];
    if (*setSlot == reinterpret_cast<void*>(&HookSetPipelineState)) {
        setReady = true;
    } else if (!g_realSetPipelineState || *setSlot == reinterpret_cast<void*>(g_realSetPipelineState)) {
        void* original = nullptr;
        if (PatchPointer(setSlot, reinterpret_cast<void*>(&HookSetPipelineState), &original)) {
            if (!g_realSetPipelineState)
                g_realSetPipelineState = reinterpret_cast<SetPipelineStateFn>(original);
            setReady = true;
        }
    }

    const bool ready = resetReady && setReady;
    if (ready && !g_wireframeCommandListHookAttached.exchange(true, std::memory_order_acq_rel))
        Log::Detail("Wireframe: DX12 command-list Reset/SetPipelineState hooks armed");
    if (ready) AttachCommandListDraws(vtable);   // (v0.9.607)
    return ready;
}

HRESULT STDMETHODCALLTYPE HookCreateCommandList(ID3D12Device* device, UINT nodeMask,
                                                D3D12_COMMAND_LIST_TYPE type,
                                                ID3D12CommandAllocator* allocator,
                                                ID3D12PipelineState* initialState,
                                                REFIID iid, void** outList) {
    const HRESULT hr = g_realCreateCommandList
        ? g_realCreateCommandList(device, nodeMask, type, allocator, initialState, iid, outList)
        : E_FAIL;
    if (SUCCEEDED(hr) && outList && *outList && type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        ID3D12GraphicsCommandList* graphics = nullptr;
        if (SUCCEEDED(reinterpret_cast<IUnknown*>(*outList)->QueryInterface(IID_PPV_ARGS(&graphics))) && graphics) {
            AttachCommandListSetPipelineState(graphics);
            RtProbe::AttachCommandList(graphics);   // (v0.9.543)
            graphics->Release();
        }
    }
    return hr;
}

std::uint64_t Fnv1a64(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

bool MarkShaderHashSeen(std::uint64_t hash) {
    while (g_shaderSeenLock.test_and_set(std::memory_order_acquire))
        YieldProcessor();

    bool isNew = true;
    for (std::size_t i = 0; i < g_seenShaderHashCount; ++i) {
        if (g_seenShaderHashes[i] == hash) {
            isNew = false;
            break;
        }
    }
    if (isNew && g_seenShaderHashCount < g_seenShaderHashes.size())
        g_seenShaderHashes[g_seenShaderHashCount++] = hash;
    else if (isNew)
        isNew = false;

    g_shaderSeenLock.clear(std::memory_order_release);
    return isNew;
}

void DumpUnknownShader(std::uint64_t hash, const void* data, std::size_t size) {
    if (!data || !size || size > 64u * 1024u * 1024u)
        return;

    char exePath[MAX_PATH]{};
    if (!GetModuleFileNameA(nullptr, exePath, MAX_PATH))
        return;
    char* slash = exePath;
    for (char* p = exePath; *p; ++p) {
        if (*p == '\\' || *p == '/')
            slash = p + 1;
    }
    *slash = '\0';

    char fileName[80]{};
    sprintf_s(fileName, "rottr_8w_vs_%016llX.bin", static_cast<unsigned long long>(hash));
    strcat_s(exePath, MAX_PATH, fileName);

    HANDLE file = CreateFileA(exePath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    const auto* cursor = static_cast<const std::uint8_t*>(data);
    std::size_t remaining = size;
    while (remaining) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 1024u * 1024u));
        DWORD written = 0;
        if (!WriteFile(file, cursor, chunk, &written, nullptr) || written != chunk)
            break;
        cursor += chunk;
        remaining -= chunk;
    }
    CloseHandle(file);
}

struct ShaderRewriteCacheEntry {
    std::uint64_t originalHash = 0;
    std::size_t originalSize = 0;
    bool success = false;
    std::vector<std::uint8_t> data;
    DxbcEightWeight::RewriteInfo info{};
    std::string error;
};

std::atomic_flag g_rewriteCacheLock = ATOMIC_FLAG_INIT;
std::deque<ShaderRewriteCacheEntry> g_rewriteCache;

const ShaderRewriteCacheEntry* GetOrRewriteShader(std::uint64_t originalHash,
                                                  const void* data,
                                                  std::size_t size) {
    while (g_rewriteCacheLock.test_and_set(std::memory_order_acquire))
        YieldProcessor();

    for (auto& entry : g_rewriteCache) {
        if (entry.originalHash == originalHash && entry.originalSize == size) {
            g_rewriteCacheLock.clear(std::memory_order_release);
            return &entry;
        }
    }

    g_rewriteCache.emplace_back();
    auto& entry = g_rewriteCache.back();
    entry.originalHash = originalHash;
    entry.originalSize = size;
    entry.success = DxbcEightWeight::Rewrite(
        data, size, entry.data, &entry.info, &entry.error);

    const ShaderRewriteCacheEntry* result = &entry;
    g_rewriteCacheLock.clear(std::memory_order_release);
    return result;
}

std::uint32_t StockDxgiFormat(std::uint32_t type) {
    switch (type) {
        case 0: return 41u;
        case 1: return 16u;
        case 2: return 6u;
        case 3: return 2u;
        case 4: case 5: case 6: case 7: case 11: return 28u;
        case 8: return 30u;
        case 9: return 38u;
        case 10: return 14u;
        case 12: case 19: return 37u;
        case 13: case 20: return 13u;
        case 14: return 35u;
        case 15: return 11u;
        case 16: return 25u;
        case 17: return 24u;
        default: return 0u;
    }
}

std::uint32_t FormatByteSize(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16u;
        case DXGI_FORMAT_R32G32B32_FLOAT: return 12u;
        case DXGI_FORMAT_R32G32_FLOAT: return 8u;
        case DXGI_FORMAT_R32_FLOAT: return 4u;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_SINT: return 8u;
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16_SINT: return 4u;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UINT: return 4u;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT: return 4u;
        default: return 0u;
    }
}

bool IsCustomSkinSemantic(const D3D12_INPUT_ELEMENT_DESC& elem, const char* semantic) {
    return elem.Format == DXGI_FORMAT_R16G16B16A16_UINT &&
           elem.SemanticIndex == 0 && elem.SemanticName &&
           std::strcmp(elem.SemanticName, semantic) == 0;
}

bool BuildExpandedEightWeightLayout(const D3D12_INPUT_LAYOUT_DESC& input,
                                    std::array<D3D12_INPUT_ELEMENT_DESC, 66>& output,
                                    UINT* outCount) {
    if (outCount)
        *outCount = 0;
    if (!input.pInputElementDescs || !input.NumElements || input.NumElements > 64)
        return false;

    std::array<UINT, 32> slotEnd{};
    UINT count = 0;
    UINT splitWeights = 0;
    UINT splitIndices = 0;

    for (UINT i = 0; i < input.NumElements; ++i) {
        const auto& elem = input.pInputElementDescs[i];
        if (elem.InputSlot >= slotEnd.size())
            return false;

        UINT resolvedOffset = elem.AlignedByteOffset;
        if (resolvedOffset == D3D12_APPEND_ALIGNED_ELEMENT)
            resolvedOffset = slotEnd[elem.InputSlot];

        const bool isWeights = IsCustomSkinSemantic(elem, "BLENDWEIGHT");
        const bool isIndices = IsCustomSkinSemantic(elem, "BLENDINDICES");
        if (isWeights || isIndices) {
            auto low = elem;
            low.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            low.SemanticIndex = 0;
            low.AlignedByteOffset = resolvedOffset;

            auto high = low;
            high.SemanticIndex = 1;
            high.AlignedByteOffset = resolvedOffset + 4;

            output[count++] = low;
            output[count++] = high;
            slotEnd[elem.InputSlot] = resolvedOffset + 8;
            if (isWeights) ++splitWeights;
            else ++splitIndices;
        } else {
            output[count++] = elem;
            const UINT bytes = FormatByteSize(elem.Format);
            if (bytes && resolvedOffset + bytes > slotEnd[elem.InputSlot])
                slotEnd[elem.InputSlot] = resolvedOffset + bytes;
        }
    }

    if (splitWeights != 1 || splitIndices != 1)
        return false;
    if (outCount)
        *outCount = count;
    return true;
}

bool HasExperimentalLayout(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, UINT* customCount) {
    if (customCount)
        *customCount = 0;
    if (!desc || !desc->InputLayout.pInputElementDescs || !desc->InputLayout.NumElements ||
        desc->InputLayout.NumElements > 64)
        return false;

    UINT count = 0;
    for (UINT i = 0; i < desc->InputLayout.NumElements; ++i) {
        if (desc->InputLayout.pInputElementDescs[i].Format == DXGI_FORMAT_R16G16B16A16_UINT)
            ++count;
    }
    if (customCount)
        *customCount = count;
    return count >= 2;
}

// (v0.9.606) The game's pipelines timed: how many, the time in the driver's CreateGraphicsPipelineState and the time in
// this hook around it (the framework's own: the shader replacements, the wireframe and lights' pass records, the ray
// tracing probe's look at the pipeline). Read by the loads' LoadProf lines and StartupTiming (PipelineStats).
std::atomic<std::uint64_t> g_pipelineCount{0}, g_pipelineDriverTicks{0}, g_pipelineHookTicks{0};
std::uint64_t QpcTicks() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<std::uint64_t>(now.QuadPart);
}
struct PipelineTimer {
    std::uint64_t start = QpcTicks();
    ~PipelineTimer() {
        g_pipelineCount.fetch_add(1, std::memory_order_relaxed);
        g_pipelineHookTicks.fetch_add(QpcTicks() - start, std::memory_order_relaxed);
    }
};
HRESULT TimedCreate(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID iid, void** outState) {
    const std::uint64_t start = QpcTicks();
    const HRESULT hr = g_realCreateGraphicsPipelineState(device, desc, iid, outState);
    g_pipelineDriverTicks.fetch_add(QpcTicks() - start, std::memory_order_relaxed);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateGraphicsPipelineState(
    ID3D12Device* device,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
    REFIID iid,
    void** outState) {
    const PipelineTimer timer;   // (v0.9.606)
    if (!g_realCreateGraphicsPipelineState || !desc)
        return g_realCreateGraphicsPipelineState
            ? TimedCreate(device, desc, iid, outState)
            : E_FAIL;

    // (v0.9.516) the sun's shadow-mask pass: its pixel shader replaced by the one that blends the cascades
    // (shadow_blend.h); the game's own if the device refuses it
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* const gameDesc = desc;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC shadowDesc;
    // (v0.9.527) the lights' pass: made with the settings' key now, its description kept to be made again for others
    const int lightVariant = ShadowBlend::LightPassVariant(desc->PS);
    // (v0.9.570) the sun's shadow pass: made with its key now (the cascade blend either way), kept to be made again
    const int sunVariant = lightVariant < 0 ? ShadowBlend::SunPassVariant(desc->PS) : -1;
    std::uint32_t lightKey = 0, sunKey = 0;
    const D3D12_SHADER_BYTECODE* ps = nullptr;
    if (lightVariant >= 0) {
        lightKey = ShadowBlend::LightPassKey();
        ps = ShadowBlend::LightPassBytecode(lightVariant, lightKey);
    } else if (sunVariant >= 0) {
        sunKey = ShadowBlend::SunPassKey();
        ps = ShadowBlend::SunPassBytecode(sunVariant, sunKey);
    } else {
        ps = ShadowBlend::Replacement(desc->PS);
    }
    bool sunKept = sunVariant >= 0;
    if (ps) {
        shadowDesc = *desc;
        shadowDesc.PS = *ps;
        shadowDesc.CachedPSO.pCachedBlob = nullptr;
        shadowDesc.CachedPSO.CachedBlobSizeInBytes = 0;
        desc = &shadowDesc;
    }

    UINT customCount = 0;
    const bool experimental = HasExperimentalLayout(desc, &customCount) &&
                              desc->VS.pShaderBytecode && desc->VS.BytecodeLength;

    if (!experimental) {
        HRESULT hr = TimedCreate(device, desc, iid, outState);
        if (FAILED(hr) && desc != gameDesc) {
            ShadowBlend::Refused(desc->PS, static_cast<long>(hr));
            desc = gameDesc;
            lightKey = 0;
            sunKept = false;   // (v0.9.570: the game's own then; not made again)
            hr = TimedCreate(device, desc, iid, outState);
        }
        if (SUCCEEDED(hr) && outState && *outState) {
            KeepWireframeDescription(*desc, *outState);
            if (lightVariant >= 0) KeepLightPassDescription(*gameDesc, lightVariant, lightKey, *outState);
            if (sunKept) KeepLightPassDescription(*gameDesc, sunVariant, sunKey, *outState, true);   // (v0.9.570)
        }
        return hr;
    }

    const auto originalBytes = static_cast<std::size_t>(desc->VS.BytecodeLength);
    const std::uint64_t originalHash = Fnv1a64(desc->VS.pShaderBytecode, originalBytes);
    const auto* replacement = GetOrRewriteShader(originalHash, desc->VS.pShaderBytecode, originalBytes);

    if (g_seenCustomPso.fetch_add(1, std::memory_order_relaxed) == 0)
        Log::Detail("8W: first 8-weight DX12 PSO reached integrated shader replacement path");

    if (!replacement || !replacement->success) {
        if (MarkShaderHashSeen(originalHash)) {
            DumpUnknownShader(originalHash, desc->VS.pShaderBytecode, originalBytes);
            Log::Writef("8W: automatic DXBC rewrite skipped shader 0x%016llX bytes=%zu (%s); using stock PSO",
                        static_cast<unsigned long long>(originalHash), originalBytes,
                        replacement ? replacement->error.c_str() : "rewrite unavailable");
        }
        const HRESULT hr = TimedCreate(device, desc, iid, outState);
        if (SUCCEEDED(hr) && outState && *outState)
            KeepWireframeDescription(*desc, *outState);
        return hr;
    }

    std::array<D3D12_INPUT_ELEMENT_DESC, 66> expanded{};
    UINT expandedCount = 0;
    if (!BuildExpandedEightWeightLayout(desc->InputLayout, expanded, &expandedCount)) {
        Log::Writef("8W: failed to expand 8-weight input layout for shader 0x%016llX; using stock PSO",
                    static_cast<unsigned long long>(originalHash));
        const HRESULT hr = TimedCreate(device, desc, iid, outState);
        if (SUCCEEDED(hr) && outState && *outState)
            KeepWireframeDescription(*desc, *outState);
        return hr;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC patched = *desc;
    patched.VS.pShaderBytecode = replacement->data.data();
    patched.VS.BytecodeLength = replacement->data.size();
    patched.InputLayout.pInputElementDescs = expanded.data();
    patched.InputLayout.NumElements = expandedCount;
    patched.CachedPSO.pCachedBlob = nullptr;
    patched.CachedPSO.CachedBlobSizeInBytes = 0;

    HRESULT hr = TimedCreate(device, &patched, iid, outState);

    if (MarkShaderHashSeen(originalHash)) {
        Log::Detailf("8W: automatically rewrote DX12 skinning shader 0x%016llX (%zu -> %zu bytes, weightedOps=%u, temps=%u->%u, layout %u->%u, hr=0x%08X)",
                    static_cast<unsigned long long>(originalHash), originalBytes, replacement->data.size(),
                    replacement->info.weightedOperationCount, replacement->info.oldTempCount, replacement->info.newTempCount,
                    desc->InputLayout.NumElements, expandedCount, static_cast<unsigned>(hr));
    }

    if (FAILED(hr)) {
        Log::Write("8W: patched PSO creation failed; retrying stock PSO");
        hr = TimedCreate(device, desc, iid, outState);
        if (SUCCEEDED(hr) && outState && *outState)
            KeepWireframeDescription(*desc, *outState);
        return hr;
    }

    if (outState && *outState)
        KeepWireframeDescription(patched, *outState);
    return hr;
}

HRESULT WINAPI HookD3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL minimumFeatureLevel,
                                     REFIID iid, void** outDevice) {
    const HRESULT hr = g_realD3D12CreateDevice
        ? g_realD3D12CreateDevice(adapter, minimumFeatureLevel, iid, outDevice)
        : E_FAIL;
    if (SUCCEEDED(hr) && outDevice && *outDevice)
        EightWeightPatch::AttachDx12Device(reinterpret_cast<ID3D12Device*>(*outDevice));
    return hr;
}

// (v0.9.517) D3D12CreateDevice hooked in d3d12.dll itself. The game looks it up (GetProcAddress through its import
// table, retail 0xC9A00E into its device manager's +0x70, and 0xC81093) and may do so before this framework's start-up
// thread hooks that import: 0.9.516's log had no "intercepted ... lookup" line, and the device was only reached through
// the overlay's swap chain, after the renderer had made its pipelines (the sun's shadow pass among them: its shader
// was never replaced). Through the export's own code every call is seen, however early it was looked up.
D3D12CreateDeviceFn g_exportD3D12CreateDevice = nullptr;   // (MinHook's trampoline)
std::atomic<unsigned> g_exportDevices{0};
HRESULT WINAPI HookD3D12CreateDeviceExport(IUnknown* adapter, D3D_FEATURE_LEVEL minimumFeatureLevel, REFIID iid,
                                           void** outDevice) {
    const HRESULT hr = g_exportD3D12CreateDevice(adapter, minimumFeatureLevel, iid, outDevice);
    if (SUCCEEDED(hr) && outDevice && *outDevice) {
        if (g_exportDevices.fetch_add(1, std::memory_order_relaxed) == 0)
            Log::Write("8W: the game's D3D12 device hooked as it was made (D3D12CreateDevice): every pipeline is seen");
        EightWeightPatch::AttachDx12Device(reinterpret_cast<ID3D12Device*>(*outDevice));
    }
    return hr;
}
std::atomic<bool> g_exportHooked{false};
bool InstallD3D12CreateDeviceExportHook() {
    if (g_exportHooked.load()) return true;
    const HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    void* target = d3d12 ? reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    if (!target) return false;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookD3D12CreateDeviceExport),
                      reinterpret_cast<void**>(&g_exportD3D12CreateDevice)) != MH_OK)
        return false;
    if (MH_EnableHook(target) != MH_OK) return false;
    g_exportHooked.store(true);
    return true;
}

FARPROC WINAPI HookGetProcAddress(HMODULE module, LPCSTR name) {
    FARPROC real = g_realGetProcAddress ? g_realGetProcAddress(module, name) : nullptr;
    if (name && reinterpret_cast<std::uintptr_t>(name) > 0xFFFFu &&
        std::strcmp(name, "D3D12CreateDevice") == 0 && real) {
        g_realD3D12CreateDevice = reinterpret_cast<D3D12CreateDeviceFn>(real);
        if (g_seenEarlyD3D12Lookup.fetch_add(1, std::memory_order_relaxed) == 0)
            Log::Detail("8W: intercepted ROTTR D3D12CreateDevice lookup for early shader-hook attachment");
        return reinterpret_cast<FARPROC>(&HookD3D12CreateDevice);
    }
    return real;
}

bool InstallEarlyD3D12LookupHook() {
    if (!g_base)
        return false;
    auto** slot = reinterpret_cast<void**>(g_base + kRvaIatGetProcAddress);
    if (!slot || !*slot)
        return false;
    void* original = nullptr;
    if (!PatchPointer(slot, reinterpret_cast<void*>(&HookGetProcAddress), &original))
        return false;
    g_realGetProcAddress = reinterpret_cast<GetProcAddressFn>(original);
    return g_realGetProcAddress != nullptr;
}

} 

extern "C" std::uint16_t VertexElemGetSizeHookCore(const void* elem) {
    if (!elem)
        return 0;
    const auto type = *(static_cast<const std::uint8_t*>(elem) + 6);
    if (type == kTypeUShort4EightWeights) {
        if (g_seenType22.fetch_add(1, std::memory_order_relaxed) == 0)
            Log::Detail("8W: vertex element type 22 enabled (8-byte skin indices)");
        return 8;
    }
    if (type == kTypeWeightsUHalf4EightWeights) {
        if (g_seenType23.fetch_add(1, std::memory_order_relaxed) == 0)
            Log::Detail("8W: vertex element type 23 enabled (8-byte skin weights)");
        return 8;
    }

    switch (type) {
        case 0: case 4: case 5: case 6: case 7: case 8: case 9: case 11:
        case 12: case 14: case 16: case 17: case 18: case 19: case 21: return 4;
        case 1: case 10: case 13: case 15: case 20: return 8;
        case 2: return 12;
        case 3: return 16;
        default: return 0;
    }
}

extern "C" std::uint32_t RendererTypeToFormatHookCore(std::uint32_t type) {
    if (type == kTypeUShort4EightWeights || type == kTypeWeightsUHalf4EightWeights) {
        if (g_seenRendererCustom.fetch_add(1, std::memory_order_relaxed) == 0)
            Log::Detail("8W: renderer accepted integrated 16-bit skin attributes");
        return static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_UINT);
    }
    return StockDxgiFormat(type);
}

extern "C" std::uint16_t VertexElemGetSizeHook(const void* elem);
extern "C" std::uint32_t RendererTypeToFormatHook(std::uint32_t type);

namespace EightWeightPatch {
bool Install() {
    if (g_installed.load(std::memory_order_acquire))
        return true;

    const HMODULE module = GetModuleHandleW(nullptr);
    if (!module)
        return false;
    g_base = reinterpret_cast<std::uintptr_t>(module);

    void* sizeFn = reinterpret_cast<void*>(g_base + kRvaVertexElemGetSize);
    void* rendererMapFn = reinterpret_cast<void*>(g_base + kRvaRendererTypeToFormat);
    if (!BytesEqual(sizeFn, kGetSizePrologue.data(), kGetSizePrologue.size()) ||
        !BytesEqual(rendererMapFn, kRendererMapPrologue.data(), kRendererMapPrologue.size())) {
        Log::Write("8W: retail vertex-format signatures do not match; integrated 8-weight support disabled");
        return false;
    }
    if (!InstallEntryRedirect(sizeFn, reinterpret_cast<void*>(&VertexElemGetSizeHook),
                              kGetSizePrologue.data(), kGetSizePrologue.size())) {
        Log::Write("8W: VertexElem::GetSize signature mismatch; integrated 8-weight support disabled");
        return false;
    }
    if (!InstallEntryRedirect(rendererMapFn, reinterpret_cast<void*>(&RendererTypeToFormatHook),
                              kRendererMapPrologue.data(), kRendererMapPrologue.size())) {
        Log::Write("8W: renderer vertex-format signature mismatch; integrated 8-weight support disabled");
        return false;
    }

    g_installed.store(true, std::memory_order_release);
    if (!InstallEarlyD3D12LookupHook())
        Log::Write("8W: early D3D12 lookup interception unavailable; swap-chain device attachment will be used");
    if (!InstallD3D12CreateDeviceExportHook())   // (v0.9.517)
        Log::Write("8W: D3D12CreateDevice could not be hooked in d3d12.dll; a device made before the import hook is "
                   "reached through the swap chain (its first pipelines not seen)");
    Log::Detail("8W: integrated vertex-format support installed; automatic DXBC skinning rewriter ready");
    return true;
}

bool InstallDeviceHook() { return InstallD3D12CreateDeviceExportHook(); }

bool AttachDx12Device(ID3D12Device* device) {
    // (v0.9.517: not only once Install() has run: the device hook comes before it, and the pipelines' hook needs none
    // of its vertex formats; an eight-weight layout cannot be met before them)
    if (!device)
        return false;

    void** vtable = *reinterpret_cast<void***>(device);
    if (!vtable)
        return false;

    bool psoHookReady = false;
    void** psoSlot = &vtable[kCreateGraphicsPipelineStateSlot];
    if (*psoSlot == reinterpret_cast<void*>(&HookCreateGraphicsPipelineState)) {
        psoHookReady = true;
    } else if (!g_realCreateGraphicsPipelineState ||
               *psoSlot == reinterpret_cast<void*>(g_realCreateGraphicsPipelineState)) {
        void* original = nullptr;
        if (PatchPointer(psoSlot, reinterpret_cast<void*>(&HookCreateGraphicsPipelineState), &original)) {
            if (!g_realCreateGraphicsPipelineState)
                g_realCreateGraphicsPipelineState = reinterpret_cast<CreateGraphicsPipelineStateFn>(original);
            psoHookReady = true;
        } else {
            Log::Write("8W: failed to patch ID3D12Device::CreateGraphicsPipelineState");
        }
    } else {
        Log::Write("8W: game DX12 device exposes a different PSO implementation; leaving it untouched");
    }

    bool commandListFactoryReady = false;
    void** commandListSlot = &vtable[kCreateCommandListSlot];
    if (*commandListSlot == reinterpret_cast<void*>(&HookCreateCommandList)) {
        commandListFactoryReady = true;
    } else if (!g_realCreateCommandList ||
               *commandListSlot == reinterpret_cast<void*>(g_realCreateCommandList)) {
        void* original = nullptr;
        if (PatchPointer(commandListSlot, reinterpret_cast<void*>(&HookCreateCommandList), &original)) {
            if (!g_realCreateCommandList)
                g_realCreateCommandList = reinterpret_cast<CreateCommandListFn>(original);
            commandListFactoryReady = true;
        } else {
            Log::Write("Wireframe: failed to patch ID3D12Device::CreateCommandList");
        }
    } else {
        Log::Write("Wireframe: game DX12 device exposes a different CreateCommandList implementation; leaving it untouched");
    }

    g_dx12Attached.store(psoHookReady, std::memory_order_release);
    if (psoHookReady) {
        Log::Detail(commandListFactoryReady
            ? "8W: integrated DX12 shader replacement + wireframe PSO tracking armed on ROTTR game device"
            : "8W: integrated DX12 eight-influence shader replacement armed on ROTTR game device");
    }
    return psoHookReady;
}

bool Installed() {
    return g_installed.load(std::memory_order_acquire);
}

bool Dx12Attached() {
    return g_dx12Attached.load(std::memory_order_acquire);
}

PipelineStats GamePipelineStats() {   // (v0.9.606)
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const double perTick = frequency.QuadPart > 0 ? 1.0 / double(frequency.QuadPart) : 0.0;
    PipelineStats stats;
    stats.count = g_pipelineCount.load(std::memory_order_relaxed);
    stats.driverSeconds = double(g_pipelineDriverTicks.load(std::memory_order_relaxed)) * perTick;
    stats.hookSeconds = double(g_pipelineHookTicks.load(std::memory_order_relaxed)) * perTick;
    return stats;
}

std::uint32_t PipelineGeneration() {   // (v0.9.557)
    return g_pipelineGeneration.load(std::memory_order_acquire);
}

bool IsLightPassPipeline(ID3D12PipelineState* pipelineState) {   // (v0.9.555)
    if (!pipelineState || !g_lightPassLive.load(std::memory_order_relaxed)) return false;
    for (std::size_t i = 0; i < kLightPassSlots; ++i)
        if (g_lightPassGame[i].load(std::memory_order_relaxed) == pipelineState)
            return !g_lightPassSun[i].load(std::memory_order_relaxed);   // (v0.9.570: not the sun pass's)
    return false;
}

bool IsSunPassPipeline(ID3D12PipelineState* pipelineState) {   // (v0.9.570)
    if (!pipelineState || !g_lightPassLive.load(std::memory_order_relaxed)) return false;
    for (std::size_t i = 0; i < kLightPassSlots; ++i)
        if (g_lightPassGame[i].load(std::memory_order_relaxed) == pipelineState)
            return g_lightPassSun[i].load(std::memory_order_relaxed);
    return false;
}

bool PipelineGeometryOf(ID3D12PipelineState* pipelineState, PipelineGeometry& out) {   // (v0.9.543)
    out = {};
    if (!pipelineState) return false;
    WireframeRecord* record = AcquireWireframeRecord(pipelineState);
    if (!record) return false;
    record->Geometry(out);
    record->Release();
    return out.valid;
}

bool WireframeAvailable() {
    return g_dx12Attached.load(std::memory_order_acquire) &&
           g_wireframeCommandListHookAttached.load(std::memory_order_acquire) &&
           g_wireframeDrawsHooked.load(std::memory_order_acquire);   // (v0.9.607)
}
bool WireframeEnabled() {
    return g_wireframeEnabled.load(std::memory_order_relaxed);
}
namespace {
const char* WireframeDepthConvention() {
    const auto less = g_depthLessPipelines.load(std::memory_order_relaxed);
    const auto greater = g_depthGreaterPipelines.load(std::memory_order_relaxed);
    return greater > less ? "reversed (near = 1)" : "near = 0";
}
}  // namespace
void SetWireframeEnabled(bool value) {
    if (value && !WireframeAvailable()) value = false;
    DrainWireframePendingReleases();
    if (value == g_wireframeEnabled.load(std::memory_order_relaxed)) return;
    if (value) {   // (v0.9.607)
        g_wireframeEpoch.fetch_add(1, std::memory_order_acq_rel);
        g_wireframeLinesThisTime.store(0, std::memory_order_relaxed);
        g_wireframeDrawsReplayed.store(0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(g_wireframeKindMutex);
            g_wireframeKindCount = 0;
            g_wireframeKindOthers = 0;
        }
        float line[4]{};
        {
            std::lock_guard<std::mutex> lock(g_wireframeOptionsMutex);
            std::memcpy(line, g_wireframeLineColor, sizeof(line));
        }
        Log::Writef("Wireframe: on (%s; lines %.2f %.2f %.2f, opacity %.2f%s%s): %u of the game's %u pipelines draw geometry "
                    "and get lines; the game's depth: %s (%u depth-writing pipelines test less, %u greater)",
                    g_wireframeStyle.load() == 1 ? "lines only" : "over the picture", line[0], line[1], line[2], line[3],
                    g_wireframeSeeThrough.load() ? ", see through" : "", g_wireframeBackFaces.load() ? ", back faces" : "",
                    g_wireframeOverlayLive.load(std::memory_order_relaxed), g_wireframeRecordsLive.load(std::memory_order_relaxed),
                    WireframeDepthConvention(), g_depthLessPipelines.load(std::memory_order_relaxed),
                    g_depthGreaterPipelines.load(std::memory_order_relaxed));
    }
    g_wireframeEnabled.store(value, std::memory_order_release);
    if (!value) {
        std::string kinds;
        {
            std::lock_guard<std::mutex> lock(g_wireframeKindMutex);
            char part[160]{};
            for (std::size_t i = 0; i < g_wireframeKindCount; ++i) {
                const WireframeKind& k = g_wireframeKinds[i];
                std::snprintf(part, sizeof(part), "%s%u x %u targets (target 0 DXGI %u), depth DXGI %u %s%s", i ? "; " : "",
                              k.count, k.targets, static_cast<unsigned>(k.target0), static_cast<unsigned>(k.depth),
                              DepthTestName(k.test), k.writesDepth ? " written" : "");
                kinds += part;
            }
            if (g_wireframeKindOthers) {
                std::snprintf(part, sizeof(part), "; %u of other kinds", g_wireframeKindOthers);
                kinds += part;
            }
        }
        Log::Writef("Wireframe: off (%u lines' pipelines made while on, %u in the session, %u flat surfaces', %u failed; "
                    "%llu draws drawn again with lines; shader bytecode kept %.1f MB). By kind: %s",
                    g_wireframeLinesThisTime.load(std::memory_order_relaxed),
                    g_wireframeMirrorsCreated.load(std::memory_order_relaxed),
                    g_wireframeFillsCreated.load(std::memory_order_relaxed),
                    g_wireframeMirrorFailures.load(std::memory_order_relaxed),
                    static_cast<unsigned long long>(g_wireframeDrawsReplayed.load(std::memory_order_relaxed)),
                    static_cast<double>(g_wireframeShaderBytesLive.load(std::memory_order_relaxed)) / (1024.0 * 1024.0),
                    kinds.empty() ? "none" : kinds.c_str());
    }
}
WireframeOptions GetWireframeOptions() {   // (v0.9.607)
    WireframeOptions options;
    options.style = g_wireframeStyle.load(std::memory_order_relaxed);
    options.seeThrough = g_wireframeSeeThrough.load(std::memory_order_relaxed);
    options.backFaces = g_wireframeBackFaces.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_wireframeOptionsMutex);
    std::memcpy(options.lineColor, g_wireframeLineColor, sizeof(options.lineColor));
    std::memcpy(options.fillColor, g_wireframeFillColor, sizeof(options.fillColor));
    return options;
}
void SetWireframeOptions(const WireframeOptions& options) {   // (v0.9.607)
    auto clampColor = [](float value, float high) { return std::isfinite(value) ? std::clamp(value, 0.0f, high) : 0.0f; };
    bool remake = options.seeThrough != g_wireframeSeeThrough.load() || options.backFaces != g_wireframeBackFaces.load();
    {
        std::lock_guard<std::mutex> lock(g_wireframeOptionsMutex);
        for (int i = 0; i < 4; ++i) {
            const float value = clampColor(options.lineColor[i], i == 3 ? 1.0f : 64.0f);
            remake = remake || value != g_wireframeLineColor[i];
            g_wireframeLineColor[i] = value;
        }
        for (int i = 0; i < 3; ++i) {
            const float value = clampColor(options.fillColor[i], 64.0f);
            remake = remake || value != g_wireframeFillColor[i];
            g_wireframeFillColor[i] = value;
        }
    }
    g_wireframeSeeThrough.store(options.seeThrough, std::memory_order_relaxed);
    g_wireframeBackFaces.store(options.backFaces, std::memory_order_relaxed);
    const int style = options.style == 1 ? 1 : 0;
    const bool restyled = style != g_wireframeStyle.exchange(style, std::memory_order_relaxed);
    if (remake) {
        // made again as they are drawn; the bindings the command lists keep are not used for the next draws
        g_wireframeGeneration.fetch_add(1, std::memory_order_acq_rel);
        g_wireframeEpoch.fetch_add(1, std::memory_order_acq_rel);
    }
    if ((remake || restyled) && g_wireframeEnabled.load(std::memory_order_relaxed))
        Log::Detailf("Wireframe: options changed (%s; lines %.2f %.2f %.2f, opacity %.2f%s%s)%s",
                     style == 1 ? "lines only" : "over the picture", options.lineColor[0], options.lineColor[1],
                     options.lineColor[2], options.lineColor[3], options.seeThrough ? ", see through" : "",
                     options.backFaces ? ", back faces" : "", remake ? ": its pipelines made again as they are drawn" : "");
}
const char* WireframeStatus() {   // (v0.9.607)
    thread_local char text[192]{};
    if (!WireframeAvailable()) return "Unavailable (DirectX 12 only; see ROTTRFramework.log)";
    std::snprintf(text, sizeof(text), "%u of the game's %u pipelines get lines; %u made since on; depth %s",
                  g_wireframeOverlayLive.load(std::memory_order_relaxed), g_wireframeRecordsLive.load(std::memory_order_relaxed),
                  g_wireframeLinesThisTime.load(std::memory_order_relaxed), WireframeDepthConvention());
    return text;
}
void SetWireframeExcludedCommandList(ID3D12GraphicsCommandList* commandList) {
    g_wireframeExcludedCommandList.store(commandList, std::memory_order_release);
}
}
