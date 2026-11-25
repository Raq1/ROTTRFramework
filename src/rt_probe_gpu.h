#pragma once
// (v0.9.543) The ray tracing probe's GPU side (see rt_probe.h): bottom-level acceleration structures built once per mesh
// from the game's own vertex and index buffers (their GPU addresses), a top-level one each frame from the frame's
// instances, and the debug view (rt_probe.hlsl) traced into a texture. Self-contained (D3D12 only), so it can be run
// on its own (a test on WARP) as well as in the game.
#include <d3d12.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace RtProbeGpu {

struct Geometry {
    D3D12_GPU_VIRTUAL_ADDRESS vertices = 0;   // the first vertex's position
    UINT vertexStride = 0;
    DXGI_FORMAT vertexFormat = DXGI_FORMAT_UNKNOWN;
    UINT vertexCount = 0;
    D3D12_GPU_VIRTUAL_ADDRESS indices = 0;   // the first index
    DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
    UINT indexCount = 0;
};

// (v0.9.545) A skinned mesh: posed each frame by rt_probe_skin.hlsl from the game's own vertex streams and its palette
struct SkinnedGeometry {
    D3D12_GPU_VIRTUAL_ADDRESS positions = 0, weights = 0, blendIndices = 0;   // the streams' buffers (their views' starts)
    UINT positionStride = 0, positionOffset = 0;                               // (float3)
    UINT weightStride = 0, weightOffset = 0, weightFormat = 0;                 // (formats: see SkinFormatCode)
    UINT indexStride = 0, indexOffset = 0, indexFormat = 0;
    UINT use = 0xFF;   // (v0.9.547) the weight components read (bits 0..3; none: one bone) and the index ones (4..7)
    UINT firstVertex = 0, vertexCount = 0;                                     // the vertices posed (base vertex on)
    D3D12_GPU_VIRTUAL_ADDRESS indices = 0;                                     // the first index (relative to firstVertex)
    DXGI_FORMAT indexBufferFormat = DXGI_FORMAT_UNKNOWN;
    UINT indexCount = 0;
};
// The skinning shader's code for a blend weight / index format (-1: not read)
int SkinFormatCode(DXGI_FORMAT format);

// (v0.9.552) A leaf cutout: a mesh's alpha test (rt_cutout.h) as one draw resolved it. Its UVs are gathered once a mesh
// (rt_probe_uv.hlsl); the rest is the draw's own, given each frame.
struct Cutout {
    D3D12_GPU_VIRTUAL_ADDRESS uvs = 0;   // the UV stream at the mesh's first vertex (its view's start + base vertex x stride)
    UINT uvStride = 0;
    UINT uvKind = 0;                     // CutoutUvKind
    UINT uvAt[2] = {0, 4};               // u's and v's bytes into a vertex
    std::uint64_t texture = 0;           // the texture's CPU descriptor (the game's; copied into the probe's heap)
    UINT channel = 3;
    float scale[2] = {1.0f, 1.0f}, offset[2] = {0.0f, 0.0f};   // uv x scale + offset
    float threshold = 0.5f;              // the texel's component below it: no hit
};
// The UV gather's code for a TEXCOORD format (-1: not read) and its components' bytes
int CutoutUvKind(DXGI_FORMAT format, UINT& componentBytes);

// (v0.9.560) The lamp shadow mask's frame: the game's own pixel mapping (SceneBuffer) and up to three lamps
struct ShadowSetup {
    bool enabled = false;
    float view[16] = {};        // SceneBuffer's View (rows: view = world x View)
    float projection[4] = {};   // SceneBuffer[18]
    float extents[4] = {};      // SceneBuffer[45] (ScreenExtents: uv = pixel x zw + xy)
    UINT renderWidth = 0, renderHeight = 0;   // the game's render resolution (SceneBuffer[46].xy)
    float lamp[3][4] = {};      // world position, reach
    float lampView[3][4] = {};  // the lights' pass's own view-space position (cb12 +0)
    UINT lampCount = 0;
    bool show = false;          // the view tinted by it
    float softness = 1.0f;      // (v0.9.564) the lamps' size (0 sharp: one ray; 1 from the reach; up to 3)
    UINT rays = 4;              // (v0.9.564) rays a lamp when soft
    int blurRadius = 2;         // (v0.9.564) the mask's blur, texels each way (0: none)
    // (v0.9.570) the sun's pass (SetSunShadows): toward the sun (world, unit) and the tangent of its disc's angular
    // radius; how far from the camera its surfaces are traced (cm)
    float sun[4] = {};
    float sunDistance = 25000.0f;
};

