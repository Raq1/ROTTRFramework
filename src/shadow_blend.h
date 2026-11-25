#pragma once

#include <d3d12.h>

#include <cstdint>

// (v0.9.516) The sun's shadows blended between their cascades (the user: the step from sharp to soft shadows a few
// metres from Lara was distracting). The game's shadow-mask pixel shaders (the full-screen pass that writes the sun's
// shadow term, 20 variants embedded in ROTTR.exe: its filter kernels, MSAA depth or not) pick one cascade per pixel,
// the nearest that holds it, and sample only that one: a hard line where one cascade hands over to the next. Patched
// copies of them (tools/shadow_blend: patch.py, built into shadow_blend_shaders.inc) run the filter kernel a second
// time on the next farther cascade over the last fifth of each cascade and blend the two by how far into that fifth
// the pixel is; past the last cascade the shadow fades to lit instead of ending in a line. They take the originals'
// place when the game makes a pipeline with one of them (ID3D12Device::CreateGraphicsPipelineState, hooked by
// eight_weight_patch.cpp), matched by the original's DXBC checksum.
//
// (v0.9.518) Lara's hair not shadowing itself, its shadow on her and the ground kept. The hair is lit in its own pass,
// the PureHair resolve (ROTTR.exe 0x137C4AC: HairPixelHeadSRV, HairElementsSRV, the tiled lights), whose shadows are
// TressFX's simplified deep shadow map: per tap of a 3 x 3 Gaussian kernel, the occluder's depth from the shadow map
// (linearised), range = max(0, the fragment's depth - (the occluder's + 1)), fibres = range / spacing (+1 when there is
// a range), light = (1 - alpha) ^ fibres. Whatever is nearest the light counts, the hair's own outer strands too: the
// hair shadows itself (and with no depth bias in the hair's shadow pass, an acne that moves with the cascades: the
// flicker of v0.9.489). The "+ 1" (game units, cm) is that pass's only slack. Patched (its four shadow paths: the
// front fragment and the ones behind it, the sun and the shadowed local lights) to "Hair self-shadow distance": the
// hair ignores what lies within that distance in front of it, its own strands, while her head, her body and the
// scenery, farther, still shadow it. The hair's shadow pass is left as the game makes it, so the shadow it casts on
// her and the ground stays whole (v0.9.490's "Hair self-shadow offset" pushed that whole shadow back instead).
//
// (v0.9.525) Lamp shadow softness. The user: the hair's shadow on Lara's neck from a lamp in stair steps, even at the
// largest map (Shadow map size). The lamps (spot lights; omni lights have no shadows) are lit in the tiled lights' pass
// (TiledLightingPS, 32 variants embedded in ROTTR.exe), whose shadow is four bilinear comparison taps half a texel
// apart (cb2[71].zw, the atlas's texel) on the shadow atlas: a filter about two texels wide, the map's texels left as
// soft-edged steps. Patched copies (tools/shadow_blend/lamp_patch.py then; light_patch.py since 0.9.526) run that
// block at four centres moved by (+-1, +-1) steps with its texel scaled to the step, and average them: a uniform 4 x 4
// grid of taps (16), 1, 1.5 or 2 texels apart (Soft, Softer, Softest), about 4, 6 or 8 texels wide. The spacing is a
// float in the patched shader, written when the game makes its pipeline (so a change takes effect at the next start).
//
// (v0.9.526) Contact shadows. The user: gameplay lighting next to cutscenes' (the inside of Lara's mouth lit, flat faces
// in the mines). Cutscenes switch on their own hand-placed lights (LightManager::UpdateLights, CinematicHandler's cine
// light modes); in play the lamps' and the sun's shadow maps miss the small scale, and the point lights cast no shadow at
// all. The same lights' pass now also traces, for the sun, point / capsule lights and spot lights (not box lights), a
// short ray from each pixel toward the light through the depth buffer (8 samples over the chosen length, closer ones
// denser, jittered per pixel); what stands in front of it within a few cm takes that light's diffuse and specular away,
// most where the occluder is nearest. Screen space only: what is off screen or hidden casts none. Both patches are in
// one shader per variant (tools/shadow_blend/light_patch.py, built into light_pass_shaders.inc), its four values written
// when the game makes its pipeline (contact strength and length, the lamp taps' steps).
namespace ShadowBlend {
// The patched shader for a pipeline's pixel shader, else nullptr (logged once per variant).
const D3D12_SHADER_BYTECODE* Replacement(const D3D12_SHADER_BYTECODE& ps);
// (v0.9.518) The hair's self-shadow distance in cm (1: the game's own); used when the game makes the hair's pass.
void SetHairSelfShadowDistance(float cm);
float HairSelfShadowDistance();
// (v0.9.525) Lamp shadow softness: 0 the game's, 1 soft, 2 softer, 3 softest; used when the game makes the lights' pass.
void SetLampShadowSoftness(int softness);
int LampShadowSoftness();
// (v0.9.526) Contact shadows: 0 off, 1 light, 2 medium, 3 strong; their length (4 .. 30 cm). Used when the game makes the
// lights' pass.
void SetContactShadows(int level);
int ContactShadows();
void SetContactShadowLength(float cm);
float ContactShadowLength();
// (v0.9.537) The debug view: every light's contribution replaced by plain white light, red where the contact shadows
// darken it (the pixel's lowest visibility in its green and blue), the specular off. Live, not saved.
void SetContactShadowsDebug(bool on);
bool ContactShadowsDebug();
// (v0.9.561) the lights' pass reads the ray tracing probe's lamp shadow mask (t45) and its lamps (t46; 0.9.561: t50,
// t51): each lamp in it lit only where its ray reaches; at once (the lights' pass made again). (v0.9.562) Switched by the
// probe only (rt_probe.h, SetLampShadows): on once its descriptors were checked to be in the pass's table
void SetRtLampShadows(bool on);
bool RtLampShadows();
// (v0.9.527) While the game runs: the lights' pass's variant of a pixel shader (its original's checksum; -1 none), the
// settings now as a key (0: both off, the game's own shader), the patched shader for a variant and key (nullptr for 0),
// a variant's name. eight_weight_patch.cpp makes the pipeline again when the key changes (no restart).
int LightPassVariant(const D3D12_SHADER_BYTECODE& ps);
std::uint32_t LightPassKey();
const D3D12_SHADER_BYTECODE* LightPassBytecode(int variant, std::uint32_t key);
const char* LightPassName(int variant);
// (v0.9.570) The sun's shadow pass the same way: its variant of a pixel shader (-1 none), the key (1: the probe's sun mask
// read, t42; 0 not: the cascades as before), the patched shader for a variant and key (the cascade blend either way), a
// variant's name. Switched by the probe only (rt_probe.h, SetSunShadows): on once its descriptor was checked to be in the
// pass's table.
int SunPassVariant(const D3D12_SHADER_BYTECODE& ps);
std::uint32_t SunPassKey();
const D3D12_SHADER_BYTECODE* SunPassBytecode(int variant, std::uint32_t key);
const char* SunPassName(int variant);
void SetRtSunShadows(bool on);
bool RtSunShadows();
// A pipeline with a patched shader the device refused (the original's used instead): logged.
void Refused(const D3D12_SHADER_BYTECODE& replacement, long hr);
}  // namespace ShadowBlend
