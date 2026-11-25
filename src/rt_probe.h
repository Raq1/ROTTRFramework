#pragma once
// (v0.9.543) The ray tracing probe: can the game's geometry be captured for ray tracing? A test, not an effect.
// While on, every static mesh the game draws (indexed triangle lists with depth writes, not skinned, one instance)
// is taken from its own draw call: its vertex and index buffers as bound (their GPU addresses, the pipeline's position
// format, offset and stride) and the world matrix the engine drew it with. Each mesh is built once into a
// bottom-level acceleration structure, each frame's instances into a top-level one, and a debug view traces a camera
// ray per pixel through it (rt_probe_gpu.*, rt_probe.hlsl): full screen or the right half beside the game's image.
// See rt_probe.cpp for where each piece comes from. Not saved.
#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12PipelineState;

namespace RtProbe {
bool Install();   // the engine's draw hook (MinHook; before the framework enables its hooks)

// From eight_weight_patch.cpp: each direct command list the game makes (its vtable's IA and draw slots hooked once),
// and each pipeline the game binds (before the framework swaps any of its own in)
void AttachCommandList(ID3D12GraphicsCommandList* list);
void OnSetPipelineState(ID3D12GraphicsCommandList* list, ID3D12PipelineState* gamePipeline);

// From dx12_overlay.cpp, on the overlay's command list each present, its descriptor heap set (before ImGui is drawn);
// and when the overlay lets its resources go (the GPU idle)
void RecordFrame(ID3D12Device* device, ID3D12GraphicsCommandList* list, unsigned frameIndex, unsigned width,
                 unsigned height);
void OnOverlayReleased();

void TickGameThread();   // the camera snapshot while on

int Mode();   // 0 off, 1 full screen, 2 split screen (the right half ray traced), 3 (v0.9.544) over the game's image
void SetMode(int mode);
bool Active();
// (v0.9.548) colour the view by how each object was captured (static, instanced, a skinned layout drawn rigid, four-weight
// skinned, one-bone skinned, tessellated); not saved
bool KindView();
void SetKindView(bool on);
// (v0.9.560) the lamp shadow mask (rt_probe_shadow.hlsl): made each frame for the three lamps nearest in reach and shown
// over the view (each lamp's shadow takes its channel away); not saved
bool ShadowMask();
void SetShadowMask(bool on);
// (v0.9.561) ray-traced lamp shadows: the probe runs (its view off or not), the mask is made before each frame's lights'
// pass and put in that pass's t45 / t46 (0.9.561: t50 / t51); (v0.9.562) the pass reads them (ShadowBlend::
// SetRtLampShadows, switched here) once 64 of its draws in a row were checked to have them
bool LampShadows();
void SetLampShadows(bool on);
const char* LampShadowsState();   // (v0.9.562) a line for the menu
// (v0.9.564) the lamps' size for soft shadows: 0 sharp (one ray), 1 a size from each lamp's reach, up to 3; at once
float LampSoftness();
void SetLampSoftness(float softness);
// (v0.9.570) ray-traced sun shadows: the probe's sun mask made before the game's sun shadow pass and read there (t42)
// where its depth agrees, the cascades elsewhere; the pass reads it once 64 of its draws in a row were checked to have
// it. Its cone: 0.5 degree x LampSoftness. A line for the menu.
bool SunShadows();
void SetSunShadows(bool on);
const char* SunShadowsState();
// (v0.9.563) At each present, before anything else: the capture buffers keep one frame at most
void OnPresent();
// (v0.9.563) Whether the overlay should draw a frame (RecordFrame: the probe's scene) even with nothing of its own to
// show: while the lamp shadows are on
bool NeedsFrames();
bool RecordsFrames();   // (v0.9.602) RecordFrame has work in the overlay's frame (else an empty frame need not be submitted)
// The view's ImGui texture (0: none yet) for the overlay to draw this frame
std::uint64_t ViewTexture();
const char* Status();   // a line for the menu
}  // namespace RtProbe