struct Camera {
    float origin[3]{}, right[3]{}, up[3]{}, forward[3]{};
    float tanHalfFov = 0.5f;   // vertical
    float maxDistance = 200000.0f, minDistance = 1.0f;
    float kindView = 0.0f;   // (v0.9.548) 1: coloured by kind (the instance ID's bits 21..23)
};

struct Stats {
    std::uint32_t instances = 0;        // in the last frame's top level
    std::uint32_t instancesWaiting = 0; // their mesh not built yet
    std::uint32_t meshes = 0;           // built
    std::uint32_t built = 0;            // built in the last frame
    std::uint32_t failed = 0;           // meshes that could not be built (too large for the scratch buffer)
    std::uint64_t memory = 0;           // bytes of bottom-level structures
    std::uint64_t builtTotal = 0;       // (v0.9.544) built since the start
    std::uint64_t evictedTotal = 0;     // (v0.9.544) meshes let go to make room (rebuilt when drawn again)
    std::uint32_t skinned = 0;          // (v0.9.545) skinned meshes posed and built in the last frame
    std::uint32_t skinnedVertices = 0;
    std::uint32_t skinnedDropped = 0;   // over the frame's limits
    std::uint64_t skinnedMemory = 0;    // their structures' bytes
    std::uint32_t cutouts = 0;          // (v0.9.552) instances traced with their alpha test in the last frame
    std::uint32_t cutoutRecords = 0;    // their distinct records
    std::uint32_t cutoutTextures = 0;   // their textures
    std::uint32_t cutoutRefused = 0;    // alpha-tested instances traced opaque (no room: textures, UVs)
    std::uint32_t uvGathered = 0;       // meshes' UVs gathered in the last frame
    std::uint64_t uvMemory = 0;         // the UV arena's bytes in use
    // (v0.9.555) the GPU's time (ms) in the last measured frame: static builds, skinned (posing and builds), UV gathers,
    // the top level, (v0.9.560) the lamp shadow mask, the view, all of it
    float gpuMs[7] = {};
    std::uint32_t maskWidth = 0, maskHeight = 0, maskLamps = 0;   // (v0.9.560) the last mask made
    std::uint32_t uncertain = 0;   // (v0.9.570) the last top level's uncertain instances (mask 2)
    float shadowMs = 0.0f;   // (v0.9.564) the shadow pass's GPU time (its trace and blur), from its own timestamps
    float sunMs = 0.0f;      // (v0.9.570) the sun's pass's
    std::uint32_t sunWidth = 0, sunHeight = 0;
};

// Whether a position format can be read by the acceleration structure build (DXR tier 1.1)
bool SupportedVertexFormat(DXGI_FORMAT format);

class Probe {
public:
    // The device must support DXR tier 1.1 (inline ray queries). error: why not.
    bool Initialize(ID3D12Device* device, std::string& error);
    void Shutdown();   // the GPU must be done with the probe's work
    bool Ready() const { return device_ != nullptr; }

    // The frame's instances (any order; the same mesh key with the same geometry each time it is given)
    void BeginFrame();
    // (v0.9.552) cutout: the draw's alpha test (null: opaque); the mesh key must tell a cutout mesh from an opaque one
    // (v0.9.570) uncertain: an alpha test not resolved (traced as solid; instance mask 2, else 1: the sun's rays take it
    // only to give up on a texel)
    void AddInstance(std::uint64_t meshKey, const Geometry& geometry, const float transform[3][4], std::uint32_t id,
                     const Cutout* cutout = nullptr, bool uncertain = false);
    // (v0.9.545) a skinned mesh for this frame (posed, built and traced this frame only); palette: entries x 64 bytes.
    // (v0.9.551) false: over the frame's limits (meshes, vertices), not taken; the caller gives the most wanted first
    // (v0.9.559) paletteGpu: the palette's GPU address (the game's own buffer, read by the posing as it is: palette may
    // be null then)
    bool AddSkinned(const SkinnedGeometry& geometry, const void* palette, UINT paletteEntries, std::uint32_t id,
                    const Cutout* cutout = nullptr, D3D12_GPU_VIRTUAL_ADDRESS paletteGpu = 0, bool uncertain = false);

