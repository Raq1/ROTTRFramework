#pragma once
#include <cstdint>

// Blendshape (facial morph) output buffer fix.
//
// Retail facts (Steam ROTTR.exe 0x632D2D99; names from the TR2_x64_final PDB):
//   CommonRenderDevice::LoadSharedResources (RVA 0x2B7FD0) creates the per-frame
//   TransientGPUBuffer that the BlendShapesCS compute pass writes its per-vertex
//   output into (24 bytes per vertex, cdc::g_iStrideVertBS). It is a fixed 10 MB
//   (0xA00000) bump allocator, reset every frame (RVA 0x2977BE).
//
//   CommonRenderModelInstance::DrawEx (call at RVA 0x278839) calls
//   TransientGPUBuffer::Alloc (RVA 0x296170) once per mesh of every blendshape
//   model, for vertexCount * 24 bytes, and IGNORES the bool result. Once the
//   models on screen need more than 10 MB in a frame (roughly 437k vertices in
//   total), Alloc fails, the mesh's offset slot keeps whatever stale value the
//   transient heap had, and the compute/skinning shaders read and write the
//   wrong region: the face of a big custom model gets garbage morphs.
//
// The fix:
//   1. raise the buffer size (ROTTRFramework.ini [Rendering] BlendShapeBufferMB,
//      default 64) before the renderer creates it;
//   2. hook Alloc so that, if the buffer still overflows, the mesh gets an
//      out-of-range offset (GPU reads return 0, writes are dropped), so the
//      face shows no morph for that frame instead of corrupted morphs, and the
//      overflow is logged with the size that would have been needed.
namespace BlendShapePatch {
bool Install();
bool Installed();

struct Stats {
    std::uint32_t capacityBytes;   // actual buffer size the game created (0 until first draw)
    std::uint32_t peakBytes;       // highest per-frame demand seen
    std::uint32_t failures;        // allocations that did not fit
};
Stats GetStats();
}
