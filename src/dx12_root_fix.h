#pragma once
// (v0.9.457) DirectX 12 with tessellation: black terrain, faces, trees in flashes (a game bug that NVIDIA drivers since
// 572 show; DX11, tessellation off or driver 566.36 hide it).
//
// The game (retail RVAs; TR2_x64_final names): the DX12 renderer has four graphics root signatures
// (PCDX12DeviceManager +0x20..+0x38: without tessellation or geometry shader, with a geometry shader, with tessellation,
// with both; the tessellation ones have six more descriptor tables, the hull and domain shaders', so only the geometry
// shader's tables move). Every graphics root parameter is a descriptor table, bound by PCDX12DescriptorCache::FlushCache
// (0x30A6E0) for the shader stages marked dirty (m_dirtySRVStage +0x1C, m_dirtyCBStage +0x20, m_dirtySamplerStage
// +0x24, m_dirtyUAVStage +0x28) and in use (m_activeStages +0x2C).
// - PCDX12RenderContext::PreDraw (0x310860, TR2 0x333B10) picks the root signature from the hull / domain / geometry
//   shaders bound and, when it differs from the one it last set (the context's pipeline state +0x34F0, its index
//   +0x3810), calls SetGraphicsRootSignature, but marks nothing dirty: the tables of the stages not dirty (most often the
//   vertex and pixel shaders') stay as they were under the other root signature. D3D12 makes every root argument
//   undefined when the root signature changes; older drivers kept them (the two layouts agree there), newer ones do
//   not: a tessellated draw after an untessellated one with the same textures samples nothing (black).
//   RestoreRootSignature (0x312790) and StateRestore (0x315560), which also set it, mark every stage dirty.
// - PCDX12RenderContext::UpdateCommandList (0x316CC0: each flush's new command list) and TransferCommandList (0x315850)
//   bind the first root signature on the new list without telling the context, which may still hold a tessellation
//   one: the next tessellated draw then skips SetGraphicsRootSignature and runs with the wrong one bound.
// The fix: PreDraw's SetGraphicsRootSignature call (0x3108DD) goes through a stub that marks every stage dirty after it
// (as RestoreRootSignature does: SRV, CB, sampler 0x3F, UAV 0x30), so FlushCache binds every table again under the new
// root signature; after UpdateCommandList / TransferCommandList the contexts hold the root signature their list has
// (the first, index 0) and their tables are marked dirty. ROTTRFramework.ini [Rendering] Dx12TessellationFix (default
// on; switchable at any time).

namespace Dx12RootFix {
bool Install();          // the patch and its two hooks (before MH_EnableHook(MH_ALL_HOOKS)); false: unavailable (logged)
bool Available();
bool Enabled();
void SetEnabled(bool enabled);
}
