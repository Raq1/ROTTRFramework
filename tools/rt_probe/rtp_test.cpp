// Runs RtProbeGpu::Probe on WARP: a ground plane and boxes (float3 positions, 16-bit indices, a second mesh with 32-bit
// indices and an R16G16B16A16_FLOAT format), instanced with row-vector world matrices turned into DXR transforms as
// rt_probe.cpp does; writes the debug view to rtp_out.ppm. rtp_test [frames]
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "rt_probe_gpu.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

static ID3D12Resource* Upload(ID3D12Device* device, const void* data, size_t size) {
    D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    if (FAILED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&r))))
        return nullptr;
    void* p = nullptr;
    r->Map(0, nullptr, &p);
    std::memcpy(p, data, size);
    r->Unmap(0, nullptr);
    return r;
}

static unsigned short Half(float f) {
    unsigned int x;
    std::memcpy(&x, &f, 4);
    const unsigned int sign = (x >> 16) & 0x8000;
    int exp = static_cast<int>((x >> 23) & 0xFF) - 127 + 15;
    unsigned int mant = (x >> 13) & 0x3FF;
    if (exp <= 0) return static_cast<unsigned short>(sign);
    if (exp >= 31) return static_cast<unsigned short>(sign | 0x7C00);
    return static_cast<unsigned short>(sign | (exp << 10) | mant);
}

// the engine's convention: rows = axes, row 3 = position (v * M); DXR: Transform[r][c] = M[c][r]
static void ToDxr(const float m[4][4], float t[3][4]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) t[r][c] = m[c][r];
}

