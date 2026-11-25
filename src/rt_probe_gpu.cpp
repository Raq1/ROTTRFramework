// (v0.9.543) The ray tracing probe's GPU side: see rt_probe_gpu.h.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "rt_probe_gpu.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace RtProbeGpu {
namespace {
#include "rt_probe_cs.inc"   // kRtProbeCs: rt_probe.hlsl as DXIL
#include "rt_probe_skin_cs.inc"   // (v0.9.545) kRtProbeSkinCs: rt_probe_skin.hlsl as DXIL
#include "rt_probe_uv_cs.inc"     // (v0.9.552) kRtProbeUvCs: rt_probe_uv.hlsl as DXIL
#include "rt_probe_shadow_cs.inc" // (v0.9.560) kRtProbeShadowCs: rt_probe_shadow.hlsl as DXIL
#include "rt_probe_blur_cs.inc"   // (v0.9.564) kRtProbeBlurCs: rt_probe_blur.hlsl as DXIL

constexpr UINT64 kAlign = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;   // 256
constexpr UINT64 kChunkSize = 32ull << 20;   // (v0.9.544: 32 MB, was 64: what one eviction lets go)
constexpr UINT64 kScratchSize = 64ull << 20;
constexpr UINT kRootConstants = 20;
constexpr UINT kSkinConstants = 12;
// (v0.9.545) a frame's skinned meshes at most: how many, and their vertices in all
// (v0.9.550: 256, 4 M before; 250 seen in a camp; v0.9.551: 512, 6 M before; 700 and more in the same camp once its
// instanced skinned draws were taken: the bushes, pots and shared clothes)
constexpr UINT kMaxSkinned = 1024, kMaxSkinnedVertices = 8u << 20;
// (v0.9.552) the cutouts: textures a frame (the heap's block a frame slot: the view's UAV, then these), the UV arena
// (64 MB: 2.8 M triangles; emptied when nine tenths full), meshes' UVs gathered a frame at most
// (v0.9.560) + the mask's UAV, the table's UAV and the mask's SRV after the textures
constexpr UINT kTexturesPerFrame = 2048, kMaskUavAt = 1 + kTexturesPerFrame, kTableUavAt = kMaskUavAt + 1,
               kMaskSrvAt = kMaskUavAt + 2, kHeapBlock = kMaskUavAt + 4;
// (v0.9.571) a mask's size for a render size: half of it, at most 1280 wide (a third at 3840): the shaders read it at
// its own size over the render size
void MaskSize(UINT renderWidth, UINT renderHeight, UINT& width, UINT& height) {
    const float scale = std::max(2.0f, static_cast<float>(renderWidth) / 1280.0f);
    width = static_cast<UINT>(std::ceil(static_cast<float>(renderWidth) / scale));
    height = static_cast<UINT>(std::ceil(static_cast<float>(renderHeight) / scale));
}
// (v0.9.564) in the shadow pass's block: kMaskUavAt the trace's output (rawMask_), kTableUavAt the table, kMaskSrvAt the
// trace's output read by the blur, kBlurUavAt the blur's output (mask_); in the view's, kMaskSrvAt is mask_'s SRV
constexpr UINT kBlurUavAt = kMaskUavAt + 3;
constexpr UINT64 kUvArenaBytes = 64ull << 20;
constexpr UINT kUvJobsPerFrame = 512, kUvConstants = 8;

std::uint64_t Fnv(const void* data, std::size_t size, std::uint64_t h = 1469598103934665603ull) {
    const auto* b = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

// (v0.9.552) a game texture's descriptor into the probe's heap (false: the source could not be read)
bool CopyDescriptor(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE to, std::uint64_t from) noexcept {
    __try {
        D3D12_CPU_DESCRIPTOR_HANDLE source;
        source.ptr = static_cast<SIZE_T>(from);
        device->CopyDescriptorsSimple(1, to, source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

HRESULT SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC& desc, ID3DBlob** blob) {
    using SerializeFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    auto serialize = d3d12 ? reinterpret_cast<SerializeFn>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
    if (!serialize) return E_FAIL;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob, &errors);
    if (errors) errors->Release();
    return hr;
}

UINT64 AlignUp(UINT64 value, UINT64 align) { return (value + align - 1) & ~(align - 1); }

template <typename T>
void SafeRelease(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = resource;
    return b;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}
}  // namespace

int SkinFormatCode(DXGI_FORMAT format) {   // (v0.9.545) rt_probe_skin.hlsl's Decode
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return 0;
        case DXGI_FORMAT_R16G16B16A16_UNORM: return 1;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return 2;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return 3;
        default: return -1;
    }
}

int CutoutUvKind(DXGI_FORMAT format, UINT& componentBytes) {   // (v0.9.552) rt_probe_uv.hlsl's LoadComponent
    switch (format) {
        case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32A32_FLOAT:
            componentBytes = 4;
            return 0;
        case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16B16A16_FLOAT:
            componentBytes = 2;
            return 1;
        case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16B16A16_SNORM:
            componentBytes = 2;
            return 2;
        case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16B16A16_UNORM:
            componentBytes = 2;
            return 3;
        case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8B8A8_SNORM:
            componentBytes = 1;
            return 4;
        case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM:
            componentBytes = 1;
            return 5;
        default:
            componentBytes = 0;
            return -1;
    }
}

bool SupportedVertexFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R32G32B32_FLOAT:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8_SNORM:
            return true;
        default:
            return false;
    }
}