    // Records the frame's builds and the debug view into the command list (a direct list, its descriptor heap set to
    // the one holding uavGpu's descriptor). The output texture is (re)made at width x height; its UAV is written at
    // uavCpu, its SRV at srvCpu. Leaves the output in PIXEL_SHADER_RESOURCE. frameSlot: which of the in-flight frames
    // this is (its upload buffer is reused when the slot comes round again, the caller having waited for it).
    // (v0.9.561) view false: the scene built, the debug view not drawn (the lamp shadows alone)
    bool Record(ID3D12GraphicsCommandList* list, UINT frameSlot, UINT width, UINT height, const Camera& camera,
                D3D12_CPU_DESCRIPTOR_HANDLE uavCpu, D3D12_GPU_DESCRIPTOR_HANDLE uavGpu, D3D12_CPU_DESCRIPTOR_HANDLE srvCpu,
                bool view = true);
    // (v0.9.561) The lamp shadow mask (SetShadows' frame) from the top level and cutouts of the last Record, into list (a
    // direct list submitted on the game's queue before its lights' pass; heaps set here). Its own descriptor block and
    // copies of the records (shadowSlot: kept until the slot comes round again: the caller waits). The mask and its table
    // are left readable (pixel and non-pixel shader resource). False: not made.
    bool RecordShadowPass(ID3D12GraphicsCommandList* list, UINT shadowSlot);
    // (v0.9.570) The sun's mask (SetSunShadows' frame) the same way, into list before the game's sun shadow pass: its
    // own blocks, records' copies, constants, masks and timestamps. False: not made.
    void SetSunShadows(const ShadowSetup& setup) { sunSetup_ = setup; }
    bool RecordSunPass(ID3D12GraphicsCommandList* list, UINT slot);
    // its SRV at this CPU handle (for the sun's pass, t42); false: no mask yet. SunGeneration: raised when it is made
    bool WriteSunPassDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) const;
    std::uint32_t SunGeneration() const { return sunGeneration_; }
    // (v0.9.562) the mask's and the table's SRVs at these CPU handles (a heap the shaders do not see, for the engine to
    // copy into the lights' pass's table); false: no mask yet. MaskGeneration: raised each time the mask or table is made
    bool WriteLightPassDescriptors(D3D12_CPU_DESCRIPTOR_HANDLE mask, D3D12_CPU_DESCRIPTOR_HANDLE table) const;
    std::uint32_t MaskGeneration() const { return maskGeneration_; }

    // Forgets every built mesh (their memory released once the GPU is past them: after kRetireFrames Record or Idle
    // calls)
    void ResetMeshes();
    void Idle() { AgeRetired(); }   // a frame without a Record (the probe off)

    ID3D12Resource* Output() const { return output_; }
    ID3D12Resource* SunMaskForTest() const { return sunMask_; }   // (v0.9.570: tools/rt_probe/rtp_test.cpp)
    const Stats& LastStats() const { return stats_; }
    void SetBuildBudget(std::uint32_t perFrame, std::uint64_t maxBytes) { buildBudget_ = perFrame; maxMemory_ = maxBytes; }
    // (v0.9.560) the lamp shadow mask's next frame (made in Record after the top level, when enabled with lamps)
    void SetShadows(const ShadowSetup& setup) { shadow_ = setup; }
    ID3D12Resource* Mask() const { return mask_; }
    // (v0.9.555) the queue's timestamp frequency (0: the GPU's time not measured)
    void SetTimestampFrequency(std::uint64_t ticksPerSecond) { timestampFrequency_ = ticksPerSecond; }

