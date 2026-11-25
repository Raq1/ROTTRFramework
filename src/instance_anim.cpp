#include "instance_anim.h"
#include "features.h"
#include "game_build.h"
#include "model_viewer.h"
#include "log.h"

#include <Windows.h>
#include <MinHook.h>

#include <algorithm>
#include <cstdarg>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

// Retail facts (Steam ROTTR.exe 0x632D2D99; names from the TR2_x64_final PDB, whose code is the same):
//
// cdc::AnimFragmentNode::DecompressFrame (0x12B140) samples a clip for one graph node: from the node op data's elapsed
// time (+0x60) and the clip's key list (AnimFragment: u16 id +0x30, u16 key count +0x32, i16 time per key +0x34, u8
// blendshape tracks +0x36, u8 bone tracks +0x37, u8 attribute tracks +0x39, u16* bone ids +0x68, track data pointers
// +0x48..+0xA0) it computes key = int(time / timePerKey) (at most keys - 1) and frac = (time - key * timePerKey) /
// timePerKey, fills a DecompressFrameData and a DecompressFrameContext on its stack and calls
// cdc::DecompressFrame::DecompressFrameWithBoneSet (0x10EF70)(ctx, AnimSegment* pose, attribBuffer, blendShapeBuffer,
// Vector3* rootTranslation, Vector3* rootRotation) — a function of its arguments only:
//   DecompressFrameContext (0x98): +0x00 AnimFragmentPtrs (12 pointers: clip+0x48..+0xA0, InitializePtrs 0x116730),
//     +0x60 DecompressFrameData*, +0x68 clip, +0x70 u16 id map (clip track -> skeleton segment, 0xFFFF none),
//     +0x78 segment pivots (TransformComponent::GetSegmentList + 0x20), +0x80 u32 pivot stride (0x50),
//     +0x88 float bone weights (per segment), +0x90 u16 blendshape map.
//   DecompressFrameData: +0 animated segment count, +4 flags (2: looped, wrap key -1 to the last key; 4: initialize
//     untouched bones; 8: add to the pose instead of writing it; 0x10: wrap the sampled rotation to the pose's), +8
//     clip, +0x10 weight, +0x14 key, +0x18 frac, +0xC0 bone usage bits (one per segment).
//   AnimSegment (0x40): +0 rotation (quaternion log), +0x10 translation, +0x20 scale, +0x30 weight.
// The id map is what AnimFragmentNode::GenerateIDMaps (0x12BFE0) builds: ObjectZone::GetBoneIndexFromID (0x234CC0) on
// the instance's ObjectZone (Instance +0x80) for every bone id of the clip (ObjectZone +8 shared: +0x30 count, +0x38
// sorted {u16 id, u16, u16 segment} entries).
// cdc::AnimBuildTransforms (0x107400)(Instance*, u32 count, AnimSegment* pose, Segment* skeleton, Matrix* out) turns an
// instance's final pose into bone matrices (also hooked by combat_air.cpp for the aim IK).
//
// Anim libraries: the registry (pointer at 0x10FB790) is a hash map (buckets +0, bucket count +0x18) of nodes {next,
// u32 key +8, dtp::AnimLibrary* +0x10}; a library has +0x10 entry count, +0x18 entries (0x18 each: i16 animId, i16
// animNum, AnimFxList* +8, char* name +0x10) and +0x20 its path. A loaded clip is animByNumber[animNum] (0x15CFD50)
// with clip+0x30 == animNum. The libraries an instance's graphs use: Instance::GetAnimComponentV2 (0x3DDF50) ->
// AnimComponentV2 +0x3B0 AnimGraphManager -> +0x50 dtp::AnimatedComponent: +0x2C main graph count, +0x30 -> {graph,
// u32 list key} (key 0: the graph's own +0), +0x38 hosted graph count, +0x40 {graph, u32 key}[] (0x10 each), +0x98 /
// +0xA0 LOD graphs (main key) (AnimGraphRoot ctor, AnimComponentV2::Init 0x115CC0).
// TransformComponent::EnableForceRebuildTransforms (0x110980) keeps an instance's matrices rebuilt every frame.
//
// Blend shapes (v0.9.435; the TR2 PDB's names and its second AnimComponentV2 layout, which retail has):
// AnimComponentV2 +0x488 float* m_blendshapeWeights (one per slot), +0x490 last frame's, +0x498 int m_numBlendShapes.
// AnimComponentV2::Init (0x115CC0) sizes them from the object's blend shape map (ObjectZone +8 shared: +0x40 count,
// +0x48 sorted {u16 id, u16, u16 slot} entries; ObjectZone::GetBShapeIndexFromID 0x2349D0 looks an id up, 0xFFFF when
// not there) and puts a component that has any into g_animComponentsWithBlendShapes. SetBlendshapeWeight (0x11C650)
// is weights[slot] = w. AnimComponentV2::BuildTransforms copies the graph's pose weights into them (0x10BB15).
// AnimComponentV2::UpdateRenderModelBlendShapes (0x122A90): weights -> last frame's, the non-zero ones packed and
// sorted in place, handed to every drawable's render model instance (vfunc +0x38), the weights zeroed. Called once a
// frame for each component of that set by AnimatedComponentManager::UpdateRenderModelBlendShapes (0x122C70; from the
// main loop after the poses are built) unless the instance (component +8) has m_flags (+0xC8) bit 16
// (m_flagForceDeactivated), or has neither bit 15 (m_flagForceActivated) nor a processing state (+0xC) other than 0
// (Deactivated). A clip's blend shape tracks: count u8 +0x36, global ids u16* +0x88 (GenerateIDMaps 0x12C021 maps them
// with GetBShapeIndexFromID). DecompressFrameWithBoneSet decodes them (0x10FB70) with the blend shape map (ctx +0x90:
// per track, 0xFFFF skips it without stepping over its data, so a map that skips a track spoils the ones after it) into
// the blendShapeBuffer: +0 int count, +4 float weights[], then at +4 + 4 * tracks one u8 per weight (its map value).
// A pose write (DecompressFrameData flags without 8) stores weight * sample and sets the segment's weight (+0x30) to the
// weight; with 8 it adds both (0x10F689); with 0x10 the sampled rotation is first wrapped to the segment's (its
// rotation over its weight; cdc::WrapQuatLog2, 0x10F5C4).
namespace {
constexpr std::uintptr_t kRvaBuildTransforms = 0x00107400ull;
constexpr std::uintptr_t kRvaDecompress = 0x0010EF70ull;
constexpr std::uintptr_t kRvaGetBoneIndexFromId = 0x00234CC0ull;
constexpr std::uintptr_t kRvaForceRebuildTransforms = 0x00110980ull;
constexpr std::uintptr_t kRvaAnimComponentInit = 0x00115CC0ull;
constexpr std::uintptr_t kRvaAnimComponentInitLod = 0x00115D58ull;
constexpr std::uintptr_t kRvaGetAnimData = 0x001146D0ull;
constexpr std::uintptr_t kRvaAnimByNumberRef = 0x00114769ull;
constexpr std::uintptr_t kRvaGetAnimComponent = 0x003DDF50ull;
constexpr std::uintptr_t kRvaDecompressFrameCtx = 0x0012B509ull;
constexpr std::uintptr_t kRvaDecompressFrameTime = 0x0012B196ull;
constexpr std::uintptr_t kRvaDecompressFrameKeys = 0x0012B1D4ull;
constexpr std::uintptr_t kRvaRegistry = 0x010FB790ull;
constexpr std::uintptr_t kRvaAnimByNumber = 0x015CFD50ull;
constexpr std::uintptr_t kRvaUpdateBlendShapes = 0x00122A90ull;
constexpr std::uintptr_t kRvaBlendShapeManagerFlags = 0x00122CE1ull;
constexpr std::uintptr_t kRvaBlendShapeManagerState = 0x00122CF9ull;
constexpr std::uintptr_t kRvaSetBlendshapeWeight = 0x0011C650ull;
constexpr std::uintptr_t kRvaInitBlendShapeCount = 0x00115E05ull;
constexpr std::uintptr_t kRvaGetBShapeIndexFromId = 0x002349D0ull;
constexpr std::uintptr_t kRvaGenerateIdMapsShapes = 0x0012C021ull;
constexpr std::uintptr_t kRvaDecompressShapeLoop = 0x0010FB36ull;
constexpr std::uintptr_t kRvaDecompressShapeSkip = 0x0010FB70ull;
constexpr std::uintptr_t kRvaDecompressShapeStore = 0x0010FD5Dull;
constexpr std::uintptr_t kRvaDecompressShapeCount = 0x0010FD79ull;
constexpr std::uintptr_t kRvaDecompressWrapFlag = 0x0010F5C4ull;
constexpr std::uintptr_t kRvaDecompressAddFlag = 0x0010F689ull;
constexpr std::uintptr_t kRvaDecompressAdd = 0x0010F6A7ull;
constexpr std::uintptr_t kRvaDecompressWrite = 0x0010F6E8ull;
constexpr std::uintptr_t kRvaPostRebuildTransforms = 0x003DD440ull;
constexpr std::uintptr_t kRvaRebuildTransforms = 0x003DD940ull;
constexpr std::uintptr_t kRvaRebuildTransformsPool = 0x003DD962ull;
constexpr std::uintptr_t kRvaRebuildTransformsAnim = 0x003DDB4Dull;
constexpr std::uintptr_t kRvaRebuildTransformsPost = 0x003DDC1Full;
constexpr std::uintptr_t kRvaZoneAnimatedSegments = 0x0023EC60ull;
constexpr std::uintptr_t kRvaZoneSegments = 0x0023EC80ull;   // (v0.9.444)
constexpr std::uintptr_t kRvaZoneSegmentList = 0x002373D0ull;
constexpr std::uintptr_t kRvaAnimBuildCount = 0x0010BC5Aull;
constexpr std::uintptr_t kRvaAnimBuildSkeleton = 0x0010BCB0ull;
constexpr std::uintptr_t kRvaAnimBuildCall = 0x0010BD0Aull;
constexpr std::uintptr_t kRvaInitShapeCountZone = 0x00115DFEull;
constexpr std::uintptr_t kRvaUpdateBlendShapesLast = 0x00122AA7ull;
constexpr std::uintptr_t kRvaUpdateBlendShapesInstance = 0x00122B5Aull;
constexpr std::uintptr_t kRvaGetLocalPlayerInstance = 0x00403BF0ull;   // (v0.9.448)
constexpr std::uintptr_t kRvaTryToGetClothComponent = 0x0024FAB0ull;   // (v0.9.448)
constexpr std::uintptr_t kRvaClothSetMatrices = 0x00175F50ull;         // (v0.9.448: what it reads, checked)

constexpr std::uint8_t kBuildTransformsSig[] = {
    0x48,0x89,0x5C,0x24,0x18,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x48,0x83,0xEC,0x40,0x48,0x8B,0xBC,0x24,0x90,0x00,0x00,0x00};
constexpr std::uint8_t kDecompressSig[] = {
    0x48,0x8B,0xC4,0x4C,0x89,0x48,0x20,0x4C,0x89,0x40,0x18,0x48,0x89,0x50,0x10,0x48,0x89,0x48,0x08,0x55,0x53,0x56,
    0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xA8,0x98,0xFE,0xFF,0xFF,0x48,0x81,0xEC,0x28,0x02,0x00};
constexpr std::uint8_t kGetBoneIndexFromIdSig[] = {
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x8B,0x71,0x08,0x45,0x33,0xC9,0x44,0x0F,0xB7,0xDA,0x44,
    0x8B,0x56,0x30,0x48,0x8B,0x5E,0x38,0x45,0x85,0xD2};
constexpr std::uint8_t kForceRebuildTransformsSig[] = {
    0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0x59,0x08,0x66,0x83,0x89,0x9E,0x00,0x00,0x00,0x10};
constexpr std::uint8_t kAnimComponentInitSig[] = {
    0x40,0x53,0x56,0x48,0x83,0xEC,0x28,0x48,0x89,0x6C,0x24,0x40,0x48,0x8B,0xD9,0x48,0x89,0x51,0x08,0x48,0x8B,0xF2,
    0x48,0x8B,0x89,0xB0,0x03,0x00,0x00,0x48,0x89,0x7C,0x24,0x48,0x49,0x8B,0xF8,0x4C,0x89,0x83,0xB0,0x03,0x00,0x00};
constexpr std::uint8_t kAnimComponentInitLodSig[] = {0x48,0x8B,0x4F,0x50,0x4C,0x39,0xB9,0xA0,0x00,0x00,0x00,0x74};
constexpr std::uint8_t kGetAnimDataSig[] = {
    0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x8B,0xD9,0x48,0x8B,0x0D,0xAD,0x70,0xFE,0x00};
constexpr std::uint8_t kAnimByNumberRefSig[] = {0x48,0x8D,0x0D,0xE0,0xB5,0x4B,0x01};
constexpr std::uint8_t kGetAnimComponentSig[] = {
    0x48,0x8B,0xC1,0x48,0x85,0xC9,0x74,0x1A,0x8B,0x0D,0xCA,0x3D,0xD2,0x00,0x39,0x88,0xD0,0x00,0x00,0x00,0x76,0x0C,
    0x48,0x8B,0x80,0xD8,0x00,0x00,0x00,0x48,0x8B,0x04,0xC8,0xC3};
// DecompressFrame's context setup (data +0x270 -> ctx+0x60, id map op+0x30 -> ctx+0x70, pivots -> ctx+0x78, stride
// 0x50 -> ctx+0x80, weights -> ctx+0x88, blendshape map op+0x58 -> ctx+0x90, InitializePtrs, ...) and the call.
constexpr std::uint8_t kDecompressFrameCtxSig[] = {
    0x48,0x8B,0x55,0x30,0x48,0x8D,0x8D,0x70,0x02,0x00,0x00,0x4C,0x8B,0x85,0x78,0x02,0x00,0x00,0x48,0x83,0xC0,0x20,
    0x48,0x89,0x8D,0xA0,0x01,0x00,0x00,0x49,0x8B,0x4E,0x30,0x48,0x89,0x85,0xB8,0x01,0x00,0x00,0x48,0x8D,0x85,0x70,
    0x03,0x00,0x00,0x48,0x89,0x8D,0xB0,0x01,0x00,0x00,0x48,0x8D,0x8D,0x40,0x01,0x00,0x00,0x48,0x89,0x95,0xA8,0x01,
    0x00,0x00,0xC7,0x85,0xC0,0x01,0x00,0x00,0x50,0x00,0x00,0x00,0x48,0x89,0x85,0xC8,0x01,0x00,0x00,0x48,0x89,0x9D,
    0xD0,0x01,0x00,0x00,0xE8,0xC6,0xB1,0xFE,0xFF,0x48,0x8B,0x45,0x40,0x48,0x8D,0x8D,0x40,0x01,0x00,0x00,0x48,0x8B,
    0x9D,0x20,0x0C,0x00,0x00,0x48,0x8B,0xD7,0x4C,0x8B,0x85,0x10,0x0C,0x00,0x00,0x4C,0x8B,0xCB,0x48,0x89,0x85,0x70,
    0x01,0x00,0x00,0x48,0x8B,0x45,0x00,0x48,0x89,0x85,0x78,0x01,0x00,0x00,0x48,0x8D,0x45,0x50,0x48,0x89,0x44,0x24,
    0x28,0x48,0x8D,0x45,0x20,0x48,0x89,0x44,0x24,0x20,0xE8,0xBE,0x39,0xFE,0xFF};
constexpr std::uint8_t kDecompressFrameTimeSig[] = {0xF3,0x41,0x0F,0x10,0x72,0x60,0x44,0x0F,0xBF,0x40,0x34};
constexpr std::uint8_t kDecompressFrameKeysSig[] = {0x48,0x8B,0x41,0x48,0xF3,0x0F,0x5E,0xD1,0x0F,0xB7,0x48,0x32};
// UpdateRenderModelBlendShapes: push rbp; sub rsp,0x40; movsxd r8,[rcx+0x498]; mov rbp,rcx; mov rdx,[rcx+0x488]
constexpr std::uint8_t kUpdateBlendShapesSig[] = {
    0x40,0x55,0x48,0x83,0xEC,0x40,0x4C,0x63,0x81,0x98,0x04,0x00,0x00,0x48,0x8B,0xE9,0x48,0x8B,0x91,0x88,0x04,0x00,0x00};
// the manager's test: mov r9d,[rdx+0xC8]; bit 16 -> skip; bit 15 -> push; ... cmp dword [rdx+0xC],0; je skip
constexpr std::uint8_t kBlendShapeManagerFlagsSig[] = {
    0x44,0x8B,0x8A,0xC8,0x00,0x00,0x00,0x41,0x8B,0xC1,0xC1,0xE8,0x10,0xA8,0x01,0x75,0x26,0x41,0xC1,0xE9,0x0F,0x41,0xF6,0xC1};
constexpr std::uint8_t kBlendShapeManagerStateSig[] = {0x01,0x75,0x06,0x83,0x7A,0x0C,0x00,0x74};
// SetBlendshapeWeight: mov rax,[rcx+0x488]; movsxd rdx,edx; movss [rax+rdx*4],xmm2; ret
constexpr std::uint8_t kSetBlendshapeWeightSig[] = {
    0x48,0x8B,0x81,0x88,0x04,0x00,0x00,0x48,0x63,0xD2,0xF3,0x0F,0x11,0x14,0x90,0xC3};
// Init: mov rcx,[rax+8] (the shared data); movzx eax,word [rcx+0x40]; mov [rbx+0x498],eax
constexpr std::uint8_t kInitBlendShapeCountSig[] = {
    0x48,0x8B,0x48,0x08,0x0F,0xB7,0x41,0x40,0x89,0x83,0x98,0x04,0x00,0x00,0x85,0xC0};
constexpr std::uint8_t kGetBShapeIndexFromIdSig[] = {
    0x48,0x89,0x7C,0x24,0x08,0x48,0x8B,0x79,0x08,0x45,0x33,0xC0,0x44,0x0F,0xB7,0xD2,0x44,0x8B,0x4F,0x40,0x4C,0x8B,0x5F,0x48};
// GenerateIDMaps: movzx ecx,byte [rax+0x36] (the clip's blend shape tracks) ... mov r12,[rax+0x88] (their ids)
constexpr std::uint8_t kGenerateIdMapsShapesSig[] = {
    0x0F,0xB6,0x48,0x36,0x4C,0x89,0x74,0x24,0x20,0x85,0xC9,0x74,0x3C,0x4C,0x89,0x64,0x24,0x28,0x44,0x8B,0xF1,0x4C,0x8B,0xA0,
    0x88,0x00,0x00,0x00};
// the decompressor's blend shapes: rbx = ctx+0x48, rdi = ctx+0x50; r15 = buffer + 4; r13 = r15 + tracks * 4
constexpr std::uint8_t kDecompressShapeLoopSig[] = {
    0x48,0x8B,0x5F,0x48,0x48,0x8B,0x7F,0x50,0x4C,0x8D,0x79,0x04,0x4D,0x8D,0x2C,0x87};
// movzx r12d,word [map+r14*2]; cmp r12w,0xFFFF; je (the next track, its data not stepped over)
constexpr std::uint8_t kDecompressShapeSkipSig[] = {0x46,0x0F,0xB7,0x24,0x71,0x66,0x45,0x3B,0xE0,0x0F,0x84,0xEE,0x01,0x00,0x00};
// movss [r15],xmm0; add r15,4; mov [r13],r12b; inc r13
constexpr std::uint8_t kDecompressShapeStoreSig[] = {0xF3,0x41,0x0F,0x11,0x07,0x49,0x83,0xC7,0x04,0x45,0x88,0x65,0x00,0x49,0xFF,0xC5};
constexpr std::uint8_t kDecompressShapeCountSig[] = {0x48,0x8B,0x8D,0x88,0x01,0x00,0x00,0x89,0x01};
// test byte [rsp+0x2D],0x10 (the data's flags); mov rdx,[rbp+0x178] (the pose); je ...
constexpr std::uint8_t kDecompressWrapFlagSig[] = {0xF6,0x44,0x24,0x2D,0x10,0x48,0x8B,0x95,0x78,0x01,0x00,0x00,0x74,0x4C};
constexpr std::uint8_t kDecompressAddFlagSig[] = {0xF6,0x46,0x04,0x08,0x41,0x0F,0x59,0xD1};
// je write; addps xmm2,[pose+seg]; movaps [pose+seg],xmm2 (add) / movss [pose+seg+0x30],xmm9 (write: the weight)
constexpr std::uint8_t kDecompressAddSig[] = {0x74,0x3F,0x42,0x0F,0x58,0x14,0x2A,0x42,0x0F,0x29,0x14,0x2A};
constexpr std::uint8_t kDecompressWriteSig[] = {0xF3,0x46,0x0F,0x11,0x4C,0x2A,0x30,0x42};
// v0.9.436, an instance without an AnimComponentV2 (see the facts):
// G2Instance_PostRebuildTransforms: mov rax,rsp; push rbp; push rsi; lea rbp,[rax-0x5F]; sub rsp,0xC8; ...
constexpr std::uint8_t kPostRebuildTransformsSig[] = {
    0x48,0x8B,0xC4,0x55,0x56,0x48,0x8D,0x68,0xA1,0x48,0x81,0xEC,0xC8,0x00,0x00,0x00,0x48,0x89,0x58,0x20,0x48,0x8B,0xF1,0x48};
// G2Instance_RebuildTransforms: ... mov rbx,[rcx+0xE0] (the TransformComponent); mov rdi,rcx
constexpr std::uint8_t kRebuildTransformsSig[] = {
    0x40,0x53,0x55,0x57,0x48,0x83,0xEC,0x60,0x48,0x8B,0x99,0xE0,0x00,0x00,0x00,0x48,0x8B,0xF9,0x48,0x8B,0xCB,0xE8};
constexpr std::uint8_t kRebuildTransformsPoolSig[] = {0x48,0x8B,0x6B,0x70};   // mov rbp,[rbx+0x70] (the matrices)
// the anim path: mov rcx,r14; mov [r14+0x400],rbp (the component's matrices); call AnimComponentV2::BuildTransforms
constexpr std::uint8_t kRebuildTransformsAnimSig[] = {0x49,0x8B,0xCE,0x49,0x89,0xAE,0x00,0x04,0x00,0x00,0xE8};
// both paths end: mov eax,0xFFEE; mov rcx,rdi; and [rbx+0x9E],ax; call G2Instance_PostRebuildTransforms (0x3DD440)
constexpr std::uint8_t kRebuildTransformsPostSig[] = {
    0xB8,0xEE,0xFF,0x00,0x00,0x48,0x8B,0xCF,0x66,0x21,0x83,0x9E,0x00,0x00,0x00,0xE8,0x0D,0xF8,0xFF,0xFF};
// ObjectZone::GetNumAnimatedSegments: (zone +8) +0x58 +0x78 (the skeleton): its u16 +8 (1 without one)
constexpr std::uint8_t kZoneAnimatedSegmentsSig[] = {
    0x48,0x8B,0x41,0x08,0x48,0x8B,0x48,0x58,0x48,0x8B,0x41,0x78,0x48,0x85,0xC0,0x75,0x06,0xB8,0x01,0x00,0x00,0x00,0xC3};
// (v0.9.444) ObjectZone::NumSegments (TR2 0x14025C9E0, after NumAnimatedSegments as here): the skeleton's [+0] u32,
// every segment (the cloth's too). (v0.9.445) Only its tail checked, from +0xF: bone_patch.cpp detours its first 15
// bytes (its detour hands back the same count), which 0.9.444's check took for a mismatch.
constexpr std::size_t kZoneSegmentsTailAt = 0xF;
constexpr std::uint8_t kZoneSegmentsTailSig[] = {0x75,0x06,0xB8,0x01,0x00,0x00,0x00,0xC3,0x48,0x8B,0x00,0x8B,0x00,0xC3};
// ObjectZone::GetSegmentList: the same skeleton's [+0] +8 (a default without one)
constexpr std::uint8_t kZoneSegmentListSig[] = {
    0x48,0x8B,0x41,0x08,0x48,0x8B,0x48,0x58,0x48,0x8B,0x41,0x78,0x48,0x85,0xC0,0x75,0x08};
// AnimComponentV2's pose: count = GetNumAnimatedSegments(the instance's zone) ...
constexpr std::uint8_t kAnimBuildCountSig[] = {
    0x48,0x8B,0x41,0x08,0x48,0x8B,0xD9,0x48,0x8B,0xB0,0xE0,0x00,0x00,0x00,0x48,0x8B,0x46,0x08,0x48,0x8B,0x88,0x80,0x00,0x00};
// ... skeleton = GetSegmentList(the zone) ...
constexpr std::uint8_t kAnimBuildSkeletonSig[] = {
    0x48,0x8B,0x83,0xC8,0x03,0x00,0x00,0x48,0x8B,0x78,0x50,0x48,0x8B,0x46,0x08,0x48,0x8B,0x88,0x80,0x00,0x00,0x00,0xE8};
// ... AnimBuildTransforms(instance, count, pose, skeleton, the component's +0x400)
constexpr std::uint8_t kAnimBuildCallSig[] = {
    0x48,0x8B,0x4B,0x08,0x4C,0x8B,0xCE,0x4C,0x8B,0xC7,0x48,0x89,0x6C,0x24,0x20,0x41,0x8B,0xD6,0xE8,0xDF,0xB6,0xFF,0xFF};
// AnimComponentV2::Init: mov rax,[rsi+0x80] (the instance's zone); mov rcx,[rax+8]; movzx eax,word [rcx+0x40]; ...
constexpr std::uint8_t kInitShapeCountZoneSig[] = {
    0x48,0x8B,0x86,0x80,0x00,0x00,0x00,0x48,0x8B,0x48,0x08,0x0F,0xB7,0x41,0x40,0x89,0x83,0x98,0x04,0x00,0x00,0x85,0xC0};
// UpdateRenderModelBlendShapes reads +0x490 (mov rcx,[rcx+0x490]) and +8 (mov rcx,[rbp+8]; call 0x3DE610), nothing else
constexpr std::uint8_t kUpdateBlendShapesLastSig[] = {0x48,0x8B,0x89,0x90,0x04,0x00,0x00};
constexpr std::uint8_t kUpdateBlendShapesInstanceSig[] = {0x48,0x8B,0x4D,0x08,0xE8,0xAD,0xBA,0x2B,0x00};
// (v0.9.448) the player's bones that nothing moves (see PoseLooseSegments):
// GetLocalPlayerInstance(int): push rbx; sub rsp,0x20; movzx ebx,cl
constexpr std::uint8_t kGetLocalPlayerInstanceSig[] = {0x40,0x53,0x48,0x83,0xEC,0x20,0x0F,0xB6,0xD9};
// cdc::TryToGetClothComponent(Instance*) (TR2 0x14026DBB0; retail AGInstanceSwitchClothConfig::ProcessMsg calls it for a
// non-player instance): the instance's handle (+0x68) looked up in the component table
constexpr std::uint8_t kTryToGetClothSig[] = {0x48,0x8B,0x51,0x68,0x48,0xB8,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0x48,0x85,
                                              0xD0,0x74,0x44,0x48,0x85,0xD2,0x74,0x3F,0x81,0xFA,0x30,0x75,0x00,0x00,0x73,0x37};
// cdc::ClothComponent::SetMatrices (TR2 0x1401899A0), the only writer of the cloth's bones, where it reads what we read:
// test byte [rcx+0x100],8 (the component running) ...
constexpr std::size_t kClothSetMatricesRunAt = 0x6;
constexpr std::uint8_t kClothSetMatricesRunSig[] = {0xF6,0x81,0x00,0x01,0x00,0x00,0x08};
// ... mov rax,[rdi+0x40]; mov rcx,[rdi+0x20]; mov r10,[rax]; mov eax,[rdi+0x18] (its data's pieces, the states, their count)
constexpr std::size_t kClothSetMatricesPiecesAt = 0xA2;
constexpr std::uint8_t kClothSetMatricesPiecesSig[] = {0x48,0x8B,0x47,0x40,0x48,0x8B,0x4F,0x20,0x4C,0x8B,0x10,0x8B,0x47,0x18};
// ... test byte [rbx+0xD8],8 (a piece simulated); je; cmp [rbx+0x3C],r12d (its knots)
constexpr std::size_t kClothSetMatricesStateAt = 0x11F;
constexpr std::uint8_t kClothSetMatricesStateSig[] = {0xF6,0x83,0xD8,0x00,0x00,0x00,0x08,0x0F,0x84,0xED,0x01,0x00,0x00,
                                                      0x44,0x39,0x63,0x3C};
// ... mov rax,[r10+rsi] (the piece's knots) ... movzx ecx,word [rax+r11+0x2A] (a knot's bone; knots 0x30 apart)
constexpr std::size_t kClothSetMatricesKnotAt = 0x178;
constexpr std::uint8_t kClothSetMatricesKnotSig[] = {0x49,0x8B,0x04,0x32,0x41,0xFF,0xC1,0x41,0x0F,0x28,0x04,0x10,0x4D,
                                                     0x8D,0x80,0x80,0x00,0x00,0x00,0x42,0x0F,0xB7,0x4C,0x18,0x2A};
// ... inc ebp; add rsi,0x20 (pieces 0x20 apart); add r14,8 (states 8); cmp ebp,[rdi+0x18]
constexpr std::size_t kClothSetMatricesNextAt = 0x319;
constexpr std::uint8_t kClothSetMatricesNextSig[] = {0xFF,0xC5,0x48,0x83,0xC6,0x20,0x49,0x83,0xC6,0x08,0x3B,0x6F,0x18};
// (v0.9.512) ... mov rdx,[rbx+0x20] (the piece's knot states: 0x80 apart, knot k's matrix at +0, what it copies to
// knot k's bone; the movaps from [r8+rdx] and lea r8,[r8+0x80] are in kClothSetMatricesKnotSig)
constexpr std::size_t kClothSetMatricesStatesAt = 0x150;
constexpr std::uint8_t kClothSetMatricesStatesSig[] = {0x48,0x8B,0x53,0x20};
// (v0.9.512) the cloth of an instance without an AnimComponentV2 (see DriveCloth):
// cdc::ClothComponent::Simulate (TR2 0x140189EA0): push rsi; sub rsp,0xD0; movzx eax,byte [rcx+0x103] (FullInit done)
constexpr std::uintptr_t kRvaClothSimulate = 0x00176450ull;
constexpr std::uint8_t kClothSimulateSig[] = {0x40,0x56,0x48,0x81,0xEC,0xD0,0x00,0x00,0x00,0x0F,0xB6,0x81,0x03,0x01,0x00,0x00};
// ... cmp dword [rsi+0x30],-1; je (no config: nothing)
constexpr std::size_t kClothSimulateConfigAt = 0x50;
constexpr std::uint8_t kClothSimulateConfigSig[] = {0x83,0x7E,0x30,0xFF,0x0F,0x84};
// ... test byte [the instance's TransformComponent +0x9E],3; jne (its matrices, or last frame's, not valid: nothing)
constexpr std::size_t kClothSimulateMatricesAt = 0xB3;
constexpr std::uint8_t kClothSimulateMatricesSig[] = {0xF6,0x80,0x9E,0x00,0x00,0x00,0x03,0x0F,0x85};
// ... running: the instance's +0xC8 bit 16 (ForceDeactivated) -> the rest; bit 15 (ForceActivated) -> on; +0xC (its
// processing state) not 0 -> on; the component's flags & 0x50 -> on; the instance's +0xC4 bit 11 (a cinematic actor)
// -> on; else nothing
constexpr std::size_t kClothSimulateActiveAt = 0x122;
constexpr std::uint8_t kClothSimulateActiveSig[] = {
    0x44,0x8B,0x8A,0xC8,0x00,0x00,0x00,0x41,0x8B,0xC1,0xC1,0xE8,0x10,0xA8,0x01,0x75,0x10,0x41,0xC1,0xE9,0x0F,0x41,0xF6,
    0xC1,0x01,0x75,0x1C,0x83,0x7A,0x0C,0x00,0x75,0x16,0x45,0x84,0xC0,0x75,0x11,0x8B,0x82,0xC4,0x00,0x00,0x00,0xC1,0xE8,
    0x0B,0xA8,0x01,0x0F,0x84};
// ... the whole simulation when flags & 6 == 6 (bit 2: picked this frame by ClothComponentManager::Process, by the
// cameras' distance and the drawable drawn last frame), flags & 0x50, or a cinematic actor; else only TransformBones
constexpr std::size_t kClothSimulateFullAt = 0x171;
constexpr std::uint8_t kClothSimulateFullSig[] = {0x41,0x80,0xFA,0x06,0x0F,0x84,0xB5,0x00,0x00,0x00,0x45,0x84,0xC0,0x0F,0x85,
                                                  0xAC,0x00,0x00,0x00,0x8B,0x82,0xC4,0x00,0x00,0x00,0xC1,0xE8,0x0B,0xA8,0x01,
                                                  0x0F,0x85};
// Instance::EnableCinematicActor(bool) (TR2 0x140403040): the bit unless set (then AnimComponentV2::OnCinematicStarted
// when it has one, and Instance::SetParentCinematicFlag on its hierarchy's children); push rbx; sub rsp,0x20; mov rbx,rcx;
// mov ecx,[rcx+0xC4]; mov eax,ecx; shr eax,0xB; test al,1; jne ... bts ecx,0xB; mov [rsp+0x30],rsi; mov [rbx+0xC4],ecx
constexpr std::uintptr_t kRvaEnableCinematicActor = 0x003D8C90ull;
constexpr std::uint8_t kEnableCinematicActorSig[] = {0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x8B,0x89,0xC4,0x00,0x00,
                                                     0x00,0x8B,0xC1,0xC1,0xE8,0x0B,0xA8,0x01,0x0F,0x85};
constexpr std::size_t kEnableCinematicActorSetAt = 0x1C;
constexpr std::uint8_t kEnableCinematicActorSetSig[] = {0x0F,0xBA,0xE9,0x0B,0x48,0x89,0x74,0x24,0x30,0x89,0x8B,0xC4,0x00,0x00,0x00};
// Instance::DisableCinematicActor(bool) (TR2 0x140400190): the same undone (OnCinematicEnded; and dword [rbx+0xC4],
// 0xFFFFF7FF; the children's flag cleared)
constexpr std::uintptr_t kRvaDisableCinematicActor = 0x003D64D0ull;
constexpr std::uint8_t kDisableCinematicActorSig[] = {0x40,0x53,0x48,0x83,0xEC,0x20,0x8B,0x81,0xC4,0x00,0x00,0x00,0x48,0x8B,
                                                      0xD9,0xC1,0xE8,0x0B,0xA8,0x01,0x0F,0x84};
constexpr std::size_t kDisableCinematicActorClearAt = 0x45;
constexpr std::uint8_t kDisableCinematicActorClearSig[] = {0x81,0xA3,0xC4,0x00,0x00,0x00,0xFF,0xF7,0xFF,0xFF};

constexpr std::size_t kInstanceObjectZone = 0x80;
constexpr std::size_t kInstanceTransformComponent = 0xE0;
constexpr std::size_t kAnimComponentGraphManager = 0x3B0;
constexpr std::size_t kGraphManagerDtp = 0x50;
constexpr std::size_t kDtpMainCount = 0x2C, kDtpMain = 0x30, kDtpHostedCount = 0x38, kDtpHosted = 0x40;
constexpr std::size_t kDtpLod1 = 0x98, kDtpLod2 = 0xA0;
constexpr std::size_t kLibraryCount = 0x10, kLibraryEntries = 0x18, kLibraryName = 0x20;
constexpr std::size_t kClipId = 0x30, kClipKeys = 0x32, kClipTimePerKey = 0x34, kClipBlendShapes = 0x36,
                      kClipBones = 0x37, kClipAttribs = 0x39, kClipBoneIds = 0x68, kClipPtrsFirst = 0x48,
                      kClipShapeIds = 0x88, kClipChannelFlags = 0x70;
constexpr std::size_t kSkeletonStride = 0x50, kSkeletonPivot = 0x20;   // cdc Segment (the skeleton): its bind translation
constexpr std::size_t kSkeletonParent = 0x38;   // (v0.9.448) its parent (int; before it in the list)
// (v0.9.448) cdc::ClothComponent (SetMatrices' reads): +0x100 flags (8: running), +0x18 its pieces' count, +0x20 their
// ClothState*, +0x40 its data (-> [0] the pieces, 0x20 each: +0 their knots, 0x30 each, +0x2A the bone); ClothState
// +0xD8 flags (8: simulated), +0x3C its knots' count
constexpr std::size_t kClothFlags = 0x100, kClothPieceCount = 0x18, kClothStates = 0x20, kClothData = 0x40;
constexpr std::size_t kClothPieceStride = 0x20, kClothKnotStride = 0x30, kClothKnotBone = 0x2A;
constexpr std::size_t kClothStateFlags = 0xD8, kClothStateKnots = 0x3C;
constexpr std::uint8_t kClothRunning = 0x8, kClothStateSimulated = 0x8;
// (v0.9.512) the rest Simulate reads (above): +0x103 initialized (FullInit), +0x30 its config (-1 none); ClothState +0x20
// its knot states (0x80 apart, the matrix at +0, its position +0x30); TransformComponent +0x9E (bits 1, 2: matrices not
// valid); Instance +0xC4 bit 11 (a cinematic actor), +0x30 its rotation (Euler: x, y, z the heading)
constexpr std::size_t kClothInitialized = 0x103, kClothConfig = 0x30;
constexpr std::size_t kClothStateKnotStates = 0x20, kClothKnotStateStride = 0x80;
constexpr std::size_t kTransformFlags = 0x9E;
constexpr std::size_t kInstanceCineFlags = 0xC4, kInstanceRotation = 0x30;
constexpr std::uint32_t kFlagCinematicActor = 1u << 11;
constexpr std::size_t kAnimWeights = 0x488, kAnimLastWeights = 0x490, kAnimWeightCount = 0x498;   // AnimComponentV2
constexpr std::size_t kAnimInstance = 0x08, kFakeComponentSize = 0x4A0;
constexpr std::size_t kTransformMatrices = 0x70;   // TransformComponent: its matrices (one before them: the root's)
constexpr std::size_t kZoneShared = 0x08, kSharedShapeCount = 0x40;   // ObjectZone -> shared: the blend shape map's count
constexpr std::size_t kInstanceProcessingState = 0x0C, kInstanceFlags = 0xC8;
constexpr std::uint32_t kFlagForceActivated = 1u << 15, kFlagForceDeactivated = 1u << 16;
constexpr std::size_t kSegmentFloats = 12, kSegmentWeight = 12;   // AnimSegment: 3 x 4 floats, then the weight
constexpr int kMaxShapes = 256;       // (slots: the object's map count is a u8 in its skeleton data)
constexpr int kMaxFidgets = 6, kMaxFidgetParts = 10;
constexpr std::uint32_t kClipNumberLimit = 0x7D00u;
constexpr double kTimeUnitsPerSecond = 3000.0;  // time per key 100 = one 30 fps frame
constexpr std::uint32_t kMaxSegments = 8192;
constexpr int kMaxPlayers = 16;
constexpr int kMaxListEntries = 30000;
constexpr int kMaxKeys = 64;

#pragma pack(push, 1)
struct LibraryEntry {
    std::int16_t animId;
    std::int16_t animNum;
    std::uint32_t pad;
    const void* fx;
    const char* name;
};
#pragma pack(pop)
static_assert(sizeof(LibraryEntry) == 0x18, "anim library entry");

struct DecompressContext {
    const std::uint8_t* ptrs[12];
    void* data;
    const std::uint8_t* clip;
    const std::uint16_t* idMap;
    const std::uint8_t* pivots;
    std::uint32_t stride;
    std::uint32_t pad;
    const float* weights;
    const std::uint16_t* blendShapeMap;
};
static_assert(offsetof(DecompressContext, data) == 0x60 && offsetof(DecompressContext, idMap) == 0x70 &&
              offsetof(DecompressContext, stride) == 0x80 && offsetof(DecompressContext, blendShapeMap) == 0x90,
              "DecompressFrameContext layout");

struct alignas(16) DecompressData {
    std::uint32_t segments;
    std::uint8_t flags;
    std::uint8_t pad0[3];
    const std::uint8_t* clip;
    float weight;
    std::int32_t key;
    float frac;
    std::uint32_t pad1;
    std::uint8_t boneSet[0xA0];
    std::uint8_t usage[kMaxSegments / 8 + 0x40];  // +0xC0: one bit per segment (all on), then the untouched mask
};
static_assert(offsetof(DecompressData, key) == 0x14 && offsetof(DecompressData, usage) == 0xC0, "DecompressFrameData");

using BuildTransformsFn = void(__fastcall*)(void* instance, std::uint32_t count, std::uint8_t* segs,
                                            const std::uint8_t* skeleton, float* out);
using DecompressFn = void(__fastcall*)(DecompressContext* ctx, std::uint8_t* segs, std::uint8_t* attribs,
                                       std::uint8_t* blendShapes, void* rootTranslation, void* rootRotation);
using GetBoneIndexFn = bool(__fastcall*)(const void* objectZone, std::uint16_t id, std::uint16_t* index);
using ForceRebuildFn = void(__fastcall*)(void* transformComponent);
using GetAnimComponentFn = void*(__fastcall*)(void* instance);
using UpdateBlendShapesFn = void(__fastcall*)(void* animComponent);
using GetBShapeIndexFn = std::uint16_t(__fastcall*)(const void* objectZone, std::uint16_t id);
using PostRebuildFn = void(__fastcall*)(void* instance);
using ZoneCountFn = std::uint16_t(__fastcall*)(const void* objectZone);
using ZoneSegmentsFn = const std::uint8_t*(__fastcall*)(const void* objectZone);
using ZoneAllSegmentsFn = std::uint32_t(__fastcall*)(const void* objectZone);

std::uintptr_t g_base = 0;
UpdateBlendShapesFn g_originalUpdateBlendShapes = nullptr;
GetBShapeIndexFn g_getBShapeIndex = nullptr;
bool g_shapesAvailable = false;    // the blend shape hook and layouts (CreateHooks)
bool g_fidgetsAvailable = false;   // the decompressor's add / wrap (CreateHooks)
bool g_posesWithoutComponent = false;   // the PostRebuildTransforms hook (CreateHooks, v0.9.436)
PostRebuildFn g_originalPostRebuild = nullptr;
ZoneCountFn g_zoneAnimatedSegments = nullptr;
ZoneAllSegmentsFn g_zoneSegments = nullptr;   // (v0.9.444; null: the animated ones alone, as before)
ZoneSegmentsFn g_zoneSegmentList = nullptr;
BuildTransformsFn g_buildTransformsEntry = nullptr;   // (the hooked entry: CombatAir's and our hooks run, then the game's)
std::atomic<int> g_noComponentPlayers{0};
// (v0.9.448) the player's bones that nothing moves, posed after its own (PoseLooseSegments)
using GetLocalPlayerInstanceFn = void*(__fastcall*)(int);
using TryToGetClothFn = void*(__fastcall*)(void* instance);
GetLocalPlayerInstanceFn g_getLocalPlayerInstance = nullptr;
TryToGetClothFn g_tryToGetCloth = nullptr;
bool g_looseSegments = false;              // (CreateHooks)
std::atomic<void*> g_localPlayer{nullptr};   // (TickGameThread)
// (v0.9.512) the cloth of an instance without an AnimComponentV2 (DriveCloth)
using CinematicActorFn = void(__fastcall*)(void* instance, bool);
CinematicActorFn g_enableCinematicActor = nullptr;
CinematicActorFn g_disableCinematicActor = nullptr;
bool g_clothDrive = false;                 // (CreateHooks)
std::atomic<std::uint32_t> g_clothKeepFaults{0};
bool g_available = false;
const char* g_unavailable = "not initialized";
BuildTransformsFn g_originalBuildTransforms = nullptr;
DecompressFn g_decompress = nullptr;
GetBoneIndexFn g_getBoneIndex = nullptr;
ForceRebuildFn g_forceRebuild = nullptr;
GetAnimComponentFn g_getAnimComponent = nullptr;
const std::uint8_t* const* g_registry = nullptr;
const std::uint8_t* const* g_animByNumber = nullptr;
float g_weights[kMaxSegments];
std::int64_t g_qpcFrequency = 1;

constexpr int kMaxLayers = 4;   // (the player's own clip and up to three more over it: PlayLayers)
constexpr int kMaxNames = 16;   // (FindNamedClips)
// Retargeted translations (PlayLayers' reference, v0.9.436): a clip's bone tracks that set a translation on a bone the
// reference has; after the clip is sampled, its segment's translation gets weight x (the instance's bind translation -
// the reference's), on the components the track sets.
struct Shift {
    std::uint8_t track = 0;   // the clip's bone track
    std::uint8_t mask = 0;    // the translation's components it sets (x 1, y 2, z 4)
    float pivot[3]{};         // the reference's bind translation of its bone
};
constexpr int kMaxShifts = 32, kMaxReference = 48;
struct Shifts {
    Shift list[kMaxShifts];
    int count = 0;
};
struct Extra {   // a layer over the player's clip (PlayLayers): looping, on its own bones
    const std::uint8_t* clip = nullptr;
    std::uint16_t clipNum = 0;
    std::uint16_t keys = 0;
    std::int16_t timePerKey = 0;
    std::uint8_t attribs = 0, blendShapes = 0;   // (its attribute and blend shape tracks: buffers given when it has them)
    float speed = 1.0f;
    // (v0.9.481) blended at weight (SetLayerWeights), with the player's clip as one sum (sumWithFirst), played once and
    // held (once), from startQpc (0: the player's start; RestartLayer)
    float weight = 1.0f;
    bool sumWithFirst = false;
    bool once = false;
    std::int64_t startQpc = 0;
    std::uint16_t idMap[256]{};
    bool shapes = false;                  // it sets blend shapes: its tracks -> the component's slots (0xFFFF: not)
    std::uint16_t shapeSlot[256]{};
    Shifts shifts;
    char name[48]{};
};
struct FidgetPart {   // one clip of a fidget: played once, on all its bones
    const std::uint8_t* clip = nullptr;
    std::uint16_t clipNum = 0;
    std::uint16_t keys = 0;
    std::int16_t timePerKey = 0;
    std::uint8_t attribs = 0, blendShapes = 0;
    std::uint16_t idMap[256]{};
    Shifts shifts;
};
struct FidgetDef {
    int firstPart = 0, partCount = 0;
    double units = 0.0;   // its clips' length, game time units
    char name[48]{};
};
struct Player {
    void* instance = nullptr;
    const std::uint8_t* zone = nullptr;
    std::uint32_t instanceId = 0;
    std::uint64_t componentId = 0;
    const std::uint8_t* clip = nullptr;
    std::uint16_t clipNum = 0;
    std::int16_t animId = 0;
    std::uint16_t keys = 0;
    std::int16_t timePerKey = 0;
    std::uint8_t bones = 0;
    std::uint8_t blendShapes = 0;
    std::uint8_t attribs = 0;
    bool loop = true;
    bool paused = false;
    float speed = 1.0f;
    double baseTime = 0.0;        // game time units at startQpc
    std::int64_t startQpc = 0;
    std::uint16_t idMap[256]{};
    char name[80]{};
    mutable volatile LONG samples = 0;  // poses written (hook threads)
    ULONGLONG startTick = 0;
    bool warnedNoPose = false;
    Extra extra[kMaxLayers - 1];
    int extraCount = 0;
    float weight = 1.0f;   // (v0.9.481) the player's own clip's (SetLayerWeights)
    // (v0.9.441) the player's own clip as a sequence (PlayLayers' first layer): its clips played back to back, the whole
    // looping, each with its own id map and shifts; chainUnits their length (game units). clip / keys are the first's.
    FidgetPart chain[3];
    int chainCount = 0;
    double chainUnits = 0.0;
    // blend shapes (PlayLayers): the layers' slots of the instance's AnimComponentV2, their values as last decoded
    // (hook threads), written into its weights right before the game hands them on (HookUpdateBlendShapes)
    void* animComponent = nullptr;
    // (v0.9.436) an instance without an AnimComponentV2 (the Model Viewer's characters): its pose built in the
    // PostRebuildTransforms hook; its blend shapes through a stand-in component of ours (see MakeFakeComponent)
    bool noComponent = false;
    void* fakeComponent = nullptr;
    int shapeCount = 0;                         // m_numBlendShapes at the start
    bool shapes = false;                        // the player's own clip sets blend shapes
    std::uint16_t shapeSlot[256]{};
    std::uint8_t shapeSet[kMaxShapes]{};        // the slots some layer sets
    int shapeSlots = 0;
    mutable float shapeValue[kMaxShapes]{};
    mutable volatile LONG shapeFrames = 0;      // decodes of them (hook threads)
    mutable volatile LONG shapePushes = 0;      // their writes into the weights
    int shapeWay = 0;                           // logged: 1 the game pushed them, 2 TickGameThread did
    // retargeted translations (PlayLayers' reference): the player's own clip's; the reference kept for AddFidgets
    Shifts shifts;
    InstanceAnim::BindPivot reference[kMaxReference];
    int referenceCount = 0;
    // fidgets (AddFidgets); TickGameThread schedules them under the exclusive lock, the hook reads
    FidgetPart parts[kMaxFidgetParts];
    int partCount = 0;
    FidgetDef fidgets[kMaxFidgets];
    int fidgetCount = 0;
    float fidgetMinGap = 10.0f, fidgetMaxGap = 20.0f;
    double fadeUnits = 0.0;
    int fidgetNow = -1, fidgetLast = -1;        // playing (-1: none), the last one played
    std::int64_t fidgetStartQpc = 0, fidgetNextQpc = 0;
    std::uint32_t random = 0;
};
// A player whose blend shapes TickGameThread pushed itself (the game does not for an instance it does not process):
// after it stops, its weights (zero by then) are pushed once more so the render model lets go of the last ones.
struct ShapeClear {
    std::uint32_t instanceId = 0;
    std::uint64_t componentId = 0;
    void* animComponent = nullptr;
    bool fake = false;   // our stand-in (freed after the push)
};
SRWLOCK g_playerLock = SRWLOCK_INIT;
Player g_players[kMaxPlayers];
int g_playerCount = 0;                  // g_playerLock
std::atomic<int> g_playerCountFast{0};  // for the hook's early out
std::atomic<std::uint64_t> g_framesSampled{0};
std::atomic<std::uint32_t> g_sampleFaults{0};
ShapeClear g_shapeClears[kMaxPlayers];   // g_playerLock
int g_shapeClearCount = 0;

std::mutex g_requestMutex;
struct PlayRequest {
    bool pending = false;
    std::uint32_t instanceId = 0;
    std::uint64_t componentId = 0;
    InstanceAnim::AnimListEntry entry;
    bool loop = true;
    float speed = 1.0f;
};
PlayRequest g_playRequest;  // g_requestMutex
struct ListRequest {
    bool pending = false;
    std::uint32_t instanceId = 0;
    std::uint64_t componentId = 0;
    bool all = false;
};
ListRequest g_listRequest;  // g_requestMutex

std::mutex g_listMutex;
std::vector<InstanceAnim::AnimListEntry> g_list;  // g_listMutex
std::uint32_t g_listInstanceId = 0;
std::uint64_t g_listComponentId = 0;
bool g_listAll = false;
bool g_listValid = false;
int g_listLibraries = 0;
std::atomic<std::uint64_t> g_listVersion{0};
char g_listStatus[160] = "No list yet";
char g_playStatus[200] = "Ready";
std::vector<InstanceAnim::AnimListEntry> g_scratch;  // game thread

void SetText(char* out, std::size_t size, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(out, size, fmt, args);
    va_end(args);
}

std::int64_t QpcNow() {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

// ---------- reading game memory (SEH) ----------
void CopyText(char* out, std::size_t size, const char* text) {
    out[0] = '\0';
    if (!text) return;
    std::size_t i = 0;
    for (; i + 1 < size && text[i]; ++i) out[i] = text[i];
    out[i] = '\0';
}

bool ClipLiveUnsafe(const std::uint8_t* clip, std::uint16_t number) {
    return clip && number && number < kClipNumberLimit && g_animByNumber[number] == clip &&
           *reinterpret_cast<const std::uint16_t*>(clip + kClipId) == number;
}

bool ClipLive(const std::uint8_t* clip, std::uint16_t number) {
    __try {
        return ClipLiveUnsafe(clip, number);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const std::uint8_t* LoadedClipUnsafe(const LibraryEntry& entry) {
    if (entry.animId == 0 || entry.animNum <= 0 || static_cast<std::uint32_t>(entry.animNum) >= kClipNumberLimit)
        return nullptr;
    const std::uint8_t* clip = g_animByNumber[entry.animNum];
    if (!clip || *reinterpret_cast<const std::uint16_t*>(clip + kClipId) != static_cast<std::uint16_t>(entry.animNum))
        return nullptr;
    if (*reinterpret_cast<const std::uint16_t*>(clip + kClipKeys) == 0 ||
        *reinterpret_cast<const std::int16_t*>(clip + kClipTimePerKey) <= 0)
        return nullptr;
    return clip;
}

const char* LastPathPart(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/') last = p + 1;
    return last;
}

// The library list keys of the graphs an instance runs (main, LOD, hosted).
int InstanceKeysUnsafe(void* instance, std::uint32_t* keys, int capacity) {
    int count = 0;
    auto add = [&](std::uint32_t key) {
        if (!key) return;
        for (int i = 0; i < count; ++i)
            if (keys[i] == key) return;
        if (count < capacity) keys[count++] = key;
    };
    auto* anim = static_cast<const std::uint8_t*>(g_getAnimComponent(instance));
    if (!anim) return -1;
    const auto* manager = *reinterpret_cast<const std::uint8_t* const*>(anim + kAnimComponentGraphManager);
    if (!manager) return -1;
    const auto* dtp = *reinterpret_cast<const std::uint8_t* const*>(manager + kGraphManagerDtp);
    if (!dtp) return -1;
    std::uint32_t mainKey = 0;
    if (*reinterpret_cast<const std::uint32_t*>(dtp + kDtpMainCount) != 0) {
        const auto* main = *reinterpret_cast<const std::uint8_t* const*>(dtp + kDtpMain);
        if (main) {
            const auto* graph = *reinterpret_cast<const std::uint8_t* const*>(main);
            mainKey = *reinterpret_cast<const std::uint32_t*>(main + 8);
            if (!mainKey && graph) mainKey = *reinterpret_cast<const std::uint32_t*>(graph);
            add(mainKey);
        }
    }
    for (std::size_t lod : {kDtpLod1, kDtpLod2}) {
        const auto* graph = *reinterpret_cast<const std::uint8_t* const*>(dtp + lod);
        if (graph) add(mainKey ? mainKey : *reinterpret_cast<const std::uint32_t*>(graph));
    }
    const std::uint32_t hostedCount = *reinterpret_cast<const std::uint32_t*>(dtp + kDtpHostedCount);
    const auto* hosted = *reinterpret_cast<const std::uint8_t* const*>(dtp + kDtpHosted);
    if (hosted && hostedCount < 256) {
        for (std::uint32_t i = 0; i < hostedCount; ++i) {
            const auto* graph = *reinterpret_cast<const std::uint8_t* const*>(hosted + i * 0x10);
            std::uint32_t key = *reinterpret_cast<const std::uint32_t*>(hosted + i * 0x10 + 8);
            if (!key && graph) key = *reinterpret_cast<const std::uint32_t*>(graph);
            add(key);
        }
    }
    return count;
}

int InstanceKeys(void* instance, std::uint32_t* keys, int capacity) {
    __try {
        return InstanceKeysUnsafe(instance, keys, capacity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

// Every loaded entry of the wanted libraries (all of them when keyCount < 0).
int ScanLibrariesUnsafe(const std::uint32_t* keys, int keyCount, InstanceAnim::AnimListEntry* out, int capacity,
                        int* libraries) {
    *libraries = 0;
    const std::uint8_t* map = *g_registry;
    if (!map) return 0;
    const auto* const* buckets = *reinterpret_cast<const std::uint8_t* const* const*>(map);
    const std::uint64_t bucketCount = *reinterpret_cast<const std::uint64_t*>(map + 0x18);
    if (!buckets || bucketCount == 0 || bucketCount > (1u << 20)) return 0;
    int count = 0;
    for (std::uint64_t b = 0; b < bucketCount; ++b) {
        const std::uint8_t* node = buckets[b];
        for (int guard = 0; node && guard < 4096; ++guard, node = *reinterpret_cast<const std::uint8_t* const*>(node)) {
            const std::uint32_t key = *reinterpret_cast<const std::uint32_t*>(node + 8);
            if (keyCount >= 0) {
                bool wanted = false;
                for (int k = 0; k < keyCount && !wanted; ++k) wanted = keys[k] == key;
                if (!wanted) continue;
            }
            const auto* library = *reinterpret_cast<const std::uint8_t* const*>(node + 0x10);
            if (!library) continue;
            const std::uint32_t entryCount = *reinterpret_cast<const std::uint32_t*>(library + kLibraryCount);
            const auto* entries = *reinterpret_cast<const LibraryEntry* const*>(library + kLibraryEntries);
            if (!entries || entryCount == 0 || entryCount > 20000) continue;
            const char* libraryName = *reinterpret_cast<const char* const*>(library + kLibraryName);
            ++*libraries;
            for (std::uint32_t i = 0; i < entryCount && count < capacity; ++i) {
                const LibraryEntry& entry = entries[i];
                const std::uint8_t* clip = LoadedClipUnsafe(entry);
                if (!clip) continue;
                InstanceAnim::AnimListEntry& e = out[count++];
                e.libraryKey = key;
                e.index = static_cast<std::uint16_t>(i);
                e.animId = entry.animId;
                e.keys = *reinterpret_cast<const std::uint16_t*>(clip + kClipKeys);
                e.timePerKey = *reinterpret_cast<const std::int16_t*>(clip + kClipTimePerKey);
                CopyText(e.name, sizeof(e.name), entry.name);
                if (!e.name[0]) std::snprintf(e.name, sizeof(e.name), "anim_%d", static_cast<int>(entry.animId));
                CopyText(e.library, sizeof(e.library), libraryName ? LastPathPart(libraryName) : "");
            }
        }
    }
    return count;
}

int ScanLibraries(const std::uint32_t* keys, int keyCount, InstanceAnim::AnimListEntry* out, int capacity,
                  int* libraries) {
    __try {
        return ScanLibrariesUnsafe(keys, keyCount, out, capacity, libraries);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// The entry the list showed, looked up again now: its clip if it is still loaded.
const std::uint8_t* FindEntryClipUnsafe(std::uint32_t key, std::uint16_t index, std::int16_t animId,
                                        std::uint16_t* number, char* name, std::size_t nameSize) {
    const std::uint8_t* map = *g_registry;
    if (!map) return nullptr;
    const auto* const* buckets = *reinterpret_cast<const std::uint8_t* const* const*>(map);
    const std::uint64_t bucketCount = *reinterpret_cast<const std::uint64_t*>(map + 0x18);
    if (!buckets || bucketCount == 0 || bucketCount > (1u << 20)) return nullptr;
    for (std::uint64_t b = 0; b < bucketCount; ++b) {
        const std::uint8_t* node = buckets[b];
        for (int guard = 0; node && guard < 4096; ++guard, node = *reinterpret_cast<const std::uint8_t* const*>(node)) {
            if (*reinterpret_cast<const std::uint32_t*>(node + 8) != key) continue;
            const auto* library = *reinterpret_cast<const std::uint8_t* const*>(node + 0x10);
            if (!library) continue;
            const std::uint32_t entryCount = *reinterpret_cast<const std::uint32_t*>(library + kLibraryCount);
            const auto* entries = *reinterpret_cast<const LibraryEntry* const*>(library + kLibraryEntries);
            if (!entries || index >= entryCount || entries[index].animId != animId) continue;
            const std::uint8_t* clip = LoadedClipUnsafe(entries[index]);
            if (!clip) continue;
            *number = static_cast<std::uint16_t>(entries[index].animNum);
            CopyText(name, nameSize, entries[index].name);
            return clip;
        }
    }
    return nullptr;
}

const std::uint8_t* FindEntryClip(std::uint32_t key, std::uint16_t index, std::int16_t animId, std::uint16_t* number,
                                  char* name, std::size_t nameSize) {
    __try {
        return FindEntryClipUnsafe(key, index, animId, number, name, nameSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// The clip's bone tracks -> the instance's skeleton segments (AnimFragmentNode::GenerateIDMaps). Returns the number
// of tracks that found a segment, -1 on a fault.
int BuildIdMap(const std::uint8_t* zone, const std::uint8_t* clip, std::uint16_t* idMap, std::uint8_t* bonesOut) {
    __try {
        const std::uint8_t bones = clip[kClipBones];
        const auto* ids = *reinterpret_cast<const std::uint16_t* const*>(clip + kClipBoneIds);
        *bonesOut = bones;
        int mapped = 0;
        for (int i = 0; i < 256; ++i) idMap[i] = 0xFFFF;
        if (!ids) return 0;
        for (std::uint8_t i = 0; i < bones; ++i) {
            std::uint16_t index = 0xFFFF;
            if (g_getBoneIndex(zone, ids[i], &index) && index < kMaxSegments) {
                idMap[i] = index;
                ++mapped;
            }
        }
        return mapped;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void ReadClipHeader(const std::uint8_t* clip, Player& p) {
    __try {
        p.keys = *reinterpret_cast<const std::uint16_t*>(clip + kClipKeys);
        p.timePerKey = *reinterpret_cast<const std::int16_t*>(clip + kClipTimePerKey);
        p.blendShapes = clip[kClipBlendShapes];
        p.attribs = clip[kClipAttribs];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        p.keys = 0;
    }
}

const std::uint8_t* ReadZone(void* instance) {
    __try {
        return *reinterpret_cast<const std::uint8_t* const*>(static_cast<const std::uint8_t*>(instance) + kInstanceObjectZone);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool HasAnimComponent(void* instance) {
    __try {
        return g_getAnimComponent(instance) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ForceRebuild(void* instance) {
    __try {
        void* transform = *reinterpret_cast<void* const*>(static_cast<std::uint8_t*>(instance) + kInstanceTransformComponent);
        if (transform) g_forceRebuild(transform);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ---------- playback ----------
// The player's clip time (game units), wrapped or held. looped: the time went past the end at least once.
double PlayerTime(const Player& p, std::int64_t now, bool* looped) {
    const double duration = p.chainCount > 0 ? p.chainUnits : static_cast<double>(p.keys) * p.timePerKey;
    double t = p.baseTime;
    if (!p.paused)
        t += static_cast<double>(now - p.startQpc) / static_cast<double>(g_qpcFrequency) * kTimeUnitsPerSecond * p.speed;
    if (looped) *looped = false;
    if (duration <= 0.0) return 0.0;
    if (p.loop) {
        if (t >= duration && looped) *looped = true;
        t = std::fmod(t, duration);
        if (t < 0.0) t += duration;
    } else {
        t = std::clamp(t, 0.0, duration - p.timePerKey * 0.001);
    }
    return t;
}

int Sample(DecompressContext* ctx, std::uint8_t* segs, std::uint8_t* attribs, std::uint8_t* blendShapes) {
    __try {
        if (!ClipLiveUnsafe(ctx->clip, *reinterpret_cast<const std::uint16_t*>(ctx->clip + kClipId))) return 0;
        for (int i = 0; i < 12; ++i)
            ctx->ptrs[i] = *reinterpret_cast<const std::uint8_t* const*>(ctx->clip + kClipPtrsFirst + i * 8);
        g_decompress(ctx, segs, attribs, blendShapes, nullptr, nullptr);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// A clip's blend shapes out of SampleClip: value[slot] for the slots shapeSlot maps its tracks to (below limit).
struct ShapeOut {
    const std::uint16_t* shapeSlot;
    float* value;
    int limit;
};
// The decoded blend shapes (the buffer's weights and their map values: track indices here) -> out.
void TakeShapes(const std::uint8_t* buffer, std::uint8_t tracks, const ShapeOut& out) {
    const int n = *reinterpret_cast<const std::int32_t*>(buffer);
    if (n <= 0 || n > tracks) return;
    const auto* weights = reinterpret_cast<const float*>(buffer + 4);
    const std::uint8_t* which = buffer + 4 + 4 * static_cast<std::size_t>(tracks);
    for (int j = 0; j < n; ++j) {
        const std::uint16_t slot = out.shapeSlot[which[j]];
        if (slot < out.limit) out.value[slot] = weights[j];
    }
}

// One clip into the pose at time t (game units): its tracks the id map maps overwrite their segments (weight 1), or,
// with flags 8 (| 0x10), are added at that weight (their rotations wrapped to the pose's). shapes: its blend shape
// tracks decoded too (all of them, in order: a map that skipped some would spoil the ones after; see the facts).
int SampleClip(const std::uint8_t* clip, std::uint16_t keys, std::int16_t timePerKey, bool wrap, double t,
               const std::uint16_t* map, std::uint8_t attribTracks, std::uint8_t blendShapeTracks, std::uint32_t count,
               std::uint8_t* segs, const std::uint8_t* skeleton, float weight = 1.0f, std::uint8_t flags = 0,
               const ShapeOut* shapes = nullptr) {
    int key = static_cast<int>(t / timePerKey);
    if (key >= keys) key = keys - 1;
    if (key < 0) key = 0;
    const float frac = static_cast<float>((t - static_cast<double>(timePerKey) * key) / timePerKey);

    DecompressData data;
    std::memset(&data, 0, offsetof(DecompressData, usage));
    std::memset(data.usage, 0xFF, sizeof(data.usage));
    data.segments = count;
    data.flags = static_cast<std::uint8_t>((wrap ? 0x02 : 0x00) | flags);  // keep untouched bones (no 4)
    data.clip = clip;
    data.weight = weight;
    data.key = key;
    data.frac = frac;

    std::uint16_t idMap[256];
    for (int i = 0; i < 256; ++i) idMap[i] = (map[i] < count) ? map[i] : 0xFFFF;
    std::uint16_t blendShapeMap[256];
    for (int i = 0; i < 256; ++i) blendShapeMap[i] = (shapes && i < blendShapeTracks) ? static_cast<std::uint16_t>(i) : 0xFFFF;

    DecompressContext ctx{};
    ctx.data = &data;
    ctx.clip = clip;
    ctx.idMap = idMap;
    ctx.pivots = skeleton + 0x20;
    ctx.stride = 0x50;
    ctx.weights = g_weights;
    ctx.blendShapeMap = blendShapeMap;

    alignas(16) std::uint8_t blendShapeBuffer[4 + 256 * 5 + 16];
    *reinterpret_cast<std::int32_t*>(blendShapeBuffer) = 0;
    std::uint8_t* attribs = nullptr;
    if (attribTracks) attribs = static_cast<std::uint8_t*>(_aligned_malloc(static_cast<std::size_t>(count) * 16 + 0x100, 16));
    if (attribTracks && !attribs) return 0;
    const int result = Sample(&ctx, segs, attribs, blendShapeTracks ? blendShapeBuffer : nullptr);
    if (attribs) _aligned_free(attribs);
    if (result > 0 && shapes && blendShapeTracks) TakeShapes(blendShapeBuffer, blendShapeTracks, *shapes);
    return result;
}

// (PlayLayers, AddFidgets) The clip's Shifts for this reference. Its bone tracks' channels: +0x70 (from its +2) one
// byte a track, two when it has 0x88 (the second the high byte; & 0x7777), bit 4 x attribute + component (rotation,
// translation, scale; as the Suite's anim reader reads the files). Returns how many, -1 on a fault.
int BuildShifts(const std::uint8_t* clip, const InstanceAnim::BindPivot* reference, int referenceCount, Shifts& out) {
    out = Shifts{};
    if (!reference || referenceCount <= 0) return 0;
    __try {
        const std::uint8_t tracks = clip[kClipBones];
        const auto* ids = *reinterpret_cast<const std::uint16_t* const*>(clip + kClipBoneIds);
        const std::uint8_t* flags = *reinterpret_cast<const std::uint8_t* const*>(clip + kClipChannelFlags);
        if (!ids || !flags) return 0;
        const std::uint8_t* at = flags + 2;
        for (int b = 0; b < tracks; ++b) {
            unsigned f = *at++;
            if (f & 0x88) f = ((unsigned(*at++) << 8) | f) & 0x7777;
            const unsigned mask = (f >> 4) & 7;
            if (!mask) continue;
            for (int r = 0; r < referenceCount; ++r) {
                if (reference[r].bone != ids[b]) continue;
                if (out.count < kMaxShifts) {
                    Shift& shift = out.list[out.count++];
                    shift.track = static_cast<std::uint8_t>(b);
                    shift.mask = static_cast<std::uint8_t>(mask);
                    for (int k = 0; k < 3; ++k) shift.pivot[k] = reference[r].pivot[k];
                }
                break;
            }
        }
        return out.count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = Shifts{};
        return -1;
    }
}
// After a clip is sampled at weight: its Shifts onto the instance's skeleton (see Shift).
bool ApplyShifts(std::uint8_t* segs, std::uint32_t count, const std::uint8_t* skeleton, const std::uint16_t* map,
                 const Shifts& shifts, float weight) {
    if (shifts.count == 0 || !skeleton) return true;
    __try {
        for (int i = 0; i < shifts.count; ++i) {
            const Shift& shift = shifts.list[i];
            const std::uint16_t seg = map[shift.track];
            if (seg >= count) continue;
            const auto* pivot = reinterpret_cast<const float*>(skeleton + std::size_t(seg) * kSkeletonStride + kSkeletonPivot);
            auto* translation = reinterpret_cast<float*>(segs + std::size_t(seg) * 0x40 + 0x10);
            for (int k = 0; k < 3; ++k)
                if (shift.mask >> k & 1) translation[k] += weight * (pivot[k] - shift.pivot[k]);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Fidget blends (SamplePlayer): the segments a clip's id map reaches scaled by keep (rotation, translation, scale and
// weight), so a clip added at 1 - keep over them blends with the pose; after it, a segment whose weight is not 1 (the
// pose's was not) divided by it.
bool ScaleSegments(std::uint8_t* segs, std::uint32_t count, const std::uint16_t* map, float keep) {
    __try {
        for (int i = 0; i < 256; ++i) {
            if (map[i] >= count) continue;
            float* f = reinterpret_cast<float*>(segs + static_cast<std::size_t>(map[i]) * 0x40);
            for (std::size_t k = 0; k < kSegmentFloats; ++k) f[k] *= keep;
            f[kSegmentWeight] *= keep;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool NormalizeSegments(std::uint8_t* segs, std::uint32_t count, const std::uint16_t* map) {
    __try {
        for (int i = 0; i < 256; ++i) {
            if (map[i] >= count) continue;
            float* f = reinterpret_cast<float*>(segs + static_cast<std::size_t>(map[i]) * 0x40);
            const float w = f[kSegmentWeight];
            if (!(w > 1e-6f) || std::fabs(w - 1.0f) < 1e-5f) continue;
            const float inv = 1.0f / w;
            for (std::size_t k = 0; k < kSegmentFloats; ++k) f[k] *= inv;
            f[kSegmentWeight] = 1.0f;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

double QpcToUnits(std::int64_t ticks) {
    return static_cast<double>(ticks) / static_cast<double>(g_qpcFrequency) * kTimeUnitsPerSecond;
}

// The fidget playing (if any) over the pose: its clip at its time, faded in and out.
int SampleFidget(const Player& p, std::int64_t now, std::uint32_t count, std::uint8_t* segs, const std::uint8_t* skeleton) {
    const int f = p.fidgetNow;
    if (f < 0 || f >= p.fidgetCount || p.paused) return 0;
    const FidgetDef& def = p.fidgets[f];
    const double e = QpcToUnits(now - p.fidgetStartQpc);
    if (e < 0.0 || e >= def.units) return 0;
    double a = 1.0;
    if (p.fadeUnits > 0.0) a = std::min({1.0, e / p.fadeUnits, (def.units - e) / p.fadeUnits});
    if (a <= 0.0) return 0;
    double start = 0.0;
    for (int k = 0; k < def.partCount; ++k) {
        const FidgetPart& part = p.parts[def.firstPart + k];
        const double units = static_cast<double>(part.keys) * part.timePerKey;
        if (e < start + units || k + 1 == def.partCount) {
            const float weight = static_cast<float>(a);
            if (!ScaleSegments(segs, count, part.idMap, 1.0f - weight)) return -1;
            const int r = SampleClip(part.clip, part.keys, part.timePerKey, false, std::max(0.0, e - start), part.idMap,
                                     part.attribs, part.blendShapes, count, segs, skeleton, weight, 0x08 | 0x10);
            if (r > 0 && !ApplyShifts(segs, count, skeleton, part.idMap, part.shifts, weight)) return -1;
            if (r < 0 || !NormalizeSegments(segs, count, part.idMap)) return -1;
            return r;
        }
        start += units;
    }
    return 0;
}

// (v0.9.481) Weighted layers (SetLayerWeights). The player's clip and the layers summed with it are one weighted sum
// over the pose below: their weights add up to how much of it they take (the rest stays the pose's). The pose's
// segments they reach scaled by what is left (or the first written at its weight, when they take all of it), each
// added at its weight (rotations wrapped to the pose's, as a fidget), and the sum divided by its weight afterwards.
constexpr float kWeightNone = 0.001f, kWeightFull = 0.999f;
bool WeightedGroup(const Player& p) {
    if (p.weight < kWeightFull) return true;
    for (int i = 0; i < p.extraCount; ++i)
        if (p.extra[i].sumWithFirst && p.extra[i].weight > kWeightNone) return true;
    return false;
}

int SampleWeightedGroup(const Player& p, std::int64_t now, double t, bool looped, std::uint32_t count, std::uint8_t* segs,
                        const std::uint8_t* skeleton, const ShapeOut* shapes) {
    const float w0 = std::clamp(p.weight, 0.0f, 1.0f);
    float total = w0;
    for (int i = 0; i < p.extraCount; ++i)
        if (p.extra[i].sumWithFirst) total += std::clamp(p.extra[i].weight, 0.0f, 1.0f);
    if (total <= kWeightNone) return 0;
    const float scale = total > 1.0f ? 1.0f / total : 1.0f;   // (more than all of it: each its share)
    total = std::min(total, 1.0f);
    const double elapsed = p.paused ? 0.0 : QpcToUnits(now - p.startQpc);
    bool written = false;   // the pose's share already gone (the first written over it)
    int result = 0;
    auto add = [&](const std::uint8_t* clip, std::uint16_t keys, std::int16_t timePerKey, bool wrap, double at,
                   const std::uint16_t* map, std::uint8_t attribs, std::uint8_t blendShapes, const Shifts& shifts, float w,
                   const ShapeOut* out) {
        if (w <= kWeightNone || result < 0) return;
        int r = 0;
        if (!written && total >= kWeightFull) {
            r = SampleClip(clip, keys, timePerKey, wrap, at, map, attribs, blendShapes, count, segs, skeleton, w, 0, out);
            written = true;
        } else {
            if (!written && !ScaleSegments(segs, count, map, 1.0f - total)) r = -1;
            written = true;
            if (r >= 0)
                r = SampleClip(clip, keys, timePerKey, wrap, at, map, attribs, blendShapes, count, segs, skeleton, w, 0x08 | 0x10,
                               nullptr);
        }
        if (r > 0 && !ApplyShifts(segs, count, skeleton, map, shifts, w)) r = -1;
        if (r < 0) result = r;
        else if (r > 0) result = 1;
    };
    add(p.clip, p.keys, p.timePerKey, p.loop && looped, t, p.idMap, p.attribs, p.blendShapes, p.shifts, w0 * scale, shapes);
    for (int i = 0; i < p.extraCount; ++i) {
        const Extra& e = p.extra[i];
        const double duration = static_cast<double>(e.keys) * e.timePerKey;
        if (!e.sumWithFirst || duration <= 0.0) continue;
        const double raw = e.startQpc && !p.paused ? QpcToUnits(now - e.startQpc) * e.speed : elapsed * e.speed;
        const double at = e.once ? std::clamp(raw, 0.0, duration - e.timePerKey * 0.001) : std::fmod(raw, duration);
        add(e.clip, e.keys, e.timePerKey, !e.once && raw >= duration, at, e.idMap, e.attribs, e.blendShapes, e.shifts,
            std::clamp(e.weight, 0.0f, 1.0f) * scale, nullptr);
    }
    if (result >= 0 && written) {
        if (!NormalizeSegments(segs, count, p.idMap)) result = -1;
        for (int i = 0; i < p.extraCount && result >= 0; ++i)
            if (p.extra[i].sumWithFirst && !NormalizeSegments(segs, count, p.extra[i].idMap)) result = -1;
    }
    return result;
}

void SamplePlayer(const Player& p, std::uint32_t count, std::uint8_t* segs, const std::uint8_t* skeleton) {
    bool looped = false;
    const std::int64_t now = QpcNow();
    const double t = PlayerTime(p, now, &looped);
    // (blend shapes: decoded into a copy, in layer order, then handed to the player at once)
    float shapeValue[kMaxShapes];
    std::memcpy(shapeValue, p.shapeValue, sizeof(shapeValue));
    const int shapeLimit = std::min(p.shapeCount, kMaxShapes);
    const ShapeOut own{p.shapeSlot, shapeValue, shapeLimit};
    int result = 0;
    if (p.chainCount > 0) {   // (v0.9.441) a sequence: the clip the time is in
        double start = 0.0;
        for (int k = 0; k < p.chainCount; ++k) {
            const FidgetPart& part = p.chain[k];
            const double units = static_cast<double>(part.keys) * part.timePerKey;
            if (t < start + units || k + 1 == p.chainCount) {
                result = SampleClip(part.clip, part.keys, part.timePerKey, false, std::clamp(t - start, 0.0, units), part.idMap,
                                    part.attribs, part.blendShapes, count, segs, skeleton, 1.0f, 0, nullptr);
                if (result > 0 && !ApplyShifts(segs, count, skeleton, part.idMap, part.shifts, 1.0f)) result = -1;
                break;
            }
            start += units;
        }
    } else if (!WeightedGroup(p)) {
        result = SampleClip(p.clip, p.keys, p.timePerKey, p.loop && looped, t, p.idMap, p.attribs, p.blendShapes, count,
                            segs, skeleton, 1.0f, 0, p.shapes ? &own : nullptr);
        if (result > 0 && !ApplyShifts(segs, count, skeleton, p.idMap, p.shifts, 1.0f)) result = -1;
    } else {
        result = SampleWeightedGroup(p, now, t, looped, count, segs, skeleton, p.shapes ? &own : nullptr);
    }
    // a fidget over it (blended), then the layers over both
    if (result >= 0 && p.fidgetNow >= 0) {
        const int r = SampleFidget(p, now, count, segs, skeleton);
        if (r < 0) result = r;
    }
    // the layers: each on its own clock (the player's, at its speed), looping
    const double elapsed = p.paused ? 0.0 : QpcToUnits(now - p.startQpc);
    for (int i = 0; i < p.extraCount && result >= 0; ++i) {
        const Extra& e = p.extra[i];
        const double duration = static_cast<double>(e.keys) * e.timePerKey;
        if (duration <= 0.0) continue;
        if (e.sumWithFirst && p.chainCount == 0) continue;   // (v0.9.481: sampled with the first layer)
        const float w = std::clamp(e.weight, 0.0f, 1.0f);
        if (w <= kWeightNone) continue;
        const double raw = e.startQpc && !p.paused ? QpcToUnits(now - e.startQpc) * e.speed : elapsed * e.speed;
        const bool wrap = !e.once && raw >= duration;
        const double at = e.once ? std::clamp(raw, 0.0, duration - e.timePerKey * 0.001) : std::fmod(raw, duration);
        const ShapeOut out{e.shapeSlot, shapeValue, shapeLimit};
        int r = 0;
        if (w >= kWeightFull) {
            r = SampleClip(e.clip, e.keys, e.timePerKey, wrap, at, e.idMap, e.attribs, e.blendShapes, count, segs, skeleton,
                           1.0f, 0, e.shapes ? &out : nullptr);
            if (r > 0 && !ApplyShifts(segs, count, skeleton, e.idMap, e.shifts, 1.0f)) result = -1;
        } else {   // (v0.9.481) blended into the pose (as a fidget)
            if (!ScaleSegments(segs, count, e.idMap, 1.0f - w)) r = -1;
            if (r >= 0) r = SampleClip(e.clip, e.keys, e.timePerKey, wrap, at, e.idMap, e.attribs, e.blendShapes, count, segs,
                                       skeleton, w, 0x08 | 0x10, nullptr);
            if (r > 0 && !ApplyShifts(segs, count, skeleton, e.idMap, e.shifts, w)) r = -1;
            if (r >= 0 && !NormalizeSegments(segs, count, e.idMap)) r = -1;
        }
        if (r < 0) result = r;
    }
    if (result > 0 && p.shapeSlots > 0) {
        std::memcpy(p.shapeValue, shapeValue, sizeof(shapeValue));
        InterlockedIncrement(&p.shapeFrames);
    }
    if (result > 0) {
        if (InterlockedIncrement(&p.samples) == 1)
            Log::Detailf("InstanceAnim: first pose of %s written on instance %u (%u segments)", p.name, p.instanceId, count);
        g_framesSampled.fetch_add(1, std::memory_order_relaxed);
    } else if (result < 0) {
        const std::uint32_t n = g_sampleFaults.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 5) Log::Writef("InstanceAnim: sampling '%s' faulted (#%u)", p.name, n);
    }
}

void __fastcall HookBuildTransforms(void* instance, std::uint32_t count, std::uint8_t* segs,
                                    const std::uint8_t* skeleton, float* out) {
    InstanceAnim::OnBuildTransforms(instance, count, segs, skeleton);
    g_originalBuildTransforms(instance, count, segs, skeleton, out);
}

Player* FindPlayerLocked(std::uint32_t instanceId, std::uint64_t componentId) {
    for (int i = 0; i < g_playerCount; ++i)
        if (g_players[i].instanceId == instanceId && g_players[i].componentId == componentId) return &g_players[i];
    return nullptr;
}

// A player going (removed or replaced): its last blend shapes let go of by TickGameThread (see ShapeClear); our
// stand-in component freed there.
void RetireLocked(const Player& gone) {
    if ((gone.shapeWay == 2 || gone.fakeComponent) && g_shapeClearCount < kMaxPlayers)
        g_shapeClears[g_shapeClearCount++] = {gone.instanceId, gone.componentId, gone.animComponent, gone.fakeComponent != nullptr};
    else if (gone.fakeComponent)
        _aligned_free(gone.fakeComponent);
}
void CountedLocked() {
    int noComponent = 0;
    for (int k = 0; k < g_playerCount; ++k) noComponent += g_players[k].noComponent ? 1 : 0;
    g_noComponentPlayers.store(noComponent, std::memory_order_release);
    g_playerCountFast.store(g_playerCount, std::memory_order_release);
}
// The player into a slot (a new one, or the one already there replaced).
Player* StoreLocked(const Player& p) {
    Player* slot = FindPlayerLocked(p.instanceId, p.componentId);
    if (slot) RetireLocked(*slot);
    else if (g_playerCount < kMaxPlayers) slot = &g_players[g_playerCount++];
    if (slot) *slot = p;
    CountedLocked();
    return slot;
}

void RemovePlayerLocked(int i) {
    RetireLocked(g_players[i]);
    for (int k = i; k + 1 < g_playerCount; ++k) g_players[k] = g_players[k + 1];
    --g_playerCount;
    CountedLocked();
}

void ProcessPlayRequest(const PlayRequest& request) {
    void* instance = Features::InstanceNowGameThread(request.instanceId, request.componentId);
    if (!instance) {
        SetText(g_playStatus, sizeof(g_playStatus), "Play failed: the Instance is gone or its slot changed.");
        return;
    }
    if (!HasAnimComponent(instance)) {
        SetText(g_playStatus, sizeof(g_playStatus), "Play failed: this Instance has no animation component.");
        return;
    }
    const std::uint8_t* zone = ReadZone(instance);
    std::uint16_t number = 0;
    char name[80]{};
    const std::uint8_t* clip = FindEntryClip(request.entry.libraryKey, request.entry.index, request.entry.animId,
                                             &number, name, sizeof(name));
    if (!zone || !clip) {
        SetText(g_playStatus, sizeof(g_playStatus), "Play failed: %s (%d) is no longer loaded.", request.entry.name,
                static_cast<int>(request.entry.animId));
        return;
    }
    Player p;
    p.instance = instance;
    p.zone = zone;
    p.instanceId = request.instanceId;
    p.componentId = request.componentId;
    p.clip = clip;
    p.clipNum = number;
    p.animId = request.entry.animId;
    const int mapped = BuildIdMap(zone, clip, p.idMap, &p.bones);
    if (mapped <= 0) {
        SetText(g_playStatus, sizeof(g_playStatus),
                mapped < 0 ? "Play failed: could not read the clip's bones."
                           : "Play failed: none of the clip's bones exist on this Instance's skeleton.");
        return;
    }
    ReadClipHeader(clip, p);
    if (!p.keys || p.timePerKey <= 0) {
        SetText(g_playStatus, sizeof(g_playStatus), "Play failed: the clip has no frames.");
        return;
    }
    p.loop = request.loop;
    p.speed = std::clamp(request.speed, 0.01f, 10.0f);
    p.baseTime = 0.0;
    p.startQpc = QpcNow();
    p.startTick = GetTickCount64();
    CopyText(p.name, sizeof(p.name), name[0] ? name : request.entry.name);

    AcquireSRWLockExclusive(&g_playerLock);
    Player* slot = StoreLocked(p);
    ReleaseSRWLockExclusive(&g_playerLock);
    if (!slot) {
        SetText(g_playStatus, sizeof(g_playStatus), "Play failed: %d Instances already play animations.", kMaxPlayers);
        return;
    }
    SetText(g_playStatus, sizeof(g_playStatus), "Playing %s (%d): %u of %u bone tracks on this skeleton.", p.name,
            static_cast<int>(p.animId), static_cast<unsigned>(mapped), static_cast<unsigned>(p.bones));
    Log::Detailf("InstanceAnim: instance %u plays %s (anim %d, clip %u, %u keys x %d, %d/%u tracks mapped, loop %d)",
                p.instanceId, p.name, static_cast<int>(p.animId), static_cast<unsigned>(p.clipNum),
                static_cast<unsigned>(p.keys), static_cast<int>(p.timePerKey), mapped, static_cast<unsigned>(p.bones),
                p.loop ? 1 : 0);
}

void ProcessListRequest(const ListRequest& request) {
    std::uint32_t keys[kMaxKeys];
    int keyCount = -1;
    if (!request.all) {
        void* instance = Features::InstanceNowGameThread(request.instanceId, request.componentId);
        if (!instance) {
            SetText(g_listStatus, sizeof(g_listStatus), "The Instance is gone or its slot changed.");
            return;
        }
        keyCount = InstanceKeys(instance, keys, kMaxKeys);
        if (keyCount < 0) {
            std::lock_guard<std::mutex> lock(g_listMutex);
            g_list.clear();
            g_listInstanceId = request.instanceId;
            g_listComponentId = request.componentId;
            g_listAll = request.all;
            g_listValid = true;
            g_listLibraries = 0;
            SetText(g_listStatus, sizeof(g_listStatus),
                    keyCount == -1 ? "This Instance has no animation component (nothing to play)."
                                   : "Could not read this Instance's animation graphs.");
            g_listVersion.fetch_add(1, std::memory_order_release);
            return;
        }
    }
    if (g_scratch.size() != static_cast<std::size_t>(kMaxListEntries)) g_scratch.resize(kMaxListEntries);
    int libraries = 0;
    const int found = ScanLibraries(keys, keyCount, g_scratch.data(), kMaxListEntries, &libraries);
    std::lock_guard<std::mutex> lock(g_listMutex);
    g_list.assign(g_scratch.begin(), g_scratch.begin() + std::max(found, 0));
    g_listInstanceId = request.instanceId;
    g_listComponentId = request.componentId;
    g_listAll = request.all;
    g_listValid = true;
    g_listLibraries = libraries;
    if (found < 0)
        SetText(g_listStatus, sizeof(g_listStatus), "Reading the animation libraries failed; try Refresh.");
    else if (request.all)
        SetText(g_listStatus, sizeof(g_listStatus), "%d animations in %d loaded libraries", found, libraries);
    else if (keyCount == 0)
        SetText(g_listStatus, sizeof(g_listStatus), "This Instance's graphs name no animation library.");
    else
        SetText(g_listStatus, sizeof(g_listStatus), "%d animations in %d of this Instance's libraries", found, libraries);
    g_listVersion.fetch_add(1, std::memory_order_release);
}

std::string Lower(const char* text) {
    std::string s(text ? text : "");
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    s = s.substr(first, s.find_last_not_of(" \t") - first + 1);
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int Score(const std::string& name, const std::string& query) {
    if (name == query) return 0;
    if (name.rfind(query, 0) == 0) return 100 + static_cast<int>(name.size() - query.size());
    const auto at = name.find(query);
    if (at != std::string::npos) return 300 + static_cast<int>(at * 4) + static_cast<int>(name.size() - query.size());
    std::size_t qi = 0;
    int gaps = 0, last = -1;
    for (std::size_t ni = 0; ni < name.size() && qi < query.size(); ++ni) {
        if (name[ni] != query[qi]) continue;
        if (last >= 0) gaps += static_cast<int>(ni) - last - 1;
        last = static_cast<int>(ni);
        ++qi;
    }
    if (qi != query.size()) return std::numeric_limits<int>::max();
    return 700 + gaps * 8 + static_cast<int>(name.size() - query.size());
}

// (PlayLayers) The library that has the most of these names among its loaded clips: each one's clip, number and
// library entry index (null when not there); its path. Returns how many it has, -1 on a fault.
int FindNamedClipsUnsafe(const char* const* names, int count, const std::uint8_t** clips, std::uint16_t* numbers,
                         std::uint32_t* key, char* library, std::size_t librarySize) {
    const std::uint8_t* map = *g_registry;
    if (!map || count <= 0 || count > kMaxNames) return 0;
    const auto* const* buckets = *reinterpret_cast<const std::uint8_t* const* const*>(map);
    const std::uint64_t bucketCount = *reinterpret_cast<const std::uint64_t*>(map + 0x18);
    if (!buckets || bucketCount == 0 || bucketCount > (1u << 20)) return 0;
    int best = 0;
    const std::uint8_t* found[kMaxNames]{};
    std::uint16_t nums[kMaxNames]{};
    for (std::uint64_t b = 0; b < bucketCount; ++b) {
        const std::uint8_t* node = buckets[b];
        for (int guard = 0; node && guard < 4096; ++guard, node = *reinterpret_cast<const std::uint8_t* const*>(node)) {
            const auto* lib = *reinterpret_cast<const std::uint8_t* const*>(node + 0x10);
            if (!lib) continue;
            const std::uint32_t entryCount = *reinterpret_cast<const std::uint32_t*>(lib + kLibraryCount);
            const auto* entries = *reinterpret_cast<const LibraryEntry* const*>(lib + kLibraryEntries);
            if (!entries || entryCount == 0 || entryCount > 20000) continue;
            int have = 0;
            for (int n = 0; n < count; ++n) {
                found[n] = nullptr;
                for (std::uint32_t i = 0; i < entryCount && !found[n]; ++i) {
                    const char* name = entries[i].name;
                    if (!name || std::strcmp(name, names[n]) != 0) continue;
                    if (const std::uint8_t* clip = LoadedClipUnsafe(entries[i])) {
                        found[n] = clip;
                        nums[n] = static_cast<std::uint16_t>(entries[i].animNum);
                    }
                }
                if (found[n]) ++have;
            }
            if (have <= best) continue;
            best = have;
            for (int n = 0; n < count; ++n) clips[n] = found[n], numbers[n] = nums[n];
            *key = *reinterpret_cast<const std::uint32_t*>(node + 8);
            const char* path = *reinterpret_cast<const char* const*>(lib + kLibraryName);
            CopyText(library, librarySize, path ? LastPathPart(path) : "");
        }
    }
    return best;
}
int FindNamedClips(const char* const* names, int count, const std::uint8_t** clips, std::uint16_t* numbers,
                   std::uint32_t* key, char* library, std::size_t librarySize) {
    __try {
        return FindNamedClipsUnsafe(names, count, clips, numbers, key, library, librarySize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// (v0.9.441) The loaded clip a library lists with this animation ID (any library: the clip is the same resource). Returns
// 1 found, 0 not, -1 on a fault.
int FindClipByIdUnsafe(std::int16_t id, const std::uint8_t** clip, std::uint16_t* number, char* library, std::size_t librarySize) {
    const std::uint8_t* map = *g_registry;
    if (!map || id == 0) return 0;
    const auto* const* buckets = *reinterpret_cast<const std::uint8_t* const* const*>(map);
    const std::uint64_t bucketCount = *reinterpret_cast<const std::uint64_t*>(map + 0x18);
    if (!buckets || bucketCount == 0 || bucketCount > (1u << 20)) return 0;
    for (std::uint64_t b = 0; b < bucketCount; ++b) {
        const std::uint8_t* node = buckets[b];
        for (int guard = 0; node && guard < 4096; ++guard, node = *reinterpret_cast<const std::uint8_t* const*>(node)) {
            const auto* lib = *reinterpret_cast<const std::uint8_t* const*>(node + 0x10);
            if (!lib) continue;
            const std::uint32_t entryCount = *reinterpret_cast<const std::uint32_t*>(lib + kLibraryCount);
            const auto* entries = *reinterpret_cast<const LibraryEntry* const*>(lib + kLibraryEntries);
            if (!entries || entryCount == 0 || entryCount > 20000) continue;
            for (std::uint32_t i = 0; i < entryCount; ++i) {
                if (entries[i].animId != id) continue;
                const std::uint8_t* found = LoadedClipUnsafe(entries[i]);
                if (!found) continue;
                *clip = found;
                *number = static_cast<std::uint16_t>(entries[i].animNum);
                const char* path = *reinterpret_cast<const char* const*>(lib + kLibraryName);
                CopyText(library, librarySize, path ? LastPathPart(path) : "");
                return 1;
            }
        }
    }
    return 0;
}
int FindClipById(std::int16_t id, const std::uint8_t** clip, std::uint16_t* number, char* library, std::size_t librarySize) {
    __try {
        return FindClipByIdUnsafe(id, clip, number, library, librarySize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// (PlayLayers) BuildIdMap, its tracks of other bones than these (global IDs) left out. Returns the tracks mapped, -1
// on a fault.
int BuildMaskedIdMap(const std::uint8_t* zone, const std::uint8_t* clip, const std::uint16_t* bones, int boneCount,
                     std::uint16_t* idMap) {
    std::uint8_t tracks = 0;
    const int mapped = BuildIdMap(zone, clip, idMap, &tracks);
    if (mapped <= 0 || boneCount == InstanceAnim::kAllTracks) return mapped;
    if (boneCount <= 0 || !bones) {   // (none of its bones: a layer of blend shapes alone)
        for (int i = 0; i < 256; ++i) idMap[i] = 0xFFFF;
        return 0;
    }
    __try {
        const auto* ids = *reinterpret_cast<const std::uint16_t* const*>(clip + kClipBoneIds);
        int kept = 0;
        for (std::uint8_t i = 0; i < tracks; ++i) {
            bool wanted = false;
            for (int k = 0; k < boneCount && !wanted; ++k) wanted = ids && ids[i] == bones[k];
            if (!wanted) idMap[i] = 0xFFFF;
            else if (idMap[i] != 0xFFFF) ++kept;
        }
        return kept;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// (PlayLayers) The clip's blend shape tracks of these global IDs (all of them: kAllTracks; none: 0) -> the instance's
// slots below limit (ObjectZone::GetBShapeIndexFromID; 0xFFFF the others). Returns how many it maps, -1 on a fault.
int BuildShapeMap(const std::uint8_t* zone, const std::uint8_t* clip, const std::uint16_t* shapes, int shapeCount,
                  int limit, std::uint16_t* slots) {
    for (int i = 0; i < 256; ++i) slots[i] = 0xFFFF;
    if (shapeCount == 0 || limit <= 0 || !g_getBShapeIndex || (shapeCount > 0 && !shapes)) return 0;
    __try {
        const std::uint8_t tracks = clip[kClipBlendShapes];
        const auto* ids = *reinterpret_cast<const std::uint16_t* const*>(clip + kClipShapeIds);
        if (!ids) return 0;
        int mapped = 0;
        for (int i = 0; i < tracks; ++i) {
            bool wanted = shapeCount == InstanceAnim::kAllTracks;
            for (int k = 0; k < shapeCount && !wanted; ++k) wanted = shapes[k] == ids[i];
            if (!wanted) continue;
            const std::uint16_t slot = g_getBShapeIndex(zone, ids[i]);
            if (slot < limit) {
                slots[i] = slot;
                ++mapped;
            }
        }
        return mapped;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// The instance's AnimComponentV2 and its blend shape slots (0: none, or no weights), -1 on a fault.
int ReadShapeSlots(void* instance, void** animComponent) {
    __try {
        auto* comp = static_cast<std::uint8_t*>(g_getAnimComponent(instance));
        *animComponent = comp;
        if (!comp || !*reinterpret_cast<float* const*>(comp + kAnimWeights)) return 0;
        return std::max(*reinterpret_cast<const std::int32_t*>(comp + kAnimWeightCount), 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// The player's blend shapes into its component's weights (the slots its layers set, as last decoded).
bool WriteShapes(const Player& p) {
    __try {
        auto* comp = static_cast<std::uint8_t*>(p.animComponent);
        float* weights = *reinterpret_cast<float* const*>(comp + kAnimWeights);
        const int n = std::min(*reinterpret_cast<const std::int32_t*>(comp + kAnimWeightCount), kMaxShapes);
        if (!weights || n <= 0) return false;
        for (int s = 0; s < n; ++s)
            if (p.shapeSet[s]) weights[s] = p.shapeValue[s];
        InterlockedIncrement(&p.shapePushes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// cdc::AnimComponentV2::UpdateRenderModelBlendShapes (main thread, once a frame for a component the game processes;
// see the facts): the weights of a player's component get its blend shapes right before they are handed on.
void __fastcall HookUpdateBlendShapes(void* animComponent) {
    if (animComponent && g_playerCountFast.load(std::memory_order_acquire) > 0) {
        AcquireSRWLockShared(&g_playerLock);
        for (int i = 0; i < g_playerCount; ++i) {
            const Player& p = g_players[i];
            if (p.animComponent == animComponent && p.shapeSlots > 0 && p.shapeFrames > 0) WriteShapes(p);
        }
        ReleaseSRWLockShared(&g_playerLock);
    }
    g_originalUpdateBlendShapes(animComponent);
}

// Whether the game pushes the instance's blend shapes this frame (AnimatedComponentManager's test): 1 yes, 0 no, -1
// unreadable.
int GamePushesShapes(void* instance) {
    __try {
        const auto* b = static_cast<const std::uint8_t*>(instance);
        const std::uint32_t flags = *reinterpret_cast<const std::uint32_t*>(b + kInstanceFlags);
        if (flags & kFlagForceDeactivated) return 0;
        if (flags & kFlagForceActivated) return 1;
        return *reinterpret_cast<const std::int32_t*>(b + kInstanceProcessingState) != 0 ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool PushShapes(void* animComponent) {
    __try {
        g_originalUpdateBlendShapes(animComponent);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SameAnimComponent(void* instance, void* animComponent) {
    __try {
        return g_getAnimComponent(instance) == animComponent;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uint32_t NextRandom(std::uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
std::int64_t FidgetGapTicks(Player& p) {
    const double r = (NextRandom(p.random) % 10001u) / 10000.0;
    const double seconds = p.fidgetMinGap + (p.fidgetMaxGap - p.fidgetMinGap) * r;
    return static_cast<std::int64_t>(seconds * static_cast<double>(g_qpcFrequency));
}
// Game thread, exclusive lock: the fidget that ended ends, the next one starts when its wait is over.
void ScheduleFidget(Player& p, std::int64_t now) {
    if (p.fidgetCount == 0 || p.paused) return;
    if (p.fidgetNow >= 0) {
        if (QpcToUnits(now - p.fidgetStartQpc) < p.fidgets[p.fidgetNow].units) return;
        p.fidgetLast = p.fidgetNow;
        p.fidgetNow = -1;
        p.fidgetNextQpc = now + FidgetGapTicks(p);
        return;
    }
    if (now < p.fidgetNextQpc) return;
    int pick = static_cast<int>(NextRandom(p.random) % static_cast<std::uint32_t>(p.fidgetCount));
    if (p.fidgetCount > 1 && pick == p.fidgetLast)
        pick = (pick + 1 + static_cast<int>(NextRandom(p.random) % static_cast<std::uint32_t>(p.fidgetCount - 1))) %
               p.fidgetCount;
    p.fidgetNow = pick;
    p.fidgetStartQpc = now;
    Log::Detailf("InstanceAnim: instance %u fidgets: %s (%.1f s)", p.instanceId, p.fidgets[pick].name,
                 p.fidgets[pick].units / kTimeUnitsPerSecond);
}

// ---- An instance without an AnimComponentV2 (v0.9.436; the Model Viewer's characters, cine objects) ----
// G2Instance_RebuildTransforms (0x3DD940; TransformComponent = Instance +0xE0, its matrices +0x70, the root's one
// before them) builds such an instance's matrices from its skeleton's bind translations alone (no AnimBuildTransforms,
// no pose), then calls G2Instance_PostRebuildTransforms (0x3DD440), as it does after AnimComponentV2::BuildTransforms.
// That one is hooked: for a player's instance the matrices are built again, before it, by AnimBuildTransforms (its
// hooked entry, so the players' hooks fill the pose) on a pose of the bind translations (rotation 0, scale 1, weight 1,
// as AnimSegments_SetBindPose), with the count and skeleton AnimComponentV2's path gives it (0x10BC5A..0x10BD1C:
// ObjectZone::GetNumAnimatedSegments 0x23EC60, ObjectZone::GetSegmentList 0x2373D0) and those matrices.
// Its blend shapes: no AnimComponentV2 holds their weights, so a stand-in does, as large as one (0x4A0) with what
// UpdateRenderModelBlendShapes reads of it (all it reads: +8 the instance, +0x488 the weights, +0x490 the last frame's,
// +0x498 their count, AnimComponentV2::Init's count: the zone's shared +0x40); TickShapes writes and pushes it.
void* MakeFakeComponent(void* instance, int count) {
    if (count <= 0 || count > kMaxShapes) return nullptr;
    const std::size_t size = kFakeComponentSize + 2 * std::size_t(count) * sizeof(float);
    auto* block = static_cast<std::uint8_t*>(_aligned_malloc(size, 16));
    if (!block) return nullptr;
    std::memset(block, 0, size);
    *reinterpret_cast<void**>(block + kAnimInstance) = instance;
    *reinterpret_cast<std::uint8_t**>(block + kAnimWeights) = block + kFakeComponentSize;
    *reinterpret_cast<std::uint8_t**>(block + kAnimLastWeights) = block + kFakeComponentSize + std::size_t(count) * sizeof(float);
    *reinterpret_cast<std::int32_t*>(block + kAnimWeightCount) = count;
    return block;
}
bool FakeComponentOf(const void* fake, const void* instance) {
    return fake && *reinterpret_cast<void* const*>(static_cast<const std::uint8_t*>(fake) + kAnimInstance) == instance;
}
// The zone's blend shape count (AnimComponentV2::Init's), -1 on a fault.
int ReadZoneShapeCount(const std::uint8_t* zone) {
    __try {
        const std::uint8_t* shared = *reinterpret_cast<const std::uint8_t* const*>(zone + kZoneShared);
        return shared ? *reinterpret_cast<const std::uint16_t*>(shared + kSharedShapeCount) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// The instance's matrices, skeleton, the segments to pose and the animated ones among them (false: not readable, or
// one segment).
bool ReadRebuild(void* instance, float** matrices, const std::uint8_t** skeleton, std::uint32_t* count,
                 std::uint32_t* animated) {
    __try {
        const auto* bytes = static_cast<const std::uint8_t*>(instance);
        const std::uint8_t* transform = *reinterpret_cast<const std::uint8_t* const*>(bytes + kInstanceTransformComponent);
        const void* zone = *reinterpret_cast<const void* const*>(bytes + kInstanceObjectZone);
        if (!transform || !zone) return false;
        *matrices = *reinterpret_cast<float* const*>(transform + kTransformMatrices);
        *count = *animated = g_zoneAnimatedSegments(zone);
        // (v0.9.444) every segment, the ones after the animated ones too: a cloth's (the bones of no simulated piece:
        // Sofia's braids' roots, the hair at her nape on them, stayed in the bind pose behind her bent head), each from
        // its parent and its bind translation. (v0.9.512) The knots of the pieces the cloth simulates among them keep
        // the cloth's matrices (HookPostRebuildTransforms).
        if (g_zoneSegments) {
            const std::uint32_t all = g_zoneSegments(zone);
            if (all > *count && all <= kMaxSegments) *count = all;
        }
        *skeleton = g_zoneSegmentList(zone);
        return *matrices && *skeleton && *count > 1 && *count <= kMaxSegments;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool BuildPose(void* instance, std::uint32_t count, std::uint8_t* segs, const std::uint8_t* skeleton, float* matrices) {
    __try {
        for (std::uint32_t i = 0; i < count; ++i) {   // the bind pose
            float* f = reinterpret_cast<float*>(segs + std::size_t(i) * 0x40);
            const float* pivot = reinterpret_cast<const float*>(skeleton + std::size_t(i) * kSkeletonStride + kSkeletonPivot);
            f[0] = f[1] = f[2] = f[3] = 0.0f;
            f[4] = pivot[0], f[5] = pivot[1], f[6] = pivot[2], f[7] = pivot[3];
            f[8] = f[9] = f[10] = f[11] = 1.0f;
            f[12] = 1.0f;
        }
        g_buildTransformsEntry(instance, count, segs, skeleton, matrices);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
std::atomic<std::uint32_t> g_noComponentFaults{0};

// ---- The player's bones that nothing moves (v0.9.448) ----
// AnimComponentV2 builds the matrices of the skeleton's first GetNumAnimatedSegments bones; the ones after them are the
// cloth's, and only ClothComponent::SetMatrices writes them, for the pieces it simulates (ClothState +0xD8 bit 8, the
// component running: +0x100 bit 8). Every other one keeps the matrix G2Instance_RebuildTransforms gave it when the
// instance was born, in the world, where it was born. In the game's own skeletons every bone there is a knot of a piece
// its cloth simulates (laracroft: 185 animated of 205, the 20 after them the knots of its 5 pieces, all in its default
// config), but not in every character's: sofia_cine's braids (pieces 0 and 1, bones 122..135) are in none of her cloth's
// configs (config 0: group 0, piece 2, the skirt), so nothing moves them. And the Modding Suite's model swap gives Lara's
// skeleton the other character's cloth bones after her 185 animated ones, with the bones they hang from that she lacks
// (jacob_cine's 80, the coat's parent; sofia_cine's 119..121): those are not knots of any piece, so they stayed where
// the player was born, and with them the coat's and the skirt's frames and their pull to the pose (ClothComponent::
// Simulate: each knot's pose is its parent's matrix times its bind translation), and the braids: the cloth was drawn
// pulled back to where the player appeared. So the player's bones after the animated ones that no simulated piece owns
// are posed every frame as G2Instance_RebuildTransforms poses a bone (its parent's matrix, moved by its bind
// translation; parents come first), here after its animation's (before G2Instance_PostRebuildTransforms, which places
// what is attached to it, and before the frame's ClothComponentManager::Process, which reads them).
struct LooseResult {
    const void* zone = nullptr;
    std::uint32_t all = 0, animated = 0;
    int loose = 0, pieces = -1, simulated = 0;   // pieces -1: no cloth component
    bool running = false;
};
// (v0.9.512, PoseLooseSegments' before) The bones from animated to all that the knots of the pieces the instance's cloth
// simulates are on (owned: bit bone - animated), while the cloth runs. Unsafe: in the callers' SEH.
struct ClothOwned {
    int pieces = -1, simulated = 0, owned = 0;   // pieces -1: no cloth component; owned: the bits set
    bool running = false;
};
void ReadClothOwnedUnsafe(void* instance, std::uint32_t animated, std::uint32_t all, std::uint8_t* owned, ClothOwned& r) {
    const auto* cloth = static_cast<const std::uint8_t*>(g_tryToGetCloth(instance));
    if (!cloth) return;
    r.pieces = *reinterpret_cast<const std::int32_t*>(cloth + kClothPieceCount);
    r.running = (cloth[kClothFlags] & kClothRunning) != 0;
    const auto* states = *reinterpret_cast<const std::uint8_t* const* const*>(cloth + kClothStates);
    const auto* data = *reinterpret_cast<const std::uint8_t* const*>(cloth + kClothData);
    const std::uint8_t* pieces = data ? *reinterpret_cast<const std::uint8_t* const*>(data) : nullptr;
    for (int i = 0; r.running && states && pieces && i < r.pieces && i < 4096; ++i) {
        const std::uint8_t* state = states[i];
        if (!state || !(state[kClothStateFlags] & kClothStateSimulated)) continue;
        ++r.simulated;
        const auto* knots = *reinterpret_cast<const std::uint8_t* const*>(pieces + std::size_t(i) * kClothPieceStride);
        const std::int32_t count = *reinterpret_cast<const std::int32_t*>(state + kClothStateKnots);
        for (std::int32_t k = 0; knots && k < count && k < 8192; ++k) {
            const std::uint32_t bone =
                *reinterpret_cast<const std::uint16_t*>(knots + std::size_t(k) * kClothKnotStride + kClothKnotBone);
            if (bone < animated || bone >= all) continue;
            const std::uint8_t bit = std::uint8_t(1u << ((bone - animated) & 7));
            if (owned[(bone - animated) >> 3] & bit) continue;
            owned[(bone - animated) >> 3] |= bit;
            ++r.owned;
        }
    }
}
bool PoseLooseSegments(void* instance, LooseResult& r) {
    __try {
        const auto* bytes = static_cast<const std::uint8_t*>(instance);
        r.zone = *reinterpret_cast<const void* const*>(bytes + kInstanceObjectZone);
        const std::uint8_t* transform = *reinterpret_cast<const std::uint8_t* const*>(bytes + kInstanceTransformComponent);
        if (!r.zone || !transform) return true;
        r.animated = g_zoneAnimatedSegments(r.zone);
        r.all = g_zoneSegments(r.zone);
        if (r.all <= r.animated || r.all > kMaxSegments) return true;
        float* matrices = *reinterpret_cast<float* const*>(transform + kTransformMatrices);
        const std::uint8_t* skeleton = g_zoneSegmentList(r.zone);
        if (!matrices || !skeleton) return true;
        std::uint8_t owned[kMaxSegments / 8] = {};   // (bone - animated): a simulated piece's knot
        ClothOwned cloth;
        ReadClothOwnedUnsafe(instance, r.animated, r.all, owned, cloth);
        r.pieces = cloth.pieces;
        r.running = cloth.running;
        r.simulated = cloth.simulated;
        for (std::uint32_t s = r.animated; s < r.all; ++s) {
            if (owned[(s - r.animated) >> 3] & (1u << ((s - r.animated) & 7))) continue;
            const std::uint8_t* segment = skeleton + std::size_t(s) * kSkeletonStride;
            const std::int32_t parent = *reinterpret_cast<const std::int32_t*>(segment + kSkeletonParent);
            if (parent < 0 || std::uint32_t(parent) >= s) continue;
            const float* pivot = reinterpret_cast<const float*>(segment + kSkeletonPivot);
            const float* p = matrices + std::size_t(parent) * 16;
            float* m = matrices + std::size_t(s) * 16;
            for (int k = 0; k < 12; ++k) m[k] = p[k];   // its parent's axes (a bone's bind pose turns nothing)
            for (int c = 0; c < 4; ++c) m[12 + c] = p[12 + c] + pivot[0] * p[c] + pivot[1] * p[4 + c] + pivot[2] * p[8 + c];
            ++r.loose;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
std::atomic<std::uint32_t> g_looseFaults{0};
SRWLOCK g_looseLogLock = SRWLOCK_INIT;
LooseResult g_looseLogged;   // g_looseLogLock: what the log last said
int g_looseLines = 0;        // g_looseLogLock: lines for this object
void PoseLoosePlayer(void* instance) {
    LooseResult r;
    if (!PoseLooseSegments(instance, r)) {
        if (g_looseFaults.fetch_add(1) == 0) Log::Write("InstanceAnim: a fault posing the player's bones that nothing moves");
        return;
    }
    // the log: once the cloth runs (before, its pieces are not simulated yet), when what is posed changes
    if (r.pieces >= 0 && !r.running) return;
    AcquireSRWLockExclusive(&g_looseLogLock);
    if (r.zone != g_looseLogged.zone) {
        g_looseLines = 0;
        g_looseLogged = LooseResult{};
        g_looseLogged.zone = r.zone;
    }
    const bool changed = r.loose != g_looseLogged.loose || r.simulated != g_looseLogged.simulated || r.all != g_looseLogged.all;
    if (changed) g_looseLogged = r;
    const bool line = changed && r.loose > 0 && g_looseLines < 8;
    if (line) ++g_looseLines;
    ReleaseSRWLockExclusive(&g_looseLogLock);
    if (line)
        Log::Writef("InstanceAnim: the player's skeleton has %u bones, %u animated; %d after them follow their parent (no "
                    "simulated cloth piece moves them; its cloth: %s%d of %d pieces simulated)", r.all, r.animated, r.loose,
                    r.pieces < 0 ? "none, " : "", r.simulated, r.pieces < 0 ? 0 : r.pieces);
}

// ---- The cloth's knots in the pose of an instance without an AnimComponentV2 (v0.9.512) ----
// The pose built here covers every segment (v0.9.444), the cloth's knots too. AnimComponentV2's, on the game's own
// characters, covers the animated ones alone: the knots after them keep the cloth's matrices until
// ClothComponent::SetMatrices writes the frame's (TransformComponent::SwapMatrixPools, TR2 0x140133390, swaps its two
// pools without a copy: the cloth's of two frames before), and ClothComponent::Simulate reads them meanwhile: a knot's
// pose (where its group's pose strength pulls it) is its parent's matrix times its bind translation (TR2
// 0x14018A3CE..0x14018A426), so a chain's knot is pulled to where its parent knot hangs, not to where the bind pose
// holds it. So the knots after the animated bones that a simulated piece owns keep their matrices across the pose built
// here (copied before it, written back after). The knots among the animated bones (a piece on bones the clip may move:
// the Modding Suite's braids on Sofia's 96..118) are posed as AnimComponentV2 poses them, and the frame's SetMatrices
// writes the cloth's over them after (TR2's _ProcessModeRunning: G2Instance_BuildTransformsForList, then
// ClothComponentManager::Process, FinishJobs, SetMatrices; the framework's tick, which asks for the rebuild, comes before
// the game's): every simulated knot is drawn with the cloth's matrix (DriveCloth checks that for the log).
// Returns the knots kept (0: none, -1: a fault); kept: the matrices from animated to all.
int SaveClothKnots(void* instance, std::uint32_t animated, std::uint32_t all, const float* matrices, std::uint8_t* owned,
                   float* kept) {
    __try {
        ClothOwned r;
        ReadClothOwnedUnsafe(instance, animated, all, owned, r);
        if (r.owned == 0) return 0;
        std::memcpy(kept, matrices + std::size_t(animated) * 16, std::size_t(all - animated) * 16 * sizeof(float));
        return r.owned;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
bool RestoreClothKnots(std::uint32_t animated, std::uint32_t all, float* matrices, const std::uint8_t* owned,
                       const float* kept) {
    __try {
        for (std::uint32_t s = animated; s < all; ++s)
            if (owned[(s - animated) >> 3] & (1u << ((s - animated) & 7)))
                std::memcpy(matrices + std::size_t(s) * 16, kept + std::size_t(s - animated) * 16, 16 * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall HookPostRebuildTransforms(void* instance) {
    if (instance && g_looseSegments && instance == g_localPlayer.load(std::memory_order_acquire)) PoseLoosePlayer(instance);
    if (instance && g_noComponentPlayers.load(std::memory_order_acquire) > 0) {
        bool ours = false;
        AcquireSRWLockShared(&g_playerLock);
        for (int i = 0; i < g_playerCount && !ours; ++i) ours = g_players[i].noComponent && g_players[i].instance == instance;
        ReleaseSRWLockShared(&g_playerLock);
        float* matrices = nullptr;
        const std::uint8_t* skeleton = nullptr;
        std::uint32_t count = 0, animated = 0;
        if (ours && ReadRebuild(instance, &matrices, &skeleton, &count, &animated)) {
            // (v0.9.512) the pose's segments, then room for the matrices after the animated ones (the cloth's knots kept)
            const std::size_t after = g_clothDrive && count > animated ? std::size_t(count - animated) : 0;
            auto* segs = static_cast<std::uint8_t*>(_aligned_malloc((std::size_t(count) + after) * 0x40, 16));
            if (segs) {
                std::uint8_t owned[kMaxSegments / 8];
                int kept = 0;
                float* keep = reinterpret_cast<float*>(segs + std::size_t(count) * 0x40);
                if (after) {
                    std::memset(owned, 0, (after + 7) / 8);
                    kept = SaveClothKnots(instance, animated, count, matrices, owned, keep);
                }
                if (!BuildPose(instance, count, segs, skeleton, matrices) && g_noComponentFaults.fetch_add(1) == 0)
                    Log::Writef("InstanceAnim: a fault building the pose of an instance without an animation component");
                if ((kept < 0 || (kept > 0 && !RestoreClothKnots(animated, count, matrices, owned, keep))) &&
                    g_clothKeepFaults.fetch_add(1) == 0)
                    Log::Write("InstanceAnim: a fault keeping the cloth's knots out of the pose of an instance without an "
                               "animation component");
                _aligned_free(segs);
            }
        }
    }
    g_originalPostRebuild(instance);
}

// ---- The cloth of an instance without an AnimComponentV2 (v0.9.512) ----
// The Model Viewer's cutscene characters (sofia_cine...) have no AnimComponentV2 (the framework poses them, above), but
// nothing in the game's cloth needs one: ClothComponentManager::Process (TR2 0x1401886E0) calls ClothComponent::Simulate
// (retail 0x176450, TR2 0x140189EA0) for every component, the cloth reads and writes the TransformComponent's matrices
// (+0x70; SetMatrices, retail 0x175F50), and FullInit (TR2 0x140187240) reads the object's cloth data alone. What
// Simulate asks (kClothSimulate*, checked): FullInit done (+0x103; its job is started from here), a config (+0x30 not
// -1), the instance's matrices valid (TransformComponent +0x9E & 3: rebuilt every frame here); then the instance running,
// else nothing at all: ForceActivated (Instance +0xC8 bit 15), or processed (+0xC not 0: Instance::EvaluateProcessingMode
// -> DetermineBestProcessingMode, TR2 0x1403FFE40: deactivated farther from the camera than its TransformComponent's
// +0x18 / +0x1C when GameInstance::ShouldDeactivate agrees), or a cinematic actor (+0xC4 bit 11); and for the whole
// simulation (the knots under their weight, the wind, the collisions) the frame's pick (the component's flags bit 2:
// ClothComponentManager::Process ranks the components by AppendFocusPosition's distance from the active cameras,
// _ProcessCameras, against the tune's near and far (dtp::ClothTune +0x10 / +0x14; Sofia's 5000 / 20000 cm), an instance
// whose drawable was not drawn last frame out of reach) or a cinematic actor, else TransformBones alone (the pieces
// carried stiff with their parents). In the game these characters' cloth runs in their cutscenes, where each is a
// cinematic actor (Instance::EnableCinematicActor, retail 0x3D8C90, TR2 0x140403040): Simulate then runs the whole
// simulation every frame, whatever the cameras' distance and the instance's processing. So the cloth of a player's
// instance without an AnimComponentV2 is run that way: when it has a cloth component it is made a cinematic actor (unless
// it is one), as in its cutscenes, and that is undone (Instance::DisableCinematicActor, retail 0x3D64D0) when it stops
// playing, if the instance is still there. What else a cinematic actor changes for such an instance (TR2): it is kept
// processed (EvaluateProcessingMode answers 2), rebuilt every frame (G2Instance_NeedsBuildTransforms; KeepPoseBuilt asks
// that anyway), its object's process function is not called and it is not removed below the level's kill height
// (Instance::ProcessFunctions), and physics bodies do not move it (MultibodyClient::UpdateInstanceFromBody).
// The log: the cloth's pieces once it runs, then what its knots did, every 3 s while the model turns (the first window
// always): the most each simulated piece's knots moved in the model's frame, and whether the drawn matrices of the
// simulated knots are the cloth's (its knot states, ClothState +0x20) after the frame (the framework's tick, before the
// game's: SetMatrices was the frame's last writer).
constexpr int kMaxWatchedKnots = 512, kMaxWatchedPieces = 16, kMaxClothLines = 12;
constexpr ULONGLONG kClothWindowMs = 3000, kClothWaitMs = 3000;
constexpr float kClothTurnLogged = 0.2f;   // radians turned in a window that make it logged
struct ClothWatch {   // game thread only (TickGameThread)
    std::uint32_t instanceId = 0;   // 0: free
    std::uint64_t componentId = 0;
    void* instance = nullptr;
    bool cinematic = false;         // made a cinematic actor here (undone when it stops playing)
    int stage = 0;                  // 1 no cloth component, 2 waiting for it to run, 3 runs
    bool waitLogged = false;
    ULONGLONG since = 0, windowStart = 0;
    bool refSet = false;
    float heading = 0.0f, turned = 0.0f;   // the instance's heading at the window's start, the most it turned from it
    int pieces = 0, simulated = 0, knots = 0, after = 0;   // after: the watched knots after the animated bones
    std::uint32_t animated = 0;
    int pieceIndex[kMaxWatchedPieces]{};   // the simulated pieces watched
    float moved[kMaxWatchedPieces]{};      // the most a knot of each moved in the model's frame this window (cm)
    std::uint16_t bone[kMaxWatchedKnots]{};
    std::uint16_t knot[kMaxWatchedKnots]{};   // its knot state's index in its piece
    std::uint8_t piece[kMaxWatchedKnots]{};   // (in pieceIndex)
    float ref[kMaxWatchedKnots][3]{};         // where it was in the model's frame at the window's start
    int lines = 0;
    char name[64]{};
};
ClothWatch g_clothWatch[kMaxPlayers];
int g_clothWatchCount = 0;   // (game thread) the watches in use

struct ClothPlayer {   // (TickGameThread: the players without an AnimComponentV2, read under the lock)
    void* instance;
    std::uint32_t instanceId;
    std::uint64_t componentId;
};

// The instance's cinematic actor bit: 1 set, 0 not, -1 unreadable.
int CinematicActorBit(void* instance) {
    __try {
        return (*reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(instance) + kInstanceCineFlags) &
                kFlagCinematicActor) != 0 ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
bool SetCinematicActor(void* instance, bool on) {
    __try {
        (on ? g_enableCinematicActor : g_disableCinematicActor)(instance, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void* ClothOf(void* instance) {
    __try {
        return g_tryToGetCloth(instance);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
float WrapAngle(float a) {
    a = std::fmod(a, 6.2831853f);
    if (a > 3.1415927f) a -= 6.2831853f;
    if (a < -3.1415927f) a += 6.2831853f;
    return a;
}

// What Simulate reads, for the log when the cloth does not run.
void ClothGatesText(void* instance, const std::uint8_t* cloth, char* out, std::size_t size) {
    __try {
        const auto* b = static_cast<const std::uint8_t*>(instance);
        const auto* transform = *reinterpret_cast<const std::uint8_t* const*>(b + kInstanceTransformComponent);
        std::snprintf(out, size,
                      "initialized %u, config %d, flags 0x%02X; its matrices' flags 0x%04X; the instance: processing %d, "
                      "+0xC8 0x%08X, +0xC4 0x%08X",
                      cloth[kClothInitialized], *reinterpret_cast<const std::int32_t*>(cloth + kClothConfig), cloth[kClothFlags],
                      transform ? *reinterpret_cast<const std::uint16_t*>(transform + kTransformFlags) : 0xFFFFu,
                      *reinterpret_cast<const std::int32_t*>(b + kInstanceProcessingState),
                      *reinterpret_cast<const std::uint32_t*>(b + kInstanceFlags),
                      *reinterpret_cast<const std::uint32_t*>(b + kInstanceCineFlags));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::snprintf(out, size, "not readable");
    }
}

// The cloth's simulated pieces and their knots into the watch (false: not running, none simulated, or unreadable);
// text: each piece, its knots and their bones.
bool CollectClothKnots(void* instance, const std::uint8_t* cloth, ClothWatch& w, char* text, std::size_t size) {
    text[0] = '\0';
    __try {
        if (!(cloth[kClothFlags] & kClothRunning)) return false;
        const void* zone = *reinterpret_cast<const void* const*>(static_cast<const std::uint8_t*>(instance) + kInstanceObjectZone);
        w.animated = zone ? g_zoneAnimatedSegments(zone) : 0;
        w.pieces = *reinterpret_cast<const std::int32_t*>(cloth + kClothPieceCount);
        const auto* states = *reinterpret_cast<const std::uint8_t* const* const*>(cloth + kClothStates);
        const auto* data = *reinterpret_cast<const std::uint8_t* const*>(cloth + kClothData);
        const std::uint8_t* pieces = data ? *reinterpret_cast<const std::uint8_t* const*>(data) : nullptr;
        w.simulated = w.knots = w.after = 0;
        std::size_t used = 0;
        int watched = 0;
        for (int i = 0; states && pieces && i < w.pieces && i < 4096; ++i) {
            const std::uint8_t* state = states[i];
            if (!state || !(state[kClothStateFlags] & kClothStateSimulated)) continue;
            ++w.simulated;
            const auto* knots = *reinterpret_cast<const std::uint8_t* const*>(pieces + std::size_t(i) * kClothPieceStride);
            const std::int32_t count = *reinterpret_cast<const std::int32_t*>(state + kClothStateKnots);
            std::uint32_t low = 0xFFFF, high = 0;
            for (std::int32_t k = 0; knots && k < count && k < 8192; ++k) {
                const std::uint16_t b =
                    *reinterpret_cast<const std::uint16_t*>(knots + std::size_t(k) * kClothKnotStride + kClothKnotBone);
                low = std::min<std::uint32_t>(low, b);
                high = std::max<std::uint32_t>(high, b);
                if (watched >= kMaxWatchedPieces || w.knots >= kMaxWatchedKnots) continue;
                w.bone[w.knots] = b;
                w.knot[w.knots] = std::uint16_t(k);
                w.piece[w.knots] = std::uint8_t(watched);
                ++w.knots;
                if (b >= w.animated) ++w.after;
            }
            if (watched < kMaxWatchedPieces) w.pieceIndex[watched++] = i;
            if (used + 1 < size) {
                const int n = std::snprintf(text + used, size - used, "%s%d (%d knots, bones %u..%u)", used ? ", " : "", i,
                                            int(count), low, high);
                if (n > 0) used = std::min(size - 1, used + std::size_t(n));
            }
        }
        return w.simulated > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// One sample of the watched knots: where each is in the model's frame (the TransformComponent's root matrix, the one
// before its bones'), against the window's start; drawn: those whose drawn matrix is their knot state's (checked: those
// read). -1: unreadable, 0: the cloth's pieces changed (collect them again), 1: sampled.
int SampleClothKnots(void* instance, const std::uint8_t* cloth, ClothWatch& w, int& drawn, int& checked) {
    drawn = checked = 0;
    __try {
        if (!(cloth[kClothFlags] & kClothRunning) || *reinterpret_cast<const std::int32_t*>(cloth + kClothPieceCount) != w.pieces)
            return 0;
        const auto* b = static_cast<const std::uint8_t*>(instance);
        const auto* transform = *reinterpret_cast<const std::uint8_t* const*>(b + kInstanceTransformComponent);
        const float* matrices = transform ? *reinterpret_cast<float* const*>(transform + kTransformMatrices) : nullptr;
        const auto* states = *reinterpret_cast<const std::uint8_t* const* const*>(cloth + kClothStates);
        if (!matrices || !states) return -1;
        const float* root = matrices - 16;
        float axisLength[3];
        for (int a = 0; a < 3; ++a) {
            const float* axis = root + a * 4;
            axisLength[a] = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
            if (axisLength[a] < 1e-6f) return -1;
        }
        const float heading = *reinterpret_cast<const float*>(b + kInstanceRotation + 8);
        for (int k = 0; k < w.knots; ++k) {
            const int i = w.pieceIndex[w.piece[k]];
            const std::uint8_t* state = states[i];
            if (!state || !(state[kClothStateFlags] & kClothStateSimulated)) return 0;
            const auto* knotStates = *reinterpret_cast<const float* const*>(state + kClothStateKnotStates);
            const float* m = matrices + std::size_t(w.bone[k]) * 16;
            if (knotStates) {
                ++checked;
                if (std::memcmp(m, knotStates + std::size_t(w.knot[k]) * (kClothKnotStateStride / sizeof(float)),
                                16 * sizeof(float)) == 0)
                    ++drawn;
            }
            float local[3];
            for (int a = 0; a < 3; ++a) {
                const float* axis = root + a * 4;
                local[a] = ((m[12] - root[12]) * axis[0] + (m[13] - root[13]) * axis[1] + (m[14] - root[14]) * axis[2]) /
                           axisLength[a];
            }
            if (!w.refSet) {
                for (int a = 0; a < 3; ++a) w.ref[k][a] = local[a];
            } else {
                const float dx = local[0] - w.ref[k][0], dy = local[1] - w.ref[k][1], dz = local[2] - w.ref[k][2];
                w.moved[w.piece[k]] = std::max(w.moved[w.piece[k]], std::sqrt(dx * dx + dy * dy + dz * dz));
            }
        }
        if (!w.refSet) {
            w.heading = heading;
            w.turned = 0.0f;
            for (float& v : w.moved) v = 0.0f;
            w.refSet = true;
        } else {
            w.turned = std::max(w.turned, std::fabs(WrapAngle(heading - w.heading)));
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

ClothWatch* FindClothWatch(std::uint32_t instanceId, std::uint64_t componentId) {
    for (ClothWatch& w : g_clothWatch)
        if (w.instanceId == instanceId && w.componentId == componentId) return &w;
    return nullptr;
}

// One player's cloth this tick (game thread, no lock held: EnableCinematicActor is the game's).
void DriveCloth(const ClothPlayer& p, ULONGLONG now) {
    ClothWatch* w = FindClothWatch(p.instanceId, p.componentId);
    if (!w) {
        for (ClothWatch& free : g_clothWatch)
            if (!free.instanceId) {
                w = &free;
                break;
            }
        if (!w) return;
        *w = ClothWatch{};
        w->instanceId = p.instanceId;
        w->componentId = p.componentId;
        w->instance = p.instance;
        w->since = now;
        CopyText(w->name, sizeof(w->name), Features::InstanceObjectName(p.instance).c_str());
        ++g_clothWatchCount;
    }
    if (w->stage == 1) return;
    const auto* cloth = static_cast<const std::uint8_t*>(ClothOf(p.instance));
    if (!cloth) {
        if (w->stage == 0) Log::Detailf("InstanceAnim: instance %u (%s) has no cloth component", p.instanceId, w->name);
        w->stage = 1;
        return;
    }
    if (w->stage == 0) {
        const int bit = CinematicActorBit(p.instance);
        if (bit == 0 && SetCinematicActor(p.instance, true) && CinematicActorBit(p.instance) == 1) w->cinematic = true;
        Log::Writef("InstanceAnim: instance %u (%s), posed without an animation component, has a cloth component: %s, so "
                    "the game simulates its cloth every frame, as in its cutscenes",
                    p.instanceId, w->name,
                    w->cinematic ? "made a cinematic actor" : bit == 1 ? "already a cinematic actor"
                                                                       : "it could not be made a cinematic actor (its cloth "
                                                                         "runs only as far as the game lets it)");
        w->stage = 2;
        w->since = now;
    }
    char text[512];
    if (w->stage == 2) {
        if (CollectClothKnots(p.instance, cloth, *w, text, sizeof(text))) {
            w->stage = 3;
            w->refSet = false;
            w->windowStart = now;
            if (w->lines++ < kMaxClothLines)
                Log::Writef("InstanceAnim: instance %u (%s)'s cloth runs: %d of its %d pieces simulated: %s; its %d knots "
                            "after the %u animated bones keep the cloth's matrices through the pose%s",
                            p.instanceId, w->name, w->simulated, w->pieces, text, w->after, w->animated,
                            w->knots >= kMaxWatchedKnots ? " (the log watches the first 512 knots)" : "");
        } else if (!w->waitLogged && now - w->since > kClothWaitMs) {
            w->waitLogged = true;
            ClothGatesText(p.instance, cloth, text, sizeof(text));
            Log::Writef("InstanceAnim: instance %u (%s)'s cloth does not run %.0f s on (%s)", p.instanceId, w->name,
                        kClothWaitMs / 1000.0, text);
        }
        return;
    }
    int drawn = 0, checked = 0;
    const int sampled = SampleClothKnots(p.instance, cloth, *w, drawn, checked);
    if (sampled == 0) {   // (it stopped, was reset, or its config changed which pieces it simulates)
        w->stage = 2;
        w->since = now;
        w->waitLogged = false;
        return;
    }
    if (sampled < 0 || now - w->windowStart < kClothWindowMs) return;
    const bool first = w->lines <= 1;
    if (w->lines < kMaxClothLines && (first || w->turned >= kClothTurnLogged)) {
        ++w->lines;
        std::size_t used = 0;
        text[0] = '\0';
        const int watched = w->knots ? w->piece[w->knots - 1] + 1 : 0;
        for (int i = 0; i < watched && used + 1 < sizeof(text); ++i) {
            const int n = std::snprintf(text + used, sizeof(text) - used, "%spiece %d %.1f cm", i ? ", " : "", w->pieceIndex[i],
                                        w->moved[i]);
            if (n > 0) used = std::min(sizeof(text) - 1, used + std::size_t(n));
        }
        Log::Writef("InstanceAnim: instance %u (%s)'s cloth over %.1f s (the model turned %.0f deg): its knots moved in the "
                    "model's frame up to %s; %d of %d simulated knots drawn with the cloth's matrices",
                    p.instanceId, w->name, (now - w->windowStart) / 1000.0, w->turned * 57.29578f, text, drawn, checked);
    }
    w->refSet = false;   // (the next window from the next sample)
    w->windowStart = now;
}

// The watches of players that are gone: the cinematic actor undone where it was made one here (game thread, no lock).
void ReleaseClothWatches(const ClothPlayer* players, int count) {
    for (ClothWatch& w : g_clothWatch) {
        if (!w.instanceId) continue;
        bool playing = false;
        for (int i = 0; i < count && !playing; ++i)
            playing = players[i].instanceId == w.instanceId && players[i].componentId == w.componentId;
        if (playing) continue;
        if (w.cinematic) {
            void* instance = Features::InstanceNowGameThread(w.instanceId, w.componentId);
            if (instance == w.instance && CinematicActorBit(instance) == 1 && SetCinematicActor(instance, false))
                Log::Detailf("InstanceAnim: instance %u (%s) stopped playing: no longer a cinematic actor", w.instanceId, w.name);
        }
        w = ClothWatch{};
        --g_clothWatchCount;
    }
}

// Game thread, exclusive lock: the blend shapes of a player the game does not push this frame are pushed here (its
// hook writes them first; our stand-in's always); the way it goes logged when it changes.
void TickShapes(Player& p) {
    if (p.shapeSlots == 0 || p.shapeFrames == 0) return;
    const int pushes = p.fakeComponent ? 0 : GamePushesShapes(p.instance);
    if (pushes < 0) return;
    const int way = pushes ? 1 : 2;
    if (way == 2) {
        WriteShapes(p);
        PushShapes(p.animComponent);
    }
    if (way != p.shapeWay) {
        Log::Detailf("InstanceAnim: instance %u's %d blend shapes %s", p.instanceId, p.shapeSlots,
                     way == 1 ? "go to its render model with the game's (it processes the instance)"
                     : p.fakeComponent ? "are handed to its render model by the framework (it has no animation component)"
                                       : "are handed to its render model by the framework (the game does not process the instance)");
        p.shapeWay = way;
    }
}
}  // namespace

namespace InstanceAnim {
bool CreateHooks() {
    g_base = GameBuild::Base();
    if (!g_base) {
        g_unavailable = "game module not found";
        return false;
    }
    struct Check {
        std::uintptr_t rva;
        const std::uint8_t* bytes;
        std::size_t size;
        const char* name;
    };
    const Check checks[] = {
        {kRvaBuildTransforms, kBuildTransformsSig, sizeof(kBuildTransformsSig), "cdc::AnimBuildTransforms"},
        {kRvaDecompress, kDecompressSig, sizeof(kDecompressSig), "DecompressFrameWithBoneSet"},
        {kRvaGetBoneIndexFromId, kGetBoneIndexFromIdSig, sizeof(kGetBoneIndexFromIdSig), "ObjectZone::GetBoneIndexFromID"},
        {kRvaForceRebuildTransforms, kForceRebuildTransformsSig, sizeof(kForceRebuildTransformsSig),
         "TransformComponent::EnableForceRebuildTransforms"},
        {kRvaAnimComponentInit, kAnimComponentInitSig, sizeof(kAnimComponentInitSig), "AnimComponentV2::Init"},
        {kRvaAnimComponentInitLod, kAnimComponentInitLodSig, sizeof(kAnimComponentInitLodSig), "AnimComponentV2::Init LOD"},
        {kRvaGetAnimData, kGetAnimDataSig, sizeof(kGetAnimDataSig), "AnimLibraryManager::GetKeylist"},
        {kRvaAnimByNumberRef, kAnimByNumberRefSig, sizeof(kAnimByNumberRefSig), "animByNumber table"},
        {kRvaGetAnimComponent, kGetAnimComponentSig, sizeof(kGetAnimComponentSig), "Instance::GetAnimComponentV2"},
        {kRvaDecompressFrameCtx, kDecompressFrameCtxSig, sizeof(kDecompressFrameCtxSig), "DecompressFrame context"},
        {kRvaDecompressFrameTime, kDecompressFrameTimeSig, sizeof(kDecompressFrameTimeSig), "DecompressFrame time"},
        {kRvaDecompressFrameKeys, kDecompressFrameKeysSig, sizeof(kDecompressFrameKeysSig), "DecompressFrame keys"},
    };
    for (const Check& c : checks) {
        if (!GameBuild::VerifyBytes(c.rva, c.bytes, c.size)) {
            Log::Writef("InstanceAnim: %s signature mismatch; the Animation tab is unavailable", c.name);
            g_unavailable = "game build signature mismatch (see the log)";
            return false;
        }
    }
    g_decompress = reinterpret_cast<DecompressFn>(g_base + kRvaDecompress);
    g_getBoneIndex = reinterpret_cast<GetBoneIndexFn>(g_base + kRvaGetBoneIndexFromId);
    g_forceRebuild = reinterpret_cast<ForceRebuildFn>(g_base + kRvaForceRebuildTransforms);
    g_getAnimComponent = reinterpret_cast<GetAnimComponentFn>(g_base + kRvaGetAnimComponent);
    g_registry = reinterpret_cast<const std::uint8_t* const*>(g_base + kRvaRegistry);
    g_animByNumber = reinterpret_cast<const std::uint8_t* const*>(g_base + kRvaAnimByNumber);
    for (float& w : g_weights) w = 1.0f;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    g_qpcFrequency = frequency.QuadPart ? frequency.QuadPart : 1;

    void* target = reinterpret_cast<void*>(g_base + kRvaBuildTransforms);
    const MH_STATUS created = MH_CreateHook(target, reinterpret_cast<void*>(&HookBuildTransforms),
                                            reinterpret_cast<void**>(&g_originalBuildTransforms));
    if (created == MH_ERROR_ALREADY_CREATED) {
        Log::Detail("InstanceAnim: AnimBuildTransforms is hooked by CombatAir, which hands the poses on");
    } else if (created != MH_OK) {
        Log::Writef("InstanceAnim: could not hook AnimBuildTransforms (status %d); the Animation tab is unavailable",
                    static_cast<int>(created));
        g_unavailable = "bone transform hook could not be created";
        return false;
    } else {
        Log::Detail("InstanceAnim: AnimBuildTransforms hook created");
    }
    g_available = true;
    g_unavailable = "";

    // blend shapes (layers that set them): without these the clips play on their bones alone
    const Check shapeChecks[] = {
        {kRvaUpdateBlendShapes, kUpdateBlendShapesSig, sizeof(kUpdateBlendShapesSig), "AnimComponentV2::UpdateRenderModelBlendShapes"},
        {kRvaBlendShapeManagerFlags, kBlendShapeManagerFlagsSig, sizeof(kBlendShapeManagerFlagsSig),
         "AnimatedComponentManager::UpdateRenderModelBlendShapes flags"},
        {kRvaBlendShapeManagerState, kBlendShapeManagerStateSig, sizeof(kBlendShapeManagerStateSig),
         "AnimatedComponentManager::UpdateRenderModelBlendShapes state"},
        {kRvaSetBlendshapeWeight, kSetBlendshapeWeightSig, sizeof(kSetBlendshapeWeightSig), "AnimComponentV2::SetBlendshapeWeight"},
        {kRvaInitBlendShapeCount, kInitBlendShapeCountSig, sizeof(kInitBlendShapeCountSig), "AnimComponentV2::Init blend shapes"},
        {kRvaGetBShapeIndexFromId, kGetBShapeIndexFromIdSig, sizeof(kGetBShapeIndexFromIdSig), "ObjectZone::GetBShapeIndexFromID"},
        {kRvaGenerateIdMapsShapes, kGenerateIdMapsShapesSig, sizeof(kGenerateIdMapsShapesSig), "GenerateIDMaps blend shapes"},
        {kRvaDecompressShapeLoop, kDecompressShapeLoopSig, sizeof(kDecompressShapeLoopSig), "DecompressFrame blend shape buffer"},
        {kRvaDecompressShapeSkip, kDecompressShapeSkipSig, sizeof(kDecompressShapeSkipSig), "DecompressFrame blend shape map"},
        {kRvaDecompressShapeStore, kDecompressShapeStoreSig, sizeof(kDecompressShapeStoreSig), "DecompressFrame blend shape store"},
        {kRvaDecompressShapeCount, kDecompressShapeCountSig, sizeof(kDecompressShapeCountSig), "DecompressFrame blend shape count"},
        {kRvaDecompressWrapFlag, kDecompressWrapFlagSig, sizeof(kDecompressWrapFlagSig), "DecompressFrame quat log wrap"},
        {kRvaDecompressAddFlag, kDecompressAddFlagSig, sizeof(kDecompressAddFlagSig), "DecompressFrame add flag"},
        {kRvaDecompressAdd, kDecompressAddSig, sizeof(kDecompressAddSig), "DecompressFrame add"},
        {kRvaDecompressWrite, kDecompressWriteSig, sizeof(kDecompressWriteSig), "DecompressFrame write"},
        {kRvaInitShapeCountZone, kInitShapeCountZoneSig, sizeof(kInitShapeCountZoneSig), "AnimComponentV2::Init blend shape count"},
        {kRvaUpdateBlendShapesLast, kUpdateBlendShapesLastSig, sizeof(kUpdateBlendShapesLastSig),
         "UpdateRenderModelBlendShapes last weights"},
        {kRvaUpdateBlendShapesInstance, kUpdateBlendShapesInstanceSig, sizeof(kUpdateBlendShapesInstanceSig),
         "UpdateRenderModelBlendShapes instance"},
    };
    bool shapesOk = true;
    for (const Check& c : shapeChecks) {
        if (GameBuild::VerifyBytes(c.rva, c.bytes, c.size)) continue;
        Log::Writef("InstanceAnim: %s signature mismatch; clips set no blend shapes (the face) and play no fidgets", c.name);
        shapesOk = false;
        break;
    }
    g_fidgetsAvailable = shapesOk;
    if (shapesOk) {
        g_getBShapeIndex = reinterpret_cast<GetBShapeIndexFn>(g_base + kRvaGetBShapeIndexFromId);
        const MH_STATUS hooked = MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaUpdateBlendShapes),
                                               reinterpret_cast<void*>(&HookUpdateBlendShapes),
                                               reinterpret_cast<void**>(&g_originalUpdateBlendShapes));
        if (hooked == MH_OK) {
            g_shapesAvailable = true;
            Log::Detail("InstanceAnim: UpdateRenderModelBlendShapes hook created");
        } else {
            Log::Writef("InstanceAnim: could not hook UpdateRenderModelBlendShapes (status %d); clips set no blend shapes",
                        static_cast<int>(hooked));
        }
    }

    // (v0.9.436) instances without an AnimComponentV2: their pose built after the game's bind pose
    const Check poseChecks[] = {
        {kRvaPostRebuildTransforms, kPostRebuildTransformsSig, sizeof(kPostRebuildTransformsSig), "G2Instance_PostRebuildTransforms"},
        {kRvaRebuildTransforms, kRebuildTransformsSig, sizeof(kRebuildTransformsSig), "G2Instance_RebuildTransforms"},
        {kRvaRebuildTransformsPool, kRebuildTransformsPoolSig, sizeof(kRebuildTransformsPoolSig), "G2Instance_RebuildTransforms matrices"},
        {kRvaRebuildTransformsAnim, kRebuildTransformsAnimSig, sizeof(kRebuildTransformsAnimSig), "G2Instance_RebuildTransforms anim path"},
        {kRvaRebuildTransformsPost, kRebuildTransformsPostSig, sizeof(kRebuildTransformsPostSig), "G2Instance_RebuildTransforms end"},
        {kRvaZoneAnimatedSegments, kZoneAnimatedSegmentsSig, sizeof(kZoneAnimatedSegmentsSig), "ObjectZone::GetNumAnimatedSegments"},
        {kRvaZoneSegmentList, kZoneSegmentListSig, sizeof(kZoneSegmentListSig), "ObjectZone::GetSegmentList"},
        {kRvaAnimBuildCount, kAnimBuildCountSig, sizeof(kAnimBuildCountSig), "AnimComponentV2 pose count"},
        {kRvaAnimBuildSkeleton, kAnimBuildSkeletonSig, sizeof(kAnimBuildSkeletonSig), "AnimComponentV2 pose skeleton"},
        {kRvaAnimBuildCall, kAnimBuildCallSig, sizeof(kAnimBuildCallSig), "AnimComponentV2 AnimBuildTransforms call"},
    };
    bool posesOk = true;
    for (const Check& c : poseChecks) {
        if (GameBuild::VerifyBytes(c.rva, c.bytes, c.size)) continue;
        Log::Writef("InstanceAnim: %s signature mismatch; instances without an animation component play nothing", c.name);
        posesOk = false;
        break;
    }
    if (posesOk) {
        g_zoneAnimatedSegments = reinterpret_cast<ZoneCountFn>(g_base + kRvaZoneAnimatedSegments);
        g_zoneSegmentList = reinterpret_cast<ZoneSegmentsFn>(g_base + kRvaZoneSegmentList);
        if (GameBuild::VerifyBytes(kRvaZoneSegments + kZoneSegmentsTailAt, kZoneSegmentsTailSig, sizeof(kZoneSegmentsTailSig)))
            g_zoneSegments = reinterpret_cast<ZoneAllSegmentsFn>(g_base + kRvaZoneSegments);
        else
            Log::Write("InstanceAnim: ObjectZone::NumSegments signature mismatch; the poses built for instances without an "
                       "animation component leave their cloth's segments in the bind pose");
        g_buildTransformsEntry = reinterpret_cast<BuildTransformsFn>(g_base + kRvaBuildTransforms);
        const MH_STATUS hooked = MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaPostRebuildTransforms),
                                               reinterpret_cast<void*>(&HookPostRebuildTransforms),
                                               reinterpret_cast<void**>(&g_originalPostRebuild));
        if (hooked == MH_OK) {
            g_posesWithoutComponent = true;
            Log::Detail("InstanceAnim: PostRebuildTransforms hook created");
            // (v0.9.448) the player's bones that nothing moves
            const Check looseChecks[] = {
                {kRvaGetLocalPlayerInstance, kGetLocalPlayerInstanceSig, sizeof(kGetLocalPlayerInstanceSig), "GetLocalPlayerInstance"},
                {kRvaTryToGetClothComponent, kTryToGetClothSig, sizeof(kTryToGetClothSig), "cdc::TryToGetClothComponent"},
                {kRvaClothSetMatrices + kClothSetMatricesRunAt, kClothSetMatricesRunSig, sizeof(kClothSetMatricesRunSig),
                 "ClothComponent::SetMatrices running flag"},
                {kRvaClothSetMatrices + kClothSetMatricesPiecesAt, kClothSetMatricesPiecesSig, sizeof(kClothSetMatricesPiecesSig),
                 "ClothComponent::SetMatrices pieces"},
                {kRvaClothSetMatrices + kClothSetMatricesStateAt, kClothSetMatricesStateSig, sizeof(kClothSetMatricesStateSig),
                 "ClothComponent::SetMatrices piece state"},
                {kRvaClothSetMatrices + kClothSetMatricesKnotAt, kClothSetMatricesKnotSig, sizeof(kClothSetMatricesKnotSig),
                 "ClothComponent::SetMatrices knot bone"},
                {kRvaClothSetMatrices + kClothSetMatricesNextAt, kClothSetMatricesNextSig, sizeof(kClothSetMatricesNextSig),
                 "ClothComponent::SetMatrices next piece"},
            };
            const char* mismatch = g_zoneSegments ? nullptr : "ObjectZone::NumSegments";
            for (const Check& c : looseChecks)
                if (!mismatch && !GameBuild::VerifyBytes(c.rva, c.bytes, c.size)) mismatch = c.name;
            if (mismatch) {
                Log::Writef("InstanceAnim: %s signature mismatch; the player's bones that neither its animation nor its "
                            "cloth moves stay where it was born", mismatch);
            } else {
                g_getLocalPlayerInstance = reinterpret_cast<GetLocalPlayerInstanceFn>(g_base + kRvaGetLocalPlayerInstance);
                g_tryToGetCloth = reinterpret_cast<TryToGetClothFn>(g_base + kRvaTryToGetClothComponent);
                g_looseSegments = true;
                // (v0.9.512) the cloth of instances without an AnimComponentV2 (see DriveCloth)
                const Check driveChecks[] = {
                    {kRvaClothSetMatrices + kClothSetMatricesStatesAt, kClothSetMatricesStatesSig,
                     sizeof(kClothSetMatricesStatesSig), "ClothComponent::SetMatrices knot states"},
                    {kRvaClothSimulate, kClothSimulateSig, sizeof(kClothSimulateSig), "ClothComponent::Simulate"},
                    {kRvaClothSimulate + kClothSimulateConfigAt, kClothSimulateConfigSig, sizeof(kClothSimulateConfigSig),
                     "ClothComponent::Simulate config"},
                    {kRvaClothSimulate + kClothSimulateMatricesAt, kClothSimulateMatricesSig, sizeof(kClothSimulateMatricesSig),
                     "ClothComponent::Simulate matrices"},
                    {kRvaClothSimulate + kClothSimulateActiveAt, kClothSimulateActiveSig, sizeof(kClothSimulateActiveSig),
                     "ClothComponent::Simulate instance running"},
                    {kRvaClothSimulate + kClothSimulateFullAt, kClothSimulateFullSig, sizeof(kClothSimulateFullSig),
                     "ClothComponent::Simulate whole simulation"},
                    {kRvaEnableCinematicActor, kEnableCinematicActorSig, sizeof(kEnableCinematicActorSig),
                     "Instance::EnableCinematicActor"},
                    {kRvaEnableCinematicActor + kEnableCinematicActorSetAt, kEnableCinematicActorSetSig,
                     sizeof(kEnableCinematicActorSetSig), "Instance::EnableCinematicActor flag"},
                    {kRvaDisableCinematicActor, kDisableCinematicActorSig, sizeof(kDisableCinematicActorSig),
                     "Instance::DisableCinematicActor"},
                    {kRvaDisableCinematicActor + kDisableCinematicActorClearAt, kDisableCinematicActorClearSig,
                     sizeof(kDisableCinematicActorClearSig), "Instance::DisableCinematicActor flag"},
                };
                const char* driveMismatch = nullptr;
                for (const Check& c : driveChecks)
                    if (!driveMismatch && !GameBuild::VerifyBytes(c.rva, c.bytes, c.size)) driveMismatch = c.name;
                if (driveMismatch) {
                    Log::Writef("InstanceAnim: %s signature mismatch; the cloth of instances without an animation component "
                                "(the Model Viewer's cutscene characters) runs only as far as the game lets it", driveMismatch);
                } else {
                    g_enableCinematicActor = reinterpret_cast<CinematicActorFn>(g_base + kRvaEnableCinematicActor);
                    g_disableCinematicActor = reinterpret_cast<CinematicActorFn>(g_base + kRvaDisableCinematicActor);
                    g_clothDrive = true;
                    Log::Detail("InstanceAnim: the cloth of instances without an animation component is simulated as in "
                                "their cutscenes");
                }
            }
        } else {
            Log::Writef("InstanceAnim: could not hook PostRebuildTransforms (status %d); instances without an animation "
                        "component play nothing", static_cast<int>(hooked));
        }
    }
    return true;
}

bool Available() { return g_available; }
const char* UnavailableReason() { return g_unavailable; }

void KeepPoseBuilt(void* instance) {
    if (instance && g_forceRebuild) ForceRebuild(instance);
}

void OnBuildTransforms(void* instance, std::uint32_t count, std::uint8_t* segs, const std::uint8_t* skeleton) {
    ModelViewer::OnBuildTransforms(instance, count, segs, skeleton);   // the Model Viewer's model: its bind pose
    if (g_playerCountFast.load(std::memory_order_acquire) == 0 || !instance || !segs || !skeleton || count == 0 ||
        count > kMaxSegments)
        return;
    AcquireSRWLockShared(&g_playerLock);
    for (int i = 0; i < g_playerCount; ++i) {
        if (g_players[i].instance != instance) continue;
        SamplePlayer(g_players[i], count, segs, skeleton);
        break;
    }
    ReleaseSRWLockShared(&g_playerLock);
}

void TickGameThread() {
    if (!g_available) return;
    // (v0.9.448) the instance whose loose bones PostRebuildTransforms poses
    if (g_looseSegments) g_localPlayer.store(g_getLocalPlayerInstance(0), std::memory_order_release);
    PlayRequest play;
    ListRequest list;
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        play = g_playRequest;
        g_playRequest.pending = false;
        list = g_listRequest;
        g_listRequest.pending = false;
    }
    if (list.pending) ProcessListRequest(list);
    if (play.pending) ProcessPlayRequest(play);

    if (g_playerCountFast.load(std::memory_order_acquire) == 0 && g_shapeClearCount == 0 && g_clothWatchCount == 0) return;
    ClothPlayer clothPlayers[kMaxPlayers];   // (v0.9.512) the players without an AnimComponentV2, for DriveCloth
    int clothPlayerCount = 0;
    AcquireSRWLockExclusive(&g_playerLock);
    for (int i = 0; i < g_shapeClearCount; ++i) {   // (see ShapeClear)
        const ShapeClear& c = g_shapeClears[i];
        void* instance = Features::InstanceNowGameThread(c.instanceId, c.componentId);
        if (instance && (c.fake ? FakeComponentOf(c.animComponent, instance) : SameAnimComponent(instance, c.animComponent)))
            PushShapes(c.animComponent);
        if (c.fake) _aligned_free(c.animComponent);
    }
    g_shapeClearCount = 0;
    const std::int64_t tickNow = QpcNow();
    for (int i = 0; i < g_playerCount;) {
        Player& p = g_players[i];
        void* now = Features::InstanceNowGameThread(p.instanceId, p.componentId);
        const char* reason = nullptr;
        if (now != p.instance || ReadZone(p.instance) != p.zone)
            reason = "the Instance went away";
        else if (!ClipLive(p.clip, p.clipNum))
            reason = "its animation was unloaded";
        for (int k = 0; k < p.extraCount && !reason; ++k)
            if (!ClipLive(p.extra[k].clip, p.extra[k].clipNum)) reason = "one of its layers' animations was unloaded";
        for (int k = 0; k < p.chainCount && !reason; ++k)
            if (!ClipLive(p.chain[k].clip, p.chain[k].clipNum)) reason = "one of its sequence's animations was unloaded";
        for (int k = 0; k < p.partCount && !reason; ++k)
            if (!ClipLive(p.parts[k].clip, p.parts[k].clipNum)) reason = "one of its fidgets' animations was unloaded";
        if (reason) {
            Log::Detailf("InstanceAnim: instance %u stopped playing %s (%s)", p.instanceId, p.name, reason);
            SetText(g_playStatus, sizeof(g_playStatus), "Stopped %s: %s.", p.name, reason);
            RemovePlayerLocked(i);
            continue;
        }
        ForceRebuild(p.instance);
        ScheduleFidget(p, tickNow);
        TickShapes(p);
        if (!p.warnedNoPose && p.samples == 0 && GetTickCount64() - p.startTick > 2000) {
            p.warnedNoPose = true;
            Log::Writef("InstanceAnim: instance %u had no pose update in 2 s while playing %s", p.instanceId, p.name);
            SetText(g_playStatus, sizeof(g_playStatus),
                    "%s: the game builds no pose for this Instance (not animated right now, or too far away).", p.name);
        }
        if (g_clothDrive && p.noComponent) clothPlayers[clothPlayerCount++] = {p.instance, p.instanceId, p.componentId};
        ++i;
    }
    ReleaseSRWLockExclusive(&g_playerLock);
    // (v0.9.512) their cloth, without the lock (the game's EnableCinematicActor; see DriveCloth)
    if (g_clothDrive) {
        const ULONGLONG nowMs = GetTickCount64();
        for (int i = 0; i < clothPlayerCount; ++i) DriveCloth(clothPlayers[i], nowMs);
        ReleaseClothWatches(clothPlayers, clothPlayerCount);
    }
}

void RequestList(std::uint32_t instanceId, std::uint64_t componentId, bool allLibraries) {
    std::lock_guard<std::mutex> lock(g_requestMutex);
    g_listRequest = {true, instanceId, componentId, allLibraries};
}

std::uint64_t ListVersion() { return g_listVersion.load(std::memory_order_acquire); }

bool GetListInfo(std::uint32_t instanceId, std::uint64_t componentId, int& entries, int& libraries, bool& allLibraries) {
    std::lock_guard<std::mutex> lock(g_listMutex);
    if (!g_listValid || g_listInstanceId != instanceId || g_listComponentId != componentId) return false;
    entries = static_cast<int>(g_list.size());
    libraries = g_listLibraries;
    allLibraries = g_listAll;
    return true;
}

const char* ListStatus() { return g_listStatus; }

int FindAnimations(const char* queryText, AnimListEntry* out, int capacity) {
    if (!out || capacity <= 0) return 0;
    const std::string query = Lower(queryText);
    std::lock_guard<std::mutex> lock(g_listMutex);
    struct Candidate {
        int score;
        int index;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(256);
    const bool numeric = !query.empty() && std::all_of(query.begin(), query.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
    for (int i = 0; i < static_cast<int>(g_list.size()); ++i) {
        const AnimListEntry& e = g_list[static_cast<std::size_t>(i)];
        int score = 0;
        if (query.empty()) {
            score = 0;
        } else if (numeric) {
            const std::string id = std::to_string(static_cast<int>(e.animId));
            if (id == query) score = 0;
            else if (id.rfind(query, 0) == 0) score = 100 + static_cast<int>(id.size() - query.size());
            else score = std::numeric_limits<int>::max();
        } else {
            score = Score(Lower(e.name), query);
        }
        if (score != std::numeric_limits<int>::max()) candidates.push_back({score, i});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [&](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) return a.score < b.score;
        return g_list[static_cast<std::size_t>(a.index)].animId < g_list[static_cast<std::size_t>(b.index)].animId;
    });
    const int count = std::min(capacity, static_cast<int>(candidates.size()));
    for (int i = 0; i < count; ++i) out[i] = g_list[static_cast<std::size_t>(candidates[static_cast<std::size_t>(i)].index)];
    return count;
}

bool RequestPlay(std::uint32_t instanceId, std::uint64_t componentId, const AnimListEntry& entry, bool loop, float speed) {
    if (!g_available || !componentId) return false;
    std::lock_guard<std::mutex> lock(g_requestMutex);
    g_playRequest.pending = true;
    g_playRequest.instanceId = instanceId;
    g_playRequest.componentId = componentId;
    g_playRequest.entry = entry;
    g_playRequest.loop = loop;
    g_playRequest.speed = speed;
    SetText(g_playStatus, sizeof(g_playStatus), "Starting %s (%d)...", entry.name, static_cast<int>(entry.animId));
    return true;
}

void Stop(std::uint32_t instanceId, std::uint64_t componentId) {
    AcquireSRWLockExclusive(&g_playerLock);
    for (int i = 0; i < g_playerCount; ++i) {
        if (g_players[i].instanceId != instanceId || g_players[i].componentId != componentId) continue;
        SetText(g_playStatus, sizeof(g_playStatus), "Stopped %s.", g_players[i].name);
        RemovePlayerLocked(i);
        break;
    }
    ReleaseSRWLockExclusive(&g_playerLock);
}

void StopAll() {
    AcquireSRWLockExclusive(&g_playerLock);
    while (g_playerCount > 0) RemovePlayerLocked(g_playerCount - 1);
    ReleaseSRWLockExclusive(&g_playerLock);
}

bool GetPlayback(std::uint32_t instanceId, std::uint64_t componentId, PlaybackInfo& out) {
    out = {};
    AcquireSRWLockShared(&g_playerLock);
    const Player* p = nullptr;
    for (int i = 0; i < g_playerCount && !p; ++i)
        if (g_players[i].instanceId == instanceId && g_players[i].componentId == componentId) p = &g_players[i];
    if (p) {
        const double t = PlayerTime(*p, QpcNow(), nullptr);
        out.playing = true;
        out.paused = p->paused;
        out.loop = p->loop;
        out.speed = p->speed;
        out.time = static_cast<float>(t / kTimeUnitsPerSecond);
        out.duration = static_cast<float>(static_cast<double>(p->keys) * p->timePerKey / kTimeUnitsPerSecond);
        out.frames = p->keys;
        out.frame = std::min(static_cast<int>(t / p->timePerKey), static_cast<int>(p->keys) - 1);
        out.animId = p->animId;
        CopyText(out.name, sizeof(out.name), p->name);
    }
    ReleaseSRWLockShared(&g_playerLock);
    return p != nullptr;
}

namespace {
// Re-bases the clock at the current time so a change of speed / pause / loop keeps the current frame.
template <typename F>
void EditPlayer(std::uint32_t instanceId, std::uint64_t componentId, F&& edit) {
    AcquireSRWLockExclusive(&g_playerLock);
    if (Player* p = FindPlayerLocked(instanceId, componentId)) {
        const std::int64_t now = QpcNow();
        p->baseTime = PlayerTime(*p, now, nullptr);
        p->startQpc = now;
        edit(*p);
    }
    ReleaseSRWLockExclusive(&g_playerLock);
}
}  // namespace

void SetLoop(std::uint32_t instanceId, std::uint64_t componentId, bool loop) {
    EditPlayer(instanceId, componentId, [&](Player& p) { p.loop = loop; });
}

void SetSpeed(std::uint32_t instanceId, std::uint64_t componentId, float speed) {
    EditPlayer(instanceId, componentId, [&](Player& p) { p.speed = std::clamp(speed, 0.01f, 10.0f); });
}

void SetPaused(std::uint32_t instanceId, std::uint64_t componentId, bool paused) {
    EditPlayer(instanceId, componentId, [&](Player& p) {
        // a clip that played to its end starts over when resumed
        if (!paused && !p.loop && p.baseTime >= (static_cast<double>(p.keys) - 1.0) * p.timePerKey) p.baseTime = 0.0;
        p.paused = paused;
    });
}

void SeekFrame(std::uint32_t instanceId, std::uint64_t componentId, float frame) {
    EditPlayer(instanceId, componentId, [&](Player& p) {
        const float last = static_cast<float>(p.keys) - 1.0f;
        p.baseTime = static_cast<double>(std::clamp(frame, 0.0f, std::max(last, 0.0f))) * p.timePerKey;
        p.paused = true;
    });
}

const char* PlayStatus() { return g_playStatus; }

bool PlayLayers(std::uint32_t instanceId, std::uint64_t componentId, const Layer* layers, int count, std::string& info,
                const BindPivot* reference, int referenceCount) {
    char text[512];
    if (!g_available) {
        info = std::string("the clip player is unavailable: ") + g_unavailable;
        return false;
    }
    if (!layers || count <= 0 || count > kMaxLayers) {
        info = "no layers";
        return false;
    }
    void* instance = Features::InstanceNowGameThread(instanceId, componentId);
    if (!instance) {
        info = "the instance is gone";
        return false;
    }
    const bool noComponent = !HasAnimComponent(instance);   // (v0.9.436: see HookPostRebuildTransforms)
    if (noComponent && !g_posesWithoutComponent) {
        info = "the instance has no animation component (and the hook for one without is unavailable)";
        return false;
    }
    const std::uint8_t* zone = ReadZone(instance);
    // the clips: the layers named, from the library that has the most of them; the layers by animation ID (v0.9.441: and
    // the first layer's sequence), each from any library that lists it
    const char* names[kMaxLayers]{};      // (the named layers', looked for together)
    int nameOf[kMaxLayers];
    int nameCount = 0;
    const int sequenceCount = std::clamp(layers[0].sequenceCount, 0, 3);
    for (int i = 0; i < count; ++i) {
        const bool byId = layers[i].animId != 0 || (i == 0 && sequenceCount > 0 && layers[0].sequence);
        nameOf[i] = byId ? -1 : nameCount;
        if (!byId) names[nameCount++] = layers[i].name ? layers[i].name : "";
    }
    const std::uint8_t* named[kMaxLayers]{};
    std::uint16_t namedNumbers[kMaxLayers]{};
    std::uint32_t key = 0;
    char library[48]{};
    const int have = nameCount ? FindNamedClips(names, nameCount, named, namedNumbers, &key, library, sizeof(library)) : 0;
    const std::uint8_t* clips[kMaxLayers]{};
    std::uint16_t numbers[kMaxLayers]{};
    static char idLabels[kMaxLayers][64];
    const char* labels[kMaxLayers]{};
    const std::uint8_t* sequenceClips[3]{};
    std::uint16_t sequenceNumbers[3]{};
    std::string missing, idLibraries;
    auto byId = [&](std::int16_t id, const std::uint8_t** clip, std::uint16_t* number) {
        char from[48]{};
        if (FindClipById(id, clip, number, from, sizeof(from)) > 0) {
            if (idLibraries.find(from) == std::string::npos) idLibraries += std::string(idLibraries.empty() ? "" : ", ") + from;
            return true;
        }
        missing += std::string(missing.empty() ? "" : ", ") + "animation " + std::to_string(id);
        return false;
    };
    for (int i = 0; i < count; ++i) {
        if (i == 0 && nameOf[0] < 0 && sequenceCount > 0) {
            std::string label;
            for (int k = 0; k < sequenceCount; ++k) {
                byId(layers[0].sequence[k], &sequenceClips[k], &sequenceNumbers[k]);
                label += std::string(k ? " > " : "") + std::to_string(layers[0].sequence[k]);
            }
            clips[0] = sequenceClips[0], numbers[0] = sequenceNumbers[0];
            CopyText(idLabels[0], sizeof(idLabels[0]), ("animations " + label).c_str());
            labels[0] = idLabels[0];
        } else if (nameOf[i] < 0) {
            byId(layers[i].animId, &clips[i], &numbers[i]);
            CopyText(idLabels[i], sizeof(idLabels[i]), ("animation " + std::to_string(layers[i].animId)).c_str());
            labels[i] = idLabels[i];
        } else {
            labels[i] = names[nameOf[i]];
            if (have > 0) clips[i] = named[nameOf[i]], numbers[i] = namedNumbers[nameOf[i]];
            if (!clips[i]) missing += std::string(missing.empty() ? "" : ", ") + labels[i];
        }
    }
    if (!missing.empty() || !zone) {
        std::snprintf(text, sizeof(text), "%s not loaded%s%s%s", missing.empty() ? "the instance's object" : missing.c_str(),
                      have > 0 ? " (the library with the most of the named: " : "", have > 0 ? library : "", have > 0 ? ")" : "");
        info = text;
        return false;
    }
    if (!idLibraries.empty()) {   // (the libraries of the clips by ID, for info)
        if (library[0]) idLibraries = std::string(library) + ", " + idLibraries;
        CopyText(library, sizeof(library), idLibraries.c_str());
    }
    // (the static Player: ~10 KB; the game thread alone builds one here)
    static Player p;
    p = Player{};
    p.instance = instance;
    p.zone = zone;
    p.instanceId = instanceId;
    p.componentId = componentId;
    p.referenceCount = reference ? std::clamp(referenceCount, 0, kMaxReference) : 0;
    for (int r = 0; r < p.referenceCount; ++r) p.reference[r] = reference[r];
    int shifted = 0;
    // its blend shape slots (none without the hook, or on an object that has no blend shapes)
    void* animComponent = nullptr;
    int slots = ReadShapeSlots(instance, &animComponent);
    p.noComponent = noComponent;
    if (noComponent) slots = zone ? ReadZoneShapeCount(zone) : 0;   // (its stand-in's, made below)
    p.animComponent = animComponent;
    p.shapeCount = g_shapesAvailable ? std::min(std::max(slots, 0), kMaxShapes) : 0;
    bool askedShapes = false;
    std::string what;
    for (int i = 0; i < count; ++i) {
        std::uint16_t map[256];
        int mapped = BuildMaskedIdMap(zone, clips[i], layers[i].bones, layers[i].boneCount, map);
        if (mapped > 0 && layers[i].segmentMask) {   // (v0.9.475) only the segments of the mask
            mapped = 0;
            for (int t = 0; t < 256; ++t) {
                if (map[t] == 0xFFFF) continue;
                if (map[t] < layers[i].segmentMaskCount && layers[i].segmentMask[map[t]]) ++mapped;
                else map[t] = 0xFFFF;
            }
        }
        std::uint16_t shapeMap[256];
        const int shapes = BuildShapeMap(zone, clips[i], layers[i].shapes, layers[i].shapeCount, p.shapeCount, shapeMap);
        askedShapes = askedShapes || layers[i].shapeCount != 0;
        static Player one;
        one = Player{};
        ReadClipHeader(clips[i], one);
        if (mapped < 0 || shapes < 0 || !one.keys || one.timePerKey <= 0 || (i == 0 && mapped == 0)) {
            std::snprintf(text, sizeof(text), "%s: %s", labels[i], mapped < 0 || shapes < 0 ? "its tracks could not be read"
                                                                 : mapped == 0 && one.keys ? "none of its bones on this skeleton"
                                                                                           : "no frames");
            info = text;
            return false;
        }
        if (mapped == 0 && shapes == 0) {   // (a layer of the face on an object without it: left out)
            std::snprintf(text, sizeof(text), "%s%s left out (none of its %s here)", i ? "; " : "", labels[i],
                          layers[i].shapeCount != 0 ? "blend shapes or bones" : "bones");
            what += text;
            continue;
        }
        std::snprintf(text, sizeof(text), "%s%s (%u frames x %d, %d bone tracks%s", i ? "; " : "", labels[i],
                      static_cast<unsigned>(one.keys), static_cast<int>(one.timePerKey), mapped,
                      layers[i].boneCount != kAllTracks ? " of its bones" : "");
        what += text;
        if (shapes > 0) {
            std::snprintf(text, sizeof(text), ", %d blend shapes", shapes);
            what += text;
            for (int k = 0; k < 256; ++k)
                if (shapeMap[k] < kMaxShapes) p.shapeSet[shapeMap[k]] = 1;
        }
        std::snprintf(text, sizeof(text), ", speed %.2f)", layers[i].speed);
        what += text;
        Shifts shifts;
        if (BuildShifts(clips[i], p.reference, p.referenceCount, shifts) > 0)
            for (int k = 0; k < shifts.count; ++k) shifted += map[shifts.list[k].track] != 0xFFFF ? 1 : 0;
        if (i == 0) {
            p.shifts = shifts;
            p.clip = clips[i];
            p.clipNum = numbers[i];
            std::memcpy(p.idMap, map, sizeof(map));
            p.shapes = shapes > 0;
            std::memcpy(p.shapeSlot, shapeMap, sizeof(shapeMap));
            p.keys = one.keys;
            p.timePerKey = one.timePerKey;
            p.blendShapes = one.blendShapes;
            p.attribs = one.attribs;
            p.loop = !layers[i].once;   // (v0.9.481: once)
            p.weight = std::clamp(layers[i].weight, 0.0f, 1.0f);
            p.speed = std::clamp(layers[i].speed, 0.01f, 10.0f);
            CopyText(p.name, sizeof(p.name), labels[i]);
        } else {
            Extra& e = p.extra[p.extraCount++];
            e.clip = clips[i];
            e.clipNum = numbers[i];
            e.keys = one.keys;
            e.timePerKey = one.timePerKey;
            e.attribs = one.attribs;
            e.blendShapes = one.blendShapes;
            e.speed = std::clamp(layers[i].speed, 0.01f, 10.0f);
            std::memcpy(e.idMap, map, sizeof(map));
            e.shapes = shapes > 0;
            std::memcpy(e.shapeSlot, shapeMap, sizeof(shapeMap));
            e.shifts = shifts;
            e.weight = std::clamp(layers[i].weight, 0.0f, 1.0f);   // (v0.9.481)
            e.sumWithFirst = layers[i].sumWithFirst;
            e.once = layers[i].once;
            CopyText(e.name, sizeof(e.name), labels[i]);
        }
    }
    // (v0.9.441) the first layer's sequence: each clip with its own id map and shifts, the player's time over all of them
    if (nameOf[0] < 0 && sequenceCount > 0) {
        for (int k = 0; k < sequenceCount; ++k) {
            FidgetPart& part = p.chain[k];
            part = FidgetPart{};
            static Player header;
            header = Player{};
            ReadClipHeader(sequenceClips[k], header);
            if (!header.keys || header.timePerKey <= 0 ||
                BuildMaskedIdMap(zone, sequenceClips[k], layers[0].bones, layers[0].boneCount, part.idMap) <= 0) {
                std::snprintf(text, sizeof(text), "animation %d: %s", int(layers[0].sequence[k]),
                              header.keys ? "none of its bones on this skeleton" : "no frames");
                info = text;
                return false;
            }
            part.clip = sequenceClips[k];
            part.clipNum = sequenceNumbers[k];
            part.keys = header.keys;
            part.timePerKey = header.timePerKey;
            part.attribs = header.attribs;
            part.blendShapes = header.blendShapes;
            BuildShifts(sequenceClips[k], p.reference, p.referenceCount, part.shifts);
            p.chainUnits += static_cast<double>(header.keys) * header.timePerKey;
        }
        p.chainCount = sequenceCount;
        p.shapes = false;   // (a sequence sets no blend shapes: the layers over it do)
        std::snprintf(text, sizeof(text), "; played in turn and looping (%.1f s)", p.chainUnits / kTimeUnitsPerSecond);
        what += text;
    }
    for (int s = 0; s < kMaxShapes; ++s) p.shapeSlots += p.shapeSet[s];
    if (p.referenceCount > 0) {
        std::snprintf(text, sizeof(text), "; translations kept as offsets from the clips' skeleton on %d tracks", shifted);
        what += text;
    }
    if (askedShapes && p.shapeSlots == 0)
        what += !g_shapesAvailable ? "; no blend shapes (the hook is unavailable)"
                                   : slots == 0 ? "; no blend shapes (the object has none)" : "; none of their blend shapes here";
    if (noComponent) {
        what += "; no animation component: its pose built after the game's bind pose";
        if (p.shapeSlots > 0) {
            p.fakeComponent = MakeFakeComponent(instance, p.shapeCount);
            p.animComponent = p.fakeComponent;
            if (!p.fakeComponent) {
                p.shapeSlots = 0;
                what += " (no memory for its blend shapes)";
            }
        }
    }
    // (each layer's clock: the player's start, at its own speed)
    p.baseTime = 0.0;
    p.startQpc = QpcNow();
    p.startTick = GetTickCount64();
    AcquireSRWLockExclusive(&g_playerLock);
    Player* slot = StoreLocked(p);
    ReleaseSRWLockExclusive(&g_playerLock);
    if (!slot) {
        if (p.fakeComponent) _aligned_free(p.fakeComponent);
        std::snprintf(text, sizeof(text), "%d instances already play animations", kMaxPlayers);
        info = text;
        return false;
    }
    info = std::string(library) + ": " + what;
    return true;
}

bool SetLayerWeights(std::uint32_t instanceId, std::uint64_t componentId, const float* weights, int count) {
    if (!weights || count <= 0) return false;
    AcquireSRWLockExclusive(&g_playerLock);
    Player* p = FindPlayerLocked(instanceId, componentId);
    if (p) {
        p->weight = std::clamp(weights[0], 0.0f, 1.0f);
        for (int i = 1; i < count && i - 1 < p->extraCount; ++i) p->extra[i - 1].weight = std::clamp(weights[i], 0.0f, 1.0f);
    }
    ReleaseSRWLockExclusive(&g_playerLock);
    return p != nullptr;
}

bool RestartLayer(std::uint32_t instanceId, std::uint64_t componentId, int layer) {
    AcquireSRWLockExclusive(&g_playerLock);
    Player* p = FindPlayerLocked(instanceId, componentId);
    bool ok = false;
    if (p && layer == 0) {
        p->baseTime = 0.0;
        p->startQpc = QpcNow();
        ok = true;
    } else if (p && layer > 0 && layer - 1 < p->extraCount) {
        p->extra[layer - 1].startQpc = QpcNow();
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_playerLock);
    return ok;
}

bool ClipLoaded(std::int16_t animId) {
    if (!g_available) return false;
    const std::uint8_t* clip = nullptr;
    std::uint16_t number = 0;
    char from[48];
    return FindClipById(animId, &clip, &number, from, sizeof(from)) > 0;
}

namespace {
// (ReferenceFromObject) The bones the clip sets translations on (as BuildShifts reads its channels) with the skeleton's
// bind translations, added to out (each bone once). zone: a stand-in ObjectZone (only its +8 is read).
int AddClipReferenceUnsafe(const std::uint8_t* zone, const std::uint8_t* clip, const std::uint8_t* segments, std::uint16_t count,
                           InstanceAnim::BindPivot* out, int n, int capacity) {
    const std::uint8_t tracks = clip[kClipBones];
    const auto* ids = *reinterpret_cast<const std::uint16_t* const*>(clip + kClipBoneIds);
    const std::uint8_t* flags = *reinterpret_cast<const std::uint8_t* const*>(clip + kClipChannelFlags);
    if (!ids || !flags) return n;
    const std::uint8_t* at = flags + 2;
    for (int b = 0; b < tracks && n < capacity; ++b) {
        unsigned f = *at++;
        if (f & 0x88) f = ((unsigned(*at++) << 8) | f) & 0x7777;
        if (!((f >> 4) & 7)) continue;
        bool have = false;
        for (int r = 0; r < n && !have; ++r) have = out[r].bone == ids[b];
        std::uint16_t index = 0xFFFF;
        if (have || !g_getBoneIndex(zone, ids[b], &index) || index >= count) continue;
        const auto* pivot = reinterpret_cast<const float*>(segments + std::size_t(index) * kSkeletonStride + kSkeletonPivot);
        out[n].bone = ids[b];
        for (int k = 0; k < 3; ++k) out[n].pivot[k] = pivot[k];
        ++n;
    }
    return n;
}
int ReferenceFromObjectSafe(const void* objectData, const std::uint8_t* const* clips, int clipCount, InstanceAnim::BindPivot* out,
                            int capacity) {
    alignas(16) std::uint8_t zone[0x10]{};   // (ObjectZone::GetBoneIndexFromID, GetNumAnimatedSegments, GetSegmentList: +8)
    *reinterpret_cast<const void**>(zone + 8) = objectData;
    __try {
        const std::uint16_t count = g_zoneAnimatedSegments(zone);
        const std::uint8_t* segments = g_zoneSegmentList(zone);
        if (count <= 1 || !segments) return 0;   // (no skeleton: one default segment)
        int n = 0;
        for (int c = 0; c < clipCount; ++c) n = AddClipReferenceUnsafe(zone, clips[c], segments, count, out, n, capacity);
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
}  // namespace

int ReferenceFromObject(const void* objectData, const std::int16_t* animIds, int count, BindPivot* out, int capacity) {
    if (!g_available || !objectData || !g_zoneAnimatedSegments || !g_zoneSegmentList || !g_getBoneIndex || !animIds || count <= 0)
        return -1;
    const std::uint8_t* clips[8]{};
    count = std::min(count, 8);
    for (int c = 0; c < count; ++c) {
        std::uint16_t number = 0;
        char from[48];
        if (FindClipById(animIds[c], &clips[c], &number, from, sizeof(from)) <= 0) return -1;
    }
    return ReferenceFromObjectSafe(objectData, clips, count, out, std::min(capacity, kMaxReference));
}

bool AddFidgets(std::uint32_t instanceId, std::uint64_t componentId, const Fidget* fidgets, int count, float minGap,
                float maxGap, float fade, std::string& info) {
    char text[256];
    if (!g_available || !g_fidgetsAvailable) {
        info = "fidgets are unavailable (see the log)";
        return false;
    }
    void* instance = Features::InstanceNowGameThread(instanceId, componentId);
    const std::uint8_t* zone = instance ? ReadZone(instance) : nullptr;
    if (!zone || !fidgets || count <= 0) {
        info = zone ? "no fidgets" : "the instance is gone";
        return false;
    }
    // every part's clip, from the library that has the most of them
    const char* names[kMaxNames]{};
    int nameCount = 0;
    int nameAt[kMaxFidgets][3];   // (each part's name in names, -1: none)
    for (int f = 0; f < count && f < kMaxFidgets; ++f)
        for (int k = 0; k < 3; ++k) {
            nameAt[f][k] = -1;
            if (k < fidgets[f].partCount && fidgets[f].parts[k] && nameCount < kMaxFidgetParts) {
                nameAt[f][k] = nameCount;
                names[nameCount++] = fidgets[f].parts[k];
            }
        }
    const std::uint8_t* clips[kMaxNames]{};
    std::uint16_t numbers[kMaxNames]{};
    std::uint32_t key = 0;
    char library[48]{};
    if (nameCount == 0 || FindNamedClips(names, nameCount, clips, numbers, &key, library, sizeof(library)) <= 0) {
        info = "none of the fidgets' animations is loaded";
        return false;
    }
    InstanceAnim::BindPivot reference[kMaxReference];   // (the player's: PlayLayers')
    int referenceCount = 0;
    AcquireSRWLockShared(&g_playerLock);
    if (const Player* p = FindPlayerLocked(instanceId, componentId)) {
        referenceCount = p->referenceCount;
        for (int r = 0; r < referenceCount; ++r) reference[r] = p->reference[r];
    }
    ReleaseSRWLockShared(&g_playerLock);
    static FidgetPart parts[kMaxFidgetParts];
    FidgetDef defs[kMaxFidgets];
    int partCount = 0, defCount = 0;
    std::string what, missing;
    for (int f = 0; f < count && f < kMaxFidgets; ++f) {
        const Fidget& in = fidgets[f];
        FidgetDef def;
        def.firstPart = partCount;
        bool ok = in.partCount > 0 && in.partCount <= 3;
        for (int k = 0; k < in.partCount && k < 3; ++k) {
            const int at = nameAt[f][k];
            if (!ok || at < 0 || !clips[at]) {
                ok = false;
                continue;
            }
            FidgetPart& part = parts[partCount + def.partCount];
            part = FidgetPart{};
            static Player header;
            header = Player{};
            ReadClipHeader(clips[at], header);
            if (!header.keys || header.timePerKey <= 0 || BuildMaskedIdMap(zone, clips[at], nullptr, kAllTracks, part.idMap) <= 0) {
                ok = false;
                continue;
            }
            part.clip = clips[at];
            part.clipNum = numbers[at];
            part.keys = header.keys;
            part.timePerKey = header.timePerKey;
            part.attribs = header.attribs;
            part.blendShapes = header.blendShapes;
            BuildShifts(clips[at], reference, referenceCount, part.shifts);
            def.units += static_cast<double>(header.keys) * header.timePerKey;
            ++def.partCount;
        }
        const char* first = in.parts[0] ? in.parts[0] : "?";
        if (!ok || def.partCount == 0) {
            missing += std::string(missing.empty() ? "" : ", ") + first;
            continue;
        }
        CopyText(def.name, sizeof(def.name), first);
        partCount += def.partCount;
        defs[defCount++] = def;
        std::snprintf(text, sizeof(text), "%s%s (%.1f s)", what.empty() ? "" : ", ", first, def.units / kTimeUnitsPerSecond);
        what += text;
    }
    if (defCount == 0) {
        info = "none of the fidgets is loaded whole: " + missing;
        return false;
    }
    bool found = false;
    AcquireSRWLockExclusive(&g_playerLock);
    if (Player* p = FindPlayerLocked(instanceId, componentId)) {
        found = true;
        for (int k = 0; k < partCount; ++k) p->parts[k] = parts[k];
        p->partCount = partCount;
        for (int k = 0; k < defCount; ++k) p->fidgets[k] = defs[k];
        p->fidgetCount = defCount;
        p->fidgetMinGap = std::max(minGap, 1.0f);
        p->fidgetMaxGap = std::max(maxGap, p->fidgetMinGap);
        p->fadeUnits = std::max(fade, 0.0f) * kTimeUnitsPerSecond;
        p->fidgetNow = p->fidgetLast = -1;
        const std::int64_t now = QpcNow();
        p->random = static_cast<std::uint32_t>(now) | 1u;
        p->fidgetNextQpc = now + FidgetGapTicks(*p);
    }
    ReleaseSRWLockExclusive(&g_playerLock);
    if (!found) {
        info = "the instance plays nothing (PlayLayers first)";
        return false;
    }
    info = std::string(library) + ": " + what + (missing.empty() ? "" : "; not loaded whole: " + missing);
    return true;
}
}  // namespace InstanceAnim