bool Probe::Initialize(ID3D12Device* device, std::string& error) {
    Shutdown();
    if (!device) {
        error = "no device";
        return false;
    }
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device_))) || !device_) {
        error = "the device has no ID3D12Device5 (DirectX Raytracing)";
        device_ = nullptr;
        return false;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
    if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options))) ||
        options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
        char text[96];
        std::snprintf(text, sizeof(text), "DirectX Raytracing tier 1.1 needed (the device has tier %d)",
                      static_cast<int>(options.RaytracingTier));
        error = text;
        Shutdown();
        return false;
    }
    // root signature: b0 the camera (constants), t0 the scene (root SRV), u0 the output (a table); (v0.9.552) t1 the
    // cutout records, t2 the UV arena (root SRVs), t0..2047 space 1 the frame's textures (a table), s0 a sampler
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1;
    D3D12_DESCRIPTOR_RANGE textureRange{};
    textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRange.NumDescriptors = kTexturesPerFrame;
    textureRange.RegisterSpace = 1;
    D3D12_DESCRIPTOR_RANGE maskRange{};   // (v0.9.560) t3: the lamp shadow mask
    maskRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    maskRange.NumDescriptors = 1;
    maskRange.BaseShaderRegister = 3;
    D3D12_ROOT_PARAMETER params[7]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = kRootConstants;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &range;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[3].Descriptor.ShaderRegister = 1;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[4].Descriptor.ShaderRegister = 2;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[5].DescriptorTable.NumDescriptorRanges = 1;
    params[5].DescriptorTable.pDescriptorRanges = &textureRange;
    params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[6].DescriptorTable.NumDescriptorRanges = 1;
    params[6].DescriptorTable.pDescriptorRanges = &maskRange;
    params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 7;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &sampler;
    using SerializeFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    auto serialize = d3d12 ? reinterpret_cast<SerializeFn>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    if (!serialize || FAILED(serialize(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) || !blob) {
        error = "the root signature could not be serialized";
        SafeRelease(errors);
        Shutdown();
        return false;
    }
    SafeRelease(errors);
    const HRESULT rsResult =
        device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSignature_));
    blob->Release();
    if (FAILED(rsResult)) {
        error = "the root signature could not be created";
        Shutdown();
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSignature_;
    pso.CS.pShaderBytecode = kRtProbeCs;
    pso.CS.BytecodeLength = sizeof(kRtProbeCs);
    const HRESULT psoResult = device_->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline_));
    if (FAILED(psoResult)) {
        char text[96];
        std::snprintf(text, sizeof(text), "the ray query shader was refused (0x%08X)", static_cast<unsigned>(psoResult));
        error = text;
        Shutdown();
        return false;
    }
    {   // (v0.9.545) the skinning: b0 the job (constants), t0..t3 the streams and the palette, u0 the posed vertices
        D3D12_ROOT_PARAMETER skin[6]{};
        skin[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        skin[0].Constants.Num32BitValues = kSkinConstants;
        for (UINT i = 1; i <= 4; ++i) {
            skin[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            skin[i].Descriptor.ShaderRegister = i - 1;
        }
        skin[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        D3D12_ROOT_SIGNATURE_DESC skinDesc{};
        skinDesc.NumParameters = 6;
        skinDesc.pParameters = skin;
        ID3DBlob* skinBlob = nullptr;
        if (FAILED(SerializeRootSignature(skinDesc, &skinBlob)) || !skinBlob ||
            FAILED(device_->CreateRootSignature(0, skinBlob->GetBufferPointer(), skinBlob->GetBufferSize(),
                                                IID_PPV_ARGS(&skinRoot_)))) {
            SafeRelease(skinBlob);
            error = "the skinning root signature could not be made";
            Shutdown();
            return false;
        }
        skinBlob->Release();
        D3D12_COMPUTE_PIPELINE_STATE_DESC skinPso{};
        skinPso.pRootSignature = skinRoot_;
        skinPso.CS.pShaderBytecode = kRtProbeSkinCs;
        skinPso.CS.BytecodeLength = sizeof(kRtProbeSkinCs);
        if (FAILED(device_->CreateComputePipelineState(&skinPso, IID_PPV_ARGS(&skinPipeline_)))) {
            error = "the skinning shader was refused";
            Shutdown();
            return false;
        }
    }
    {   // (v0.9.552) the cutouts' UV gather: b0 the job, t0 the indices, t1 the UV stream, u0 the arena
        D3D12_ROOT_PARAMETER uv[4]{};
        uv[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        uv[0].Constants.Num32BitValues = kUvConstants;
        uv[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        uv[1].Descriptor.ShaderRegister = 0;
        uv[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        uv[2].Descriptor.ShaderRegister = 1;
        uv[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        D3D12_ROOT_SIGNATURE_DESC uvDesc{};
        uvDesc.NumParameters = 4;
        uvDesc.pParameters = uv;
        ID3DBlob* uvBlob = nullptr;
        if (FAILED(SerializeRootSignature(uvDesc, &uvBlob)) || !uvBlob ||
            FAILED(device_->CreateRootSignature(0, uvBlob->GetBufferPointer(), uvBlob->GetBufferSize(),
                                                IID_PPV_ARGS(&uvRoot_)))) {
            SafeRelease(uvBlob);
            error = "the UV gather's root signature could not be made";
            Shutdown();
            return false;
        }
        uvBlob->Release();
        D3D12_COMPUTE_PIPELINE_STATE_DESC uvPso{};
        uvPso.pRootSignature = uvRoot_;
        uvPso.CS.pShaderBytecode = kRtProbeUvCs;
        uvPso.CS.BytecodeLength = sizeof(kRtProbeUvCs);
        if (FAILED(device_->CreateComputePipelineState(&uvPso, IID_PPV_ARGS(&uvPipeline_)))) {
            error = "the UV gather shader was refused";
            Shutdown();
            return false;
        }
        uvArena_ = CreateBuffer(kUvArenaBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 3 * kSlots * kHeapBlock;   // (v0.9.561: the shadow pass's blocks after the view's; v0.9.570 the sun's after them)
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (!uvArena_ || FAILED(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_)))) {
            error = "the cutouts' buffers could not be made";
            Shutdown();
            return false;
        }
        heapIncrement_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    {   // (v0.9.560) the lamp shadow mask: b0 its frame, t0 the scene, t1 / t2 the cutouts' records and UVs (root views),
        // the textures (space 1), u0 / u1 the mask and the table (a table), s0 a sampler
        D3D12_DESCRIPTOR_RANGE textures{};
        textures.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        textures.NumDescriptors = kTexturesPerFrame;
        textures.RegisterSpace = 1;
        D3D12_DESCRIPTOR_RANGE outputs{};
        outputs.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        outputs.NumDescriptors = 2;
        D3D12_ROOT_PARAMETER sp[6]{};
        sp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        for (UINT i = 1; i <= 3; ++i) {
            sp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            sp[i].Descriptor.ShaderRegister = i - 1;
        }
        sp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        sp[4].DescriptorTable.NumDescriptorRanges = 1;
        sp[4].DescriptorTable.pDescriptorRanges = &textures;
        sp[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        sp[5].DescriptorTable.NumDescriptorRanges = 1;
        sp[5].DescriptorTable.pDescriptorRanges = &outputs;
        D3D12_STATIC_SAMPLER_DESC wrap{};
        wrap.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        wrap.AddressU = wrap.AddressV = wrap.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        wrap.MaxLOD = D3D12_FLOAT32_MAX;
        wrap.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC shadowDesc{};
        shadowDesc.NumParameters = 6;
        shadowDesc.pParameters = sp;
        shadowDesc.NumStaticSamplers = 1;
        shadowDesc.pStaticSamplers = &wrap;
        ID3DBlob* shadowBlob = nullptr;
        if (FAILED(SerializeRootSignature(shadowDesc, &shadowBlob)) || !shadowBlob ||
            FAILED(device_->CreateRootSignature(0, shadowBlob->GetBufferPointer(), shadowBlob->GetBufferSize(),
                                                IID_PPV_ARGS(&shadowRoot_)))) {
            SafeRelease(shadowBlob);
            error = "the lamp shadow mask's root signature could not be made";
            Shutdown();
            return false;
        }
        shadowBlob->Release();
        D3D12_COMPUTE_PIPELINE_STATE_DESC shadowPso{};
        shadowPso.pRootSignature = shadowRoot_;
        shadowPso.CS.pShaderBytecode = kRtProbeShadowCs;
        shadowPso.CS.BytecodeLength = sizeof(kRtProbeShadowCs);
        if (FAILED(device_->CreateComputePipelineState(&shadowPso, IID_PPV_ARGS(&shadowPipeline_)))) {
            error = "the lamp shadow mask's shader was refused";
            Shutdown();
            return false;
        }
        {   // (v0.9.564) the mask's blur: b0 four constants, a table of t0 (the trace's output) and u0 (the blurred mask)
            D3D12_DESCRIPTOR_RANGE ranges[2]{};
            ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[0].NumDescriptors = 1;
            ranges[0].OffsetInDescriptorsFromTableStart = 0;
            ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            ranges[1].NumDescriptors = 1;
            ranges[1].OffsetInDescriptorsFromTableStart = 1;
            D3D12_ROOT_PARAMETER bp[2]{};
            bp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            bp[0].Constants.Num32BitValues = 4;
            bp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            bp[1].DescriptorTable.NumDescriptorRanges = 2;
            bp[1].DescriptorTable.pDescriptorRanges = ranges;
            D3D12_ROOT_SIGNATURE_DESC blurDesc{};
            blurDesc.NumParameters = 2;
            blurDesc.pParameters = bp;
            ID3DBlob* blurBlob = nullptr;
            if (FAILED(SerializeRootSignature(blurDesc, &blurBlob)) || !blurBlob ||
                FAILED(device_->CreateRootSignature(0, blurBlob->GetBufferPointer(), blurBlob->GetBufferSize(),
                                                    IID_PPV_ARGS(&blurRoot_)))) {
                SafeRelease(blurBlob);
                error = "the lamp shadow mask's blur could not be made";
                Shutdown();
                return false;
            }
            blurBlob->Release();
            D3D12_COMPUTE_PIPELINE_STATE_DESC blurPso{};
            blurPso.pRootSignature = blurRoot_;
            blurPso.CS.pShaderBytecode = kRtProbeBlurCs;
            blurPso.CS.BytecodeLength = sizeof(kRtProbeBlurCs);
            if (FAILED(device_->CreateComputePipelineState(&blurPso, IID_PPV_ARGS(&blurPipeline_)))) {
                error = "the lamp shadow mask's blur shader was refused";
                Shutdown();
                return false;
            }
            D3D12_QUERY_HEAP_DESC queries{};   // (optional: the pass is not timed without them)
            queries.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            queries.Count = kSlots * 4;   // (v0.9.570: the sun's pass's after the lamps')
            if (FAILED(device_->CreateQueryHeap(&queries, IID_PPV_ARGS(&shadowQueries_)))) shadowQueries_ = nullptr;
            shadowReadback_ = CreateBuffer(kSlots * 4 * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
            for (bool& w : shadowStamped_) w = false;
            for (bool& w : sunStamped_) w = false;
        }
        {   // (v0.9.562) the table: a 4 x 1 RGBA32F texture (texels 0 .. 2 the lamps; was a buffer)
            D3D12_HEAP_PROPERTIES props{};
            props.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = 4;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(device_->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&table_))))
                table_ = nullptr;
            ++maskGeneration_;
        }
        tableReadable_ = false;
        for (UINT i = 0; i < kSlots; ++i) {
            shadowConstants_[i] = CreateBuffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
            sunConstants_[i] = CreateBuffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);   // (v0.9.570)
        }
    }
    {   // (v0.9.555) the timestamps (optional: the GPU's time not measured without them)
        D3D12_QUERY_HEAP_DESC queries{};
        queries.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        queries.Count = kSlots * kStamps;
        if (FAILED(device_->CreateQueryHeap(&queries, IID_PPV_ARGS(&timestamps_)))) timestamps_ = nullptr;
        timestampReadback_ = CreateBuffer(kSlots * kStamps * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
        for (bool& w : timestampsWritten_) w = false;
    }
    scratch_ = CreateBuffer(kScratchSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    scratchSize_ = scratch_ ? kScratchSize : 0;
    if (!scratch_) {
        error = "the build scratch buffer could not be made";
        Shutdown();
        return false;
    }
    return true;
}

void Probe::Shutdown() {
    for (auto& r : retired_) SafeRelease(r.resource);
    retired_.clear();
    for (auto& c : chunks_) SafeRelease(c.buffer);
    chunks_.clear();
    meshes_.clear();
    pending_.clear();
    SafeRelease(scratch_);
    SafeRelease(tlas_);
    SafeRelease(tlasScratch_);
    for (UINT i = 0; i < kSlots; ++i) {
        SafeRelease(instanceUpload_[i]);
        instanceCapacity_[i] = 0;
    }
    SafeRelease(output_);
    outputWidth_ = outputHeight_ = 0;
    outputInShaderResource_ = false;
    for (UINT i = 0; i < kSlots; ++i) {   // (v0.9.545)
        SafeRelease(paletteUpload_[i]);
        paletteCapacity_[i] = 0;
    }
    SafeRelease(skinOutput_);
    SafeRelease(skinArena_);
    skinOutputSize_ = skinArenaSize_ = 0;
    skinned_.clear();
    paletteBytes_.clear();
    SafeRelease(skinPipeline_);
    SafeRelease(skinRoot_);
    for (UINT i = 0; i < kSlots; ++i) {   // (v0.9.552)
        SafeRelease(recordUpload_[i]);
        recordCapacity_[i] = 0;
    }
    SafeRelease(uvArena_);
    SafeRelease(uvPipeline_);
    SafeRelease(uvRoot_);
    SafeRelease(heap_);
    SafeRelease(timestamps_);   // (v0.9.555)
    SafeRelease(shadowPipeline_);   // (v0.9.560)
    SafeRelease(shadowRoot_);
    SafeRelease(mask_);
    SafeRelease(rawMask_);   // (v0.9.564)
    SafeRelease(sunMask_);   // (v0.9.570)
    SafeRelease(rawSun_);
    for (UINT i = 0; i < kSlots; ++i) {
        SafeRelease(sunConstants_[i]);
        SafeRelease(sunRecords_[i]);
        sunRecordCapacity_[i] = 0;
    }
    for (bool& w : sunStamped_) w = false;
    sunWidth_ = sunHeight_ = 0;
    rawSunReadable_ = sunMaskReadable_ = false;
    SafeRelease(blurPipeline_);
    SafeRelease(blurRoot_);
    SafeRelease(shadowQueries_);
    SafeRelease(shadowReadback_);
    for (bool& w : shadowStamped_) w = false;
    rawReadable_ = false;
    SafeRelease(table_);
    for (UINT i = 0; i < kSlots; ++i) SafeRelease(shadowConstants_[i]);
    maskWidth_ = maskHeight_ = 0;
    for (UINT i = 0; i < kSlots; ++i) {
        SafeRelease(shadowRecords_[i]);
        shadowRecordCapacity_[i] = 0;
    }
    maskReadable_ = tableReadable_ = false;
    lastSlot_ = kSlots;
    SafeRelease(timestampReadback_);
    uvUsed_ = 0;
    uvs_.clear();
    uvJobs_.clear();
    records_.clear();
    recordIndex_.clear();
    textures_.clear();
    textureIndex_.clear();
    SafeRelease(pipeline_);
    SafeRelease(rootSignature_);
    SafeRelease(device_);
    memory_ = 0;
    scratchSize_ = tlasSize_ = tlasScratchSize_ = 0;
    stats_ = {};
}

ID3D12Resource* Probe::CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, bool uav) {
    D3D12_HEAP_PROPERTIES props{};
    props.Type = heap;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* resource = nullptr;
    if (FAILED(device_->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource))))
        return nullptr;
    return resource;
}

// (v0.9.544) Chunks are filled in order and let go oldest first when the budget is reached: their meshes are built
// again when they are drawn again (0.9.543 stopped building at the budget: 12,000 meshes in a level, then nothing new).
D3D12_GPU_VIRTUAL_ADDRESS Probe::AllocateBlas(UINT64 size, std::uint64_t key) {
    size = AlignUp(size, kAlign);
    if (!chunks_.empty()) {   // (the newest chunk: chunks fill in order)
        Chunk& c = chunks_.back();
        if (c.size - c.used >= size) {
            const D3D12_GPU_VIRTUAL_ADDRESS at = c.buffer->GetGPUVirtualAddress() + c.used;
            c.used += size;
            c.keys.push_back(key);
            c.lastBuild = frame_;
            return at;
        }
    }
    const UINT64 chunkSize = std::max(size, kChunkSize);
    while (memory_ + chunkSize > maxMemory_)
        if (!EvictOldest()) return 0;
    Chunk c;
    c.size = chunkSize;
    c.buffer = CreateBuffer(c.size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
    if (!c.buffer) return 0;
    memory_ += c.size;
    c.used = size;
    c.keys.push_back(key);
    c.lastBuild = frame_;
    chunks_.push_back(c);
    return chunks_.back().buffer->GetGPUVirtualAddress();
}

bool Probe::EvictOldest() {
    if (chunks_.empty() || chunks_.front().lastBuild == frame_) return false;
    Chunk c = chunks_.front();
    chunks_.erase(chunks_.begin());
    for (std::uint64_t key : c.keys) {
        auto it = meshes_.find(key);
        if (it != meshes_.end() && it->second.blas) {
            it->second.blas = 0;
            ++stats_.evictedTotal;
        }
    }
    memory_ -= c.size;
    Retire(c.buffer);   // (released when the GPU is past this frame's earlier uses)
    return true;
}

void Probe::Retire(ID3D12Resource*& resource) {
    if (resource) retired_.push_back({resource, 0});
    resource = nullptr;
}

void Probe::AgeRetired() {
    for (auto& r : retired_) ++r.age;
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                  [](Retired& r) {
                                      if (r.age < kRetireFrames) return false;
                                      SafeRelease(r.resource);
                                      return true;
                                  }),
                   retired_.end());
}

void Probe::ResetMeshes() {
    for (auto& c : chunks_) Retire(c.buffer);
    chunks_.clear();
    meshes_.clear();
    memory_ = 0;
}

void Probe::BeginFrame() {
    pending_.clear();
    stats_.skinnedDropped = 0;
    skinned_.clear();   // (v0.9.545)
    skinnedQueued_ = 0;   // (v0.9.551)
    paletteBytes_.clear();
    // (v0.9.552) the cutouts: a frame's records and textures; UVs not gathered (a frame not recorded) forgotten; the arena
    // emptied when nine tenths full (the frames before read it first: one queue)
    for (const UvJob& job : uvJobs_) uvs_.erase(job.key);
    uvJobs_.clear();
    records_.clear();
    recordIndex_.clear();
    textures_.clear();
    textureIndex_.clear();
    cutoutRefused_ = 0;
    if (uvUsed_ * 8 > kUvArenaBytes * 9 / 10) {
        uvs_.clear();
        uvUsed_ = 0;
    }
}

// (v0.9.552) A draw's cutout record (deduplicated; -1: no room this frame, the instance traced opaque): its mesh's UVs
// (gathered this frame if not yet) and its texture in the frame's table
int Probe::RecordFor(const Cutout& cutout, D3D12_GPU_VIRTUAL_ADDRESS indices, DXGI_FORMAT indexFormat, UINT indexCount) {
    if (!heap_ || !uvArena_ || !cutout.texture || !cutout.uvs || !cutout.uvStride || indexCount < 3) return -1;
    struct UvSource {
        D3D12_GPU_VIRTUAL_ADDRESS uvs, indices;
        UINT stride, kind, at0, at1, format, count;
    } source{cutout.uvs, indices, cutout.uvStride, cutout.uvKind, cutout.uvAt[0], cutout.uvAt[1],
             static_cast<UINT>(indexFormat), indexCount};
    const std::uint64_t uvKey = Fnv(&source, sizeof(source));
    UINT uvAt = 0;
    auto uv = uvs_.find(uvKey);
    if (uv != uvs_.end()) {
        uvAt = uv->second;
    } else {
        const UINT triangles = indexCount / 3;
        if (uvJobs_.size() >= kUvJobsPerFrame || (uvUsed_ + triangles * 3ull) * 8 > kUvArenaBytes) {
            ++cutoutRefused_;
            return -1;
        }
        uvAt = static_cast<UINT>(uvUsed_);
        uvUsed_ += triangles * 3ull;
        uvs_.emplace(uvKey, uvAt);
        uvJobs_.push_back({cutout, indices, indexFormat, triangles, uvAt, uvKey});
    }
    UINT texture = 0;
    auto t = textureIndex_.find(cutout.texture);
    if (t != textureIndex_.end()) {
        texture = t->second;
    } else {
        if (textures_.size() >= kTexturesPerFrame) {
            ++cutoutRefused_;
            return -1;
        }
        texture = static_cast<UINT>(textures_.size());
        textures_.push_back(cutout.texture);
        textureIndex_.emplace(cutout.texture, texture);
    }
    const CutoutRecord r{{cutout.scale[0], cutout.scale[1]}, {cutout.offset[0], cutout.offset[1]}, texture,
                         cutout.channel & 3, cutout.threshold, uvAt};
    const std::uint64_t key = Fnv(&r, sizeof(r));
    auto existing = recordIndex_.find(key);
    if (existing != recordIndex_.end()) return static_cast<int>(existing->second);
    records_.push_back(r);
    recordIndex_.emplace(key, static_cast<UINT>(records_.size() - 1));
    return static_cast<int>(records_.size() - 1);
}

// (v0.9.552) The frame's new meshes' UVs gathered into the arena (in UNORDERED_ACCESS)
void Probe::RecordUvJobs(ID3D12GraphicsCommandList4* rt) {
    stats_.uvGathered = 0;
    if (uvJobs_.empty()) return;
    rt->SetComputeRootSignature(uvRoot_);
    rt->SetPipelineState(uvPipeline_);
    for (const UvJob& job : uvJobs_) {
        const UINT indexSize = job.indexFormat == DXGI_FORMAT_R16_UINT ? 2 : 4;
        const UINT indexSkew = static_cast<UINT>(job.indices & 3);
        const UINT uvSkew = static_cast<UINT>(job.cutout.uvs & 3);
        const UINT constants[kUvConstants] = {job.triangles, indexSize, indexSkew, job.cutout.uvStride, job.cutout.uvKind,
                                              job.cutout.uvAt[0] + uvSkew, job.cutout.uvAt[1] + uvSkew, job.at};
        rt->SetComputeRoot32BitConstants(0, kUvConstants, constants, 0);
        rt->SetComputeRootShaderResourceView(1, job.indices & ~static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(3));
        rt->SetComputeRootShaderResourceView(2, job.cutout.uvs & ~static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(3));
        rt->SetComputeRootUnorderedAccessView(3, uvArena_->GetGPUVirtualAddress());
        rt->Dispatch((job.triangles + 63) / 64, 1, 1);
        ++stats_.uvGathered;
    }
    uvJobs_.clear();   // (recorded: kept in uvs_)
}

bool Probe::AddSkinned(const SkinnedGeometry& geometry, const void* palette, UINT paletteEntries, std::uint32_t id,
                       const Cutout* cutout, D3D12_GPU_VIRTUAL_ADDRESS paletteGpu, bool uncertain) {
    // (v0.9.551) the vertex limit here too (the caller learns which were taken)
    if ((!palette && !paletteGpu) || !paletteEntries || skinned_.size() >= kMaxSkinned || !geometry.vertexCount ||
        skinnedQueued_ + geometry.vertexCount > kMaxSkinnedVertices) {
        ++stats_.skinnedDropped;
        return false;
    }
    skinnedQueued_ += geometry.vertexCount;
    Skinned k;
    k.geometry = geometry;
    k.paletteOffset = static_cast<UINT>(paletteBytes_.size());
    k.paletteEntries = paletteEntries;
    k.id = id & 0xFFFFFF;
    k.record = cutout ? RecordFor(*cutout, geometry.indices, geometry.indexBufferFormat, geometry.indexCount) : -1;
    k.paletteGpu = paletteGpu;   // (v0.9.559) read where it is: nothing copied
    if (!paletteGpu) {
        const auto* bytes = static_cast<const std::uint8_t*>(palette);
        paletteBytes_.insert(paletteBytes_.end(), bytes, bytes + static_cast<std::size_t>(paletteEntries) * 64);
    }
    k.uncertain = uncertain;   // (v0.9.570)
    skinned_.push_back(k);
    return true;
}

// (v0.9.545) Each skinned mesh posed into skinOutput_ (one dispatch each, its streams and palette as root views), then
// built (fast build) into skinArena_ (made again larger when the frame needs more), with an instance of identity
// transform (the posed positions are world space already).
void Probe::RecordSkinned(ID3D12GraphicsCommandList4* rt, UINT frameSlot, UINT64& scratchUsed,
                          std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& instances) {
    stats_.skinned = 0;
    stats_.skinnedVertices = 0;
    if (skinned_.empty()) return;
    // what fits this frame
    std::vector<UINT> outputAt(skinned_.size(), UINT_MAX);
    UINT64 vertices = 0;
    for (std::size_t i = 0; i < skinned_.size(); ++i) {
        const UINT n = skinned_[i].geometry.vertexCount;
        if (!n || vertices + n > kMaxSkinnedVertices) {
            ++stats_.skinnedDropped;
            continue;
        }
        outputAt[i] = static_cast<UINT>(vertices);
        vertices += n;
    }
    if (!vertices) return;
    // the palettes into this slot's upload buffer
    const UINT64 paletteBytes = AlignUp(std::max<UINT64>(paletteBytes_.size(), 256), 256);
    if (paletteCapacity_[frameSlot] < paletteBytes) {
        Retire(paletteUpload_[frameSlot]);
        const UINT64 capacity = AlignUp(paletteBytes * 3 / 2 + 64 * 1024, 64 * 1024);
        paletteUpload_[frameSlot] = CreateBuffer(capacity, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
        paletteCapacity_[frameSlot] = paletteUpload_[frameSlot] ? capacity : 0;
        if (!paletteUpload_[frameSlot]) return;
    }
    void* mapped = nullptr;
    D3D12_RANGE none{0, 0};
    if (FAILED(paletteUpload_[frameSlot]->Map(0, &none, &mapped)) || !mapped) return;
    std::memcpy(mapped, paletteBytes_.data(), paletteBytes_.size());
    paletteUpload_[frameSlot]->Unmap(0, nullptr);
    // the posed vertices' buffer
    const UINT64 outputBytes = AlignUp(vertices * 12, 64 * 1024);
    if (skinOutputSize_ < outputBytes) {
        Retire(skinOutput_);
        skinOutputSize_ = AlignUp(outputBytes * 3 / 2, 64 * 1024);
        skinOutput_ = CreateBuffer(skinOutputSize_, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        if (!skinOutput_) {
            skinOutputSize_ = 0;
            return;
        }
    }
    // pose them
    rt->SetComputeRootSignature(skinRoot_);
    rt->SetPipelineState(skinPipeline_);
    const D3D12_GPU_VIRTUAL_ADDRESS palettes = paletteUpload_[frameSlot]->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS posed = skinOutput_->GetGPUVirtualAddress();
    for (std::size_t i = 0; i < skinned_.size(); ++i) {
        if (outputAt[i] == UINT_MAX) continue;
        const Skinned& k = skinned_[i];
        const SkinnedGeometry& g = k.geometry;
        const UINT job[kSkinConstants] = {g.firstVertex, g.vertexCount, g.positionStride, g.positionOffset, g.weightStride,
                                          g.weightOffset, g.weightFormat, g.indexStride, g.indexOffset, g.indexFormat,
                                          k.paletteEntries, g.use};
        rt->SetComputeRoot32BitConstants(0, kSkinConstants, job, 0);
        rt->SetComputeRootShaderResourceView(1, g.positions);
        rt->SetComputeRootShaderResourceView(2, g.weights);
        rt->SetComputeRootShaderResourceView(3, g.blendIndices);
        rt->SetComputeRootShaderResourceView(4, k.paletteGpu ? k.paletteGpu : palettes + k.paletteOffset);   // (v0.9.559)
        rt->SetComputeRootUnorderedAccessView(5, posed + static_cast<UINT64>(outputAt[i]) * 12);
        rt->Dispatch((g.vertexCount + 63) / 64, 1, 1);
    }
    {
        const D3D12_RESOURCE_BARRIER b = Transition(skinOutput_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        rt->ResourceBarrier(1, &b);
    }
    // their structures: sizes first, then the arena, then the builds
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries(skinned_.size());
    std::vector<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO> infos(skinned_.size());
    UINT64 arenaNeeded = 0;
    for (std::size_t i = 0; i < skinned_.size(); ++i) {
        if (outputAt[i] == UINT_MAX) continue;
        const SkinnedGeometry& g = skinned_[i].geometry;
        D3D12_RAYTRACING_GEOMETRY_DESC& d = geometries[i];
        d.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        d.Flags = skinned_[i].record >= 0 ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        d.Triangles.VertexBuffer.StartAddress = posed + static_cast<UINT64>(outputAt[i]) * 12;
        d.Triangles.VertexBuffer.StrideInBytes = 12;
        d.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        d.Triangles.VertexCount = g.vertexCount;
        d.Triangles.IndexBuffer = g.indices;
        d.Triangles.IndexFormat = g.indexBufferFormat;
        d.Triangles.IndexCount = g.indexCount;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        inputs.NumDescs = 1;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.pGeometryDescs = &d;
        device_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &infos[i]);
        if (!infos[i].ResultDataMaxSizeInBytes || AlignUp(infos[i].ScratchDataSizeInBytes, kAlign) > scratchSize_) {
            outputAt[i] = UINT_MAX;
            ++stats_.skinnedDropped;
            continue;
        }
        arenaNeeded += AlignUp(infos[i].ResultDataMaxSizeInBytes, kAlign);
    }
    if (skinArenaSize_ < arenaNeeded) {
        Retire(skinArena_);
        skinArenaSize_ = AlignUp(arenaNeeded * 3 / 2, 1 << 20);
        skinArena_ = CreateBuffer(skinArenaSize_, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
        if (!skinArena_) {
            skinArenaSize_ = 0;
            const D3D12_RESOURCE_BARRIER b = Transition(skinOutput_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            rt->ResourceBarrier(1, &b);
            return;
        }
    }
    stats_.skinnedMemory = skinArenaSize_;
    UINT64 arenaUsed = 0;
    for (std::size_t i = 0; i < skinned_.size(); ++i) {
        if (outputAt[i] == UINT_MAX) continue;
        const UINT64 scratchNeeded = AlignUp(infos[i].ScratchDataSizeInBytes, kAlign);
        if (scratchUsed + scratchNeeded > scratchSize_) {
            const D3D12_RESOURCE_BARRIER b = UavBarrier(scratch_);
            rt->ResourceBarrier(1, &b);
            scratchUsed = 0;
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        build.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        build.Inputs.NumDescs = 1;
        build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        build.Inputs.pGeometryDescs = &geometries[i];
        build.DestAccelerationStructureData = skinArena_->GetGPUVirtualAddress() + arenaUsed;
        build.ScratchAccelerationStructureData = scratch_->GetGPUVirtualAddress() + scratchUsed;
        rt->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        scratchUsed += scratchNeeded;
        D3D12_RAYTRACING_INSTANCE_DESC d{};
        d.Transform[0][0] = d.Transform[1][1] = d.Transform[2][2] = 1.0f;
        d.InstanceID = skinned_[i].id;
        d.InstanceMask = skinned_[i].uncertain ? 0x02 : 0x01;   // (v0.9.570; was 0xFF)
        if (skinned_[i].uncertain) ++stats_.uncertain;
        const bool uncertain = skinned_[i].uncertain && skinned_[i].record < 0;
        d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE | D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
        if (skinned_[i].record >= 0) {   // (v0.9.552) its alpha test: record + 1
            d.InstanceContributionToHitGroupIndex = static_cast<UINT>(skinned_[i].record) + 1;
            d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
            ++stats_.cutouts;
        } else if (uncertain) {   // (v0.9.571) not opaque, its record the shaders' kUncertain
            d.InstanceContributionToHitGroupIndex = 0xFFFFFF;
            d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE | D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;
        }
        d.AccelerationStructure = build.DestAccelerationStructureData;
        instances.push_back(d);
        arenaUsed += AlignUp(infos[i].ResultDataMaxSizeInBytes, kAlign);
        ++stats_.skinned;
        stats_.skinnedVertices += skinned_[i].geometry.vertexCount;
    }
    {   // (back for the next frame's posing; the builds read it before)
        const D3D12_RESOURCE_BARRIER b[2] = {UavBarrier(nullptr),
                                             Transition(skinOutput_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        rt->ResourceBarrier(2, b);
    }
}

void Probe::AddInstance(std::uint64_t meshKey, const Geometry& geometry, const float transform[3][4], std::uint32_t id,
                        const Cutout* cutout, bool uncertain) {
    auto it = meshes_.find(meshKey);
    if (it == meshes_.end()) {
        Mesh m;
        m.geometry = geometry;
        m.cutout = cutout != nullptr;   // (v0.9.552)
        it = meshes_.emplace(meshKey, m).first;
    }
    if (it->second.failed) return;
    Pending p;
    p.key = meshKey;
    std::memcpy(p.transform, transform, sizeof(p.transform));
    p.id = id & 0xFFFFFF;
    p.uncertain = uncertain;   // (v0.9.570)
    if (cutout && it->second.cutout)   // (v0.9.552)
        p.record = RecordFor(*cutout, geometry.indices, geometry.indexFormat, geometry.indexCount);
    pending_.push_back(p);
}

// (v0.9.560) the lamp shadow mask at this size (made again on a change; in UNORDERED_ACCESS)
bool Probe::EnsureMask(UINT width, UINT height) {
    if (mask_ && rawMask_ && maskWidth_ == width && maskHeight_ == height) return true;
    Retire(mask_);
    Retire(rawMask_);   // (v0.9.564)
    maskWidth_ = maskHeight_ = 0;
    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device_->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                nullptr, IID_PPV_ARGS(&mask_))))
        return false;
    if (FAILED(device_->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                nullptr, IID_PPV_ARGS(&rawMask_)))) {   // (v0.9.564) the trace's own
        Retire(mask_);
        return false;
    }
    rawReadable_ = false;
    maskWidth_ = width;
    maskHeight_ = height;
    maskReadable_ = false;   // (v0.9.561: made in UNORDERED_ACCESS)
    ++maskGeneration_;       // (v0.9.562: the lights' pass's descriptors written again, in a new pair)
    return true;
}

// (v0.9.562) the lights' pass's descriptors: the mask's SRV and the table's (a 4 x 1 texture), at the caller's handles
bool Probe::WriteLightPassDescriptors(D3D12_CPU_DESCRIPTOR_HANDLE mask, D3D12_CPU_DESCRIPTOR_HANDLE table) const {
    if (!device_ || !mask_ || !table_ || !mask.ptr || !table.ptr) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(mask_, &srv, mask);
    srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    device_->CreateShaderResourceView(table_, &srv, table);
    return true;
}

// (v0.9.570) the sun's masks at this size (made again on a change; in UNORDERED_ACCESS)
bool Probe::EnsureSunMask(UINT width, UINT height) {
    if (sunMask_ && rawSun_ && sunWidth_ == width && sunHeight_ == height) return true;
    Retire(sunMask_);
    Retire(rawSun_);
    sunWidth_ = sunHeight_ = 0;
    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device_->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                nullptr, IID_PPV_ARGS(&sunMask_))))
        return false;
    if (FAILED(device_->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                nullptr, IID_PPV_ARGS(&rawSun_)))) {
        Retire(sunMask_);
        return false;
    }
    sunWidth_ = width;
    sunHeight_ = height;
    rawSunReadable_ = sunMaskReadable_ = false;
    ++sunGeneration_;
    return true;
}

bool Probe::WriteSunPassDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) const {   // (v0.9.570)
    if (!device_ || !sunMask_ || !handle.ptr) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(sunMask_, &srv, handle);
    return true;
}

