#pragma once

#include "rt_cutout.h"

#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12PipelineState;

namespace EightWeightPatch {



bool Install();



bool AttachDx12Device(ID3D12Device* device);
// (v0.9.517) D3D12CreateDevice hooked in d3d12.dll (MinHook initialized): as early as the start-up allows, so the
// game's device is reached as it is made, before its first pipelines. Install() tries it too.
bool InstallDeviceHook();

bool Installed();
bool Dx12Attached();
// (v0.9.606) The game's pipelines made since the start (CreateGraphicsPipelineState through the hook): their count, the
// seconds in the driver's call and in the hook in all (the framework's own = hook - driver). Any thread.
struct PipelineStats {
    std::uint64_t count = 0;
    double driverSeconds = 0.0, hookSeconds = 0.0;
};
PipelineStats GamePipelineStats();



// (v0.9.607) Draw wireframe (the Draw menu; it was General > Graphics > Wireframe): the triangles' edges of every
// geometry draw of the game's (a vertex input, render targets, a depth test, render target 0 not blended: the level,
// characters, objects; not the shadow maps, the depth pre-pass, the translucent or full-screen passes), drawn again
// over it in a flat colour. Lines only: those draws' surfaces in one flat colour too. Not saved; the options are
// ([Draw] Wireframe*). Any thread.
bool WireframeAvailable();
bool WireframeEnabled();
void SetWireframeEnabled(bool value);
struct WireframeOptions {
    int style = 0;                                       // 0 over the picture, 1 lines only
    float lineColor[4] = {0.35f, 0.85f, 1.0f, 1.0f};     // linear, before the game's exposure; [3] the opacity
    float fillColor[3] = {0.08f, 0.08f, 0.09f};          // lines only: the surfaces
    bool seeThrough = false;                             // the lines of what is hidden too
    bool backFaces = false;                              // the edges of the triangles facing away too
};
WireframeOptions GetWireframeOptions();
void SetWireframeOptions(const WireframeOptions& options);
const char* WireframeStatus();


void SetWireframeExcludedCommandList(ID3D12GraphicsCommandList* commandList);
// (v0.9.543) What a game pipeline draws, from its kept description (the ray tracing probe, rt_probe.cpp)
struct PipelineGeometry {
    bool valid = false;
    unsigned positionFormat = 0;   // DXGI_FORMAT of POSITION 0
    unsigned positionSlot = 0, positionOffset = 0, positionSize = 0;
    bool skinned = false;          // BLENDINDICES or BLENDWEIGHT in its input
    bool depthWrite = false;       // depth on, written, a depth buffer
    bool triangles = false;        // its primitive topology type
    bool patches = false;          // (v0.9.544) a tessellated one (patch topology type)
    // (v0.9.545) BLENDWEIGHT 0 and BLENDINDICES 0 (format, slot, offset); eightWeights: a second set (the 8-weight layout)
    unsigned weightFormat = 0, weightSlot = 0, weightOffset = 0;
    unsigned blendIndexFormat = 0, blendIndexSlot = 0, blendIndexOffset = 0;
    bool eightWeights = false;
    // (v0.9.547) the components of BLENDWEIGHT 0 / BLENDINDICES 0 the vertex shader reads (its input signature's read
    // masks; vertexShaderKnown false: no signature found)
    unsigned weightUse = 0, blendIndexUse = 0;
    bool vertexShaderKnown = false;
    unsigned renderTargets = 0;
    // (v0.9.552) its alpha test (rt_cutout.h: analyzed once, as the pipeline is made) and the TEXCOORD element its UV
    // comes from (format, slot, offset; texcoordFound false: none in the layout)
    RtCutout::Program cutout;
    unsigned texcoordFormat = 0, texcoordSlot = 0, texcoordOffset = 0;
    bool texcoordFound = false;
    char layoutSummary[120] = {};   // (v0.9.554) a followed test without its TEXCOORD: the layout (name index slot rate DXGI)
};
bool PipelineGeometryOf(ID3D12PipelineState* pipelineState, PipelineGeometry& out);
// (v0.9.555) Whether a game pipeline is one of the tiled lights' pass's (its pixel shader a TiledLightingPS variant)
bool IsLightPassPipeline(ID3D12PipelineState* pipelineState);
// (v0.9.570) whether a pipeline is the sun's shadow pass's (the game's own, as bound)
bool IsSunPassPipeline(ID3D12PipelineState* pipelineState);
// (v0.9.557) Counts the game pipelines gone (their records let go): what was read of a pipeline holds while it is the same
std::uint32_t PipelineGeneration();
}
