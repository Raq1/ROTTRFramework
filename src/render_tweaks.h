#pragma once
#include <cstdint>

// (v0.9.487) Draw distances and shadow detail past the game's own settings: F8 > General > Graphics.
//
// - LOD distance: CommonRenderDevice::ApplyLODDistanceScale (TR2 0x2B5810, retail 0x296C90) turns every LOD switch
//   distance into distance * g_settingsLODScale_InUse (from the quality settings; retail 0xFC1F58) * the caller's scale
//   * g_globalLODScale (retail 0xFC1940, 1.0; only the VR manager writes it) + g_settingsLODBias_InUse (0x1A53E3C).
//   v0.9.487 wrote the multiplier into g_globalLODScale every tick, but VrManager::Process (TR2 0x4C5B00) sets it back
//   to 1.0 every frame outside VR, so frames alternated between the two and the LODs flickered (the user's video).
//   v0.9.488: ApplyLODDistanceScale is hooked and the scale it is given times the multiplier (not for calls with flag
//   bit 0, which the game keeps from scaling up): higher keeps detailed models farther. At once.
// - Shadow resolution (v0.9.487-489): NxAppImpl::ConvertShadowResolution (TR2 0xCCA9E0, retail 0xC82170) was hooked, but
//   the user saw no difference: nothing calls it (its one vtable slot, +0x268 of the table at TR2 0x140E912A0, is
//   called only by unrelated classes'). The real sizes (v0.9.490): CommonLightManager::CreateShadowMap (TR2 0x2BE560,
//   retail 0x29F310) makes one shadow atlas, 8192 x 4096 shifted by (DisplaySettings +0x6C) - 1 (the Shadow quality
//   option; +0x64: shadows on), the shift kept at +0x1450. Every light's map (GetShadowMapSize: the light's own size,
//   at least 1024 for the sun) is placed by ShadowMapPool::AddMap in an 8192 x 4096 space, capped at 2048 there, and
//   scaled by the same shift. At the top option (the user's registry ShadowQuality 3) that is 16384 x 8192, the widest texture
//   D3D12 allows, with the sun's cascades at up to 4096 each: a larger sun map does not fit. Removed.
// - (v0.9.490) Shadow distance and sharpness: the sun is a PSSM light (type 5) of up to 4 cascades, whose ends are the
//   scene's +0xA80 .. +0xA8C (the farthest first; count +0xA94; a new scene's: 2000, 4000, 8000, 16000), read by
//   GetShadowRegionPlanes for every cascade's region. TERRAIN_CalculateFogging (every frame) takes them from the stream
//   unit's fog data (blended, GraphicsOverrideSet's override, one cascade fewer at Low) into
//   CommonScene::SetPSSMDistances (TR2 0x2E3160, retail 0x2C0590; this, count, d1 .. d4, nearest first). Past the last
//   cascade's end the sun casts no shadow at all: the user's far shadows that vanish. Hooked, for that call only
//   (return 0x42CAE1): the farthest end times "Shadow distance", the nearest divided by "Shadow sharpness" (each
//   cascade's map covers less ground, so its texels are smaller close to the camera), the ones between on the same
//   curve in log space. Both at once. A far cascade covering more ground is coarser; sharpness makes up for it close by.
//   (v0.9.515) The ones between: each keeps the level's own far-to-near ratio (its sharpness), the extra span going
//   mostly to the farthest (weights 0 / 0.3 / 0.7): at x8 4.5 / 12.9 / 48 / 1040 m instead of 4.5 / 32 / 90 / 1040 m.
//   A sharper sun map is not possible: at the top Shadow quality the atlas is already the widest texture D3D12 allows.
//   (v0.9.516) The hand-over from one cascade to the next blended in the shadow pass itself: shadow_blend.h.
// - Fog distance: TERRAIN_CalculateFogging (every frame) gives GameRender::SetFog (TR2 0x8B7A80, retail 0x879010;
//   this, screen, enabled, const Vector3& color, float near, float far) the distance fog's start and end. Hooked:
//   both times the multiplier. At once. (The non-linear height fog, AddNonLinearFog, is left as it is.)
// - (v0.9.489) Hair shadows. The user: Lara's hair shadow flickers (a vanilla bug). Frames of their video: dark patches on
//   the crown and the ponytail change from one frame to the next with the camera nearly still (aiming). The hair is
//   drawn into the shadow maps by HairShadowDrawable (its constructor TR2 0x2F7500, retail 0x2D4630: (this, device,
//   RenderHairData*, float a, float b, const Matrix&, bool, const Vector3&); a at +0x28, b at +0x2C; Draw, vtable 0,
//   retail 0x2D5B70, does nothing while +0x18, the hair data, is null). DynamicHairDrawable::Draw (TR2 0x296672) makes
//   it per shadow pass with a = the pass info's +0x10 times a scale and b = its +0x14 times another (1 when there is no
//   pass info), and SetConstantData hands b to the shader: most likely the depth and slope biases. Thin strands with
//   too little bias give shadow acne that moves with every tiny change of the cascades (the aiming camera's sway): a
//   flicker. v0.9.489 scaled a and b ("Hair shadow bias"); with "Hair shadows" off, +0x18 is cleared after the
//   constructor, so the hair casts no shadow at all.
//   v0.9.490: the constants say otherwise: SetConstantData puts b in g_FiberRadius (the cb's layout from the shader's
//   reflection, embedded in the exe: g_mViewProj, g_mInvViewProj, g_vEye, g_FiberAlpha = 1, g_WinSize, g_FiberRadius,
//   g_fvFov, g_zMinMax, g_zWriteValue, ...), and DrawHairSM (retail 0x2D5D20) gives a to GetNumSlaveStripIndicesAtLOD:
//   a is the strand LOD, b the fibre radius, both left as the game has them now. The flicker's cause: DrawHairSM sets
//   RenderHairData's no-cull rasterizer state (retail 0xFF4450: depth bias 0, clamp 1, slope 0; GetNoCullRS, also used
//   by the hair's own drawing), so the strands go into the shadow map with no bias at all, while every other caster
//   gets the light's (RenderShadowMapDirectional / Spot: SetDepthBias(light +0xCC), SetDepthBiasSlope(light +0xD0)):
//   the hair shadows itself everywhere (shadow acne), a pattern that moves with every tiny change of the cascades.
//   "Hair self-shadow offset" (the user: keep its shadow on her and the ground, drop it on itself): that one
//   SetRasterizerState call is hooked (by its return address) and given a copy with the light's own bias and slope
//   times the offset: the hair's depth in the shadow map pushed back far enough that it stops shadowing itself, while
//   what is farther from the light (her body, the ground) still gets its shadow. 0: the game's own (no bias).
//   (v0.9.518) Left at 0 for good: pushing the hair's depth back moved its shadow on her body too (the user could not
//   keep both). The self-shadow is now left out where the hair is lit instead: shadow_blend.h, Hair self-shadow distance.
// - (v0.9.496) LOD distance and Shadow distance to x8. A shadow caster has its own range too: DrawableInfo::
//   ShadowMaskAndDistanceCull (TR2 0x37BF80, retail 0x359480) puts its distance through ApplyLODDistanceScale (the call
//   at retail 0x35956A) and drops it past the drawable's +0x84 (or before +0x7C): far objects cast nothing however far
//   the cascades go. That call (by its return address) now also gets the scale divided by Shadow distance (above x1,
//   whatever its flags). The cascades between the two ends keep the level's own spacing (in log space); the nearest
//   never under 4.5 m with Shadow sharpness, the farthest at most 4 km (unless the level's own is farther).
// - (v0.9.523) Shadow map size (x1 / x2 / x4; x8 since v0.9.524): the user's video, the hair's shadow on Lara's neck in blocks. Not the
//   atlas (above): each light asks for its own map size (CommonDeferredLight::GetShadowMapSize, retail 0x2B4F40: the
//   light's authored size, the sun at least 1024, others halved with distance) and ShadowMapPool::AddMap (retail
//   0x2953B0) caps it at 2048 of the atlas's 8192 x 4096 (twice that in pixels at the top Shadow quality). A light below
//   the cap gets its size multiplied, only while the frame's maps still fit the atlas (render_tweaks.cpp: the area
//   placed, this map, and what the lights after it asked for the frame before); a light at the cap stays. Each light's
//   result is logged ("asks for ... given ...").
// ROTTRFramework.ini [Rendering] LodDistance, ShadowDistance, ShadowSharpness, FogDistance (1 = the game's own),
// ShadowMapSize (1, 2, 4 or 8),
// HairSelfShadowDistance (10, cm: shadow_blend.h).
namespace RenderTweaks {
bool Install();          // the hooks (early startup group); false: some unavailable (logged)
void TickGameThread();   // the LOD multiplier into the game

bool LodAvailable();
bool ShadowAvailable();
bool FogAvailable();
float LodDistance();
float ShadowDistance();
float ShadowSharpness();
// (v0.9.523) each light's shadow map size multiplier (1, 2, 4; v0.9.524: 8) while the atlas has room
bool ShadowMapSizeAvailable();
int ShadowMapSize();
void SetShadowMapSize(int scale);
float FogDistance();
bool HairOffsetAvailable();
float HairSelfShadowOffset();
void SetHairSelfShadowOffset(float value);
void SetLodDistance(float value);
// (v0.9.519) the models the game keeps from LOD scaling up (its foliage): their own multiplier
float FoliageLodDistance();
void SetFoliageLodDistance(float value);
// (v0.9.540) Skin scattering: the game's subsurface scattering of the skin (SSSRender: the diffuse light of the skin
// materials blurred under the surface, softening pores and the edges of shadows), which it draws in its cutscenes only,
// drawn in play too. The ears' transmission is drawn inside it: (v0.9.541) this switch keeps it on too (it was
// "Ear light transmission", v0.9.528). Takes effect at once.
bool SkinScatteringAvailable();
bool SkinScattering();
void SetSkinScattering(bool value);
// The model viewer: the skin's scattering while a model is shown (whatever Skin scattering is)
void SetViewerSkinScattering(bool on);
// How often BeginScene was answered yes since the last call (the model viewer's log)
std::uint32_t TakeSkinScatteringAsked();
void SetShadowDistance(float value);
void SetShadowSharpness(float value);
void SetFogDistance(float value);
}  // namespace RenderTweaks