// (v0.9.570) The sun's mask before the game's sun shadow pass: the shadow pass's shader in its sun mode, then the blur;
// its own block (after the lamps' blocks), records' copies, constants and timestamps (after the lamps')
bool Probe::RecordSunPass(ID3D12GraphicsCommandList* list, UINT slot) {
    if (!device_ || !list || !tlas_ || lastSlot_ >= kSlots || !shadowPipeline_ || !blurPipeline_ || !heap_ || !uvArena_)
        return false;
    const ShadowSetup& sh = sunSetup_;
    UINT maskWidth = 0, maskHeight = 0;
    MaskSize(sh.renderWidth, sh.renderHeight, maskWidth, maskHeight);   // (v0.9.571; was half)
    if (!sh.enabled || !maskWidth || !maskHeight) return false;
    slot %= kSlots;
    if (!sunConstants_[slot] || !EnsureSunMask(maskWidth, maskHeight)) return false;
    ID3D12GraphicsCommandList4* rt = nullptr;
    if (FAILED(list->QueryInterface(IID_PPV_ARGS(&rt))) || !rt) return false;
    const UINT query = (kSlots + slot) * 2;
    const bool timed = shadowQueries_ && shadowReadback_;
    if (timed && sunStamped_[slot] && timestampFrequency_) {
        const D3D12_RANGE range{query * 8, query * 8 + 16};
        const D3D12_RANGE nothing{0, 0};
        void* stamps = nullptr;
        if (SUCCEEDED(shadowReadback_->Map(0, &range, &stamps)) && stamps) {
            const auto* t = reinterpret_cast<const UINT64*>(static_cast<const std::uint8_t*>(stamps) + query * 8);
            stats_.sunMs = t[1] >= t[0] ? static_cast<float>((t[1] - t[0]) * 1000.0 / static_cast<double>(timestampFrequency_))
                                        : 0.0f;
            shadowReadback_->Unmap(0, &nothing);
        }
    }
    if (timed) rt->EndQuery(shadowQueries_, D3D12_QUERY_TYPE_TIMESTAMP, query);
    const UINT block = (2 * kSlots + slot) * kHeapBlock;
    const D3D12_CPU_DESCRIPTOR_HANDLE blockCpu{heap_->GetCPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(block) * heapIncrement_};
    const D3D12_GPU_DESCRIPTOR_HANDLE blockGpu{heap_->GetGPUDescriptorHandleForHeapStart().ptr + static_cast<UINT64>(block) * heapIncrement_};
    std::vector<CutoutRecord> records = records_;
    std::vector<bool> unread(textures_.size(), false);
    for (std::size_t i = 0; i < textures_.size(); ++i)
        unread[i] = !CopyDescriptor(device_, D3D12_CPU_DESCRIPTOR_HANDLE{blockCpu.ptr + (1 + i) * heapIncrement_}, textures_[i]);
    for (CutoutRecord& r : records)
        if (r.texture < unread.size() && unread[r.texture]) r.threshold = -1.0e30f;
    const UINT64 recordBytes = std::max<UINT64>(1, records.size()) * sizeof(CutoutRecord);
    if (sunRecordCapacity_[slot] < recordBytes) {
        Retire(sunRecords_[slot]);
        const UINT64 capacity = AlignUp(recordBytes * 3 / 2 + 64 * 1024, 64 * 1024);
        sunRecords_[slot] = CreateBuffer(capacity, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
        sunRecordCapacity_[slot] = sunRecords_[slot] ? capacity : 0;
    }
    void* mapped = nullptr;
    const D3D12_RANGE none{0, 0};
    if (!sunRecords_[slot] || FAILED(sunRecords_[slot]->Map(0, &none, &mapped)) || !mapped) {
        rt->Release();
        return false;
    }
    if (!records.empty()) std::memcpy(mapped, records.data(), records.size() * sizeof(CutoutRecord));
    sunRecords_[slot]->Unmap(0, nullptr);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    // the trace writes rawSun_ (u0; u1, the lamps' table there, given rawSun_ too: not written in this mode); the blur
    // reads it (kMaskSrvAt) into sunMask_ (kBlurUavAt)
    device_->CreateUnorderedAccessView(rawSun_, nullptr, &uav, D3D12_CPU_DESCRIPTOR_HANDLE{blockCpu.ptr + static_cast<SIZE_T>(kMaskUavAt) * heapIncrement_});
    device_->CreateUnorderedAccessView(rawSun_, nullptr, &uav, D3D12_CPU_DESCRIPTOR_HANDLE{blockCpu.ptr + static_cast<SIZE_T>(kTableUavAt) * heapIncrement_});
    device_->CreateUnorderedAccessView(sunMask_, nullptr, &uav, D3D12_CPU_DESCRIPTOR_HANDLE{blockCpu.ptr + static_cast<SIZE_T>(kBlurUavAt) * heapIncrement_});
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC raw{};
        raw.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        raw.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        raw.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        raw.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(rawSun_, &raw, D3D12_CPU_DESCRIPTOR_HANDLE{blockCpu.ptr + static_cast<SIZE_T>(kMaskSrvAt) * heapIncrement_});
    }
    struct {
        float view[16], projection[4], extents[4], lamp[3][4], lampView[3][4];
        UINT lampCount, width, height;
        float pixelScale;
        float softness;
        UINT rays;
        float spare[2];
        float sun[4];
        float sunDistance;
        UINT mode;
        float pixelScaleY;   // (v0.9.571)
        float spare2;
    } c{};
    static_assert(sizeof(c) == 256, "the shadow pass's constants");
    std::memcpy(c.view, sh.view, sizeof(c.view));
    std::memcpy(c.projection, sh.projection, sizeof(c.projection));
    std::memcpy(c.extents, sh.extents, sizeof(c.extents));
    std::memcpy(c.sun, sh.sun, sizeof(c.sun));
    c.width = maskWidth;
    c.height = maskHeight;
    c.pixelScale = static_cast<float>(sh.renderWidth) / static_cast<float>(maskWidth);   // (v0.9.571; was 2)
    c.pixelScaleY = static_cast<float>(sh.renderHeight) / static_cast<float>(maskHeight);
    c.softness = std::clamp(sh.softness, 0.0f, 3.0f);
    c.rays = c.softness > 0.0f && c.sun[3] > 0.0f ? std::clamp<UINT>(sh.rays, 1, 16) : 1;
    c.sunDistance = std::clamp(sh.sunDistance, 100.0f, 60000.0f);
    c.mode = 1;
    if (FAILED(sunConstants_[slot]->Map(0, &none, &mapped)) || !mapped) {
        rt->Release();
        return false;
    }
    std::memcpy(mapped, &c, sizeof(c));
    sunConstants_[slot]->Unmap(0, nullptr);
    const D3D12_RESOURCE_STATES readable = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    {
        D3D12_RESOURCE_BARRIER b[2];
        UINT n = 0;
        if (rawSunReadable_)
            b[n++] = Transition(rawSun_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        b[n++] = Transition(uvArena_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        rt->ResourceBarrier(n, b);
    }
    ID3D12DescriptorHeap* heaps[] = {heap_};
    rt->SetDescriptorHeaps(1, heaps);
    rt->SetComputeRootSignature(shadowRoot_);
    rt->SetPipelineState(shadowPipeline_);
    rt->SetComputeRootConstantBufferView(0, sunConstants_[slot]->GetGPUVirtualAddress());
    rt->SetComputeRootShaderResourceView(1, tlas_->GetGPUVirtualAddress());
    rt->SetComputeRootShaderResourceView(2, sunRecords_[slot]->GetGPUVirtualAddress());
    rt->SetComputeRootShaderResourceView(3, uvArena_->GetGPUVirtualAddress());
    rt->SetComputeRootDescriptorTable(4, D3D12_GPU_DESCRIPTOR_HANDLE{blockGpu.ptr + heapIncrement_});
    rt->SetComputeRootDescriptorTable(5, D3D12_GPU_DESCRIPTOR_HANDLE{blockGpu.ptr + static_cast<UINT64>(kMaskUavAt) * heapIncrement_});
    rt->Dispatch((maskWidth + 7) / 8, (maskHeight + 7) / 8, 1);
    {
        D3D12_RESOURCE_BARRIER b[4];
        UINT n = 0;
        b[n++] = UavBarrier(rawSun_);
        b[n++] = Transition(rawSun_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        b[n++] = Transition(uvArena_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (sunMaskReadable_) b[n++] = Transition(sunMask_, readable, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        rt->ResourceBarrier(n, b);
    }
    {
        rt->SetComputeRootSignature(blurRoot_);
        rt->SetPipelineState(blurPipeline_);
        const UINT blur[4] = {maskWidth, maskHeight, static_cast<UINT>(c.rays > 1 ? std::clamp(sh.blurRadius, 0, 4) : 0), 0};
        rt->SetComputeRoot32BitConstants(0, 4, blur, 0);
        rt->SetComputeRootDescriptorTable(1, D3D12_GPU_DESCRIPTOR_HANDLE{blockGpu.ptr + static_cast<UINT64>(kMaskSrvAt) * heapIncrement_});
        rt->Dispatch((maskWidth + 7) / 8, (maskHeight + 7) / 8, 1);
        const D3D12_RESOURCE_BARRIER b[2] = {UavBarrier(sunMask_),
                                             Transition(sunMask_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, readable)};
        rt->ResourceBarrier(2, b);
    }
    if (timed) {
        rt->EndQuery(shadowQueries_, D3D12_QUERY_TYPE_TIMESTAMP, query + 1);
        rt->ResolveQueryData(shadowQueries_, D3D12_QUERY_TYPE_TIMESTAMP, query, 2, shadowReadback_, query * 8);
        sunStamped_[slot] = true;
    }
    sunMaskReadable_ = rawSunReadable_ = true;
    stats_.sunWidth = maskWidth;
    stats_.sunHeight = maskHeight;
    rt->Release();
    return true;
}

// (v0.9.561) The lamp shadow mask before the lights' pass: its own block (the last Record's textures copied again from the
// game's descriptors, the mask's and table's UAVs), its own copy of the last Record's records, its own constants
bool Probe::RecordShadowPass(ID3D12GraphicsCommandList* list, UINT shadowSlot) {
    if (!device_ || !list || !tlas_ || lastSlot_ >= kSlots || !shadowPipeline_ || !blurPipeline_ || !table_ || !heap_ ||
        !uvArena_)
        return false;
    const ShadowSetup& sh = shadow_;
    UINT maskWidth = 0, maskHeight = 0;
    MaskSize(sh.renderWidth, sh.renderHeight, maskWidth, maskHeight);   // (v0.9.571; was half)
    if (!sh.enabled || !sh.lampCount || !maskWidth || !maskHeight) return false;
    shadowSlot %= kSlots;
    if (!shadowConstants_[shadowSlot] || !EnsureMask(maskWidth, maskHeight)) return false;
    ID3D12GraphicsCommandList4* rt = nullptr;
    if (FAILED(list->QueryInterface(IID_PPV_ARGS(&rt))) || !rt) return false;
    // (v0.9.564) this slot's last timing (its work done: the caller waited for the slot), then this one's first stamp
    const bool timed = shadowQueries_ && shadowReadback_;
    if (timed && shadowStamped_[shadowSlot] && timestampFrequency_) {
        const D3D12_RANGE range{shadowSlot * 16, shadowSlot * 16 + 16};
        const D3D12_RANGE nothing{0, 0};
        void* stamps = nullptr;
        if (SUCCEEDED(shadowReadback_->Map(0, &range, &stamps)) && stamps) {
            const auto* t = reinterpret_cast<const UINT64*>(static_cast<const std::uint8_t*>(stamps) + shadowSlot * 16);
            stats_.shadowMs = t[1] >= t[0] ? static_cast<float>((t[1] - t[0]) * 1000.0 / static_cast<double>(timestampFrequency_))
                                           : 0.0f;
            shadowReadback_->Unmap(0, &nothing);
        }
    }
    if (timed) rt->EndQuery(shadowQueries_, D3D12_QUERY_TYPE_TIMESTAMP, shadowSlot * 2);
    const UINT block = (kSlots + shadowSlot) * kHeapBlock;
    const D3D12_CPU_DESCRIPTOR_HANDLE blockCpu{heap_->GetCPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(block) * heapIncrement_};
    const D3D12_GPU_DESCRIPTOR_HANDLE blockGpu{heap_->GetGPUDescriptorHandleForHeapStart().ptr + static_cast<UINT64>(block) * heapIncrement_};
    std::vector<CutoutRecord> records = records_;
    std::vector<bool> unread(textures_.size(), false);
    for (std::size_t i = 0; i < textures_.size(); ++i)
        unread[i] = !CopyDescriptor(device_, D3D12_CPU_DESCRIPTOR_HANDLE{blockCpu.ptr + (1 + i) * heapIncrement_}, textures_[i]);
    for (CutoutRecord& r : records)
        if (r.texture < unread.size() && unread[r.texture]) r.threshold = -1.0e30f;
    const UINT64 recordBytes = std::max<UINT64>(1, records.size()) * sizeof(CutoutRecord);
    if (shadowRecordCapacity_[shadowSlot] < recordBytes) {
        Retire(shadowRecords_[shadowSlot]);
        const UINT64 capacity = AlignUp(recordBytes * 3 / 2 + 64 * 1024, 64 * 1024);
        shadowRecords_[shadowSlot] = CreateBuffer(capacity, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
        shadowRecordCapacity_[shadowSlot] = shadowRecords_[shadowSlot] ? capacity : 0;
    }
    void* mapped = nullptr;
    const D3D12_RANGE none{0, 0};
    if (!shadowRecords_[shadowSlot] || FAILED(shadowRecords_[shadowSlot]->Map(0, &none, &mapped)) || !mapped) {
        rt->Release();
        return false;
    }
    if (!records.empty()) std::memcpy(mapped, records.data(), records.size() * sizeof(CutoutRecord));
    shadowRecords_[shadowSlot]->Unmap(0, nullptr);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    // (v0.9.564) the trace writes rawMask_; the blur reads it (kMaskSrvAt) into mask_ (kBlurUavAt)
    device_->CreateUnorderedAccessView(rawMask_, nullptr, &uav, D3D12_CPU_DESCRIPTOR_HANDLE{blockCpu.ptr + static_cast<SIZE_T>(kMaskUavAt) * heapIncrement_});
    device_->CreateUnorderedAccessView(mask_, nullptr, &uav, D3D12_CPU_DESCRIPTOR_HANDLE{blockCpu.ptr + static_cast<SIZE_T>(kBlurUavAt) * heapIncrement_});
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC raw{};
        raw.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        raw.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        raw.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        raw.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(rawMask_, &raw, D3D12_CPU_DESCRIPTOR_HANDLE{blockCpu.ptr + static_cast<SIZE_T>(kMaskSrvAt) * heapIncrement_});
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC tableDesc{};   // (v0.9.562: a 4 x 1 texture; was a buffer)
    tableDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    tableDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device_->CreateUnorderedAccessView(table_, nullptr, &tableDesc, D3D12_CPU_DESCRIPTOR_HANDLE{blockCpu.ptr + static_cast<SIZE_T>(kTableUavAt) * heapIncrement_});
    struct {
        float view[16], projection[4], extents[4], lamp[3][4], lampView[3][4];
        UINT lampCount, width, height;
        float pixelScale;
        float softness;   // (v0.9.564)
        UINT rays;
        float spare[2];
        float sun[4];     // (v0.9.570: the sun's pass's; 0 here)
        float sunDistance;
        UINT mode;
        float pixelScaleY;   // (v0.9.571)
        float spare2;
    } c{};
    static_assert(sizeof(c) == 256, "the shadow pass's constants");
    std::memcpy(c.view, sh.view, sizeof(c.view));
    std::memcpy(c.projection, sh.projection, sizeof(c.projection));
    std::memcpy(c.extents, sh.extents, sizeof(c.extents));
    std::memcpy(c.lamp, sh.lamp, sizeof(c.lamp));
    std::memcpy(c.lampView, sh.lampView, sizeof(c.lampView));
    c.lampCount = std::min<UINT>(sh.lampCount, 3);
    c.width = maskWidth;
    c.height = maskHeight;
    c.pixelScale = static_cast<float>(sh.renderWidth) / static_cast<float>(maskWidth);   // (v0.9.571; was 2)
    c.pixelScaleY = static_cast<float>(sh.renderHeight) / static_cast<float>(maskHeight);
    c.softness = std::clamp(sh.softness, 0.0f, 3.0f);   // (v0.9.564)
    c.rays = c.softness > 0.0f ? std::clamp<UINT>(sh.rays, 1, 16) : 1;
    if (FAILED(shadowConstants_[shadowSlot]->Map(0, &none, &mapped)) || !mapped) {
        rt->Release();
        return false;
    }
    std::memcpy(mapped, &c, sizeof(c));
    shadowConstants_[shadowSlot]->Unmap(0, nullptr);
    const D3D12_RESOURCE_STATES readable = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    {
        D3D12_RESOURCE_BARRIER b[3];
        UINT n = 0;
        if (rawReadable_)   // (v0.9.564: the trace's output; mask_ goes back to unordered access before the blur)
            b[n++] = Transition(rawMask_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (tableReadable_) b[n++] = Transition(table_, readable, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        b[n++] = Transition(uvArena_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        rt->ResourceBarrier(n, b);
    }
    ID3D12DescriptorHeap* heaps[] = {heap_};
    rt->SetDescriptorHeaps(1, heaps);
    rt->SetComputeRootSignature(shadowRoot_);
    rt->SetPipelineState(shadowPipeline_);
    rt->SetComputeRootConstantBufferView(0, shadowConstants_[shadowSlot]->GetGPUVirtualAddress());
    rt->SetComputeRootShaderResourceView(1, tlas_->GetGPUVirtualAddress());
    rt->SetComputeRootShaderResourceView(2, shadowRecords_[shadowSlot]->GetGPUVirtualAddress());
    rt->SetComputeRootShaderResourceView(3, uvArena_->GetGPUVirtualAddress());
    rt->SetComputeRootDescriptorTable(4, D3D12_GPU_DESCRIPTOR_HANDLE{blockGpu.ptr + heapIncrement_});
    rt->SetComputeRootDescriptorTable(5, D3D12_GPU_DESCRIPTOR_HANDLE{blockGpu.ptr + static_cast<UINT64>(kMaskUavAt) * heapIncrement_});
    rt->Dispatch((maskWidth + 7) / 8, (maskHeight + 7) / 8, 1);
    {
        D3D12_RESOURCE_BARRIER b[6];
        UINT n = 0;
        b[n++] = UavBarrier(rawMask_);
        b[n++] = UavBarrier(table_);
        b[n++] = Transition(rawMask_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        b[n++] = Transition(table_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, readable);
        b[n++] = Transition(uvArena_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (maskReadable_) b[n++] = Transition(mask_, readable, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        rt->ResourceBarrier(n, b);
    }
    {   // (v0.9.564) the blur: rawMask_ into mask_
        rt->SetComputeRootSignature(blurRoot_);
        rt->SetPipelineState(blurPipeline_);
        const UINT blur[4] = {maskWidth, maskHeight,
                              static_cast<UINT>(c.softness > 0.0f ? std::clamp(sh.blurRadius, 0, 4) : 0), 0};
        rt->SetComputeRoot32BitConstants(0, 4, blur, 0);
        rt->SetComputeRootDescriptorTable(1, D3D12_GPU_DESCRIPTOR_HANDLE{blockGpu.ptr + static_cast<UINT64>(kMaskSrvAt) * heapIncrement_});
        rt->Dispatch((maskWidth + 7) / 8, (maskHeight + 7) / 8, 1);
        const D3D12_RESOURCE_BARRIER b[2] = {UavBarrier(mask_),
                                             Transition(mask_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, readable)};
        rt->ResourceBarrier(2, b);
    }
    if (timed) {   // (v0.9.564)
        rt->EndQuery(shadowQueries_, D3D12_QUERY_TYPE_TIMESTAMP, shadowSlot * 2 + 1);
        rt->ResolveQueryData(shadowQueries_, D3D12_QUERY_TYPE_TIMESTAMP, shadowSlot * 2, 2, shadowReadback_, shadowSlot * 16);
        shadowStamped_[shadowSlot] = true;
    }
    maskReadable_ = tableReadable_ = rawReadable_ = true;
    stats_.maskWidth = maskWidth;
    stats_.maskHeight = maskHeight;
    stats_.maskLamps = c.lampCount;
    rt->Release();
    return true;
}

bool Probe::EnsureOutput(UINT width, UINT height, D3D12_CPU_DESCRIPTOR_HANDLE uavCpu, D3D12_CPU_DESCRIPTOR_HANDLE srvCpu) {
    if (output_ && outputWidth_ == width && outputHeight_ == height && outputUav_.ptr == uavCpu.ptr &&
        outputSrv_.ptr == srvCpu.ptr)
        return true;
    Retire(output_);
    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device_->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                nullptr, IID_PPV_ARGS(&output_))))
        return false;
    outputWidth_ = width;
    outputHeight_ = height;
    outputInShaderResource_ = false;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = desc.Format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device_->CreateUnorderedAccessView(output_, nullptr, &uav, uavCpu);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = desc.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    if (srvCpu.ptr) device_->CreateShaderResourceView(output_, &srv, srvCpu);
    outputUav_ = uavCpu;
    outputSrv_ = srvCpu;
    return true;
}

bool Probe::Record(ID3D12GraphicsCommandList* list, UINT frameSlot, UINT width, UINT height, const Camera& camera,
                   D3D12_CPU_DESCRIPTOR_HANDLE uavCpu, D3D12_GPU_DESCRIPTOR_HANDLE uavGpu,
                   D3D12_CPU_DESCRIPTOR_HANDLE srvCpu, bool view) {
    if (!device_ || !list || !width || !height) return false;
    (void)uavGpu;   // (v0.9.552: the view's UAV is in the probe's own heap)
    AgeRetired();
    ID3D12GraphicsCommandList4* rt = nullptr;
    if (FAILED(list->QueryInterface(IID_PPV_ARGS(&rt))) || !rt) return false;
    stats_.built = 0;
    stats_.instancesWaiting = 0;
    ++frame_;
    bool ok = !view || EnsureOutput(width, height, uavCpu, srvCpu);   // (v0.9.561: no view, no output)
    frameSlot %= kSlots;
    // (v0.9.555) this slot's timestamps from its last use (the caller waited for that frame before reusing the slot)
    if (timestamps_ && timestampReadback_ && timestampFrequency_ && timestampsWritten_[frameSlot]) {
        const D3D12_RANGE range{frameSlot * kStamps * 8, (frameSlot + 1) * kStamps * 8};
        void* mapped = nullptr;
        if (SUCCEEDED(timestampReadback_->Map(0, &range, &mapped)) && mapped) {
            UINT64 t[kStamps];
            std::memcpy(t, static_cast<const std::uint8_t*>(mapped) + range.Begin, sizeof(t));
            const D3D12_RANGE none{0, 0};
            timestampReadback_->Unmap(0, &none);
            const double toMs = 1000.0 / static_cast<double>(timestampFrequency_);
            for (UINT i = 0; i + 1 < kStamps; ++i)
                stats_.gpuMs[i] = t[i + 1] >= t[i] ? static_cast<float>((t[i + 1] - t[i]) * toMs) : 0.0f;
            stats_.gpuMs[kStamps - 1] = t[kStamps - 1] >= t[0] ? static_cast<float>((t[kStamps - 1] - t[0]) * toMs) : 0.0f;
            // (v0.9.560: seven stamps, six spans and the whole)
        }
    }
    auto stamp = [&](UINT i) {
        if (timestamps_) rt->EndQuery(timestamps_, D3D12_QUERY_TYPE_TIMESTAMP, frameSlot * kStamps + i);
    };
    stamp(0);

    // 1. the meshes not built yet (a budget a frame), each in its own slice of the scratch buffer
    UINT64 scratchUsed = 0;
    std::uint32_t builds = 0;
    for (const Pending& p : pending_) {
        Mesh& m = meshes_[p.key];
        if (m.blas || m.failed || !ok) continue;
        if (builds >= buildBudget_) continue;
        D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
        geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometry.Flags = m.cutout ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;   // (v0.9.552)
        geometry.Triangles.VertexBuffer.StartAddress = m.geometry.vertices;
        geometry.Triangles.VertexBuffer.StrideInBytes = m.geometry.vertexStride;
        geometry.Triangles.VertexFormat = m.geometry.vertexFormat;
        geometry.Triangles.VertexCount = m.geometry.vertexCount;
        geometry.Triangles.IndexBuffer = m.geometry.indices;
        geometry.Triangles.IndexFormat = m.geometry.indexFormat;
        geometry.Triangles.IndexCount = m.geometry.indexCount;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        build.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        build.Inputs.NumDescs = 1;
        build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        build.Inputs.pGeometryDescs = &geometry;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device_->GetRaytracingAccelerationStructurePrebuildInfo(&build.Inputs, &info);
        const UINT64 scratchNeeded = AlignUp(info.ScratchDataSizeInBytes, kAlign);
        if (!info.ResultDataMaxSizeInBytes || scratchNeeded > scratchSize_) {
            m.failed = true;
            ++stats_.failed;
            continue;
        }
        if (scratchUsed + scratchNeeded > scratchSize_) {   // (the scratch reused: the builds before it finish first)
            const D3D12_RESOURCE_BARRIER b = UavBarrier(scratch_);
            rt->ResourceBarrier(1, &b);
            scratchUsed = 0;
        }
        const D3D12_GPU_VIRTUAL_ADDRESS result = AllocateBlas(info.ResultDataMaxSizeInBytes, p.key);
        if (!result) break;   // (no room this frame: the rest wait)
        build.DestAccelerationStructureData = result;
        build.ScratchAccelerationStructureData = scratch_->GetGPUVirtualAddress() + scratchUsed;
        rt->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        scratchUsed += scratchNeeded;
        m.blas = result;
        ++builds;
        ++stats_.builtTotal;
    }
    stamp(1);   // (v0.9.555)
    // (v0.9.545) the skinned meshes posed and built (their instances first)
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
    instances.reserve(pending_.size() + skinned_.size());
    stats_.cutouts = 0;   // (v0.9.552)
    stats_.uncertain = 0;   // (v0.9.570)
    if (ok) RecordSkinned(rt, frameSlot, scratchUsed, instances);
    stamp(2);
    if (ok) RecordUvJobs(rt);   // (v0.9.552)
    stamp(3);
    if (builds || !instances.empty()) {
        const D3D12_RESOURCE_BARRIER b = UavBarrier(nullptr);   // (every bottom level built before the top one reads it)
        rt->ResourceBarrier(1, &b);
    }
    stats_.built = builds;

    // 2. the frame's instances (each mesh and transform once) into this slot's upload buffer
    for (const Pending& p : pending_) {
        const Mesh& m = meshes_[p.key];
        if (!m.blas) {
            if (!m.failed) ++stats_.instancesWaiting;
            continue;
        }
        D3D12_RAYTRACING_INSTANCE_DESC d{};
        std::memcpy(d.Transform, p.transform, sizeof(d.Transform));
        d.InstanceID = p.id;
        d.InstanceMask = p.uncertain ? 0x02 : 0x01;   // (v0.9.570; was 0xFF)
        if (p.uncertain) ++stats_.uncertain;
        d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE | D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
        if (p.record >= 0) {   // (v0.9.552) its alpha test: record + 1
            d.InstanceContributionToHitGroupIndex = static_cast<UINT>(p.record) + 1;
            d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
            ++stats_.cutouts;
        } else if (p.uncertain) {   // (v0.9.571) not opaque, its record the shaders' kUncertain
            d.InstanceContributionToHitGroupIndex = 0xFFFFFF;
            d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE | D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;
        }
        d.AccelerationStructure = m.blas;
        instances.push_back(d);
    }
    stats_.cutoutRecords = static_cast<std::uint32_t>(records_.size());   // (v0.9.552)
    stats_.cutoutTextures = static_cast<std::uint32_t>(textures_.size());
    stats_.cutoutRefused = cutoutRefused_;
    stats_.uvMemory = uvUsed_ * 8;
    stats_.instances = static_cast<std::uint32_t>(instances.size());
    stats_.meshes = 0;
    for (const auto& kv : meshes_)
        if (kv.second.blas) ++stats_.meshes;
    stats_.memory = memory_;

    const UINT64 instanceBytes = std::max<UINT64>(1, instances.size()) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    if (ok && instanceCapacity_[frameSlot] < instanceBytes) {
        Retire(instanceUpload_[frameSlot]);
        const UINT64 capacity = AlignUp(instanceBytes * 3 / 2 + 64 * 1024, 64 * 1024);
        instanceUpload_[frameSlot] = CreateBuffer(capacity, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
        instanceCapacity_[frameSlot] = instanceUpload_[frameSlot] ? capacity : 0;
        ok = instanceUpload_[frameSlot] != nullptr;
    }
    if (ok && !instances.empty()) {
        void* mapped = nullptr;
        D3D12_RANGE none{0, 0};
        if (SUCCEEDED(instanceUpload_[frameSlot]->Map(0, &none, &mapped)) && mapped) {
            std::memcpy(mapped, instances.data(), instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
            instanceUpload_[frameSlot]->Unmap(0, nullptr);
        } else {
            ok = false;
        }
    }

    // 3. the top level
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC top{};
    top.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    top.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    top.Inputs.NumDescs = static_cast<UINT>(instances.size());
    top.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    top.Inputs.InstanceDescs = ok ? instanceUpload_[frameSlot]->GetGPUVirtualAddress() : 0;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topInfo{};
    if (ok) device_->GetRaytracingAccelerationStructurePrebuildInfo(&top.Inputs, &topInfo);
    if (ok && (tlasSize_ < topInfo.ResultDataMaxSizeInBytes || !tlas_)) {
        Retire(tlas_);
        tlasSize_ = AlignUp(std::max<UINT64>(topInfo.ResultDataMaxSizeInBytes * 3 / 2, 64 * 1024), kAlign);
        tlas_ = CreateBuffer(tlasSize_, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
        if (!tlas_) tlasSize_ = 0;
        ok = tlas_ != nullptr;
    }
    if (ok && (tlasScratchSize_ < topInfo.ScratchDataSizeInBytes || !tlasScratch_)) {
        Retire(tlasScratch_);
        tlasScratchSize_ = AlignUp(std::max<UINT64>(topInfo.ScratchDataSizeInBytes * 3 / 2, 64 * 1024), kAlign);
        tlasScratch_ = CreateBuffer(tlasScratchSize_, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        if (!tlasScratch_) tlasScratchSize_ = 0;
        ok = tlasScratch_ != nullptr;
    }
    if (ok) {
        top.DestAccelerationStructureData = tlas_->GetGPUVirtualAddress();
        top.ScratchAccelerationStructureData = tlasScratch_->GetGPUVirtualAddress();
        rt->BuildRaytracingAccelerationStructure(&top, 0, nullptr);
        const D3D12_RESOURCE_BARRIER b = UavBarrier(tlas_);
        rt->ResourceBarrier(1, &b);
    }
    stamp(4);   // (v0.9.555)

    // (v0.9.552) the cutouts: this slot's heap block (the view's UAV, the textures copied from the game's descriptors)
    // and its records (one at least: the root view needs a buffer)
    const UINT block = frameSlot * kHeapBlock;
    D3D12_CPU_DESCRIPTOR_HANDLE blockCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE blockGpu{};
    if (ok) {
        blockCpu.ptr = heap_->GetCPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(block) * heapIncrement_;
        blockGpu.ptr = heap_->GetGPUDescriptorHandleForHeapStart().ptr + static_cast<UINT64>(block) * heapIncrement_;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(output_, nullptr, &uav, blockCpu);
        std::vector<bool> unread(textures_.size(), false);
        for (std::size_t i = 0; i < textures_.size(); ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE to{blockCpu.ptr + (1 + i) * heapIncrement_};
            unread[i] = !CopyDescriptor(device_, to, textures_[i]);
        }
        for (CutoutRecord& r : records_)   // (a texture not read: its test always passes)
            if (r.texture < unread.size() && unread[r.texture]) r.threshold = -1.0e30f;
        const UINT64 recordBytes = std::max<UINT64>(1, records_.size()) * sizeof(CutoutRecord);
        if (recordCapacity_[frameSlot] < recordBytes) {
            Retire(recordUpload_[frameSlot]);
            const UINT64 capacity = AlignUp(recordBytes * 3 / 2 + 64 * 1024, 64 * 1024);
            recordUpload_[frameSlot] = CreateBuffer(capacity, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
            recordCapacity_[frameSlot] = recordUpload_[frameSlot] ? capacity : 0;
            ok = recordUpload_[frameSlot] != nullptr;
        }
        void* mapped = nullptr;
        D3D12_RANGE none{0, 0};
        if (ok && SUCCEEDED(recordUpload_[frameSlot]->Map(0, &none, &mapped)) && mapped) {
            if (!records_.empty()) std::memcpy(mapped, records_.data(), records_.size() * sizeof(CutoutRecord));
            recordUpload_[frameSlot]->Unmap(0, nullptr);
        } else {
            ok = false;
        }
    }

    // (v0.9.560) the lamp shadow mask; (v0.9.561) made before the lights' pass (RecordShadowPass): the view only reads it
    // (its SRV in this slot's block; a null one without it)
    const bool maskShown = ok && view && mask_ && maskReadable_ && shadow_.show;
    if (ok) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(maskShown ? mask_ : nullptr, &srv,
                                          D3D12_CPU_DESCRIPTOR_HANDLE{blockCpu.ptr + static_cast<SIZE_T>(kMaskSrvAt) * heapIncrement_});
    }
    stamp(5);   // (v0.9.560)

    // 4. the debug view
    if (ok && view) {   // (v0.9.561: not drawn for the lamp shadows alone)
        if (outputInShaderResource_) {
            const D3D12_RESOURCE_BARRIER b =
                Transition(output_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            rt->ResourceBarrier(1, &b);
            outputInShaderResource_ = false;
        }
        float constants[kRootConstants]{};
        std::memcpy(constants + 0, camera.origin, 12);
        constants[3] = camera.tanHalfFov;
        std::memcpy(constants + 4, camera.right, 12);
        constants[7] = static_cast<float>(width) / static_cast<float>(height);
        std::memcpy(constants + 8, camera.up, 12);
        constants[11] = static_cast<float>(width);
        std::memcpy(constants + 12, camera.forward, 12);
        constants[15] = static_cast<float>(height);
        constants[16] = camera.maxDistance;
        constants[17] = camera.minDistance;
        constants[18] = camera.kindView;   // (v0.9.548)
        // (v0.9.560) the mask over the view: a view pixel's texel (the mask spans the render size, the view the output's)
        constants[19] = maskShown ? static_cast<float>(maskWidth_) / static_cast<float>(width) : 0.0f;
        {   // (v0.9.552) the UVs gathered before they are read
            const D3D12_RESOURCE_BARRIER b[2] = {UavBarrier(uvArena_),
                                                 Transition(uvArena_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
            rt->ResourceBarrier(2, b);
        }
        ID3D12DescriptorHeap* heaps[] = {heap_};   // (v0.9.552: the caller sets its own again after)
        rt->SetDescriptorHeaps(1, heaps);
        rt->SetComputeRootSignature(rootSignature_);
        rt->SetPipelineState(pipeline_);
        rt->SetComputeRoot32BitConstants(0, kRootConstants, constants, 0);
        rt->SetComputeRootShaderResourceView(1, tlas_->GetGPUVirtualAddress());
        rt->SetComputeRootDescriptorTable(2, blockGpu);
        rt->SetComputeRootShaderResourceView(3, recordUpload_[frameSlot]->GetGPUVirtualAddress());
        rt->SetComputeRootShaderResourceView(4, uvArena_->GetGPUVirtualAddress());
        rt->SetComputeRootDescriptorTable(5, D3D12_GPU_DESCRIPTOR_HANDLE{blockGpu.ptr + heapIncrement_});
        rt->SetComputeRootDescriptorTable(6, D3D12_GPU_DESCRIPTOR_HANDLE{blockGpu.ptr + static_cast<UINT64>(kMaskSrvAt) * heapIncrement_});
        rt->Dispatch((width + 7) / 8, (height + 7) / 8, 1);   // (v0.9.561: the mask already readable)
        const D3D12_RESOURCE_BARRIER b[2] = {
            Transition(output_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            Transition(uvArena_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        rt->ResourceBarrier(2, b);
        outputInShaderResource_ = true;
    }
    stamp(6);   // (v0.9.555; v0.9.560: the seventh)
    if (ok) lastSlot_ = frameSlot;   // (v0.9.561) the shadow pass takes this frame's textures and records
    if (timestamps_ && timestampReadback_) {
        rt->ResolveQueryData(timestamps_, D3D12_QUERY_TYPE_TIMESTAMP, frameSlot * kStamps, kStamps, timestampReadback_,
                             static_cast<UINT64>(frameSlot) * kStamps * 8);
        timestampsWritten_[frameSlot] = true;
    }
    rt->Release();
    return ok;
}

}  // namespace RtProbeGpu
