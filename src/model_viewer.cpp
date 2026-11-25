#include "model_viewer.h"

#include <windows.h>
#include <intrin.h>
#include <MinHook.h>
#include <xinput.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "cutscenes.h"
#include "features.h"
#include "game_build.h"
#include "imgui.h"
#include "instance_anim.h"
#include "overlay.h"
#include "log.h"
#include "nlohmann/json.hpp"
#include "render_tweaks.h"
#include "viewer_audio.h"
#include "viewer_text.h"
#include "zone_list.h"

namespace ModelViewer {
namespace {

// ---- What the viewer reads from the game (retail RVAs; TR2 PDB names) ---------------------------------------------------
// The model's size: the bind pose of its skeleton (Instance +0x80 the ObjectZone, +0x08 shared, +0x58 dtp::ZoneData,
// +0x78 Skeleton* (null: one segment), +0 SegmentData*: +0 count, +8 Segment[] (ObjectZone::GetSegmentList 0x2373D0,
// NumSegments' tail 0x23EC97; as live_pose.cpp)).
constexpr std::size_t kInstanceZone = 0x80, kZoneShared = 0x08, kSharedZoneData = 0x58, kZoneDataSkeleton = 0x78,
                      kSegmentDataSegments = 0x08;
// A player object (Lara's outfits, laracroft: the player's object is the outfit) cannot be born here: the instance's
// init (retail 0x3DC8BE) looks up its object data's "player" component data (the object's [+0x80] vfunc +8 with the
// hash of "player", the global 0x1AA0A58 the static init at 0xFA60 hashes) and, when there, calls
// PlayerComponentManager::AddComponent (retail 0x814340, TR2 0x14084F840): PlayerComponent::Init (retail 0x8355C0)
// for the player slot of the component ID's low half, which reads its instance (+8) unchecked: v0.9.407 crashed there
// on laracroft ([+8] null, read at +0x80). The viewer reads the same data before the birth: the instance's object data
// is the zone's (Instance +0x80 = 0x231010(zone): the zones' map [0x1A50B20] (buckets [+0], count [+0x10], bucket
// count [+0x18], zone % that; entry {next, zone +8, holder +0x10}; holder +8 the data, loaded at +0x8B), and its vfunc
// +8 (retail 0x234DA0) finds a type in [data +0x58]: count +0x28, entries +0x30 of 16 bytes {hash, -, data +8}.
constexpr std::uintptr_t kRvaPlayerDataCheck = 0x003DC8BEull;
constexpr std::size_t kZoneMapCount = 0x10, kZoneMapBuckets = 0x18, kZoneEntryId = 0x08, kZoneEntryHolder = 0x10,
                      kHolderData = 0x08, kDataLoaded = 0x8B, kDataComponents = 0x58, kComponentsCount = 0x28,
                      kComponentsEntries = 0x30, kComponentEntrySize = 0x10;
// The studio lights on every surface (v0.9.411): lights made by Scene::CreateLight light the forward materials only
// unless drawn deferred too; since v0.9.414 the framework does that for every light it makes (Features::
// DeferredLightsAvailable: Scene::TraverseForLights hooked there, CommonRenderLight::Draw for each enabled one).
// The black background (v0.9.411): while the model is shown, cdc::Scene::DoRender (retail 0x367470) draws nothing of
// the model's scene but the model: cdc::Scene::DrawEntity (retail 0x3556E0; (context, SceneEntity*, node, .., ..),
// from JobDrawEntities and DrawEntities; it ends with the entity's drawable (+0x100) vfunc +0x60) leaves out every
// entity of that scene (SceneEntity +0x40) whose drawable is not one of the model's (cdc::InstanceDrawable: IDrawable
// at +0, HighlightableDrawable at +8 with its Instance at +8: +0x10; m_sceneEntity +0xC0); cdc::Scene::DrawStaticMeshes
// (retail 0x367F70) and cdc::Scene::DrawGrass (retail 0x367E00, the static meshes from +0x1F0 to +0x1C0) are left out.
// The level's sky is an Instance (SkydomeController) so an entity: left out too. Checked once per scene on the model's
// anim drawable (Instance +0xD8 [8] +0x98): its entity's drawable is it, its +0x10 the model, its entity's scene the
// lights' scene; else the background stays.
constexpr std::uintptr_t kRvaDrawEntity = 0x003556E0ull, kRvaDrawStaticMeshes = 0x00367F70ull, kRvaDrawGrass = 0x00367E00ull;
constexpr std::size_t kEntityScene = 0x40, kEntityDrawable = 0x100, kDrawableOwner = 0x10, kDrawableSceneEntity = 0xC0;
constexpr std::size_t kInstanceComponentCount = 0xD0, kInstanceComponents = 0xD8, kDrawableComponent = 8,
                      kAnimDrawable = 0x98;
// The studio lights alone (v0.9.412; [ModelViewer] SceneLight=Off by default, E / Start toggles it): the scene's own
// light left out while the viewer is open.
// - The level's lights (the sun among them): LightRenderer::RenderLight (retail 0x1E2470; (renderer, ISceneLight*) ->
//   bool), what CoLightingRenderer::RegisterLight (retail 0x898A70, the terrain callback's SetupLight) ends with,
//   returns false for every light but the studio ones: Scene::TraverseForLights then takes the light's visible bit for
//   the view back (SceneLight +0x518; TR2 0x14039F22E btr) so the light is neither drawn deferred (no IRenderLight::
//   Draw) nor approved (no LightApproved), and Scene::DrawEntity's light gathering (TR2 0x1403782DD bt) passes it by
//   for the forward materials.
// - The ambient: GameRender::SubFrameStart (retail 0x87AC20) sets it every sub-frame from the unit's
//   dtp::UnitData::DTPNextGenData (StreamUnit +0xE8: globalAmbient +0, secondaryAmbient +0x680, sphericalHarmonic
//   +0x688, beautySpecular +0x730) and the graphics overrides: ApplyAmbient (retail 0x86FD20) ends with the render
//   device's (retail [0x1A53E40]) vfunc +0x240 (r, g, b: SceneBuffer GlobalAmbient, the sky), ApplySecondAmbient (retail
//   0x870A60) with vfunc +0x2D0 (global parameter 0xA, components 0..2, value: the ground), ApplyIrradiance (retail
//   0x870230) with vfunc +0x258 (10 float4 of spherical harmonics (cb6), true; zeros when the game's own "no
//   irradiance" constant is set), UpdateBeautySpecular (retail 0x87B2D0) with vfunc +0x2C8 (global vector 5: the beauty
//   light's direction, 6: its colour). Each is hooked: after the game's own, the same call again with zeros (the sky
//   and the ground the studio's since v0.9.422: see kStudioSky).
constexpr std::uintptr_t kRvaRendererRenderLight = 0x001E2470ull, kRvaApplyAmbient = 0x0086FD20ull,
                         kRvaApplySecondAmbient = 0x00870A60ull, kRvaApplyIrradiance = 0x00870230ull,
                         kRvaUpdateBeautySpecular = 0x0087B2D0ull, kRvaDeviceGlobal = 0x0087AC6Dull;
constexpr std::size_t kDeviceSetAmbient = 0x240 / sizeof(void*), kDeviceSetGlobalParam = 0x2D0 / sizeof(void*),
                      kDeviceSetIrradiance = 0x258 / sizeof(void*), kDeviceSetGlobalVector = 0x2C8 / sizeof(void*);
constexpr int kParamSecondAmbient = 0xA, kVectorBeautyColor = 6, kIrradianceVectors = 10;
// The fixed exposure (v0.9.413; [ModelViewer] AutoExposure=Off by default, A (pad A) toggles it). The game's eye
// adaptation brightens the frame towards a target from its average luminance: on the black background the model was
// raised far past white (and bloomed). BloomRender::Draw (retail 0x886DF0 -> InterpolateParams 0x893480, then
// SetupTonemapParams 0x8954B0 at 0x886E6C) blends the units' and the overrides' dtp::TonemapData into the BloomRender
// at +0xC4 (exposure_pre +0xC4, exposure_post +0xC8, ..., adaptationEnabled +0xD8 (InterpolateParams writes it every
// frame, retail 0x893977), AdaptationPerc +0xDC, AdaptationShiftLimit +0xE0, whitePoint +0xE4, adaptationSpeed +0xE8;
// TR2's tonemap debug text names them); SetupTonemapParams returns whether the luminance pass (CalculateLuminance)
// runs: not when +0xD8 is 0. The viewer's hook clears +0xD8 before the game's SetupTonemapParams.
// v0.9.423: the Suite's tone mapping (studio.h Environment: exposure 1.6, white point 4). The tone mapping is drawn by
// BloomRender::CalculateLuminance (retail 0x87FF30, called from the bloom drawable's Draw at 0x886D09 only when
// SetupTonemapParams said so: 0.9.413 stopped the adaptation, so it was not drawn at all): it fills the ToneMapInfo
// constants (+0x148..: adaptationPerc +0x170 = AdaptationPerc x 0.01 (0x880128), g_fExposure_pre +0x188 = 2^exposure_pre
// (0x880140, powf(2, x)), g_fExposure_post +0x18C = 2^exposure_post, whitePoint +0x184 = +0xE4 (0x8801B6; 5 when the
// render device's vfunc +0x38 says so)), runs the luminance passes (AdaptiveLuminanceCalcCS: the frame's brightness
// scale, inf / NaN made 0) and draws TonemapPS (the BloomRender's +0x208, read at 0x88064D; the same DXBC as TR2's,
// hash d00656d0b98f4e56): c x g_fExposure_pre x (1 + adaptationPerc x (brightness - 1)) x g_fExposure_post, then on the
// lightness L = (max + min) / 2: L (1 + L / W^2) / (1 + L) / L (W: whitePoint) -- the Suite's PSTone. So the viewer's
// hook sets, before the game's SetupTonemapParams: the adaptation on (+0xD8 = 1: the pass drawn), AdaptationPerc 0 (the
// frame's brightness out of it), exposure_pre log2(1.6), exposure_post 0, whitePoint 4 ([ModelViewer] AutoExposure=On,
// A / pad A: the game's own, its adaptation).
constexpr std::uintptr_t kRvaSetupTonemapParams = 0x008954B0ull, kRvaCalculateLuminance = 0x0087FF30ull;
constexpr std::size_t kBloomExposurePre = 0xC4, kBloomExposurePost = 0xC8, kBloomAdaptation = 0xD8, kBloomAdaptPerc = 0xDC,
                      kBloomShiftLimit = 0xE0, kBloomWhitePoint = 0xE4, kBloomAdaptSpeed = 0xE8;
constexpr float kStudioExposure = 1.6f, kStudioWhitePoint = 4.0f;
// v0.9.424: and none of the game's post effects the Suite does not have, in the same mode:
// - the bloom and the light scatter: BloomRender +0x80 m_InterpolatedData is the units' dtp::ScatterBloomData (0x44
//   bytes: scatterData.enabled +0x20, bloomData.enabled +0x34) before the TonemapData (+0xC4); SetupBloomParams (retail
//   0x894F40, called at 0x886E84 after SetupTonemapParams) returns false when +0xB4 is 0 (0x894F56), SetupScatterParams
//   (0x8951C0, at 0x886E8F) when +0xA0 is 0 (0x8952EA); their results pick DoBloom / DoDownsample + DoLightScatter and
//   the combine (none drawn when both are false, as with the game's own bloom option off). The hook clears both after
//   InterpolateParams wrote them.
// - the colour grading: LensEffectRender::DoColorCorrection (retail 0x882470, called at 0x88AED5; TR2 0x1408C1E90)
//   draws the frame again through a shader whose permutation is built from its flags (0x882BD1..: the third argument
//   (the units' contrast / saturation / tint curves, ColorCorrectionParams' 256-entry tables at LensEffectRender +0xC0,
//   on when they are not the identity: 0x8822A0 at 0x88A285) 2, the fourth (a colour transform from +0x2310.., the
//   identity when off) 1, the fifth (the film grain, with +0x22AC) 4, the sixth (the units' LUT texture, +0x20C0) 8,
//   +0x10 from +0x227C / +0x228C, +0x20 always): hooked, the four are passed as false.
//   v0.9.425: not drawn at all instead. With the four false the shader is permutation 0x20 (TR2
//   NxShader_colorcorrection_Default, 512 entries: 0x20 is saturate(c)^2.2, its input taken as gamma encoded), while
//   the frame it reads is linear (permutation 0x22, the curves: c^(1/2.2), the curves, ^2.2): every pixel was raised to
//   the power 2.2 more (0.9.424 and 0.9.425's pictures against the Suite's: the tank top 28, 20, 12 against 88, 77, 68
//   whose ^2.2 is 24, 18, 14; the trousers 96, 53, 30 against 158, 122, 104, ^2.2 89, 50, 35): darker, the skin orange.
//   The game itself does not call it when no grading is asked for (LensEffectRender::Draw, 0x88AE6C..0x88AE92: none of
//   the flags, no LUT: straight to 0x88AEDA, after the call); it sets "the colour correction drawn" (0x88AECE: mov byte
//   [0x142E1BFA0], 1, read and cleared by OffscreenAlphaRenderNext::Draw, 0x88B628 / 0x88B64B) just before the call.
//   The hook returns without drawing and clears that byte: as the game's own path without grading.
constexpr std::size_t kBloomScatterEnabled = 0xA0, kBloomBloomEnabled = 0xB4;
constexpr std::uintptr_t kRvaDoColorCorrection = 0x00882470ull, kRvaColorCorrectionDone = 0x0088AECEull;
// v0.9.423: the Suite's reflections (its PSReflect: what the reflected ray sees with no cube map, the harmonics (0) and
// the hemisphere (the studio's sky and ground), x its reflections 1). The game's reflection pass (cubereflection_SM5,
// the same DXBC as TR2's, hash 844d04f8366fdc8f): with g_fUberCubeInfl (cbCubeRefl +40) > 0 the level's global cube
// map (t4) x saturate(that), else max(SH(r), 0) + lerp(ground [38], sky [27], r.z / 2 + 0.5); then the probes of the
// pixel's tile (records of the tiled lights' buffer, CommonLightManager::SetupLightData's from the approved lights,
// which the studio's light filter leaves out);
// then the screen-space ray when SceneBuffer SSREnabled; x g_vReflIntensity (cbCubeRefl +16). Its constants are
// CubeReflRenderConstantsDrawable's (vtable retail 0xDAF040; its Draw, retail 0x888820, uploads the drawable's +0x18,
// 8 rows, as cb5; CubeReflRender::Draw fills it from the units' reflection data): the hook sets +0x18 + 16 (the
// intensity) to 1 and +0x18 + 40 (the global cube map's influence) to 0 while the studio lights are alone.
constexpr std::uintptr_t kRvaCubeReflConstantsDraw = 0x00888820ull, kRvaCubeReflConstantsVtable = 0x00DAF040ull;
constexpr std::size_t kCubeReflConstants = 0x18, kCubeReflIntensity = 16, kCubeReflUberInfluence = 40;
constexpr float kStudioReflections = 1.0f;
// v0.9.425: the game's ambient occlusion can be left out while a model is shown ([ModelViewer] AmbientOcclusion=On by
// default, as the Suite's preview has its own; O toggles it). SSAORender::Draw (retail 0x88C0D0, TR2 0x1408CCC80) blends the units' and the overrides' dtp::SSAOSystem
// (0x30 bytes, TR2 PDB: ssaoEnabled +0 (bool), angularBias +4, blurRadius +8, blurSharpness +0xC, powExponent +0x10,
// sceneScale +0x14, ssaoStrength +0x18, rangeScale +0x1C, ssaoColor r g b +0x20..0x22) with its InterpolateParams
// (retail 0x892F00, called at 0x88C14A only: the StreamUnit's DTPNextGenData +0x240 first), then reads +0 (0x88C160)
// and the render settings' ambient occlusion quality (IPCDeviceManagerPtr retail 0x3190A0, vfunc +0xA8, +0x70: 0 off;
// 2 NVIDIA's HBAO+ with its own fixed radius, else the game's SAO / HDAO, the HDAO's radius sceneScale x rangeScale):
// with +0 false the pass is not drawn, the ambient pass's bent normal weights stay (0, 1) (retail 0xD11AD0: its
// occlusion 1) and the occlusion texture is unbound, as with the game's own option off. 0.9.424's pictures had the
// clothes and the metals far darker (and bluer) than the Suite's, the skin not: the occlusion fits that (the ambient
// pass DefAmbientSSAOLightPS keeps 70 % of the light on the skin flag's pixels, the materials blend to the unit's
// ssaoColor by it: CommonScene::SetColorSSAO, scene vfunc +0xA0 at 0x88C1FE), the Suite's (12 cm, black) darkens only
// the creases. The hook clears +0 after the game's InterpolateParams; the values the game asked for go to the log.
constexpr std::uintptr_t kRvaSsaoInterpolate = 0x00892F00ull, kRvaDeviceManagerPtr = 0x003190A0ull;
constexpr std::size_t kSsaoEnabled = 0, kSsaoStrength = 0x18, kSsaoSceneScale = 0x14, kSsaoRangeScale = 0x1C,
                      kSsaoPower = 0x10, kSsaoColor = 0x20, kDeviceManagerSettingsSlot = 0xA8 / sizeof(void*),
                      kSettingsAoQuality = 0x70;
// v0.9.425: no wind on the model ([ModelViewer] Wind=Off by default, W toggles it). cdc::FXWindMgr::GetWindForce
// (retail 0x32AFE0, TR2 0x14034DF00: (out float4, the position, the instance's FX component, its stream unit, 2) -> out)
// gives the strand hair's simulation its wind (cdc::DynamicHairDrawable::Draw, retail 0x277A63: the position its
// Instance's +0x10, the force into HairComputeDrawable's constructor) and the cloth's (ClothComponent::Simulate, retail
// 0x1766EC, the position its instance's +0x10 too): the menu's level blows. The hook answers zero for the model's
// position (its Instance + 0x10).
constexpr std::uintptr_t kRvaGetWindForce = 0x0032AFE0ull;   // (the position: kInstancePosition, below)
// v0.9.425 (for the log): the strand hair's lights. Scene::DrawEntity (retail 0x3556E0) gathers a forward drawable's
// lights when its entity's +0x200 & 0x60 is 0x40 and its node's (third argument) +0x28 bit 1 is set: the node's +8
// light set (33 chunks of 16 bytes {SceneLight** +0, count +0xC}, retail 0x355777..0x355816), then the entity's static
// lights (+0x280 count, +0x288). The SceneLight's ISceneLight is at +0x2B0 (the viewer's handles).
constexpr std::size_t kEntityFlags = 0x200, kEntityLightMask = 0x204, kNodeFlags = 0x28, kNodeLights = 0x08,
                      kLightChunks = 33, kLightChunkSize = 16, kLightChunkCount = 0x0C, kSceneLightInterface = 0x2B0;
// v0.9.427: the relic viewer's lighting (v0.9.430: on the relics, and only on them; the studio's on every other model:
// the RELICS category's models, see Category::relics). The game's
// relic (collectible) viewer is CollectibleRender (GetInst retail 0x8763D0): CollectibleRender::RenderCollectibleView
// (retail 0x877130, TR2 0x1408B5BA0; from the sub-frame's end, retail 0x87ABBB) draws the relic in a view of its own,
// its camera at the origin, its axes the rows (1, 0, 0), (0, 0, 1), (0, 1, 0) (retail 0x87AAD5..: x right, +Y ahead,
// +Z up), with:
// - three lights of its own (InitLights, retail 0x876920; the render lights at retail 0x2E1BCF0): the global object's
//   (dtp::globalobject, the pointer at retail 0x2D57470, read at 0x876942) collectibleRigData (+0x14D8: m_bloomParams
//   +0, m_light0 +0x48, m_light1 +0x278, m_light2 +0x4A8), each {rotation (degrees) +0, position +0xC,
//   dtp::LightTemplateData +0x18 (0x218 bytes)}. The template is copied to +0xD8 of what retail 0x871F20 converts into
//   the render device's light properties (0x350 bytes, zeroed first: the type, the range, the intensity, the cone, the
//   near radius, the colour, the attenuation curve (the template's +0xB8 pointer), the shadows, the lambert / half
//   lambert / ambient / specular weights; the indirect values, the minimum shadow specular and the range scale left 0);
//   its matrix cdc::Matrix::Build (retail 0x1E3310) of cdc::Quat::Build (retail 0x1E3460) of the rotation x pi / 180,
//   its last row the position, then x the CollectibleRender's +0x60 (cdc::Mul4x4, retail 0x1E4FD0, (out, a, b): b x a):
//   the translation by retail 0x2E1BDB0 (its static init, retail 0x38970: (0, 250, 0); set at 0x86F194 when the viewer
//   opens). The relic itself at its +0x50 (0, 50, 0) (UiMenuStateCollectibleViewerBase, retail 0x91BB6C) + that.
// - no ambient: SetAmbient 0, global parameter 0xA 0, the irradiance nine zeros and (0.318, 0, 0, 0) (retail
//   0x87727B), the aperture adjustment (1, 0, 0) (RenderManager::SetAperatureAdjustment, retail 0x878720: global
//   parameters 1..3 .w = 1, 0, 0; the game's own frame 1, -1.5, 1);
// - the rig's bloom: the BloomRender drawn with +0x198 = 1 and +0x1A0 = the rig (retail 0x877629..0x877644):
//   InterpolateParams (retail 0x893480) then copies the 0x44 bytes of +0x1A0 into +0x80 (the units' ScatterBloomData)
//   and leaves the TonemapData alone; SetupTonemapParams (retail 0x8954B0) answers false with +0x198 set (0x895542):
//   no tone mapping.
// In the viewer: the three lights made from the rig as the game converts them (0x871F20 called on a copy), placed about
// the viewer's camera as the relic viewer's about its own (its axes the camera's right, forward and up), scaled by the
// viewer's camera distance over the relic viewer's (the range and the near radius too), without shadows; the ambient
// as the relic viewer's; the rig's bloom into the BloomRender's +0x80 and +0xD8 cleared (no tone mapping) by the hook on
// SetupTonemapParams, the aperture (1, 0, 0) after it; the level's reflections left as they are.
constexpr std::uintptr_t kRvaCollectibleInitLights = 0x00876920ull, kRvaRigObjectRead = 0x00876942ull,
                         kRvaRigLightProperties = 0x00871F20ull, kRvaQuatFromEuler = 0x001E3460ull,
                         kRvaMatrixFromQuat = 0x001E3310ull, kRvaSetApertureAdjustment = 0x00878720ull,
                         kRvaRigOffsetRead = 0x0086F194ull, kRvaRelicPlaceRead = 0x0091BB6Cull,
                         kRvaRigIrradianceRead = 0x0087727Bull;
constexpr std::size_t kRigBloom = 0x14D8, kRigBloomSize = 0x44, kBloomInterpolated = 0x80, kRigLightRotation = 0,
                      kRigLightPosition = 0xC, kRigLightTemplate = 0x18, kRigTemplateSize = 0x218, kRigBuildTemplate = 0xD8,
                      kTemplateComplexity = 0x10;
constexpr std::size_t kRigLights[3] = {0x1520, 0x1750, 0x1980};
// v0.9.428: a light of complexity refl (8) is a reflection light: InitLights sets its render light's +0x144 bit 8
// (retail 0x876AF2), as cdc::SetLightProperties does for the level's (TR2 0x140453401); cdc::LightSet::
// BuildConstantBuffer leaves such a light out of the forward lights (retail 0x298858), CommonLightManager::SetupLightData
// marks it in its tiled record (retail 0x2C2E5F). Features::ViewerLightSetReflectionOnly does the same for the viewer's.
constexpr int kComplexityReflection = 8;
// v0.9.430: the skin's subsurface scattering (always on since v0.9.436: no setting, no key). The game
// has it (SSSRender::Draw, retail 0x88C860, TR2 0x1408CD420: the materials whose stencil reference has bit 0x10 (head,
// arms, hands, ears, eyes, teeth) blurred in the diffuse light buffer before the composite, SSSRenderPS, and the ears'
// transmission, SSSTransmitPS; docs/materials.md, the Suite's "Skin"), but draws it in its cutscenes only:
// GameRender::BeginScene (retail 0x871257..0x87128A, TR2 0x1408AFC47..) calls it when DebugDrawFlags_GetMdlView
// (retail 0x3D4BA0: a debug flag's bit 1; TR2's other caller is GameRender::DrawPlayerDebug, retail 0x8735ED) answers
// non-zero or cdc::CinematicHandler::IsCinematicPlaying (retail 0x169D30) does. SSSRender::Draw itself goes on only
// while the byte the cutscenes' AGToggleSubSurfaceScatteringNode sets and clears (retail 0x732CDB..0x732CF9) is set
// (0x88C878; 1 in the exe's data). Hooked: DebugDrawFlags_GetMdlView answers 1 when called from BeginScene's place
// while a model is shown (its other caller and every other answer are the game's), and that byte is
// set while so (cleared again on closing when the viewer set it); SSSRender::Draw is hooked to count its calls.
// (v0.9.540) The flag's hook is RenderTweaks' now (Skin scattering in play too): the viewer asks it
// (RenderTweaks::SetViewerSkinScattering), which keeps the byte set while asked.
constexpr std::uintptr_t kRvaModelViewFlag = 0x003D4BA0ull, kRvaModelViewFlagCall = 0x00871257ull,
                         kRvaSssDraw = 0x0088C860ull, kRvaSssToggleRead = 0x0088C878ull;
// v0.9.445: no motion blur while the viewer is open (turning the view smeared the model over the black background in
// trails of copies). LensEffectRender::Draw (retail 0x88A36A, TR2 0x1408CAE17) blurs only while cdc::
// g_globalMotionBlurEnabled (retail 0xFC1944: ROTTR.pdb's .data +0x10944; 1 in the exe's data) is set; the scripts'
// AGSetMotionBlur::ProcessMsg (retail 0x746560, TR2 0x14079E160) sets and clears it. Cleared while the viewer is open
// (again each frame, should a script set it), set again on closing when the viewer cleared it.
constexpr std::uintptr_t kRvaMotionBlurRead = 0x0088A36Aull, kRvaMotionBlurClear = 0x00746576ull;
// The model's components (v0.9.418). Instance::Finalize (retail 0x3DC820, TR2 0x140406BC0) makes an instance's
// components from its object's data: Instance::InitComponents_Object (retail 0x3E0DA0) asks the object data (Instance
// +0x80, cdc::ObjectZoneRef: vtable retail 0xD288F8) for each component type's data by its hash (vfunc +8,
// ObjectZoneRef::GetComponent, retail 0x234DA0: the placement's override list (+0x10: count +0x94, entries +0x98) then
// the object's ([+8] +0x58: count +0x28, entries +0x30 of 16 bytes {hash, -, data +8})) and calls that type's
// manager's AddComponent when there is data (DrawableComponentManager's even without); Finalize then asks for "player"
// (the global the code at 0x3DC8BE reads) and calls PlayerComponentManager::AddComponent. The hashes are the names'
// (h ^= (h << 5) + (h >> 2) + c: the globals the static inits fill, e.g. "drawable" at 0x1AA09B0, read at 0x3E0E5C).
// While the viewer's birth runs (its thread, the model's object data), GetComponent (hooked) answers nothing for the
// types left out:
// - every model's sound ("sound", "soundskeletal", "bark": their AddComponent start the object's sounds and barks) but
//   while the sound is on (S / pad Back on the view: the model born again);
// - a player object's (Lara's outfits: PlayerComponent::Init (retail 0x8355C0) needs a player slot, v0.9.407 crashed in
//   it) all but its look: "drawable", "animated", "materialswap", "cloth", "bonesprings", "hardaccessory" and
//   "dynamichair" ("animated" since v0.9.420: the AnimComponent (made at 0x3E0FA1 by 0x3D8B70) builds the bones' matrices
//   the skin, the cloth (ClothComponent::TransformBones) and the hair's head marker follow, and the bind pose is written
//   in its AnimBuildTransforms; the hair: DynamicHairComponent loads the outfit's hair object (its data +4), births it
//   on a later frame (AllocHairInstance, TR2 0x140247A10) and links it to the head's marker; its
//   cdc::DynamicHairDrawable (TR2 0x14028E7C0) has the outfit's instance at +0x10 as an InstanceDrawable has, so the
//   black background draws it).
constexpr std::uintptr_t kRvaGetComponent = 0x00234DA0ull, kRvaObjectZoneRefVtable = 0x00D288F8ull,
                         kRvaDrawableHashRead = 0x003E0E5Cull;
// v0.9.419: the components the game makes from the object's lists themselves. InitComponents_Object (retail 0x3E0DA0)
// ends in 0x22CA30 (its tail jump at 0x3E1B1A), whose 0x221EC0 goes through the placement's list ({hash, data} pairs:
// ObjectZoneRef +0x10 -> +0x94 count, +0x98 entries) and then the zone's (ObjectZoneRef +8 -> +0x58 -> +0x28 count, +0x30
// entries) and calls each hash's maker with (the instance, its data), from a cdc hash map (retail 0x1A50C58: +0 the
// buckets, +0x10 the count, +0x18 the buckets' count; a node: +0 next, +8 the hash, +0x10 the maker; 0x225150 finds or
// adds one; the static inits fill it through 0x2295D0). Those makers (soundskeletal, cold, collectible, concealment,
// fingerofgod, grapple, hunger, markupdetection, objectnetdata, powermeter, relic, respawnmarker, scoreattack,
// targetindicator, watervolumedetection) don't ask GetComponent; the components they make do, later: the
// SoundSkeletalComponent's (0x39EC30) init, 0x3AC6D0, reads its data by GetComponent and uses it unchecked, and 0.9.418
// crashed there at the first outfit (its data left out, the component made all the same). The map's makers are wrapped
// (at the first opening): a type left out is not made for the model at all. The other maps' calls for these types
// (0x1A50C08, 0x1A50C80, 0x1A50C30: 0x3AA980, 0x3AA9C0, 0x3AAA00, 0x60C5D0...) find the component in the instance
// (+0xD0 count, +0xD8 the components) and do nothing without it.
constexpr std::uintptr_t kRvaComponentMakers = 0x01A50C58ull;
// v0.9.421: the makers' map is one of five per-type maps of calls (cdc hash maps alike, filled by the static inits:
// 0x2295B0 0x1A50C08, 0x2295D0 0x1A50C58 the makers, 0x2295F0 0x1A50C80, 0x229610 0x1A50C30, 0x229650 0x1A50CA8), each
// gone through by a loop over the object's lists as the makers' is (0x221D10, 0x221EC0, 0x2220A0, 0x222250, 0x222400:
// the instance from rcx in r15 (r13), its ObjectZoneRef at +0x80; the call with the instance, the makers' with the
// data too). 0x1A50CA8's are the removal's (0x222400, the tail jump at 0x22D5B8 in 0x22D570, which the instance's
// removal calls at 0x3E7F3E): the cold one (0x5DC820) reads the instance's ColdComponent (the id at 0x2D30A60) and its
// +0x10 unchecked, and 0.9.420 crashed there (ROTTR+0x5DC84B, reading 0x10) when the first outfit, born without it,
// was taken away for the next. Now every map's entries are wrapped: for a model, a type left out has none of its calls.
constexpr std::uintptr_t kComponentCallMaps[] = {0x01A50C08ull, kRvaComponentMakers, 0x01A50C80ull, 0x01A50C30ull,
                                                 0x01A50CA8ull};
constexpr const char* kComponentCallMapNames[] = {"0x1A50C08", "the makers", "0x1A50C80", "0x1A50C30", "the removals"};
constexpr std::size_t kInstanceObjectRef = 0x80, kMapBuckets = 0x00, kMapCount = 0x10, kMapBucketCount = 0x18,
                      kNodeNext = 0x00, kNodeHash = 0x08, kNodeValue = 0x10;
// v0.9.420: the object data's item lists, a second list in the zone's data (ObjectZoneRef +8 -> +0x58 -> +0x38
// count, +0x40 entries of 16 bytes {hash, ...} sorted by hash; retail 0x235800 finds one by binary search and returns
// the entry, else null; 85 callers: markers, shapes, splines, "actiongraphs"...). Finalize (0x3DCB43) calls 0x3D09D0,
// which asks it for "actiongraphs" (the hash at 0x1AA0854, read at 0x3D0A15) and births each of the object's action
// graphs as an instance of its own (InstanceBirthParams type 1, vtable 0xD43930, through 0x3D1310); for type 1 0x3D1609
// hands it to [0x2D9AA50]'s 0x71F180, which starts the graph's nodes (0x71E5F0, 0x720660: each node's vfunc +8): Lara's
// graph's AGGetPlayerInfo (vtable 0xD6C2E8, vfunc +8 0x737450) reads the outfit's PlayerComponent (Instance +0xD8 [the id at
// 0x10FAA90], the PlayerComponent array's: 0x382B0) at +0x2A8 unchecked when the object's flags have 0x20000, and
// 0.9.419 crashed there (ROTTR+0x737513, reading 0x2A8: the player component left out). Hooked: for a
// player object "actiongraphs" is left out (its other lists, the cloth's shapes among them, stay); the three callers
// asking for it (0x3D0A2A, 0x3D119D, 0x3EAAA1) check for null.
constexpr std::uintptr_t kRvaFindItemList = 0x00235800ull, kRvaActionGraphsHashRead = 0x003D0A15ull;
// ... and the player's flag: the object's flags (retail 0x2357D0: the zone data +0x58 -> +0x20 -> +0x44, two words: its
// 202 callers read at most those 8 bytes and keep no pointer) have 0x20000 for a player object, and the code that sees it
// reads the instance's PlayerComponent and uses it unchecked (AGGetPlayerInfo at 0x7374D2, 0x13AB2C, 0x16550F,
// 0x16C798, 0x3EF8E1...: 82 places). Hooked: while the model is a player object its flags are answered with a copy
// without 0x20000 (a thread's own), as for any other object, so none of them takes the player's way.
constexpr std::uintptr_t kRvaObjectFlags = 0x002357D0ull;
constexpr std::uint32_t kObjectPlayerFlag = 0x20000;
// The model's bounds (v0.9.418): what the game culls it by. Each cdc::SceneEntity has its drawable's volume
// (cdc::BasicCullingVolume, 0x60 bytes: its data at +0, its type at +0x50: 0 a sphere (x, y, z, r), 5 a box (half
// axes x, y, z, centre: four float4)) in its own space at +0x130 (SceneEntity::QueryVolumeFromDrawable, retail 0x372A10:
// the drawable's (+0x100) vfunc +8 GetBoundingVolume) and in the world's at +0x190 (TransformVolumeAndPivot, retail
// 0x379D10: +0x130 by the matrix at +0x70); the model's entities' world volumes as Scene::DrawEntity sees them make its
// box (about its place), the camera's distance (a rigid relic or weapon has one bone: its size was a guess before).
constexpr std::size_t kEntityWorldVolume = 0x190, kVolumeType = 0x50;
constexpr std::uintptr_t kRvaQueryVolume = 0x00372A10ull, kRvaTransformVolume = 0x00379D10ull;
// The game's lists (v0.9.418):
// - Outfits: dtp::GlobalCrafting ([0x2D57430], read at 0x452F8F by Crafting::HasDLCForOutfit) +0x30 m_numOutfits (read at
//   0x4431B9), +0x38 m_outfitsList of dtp::OutfitsData (0xA8: +0 m_name, +2 m_description (string IDs), +4
//   m_playerObject (the outfit's object), +0x38 m_previewImage (dtp::ImagePath: the campsite's picture), +0x80
//   m_debugName); the custom outfits are in it (CustomOutfits).
// - Weapons: dtp::GlobalUIInfo ([0x2E1E720], read at 0x8E6E49) +0x1060 NumInGameWeapons, +0x1068 m_weaponList of
//   dtp::WeaponUIInfo (0x50; UiMenuStateCampsiteGear::DetermineWhatWeaponToUpgrade..., retail 0x8E6DF0, and the weapon
//   upgrade screen's Process, 0x93CA34: +4 m_objectID, +6 m_uiObjectID (the menu's model), +8 m_name (SendWeapon-
//   Information reads it at 0x9BA4FE), +0xA m_weaponDesc, +0x44 m_IAmABrokenWeapon).
// - Relics (v0.9.419): GlobalUIInfo +0x3F8 m_collectibleSetMasterCollection: +0x3F8 the sets, +0x400 their count (read at
//   0x447D80, 0x447D8E, 0x448034: the game finds a collectible by its ID there). GlobalUIInfo +0xA8 m_collectibleRelics
//   (+0xB0 the sets, +0xB8 their count: 0x447ED0, 0x447B00), where 0.9.418 looked, has no sets in the game's
//   globaluiinfo (the documents' and the murals' lists beside it have theirs); the master collection has the 24 relic
//   sets, 130 relics (Blood Ties' among them). dtp::GlobalUIInfo::CollectibleSet (0x78: +0x58 m_collectibleType (0: a
//   relic), +0x68 its items, +0x70 their count; 0x446FC2, 0x447E75, 0x447E82), CollectibleItem (0xE8: +0 m_sItemName, +2
//   m_sItemDesc, +8 m_iType (0: a relic), +0xE m_ObjectID: the relic viewer births it, retail 0x908ED2; +0x14
//   m_audioClip, v0.9.431: the line's sound DTP, which the game's reward node plays, retail 0x728E8E).
// Their names: localstr_get (ViewerText::GameText).
constexpr std::uintptr_t kRvaCraftingRead = 0x00452F88ull, kRvaUiInfoRead = 0x008E6E49ull;
constexpr std::size_t kCraftingOutfitCount = 0x30, kCraftingOutfits = 0x38, kOutfitSize = 0xA8, kOutfitName = 0x00,
                      kOutfitDescription = 0x02, kOutfitObject = 0x04, kOutfitDebugName = 0x80;
constexpr std::size_t kUiWeaponCount = 0x1060, kUiWeapons = 0x1068, kWeaponSize = 0x50, kWeaponObject = 0x04,
                      kWeaponUiObject = 0x06, kWeaponName = 0x08, kWeaponDescription = 0x0A, kWeaponBroken = 0x44;
constexpr std::size_t kUiCollectibleSets = 0x3F8, kUiCollectibleSetCount = 0x400, kSetSize = 0x78, kSetType = 0x58,
                      kSetItems = 0x68, kSetItemCount = 0x70, kItemSize = 0xE8, kItemName = 0x00, kItemDescription = 0x02,
                      kItemType = 0x08, kItemObject = 0x0E, kItemAudioClip = 0x14;
constexpr std::uint32_t kCollectibleRelic = 0;   // dtp::EnumCollectibleType CollectibleTypeRelic
struct Check {
    std::uintptr_t rva;
    std::uint8_t bytes[24];
    std::size_t count;
    const char* what;
};
const Check kChecks[] = {
    {0x002373D0ull, {0x48, 0x8B, 0x41, 0x08, 0x48, 0x8B, 0x48, 0x58, 0x48, 0x8B, 0x41, 0x78, 0x48, 0x85, 0xC0, 0x75, 0x08}, 17,
     "ObjectZone::GetSegmentList (skeleton)"},
    {0x002373E9ull, {0x48, 0x8B, 0x00, 0x48, 0x8B, 0x40, 0x08, 0xC3}, 8, "ObjectZone::GetSegmentList (segments)"},
    {0x0023EC97ull, {0x48, 0x8B, 0x00, 0x8B, 0x00, 0xC3}, 6, "ObjectZone::NumSegments (tail)"},
    // ... and the player objects' (see kRvaPlayerDataCheck)
    {kRvaPlayerDataCheck, {0x48, 0x8B, 0x8B, 0x80, 0x00, 0x00, 0x00, 0x8B, 0x15, 0x8D, 0x41, 0x6C, 0x01, 0x48, 0x8B, 0x01, 0xFF,
                           0x50, 0x08, 0x48, 0x85, 0xC0, 0x74, 0x0C}, 24, "Instance init: the \"player\" component data"},
    {0x003DC8D6ull, {0x48, 0x8B, 0x4B, 0x68, 0x48, 0x8B, 0xD0, 0xE8, 0x5E, 0x7A, 0x43, 0x00}, 12,
     "Instance init: PlayerComponentManager::AddComponent"},
    {0x00814340ull, {0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20}, 10, "PlayerComponentManager::AddComponent"},
    {0x00231016ull, {0x85, 0xC9, 0x74, 0x4E, 0x4C, 0x8B, 0x05, 0xFF, 0xFA, 0x81, 0x01, 0x49, 0x83, 0x78, 0x10, 0x00, 0x74, 0x40,
                     0x33, 0xD2, 0x8B, 0xC1, 0x49, 0xF7}, 24, "the zone's object data: the zones' map"},
    {0x0023102Eull, {0x70, 0x18, 0x49, 0x8B, 0x00, 0x48, 0x8B, 0x04, 0xD0, 0x48, 0x85, 0xC0, 0x74, 0x2C, 0x0F, 0x1F, 0x40, 0x00,
                     0x39, 0x48, 0x08, 0x74, 0x08, 0x48}, 24, "the zone's object data: the bucket"},
    {0x00231046ull, {0x8B, 0x00, 0x48, 0x85, 0xC0, 0x75, 0xF3, 0x48, 0x85, 0xC0, 0x74, 0x16, 0x48, 0x8B, 0x58, 0x10, 0x48, 0x85,
                     0xDB, 0x74, 0x0D, 0x48, 0x8B, 0x43}, 24, "the zone's object data: the holder"},
    {0x0023105Bull, {0x48, 0x8B, 0x43, 0x08, 0x80, 0xB8, 0x8B, 0x00, 0x00, 0x00, 0x00}, 11, "the zone's object data: loaded"},
    {0x00234DE0ull, {0x48, 0x8B, 0x43, 0x08, 0x41, 0x8B, 0xCA, 0x48, 0x8B, 0x40, 0x58, 0x44, 0x8B, 0x40, 0x28, 0x45, 0x85, 0xC0,
                     0x74, 0x30, 0x48, 0x8B, 0x40, 0x30}, 24, "the object data's component data"},
    {0x00234DF8ull, {0x39, 0x10, 0x74, 0x24, 0xFF, 0xC1, 0x48, 0x83, 0xC0, 0x10, 0x41, 0x3B, 0xC8, 0x72, 0xF1}, 15,
     "the object data's component data: the entries"},
    {0x00234E20ull, {0x4C, 0x8B, 0x50, 0x08}, 4, "the object data's component data: its data"},
    // ... and the black background (see kRvaDrawEntity)
    {kRvaDrawEntity, {0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C,
                      0x24, 0x08, 0x55, 0x56, 0x41, 0x55}, 24, "cdc::Scene::DrawEntity"},
    {0x00355DC8ull, {0x49, 0x8B, 0x8D, 0x00, 0x01, 0x00, 0x00}, 7, "DrawEntity: the entity's drawable (+0x100)"},
    {0x00355DE8ull, {0x48, 0x8B, 0x01, 0x4D, 0x03, 0xC5, 0xFF, 0x50, 0x60}, 9, "DrawEntity: the drawable's draw (vfunc +0x60)"},
    {0x00355E91ull, {0xE8, 0x4A, 0xF8, 0xFF, 0xFF}, 5, "JobDrawEntities: DrawEntity"},
    {kRvaDrawStaticMeshes, {0x48, 0x8B, 0xC4, 0x57, 0x48, 0x81, 0xEC, 0x60, 0x02, 0x00, 0x00, 0x83, 0xB9, 0xC0, 0x01, 0x00, 0x00, 0x00,
                            0x48, 0x8B, 0xF9}, 21, "cdc::Scene::DrawStaticMeshes"},
    {kRvaDrawGrass, {0x40, 0x55, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x48, 0x02, 0x00, 0x00, 0x48, 0x8B, 0x05}, 17,
     "cdc::Scene::DrawGrass"},
    {0x00367699ull, {0xE8, 0xD2, 0x08, 0x00, 0x00}, 5, "cdc::Scene::DoRender: DrawStaticMeshes"},
    {0x003676A4ull, {0xE8, 0x57, 0x07, 0x00, 0x00}, 5, "cdc::Scene::DoRender: DrawGrass"},
    {0x00877391ull, {0x48, 0x8B, 0x81, 0xD8, 0x00, 0x00, 0x00}, 7, "Instance +0xD8 (its components)"},
    {0x008773B0ull, {0x48, 0x8B, 0x50, 0x40, 0x4C, 0x8B, 0xBA, 0x98, 0x00, 0x00, 0x00}, 11,
     "the DrawableComponent (component 8) and its anim drawable (+0x98)"},
    // ... and the scene's own light (see kRvaRendererRenderLight)
    {kRvaRendererRenderLight, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xD9, 0x48,
                               0x85, 0xD2, 0x0F, 0x84}, 21, "LightRenderer::RenderLight"},
    {0x00898AD8ull, {0xE9, 0x93, 0x99, 0x94, 0xFF}, 5, "CoLightingRenderer::RegisterLight: LightRenderer::RenderLight"},
    {kRvaApplyAmbient, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x0F, 0x29, 0x74, 0x24, 0x40, 0x4C, 0x8D, 0x4C, 0x24, 0x60, 0x48, 0x8B,
                        0xD9}, 19, "ApplyAmbient"},
    {kRvaApplySecondAmbient, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x0F, 0x29, 0x74, 0x24, 0x40, 0x4C, 0x8D, 0x4C, 0x24, 0x60, 0x48,
                              0x8B, 0xD9}, 19, "ApplySecondAmbient"},
    {kRvaApplyIrradiance, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55, 0x48, 0x8D, 0x6C, 0x24, 0xA9, 0x48, 0x81,
                           0xEC, 0xF0, 0x00, 0x00, 0x00}, 23, "ApplyIrradiance"},
    {kRvaUpdateBeautySpecular, {0x40, 0x57, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0x89, 0x70, 0x06,
                                0x00, 0x00}, 19, "UpdateBeautySpecular"},
    {0x0087AC84ull, {0x48, 0x8B, 0xCB, 0xE8, 0x94, 0x50, 0xFF, 0xFF, 0x48, 0x8B, 0xCB, 0xE8, 0xCC, 0x5D, 0xFF, 0xFF, 0x48, 0x8B, 0xCB,
                     0xE8, 0x94, 0x55, 0xFF, 0xFF}, 24, "GameRender::SubFrameStart: the ambient, the second, the irradiance"},
    {0x0087AC9Cull, {0x48, 0x8B, 0xCB, 0xE8, 0x2C, 0x06, 0x00, 0x00}, 8, "GameRender::SubFrameStart: the beauty light"},
    {kRvaDeviceGlobal, {0x48, 0x8B, 0x0D}, 3, "GameRender::SubFrameStart: the render device"},
    {0x0087022Aull, {0xFF, 0xA0, 0x40, 0x02, 0x00, 0x00}, 6, "ApplyAmbient: the device's vfunc +0x240"},
    {0x00870F7Bull, {0x45, 0x33, 0xC0, 0x0F, 0x14, 0xF0, 0x48, 0x8B, 0x01, 0x0F, 0x14, 0xF2, 0x0F, 0x28, 0xDE, 0x41, 0x8D, 0x50, 0x0A,
                     0xFF, 0x90, 0xD0, 0x02, 0x00}, 24, "ApplySecondAmbient: the device's vfunc +0x2D0 (0xA, 0, value)"},
    {0x008703FCull, {0x48, 0x8D, 0x55, 0x87, 0x41, 0xB0, 0x01, 0x48, 0x8B, 0x01, 0xFF, 0x90, 0x58, 0x02, 0x00, 0x00}, 16,
     "ApplyIrradiance: the device's vfunc +0x258 (harmonics, true)"},
    {0x0087024Eull, {0xBB, 0x0A, 0x00, 0x00, 0x00}, 5, "ApplyIrradiance: 10 vectors"},
    {0x0087B451ull, {0x48, 0x8B, 0x03, 0x4C, 0x8D, 0x4C, 0x24, 0x20, 0xBA, 0x06, 0x00, 0x00, 0x00, 0x66, 0x0F, 0x7F, 0x74, 0x24, 0x20},
     19, "UpdateBeautySpecular: global vector 6 (the colour)"},
    {0x0087B464ull, {0x48, 0x8B, 0xCB, 0x44, 0x8D, 0x42, 0xFB, 0xFF, 0x90, 0xC8, 0x02, 0x00, 0x00}, 13,
     "UpdateBeautySpecular: the device's vfunc +0x2C8 (6, 1, the colour)"},
    // ... and the fixed exposure (see kRvaSetupTonemapParams)
    {kRvaSetupTonemapParams, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x80, 0x3D, 0x93, 0x64, 0x49, 0x00, 0x00, 0x48, 0x8B, 0xD9}, 16,
     "BloomRender::SetupTonemapParams"},
    {0x0089554Aull, {0x38, 0x83, 0xD8, 0x00, 0x00, 0x00}, 6, "SetupTonemapParams: the adaptation (+0xD8)"},
    {0x00886E69ull, {0x48, 0x8B, 0xCE, 0xE8, 0x3F, 0xE6, 0x00, 0x00}, 8, "BloomRender::Draw: SetupTonemapParams"},
    // ... the Suite's tone mapping (v0.9.423: see kRvaCalculateLuminance)
    {kRvaCalculateLuminance, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}, 16,
     "BloomRender::CalculateLuminance"},
    {0x00886D09ull, {0xE8, 0x22, 0x92, 0xFF, 0xFF}, 5, "the bloom drawable's Draw: CalculateLuminance"},
    {0x00880128ull, {0xF3, 0x0F, 0x10, 0x87, 0xDC, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x59, 0x05}, 12,
     "CalculateLuminance: AdaptationPerc (+0xDC) x 0.01"},
    {0x00880140ull, {0xF3, 0x0F, 0x10, 0x8F, 0xC4, 0x00, 0x00, 0x00}, 8, "CalculateLuminance: exposure_pre (+0xC4)"},
    {0x0088015Eull, {0xF3, 0x0F, 0x10, 0x8F, 0xC8, 0x00, 0x00, 0x00}, 8, "CalculateLuminance: exposure_post (+0xC8)"},
    {0x008801B6ull, {0x8B, 0x87, 0xE4, 0x00, 0x00, 0x00, 0x89, 0x87, 0x84, 0x01, 0x00, 0x00}, 12,
     "CalculateLuminance: whitePoint (+0xE4 to ToneMapInfo +0x184)"},
    {0x0088064Dull, {0x48, 0x8B, 0xBF, 0x08, 0x02, 0x00, 0x00}, 7, "CalculateLuminance: TonemapPS (+0x208)"},
    // ... no bloom, light scatter or colour grading (v0.9.424: see kBloomScatterEnabled)
    {0x00886E84ull, {0xE8, 0xB7, 0xE0, 0x00, 0x00}, 5, "BloomRender::Draw: SetupBloomParams"},
    {0x00886E8Full, {0xE8, 0x2C, 0xE3, 0x00, 0x00}, 5, "BloomRender::Draw: SetupScatterParams"},
    {0x00894F56ull, {0x80, 0xB9, 0xB4, 0x00, 0x00, 0x00, 0x00}, 7, "SetupBloomParams: bloomData.enabled (+0xB4)"},
    {0x008952EAull, {0x38, 0x83, 0xA0, 0x00, 0x00, 0x00}, 6, "SetupScatterParams: scatterData.enabled (+0xA0)"},
    {kRvaDoColorCorrection, {0x44, 0x88, 0x4C, 0x24, 0x20, 0x44, 0x88, 0x44, 0x24, 0x18, 0x55, 0x53, 0x56, 0x57, 0x41, 0x55, 0x41,
                             0x56, 0x41, 0x57}, 20, "LensEffectRender::DoColorCorrection"},
    {0x0088AED5ull, {0xE8, 0x96, 0x75, 0xFF, 0xFF}, 5, "LensEffectRender::Draw: DoColorCorrection"},
    {0x0088A272ull, {0x48, 0x8D, 0x8F, 0xB0, 0x00, 0x00, 0x00}, 7, "LensEffectRender::Draw: its ColorCorrectionParams (+0xB0)"},
    {0x00882BD1ull, {0xF6, 0x9D, 0xD0, 0x00, 0x00, 0x00}, 6, "DoColorCorrection: the curves' flag (permutation 2)"},
    {0x00882BE1ull, {0x80, 0xBD, 0xD8, 0x00, 0x00, 0x00, 0x00}, 7, "DoColorCorrection: the transform's flag (permutation 1)"},
    {0x00882BEBull, {0x80, 0xBD, 0xE0, 0x00, 0x00, 0x00, 0x00}, 7, "DoColorCorrection: the film grain's flag (permutation 4)"},
    {0x00882C09ull, {0x80, 0xBD, 0xE8, 0x00, 0x00, 0x00, 0x00}, 7, "DoColorCorrection: the LUT's flag (permutation 8)"},
    // ... not drawn (v0.9.425: see kRvaColorCorrectionDone)
    {kRvaColorCorrectionDone, {0xC6, 0x05, 0xCB, 0x10, 0x59, 0x02, 0x01}, 7,
     "LensEffectRender::Draw: the colour correction's \"drawn\" byte set before the call"},
    {0x0088AE6Cull, {0x44, 0x0F, 0xB6, 0x44, 0x24, 0x52, 0x8B, 0x44, 0x24, 0x60, 0x8B, 0x54, 0x24, 0x64, 0x45, 0x84, 0xC0, 0x75,
                     0x15, 0x84}, 20, "LensEffectRender::Draw: no colour correction without its flags"},
    {0x0088AE80ull, {0xC0, 0x75, 0x11, 0x40, 0x38, 0x74, 0x24, 0x53, 0x75, 0x0A, 0x84, 0xD2, 0x75, 0x06, 0x48, 0x39, 0x75, 0x90,
                     0x74, 0x46}, 20, "LensEffectRender::Draw: no colour correction without its flags (to after the call)"},
    {0x0088B628ull, {0xC6, 0x05, 0x71, 0x09, 0x59, 0x02, 0x00}, 7, "OffscreenAlphaRenderNext::Draw: the \"drawn\" byte cleared"},
    {0x0088B64Bull, {0x0F, 0xB6, 0x0D, 0x4E, 0x09, 0x59, 0x02}, 7, "OffscreenAlphaRenderNext::Draw: the \"drawn\" byte read"},
    // ... the Suite's reflections (v0.9.423: see kRvaCubeReflConstantsDraw)
    {kRvaCubeReflConstantsDraw, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55, 0x48,
                                 0x8D, 0x6C, 0x24}, 20, "CubeReflRenderConstantsDrawable::Draw"},
    {0x008888E7ull, {0x4C, 0x8D, 0x46, 0x18}, 4, "CubeReflRenderConstantsDrawable::Draw: its constants (+0x18)"},
    {0x008888F0ull, {0xBA, 0x08, 0x00, 0x00, 0x00}, 5, "CubeReflRenderConstantsDrawable::Draw: 8 rows"},
    // ... the game's ambient occlusion off (v0.9.425: see kRvaSsaoInterpolate)
    {kRvaSsaoInterpolate, {0x48, 0x8B, 0xC4, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x68, 0x0F, 0x29, 0x70, 0xD8, 0x48, 0x8B, 0xD9}, 16,
     "SSAORender's InterpolateParams"},
    {0x00892F35ull, {0x48, 0x8B, 0x90, 0xE8, 0x00, 0x00, 0x00, 0x0F, 0x10, 0x82, 0x40, 0x02, 0x00, 0x00}, 14,
     "SSAORender's InterpolateParams: the unit's dtp::SSAOSystem (DTPNextGenData +0x240)"},
    {0x00892F66ull, {0x80, 0x3B, 0x00, 0x75, 0x07, 0xC7, 0x43, 0x18, 0x00, 0x00, 0x00, 0x00}, 12,
     "SSAORender's InterpolateParams: ssaoEnabled (+0), ssaoStrength (+0x18)"},
    {0x0088C144ull, {0x49, 0x8B, 0xD5, 0x48, 0x8B, 0xF8, 0xE8, 0xB1, 0x6D, 0x00, 0x00, 0xE8, 0x4C, 0xCF, 0xA8, 0xFF, 0x48, 0x8B}, 18,
     "SSAORender::Draw: InterpolateParams, IPCDeviceManagerPtr"},
    {0x0088C156ull, {0xC8, 0x48, 0x8B, 0x10, 0xFF, 0x92, 0xA8, 0x00, 0x00, 0x00, 0x0F, 0xB6, 0x5D, 0xC0, 0x44, 0x39, 0x70, 0x70}, 18,
     "SSAORender::Draw: ssaoEnabled and the occlusion quality (its settings, vfunc +0xA8: +0x70)"},
    {0x0088C124ull, {0x0F, 0x28, 0x05, 0xA5, 0x59, 0x48, 0x00}, 7, "SSAORender::Draw: the bent normal weights when off"},
    {0x00D11AD0ull, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 16,
     "SSAORender::Draw: (0, 1, 0, 0)"},
    {0x0088C1F0ull, {0x44, 0x0F, 0xB6, 0x4D, 0xE2, 0x44, 0x0F, 0xB6, 0x45, 0xE1, 0x0F, 0xB6, 0x55, 0xE0, 0xFF, 0x90, 0xA0, 0x00, 0x00,
                     0x00}, 20, "SSAORender::Draw: the scene's SetColorSSAO (ssaoColor +0x20..0x22)"},
    // ... no wind on the model (v0.9.425: see kRvaGetWindForce)
    {kRvaGetWindForce, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x8B, 0x44, 0x24, 0x60, 0x48, 0x8B, 0xD9, 0x89, 0x44, 0x24, 0x28}, 17,
     "cdc::FXWindMgr::GetWindForce"},
    {0x00277A3Aull, {0x48, 0x8D, 0x79, 0x10}, 4, "DynamicHairDrawable::Draw: its Instance's position (+0x10)"},
    {0x00277A5Cull, {0x48, 0x8D, 0x4D, 0xA0, 0x48, 0x8B, 0xD7, 0xE8, 0x78, 0x35, 0x0B, 0x00}, 12,
     "DynamicHairDrawable::Draw: FXWindMgr::GetWindForce"},
    {0x001766E0ull, {0x48, 0x8D, 0x57, 0x10}, 4, "ClothComponent::Simulate: its instance's position (+0x10)"},
    // ... the strand hair's lights, for the log (v0.9.425: see kEntityFlags)
    {0x00355711ull, {0x8B, 0x82, 0x00, 0x02, 0x00, 0x00}, 6, "Scene::DrawEntity: the entity's flags (+0x200)"},
    {0x0035574Dull, {0x41, 0x8B, 0x40, 0x28}, 4, "Scene::DrawEntity: the node's flags (+0x28)"},
    {0x0035575Bull, {0x4D, 0x8B, 0x40, 0x08}, 4, "Scene::DrawEntity: the node's light set (+8)"},
    {0x00355777ull, {0x41, 0x39, 0x48, 0x0C}, 4, "Scene::DrawEntity: a chunk's count (+0xC)"},
    {0x00355812ull, {0x48, 0x8B, 0x04, 0xC7, 0x48, 0x8B, 0x0C, 0xC8}, 8, "Scene::DrawEntity: a chunk's lights"},
    {0x0035582Eull, {0x48, 0x81, 0xC1, 0xB0, 0x02, 0x00, 0x00}, 7, "Scene::DrawEntity: the SceneLight's ISceneLight (+0x2B0)"},
    // ... the relic viewer's lighting (v0.9.427: see kRvaCollectibleInitLights)
    {kRvaRigObjectRead, {0x48, 0x8B, 0x15, 0x27, 0x0B, 0x4E, 0x02}, 7, "CollectibleRender::InitLights: the global object"},
    {0x0087696Eull, {0x48, 0x8D, 0x82, 0x20, 0x15, 0x00, 0x00}, 7, "CollectibleRender::InitLights: the rig's light 0 (+0x1520)"},
    {0x0087697Full, {0x48, 0x8D, 0x82, 0x50, 0x17, 0x00, 0x00}, 7, "CollectibleRender::InitLights: the rig's light 1 (+0x1750)"},
    {0x0087698Eull, {0x48, 0x8D, 0x82, 0x80, 0x19, 0x00, 0x00}, 7, "CollectibleRender::InitLights: the rig's light 2 (+0x1980)"},
    {0x008769B4ull, {0x48, 0x8D, 0x8D, 0xB8, 0x00, 0x00, 0x00, 0x41, 0xB8, 0x18, 0x02, 0x00, 0x00, 0x48, 0x8D, 0x53, 0x18}, 17,
     "CollectibleRender::InitLights: the light's template (+0x18, 0x218 bytes, to +0xD8)"},
    {0x008769DFull, {0xF3, 0x0F, 0x10, 0x0B, 0x0F, 0x57, 0xD2, 0xF3, 0x0F, 0x10, 0x43, 0x08, 0xF3, 0x0F, 0x10, 0x5B, 0x04}, 17,
     "CollectibleRender::InitLights: the light's rotation (+0)"},
    {0x00876A20ull, {0xE8, 0x3B, 0xCA, 0x96, 0xFF}, 5, "CollectibleRender::InitLights: cdc::Quat::Build"},
    {0x00876A2Eull, {0xE8, 0xDD, 0xC8, 0x96, 0xFF}, 5, "CollectibleRender::InitLights: cdc::Matrix::Build"},
    {0x00876A10ull, {0xF3, 0x0F, 0x10, 0x73, 0x14, 0xF3, 0x0F, 0x10, 0x7B, 0x10, 0xF3, 0x44, 0x0F, 0x10, 0x43, 0x0C}, 16,
     "CollectibleRender::InitLights: the light's position (+0xC)"},
    {0x00876AA0ull, {0xE8, 0x83, 0x3D, 0x45, 0x00, 0x48, 0x8D, 0x55, 0xE0, 0x48, 0x8D, 0x8D, 0xD0, 0x02, 0x00, 0x00, 0xE8, 0x6B,
                     0xB4, 0xFF, 0xFF}, 21, "CollectibleRender::InitLights: the light's properties (retail 0x871F20)"},
    {kRvaRigLightProperties, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x82, 0xE4, 0x00, 0x00, 0x00}, 16,
     "the collectible lights' properties"},
    {0x00871F96ull, {0x48, 0x8B, 0x92, 0x90, 0x01, 0x00, 0x00}, 7, "the collectible lights' properties: the attenuation"},
    {kRvaQuatFromEuler, {0x40, 0x53, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x0D}, 13, "cdc::Quat::Build"},
    {kRvaMatrixFromQuat, {0x48, 0x83, 0xEC, 0x68, 0xF3, 0x0F, 0x10, 0x02, 0x33, 0xC0, 0xF3, 0x0F, 0x10, 0x4A, 0x04}, 15,
     "cdc::Matrix::Build"},
    {kRvaRigOffsetRead, {0x0F, 0x28, 0x15, 0x15, 0xCC, 0x5A, 0x02}, 7, "CollectibleRender: the view's offset"},
    {kRvaRelicPlaceRead, {0x0F, 0x28, 0x35, 0x9D, 0x94, 0x4A, 0x00}, 7, "UiMenuStateCollectibleViewerBase: the relic's place"},
    {0x00877636ull, {0x48, 0x8B, 0x0D, 0x33, 0xFE, 0x4D, 0x02, 0x48, 0x81, 0xC1, 0xD8, 0x14, 0x00, 0x00, 0x49, 0x89, 0x8C, 0x24,
                     0xA0, 0x01, 0x00}, 21, "CollectibleRender::RenderCollectibleView: the rig's bloom (+0x14D8)"},
    {0x008934A6ull, {0x0F, 0x10, 0x00, 0x0F, 0x11, 0x81, 0x80, 0x00}, 8, "BloomRender::InterpolateParams: its given bloom to +0x80"},
    {0x008934C6ull, {0x0F, 0x10, 0x48, 0x30, 0x0F, 0x11, 0x89, 0xB0, 0x00, 0x00, 0x00, 0x8B, 0x40, 0x40, 0x89, 0x81, 0xC0, 0x00,
                     0x00, 0x00}, 20, "BloomRender::InterpolateParams: its given bloom (0x44 bytes)"},
    {kRvaSetApertureAdjustment, {0x48, 0x83, 0xEC, 0x48, 0x48, 0x8B, 0x0D, 0x15, 0xB7, 0x1D, 0x01, 0xBA, 0x01, 0x00, 0x00, 0x00}, 16,
     "RenderManager::SetAperatureAdjustment"},
    {0x008771BBull, {0xE8, 0x60, 0x15, 0x00, 0x00}, 5, "CollectibleRender::RenderCollectibleView: SetAperatureAdjustment"},
    {kRvaRigIrradianceRead, {0x0F, 0x28, 0x05, 0x2E, 0xB2, 0x50, 0x00, 0x41, 0xB0, 0x01, 0x0F, 0x29, 0x85, 0x90}, 14,
     "CollectibleRender::RenderCollectibleView: its irradiance"},
    // ... the reflection lights (v0.9.428: see kComplexityReflection)
    {0x00876AF2ull, {0x83, 0x7B, 0x28, 0x08, 0x75, 0x0B, 0x4A, 0x8B, 0x04, 0x3F, 0x80, 0x88, 0x44, 0x01, 0x00, 0x00, 0x08}, 17,
     "CollectibleRender::InitLights: a refl light's render light (+0x144 bit 8)"},
    {0x00298858ull, {0x41, 0xF6, 0x81, 0x44, 0x01, 0x00, 0x00, 0x08, 0x75, 0x0E}, 10,
     "cdc::LightSet::BuildConstantBuffer: no reflection light"},
    {0x002C2E5Full, {0xF6, 0x80, 0x44, 0x01, 0x00, 0x00, 0x08, 0x74, 0x3A}, 9, "CommonLightManager::SetupLightData: a reflection light"},
    // ... the skin's subsurface scattering (v0.9.430: see kRvaModelViewFlag)
    // (v0.9.541: DebugDrawFlags_GetMdlView's own bytes are RenderTweaks' to check: it hooks the function before the
    // viewer checks, and 0.9.540 found its hook there and turned the Model Viewer off)
    {kRvaModelViewFlagCall, {0xE8, 0x44, 0x39, 0xB6, 0xFF, 0x85, 0xC0, 0x75, 0x17}, 9,
     "GameRender::BeginScene: the subsurface scattering with the model view flag"},
    {0x00871260ull, {0x48, 0x8B, 0x0D, 0x71, 0x67, 0x4E, 0x02, 0x48, 0x85, 0xC9, 0x74, 0x23, 0x33, 0xD2, 0xE8, 0xBD, 0x8A, 0x8F, 0xFF,
                     0x84, 0xC0, 0x74}, 22, "GameRender::BeginScene: ... or while a cinematic plays"},
    {0x00871277ull, {0x48, 0x8B, 0x06, 0x48, 0x8B, 0xCE, 0xFF, 0x50, 0x68, 0x49, 0x8B, 0x8F, 0x08, 0xCE, 0x00, 0x00, 0x48, 0x8B, 0xD0,
                     0xE8, 0xD1, 0xB5, 0x01, 0x00}, 24, "GameRender::BeginScene: SSSRender::Draw"},
    {kRvaSssDraw, {0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x57, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0x60, 0xFE, 0xFF, 0xFF}, 17,
     "SSSRender::Draw"},
    {kRvaSssToggleRead, {0x80, 0x3D, 0x4E, 0x4F, 0x82, 0x00, 0x00}, 7, "SSSRender::Draw: the cutscenes' toggle"},
    // ... no motion blur (v0.9.445: see kRvaMotionBlurRead)
    {kRvaMotionBlurRead, {0x40, 0x38, 0x35, 0xD3, 0x75, 0x73, 0x00, 0x74, 0x0D}, 9,
     "LensEffectRender::Draw: cdc::g_globalMotionBlurEnabled"},
    {kRvaMotionBlurClear, {0xC6, 0x05, 0xC7, 0xB3, 0x87, 0x00, 0x00}, 7, "AGSetMotionBlur::ProcessMsg: off"},
    {0x00732CDBull, {0xC6, 0x05, 0xEB, 0xEA, 0x97, 0x00, 0x01}, 7, "AGToggleSubSurfaceScatteringNode::ProcessMsg: on"},
    {0x00732CF9ull, {0x88, 0x05, 0xCE, 0xEA, 0x97, 0x00}, 6, "AGToggleSubSurfaceScatteringNode::ProcessMsg: off"},
    // ... the Suite's look (v0.9.423: see SuiteLook and WornHair)
    {0x0075E26Aull, {0xE8, 0xA1, 0x03, 0xC8, 0xFF, 0x48, 0x8B, 0xB8, 0xA0, 0x00, 0x00, 0x00}, 12,
     "AGInstanceDrawGroups::ProcessMsg: the DrawableComponent's default drawable (+0xA0)"},
    {0x0075E298ull, {0x48, 0x8B, 0x87, 0xA8, 0x00, 0x00, 0x00, 0x48, 0xF7, 0xD6, 0x48, 0x21, 0x70, 0x10}, 14,
     "AGInstanceDrawGroups::ProcessMsg: its settings' (+0xA8) m_noDrawGroups (+0x10)"},
    {0x003CFD84ull, {0xBF, 0x01, 0x00, 0x00, 0x00}, 5, "cdc::ApplyDrawGroupMask: bit k, draw group k + 1"},
    {0x002266D0ull, {0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0xB8}, 12,
     "DynamicHairComponentManager::AddComponent"},
    {0x00226725ull, {0x48, 0x89, 0x73, 0x50}, 4, "DynamicHairComponentManager::AddComponent: the data (+0x50)"},
    {0x00226783ull, {0x8B, 0x56, 0x04, 0x85, 0xD2}, 5, "DynamicHairComponentManager::AddComponent: the hair object (data +4)"},
    {0x0024C8E6ull, {0x48, 0x8B, 0x79, 0x50, 0x48, 0x83, 0xC7, 0x04}, 8, "DynamicHairComponent::SetHairModel: the data's hair (+0x50, +4)"},
    {0x00893977ull, {0xC6, 0x87, 0xD8, 0x00, 0x00, 0x00, 0x01}, 7, "BloomRender::InterpolateParams: the adaptation (+0xD8)"},
    {0x00893A4Eull, {0xF3, 0x0F, 0x10, 0x87, 0xC4, 0x00, 0x00, 0x00}, 8, "BloomRender::InterpolateParams: exposure_pre (+0xC4)"},
    {0x00893A70ull, {0xF3, 0x0F, 0x10, 0x87, 0xC8, 0x00, 0x00, 0x00}, 8, "BloomRender::InterpolateParams: exposure_post (+0xC8)"},
    // ... the components (see kRvaGetComponent)
    {kRvaGetComponent, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x4C, 0x8B, 0x41, 0x10, 0x45, 0x33, 0xD2, 0x48, 0x8B, 0xD9, 0x4D, 0x85, 0xC0,
                        0x74, 0x2C}, 20, "ObjectZoneRef::GetComponent"},
    {kRvaDrawableHashRead, {0x8B, 0x15, 0x4E, 0xFB, 0x6B, 0x01, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x08}, 12,
     "Instance::InitComponents_Object: the drawable's data"},
    // ... the makers (see kRvaComponentMakers)
    {0x003E1B1Aull, {0xE9, 0x11, 0xAF, 0xE4, 0xFF}, 5, "InitComponents_Object: its end (0x22CA30)"},
    {0x0022CA4Aull, {0xE8, 0x71, 0x54, 0xFF, 0xFF}, 5, "0x22CA30: the makers' loop (0x221EC0)"},
    {0x00221EE3ull, {0x4D, 0x8B, 0xA5, 0x80, 0x00, 0x00, 0x00}, 7, "the makers' loop: the instance's ObjectZoneRef (+0x80)"},
    {0x00221F40ull, {0x4D, 0x8B, 0xB7, 0x98, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x0D, 0x0A, 0xED, 0x82, 0x01, 0x4C, 0x03, 0xF3, 0x41,
                     0x8B, 0x16, 0x49, 0x8B, 0x6E, 0x08}, 24, "the makers' loop: the placement's components, the makers' map"},
    {0x00221F5Dull, {0x4C, 0x8B, 0x00, 0x4D, 0x85, 0xC0, 0x74, 0x0D, 0x48, 0x8B, 0xD5, 0x49, 0x8B, 0xCD, 0x41, 0xFF, 0xD0}, 17,
     "the makers' loop: a placement component's maker (instance, data)"},
    {0x00222012ull, {0x48, 0x8B, 0x7C, 0x37, 0x08, 0x8B, 0xD1, 0x48, 0x8D, 0x0D, 0x38, 0xEC, 0x82, 0x01}, 14,
     "the makers' loop: the zone's components, the makers' map"},
    {0x00222025ull, {0x4C, 0x8B, 0x00, 0x4D, 0x85, 0xC0, 0x74, 0x09, 0x48, 0x8B, 0xD7, 0x49, 0x8B, 0xCD, 0x41, 0xFF, 0xD0}, 17,
     "the makers' loop: a zone component's maker (instance, data)"},
    {0x0022515Full, {0x48, 0x8B, 0x49, 0x10, 0x48, 0x85, 0xC9, 0x74, 0x25, 0x33, 0xD2, 0x8B, 0xC7, 0x48, 0xF7, 0x73, 0x18, 0x48,
                     0x8B, 0x03, 0x48, 0x8B, 0x04, 0xD0}, 24, "the hash map: its count, buckets"},
    {0x00225177ull, {0x48, 0x85, 0xC0, 0x74, 0x11, 0x0F, 0x1F, 0x40, 0x00, 0x39, 0x78, 0x08, 0x74, 0x79, 0x48, 0x8B}, 16,
     "the hash map: a node's hash (+8), next (+0)"},
    {0x002251FEull, {0x48, 0x85, 0xC0, 0x74, 0x8A, 0x48, 0x83, 0xC0, 0x10}, 9, "the hash map: a node's value (+0x10)"},
    {0x002295D0ull, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8D, 0x0D, 0x78, 0x76, 0x82, 0x01}, 16,
     "the makers' registration"},
    {0x002295B0ull, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8D, 0x0D, 0x48, 0x76, 0x82, 0x01}, 16,
     "the registration of 0x1A50C08"},
    {0x002295F0ull, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8D, 0x0D, 0x80, 0x76, 0x82, 0x01}, 16,
     "the registration of 0x1A50C80"},
    {0x00229610ull, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8D, 0x0D, 0x10, 0x76, 0x82, 0x01}, 16,
     "the registration of 0x1A50C30"},
    {0x00229650ull, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8D, 0x0D, 0x48, 0x76, 0x82, 0x01}, 16,
     "the removals' registration (0x1A50CA8)"},
    {0x00221D23ull, {0x4C, 0x8B, 0xF9}, 3, "the loop of 0x1A50C08: the instance"},
    {0x00221D87ull, {0x48, 0x8D, 0x0D, 0x7A, 0xEE, 0x82, 0x01}, 7, "the loop of 0x1A50C08: the placement's"},
    {0x00221D99ull, {0x48, 0x8B, 0x10, 0x48, 0x85, 0xD2, 0x74, 0x09, 0x49, 0x8B, 0xCF, 0xFF, 0xD2}, 13,
     "the loop of 0x1A50C08: call(instance)"},
    {0x00221E44ull, {0x48, 0x8D, 0x0D, 0xBD, 0xED, 0x82, 0x01}, 7, "the loop of 0x1A50C08: the zone's"},
    {0x00221E50ull, {0x48, 0x8B, 0x10, 0x48, 0x85, 0xD2, 0x74, 0x05, 0x49, 0x8B, 0xCF, 0xFF, 0xD2}, 13,
     "the loop of 0x1A50C08: call(instance)"},
    {0x002220B3ull, {0x4C, 0x8B, 0xF9}, 3, "the loop of 0x1A50C80: the instance"},
    {0x00222117ull, {0x48, 0x8D, 0x0D, 0x62, 0xEB, 0x82, 0x01}, 7, "the loop of 0x1A50C80: the placement's"},
    {0x00222129ull, {0x48, 0x8B, 0x10, 0x48, 0x85, 0xD2, 0x74, 0x09, 0x49, 0x8B, 0xCF, 0xFF, 0xD2}, 13,
     "the loop of 0x1A50C80: call(instance)"},
    {0x002221D4ull, {0x48, 0x8D, 0x0D, 0xA5, 0xEA, 0x82, 0x01}, 7, "the loop of 0x1A50C80: the zone's"},
    {0x002221E0ull, {0x48, 0x8B, 0x10, 0x48, 0x85, 0xD2, 0x74, 0x05, 0x49, 0x8B, 0xCF, 0xFF, 0xD2}, 13,
     "the loop of 0x1A50C80: call(instance)"},
    {0x00222263ull, {0x4C, 0x8B, 0xF9}, 3, "the loop of 0x1A50C30: the instance"},
    {0x002222C7ull, {0x48, 0x8D, 0x0D, 0x62, 0xE9, 0x82, 0x01}, 7, "the loop of 0x1A50C30: the placement's"},
    {0x002222D9ull, {0x48, 0x8B, 0x10, 0x48, 0x85, 0xD2, 0x74, 0x09, 0x49, 0x8B, 0xCF, 0xFF, 0xD2}, 13,
     "the loop of 0x1A50C30: call(instance)"},
    {0x00222384ull, {0x48, 0x8D, 0x0D, 0xA5, 0xE8, 0x82, 0x01}, 7, "the loop of 0x1A50C30: the zone's"},
    {0x00222390ull, {0x48, 0x8B, 0x10, 0x48, 0x85, 0xD2, 0x74, 0x05, 0x49, 0x8B, 0xCF, 0xFF, 0xD2}, 13,
     "the loop of 0x1A50C30: call(instance)"},
    {0x00222413ull, {0x4C, 0x8B, 0xF9}, 3, "the removals' loop: the instance"},
    {0x00222477ull, {0x48, 0x8D, 0x0D, 0x2A, 0xE8, 0x82, 0x01}, 7, "the removals' loop: the placement's"},
    {0x00222489ull, {0x48, 0x8B, 0x10, 0x48, 0x85, 0xD2, 0x74, 0x09, 0x49, 0x8B, 0xCF, 0xFF, 0xD2}, 13,
     "the removals' loop: call(instance)"},
    {0x00222534ull, {0x48, 0x8D, 0x0D, 0x6D, 0xE7, 0x82, 0x01}, 7, "the removals' loop: the zone's"},
    {0x00222540ull, {0x48, 0x8B, 0x10, 0x48, 0x85, 0xD2, 0x74, 0x05, 0x49, 0x8B, 0xCF, 0xFF, 0xD2}, 13,
     "the removals' loop: call(instance)"},
    {0x0022D5B8ull, {0xE9, 0x43, 0x4E, 0xFF, 0xFF}, 5, "0x22D570: the removals' loop (0x222400)"},
    {0x003E7F3Eull, {0xE8, 0x2D, 0x56, 0xE4, 0xFF}, 5, "the instance's removal: 0x22D570"},
    // ... the item lists (see kRvaFindItemList)
    {kRvaFindItemList, {0x48, 0x8B, 0x41, 0x08, 0x45, 0x33, 0xC0, 0x44, 0x8B, 0xD2, 0x48, 0x8B, 0x48, 0x58, 0x44, 0x8B, 0x49, 0x38,
                        0x41, 0x83, 0xE9, 0x01, 0x4C, 0x8B}, 24, "the object data's item lists (zone data +0x58: +0x38, +0x40)"},
    {kRvaActionGraphsHashRead, {0x8B, 0x15, 0x39, 0xFE, 0x6C, 0x01, 0x48, 0x8B, 0x8B, 0x80, 0x00, 0x00, 0x00}, 13,
     "the action graphs' births: the hash, the instance's ObjectZoneRef"},
    {0x003D0A2Aull, {0xE8, 0xD1, 0x4D, 0xE6, 0xFF}, 5, "the action graphs' births: the item list"},
    {0x003DCB43ull, {0xE8, 0x88, 0x3E, 0xFF, 0xFF}, 5, "Instance::Finalize: the action graphs' births"},
    {kRvaObjectFlags, {0x48, 0x8B, 0x51, 0x08, 0x8B, 0x05, 0x82, 0xAE, 0x86, 0x01, 0x39, 0x42, 0x54, 0x75, 0x0D, 0x48, 0x8B, 0x42,
                       0x58, 0x48, 0x8B, 0x40, 0x20, 0x48}, 24, "the object's flags (zone data +0x58 -> +0x20 -> +0x44)"},
    {0x007374D2ull, {0xE8, 0xF9, 0xE2, 0xAF, 0xFF, 0xF7, 0x00, 0x00, 0x00, 0x02, 0x00, 0x0F, 0x84, 0xE5, 0x00, 0x00, 0x00, 0x48,
                     0x8B, 0x87, 0xD8, 0x00, 0x00, 0x00}, 24, "AGGetPlayerInfo: the player's flag, then the instance's components"},
    {0x003EF8DCull, {0xE8, 0xEF, 0x5E, 0xE4, 0xFF, 0xF7, 0x00, 0x00, 0x00, 0x02, 0x00, 0x74, 0x27, 0x8B, 0x0D, 0xA1, 0xB1, 0xD0,
                     0x00}, 19, "the player's flag, then the PlayerComponent (its array's id at 0x10FAA90)"},
    // ... the bounds (see kEntityWorldVolume)
    {kRvaQueryVolume, {0x40, 0x53, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x89, 0x00, 0x01, 0x00,
                       0x00}, 19, "SceneEntity::QueryVolumeFromDrawable"},
    {0x00372A2Full, {0xC7, 0x83, 0x80, 0x01, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0xC7, 0x83, 0xE0, 0x01, 0x00, 0x00, 0x09}, 17,
     "SceneEntity::QueryVolumeFromDrawable: the volumes' types (+0x180, +0x1E0)"},
    {kRvaTransformVolume, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8D, 0x91, 0x30, 0x01, 0x00, 0x00, 0x48,
                           0x8B, 0xF9, 0x48, 0x8D, 0x99, 0x90}, 24, "SceneEntity::TransformVolumeAndPivot"},
    // ... the lists (see kRvaCraftingRead)
    {kRvaCraftingRead, {0x48, 0x69, 0xD8, 0xA8, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05, 0x9A, 0x44, 0x90, 0x02, 0x48, 0x03, 0x58, 0x38}, 18,
     "Crafting::HasDLCForOutfit: the outfits"},
    {0x004431B0ull, {0x4C, 0x8B, 0x15, 0x79, 0x42, 0x91, 0x02, 0x33, 0xC0, 0x45, 0x8B, 0x4A, 0x30}, 13, "the outfits' count"},
    {kRvaUiInfoRead, {0x48, 0x8B, 0x2D, 0xD0, 0x78, 0x53, 0x02}, 7, "the gear screen: GlobalUIInfo"},
    {0x008E6D5Bull, {0x41, 0x39, 0x98, 0x60, 0x10, 0x00, 0x00}, 7, "the weapons' count"},
    {0x008E6D66ull, {0x48, 0x8D, 0x14, 0x9B, 0x48, 0xC1, 0xE2, 0x04, 0x49, 0x03, 0x90, 0x68, 0x10, 0x00, 0x00}, 15, "the weapons"},
    {0x008E6EFFull, {0x41, 0x0F, 0xB6, 0x44, 0xCB, 0x44}, 6, "a weapon's broken flag (+0x44)"},
    {0x0093CA34ull, {0x0F, 0xB7, 0x50, 0x06}, 4, "a weapon's menu model (+6)"},
    {0x009BA4FEull, {0x0F, 0xBF, 0x4F, 0x08}, 4, "a weapon's name (+8)"},
    {0x00447D80ull, {0x41, 0x8B, 0xB8, 0x00, 0x04, 0x00, 0x00}, 7, "the collectible sets' count (+0x400)"},
    {0x00447D8Eull, {0x4D, 0x8B, 0x98, 0xF8, 0x03, 0x00, 0x00}, 7, "the collectible sets (+0x3F8)"},
    {0x00448034ull, {0x48, 0x8B, 0x80, 0xF8, 0x03, 0x00, 0x00}, 7, "the collectible sets (+0x3F8, the getter)"},
    {0x00446FC2ull, {0x48, 0x6B, 0xD1, 0x78, 0x48, 0x69, 0xC3, 0xE8, 0x00, 0x00, 0x00, 0x4A, 0x03, 0x44, 0x02, 0x68}, 16,
     "a set's collectibles"},
    {0x00447E75ull, {0x48, 0x6B, 0xC0, 0x78, 0x42, 0x8B, 0x54, 0x08, 0x70}, 9, "a set's count"},
    {0x00447E82ull, {0x4A, 0x8B, 0x44, 0x08, 0x68}, 5, "a set's collectibles (+0x68)"},
    {0x00447E96ull, {0x48, 0x05, 0xE8, 0x00, 0x00, 0x00}, 6, "a collectible's size (0xE8)"},
    {0x00908ED2ull, {0x44, 0x0F, 0xB7, 0x70, 0x0E}, 5, "a relic's object (+0xE)"},
    // (v0.9.431) RewardsCollectibleUIMessengerNode::ProcessMsg: SOUND_Play (retail 0x3B8540) with the collectible's
    // m_audioClip (+0x14, a sound DTP: the line the game plays when the relic is found)
    {0x00728E8Eull, {0x41, 0x8B, 0x56, 0x14, 0xE8, 0xA9, 0xF6, 0xC8, 0xFF}, 9, "a relic's line (+0x14, SOUND_Play)"},
};
// ---- The weapons' upgrades (v0.9.438: the grid's ALL UPGRADES) ----
// The game's weapon upgrade screen (UiMenuStateCampsiteWeaponUpgrades, TR2 PDB names) lists a weapon's upgrades from
// globalweaponupgrades.drm's data ([0x2D58868], set by MAIN_PostGameloopInit, retail 0x458DC2): +0 the weapons' count,
// +8 their records (0x20 each: +0 the weapon's object, its WeaponUIInfo m_objectID, as DetermineWhatWeaponToUpgrade...
// compares them, retail 0x8E6F67; +0x10 the upgrades' count, +0x18 the upgrades: pointers to {u16 the upgrade's trait,
// u16 its preview trait}); PopulateSkills (retail 0x93A520) shows the ones whose dtp::PlayerTrait (PlayerTraits::GetTrait,
// retail 0x44D5A0: GlobalAchievements [0x2D574A0] +0x20 count, +0x28 the traits, 0x1C0 each) has m_trait (+8) 4 (a
// weapon upgrade), m_isWeaponAttachment (+0x2B) 0 and m_hidden (+0x28) 0 or owned. What an upgrade shows on the weapon's
// model (the menu's, its m_uiObjectID, the one shown here) the trait says (TR2 PDB dtp::PlayerTrait; the offsets are not
// read by retail code, whose screen asks the model's script through the player's traits, TR2's "UI/Use Traits For UI
// Weapon Upgrades" on: they were checked on the game's own data, globalachievements.drm DTP 193647, 969 traits, their
// debug names at +0x188): m_drawGroupFlags +0x150 / m_drawGroupFlagsHide +0x154 (draw groups 1-32: bit k group k + 1)
// with m_drawGroupFlagsExtended +0x158 / +0x15C (33-64), m_gatedDrawGroups +0x178 (count +0x180; dtp::GatedDrawGroup,
// 0x28: +0 its conditions {u16 trait, u32 "not owned"} 8 bytes each, +8 their count, then the same four flags at +0xC,
// +0x10, +0x14, +0x18). TR2's UiMenuStateCampsiteWeaponUpgrades::ApplyTraitUpgradesToDrawGroupBits (0x14091BEE0) adds
// an owned upgrade's flags and those of its gated groups whose conditions all hold; ResetDrawGroupsUsingDTP
// (0x140989F80) writes m_noDrawGroups = ~shown | hidden.
// - ALL UPGRADES: every upgrade the weapon lists owned (the hidden ones too), as that.
// - Without: the game has no look with no upgrade (its formula hides every group then), but each upgrade's own look (that
//   one alone owned, as the screen previews it: DrawGroupUsingDTP, 0x14092B7D0) keeps the weapon's base parts (the bows'
//   groups 1 to 4 in 18 or 19 of their 24 upgrades' looks, an upgrade's own part in 1 to 5): the groups more than half
//   of those looks show (the hidden ones, the story's arrows, left out). None: the weapon as it is (every group).
constexpr std::uintptr_t kRvaWeaponUpgradesRead = 0x008E6F40ull, kRvaGetTrait = 0x0044D5A0ull;
const Check kUpgradeChecks[] = {
    {kRvaWeaponUpgradesRead, {0x48, 0x8B, 0x0D, 0x21, 0x19, 0x47, 0x02, 0x83, 0x39, 0x00, 0x76, 0x37}, 12,
     "the weapon upgrades' count (+0)"},
    {0x008E6F57ull, {0x48, 0x8B, 0x49, 0x08, 0x41, 0x8B, 0xD0, 0x41, 0x0F, 0xB7, 0x42, 0x04, 0x48, 0xC1, 0xE2, 0x05, 0x66, 0x39,
                     0x04, 0x0A}, 20, "a weapon's record (+8, 0x20 each: +0 the weapon's object)"},
    {0x0093A600ull, {0x48, 0x8B, 0x05, 0x61, 0xE2, 0x41, 0x02, 0x8B, 0x97, 0x00, 0x02, 0x00, 0x00, 0x48, 0xC1, 0xE2, 0x05, 0x48,
                     0x8B, 0x48, 0x08, 0x83, 0x7C, 0x0A}, 24, "PopulateSkills: a weapon's upgrades' count (+0x10)"},
    {0x0093A5D3ull, {0x4C, 0x8B, 0x6C, 0x0A, 0x18}, 5, "PopulateSkills: a weapon's upgrades (+0x18)"},
    {0x0093A62Dull, {0x0F, 0xB7, 0x08, 0xE8, 0x6B, 0x2F, 0xB1, 0xFF, 0x83, 0x78, 0x08, 0x04, 0x75, 0x3C, 0x80, 0x78, 0x2B, 0x00,
                     0x75, 0x36, 0x44, 0x39, 0x70, 0x34}, 24, "PopulateSkills: an upgrade's trait (m_trait 4, not an attachment)"},
    {0x0093A647ull, {0x80, 0x78, 0x28, 0x00}, 4, "PopulateSkills: m_hidden (+0x28)"},
    {kRvaGetTrait, {0x48, 0x8B, 0x15, 0xF9, 0x9E, 0x90, 0x02, 0x48, 0x85, 0xD2, 0x74, 0x18, 0x85, 0xC9, 0x78, 0x14, 0x3B, 0x4A, 0x20,
                    0x73, 0x0F, 0x48, 0x63, 0xC1}, 24, "PlayerTraits::GetTrait (the count, +0x20)"},
    {0x0044D5B8ull, {0x48, 0x69, 0xC0, 0xC0, 0x01, 0x00, 0x00, 0x48, 0x03, 0x42, 0x28, 0xC3}, 12,
     "PlayerTraits::GetTrait (0x1C0 each, +0x28)"},
};
constexpr std::size_t kUpgradeCount = 0x00, kUpgradeRecords = 0x08, kUpgradeRecordSize = 0x20, kRecordObject = 0x00,
                      kRecordUpgradeCount = 0x10, kRecordUpgrades = 0x18;
constexpr std::size_t kTraitCount = 0x20, kTraits = 0x28, kTraitSize = 0x1C0, kTraitType = 0x08, kTraitHidden = 0x28,
                      kTraitAttachment = 0x2B, kTraitShow = 0x150, kTraitHide = 0x154, kTraitShowHigh = 0x158,
                      kTraitHideHigh = 0x15C, kTraitGated = 0x178, kTraitGatedCount = 0x180, kGatedSize = 0x28,
                      kGatedConditions = 0x00, kGatedConditionCount = 0x08, kGatedShow = 0x0C, kGatedHide = 0x10,
                      kGatedShowHigh = 0x14, kGatedHideHigh = 0x18, kConditionSize = 0x08;
constexpr std::int32_t kTraitWeaponUpgrade = 4;
std::uintptr_t g_weaponUpgradesGlobal = 0, g_traitsGlobal = 0;   // (Check: 0 when its code was not as expected)
// the instance (features.cpp's offsets): +0x10 its position, +0x68 the component id, +0x118 the slot
constexpr std::size_t kInstancePosition = 0x10, kInstanceComponentId = 0x68, kInstanceSlotId = 0x118;
// cdc Segment (0x50): +0x20 pivot (the bind pose's translation, from the parent), +0x38 parent
constexpr std::size_t kSegmentStride = 0x50, kSegmentPivot = 0x20, kSegmentParent = 0x38;
// cdc AnimSegment (0x40, the pose AnimBuildTransforms turns into matrices): +0 rotation (quaternion log), +0x10
// translation, +0x20 scale, +0x30 weight (cdc::AnimSegments_SetBindPose)
constexpr std::size_t kAnimSegmentStride = 0x40;
// (v0.9.603) a skeleton's segments at most: the framework's bone limit (bone_patch.cpp kMaxModelBones). 4096 before:
// a model of more bones had no size from its bones (SkeletonOf) and no bind pose (OnBuildTransforms). Only a check on
// the count the game gives: what is read and written is sized by it (the game's own segment buffers, MeasureModel's).
constexpr std::uint32_t kMaxSegments = 8191;

int g_checked = 0;   // 0 not yet, 1 ready, -1 not where expected
std::uintptr_t g_base = 0;
std::uint8_t** g_zoneMap = nullptr;
const std::uint32_t* g_playerHash = nullptr;
const std::uint32_t* g_drawableHash = nullptr;
const std::uint32_t* g_actionGraphsHash = nullptr;
bool g_getComponentHooked = false;
std::uintptr_t g_craftingGlobal = 0, g_uiInfoGlobal = 0;   // (Check: the lists' globals, by the code reading them)

// ---- The models (v0.9.418): made each time the viewer opens, in this order: the game's outfits (Lara's, the custom
// outfits' slots among them), our characters and animals (by object, named as the author named them), the game's
// weapons and relics (see kRvaCraftingRead). On the view Left / Right (d-pad) go through a category's models,
// PgUp / PgDn through the categories (each keeps its place). The lists are written to
// <game>\ROTTRFramework\model_viewer\models.json (the Modding Suite makes the grid's pictures of them).
struct Model {
    std::string object, name, description;
    const std::uint8_t* outfit = nullptr;   // the outfit's dtp::OutfitsData (Outfits: its campsite picture)
    std::uint32_t voice = 0;                // a relic's line: its CollectibleItem's m_audioClip, a sound DTP (v0.9.431)
    std::uint16_t weaponObject = 0;         // a weapon's WeaponUIInfo m_objectID (its upgrades' record, v0.9.438)
};
struct Category {
    std::string name;
    std::vector<Model> models;
    int index = 0;              // the one shown last
    Kind kind = Kind::Outfits;  // (the relics lit as the relic viewer lights them, v0.9.430; the weapons and the relics
                                // turning on the grid, the characters and the animals without the view's text, v0.9.431)
};
std::vector<Category> g_categories;
int g_category = 0;
// (v0.9.447) The characters' and the animals' names in the game's text language: one per language (en, fr, de, it,
// es-mx, es, ja, pt, pl, ru, nl, ko, cht, chs, ar: ViewerText::Pick), the game's own words where it has them (its
// London, Remnant, Witch, jacket, deer, squirrel, lynx, snow leopard, wolf, bear, boar, crow); a character's own name
// kept as it is, the rest of it translated. Only characters the game's own text in that language uses (what its font
// is known to draw), so a bird whose name needs another one has a plainer word there (Chinese hawk, songbird, wag-tail
// bird; Korean 매); the Arabic in its reading order (made into the game's form when picked).
struct Named {
    const char* object;
    const char* name[ViewerText::kLanguageCount];
};
const Named kCharacters[] = {
    {"jonah_cine",
     {"Jonah Snow", "Jonah (Neige)", "Jonah (Schnee)", "Jonah (Neve)", "Jonah (Nieve)", "Jonah (Nieve)", "Jonah（雪）",
      "Jonah (Neve)", "Jonah (Śnieg)", "Jonah (Снег)", "Jonah (Sneeuw)", "Jonah (설원)", "Jonah（雪）", "Jonah（雪）",
      "Jonah في الثلج"}},
    {"jonah_london_cine",
     {"Jonah London", "Jonah (Londres)", "Jonah (London)", "Jonah (Londra)", "Jonah (Londres)", "Jonah (Londres)",
      "Jonah（ロンドン）", "Jonah (Londres)", "Jonah (Londyn)", "Jonah (Лондон)", "Jonah (Londen)", "Jonah (런던)",
      "Jonah（倫敦）", "Jonah（伦敦）", "Jonah في لندن"}},
    {"ana_cine", {"Ana"}},
    {"konstantin_combat_cine", {"Konstantin"}},
    {"konstantin_jacket_cine",
     {"Konstantin Jacket", "Konstantin (Veste)", "Konstantin (Jacke)", "Konstantin (Giacca)", "Konstantin (Chaqueta)",
      "Konstantin (Chaqueta)", "Konstantin（ジャケット）", "Konstantin (Jaqueta)", "Konstantin (Kurtka)",
      "Konstantin (Куртка)", "Konstantin (Jas)", "Konstantin (재킷)", "Konstantin（夾克）", "Konstantin（夹克）",
      "Konstantin بالسترة"}},
    {"jacob_cine", {"Jacob"}},
    {"sofia_cine", {"Sofia"}},
    {"syrian_driver",
     {"Turkish Guide", "Guide turc", "Türkischer Fremdenführer", "Guida turca", "Guía turco", "Guía turco",
      "トルコ人ガイド", "Guia turco", "Turecki przewodnik", "Турецкий проводник", "Turkse gids", "터키인 가이드",
      "土耳其嚮導", "土耳其向导", "المرشد التركي"}},
    {"lord_croft_cine", {"Richard Croft"}},
    {"dlc_nadia_cine", {"Nadia"}},
    {"dlc_by_ivan_puppet_parent", {"Ivan"}},
    {"witchcup_witchywitch_cine",
     {"Serafima \"The Witch\"", "Serafima « La Sorcière »", "Serafima „Die Hexe“", "Serafima \"La Strega\"",
      "Serafima \"La Bruja\"", "Serafima \"La Bruja\"", "Serafima「魔女」", "Serafima \"A Bruxa\"",
      "Serafima \"Wiedźma\"", "Serafima «Ведьма»", "Serafima \"De Heks\"", "Serafima \"마녀\"", "Serafima「女巫」",
      "Serafima“女巫”", "Serafima \"الساحرة\""}},
    {"trinity_agent", {"Kennard Montez"}},
    {"remnant_warrior_cine",
     {"Remnant Warrior", "Guerrier rescapé", "Verbliebenen-Krieger", "Guerriero dei Discendenti",
      "Guerrero del Remanente", "Guerrero del Resto", "谷の戦士", "Guerreiro Remanescente", "Wojownik Ocalałych",
      "Воин Потомков", "Krijger van de volgelingen", "원주민 전사", "遺民戰士", "遗民战士", "محارب البقايا"}},
};
// (v0.9.436) The game's relics the list leaves out: relic_cm_projector_slides (it has no name in the game's text).
const char* const kRelicsLeftOut[] = {"relic_cm_projector_slides"};
const Named kAnimals[] = {
    {"trx_alpha_red_deer",
     {"Alpha Deer", "Cerf alpha", "Alpha-Hirsch", "Cervo alfa", "Ciervo alfa", "Ciervo alfa", "アルファのシカ",
      "Cervo alfa", "Jeleń alfa", "Олень-вожак", "Alfahert", "우두머리 사슴", "頭鹿", "头鹿", "أيل قائد"}},
    {"trx_female_red_deer",
     {"Female Deer", "Biche", "Hirschkuh", "Cerva", "Cierva", "Cierva", "雌ジカ", "Corça", "Łania", "Самка оленя",
      "Hinde", "암사슴", "母鹿", "母鹿", "أنثى الأيل"}},
    {"trx_male_red_deer",
     {"Male Deer", "Cerf", "Hirsch", "Cervo", "Ciervo", "Ciervo", "雄ジカ", "Cervo macho", "Jeleń", "Самец оленя",
      "Hert", "수사슴", "雄鹿", "雄鹿", "ذكر الأيل"}},
    {"trx_siberian_red_squirrel",
     {"Squirrel", "Écureuil", "Eichhörnchen", "Scoiattolo", "Ardilla", "Ardilla", "リス", "Esquilo", "Wiewiórka",
      "Белка", "Eekhoorn", "다람쥐", "松鼠", "松鼠", "سنجاب"}},
    {"trx_arctic_hare",
     {"Arctic Hare", "Lièvre arctique", "Polarhase", "Lepre artica", "Liebre ártica", "Liebre ártica",
      "ホッキョクウサギ", "Lebre-ártica", "Zając polarny", "Полярный заяц", "Poolhaas", "북극토끼", "北極兔", "北极兔",
      "أرنب قطبي"}},
    {"trx_mountain_hare",
     {"Mountain Hare", "Lièvre variable", "Schneehase", "Lepre variabile", "Liebre de montaña", "Liebre de montaña",
      "ユキウサギ", "Lebre-da-montanha", "Zając bielak", "Заяц-беляк", "Sneeuwhaas", "산토끼", "雪兔", "雪兔",
      "أرنب جبلي"}},
    {"trx_jackalope",
     {"Jackalope", "Jackalope", "Jackalope", "Jackalope", "Jackalope", "Jackalope", "ジャッカロープ", "Jackalope",
      "Jackalope", "Джекалоп", "Jackalope", "재칼로프", "鹿角兔", "鹿角兔", "جاكالوب"}},
    {"bear_trx_oms",
     {"Bear", "Ours", "Bär", "Orso", "Oso", "Oso", "クマ", "Urso", "Niedźwiedź", "Медведь", "Beer", "곰", "熊", "熊",
      "دب"}},
    {"trx_razor_back_boar",
     {"Back Boar", "Sanglier razorback", "Razorback-Wildschwein", "Cinghiale razorback", "Jabalí razorback",
      "Jabalí razorback", "レイザーバックイノシシ", "Javali razorback", "Dzik razorback", "Остроспинный кабан",
      "Razorback-zwijn", "레이저백 멧돼지", "刀背野豬", "刀背野猪", "خنزير بري حاد الظهر"}},
    {"trx_russian_boar",
     {"Russian Boar", "Sanglier russe", "Russisches Wildschwein", "Cinghiale russo", "Jabalí ruso", "Jabalí ruso",
      "ロシアイノシシ", "Javali russo", "Dzik rosyjski", "Русский кабан", "Russisch wild zwijn", "러시아 멧돼지",
      "俄羅斯野豬", "俄罗斯野猪", "خنزير بري روسي"}},
    {"trx_chicken",
     {"Chicken", "Poulet", "Huhn", "Pollo", "Gallina", "Gallina", "ニワトリ", "Galinha", "Kura", "Курица", "Kip", "닭",
      "雞", "鸡", "دجاجة"}},
    {"trx_crow",
     {"Crow", "Corbeau", "Krähe", "Corvo", "Cuervo", "Cuervo", "カラス", "Corvo", "Wrona", "Ворона", "Kraai", "까마귀",
      "烏鴉", "乌鸦", "غراب"}},
    {"trx_rock_ptarmigan",
     {"Rock Ptarmigan", "Lagopède alpin", "Alpenschneehuhn", "Pernice bianca", "Lagópodo alpino", "Lagópodo alpino",
      "ライチョウ", "Lagópode-branco", "Pardwa górska", "Тундряная куропатка", "Alpensneeuwhoen", "뇌조", "岩雷鳥",
      "岩雷鸟", "طيهوج صخري"}},
    {"trx_seagull",
     {"Seagull", "Mouette", "Möwe", "Gabbiano", "Gaviota", "Gaviota", "カモメ", "Gaivota", "Mewa", "Чайка", "Meeuw",
      "갈매기", "海鷗", "海鸥", "نورس"}},
    {"trx_siberian_buzzard",
     {"Siberian Buzzard", "Buse de Sibérie", "Sibirischer Bussard", "Poiana siberiana", "Busardo siberiano",
      "Busardo siberiano", "シベリアノスリ", "Gavião-siberiano", "Myszołów syberyjski", "Сибирский канюк",
      "Siberische buizerd", "시베리아 매", "西伯利亞鷹", "西伯利亚鹰", "صقر حوام سيبيري"}},
    {"trx_naumanns_thrush",
     {"Naumanns Thrush", "Grive de Naumann", "Naumann-Drossel", "Tordo di Naumann", "Zorzal de Naumann",
      "Zorzal de Naumann", "ハチジョウツグミ", "Tordo-de-naumann", "Drozd Naumanna", "Дрозд Науманна",
      "Naumanns lijster", "노랑지빠귀", "紅尾歌鳥", "红尾歌鸟", "سمنة ناومان"}},
    {"trx_siberian_yellow_wagtail",
     {"Siberian Yellow Wagtail", "Bergeronnette jaune de Sibérie", "Sibirische Schafstelze", "Cutrettola siberiana",
      "Lavandera boyera siberiana", "Lavandera boyera siberiana", "シベリアツメナガセキレイ",
      "Alvéola-amarela-siberiana", "Pliszka żółta syberyjska", "Сибирская жёлтая трясогузка",
      "Siberische gele kwikstaart", "시베리아 긴발톱할미새", "西伯利亞黃搖尾鳥", "西伯利亚黄摇尾鸟",
      "ذعرة صفراء سيبيرية"}},
    {"live_en_lynx",
     {"Lynx", "Lynx", "Luchs", "Lince", "Lince", "Lince", "オオヤマネコ", "Lince", "Ryś", "Рысь", "Lynx", "스라소니",
      "山貓", "山猫", "وشق"}},
    {"live_en_snow_leopard",
     {"Snow Leopard", "Panthère des neiges", "Schneeleopard", "Leopardo delle nevi", "Leopardo de las nieves",
      "Leopardo de las nieves", "ユキヒョウ", "Leopardo-das-neves", "Pantera śnieżna", "Снежный барс", "Sneeuwpanter",
      "눈표범", "雪豹", "雪豹", "فهد ثلجي"}},
    {"live_en_tundra_wolf",
     {"Tundra Wolf", "Loup de la toundra", "Tundrawolf", "Lupo della tundra", "Lobo de la tundra", "Lobo de la tundra",
      "ツンドラオオカミ", "Lobo-da-tundra", "Wilk tundrowy", "Тундровый волк", "Toendrawolf", "툰드라 늑대", "凍原狼",
      "冻原狼", "ذئب التندرا"}},
    {"trx_alpha_tundra_wolf",
     {"Alpha Tundra Wolf", "Loup alpha de la toundra", "Alpha-Tundrawolf", "Lupo alfa della tundra",
      "Lobo alfa de la tundra", "Lobo alfa de la tundra", "アルファのツンドラオオカミ", "Lobo-da-tundra alfa",
      "Wilk tundrowy alfa", "Тундровый волк-вожак", "Alfa-toendrawolf", "우두머리 툰드라 늑대", "凍原頭狼", "冻原头狼",
      "ذئب تندرا قائد"}},
    {"pdlc_babayaga_enm_crazywolf",
     {"Witch Wolf", "Loup de la sorcière", "Hexenwolf", "Lupo della strega", "Lobo de la bruja", "Lobo de la bruja",
      "魔女のオオカミ", "Lobo da Bruxa", "Wilk wiedźmy", "Волк ведьмы", "Heksenwolf", "마녀의 늑대", "女巫之狼",
      "女巫之狼", "ذئب الساحرة"}},
    {"trx_rat",
     {"Rat", "Rat", "Ratte", "Ratto", "Rata", "Rata", "ネズミ", "Rato", "Szczur", "Крыса", "Rat", "쥐", "老鼠", "老鼠",
      "جرذ"}},
};

// ---- Where the model stands ---------------------------------------------------------------------------------------------
// Away from the main menu's room (the menu camera's place when the viewer opened): above it (default), in front of it
// (in the room), below it. P (L3) goes through them. [ModelViewer] Height= (cm above / below), Ahead= (cm in front).
enum Place { kPlaceAbove, kPlaceFront, kPlaceBelow, kPlaceCount };
const char* const kPlaceNames[kPlaceCount] = {"above the menu's camera", "in front of the menu's camera",
                                              "below the menu's camera"};
float g_height = 3000.0f, g_ahead = 500.0f;
std::atomic<int> g_place{kPlaceAbove};
Features::DrawCameraState g_menuCamera;   // the menu's camera when the viewer opened
float g_fov = 0.9f;                       // its vertical field of view (radians; the viewer's camera keeps it)
float g_at[3]{};                          // the model's place (its origin) now

enum class Phase { Closed, Loading, Shown, Failed };
std::atomic<bool> g_open{false};
Phase g_phase = Phase::Closed;
std::uint32_t g_zone = 0;
bool g_zoneRef = false;
double g_loadStart = 0.0, g_shownAt = 0.0, g_lastTick = 0.0;
void* g_instance = nullptr;
std::uint32_t g_instanceId = 0;
std::uint64_t g_componentId = 0;
int g_unit = 0;
bool g_positionLogged = false, g_placeFailLogged = false;
int g_placeResets = 0;
float g_bornAt[3]{};
// The model turned (v0.9.423): the view's yaw turns the instance itself (Features::TurnInstanceNow, as the game moves an
// instance: its cloth, bone springs and hair swing), the camera stays (its pitch, zoom and pan the view's); from the
// moment it is shown (sized and drawn at 0 before). When the turn cannot be written the camera turns about it as before.
float g_turned = 0.0f;          // radians: the instance's turn about the vertical (cdc: x to y)
bool g_modelTurns = false;      // the turn is the instance's (else the camera's heading)
bool g_turnFailLogged = false;

// the bind pose: the instance whose pose AnimBuildTransforms gets as its bind pose (any thread reads it)
std::atomic<void*> g_poseInstance{nullptr};
std::atomic<std::uint32_t> g_poseCalls{0};

// the requests (other threads)
std::atomic<bool> g_lightsToggle{false}, g_resetRequest{false}, g_placeRequest{false};
std::atomic<int> g_step{0}, g_categoryStep{0};
std::atomic<bool> g_lightsOn{true};
std::atomic<float> g_brightness{1.0f};   // the studio lights' intensities x this ([ModelViewer] Brightness; + / -)
std::atomic<int> g_brightnessStep{0};
std::atomic<double> g_closedAt{-10.0};
bool g_depthOfFieldOff = false;   // the depth of field turned off when the viewer opened (Close turns it on again)

// ---- The screens (v0.9.418): the campsite's Outfits grid and the relic viewer (CutsceneViewer sets them up). The grid:
// the chosen model on the right of the picture, beside the grid (on the left: CutsceneViewer, [ModelViewer] GridScale,
// GridLeft, GridTop, v0.9.431), its middle at [ModelViewer] GridModelX of the width (in NDC, -1 the left edge, 1 the
// right), in GridModelHalf of it each way, turning by itself (v0.9.431: the weapons and the relics only); it follows
// the grid's highlight once that rests (kGridRest: moving over the grid loads nothing on the way). The view: the model
// alone in the middle, the controls the viewer's.
std::atomic<Screen> g_screen{Screen::Grid};
std::atomic<bool> g_backRequest{false};
std::atomic<float> g_aspect{16.0f / 9.0f};      // the screen's (Draw)
constexpr double kGridRest = 0.2;               // s (v0.9.433: 0.3 before)
float g_gridModelX = 0.42f, g_gridModelHalf = 0.42f;
// the grid on the movie's 1280 x 720 stage (v0.9.431): its scale, its tiles' left edge, its mask's top
float g_gridScale = 0.55f, g_gridLeft = 64.0f, g_gridTop = 112.0f;
std::string g_musicTrack;                       // [ModelViewer] Music: the stream (v0.9.431; "" none)
int g_voiceCategory = -1, g_voiceIndex = -1;    // the model whose relic line was asked for (game thread)
// (v0.9.443) A character's line on V (the grid's highlight or the view's model; the author's choices): the voice stream's
// name (pcx64-w\audio\streams\<name>.mul, as the Suite's sound catalog lists them), played in the voice language.
struct CharacterLine {
    const char* character;
    const char* stream;
};
const CharacterLine kCharacterLines[] = {
    {"sofia_cine", "vo\\act_02\\220_vh_valley_hub\\220_vh_037_supplies_prime\\220_vh_037_390_sofia"},
    {"jonah_cine", "vo\\secondary_narrative\\405_cn_misc\\405_cn_120_post_beta_new\\405_cn_120_240_jonah"},   // (v0.9.447: Jonah London's)
    {"jonah_london_cine", "vo\\secondary_narrative\\405_cn_misc\\405_cn_120_post_beta_new\\405_cn_120_240_jonah"},
    {"ana_cine", "vo\\secondary_narrative\\430_hd_documents\\430_hd_080_sessions\\430_hd_080_065_ana"},
    {"konstantin_combat_cine", "vo\\act_02\\280_gr_gulag_rescue\\280_gr_040_run_lara_run\\280_gr_040_190_konstantin"},
    {"konstantin_jacket_cine", "vo\\act_02\\280_gr_gulag_rescue\\280_gr_040_run_lara_run\\280_gr_040_210_konstantin"},
    {"jacob_cine", "vo\\act_02\\220_vh_valley_hub\\220_vh_085_triage_ii\\220_vh_085_180_jacob"},
    {"dlc_nadia_cine", "vo\\dlc\\600_cn_dlc_cinematics\\600_cn_040_gondola_ride\\600_cn_040_010_nadia"},
    {"witchcup_witchywitch_cine", "vo\\dlc\\610_by_dlc_gameplay\\610_by_085_supplemental\\610_by_085_390_baba_yaga"},   // (v0.9.449)
};
const char* CharacterLineOf(const std::string& object) {
    for (const auto& line : kCharacterLines)
        if (_stricmp(line.character, object.c_str()) == 0) return line.stream;
    return nullptr;
}
// (v0.9.449) Lara's lines on V, for every one of her outfits (the Outfits list; the author's choices): one at random,
// each of them once before any comes again (a shuffled bag; a new bag never starts with the line just heard).
const char* const kLaraLines[] = {
    "vo\\systemic\\510_lc_lara_systemic\\510_lc_030_experience_reactions\\510_lc_030_015_lara",
    "vo\\secondary_narrative\\405_cn_misc\\405_cn_080_beta_new_1\\405_cn_080_350_lara",
    "vo\\secondary_narrative\\405_cn_misc\\405_cn_080_beta_new_1\\405_cn_080_730_lara",
    "vo\\secondary_narrative\\405_cn_misc\\405_cn_130_post_beta_boss_fight\\405_cn_130_250_lara",
    "vo\\secondary_narrative\\405_cn_misc\\405_cn_040_lara_extra\\405_cn_040_030_lara",
    "vo\\act_01\\100_bp_black_sea_prologue\\100_bp_065_murals\\100_bp_065_250_lara",
    "vo\\act_01\\100_bp_black_sea_prologue\\100_bp_065_murals\\100_bp_065_470_lara",
    "vo\\act_02\\250_fa_flooded_archive\\250_fa_120_the_grotto\\250_fa_120_090_lara",
    "vo\\act_02\\212_ge_gulag_escape\\212_ge_090_control_room\\212_ge_090_007_lara",
    "vo\\act_02\\212_ge_gulag_escape\\212_ge_130_gatekeeper\\212_ge_130_110_lara",
};
std::vector<int> g_laraBag;    // the lines still to come in this round (game thread)
int g_laraLast = -1;           // the line played last
const char* NextLaraLine() {
    if (g_laraBag.empty()) {
        static std::mt19937 random{std::random_device{}()};
        for (int i = 0; i < int(std::size(kLaraLines)); ++i) g_laraBag.push_back(i);
        std::shuffle(g_laraBag.begin(), g_laraBag.end(), random);
        if (g_laraBag.size() > 1 && g_laraBag.back() == g_laraLast) std::swap(g_laraBag.front(), g_laraBag.back());
    }
    g_laraLast = g_laraBag.back();
    g_laraBag.pop_back();
    return kLaraLines[g_laraLast];
}
std::atomic<bool> g_lineRequest{false};          // V (the view's key, or the grid's: RequestLine)
std::atomic<bool> g_lineKeyHeld{false};
int g_lineCategory = -1, g_lineIndex = -1;       // the model whose character line plays (game thread)
int g_shownCategory = -1, g_shownIndex = -1;    // the model loaded (game thread)
double g_cursorAt = 0.0;                        // when the choice moved last (game thread)
std::atomic<unsigned> g_choiceSerial{0};
double g_viewLeftAt = -10.0;                    // when the view was left (its keys let go)

// the view: the camera turned about the model, tilted, zoomed, moved (the mouse on the overlay's thread, the pad on the
// game's)
struct View {
    float yaw = 0.0f, pitch = 0.0f;   // degrees: about the model (positive: the model turns to the right), from above
    float zoom = 1.0f;                // the distance's factor
    float panX = 0.0f, panZ = 0.0f;   // cm: the model moved right, up in the picture
};
std::mutex g_viewMutex;
View g_view;
std::atomic<float> g_distanceNow{300.0f};
// the pad's sticks and triggers (FilterPad), applied per frame by the tick
std::mutex g_padMutex;
float g_padRX = 0, g_padRY = 0, g_padLX = 0, g_padLY = 0, g_padLT = 0, g_padRT = 0;
std::atomic<WORD> g_padHeld[4];
std::atomic<bool> g_padDrain[4];

// the model's size: its skeleton's bones in the bind pose (measured when born)
struct Bounds {
    bool valid = false;
    float min[3]{}, max[3]{};
    std::uint32_t segments = 0;
};
Bounds g_bounds;   // (game thread)

// the model's components (see kRvaGetComponent): left out from its birth until it is taken away
using GetComponentFn = void*(__fastcall*)(void*, std::uint32_t);
GetComponentFn g_origGetComponent = nullptr;
bool g_filterReady = false;                        // the hooks made, the hashes the names', the maps wrapped
std::atomic<bool> g_soundOn{false}, g_soundToggle{false};
// The models filtered (v0.9.421): [0] the one shown (from its birth), [1] the one taken away last (until a second
// after: its removal comes at that frame's end, perhaps after the next one's birth), each as it was born, so that what
// was left out at its birth stays left out until it is gone (its removal's calls too).
struct FilterSlot {
    std::atomic<const void*> data{nullptr};       // its object data (ObjectZoneRef +8)
    std::atomic<const void*> instance{nullptr};   // its instance once born (the maps' calls name it)
    std::atomic<ULONGLONG> until{0};              // (GetTickCount64) the one taken away: until then
    std::atomic<bool> player{false};              // a player object: its look alone
    std::atomic<bool> soundOn{false};             // born with its sound
    std::atomic<std::uint32_t> hairObject{0};     // the hair it wears (v0.9.423: the Suite's look; 0: its own)
};
FilterSlot g_slots[2];
constexpr std::uint32_t HashName(const char* s) {
    std::uint32_t h = 0;
    for (; *s; ++s) h ^= (h << 5) + (h >> 2) + std::uint8_t(*s);
    return h;
}
constexpr const char* kSoundComponents[] = {"sound", "soundskeletal", "bark"};
constexpr const char* kPlayerLook[] = {"drawable",      "animated",     "materialswap", "cloth",
                                       "bonesprings",   "hardaccessory", "dynamichair"};
// (the names the log gives the ones left out: laracroft's components, the NPCs' and the relics')
constexpr const char* kComponentNames[] = {
    "animated", "animtarget", "bark", "bonesprings", "capabilities", "cloth", "cold", "concealment", "controller",
    "drawable", "dynamichair", "dynamictuning", "forcefield", "fx", "hardaccessory", "hunger", "ik", "interact",
    "interactable", "inventory", "markupdetection", "materialswap", "meleemove", "movesetprofile", "objectnetdata",
    "physical", "player", "powermeter", "proxy", "ragdoll", "scoreattack", "script", "sensible", "sound", "soundskeletal",
    "spatialawareness", "spawn", "statecontrol", "team", "camera", "mesh", "npc", "health", "damage", "corpse", "loot",
    "flammable", "adrenalinemode", "survivalinstinct", "projectileresponse", "p2pcommunicationmarker", "collectible",
    "fingerofgod", "grapple", "relic", "respawnmarker", "targetindicator", "watervolumedetection", "multiplayer3dtext",
    "actiongraphs"};
std::uint32_t g_leftOut[64];                       // the types left out of the model shown (the hooks' thread)
std::atomic<int> g_leftOutCount{0};

bool OneOf(std::uint32_t hash, const char* const* names, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i)
        if (HashName(names[i]) == hash) return true;
    return false;
}
bool LeftOut(const FilterSlot& slot, std::uint32_t hash) {
    const bool player = slot.player.load(std::memory_order_relaxed);
    if (OneOf(hash, kSoundComponents, std::size(kSoundComponents)))
        return !slot.soundOn.load(std::memory_order_relaxed) || (player && hash == HashName("bark"));
    return player && !OneOf(hash, kPlayerLook, std::size(kPlayerLook));
}
// the item lists (see kRvaFindItemList) a player object goes without
bool ListLeftOut(const FilterSlot& slot, std::uint32_t hash) {
    return slot.player.load(std::memory_order_relaxed) && hash == HashName("actiongraphs");
}

// The model an ObjectZoneRef (its object data at +8) is, and the instance when the call names it: the slot, or null.
FilterSlot* ModelSlot(const void* ref, const void* instance = nullptr) {
    const void* data = *reinterpret_cast<void* const*>(static_cast<const std::uint8_t*>(ref) + 8);
    if (!data) return nullptr;
    for (FilterSlot& slot : g_slots) {
        if (slot.data.load(std::memory_order_acquire) != data) continue;
        if (instance) {   // (born: only its own instance; being born: any with its data)
            const void* own = slot.instance.load(std::memory_order_acquire);
            if (own && own != instance) continue;
        }
        const ULONGLONG until = slot.until.load(std::memory_order_relaxed);
        if (until && GetTickCount64() > until) {   // (gone a while: its data may be another's now)
            slot.data.store(nullptr, std::memory_order_release);
            continue;
        }
        return &slot;
    }
    return nullptr;
}

// A type left out of the model, noted for the log: a component's (LeftOut) or an item list's (ListLeftOut).
bool Filtered(const void* ref, std::uint32_t hash, bool list = false, const void* instance = nullptr) {
    const FilterSlot* slot = ModelSlot(ref, instance);
    if (!slot || !(list ? ListLeftOut(*slot, hash) : LeftOut(*slot, hash))) return false;
    if (slot != &g_slots[0]) return true;
    const int n = g_leftOutCount.load(std::memory_order_relaxed);
    bool seen = false;
    for (int i = 0; i < n && i < 64; ++i) seen = seen || g_leftOut[i] == hash;
    if (!seen && n < 64) {
        g_leftOut[n] = hash;
        g_leftOutCount.store(n + 1, std::memory_order_relaxed);
    }
    return true;
}
// The hair a model wears (v0.9.423; the Suite's look, see SuiteLook): its "dynamichair" data
// (dtp::DynamicHairComponent, 0x80 bytes in TR2's PDB: m_hairEnabled +0, m_hairSplineData +4 = {m_hairSplineObject (the
// hair object's ID), m_playerBoneMarker}, then the profiles' pointers) answered as a copy with that hair object's ID.
// DynamicHairComponentManager::AddComponent (retail 0x2266D0) keeps the data (the component's +0x50, 0x226725) and asks
// for the object by that ID (+0x58, 0x24D260: ObjectZoneManager's request, from data +4 at 0x226783); AllocHairInstance
// births it once loaded, SetHairModel (retail 0x24C8D0) reads the same +0x50 data; the marker kept (a hood's hair is
// switched to with the same bone in the game's scripts: the Suite's AddHair reads the pairs). The copies live in a ring
// (a removed component's data stays valid a while: 16 births later it is long gone).
constexpr std::uint32_t kHashDynamicHair = HashName("dynamichair");
constexpr std::size_t kHairDataSize = 0x80, kHairDataObject = 4, kHairCopies = 16;
struct alignas(16) HairCopy {
    std::uint8_t bytes[kHairDataSize];
};
HairCopy g_hairCopies[kHairCopies];
const void* g_hairCopySource[kHairCopies]{};
std::uint32_t g_hairCopyObject[kHairCopies]{};
std::size_t g_hairCopyNext = 0;
std::mutex g_hairCopyMutex;
std::atomic<std::uint32_t> g_hairSwapped{0}, g_hairOwnObject{0};
// The data copied with the object's ID, false when it cannot be read or has no hair object. SEH only.
bool CopyHairData(const void* data, std::uint32_t object, HairCopy& out, std::uint32_t& own) noexcept {
    __try {
        std::memcpy(out.bytes, data, kHairDataSize);
        std::memcpy(&own, out.bytes + kHairDataObject, sizeof(own));
        if (!own) return false;
        std::memcpy(out.bytes + kHairDataObject, &object, sizeof(object));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void* WornHair(const void* ref, void* data) {
    const FilterSlot* slot = ModelSlot(ref);
    const std::uint32_t object = slot ? slot->hairObject.load(std::memory_order_relaxed) : 0;
    if (!object) return data;
    std::lock_guard lock(g_hairCopyMutex);
    for (std::size_t i = 0; i < kHairCopies; ++i)   // (asked again: the same copy)
        if (g_hairCopySource[i] == data && g_hairCopyObject[i] == object) return g_hairCopies[i].bytes;
    const std::size_t i = g_hairCopyNext;
    std::uint32_t own = 0;
    if (!CopyHairData(data, object, g_hairCopies[i], own)) return data;
    g_hairOwnObject.store(own, std::memory_order_relaxed);
    if (own == object) return data;   // (its own already)
    g_hairCopyNext = (i + 1) % kHairCopies;
    g_hairCopySource[i] = data;
    g_hairCopyObject[i] = object;
    g_hairSwapped.fetch_add(1, std::memory_order_relaxed);
    return g_hairCopies[i].bytes;
}
void* __fastcall HookGetComponent(void* ref, std::uint32_t hash) {
    if (Filtered(ref, hash)) return nullptr;
    void* data = g_origGetComponent(ref, hash);
    return data && hash == kHashDynamicHair ? WornHair(ref, data) : data;
}
GetComponentFn g_origFindItemList = nullptr;   // (the same shape: ObjectZoneRef, hash -> the entry or null)
bool g_findItemListHooked = false;
void* __fastcall HookFindItemList(void* ref, std::uint32_t hash) {
    return Filtered(ref, hash, true) ? nullptr : g_origFindItemList(ref, hash);
}
using ObjectFlagsFn = const std::uint32_t*(__fastcall*)(const void*);
ObjectFlagsFn g_origObjectFlags = nullptr;
bool g_objectFlagsHooked = false;
std::atomic<std::uint32_t> g_playerFlagHidden{0};   // the lookups answered without the player's flag (the log)
const std::uint32_t* __fastcall HookObjectFlags(const void* ref) {
    const std::uint32_t* flags = g_origObjectFlags(ref);
    if (!flags || !(flags[0] & kObjectPlayerFlag)) return flags;
    const FilterSlot* slot = ModelSlot(ref);
    if (!slot || !slot->player.load(std::memory_order_relaxed)) return flags;
    thread_local std::uint32_t copy[2];   // (its callers read these two words)
    copy[0] = flags[0] & ~kObjectPlayerFlag;
    copy[1] = flags[1];
    g_playerFlagHidden.fetch_add(1, std::memory_order_relaxed);
    return copy;
}

// The per-type calls (see kRvaComponentMakers and kComponentCalls): each map's entry in a slot of its own, its wrapper
// in the map. A model's type left out: none of its calls (its maker, its removal's...) for that model.
using TypeCallFn = void*(__fastcall*)(void*, const void*);
constexpr int kCallSlots = 128;
struct TypeCall {
    std::uint32_t hash = 0;
    TypeCallFn original = nullptr;
};
TypeCall g_typeCalls[kCallSlots];
int g_typeCallsUsed = 0;
template <int I>
void* __fastcall WrappedTypeCall(void* instance, const void* data) {
    const void* ref = *reinterpret_cast<void* const*>(static_cast<const std::uint8_t*>(instance) + kInstanceObjectRef);
    if (ref && Filtered(ref, g_typeCalls[I].hash, false, instance)) return nullptr;   // (the loop goes on)
    return g_typeCalls[I].original(instance, data);
}
template <int... I>
constexpr std::array<TypeCallFn, sizeof...(I)> TypeCallWrappers(std::integer_sequence<int, I...>) {
    return {{&WrappedTypeCall<I>...}};
}
constexpr std::array<TypeCallFn, kCallSlots> kTypeCallWrappers =
    TypeCallWrappers(std::make_integer_sequence<int, kCallSlots>{});

// A map's entries wrapped (game thread, once): how many; -1 the map not as expected (some may be wrapped then: the
// wrappers call the game's unless a model's type is left out). total: its nodes (its null entries among them: 0x225150
// adds one for each hash looked up and not there).
int WrapTypeCalls(std::uintptr_t map, int& total, bool& soundSkeletal) noexcept {
    int wrapped = 0;
    total = 0;
    soundSkeletal = false;
    __try {
        auto* const* buckets = *reinterpret_cast<std::uint8_t* const* const*>(map + kMapBuckets);
        const std::uint64_t count = *reinterpret_cast<const std::uint64_t*>(map + kMapCount);
        const std::uint64_t bucketCount = *reinterpret_cast<const std::uint64_t*>(map + kMapBucketCount);
        if (!buckets || !bucketCount || bucketCount > 65536 || count > 65536) return -1;
        for (std::uint64_t b = 0; b < bucketCount; ++b)
            for (std::uint8_t* node = buckets[b]; node && total <= 65536;
                 node = *reinterpret_cast<std::uint8_t**>(node + kNodeNext)) {
                ++total;
                auto* slot = reinterpret_cast<TypeCallFn*>(node + kNodeValue);
                const TypeCallFn call = *slot;
                if (!call) continue;
                if (g_typeCallsUsed >= kCallSlots) return -1;
                const std::uint32_t hash = *reinterpret_cast<const std::uint32_t*>(node + kNodeHash);
                TypeCall& entry = g_typeCalls[g_typeCallsUsed];
                entry.hash = hash;
                entry.original = call;
                std::atomic_thread_fence(std::memory_order_release);   // (the slot before its wrapper is seen)
                *slot = kTypeCallWrappers[g_typeCallsUsed];
                ++g_typeCallsUsed;
                soundSkeletal = soundSkeletal || hash == HashName("soundskeletal");
                ++wrapped;
            }
        return std::uint64_t(total) == count ? wrapped : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
std::string LeftOutNames() {
    std::string names;
    const int n = std::min(g_leftOutCount.load(), 64);
    for (int i = 0; i < n; ++i) {
        const char* name = nullptr;
        for (const char* known : kComponentNames)
            if (HashName(known) == g_leftOut[i]) name = known;
        char hex[16];
        snprintf(hex, sizeof(hex), "%08X", g_leftOut[i]);
        names += (names.empty() ? "" : ", ") + std::string(name ? name : hex);
    }
    return names;
}

// the model's bounds as drawn (see kEntityWorldVolume): its entities' world volumes, from Scene::DrawEntity
std::atomic<void*> g_boundsInstance{nullptr};
std::mutex g_volumeMutex;
struct WorldBox {
    float min[3]{}, max[3]{};
    int count = 0;
};
WorldBox g_worldBox;   // (under g_volumeMutex)
Bounds g_drawnBounds;   // (game thread: g_worldBox about the model's place)

// The Modding Suite's records (v0.9.422: the bounds; v0.9.423: the look): <game>\ROTTRFramework\model_viewer\bounds.json
// ({"models": {"<object>": {"min": [x, y, z], "max": [x, y, z], "hide": [draw groups], "hair": "<object>"}}}),
// written by the Suite's "Make the pictures" (2.0.111: the outfits' records too, without a picture).
// - The bounds: cm in the model's own space, its bind pose, from each model's vertices (its icon's camera fits the same
//   box). The best size there is: the drawn bounds are what the game culls by (a cinematic character's a 15 m ball,
//   the red deer's a 2 m one, the relics' boxes far larger than they look), the bones miss a rigid object.
// - The look: the Suite's 3D view with Gear on Default (Lara's outfits: the usual gear, no upgrades, the outfit's
//   default hood state; its hair in that state). The game shows and hides an outfit's draw groups and switches its hair
//   from Lara's action graphs (the gear graph's AGInstanceDrawGroups nodes, retail ProcessMsg 0x75E220: the
//   DrawableComponent (Instance +0xD8 [8], 0x3DE610) -> its default drawable (+0xA0) -> its parent settings (+0xA8) ->
//   m_noDrawGroups (+0x10), bit k: draw group k + 1 (cdc::ApplyDrawGroupMask, retail 0x3CFD60: the drawable's
//   draw-group visibility from that mask | the FX groups' (+0x18) | its own (+0xA0)); AGSetHairObjectNode ->
//   DynamicHairComponent::SetHairModel); the viewer births them without those graphs, so every group was drawn (the
//   upgrades', both hoods') and the component's own hair worn. "hide" (the Suite's numbers: draw group g, 1..63) is
//   written as the instance's mask (Features::SetInstanceNoDrawMaskNow: the DrawableComponent's +0xB8, its settings'
//   +0x10) every frame from its birth (before it is shown); "hair" is the hair object the component loads (see
//   g_hairCopies).
struct SuiteLook {
    bool known = false;          // the record has a look (2.0.111)
    std::uint64_t noDraw = 0;    // the instance's mask (bit g - 1: draw group g)
    int hidden = 0;              // how many groups
    std::string hair;            // the hair object worn ("": the outfit's own)
};
struct SuiteRecord {
    Bounds bounds;
    SuiteLook look;
};
std::map<std::string, SuiteRecord> g_suiteRecords;   // (read when the viewer opens)
Bounds g_modelSuiteBounds;                     // the model shown's (game thread)
SuiteLook g_modelLook;                         // likewise
std::uint32_t g_modelHairObject = 0;           // its hair object's ID (0: its own)
bool g_lookLogged = false;
std::filesystem::path ViewerFolder();
void LoadSuiteBounds() {
    g_suiteRecords.clear();
    std::ifstream in(ViewerFolder() / L"bounds.json", std::ios::binary);
    if (!in) {
        Log::Write("ModelViewer: no Modding Suite records (ROTTRFramework\\model_viewer\\bounds.json is not there): Lara's outfits "
                   "are shown with every part and their own hair, the models sized by their bones or drawn bounds. In the Suite "
                   "(2.0.111 or later): the Models tab, For the Model Viewer, Make the pictures.");
        return;
    }
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    const auto models = j.is_object() ? j.find("models") : j.end();
    if (j.is_discarded() || models == j.end() || !models->is_object()) {
        Log::Write("ModelViewer: ROTTRFramework\\model_viewer\\bounds.json could not be read");
        return;
    }
    int bounds = 0, looks = 0;
    for (auto it = models->begin(); it != models->end(); ++it) {
        const nlohmann::json& m = it.value();
        if (!m.is_object()) continue;
        SuiteRecord r;
        if (m.contains("min") && m.contains("max")) {
            const nlohmann::json &lo = m["min"], &hi = m["max"];
            if (lo.is_array() && hi.is_array() && lo.size() == 3 && hi.size() == 3) {
                Bounds& b = r.bounds;
                b.valid = true;
                for (std::size_t k = 0; k < 3; ++k) {
                    if (!lo[k].is_number() || !hi[k].is_number()) b.valid = false;
                    else b.min[k] = lo[k].get<float>(), b.max[k] = hi[k].get<float>();
                    b.valid = b.valid && std::isfinite(b.min[k]) && std::isfinite(b.max[k]) && b.min[k] <= b.max[k] &&
                              b.max[k] - b.min[k] < 100000.0f;
                }
            }
        }
        if (const auto hide = m.find("hide"); hide != m.end() && hide->is_array()) {
            r.look.known = true;
            for (const auto& g : *hide) {
                if (!g.is_number_integer()) continue;
                const int group = g.get<int>();
                if (group >= 1 && group <= 64 && !(r.look.noDraw >> (group - 1) & 1u))
                    r.look.noDraw |= std::uint64_t(1) << (group - 1), ++r.look.hidden;
            }
            if (const auto hair = m.find("hair"); hair != m.end() && hair->is_string()) r.look.hair = hair->get<std::string>();
        }
        if (!r.bounds.valid && !r.look.known) continue;
        bounds += r.bounds.valid ? 1 : 0;
        looks += r.look.known ? 1 : 0;
        g_suiteRecords[it.key()] = std::move(r);
    }
    Log::Writef("ModelViewer: the Modding Suite's records (model_viewer\\bounds.json): the bounds of %d models, the look of %d",
                bounds, looks);
}

// The model shown hidden until all of it is there (v0.9.422): no parts drawn before the rest (an outfit's hair came a
// moment before its body), no size changed once seen (a relic was sized by its kind until its drawn bounds came, big
// for a moment). Scene::DrawEntity (hooked) leaves out the model's entities (their drawables' +0x10 the model, the
// hair's too) until TickGameThread shows it: its body (the DrawableComponent's anim drawable, see kAnimDrawable) drawn,
// an outfit's hair drawn (cdc::DynamicHairDrawable, vtable retail 0xD2B660, or a second gone by), its size known (the
// Suite's bounds, its bones, or its drawn bounds), three frames on; 2.5 s at most.
constexpr std::uintptr_t kRvaHairDrawableVtable = 0x00D2B660ull, kRvaHairDrawableFirst = 0x002F0DE0ull;
std::atomic<bool> g_revealed{true};
std::atomic<void*> g_bodyDrawable{nullptr};
std::atomic<std::uint32_t> g_bodyDraws{0}, g_hairDraws{0}, g_otherDraws{0};
int g_revealTicks = 0;
bool g_shownPlayer = false;
bool g_hairVtableOk = false;   // (Check: the vtable's first entry where expected)
bool g_cubeReflVtableOk = false, g_cubeReflHooked = false;   // (the reflections' constants: Check, CreateHooks)
std::uint8_t* g_colorCorrectionDone = nullptr;   // the game's "colour correction drawn this frame" byte (Check)
std::uint8_t* g_sssToggle = nullptr;             // the cutscenes' subsurface scattering toggle (Check; v0.9.430)
std::uint8_t* g_motionBlur = nullptr;            // cdc::g_globalMotionBlurEnabled (Check; v0.9.445)
bool g_motionBlurCleared = false;                // (the viewer cleared it: set again on closing)

// the camera this frame (game thread): where, its axes, what it looks at
struct Frame {
    float position[3]{}, forward[3]{}, right[3]{}, up[3]{}, center[3]{};
    float radius = 100.0f, distance = 300.0f;
};
Frame g_frame;

// ---- The lights: the Modding Suite's studio preset 0 (studio.cpp, "Studio (follows the view)") as native scene lights
// (point lights far away, so nearly the Suite's directional ones): around the camera (azimuth 0: from the camera,
// positive to the left of the picture; elevation above), linear colour, intensity as the game's.
struct StudioLight {
    const char* name;
    float color[3];
    float intensity, azimuth, elevation;
};
const StudioLight kStudio[3] = {
    {"Key", {1.0f, 0.955f, 0.89f}, 1.45f, 38.0f, 33.0f},
    {"Fill", {0.71f, 0.81f, 1.0f}, 0.42f, -53.0f, -6.0f},
    {"Rim", {1.0f, 1.0f, 1.0f}, 0.9f, -155.0f, 30.0f},
};
constexpr float kLightAway = 1500.0f;     // cm from the model at least
constexpr float kLightRangeFactor = 20.0f;  // the range: this x the distance (attenuation 1 - t^2: 0.9975 at the model)
int g_lights[3] = {-1, -1, -1};           // Features::ViewerLight* indices
void* g_lightScene = nullptr;
bool g_lightsMade = false, g_lightsGaveUp = false;
double g_lightsTriedAt = -10.0;
int g_lightTries = 0;
// the relic viewer's lights instead (v0.9.427: see kRvaCollectibleInitLights)
bool g_lightsForRelic = false;            // the lights were made for a relic (v0.9.430: the relic viewer's asked for)
std::atomic<bool> g_relicActive{false};   // the lights made are the rig's (the environment the relic viewer's too)
std::uintptr_t g_rigObjectSlot = 0;       // where the global object's pointer is (Check)
const float* g_rigOffset = nullptr;       // CollectibleRender's view offset, (0, 250, 0) (Check)
const float* g_relicPlace = nullptr;      // the relic's place in that view before it, (0, 50, 0) (Check)
alignas(16) float g_relicIrradiance[10 * 4] = {};   // the relic view's: nine zeros, then the game's (0.318, 0, 0, 0)
struct RigLight {
    alignas(16) std::uint8_t properties[Features::kViewerLightPropertiesSize];
    Features::LightSettings base;
    float axes[3][3];       // its rows in the relic view
    float place[3];         // its position there
    float rotation[3], position[3];   // the rig's (degrees; cm)
    int complexity;
    float matrix[16];       // where it was placed last (world)
};
RigLight g_rig[3];
float g_relicDistance = 300.0f;   // the relic from the relic view's camera
std::atomic<std::uint32_t> g_relicToneSet{0}, g_relicApertureSet{0};

bool RelicActive() { return g_relicActive.load(std::memory_order_acquire); }

// ---- The render side (the hooks run on the game's render jobs) ---------------------------------------------------------
using DrawEntityFn = void(__fastcall*)(void*, void*, void*, void*, void*);
using SceneDrawFn = void(__fastcall*)(void*, void*);
DrawEntityFn g_origDrawEntity = nullptr;
SceneDrawFn g_origDrawStaticMeshes = nullptr, g_origDrawGrass = nullptr;
bool g_hooksMade = false;
std::uint64_t g_lightDrawsBase = 0;   // Features::DeferredLightDraws() when the model was shown
// the black background: the model's scene and Instance (nullptr while none is shown: nothing of the scene is drawn)
std::atomic<bool> g_isolate{false};
std::atomic<void*> g_isolateScene{nullptr}, g_isolateInstance{nullptr};
// (v0.9.508) every scene left out (SetBackdropHidden: the Cutscene Viewer's list, on black as the pause screen in game)
std::atomic<bool> g_hideBackdrop{false};
std::atomic<std::uint32_t> g_backdropHiddenDraws{0};
// (v0.9.509) the frame dimmed on the way there and back (SetBackdropDarkness): the tone mapping's exposure_post
// (BloomRender +0xC8, its 2^x the last factor of TonemapPS, see kRvaSetupTonemapParams) set from the game's own value,
// taken while not dimmed, plus log2(1 - darkness); the eye adaptation (+0xD8) held while the scenes are left out (the
// luminance pass not run: the black does not raise it, the room comes back as it was). The main menu has its adaptation
// on (the Model Viewer's model was raised past white by it on the black background, 0.9.413), so the pass is drawn there.
std::atomic<float> g_backdropDarkness{0.0f};
std::atomic<std::uint32_t> g_backdropDimmed{0};
float g_backdropBasePost = 0.0f;
bool g_backdropBaseValid = false;
std::atomic<std::uint32_t> g_entitiesDrawn{0}, g_entitiesHidden{0}, g_meshPassesHidden{0};
std::atomic<bool> g_blackBackground{true}, g_backgroundToggle{false};   // [ModelViewer] Background=; B (R3)
// the scene's own light off: the studio lights' ISceneLights (the only ones LightRenderer::RenderLight lets through)
using ViewportFn = void(__fastcall*)(void*);
using RendererRenderLightFn = bool(__fastcall*)(void*, void*);
ViewportFn g_origApplyAmbient = nullptr, g_origApplySecondAmbient = nullptr, g_origApplyIrradiance = nullptr,
           g_origUpdateBeautySpecular = nullptr;
RendererRenderLightFn g_origRendererRenderLight = nullptr;
void** g_deviceGlobal = nullptr;
std::atomic<bool> g_studioOnly{false};
std::atomic<bool> g_sceneLight{false}, g_sceneLightToggle{false};   // [ModelViewer] SceneLight=; E (Start)
std::atomic<void*> g_ownLights[3];
std::atomic<std::uint32_t> g_gameLightsLeftOut{0}, g_ambientZeroed{0};
// the fixed exposure
using SetupTonemapFn = bool(__fastcall*)(void*);
SetupTonemapFn g_origSetupTonemap = nullptr;
std::atomic<bool> g_exposureFixed{false};
std::atomic<bool> g_autoExposure{false}, g_autoExposureToggle{false};   // [ModelViewer] AutoExposure=; A (pad A)
std::atomic<std::uint32_t> g_adaptationsStopped{0}, g_toneNotDrawn{0};   // (the studio's tone set; the pass not drawn)
std::mutex g_tonemapMutex;
float g_tonemapSeen[7]{};    // the game's values the last time the hook saw them (pre, post, adaptation, perc, limit, white, speed)
bool g_tonemapSeenValid = false;
void* g_isolationCheckedScene = nullptr;   // the scene whose layout the check found as expected (game thread)
bool g_isolationFailed = false;

// what the view says about the model
std::mutex g_textMutex;
std::string g_status;

constexpr float kPi = 3.14159265358979f;

double Now() {
    static LARGE_INTEGER frequency{};
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return double(now.QuadPart) / double(frequency.QuadPart ? frequency.QuadPart : 1);
}

void SetStatus(const char* fmt, ...) {
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    std::lock_guard lock(g_textMutex);
    g_status = text;
}

const Model* CurrentModel() {
    if (g_category < 0 || g_category >= int(g_categories.size())) return nullptr;
    const Category& c = g_categories[std::size_t(g_category)];
    return c.index >= 0 && c.index < int(c.models.size()) ? &c.models[std::size_t(c.index)] : nullptr;
}
// The model loaded or being loaded (else the one chosen): its object (v0.9.433: the one chosen before, which named a
// model still loading after the grid's highlight had moved on).
const std::string& ModelName() {
    static const std::string none;
    const Model* m = nullptr;
    if (g_shownCategory >= 0 && g_shownCategory < int(g_categories.size())) {
        const Category& c = g_categories[std::size_t(g_shownCategory)];
        if (g_shownIndex >= 0 && g_shownIndex < int(c.models.size())) m = &c.models[std::size_t(g_shownIndex)];
    }
    if (!m) m = CurrentModel();
    return m ? m->object : none;
}
int ModelCount() {
    int n = 0;
    for (const auto& c : g_categories) n += int(c.models.size());
    return n;
}

std::uintptr_t RipTarget(std::uintptr_t rva, std::size_t length) {
    const auto disp = *reinterpret_cast<const std::int32_t*>(g_base + rva + 3);
    return g_base + rva + length + std::intptr_t(disp);
}

bool Check() {
    if (g_checked) return g_checked > 0;
    g_checked = -1;
    g_base = GameBuild::Base();
    if (!g_base) return false;
    for (const auto& c : kChecks)
        if (!GameBuild::VerifyBytes(c.rva, c.bytes, c.count)) {
            Log::Writef("ModelViewer: %s is not where expected (retail 0x%llX); the Model Viewer is off", c.what,
                        static_cast<unsigned long long>(c.rva));
            return false;
        }
    g_zoneMap = reinterpret_cast<std::uint8_t**>(RipTarget(0x0023101Aull, 7));   // mov r8, [rip + the zones' map]
    g_deviceGlobal = reinterpret_cast<void**>(RipTarget(kRvaDeviceGlobal, 7));    // mov rcx, [rip + the render device]
    {   // mov edx, [rip + the hash of "player"] (6 bytes)
        const auto disp = *reinterpret_cast<const std::int32_t*>(g_base + kRvaPlayerDataCheck + 7 + 2);
        g_playerHash = reinterpret_cast<const std::uint32_t*>(g_base + kRvaPlayerDataCheck + 7 + 6 + std::intptr_t(disp));
    }
    {   // mov edx, [rip + the hash of "drawable"] (6 bytes)
        const auto disp = *reinterpret_cast<const std::int32_t*>(g_base + kRvaDrawableHashRead + 2);
        g_drawableHash = reinterpret_cast<const std::uint32_t*>(g_base + kRvaDrawableHashRead + 6 + std::intptr_t(disp));
    }
    {   // mov edx, [rip + the hash of "actiongraphs"] (6 bytes)
        const auto disp = *reinterpret_cast<const std::int32_t*>(g_base + kRvaActionGraphsHashRead + 2);
        g_actionGraphsHash = reinterpret_cast<const std::uint32_t*>(g_base + kRvaActionGraphsHashRead + 6 + std::intptr_t(disp));
    }
    g_hairVtableOk = *reinterpret_cast<const std::uintptr_t*>(g_base + kRvaHairDrawableVtable) == g_base + kRvaHairDrawableFirst;
    g_cubeReflVtableOk =
        *reinterpret_cast<const std::uintptr_t*>(g_base + kRvaCubeReflConstantsVtable) == g_base + kRvaCubeReflConstantsDraw;
    if (!g_cubeReflVtableOk) Log::Write("ModelViewer: CubeReflRenderConstantsDrawable's vtable is not where expected: the level's "
                                        "reflections stay");
    if (!g_hairVtableOk) Log::Write("ModelViewer: cdc::DynamicHairDrawable's vtable is not where expected: an outfit is shown without "
                                    "waiting for its hair");
    {   // mov byte [rip + the colour correction's "drawn" byte], 1 (7 bytes: C6 05 disp32 01)
        const auto disp = *reinterpret_cast<const std::int32_t*>(g_base + kRvaColorCorrectionDone + 2);
        g_colorCorrectionDone = reinterpret_cast<std::uint8_t*>(g_base + kRvaColorCorrectionDone + 7 + std::intptr_t(disp));
    }
    {   // cmp byte [rip + the cutscenes' scattering toggle], 0 (7 bytes: 80 3D disp32 00)
        const auto disp = *reinterpret_cast<const std::int32_t*>(g_base + kRvaSssToggleRead + 2);
        g_sssToggle = reinterpret_cast<std::uint8_t*>(g_base + kRvaSssToggleRead + 7 + std::intptr_t(disp));
    }
    {   // cmp byte [rip + cdc::g_globalMotionBlurEnabled], sil (7 bytes: 40 38 35 disp32)
        const auto disp = *reinterpret_cast<const std::int32_t*>(g_base + kRvaMotionBlurRead + 3);
        g_motionBlur = reinterpret_cast<std::uint8_t*>(g_base + kRvaMotionBlurRead + 7 + std::intptr_t(disp));
    }
    g_rigObjectSlot = RipTarget(kRvaRigObjectRead, 7);                               // mov rdx, [rip + the global object]
    g_rigOffset = reinterpret_cast<const float*>(RipTarget(kRvaRigOffsetRead, 7));   // movaps xmm2, [rip + the offset]
    g_relicPlace = reinterpret_cast<const float*>(RipTarget(kRvaRelicPlaceRead, 7)); // movaps xmm6, [rip + (0, 50, 0)]
    std::memcpy(g_relicIrradiance + 9 * 4, reinterpret_cast<const float*>(RipTarget(kRvaRigIrradianceRead, 7)), 4 * sizeof(float));
    g_craftingGlobal = RipTarget(kRvaCraftingRead + 7, 7);   // mov rax, [rip + GlobalCrafting*]
    g_uiInfoGlobal = RipTarget(kRvaUiInfoRead, 7);           // mov rbp, [rip + GlobalUIInfo*]
    {   // the weapons' upgrades (v0.9.438): without them the weapons are shown as before (every draw group)
        bool upgrades = true;
        for (const auto& c : kUpgradeChecks)
            if (upgrades && !GameBuild::VerifyBytes(c.rva, c.bytes, c.count)) {
                upgrades = false;
                Log::Writef("ModelViewer: %s is not where expected (retail 0x%llX); the weapons are shown with every part and "
                            "no ALL UPGRADES", c.what, static_cast<unsigned long long>(c.rva));
            }
        if (upgrades) {
            g_weaponUpgradesGlobal = RipTarget(kRvaWeaponUpgradesRead, 7);   // mov rcx, [rip + the weapon upgrades]
            g_traitsGlobal = RipTarget(kRvaGetTrait, 7);                     // mov rdx, [rip + GlobalAchievements*]
        }
    }
    g_checked = 1;
    return true;
}

// The component filter's hook is the object data's own vfunc, the game's hashes are the names' and the makers' map is
// wrapped (at the first opening: the game's static inits fill them).
bool FilterUsable() {
    static int state = 0;
    if (state) return state > 0;
    if (!g_getComponentHooked || !g_findItemListHooked || !g_objectFlagsHooked) return false;
    const auto* vtable = reinterpret_cast<const std::uintptr_t*>(g_base + kRvaObjectZoneRefVtable);
    const bool vfunc = vtable[1] == g_base + kRvaGetComponent;
    const bool hashes = g_drawableHash && g_playerHash && g_actionGraphsHash && *g_drawableHash == HashName("drawable") &&
                        *g_playerHash == HashName("player") && *g_actionGraphsHash == HashName("actiongraphs");
    const char* why = !vfunc ? "ObjectZoneRef's vtable does not have GetComponent"
                      : !hashes ? "the game's component hashes are not the names'"
                                : nullptr;
    char counts[160] = "";
    int calls = 0;
    for (std::size_t m = 0; !why && m < std::size(kComponentCallMaps); ++m) {
        int total = 0;
        bool soundSkeletal = false;
        const int wrapped = WrapTypeCalls(g_base + kComponentCallMaps[m], total, soundSkeletal);
        const bool needsSound = kComponentCallMaps[m] == kRvaComponentMakers || m + 1 == std::size(kComponentCallMaps);
        if (wrapped < 0 || (needsSound && !soundSkeletal)) {
            static char buffer[96];
            snprintf(buffer, sizeof(buffer), "the per-type map %s is not as expected", kComponentCallMapNames[m]);
            why = buffer;
        }
        calls += std::max(wrapped, 0);
        const std::size_t used = std::strlen(counts);
        snprintf(counts + used, sizeof(counts) - used, "%s%s %d", m ? ", " : "", kComponentCallMapNames[m], wrapped);
    }
    state = why ? -1 : 1;
    if (state < 0)
        Log::Writef("ModelViewer: the component filter is off (%s): the models keep their sounds, Lara's outfits are not shown",
                    why);
    else
        Log::Writef("ModelViewer: the component filter is on (ObjectZoneRef::GetComponent, the item lists, the object "
                    "flags and %d per-type calls: %s; the hashes checked)", calls, counts);
    return state > 0;
}

// ---- SEH-guarded reads and writes (no C++ objects here) ----------------------------------------------------------------
// The zone's skeleton (see kInstanceZone): its segment count and segments; 0 without one (a rigid object).
std::uint32_t SkeletonOf(void* instance, const std::uint8_t*& segments) noexcept {
    segments = nullptr;
    __try {
        auto* zone = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(instance) + kInstanceZone);
        auto* shared = zone ? *reinterpret_cast<std::uint8_t**>(zone + kZoneShared) : nullptr;
        auto* zoneData = shared ? *reinterpret_cast<std::uint8_t**>(shared + kSharedZoneData) : nullptr;
        auto* skeleton = zoneData ? *reinterpret_cast<std::uint8_t**>(zoneData + kZoneDataSkeleton) : nullptr;
        auto* segmentData = skeleton ? *reinterpret_cast<std::uint8_t**>(skeleton) : nullptr;
        if (!segmentData) return 0;
        const std::uint32_t count = *reinterpret_cast<std::uint32_t*>(segmentData);
        segments = *reinterpret_cast<const std::uint8_t**>(segmentData + kSegmentDataSegments);
        return segments && count <= kMaxSegments ? count : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// 1: the zone's object has "player" component data (a player object), 0: it has not, -1: its data is not there. Its
// data (what its instances' ObjectZoneRef +8 is) in objectData.
int IsPlayerObject(std::uint32_t zone, const void*& objectData) noexcept {
    objectData = nullptr;
    __try {
        std::uint8_t* map = g_zoneMap ? *g_zoneMap : nullptr;
        if (!map || !zone || !g_playerHash) return -1;
        if (!*reinterpret_cast<std::uint64_t*>(map + kZoneMapCount)) return -1;
        const std::uint64_t buckets = *reinterpret_cast<std::uint64_t*>(map + kZoneMapBuckets);
        if (!buckets) return -1;
        auto* entry = (*reinterpret_cast<std::uint8_t***>(map))[std::uint64_t(zone) % buckets];
        while (entry && *reinterpret_cast<std::uint32_t*>(entry + kZoneEntryId) != zone) entry = *reinterpret_cast<std::uint8_t**>(entry);
        if (!entry) return -1;
        auto* holder = *reinterpret_cast<std::uint8_t**>(entry + kZoneEntryHolder);
        auto* data = holder ? *reinterpret_cast<std::uint8_t**>(holder + kHolderData) : nullptr;
        if (!data || !data[kDataLoaded]) return -1;
        objectData = data;
        auto* table = *reinterpret_cast<std::uint8_t**>(data + kDataComponents);
        if (!table) return 0;
        const std::uint32_t count = *reinterpret_cast<std::uint32_t*>(table + kComponentsCount);
        auto* entries = *reinterpret_cast<std::uint8_t**>(table + kComponentsEntries);
        for (std::uint32_t i = 0; entries && i < count && i < 4096; ++i)
            if (*reinterpret_cast<std::uint32_t*>(entries + std::size_t(i) * kComponentEntrySize) == *g_playerHash) return 1;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// (v0.9.441) A loaded zone's object data (what its instances' ObjectZoneRef +8 is), else null.
const void* ZoneObjectData(std::uint32_t zone) noexcept {
    __try {
        std::uint8_t* map = g_zoneMap ? *g_zoneMap : nullptr;
        if (!map || !zone || !*reinterpret_cast<std::uint64_t*>(map + kZoneMapCount)) return nullptr;
        const std::uint64_t buckets = *reinterpret_cast<std::uint64_t*>(map + kZoneMapBuckets);
        if (!buckets) return nullptr;
        auto* entry = (*reinterpret_cast<std::uint8_t***>(map))[std::uint64_t(zone) % buckets];
        while (entry && *reinterpret_cast<std::uint32_t*>(entry + kZoneEntryId) != zone) entry = *reinterpret_cast<std::uint8_t**>(entry);
        if (!entry) return nullptr;
        auto* holder = *reinterpret_cast<std::uint8_t**>(entry + kZoneEntryHolder);
        auto* data = holder ? *reinterpret_cast<std::uint8_t**>(holder + kHolderData) : nullptr;
        return data && data[kDataLoaded] ? data : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool ReadIds(void* instance, std::uint32_t& id, std::uint64_t& component, float at[3]) noexcept {
    __try {
        auto* bytes = static_cast<std::uint8_t*>(instance);
        id = std::uint32_t(*reinterpret_cast<std::uint64_t*>(bytes + kInstanceSlotId));
        component = *reinterpret_cast<std::uint64_t*>(bytes + kInstanceComponentId);
        std::memcpy(at, bytes + kInstancePosition, sizeof(float) * 3);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool BonePlaces(std::uint32_t count, const std::uint8_t* skeleton, float* places) noexcept {
    __try {
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint8_t* seg = skeleton + std::size_t(i) * kSegmentStride;
            const float* pivot = reinterpret_cast<const float*>(seg + kSegmentPivot);
            const int parent = *reinterpret_cast<const std::int32_t*>(seg + kSegmentParent);
            float* p = places + std::size_t(i) * 3;
            p[0] = pivot[0], p[1] = pivot[1], p[2] = pivot[2];
            if (parent >= 0 && std::uint32_t(parent) < i) {
                const float* q = places + std::size_t(parent) * 3;
                p[0] += q[0], p[1] += q[1], p[2] += q[2];
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The bind pose (cdc::AnimSegments_SetBindPose): rotation 0 (quaternion log), translation = the pivot, scale 1,
// weight 1.
bool WriteBindPose(std::uint32_t count, std::uint8_t* segs, const std::uint8_t* skeleton) noexcept {
    __try {
        for (std::uint32_t i = 0; i < count; ++i) {
            float* f = reinterpret_cast<float*>(segs + std::size_t(i) * kAnimSegmentStride);
            const float* pivot = reinterpret_cast<const float*>(skeleton + std::size_t(i) * kSegmentStride + kSegmentPivot);
            f[0] = f[1] = f[2] = f[3] = 0.0f;
            f[4] = pivot[0], f[5] = pivot[1], f[6] = pivot[2], f[7] = pivot[3];
            f[8] = f[9] = f[10] = f[11] = 1.0f;
            f[12] = 1.0f;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Drawn: an entity of another scene, or one whose drawable belongs to the model (see kRvaDrawEntity).
bool EntityShown(void* entity, void* scene, void* instance) noexcept {
    __try {
        auto* bytes = static_cast<std::uint8_t*>(entity);
        if (*reinterpret_cast<void**>(bytes + kEntityScene) != scene) return true;
        auto* drawable = *reinterpret_cast<std::uint8_t**>(bytes + kEntityDrawable);
        return instance && drawable && *reinterpret_cast<void**>(drawable + kDrawableOwner) == instance;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

// The layout the filter reads, on the model's anim drawable: 0 as expected; 1 no anim drawable; 2 its entity's
// drawable is not it; 3 its +0x10 is not the model; 4 its entity's scene is not the scene; -1 a fault.
int CheckIsolation(void* instance, void* scene) noexcept {
    __try {
        auto* bytes = static_cast<std::uint8_t*>(instance);
        if (*reinterpret_cast<std::uint32_t*>(bytes + kInstanceComponentCount) <= kDrawableComponent) return 1;
        auto** components = *reinterpret_cast<std::uint8_t***>(bytes + kInstanceComponents);
        std::uint8_t* component = components ? components[kDrawableComponent] : nullptr;
        auto* drawable = component ? *reinterpret_cast<std::uint8_t**>(component + kAnimDrawable) : nullptr;
        if (!drawable) return 1;
        auto* entity = *reinterpret_cast<std::uint8_t**>(drawable + kDrawableSceneEntity);
        if (!entity || *reinterpret_cast<void**>(entity + kEntityDrawable) != drawable) return 2;
        if (*reinterpret_cast<void**>(drawable + kDrawableOwner) != instance) return 3;
        if (*reinterpret_cast<void**>(entity + kEntityScene) != scene) return 4;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// The entity's world volume as a box when its drawable is the model's (see kEntityWorldVolume). SEH only.
bool EntityBox(void* entity, void* instance, float lo[3], float hi[3]) noexcept {
    __try {
        auto* bytes = static_cast<std::uint8_t*>(entity);
        auto* drawable = *reinterpret_cast<std::uint8_t**>(bytes + kEntityDrawable);
        if (!drawable || *reinterpret_cast<void**>(drawable + kDrawableOwner) != instance) return false;
        const float* v = reinterpret_cast<const float*>(bytes + kEntityWorldVolume);
        const int type = *reinterpret_cast<const std::int32_t*>(bytes + kEntityWorldVolume + kVolumeType);
        if (type == 0) {   // a sphere: x, y, z, r
            if (!(v[3] > 0.0f && v[3] < 1e5f)) return false;
            for (int k = 0; k < 3; ++k) lo[k] = v[k] - v[3], hi[k] = v[k] + v[3];
        } else if (type == 5) {   // a box: its half axes and its centre
            for (int k = 0; k < 3; ++k) {
                const float half = std::fabs(v[k]) + std::fabs(v[4 + k]) + std::fabs(v[8 + k]);
                if (!(half < 1e5f)) return false;
                lo[k] = v[12 + k] - half, hi[k] = v[12 + k] + half;
            }
        } else {
            return false;
        }
        for (int k = 0; k < 3; ++k)
            if (!std::isfinite(lo[k]) || !std::isfinite(hi[k])) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void NoteVolume(void* entity, void* instance) {
    float lo[3], hi[3];
    if (!EntityBox(entity, instance, lo, hi)) return;
    std::lock_guard lock(g_volumeMutex);
    WorldBox& b = g_worldBox;
    for (int k = 0; k < 3; ++k) {
        b.min[k] = b.count ? std::min(b.min[k], lo[k]) : lo[k];
        b.max[k] = b.count ? std::max(b.max[k], hi[k]) : hi[k];
    }
    ++b.count;
}

// The model's body: its DrawableComponent's anim drawable (Instance +0xD8 [8] +0x98), null while it has none. SEH only.
void* BodyDrawable(void* instance) noexcept {
    __try {
        auto* bytes = static_cast<std::uint8_t*>(instance);
        if (*reinterpret_cast<std::uint32_t*>(bytes + kInstanceComponentCount) <= kDrawableComponent) return nullptr;
        auto** components = *reinterpret_cast<std::uint8_t***>(bytes + kInstanceComponents);
        std::uint8_t* component = components ? components[kDrawableComponent] : nullptr;
        return component ? *reinterpret_cast<void**>(component + kAnimDrawable) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Which of the model's an entity is (see g_revealed): 0 not the model's, 1 one of its parts, 2 its body, 3 its hair.
// SEH only.
int ModelEntityKind(void* entity, void* instance) noexcept {
    __try {
        auto* drawable = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(entity) + kEntityDrawable);
        if (!drawable || *reinterpret_cast<void**>(drawable + kDrawableOwner) != instance) return 0;
        if (drawable == g_bodyDrawable.load(std::memory_order_relaxed)) return 2;
        const auto vtable = *reinterpret_cast<std::uintptr_t*>(drawable);
        if (g_hairVtableOk && vtable == g_base + kRvaHairDrawableVtable) return 3;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The strand hair's lights, its last draw's (for the log, see kEntityFlags). SEH only.
bool OwnLight(void* light);
std::atomic<std::uint32_t> g_hairLightsAll{0}, g_hairLightsStudio{0}, g_hairEntityFlags{0}, g_hairEntityMask{0},
    g_hairNodeFlags{0}, g_hairLightNotes{0};
// (v0.9.602) the strand hair's lights noted only until the model's 3 s line (all they are for): every hair draw walked
// its light set (33 chunks) for the rest of the model's time on screen
std::atomic<bool> g_noteHairLights{true};
void NoteHairLights(void* entity, void* node) noexcept {
    __try {
        const auto* e = static_cast<const std::uint8_t*>(entity);
        const auto* n = static_cast<const std::uint8_t*>(node);
        std::uint32_t all = 0, studio = 0;
        const std::uint8_t* set = n ? *reinterpret_cast<const std::uint8_t* const*>(n + kNodeLights) : nullptr;
        for (std::size_t c = 0; set && c < kLightChunks; ++c) {
            const std::uint8_t* chunk = set + c * kLightChunkSize;
            const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(chunk + kLightChunkCount);
            const auto* const* lights = *reinterpret_cast<const std::uint8_t* const* const*>(chunk);
            for (std::uint32_t i = 0; lights && i < count && i < 128; ++i) {
                ++all;
                if (lights[i] && OwnLight(const_cast<std::uint8_t*>(lights[i]) + kSceneLightInterface)) ++studio;
            }
        }
        g_hairLightsAll.store(all, std::memory_order_relaxed);
        g_hairLightsStudio.store(studio, std::memory_order_relaxed);
        g_hairEntityFlags.store(*reinterpret_cast<const std::uint32_t*>(e + kEntityFlags), std::memory_order_relaxed);
        g_hairEntityMask.store(*reinterpret_cast<const std::uint32_t*>(e + kEntityLightMask), std::memory_order_relaxed);
        g_hairNodeFlags.store(n ? *reinterpret_cast<const std::uint32_t*>(n + kNodeFlags) : 0, std::memory_order_relaxed);
        g_hairLightNotes.fetch_add(1, std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// (v0.9.602) the hook's own part timed on one call in 64 per thread (for the 30 s timing line): a thread's count, no
// shared counter per draw
std::atomic<std::uint64_t> g_drawHookSampleTicks{0};
std::atomic<std::uint32_t> g_drawHookSamples{0};
thread_local std::uint32_t t_drawHookCalls = 0;
bool DrawEntityOurs(void* entity, void* node);

void __fastcall HookDrawEntity(void* context, void* entity, void* node, void* a4, void* a5) {
    const bool sample = (++t_drawHookCalls & 63u) == 0;
    LARGE_INTEGER start{}, end{};
    if (sample) QueryPerformanceCounter(&start);
    const bool draw = DrawEntityOurs(entity, node);
    if (sample) {
        QueryPerformanceCounter(&end);
        g_drawHookSampleTicks.fetch_add(std::uint64_t(end.QuadPart - start.QuadPart), std::memory_order_relaxed);
        g_drawHookSamples.fetch_add(1, std::memory_order_relaxed);
    }
    if (draw) g_origDrawEntity(context, entity, node, a4, a5);
}

// Whether the entity is drawn (false: left out), and what the viewer notes of it.
bool DrawEntityOurs(void* entity, void* node) {
    if (g_noteHairLights.load(std::memory_order_relaxed))
        if (void* shown = g_isolateInstance.load(std::memory_order_acquire); shown && ModelEntityKind(entity, shown) == 3)
            NoteHairLights(entity, node);
    if (void* instance = g_boundsInstance.load(std::memory_order_acquire)) {
        // (v0.9.602) the drawn volume only until the model is revealed: UpdateDrawnBounds reads it only before (it was
        // noted, under a mutex, on every draw of the model's entities for as long as it was shown)
        if (!g_revealed.load(std::memory_order_acquire)) NoteVolume(entity, instance);
        if (!g_revealed.load(std::memory_order_acquire)) {   // (all of it at once, see g_revealed)
            switch (ModelEntityKind(entity, instance)) {
                case 2: g_bodyDraws.fetch_add(1, std::memory_order_relaxed); return false;
                case 3: g_hairDraws.fetch_add(1, std::memory_order_relaxed); return false;
                case 1: g_otherDraws.fetch_add(1, std::memory_order_relaxed); return false;
                default: break;
            }
        }
    }
    if (g_hideBackdrop.load(std::memory_order_acquire) && !g_isolate.load(std::memory_order_acquire)) {   // (v0.9.508)
        g_backdropHiddenDraws.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (g_isolate.load(std::memory_order_acquire)) {
        void* scene = g_isolateScene.load(std::memory_order_acquire);
        if (scene && !EntityShown(entity, scene, g_isolateInstance.load(std::memory_order_acquire))) {
            g_entitiesHidden.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        g_entitiesDrawn.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

bool HidesScene(void* scene) {
    if (g_hideBackdrop.load(std::memory_order_acquire) && !g_isolate.load(std::memory_order_acquire)) {   // (v0.9.508)
        g_backdropHiddenDraws.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return g_isolate.load(std::memory_order_acquire) && scene && scene == g_isolateScene.load(std::memory_order_acquire);
}

void __fastcall HookDrawStaticMeshes(void* scene, void* params) {
    if (HidesScene(scene)) {
        g_meshPassesHidden.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_origDrawStaticMeshes(scene, params);
}

void __fastcall HookDrawGrass(void* scene, void* params) {
    if (HidesScene(scene)) {
        g_meshPassesHidden.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_origDrawGrass(scene, params);
}

bool OwnLight(void* light) {
    for (const auto& own : g_ownLights)
        if (light && own.load(std::memory_order_acquire) == light) return true;
    return false;
}

bool __fastcall HookRendererRenderLight(void* renderer, void* light) {
    if (g_studioOnly.load(std::memory_order_acquire) && !OwnLight(light)) {
        g_gameLightsLeftOut.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return g_origRendererRenderLight(renderer, light);
}

// The render device's calls the Apply functions end with, with zeros (see kRvaApplyAmbient).
void* Device() noexcept {
    __try {
        return g_deviceGlobal ? *g_deviceGlobal : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
void* DeviceSlot(void* device, std::size_t slot) noexcept {
    __try {
        return (*reinterpret_cast<void***>(device))[slot];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
// The studio's ambient (v0.9.422): the Modding Suite's preset 0 environment (studio.h Environment: the sky above, the
// ground below, as its preview hands them to the game's light pass: SceneBuffer row 27 GlobalAmbient, row 38
// GlobalParams[10]), in place of the level's; its spherical harmonics' constant (0 there) and the beauty light stay 0.
constexpr float kStudioSky[3] = {0.20f, 0.21f, 0.24f}, kStudioGround[3] = {0.08f, 0.07f, 0.065f};
void SetStudioAmbient() noexcept {
    void* device = Device();
    auto set = device ? reinterpret_cast<void(__fastcall*)(void*, float, float, float)>(DeviceSlot(device, kDeviceSetAmbient)) : nullptr;
    if (!set) return;
    __try {
        set(device, kStudioSky[0], kStudioSky[1], kStudioSky[2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void SetStudioGround() noexcept {
    void* device = Device();
    auto set = device ? reinterpret_cast<void(__fastcall*)(void*, int, int, float)>(DeviceSlot(device, kDeviceSetGlobalParam)) : nullptr;
    if (!set) return;
    __try {
        for (int c = 0; c < 3; ++c) set(device, kParamSecondAmbient, c, kStudioGround[c]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
alignas(16) const float kZeroVectors[kIrradianceVectors * 4] = {};
void ZeroIrradiance() noexcept {
    void* device = Device();
    auto set = device ? reinterpret_cast<void(__fastcall*)(void*, const float*, bool)>(DeviceSlot(device, kDeviceSetIrradiance)) : nullptr;
    if (!set) return;
    __try {
        set(device, kZeroVectors, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void ZeroBeautyLight() noexcept {
    void* device = Device();
    auto set = device ? reinterpret_cast<void(__fastcall*)(void*, int, int, const float*)>(DeviceSlot(device, kDeviceSetGlobalVector))
                      : nullptr;
    if (!set) return;
    __try {
        set(device, kVectorBeautyColor, 1, kZeroVectors);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The relic viewer's environment (v0.9.427: see kRvaCollectibleInitLights): no ambient, its irradiance, its aperture.
void SetRelicAmbient() noexcept {
    void* device = Device();
    auto set = device ? reinterpret_cast<void(__fastcall*)(void*, float, float, float)>(DeviceSlot(device, kDeviceSetAmbient)) : nullptr;
    if (!set) return;
    __try {
        set(device, 0.0f, 0.0f, 0.0f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void SetRelicGround() noexcept {
    void* device = Device();
    auto set = device ? reinterpret_cast<void(__fastcall*)(void*, int, int, float)>(DeviceSlot(device, kDeviceSetGlobalParam)) : nullptr;
    if (!set) return;
    __try {
        for (int c = 0; c < 3; ++c) set(device, kParamSecondAmbient, c, 0.0f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void SetRelicIrradiance() noexcept {
    void* device = Device();
    auto set = device ? reinterpret_cast<void(__fastcall*)(void*, const float*, bool)>(DeviceSlot(device, kDeviceSetIrradiance)) : nullptr;
    if (!set) return;
    __try {
        set(device, g_relicIrradiance, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
bool SetRelicAperture() noexcept {
    __try {
        reinterpret_cast<void(__fastcall*)(float, float, float)>(g_base + kRvaSetApertureAdjustment)(1.0f, 0.0f, 0.0f);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void* RigObject() noexcept {
    __try {
        return g_rigObjectSlot ? *reinterpret_cast<void* const*>(g_rigObjectSlot) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
// The rig's bloom as the relic viewer's BloomRender gets it (InterpolateParams' copy into +0x80) and no tone mapping
// (+0xD8 0: SetupTonemapParams answers false). SEH only.
bool SetRelicTone(void* bloom) noexcept {
    auto* rig = static_cast<const std::uint8_t*>(RigObject());
    if (!rig) return false;
    __try {
        auto* b = static_cast<std::uint8_t*>(bloom);
        std::memcpy(b + kBloomInterpolated, rig + kRigBloom, kRigBloomSize);
        b[kBloomAdaptation] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall HookApplyAmbient(void* viewport) {
    g_origApplyAmbient(viewport);
    if (g_studioOnly.load(std::memory_order_acquire)) {
        if (RelicActive()) SetRelicAmbient();
        else SetStudioAmbient();
        g_ambientZeroed.fetch_add(1, std::memory_order_relaxed);
    }
}
void __fastcall HookApplySecondAmbient(void* viewport) {
    g_origApplySecondAmbient(viewport);
    if (g_studioOnly.load(std::memory_order_acquire)) {
        if (RelicActive()) SetRelicGround();
        else SetStudioGround();
    }
}
void __fastcall HookApplyIrradiance(void* viewport) {
    g_origApplyIrradiance(viewport);
    if (g_studioOnly.load(std::memory_order_acquire)) {
        if (RelicActive()) SetRelicIrradiance();
        else ZeroIrradiance();
    }
}
bool ReadTonemap(const void* bloom, float out[7]) noexcept {
    __try {
        auto* b = static_cast<const std::uint8_t*>(bloom);
        out[0] = *reinterpret_cast<const float*>(b + kBloomExposurePre);
        out[1] = *reinterpret_cast<const float*>(b + kBloomExposurePost);
        out[2] = b[kBloomAdaptation] ? 1.0f : 0.0f;
        out[3] = *reinterpret_cast<const float*>(b + kBloomAdaptPerc);
        out[4] = *reinterpret_cast<const float*>(b + kBloomShiftLimit);
        out[5] = *reinterpret_cast<const float*>(b + kBloomWhitePoint);
        out[6] = *reinterpret_cast<const float*>(b + kBloomAdaptSpeed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SetStudioTone(void* bloom) noexcept {
    __try {
        auto* b = static_cast<std::uint8_t*>(bloom);
        *reinterpret_cast<float*>(b + kBloomExposurePre) = std::log2(kStudioExposure);
        *reinterpret_cast<float*>(b + kBloomExposurePost) = 0.0f;
        b[kBloomAdaptation] = 1;
        *reinterpret_cast<float*>(b + kBloomAdaptPerc) = 0.0f;
        *reinterpret_cast<float*>(b + kBloomWhitePoint) = kStudioWhitePoint;
        b[kBloomScatterEnabled] = 0;
        b[kBloomBloomEnabled] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// (v0.9.509) The Cutscene Viewer's backdrop: its exposure dimmed, or its adaptation held while hidden. SEH only.
bool BackdropTone(void* bloom, bool hidden, float darkness) noexcept {
    __try {
        auto* b = static_cast<std::uint8_t*>(bloom);
        auto* post = reinterpret_cast<float*>(b + kBloomExposurePost);
        if (hidden) {
            b[kBloomAdaptation] = 0;   // (held: SetupTonemapParams answers false, the luminance pass not run)
            return true;
        }
        if (!(darkness > 0.0f)) {
            if (std::isfinite(*post)) g_backdropBasePost = *post, g_backdropBaseValid = true;
            return false;
        }
        if (!g_backdropBaseValid) return false;
        *post = g_backdropBasePost + std::log2(std::max(1.0f - darkness, 0.001f));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool __fastcall HookSetupTonemapParams(void* bloom) {
    if (!g_open.load(std::memory_order_acquire) &&   // (v0.9.509; the Model Viewer's own tone mapping not in use)
        BackdropTone(bloom, g_hideBackdrop.load(std::memory_order_acquire), g_backdropDarkness.load(std::memory_order_acquire)))
        g_backdropDimmed.fetch_add(1, std::memory_order_relaxed);
    if (g_open.load(std::memory_order_acquire)) {
        float seen[7];
        if (ReadTonemap(bloom, seen)) {
            std::lock_guard lock(g_tonemapMutex);
            std::memcpy(g_tonemapSeen, seen, sizeof(seen));
            g_tonemapSeenValid = true;
        }
    }
    const bool fixed = g_exposureFixed.load(std::memory_order_acquire), relic = fixed && RelicActive();
    if (relic) {
        if (SetRelicTone(bloom)) g_relicToneSet.fetch_add(1, std::memory_order_relaxed);
    } else if (fixed && SetStudioTone(bloom)) {
        g_adaptationsStopped.fetch_add(1, std::memory_order_relaxed);
    }
    const bool drawn = g_origSetupTonemap(bloom);
    if (fixed && !drawn) g_toneNotDrawn.fetch_add(1, std::memory_order_relaxed);
    if (relic && SetRelicAperture()) g_relicApertureSet.fetch_add(1, std::memory_order_relaxed);   // (after the game's)
    return drawn;
}

// The reflections' constants (see kRvaCubeReflConstantsDraw): the game's seen, then the Suite's. SEH only.
bool StudioReflections(void* drawable, float seen[4]) noexcept {
    __try {
        auto* c = static_cast<std::uint8_t*>(drawable) + kCubeReflConstants;
        auto* intensity = reinterpret_cast<float*>(c + kCubeReflIntensity);
        auto* influence = reinterpret_cast<float*>(c + kCubeReflUberInfluence);
        seen[0] = intensity[0], seen[1] = intensity[1], seen[2] = intensity[2], seen[3] = *influence;
        intensity[0] = intensity[1] = intensity[2] = kStudioReflections;
        *influence = 0.0f;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
using CubeReflConstantsDrawFn = void(__fastcall*)(void*, unsigned, void*);
CubeReflConstantsDrawFn g_origCubeReflConstantsDraw = nullptr;
std::atomic<std::uint32_t> g_reflectionsSet{0};
std::mutex g_reflectionMutex;
float g_reflectionSeen[4]{};   // the game's: intensity (r, g, b), the global cube map's influence
bool g_reflectionSeenValid = false;
void __fastcall HookCubeReflConstantsDraw(void* drawable, unsigned flags, void* list) {
    if (g_studioOnly.load(std::memory_order_acquire) && !RelicActive()) {   // (the relic lighting: the level's)
        float seen[4];
        if (StudioReflections(drawable, seen)) {
            g_reflectionsSet.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard lock(g_reflectionMutex);
            std::memcpy(g_reflectionSeen, seen, sizeof(seen));
            g_reflectionSeenValid = true;
        }
    }
    g_origCubeReflConstantsDraw(drawable, flags, list);
}

// The colour grading left out (see kRvaDoColorCorrection): not drawn, as the game's own path without grading.
using DoColorCorrectionFn = void(__fastcall*)(void*, void*, bool, bool, bool, bool, unsigned);
DoColorCorrectionFn g_origDoColorCorrection = nullptr;
bool g_colorCorrectionHooked = false;
std::atomic<std::uint32_t> g_gradingLeftOut{0}, g_gradingSeen{0};   // (the calls; the flags the game asked for, ORed)
void __fastcall HookDoColorCorrection(void* lens, void* list, bool tables, bool transform, bool grain, bool lut, unsigned passes) {
    if (g_exposureFixed.load(std::memory_order_acquire) && g_colorCorrectionDone) {
        g_gradingSeen.fetch_or((tables ? 1u : 0u) | (transform ? 2u : 0u) | (grain ? 4u : 0u) | (lut ? 8u : 0u),
                               std::memory_order_relaxed);
        g_gradingLeftOut.fetch_add(1, std::memory_order_relaxed);
        *g_colorCorrectionDone = 0;
        return;
    }
    g_origDoColorCorrection(lens, list, tables, transform, grain, lut, passes);
}

// The game's ambient occlusion off (see kRvaSsaoInterpolate): what the game asked for, then +0 cleared. SEH only.
using SsaoInterpolateFn = void(__fastcall*)(void*, void*, int);
SsaoInterpolateFn g_origSsaoInterpolate = nullptr;
bool g_ssaoHooked = false;
std::atomic<bool> g_aoOff{false};                                         // (UpdateIsolation: shown, the setting off)
std::atomic<bool> g_gameAo{true}, g_gameAoToggle{false};                  // [ModelViewer] AmbientOcclusion=; O
std::atomic<std::uint32_t> g_aoLeftOut{0}, g_aoSeenCount{0};
std::mutex g_aoMutex;
float g_aoSeen[6]{};   // the game's: enabled, strength, scene scale, range scale, power, its quality setting
std::uint8_t g_aoSeenColor[3]{};
int AoQuality() noexcept {
    __try {
        using ManagerFn = void*(__fastcall*)();
        using SettingsFn = std::uint8_t*(__fastcall*)(void*);
        void* manager = reinterpret_cast<ManagerFn>(g_base + kRvaDeviceManagerPtr)();
        if (!manager) return -1;
        const auto settings = (*reinterpret_cast<SettingsFn**>(manager))[kDeviceManagerSettingsSlot](manager);
        return settings ? *reinterpret_cast<const int*>(settings + kSettingsAoQuality) : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
bool ReadAndClearAo(void* params, bool clear, float seen[6], std::uint8_t color[3]) noexcept {
    __try {
        auto* p = static_cast<std::uint8_t*>(params);
        seen[0] = p[kSsaoEnabled] ? 1.0f : 0.0f;
        seen[1] = *reinterpret_cast<const float*>(p + kSsaoStrength);
        seen[2] = *reinterpret_cast<const float*>(p + kSsaoSceneScale);
        seen[3] = *reinterpret_cast<const float*>(p + kSsaoRangeScale);
        seen[4] = *reinterpret_cast<const float*>(p + kSsaoPower);
        std::memcpy(color, p + kSsaoColor, 3);
        if (clear) p[kSsaoEnabled] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void __fastcall HookSsaoInterpolate(void* params, void* a2, int unit) {
    g_origSsaoInterpolate(params, a2, unit);
    if (!g_isolateInstance.load(std::memory_order_acquire)) return;   // (a model shown)
    const bool clear = g_aoOff.load(std::memory_order_acquire);
    float seen[6];
    std::uint8_t color[3];
    if (!ReadAndClearAo(params, clear, seen, color)) return;
    if (clear && seen[0] != 0.0f) g_aoLeftOut.fetch_add(1, std::memory_order_relaxed);
    if ((g_aoSeenCount.fetch_add(1, std::memory_order_relaxed) & 63) == 0) {   // (the quality: a call, now and then)
        seen[5] = float(AoQuality());
        std::lock_guard lock(g_aoMutex);
        std::memcpy(g_aoSeen, seen, sizeof(seen));
        std::memcpy(g_aoSeenColor, color, sizeof(color));
    }
}

// No wind on the model (see kRvaGetWindForce).
using GetWindForceFn = float*(__fastcall*)(float*, const void*, void*, unsigned, int);
GetWindForceFn g_origGetWindForce = nullptr;
bool g_windHooked = false;
std::atomic<bool> g_windOff{false};                                       // (UpdateIsolation: shown, the setting off)
std::atomic<bool> g_wind{false};                                          // [ModelViewer] Wind= (v0.9.443: no key)
std::atomic<bool> g_uiHideToggle{false};   // H on the view: its UI hidden or shown (CutsceneViewer takes it, v0.9.436)
std::atomic<std::uint32_t> g_windsStopped{0};
std::atomic<float> g_windSeen{0.0f};   // the strength the game gave the model last (for the log)
bool ZeroWind(float* force) noexcept {
    __try {
        g_windSeen.store(std::sqrt(force[0] * force[0] + force[1] * force[1] + force[2] * force[2]), std::memory_order_relaxed);
        force[0] = force[1] = force[2] = force[3] = 0.0f;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
float* __fastcall HookGetWindForce(float* out, const void* position, void* fx, unsigned unit, int flags) {
    float* force = g_origGetWindForce(out, position, fx, unit, flags);
    if (force && g_windOff.load(std::memory_order_acquire)) {
        void* instance = g_isolateInstance.load(std::memory_order_acquire);
        if (instance && position == static_cast<const std::uint8_t*>(instance) + kInstancePosition && ZeroWind(force))
            g_windsStopped.fetch_add(1, std::memory_order_relaxed);
    }
    return force;
}

// The skin's subsurface scattering (v0.9.430: see kRvaModelViewFlag; v0.9.540: the flag's hook in RenderTweaks).
using SssDrawFn = void(__fastcall*)(void*, void*);
SssDrawFn g_origSssDraw = nullptr;
bool g_sssHooked = false;
std::atomic<bool> g_sssOn{false};                                                // (UpdateIsolation: a model shown)
// ---- Lara's outfits idle (v0.9.434; the face and the fidgets v0.9.435) ----
// As the player's action graph has her stand (laracroft.drm's graphs, the research doc): her body's locomotion "Idle",
// ground_idle; her face's mood "Default", face_neutral, on all its blend shapes; her eyes' "Eye_blink", face_eyes, on
// the eye bones (global 99 and 102) and the eye blend shapes (global 332..353, the ones it moves: the blinks); her
// hair's "Hair Idle", hair_ground_idle at half speed, on bones 305..309. The graph's face layering (267229) puts the
// mood over the pose on bone set 8 and the eyes over that on bone set 10, both Replace overlays, so the eyes' shapes
// replace the mood's. Played by InstanceAnim::PlayLayers over the bind pose the viewer writes (the bones none of them
// sets keep it). Now and then one of her idle fidgets (the idle state machine's ground_idle_look_around and _neck_rub;
// _playing_with_neckless left out in v0.9.436, it does not seem to be played in the game) is blended over the body
// (InstanceAnim::AddFidgets; the wait and the fades are the viewer's: the state machine's own timing was not read). The clips are laracroft.drm's (its
// animation library): when no loaded library has them, laracroft's object is loaded (a reference of the viewer's until
// it closes) and they are looked for again.
std::atomic<bool> g_outfitIdle{true}, g_outfitIdleToggle{false};   // [ModelViewer] OutfitIdle=; I
bool g_idlePlaying = false;
double g_idleTriedAt = -10.0, g_idleSince = 0.0;
int g_idleTries = 0;
std::uint32_t g_laraZone = 0;
bool g_laraZoneRef = false;
constexpr std::uint16_t kEyeBones[] = {99, 102};
constexpr std::uint16_t kEyeShapes[] = {332, 333, 334, 335, 336, 337, 338, 339, 340, 341, 342,
                                        343, 344, 345, 346, 347, 348, 349, 350, 351, 352, 353};
constexpr std::uint16_t kHairBones[] = {305, 306, 307, 308, 309};
constexpr float kFidgetMinGap = 8.0f, kFidgetMaxGap = 16.0f, kFidgetFade = 0.4f;   // seconds
// (v0.9.436) The characters stand in the same idle: the cdc bind poses have no rotations (a Segment's bind pose is its
// translation from its parent, +0x20), so the clips' rotations are theirs as they are; the translations the clips set
// (ground_idle's, the fidgets' tracks of these bones) are offsets from the bind translations of Lara's skeleton, the one
// they were made on (laracroft.drm's skeleton DTP 267639: 205 bones, its +0x28 / +0x30 global -> local map, the bones'
// +0x20), put onto the character's own (InstanceAnim::PlayLayers' reference). A character without one of Lara's bones
// (jonah_cine has no 2, his hips are on 3 under the root) leaves its tracks out.
const InstanceAnim::BindPivot kLaraBind[] = {
    {2, {0.0f, 1.55733585f, 87.8888626f}},
    {77, {-12.6697159f, 4.36899996f, 10.3327999f}},
    {78, {12.6693983f, 4.36900473f, 10.3327579f}},
    {80, {-37.3421173f, -7.45456028f, -33.3073425f}},
    {81, {37.3203278f, -7.4547596f, -33.3256989f}},
    {83, {0.0f, 0.0f, 0.0f}},
    {88, {-0.00157346402f, -1.2732749f, 4.3135376f}},
    {129, {0.0f, 0.0f, 0.0f}},
    {154, {0.0f, 0.0f, 0.0f}},
    {172, {0.0f, 0.0f, 112.0f}},
    {198, {-0.131578445f, 0.00438785553f, -0.11277771f}},
    {285, {0.33078289f, 0.0110387802f, -0.283683777f}},
    {290, {0.234403133f, -0.482159615f, 0.00144958496f}},
    {293, {-0.23190546f, -0.485645771f, 0.0f}},
    {296, {37.3203239f, -7.45475864f, -33.3256989f}},
    {297, {-37.3421211f, -7.45456028f, -33.3073425f}},
};
// (v0.9.441) The characters' own idles (chosen by the author): a clip of another object's animation library, by its
// animation ID as the Suite lists it, on the body; or three played in turn, an enter, a loop and an exit, the three
// looping as one (Konstantin's crossed arms). The face stays Lara's (face_neutral and face_eyes over the body: its blend
// shapes and eye bones as before), and they have no fidgets. The object with the clips is loaded for them (a reference
// of the viewer's until it closes); their translations are kept as offsets from that object's skeleton
// (InstanceAnim::ReferenceFromObject: its bind translations), each character keeping its proportions; an object without a
// skeleton (an animhost_*, which only hosts clips) gives none, its clips' translations played as they are, as the game
// plays them on its people.
struct CharacterIdle {
    const char* character;
    const char* source;            // the object whose animation library has the clips
    std::int16_t clips[3];
    int clipCount;                 // 1: one clip looping; 3: in turn, looping as one
    const char* names;             // (the clips' names in that library, for the log)
};
// (v0.9.442) Nadia (dlc_nadia_cine) is not one of them: she stands in Lara's idle, as the characters without one do.
const CharacterIdle kCharacterIdles[] = {
    {"jonah_cine", "enm_shield_manor", {611}, 1, "Hosted_Idle_Looping"},
    {"jonah_london_cine", "enm_shield_manor", {611}, 1, "Hosted_Idle_Looping"},
    {"jacob_cine", "enm_shield_manor", {611}, 1, "Hosted_Idle_Looping"},
    {"lord_croft_cine", "jacob_companion", {9098}, 1, "Ground_LowAlert_Idle_C"},
    {"syrian_driver", "enm_shield_manor", {253}, 1, "Ground_LowAlert_Idle_F"},
    {"trinity_agent", "enm_shield_manor", {253}, 1, "Ground_LowAlert_Idle_F"},   // (v0.9.511) the Turkish Guide's
    {"sofia_cine", "enm_shield_manor", {246}, 1, "Ground_LowAlert_Idle_M"},
    {"konstantin_combat_cine", "animhost_npc_human_multipose_listen", {2197, 2198, 2196}, 3,
     "Dialogue_Idle2_CrossedArmsEnter > Dialogue_Idle2_CrossedArms > Dialogue_Idle2_CrossedArmsExit"},
    {"konstantin_jacket_cine", "animhost_npc_human_multipose_listen", {2197, 2198, 2196}, 3,
     "Dialogue_Idle2_CrossedArmsEnter > Dialogue_Idle2_CrossedArms > Dialogue_Idle2_CrossedArmsExit"},
    {"ana_cine", "ana_companion", {1024}, 1, "Ana_Idle_Default"},
    {"dlc_by_ivan_puppet_parent", "enm_shield_manor", {254}, 1, "Ground_LowAlert_Idle_E"},
    {"witchcup_witchywitch_cine", "animhost_npc_human_standing_missiongiver_general", {2321}, 1, "Hosted_Idle_LoopingB"},
    {"remnant_warrior_cine", "ana_companion", {1021}, 1, "Hosted_Idle_Looping"},
};
const CharacterIdle* CharacterIdleOf(const std::string& object) {
    for (const auto& idle : kCharacterIdles)
        if (_stricmp(idle.character, object.c_str()) == 0) return &idle;
    return nullptr;
}
// The objects loaded for their clips (their zones' references, released when the viewer closes).
struct IdleSource {
    std::string name;
    std::uint32_t zone = 0;
    bool ref = false;
    bool failed = false;
};
std::vector<IdleSource> g_idleSources;
std::atomic<std::uint32_t> g_sssDraws{0};
void __fastcall HookSssDraw(void* sss, void* list) {
    g_sssDraws.fetch_add(1, std::memory_order_relaxed);
    g_origSssDraw(sss, list);
}

void __fastcall HookUpdateBeautySpecular(void* viewport) {
    g_origUpdateBeautySpecular(viewport);
    if (g_studioOnly.load(std::memory_order_acquire)) ZeroBeautyLight();
}

// ---- The model's size ---------------------------------------------------------------------------------------------------
bool StartsWith(const std::string& s, const char* prefix) { return _strnicmp(s.c_str(), prefix, std::strlen(prefix)) == 0; }

// Its skeleton's bones in the bind pose (each pivot from its parent's), a little more around them.
void MeasureModel(void* instance) {
    Bounds b;
    const std::uint8_t* segments = nullptr;
    const std::uint32_t count = SkeletonOf(instance, segments);
    if (count > 1) {
        std::vector<float> places(std::size_t(count) * 3);
        if (BonePlaces(count, segments, places.data())) {
            b.valid = true;
            b.segments = count;
            for (int k = 0; k < 3; ++k) b.min[k] = b.max[k] = places[std::size_t(k)];
            for (std::uint32_t i = 0; i < count; ++i)
                for (int k = 0; k < 3; ++k) {
                    const float v = places[std::size_t(i) * 3 + std::size_t(k)];
                    if (!std::isfinite(v)) continue;
                    b.min[k] = std::min(b.min[k], v);
                    b.max[k] = std::max(b.max[k], v);
                }
            for (int k = 0; k < 3; ++k) {   // the bones are inside the skin
                const float pad = (b.max[k] - b.min[k]) * 0.08f + 4.0f;
                b.min[k] -= pad;
                b.max[k] += pad;
            }
        }
    }
    g_bounds = b;
    if (b.valid)
        Log::Detailf("ModelViewer: %s: %u bones, bind pose box (%.0f, %.0f, %.0f) to (%.0f, %.0f, %.0f) cm", ModelName().c_str(),
                    count, b.min[0], b.min[1], b.min[2], b.max[0], b.max[1], b.max[2]);
    else
        Log::Detailf("ModelViewer: %s: %s: sized by its kind", ModelName().c_str(), count == 1 ? "one bone" : "no skeleton");
}

// The drawn bounds (see kEntityWorldVolume) about the model's place, once its entities were drawn (game thread).
// True when they changed.
bool UpdateDrawnBounds() {
    WorldBox box;
    {
        std::lock_guard lock(g_volumeMutex);
        box = g_worldBox;
    }
    if (!box.count) return false;
    Bounds b;
    b.valid = true;
    for (int k = 0; k < 3; ++k) b.min[k] = box.min[k] - g_at[k], b.max[k] = box.max[k] - g_at[k];
    bool changed = !g_drawnBounds.valid;
    for (int k = 0; k < 3 && !changed; ++k)
        changed = std::fabs(b.min[k] - g_drawnBounds.min[k]) > 0.5f || std::fabs(b.max[k] - g_drawnBounds.max[k]) > 0.5f;
    if (!changed) return false;
    const bool first = !g_drawnBounds.valid;
    g_drawnBounds = b;
    if (first)
        Log::Detailf("ModelViewer: %s: drawn bounds (%.0f, %.0f, %.0f) to (%.0f, %.0f, %.0f) cm about its place", ModelName().c_str(),
                    b.min[0], b.min[1], b.min[2], b.max[0], b.max[1], b.max[2]);
    return true;
}

void ResetDrawnBounds() {
    std::lock_guard lock(g_volumeMutex);
    g_worldBox = WorldBox{};
    g_drawnBounds = Bounds{};
}

// The model's middle (cm, from its origin) and the radius of a ball around it (v0.9.422): the Suite's bounds, else its
// bones' (a skinned model), else its drawn bounds (a rigid one), else by its kind.
// (v0.9.436) A rigid model the game draws far larger than the Suite measured it gets its drawn bounds (a rigid
// model's are its meshes' as drawn: every other relic's are the Suite's to the centimetre). relic_cm_crowbar: drawn
// (-999, -1155, -2353) to (994, 1203, 1573) cm, the Suite's (-35, -41, -83) to (35, 42, 55); on the view, 3146 cm away
// with the menu's 32 degrees (1804 cm high there), its hook and grip filled the height: about 40 m long, as drawn.
float BoundsRadius(const Bounds& b) {
    float sum = 0.0f;
    for (int k = 0; k < 3; ++k) sum += (b.max[k] - b.min[k]) * (b.max[k] - b.min[k]);
    return 0.5f * std::sqrt(sum);
}
constexpr float kDrawnLarger = 4.0f;
const Bounds* SizeSource(const char** what = nullptr) {
    const char* name = "its kind";
    const Bounds* b = nullptr;
    if (g_modelSuiteBounds.valid) {
        b = &g_modelSuiteBounds, name = "the Suite's bounds";
        if (!g_bounds.valid && g_drawnBounds.valid &&
            BoundsRadius(g_drawnBounds) > kDrawnLarger * BoundsRadius(g_modelSuiteBounds))
            b = &g_drawnBounds, name = "its drawn bounds (far larger than the Suite's)";
    } else if (g_bounds.valid) b = &g_bounds, name = "its bones";
    else if (g_drawnBounds.valid) b = &g_drawnBounds, name = "its drawn bounds";
    if (what) *what = name;
    return b;
}
void ModelBox(float center[3], float& radius) {
    if (const Bounds* b = SizeSource()) {
        float extent[3];
        for (int k = 0; k < 3; ++k) center[k] = (b->min[k] + b->max[k]) * 0.5f, extent[k] = b->max[k] - b->min[k];
        radius = 0.5f * std::sqrt(extent[0] * extent[0] + extent[1] * extent[1] + extent[2] * extent[2]);
        if (radius >= 1.0f) return;
    }
    // no bones to measure (a rigid object), or before it is born: by the kind of object
    const std::string& name = ModelName();
    center[0] = center[1] = 0.0f;
    if (StartsWith(name, "relic_") || StartsWith(name, "doc_")) radius = 20.0f, center[2] = 0.0f;
    else if (StartsWith(name, "wp_") || StartsWith(name, "weapon")) radius = 40.0f, center[2] = 0.0f;
    else if (StartsWith(name, "trx_") || StartsWith(name, "animal")) radius = 120.0f, center[2] = 50.0f;
    else radius = 115.0f, center[2] = 90.0f;
}

// ---- The place, the camera, the lights ----------------------------------------------------------------------------------
void PlaceFor(int place, float out[3]) {
    const float* c = g_menuCamera.position;
    out[0] = c[0], out[1] = c[1], out[2] = c[2];
    switch (place) {
        case kPlaceFront: {   // ahead along the menu camera's level forward, a standing figure's middle at its height
            const float* q = g_menuCamera.quaternion;
            float fx = 2.0f * (q[0] * q[1] - q[2] * q[3]), fy = 1.0f - 2.0f * (q[0] * q[0] + q[2] * q[2]);
            const float length = std::sqrt(fx * fx + fy * fy);
            if (length > 1e-4f) fx /= length, fy /= length;
            else fx = 0.0f, fy = 1.0f;
            out[0] += fx * g_ahead, out[1] += fy * g_ahead, out[2] -= 90.0f;
            break;
        }
        case kPlaceBelow: out[2] -= g_height; break;
        default: out[2] += g_height; break;
    }
}

// The camera about the model: yaw about the vertical, pitch from above, at the distance that shows the ball around the
// model whole (the menu's field of view), moved with the model's pan. The quaternion as the free camera builds it
// (cdc: +Y forward, +X right, +Z up; Rz(-heading) Rx(pitch)).
// (v0.9.436: 200 m, from 50: relic_cm_crowbar's ball, 25 m, asks for 100 m)
constexpr float kMaxDistance = 20000.0f;
void UpdateCamera() {
    View v;
    {
        std::lock_guard lock(g_viewMutex);
        v = g_view;
    }
    float center[3], radius;
    ModelBox(center, radius);
    // turned: the camera ahead of the model's front, its middle turned with it (the model turns about its origin)
    float heading = v.yaw * kPi / 180.0f;
    if (g_modelTurns) {
        heading = 0.0f;
        const float c = std::cos(g_turned), sn = std::sin(g_turned), x = center[0], y = center[1];
        center[0] = x * c - y * sn, center[1] = x * sn + y * c;
    }
    const float tilt = -v.pitch * kPi / 180.0f;   // (looking down when above)
    const float sh = std::sin(heading), ch = std::cos(heading), st = std::sin(tilt), ct = std::cos(tilt);
    Frame f;
    f.forward[0] = ct * sh, f.forward[1] = ct * ch, f.forward[2] = st;
    f.right[0] = ch, f.right[1] = -sh, f.right[2] = 0.0f;
    f.up[0] = f.right[1] * f.forward[2] - f.right[2] * f.forward[1];
    f.up[1] = f.right[2] * f.forward[0] - f.right[0] * f.forward[2];
    f.up[2] = f.right[0] * f.forward[1] - f.right[1] * f.forward[0];
    const float half = std::clamp(g_fov, 0.2f, 2.5f) * 0.5f;
    f.radius = radius;
    f.distance = std::clamp(radius / std::sin(half) * 1.1f, 15.0f, kMaxDistance) * v.zoom;
    // the grid: the model on the right of the picture (its middle at g_gridModelX of the width, in NDC), in
    // g_gridModelHalf of it each way (the ball the distance shows whole: about 0.87 of the height each way, so
    // 0.87 / aspect of the width)
    float shift = 0.0f;
    if (g_screen.load() == Screen::Grid) {
        const float aspect = std::clamp(g_aspect.load(), 0.5f, 4.0f);
        f.distance *= std::max(1.0f, 0.87f / aspect / g_gridModelHalf);
        shift = g_gridModelX * f.distance * std::tan(half) * aspect;
    }
    g_distanceNow.store(f.distance);
    for (int k = 0; k < 3; ++k) {
        f.center[k] = g_at[k] + center[k];
        const float target = f.center[k] - f.right[k] * (v.panX + shift) - f.up[k] * v.panZ;
        f.position[k] = target - f.forward[k] * f.distance;
    }
    const float hh = heading * 0.5f, ht = tilt * 0.5f;
    const float shh = std::sin(hh), chh = std::cos(hh), sht = std::sin(ht), cht = std::cos(ht);
    const float q[4] = {chh * sht, -shh * sht, -shh * cht, chh * cht};
    Features::SetViewerCamera(f.position, q);
    g_frame = f;
}

Features::LightSettings StudioSettings(int i) {
    const StudioLight& s = kStudio[i];
    const Frame& f = g_frame;
    const float az = s.azimuth * kPi / 180.0f, el = s.elevation * kPi / 180.0f;
    const float away = std::max(kLightAway, f.radius * 8.0f);
    Features::LightSettings l{};
    l.type = 0;   // point
    for (int k = 0; k < 3; ++k) {
        const float toLight = -f.forward[k] * std::cos(az) * std::cos(el) - f.right[k] * std::sin(az) * std::cos(el) +
                              f.up[k] * std::sin(el);
        l.position[k] = f.center[k] + toLight * away;
        l.color[k] = s.color[k];
    }
    l.intensity = s.intensity * g_brightness.load();
    l.range = away * kLightRangeFactor;
    l.enabled = g_lightsOn.load();
    l.shadowEnabled = false;
    l.lambertWeight = 1.0f, l.halfLambertWeight = 0.0f, l.ambientWeight = 0.0f, l.specularStrength = 1.0f;
    return l;
}

// ---- The relic viewer's lights (v0.9.427: see kRvaCollectibleInitLights) ----
bool ReadFloats(const float* from, float* out, int n) noexcept {
    __try {
        for (int i = 0; i < n; ++i) out[i] = from[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The rig's light as InitLights converts it (0x871F20 on a copy of its template at +0xD8), its rotation and position
// and its complexity. SEH only.
bool RigLightRaw(int i, std::uint8_t* properties, float rotation[3], float position[3], int& complexity) noexcept {
    auto* rig = static_cast<const std::uint8_t*>(RigObject());
    if (!rig) return false;
    __try {
        const std::uint8_t* light = rig + kRigLights[i];
        alignas(16) std::uint8_t build[kRigBuildTemplate + kRigTemplateSize] = {};
        std::memcpy(build + kRigBuildTemplate, light + kRigLightTemplate, kRigTemplateSize);
        std::memcpy(rotation, light + kRigLightRotation, 3 * sizeof(float));
        std::memcpy(position, light + kRigLightPosition, 3 * sizeof(float));
        complexity = *reinterpret_cast<const int*>(light + kRigLightTemplate + kTemplateComplexity);
        std::memset(properties, 0, Features::kViewerLightPropertiesSize);
        reinterpret_cast<void(__fastcall*)(void*, const void*)>(g_base + kRvaRigLightProperties)(properties, build);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Its axes as InitLights builds them: cdc::Matrix::Build of cdc::Quat::Build of the rotation in radians. SEH only.
bool RigAxes(const float rotation[3], float axes[3][3]) noexcept {
    __try {
        alignas(16) float euler[4] = {rotation[0] * kPi / 180.0f, rotation[1] * kPi / 180.0f, rotation[2] * kPi / 180.0f, 0.0f};
        alignas(16) float quat[4] = {};
        alignas(16) float m[16] = {};
        using BuildFn = void*(__fastcall*)(float*, const float*);
        reinterpret_cast<BuildFn>(g_base + kRvaQuatFromEuler)(quat, euler);
        reinterpret_cast<BuildFn>(g_base + kRvaMatrixFromQuat)(m, quat);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) axes[r][c] = m[r * 4 + c];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// (dtp::LightTemplateData's LightType and LightComplexity, TR2 PDB)
const char* RigLightTypeName(int type) {
    static const char* const kNames[] = {"point", "capsule", "spot", "capsule spot", "box", "directional", "directional fill"};
    return type >= 0 && type <= 6 ? kNames[type] : "?";
}
const char* RigComplexityName(int complexity) {
    static const char* const kNames[] = {"key", "universal", "fill", "dark", "wet", "player", "lowres", "fire", "refl"};
    return complexity >= 0 && complexity <= 8 ? kNames[complexity] : "?";
}
// The rig read again (each time the lights are made): false when the global object or a light can't be read.
bool ReadRig() {
    float offset[3]{}, place[3]{};
    if (!g_rigOffset || !g_relicPlace || !ReadFloats(g_rigOffset, offset, 3) || !ReadFloats(g_relicPlace, place, 3)) return false;
    for (int i = 0; i < 3; ++i) {
        RigLight& r = g_rig[i];
        if (!RigLightRaw(i, r.properties, r.rotation, r.position, r.complexity) || !RigAxes(r.rotation, r.axes) ||
            !Features::ViewerLightSettingsFrom(r.properties, r.base))
            return false;
        for (int k = 0; k < 3; ++k) r.place[k] = r.position[k] + offset[k];   // (x the +0x60 translation)
        std::fill(std::begin(r.matrix), std::end(r.matrix), 0.0f);
    }
    float relic[3];
    for (int k = 0; k < 3; ++k) relic[k] = place[k] + offset[k];
    g_relicDistance = std::max(1.0f, std::sqrt(relic[0] * relic[0] + relic[1] * relic[1] + relic[2] * relic[2]));
    auto* rig = static_cast<const std::uint8_t*>(RigObject());
    float bloom[kRigBloomSize / 4]{};
    const bool bloomRead = rig && ReadFloats(reinterpret_cast<const float*>(rig + kRigBloom), bloom, int(kRigBloomSize / 4));
    std::uint8_t flags[2]{};
    if (bloomRead) std::memcpy(flags, bloom + 8, 1), std::memcpy(flags + 1, bloom + 13, 1);   // (+0x20, +0x34)
    Log::Detailf("ModelViewer: the relic viewer's rig (global object %p): the view's offset (%.0f, %.0f, %.0f), the relic at "
                "(%.0f, %.0f, %.0f) + that, %.0f cm from its camera; its bloom: %s (sky %.2f, sun %.2f; light scatter %s, "
                "intensity %.2f; bloom %s, strength %.2f, threshold %.2f, radius %.2f)", static_cast<const void*>(rig),
                offset[0], offset[1], offset[2], place[0], place[1], place[2], g_relicDistance, bloomRead ? "read" : "not read",
                bloom[4], bloom[7], flags[0] ? "on" : "off", bloom[10], flags[1] ? "on" : "off", bloom[14], bloom[15], bloom[16]);
    for (int i = 0; i < 3; ++i) {
        const RigLight& r = g_rig[i];
        const Features::LightSettings& b = r.base;
        Log::Detailf("ModelViewer: the rig's light %d: %s (type %d), complexity %s (%d), range %.0f, intensity %.2f, colour %.2f "
                    "%.2f %.2f, cone %.2f / %.2f, near %.0f, lambert %.2f, half lambert %.2f, ambient %.2f, specular %.2f, "
                    "shadows %s; rotation (%.1f, %.1f, %.1f), position (%.0f, %.0f, %.0f), its axes (%.2f %.2f %.2f | %.2f %.2f "
                    "%.2f | %.2f %.2f %.2f)", i, RigLightTypeName(b.type), b.type, RigComplexityName(r.complexity), r.complexity,
                    b.range, b.intensity, b.color[0], b.color[1], b.color[2],
                    b.umbra, b.penumbra, b.nearRadius, b.lambertWeight, b.halfLambertWeight, b.ambientWeight,
                    b.specularStrength, b.shadowEnabled ? "on (left out)" : "off", r.rotation[0], r.rotation[1], r.rotation[2],
                    r.position[0], r.position[1], r.position[2], r.axes[0][0], r.axes[0][1], r.axes[0][2], r.axes[1][0],
                    r.axes[1][1], r.axes[1][2], r.axes[2][0], r.axes[2][1], r.axes[2][2]);
    }
    return true;
}
// Light i of the rig about the viewer's camera as about the relic viewer's (its right, forward and up the camera's),
// scaled by the cameras' distances to what they look at; matrix: rows (its axes, its position).
Features::LightSettings RigSettings(int i, float matrix[16]) {
    const RigLight& r = g_rig[i];
    const Frame& f = g_frame;
    const float scale = std::max(0.01f, f.distance / g_relicDistance);
    const auto world = [&f](const float v[3], float out[3]) {
        for (int k = 0; k < 3; ++k) out[k] = v[0] * f.right[k] + v[1] * f.forward[k] + v[2] * f.up[k];
    };
    Features::LightSettings l = r.base;
    float place[3];
    world(r.place, place);
    for (int row = 0; row < 3; ++row) {
        world(r.axes[row], matrix + row * 4);
        matrix[row * 4 + 3] = 0.0f;
    }
    for (int k = 0; k < 3; ++k) l.position[k] = matrix[12 + k] = f.position[k] + place[k] * scale;
    matrix[15] = 1.0f;
    l.rotation[0] = l.rotation[1] = l.rotation[2] = 0.0f;
    l.range = r.base.range * scale;
    if (r.base.nearRadius >= 1.0f) l.nearRadius = std::max(1.0f, r.base.nearRadius * scale);
    l.intensity = r.base.intensity * g_brightness.load();
    l.enabled = g_lightsOn.load();
    l.shadowEnabled = false;
    return l;
}

// ---- The lights kept in a cell (v0.9.429) ----
// The game lights with a light only while its place is in one of the scene's cells (see Features::ViewerLightInCell):
// cdc::Scene::UpdateLight (retail 0x381830) gives a light the cell at its position, and Scene::TraverseForLights
// (retail 0x37BF10) passes a light without one by (0x37C220): no deferred light, no forward light (0.9.428's log: the
// lights in no cell 1811 times in 3 s of 2844; its video: the cross pitch black whenever the view put the rig's two
// spots out of the level's cells, 0.9.427's: the dice lit by the refl light alone). The model is in a cell (its body's,
// the log's "scene cells" line), the lights 15 m and more away (the studio's, the relic rig's scaled by the camera's
// distance) often are not. So a light out of every cell is moved towards what it lights along the same line (its range
// and near radius with it, so the light there is the same), as far as a cell allows: the fraction of its distance kept
// is tried back towards 1 every 0.3 s.
constexpr float kFitMin = 0.02f, kFitStep = 0.6f, kFitGrow = 1.5f;
constexpr double kFitGrowEvery = 0.3;
float g_lightFit[3] = {1.0f, 1.0f, 1.0f};   // the fraction of its distance each light is at
double g_lightGrownAt[3] = {};
std::uint32_t g_lightFitMoves = 0, g_lightFitFailed = 0, g_lightFitFrames = 0;   // (the log: in 3 s)
float g_lightFitLeast[3] = {1.0f, 1.0f, 1.0f};
// Light i where it is wanted, then at fraction s of its distance from what it lights; false: not in a cell after it.
bool PlaceLight(int i, float s, bool relic) {
    float matrix[16];
    Features::LightSettings l = relic ? RigSettings(i, matrix) : StudioSettings(i);
    const Frame& f = g_frame;
    float about[3];
    for (int k = 0; k < 3; ++k) about[k] = relic ? f.position[k] + f.forward[k] * f.distance : f.center[k];
    for (int k = 0; k < 3; ++k) {
        l.position[k] = about[k] + s * (l.position[k] - about[k]);
        matrix[12 + k] = l.position[k];
    }
    l.range *= s;
    if (l.nearRadius >= 1.0f) l.nearRadius = std::max(1.0f, l.nearRadius * s);
    if (relic) {   // (the matrix passed on when it changed)
        const bool moved = std::memcmp(matrix, g_rig[i].matrix, sizeof(matrix)) != 0;
        if (Features::ViewerLightUpdate(g_lights[i], l, moved ? matrix : nullptr) && moved)
            std::memcpy(g_rig[i].matrix, matrix, sizeof(matrix));
    } else {
        Features::ViewerLightUpdate(g_lights[i], l);
    }
    return Features::ViewerLightInCell(g_lights[i]);
}
void FitLight(int i, bool relic, double now) {
    float s = g_lightFit[i];
    if (s < 1.0f && now - g_lightGrownAt[i] >= kFitGrowEvery) {
        g_lightGrownAt[i] = now;
        s = std::min(1.0f, s * kFitGrow);
    }
    while (!PlaceLight(i, s, relic)) {
        ++g_lightFitMoves;
        if (s <= kFitMin) {
            ++g_lightFitFailed;
            break;
        }
        s = std::max(kFitMin, s > g_lightFit[i] ? g_lightFit[i] : s * kFitStep);
    }
    g_lightFit[i] = s;
    g_lightFitLeast[i] = std::min(g_lightFitLeast[i], s);
    if (i == 0) ++g_lightFitFrames;
}

// What FitLight did since the last time (the 3 s line; and when the lights are released).
void LogLightFit() {
    if (!g_lightFitFrames) return;
    const bool relic = RelicActive();
    const char* const names[3] = {relic ? "light 0" : "the key", relic ? "light 1" : "the fill", relic ? "light 2" : "the rim"};
    Log::Writef("ModelViewer: the lights kept in a cell: %s at %.2f of its distance now (%.2f at least), %s at %.2f (%.2f), %s "
                "at %.2f (%.2f); %u moves towards the model over %u frames, %u times in no cell even beside it", names[0],
                g_lightFit[0], g_lightFitLeast[0], names[1], g_lightFit[1], g_lightFitLeast[1], names[2], g_lightFit[2],
                g_lightFitLeast[2], g_lightFitMoves, g_lightFitFrames, g_lightFitFailed);
    g_lightFitMoves = g_lightFitFailed = g_lightFitFrames = 0;
    for (float& least : g_lightFitLeast) least = 1.0f;
}

void ReleaseLights() {
    if (g_lightsMade) LogLightFit();
    g_relicActive.store(false, std::memory_order_release);
    for (int i = 0; i < 3; ++i) g_lightFit[i] = g_lightFitLeast[i] = 1.0f, g_lightGrownAt[i] = 0.0;
    for (auto& own : g_ownLights) own.store(nullptr, std::memory_order_release);
    if (g_lightsMade) Features::ViewerLightsRelease();
    g_lightsMade = false;
    g_lightScene = nullptr;
    for (int& i : g_lights) i = -1;
}

// Made in the scene the model is drawn in (once it is in one), kept while the next model is in the same scene; then
// moved with the camera every frame (only what changed is passed on).
// The model shown is one of the game's relics (v0.9.430: lit by the relic viewer's lights, the others by the studio's).
bool ShownIsRelic() {
    return g_shownCategory >= 0 && g_shownCategory < int(g_categories.size()) &&
           g_categories[std::size_t(g_shownCategory)].kind == Kind::Relics;
}
// ... one of our characters (v0.9.436: they stand in Lara's idle too, see StartIdle)
bool ShownIsCharacter() {
    return g_shownCategory >= 0 && g_shownCategory < int(g_categories.size()) &&
           g_categories[std::size_t(g_shownCategory)].kind == Kind::Characters;
}
// The model shown stands in Lara's idle (with g_outfitIdle): her outfits (player objects) and the characters.
bool ShownIdles() { return g_shownPlayer || ShownIsCharacter(); }

void UpdateLights(double now) {
    if (!Features::ViewerLightsAvailable()) return;
    if (g_instance && g_lightsMade && ShownIsRelic() != g_lightsForRelic) {   // (a relic after another model, or the reverse)
        ReleaseLights();
        g_lightsTriedAt = -10.0;
    }
    // (made: its scene checked each second; not yet: tried every 0.1 s, for 10 s a model: v0.9.433, the tries were counted
    // over all the models before, so that after a few the lights gave up until the viewer was opened again)
    if (g_instance && !g_lightsGaveUp && now - g_lightsTriedAt >= (g_lightsMade ? 1.0 : 0.1)) {
        void* scene = Features::ViewerSceneOf(g_instance);
        if (g_lightsMade && scene && scene != g_lightScene) {
            Log::Writef("ModelViewer: %s is drawn in another scene (%p, the lights' %p): the lights made again",
                        ModelName().c_str(), scene, g_lightScene);
            ReleaseLights();
        }
        if (!g_lightsMade) {
            g_lightsTriedAt = now;
            if (!scene) {
                if (++g_lightTries >= 100) {
                    g_lightsGaveUp = true;
                    Log::Writef("ModelViewer: %s is in no scene the lights can be made in (10 s); no lights on it",
                                ModelName().c_str());
                }
            } else {
                bool relic = g_lightsForRelic = ShownIsRelic();
                if (relic && !ReadRig()) {
                    relic = false;
                    Log::Write("ModelViewer: the relic viewer's rig could not be read (its global object or a light): the studio "
                               "lights instead");
                    SetStatus("The relic viewer's lights could not be read (the log says why): the studio lights instead.");
                }
                int made = 0;
                for (int i = 0; i < 3; ++i) {
                    if (relic) {
                        float matrix[16];
                        const Features::LightSettings l = RigSettings(i, matrix);
                        g_lights[i] = Features::ViewerLightCreate(scene, l, g_rig[i].properties, matrix);
                        std::memcpy(g_rig[i].matrix, matrix, sizeof(matrix));
                        if (g_lights[i] >= 0 && g_rig[i].complexity == kComplexityReflection)   // (v0.9.428)
                            Log::Detailf("ModelViewer: the rig's light %d (complexity refl) %s", i,
                                        Features::ViewerLightSetReflectionOnly(g_lights[i])
                                            ? "made a reflection light, as the game's (no forward lighting)"
                                            : "could not be made a reflection light: it lights as any other");
                    } else {
                        g_lights[i] = Features::ViewerLightCreate(scene, StudioSettings(i));
                    }
                    if (g_lights[i] >= 0) ++made;
                }
                g_lightsMade = true;
                g_relicActive.store(relic, std::memory_order_release);
                g_lightScene = scene;
                for (int i = 0; i < 3; ++i)   // (the lights LightRenderer::RenderLight lets through: see g_studioOnly)
                    g_ownLights[i].store(g_lights[i] >= 0 ? Features::ViewerLightHandle(g_lights[i]) : nullptr,
                                         std::memory_order_release);
                if (made && !Features::DeferredLightsAvailable())
                    Log::Write("ModelViewer: the framework's lights are not drawn deferred (the log's Features lines say why): "
                               "the studio lights light the forward materials only");
                if (relic) {
                    float matrix[16];
                    const Features::LightSettings key = RigSettings(0, matrix);
                    Log::Detailf("ModelViewer: %d of 3 relic viewer's lights made in scene %p (x %.3f: the camera %.0f cm from "
                                "what it looks at; light 0 at (%.0f, %.0f, %.0f), range %.0f, intensity %.2f)", made, scene,
                                std::max(0.01f, g_frame.distance / g_relicDistance), g_frame.distance, key.position[0],
                                key.position[1], key.position[2], key.range, key.intensity);
                } else {
                    const Features::LightSettings key = StudioSettings(0);
                    Log::Detailf("ModelViewer: %d of 3 studio lights made in scene %p (the key at (%.0f, %.0f, %.0f), range %.0f, "
                                "intensity %.2f)", made, scene, key.position[0], key.position[1], key.position[2], key.range,
                                key.intensity);
                }
                if (!made) SetStatus("The studio lights could not be made (the log says why).");
            }
        }
    }
    if (!g_lightsMade) return;
    const bool relic = RelicActive();
    for (int i = 0; i < 3; ++i)
        if (g_lights[i] >= 0) FitLight(i, relic, now);
}

// The model back at its place, turned as the view says, when it moved (the AI, physics, a birth that ignored the place).
// A teleport resets cloth, so only then.
float AngleBetween(float a, float b) {
    float d = std::fmod(a - b, 2.0f * kPi);
    if (d > kPi) d -= 2.0f * kPi;
    if (d < -kPi) d += 2.0f * kPi;
    return std::fabs(d);
}
void KeepInPlace() {
    Features::PlayerTransform now{};
    if (!Features::GetInstanceTransformNow(g_instanceId, g_componentId, now, true)) return;
    const float turn = g_modelTurns ? g_turned : 0.0f;
    float moved = 0.0f;
    for (int k = 0; k < 3; ++k) moved = std::max(moved, std::fabs(now.position[k] - g_at[k]));
    const float turned = std::max({AngleBetween(now.rotation[0], 0.0f), AngleBetween(now.rotation[1], 0.0f),
                                   AngleBetween(now.rotation[2], turn)});
    if (moved < 1.0f && turned < 0.01f) return;
    Features::PlayerTransform want{};
    std::memcpy(want.position, g_at, sizeof(want.position));
    want.rotation[2] = turn;
    if (Features::SetInstanceTransformNow(g_instanceId, g_componentId, want)) {
        if (++g_placeResets <= 3)
            Log::Detailf("ModelViewer: %s put back at (%.0f, %.0f, %.0f) (it was at (%.0f, %.0f, %.0f), turned (%.2f, %.2f, %.2f))",
                        ModelName().c_str(), g_at[0], g_at[1], g_at[2], now.position[0], now.position[1], now.position[2],
                        now.rotation[0], now.rotation[1], now.rotation[2]);
    } else if (!g_placeFailLogged) {
        g_placeFailLogged = true;
        Log::Writef("ModelViewer: %s could not be put at its place (the instance tools' transform write failed)",
                    ModelName().c_str());
    }
}

// Which scene cells the studio lights and the model's body are in (v0.9.424, for the log): a light lights the forward
// passes' entities (PureHair's light set among them: DynamicHairDrawable::Draw takes RenderDrawState::GetLightState)
// only through its cells (cdc::SceneLight::FindDynamicEntities -> SceneCell::FindDynamicEntitiesForLight, TR2
// 0x14038B6E0); the deferred pass draws them without (Features). The light (its ISceneLight, the scene's SceneLight +
// 0x2B0): +0x50 its cells' count, +0x58 their array (TR2 GetCellsContainingLight), +0x2B object culling, +0x2D portal
// culling; the entity: +0xF4, +0x118 the count, +0x120 the entries (+0x40 the cell; TR2 GetCellsContainingEntity).
// SEH only.
void CellsText(const void* light, const void* entity, char* out, std::size_t size) noexcept {
    __try {
        if (light) {
            auto* b = static_cast<const std::uint8_t*>(light);
            const std::uint32_t n = *reinterpret_cast<const std::uint32_t*>(b + 0x50);
            auto* cells = *reinterpret_cast<void* const* const*>(b + 0x58);
            snprintf(out, size, "%u cells (%p%s), object culling %u, portal culling %u", n, n && cells ? cells[0] : nullptr,
                     n > 1 ? ", ..." : "", b[0x2B], b[0x2D]);
        } else if (entity) {
            auto* b = static_cast<const std::uint8_t*>(entity);
            const std::uint32_t n = *reinterpret_cast<const std::uint32_t*>(b + 0x118);
            auto* entries = *reinterpret_cast<std::uint8_t* const* const*>(b + 0x120);
            void* first = n && entries && entries[0] ? *reinterpret_cast<void* const*>(entries[0] + 0x40) : nullptr;
            snprintf(out, size, "%u cells (%p%s), +0xF4 %u", n, first, n > 1 ? ", ..." : "", b[0xF4]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(out, size, "not readable");
    }
}

// A drawable's scene entity (+0xC0), null when none. SEH only.
const void* SceneEntityOf(const void* drawable) noexcept {
    if (!drawable) return nullptr;
    __try {
        return *reinterpret_cast<void* const*>(static_cast<const std::uint8_t*>(drawable) + kDrawableSceneEntity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// The Suite's look on the model shown (see SuiteLook): its draw groups hidden, every frame from its birth (so before it is
// drawn); game thread.
// (v0.9.436) The models shown with every draw group (their settings' m_noDrawGroups 0 every frame, whatever the
// object's own default hides): sofia_cine.
const char* const kAllDrawGroups[] = {"sofia_cine"};
bool ShowsAllDrawGroups() {
    for (const char* object : kAllDrawGroups)
        if (_stricmp(ModelName().c_str(), object) == 0) return true;
    return false;
}

// (v0.9.438) A weapon's look by its upgrades (see kUpgradeChecks): the instance's masks without them and with all.
struct UpgradeLook {
    bool known = false;
    int upgrades = 0, hidden = 0, looks = 0;   // its upgrades (the hidden ones among them), the looks counted
    std::uint64_t base = 0;                    // the groups shown without upgrades (bit g - 1: draw group g)
    std::uint64_t none = 0, all = 0;           // the masks (bit g - 1: draw group g hidden)
};
UpgradeLook g_upgradeLook;                     // the weapon shown's (game thread)
int g_upgradeLogged = 0;                       // the look last logged: 1 none, 2 all
std::atomic<bool> g_allUpgrades{false};        // the grid's ALL UPGRADES (the session's)
constexpr int kMaxUpgrades = 96;
std::uint64_t Flags64(const std::uint8_t* at, std::size_t low, std::size_t high) {
    return std::uint64_t(*reinterpret_cast<const std::uint32_t*>(at + low)) |
           (std::uint64_t(*reinterpret_cast<const std::uint32_t*>(at + high)) << 32);
}
bool UpgradeOwned(std::uint16_t id, const std::uint16_t* owned, int count) {
    for (int i = 0; i < count; ++i)
        if (owned[i] == id) return true;
    return false;
}
// ApplyTraitUpgradesToDrawGroupBits with these upgrades owned. (Reads the game's data: called inside SEH.)
void ApplyUpgrade(const std::uint8_t* trait, const std::uint16_t* owned, int count, std::uint64_t& show, std::uint64_t& hide) {
    show |= Flags64(trait, kTraitShow, kTraitShowHigh);
    hide |= Flags64(trait, kTraitHide, kTraitHideHigh);
    const std::uint32_t gatedCount = *reinterpret_cast<const std::uint32_t*>(trait + kTraitGatedCount);
    const std::uint8_t* gated = *reinterpret_cast<const std::uint8_t* const*>(trait + kTraitGated);
    for (std::uint32_t g = 0; gated && g < gatedCount && g < 256; ++g) {
        const std::uint8_t* entry = gated + std::size_t(g) * kGatedSize;
        const std::uint32_t conditionCount = *reinterpret_cast<const std::uint32_t*>(entry + kGatedConditionCount);
        const std::uint8_t* conditions = *reinterpret_cast<const std::uint8_t* const*>(entry + kGatedConditions);
        bool holds = conditionCount == 0 || conditions != nullptr;
        for (std::uint32_t c = 0; holds && c < conditionCount && c < 64; ++c) {
            const std::uint8_t* condition = conditions + std::size_t(c) * kConditionSize;
            const bool notOwned = *reinterpret_cast<const std::uint32_t*>(condition + 4) != 0;
            if (UpgradeOwned(*reinterpret_cast<const std::uint16_t*>(condition), owned, count) == notOwned) holds = false;
        }
        if (!holds) continue;
        show |= Flags64(entry, kGatedShow, kGatedShowHigh);
        hide |= Flags64(entry, kGatedHide, kGatedHideHigh);
    }
}
// The weapon's record (by its m_objectID) and its upgrades' looks. SEH only.
bool ReadUpgradeLook(std::uint16_t weapon, UpgradeLook& out) noexcept {
    __try {
        const auto* table = *reinterpret_cast<const std::uint8_t* const*>(g_weaponUpgradesGlobal);
        const auto* traits = *reinterpret_cast<const std::uint8_t* const*>(g_traitsGlobal);
        if (!table || !traits) return false;
        const std::uint32_t weapons = *reinterpret_cast<const std::uint32_t*>(table + kUpgradeCount);
        const auto* records = *reinterpret_cast<const std::uint8_t* const*>(table + kUpgradeRecords);
        const std::uint32_t traitCount = *reinterpret_cast<const std::uint32_t*>(traits + kTraitCount);
        const auto* list = *reinterpret_cast<const std::uint8_t* const*>(traits + kTraits);
        if (!records || !list) return false;
        const std::uint8_t* record = nullptr;
        for (std::uint32_t i = 0; !record && i < weapons && i < 1024; ++i)
            if (*reinterpret_cast<const std::uint16_t*>(records + std::size_t(i) * kUpgradeRecordSize + kRecordObject) == weapon)
                record = records + std::size_t(i) * kUpgradeRecordSize;
        if (!record) return false;
        const std::uint32_t entries = *reinterpret_cast<const std::uint32_t*>(record + kRecordUpgradeCount);
        const auto* upgrades = *reinterpret_cast<const std::uint8_t* const* const*>(record + kRecordUpgrades);
        std::uint16_t ids[kMaxUpgrades];
        const std::uint8_t* of[kMaxUpgrades];
        bool hidden[kMaxUpgrades];
        int count = 0;
        for (std::uint32_t k = 0; upgrades && k < entries && k < 512 && count < kMaxUpgrades; ++k) {
            const std::uint8_t* entry = upgrades[k];
            if (!entry) continue;
            const std::uint16_t id = *reinterpret_cast<const std::uint16_t*>(entry);
            if (id >= traitCount) continue;
            const std::uint8_t* trait = list + std::size_t(id) * kTraitSize;
            if (*reinterpret_cast<const std::int32_t*>(trait + kTraitType) != kTraitWeaponUpgrade || trait[kTraitAttachment]) continue;
            ids[count] = id, of[count] = trait, hidden[count] = trait[kTraitHidden] != 0;
            ++count;
        }
        out.upgrades = count;
        std::uint64_t show = 0, hide = 0;
        for (int k = 0; k < count; ++k) ApplyUpgrade(of[k], ids, count, show, hide);
        out.all = ~show | hide;
        int votes[64] = {}, hides[64] = {};
        for (int k = 0; k < count; ++k) {
            if (hidden[k]) {
                ++out.hidden;
                continue;
            }
            std::uint64_t s = 0, h = 0;
            ApplyUpgrade(of[k], &ids[k], 1, s, h);
            if (!s && !h) continue;
            ++out.looks;
            for (int b = 0; b < 64; ++b) {
                votes[b] += int(((s & ~h) >> b) & 1u);
                hides[b] += int((h >> b) & 1u);
            }
        }
        // (v0.9.441) also a part every look either shows or replaces (hides): the axes' handle, group 1, which the two
        // handle upgrades hide and the other two show (half of the looks: left out before, the Reckoner without its handle)
        for (int b = 0; b < 64; ++b)
            if (votes[b] * 2 > out.looks || (votes[b] > 0 && hides[b] > 0 && votes[b] + hides[b] >= out.looks))
                out.base |= std::uint64_t(1) << b;
        out.none = ~out.base;
        out.known = out.looks > 0 && out.base != 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// (v0.9.443) The menu models' looks, as their scripts make them (replaces 0.9.442's one base per family, which hid parts
// the script never touches: the Voidhammer's barrel 5, the Military Rifle's 55; and the upgrades' own looks for ALL
// UPGRADES, which retail's screen does not use: the Deathsong's shaft, the Hailstorm's muzzle). Each family of menu
// models (wp_*_ui) shares one action graph (the bows' DTP 701715, the axes' 701632, the pistols' 704729, the
// Hailstorm's 976477, the rifles' 703721, the shotguns' 687314) whose inputs "Use Base Drawgroups!" and "Update
// Drawgroups!" set the model's draw groups: Instance_DrawGroups nodes (SetCheckedToDraw / SetCheckedToNoDraw, groups at
// +0x94 / +0x98 of their settings, bit k group k + 1) behind Instance_HasTrait checks of the player's upgrades (the
// "wup - <class> - ..." traits and their transient "trans - ..." previews), and per model type (Misc_GetNamedValue
// "PistolType" / "RifleType" / "ShotgunType" / "BowType" compared with 0-3: each model's own named value, e.g. the hand
// cannon's PistolType 3 in its DTP 705083, behind Station_IsStandalone's OnSuccess) and "UI Weapon?" (1 in each model's
// graph inputs, DTP 705986 +0x43C8 for wp_pistol_ui). Worked out from the game's data by running each graph offline
// the way the game runs them (ROTTR.pdb build, its code matching retail's: ActionGraphNode::OutputData, retail
// 0x720CC0, writes the target's input and queues it once, behind the others; ActionGraphInstance::ProcessMessageQueues
// handles a node's pending inputs together; Logic_Any outputs only its highest pending input; Instance_DrawGroups,
// retail 0x75E220, shows (bit 5) before it hides (bit 6), and its Trigger sets the whole mask): "Use Base Drawgroups!",
// then "Update Drawgroups!" with no upgrade (the look without upgrades) or with every upgrade the weapon's class lists
// (ALL UPGRADES: the "wup - <class> - ..." traits the graph checks, not the attachments, +0x2B). A group the script
// never sets stays drawn (the Voidhammer's 5, the hand cannon's 33). Masks: bit g - 1 set, draw group g hidden.
struct MenuLook {
    const char* name;     // the menu model
    std::uint64_t none;   // without upgrades
    std::uint64_t all;    // with every upgrade (ALL UPGRADES)
    std::uint32_t graph;  // its family's action graph (DTP)
};
const MenuLook kMenuLooks[] = {
    {"wp_bow_ancient_horn__witch_ui", 0xFFFFFFFFFFFFFFF0ULL, 0xFFFFFFFFF9C9EFA2ULL, 701715},   // (BowType 3)
    {"wp_bow_ancient_horn_grimwhisper_ui", 0xFFFFFFFFFFFFFFF0ULL, 0xFFFFFFFFF9C9EFA2ULL, 701715},   // (BowType 3)
    {"wp_bow_ancient_horn_guidinglight_ui", 0xFFFFFFFFFFFFFFF0ULL, 0xFFFFFFFFF9C9EFA2ULL, 701715},   // (BowType 3)
    {"wp_bow_ancient_horn_ui", 0xFFFFFFFFFFFFFFF0ULL, 0xFFFFFFFFF9C9EFA2ULL, 701715},   // (BowType 3)
    {"wp_bow_compound_deathsbreath_ui", 0xFFFFFFFFFFFFFFF0ULL, 0xFFFFFFFFFE8463BEULL, 701715},   // (BowType 2)
    {"wp_bow_compound_ui", 0xFFFFFFFFFFFFFFF0ULL, 0xFFFFFFFFFE8463BEULL, 701715},   // (BowType 2)
    {"wp_bow_recurve_ui", 0xFFFFFFFFFFFFFFF0ULL, 0xFFFFFFFFFC4E9ED3ULL, 701715},   // (BowType 1)
    {"wp_bow_recurve_whitewidow_ui", 0xFFFFFFFFFFFFFFF0ULL, 0xFFFFFFFFFC4E9ED3ULL, 701715},   // (BowType 1)
    {"wp_bow_ui", 0xFFFFFFFFFFFFFFF0ULL, 0xFFFFFFFFFFCDEFB2ULL, 701715},   // (BowType 0)
    {"wp_climbing_axe_battleaxe_ui", 0x0000000000000078ULL, 0x0000000000000004ULL, 701632},
    {"wp_climbing_axe_deathsong_ui", 0x0000000000000078ULL, 0x0000000000000004ULL, 701632},
    {"wp_climbing_axe_ui", 0x0000000000000078ULL, 0x0000000000000004ULL, 701632},
    {"wp_climbing_axe_vintage_ui", 0x0000000000000078ULL, 0x0000000000000004ULL, 701632},
    {"wp_pistol_hand_cannon_ui", 0xFF3F6FDE7FFFFFC0ULL, 0xFF3F6FDEFF6E76DAULL, 704729},   // (PistolType 3)
    {"wp_pistol_magnum_moonshadow_ui", 0xFF3F6FDEFFFFFFC0ULL, 0xFEAFF7EAFBE6FF76ULL, 704729},   // (PistolType 2)
    {"wp_pistol_magnum_ui", 0xFF3F6FDEFFFFFFC0ULL, 0xFEAFF7EAFBE6FF76ULL, 704729},   // (PistolType 2)
    {"wp_pistol_microuzi_ui", 0xFF3F6FDE7FFFFFC0ULL, 0xFF3F6FFE778E7B5CULL, 976477},   // (PistolType 1)
    {"wp_pistol_nighthawk_ui", 0xFF3F6FDEFFFFFFC0ULL, 0xFEAFE7EEF7BE7F7FULL, 704729},   // (PistolType 1)
    {"wp_pistol_revolver_rattlesnake_ui", 0xFF3F6FDE7FFFFFC0ULL, 0xFF3F6FDEFF6E76DAULL, 704729},   // (PistolType 0)
    {"wp_pistol_revolver_ui", 0xFF3F6FDE7FFFFFC0ULL, 0xFF3F6FDEFF6E76DAULL, 704729},   // (PistolType 0)
    {"wp_pistol_roth_ui", 0xFF3F6FDEFFFFFFC0ULL, 0xFEAFE7EEF7BE7F7FULL, 704729},   // (PistolType 1)
    {"wp_pistol_ui", 0xFF3F6FDEFFFFFFC0ULL, 0xFEAFE7EEF7BE7F7FULL, 704729},   // (PistolType 1)
    {"wp_shotgun_break_action_thunderclap_ui", 0x00007F63DFFFFF80ULL, 0x00047F68DB7DFE01ULL, 687314},   // (ShotgunType 0)
    {"wp_shotgun_break_action_ui", 0x00007F63DFFFFF80ULL, 0x00047F68DB7DFE01ULL, 687314},   // (ShotgunType 0)
    {"wp_shotgun_break_action_voidhammer_ui", 0x00007F63DFFFFF80ULL, 0x00047F68DB7DFE01ULL, 687314},   // (ShotgunType 0)
    {"wp_shotgun_full_auto_harbinger_ui", 0x00007F6BDFFFFF80ULL, 0x00087F6BC2FC7B88ULL, 687314},   // (ShotgunType 3)
    {"wp_shotgun_full_auto_ui", 0x00007F6BDFFFFF80ULL, 0x00087F6BC2FC7B88ULL, 687314},   // (ShotgunType 3)
    {"wp_shotgun_tactical_ui", 0x00007F6BDFFFFF80ULL, 0x000C7F6BE01D7F88ULL, 687314},   // (ShotgunType 1)
    {"wp_shotgun_ui", 0x00007F6BDFFFFF80ULL, 0x000C7F6A4A3DD780ULL, 687314},   // (ShotgunType 2)
    {"wp_smg_assault_rifle_2_snowflake_ui", 0x3CB8DF6FBBFCF300ULL, 0x5C78DB7F96D8A269ULL, 703721},   // (RifleType 3)
    {"wp_smg_assault_rifle_2_ui", 0x3CB8DF6FBBFCF300ULL, 0x5C78DB7F96D8A269ULL, 703721},   // (RifleType 3)
    {"wp_smg_bolt_action_rifle_goldenfang_ui", 0x07FADB67BFFCFF00ULL, 0x04F94B67B47CEE07ULL, 703721},   // (RifleType 2)
    {"wp_smg_bolt_action_rifle_ui", 0x07FADB67BFFCFF00ULL, 0x04F94B67B47CEE07ULL, 703721},   // (RifleType 2)
    {"wp_smg_mp7_ui", 0x04F8DB669DFCF300ULL, 0x00F4BB7680F8E2A9ULL, 703721},   // (RifleType 1)
    {"wp_smg_russian_ui", 0x5CF8DF6FBFFCF300ULL, 0x24D8CA6F92B8C279ULL, 703721},   // (RifleType 0)
};
const MenuLook* MenuLookOf(const std::string& name) {
    for (const auto& look : kMenuLooks)
        if (_stricmp(name.c_str(), look.name) == 0) return &look;
    return nullptr;
}
std::string GroupList(std::uint64_t groups) {
    std::string text;
    for (int b = 0; b < 64; ++b)
        if ((groups >> b) & 1u) text += (text.empty() ? "" : " ") + std::to_string(b + 1);
    return text.empty() ? "none" : text;
}
// The shown model's (StartLoad): a menu model's looks (above), else a weapon's upgrades read from the game's data.
void ReadShownUpgrades(std::uint16_t weapon, const std::string& name) {
    g_upgradeLook = UpgradeLook{};
    g_upgradeLogged = 0;
    if (const MenuLook* menu = MenuLookOf(name)) {
        UpgradeLook look;
        look.known = true;
        look.none = menu->none;
        look.all = menu->all;
        look.base = ~menu->none;
        g_upgradeLook = look;
        Log::Detailf("ModelViewer: %s: its looks as its script (DTP %u) makes them: without upgrades draw group mask 0x%016llX, "
                    "with every upgrade (ALL UPGRADES) 0x%016llX", name.c_str(), unsigned(menu->graph),
                    static_cast<unsigned long long>(menu->none), static_cast<unsigned long long>(menu->all));
        return;
    }
    if (!weapon || !g_weaponUpgradesGlobal || !g_traitsGlobal) return;
    UpgradeLook look;
    const bool read = ReadUpgradeLook(weapon, look);
    if (!look.known && !read) {
        Log::Detailf("ModelViewer: %s: no upgrades in the game's list for its weapon (object %u): shown with every part",
                    name.c_str(), unsigned(weapon));
        return;
    }
    if (!look.known) {
        Log::Writef("ModelViewer: %s: %d upgrades (%d hidden, %d with a look of their own), no part most of those looks show: "
                    "shown with every part", name.c_str(), look.upgrades, look.hidden, look.looks);
        return;
    }
    g_upgradeLook = look;
    Log::Writef("ModelViewer: %s: %d upgrades (%d hidden, %d with a look of their own; weapon object %u): without them draw groups "
                "%s (mask 0x%016llX), with all (ALL UPGRADES) %s (mask 0x%016llX)", name.c_str(), look.upgrades, look.hidden,
                look.looks, unsigned(weapon), GroupList(look.base).c_str(), static_cast<unsigned long long>(look.none),
                GroupList(~look.all).c_str(), static_cast<unsigned long long>(look.all));
}

void ApplyLook() {
    const bool all = ShowsAllDrawGroups();
    const bool weapon = g_upgradeLook.known;
    if (!all && !weapon && (!g_modelLook.known || !g_modelLook.noDraw)) return;
    const bool allUpgrades = g_allUpgrades.load();
    const std::uint64_t mask = all ? 0 : weapon ? (allUpgrades ? g_upgradeLook.all : g_upgradeLook.none) : g_modelLook.noDraw;
    std::uint64_t before = 0;
    const bool ok = Features::SetInstanceNoDrawMaskNow(g_instanceId, g_componentId, mask, &before);
    if (ok && weapon && !all) {
        if (g_upgradeLogged == (allUpgrades ? 2 : 1)) return;
        g_upgradeLogged = allUpgrades ? 2 : 1;
        Log::Detailf("ModelViewer: %s: %s (its draw group mask 0x%016llX; it had 0x%016llX)", ModelName().c_str(),
                    allUpgrades ? "every upgrade (ALL UPGRADES)" : "no upgrade", static_cast<unsigned long long>(mask),
                    static_cast<unsigned long long>(before));
        return;
    }
    if (!ok || g_lookLogged) return;
    g_lookLogged = true;
    if (all)
        Log::Writef("ModelViewer: %s: every draw group shown (its draw group mask 0; it had 0x%016llX)", ModelName().c_str(),
                    static_cast<unsigned long long>(before));
    else
        Log::Detailf("ModelViewer: %s: the Suite's look (Gear on Default): %d draw groups hidden (mask 0x%016llX; it had 0x%016llX)",
                    ModelName().c_str(), g_modelLook.hidden, static_cast<unsigned long long>(g_modelLook.noDraw),
                    static_cast<unsigned long long>(before));
}

// The model turned as the view's yaw says (see g_turned), once it is shown; game thread.
void TurnModel() {
    if (!g_revealed.load(std::memory_order_acquire)) return;
    float yaw;
    {
        std::lock_guard lock(g_viewMutex);
        g_view.yaw = std::fmod(g_view.yaw, 360.0f);   // (kept small: it adds up while the grid's model turns)
        yaw = g_view.yaw;
    }
    const float want = yaw * kPi / 180.0f;
    if (g_modelTurns && AngleBetween(want, g_turned) < 1e-5f) return;
    const float rotation[3] = {0.0f, 0.0f, want};
    if (Features::TurnInstanceNow(g_instanceId, g_componentId, rotation)) {
        g_turned = want;
        g_modelTurns = true;
    } else {
        g_modelTurns = false;
        if (!g_turnFailLogged) {
            g_turnFailLogged = true;
            Log::Writef("ModelViewer: %s could not be turned (Instance::SetRotationEuler): the camera turns about it",
                        ModelName().c_str());
        }
    }
}

void ResetView() {
    std::lock_guard lock(g_viewMutex);
    g_view = View{};
}

// ---- Loading, birth, removal --------------------------------------------------------------------------------------------
void LogUnits() {
    Cutscenes::UnitSlot slots[32];
    const int n = Cutscenes::StreamUnitSlots(slots, 32);
    std::string line;
    for (int i = 0; i < n; ++i) {
        if (slots[i].state == 0) continue;
        char one[96];
        snprintf(one, sizeof(one), "%s%s (%d, state %d)", line.empty() ? "" : ", ", slots[i].name, slots[i].id, slots[i].state);
        line += one;
    }
    Log::Writef("ModelViewer: the loaded units: %s; the player's unit %d", line.empty() ? "none" : line.c_str(),
                Features::ViewerPlayerStreamUnit());
}

int ChooseUnit() {
    if (const int player = Features::ViewerPlayerStreamUnit()) return player;
    Cutscenes::UnitSlot slots[32];
    const int n = Cutscenes::StreamUnitSlots(slots, 32);
    for (int i = 0; i < n; ++i)
        if (slots[i].state == 2 && slots[i].id > 0) return slots[i].id;
    return 0;
}

// The model away: removed, its zone released (the lights stay for the next one).
// The model shown taken away: as it was born for a second more (its removal's calls; see g_slots).
void RetireFilterSlot() {
    const void* data = g_slots[0].data.load(std::memory_order_acquire);
    if (!data) return;
    FilterSlot& leaving = g_slots[1];
    leaving.data.store(nullptr, std::memory_order_release);
    leaving.instance.store(g_slots[0].instance.load());
    leaving.player.store(g_slots[0].player.load());
    leaving.soundOn.store(g_slots[0].soundOn.load());
    leaving.hairObject.store(g_slots[0].hairObject.load());
    leaving.until.store(GetTickCount64() + 1000);
    leaving.data.store(data, std::memory_order_release);
    g_slots[0].data.store(nullptr, std::memory_order_release);
}

// ---- The models' objects kept loaded (v0.9.433) ----
// The models seen last, and the grid's neighbours of the one shown (the tiles before and after it), keep their object
// zones loaded (one reference of ours each: ObjectZoneManager::LoadAndAddRef2, Release2), so going back to one, or on to
// the next, needs no load: up to kKeptZones of them, the one used longest ago let go first. The model shown is asked
// for at HIGH priority (cdc::FileRequest::Priority 1; every load of the game's own is NORMAL, 3), the neighbours at
// BELOW_NORMAL (4), once the model shown is drawn (so that they never come before it).
constexpr std::size_t kKeptZones = 6;
constexpr int kPriorityShown = 1, kPriorityNext = 4;
struct KeptZone {
    std::uint32_t zone;
    double used;
};
std::vector<KeptZone> g_kept;
int g_keptLoads = 0, g_keptHits = 0;   // (the log, when the viewer closes)
// A zone kept (loaded at that priority when it was not kept yet); true: it was kept already.
bool KeepZone(std::uint32_t zone, int priority, double now) {
    for (KeptZone& k : g_kept)
        if (k.zone == zone) {
            k.used = now;
            return true;
        }
    if (!Features::ViewerZoneLoad(zone, priority)) return false;
    g_kept.push_back({zone, now});
    ++g_keptLoads;
    while (g_kept.size() > kKeptZones) {   // (the one used longest ago let go)
        auto oldest = std::min_element(g_kept.begin(), g_kept.end(), [](const KeptZone& a, const KeptZone& b) { return a.used < b.used; });
        Features::ViewerZoneRelease(oldest->zone);
        g_kept.erase(oldest);
    }
    return false;
}
void ReleaseKept() {
    for (const KeptZone& k : g_kept) Features::ViewerZoneRelease(k.zone);
    if (g_keptLoads)
        Log::Detailf("ModelViewer: the models' objects kept loaded: %d asked for, %d of the models shown found already loaded; "
                    "%zu let go", g_keptLoads, g_keptHits, g_kept.size());
    g_kept.clear();
    g_keptLoads = g_keptHits = 0;
}
// (the grid, the model shown drawn) its neighbours' objects asked for, once a model
int g_nextFor[2] = {-1, -1};
void KeepNeighbours(double now) {
    if (g_shownCategory < 0 || g_shownCategory >= int(g_categories.size())) return;
    if (g_nextFor[0] == g_shownCategory && g_nextFor[1] == g_shownIndex) return;
    g_nextFor[0] = g_shownCategory, g_nextFor[1] = g_shownIndex;
    const Category& c = g_categories[std::size_t(g_shownCategory)];
    const int n = int(c.models.size());
    for (const int d : {+1, -1}) {
        const int i = g_shownIndex + d;
        if (n < 2 || i < 0 || i >= n || i == g_shownIndex) continue;
        std::uint32_t zone = 0;
        if (!Features::ViewerZoneId(c.models[std::size_t(i)].object.c_str(), zone)) KeepZone(zone, kPriorityNext, now);
    }
}

// Lara's idle on the outfit or the character shown (see g_outfitIdle, ShownIdles): tried every 0.5 s for 15 s
// (laracroft's object loaded meanwhile).
constexpr int kIdleTries = 30;
void StopIdle() {
    if (g_idlePlaying) InstanceAnim::Stop(g_instanceId, g_componentId);
    g_idlePlaying = false;
}
// (v0.9.441) A character's own idle: its clips loaded (else its object asked for, once). True when they are.
// (v0.9.443) The source is loaded even when the clips are there already (another object's library has them): its
// skeleton is the translations' reference. 0.9.441 took the clips alone then, without one: their translations played as
// they are, Sofia (her height on bone ID 2, enm_shield_manor's on ID 3, 107.5 cm) a metre up.
bool OwnIdleLoaded(const CharacterIdle& idle, std::string& why) {
    bool all = true;
    for (int k = 0; k < idle.clipCount; ++k) all = all && InstanceAnim::ClipLoaded(idle.clips[k]);
    IdleSource* source = nullptr;
    for (auto& s : g_idleSources)
        if (s.name == idle.source) source = &s;
    if (!source) {
        g_idleSources.push_back({idle.source});
        source = &g_idleSources.back();
    }
    if (source->failed) {
        why = std::string(idle.source) + " cannot be loaded";
        return all;   // (its clips there: played without the reference)
    }
    if (!source->ref) {
        if (const char* error = Features::ViewerZoneId(idle.source, source->zone)) {
            source->failed = true;
            why = std::string(idle.source) + ": " + error;
            Log::Writef("ModelViewer: %s's idle: %s, the object with its animations, cannot be loaded: %s", ModelName().c_str(),
                        idle.source, error);
            return false;
        }
        if (Features::ViewerZoneLoad(source->zone, kPriorityShown)) {
            source->ref = true;
            Log::Detailf("ModelViewer: %s's idle: %s loaded for its animations (zone %u)", ModelName().c_str(), idle.source,
                        source->zone);
        }
    }
    if (all && source->ref && ZoneObjectData(source->zone)) return true;
    why = std::string(idle.source) + (all ? " (its skeleton) not loaded yet" : "'s animations not loaded yet");
    return false;
}
void ReleaseIdleSources() {
    for (auto& s : g_idleSources)
        if (s.ref) Features::ViewerZoneRelease(s.zone);
    g_idleSources.clear();
}
void StartIdle(double now) {
    g_idleTriedAt = now;
    using InstanceAnim::kAllTracks;
    // (v0.9.441) a character with an idle of its own: that on the body, Lara's face over it
    const CharacterIdle* own = g_shownPlayer ? nullptr : CharacterIdleOf(ModelName());
    if (own) {
        std::string why;
        if (!OwnIdleLoaded(*own, why)) {
            if (++g_idleTries == kIdleTries)
                Log::Writef("ModelViewer: %s's idle not played in %.0f s: %s (the bind pose instead)", ModelName().c_str(),
                            kIdleTries * 0.5, why.c_str());
            return;
        }
    }
    InstanceAnim::Layer layers[] = {
        {"ground_idle", 1.0f, nullptr, kAllTracks, nullptr, 0},
        {"face_neutral", 1.0f, nullptr, 0, nullptr, kAllTracks},
        {"face_eyes", 1.0f, kEyeBones, int(std::size(kEyeBones)), kEyeShapes, int(std::size(kEyeShapes))},
        {"hair_ground_idle", 0.5f, kHairBones, int(std::size(kHairBones)), nullptr, 0},
    };
    int layerCount = int(std::size(layers));
    InstanceAnim::BindPivot ownBind[48];
    int ownBindCount = 0;
    if (own) {   // its clip(s) on all its bones, no blend shapes (the face's stay Lara's); Lara's hair layer left out
        layers[0] = InstanceAnim::Layer{};
        layers[0].name = nullptr;
        layers[0].boneCount = kAllTracks;
        layers[0].shapeCount = 0;
        if (own->clipCount > 1) layers[0].sequence = own->clips, layers[0].sequenceCount = own->clipCount;
        else layers[0].animId = own->clips[0];
        layerCount = 3;
        IdleSource* source = nullptr;
        for (auto& s : g_idleSources)
            if (s.name == own->source) source = &s;
        const void* data = source && source->ref ? ZoneObjectData(source->zone) : nullptr;
        ownBindCount = data ? InstanceAnim::ReferenceFromObject(data, own->clips, own->clipCount, ownBind, int(std::size(ownBind))) : -1;
        if (ownBindCount < 0) ownBindCount = 0;
    }
    std::string info;
    // (a character: on its own proportions, see kLaraBind; Lara's outfits as the clips are)
    const bool character = !g_shownPlayer;
    const InstanceAnim::BindPivot* reference = own ? (ownBindCount ? ownBind : nullptr) : character ? kLaraBind : nullptr;
    const int referenceCount = own ? ownBindCount : character ? int(std::size(kLaraBind)) : 0;
    if (InstanceAnim::PlayLayers(g_instanceId, g_componentId, layers, layerCount, info, reference, referenceCount)) {
        g_idlePlaying = true;
        g_idleSince = now;
        if (own) {
            Log::Writef("ModelViewer: %s stands in its own idle, %s (%s's animations; translations %s): %s", ModelName().c_str(),
                        own->names, own->source,
                        ownBindCount ? ("kept as offsets from its skeleton on " + std::to_string(ownBindCount) + " bones").c_str()
                                     : "as they are (its object has no skeleton)",
                        info.c_str());
            return;
        }
        Log::Detailf("ModelViewer: %s stands as Lara does in the game: %s", ModelName().c_str(), info.c_str());
        InstanceAnim::Fidget fidgets[2];
        fidgets[0].parts[0] = "ground_idle_look_around", fidgets[0].partCount = 1;
        fidgets[1].parts[0] = "ground_idle_neck_rub", fidgets[1].partCount = 1;
        std::string fidgetInfo;
        const bool fidgeting = InstanceAnim::AddFidgets(g_instanceId, g_componentId, fidgets, int(std::size(fidgets)),
                                                        kFidgetMinGap, kFidgetMaxGap, kFidgetFade, fidgetInfo);
        Log::Detailf("ModelViewer: %s's fidgets%s: %s", ModelName().c_str(), fidgeting ? "" : " not played", fidgetInfo.c_str());
        return;
    }
    ++g_idleTries;
    if (!g_laraZoneRef) {   // (her clips are laracroft's: its object loaded for them, once)
        if (const char* why = Features::ViewerZoneId("laracroft", g_laraZone)) {
            Log::Writef("ModelViewer: %s's idle not played (%s), and laracroft's object cannot be loaded for it: %s",
                        ModelName().c_str(), info.c_str(), why);
            g_idleTries = kIdleTries;
            return;
        }
        if (Features::ViewerZoneLoad(g_laraZone, kPriorityShown)) {
            g_laraZoneRef = true;
            Log::Detailf("ModelViewer: Lara's idle: %s; laracroft's object loaded for its animations (zone %u)", info.c_str(),
                        g_laraZone);
        }
        return;
    }
    if (g_idleTries == kIdleTries)
        Log::Writef("ModelViewer: %s's idle not played in %.0f s: %s (the bind pose instead)", ModelName().c_str(),
                    kIdleTries * 0.5, info.c_str());
}

void Unload() {
    StopIdle();
    g_idleTries = 0;
    g_idleTriedAt = -10.0;
    g_poseInstance.store(nullptr, std::memory_order_release);
    g_isolateInstance.store(nullptr, std::memory_order_release);
    g_boundsInstance.store(nullptr, std::memory_order_release);
    if (g_instance) {
        void* alive = Features::InstanceNowGameThread(g_instanceId, g_componentId);
        if (alive) Features::ViewerRemove(alive);
        Log::Detailf("ModelViewer: %s taken away (%s)", ModelName().c_str(), alive ? "removed" : "already gone");
        g_instance = nullptr;
    }
    if (g_zoneRef) {
        Features::ViewerZoneRelease(g_zone);
        g_zoneRef = false;
    }
    RetireFilterSlot();   // (its removal at the frame's end)
    g_bounds = Bounds{};
    ResetDrawnBounds();
}

void StartLoad() {
    g_shownCategory = g_category;
    g_shownIndex = g_categories.empty() ? -1 : g_categories[std::size_t(g_category)].index;
    const std::string& name = ModelName();
    g_lightTries = 0;   // (each model its own 10 s to find its scene)
    g_lightsGaveUp = false;
    g_positionLogged = g_placeFailLogged = false;
    g_noteHairLights.store(true, std::memory_order_relaxed);   // (v0.9.602)
    g_placeResets = 0;
    g_poseCalls.store(0);
    {
        const auto it = g_suiteRecords.find(name);
        g_modelSuiteBounds = it != g_suiteRecords.end() ? it->second.bounds : Bounds{};
        g_modelLook = it != g_suiteRecords.end() ? it->second.look : SuiteLook{};
        g_modelHairObject = 0;
        g_lookLogged = false;
        // a weapon's look by its upgrades (v0.9.438)
        std::uint16_t weapon = 0;
        if (g_shownCategory >= 0 && g_shownCategory < int(g_categories.size())) {
            const Category& c = g_categories[std::size_t(g_shownCategory)];
            if (c.kind == Kind::Weapons && g_shownIndex >= 0 && g_shownIndex < int(c.models.size()))
                weapon = c.models[std::size_t(g_shownIndex)].weaponObject;
        }
        ReadShownUpgrades(weapon, name);
        // its hair: the record's object's ID (the hair objects are objects of their own: the zone list's)
        if (!g_modelLook.hair.empty()) {
            std::uint32_t hair = 0;
            if (const char* why = Features::ViewerZoneId(g_modelLook.hair.c_str(), hair))
                Log::Writef("ModelViewer: %s's hair %s (the Suite's record) not found: %s; its own worn", name.c_str(),
                            g_modelLook.hair.c_str(), why);
            else
                g_modelHairObject = hair;
        }
    }
    if (const char* why = Features::ViewerZoneId(name.c_str(), g_zone)) {
        g_phase = Phase::Failed;
        SetStatus("%s: %s.", name.c_str(), why);
        Log::Writef("ModelViewer: %s not loaded: %s", name.c_str(), why);
        return;
    }
    if (KeepZone(g_zone, kPriorityShown, Now()) && Features::ViewerZoneLoaded(g_zone)) ++g_keptHits;
    if (!Features::ViewerZoneLoad(g_zone, kPriorityShown)) {
        g_phase = Phase::Failed;
        SetStatus("%s could not be loaded.", name.c_str());
        Log::Writef("ModelViewer: ObjectZoneManager::LoadAndAddRef2 refused %s (zone %u)", name.c_str(), g_zone);
        return;
    }
    g_zoneRef = true;
    g_loadStart = Now();
    g_phase = Phase::Loading;
    SetStatus("Loading %s...", name.c_str());
    Log::Detailf("ModelViewer: loading %s (zone %u)", name.c_str(), g_zone);
}

void Birth(const void* objectData, bool player) {
    const std::string& name = ModelName();
    g_unit = ChooseUnit();
    if (g_unit <= 0) {
        g_phase = Phase::Failed;
        SetStatus("No stream unit to put %s in.", name.c_str());
        Log::Writef("ModelViewer: no loaded stream unit to birth %s in", name.c_str());
        LogUnits();
        Unload();
        return;
    }
    // its components (see kRvaGetComponent): from now on until it is taken away
    g_leftOutCount = 0;
    g_playerFlagHidden = 0;
    FilterSlot& slot = g_slots[0];
    slot.data.store(nullptr, std::memory_order_release);
    slot.instance.store(nullptr, std::memory_order_release);
    slot.player.store(player);
    slot.soundOn.store(g_soundOn.load());
    slot.hairObject.store(g_modelHairObject);
    slot.until.store(0);
    g_hairSwapped = 0, g_hairOwnObject = 0;
    slot.data.store(g_filterReady ? objectData : nullptr, std::memory_order_release);
    ResetDrawnBounds();
    g_revealed.store(false, std::memory_order_release);   // (shown once all of it is there: TickGameThread)
    g_bodyDrawable = nullptr;
    g_bodyDraws = 0, g_hairDraws = 0, g_otherDraws = 0;
    g_revealTicks = 0;
    g_turned = 0.0f, g_modelTurns = false;
    g_shownPlayer = player;
    // (v0.9.438) the lights' fractions of their distances start again at 1 for each model: kept, a far camera's (the
    // crowbar's, 99 m away: its lights at 0.03 to 0.06) put the next relic's lights a few cm from it until they had grown
    // back (x 1.5 every 0.3 s: about 2.6 s). FitLight moves them in again in the same frame where a cell asks for it.
    for (int i = 0; i < 3; ++i) g_lightFit[i] = 1.0f, g_lightGrownAt[i] = 0.0;
    void* born = Features::ViewerBirth(g_zone, g_unit, g_at);
    // (the instance holds its own reference to the zone now)
    if (g_zoneRef) {
        Features::ViewerZoneRelease(g_zone);
        g_zoneRef = false;
    }
    const double loadMs = (Now() - g_loadStart) * 1000.0;
    if (!born) {
        RetireFilterSlot();
        g_phase = Phase::Failed;
        SetStatus("%s could not be born.", name.c_str());
        Log::Writef("ModelViewer: BirthObjectInstance returned null for %s (zone %u, unit %d)", name.c_str(), g_zone, g_unit);
        return;
    }
    g_instance = born;
    g_slots[0].instance.store(born, std::memory_order_release);   // (from now on its calls by its instance)
    ReadIds(born, g_instanceId, g_componentId, g_bornAt);
    MeasureModel(born);
    g_bodyDrawable = BodyDrawable(born);
    if (g_modelSuiteBounds.valid) {
        const Bounds& b = g_modelSuiteBounds;
        Log::Detailf("ModelViewer: %s: the Suite's bounds (%.0f, %.0f, %.0f) to (%.0f, %.0f, %.0f) cm", name.c_str(), b.min[0],
                    b.min[1], b.min[2], b.max[0], b.max[1], b.max[2]);
    }
    if (g_leftOutCount.load())
        Log::Detailf("ModelViewer: %s born without %s%s", name.c_str(), LeftOutNames().c_str(),
                    player ? " (a player object: its look alone)" : g_soundOn.load() ? "" : " (its sound: S / pad Back on the view)");
    if (player)
        Log::Detailf("ModelViewer: %s's object flags answered without the player's (0x20000) %u times at its birth", name.c_str(),
                    g_playerFlagHidden.load());
    if (g_modelHairObject)
        Log::Detailf("ModelViewer: %s wears %s (the Suite's look; object %u, its own %u): %s", name.c_str(), g_modelLook.hair.c_str(),
                    g_modelHairObject, g_hairOwnObject.load(),
                    g_hairSwapped.load() ? "its hair data answered with it"
                    : g_hairOwnObject.load() == g_modelHairObject ? "its own"
                                                                   : "its hair data was not asked for at its birth");
    ApplyLook();
    g_boundsInstance.store(born, std::memory_order_release);
    g_poseInstance.store(born, std::memory_order_release);
    InstanceAnim::KeepPoseBuilt(born);
    g_phase = Phase::Shown;
    g_shownAt = Now();
    g_lightDrawsBase = Features::DeferredLightDraws();
    g_entitiesDrawn = 0, g_entitiesHidden = 0, g_meshPassesHidden = 0;
    g_gameLightsLeftOut = 0, g_ambientZeroed = 0, g_adaptationsStopped = 0;
    g_gradingLeftOut = 0, g_gradingSeen = 0;
    // (an outfit without the Suite's look: said on the screen, it is shown with every part)
    if (player && !g_modelLook.known)
        SetStatus("Shown with every part and its own hair: no Modding Suite record of its look. In the Suite: Models, For the "
                  "Model Viewer, Make the pictures.");
    else
        SetStatus("");
    Log::Writef("ModelViewer: %s shown (zone %u, loaded in %.0f ms; instance %u in unit %d, born at (%.0f, %.0f, %.0f), its place "
                "(%.0f, %.0f, %.0f))", name.c_str(), g_zone, loadMs, g_instanceId, g_unit, g_bornAt[0], g_bornAt[1], g_bornAt[2],
                g_at[0], g_at[1], g_at[2]);
}

float IniFloat(const char* path, const char* key, const char* fallback, float lo, float hi) {
    char text[32]{};
    GetPrivateProfileStringA("ModelViewer", key, fallback, text, sizeof(text), path);
    const float v = float(std::atof(text));
    return std::isfinite(v) ? std::clamp(v, lo, hi) : float(std::atof(fallback));
}

// ---- The lists (see kRvaCraftingRead) -------------------------------------------------------------------------------------
// One entry of a game list as read (SEH-guarded reads; the strings made after).
struct RawModel {
    std::uint16_t object = 0;
    std::int16_t name = -1, description = -1;
    const std::uint8_t* outfit = nullptr;
    const char* debugName = nullptr;
    std::uint32_t voice = 0;
    std::uint16_t weaponObject = 0;
};

int ReadOutfits(RawModel* out, int max) noexcept {
    int n = 0;
    __try {
        auto* crafting = g_craftingGlobal ? *reinterpret_cast<std::uint8_t**>(g_craftingGlobal) : nullptr;
        if (!crafting) return -1;
        const std::uint32_t count = *reinterpret_cast<std::uint32_t*>(crafting + kCraftingOutfitCount);
        auto* list = *reinterpret_cast<std::uint8_t**>(crafting + kCraftingOutfits);
        for (std::uint32_t i = 0; list && i < count && i < 4096 && n < max; ++i) {
            const std::uint8_t* o = list + std::size_t(i) * kOutfitSize;
            RawModel& m = out[n];
            m = RawModel{};
            m.object = *reinterpret_cast<const std::uint16_t*>(o + kOutfitObject);
            if (!m.object) continue;
            m.name = *reinterpret_cast<const std::int16_t*>(o + kOutfitName);
            m.description = *reinterpret_cast<const std::int16_t*>(o + kOutfitDescription);
            m.outfit = o;
            m.debugName = *reinterpret_cast<const char* const*>(o + kOutfitDebugName);
            ++n;
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return n ? n : -1;
    }
}

int ReadWeapons(RawModel* out, int max) noexcept {
    int n = 0;
    __try {
        auto* info = g_uiInfoGlobal ? *reinterpret_cast<std::uint8_t**>(g_uiInfoGlobal) : nullptr;
        if (!info) return -1;
        const std::uint32_t count = *reinterpret_cast<std::uint32_t*>(info + kUiWeaponCount);
        auto* list = *reinterpret_cast<std::uint8_t**>(info + kUiWeapons);
        for (std::uint32_t i = 0; list && i < count && i < 4096 && n < max; ++i) {
            const std::uint8_t* w = list + std::size_t(i) * kWeaponSize;
            if (w[kWeaponBroken]) continue;   // (a broken weapon's parts)
            RawModel& m = out[n];
            m = RawModel{};
            m.weaponObject = *reinterpret_cast<const std::uint16_t*>(w + kWeaponObject);
            m.object = *reinterpret_cast<const std::uint16_t*>(w + kWeaponUiObject);
            if (!m.object) m.object = m.weaponObject;
            if (!m.object) continue;
            m.name = *reinterpret_cast<const std::int16_t*>(w + kWeaponName);
            m.description = *reinterpret_cast<const std::int16_t*>(w + kWeaponDescription);
            ++n;
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return n ? n : -1;
    }
}

int ReadRelics(RawModel* out, int max) noexcept {
    int n = 0;
    __try {
        auto* info = g_uiInfoGlobal ? *reinterpret_cast<std::uint8_t**>(g_uiInfoGlobal) : nullptr;
        if (!info) return -1;
        auto* sets = *reinterpret_cast<std::uint8_t**>(info + kUiCollectibleSets);
        const std::uint32_t setCount = *reinterpret_cast<std::uint32_t*>(info + kUiCollectibleSetCount);
        for (std::uint32_t s = 0; sets && s < setCount && s < 1024; ++s) {
            const std::uint8_t* set = sets + std::size_t(s) * kSetSize;
            if (*reinterpret_cast<const std::uint32_t*>(set + kSetType) != kCollectibleRelic) continue;
            auto* items = *reinterpret_cast<std::uint8_t* const*>(set + kSetItems);
            const std::uint32_t itemCount = *reinterpret_cast<const std::uint32_t*>(set + kSetItemCount);
            for (std::uint32_t i = 0; items && i < itemCount && i < 1024 && n < max; ++i) {
                const std::uint8_t* item = items + std::size_t(i) * kItemSize;
                if (*reinterpret_cast<const std::uint32_t*>(item + kItemType) != kCollectibleRelic) continue;
                RawModel& m = out[n];
                m = RawModel{};
                m.object = *reinterpret_cast<const std::uint16_t*>(item + kItemObject);
                if (!m.object || m.object == 0xFFFF) continue;
                m.name = *reinterpret_cast<const std::int16_t*>(item + kItemName);
                m.description = *reinterpret_cast<const std::int16_t*>(item + kItemDescription);
                m.voice = *reinterpret_cast<const std::uint32_t*>(item + kItemAudioClip);
                ++n;
            }
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return n ? n : -1;
    }
}

bool ReadByte(const char* at, char& out) noexcept {
    __try {
        out = *at;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string SafeText(const char* text) {
    if (!text) return {};
    char copy[256]{};
    std::size_t k = 0;
    for (; k + 1 < sizeof(copy); ++k) {
        char c = 0;
        if (!ReadByte(text + k, c) || !c) break;
        copy[k] = c;
    }
    return std::string(copy, k);
}

// A game list's models: its objects by name (once each, in the list's order), its names (the game's text; the debug
// name, else the object's, without one).
Category ListCategory(const char* name, Kind kind, const std::vector<RawModel>& raw, int read, const char* what) {
    Category c;
    c.name = name;
    c.kind = kind;
    if (read < 0) {
        Log::Writef("ModelViewer: the game's %s could not be read", what);
        return c;
    }
    int unnamed = 0, unknown = 0;
    for (int i = 0; i < read; ++i) {
        const RawModel& r = raw[std::size_t(i)];
        const char* object = ZoneList::Name(r.object);
        if (!object || !*object) {
            ++unknown;
            continue;
        }
        bool twice = false;
        for (const Model& m : c.models) twice = twice || _stricmp(m.object.c_str(), object) == 0;
        if (twice) continue;
        Model m;
        m.object = object;
        m.name = r.name >= 0 ? ViewerText::GameText(r.name) : std::string();
        m.description = r.description >= 0 ? ViewerText::GameText(r.description) : std::string();
        m.outfit = r.outfit;
        m.voice = r.voice;
        m.weaponObject = r.weaponObject;
        if (m.name.empty()) {
            ++unnamed;
            m.name = r.debugName ? SafeText(r.debugName) : std::string();
            if (m.name.empty()) m.name = m.object;
        }
        c.models.push_back(std::move(m));
    }
    Log::Writef("ModelViewer: %s: %zu models from the game's %s (%d read%s%s)", c.name.c_str(), c.models.size(), what, read,
                unknown ? ", some with no object in the game's list" : "", unnamed ? ", some without a name" : "");
    return c;
}

Category NamedCategory(const char* name, Kind kind, const Named* models, std::size_t count) {
    Category c;
    c.name = name;
    c.kind = kind;
    for (std::size_t i = 0; i < count; ++i)
        c.models.push_back({models[i].object, ViewerText::Pick(models[i].name), std::string(), nullptr, 0});
    return c;
}

std::filesystem::path ViewerFolder() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe).parent_path() / L"ROTTRFramework" / L"model_viewer";
}

// The lists for the Modding Suite (its grid pictures): models.json, when it changed. (v0.9.510, for the Suite's
// 2.0.121 pictures) Each model's kind (its category's); a character's idle as the viewer plays it (kCharacterIdles' clip,
// the looping one of three, by its ID in that object's animations; else Lara's ground_idle by name); a weapon's look
// without its upgrades as the viewer shows it with ALL UPGRADES off ("hide": its draw groups hidden, 1..64: a menu
// model's look from kMenuLooks, else the one ReadUpgradeLook works out from the game's data; none known: no "hide").
const char* KindName(Kind kind) {
    switch (kind) {
        case Kind::Outfits: return "outfits";
        case Kind::Characters: return "characters";
        case Kind::Animals: return "animals";
        case Kind::Weapons: return "weapons";
        case Kind::Relics: return "relics";
    }
    return "";
}
void WriteModelList() {
    nlohmann::json j;
    j["about"] = "The framework's Model Viewer lists (written each time it opens): the Modding Suite's Models tab makes "
                 "the grid's pictures of them (icons\\<object>.png): the characters in their idle's first frame, the "
                 "weapons in their look without upgrades (hide: the draw groups hidden).";
    auto& categories = j["categories"] = nlohmann::json::array();
    for (const Category& c : g_categories) {
        nlohmann::json category;
        category["name"] = c.name;
        auto& models = category["models"] = nlohmann::json::array();
        for (const Model& m : c.models) {
            nlohmann::json model = {{"object", m.object}, {"name", m.name}, {"gamePicture", m.outfit != nullptr},
                                    {"kind", KindName(c.kind)}};
            if (c.kind == Kind::Characters) {
                if (const CharacterIdle* own = CharacterIdleOf(m.object))
                    model["idle"] = {{"object", own->source}, {"clip", own->clips[own->clipCount == 3 ? 1 : 0]}};
                else
                    model["idle"] = {{"object", "laracroft"}, {"name", "ground_idle"}};
            }
            if (c.kind == Kind::Weapons) {
                std::uint64_t none = 0;
                bool known = false;
                if (const MenuLook* menu = MenuLookOf(m.object)) {
                    none = menu->none, known = true;
                } else if (m.weaponObject && g_weaponUpgradesGlobal && g_traitsGlobal) {
                    UpgradeLook look;
                    if (ReadUpgradeLook(m.weaponObject, look) && look.known) none = look.none, known = true;
                }
                if (known) {
                    nlohmann::json hide = nlohmann::json::array();
                    for (int b = 0; b < 64; ++b)
                        if ((none >> b) & 1u) hide.push_back(b + 1);
                    model["hide"] = std::move(hide);
                }
            }
            models.push_back(std::move(model));
        }
        categories.push_back(std::move(category));
    }
    const std::string text = j.dump(1, '\t', false, nlohmann::json::error_handler_t::replace);
    const std::filesystem::path file = ViewerFolder() / L"models.json";
    {
        std::ifstream in(file, std::ios::binary);
        if (in) {
            const std::string before((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (before == text) return;
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    if (out) Log::Write("ModelViewer: the lists written to ROTTRFramework\\model_viewer\\models.json (for the Modding Suite's pictures)");
    else Log::Write("ModelViewer: ROTTRFramework\\model_viewer\\models.json could not be written");
}

void LoadModels() {
    std::vector<RawModel> raw(2048);
    std::vector<Category> made;
    made.push_back(ListCategory(ViewerText::Get(ViewerText::Outfits), Kind::Outfits, raw, ReadOutfits(raw.data(), int(raw.size())),
                                "outfits"));
    made.push_back(NamedCategory(ViewerText::Get(ViewerText::Characters), Kind::Characters, kCharacters, std::size(kCharacters)));
    made.push_back(NamedCategory(ViewerText::Get(ViewerText::Animals), Kind::Animals, kAnimals, std::size(kAnimals)));
    made.push_back(ListCategory(ViewerText::Get(ViewerText::Weapons), Kind::Weapons, raw, ReadWeapons(raw.data(), int(raw.size())),
                                "weapons"));
    made.push_back(ListCategory(ViewerText::Get(ViewerText::Relics), Kind::Relics, raw, ReadRelics(raw.data(), int(raw.size())),
                                "relics"));
    // (v0.9.436) the relics left out of the list; the characters', the animals' and the relics' names in capitals, as
    // the game's outfit and weapon names are
    for (Category& c : made) {
        if (c.kind == Kind::Relics)
            c.models.erase(std::remove_if(c.models.begin(), c.models.end(),
                                          [](const Model& m) {
                                              for (const char* out : kRelicsLeftOut)
                                                  if (_stricmp(m.object.c_str(), out) == 0) return true;
                                              return false;
                                          }),
                           c.models.end());
        if (c.kind == Kind::Characters || c.kind == Kind::Animals || c.kind == Kind::Relics)
            for (Model& m : c.models) m.name = ViewerText::ToUpper(m.name);
    }
    made.erase(std::remove_if(made.begin(), made.end(), [](const Category& c) { return c.models.empty(); }), made.end());
    // (each category where it was)
    for (auto& c : made)
        for (const auto& before : g_categories)
            if (before.name == c.name && before.index < int(c.models.size())) c.index = before.index;
    const std::string current = g_category < int(g_categories.size()) ? g_categories[std::size_t(g_category)].name : std::string();
    g_categories = std::move(made);
    g_category = 0;
    for (int i = 0; i < int(g_categories.size()); ++i)
        if (g_categories[std::size_t(i)].name == current) g_category = i;
    WriteModelList();
}

void LoadSettings() {
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* slash = std::strrchr(path, '\\')) slash[1] = '\0';
    strncat_s(path, "ROTTRFramework.ini", _TRUNCATE);
    LoadModels();
    g_height = IniFloat(path, "Height", "3000", 0.0f, 100000.0f);
    g_ahead = IniFloat(path, "Ahead", "500", 50.0f, 10000.0f);
    g_brightness = IniFloat(path, "Brightness", "1", 0.1f, 10.0f);
    g_gridModelX = IniFloat(path, "GridModelX", "0.42", -1.0f, 1.0f);
    g_gridModelHalf = IniFloat(path, "GridModelHalf", "0.42", 0.05f, 1.0f);
    g_gridScale = IniFloat(path, "GridScale", "0.55", 0.3f, 1.0f);
    g_gridLeft = IniFloat(path, "GridLeft", "64", -200.0f, 1280.0f);
    g_gridTop = IniFloat(path, "GridTop", "112", -200.0f, 720.0f);
    {   // the stream's name: under pcx64-w\audio\streams\, without .mul (either given is taken off); Off: none
        char text[260]{};
        GetPrivateProfileStringA("ModelViewer", "Music", "music\\700_ui_stingers\\ui_campsite_menu_loop_02", text, sizeof(text),
                                 path);
        std::string track = text;
        for (char& c : track) c = c == '/' ? '\\' : char(std::tolower(static_cast<unsigned char>(c)));
        while (!track.empty() && (track.back() == ' ' || track.back() == '\\')) track.pop_back();
        while (!track.empty() && (track.front() == ' ' || track.front() == '\\')) track.erase(track.begin());
        if (track.rfind("pcx64-w\\audio\\streams\\", 0) == 0) track.erase(0, 22);
        if (track.size() > 4 && track.compare(track.size() - 4, 4, ".mul") == 0) track.resize(track.size() - 4);
        g_musicTrack = track == "off" || track == "none" ? std::string() : track;
    }
    LoadSuiteBounds();
    char background[32]{};
    GetPrivateProfileStringA("ModelViewer", "Background", "Black", background, sizeof(background), path);
    g_blackBackground = _stricmp(background, "Scene") != 0;
    char sceneLight[32]{};
    GetPrivateProfileStringA("ModelViewer", "SceneLight", "Off", sceneLight, sizeof(sceneLight), path);
    g_sceneLight = _stricmp(sceneLight, "On") == 0;
    char autoExposure[32]{};
    GetPrivateProfileStringA("ModelViewer", "AutoExposure", "Off", autoExposure, sizeof(autoExposure), path);
    g_autoExposure = _stricmp(autoExposure, "On") == 0;
    char occlusion[32]{};
    GetPrivateProfileStringA("ModelViewer", "AmbientOcclusion", "On", occlusion, sizeof(occlusion), path);
    g_gameAo = _stricmp(occlusion, "Off") != 0;
    char wind[32]{};
    GetPrivateProfileStringA("ModelViewer", "Wind", "Off", wind, sizeof(wind), path);
    g_wind = _stricmp(wind, "On") == 0;
    char idle[32]{};
    GetPrivateProfileStringA("ModelViewer", "OutfitIdle", "On", idle, sizeof(idle), path);
    g_outfitIdle = _stricmp(idle, "Off") != 0;
}

// The black background on while a model is shown and the filter's layout was found as expected in its scene.
void UpdateIsolation() {
    void* instance = g_phase == Phase::Shown ? g_instance : nullptr;
    void* scene = instance ? Features::ViewerSceneOf(instance) : nullptr;
    if (instance && scene && scene != g_isolationCheckedScene && !g_isolationFailed) {
        const int check = CheckIsolation(instance, scene);
        if (check == 0) {
            g_isolationCheckedScene = scene;
            Log::Writef("ModelViewer: the black background's filter checked on %s (scene %p)", ModelName().c_str(), scene);
        } else if (check != 1) {   // (no anim drawable: checked on the next model)
            g_isolationFailed = true;
            static const char* const kWhy[] = {"a fault", "", "", "its entity's drawable is not it", "its +0x10 is not the model",
                                               "its entity's scene is not the scene"};
            Log::Writef("ModelViewer: the black background is off: on %s's anim drawable, %s", ModelName().c_str(),
                        kWhy[std::clamp(check + 1, 0, 5)]);
            SetStatus("The black background is not available (the log says why).");
        }
    }
    if (scene && scene == g_isolationCheckedScene) g_isolateScene.store(scene, std::memory_order_release);
    g_isolateInstance.store(instance, std::memory_order_release);
    g_isolate.store(g_hooksMade && g_blackBackground.load() && !g_isolationFailed &&
                        g_isolateScene.load(std::memory_order_acquire) != nullptr,
                    std::memory_order_release);
    g_studioOnly.store(g_hooksMade && !g_sceneLight.load(), std::memory_order_release);
    g_exposureFixed.store(g_hooksMade && !g_autoExposure.load(), std::memory_order_release);
    g_aoOff.store(g_ssaoHooked && instance && !g_gameAo.load(), std::memory_order_release);
    g_windOff.store(g_windHooked && instance && !g_wind.load(), std::memory_order_release);
    const bool sss = g_sssHooked && instance;
    if (sss != g_sssOn.exchange(sss, std::memory_order_acq_rel))
        RenderTweaks::SetViewerSkinScattering(sss);   // (v0.9.540: it keeps the cutscenes' toggle set while asked)
    if (g_motionBlur && *g_motionBlur) {   // (v0.9.445: no motion blur while open)
        *g_motionBlur = 0;
        if (!g_motionBlurCleared) Log::Write("ModelViewer: the game's motion blur off while the viewer is open");
        g_motionBlurCleared = true;
    }
}

// The black background from the first frame (v0.9.436): before the viewer's first model is shown (the scene it is
// checked on, see UpdateIsolation) the menu level's scene from one of its own instances, checked as the model is
// (CheckIsolation: its anim drawable's entity is that drawable's, the drawable's owner the instance, the entity's scene
// the scene), so the level is left out while the first model loads (the horizon showed for a moment before). The models
// are born in the menu level's unit (ChooseUnit), in that scene.
void FindMenuScene() {
    if (g_isolationCheckedScene || g_isolationFailed) return;
    Features::LiveInstance live[64];
    const int count = Features::CopyLiveInstancesNear(g_menuCamera.position, 1.0e7f, live, int(std::size(live)));
    for (int i = 0; i < count; ++i) {
        void* instance = Features::InstanceInSlotGameThread(live[i].id);
        void* scene = instance ? Features::ViewerSceneOf(instance) : nullptr;
        if (!scene || CheckIsolation(instance, scene) != 0) continue;
        g_isolationCheckedScene = scene;
        g_isolateScene.store(scene, std::memory_order_release);
        Log::Writef("ModelViewer: the black background's filter checked on the menu level's instance %u (scene %p), before the "
                    "first model", live[i].id, scene);
        return;
    }
    Log::Writef("ModelViewer: none of the menu level's %d instances near gave its scene: the black background comes with the "
                "first model", count);
}

// The shown model turns by itself on the grid: a weapon, a relic (v0.9.431: Lara's outfits, the characters and the
// animals stand still there).
bool ShownTurns() {
    if (g_shownCategory < 0 || g_shownCategory >= int(g_categories.size())) return false;
    const Kind kind = g_categories[std::size_t(g_shownCategory)].kind;
    return kind == Kind::Weapons || kind == Kind::Relics;
}

}  // namespace

// The render hooks (enabled with the init's MH_ALL_HOOKS): the black background, the studio lights alone, the fixed
// exposure (the studio lights are drawn deferred by Features).
bool CreateHooks() {
    if (!Check()) return false;
    struct Hook {
        std::uintptr_t rva;
        void* detour;
        void** original;
        const char* what;
    };
    const Hook hooks[] = {
        {kRvaDrawEntity, reinterpret_cast<void*>(&HookDrawEntity), reinterpret_cast<void**>(&g_origDrawEntity), "Scene::DrawEntity"},
        {kRvaDrawStaticMeshes, reinterpret_cast<void*>(&HookDrawStaticMeshes), reinterpret_cast<void**>(&g_origDrawStaticMeshes),
         "Scene::DrawStaticMeshes"},
        {kRvaDrawGrass, reinterpret_cast<void*>(&HookDrawGrass), reinterpret_cast<void**>(&g_origDrawGrass), "Scene::DrawGrass"},
        {kRvaRendererRenderLight, reinterpret_cast<void*>(&HookRendererRenderLight),
         reinterpret_cast<void**>(&g_origRendererRenderLight), "LightRenderer::RenderLight"},
        {kRvaApplyAmbient, reinterpret_cast<void*>(&HookApplyAmbient), reinterpret_cast<void**>(&g_origApplyAmbient), "ApplyAmbient"},
        {kRvaApplySecondAmbient, reinterpret_cast<void*>(&HookApplySecondAmbient), reinterpret_cast<void**>(&g_origApplySecondAmbient),
         "ApplySecondAmbient"},
        {kRvaApplyIrradiance, reinterpret_cast<void*>(&HookApplyIrradiance), reinterpret_cast<void**>(&g_origApplyIrradiance),
         "ApplyIrradiance"},
        {kRvaUpdateBeautySpecular, reinterpret_cast<void*>(&HookUpdateBeautySpecular),
         reinterpret_cast<void**>(&g_origUpdateBeautySpecular), "UpdateBeautySpecular"},
        {kRvaSetupTonemapParams, reinterpret_cast<void*>(&HookSetupTonemapParams), reinterpret_cast<void**>(&g_origSetupTonemap),
         "BloomRender::SetupTonemapParams"},
    };
    if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaGetComponent), reinterpret_cast<void*>(&HookGetComponent),
                      reinterpret_cast<void**>(&g_origGetComponent)) == MH_OK)
        g_getComponentHooked = true;
    else
        Log::Write("ModelViewer: the ObjectZoneRef::GetComponent hook was not created; the models keep their sounds, Lara's "
                   "outfits are not shown");
    if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaFindItemList), reinterpret_cast<void*>(&HookFindItemList),
                      reinterpret_cast<void**>(&g_origFindItemList)) == MH_OK)
        g_findItemListHooked = true;
    else
        Log::Write("ModelViewer: the object data's item lists hook was not created; the models keep their sounds, Lara's "
                   "outfits are not shown");
    if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaObjectFlags), reinterpret_cast<void*>(&HookObjectFlags),
                      reinterpret_cast<void**>(&g_origObjectFlags)) == MH_OK)
        g_objectFlagsHooked = true;
    else
        Log::Write("ModelViewer: the object flags hook was not created; the models keep their sounds, Lara's outfits are "
                   "not shown");
    int made = 0;
    for (const Hook& h : hooks) {
        if (MH_CreateHook(reinterpret_cast<void*>(g_base + h.rva), h.detour, h.original) != MH_OK) {
            Log::Writef("ModelViewer: the %s hook was not created; the background, the scene's own light and the eye "
                        "adaptation stay", h.what);
            for (int i = 0; i < made; ++i) MH_RemoveHook(reinterpret_cast<void*>(g_base + hooks[i].rva));
            return true;   // (the viewer works without them)
        }
        ++made;
    }
    g_hooksMade = true;
    if (g_cubeReflVtableOk && MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaCubeReflConstantsDraw),
                                            reinterpret_cast<void*>(&HookCubeReflConstantsDraw),
                                            reinterpret_cast<void**>(&g_origCubeReflConstantsDraw)) == MH_OK)
        g_cubeReflHooked = true;
    else
        Log::Write("ModelViewer: the reflections' constants hook was not created; the level's reflections stay");
    if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaDoColorCorrection), reinterpret_cast<void*>(&HookDoColorCorrection),
                      reinterpret_cast<void**>(&g_origDoColorCorrection)) == MH_OK)
        g_colorCorrectionHooked = true;
    else
        Log::Write("ModelViewer: the colour grading hook was not created; the level's grading stays");
    if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaSsaoInterpolate), reinterpret_cast<void*>(&HookSsaoInterpolate),
                      reinterpret_cast<void**>(&g_origSsaoInterpolate)) == MH_OK)
        g_ssaoHooked = true;
    else
        Log::Write("ModelViewer: the ambient occlusion's hook was not created; the game's occlusion stays");
    if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaGetWindForce), reinterpret_cast<void*>(&HookGetWindForce),
                      reinterpret_cast<void**>(&g_origGetWindForce)) == MH_OK)
        g_windHooked = true;
    else
        Log::Write("ModelViewer: the wind's hook was not created; the level's wind blows the models' hair and cloth");
    if (RenderTweaks::SkinScatteringAvailable() &&
        MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaSssDraw), reinterpret_cast<void*>(&HookSssDraw),
                      reinterpret_cast<void**>(&g_origSssDraw)) == MH_OK)
        g_sssHooked = true;
    else
        Log::Write("ModelViewer: the subsurface scattering's hooks were not created; the skin is drawn without it");
    Log::Detail("ModelViewer: hooks created (Scene::DrawEntity, DrawStaticMeshes, DrawGrass; "
                "LightRenderer::RenderLight, ApplyAmbient, ApplySecondAmbient, ApplyIrradiance, UpdateBeautySpecular; "
                "BloomRender::SetupTonemapParams; CubeReflRenderConstantsDrawable::Draw; LensEffectRender::DoColorCorrection; "
                "SSAORender's InterpolateParams; FXWindMgr::GetWindForce; DebugDrawFlags_GetMdlView, SSSRender::Draw)");
    return true;
}

bool Available() {
    return Check() && Features::ViewerObjectsAvailable() && Features::ViewerCameraAvailable();
}

bool IsOpen() { return g_open.load(); }
bool OwnsInput() { return g_open.load() && g_screen.load() == Screen::View; }
void RequestReset() { g_resetRequest = true; }
float GridScale() { return g_gridScale; }
float GridLeft() { return g_gridLeft; }
float GridTop() { return g_gridTop; }
Kind CategoryKind(int category) {
    return category >= 0 && category < int(g_categories.size()) ? g_categories[std::size_t(category)].kind : Kind::Outfits;
}

// The view's button bar (the game's legend, bottom right: Reset, Back...): there the mouse is the game's (v0.9.422).
bool InButtonBar(float x, float y, float width, float height) {
    return width > 0 && height > 0 && y >= height * 0.88f && x >= width * 0.40f;
}
bool SetBackdropHidden(bool hidden) {   // (v0.9.508)
    if (!g_hooksMade) return false;
    g_hideBackdrop.store(hidden, std::memory_order_release);
    return true;
}
std::uint32_t TakeBackdropHiddenDraws() { return g_backdropHiddenDraws.exchange(0); }
void SetBackdropDarkness(float darkness) {   // (v0.9.509)
    g_backdropDarkness.store(std::clamp(darkness, 0.0f, 1.0f), std::memory_order_release);
}
std::uint32_t TakeBackdropDimmed() { return g_backdropDimmed.exchange(0); }

bool WantsFrame() { return g_open.load(); }

bool Open() {
    if (g_open.load()) return true;
    if (!Available()) {
        Log::Write("ModelViewer: not available (the Birth Instance functions or the camera hook are missing)");
        return false;
    }
    Features::SnapshotDrawCameraNow();
    if (!Features::GetDrawCameraState(g_menuCamera) || !g_menuCamera.valid) {
        Log::Write("ModelViewer: the menu's camera could not be read; not opened");
        return false;
    }
    g_fov = std::isfinite(g_menuCamera.fov) && g_menuCamera.fov > 0.1f && g_menuCamera.fov < 3.0f ? g_menuCamera.fov : 0.9f;
    g_filterReady = FilterUsable();
    g_soundOn = false;
    g_soundToggle = false;
    LoadSettings();
    if (g_categories.empty()) {
        Log::Write("ModelViewer: no models to show; not opened");
        return false;
    }
    LogUnits();
    ResetView();
    g_lightsToggle = g_resetRequest = g_placeRequest = false;
    g_brightnessStep = 0;
    g_step = 0, g_categoryStep = 0;
    g_backRequest = false;
    g_screen = Screen::Grid;
    g_shownCategory = g_shownIndex = -1;
    g_cursorAt = Now();
    for (auto& d : g_padDrain) d = false;
    g_lightsOn = true;
    g_lightsGaveUp = false;
    g_lightTries = 0;
    g_backgroundToggle = false;
    g_sceneLightToggle = false;
    g_autoExposureToggle = false;
    g_gameAoToggle = false;
    g_uiHideToggle = false;
    g_outfitIdleToggle = false;
    g_isolationFailed = false;
    {
        std::lock_guard lock(g_tonemapMutex);
        g_tonemapSeenValid = false;
    }
    {
        std::lock_guard lock(g_reflectionMutex);
        g_reflectionSeenValid = false;
    }
    g_exposureFixed.store(g_hooksMade && !g_autoExposure.load(), std::memory_order_release);   // (from the first frame)
    g_lightsTriedAt = -10.0;
    PlaceFor(g_place.load(), g_at);
    g_phase = Phase::Closed;
    SetStatus("");
    // no depth of field while open (v0.9.422: the menu camera's blurred the model; the F7 toggle's, back on Close)
    g_depthOfFieldOff = Features::DepthOfFieldAvailable() && Features::DepthOfFieldEnabled();
    if (g_depthOfFieldOff) Features::SetDepthOfFieldEnabled(false);
    g_open = true;
    g_lastTick = Now();
    g_voiceCategory = g_voiceIndex = -1;
    {   // the music, the level's sounds paused (v0.9.431); the relics' lines for the owner of ours
        std::vector<std::uint32_t> clips;
        for (const Category& c : g_categories)
            if (c.kind == Kind::Relics)
                for (const Model& m : c.models)
                    if (m.voice) clips.push_back(m.voice);
        ViewerAudio::Start(clips, g_musicTrack);
    }
    {
        const float* q = g_menuCamera.quaternion;
        Log::Writef("ModelViewer: open (%d models in %d categories; the menu's camera at (%.0f, %.0f, %.0f) looking (%.2f, %.2f, %.2f), field of view "
                    "%.1f deg; the model %s at (%.0f, %.0f, %.0f); lights %s)", ModelCount(), int(g_categories.size()),
                    g_menuCamera.position[0],
                    g_menuCamera.position[1], g_menuCamera.position[2], 2.0f * (q[0] * q[1] - q[2] * q[3]),
                    1.0f - 2.0f * (q[0] * q[0] + q[2] * q[2]), 2.0f * (q[1] * q[2] + q[0] * q[3]), g_fov * 180.0f / kPi,
                    kPlaceNames[g_place.load()], g_at[0], g_at[1], g_at[2],
                    Features::ViewerLightsAvailable() ? "the Suite's studio preset" : "not available (the light editor's are off)");
    }
    FindMenuScene();
    UpdateCamera();
    return true;
}

void Close() {
    if (!g_open.load()) return;
    ViewerAudio::Stop();
    g_isolate.store(false, std::memory_order_release);
    g_isolateInstance.store(nullptr, std::memory_order_release);
    g_studioOnly.store(false, std::memory_order_release);   // (the game's next sub-frame sets its own light again)
    g_exposureFixed.store(false, std::memory_order_release);
    g_aoOff.store(false, std::memory_order_release);
    g_windOff.store(false, std::memory_order_release);
    if (g_sssOn.exchange(false, std::memory_order_acq_rel)) RenderTweaks::SetViewerSkinScattering(false);
    if (g_motionBlurCleared && g_motionBlur) *g_motionBlur = 1;
    g_motionBlurCleared = false;
    Features::SetViewerCamera(nullptr, nullptr);
    ReleaseLights();
    Unload();
    ReleaseKept();
    if (g_laraZoneRef) Features::ViewerZoneRelease(g_laraZone);
    g_laraZoneRef = false;
    ReleaseIdleSources();
    g_nextFor[0] = g_nextFor[1] = -1;
    if (g_depthOfFieldOff) Features::SetDepthOfFieldEnabled(true);
    g_depthOfFieldOff = false;
    g_phase = Phase::Closed;
    g_open = false;
    g_closedAt = Now();
    for (int i = 0; i < 4; ++i) g_padDrain[i] = g_padHeld[i].load() != 0;
    Log::Write("ModelViewer: closed");
}

void SetScreen(Screen screen) {
    const Screen before = g_screen.exchange(screen);
    if (before == screen) return;
    ResetView();
    g_backRequest = false;
    if (before == Screen::View) {   // (the pad's buttons let go before the game sees them again: no second Back)
        g_viewLeftAt = Now();
        for (int i = 0; i < 4; ++i) g_padDrain[i] = g_padHeld[i].load() != 0;
    }
    {
        std::lock_guard lock(g_padMutex);
        g_padRX = g_padRY = g_padLX = g_padLY = g_padLT = g_padRT = 0.0f;
    }
    if (screen == Screen::View) g_cursorAt = Now() - kGridRest;   // (the choice at once)
}

int CategoryCount() { return int(g_categories.size()); }
std::string CategoryName(int category) {
    return category >= 0 && category < int(g_categories.size()) ? g_categories[std::size_t(category)].name : std::string();
}
int ModelCount(int category) {
    return category >= 0 && category < int(g_categories.size()) ? int(g_categories[std::size_t(category)].models.size()) : 0;
}
std::string ModelLabel(int category, int index) {
    if (index < 0 || index >= ModelCount(category)) return {};
    return g_categories[std::size_t(category)].models[std::size_t(index)].name;
}
std::string ModelObject(int category, int index) {
    if (index < 0 || index >= ModelCount(category)) return {};
    return g_categories[std::size_t(category)].models[std::size_t(index)].object;
}
std::string ModelDescription(int category, int index) {
    if (index < 0 || index >= ModelCount(category)) return {};
    return g_categories[std::size_t(category)].models[std::size_t(index)].description;
}
const void* ModelOutfit(int category, int index) {
    if (index < 0 || index >= ModelCount(category)) return nullptr;
    return g_categories[std::size_t(category)].models[std::size_t(index)].outfit;
}
std::wstring ModelIcon(int category, int index) {
    if (index < 0 || index >= ModelCount(category)) return {};
    const std::string& object = g_categories[std::size_t(category)].models[std::size_t(index)].object;
    return (ViewerFolder() / L"icons" / std::filesystem::path(std::wstring(object.begin(), object.end()) + L".png")).wstring();
}
bool SoundOn() { return g_soundOn.load(); }
void Choose(int category, int index) {
    if (index < 0 || index >= ModelCount(category)) return;
    if (category == g_category && index == g_categories[std::size_t(category)].index) return;
    g_category = category;
    g_categories[std::size_t(category)].index = index;
    g_cursorAt = Now();
}
void ChooseCategory(int category) {
    if (category < 0 || category >= int(g_categories.size()) || category == g_category) return;
    g_category = category;
    g_cursorAt = Now();
}
void Current(int& category, int& index) {
    category = g_category;
    index = g_category >= 0 && g_category < int(g_categories.size()) ? g_categories[std::size_t(g_category)].index : 0;
}
unsigned ChoiceSerial() { return g_choiceSerial.load(); }
bool TakeBack() { return g_backRequest.exchange(false); }
bool TakeUiHideToggle() { return g_uiHideToggle.exchange(false); }
void RequestLine() { g_lineRequest = true; }
bool WeaponUpgradesAvailable() { return g_weaponUpgradesGlobal && g_traitsGlobal; }
bool AllUpgrades() { return g_allUpgrades.load(); }
void SetAllUpgrades(bool on) {
    if (g_allUpgrades.exchange(on) != on) Log::Writef("ModelViewer: ALL UPGRADES %s", on ? "on" : "off");
}
bool ModelShown() { return g_open.load() && g_phase == Phase::Shown; }
bool ModelLoading() {
    if (!g_open.load()) return false;
    const int index = g_category >= 0 && g_category < int(g_categories.size()) ? g_categories[std::size_t(g_category)].index : -1;
    return g_phase == Phase::Loading || g_category != g_shownCategory || index != g_shownIndex;
}
std::string Status() {
    std::lock_guard lock(g_textMutex);
    return g_status;
}

// The model shown once all of it is there (see g_revealed).
void RevealWhenReady(double now) {
    ++g_revealTicks;
    if (!g_bodyDrawable.load()) g_bodyDrawable = BodyDrawable(g_instance);
    const double waited = now - g_shownAt;
    const bool body = !g_bodyDrawable.load() ? g_otherDraws.load() > 0 : g_bodyDraws.load() > 0;
    // (a hair of the Suite's look may be loaded only now: up to the 2.5 s)
    const bool hair = !g_shownPlayer || !g_hairVtableOk || g_hairDraws.load() > 0 || waited > (g_hairSwapped.load() ? 2.4 : 1.0);
    const char* size = nullptr;
    const bool sized = SizeSource(&size) != nullptr;
    // (its lights made for it: v0.9.433, it was shown unlit until they were)
    const bool lit = !Features::ViewerLightsAvailable() || g_lightsGaveUp || (g_lightsMade && g_lightsForRelic == ShownIsRelic());
    // (an outfit standing as in the game before it is seen: v0.9.434)
    const bool posed = !ShownIdles() || !g_outfitIdle.load() || g_idlePlaying || g_idleTries >= kIdleTries;
    const bool ready = body && hair && sized && lit && posed && g_revealTicks >= 3;
    if (!ready && waited < 2.5) return;
    g_revealed.store(true, std::memory_order_release);
    Log::Detailf("ModelViewer: %s drawn after %d frames (%.2f s)%s: its body %s, its hair %s, its size from %s, its lights %s%s",
                ModelName().c_str(), g_revealTicks, waited, ready ? "" : " (2.5 s: not all of it there)",
                body ? "drawn" : "not drawn", !g_shownPlayer ? "-" : g_hairDraws.load() ? "drawn" : "none drawn", size,
                lit ? "made" : "not made yet", !ShownIdles() ? "" : g_idlePlaying ? ", standing idle" : ", in its bind pose");
}

// (v0.9.602) Where the viewer's frame time goes, written every 30 s while it is open: the frames (the game thread's
// ticks apart), this tick's own time, the draw hook's part per call (sampled) and the overlay's empty frames not
// submitted.
struct ViewerTiming {
    double startedAt = -1.0, tickMsSum = 0.0, tickMsMax = 0.0, gapMax = 0.0;
    std::uint32_t ticks = 0;
};
ViewerTiming g_timing;
void TickGameThreadTimed(double now);

void TickGameThread() {
    if (!g_open.load()) {
        g_timing = ViewerTiming{};
        return;
    }
    LARGE_INTEGER start, end, frequency;
    QueryPerformanceCounter(&start);
    const double now = Now();
    if (g_timing.startedAt < 0.0) {
        g_timing.startedAt = now;
        g_drawHookSampleTicks.store(0), g_drawHookSamples.store(0);
        Overlay::TakeEmptyFrames();
    } else {
        g_timing.gapMax = std::max(g_timing.gapMax, now - g_lastTick);
    }
    TickGameThreadTimed(now);
    QueryPerformanceCounter(&end);
    QueryPerformanceFrequency(&frequency);
    const double ms = double(end.QuadPart - start.QuadPart) * 1000.0 / double(frequency.QuadPart);
    ViewerTiming& t = g_timing;
    t.tickMsSum += ms, t.tickMsMax = std::max(t.tickMsMax, ms), ++t.ticks;
    if (now - t.startedAt >= 30.0 && t.ticks) {
        const double span = now - t.startedAt;
        const std::uint32_t samples = g_drawHookSamples.exchange(0);
        const double sampleMs = double(g_drawHookSampleTicks.exchange(0)) * 1000.0 / double(frequency.QuadPart);
        const std::uint32_t empty = Overlay::TakeEmptyFrames();
        Log::Writef("ModelViewer: timing over %.0f s: %u frames (%.1f ms apart, %.0f fps; the longest %.0f ms); the viewer's tick "
                    "%.3f ms a frame (max %.2f); the draw hook about %.0f calls a frame, its own part %.2f us a call; %u of the "
                    "overlay's frames had nothing to draw (not submitted)",
                    span, t.ticks, span * 1000.0 / t.ticks, t.ticks / span, t.gapMax * 1000.0, t.tickMsSum / t.ticks, t.tickMsMax,
                    double(samples) * 64.0 / t.ticks, samples ? sampleMs * 1000.0 / samples : 0.0, empty);
        g_timing = ViewerTiming{};
        g_timing.startedAt = now;
    }
}

void TickGameThreadTimed(double now) {
    const float dt = float(std::clamp(now - g_lastTick, 0.0, 0.1));
    g_lastTick = now;
    const bool view = g_screen.load() == Screen::View;
    // the choice: the view's own keys (the model, the category)
    if (!g_categories.empty()) {
        bool moved = false;
        const int categories = int(g_categories.size());
        if (const int categoryStep = g_categoryStep.exchange(0); categoryStep != 0 && categories > 1 && view) {
            g_category = ((g_category + categoryStep) % categories + categories) % categories;
            moved = true;
        }
        Category& c = g_categories[std::size_t(g_category)];
        const int n = int(c.models.size());
        if (const int step = g_step.exchange(0); step != 0 && n > 0 && view) {
            c.index = ((c.index + step) % n + n) % n;
            moved = true;
        }
        if (moved) {
            g_cursorAt = now;
            g_choiceSerial.fetch_add(1);
        }
        // the model shown follows: at once on the view, on the grid once the choice rests
        if ((g_category != g_shownCategory || c.index != g_shownIndex) && (view || now - g_cursorAt >= kGridRest)) {
            Unload();
            ResetView();
            StartLoad();
        }
    }
    {   // a relic's line on the view (v0.9.431): the model chosen there, each time it is chosen
        const bool any = view && g_category >= 0 && g_category < int(g_categories.size());
        const int category = any ? g_category : -1;
        const int index = any ? g_categories[std::size_t(category)].index : -1;
        if (category != g_voiceCategory || index != g_voiceIndex) {
            g_voiceCategory = category, g_voiceIndex = index;
            const Category* c = any ? &g_categories[std::size_t(category)] : nullptr;
            const Model* m = c && c->kind == Kind::Relics && index >= 0 && index < int(c->models.size()) ? &c->models[std::size_t(index)]
                                                                                                          : nullptr;
            if (m && m->voice) {
                ViewerAudio::PlayVoice(m->voice, m->object.c_str());
                g_lineCategory = g_lineIndex = -1;
            } else if (g_lineCategory < 0 || g_lineCategory != category || g_lineIndex != index) {
                // (a character's line on its own model goes on; v0.9.446: else stopped, leaving the view too: 0.9.443
                // took a relic's line, no character's (-1, -1), for one on the view left (-1, -1) and kept it)
                ViewerAudio::StopVoice();
            }
        }
    }
    {   // a character's line (v0.9.443): V on its model, on the grid or the view; ends when another model is chosen
        int category = 0, index = 0;
        Current(category, index);
        if (g_lineCategory >= 0 && (category != g_lineCategory || index != g_lineIndex)) {
            if (ViewerAudio::VoicePlaying()) ViewerAudio::StopVoice();
            g_lineCategory = g_lineIndex = -1;
        }
        if (g_lineRequest.exchange(false) && category >= 0 && category < int(g_categories.size())) {
            const Category& c = g_categories[std::size_t(category)];
            const Model* m = index >= 0 && index < int(c.models.size()) ? &c.models[std::size_t(index)] : nullptr;
            // (v0.9.449: on Lara's outfits the next of her lines, each V)
            const char* line = !m ? nullptr : c.kind == Kind::Outfits ? NextLaraLine() : CharacterLineOf(m->object);
            if (line) {
                ViewerAudio::PlayVoiceStream(line, m->object.c_str());
                g_lineCategory = category, g_lineIndex = index;
            } else if (m && m->voice) {   // (a relic's: its line again, until another model is chosen or the view left)
                ViewerAudio::PlayVoice(m->voice, m->object.c_str());
                g_lineCategory = category, g_lineIndex = index;
            }
        }
    }
    if (g_lightsToggle.exchange(false)) {
        g_lightsOn = !g_lightsOn.load();
        Log::Writef("ModelViewer: studio lights %s", g_lightsOn.load() ? "on" : "off (the scene's own light only)");
    }
    if (g_soundToggle.exchange(false)) {   // (the model born again with its sound components, or without)
        g_soundOn = !g_soundOn.load();
        Log::Writef("ModelViewer: the models' sound %s", g_soundOn.load() ? "on" : "off");
        SetStatus("");
        if (!g_categories.empty()) {
            Unload();
            StartLoad();
        }
    }
    if (g_backgroundToggle.exchange(false)) {
        g_blackBackground = !g_blackBackground.load();
        Log::Writef("ModelViewer: %s", g_blackBackground.load() ? "black background" : "the scene behind the model");
    }
    if (g_autoExposureToggle.exchange(false)) {
        g_autoExposure = !g_autoExposure.load();
        Log::Writef("ModelViewer: %s", g_autoExposure.load() ? "the game's tone mapping (its eye adaptation)"
                                                             : "the Suite's tone mapping (exposure 1.6, white point 4)");
    }
    if (g_gameAoToggle.exchange(false)) {
        g_gameAo = !g_gameAo.load();
        Log::Writef("ModelViewer: %s", g_gameAo.load() ? "the game's ambient occlusion on" : "the game's ambient occlusion off");
    }
    if (g_outfitIdleToggle.exchange(false)) {
        g_outfitIdle = !g_outfitIdle.load();
        g_idleTries = 0;
        Log::Writef("ModelViewer: %s", g_outfitIdle.load() ? "Lara's outfits and the characters stand idle as in the game"
                                                           : "Lara's outfits and the characters in their bind pose");
    }
    if (g_sceneLightToggle.exchange(false)) {
        g_sceneLight = !g_sceneLight.load();
        Log::Writef("ModelViewer: %s", g_sceneLight.load() ? "the scene's own light on (sun, ambient)" : "the studio lights alone");
    }
    if (g_resetRequest.exchange(false)) ResetView();
    if (const int steps = g_brightnessStep.exchange(0); steps != 0) {
        g_brightness = std::clamp(g_brightness.load() * std::pow(1.25f, float(steps)), 0.1f, 10.0f);
        Log::Writef("ModelViewer: studio lights' brightness %.2f", g_brightness.load());
    }
    if (g_placeRequest.exchange(false)) {
        const int place = (g_place.load() + 1) % kPlaceCount;
        g_place = place;
        PlaceFor(place, g_at);
        Log::Writef("ModelViewer: the model's place: %s, (%.0f, %.0f, %.0f)", kPlaceNames[place], g_at[0], g_at[1], g_at[2]);
    }
    if (view) {   // the pad's sticks: turn (right stick), move (left stick), zoom (the triggers)
        float rx, ry, lx, ly, lt, rt;
        {
            std::lock_guard lock(g_padMutex);
            rx = g_padRX, ry = g_padRY, lx = g_padLX, ly = g_padLY, lt = g_padLT, rt = g_padRT;
        }
        if (rx || ry || lx || ly || lt || rt) {
            const float distance = g_distanceNow.load();
            std::lock_guard lock(g_viewMutex);
            g_view.yaw += rx * 150.0f * dt;
            g_view.pitch = std::clamp(g_view.pitch - ry * 100.0f * dt, -80.0f, 80.0f);
            g_view.panX += lx * distance * 0.6f * dt;
            g_view.panZ += ly * distance * 0.6f * dt;
            g_view.zoom = std::clamp(g_view.zoom * std::pow(2.0f, (lt - rt) * dt * 1.5f), 0.15f, 6.0f);
        }
    } else if (g_phase == Phase::Shown && g_revealed.load(std::memory_order_acquire) && ShownTurns()) {   // (turns by itself)
        std::lock_guard lock(g_viewMutex);
        g_view.yaw += 20.0f * dt;
    }
    switch (g_phase) {
        case Phase::Loading:
            if (!Features::ViewerZoneLoaded(g_zone)) {
                if (now - g_loadStart > 30.0) {
                    g_phase = Phase::Failed;
                    SetStatus("%s did not load in 30 s.", ModelName().c_str());
                    Log::Writef("ModelViewer: %s (zone %u) not loaded after 30 s", ModelName().c_str(), g_zone);
                    Unload();
                }
                break;
            }
            {
                const void* objectData = nullptr;
                const int player = IsPlayerObject(g_zone, objectData);
                if (player < 0 || (player > 0 && !g_filterReady)) {
                    const std::string& name = ModelName();
                    g_phase = Phase::Failed;
                    if (player > 0) {
                        SetStatus("%s is a player object (Lara as an outfit): not shown (the component filter is off).", name.c_str());
                        Log::Writef("ModelViewer: %s is a player object (\"player\" component data): not born (its PlayerComponent "
                                    "needs a player slot, and the component filter is off: the log says why)", name.c_str());
                    } else {
                        SetStatus("%s: its object data could not be read.", name.c_str());
                        Log::Writef("ModelViewer: %s: its object data could not be read (zone %u); not born", name.c_str(), g_zone);
                    }
                    Unload();
                    break;
                }
                Birth(objectData, player > 0);
                break;   // (kept at its place from the next frame on)
            }
        case Phase::Shown: {
            void* alive = Features::InstanceNowGameThread(g_instanceId, g_componentId);
            if (alive != g_instance) {
                Log::Writef("ModelViewer: %s went away by itself (the game removed instance %u)", ModelName().c_str(),
                            g_instanceId);
                g_poseInstance.store(nullptr, std::memory_order_release);
                g_boundsInstance.store(nullptr, std::memory_order_release);
                RetireFilterSlot();
                g_instance = nullptr;
                g_phase = Phase::Failed;
                SetStatus("The game took %s away.", ModelName().c_str());
                break;
            }
            ApplyLook();
            KeepInPlace();   // (against the turn written last)
            TurnModel();
            if (ShownIdles() && g_outfitIdle.load() && !g_idlePlaying && g_idleTries < kIdleTries && now - g_idleTriedAt >= 0.5)
                StartIdle(now);
            else if (g_idlePlaying && !g_outfitIdle.load())
                StopIdle();
            InstanceAnim::KeepPoseBuilt(g_instance);
            if (!g_revealed.load(std::memory_order_acquire)) UpdateDrawnBounds();   // (measured unturned, then kept)
            if (!g_revealed.load(std::memory_order_acquire)) RevealWhenReady(now);
            else KeepNeighbours(now);   // (the next and the one before loaded meanwhile)
            if (!g_positionLogged && now - g_shownAt > 3.0) {
                g_positionLogged = true;
                g_noteHairLights.store(false, std::memory_order_relaxed);   // (v0.9.602: read below, then no longer noted)
                Log::Writef("ModelViewer: %s in 3 s: %u studio light draws (deferred), %u entities drawn and %u left out, %u "
                            "static mesh passes left out (black background %s); %u of the scene's lights left out, the ambient "
                            "the studio's %u times (studio lights alone %s)", ModelName().c_str(),
                            unsigned(Features::DeferredLightDraws() - g_lightDrawsBase),
                            g_entitiesDrawn.exchange(0), g_entitiesHidden.exchange(0), g_meshPassesHidden.exchange(0),
                            g_isolate.load() ? "on" : "off", g_gameLightsLeftOut.exchange(0), g_ambientZeroed.exchange(0),
                            g_studioOnly.load() ? "on" : "off");
                {
                    float t[7]{};
                    bool valid;
                    {
                        std::lock_guard lock(g_tonemapMutex);
                        valid = g_tonemapSeenValid;
                        std::memcpy(t, g_tonemapSeen, sizeof(t));
                    }
                    if (valid)
                        Log::Writef("ModelViewer: the game's tone mapping: exposure pre %.3f, post %.3f, adaptation %s, perc %.3f, shift "
                                    "limit %.3f, white point %.3f, speed %.3f; the Suite's (exposure %.2f, white point %.2f) set %u "
                                    "times in 3 s, the pass not drawn %u times (the Suite's tone mapping %s)",
                                    t[0], t[1], t[2] != 0.0f ? "on" : "off", t[3], t[4], t[5], t[6], kStudioExposure,
                                    kStudioWhitePoint, g_adaptationsStopped.exchange(0), g_toneNotDrawn.exchange(0),
                                    g_exposureFixed.load() ? "on" : "off");
                    else
                        Log::Write("ModelViewer: BloomRender::SetupTonemapParams not seen while open");
                    const std::uint32_t seen = g_gradingSeen.exchange(0);
                    Log::Writef("ModelViewer: the colour correction not drawn %u times in 3 s (hook %s; the game asked for: %s%s%s%s%s); "
                                "the bloom and the light scatter off with the Suite's tone mapping", g_gradingLeftOut.exchange(0),
                                g_colorCorrectionHooked ? "made" : "not made", seen & 1 ? "curves " : "", seen & 2 ? "transform " : "",
                                seen & 4 ? "grain " : "", seen & 8 ? "LUT " : "", seen ? "" : "none");
                }
                {
                    float r[4]{};
                    bool valid;
                    {
                        std::lock_guard lock(g_reflectionMutex);
                        valid = g_reflectionSeenValid;
                        std::memcpy(r, g_reflectionSeen, sizeof(r));
                    }
                    if (valid)
                        Log::Writef("ModelViewer: the game's reflections: intensity (%.3f, %.3f, %.3f), its global cube map's "
                                    "influence %.3f; the Suite's (intensity %.2f, no cube map) set %u times in 3 s", r[0], r[1],
                                    r[2], r[3], kStudioReflections, g_reflectionsSet.exchange(0));
                    else
                        Log::Writef("ModelViewer: the reflections' constants not seen in 3 s (hook %s, studio lights alone %s)",
                                    g_cubeReflHooked ? "made" : "not made", g_studioOnly.load() ? "on" : "off");
                }
                LogLightFit();   // (v0.9.429)
                Log::Writef("ModelViewer: the skin's subsurface scattering in 3 s: asked for %u times, drawn %u times (the "
                            "cutscenes' toggle %s; %s)", RenderTweaks::TakeSkinScatteringAsked(), g_sssDraws.exchange(0),
                            g_sssToggle ? (*g_sssToggle ? "on" : "off") : "?",
                            g_sssOn.load() ? "on" : g_sssHooked ? "off" : "no hook");   // (v0.9.430)
                if (Features::ViewerLightsKeptVisible()) {   // (v0.9.428)
                    std::uint32_t listed = 0, added = 0, noCell = 0;
                    Features::ViewerLightVisibility(listed, added, noCell);
                    Log::Writef("ModelViewer: the lights in the frame's list in 3 s: %u times found visible by the game, %u "
                                "put there by the viewer (the forward materials' light), %u times in no cell", listed, added,
                                noCell);
                } else {
                    Log::Write("ModelViewer: the lights are not kept visible (the log's Features line says why): the forward "
                               "materials have them only where the game's own test finds them");
                }
                if (RelicActive())   // (v0.9.427)
                    Log::Writef("ModelViewer: the relic viewer's lighting: its bloom and no tone mapping %u times in 3 s, its "
                                "aperture (1, 0, 0) %u times; its ambient %s; the level's reflections",
                                g_relicToneSet.exchange(0), g_relicApertureSet.exchange(0),
                                g_studioOnly.load() ? "(none)" : "not (the scene's own light on)");
                {   // the game's ambient occlusion (what the menu asked for), the wind, the strand hair's lights
                    float a[6];
                    std::uint8_t c[3];
                    {
                        std::lock_guard lock(g_aoMutex);
                        std::memcpy(a, g_aoSeen, sizeof(a));
                        std::memcpy(c, g_aoSeenColor, sizeof(c));
                    }
                    const std::uint32_t aoCalls = g_aoSeenCount.exchange(0), aoLeft = g_aoLeftOut.exchange(0);
                    if (aoCalls)
                        Log::Writef("ModelViewer: the game's ambient occlusion: %s (quality setting %d; strength %.2f, scene scale "
                                    "%.2f x range scale %.2f, power %.2f, colour %u %u %u); %s", a[0] != 0.0f ? "asked for" :
                                    "not asked for", int(a[5]), a[1], a[2], a[3], a[4], c[0], c[1], c[2],
                                    g_aoOff.load() ? (aoLeft ? "left out" : "off") : "on (O)");
                    else
                        Log::Writef("ModelViewer: the game's ambient occlusion: its parameters not seen in 3 s (hook %s)",
                                    g_ssaoHooked ? "made" : "not made");
                    Log::Writef("ModelViewer: the wind on the model: %s (%u answers zeroed in 3 s, the level's last %.2f)",
                                g_windOff.load() ? "none" : g_windHooked ? "the level's (Wind=On)" : "the level's (no hook)",
                                g_windsStopped.exchange(0), g_windSeen.load());
                    if (g_hairLightNotes.exchange(0))
                        Log::Writef("ModelViewer: the strand hair's lights: %u in its light set (%u of the studio's); its entity's "
                                    "flags 0x%X (lit when & 0x60 is 0x40), light mask 0x%X, its node's flags 0x%X (lights read "
                                    "with bit 1)", g_hairLightsAll.load(), g_hairLightsStudio.load(), g_hairEntityFlags.load(),
                                    g_hairEntityMask.load(), g_hairNodeFlags.load());
                }
                std::uint32_t id;
                std::uint64_t component;
                float at[3]{};
                if (ReadIds(g_instance, id, component, at))
                    Log::Writef("ModelViewer: %s after 3 s: at (%.0f, %.0f, %.0f), put back %d time(s), %u bind pose builds, "
                                "scene %p, the camera at (%.0f, %.0f, %.0f) %.0f cm away", ModelName().c_str(), at[0], at[1], at[2],
                                g_placeResets, g_poseCalls.load(), Features::ViewerSceneOf(g_instance), g_frame.position[0],
                                g_frame.position[1], g_frame.position[2], g_frame.distance);
                const Bounds& d = g_drawnBounds;
                char drawn[128] = "none seen (its size from its bones or its kind)";
                if (d.valid)
                    snprintf(drawn, sizeof(drawn), "(%.0f, %.0f, %.0f) to (%.0f, %.0f, %.0f) cm", d.min[0], d.min[1], d.min[2],
                             d.max[0], d.max[1], d.max[2]);
                {
                    char cells[4][160]{};
                    for (int i = 0; i < 3; ++i)
                        if (g_lights[i] >= 0) CellsText(Features::ViewerLightHandle(g_lights[i]), nullptr, cells[i], sizeof(cells[i]));
                    if (const void* entity = SceneEntityOf(g_bodyDrawable.load())) CellsText(nullptr, entity, cells[3], sizeof(cells[3]));
                    Log::Writef("ModelViewer: the scene cells: the key %s; the fill %s; the rim %s; the model's body %s",
                                cells[0][0] ? cells[0] : "-", cells[1][0] ? cells[1] : "-", cells[2][0] ? cells[2] : "-",
                                cells[3][0] ? cells[3] : "-");
                }
                const std::string left = LeftOutNames();
                Log::Writef("ModelViewer: %s after 3 s: components left out: %s; drawn bounds %s", ModelName().c_str(),
                            left.empty() ? "none" : left.c_str(), drawn);
            }
            break;
        }
        default:
            break;
    }
    UpdateIsolation();
    UpdateCamera();
    UpdateLights(now);
}

// The view's keys (the grid's are the game's; its categories its tabs and LB / RB, v0.9.431). Esc / Backspace: back to
// the grid; Left / Right: the model, PgUp / PgDn: the category; L lights, R reset, P place, B background, E scene light, A auto exposure, S the models' sound, O the
// game's ambient occlusion, H the view's UI (v0.9.436), I Lara's idle (v0.9.434), V the character's line (v0.9.443), + / - (Up / Down)
// brightness.
bool HandleKey(UINT msg, WPARAM wParam) {
    const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN, up = msg == WM_KEYUP || msg == WM_SYSKEYUP;
    if (!down && !up) return false;
    const bool ours = wParam == VK_ESCAPE || wParam == VK_BACK || wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_PRIOR ||
                      wParam == VK_NEXT || wParam == 'L' || wParam == 'R' || wParam == 'P' || wParam == 'B' || wParam == 'E' ||
                      wParam == 'A' || wParam == 'S' || wParam == 'O' || wParam == 'H' || wParam == 'I' || wParam == 'V' || wParam == VK_ADD ||
                      wParam == VK_SUBTRACT || wParam == VK_OEM_PLUS ||
                      wParam == VK_OEM_MINUS || wParam == VK_UP || wParam == VK_DOWN;
    if (up && wParam == 'V') g_lineKeyHeld = false;
    if (!OwnsInput()) {   // the key that left the view, let go (the game saw it go down)
        const double since = Now() - std::max(g_viewLeftAt, g_closedAt.load());
        return ours && up && since < 0.6;
    }
    if (!ours) return false;
    if (down) {
        switch (wParam) {
            case VK_ESCAPE:
            case VK_BACK: g_backRequest = true; break;
            case VK_LEFT: g_step.fetch_sub(1); break;
            case VK_RIGHT: g_step.fetch_add(1); break;
            case VK_PRIOR: g_categoryStep.fetch_sub(1); break;
            case VK_NEXT: g_categoryStep.fetch_add(1); break;
            case 'L': g_lightsToggle = true; break;
            case 'R': g_resetRequest = true; break;
            case 'P': g_placeRequest = true; break;
            case 'B': g_backgroundToggle = true; break;
            case 'E': g_sceneLightToggle = true; break;
            case 'A': g_autoExposureToggle = true; break;
            case 'S': g_soundToggle = true; break;
            case 'O': g_gameAoToggle = true; break;
            case 'H': g_uiHideToggle = true; break;
            case 'I': g_outfitIdleToggle = true; break;
            case 'V':
                if (!g_lineKeyHeld.exchange(true)) g_lineRequest = true;   // (not again while held: the key's repeats)
                break;
            case VK_ADD:
            case VK_OEM_PLUS:
            case VK_UP: g_brightnessStep.fetch_add(1); break;
            case VK_SUBTRACT:
            case VK_OEM_MINUS:
            case VK_DOWN: g_brightnessStep.fetch_sub(1); break;
            default: break;
        }
    }
    return true;
}

// The view's pad (the grid's is the game's): B back to the grid; right stick turn, left stick move, triggers zoom, R3 (X)
// reset (the relic viewer's legend: R3 reset, LT / RT zoom); LB / RB (d-pad left / right) the model; Y lights, d-pad up
// / down brightness, L3 place, Start scene light, A auto exposure, Back the models' sound.
bool FilterPad(unsigned long user, void* xinputState) {
    if (user >= 4 || !xinputState) return false;
    auto* state = static_cast<XINPUT_STATE*>(xinputState);
    const WORD buttons = state->Gamepad.wButtons;
    if (!OwnsInput()) {
        g_padHeld[user] = buttons;   // (the A that opened the view is not a press there)
        if (!g_padDrain[user].load()) return false;
        // after the view: the game sees nothing until every button is let go (no new press of the B that left it)
        if (buttons == 0 && state->Gamepad.bLeftTrigger < 30 && state->Gamepad.bRightTrigger < 30) {
            g_padDrain[user] = false;
            return false;
        }
        std::memset(&state->Gamepad, 0, sizeof(state->Gamepad));
        return true;
    }
    const WORD previous = g_padHeld[user].exchange(buttons);
    const WORD pressed = WORD(buttons & ~previous);
    const auto stick = [](SHORT v) {
        const float f = float(v) / 32767.0f;
        return std::fabs(f) < 0.2f ? 0.0f : std::clamp((std::fabs(f) - 0.2f) / 0.8f, 0.0f, 1.0f) * (f < 0 ? -1.0f : 1.0f);
    };
    if (pressed & XINPUT_GAMEPAD_B) g_backRequest = true;
    if (pressed & XINPUT_GAMEPAD_BACK) g_soundToggle = true;
    if (pressed & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_LEFT_SHOULDER)) g_step.fetch_sub(1);
    if (pressed & (XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_RIGHT_SHOULDER)) g_step.fetch_add(1);
    if (pressed & XINPUT_GAMEPAD_Y) g_lightsToggle = true;
    if (pressed & (XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_RIGHT_THUMB)) g_resetRequest = true;
    if (pressed & XINPUT_GAMEPAD_LEFT_THUMB) g_placeRequest = true;
    if (pressed & XINPUT_GAMEPAD_START) g_sceneLightToggle = true;
    if (pressed & XINPUT_GAMEPAD_A) g_autoExposureToggle = true;
    if (pressed & XINPUT_GAMEPAD_DPAD_UP) g_brightnessStep.fetch_add(1);
    if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) g_brightnessStep.fetch_sub(1);
    const auto trigger = [](BYTE v) { return v < 30 ? 0.0f : float(v - 30) / 225.0f; };
    if (user == 0 || state->Gamepad.sThumbRX || state->Gamepad.sThumbLX) {
        std::lock_guard lock(g_padMutex);
        g_padRX = stick(state->Gamepad.sThumbRX), g_padRY = stick(state->Gamepad.sThumbRY);
        g_padLX = stick(state->Gamepad.sThumbLX), g_padLY = stick(state->Gamepad.sThumbLY);
        g_padLT = trigger(state->Gamepad.bLeftTrigger), g_padRT = trigger(state->Gamepad.bRightTrigger);
    }
    std::memset(&state->Gamepad, 0, sizeof(state->Gamepad));
    return true;
}

// (present thread) The screen's size (the list's model on its side); on the view, the mouse: left drag turns, right drag
// moves, the wheel zooms.
void Draw() {
    if (!g_open.load()) return;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 size = io.DisplaySize;
    if (size.x <= 0 || size.y <= 0) return;
    g_aspect = size.x / size.y;
    if (!OwnsInput()) return;
    const float distance = g_distanceNow.load();
    std::lock_guard lock(g_viewMutex);
    // (a press on the button bar is the game's: no turn, no move)
    const auto onBar = [&](int button) {
        const ImVec2 at = io.MouseClickedPos[button];
        return InButtonBar(at.x, at.y, size.x, size.y);
    };
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !onBar(ImGuiMouseButton_Left)) {
        g_view.yaw += io.MouseDelta.x * 0.4f;
        g_view.pitch = std::clamp(g_view.pitch + io.MouseDelta.y * 0.3f, -80.0f, 80.0f);
    }
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right) && !onBar(ImGuiMouseButton_Right)) {
        const float perPixel = distance / std::max(200.0f, size.y) * 1.2f;
        g_view.panX += io.MouseDelta.x * perPixel;
        g_view.panZ -= io.MouseDelta.y * perPixel;
    }
    if (io.MouseWheel != 0.0f) g_view.zoom = std::clamp(g_view.zoom * std::pow(0.88f, io.MouseWheel), 0.15f, 6.0f);
}

void OnBuildTransforms(void* instance, std::uint32_t count, std::uint8_t* segs, const std::uint8_t* skeleton) {
    if (!instance || instance != g_poseInstance.load(std::memory_order_acquire)) return;
    if (!segs || !skeleton || count == 0 || count > kMaxSegments) return;
    g_poseCalls.fetch_add(1, std::memory_order_relaxed);
    WriteBindPose(count, segs, skeleton);
}

}  // namespace ModelViewer