private:
    struct Mesh {
        Geometry geometry;
        D3D12_GPU_VIRTUAL_ADDRESS blas = 0;
        bool failed = false;
        bool cutout = false;   // (v0.9.552) built not opaque
    };
    struct Chunk {
        ID3D12Resource* buffer = nullptr;
        UINT64 size = 0, used = 0;
        std::vector<std::uint64_t> keys;   // (v0.9.544) the meshes built into it
        std::uint64_t lastBuild = 0;       // the frame it was last built into
    };
    struct Pending {
        std::uint64_t key;
        float transform[3][4];
        std::uint32_t id;
        int record = -1;   // (v0.9.552) its cutout record (-1: none)
        bool uncertain = false;   // (v0.9.570) instance mask 2
    };
    struct Retired {
        ID3D12Resource* resource;
        UINT age;
    };
    static constexpr UINT kSlots = 4, kRetireFrames = 8;

    ID3D12Resource* CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, bool uav);
    D3D12_GPU_VIRTUAL_ADDRESS AllocateBlas(UINT64 size, std::uint64_t key);
    bool EvictOldest();   // (v0.9.544) the oldest chunk not built into this frame let go
    void Retire(ID3D12Resource*& resource);
    void AgeRetired();
    bool EnsureOutput(UINT width, UINT height, D3D12_CPU_DESCRIPTOR_HANDLE uavCpu, D3D12_CPU_DESCRIPTOR_HANDLE srvCpu);

    struct Skinned {
        SkinnedGeometry geometry;
        UINT paletteOffset, paletteEntries;   // in paletteBytes_
        std::uint32_t id;
        int record = -1;   // (v0.9.552) its cutout record (-1: none)
        D3D12_GPU_VIRTUAL_ADDRESS paletteGpu = 0;   // (v0.9.559) the game's buffer (0: paletteOffset in paletteBytes_)
        bool uncertain = false;   // (v0.9.570) instance mask 2
    };
    // (v0.9.545) the frame's skinned meshes posed and built; their instances appended to instances
    void RecordSkinned(ID3D12GraphicsCommandList4* list, UINT frameSlot, UINT64& scratchUsed,
                       std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& instances);

    ID3D12Device5* device_ = nullptr;
    ID3D12RootSignature* rootSignature_ = nullptr;
    ID3D12PipelineState* pipeline_ = nullptr;
    ID3D12RootSignature* skinRoot_ = nullptr;
    ID3D12PipelineState* skinPipeline_ = nullptr;
    std::vector<Skinned> skinned_;
    UINT64 skinnedQueued_ = 0;   // (v0.9.551) their vertices
    std::vector<std::uint8_t> paletteBytes_;
    ID3D12Resource* paletteUpload_[4]{};
    UINT64 paletteCapacity_[4]{};
    ID3D12Resource* skinOutput_ = nullptr;   // the posed vertices (float3), in UNORDERED_ACCESS between frames
    UINT64 skinOutputSize_ = 0;
    ID3D12Resource* skinArena_ = nullptr;    // the skinned meshes' structures, rebuilt each frame
    UINT64 skinArenaSize_ = 0;
    std::unordered_map<std::uint64_t, Mesh> meshes_;
    std::vector<Pending> pending_;
    std::vector<Chunk> chunks_;
    std::vector<Retired> retired_;
    ID3D12Resource* scratch_ = nullptr;
    UINT64 scratchSize_ = 0;
    ID3D12Resource* tlas_ = nullptr;
    UINT64 tlasSize_ = 0;
    ID3D12Resource* tlasScratch_ = nullptr;
    UINT64 tlasScratchSize_ = 0;
    ID3D12Resource* instanceUpload_[kSlots]{};
    UINT64 instanceCapacity_[kSlots]{};
    ID3D12Resource* output_ = nullptr;
    UINT outputWidth_ = 0, outputHeight_ = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE outputUav_{}, outputSrv_{};
    bool outputInShaderResource_ = false;
    std::uint64_t memory_ = 0;
    std::uint64_t frame_ = 0;
    std::uint32_t buildBudget_ = 128;
    std::uint64_t maxMemory_ = 1536ull << 20;
    Stats stats_;

    // (v0.9.552) the cutouts: each alpha-tested draw a record (deduplicated), its texture in this frame's table, its mesh's
    // UVs in the arena (gathered once: uvs_ by the mesh's index range and UV stream)
    struct CutoutRecord {
        float scale[2], offset[2];
        UINT texture, channel;
        float threshold;
        UINT uvAt;
    };
    struct UvJob {
        Cutout cutout;
        D3D12_GPU_VIRTUAL_ADDRESS indices;
        DXGI_FORMAT indexFormat;
        UINT triangles, at;
        std::uint64_t key;
    };
    int RecordFor(const Cutout& cutout, D3D12_GPU_VIRTUAL_ADDRESS indices, DXGI_FORMAT indexFormat, UINT indexCount);
    void RecordUvJobs(ID3D12GraphicsCommandList4* list);
    ID3D12RootSignature* uvRoot_ = nullptr;
    ID3D12PipelineState* uvPipeline_ = nullptr;
    ID3D12Resource* uvArena_ = nullptr;   // in UNORDERED_ACCESS between frames
    UINT64 uvUsed_ = 0;                   // float2s
    std::unordered_map<std::uint64_t, UINT> uvs_;
    std::vector<UvJob> uvJobs_;
    std::vector<CutoutRecord> records_;
    std::unordered_map<std::uint64_t, UINT> recordIndex_;
    std::vector<std::uint64_t> textures_;
    std::unordered_map<std::uint64_t, UINT> textureIndex_;
    ID3D12Resource* recordUpload_[kSlots]{};
    UINT64 recordCapacity_[kSlots]{};
    ID3D12DescriptorHeap* heap_ = nullptr;   // a block a frame slot: the view's UAV, then the frame's textures
    UINT heapIncrement_ = 0;
    std::uint32_t cutoutRefused_ = 0;

    // (v0.9.555) timestamps: kStamps a frame slot (start, after the static builds, the skinned, the UVs, the top level,
    // the view), resolved into the readback buffer and read when the slot comes round again
    static constexpr UINT kStamps = 7;   // (v0.9.560: the mask's)
    ID3D12QueryHeap* timestamps_ = nullptr;
    ID3D12Resource* timestampReadback_ = nullptr;
    std::uint64_t timestampFrequency_ = 0;
    bool timestampsWritten_[kSlots]{};

    // (v0.9.560) the lamp shadow mask (rt_probe_shadow.hlsl): half the render resolution, RGBA16F (three lamps'
    // visibility, the view depth), in UNORDERED_ACCESS between frames; the table of its lamps (three float4; (v0.9.562) a
    // 4 x 1 RGBA32F texture)
    bool EnsureMask(UINT width, UINT height);
    ShadowSetup shadow_;
    ID3D12RootSignature* shadowRoot_ = nullptr;
    ID3D12PipelineState* shadowPipeline_ = nullptr;
    ID3D12Resource* mask_ = nullptr;
    UINT maskWidth_ = 0, maskHeight_ = 0;
    // (v0.9.564) the trace's own output (mask_ is now its blur: what the lights' pass and the view read), in
    // NON_PIXEL_SHADER_RESOURCE between passes (rawReadable_); the blur's root signature and pipeline; the shadow pass's
    // timestamps (two a slot) and their readback
    ID3D12Resource* rawMask_ = nullptr;
    bool rawReadable_ = false;
    ID3D12RootSignature* blurRoot_ = nullptr;
    ID3D12PipelineState* blurPipeline_ = nullptr;
    ID3D12QueryHeap* shadowQueries_ = nullptr;   // (v0.9.570: 4 a slot, the sun's pass's after the lamps')
    ID3D12Resource* shadowReadback_ = nullptr;
    bool shadowStamped_[kSlots]{};
    // (v0.9.570) the sun's pass: its setup, raw and blurred masks (RGBA16F, .x the visibility, .w the depth), their size
    // and states, its constants and records' copies (a slot each), its timestamps' slots, the mask's generation
    bool EnsureSunMask(UINT width, UINT height);
    ShadowSetup sunSetup_;
    ID3D12Resource* rawSun_ = nullptr;
    ID3D12Resource* sunMask_ = nullptr;
    UINT sunWidth_ = 0, sunHeight_ = 0;
    bool rawSunReadable_ = false, sunMaskReadable_ = false;
    ID3D12Resource* sunConstants_[kSlots]{};
    ID3D12Resource* sunRecords_[kSlots]{};
    UINT64 sunRecordCapacity_[kSlots]{};
    bool sunStamped_[kSlots]{};
    std::uint32_t sunGeneration_ = 0;
    ID3D12Resource* table_ = nullptr;
    ID3D12Resource* shadowConstants_[kSlots]{};
    // (v0.9.561) the shadow pass's own: the records' copies, the states, the last Record's slot (its cutouts' textures
    // and records the pass takes); (v0.9.562) the mask's generation (the lights' pass's descriptors: rt_probe.cpp's)
    std::uint32_t maskGeneration_ = 0;
    ID3D12Resource* shadowRecords_[kSlots]{};
    UINT64 shadowRecordCapacity_[kSlots]{};
    bool maskReadable_ = false, tableReadable_ = false;
    UINT lastSlot_ = kSlots;
};

}  // namespace RtProbeGpu