int main(int argc, char** argv) {
    const int frames = argc > 1 ? atoi(argv[1]) : 2;
    IDXGIFactory4* factory = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    IDXGIAdapter* warp = nullptr;
    factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
    ID3D12Device* device = nullptr;
    if (FAILED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
        std::printf("no WARP device\n");
        return 2;
    }
    RtProbeGpu::Probe probe;
    std::string error;
    if (!probe.Initialize(device, error)) {
        std::printf("Initialize failed: %s\n", error.c_str());
        return 3;
    }
    // a unit box (float3, 16-bit indices, stride 24 with a second attribute after the position)
    struct V { float p[3]; float n[3]; };
    std::vector<V> box;
    std::vector<unsigned short> boxIdx;
    const float c[8][3] = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}, {-1, -1, 2}, {1, -1, 2}, {1, 1, 2}, {-1, 1, 2}};
    for (auto& p : c) box.push_back({{p[0] * 50, p[1] * 50, p[2] * 50}, {0, 0, 1}});
    const unsigned short faces[] = {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1, 1, 5, 6, 1, 6, 2, 2, 6, 7, 2, 7, 3, 3, 7, 4, 3, 4, 0};
    boxIdx.assign(std::begin(faces), std::end(faces));
    // a ground plane (R16G16B16A16_FLOAT, 32-bit indices), a few vertices of padding before it (a base vertex)
    std::vector<unsigned short> plane;
    const float pv[6][3] = {{0, 0, 0}, {0, 0, 0}, {-1000, -1000, 0}, {1000, -1000, 0}, {1000, 1000, 0}, {-1000, 1000, 0}};
    for (auto& p : pv) {
        plane.push_back(Half(p[0]));
        plane.push_back(Half(p[1]));
        plane.push_back(Half(p[2]));
        plane.push_back(Half(1));
    }
    const unsigned int planeIdx[] = {0, 1, 2, 0, 2, 3};
    ID3D12Resource* vb1 = Upload(device, box.data(), box.size() * sizeof(V));
    ID3D12Resource* ib1 = Upload(device, boxIdx.data(), boxIdx.size() * 2);
    ID3D12Resource* vb2 = Upload(device, plane.data(), plane.size() * 2);
    ID3D12Resource* ib2 = Upload(device, planeIdx, sizeof(planeIdx));
    RtProbeGpu::Geometry gBox{vb1->GetGPUVirtualAddress(), sizeof(V), DXGI_FORMAT_R32G32B32_FLOAT, 8,
                              ib1->GetGPUVirtualAddress(), DXGI_FORMAT_R16_UINT, 36};
    RtProbeGpu::Geometry gPlane{vb2->GetGPUVirtualAddress() + 2 * 8, 8, DXGI_FORMAT_R16G16B16A16_FLOAT, 4,
                                ib2->GetGPUVirtualAddress(), DXGI_FORMAT_R32_UINT, 6};

    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    ID3D12CommandAllocator* alloc = nullptr;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    ID3D12GraphicsCommandList* list = nullptr;
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
    list->Close();
    ID3D12Fence* fence = nullptr;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    ID3D12DescriptorHeap* heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
    const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE uavCpu = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE uavGpu = heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{uavCpu.ptr + inc};

    const UINT W = 320, H = 180;
    RtProbeGpu::Camera cam;
    // the camera at (0, -600, 250) looking along +y (z up), a little down
    const float pitch = -0.25f;
    cam.origin[0] = 0; cam.origin[1] = -600; cam.origin[2] = 250;
    cam.forward[0] = 0; cam.forward[1] = std::cos(pitch); cam.forward[2] = std::sin(pitch);
    cam.right[0] = 1; cam.right[1] = 0; cam.right[2] = 0;
    cam.up[0] = 0; cam.up[1] = -std::sin(pitch); cam.up[2] = std::cos(pitch);
    cam.tanHalfFov = std::tan(0.5f * 0.96f);
    cam.maxDistance = 5000;
    cam.minDistance = 1;

    // (v0.9.544) "evict": ten large grids (each its own mesh key) against a 32 MB budget: chunks let go and built again
    const bool evict = argc > 2 && std::string(argv[2]) == "evict";
    std::vector<float> grid;
    std::vector<unsigned> gridIdx;
    const int N = 200;
    for (int y = 0; y <= N; ++y)
        for (int x = 0; x <= N; ++x) {
            grid.push_back(x * 2.0f - N);
            grid.push_back(y * 2.0f - N);
            grid.push_back(std::sin(x * 0.3f) * std::cos(y * 0.2f) * 6.0f);
        }
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            const unsigned a = y * (N + 1) + x, b = a + 1, c2 = a + N + 1, d = c2 + 1;
            gridIdx.insert(gridIdx.end(), {a, b, d, a, d, c2});
        }
    ID3D12Resource* vb3 = evict ? Upload(device, grid.data(), grid.size() * 4) : nullptr;
    ID3D12Resource* ib3 = evict ? Upload(device, gridIdx.data(), gridIdx.size() * 4) : nullptr;
    RtProbeGpu::Geometry gGrid{};
    if (evict) {
        gGrid = {vb3->GetGPUVirtualAddress(), 12, DXGI_FORMAT_R32G32B32_FLOAT, static_cast<UINT>((N + 1) * (N + 1)),
                 ib3->GetGPUVirtualAddress(), DXGI_FORMAT_R32_UINT, static_cast<UINT>(gridIdx.size())};
        probe.SetBuildBudget(3, 32ull << 20);
    }

    // (v0.9.545) a skinned box beside the others: positions (float3, stride 24) in one stream, weights and indices
    // (R8G8B8A8_UNORM each, stride 8) in another; two palette entries in the game's layout (three column registers and
    // one of effects, 64 bytes): bone 0 where it stands, bone 1 (the top four corners) turned 45 degrees about z and
    // raised 40
    const unsigned char skinStream[8][8] = {
        {255, 0, 0, 0, 0, 0, 0, 0}, {255, 0, 0, 0, 0, 0, 0, 0}, {255, 0, 0, 0, 0, 0, 0, 0}, {255, 0, 0, 0, 0, 0, 0, 0},
        {255, 0, 0, 0, 1, 0, 0, 0}, {255, 0, 0, 0, 1, 0, 0, 0}, {255, 0, 0, 0, 1, 0, 0, 0}, {255, 0, 0, 0, 1, 0, 0, 0}};
    ID3D12Resource* vbSkin = Upload(device, skinStream, sizeof(skinStream));
    float palette[2][16] = {};
    {
        const float b0[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {420, 150, 0, 1}};
        const float a = 0.785398f;
        const float b1[4][4] = {{std::cos(a), std::sin(a), 0, 0}, {-std::sin(a), std::cos(a), 0, 0}, {0, 0, 1, 0}, {420, 150, 40, 1}};
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 4; ++r) {
                palette[0][c * 4 + r] = b0[r][c];
                palette[1][c * 4 + r] = b1[r][c];
            }
    }
    RtProbeGpu::SkinnedGeometry gSkin;
    gSkin.positions = vb1->GetGPUVirtualAddress();
    gSkin.positionStride = sizeof(V);
    gSkin.weights = gSkin.blendIndices = vbSkin->GetGPUVirtualAddress();
    gSkin.weightStride = gSkin.indexStride = 8;
    gSkin.indexOffset = 4;
    gSkin.weightFormat = gSkin.indexFormat = 0;
    gSkin.vertexCount = 8;
    gSkin.indices = ib1->GetGPUVirtualAddress();
    gSkin.indexBufferFormat = DXGI_FORMAT_R16_UINT;
    gSkin.indexCount = 36;
    gSkin.use = argc > 3 && std::string(argv[3]) == "onebone" ? 0x10 : 0xFF;   // (v0.9.547)
    // (v0.9.559) "gpupalette": the palette given by its GPU address (an upload buffer, as the game's own buffer)
    const bool gpuPalette = argc > 5 && std::string(argv[5]) == "gpupalette";
    ID3D12Resource* paletteBuffer = Upload(device, palette, sizeof(palette));

    // (v0.9.552) a cutout: a standing card (float3 positions and R16G16_SNORM UVs, stride 16) before the boxes, its texture
    // 64 x 64 with alpha in stripes (8 texels opaque, 8 not), UVs x 2 (the stripes twice over); its descriptor in a heap
    // the shaders do not see (as the game's are), copied by the probe
    struct Q { float p[3]; short uv[2]; };
    const Q quad[4] = {{{-200, 250, 0}, {0, 32767}}, {{200, 250, 0}, {32767, 32767}}, {{200, 250, 400}, {32767, 0}},
                       {{-200, 250, 400}, {0, 0}}};
    const unsigned short quadIdx[6] = {0, 1, 2, 0, 2, 3};
    ID3D12Resource* vbQuad = Upload(device, quad, sizeof(quad));
    ID3D12Resource* ibQuad = Upload(device, quadIdx, sizeof(quadIdx));
    std::vector<unsigned> texels(64 * 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) texels[y * 64 + x] = 0x0080FF40u | ((x % 16) < 8 ? 0xFF000000u : 0u);
    ID3D12Resource* texture = nullptr;
    {
        D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = 64;
        td.Height = 64;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&texture));
        ID3D12Resource* staging = Upload(device, texels.data(), texels.size() * 4);
        alloc->Reset();
        list->Reset(alloc, nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{staging, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER tb{};
        tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        tb.Transition.pResource = texture;
        tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        tb.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &tb);
        list->Close();
        queue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList**>(&list));
        queue->Signal(fence, 500);
        while (fence->GetCompletedValue() < 500) Sleep(1);
    }
    ID3D12DescriptorHeap* gameHeap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC ghd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE};
    device->CreateDescriptorHeap(&ghd, IID_PPV_ARGS(&gameHeap));
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(texture, &sd, gameHeap->GetCPUDescriptorHandleForHeapStart());
    RtProbeGpu::Geometry gQuad{vbQuad->GetGPUVirtualAddress(), sizeof(Q), DXGI_FORMAT_R32G32B32_FLOAT, 4,
                               ibQuad->GetGPUVirtualAddress(), DXGI_FORMAT_R16_UINT, 6};
    RtProbeGpu::Cutout cut;
    cut.uvs = vbQuad->GetGPUVirtualAddress();
    cut.uvStride = sizeof(Q);
    UINT uvBytes = 0;
    cut.uvKind = static_cast<UINT>(RtProbeGpu::CutoutUvKind(DXGI_FORMAT_R16G16_SNORM, uvBytes));
    cut.uvAt[0] = 12;
    cut.uvAt[1] = 12 + uvBytes;
    cut.texture = gameHeap->GetCPUDescriptorHandleForHeapStart().ptr;
    cut.channel = 3;
    cut.scale[0] = cut.scale[1] = 2.0f;
    cut.threshold = 0.5f;
    const bool noCut = argc > 4 && std::string(argv[4]) == "opaque";

    // (v0.9.560) "shadows": the lamp shadow mask with one lamp above and before the boxes, the camera given as the game's
    // SceneBuffer gives it (View's rows: view = world x View; x right, y up, z forward; uv from the pixel, the projection
    // from uv)
    RtProbeGpu::ShadowSetup shadows;
    if (argc > 6 && std::string(argv[6]) == "shadows") {
        const float* axes[3] = {cam.right, cam.up, cam.forward};
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) shadows.view[j * 4 + k] = axes[k][j];
        for (int k = 0; k < 3; ++k)
            shadows.view[12 + k] = -(cam.origin[0] * axes[k][0] + cam.origin[1] * axes[k][1] + cam.origin[2] * axes[k][2]);
        shadows.view[15] = 1.0f;
        const float aspect = static_cast<float>(W) / static_cast<float>(H);
        shadows.projection[0] = 2.0f * cam.tanHalfFov * aspect;
        shadows.projection[1] = -2.0f * cam.tanHalfFov;
        shadows.projection[2] = -cam.tanHalfFov * aspect;
        shadows.projection[3] = cam.tanHalfFov;
        shadows.extents[2] = 1.0f / W;
        shadows.extents[3] = 1.0f / H;
        shadows.renderWidth = W;
        shadows.renderHeight = H;
        // (v0.9.565) argv[8]: the lamp's reach (default 3,000: all of the scene)
        const float lamp[4] = {-150.0f, -150.0f, 350.0f, argc > 8 ? static_cast<float>(atof(argv[8])) : 3000.0f};
        std::memcpy(shadows.lamp[0], lamp, sizeof(lamp));
        shadows.lampCount = 1;
        shadows.enabled = shadows.show = true;
        // (v0.9.564) argv[7]: the softness (0 sharp; default 1)
        shadows.softness = argc > 7 ? static_cast<float>(atof(argv[7])) : 1.0f;
        shadows.rays = shadows.softness > 0.0f ? 4u : 1u;
        shadows.blurRadius = shadows.softness > 0.0f ? 2 : 0;
        UINT64 frequency = 0;
        if (SUCCEEDED(queue->GetTimestampFrequency(&frequency))) probe.SetTimestampFrequency(frequency);
    }
    // (v0.9.570) argv[9] "sun": the sun's pass too (the shadows' camera; the sun up and to one side), its mask written to
    // rtp_sun.ppm (grey: the visibility; blue: nothing hit)
    const bool sunTest = shadows.enabled && argc > 9 && std::string(argv[9]) == "sun";
    RtProbeGpu::ShadowSetup sunSetup = shadows;
    if (sunTest) {
        const float d[3] = {0.45f, -0.30f, 0.84f};
        const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int j = 0; j < 3; ++j) sunSetup.sun[j] = d[j] / l;
        sunSetup.sun[3] = std::tan(0.5f * 0.0174532925f * shadows.softness);
        sunSetup.sunDistance = 25000.0f;
        sunSetup.lampCount = 0;
    }
    ID3D12Fence* shadowFence = nullptr;   // (v0.9.564) the shadow pass's own
    UINT64 shadowValue = 0;
    if (shadows.enabled) device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&shadowFence));

    for (int f = 0; f < frames; ++f) {
        probe.BeginFrame();
        probe.SetShadows(shadows);
        {
            const float I[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
            float tq[3][4];
            ToDxr(I, tq);
            // (v0.9.571) argv[10] "uncertain": the striped card as an alpha test not resolved (solid for the lamps; the
            // sun's rays through it give their texel up)
            if (argc > 10 && std::string(argv[10]) == "uncertain") probe.AddInstance(4, gQuad, tq, 500, nullptr, true);
            else probe.AddInstance(noCut ? 4 : 3, gQuad, tq, 500, noCut ? nullptr : &cut);
        }
        if (gpuPalette) probe.AddSkinned(gSkin, nullptr, 2, 400, nullptr, paletteBuffer->GetGPUVirtualAddress());
        else probe.AddSkinned(gSkin, palette, 2, 400);
        if (evict)
            for (int k = 0; k < 10; ++k) {
                const float G[4][4] = {{0.5f, 0, 0, 0}, {0, 0.5f, 0, 0}, {0, 0, 1, 0}, {-450.0f + 100.0f * k, 600.0f, 20.0f * k, 1}};
                float tg[3][4];
                ToDxr(G, tg);
                probe.AddInstance(1000 + k, gGrid, tg, 300 + k);
            }
        const float I[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
        float t[3][4];
        ToDxr(I, t);
        probe.AddInstance(2, gPlane, t, 7);
        for (int k = 0; k < 3; ++k) {
            const float a = 0.5f * k;
            const float M[4][4] = {{std::cos(a), std::sin(a), 0, 0}, {-std::sin(a), std::cos(a), 0, 0}, {0, 0, 1, 0},
                                   {-250.0f + 250.0f * k, 100.0f * k, 0, 1}};
            ToDxr(M, t);
            probe.AddInstance(1, gBox, t, 100 + k);
        }
        // a tall thin one: scaled by the matrix
        const float S[4][4] = {{0.4f, 0, 0, 0}, {0, 0.4f, 0, 0}, {0, 0, 3, 0}, {0, 300, 0, 1}};
        ToDxr(S, t);
        probe.AddInstance(1, gBox, t, 200);
        alloc->Reset();
        list->Reset(alloc, nullptr);
        list->SetDescriptorHeaps(1, &heap);
        const bool ok = probe.Record(list, f, W, H, cam, uavCpu, uavGpu, srvCpu);
        list->Close();
        queue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList**>(&list));
        queue->Signal(fence, f + 1);
        while (fence->GetCompletedValue() < static_cast<UINT64>(f + 1)) Sleep(1);
        if (shadows.enabled && ok) {   // (v0.9.564; since 0.9.561 the mask is the shadow pass's, the view only reads it)
            alloc->Reset();
            list->Reset(alloc, nullptr);
            const bool made = probe.RecordShadowPass(list, static_cast<UINT>(f % 3));
            list->Close();
            if (made && shadowFence) {
                queue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList**>(&list));
                queue->Signal(shadowFence, ++shadowValue);
                while (shadowFence->GetCompletedValue() < shadowValue) Sleep(1);
            }
            std::printf("         shadow pass %s (%.3f ms on the GPU)\n", made ? "made" : "NOT made", probe.LastStats().shadowMs);
        }
        if (sunTest && ok && shadowFence) {   // (v0.9.570)
            probe.SetSunShadows(sunSetup);
            alloc->Reset();
            list->Reset(alloc, nullptr);
            const bool made = probe.RecordSunPass(list, static_cast<UINT>(f % 3));
            list->Close();
            if (made) {
                queue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList**>(&list));
                queue->Signal(shadowFence, ++shadowValue);
                while (shadowFence->GetCompletedValue() < shadowValue) Sleep(1);
            }
            std::printf("         sun pass %s (%.3f ms on the GPU)\n", made ? "made" : "NOT made", probe.LastStats().sunMs);
        }
        const auto& s = probe.LastStats();
        std::printf("frame %d: record %s, instances %u (waiting %u), meshes %u, built %u (total %llu), evicted %llu, failed %u, "
                    "memory %llu; device %s\n", f, ok ? "ok" : "FAILED", s.instances, s.instancesWaiting, s.meshes, s.built,
                    static_cast<unsigned long long>(s.builtTotal), static_cast<unsigned long long>(s.evictedTotal), s.failed,
                    static_cast<unsigned long long>(s.memory), device->GetDeviceRemovedReason() == S_OK ? "fine" : "REMOVED");
        std::printf("         skinned %u (%u vertices, %u dropped, %llu bytes)\n", s.skinned, s.skinnedVertices,
                    s.skinnedDropped, static_cast<unsigned long long>(s.skinnedMemory));
        std::printf("         mask %u x %u, %u lamps\n", s.maskWidth, s.maskHeight, s.maskLamps);
        std::printf("         cutouts %u (%u records, %u textures, %u refused), UVs gathered %u (%llu bytes)\n", s.cutouts,
                    s.cutoutRecords, s.cutoutTextures, s.cutoutRefused, s.uvGathered,
                    static_cast<unsigned long long>(s.uvMemory));
    }
    // read back
    ID3D12Resource* out = probe.Output();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    D3D12_RESOURCE_DESC od = out->GetDesc();
    device->GetCopyableFootprints(&od, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES rb{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* readback = nullptr;
    device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    alloc->Reset();
    list->Reset(alloc, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = out;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{out, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    list->Close();
    queue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList**>(&list));
    queue->Signal(fence, 1000);
    while (fence->GetCompletedValue() < 1000) Sleep(1);
    unsigned char* px = nullptr;
    readback->Map(0, nullptr, reinterpret_cast<void**>(&px));
    FILE* file = std::fopen("rtp_out.ppm", "wb");
    std::fprintf(file, "P6 %u %u 255\n", W, H);
    for (UINT y = 0; y < H; ++y)
        for (UINT x = 0; x < W; ++x) std::fwrite(px + y * fp.Footprint.RowPitch + x * 4, 1, 3, file);
    std::fclose(file);
    std::printf("wrote rtp_out.ppm\n");
    if (sunTest && probe.SunMaskForTest()) {   // (v0.9.570) the sun's mask: RGBA16F, .x the visibility, .w the depth
        ID3D12Resource* sun = probe.SunMaskForTest();
        D3D12_RESOURCE_DESC sd = sun->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT sfp{};
        UINT64 sunTotal = 0;
        device->GetCopyableFootprints(&sd, 0, 1, 0, &sfp, nullptr, nullptr, &sunTotal);
        D3D12_RESOURCE_DESC sbd = bd;
        sbd.Width = sunTotal;
        ID3D12Resource* sunBack = nullptr;
        device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &sbd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&sunBack));
        alloc->Reset();
        list->Reset(alloc, nullptr);
        D3D12_RESOURCE_BARRIER sb{};
        sb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        sb.Transition.pResource = sun;
        sb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        sb.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        sb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &sb);
        D3D12_TEXTURE_COPY_LOCATION sdst{sunBack, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        sdst.PlacedFootprint = sfp;
        D3D12_TEXTURE_COPY_LOCATION ssrc{sun, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        list->CopyTextureRegion(&sdst, 0, 0, 0, &ssrc, nullptr);
        std::swap(sb.Transition.StateBefore, sb.Transition.StateAfter);
        list->ResourceBarrier(1, &sb);
        list->Close();
        queue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList**>(&list));
        queue->Signal(fence, 2000);
        while (fence->GetCompletedValue() < 2000) Sleep(1);
        auto half = [](std::uint16_t h) {
            const std::uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 1023;
            float v = exp == 0 ? std::ldexp(static_cast<float>(man), -24)
                    : exp == 31 ? 65504.0f : std::ldexp(static_cast<float>(man | 1024), static_cast<int>(exp) - 25);
            return sign ? -v : v;
        };
        unsigned char* sp = nullptr;
        sunBack->Map(0, nullptr, reinterpret_cast<void**>(&sp));
        FILE* sf = std::fopen("rtp_sun.ppm", "wb");
        std::fprintf(sf, "P6 %u %u 255\n", static_cast<unsigned>(sd.Width), sd.Height);
        unsigned lit = 0, shadowed = 0, partial = 0, none = 0;
        for (UINT y = 0; y < sd.Height; ++y)
            for (UINT x = 0; x < static_cast<UINT>(sd.Width); ++x) {
                const auto* t = reinterpret_cast<const std::uint16_t*>(sp + y * sfp.Footprint.RowPitch + x * 8);
                const float v = half(t[0]), depth = half(t[3]);
                unsigned char c[3];
                if (depth <= 0.0f) {
                    c[0] = 40, c[1] = 60, c[2] = 160;
                    ++none;
                } else {
                    c[0] = c[1] = c[2] = static_cast<unsigned char>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
                    (v > 0.99f ? lit : v < 0.01f ? shadowed : partial)++;
                }
                std::fwrite(c, 1, 3, sf);
            }
        std::fclose(sf);
        sunBack->Unmap(0, nullptr);
        std::printf("wrote rtp_sun.ppm: %u lit, %u shadowed, %u partly, %u nothing hit\n", lit, shadowed, partial, none);
    }
    probe.Shutdown();
    return 0;
}
