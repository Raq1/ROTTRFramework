#include "combat_air.h"
#include "instance_anim.h"
#include "acro.h"
#include "brawl.h"
#include "features.h"
#include "game_build.h"
#include "glow_color.h"
#include "grapple.h"
#include "log.h"
#include "overlay.h"
#include "settings.h"
#include <Windows.h>
#include <Xinput.h>
#include <MinHook.h>
#include <imgui.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <iterator>

// Aiming in the air. Lara's primary StateControl agent leaves Combat_CombatAllowed at its default
// (false) in every jump state, and the fall states set Combat_InventoryPause, so the weapon agent never
// draws or aims in the air: its "default_to_combat_wp_equip" needs CombatAllowed, and the inventory
// agent parks in "inventory_pause" while InventoryPause is set. (The SC also has unused in-air combat
// states, combat_jumpShot_*, with aim and fire allowed and time dilation 0.15, which nothing leads to.)
// While the aim input is held and the primary agent is in a jump or fall state, SCPROPERTY_GetBool
// answers Lara's combat permissions the way the ground combat states set them: CombatAllowed,
// LockOnAllowed, DualGunsAllowed, AccurateAim and TwoHandedGunsAllowed true, InventoryPause and
// UnequipItemInInvPause false. States that forbid guns (fall_parent_noguns, fall_thrown_parent) and
// the ledge/rope pre-attach and rescue states keep the game's values.
// Every draw (and reload) transition of the weapon agent also needs spatial probe 40, the ground
// probe the primary agent uses to tell a short fall from a long one (v0.9.198 test: the weapon agent
// stayed in "default" through every jump and drew on landing). For the weapon agent only, and only
// where the condition asks for a hit, the probe answers "hit"; its one NOT use there
// (combat_bow_ready_jump_to_charge: charge a drawn bow high in the air) keeps the real probe.
// v0.9.199 still did not draw in the air: the draw transition got past the probe and failed on a
// later condition. SCAgent::CheckConditions now reports the weapon agent's failing draw conditions in
// the air, and lets the draw through when only conditions that describe standing on the ground fail
// (the probe, low ceiling, the named-value blockers, the no-process / primary-override flags); never
// without the draw request, a ranged weapon as the item, or with melee, a throwable or water.
// v0.9.200 log: the torch draw (same first 13 conditions as the plain draw) failed only on its torch
// conditions, so the plain draw "default_to_combat_wp_equip" fails on its own last two:
// NOT Interacted_IsInteractableType (in the air the jump target counts as the interacted object) and
// a named value. The interacted-object check is air-tolerant too.
// v0.9.201 log: the draw now starts in the air, but "combat_wp_equip_to_ready" waits for
// Frames_InStateHosted: the weapon's equip animation never runs. Lara's anim graph plays the weapon's
// hosted graph ("Ranged") only in its full-body "Combat weapon_hosted" state, chosen by the moveset
// goal "Combat"; "In_Air" hosts the in-air moveset graph, whose overlays take the advertise and
// throwable hosts, never the weapon, and Rise has no in-air aim animation. So while aiming in the
// air, the moveset goal the state control asks for ("InAir") is answered with "Combat"
// (AnimComponentV2::SetAnimGraphGoal): the weapon graph runs with its ground combat stance while
// Lara keeps flying; letting go of aim in the air gives the graph "InAir" back. (The SC's own cut
// combat jump, combat_jump_falling_far, does the same: moveset "Combat", cluster "JumpFalling".)
// v0.9.202 log: the weapon agent keeps going back from combat_wp_equip to "default" before the
// equip time runs out (Frames_InStateHosted compares the frames in the state with the weapon's
// equip-time named value, WeaponComponent::SetHostedTimers), so the draw restarts over and over.
// While aiming in the air, the weapon agent's ways out of its combat states towards the holster
// (postCombat*, the immediate unequips, "default", the sprint lowering) are held back; SCAgent::
// DoTransition is traced for Lara's agents during and just after an air aim.
// v0.9.203 log (air aim works): the exit was combat_to_immediate_Unequip (PostCombatAllowed false in
// the air). Then:
// - a pistol fired once per jump: combat_wp_fire -> combat_wp_fire_cooldown ("combat_wp_fire_to_ready")
//   needs NOT Status_IsFalling; the falling variant needs the cut adrenaline mode. In the air it passes,
//   and with "faster shooting" the fire interval and the recoil animation's critical section do not
//   hold the next shot longer than our own interval.
// - Lara kept her jump facing: the jump states set the steering to None (rising) or 4 deg/s (falling).
//   PlayerSteering::Process is fed the camera's forward, yaw steering, and our turn rate while aiming.
// - the slow motion start plays the survival instinct sound: the event VisionMode::Enable sends to Lara
//   ("evVisionSoundFadeIn", EventSystem::SendEvent by name).
// v0.9.204 test: shots in the air did no damage and the arms did not follow the aim. The jump states
// turn Combat_AccurateAim off, so the CombatManager ran no aim probe: no aim target under the reticle,
// Tertiary Aiming could not enter combat_aim. It is kept on while aiming in the air. The event alone
// played no sound; the slow motion now starts the game's own survival instinct view (VisionMode::Enable,
// like its listener) without highlights, objective indicator and hint line, and ends it with the slow
// motion. The slow motion button is LB again.
// v0.9.205 test: damage, instinct view and sound work; the upper body did not bend up/down with the aim
// in the air. The weapon graph takes it from the aim pitch/yaw on the first SC agent (+0x580/+0x584,
// PlayerAG_Camera), computed by _Combat_AvoidWeaponClipping_Aiming, which only the ground aim states run
// (Combat_AvoidWeaponClipping activity); it is now called every frame while aiming in the air.
//
// v0.9.473: weapons on beams. Lara's balance beam states (the state or one of its parents named balancebeam_...; since
// v0.9.474, 0.9.473's beam... / ..._beam... matched none) forbid combat as the jump states do. With "Shoot on beams" on and aim held there, the same overrides as
// in the air apply (the combat permissions, the draw and fire conditions, accurate aim, the aim angles) and the moveset
// goal the state control asks for there becomes "Combat" while aiming (the weapon's hosted graph runs only there), the
// state control's own goal given back when aim is let go. Lara is not turned towards the camera on a beam (her
// heading follows the beam). The first beam states met are logged by name.
//
// Slow motion. Time dilation goes through TimeDilationManager::ms_timeDilationManager: one request
// slot per dtp::TimeDilationType, the lowest active type wins. A request holds a world factor (all
// but the player and the exempt instances) and a global factor (everything): the world runs at
// world * global, Lara at global. The state control (type 7), scripts (6) and the expedition "hunter
// sense" card (8, cancelled every frame while aiming without the card) use it; we take type 9
// (Default), which nothing requests, so every request of the game overrides ours while it lasts.
// Rise has no player-triggered slow motion: the Adrenaline class (Player+0x3228: points from XP
// awards, Ready/Active/Cooldown, started only where the SC property
// Combat_ButtonInitiatedAdrenalineEnabled is set, which Lara's SC never does) does not dilate time.

namespace {
constexpr std::uintptr_t kRvaScPropertyGetBool        = 0x00708C60ull; // SCPROPERTY_GetBool(SCPropertyData*, SCAgent*)
constexpr std::uintptr_t kRvaSpatialProbeHit          = 0x006BFBE0ull; // CSCCF_SpatialProbeHit(SCAgent*, SCCondMemory*, SCConditionData*)
constexpr std::uintptr_t kRvaCheckConditions          = 0x006D2570ull; // SCAgent::CheckConditions(SCTransitionData*)
constexpr std::uintptr_t kRvaGetCondProperty          = 0x006DC520ull; // GetSCCondProperty_WithContextHack(SCConditionData*)
constexpr std::uintptr_t kRvaSymbolCStr               = 0x003921A0ull; // cdc::Symbol::c_str
constexpr std::uintptr_t kRvaSetAnimGraphGoal         = 0x0011C580ull; // cdc::AnimComponentV2::SetAnimGraphGoal(type, value)
constexpr std::uintptr_t kRvaGoalMovesetPropFunction  = 0x006C6B30ull; // SCPropFunction_SetAnimGraph_Goal_Moveset
constexpr std::uintptr_t kRvaTimeDilationRequestCtor  = 0x00483B90ull; // TimeDilationRequest::TimeDilationRequest
constexpr std::uintptr_t kRvaTimeDilationRequest      = 0x00497AF0ull; // TimeDilationManager::Request
constexpr std::uintptr_t kRvaTimeDilationCancel       = 0x004865B0ull; // TimeDilationManager::CancelRequest
constexpr std::uintptr_t kRvaSetTimeDilationManager   = 0x006A947Full; // lea rcx,[ms_timeDilationManager] (PSCPF_SetTimeDilation)
constexpr std::uintptr_t kRvaInventoryPauseScId      = 0x006D3245ull; // mov ecx,[ComponentArrayID<StateControlComponent>::s_id]
constexpr std::uintptr_t kRvaGetLocalPlayerInstance   = 0x00403BF0ull;
constexpr std::uintptr_t kRvaPlayerComponentId        = 0x010FAA90ull; // ComponentArrayID<PlayerComponent>::s_id
constexpr std::uintptr_t kRvaDoTransition             = 0x006D8060ull; // SCAgent::DoTransition(SCTransitionData*)
constexpr std::uintptr_t kRvaSteeringProcess          = 0x0083F260ull; // PlayerSteering::Process
constexpr std::uintptr_t kRvaSteeringSetTargetDir     = 0x00850570ull; // PlayerSteering::SetTargetDir(Vector3*)
constexpr std::uintptr_t kRvaSteeringSetTurnParams    = 0x00850F50ull; // PlayerSteering::SetTurnParameters
constexpr std::uintptr_t kRvaCameraForward            = 0x0014A680ull; // CameraManager::GetActiveCamera_Forward(out, id)
// cdc::AnimBuildTransforms(Instance*, u32 count, AnimSegment* segs, Segment* skeleton, Matrix* out) (TR2 0x140119BA0):
// the final pose's local transforms to bone matrices; out[-1] is the root's parent (the instance's world matrix).
constexpr std::uintptr_t kRvaAnimBuildTransforms      = 0x00107400ull;
constexpr std::uintptr_t kRvaCameraManagerLoad        = 0x006B7258ull; // mov rcx,[m_pCameraManager] (PSCAF_P_Turn_CameraDir)
constexpr std::uintptr_t kRvaCameraForwardCall        = 0x006B72ADull; // call GetActiveCamera_Forward (same function)
constexpr std::uintptr_t kRvaSendEventByName          = 0x007F94D0ull; // EventSystem::SendEvent(char*, Instance*, int, void*, u64)
constexpr std::uintptr_t kRvaEventSystemLoad          = 0x005D1703ull; // mov rcx,[EventSystem::s_instance] (VisionMode::Enable)
constexpr std::uintptr_t kRvaSendEventCall            = 0x005D1717ull; // call SendEvent "evVisionSoundFadeIn" (same function)
constexpr std::uintptr_t kRvaSetAccurateAim           = 0x006B6D20ull; // PSCPF_SetAccurateAim(SCAgent*, SCPropertyData*)
constexpr std::uintptr_t kRvaGetPropertyData          = 0x006DC000ull; // (StateControlComponent*, u16) -> current SCPropertyData*
constexpr std::uintptr_t kRvaVisionEnable             = 0x005D1380ull; // VisionMode::Enable(Instance*, eVisionMode, float)
constexpr std::uintptr_t kRvaVisionDisable            = 0x005CDB80ull; // VisionMode::Disable(Instance*)
constexpr std::uintptr_t kRvaVisionInstanceLoad       = 0x005F4EA9ull; // mov rcx,[VisionMode::mInstance] (VisionModeListener)
constexpr std::uintptr_t kRvaVisionEnableJump         = 0x005F4EBAull; // jmp VisionMode::Enable (same listener)
constexpr std::uintptr_t kRvaInstinctHighlight        = 0x005D2260ull; // SurvivalInstinctComponent::EnableHighlight(Instance*)
constexpr std::uintptr_t kRvaAsymInstinctHighlight    = 0x005D2200ull; // AsymmetricSurvivalInstinctComponent::EnableHighlight
constexpr std::uintptr_t kRvaAddAsymComponents        = 0x005ABE30ull; // VisionMode::AddAsymetricComponents (per frame)
constexpr std::uintptr_t kRvaNotifyDialogueContext    = 0x005594C0ull; // DialogueManager::NotifyDialogueContext(char*)
constexpr std::uintptr_t kRvaAvoidClippingAiming      = 0x00714360ull; // _Combat_AvoidWeaponClipping_Aiming(SCAgent*)
constexpr std::uintptr_t kRvaAvoidClippingProcess     = 0x00693000ull; // PSCAF_P_Combat_AvoidWeaponClipping (calls it)
// PROJECTILE_FireInstant(ProjectileComponent::Properties*, SingleFireDataBlock*, TraceFireData*, bool): an instant bullet
// (TR2 0x1406467E0; one caller, WeaponComponent::FireSingle, ignores the result). SingleFireDataBlock: +0 weapon, +8
// owner, +0x10 target instance handles (Instance +0x118, checked below); TraceFireData: +0 start, +0x10 end (Vector4),
// +0x20 max range; the trace (PROJECTILE_TraceFire 0x613400) runs inside with the owner ignored.
constexpr std::uintptr_t kRvaFireInstant              = 0x006102F0ull;
constexpr std::uintptr_t kRvaFireInstantOwnerCheck    = 0x00610358ull; // cmp [r13+118h],rcx: owner handle vs Instance +0x118
// Enemy guns fire FX projectiles (PROJECTILE_FireFxProjectile: particles that fly and collide; no enemy bullet went
// through FireInstant in the v0.9.242 test). The FX system asks FxGameFilterCollisions(Instance* owner, bool, bool,
// bool, bool filterAllies, bool skip, LineProbeResult*) whether a particle's hit counts (the game lets its allies' shots
// pass); the stored pointer [0x141A7A880] is called from the particle collision code (0x31DBCF, 0x3285ED, 0x329379).
constexpr std::uintptr_t kRvaFxFilterCollisions        = 0x00489B30ull;
// cdc::HealthComponent::ApplyDamage(DamageData*) -> bool (health +8 = its Instance; DamageData +0 damage, +4 type,
// +0x60 attacker handle, +0xB2 bit field: bit 8 causedByMeleeAttack).
constexpr std::uintptr_t kRvaApplyDamage               = 0x0022A0D0ull;
// Aim camera (TR2 names; retail offsets checked in ResetToDefaults 0x7E2FC0 and in the function below).
constexpr std::uintptr_t kRvaAimCamDo3Point           = 0x007C65B0ull; // AimCameraController::Do3PointInterpolations(float, int)
constexpr std::uintptr_t kRvaAimCamLeaMid             = 0x007C664Aull; // lea rdi,[rbx+620h]  (m_midPoint)
constexpr std::uintptr_t kRvaAimCamLeaLow             = 0x007C66A8ull; // lea rdi,[rbx+710h]  (m_lowPoint)
constexpr std::uintptr_t kRvaAimCamLeaHigh            = 0x007C67DEull; // lea rsi,[rbx+530h]  (m_highPoint)
constexpr std::uintptr_t kRvaAimCamDollyClamp         = 0x007C6E5Aull; // movss xmm0,[rbx+234h] (m_dollyMax), then [rbx+238h]
constexpr std::uintptr_t kRvaAimCamDollyStore         = 0x007C6E88ull; // movss [rbx+300h],xmm0 (clamped target dolly)

constexpr std::uint8_t kScPropertyGetBoolSig[] = {
    0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x0F,0xBF,0x41,0x02,0x45,0x32,0xC9,0x48,0x8B,0xDA,0x48,0x8B,0xD1,
    0x83,0xF8,0x63
};
constexpr std::uint8_t kSpatialProbeHitSig[] = {
    0x40,0x57,0x48,0x83,0xEC,0x30,0x48,0x89,0x5C,0x24,0x48,0x48,0x8B,0xD9,0x8B,0x4A,0x0C,0x8B,0xF9,0x48,
    0x89,0x74,0x24,0x58,0x49,0x63,0x70,0x0C
};
constexpr std::uint8_t kCheckConditionsSig[] = {
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xEC,0x20,
    0x0F,0xB7,0x5A,0x04,0x48,0x8B,0xF1
};
constexpr std::uint8_t kGetCondPropertySig[] = {0x8B,0x01,0x83,0xC0,0xBE,0x83,0xF8,0x1B,0x77,0x55};
constexpr std::uint8_t kSetAnimGraphGoalSig[] = {
    0x40,0x53,0x48,0x83,0xEC,0x30,0x48,0x8B,0xD9,0x48,0x63,0xCA,0x44,0x8B,0x8C,0x8B,0xB4,0x04,0x00,0x00,
    0x45,0x3B,0xC8
};
// cmp ax,7 ... mov r8d,[rbx+10h]; xor edx,edx; ... jmp SetAnimGraphGoal: the moveset goal is the
// property's u32 at +0x10 and goal type 0.
constexpr std::uint8_t kGoalMovesetPropFunctionSig[] = {
    0x48,0x89,0x5C,0x24,0x20,0x57,0x48,0x83,0xEC,0x20,0x0F,0xB7,0x42,0x02,0x48,0x8B,0xDA,0x48,0x8B,0xF9,
    0x66,0x83,0xF8,0x07
};
constexpr std::uint8_t kSymbolCStrSig[] = {0x0F,0xB7,0x11,0x48,0x8B,0x0D};
constexpr std::uint8_t kFireInstantSig[] = {
    0x48,0x8B,0xC4,0x48,0x89,0x50,0x10,0x48,0x89,0x48,0x08,0x55,0x41,0x56,0x48,0x8D,0x68,0x98,0x48,0x81,0xEC,0x58,0x01,0x00
};
constexpr std::uint8_t kFireInstantOwnerCheckSig[] = {0x49,0x39,0x8D,0x18,0x01,0x00,0x00};
constexpr std::uint8_t kFxFilterCollisionsSig[] = {
    0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x80,0x7C,0x24,0x50,0x00,0x48,0x8B,0xF1,0x40,0xB7,0x01,0x74,0x67,0x80
};
constexpr std::uint8_t kApplyDamageSig[] = {
    0x40,0x55,0x53,0x57,0x48,0x8D,0x6C,0x24,0xF0,0x48,0x81,0xEC,0x10,0x01,0x00,0x00,0x48,0x8B,0xF9,0x48,0x8B,0xDA,0x48,0x8B
};
constexpr std::uint8_t kTimeDilationRequestCtorSig[] = {
    0x89,0x11,0x0F,0x57,0xC0,0xF3,0x0F,0x11,0x51,0x04,0xF3,0x0F,0x11,0x59,0x08,0x66,0xC7,0x41,0x0C,0x00,0x01
};
constexpr std::uint8_t kTimeDilationRequestSig[] = {
    0x4C,0x63,0x12,0x4C,0x8B,0xD9,0x44,0x8B,0x89,0x18,0x01,0x00,0x00,0x0F,0x10,0x02,0x4D,0x6B,0xC2,0x1C
};
constexpr std::uint8_t kTimeDilationCancelSig[] = {
    0x48,0x63,0xC2,0x0F,0x57,0xC0,0x48,0x6B,0xD0,0x1C,0x48,0x03,0xD1,0xC7,0x02,0x0A,0x00,0x00,0x00
};
constexpr std::uint8_t kSetTimeDilationManagerSig[] = {0x48,0x8D,0x0D};           // + rel32
constexpr std::uint8_t kInventoryPauseScIdSig[] = {0x8B,0x0D};                    // + rel32
constexpr std::uint8_t kGetLocalPlayerInstanceSig[] = {0x40,0x53,0x48,0x83,0xEC,0x20,0x0F,0xB6,0xD9};
constexpr std::uint8_t kDoTransitionSig[] = {
    0x48,0x89,0x5C,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x20,0x48,
    0x8B,0xFA
};
constexpr std::uint8_t kSteeringProcessSig[] = {
    0x48,0x8B,0xC4,0x55,0x53,0x57,0x48,0x8D,0x6C,0x24,0x80,0x48,0x81,0xEC,0x80,0x01,0x00,0x00,0x0F,0x29,
    0x70,0xD8
};
constexpr std::uint8_t kSteeringSetTargetDirSig[] = {
    0x48,0x83,0xEC,0x18,0x0F,0x28,0x1A,0x0F,0x28,0xD3,0x0F,0x29,0x34,0x24,0x0F,0x59,0xD3,0x0F,0x57,0xF6
};
constexpr std::uint8_t kSteeringSetTurnParamsSig[] = {
    0x48,0x83,0xEC,0x28,0x0F,0xB6,0x44,0x24,0x58,0x0F,0x57,0xE4,0x33,0xD2,0x0F,0x29,0x74,0x24,0x10,0x0F,
    0x29,0x3C,0x24
};
constexpr std::uint8_t kAnimBuildTransformsSig[] = {
    0x48,0x89,0x5C,0x24,0x18,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x48,0x83,0xEC,0x40,0x48,0x8B,0xBC,0x24,0x90,0x00,0x00,0x00
};
constexpr std::uint8_t kCameraForwardSig[] = {
    0x48,0x83,0xB9,0x98,0x00,0x00,0x00,0x00,0x4C,0x8B,0xCA,0x74,0x42,0x33,0xD2,0x41,0x8B,0xC0,0x48,0xF7,
    0xB1,0xA0,0x00,0x00,0x00
};
constexpr std::uint8_t kSendEventByNameSig[] = {
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x40,0x49,0x8B,0xF8,0x48,0x8B,
    0xF1,0x4C,0x8B,0xC2
};
constexpr std::uint8_t kMovRcxRipSig[] = {0x48,0x8B,0x0D};   // + rel32
constexpr std::uint8_t kCallRel32Sig[] = {0xE8};             // + rel32
constexpr std::uint8_t kJmpRel32Sig[] = {0xE9};              // + rel32
// cmp byte [rdx+8],0; mov rax,[rcx+10h]; mov r8d,[ComponentArrayID<PlayerComponent>::s_id]
constexpr std::uint8_t kSetAccurateAimSig[] = {0x80,0x7A,0x08,0x00,0x48,0x8B,0x41,0x10,0x44,0x8B,0x05};
constexpr std::uint8_t kGetPropertyDataSig[] = {
    0x48,0x8B,0x81,0xB0,0x02,0x00,0x00,0x44,0x0F,0xB7,0xDA,0x45,0x33,0xC9,0x48,0x85,0xC0
};
constexpr std::uint8_t kVisionEnableSig[] = {
    0x40,0x55,0x56,0x57,0x48,0x81,0xEC,0x80,0x00,0x00,0x00,0x0F,0x29,0x74,0x24,0x60,0x0F,0x28,0xF3,0x41,
    0x8B,0xE8
};
constexpr std::uint8_t kVisionDisableSig[] = {
    0x40,0x53,0x41,0x56,0x48,0x83,0xEC,0x68,0x83,0x39,0x01,0x48,0x8B,0xDA,0x4C,0x8B,0xF1
};
constexpr std::uint8_t kInstinctHighlightSig[] = {
    0x40,0x56,0x57,0x48,0x83,0xEC,0x78,0x48,0x8B,0xF2,0x48,0x8B,0xF9,0x48,0x85,0xD2,0x75,0x09,0x32,0xC0
};
constexpr std::uint8_t kAsymInstinctHighlightSig[] = {
    0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xFA,0x48,0x8B,0xD9
};
constexpr std::uint8_t kAddAsymComponentsSig[] = {
    0x40,0x56,0x48,0x83,0xEC,0x20,0x8B,0x41,0x14,0x48,0x8B,0xF1,0xFF,0xC8,0x83,0xF8,0x01,0x0F,0x87
};
constexpr std::uint8_t kNotifyDialogueContextSig[] = {0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x4C,0x8B,0xC2};
constexpr std::uint8_t kAvoidClippingAimingSig[] = {
    0x48,0x8B,0xC4,0x55,0x53,0x57,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xA8,0x48,0xFE,0xFF,0xFF,0x48,
    0x81,0xEC,0x88,0x02,0x00,0x00
};
constexpr std::uint8_t kAvoidClippingProcessSig[] = {0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0xE8};
constexpr std::uint8_t kAimCamDo3PointSig[] = {
    0x48,0x8B,0xC4,0x53,0x48,0x81,0xEC,0x90,0x00,0x00,0x00,0x8B,0x91,0x04,0x09,0x00,0x00,0x48,0x8B,0xD9
};
constexpr std::uint8_t kAimCamLeaMidSig[] = {0x48,0x8D,0xBB,0x20,0x06,0x00,0x00};
constexpr std::uint8_t kAimCamLeaLowSig[] = {0x48,0x8D,0xBB,0x10,0x07,0x00,0x00};
constexpr std::uint8_t kAimCamLeaHighSig[] = {0x48,0x8D,0xB3,0x30,0x05,0x00,0x00};
constexpr std::uint8_t kAimCamDollyClampSig[] = {
    0xF3,0x0F,0x10,0x83,0x34,0x02,0x00,0x00,0xF3,0x41,0x0F,0x59,0xD3,0xF3,0x41,0x0F,0x58,0xD0,0x0F,0x28,
    0xCA,0x0F,0x28,0xC8,0xF3,0x0F,0x10,0x83,0x38,0x02,0x00,0x00
};
constexpr std::uint8_t kAimCamDollyStoreSig[] = {0xF3,0x0F,0x11,0x83,0x00,0x03,0x00,0x00};

// AimCameraController (retail = TR2 layout): three AimThreePointSet (0xF0 bytes) whose dolly is a
// Smoother<float> at +0x1C {from, to, curve, t}; Do3PointInterpolations blends them by pitch, clamps the
// result to [m_dollyMin, m_dollyMax] and stores the target dolly at +0x300 that DoPosition and the aim
// camera collision then use.
constexpr std::size_t kAimCamPoints[3] = {0x530, 0x620, 0x710};   // high, mid, low
constexpr std::size_t kAimCamPointDollyFrom = 0x1C;
constexpr std::size_t kAimCamPointDollyTo = 0x20;
constexpr std::size_t kAimCamDollyMax = 0x234;
constexpr std::size_t kAimCamDollyMin = 0x238;
constexpr std::size_t kAimCamTargetDolly = 0x300;

// Instance components (InstanceManager slot layout).
constexpr std::size_t kInstanceComponentCount = 0xD0;
constexpr std::size_t kInstanceComponents = 0xD8;
// SCAgent / StateControlComponent / dtp::SCAgentData / dtp::SCStateData (retail).
constexpr std::size_t kAgentComponent = 0x00;       // SCAgent::m_pStateControlComponent
constexpr std::size_t kAgentData = 0x08;            // SCAgent::m_scAgentData
constexpr std::size_t kAgentInstance = 0x10;        // SCAgent::m_instance
constexpr std::size_t kAgentFramesInState = 0x618;  // float, reset by DoTransition, read by Frames_InState*
// Aim pitch / yaw of the camera relative to Lara (radians), written by _Combat_AvoidWeaponClipping_Aiming
// on the first agent and read by the anim graph (kAnimGraphRTValues_Player_Aim_Pitch / _Yaw, PlayerAG_Camera).
constexpr std::size_t kAgentAimPitch = 0x580;
constexpr std::size_t kAgentAimYaw = 0x584;
constexpr std::size_t kAgentCurrentState = 0x1C10;  // SCAgent::m_currentState
constexpr std::size_t kComponentDtp = 0x2A8;        // StateControlComponent::m_pDTPData (+8: agent count)
constexpr std::size_t kComponentBaseAgent = 0x2B0;  // StateControlComponent::m_pBaseAgent
constexpr std::size_t kComponentAgents = 0x2C0;     // StateControlComponent::m_pSCAgent data
constexpr std::size_t kAgentDataNumStates = 0x18;   // dtp::SCAgentData::miNumStates
constexpr std::size_t kAgentDataStateRefs = 0x20;   // dtp::SCAgentData::mpSCStateRefList
constexpr std::size_t kAgentDataName = 0x50;        // dtp::SCAgentData::SCAgentDebugName
constexpr std::size_t kStateParent = 0x00;          // dtp::SCStateData::miParentStateRef
constexpr std::size_t kStateName = 0x38;            // dtp::SCStateData::mpDebugName
// Player (retail PlayerComponent+0x2A8) and its Adrenaline.
constexpr std::size_t kPlayerComponentPlayer = 0x2A8;
// Player::m_steering (PlayerSteering, rotates Lara towards its target orientation at its turn rate) and
// the id of the player's camera (PSCAF_P_Turn_CameraDir passes it to GetActiveCamera_Forward).
constexpr std::size_t kPlayerSteering = 0x3430;
constexpr std::size_t kPlayerCameraId = 0x3258;
// Player+0x31B0 CombatManager; its +0x4A0 flags, bit 0x800 = accurate aim (IsInAccurateAim).
constexpr std::size_t kPlayerCombatManager = 0x31B0;
constexpr std::size_t kCombatFlags = 0x4A0;
constexpr std::uint32_t kCombatAccurateAimBit = 0x800;
// PlayerSteering: +0 m_instance, +0xC m_steeringMode (dtp::EnumPlayerSteeringMode, 1 = Yaw), +0x91
// m_targetOrientationDirty, +0x1D4 m_deltaAnglesDirty (what SetSteeringMode writes).
constexpr std::size_t kSteeringInstance = 0x00;
constexpr std::size_t kSteeringMode = 0x0C;
constexpr std::size_t kSteeringTargetDirty = 0x91;
constexpr std::size_t kSteeringDeltaDirty = 0x1D4;
constexpr std::int32_t kSteeringModeYaw = 1;
// VisionMode (the survival instinct view): +0 mStatus (0 off), +0x7B m_BeaconTurnedOn (the objective
// indicator Enable opens). Mode 1 = Instinct.
constexpr std::size_t kVisionStatus = 0x00;
constexpr std::size_t kVisionBeacon = 0x7B;
constexpr int kVisionModeInstinct = 1;
constexpr float kVisionSeconds = 3600.0f;     // ours ends with the slow motion
constexpr const char* kInstinctDialogueContext = "OnSurvivalInstinctsActivated";

// dtp::EnumSCStateProperty values answered in the air.
constexpr std::int16_t kPropLockOnAllowed = 483;
constexpr std::int16_t kPropCombatAllowed = 488;
constexpr std::int16_t kPropDualGunsAllowed = 490;
constexpr std::int16_t kPropAccurateAim = 499;
constexpr std::int16_t kPropTypeBoolean = 0x10;
constexpr std::int16_t kPropInventoryPause = 531;
constexpr std::int16_t kPropTwoHandedGunsAllowed = 544;
constexpr std::int16_t kPropUnequipItemInInvPause = 545;
constexpr std::int16_t kPropIncomingMeleeDisabled = 751;  // IncomingAttack_MeleeDisabled
constexpr std::int16_t kPropIncomingRangeDisabled = 752;  // IncomingAttack_RangeDisabled
constexpr ULONGLONG kAcroProtectGraceMs = 300;            // after a roll / evade ends
// dtp::SCConditionData (0x28): +0 context, +8 property (a symbol for the named-value contexts),
// +0xC value (the probe index for Physics_SpatialProbeHit), +0x10 bit 0 NOT.
constexpr std::size_t kConditionSize = 0x28;
constexpr std::size_t kConditionContext = 0x00;
constexpr std::size_t kConditionProperty = 0x08;
constexpr std::size_t kConditionValue = 0x0C;
constexpr std::size_t kConditionFlags = 0x10;
constexpr std::int32_t kGroundProbe = 40;
// dtp::SCTransitionData (0x28): +0 target state index (i16, <0 none), +4 condition count,
// +0x10 conditions, +0x18 debug name.
constexpr std::size_t kTransitionTarget = 0x00;
constexpr std::size_t kTransitionNumConditions = 0x04;
constexpr std::size_t kTransitionConditions = 0x10;
constexpr std::size_t kTransitionName = 0x18;
// SCAgent::m_enumData: 0x80 per condition property, +8 the condition function, +0x60 its memory.
constexpr std::size_t kAgentConditionTable = 0x1C40;
constexpr std::size_t kConditionEntrySize = 0x80;
constexpr std::size_t kConditionEntryFunction = 0x08;
constexpr std::size_t kConditionEntryMemory = 0x60;
// dtp::EnumSCCondContext / EnumSCCondProperty values the draw override looks at.
constexpr std::int32_t kContextNamedValueBoolean = 66;
constexpr std::int16_t kCondAnimCriticalSection = 60;
constexpr std::int16_t kCondStatusLowCeiling = 165;
constexpr std::int16_t kCondStatusIsFalling = 166;
constexpr std::int16_t kCondInvStateTimerElapsed = 730;
constexpr std::int16_t kCondPrimaryOverrideVar = 461;
constexpr std::int16_t kCondInventoryStateNoProcess = 555;
constexpr std::int16_t kCondSpatialProbeHit = 620;
constexpr std::int16_t kCondInteractedIsInteractableType = 658;
constexpr std::int16_t kCondExperiencePlayerTrait = 828;     // ctx Experience; value = a dtp::EnumSCCondComparison trait
constexpr std::int32_t kTraitConvexTurn = 123;               // MarkupLookahead_ConvexTurn (a downhill / convex slope ahead)
constexpr std::int16_t kCondFramesInState = 5;                // Frames_InState: the agent's frames in its state >= a tuning value
constexpr std::int16_t kCondMeleeCounterWindow = 510;         // Combat_MeleeTargetInCounterAllowedWindow
constexpr std::int16_t kCondProbeToMeleeTargetClear = 827;    // Combat_HardCodedProbeToMeleeTargetClear
// AnimComponentV2 goals (retail +0x4B4, dtp::AnimGraphGoalRTValues; 0 = moveset) and the moveset goal
// values the state control writes (the u32 of its AGGoalMoveset properties).
constexpr std::size_t kAnimComponentOwner = 0x08;
constexpr std::size_t kAnimComponentGoals = 0x4B4;
constexpr int kGoalMoveset = 0;
constexpr std::uint32_t kMovesetInAir = 0x8F89FE48u;
constexpr std::uint32_t kMovesetCombat = 0xA24C8D11u;

constexpr int kTimeDilationTypeDefault = 9;
constexpr int kTimeDilationTypeNone = 10;
constexpr std::size_t kTimeDilationRequestSize = 0x1C;
constexpr const char* kSlowMotionName = "ROTTRFramework slow motion";
constexpr float kTriggerHeld = 0.3f;

// TimeDilationRequest (0x1C).
struct TimeDilationRequestRaw {
    std::int32_t priority;   // dtp::TimeDilationType; 10 = no request
    float worldFactor;       // m_data: everything but the player and the exempt instances
    float globalFactor;      // m_data: everything
    std::uint8_t forceActive;
    std::uint8_t dirty;
    std::uint8_t pad[2];
    float startRampTime;
    float rampTime;
    float activateTime;
};
static_assert(sizeof(TimeDilationRequestRaw) == kTimeDilationRequestSize, "TimeDilationRequest is 0x1C bytes");

using ScPropertyGetBoolFn = bool(__fastcall*)(const void* property, void* agent);
using SpatialProbeHitFn = bool(__fastcall*)(void* agent, void* memory, const std::uint8_t* condition);
using CheckConditionsFn = bool(__fastcall*)(void* agent, const std::uint8_t* transition);
using GetCondPropertyFn = int(__fastcall*)(const std::uint8_t* condition);
using ConditionFn = bool(__fastcall*)(void* agent, void* memory, const std::uint8_t* condition);
using SymbolCStrFn = const char*(__fastcall*)(const std::uint16_t* symbol);
using SetAnimGraphGoalFn = bool(__fastcall*)(void* animComponent, int type, std::uint32_t value);
using TimeDilationRequestCtorFn = void*(__fastcall*)(void* request, int type, float world, float global, const char* name);
using TimeDilationRequestFn = void(__fastcall*)(void* manager, const void* request);
using TimeDilationCancelFn = void(__fastcall*)(void* manager, int type, const char* name);
using GetLocalPlayerInstanceFn = void*(__fastcall*)(std::uint8_t);
using DoTransitionFn = void(__fastcall*)(void* agent, const std::uint8_t* transition);
using SteeringProcessFn = void(__fastcall*)(void* steering);
using SteeringSetTargetDirFn = void(__fastcall*)(void* steering, const float* direction);
using SteeringSetTurnParamsFn = void(__fastcall*)(void* steering, float rate, float accel, float decel, float timeLimit,
                                                  bool anyDirection, int curve, const void* curveOption);
using CameraForwardFn = float*(__fastcall*)(void* manager, float* out, std::uint32_t cameraId);
using SendEventByNameFn = void(__fastcall*)(void* eventSystem, const char* name, void* instance, int arg, void* data,
                                            std::uint64_t extra);
using SetAccurateAimFn = void(__fastcall*)(void* agent, const std::uint8_t* property);
using GetPropertyDataFn = const std::uint8_t*(__fastcall*)(void* component, std::uint16_t property);
using VisionEnableFn = void(__fastcall*)(void* vision, void* player, int mode, float seconds);
using VisionDisableFn = void(__fastcall*)(void* vision, void* player);
using InstinctHighlightFn = bool(__fastcall*)(void* component, void* player);
using AddAsymComponentsFn = void(__fastcall*)(void* vision);
using NotifyDialogueContextFn = void(__fastcall*)(void* manager, const char* context);
using AvoidClippingAimingFn = void(__fastcall*)(void* agent);
using AimCamDo3PointFn = void(__fastcall*)(void* controller, float dt, int flags);
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

enum class SlowButton : int { LB = 0, RB, LeftStick, RightStick, Back, DPadUp, DPadDown, DPadLeft, DPadRight, Count };
struct SlowButtonChoice {
    const char* name;
    WORD mask;
};
constexpr SlowButtonChoice kSlowButtons[] = {
    {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER},
    {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {"Left stick click (L3)", XINPUT_GAMEPAD_LEFT_THUMB},
    {"Right stick click (R3)", XINPUT_GAMEPAD_RIGHT_THUMB},
    {"Back / View", XINPUT_GAMEPAD_BACK},
    {"D-pad up", XINPUT_GAMEPAD_DPAD_UP},
    {"D-pad down", XINPUT_GAMEPAD_DPAD_DOWN},
    {"D-pad left", XINPUT_GAMEPAD_DPAD_LEFT},
    {"D-pad right", XINPUT_GAMEPAD_DPAD_RIGHT},
};
static_assert(std::size(kSlowButtons) == static_cast<std::size_t>(SlowButton::Count), "button table");

std::uintptr_t g_base = 0;
ScPropertyGetBoolFn g_originalGetBool = nullptr;
SpatialProbeHitFn g_originalSpatialProbeHit = nullptr;
CheckConditionsFn g_originalCheckConditions = nullptr;
GetCondPropertyFn g_getCondProperty = nullptr;
SymbolCStrFn g_symbolCStr = nullptr;
SetAnimGraphGoalFn g_originalSetAnimGraphGoal = nullptr;
TimeDilationRequestCtorFn g_requestCtor = nullptr;
TimeDilationRequestFn g_request = nullptr;
TimeDilationCancelFn g_cancel = nullptr;
GetLocalPlayerInstanceFn g_getLocalPlayerInstance = nullptr;
DoTransitionFn g_originalDoTransition = nullptr;
SteeringProcessFn g_originalSteeringProcess = nullptr;
SteeringSetTargetDirFn g_setTargetDir = nullptr;
SteeringSetTurnParamsFn g_setTurnParams = nullptr;
CameraForwardFn g_cameraForward = nullptr;
void* const* g_cameraManager = nullptr;       // &CameraManager::m_pCameraManager
SendEventByNameFn g_sendEvent = nullptr;
void* const* g_eventSystem = nullptr;         // &EventSystem::s_instance
SetAccurateAimFn g_originalSetAccurateAim = nullptr;
GetPropertyDataFn g_getPropertyData = nullptr;
VisionEnableFn g_visionEnable = nullptr;
VisionDisableFn g_visionDisable = nullptr;
std::uint8_t* const* g_visionMode = nullptr;  // &VisionMode::mInstance
InstinctHighlightFn g_originalInstinctHighlight = nullptr;
InstinctHighlightFn g_originalAsymInstinctHighlight = nullptr;
AddAsymComponentsFn g_originalAddAsymComponents = nullptr;
NotifyDialogueContextFn g_originalNotifyDialogueContext = nullptr;
AvoidClippingAimingFn g_avoidClippingAiming = nullptr;
AimCamDo3PointFn g_originalAimCamDo3Point = nullptr;
std::uint8_t* g_timeDilationManager = nullptr;
std::uint32_t g_scComponentIdRva = 0;
bool g_airAimAvailable = false;
bool g_groundProbeAvailable = false;
bool g_drawCheckAvailable = false;
bool g_stanceAvailable = false;
bool g_traceAvailable = false;
bool g_facingAvailable = false;
bool g_accurateAimAvailable = false;
bool g_aimAnglesAvailable = false;
bool g_slowAvailable = false;
bool g_soundAvailable = false;
bool g_instinctAvailable = false;
bool g_aimCamAvailable = false;

// Settings.
std::atomic_bool g_airAimEnabled{false};   // (v1.0.0: these five off by default)
std::atomic_bool g_slowEnabled{false};
std::atomic<int> g_slowButton{static_cast<int>(SlowButton::LB)};
std::atomic_bool g_slowHold{false};
std::atomic_bool g_slowOnlyAiming{true};
std::atomic_bool g_airCombatStance{true};
std::atomic_bool g_airFaceCamera{true};
std::atomic<float> g_airTurnSpeed{540.0f};      // degrees per second
std::atomic_bool g_airFastFire{true};
std::atomic<float> g_airFireIntervalMs{110.0f};
std::atomic_bool g_dodgeFire{false};             // shoot during the combat evades (aim held), like in the air
std::atomic_bool g_beamFire{false};              // (v0.9.473) draw and shoot on beams (aim held), like in the air
std::atomic_bool g_beamLegs{true};              // (v0.9.475; v0.9.480: the upper-body mode) BeamAimLegs
std::atomic<float> g_beamTurnMaxDeg{90.0f};     // (v0.9.475; v0.9.479: 90, no setting) the upper body's turn with the camera on a beam
std::atomic_bool g_dodgeAimIk{true};            // the arms point along the aim during an aimed combat evade (IK)
// Aim IK runtime: the tick's target weight and aim direction (world), the hook's current weight.
bool g_ikAvailable = false;
std::atomic<float> g_ikTarget{0.0f};
std::atomic<float> g_ikDirX{0.0f}, g_ikDirY{-1.0f}, g_ikDirZ{0.0f};
std::atomic<float> g_ikWeight{0.0f};
std::atomic<long long> g_ikLastTicks{0};
std::atomic_bool g_ikSkeletonChecked{false};
std::atomic_bool g_ikSkeletonOk{false};
std::atomic<std::uint32_t> g_ikFrames{0};
std::atomic<float> g_ikAimX{0.0f}, g_ikAimY{0.0f}, g_ikAimZ{0.0f};  // the last aim in Lara's space (trace)
std::atomic_bool g_slowSound{true};
std::atomic_bool g_slowInstinct{true};
char g_slowSoundEvent[64] = "";
char g_slowSoundEndEvent[64] = "";
std::atomic<float> g_slowWorldSpeed{0.30f};
std::atomic<float> g_slowLaraSpeed{0.70f};
std::atomic<float> g_slowMaxSeconds{0.0f};
// Aim camera further back (scales the aim camera's dolly = distance along its view axis).
std::atomic_bool g_aimCamFarther{false};
std::atomic_bool g_aimCamPistolsOnly{true};
// v0.9.263: with the SMG / rifle and the shotgun the aim camera stands where the pistols' does (their dolly values
// instead of the weapon's own); the bow keeps its own camera.
std::atomic_bool g_aimCamLikePistols{true};
// The pistols' dolly (high, mid, low) and limits as the game has them: its known values until a pistol is aimed.
std::atomic<float> g_pistolDolly[3] = {{72.0f}, {90.0f}, {90.0f}};
std::atomic<float> g_pistolDollyMin{30.0f}, g_pistolDollyMax{1000.0f};
std::atomic_bool g_pistolDollyLearned{false};
std::atomic<std::uint32_t> g_aimCamClassLogged{0};
std::atomic<float> g_aimCamScale{1.6f};
std::atomic<float> g_aimCamStatusTarget{0.0f};
std::atomic_bool g_aimCamStatusActive{false};

// Read by the property hook (any thread that runs the state control).
std::atomic<void*> g_laraInstance{nullptr};
std::atomic_bool g_airAimArmed{false};
// The grapple axe has Lara's arms (an agent other than the primary in a grappleaxe_* state): no air aim overrides then.
// Seen in v0.9.270: jump at a wall, X, the axe cannot bite (grappleaxe_pre_throw_facing_wall ->
// grappleaxe_no_wallclimb_no_attach), aim held -> the combat stance and the combat permissions pulled the weapon agent out
// of the axe (default_to_combat_wp_equip) while the axe kept re-entering its state: Lara curled up into a ball and
// flickered until the aim was let go.
std::atomic_bool g_airAimPaused{false};
std::atomic<std::uint32_t> g_airOverrides{0};
// Roll chain on a downhill: since when its "falling" exits have been held back (GetTickCount64, 0 = not now).
std::atomic<unsigned long long> g_chainFallSince{0};
std::atomic<unsigned long long> g_chainFallLast{0};
std::atomic<std::uint32_t> g_chainFallLogs{0};
constexpr unsigned long long kChainFallHoldMs = 400;
std::atomic<std::uint32_t> g_groundProbeOverrides{0};
std::atomic<std::uint32_t> g_drawForced{0};
// Combat stance in the air: Lara's AnimComponentV2 (seen in the goal setter), the moveset the state
// control last asked for, and whether "InAir" is answered with "Combat".
std::atomic<void*> g_laraAnimComponent{nullptr};
std::atomic<std::uint32_t> g_scMovesetGoal{0};
// (v0.9.479) AnimComponentV2 goal types (dtp::AnimGraphGoalRTValues, by the SC property functions that set them: TR2
// SCPropFunction_SetAnimGraph_Goal_Cluster 1, _State 2, _Overlay 3, _Mainhand 4, _Offhand 5; moveset 0)
constexpr int kGoalCluster = 1, kGoalState = 2, kGoalTypes = 6;
std::atomic_bool g_stanceOnBeam{false};   // (v0.9.473) the combat stance is for a beam: any moveset the SC asks becomes Combat
std::atomic_bool g_stanceActive{false};
std::atomic<std::uint32_t> g_stanceSwaps{0};
// Facing the camera and firing faster while aiming in the air.
std::atomic_bool g_airFacingActive{false};
std::atomic<std::uint32_t> g_airFacingFrames{0};
std::atomic<ULONGLONG> g_lastAirShotMs{0};
std::atomic<std::uint32_t> g_airShots{0};
std::atomic<std::uint32_t> g_airFireLetThrough{0};
// Accurate aim (CombatManager+0x4A0 bit 0x800: the camera aim probe that places the aim target under the
// reticle, which the arms and the shots use) kept on while aiming in the air.
std::atomic_bool g_airAccurateAim{false};
std::atomic<std::uint32_t> g_accurateAimKept{0};
alignas(16) constexpr std::uint8_t kAccurateAimOn[16] = {0xF3, 0x01, 0x10, 0x00, 0, 0, 0, 0, 1};    // 499, Boolean, true
alignas(16) constexpr std::uint8_t kAccurateAimOff[16] = {0xF3, 0x01, 0x10, 0x00, 0, 0, 0, 0, 0};   // 499, Boolean, false
// The survival instinct view started by the slow motion: no object highlights while it is ours, no
// objective indicator, no dialogue hint.
std::atomic_bool g_instinctOurs{false};
std::atomic_bool g_instinctNoHighlights{false};
std::atomic_bool g_instinctNoDialogue{false};
std::atomic<std::uint32_t> g_instinctHighlightsSkipped{0};
// Holster exits held back in the air (weapon agent).
std::atomic<std::uint32_t> g_unequipBlocked{0};

// Game thread.
XInputGetStateFn g_xinputGetState = nullptr;
bool g_xinputLoadTried = false;
DWORD g_padSlot = XUSER_MAX_COUNT;
// When the jump button (pad A / Space) went down, 0 while it is up (per-frame tick; the acrobatic climb-up wants a hold).
std::atomic<ULONGLONG> g_jumpDownSinceMs{0};
// Acrobatics protection: until when Lara counts as rolling / evading (updated every tick and on each read).
std::atomic<ULONGLONG> g_acroProtectUntilMs{0};
std::atomic<std::uint32_t> g_enemyShots{0};
std::atomic<std::uint32_t> g_shotsTurned{0};
std::atomic<std::uint32_t> g_shotLogs{0};
std::atomic<std::uint32_t> g_strafeDodgeLogs{0};
std::atomic<std::uint32_t> g_fxHitsOnLara{0};
std::atomic<std::uint32_t> g_fxHitsPassed{0};
std::atomic<std::uint32_t> g_fxLogs{0};
std::atomic<std::uint32_t> g_laraDamageLogs{0};
std::atomic<std::uint32_t> g_laraShotsCancelled{0};
std::atomic<std::uint32_t> g_acroProtectReads{0};
std::atomic<std::uint32_t> g_acroProtectAnswers{0};
std::atomic<std::uint32_t> g_acroProtectLogs{0};
ULONGLONG g_padNextScanMs = 0;
WORD g_padButtonsWas = 0;
bool g_slowActive = false;
ULONGLONG g_slowStartMs = 0;
float g_slowAppliedWorld = 0.0f;
float g_slowAppliedGlobal = 0.0f;
void* g_slowInstance = nullptr;
bool g_slowSlotLostLogged = false;
std::uint32_t g_airOverridesLogged = 0;

// Menu snapshot.
std::atomic_bool g_statusSlowActive{false};
std::atomic_bool g_statusPadConnected{false};
std::atomic_bool g_statusInAir{false};
std::atomic_bool g_statusOnBeam{false};   // (v0.9.473)

template <typename T>
T At(std::uintptr_t rva) {
    return reinterpret_cast<T>(g_base + rva);
}

bool Verify(std::uintptr_t rva, const std::uint8_t* sig, std::size_t size, const char* name) {
    if (GameBuild::VerifyBytes(rva, sig, size)) return true;
    Log::Writef("CombatAir: %s signature mismatch; related feature disabled", name);
    return false;
}

// Target of a rel32 operand that ends the instruction at rva + length.
std::uintptr_t RelTarget(std::uintptr_t rva, std::size_t operandOffset, std::size_t length) {
    const std::int32_t rel = *reinterpret_cast<const std::int32_t*>(g_base + rva + operandOffset);
    return static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rva + length) + rel);
}

const char* IniPath() {
    static char path[MAX_PATH]{};
    if (path[0]) return path;
    char root[MAX_PATH]{};
    GetModuleFileNameA(nullptr, root, MAX_PATH);
    char* slash = root;
    for (char* p = root; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p + 1;
    *slash = '\0';
    strcpy_s(path, root);
    strcat_s(path, "ROTTRFramework.ini");
    return path;
}

constexpr const char* kIniSection = "CombatExtras";

float IniFloat(const char* key, float fallback) {
    char def[32]{};
    sprintf_s(def, "%.6f", fallback);
    char buf[64]{};
    GetPrivateProfileStringA(kIniSection, key, def, buf, sizeof(buf), IniPath());
    const float value = static_cast<float>(std::atof(buf));
    return std::isfinite(value) ? value : fallback;
}

int IniInt(const char* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntA(kIniSection, key, fallback, IniPath()));
}

void IniWriteFloat(const char* key, float value) {
    char buf[32]{};
    sprintf_s(buf, "%.7g", value);
    WritePrivateProfileStringA(kIniSection, key, buf, IniPath());
}

void IniWriteInt(const char* key, int value) {
    char buf[32]{};
    sprintf_s(buf, "%d", value);
    WritePrivateProfileStringA(kIniSection, key, buf, IniPath());
}

void IniString(const char* key, const char* fallback, char* out, DWORD size) {
    GetPrivateProfileStringA(kIniSection, key, fallback, out, size, IniPath());
    // Event names only: letters, digits and underscores.
    for (char* p = out; *p; ++p) {
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
            *p = '\0';
            break;
        }
    }
}

void LoadSettings() {
    g_airAimEnabled.store(IniInt("AimInAir", 0) != 0);   // (v1.0.0: the features default off)
    g_airFaceCamera.store(IniInt("AirAimFaceCamera", 1) != 0);
    g_airTurnSpeed.store(std::clamp(IniFloat("AirAimTurnSpeed", 540.0f), 90.0f, 1440.0f));
    g_airFastFire.store(IniInt("AirFastFire", 1) != 0);
    g_airFireIntervalMs.store(std::clamp(IniFloat("AirFireIntervalMs", 110.0f), 40.0f, 500.0f));
    g_dodgeFire.store(IniInt("FireWhileDodging", 0) != 0);
    g_beamFire.store(IniInt("FireOnBeams", 0) != 0);
    g_beamLegs.store(IniInt("BeamAimLegs", 1) != 0);
    g_dodgeAimIk.store(IniInt("DodgeAimIK", 1) != 0);
    g_slowSound.store(IniInt("SlowMotionSound", 1) != 0);
    g_slowInstinct.store(IniInt("SlowMotionInstinctView", 1) != 0);
    IniString("SlowMotionSoundEvent", "", g_slowSoundEvent, sizeof(g_slowSoundEvent));
    IniString("SlowMotionSoundEndEvent", "", g_slowSoundEndEvent, sizeof(g_slowSoundEndEvent));
    g_slowEnabled.store(IniInt("SlowMotion", 0) != 0);
    // v0.9.205: the button went back to LB; a saved RB from before is moved once.
    int button = IniInt("SlowMotionButton", static_cast<int>(SlowButton::LB));
    if (IniInt("SlowMotionBindVersion", 1) < 2) {
        button = static_cast<int>(SlowButton::LB);
        IniWriteInt("SlowMotionButton", button);
        IniWriteInt("SlowMotionBindVersion", 2);
    }
    g_slowButton.store(button >= 0 && button < static_cast<int>(SlowButton::Count)
                           ? button : static_cast<int>(SlowButton::LB));
    g_slowHold.store(IniInt("SlowMotionHold", 0) != 0);
    g_slowOnlyAiming.store(IniInt("SlowMotionOnlyWhileAiming", 1) != 0);
    g_airCombatStance.store(IniInt("AirAimCombatStance", 1) != 0);
    g_slowWorldSpeed.store(std::clamp(IniFloat("SlowMotionWorldSpeed", 0.30f), 0.05f, 1.0f));
    g_slowLaraSpeed.store(std::clamp(IniFloat("SlowMotionLaraSpeed", 0.70f), 0.05f, 1.0f));
    g_slowMaxSeconds.store(std::clamp(IniFloat("SlowMotionMaxSeconds", 0.0f), 0.0f, 120.0f));
    g_aimCamFarther.store(IniInt("AimCameraFarther", 0) != 0);
    g_aimCamPistolsOnly.store(IniInt("AimCameraPistolsOnly", 1) != 0);
    g_aimCamLikePistols.store(IniInt("AimCameraLikePistols", 1) != 0);
    g_aimCamScale.store(std::clamp(IniFloat("AimCameraDistanceScale", 1.6f), 1.0f, 3.0f));
}

void SaveSettings() {
    IniWriteInt("AimInAir", g_airAimEnabled.load() ? 1 : 0);
    IniWriteInt("AirAimFaceCamera", g_airFaceCamera.load() ? 1 : 0);
    IniWriteFloat("AirAimTurnSpeed", g_airTurnSpeed.load());
    IniWriteInt("AirFastFire", g_airFastFire.load() ? 1 : 0);
    IniWriteFloat("AirFireIntervalMs", g_airFireIntervalMs.load());
    IniWriteInt("FireWhileDodging", g_dodgeFire.load() ? 1 : 0);
    IniWriteInt("FireOnBeams", g_beamFire.load() ? 1 : 0);
    IniWriteInt("BeamAimLegs", g_beamLegs.load() ? 1 : 0);
    IniWriteInt("DodgeAimIK", g_dodgeAimIk.load() ? 1 : 0);
    IniWriteInt("SlowMotionSound", g_slowSound.load() ? 1 : 0);
    IniWriteInt("SlowMotionInstinctView", g_slowInstinct.load() ? 1 : 0);
    WritePrivateProfileStringA(kIniSection, "SlowMotionSoundEvent", g_slowSoundEvent, IniPath());
    WritePrivateProfileStringA(kIniSection, "SlowMotionSoundEndEvent", g_slowSoundEndEvent, IniPath());
    IniWriteInt("SlowMotion", g_slowEnabled.load() ? 1 : 0);
    IniWriteInt("SlowMotionButton", g_slowButton.load());
    IniWriteInt("SlowMotionHold", g_slowHold.load() ? 1 : 0);
    IniWriteInt("SlowMotionOnlyWhileAiming", g_slowOnlyAiming.load() ? 1 : 0);
    IniWriteInt("AirAimCombatStance", g_airCombatStance.load() ? 1 : 0);
    IniWriteFloat("SlowMotionWorldSpeed", g_slowWorldSpeed.load());
    IniWriteFloat("SlowMotionLaraSpeed", g_slowLaraSpeed.load());
    IniWriteFloat("SlowMotionMaxSeconds", g_slowMaxSeconds.load());
    IniWriteInt("AimCameraFarther", g_aimCamFarther.load() ? 1 : 0);
    IniWriteInt("AimCameraPistolsOnly", g_aimCamPistolsOnly.load() ? 1 : 0);
    IniWriteInt("AimCameraLikePistols", g_aimCamLikePistols.load() ? 1 : 0);
    IniWriteFloat("AimCameraDistanceScale", g_aimCamScale.load());
}

bool GameWindowForeground() {
    HWND window = GetForegroundWindow();
    if (!window) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    return pid == GetCurrentProcessId();
}

// Game thread. The first connected XInput pad, rescanned at most every 2 s while none is found.
bool PollGamepad(XINPUT_STATE& state) {
    if (!g_xinputLoadTried) {
        g_xinputLoadTried = true;
        for (const wchar_t* name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
            HMODULE module = LoadLibraryW(name);
            if (!module) continue;
            g_xinputGetState = reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState"));
            if (g_xinputGetState) break;
        }
        Log::Detailf("CombatAir: XInput %s", g_xinputGetState ? "loaded" : "unavailable");
    }
    state = {};
    if (!g_xinputGetState) return false;
    if (g_padSlot < XUSER_MAX_COUNT && g_xinputGetState(g_padSlot, &state) == ERROR_SUCCESS) return true;
    g_padSlot = XUSER_MAX_COUNT;
    const ULONGLONG now = GetTickCount64();
    if (now < g_padNextScanMs) return false;
    g_padNextScanMs = now + 2000;
    for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
        if (g_xinputGetState(slot, &state) == ERROR_SUCCESS) {
            g_padSlot = slot;
            return true;
        }
    }
    state = {};
    return false;
}

std::uint8_t* InstanceComponent(std::uint8_t* instance, std::uint32_t id) {
    const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(instance + kInstanceComponentCount);
    auto** components = *reinterpret_cast<std::uint8_t***>(instance + kInstanceComponents);
    if (!components || id >= count) return nullptr;
    return components[id];
}

std::uint8_t* StateControlOf(std::uint8_t* instance) {
    if (!instance || !g_scComponentIdRva) return nullptr;
    return InstanceComponent(instance, *At<const std::uint32_t*>(g_scComponentIdRva));
}

bool StartsWith(const char* text, const char* prefix) {
    return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

enum class AirClass : std::uint8_t { Ground, Air, Excluded, Dodge, Roll, Beam };

// (v0.9.474) a balance beam state: Lara's state control names them balancebeam_... (balancebeam_parent, _idle_parent,
// _walk_parent, _noncombat_parent, _turn180, ...; read from the game's data, update2's player state control). 1: a beam
// state, -1: one with her hands busy (pull-ups, the saving grab, the ladder, the zipline, a rope), 0: none. (v0.9.473
// looked for beam... / ..._beam...: no state matched, so nothing changed on beams.)
int BeamNameClass(const char* name) {
    char lower[96];
    std::size_t n = 0;
    for (; name[n] && n + 1 < sizeof(lower); ++n)
        lower[n] = static_cast<char>(name[n] >= 'A' && name[n] <= 'Z' ? name[n] - 'A' + 'a' : name[n]);
    lower[n] = '\0';
    if (!std::strstr(lower, "balancebeam")) return 0;
    for (const char* busy : {"pullup", "saving_grab", "ladder", "zipline", "rope"})
        if (std::strstr(lower, busy)) return -1;
    return 1;
}

// Walks the state and its parents: a jump or fall parent makes it an in-air state; the no-guns falls
// and the ledge/rope attach and rescue states are left to the game. The combat evades (their parent
// "@_combat_evade_parent" turns combat off and sets Combat_PrimaryOverrideVar, which parks the weapon agent in
// combat_hitreact) are a class of their own: shooting during them is a separate option.
AirClass ClassifyState(const std::uint8_t* agentData, const std::uint8_t* state) {
    const std::uint16_t numStates = *reinterpret_cast<const std::uint16_t*>(agentData + kAgentDataNumStates);
    auto* const* refs = *reinterpret_cast<const std::uint8_t* const* const*>(agentData + kAgentDataStateRefs);
    for (int depth = 0; state && depth < 24; ++depth) {
        const char* name = *reinterpret_cast<const char* const*>(state + kStateName);
        if (name) {
            if (StartsWith(name, "fall_parent_noguns") || StartsWith(name, "fall_thrown") ||
                StartsWith(name, "jump_preattach") || StartsWith(name, "jump_quickattach") ||
                StartsWith(name, "jump_rescue"))
                return AirClass::Excluded;
            if (std::strcmp(name, "jump_prop_parent") == 0 ||
                std::strcmp(name, "fall_parent_no_transitions_or_events") == 0 ||
                std::strcmp(name, "fall_init_parent") == 0)
                return AirClass::Air;
            if (std::strcmp(name, "@_combat_evade_parent") == 0) return AirClass::Dodge;
            if (std::strcmp(name, "@_roll_parent") == 0) return AirClass::Roll;  // rolls / the roll chain
            if (const int beam = BeamNameClass(name)) return beam > 0 ? AirClass::Beam : AirClass::Excluded;   // (v0.9.474)
        }
        const std::uint16_t parent = *reinterpret_cast<const std::uint16_t*>(state + kStateParent);
        if (parent == 0xFFFF || parent >= numStates || !refs) break;
        state = refs[parent];
    }
    return AirClass::Ground;
}

struct AirCache {
    const void* state = nullptr;
    AirClass value = AirClass::Ground;
};
thread_local AirCache t_airCache;

// The class of the state of the primary agent of the component this agent belongs to.
AirClass PrimaryAgentClass(void* agent) {
#if defined(_MSC_VER)
    __try {
#endif
        auto* component = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(agent) + kAgentComponent);
        if (!component) return AirClass::Ground;
        auto* primary = *reinterpret_cast<std::uint8_t**>(component + kComponentBaseAgent);
        if (!primary) return AirClass::Ground;
        auto* state = *reinterpret_cast<const std::uint8_t**>(primary + kAgentCurrentState);
        if (!state) return AirClass::Ground;
        if (t_airCache.state != state) {
            auto* agentData = *reinterpret_cast<const std::uint8_t**>(primary + kAgentData);
            t_airCache.value = agentData ? ClassifyState(agentData, state) : AirClass::Ground;
            t_airCache.state = state;
        }
        return t_airCache.value;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t_airCache = {};
        return AirClass::Ground;
    }
#endif
}

// The state of the first agent other than the primary that is in a grappleaxe_* state (the grapple axe out), or null.
const char* GrappleAxeState(std::uint8_t* component) {
#if defined(_MSC_VER)
    __try {
#endif
        auto* dtp = *reinterpret_cast<const std::uint8_t* const*>(component + kComponentDtp);
        auto* const* agents = *reinterpret_cast<std::uint8_t* const* const*>(component + kComponentAgents);
        auto* primary = *reinterpret_cast<std::uint8_t* const*>(component + kComponentBaseAgent);
        const std::uint32_t count = dtp ? std::min(*reinterpret_cast<const std::uint32_t*>(dtp + 8), 8u) : 0u;
        for (std::uint32_t i = 0; agents && i < count; ++i) {
            const std::uint8_t* agent = agents[i];
            if (!agent || agent == primary) continue;
            auto* state = *reinterpret_cast<const std::uint8_t* const*>(agent + kAgentCurrentState);
            const char* name = state ? *reinterpret_cast<const char* const*>(state + kStateName) : nullptr;
            if (name && StartsWith(name, "grappleaxe")) return name;
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#endif
    return nullptr;
}

// Where the aim overrides apply: in the air, and in a combat evade when shooting while dodging is on.
bool PrimaryAgentInAir(void* agent) {
    const AirClass value = PrimaryAgentClass(agent);
    return value == AirClass::Air || (value == AirClass::Dodge && g_dodgeFire.load(std::memory_order_relaxed)) ||
           (value == AirClass::Beam && g_beamFire.load(std::memory_order_relaxed));
}

bool PrimaryAgentDodging(void* agent) {
    return g_dodgeFire.load(std::memory_order_relaxed) && PrimaryAgentClass(agent) == AirClass::Dodge;
}

// Acrobatics protection: Lara's IncomingAttack_MeleeDisabled read while she rolls or evades (or up to
// kAcroProtectGraceMs after). The game sets it (and IncomingAttack_RangeDisabled) in the seize / thrown states (and a
// hosted timer on the ground states) so enemies do not attack her then; answering true holds melee attacks off during
// the acrobatics. RangeDisabled is left to the game since v0.9.240 (v0.9.236 answered it too: the enemies stopped
// shooting at all); bullets are turned away instead (HookFireInstant).
bool AcroProtectedRead(const void* property, void* agent, bool& answer) {
    const std::int16_t id = *static_cast<const std::int16_t*>(property);
    if (id != kPropIncomingMeleeDisabled) return false;
    if (!Acro::AcrobaticsProtection() ||
        *reinterpret_cast<void**>(static_cast<std::uint8_t*>(agent) + kAgentInstance) != g_laraInstance.load(std::memory_order_relaxed))
        return false;
    const ULONGLONG now = GetTickCount64();
    const AirClass cls = PrimaryAgentClass(agent);
    if (cls == AirClass::Roll || cls == AirClass::Dodge)
        g_acroProtectUntilMs.store(now + kAcroProtectGraceMs, std::memory_order_relaxed);
    g_acroProtectReads.fetch_add(1, std::memory_order_relaxed);
    const bool protect = now < g_acroProtectUntilMs.load(std::memory_order_relaxed);
    static std::atomic<std::uint32_t> s_protectedLogs{0};
    if (protect ? s_protectedLogs.fetch_add(1, std::memory_order_relaxed) < 24
                : g_acroProtectLogs.fetch_add(1, std::memory_order_relaxed) < 4)
        Log::Detailf("CombatAir: Lara's %s read (caller +0x%llX, %s) -> %s",
                    id == kPropIncomingRangeDisabled ? "IncomingAttack_RangeDisabled" : "IncomingAttack_MeleeDisabled",
                    static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_base),
                    cls == AirClass::Roll ? "rolling" : cls == AirClass::Dodge ? "evading" : "not in acrobatics",
                    protect ? "true (acrobatics protection)" : "the game's own");
    if (!protect) return false;
    g_acroProtectAnswers.fetch_add(1, std::memory_order_relaxed);
    answer = true;
    return true;
}

bool __fastcall HookScPropertyGetBool(const void* property, void* agent) {
    if (property && agent) {
        bool answer = false;
        if (AcroProtectedRead(property, agent, answer)) return answer;
    }
    if (g_airAimArmed.load(std::memory_order_relaxed) && !g_airAimPaused.load(std::memory_order_relaxed) && property && agent) {
        int forced = -1;
        switch (*static_cast<const std::int16_t*>(property)) {
        case kPropLockOnAllowed:
        case kPropCombatAllowed:
        case kPropDualGunsAllowed:
        case kPropAccurateAim:
        case kPropTwoHandedGunsAllowed:
            forced = 1;
            break;
        case kPropInventoryPause:
        case kPropUnequipItemInInvPause:
            forced = 0;
            break;
        default:
            break;
        }
        // (Combat_PrimaryOverrideVar is left alone: the weapon agent leaves its evade state on the frame it turns
        // false, "NewlyFalse"; answering false all through the evade kept it there after the evade in v0.9.218.)
        if (forced >= 0 &&
            *reinterpret_cast<void**>(static_cast<std::uint8_t*>(agent) + kAgentInstance) ==
                g_laraInstance.load(std::memory_order_relaxed) &&
            PrimaryAgentInAir(agent)) {
            g_airOverrides.fetch_add(1, std::memory_order_relaxed);
            return forced == 1;
        }
    }
    return g_originalGetBool(property, agent);
}

// Not the primary agent: its falls choose short/long on this probe.
bool IsSecondaryAgent(void* agent) {
#if defined(_MSC_VER)
    __try {
#endif
        auto* component = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(agent) + kAgentComponent);
        return component && *reinterpret_cast<void**>(component + kComponentBaseAgent) != agent;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

bool __fastcall HookSpatialProbeHit(void* agent, void* memory, const std::uint8_t* condition) {
    if (g_airAimArmed.load(std::memory_order_relaxed) && !g_airAimPaused.load(std::memory_order_relaxed) && agent && condition &&
        *reinterpret_cast<const std::int32_t*>(condition + kConditionValue) == kGroundProbe &&
        (condition[kConditionFlags] & 1) == 0 &&
        *reinterpret_cast<void**>(static_cast<std::uint8_t*>(agent) + kAgentInstance) ==
            g_laraInstance.load(std::memory_order_relaxed) &&
        IsSecondaryAgent(agent) && PrimaryAgentInAir(agent)) {
        g_groundProbeOverrides.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return g_originalSpatialProbeHit(agent, memory, condition);
}

// What the air override does with a weapon-agent transition that failed in the air: the draws may
// pass when only ground-only conditions fail; a shot (ready -> fire) may pass before the weapon's fire
// interval and recoil animation are over once the air fire interval has passed; the end of a shot
// (fire -> cooldown, which waits for "NOT Status_IsFalling", so a pistol fired once per jump) passes
// in the air; the equip-to-ready step is only reported.
// In a combat evade the weapon agent also leaves combat_hitreact for ready as soon as it may (the evade's
// Combat_PrimaryOverrideVar parks it there).
enum class AirCheck : std::uint8_t { None, Draw, Fire, FireEnd, Report, OverrideExit, ChainGate, StrafeDodge };

AirCheck AirCheckFor(const char* name) {
    if (StartsWith(name, "default_to_combat_wp_equip")) return AirCheck::Draw;
    if (StartsWith(name, "combat_wp_ready_to_fire")) return AirCheck::Fire;
    if (std::strcmp(name, "combat_primary_override_to_combat_ready") == 0) return AirCheck::OverrideExit;
    if (std::strcmp(name, "combat_wp_fire_to_ready") == 0 || std::strcmp(name, "combat_wp_fire_to_ready_alt_ammo") == 0)
        return AirCheck::FireEnd;
    if (StartsWith(name, "combat_wp_equip_to_ready")) return AirCheck::Report;
    return AirCheck::None;
}

// Conditions that only say Lara is not standing on the ground (or, for the interacted object, that
// the jump target is set).
bool IsGroundOnlyCondition(const std::uint8_t* condition) {
    if (*reinterpret_cast<const std::int32_t*>(condition + kConditionContext) == kContextNamedValueBoolean) return true;
    switch (*reinterpret_cast<const std::int16_t*>(condition + kConditionProperty)) {
    case kCondSpatialProbeHit:
    case kCondStatusLowCeiling:
    case kCondPrimaryOverrideVar:
    case kCondInventoryStateNoProcess:
    case kCondInteractedIsInteractableType:
        return true;
    default:
        return false;
    }
}

struct AirTolerance {
    AirCheck check = AirCheck::None;
    bool fireIntervalDone = false;  // fast fire: the air fire interval has passed since the last shot
    bool halfIntervalDone = false;
    bool dodging = false;           // in a combat evade (shooting while dodging on)
};

bool ToleratedInAir(const std::uint8_t* condition, const AirTolerance& tolerance) {
    const std::int16_t property = *reinterpret_cast<const std::int16_t*>(condition + kConditionProperty);
    switch (tolerance.check) {
    case AirCheck::Draw:
        return IsGroundOnlyCondition(condition);
    case AirCheck::FireEnd:
        if (property == kCondStatusIsFalling) return true;
        // T359 "combat_wp_fire_to_ready" also waits for NOT Combat_PrimaryOverrideVar: one shot per evade
        if (property == kCondPrimaryOverrideVar && tolerance.dodging) return true;
        return property == kCondInvStateTimerElapsed && tolerance.halfIntervalDone;
    case AirCheck::Fire:
        if (property == kCondPrimaryOverrideVar && tolerance.dodging) return true;
        return (property == kCondInvStateTimerElapsed || property == kCondAnimCriticalSection) &&
               tolerance.fireIntervalDone;
    case AirCheck::OverrideExit:
        return property == kCondPrimaryOverrideVar && tolerance.dodging;
    case AirCheck::ChainGate:  // the roll chain's next move: the SC's own 18-frame wait and "NOT Experience_PlayerTrait
                               // MarkupLookahead_ConvexTurn" (see ChainGatesOnly)
        if (property == kCondFramesInState) return true;
        return property == kCondExperiencePlayerTrait &&
               *reinterpret_cast<const std::int32_t*>(condition + kConditionValue) == kTraitConvexTurn;
    case AirCheck::StrafeDodge:  // LB strafe + B: the combat evade without a melee counter window (see StrafeDodgeGatesOnly)
        return property == kCondMeleeCounterWindow || property == kCondProbeToMeleeTargetClear;
    default:
        return false;
    }
}

// One condition's function (guarded: a fault counts as failed).
bool CallCondition(ConditionFn fn, void* agent, std::uint8_t* memory, const std::uint8_t* condition, bool& faulted) {
    faulted = false;
#if defined(_MSC_VER)
    __try {
#endif
        return fn(agent, memory, condition);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return false;
    }
#endif
}

// Evaluates the transition's conditions in order the way SCAgent::CheckConditions does, a failing one that is tolerated
// in the air counting as passed, and lists the failing ones. True when some fail and all of them are tolerated.
// v0.9.337: it stops at the first failure that is not tolerated, as the game stops at the first failure. Before, it
// went on through every condition, so the ones after a guard ran without it: condition 738 (the item in inventory slot
// 0x14 in state 2..4, ROTTR.exe+0x6E6490) reads that item without checking that there is one, and with none faulted at
// ROTTR.exe+0x6E651B at each air aim start (0.9.334..0.9.336 logs; caught here, which also cut that tick's air check
// short).
bool AirFailures(void* agent, const std::uint8_t* transition, const AirTolerance& tolerance, char* report,
                 std::size_t size, int& failures) {
    failures = 0;
    int length = 0;
    report[0] = '\0';
    const std::uint16_t count = *reinterpret_cast<const std::uint16_t*>(transition + kTransitionNumConditions);
    auto* conditions = *reinterpret_cast<const std::uint8_t* const*>(transition + kTransitionConditions);
    if (!conditions || count == 0 || count > 40) return false;
    constexpr int kTableEntries = static_cast<int>((0x1D340 - kAgentConditionTable) / kConditionEntrySize);
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::uint8_t* condition = conditions + static_cast<std::size_t>(i) * kConditionSize;
        const int property = static_cast<std::int16_t>(g_getCondProperty(condition));
        bool passed = false;
        if (property >= 0 && property < kTableEntries) {
            auto* entry = static_cast<std::uint8_t*>(agent) + kAgentConditionTable +
                          static_cast<std::size_t>(property) * kConditionEntrySize;
            auto fn = *reinterpret_cast<ConditionFn*>(entry + kConditionEntryFunction);
            bool faulted = false;
            passed = fn && CallCondition(fn, agent, entry + kConditionEntryMemory, condition, faulted);
            if (faulted) return false;
        }
        if (passed) continue;
        ++failures;
        const bool tolerated = ToleratedInAir(condition, tolerance);
        const std::int32_t context = *reinterpret_cast<const std::int32_t*>(condition + kConditionContext);
        const std::int16_t raw = *reinterpret_cast<const std::int16_t*>(condition + kConditionProperty);
        const std::int32_t value = *reinterpret_cast<const std::int32_t*>(condition + kConditionValue);
        const bool negated = (condition[kConditionFlags] & 1) != 0;
        const char* symbol = nullptr;
        if (context >= kContextNamedValueBoolean && context <= kContextNamedValueBoolean + 4 && g_symbolCStr) {
            const std::uint16_t id = static_cast<std::uint16_t>(raw);
            symbol = g_symbolCStr(&id);
        }
        if (length < static_cast<int>(size) - 96)
            length += std::snprintf(report + length, size - length, "%s#%u ctx %d prop %d%s%s%s value %d%s",
                                    failures > 1 ? "; " : "", static_cast<unsigned>(i), context,
                                    static_cast<int>(raw), symbol ? " '" : "", symbol ? symbol : "",
                                    symbol ? "'" : "", value, negated ? " NOT" : "");
        if (!tolerated) return false;   // (as the game: the conditions after it are not asked)
    }
    return failures > 0;
}

const char* StateDebugName(const std::uint8_t* state) {
    return state ? *reinterpret_cast<const char* const*>(state + kStateName) : nullptr;
}

const char* StateName(const std::uint8_t* agent) {
    auto* state = *reinterpret_cast<const std::uint8_t* const*>(agent + kAgentCurrentState);
    if (!state) return "(none)";
    const char* name = *reinterpret_cast<const char* const*>(state + kStateName);
    return name ? name : "(unnamed)";
}

const char* AgentDebugName(const std::uint8_t* agentData) {
    return agentData ? *reinterpret_cast<const char* const*>(agentData + kAgentDataName) : nullptr;
}

// The state a transition leads to (nullptr when it names none).
const std::uint8_t* TransitionTarget(const std::uint8_t* agent, const std::uint8_t* transition) {
    const std::int16_t index = *reinterpret_cast<const std::int16_t*>(transition + kTransitionTarget);
    auto* agentData = *reinterpret_cast<const std::uint8_t* const*>(agent + kAgentData);
    if (index < 0 || !agentData) return nullptr;
    const std::uint16_t numStates = *reinterpret_cast<const std::uint16_t*>(agentData + kAgentDataNumStates);
    auto* const* refs = *reinterpret_cast<const std::uint8_t* const* const*>(agentData + kAgentDataStateRefs);
    if (!refs || static_cast<std::uint16_t>(index) >= numStates) return nullptr;
    return refs[index];
}

// Lara's weapon and inventory agent.
bool IsWeaponAgent(const std::uint8_t* agent) {
    const char* name = AgentDebugName(*reinterpret_cast<const std::uint8_t* const*>(agent + kAgentData));
    return name && std::strcmp(name, "Secondary Agent") == 0;
}

// Weapon agent states that put the weapon away (or lower it for a sprint).
bool IsHolsterState(const char* name) {
    if (!name) return false;
    static constexpr const char* kExact[] = {
        "default", "combat_wp_immediate_unequip_and_go_default", "combat_wp_unequip", "combat_quick_unequip",
        "combat_ranged_unequip", "combat_slope_unequip", "combat_ranged_sprint",
    };
    for (const char* exact : kExact)
        if (std::strcmp(name, exact) == 0) return true;
    return StartsWith(name, "postCombat");
}

// The weapon is being drawn, held or fired (a state under combat_ranged_parent), or it is lowered
// in postCombat.
bool InCombatChain(const std::uint8_t* agent) {
    auto* agentData = *reinterpret_cast<const std::uint8_t* const*>(agent + kAgentData);
    auto* state = *reinterpret_cast<const std::uint8_t* const*>(agent + kAgentCurrentState);
    if (!agentData || !state) return false;
    const char* leaf = StateDebugName(state);
    if (leaf && StartsWith(leaf, "postCombat")) return true;
    const std::uint16_t numStates = *reinterpret_cast<const std::uint16_t*>(agentData + kAgentDataNumStates);
    auto* const* refs = *reinterpret_cast<const std::uint8_t* const* const*>(agentData + kAgentDataStateRefs);
    for (int depth = 0; state && depth < 24; ++depth) {
        const char* name = StateDebugName(state);
        if (name && std::strcmp(name, "combat_ranged_parent") == 0) return true;
        const std::uint16_t parent = *reinterpret_cast<const std::uint16_t*>(state + kStateParent);
        if (parent == 0xFFFF || parent >= numStates || !refs) break;
        state = refs[parent];
    }
    return false;
}

// A transition of the weapon agent that passed its conditions: in the air, with aim held, the ways
// from a drawn (or drawing) weapon to the holster wait until Lara lands or aim is let go.
bool HoldBackHolsterExit(void* agent, const std::uint8_t* transition) {
    const char* target = nullptr;
#if defined(_MSC_VER)
    __try {
#endif
        auto* bytes = static_cast<const std::uint8_t*>(agent);
        if (!IsWeaponAgent(bytes) || !PrimaryAgentInAir(agent)) return false;
        target = StateDebugName(TransitionTarget(bytes, transition));
        if (!IsHolsterState(target) || !InCombatChain(bytes)) return false;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
    g_unequipBlocked.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// A combat evade with aim held and shooting while dodging on: the weapon agent does not go to its evade state
// combat_ranged_dodge_cam (T294 "_to_dodge_camera" / T307 "_to_dodge_camera_reload" on Combat_InDodge; it has no
// fire transitions and leaves only when Combat_PrimaryOverrideVar turns false) nor to combat_hitreact (T475 / T479
// on Combat_PrimaryOverrideVar), but stays ready to fire.
bool HoldBackDodgeHitReact(void* agent, const std::uint8_t* transition) {
    const char* name = nullptr;
#if defined(_MSC_VER)
    __try {
#endif
        name = *reinterpret_cast<const char* const*>(transition + kTransitionName);
        if (!name || !(StartsWith(name, "combat_ranged_to_combat_ranged_hitreact") || StartsWith(name, "_to_dodge_camera")))
            return false;
        if (!IsWeaponAgent(static_cast<const std::uint8_t*>(agent)) || !PrimaryAgentDodging(agent)) return false;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
    return true;
}

// A shot of the weapon agent in the air (ready -> fire passed): the start of the next air fire interval.
void NoteAirShot(void* agent, const std::uint8_t* transition) {
    bool shot = false;
#if defined(_MSC_VER)
    __try {
#endif
        const char* name = *reinterpret_cast<const char* const*>(transition + kTransitionName);
        shot = name && StartsWith(name, "combat_wp_ready_to_fire") &&
               IsWeaponAgent(static_cast<const std::uint8_t*>(agent)) && PrimaryAgentInAir(agent);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
#endif
    if (!shot) return;
    g_lastAirShotMs.store(GetTickCount64(), std::memory_order_relaxed);
    g_airShots.fetch_add(1, std::memory_order_relaxed);
}

// A roll chain move running down a slope: the ground drops away under the flying moves, Status_IsFalling turns on and
// T139 "ground_to_ground_gone_rolling" / T11 "default_to_fall" end the chain (v0.9.222 log: move 3 ended at 16 frames).
// They wait up to kChainFallHoldMs of continued falling; on a slope Lara finds the ground again before that, over a
// real drop she falls a moment later.
bool HoldBackChainFall(void* agent, const std::uint8_t* transition) {
    if (!Acro::IsChainMove(agent)) return false;
    const char* name = nullptr;
#if defined(_MSC_VER)
    __try {
#endif
        name = *reinterpret_cast<const char* const*>(transition + kTransitionName);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
    if (!name || (std::strcmp(name, "ground_to_ground_gone_rolling") != 0 && std::strcmp(name, "default_to_fall") != 0))
        return false;
    const unsigned long long now = GetTickCount64();
    unsigned long long since = g_chainFallSince.load(std::memory_order_relaxed);
    if (!since || now - g_chainFallLast.load(std::memory_order_relaxed) > 150) {  // a new fall (not the same one)
        since = now;
        g_chainFallSince.store(now, std::memory_order_relaxed);
    }
    g_chainFallLast.store(now, std::memory_order_relaxed);
    const bool hold = now - since < kChainFallHoldMs;
    if (g_chainFallLogs.fetch_add(1, std::memory_order_relaxed) < 30)
        Log::Detailf("CombatAir: roll chain '%s' %s (falling for %llu ms)", name, hold ? "held back" : "let through",
                    now - since);
    return hold;
}

// The roll chain's next move (T327 / T338) failed only the SC's own waits: "Frames_InState 550" (18 frames in the
// state, made for the game's 32-frame roll) and "NOT MarkupLookahead_ConvexTurn". A chain move may go on from its
// clip's ready frame instead (Acro::ChainMoveReady): the twist (move 3) is 19 frames, its end (T73
// ground_roll_to_ground_move_entry on Anim_Ended) came in the same tick as the 18 frames, and a little faster
// playback downhill made it win (v0.9.223 log: 3 -> 4 at 594 ms, the cut chains at 578 ms). The roll press, a low
// ceiling ahead and injury still decide as before.
bool ChainGatesOnly(void* agent, const std::uint8_t* transition, char* report, std::size_t size) {
    int failures = 0;
    AirTolerance tolerance;
    tolerance.check = AirCheck::ChainGate;
#if defined(_MSC_VER)
    __try {
#endif
        return AirFailures(agent, transition, tolerance, report, size, failures);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

// LB strafe (acro.cpp) + B: the roll's entry state [11] ground_roll_entry first tries T1930 "ground_to_evade_move" ->
// [968] combat_evade_move_entry, the evade the game allows in a melee counter window; while strafing it may go there
// without one (Combat_MeleeTargetInCounterAllowedWindow 510, Combat_HardCodedProbeToMeleeTargetClear 827), so B plays
// the same directional evades as when aiming (combat_ranged_move T1908 -> [968], direction from the stick). Already in a
// dodge, in water, a low ceiling ahead or the dodge counter blocked still decide.
bool StrafeDodgeGatesOnly(void* agent, const std::uint8_t* transition) {
    char report[256];
    int failures = 0;
    AirTolerance tolerance;
    tolerance.check = AirCheck::StrafeDodge;
    bool ok = false;
#if defined(_MSC_VER)
    __try {
#endif
        const char* name = *reinterpret_cast<const char* const*>(transition + kTransitionName);
        if (!name || std::strcmp(name, "ground_to_evade_move") != 0) return false;
        if (*reinterpret_cast<void**>(static_cast<std::uint8_t*>(agent) + kAgentInstance) !=
            g_laraInstance.load(std::memory_order_relaxed))
            return false;
        ok = AirFailures(agent, transition, tolerance, report, sizeof(report), failures);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
    if (ok && g_strafeDodgeLogs.fetch_add(1, std::memory_order_relaxed) < 20)
        Log::Detailf("CombatAir: LB strafe roll -> combat evade (the SC still wanted %s)", report);
    return ok;
}

// ---------- enemy shots miss Lara in acrobatics ----------
// An instant bullet of anyone but Lara whose line passes within kShotHitRadius of her (a vertical segment over her
// position, generous for the flips) while she rolls or evades (the acrobatics protection window) is turned about its
// start so that it passes kShotMissRadius beside her, and goes on at least as far as its range: the tracer, the impact
// on what is behind and the enemy's "missed" bookkeeping are the game's own, and no blood.
using FireInstantFn = std::uint64_t(__fastcall*)(void* properties, std::uint8_t* fire, std::uint8_t* trace, bool flag);
FireInstantFn g_originalFireInstant = nullptr;
bool g_shotHookAvailable = false;
constexpr std::size_t kFireOwnerHandle = 0x08;        // SingleFireDataBlock::weaponOwnerInst (instance handle)
constexpr std::size_t kInstanceHandle = 0x118;        // the instance's own handle (FireInstant's owner check)
constexpr std::size_t kInstancePosition = 0x10;       // Vector3 (cm, z up)
constexpr std::size_t kTraceStart = 0x00, kTraceEnd = 0x10, kTraceMaxRange = 0x20;
constexpr float kShotHitRadius = 100.0f;              // cm from Lara's axis: a shot this close is turned
constexpr float kShotMissRadius = 160.0f;             // cm from her axis after the turn
constexpr float kShotAxisBottom = -30.0f, kShotAxisTop = 230.0f;  // cm over her position
constexpr float kShotMaxLength = 20000.0f;

float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Closest points of segments p1-q1 and p2-q2 (Ericson, Real-Time Collision Detection 5.1.9); the squared distance.
float ClosestSegmentPoints(const float* p1, const float* q1, const float* p2, const float* q2, float* c1, float* c2) {
    float d1[3], d2[3], r[3];
    for (int i = 0; i < 3; ++i) {
        d1[i] = q1[i] - p1[i];
        d2[i] = q2[i] - p2[i];
        r[i] = p1[i] - p2[i];
    }
    const float a = Dot3(d1, d1), e = Dot3(d2, d2), f = Dot3(d2, r);
    constexpr float kEps = 1e-6f;
    float s = 0.0f, t = 0.0f;
    if (a <= kEps && e <= kEps) {
        s = t = 0.0f;
    } else if (a <= kEps) {
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = Dot3(d1, r);
        if (e <= kEps) {
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = Dot3(d1, d2), denom = a * e - b * b;
            s = denom != 0.0f ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    float d = 0.0f;
    for (int i = 0; i < 3; ++i) {
        c1[i] = p1[i] + d1[i] * s;
        c2[i] = p2[i] + d2[i] * t;
        d += (c1[i] - c2[i]) * (c1[i] - c2[i]);
    }
    return d;
}

void TurnShotAwayFromLara(const std::uint8_t* fire, std::uint8_t* trace) {
    auto* lara = static_cast<const std::uint8_t*>(g_laraInstance.load(std::memory_order_relaxed));
    if (!lara) return;
    float before = 0.0f, after = 0.0f;
    bool turned = false;
#if defined(_MSC_VER)
    __try {
#endif
        const std::uint64_t owner = *reinterpret_cast<const std::uint64_t*>(fire + kFireOwnerHandle);
        if (!owner || owner == *reinterpret_cast<const std::uint64_t*>(lara + kInstanceHandle)) return;  // her own shot
        const std::uint32_t shot = g_enemyShots.fetch_add(1, std::memory_order_relaxed) + 1;
        const bool acrobatics = GetTickCount64() < g_acroProtectUntilMs.load(std::memory_order_relaxed);
        float* start = reinterpret_cast<float*>(trace + kTraceStart);
        float* end = reinterpret_cast<float*>(trace + kTraceEnd);
        const float* pos = reinterpret_cast<const float*>(lara + kInstancePosition);
        float dir[3] = {end[0] - start[0], end[1] - start[1], end[2] - start[2]};
        const float length = std::sqrt(Dot3(dir, dir));
        if (!(length > 1.0f) || !std::isfinite(length)) return;
        for (float& x : dir) x /= length;
        const float bottom[3] = {pos[0], pos[1], pos[2] + kShotAxisBottom}, top[3] = {pos[0], pos[1], pos[2] + kShotAxisTop};
        float onShot[3], onLara[3];
        const float d2 = ClosestSegmentPoints(start, end, bottom, top, onShot, onLara);
        if (shot <= 4)  // the hook sees the enemies' bullets (any time)
            Log::Detailf("CombatAir: enemy shot #%u: %.0f cm long, passes %.0f cm from Lara%s", shot, length, std::sqrt(d2),
                        acrobatics ? " (in acrobatics)" : "");
        if (!acrobatics || !(d2 < kShotHitRadius * kShotHitRadius)) return;
        before = std::sqrt(d2);
        // the side the shot already leans to (square to the shot), else sideways
        float side[3] = {onShot[0] - onLara[0], onShot[1] - onLara[1], onShot[2] - onLara[2]};
        const float along = Dot3(side, dir);
        for (int i = 0; i < 3; ++i) side[i] -= dir[i] * along;
        float n = std::sqrt(Dot3(side, side));
        if (n < 1.0f) {
            side[0] = -dir[1];  // cross(dir, up)
            side[1] = dir[0];
            side[2] = 0.0f;
            n = std::sqrt(Dot3(side, side));
            if (n < 1e-3f) {
                side[0] = 1.0f;
                side[1] = side[2] = 0.0f;
                n = 1.0f;
            }
        }
        for (float& x : side) x /= n;
        float aim[3];
        for (int i = 0; i < 3; ++i) aim[i] = onLara[i] + side[i] * kShotMissRadius - start[i];
        const float aimLength = std::sqrt(Dot3(aim, aim));
        if (!(aimLength > 1.0f)) return;
        const float range = *reinterpret_cast<const float*>(trace + kTraceMaxRange);
        const float newLength = std::min(std::max(length, std::isfinite(range) && range > 0.0f ? range : 0.0f), kShotMaxLength);
        float newEnd[3];
        for (int i = 0; i < 3; ++i) newEnd[i] = start[i] + aim[i] / aimLength * newLength;
        float c1[3], c2[3];
        after = std::sqrt(ClosestSegmentPoints(start, newEnd, bottom, top, c1, c2));
        if (after < kShotHitRadius) return;  // odd geometry (Lara at the muzzle): leave it to the game
        end[0] = newEnd[0];
        end[1] = newEnd[1];
        end[2] = newEnd[2];
        turned = true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
#endif
    if (!turned) return;
    g_shotsTurned.fetch_add(1, std::memory_order_relaxed);
    if (g_shotLogs.fetch_add(1, std::memory_order_relaxed) < 24)
        Log::Detailf("CombatAir: enemy shot turned away from Lara in acrobatics (it passed %.0f cm from her, now %.0f cm)",
                    before, after);
}

// ---------- enemy FX bullets pass through Lara in acrobatics ----------
using FxFilterCollisionsFn = bool(__fastcall*)(void* owner, bool a, bool b, bool c, bool filterAllies, bool skip,
                                               const std::uint8_t* result);
FxFilterCollisionsFn g_originalFxFilter = nullptr;
bool g_fxFilterAvailable = false;

// The instance a particle's line probe hit, the way the game's filter finds it (LineProbeResult +0x24 == 2: a
// collision primitive; +0x38 -> +0x78 -> +0x50 -> [0] Instance*).
void* FxProbeHitInstance(const std::uint8_t* result) {
    if (*reinterpret_cast<const std::int32_t*>(result + 0x24) != 2) return nullptr;
    const auto* a = *reinterpret_cast<const std::uint8_t* const*>(result + 0x38);
    if (!a) return nullptr;
    const auto* b = *reinterpret_cast<const std::uint8_t* const*>(a + 0x78);
    if (!b) return nullptr;
    const auto* c = *reinterpret_cast<const std::uint8_t* const*>(b + 0x50);
    if (!c) return nullptr;
    return *reinterpret_cast<void* const*>(c);
}

bool __fastcall HookFxFilterCollisions(void* owner, bool a, bool b, bool c, bool filterAllies, bool skip,
                                       const std::uint8_t* result) {
    const bool collide = g_originalFxFilter(owner, a, b, c, filterAllies, skip, result);
    if (!collide || !result || !Acro::AcrobaticsProtection()) return collide;
    void* lara = g_laraInstance.load(std::memory_order_relaxed);
    if (!lara || owner == lara) return collide;
    void* hit = nullptr;
#if defined(_MSC_VER)
    __try {
#endif
        hit = FxProbeHitInstance(result);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return collide;
    }
#endif
    if (hit != lara) return collide;
    const std::uint32_t n = g_fxHitsOnLara.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool acrobatics = GetTickCount64() < g_acroProtectUntilMs.load(std::memory_order_relaxed);
    if (g_fxLogs.fetch_add(1, std::memory_order_relaxed) < 20)
        Log::Detailf("CombatAir: FX projectile collision with Lara #%u (from %p, caller +0x%llX)%s", n, owner,
                    static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_base),
                    acrobatics ? ": passes through her (acrobatics)" : "");
    if (!acrobatics) return collide;
    g_fxHitsPassed.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// Lara's damage: logged (type, melee, caller: which way the enemies' shots hurt her) and, in the acrobatics, shot
// damage cancelled (a fallback for shots that are not FX particles).
using ApplyDamageFn = bool(__fastcall*)(void* health, std::uint8_t* damage);
ApplyDamageFn g_originalApplyDamage = nullptr;
bool g_applyDamageAvailable = false;
constexpr std::size_t kHealthInstance = 0x08;
constexpr std::size_t kDamageAmount = 0x00, kDamageType = 0x04, kDamageAttacker = 0x60, kDamageFlags = 0xB2;

bool IsShotDamage(int type) {
    switch (type) {
    case 2: case 3: case 4:                   // Light / Medium / Heavy (not melee: checked by the caller)
    case 10: case 11: case 12:                // Pellet, PelletIncendiary, AltPellet
    case 14: case 15:                         // Bullet, HandgunBullet
    case 16: case 17: case 18: case 19: case 20: case 21: case 24: case 25: case 26: case 33:  // arrows, adrenaline
    case 46: case 56: case 57: case 58: case 59: case 60: case 67: case 68:  // turret, compound arrows, gatling, AP, flechette
        return true;
    default:
        return false;
    }
}

bool __fastcall HookApplyDamage(void* health, std::uint8_t* damage) {
    void* lara = g_laraInstance.load(std::memory_order_relaxed);
    if (health && damage && lara) {
        void* victim = nullptr;
        float amount = 0.0f;
        int type = -1;
        bool melee = false;
        std::uint64_t attacker = 0;
#if defined(_MSC_VER)
        __try {
#endif
            victim = *reinterpret_cast<void* const*>(static_cast<std::uint8_t*>(health) + kHealthInstance);
            amount = *reinterpret_cast<const float*>(damage + kDamageAmount);
            type = *reinterpret_cast<const std::int32_t*>(damage + kDamageType);
            melee = (damage[kDamageFlags + 1] & 1) != 0;
            attacker = *reinterpret_cast<const std::uint64_t*>(damage + kDamageAttacker);
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            victim = nullptr;
        }
#endif
        if (victim == lara) {
            const bool acrobatics = Acro::AcrobaticsProtection() &&
                                    GetTickCount64() < g_acroProtectUntilMs.load(std::memory_order_relaxed);
            const bool cancel = acrobatics && !melee && IsShotDamage(type);
            if (g_laraDamageLogs.fetch_add(1, std::memory_order_relaxed) < 24)
                Log::Detailf("CombatAir: Lara takes damage %.1f type %d%s from %llX (caller +0x%llX)%s", amount, type,
                            melee ? " (melee)" : "", static_cast<unsigned long long>(attacker),
                            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_base),
                            cancel ? " -> cancelled (acrobatics)" : acrobatics ? " (in acrobatics, not a shot)" : "");
            if (cancel) {
                g_laraShotsCancelled.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
    }
    return g_originalApplyDamage(health, damage);
}

std::uint64_t __fastcall HookFireInstant(void* properties, std::uint8_t* fire, std::uint8_t* trace, bool flag) {
    if (fire && trace && Acro::AcrobaticsProtection()) TurnShotAwayFromLara(fire, trace);
    return g_originalFireInstant(properties, fire, trace, flag);
}

// (v0.9.477) Lara's glowstick-equip transitions while GlowColor keeps the stick on her belt
bool GlowstickHeldBack(void* agent, const std::uint8_t* transition) {
    if (!agent || !transition) return false;
    const char* name = nullptr;
#if defined(_MSC_VER)
    __try {
#endif
        if (*reinterpret_cast<void**>(static_cast<std::uint8_t*>(agent) + kAgentInstance) != g_laraInstance.load(std::memory_order_relaxed))
            return false;
        name = *reinterpret_cast<const char* const*>(transition + kTransitionName);
        if (!name || name[0] != 'd' || !std::strstr(name, "glow_stick")) return false;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
    return GlowColor::HoldBackTransition(name);
}

bool __fastcall HookCheckConditions(void* agent, const std::uint8_t* transition) {
    const bool passed = g_originalCheckConditions(agent, transition);
    if (!passed && Grapple::ForceTransition(agent, transition)) return true;  // grapple kick armed in a jump
    if (!passed && Brawl::ForceTransition(agent, transition)) return true;    // hand-to-hand: the chain's next strike
    if (passed && !Acro::AllowTransition(agent, transition)) return false;  // roll chain: wait for the landing
    if (passed && Acro::HoldClimbTransition(agent, transition)) return false;  // climb-up: a tap of A or a hold?
    if (passed && Acro::HoldBackWhileStrafing(agent, transition)) return false;  // LB strafe: no sprint
    if (passed && HoldBackChainFall(agent, transition)) return false;       // roll chain: ride out a downhill
    if (passed && !Grapple::AllowTransition(agent, transition)) return false;  // grapple kick: let its clip play out
    if (passed && !Brawl::AllowTransition(agent, transition)) return false;    // hand-to-hand: the strike plays to its end
    if (passed && GlowstickHeldBack(agent, transition)) return false;          // (v0.9.477) the glowstick kept on her belt
    if (!passed && Acro::IsChainTransition(agent, transition)) {
        char gates[256];
        float frames = 0.0f;
        int ready = 0;
        if (ChainGatesOnly(agent, transition, gates, sizeof(gates)) && Acro::ChainMoveReady(agent, &frames, &ready)) {
            return true;
        }
    }
    if (!passed && agent && transition && Acro::StrafeHeld() && StrafeDodgeGatesOnly(agent, transition))
        return true;  // LB strafe + B: the combat evade (as when aiming)
    if (!g_airAimArmed.load(std::memory_order_relaxed) || g_airAimPaused.load(std::memory_order_relaxed) || !agent ||
        !transition ||
        *reinterpret_cast<void**>(static_cast<std::uint8_t*>(agent) + kAgentInstance) !=
            g_laraInstance.load(std::memory_order_relaxed))
        return passed;
    if (passed) {
        if (HoldBackHolsterExit(agent, transition)) return false;
        if (HoldBackDodgeHitReact(agent, transition)) return false;
        NoteAirShot(agent, transition);
        return true;
    }
    char report[720];
    int failures = 0;
    bool force = false;
    const char* name = nullptr;
    AirTolerance tolerance;
#if defined(_MSC_VER)
    __try {
#endif
        name = *reinterpret_cast<const char* const*>(transition + kTransitionName);
        if (!name) return passed;
        tolerance.check = AirCheckFor(name);
        if (tolerance.check == AirCheck::None || !IsSecondaryAgent(agent) || !PrimaryAgentInAir(agent))
            return passed;
        tolerance.dodging = PrimaryAgentDodging(agent);
        if (tolerance.check == AirCheck::OverrideExit && !tolerance.dodging) return passed;
        if (tolerance.check == AirCheck::Fire || tolerance.check == AirCheck::FireEnd) {
            // (v0.9.487) not on a beam: there the weapon keeps its own rate (the user: no faster shooting on beams)
            const bool fast = g_airFastFire.load(std::memory_order_relaxed) && PrimaryAgentClass(agent) != AirClass::Beam;
            const float interval = g_airFireIntervalMs.load(std::memory_order_relaxed);
            const float since = static_cast<float>(GetTickCount64() - g_lastAirShotMs.load(std::memory_order_relaxed));
            tolerance.fireIntervalDone = fast && since >= interval;
            tolerance.halfIntervalDone = fast && since >= interval * 0.5f;
        }
        force = AirFailures(agent, transition, tolerance, report, sizeof(report), failures) &&
                tolerance.check != AirCheck::Report;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return passed;
    }
#endif
    // (v0.9.480) on a beam in the upper-body mode Lara's graph runs no weapon graph: should "equip to ready" still be
    // failing 0.6 s into an equip (it ought to count the weapon agent's own frames), it is let through
    if (tolerance.check == AirCheck::Report && PrimaryAgentClass(agent) == AirClass::Beam &&
        g_beamLegs.load(std::memory_order_relaxed)) {
        static ULONGLONG s_first = 0, s_last = 0;
        static int s_logs = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - s_last > 300) s_first = now;
        s_last = now;
        if (now - s_first < 600) return passed;
        if (s_logs++ < 8) Log::Writef("CombatAir: beam aim: '%s' still failing after 0.6 s on the beam: let through (%s)", name, report);
        s_first = now;
        return true;
    }
    if (tolerance.check == AirCheck::OverrideExit) {
        if (!force) return passed;
        return true;
    }
    if (tolerance.check == AirCheck::Fire || tolerance.check == AirCheck::FireEnd) {
        if (!force) return passed;
        g_airFireLetThrough.fetch_add(1, std::memory_order_relaxed);
        if (tolerance.check == AirCheck::Fire) {
            g_lastAirShotMs.store(GetTickCount64(), std::memory_order_relaxed);
            g_airShots.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }
    if (!force) return passed;
    g_drawForced.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---------- aim IK during aimed combat evades ----------
// The evade clips carry the arms of the dodge's aim stance; the target is where the camera aims. Before the game turns
// Lara's final pose into bone matrices, each upper arm is turned about the shoulder so that shoulder-to-hand points
// along the aim (the forearm, hand, fingers and the weapon follow as children). The turn fades in and out with the
// aimed evade, and fades away for turns over 60..120 degrees (an upside-down flip keeps its own arms).
// Pose: AnimSegment 0x40 (+0 quat-log rotation = rotation vector, +0x10 translation); skeleton Segment 0x50 with the
// parent index (i32) at +0x38; out[-1] = the root's parent matrix (Lara's world matrix, rows = its axes).
using AnimBuildTransformsFn = void(__fastcall*)(void* instance, std::uint32_t count, std::uint8_t* segs,
                                                const std::uint8_t* skeleton, float* out);
AnimBuildTransformsFn g_originalAnimBuildTransforms = nullptr;
constexpr std::size_t kPoseSegmentSize = 0x40;
constexpr std::size_t kPoseSegmentTranslation = 0x10;
constexpr std::size_t kSkeletonSegmentSize = 0x50;
constexpr std::size_t kSkeletonSegmentParent = 0x38;
struct IkArm { int clavicle, upper, fore, hand; };
constexpr IkArm kIkArms[] = {{114, 115, 116, 117}, {89, 90, 91, 92}};  // right (pistol hand), left
constexpr int kIkClavicleParent = 6;                                     // the upper spine bone both clavicles hang on

struct M3 { float m[9]; };
M3 M3Identity() { return {{1, 0, 0, 0, 1, 0, 0, 0, 1}}; }
M3 M3Mul(const M3& a, const M3& b) {
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i * 3 + j] = a.m[i * 3] * b.m[j] + a.m[i * 3 + 1] * b.m[3 + j] + a.m[i * 3 + 2] * b.m[6 + j];
    return r;
}
M3 M3Transpose(const M3& a) { return {{a.m[0], a.m[3], a.m[6], a.m[1], a.m[4], a.m[7], a.m[2], a.m[5], a.m[8]}}; }
void M3Apply(const M3& a, const float* v, float* out) {
    for (int r = 0; r < 3; ++r) out[r] = a.m[r * 3] * v[0] + a.m[r * 3 + 1] * v[1] + a.m[r * 3 + 2] * v[2];
}
// Rotation vector (axis * angle) -> matrix, the way cdc::MathUtil::QuatLogToMatrix reads the pose.
M3 M3FromRotvec(const float* v) {
    const float t = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (t < 1e-8f) return M3Identity();
    const float x = v[0] / t, y = v[1] / t, z = v[2] / t, s = std::sin(t), c = std::cos(t), k = 1.0f - c;
    return {{c + x * x * k, x * y * k - z * s, x * z * k + y * s,
             y * x * k + z * s, c + y * y * k, y * z * k - x * s,
             z * x * k - y * s, z * y * k + x * s, c + z * z * k}};
}
void QuatFromM3(const M3& a, float* q) {
    const float* m = a.m;
    const float trace = m[0] + m[4] + m[8];
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q[3] = s / 4; q[0] = (m[7] - m[5]) / s; q[1] = (m[2] - m[6]) / s; q[2] = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const float s = std::sqrt(1.0f + m[0] - m[4] - m[8]) * 2.0f;
        q[3] = (m[7] - m[5]) / s; q[0] = s / 4; q[1] = (m[1] + m[3]) / s; q[2] = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        const float s = std::sqrt(1.0f + m[4] - m[0] - m[8]) * 2.0f;
        q[3] = (m[2] - m[6]) / s; q[0] = (m[1] + m[3]) / s; q[1] = s / 4; q[2] = (m[5] + m[7]) / s;
    } else {
        const float s = std::sqrt(1.0f + m[8] - m[0] - m[4]) * 2.0f;
        q[3] = (m[3] - m[1]) / s; q[0] = (m[2] + m[6]) / s; q[1] = (m[5] + m[7]) / s; q[2] = s / 4;
    }
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; ++i) q[i] /= n;
}
void QuatFromRotvec(const float* v, float* q) {
    const float t = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (t < 1e-12f) { q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f; return; }
    const float s = std::sin(t * 0.5f) / t;
    q[0] = v[0] * s; q[1] = v[1] * s; q[2] = v[2] * s; q[3] = std::cos(t * 0.5f);
}
void RotvecFromQuat(const float* in, float* v) {  // canonical: |v| <= pi
    float q[4] = {in[0], in[1], in[2], in[3]};
    if (q[3] < 0.0f) for (float& c : q) c = -c;
    const float s = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    if (s < 1e-9f) { v[0] = v[1] = v[2] = 0.0f; return; }
    const float t = 2.0f * std::atan2(s, q[3]);
    v[0] = q[0] / s * t; v[1] = q[1] / s * t; v[2] = q[2] / s * t;
}
// The shortest rotation taking direction a onto direction b (and its angle).
M3 M3Between(const float* a, const float* b, float& angle) {
    const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    const float lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    angle = 0.0f;
    if (la < 1e-6f || lb < 1e-6f) return M3Identity();
    const float x[3] = {a[0] / la, a[1] / la, a[2] / la}, y[3] = {b[0] / lb, b[1] / lb, b[2] / lb};
    const float axis[3] = {x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]};
    const float s = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    const float c = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
    angle = std::atan2(s, c);
    if (s < 1e-6f) return M3Identity();  // parallel (an opposite aim is left alone by the fade anyway)
    const float v[3] = {axis[0] / s * angle, axis[1] / s * angle, axis[2] / s * angle};
    return M3FromRotvec(v);
}

void ApplyAimIkUnsafe(std::uint32_t count, std::uint8_t* segs, const std::uint8_t* skeleton, const float* out) {
    static const long long s_frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const long long last = g_ikLastTicks.exchange(now.QuadPart, std::memory_order_relaxed);
    const float dt = last ? std::clamp(static_cast<float>(now.QuadPart - last) / static_cast<float>(s_frequency), 0.0f, 0.1f) : 0.0f;
    const float target = g_ikTarget.load(std::memory_order_relaxed);
    float weight = g_ikWeight.load(std::memory_order_relaxed);
    weight = target > weight ? std::min(target, weight + 8.0f * dt) : std::max(target, weight - 5.0f * dt);
    g_ikWeight.store(weight, std::memory_order_relaxed);
    if (weight <= 0.001f) return;

    const auto parentOf = [&](int i) {
        return *reinterpret_cast<const std::int32_t*>(skeleton + static_cast<std::size_t>(i) * kSkeletonSegmentSize +
                                                      kSkeletonSegmentParent);
    };
    if (!g_ikSkeletonChecked.load(std::memory_order_relaxed)) {
        bool ok = count > 118;
        for (const IkArm& arm : kIkArms)
            ok = ok && parentOf(arm.clavicle) == kIkClavicleParent && parentOf(arm.upper) == arm.clavicle &&
                 parentOf(arm.fore) == arm.upper && parentOf(arm.hand) == arm.fore;
        g_ikSkeletonOk.store(ok, std::memory_order_relaxed);
        g_ikSkeletonChecked.store(true, std::memory_order_relaxed);
    }
    if (!g_ikSkeletonOk.load(std::memory_order_relaxed)) return;

    // The aim direction in the root's parent space (the rows of out[-1] are its axes in the world).
    const float* parent = out - 16;
    const float world[3] = {g_ikDirX.load(std::memory_order_relaxed), g_ikDirY.load(std::memory_order_relaxed),
                            g_ikDirZ.load(std::memory_order_relaxed)};
    float aim[3];
    for (int j = 0; j < 3; ++j) {
        const float* row = parent + j * 4;
        const float length = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
        if (length < 0.5f || length > 2.0f) return;  // not a rotation: leave the pose alone
        aim[j] = (world[0] * row[0] + world[1] * row[1] + world[2] * row[2]) / length;
    }
    g_ikAimX.store(aim[0], std::memory_order_relaxed);  // for the trace: straight ahead should read about (0, -1, 0)
    g_ikAimY.store(aim[1], std::memory_order_relaxed);
    g_ikAimZ.store(aim[2], std::memory_order_relaxed);

    const auto rotation = [&](int i) { return reinterpret_cast<float*>(segs + static_cast<std::size_t>(i) * kPoseSegmentSize); };
    const auto translation = [&](int i) {
        return reinterpret_cast<const float*>(segs + static_cast<std::size_t>(i) * kPoseSegmentSize + kPoseSegmentTranslation);
    };
    // Forward kinematics from the root to the spine bone the clavicles hang on.
    int chain[40];
    int length = 0;
    for (int i = kIkClavicleParent; i >= 0 && length < 40; i = parentOf(i)) {
        if (static_cast<std::uint32_t>(i) >= count) return;
        chain[length++] = i;
    }
    M3 spine = M3Identity();
    float spinePos[3] = {0.0f, 0.0f, 0.0f};
    for (int k = length - 1; k >= 0; --k) {
        float offset[3];
        M3Apply(spine, translation(chain[k]), offset);
        for (int c = 0; c < 3; ++c) spinePos[c] += offset[c];
        spine = M3Mul(spine, M3FromRotvec(rotation(chain[k])));
    }
    for (const IkArm& arm : kIkArms) {
        float offset[3];
        const M3 clavicle = M3Mul(spine, M3FromRotvec(rotation(arm.clavicle)));
        float clavPos[3];
        M3Apply(spine, translation(arm.clavicle), offset);
        for (int c = 0; c < 3; ++c) clavPos[c] = spinePos[c] + offset[c];
        const M3 upper = M3Mul(clavicle, M3FromRotvec(rotation(arm.upper)));
        float upperPos[3];
        M3Apply(clavicle, translation(arm.upper), offset);
        for (int c = 0; c < 3; ++c) upperPos[c] = clavPos[c] + offset[c];
        const M3 fore = M3Mul(upper, M3FromRotvec(rotation(arm.fore)));
        float forePos[3], handPos[3];
        M3Apply(upper, translation(arm.fore), offset);
        for (int c = 0; c < 3; ++c) forePos[c] = upperPos[c] + offset[c];
        M3Apply(fore, translation(arm.hand), offset);
        for (int c = 0; c < 3; ++c) handPos[c] = forePos[c] + offset[c];
        // the pistol lies along the forearm: turning the upper arm points elbow-to-hand along the aim
        const float reach[3] = {handPos[0] - forePos[0], handPos[1] - forePos[1], handPos[2] - forePos[2]};
        float angle = 0.0f;
        const M3 turn = M3Between(reach, aim, angle);
        const float fade = std::clamp((2.0944f - angle) / 1.0472f, 0.0f, 1.0f);  // full to 60 deg, none past 120
        const float w = weight * fade;
        if (w <= 0.001f) continue;
        float target4[4], current4[4], blended[4], result[3];
        QuatFromM3(M3Mul(M3Transpose(clavicle), M3Mul(turn, upper)), target4);
        QuatFromRotvec(rotation(arm.upper), current4);
        const float dot = target4[0] * current4[0] + target4[1] * current4[1] + target4[2] * current4[2] + target4[3] * current4[3];
        const float sign = dot < 0.0f ? -1.0f : 1.0f;
        float n = 0.0f;
        for (int c = 0; c < 4; ++c) { blended[c] = current4[c] * (1.0f - w) + target4[c] * sign * w; n += blended[c] * blended[c]; }
        n = std::sqrt(n);
        if (n < 1e-6f) continue;
        for (float& c : blended) c /= n;
        RotvecFromQuat(blended, result);
        float* dst = rotation(arm.upper);
        dst[0] = result[0]; dst[1] = result[1]; dst[2] = result[2];
    }
    g_ikFrames.fetch_add(1, std::memory_order_relaxed);
}

// ---------- (v0.9.475) aiming on a balance beam: the legs keep the beam, the upper body turns with the camera ----------
// While aiming on a beam the moveset goal is "Combat" (the weapon's hosted graph runs only there), so the whole body took
// the ground combat stance. Now her pelvis and legs (the pelvis and everything under it but the spine: found on her
// skeleton from the spine bone the clavicles hang on, the pelvis being the first bone of that chain with two legs under
// it) get the game's own beam clips (beam_idle, beam_idle_perp, beam_walk_slow, by the beam state) through
// InstanceAnim::PlayLayers with a segment mask; the spine and everything above keep the weapon's pose. The upper body
// also turns towards where the camera looks, at most BeamAimTurnDegrees (45) either way, spread over the spine bones
// from the pelvis up to the clavicles' bone, about Lara's vertical axis (faded in and out with the aim).
constexpr int kBeamMaxSegments = 8192;
std::uint8_t g_beamLegMask[kBeamMaxSegments];
std::uint8_t g_beamUpperMask[kBeamMaxSegments];     // (v0.9.480) the spine root and everything above it
std::atomic<int> g_beamUpperSegmentCount{0};
std::atomic<int> g_beamLegMaskCount{0};           // 0: not worked out (yet)
std::atomic<const void*> g_beamLegSkeleton{nullptr};
int g_beamSpine[16];
std::atomic<int> g_beamSpineCount{0};
std::atomic<float> g_beamTwistTarget{0.0f};
std::atomic<float> g_beamTwistWeight{0.0f};
// (v0.9.482) The aim clips' own heading. 0.9.481's video: on a beam her arms pointed well left of the reticle (about
// 60 to 90 degrees) even with the camera straight behind her: the hosted aim clips turn the spine against the combat
// stance's own hips (turned to the side there), and the beam's hips face ahead. So the heading is measured instead of
// assumed: after the game built her bones (AnimBuildTransforms' world matrices), the line from the top spine bone to
// the middle of her wrists (segments 92 and 117, as the grapple's), flattened, against the aim the twist wants, in the
// twist's own measure (the root's parent space, atan2(x, -y)); the difference is added to the twist bit by bit (a
// fifth each time, at most 6 degrees, the correction within 150 degrees), only while the aim clips play alone (no
// draw, reload or shot over them) at full weight. Kept per weapon set.
std::atomic<float> g_beamYawCorrection{0.0f};
// (v0.9.486) The upper body upright on a beam. The user, 0.9.485: the upper body still follows the balancing
// animations, so the aim looks goofy and imprecise. The aim clips set the spine relative to its parent, and the beam
// clips sway and tilt the hips (the spine root's parent chain) to keep balance: the whole upper body swayed with them.
// Now, while the aim clips play (their weight, UpdateBeamAimBody's master), the spine root's local rotation is set so
// that its rotation in the root's parent space is the clip's on a parent that keeps the hips' heading (its forward,
// (0, -1, 0) of the bind pose, flattened: yaw only) without their tilt and roll: new local = W^T * Yaw(W) * L (W the
// parent chain's rotation, L the clip's local rotation), blended in by that weight (quaternion slerp). The twist and the
// heading correction then turn it from there.
std::atomic<float> g_beamUprightWeight{0.0f};
std::atomic_bool g_beamMeasureAim{false};
std::atomic<int> g_beamYawMeasures{0};
constexpr int kLaraLeftWrist = 92, kLaraRightWrist = 117;
std::atomic<long long> g_beamTwistLastTicks{0};
std::atomic<float> g_beamDirX{0.0f}, g_beamDirY{-1.0f}, g_beamDirZ{0.0f};

void ComputeBeamLegMaskUnsafe(std::uint32_t count, const std::uint8_t* skeleton) {
    // (v0.9.477) 0.9.475 found no pelvis on Lara's skeleton (its depths assumed children after their parents and a toe
    // under each foot). Now: the depths from the parent links alone; a leg is a child chain two bones deep or more; the
    // pelvis is the first bone of the spine chain (root side) with two legs under it, or, failing that, a bone beside the
    // chain (a child of one of its bones) with two legs under it (a pelvis that is a sibling of the spine). The lower
    // body is the pelvis and everything under it but the spine. When nothing is found the chain and its children are
    // logged.
    const int n = static_cast<int>(std::min<std::uint32_t>(count, kBeamMaxSegments));
    const auto parentOf = [&](int i) {
        return *reinterpret_cast<const std::int32_t*>(skeleton + static_cast<std::size_t>(i) * kSkeletonSegmentSize +
                                                      kSkeletonSegmentParent);
    };
    if (n <= kIkClavicleParent) return;
    int chain[40];
    int length = 0;   // from the clavicles' bone down to the root
    for (int i = kIkClavicleParent; i >= 0 && i < n && length < 40; i = parentOf(i)) chain[length++] = i;
    if (length < 2) return;
    static int depth[kBeamMaxSegments];
    for (int i = 0; i < n; ++i) depth[i] = 0;
    for (int i = 0; i < n; ++i) {   // each bone raises its ancestors' depths
        int d = 1;
        for (int a = parentOf(i), guard = 0; a >= 0 && a < n && guard < 128; a = parentOf(a), ++d, ++guard)
            if (depth[a] < d) depth[a] = d;
    }
    auto inChain = [&](int i) {
        for (int k = 0; k < length; ++k)
            if (chain[k] == i) return true;
        return false;
    };
    auto legsUnder = [&](int bone) {
        int legs = 0;
        for (int i = 0; i < n; ++i)
            if (parentOf(i) == bone && !inChain(i) && depth[i] >= 2) ++legs;
        return legs;
    };
    int pelvis = -1, spineRoot = -1;
    for (int k = length - 1; k >= 1 && pelvis < 0; --k)   // root first: a pelvis in the chain
        if (legsUnder(chain[k]) >= 2) {
            pelvis = chain[k];
            spineRoot = chain[k - 1];
        }
    for (int k = length - 1; k >= 1 && pelvis < 0; --k)   // a pelvis beside the chain
        for (int i = 0; i < n && pelvis < 0; ++i)
            if (parentOf(i) == chain[k] && !inChain(i) && legsUnder(i) >= 2) {
                pelvis = i;
                spineRoot = chain[k - 1];
            }
    if (pelvis < 0) {
        g_beamLegMaskCount.store(-1);
        std::string text;
        char part[96];
        for (int k = length - 1; k >= 0; --k) {
            std::snprintf(part, sizeof(part), "%s%d [", k == length - 1 ? "" : " > ", chain[k]);
            text += part;
            int shown = 0;
            for (int i = 0; i < n && shown < 12; ++i)
                if (parentOf(i) == chain[k] && !inChain(i)) {
                    std::snprintf(part, sizeof(part), "%s%d:%d", shown++ ? " " : "", i, depth[i]);
                    text += part;
                }
            text += "]";
        }
        Log::Writef("CombatAir: beam aim: no pelvis with two legs found on Lara's skeleton (%d segments; the spine chain, "
                    "root first, with each bone's other children as segment:depth: %s); her legs are left to the weapon's pose",
                    n, text.c_str());
        return;
    }
    int lower = 0;
    for (int i = 0; i < n; ++i) {
        bool below = false;
        for (int j = i, guard = 0; j >= 0 && guard < 128; j = parentOf(j), ++guard) {
            if (j == spineRoot) break;
            if (j == pelvis) { below = true; break; }
        }
        if (below && parentOf(i) < 0) below = false;   // (never the skeleton's root: its track moves Lara herself)
        g_beamLegMask[i] = below ? 1 : 0;
        lower += below;
    }
    int upper = 0;   // (v0.9.480) the spine root's subtree: the weapon's upper-body clips
    for (int i = 0; i < n; ++i) {
        bool above = false;
        for (int j = i, guard = 0; j >= 0 && guard < 128; j = parentOf(j), ++guard)
            if (j == spineRoot) { above = true; break; }
        g_beamUpperMask[i] = above ? 1 : 0;
        upper += above;
    }
    g_beamUpperSegmentCount.store(upper > 0 ? n : 0);
    // the spine bones turned with the camera: from the spine root up to the clavicles' bone
    int spine = 0;
    bool from = false;
    for (int k = length - 1; k >= 0 && spine < 16; --k) {
        if (chain[k] == spineRoot) from = true;
        if (from) g_beamSpine[spine++] = chain[k];
    }
    if (spine <= 0) {
        g_beamLegMaskCount.store(-1);
        return;
    }
    g_beamSpineCount.store(spine);
    g_beamLegMaskCount.store(n);
    Log::Detailf("CombatAir: beam aim: Lara's pelvis is segment %d%s, spine %d..%d (%d bones), %d lower-body and %d upper-body "
                "segments", pelvis, inChain(pelvis) ? "" : " (beside the spine)", g_beamSpine[0], g_beamSpine[spine - 1], spine,
                lower, upper);
}

void QuatSlerp(const float* a, const float* b, float t, float* out) {
    float bb[4] = {b[0], b[1], b[2], b[3]};
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (dot < 0.0f) {
        for (float& c : bb) c = -c;
        dot = -dot;
    }
    float wa = 1.0f - t, wb = t;
    if (dot < 0.9995f) {
        const float theta = std::acos(std::min(dot, 1.0f)), s = std::sin(theta);
        wa = std::sin((1.0f - t) * theta) / s;
        wb = std::sin(t * theta) / s;
    }
    float n = 0.0f;
    for (int i = 0; i < 4; ++i) {
        out[i] = wa * a[i] + wb * bb[i];
        n += out[i] * out[i];
    }
    n = std::sqrt(n);
    if (n > 1e-9f)
        for (int i = 0; i < 4; ++i) out[i] /= n;
}

// (v0.9.486) see g_beamUprightWeight
void ApplyBeamUprightUnsafe(std::uint32_t count, std::uint8_t* segs, const std::uint8_t* skeleton) {
    const float weight = std::clamp(g_beamUprightWeight.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const int spineCount = g_beamSpineCount.load(std::memory_order_relaxed);
    if (weight <= 0.001f || spineCount <= 0) return;
    const int rootBone = g_beamSpine[0];
    if (rootBone < 0 || static_cast<std::uint32_t>(rootBone) >= count) return;
    const auto parentOf = [&](int i) {
        return *reinterpret_cast<const std::int32_t*>(skeleton + static_cast<std::size_t>(i) * kSkeletonSegmentSize +
                                                      kSkeletonSegmentParent);
    };
    const auto rotation = [&](int i) { return reinterpret_cast<float*>(segs + static_cast<std::size_t>(i) * kPoseSegmentSize); };
    int chain[64];
    int length = 0;
    for (int i = parentOf(rootBone); i >= 0 && length < 64; i = parentOf(i)) {
        if (static_cast<std::uint32_t>(i) >= count) return;
        chain[length++] = i;
    }
    M3 parent = M3Identity();
    for (int k = length - 1; k >= 0; --k) parent = M3Mul(parent, M3FromRotvec(rotation(chain[k])));
    const float bindForward[3] = {0.0f, -1.0f, 0.0f};
    float forward[3];
    M3Apply(parent, bindForward, forward);
    if (forward[0] * forward[0] + forward[1] * forward[1] < 1e-4f) return;   // (facing straight up or down: leave it)
    const float yawVec[3] = {0.0f, 0.0f, std::atan2(forward[0], -forward[1])};
    const M3 yaw = M3FromRotvec(yawVec);
    float* local = rotation(rootBone);
    const M3 clip = M3FromRotvec(local);
    const M3 upright = M3Mul(M3Transpose(parent), M3Mul(yaw, clip));
    float qa[4], qb[4], q[4], v[3];
    QuatFromRotvec(local, qa);
    QuatFromM3(upright, qb);
    QuatSlerp(qa, qb, weight, q);
    RotvecFromQuat(q, v);
    local[0] = v[0];
    local[1] = v[1];
    local[2] = v[2];
}

void ApplyBeamTwistUnsafe(std::uint32_t count, std::uint8_t* segs, const std::uint8_t* skeleton, const float* out) {
    static const long long s_frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const long long last = g_beamTwistLastTicks.exchange(now.QuadPart, std::memory_order_relaxed);
    const float dt = last ? std::clamp(static_cast<float>(now.QuadPart - last) / static_cast<float>(s_frequency), 0.0f, 0.1f) : 0.0f;
    const float target = g_beamTwistTarget.load(std::memory_order_relaxed);
    float weight = g_beamTwistWeight.load(std::memory_order_relaxed);
    weight = target > weight ? std::min(target, weight + 6.0f * dt) : std::max(target, weight - 6.0f * dt);
    g_beamTwistWeight.store(weight, std::memory_order_relaxed);
    const int spineCount = g_beamSpineCount.load(std::memory_order_relaxed);
    if (weight <= 0.001f || spineCount <= 0) return;
    const auto parentOf = [&](int i) {
        return *reinterpret_cast<const std::int32_t*>(skeleton + static_cast<std::size_t>(i) * kSkeletonSegmentSize +
                                                      kSkeletonSegmentParent);
    };
    // the camera's aim in the root's parent space (straight ahead is about (0, -1, 0), up +Z: see ApplyAimIkUnsafe)
    const float* parent = out - 16;
    const float world[3] = {g_beamDirX.load(std::memory_order_relaxed), g_beamDirY.load(std::memory_order_relaxed),
                            g_beamDirZ.load(std::memory_order_relaxed)};
    float aim[3];
    for (int j = 0; j < 3; ++j) {
        const float* row = parent + j * 4;
        const float length = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
        if (length < 0.5f || length > 2.0f) return;
        aim[j] = (world[0] * row[0] + world[1] * row[1] + world[2] * row[2]) / length;
    }
    if (aim[0] * aim[0] + aim[1] * aim[1] < 0.01f) return;   // straight up or down: no heading
    const float maxTurn = std::clamp(g_beamTurnMaxDeg.load(std::memory_order_relaxed), 0.0f, 90.0f) * 0.0174533f;
    const float yaw = (std::clamp(std::atan2(aim[0], -aim[1]), -maxTurn, maxTurn) +
                       g_beamYawCorrection.load(std::memory_order_relaxed)) * weight;   // (v0.9.482: the clips' heading)
    const auto rotation = [&](int i) { return reinterpret_cast<float*>(segs + static_cast<std::size_t>(i) * kPoseSegmentSize); };
    // the world (root parent space) rotation of the first spine bone's parent
    int chain[64];
    int length = 0;
    for (int i = parentOf(g_beamSpine[0]); i >= 0 && length < 64; i = parentOf(i)) {
        if (static_cast<std::uint32_t>(i) >= count) return;
        chain[length++] = i;
    }
    M3 world3 = M3Identity();
    for (int k = length - 1; k >= 0; --k) world3 = M3Mul(world3, M3FromRotvec(rotation(chain[k])));
    const float stepVec[3] = {0.0f, 0.0f, yaw / static_cast<float>(spineCount)};
    const M3 step = M3FromRotvec(stepVec);
    for (int k = 0; k < spineCount; ++k) {
        const int bone = g_beamSpine[k];
        if (static_cast<std::uint32_t>(bone) >= count) return;
        const M3 local = M3FromRotvec(rotation(bone));
        const M3 turned = M3Mul(M3Transpose(world3), M3Mul(step, M3Mul(world3, local)));
        float q[4], v[3];
        QuatFromM3(turned, q);
        RotvecFromQuat(q, v);
        float* dst = rotation(bone);
        dst[0] = v[0]; dst[1] = v[1]; dst[2] = v[2];
        world3 = M3Mul(world3, turned);
    }
}

// (v0.9.482) After the build: the arms' heading against the aim's, into the twist's correction (see g_beamYawCorrection).
void MeasureBeamAimUnsafe(std::uint32_t count, const float* out) {
    const int spineCount = g_beamSpineCount.load(std::memory_order_relaxed);
    if (spineCount <= 0 || count <= static_cast<std::uint32_t>(std::max(kLaraLeftWrist, kLaraRightWrist))) return;
    const int top = g_beamSpine[spineCount - 1];
    if (top < 0 || static_cast<std::uint32_t>(top) >= count) return;
    const float* chest = out + static_cast<std::size_t>(top) * 16 + 12;
    const float* left = out + static_cast<std::size_t>(kLaraLeftWrist) * 16 + 12;
    const float* right = out + static_cast<std::size_t>(kLaraRightWrist) * 16 + 12;
    const float arms[3] = {(left[0] + right[0]) * 0.5f - chest[0], (left[1] + right[1]) * 0.5f - chest[1],
                           (left[2] + right[2]) * 0.5f - chest[2]};
    const float* parent = out - 16;
    const float world[3] = {g_beamDirX.load(std::memory_order_relaxed), g_beamDirY.load(std::memory_order_relaxed),
                            g_beamDirZ.load(std::memory_order_relaxed)};
    float a[3], c[3];
    for (int j = 0; j < 3; ++j) {
        const float* row = parent + j * 4;
        const float length = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
        if (length < 0.5f || length > 2.0f) return;
        a[j] = (arms[0] * row[0] + arms[1] * row[1] + arms[2] * row[2]) / length;
        c[j] = (world[0] * row[0] + world[1] * row[1] + world[2] * row[2]) / length;
    }
    if (a[0] * a[0] + a[1] * a[1] < 15.0f * 15.0f) return;   // (the hands near the chest: no heading)
    if (c[0] * c[0] + c[1] * c[1] < 0.01f) return;
    const float maxTurn = std::clamp(g_beamTurnMaxDeg.load(std::memory_order_relaxed), 0.0f, 90.0f) * 0.0174533f;
    const float want = std::clamp(std::atan2(c[0], -c[1]), -maxTurn, maxTurn);
    float error = want - std::atan2(a[0], -a[1]);
    while (error > 3.14159265f) error -= 6.2831853f;
    while (error < -3.14159265f) error += 6.2831853f;
    const float step = std::clamp(error * 0.2f, -0.105f, 0.105f);
    const float correction = std::clamp(g_beamYawCorrection.load(std::memory_order_relaxed) + step, -2.62f, 2.62f);
    g_beamYawCorrection.store(correction, std::memory_order_relaxed);
    g_beamYawMeasures.fetch_add(1, std::memory_order_relaxed);
}

void __fastcall HookAnimBuildTransforms(void* instance, std::uint32_t count, std::uint8_t* segs,
                                        const std::uint8_t* skeleton, float* out) {
    InstanceAnim::OnBuildTransforms(instance, count, segs, skeleton);  // a clip played on an Instance (Animation tab)
    if (instance && segs && skeleton && out && instance == g_laraInstance.load(std::memory_order_relaxed) &&
        (g_ikTarget.load(std::memory_order_relaxed) > 0.0f || g_ikWeight.load(std::memory_order_relaxed) > 0.0f)) {
#if defined(_MSC_VER)
        __try {
#endif
            ApplyAimIkUnsafe(count, segs, skeleton, out);
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_ikWeight.store(0.0f, std::memory_order_relaxed);
        }
#endif
    }
    if (instance && segs && skeleton && out && instance == g_laraInstance.load(std::memory_order_relaxed)) {   // (v0.9.475)
#if defined(_MSC_VER)
        __try {
#endif
            if (g_beamLegSkeleton.load(std::memory_order_relaxed) != skeleton) {
                g_beamLegSkeleton.store(skeleton, std::memory_order_relaxed);
                g_beamLegMaskCount.store(0);
                g_beamSpineCount.store(0);
                ComputeBeamLegMaskUnsafe(count, skeleton);
            }
            ApplyBeamUprightUnsafe(count, segs, skeleton);   // (v0.9.486) before the twist
            if (g_beamTwistTarget.load(std::memory_order_relaxed) > 0.0f || g_beamTwistWeight.load(std::memory_order_relaxed) > 0.0f)
                ApplyBeamTwistUnsafe(count, segs, skeleton, out);
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_beamTwistWeight.store(0.0f, std::memory_order_relaxed);
        }
#endif
    }
    g_originalAnimBuildTransforms(instance, count, segs, skeleton, out);
    if (instance && out && instance == g_laraInstance.load(std::memory_order_relaxed))
        Grapple::OnLaraTransforms(count, out);  // the hand the grapple rope leaves from
    if (instance && out && instance == g_laraInstance.load(std::memory_order_relaxed) &&
        g_beamMeasureAim.load(std::memory_order_relaxed) && g_beamTwistWeight.load(std::memory_order_relaxed) >= 0.95f) {
#if defined(_MSC_VER)
        __try {
#endif
            MeasureBeamAimUnsafe(count, out);   // (v0.9.482)
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_beamMeasureAim.store(false, std::memory_order_relaxed);
        }
#endif
    }
}

// Game thread: the camera's aim direction (world, unit) for the aim IK.
bool CameraAimDirection(void* instance, float* dir) {
    if (!g_cameraForward || !g_cameraManager) return false;
#if defined(_MSC_VER)
    __try {
#endif
        auto* playerComponent =
            InstanceComponent(static_cast<std::uint8_t*>(instance), *At<const std::uint32_t*>(kRvaPlayerComponentId));
        if (!playerComponent) return false;
        auto* player = *reinterpret_cast<std::uint8_t**>(playerComponent + kPlayerComponentPlayer);
        void* manager = *g_cameraManager;
        if (!player || !manager) return false;
        alignas(16) float forward[4]{};
        g_cameraForward(manager, forward, *reinterpret_cast<const std::uint32_t*>(player + kPlayerCameraId));
        const float length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
        if (!std::isfinite(length) || length < 1e-3f) return false;
        for (int c = 0; c < 3; ++c) dir[c] = forward[c] / length;
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

// Every state change of any agent: the roll chain, hand-to-hand and grapple kick see Lara's.
void __fastcall HookDoTransition(void* agent, const std::uint8_t* transition) {
    Acro::OnStateTransition(agent, transition);  // roll chain: picks the clip before the state's animation starts
    Brawl::OnStateTransition(agent, transition); // hand-to-hand: a melee push state after an X press
    // Hand-to-hand: the change the brawl forced goes to the chain's next melee state (runs the original itself).
    if (Brawl::RedirectTransition(agent, transition, g_originalDoTransition)) return;
    // Grapple kick: a melee press with a target at range goes to combat_melee_kick (runs the original itself).
    if (Grapple::RedirectTransition(agent, transition, g_originalDoTransition)) return;
    g_originalDoTransition(agent, transition);
}

bool IsLaraAnimComponent(void* component) {
#if defined(_MSC_VER)
    __try {
#endif
        return *reinterpret_cast<void**>(static_cast<std::uint8_t*>(component) + kAnimComponentOwner) ==
               g_laraInstance.load(std::memory_order_relaxed);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

// The state control sets Lara's moveset goal on state entry; while aiming in the air "InAir" becomes
// "Combat". The tick's own changes call the original directly and are not recorded here.
bool __fastcall HookSetAnimGraphGoal(void* component, int type, std::uint32_t value) {
    if (type > 0 && type < kGoalTypes && component && IsLaraAnimComponent(component)) {   // (v0.9.479, for the log)
        static std::atomic<int> s_beamGoalLogs{0};
        if (g_stanceOnBeam.load(std::memory_order_relaxed) && s_beamGoalLogs.fetch_add(1, std::memory_order_relaxed) < 24)
            Log::Detailf("CombatAir: beam aim: the state control sets goal %d to %08X", type, value);
    }
    if (type == kGoalMoveset && component && IsLaraAnimComponent(component)) {
        g_laraAnimComponent.store(component, std::memory_order_relaxed);
        g_scMovesetGoal.store(value, std::memory_order_relaxed);
        if (g_stanceActive.load(std::memory_order_relaxed) &&
            (value == kMovesetInAir || (g_stanceOnBeam.load(std::memory_order_relaxed) && value != kMovesetCombat))) {
            g_stanceSwaps.fetch_add(1, std::memory_order_relaxed);
            value = kMovesetCombat;
        }
    }
    return g_originalSetAnimGraphGoal(component, type, value);
}

// (v0.9.479) Back on the beam after aiming: the beam moveset ("BalanceBeam") is entered again, and the graph took its
// first state, the attach (Lara "got on" the beam again: BBeam_HopAttach / BBeam_TumblingAttach are state goals the
// attach states set; the idle states set Beam_IdleParallel / Beam_IdlePerpendicular; read from the game's data with the
// goals' hash, cdc::HashLowerCase("animgoalstate", name)). A goal set to the value it has is ignored
// (SetAnimGraphGoal: no change, no time stamp, no InstanceAnimGoalChanged event), so the cluster and state goals the
// state control set last (unchanged while aiming) are set to 0 and back right after the moveset: the graph sees them
// newly set and goes to that state rather than its entry.
std::uint32_t CurrentGoal(void* component, int type) {
#if defined(_MSC_VER)
    __try {
#endif
        return *reinterpret_cast<const std::uint32_t*>(static_cast<std::uint8_t*>(component) + kAnimComponentGoals + 4 * type);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
#endif
}

void RetriggerBeamGoals(void* component) {
    std::uint32_t values[2]{};
    const int types[2] = {kGoalCluster, kGoalState};
    for (int i = 0; i < 2; ++i) {
        values[i] = CurrentGoal(component, types[i]);
        if (!values[i]) continue;
        g_originalSetAnimGraphGoal(component, types[i], 0);
        g_originalSetAnimGraphGoal(component, types[i], values[i]);
    }
    static int s_logs = 0;
    if (s_logs++ < 12)
        Log::Detailf("CombatAir: beam aim ended: the beam moveset back, its cluster %08X and state %08X goals set again", values[0],
                     values[1]);
}

std::uint32_t CurrentMovesetGoal(void* component) {
#if defined(_MSC_VER)
    __try {
#endif
        return *reinterpret_cast<const std::uint32_t*>(static_cast<std::uint8_t*>(component) + kAnimComponentGoals);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
#endif
}

// PlayerSteering::Process for Lara while aiming in the air: turn her towards where the camera looks,
// like the ground aim states do (Turn_TargetDir / Turn_CameraDir at 360 deg/s). The jump states leave
// the steering off (jump_rising: mode None) or at 4 deg/s with a ramp (jump_falling). Set every frame
// right before the steering runs: no ramp, no time limit, yaw only, target = camera forward.
void FaceCameraInAir(std::uint8_t* steering) {
#if defined(_MSC_VER)
    __try {
#endif
        void* manager = *g_cameraManager;
        if (!manager) return;
        const std::uint32_t cameraId =
            *reinterpret_cast<const std::uint32_t*>(steering - kPlayerSteering + kPlayerCameraId);
        alignas(16) float forward[4]{};
        g_cameraForward(manager, forward, cameraId);
        const float length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1]);
        if (!std::isfinite(length) || length < 1.0e-3f) return;
        alignas(16) const float direction[4]{forward[0] / length, forward[1] / length, 0.0f, 0.0f};
        const float rate = g_airTurnSpeed.load(std::memory_order_relaxed) * (3.14159265f / 180.0f);
        g_setTurnParams(steering, rate, 0.0f, 0.0f, 0.0f, true, 0, nullptr);
        *reinterpret_cast<std::int32_t*>(steering + kSteeringMode) = kSteeringModeYaw;
        steering[kSteeringTargetDirty] = 1;
        steering[kSteeringDeltaDirty] = 1;
        g_setTargetDir(steering, direction);
        g_airFacingFrames.fetch_add(1, std::memory_order_relaxed);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#endif
}

// The same yaw steering towards a given direction (unit x, y) at rateDeg per second (the grapple kick).
void FaceDirection(std::uint8_t* steering, float dirX, float dirY, float rateDeg) {
#if defined(_MSC_VER)
    __try {
#endif
        alignas(16) const float direction[4]{dirX, dirY, 0.0f, 0.0f};
        g_setTurnParams(steering, rateDeg * (3.14159265f / 180.0f), 0.0f, 0.0f, 0.0f, true, 0, nullptr);
        *reinterpret_cast<std::int32_t*>(steering + kSteeringMode) = kSteeringModeYaw;
        steering[kSteeringTargetDirty] = 1;
        steering[kSteeringDeltaDirty] = 1;
        g_setTargetDir(steering, direction);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#endif
}

void __fastcall HookSteeringProcess(void* steering) {
    if (steering && *reinterpret_cast<void* const*>(static_cast<std::uint8_t*>(steering) + kSteeringInstance) ==
                        g_laraInstance.load(std::memory_order_relaxed)) {
        float dirX = 0.0f, dirY = 0.0f, rate = 0.0f;
        if (Grapple::FacingOverride(dirX, dirY, rate) || Brawl::FacingOverride(dirX, dirY, rate))
            FaceDirection(static_cast<std::uint8_t*>(steering), dirX, dirY, rate);
        else if (g_airFacingActive.load(std::memory_order_relaxed))
            FaceCameraInAir(static_cast<std::uint8_t*>(steering));
    }
    g_originalSteeringProcess(steering);
}

// Game thread: one of Lara's sound events (EventSystem::SendEvent by name, the way VisionMode::Enable
// plays the survival instinct sound with "evVisionSoundFadeIn").
void SendSoundEvent(void* instance, const char* eventName) {
    if (!g_soundAvailable || !g_slowSound.load(std::memory_order_relaxed) || !instance || !eventName || !eventName[0])
        return;
    bool sent = false;
#if defined(_MSC_VER)
    __try {
#endif
        void* system = *g_eventSystem;
        if (system) {
            g_sendEvent(system, eventName, instance, 0, nullptr, 0);
            sent = true;
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sent = false;
    }
#endif
    static int s_logged = 0;
    if (s_logged < 4) {
        ++s_logged;
        Log::Detailf("CombatAir: sound event '%s' %s", eventName, sent ? "sent to Lara" : "not sent (no event system)");
    }
}

// Combat_AccurateAim (state property 499) turns the CombatManager's accurate aim on and off: the aim
// probe from the camera that puts the aim target under the reticle (CombatManager::UpdateAccurateAim),
// which the arms aim at and the shots fly to, and what Tertiary Aiming's combat_aim needs
// (Status_InAccurateAim). Every state that does not set it turns it off, the jump states included.
// While aiming in the air an "off" for Lara is answered with "on".
void __fastcall HookSetAccurateAim(void* agent, const std::uint8_t* property) {
    if (g_airAccurateAim.load(std::memory_order_relaxed) && agent && property && property[8] == 0 &&
        *reinterpret_cast<void**>(static_cast<std::uint8_t*>(agent) + kAgentInstance) ==
            g_laraInstance.load(std::memory_order_relaxed)) {
        g_accurateAimKept.fetch_add(1, std::memory_order_relaxed);
        g_originalSetAccurateAim(agent, kAccurateAimOn);
        return;
    }
    g_originalSetAccurateAim(agent, property);
}

// Game thread: accurate aim on at the start of an air aim, the state's own value back at its end.
void ApplyAirAccurateAim(std::uint8_t* component, bool on) {
#if defined(_MSC_VER)
    __try {
#endif
        void* primary = *reinterpret_cast<void**>(component + kComponentBaseAgent);
        if (!primary) return;
        const std::uint8_t* property = kAccurateAimOn;
        if (!on) {
            property = g_getPropertyData(component, static_cast<std::uint16_t>(kPropAccurateAim));
            if (!property || *reinterpret_cast<const std::int16_t*>(property + 2) != kPropTypeBoolean)
                property = kAccurateAimOff;
        }
        g_originalSetAccurateAim(primary, property);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#endif
}

bool __fastcall HookInstinctHighlight(void* component, void* player) {
    if (g_instinctNoHighlights.load(std::memory_order_relaxed)) {
        g_instinctHighlightsSkipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return g_originalInstinctHighlight(component, player);
}

bool __fastcall HookAsymInstinctHighlight(void* component, void* player) {
    if (g_instinctNoHighlights.load(std::memory_order_relaxed)) {
        g_instinctHighlightsSkipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return g_originalAsymInstinctHighlight(component, player);
}

// VisionMode::Process adds the asymmetric highlights every frame (their EnableHighlight is inlined there).
void __fastcall HookAddAsymComponents(void* vision) {
    if (g_instinctNoHighlights.load(std::memory_order_relaxed)) return;
    g_originalAddAsymComponents(vision);
}

void __fastcall HookNotifyDialogueContext(void* manager, const char* context) {
    if (g_instinctNoDialogue.load(std::memory_order_relaxed) && context &&
        std::strcmp(context, kInstinctDialogueContext) == 0)
        return;
    g_originalNotifyDialogueContext(manager, context);
}

// Game thread: the survival instinct view (desaturated look, particles, its sound), the way the game's
// own listener starts it (VisionMode::Enable, mode Instinct), without the object highlights, the
// objective indicator (m_BeaconTurnedOn is cleared around the call) and the dialogue hint. Only when
// the view is off; one the player started stays theirs.
void StartInstinctView(void* player) {
    if (!g_instinctAvailable || !g_slowInstinct.load(std::memory_order_relaxed) || !player) return;
    const char* result = "started";
#if defined(_MSC_VER)
    __try {
#endif
        std::uint8_t* vision = *g_visionMode;
        if (!vision) {
            result = "unavailable (no VisionMode)";
        } else if (*reinterpret_cast<const std::int32_t*>(vision + kVisionStatus) != 0) {
            result = "left alone (already on)";
        } else {
            const std::uint8_t beacon = vision[kVisionBeacon];
            vision[kVisionBeacon] = 0;
            g_instinctNoHighlights.store(true, std::memory_order_relaxed);
            g_instinctNoDialogue.store(true, std::memory_order_relaxed);
            g_visionEnable(vision, player, kVisionModeInstinct, kVisionSeconds);
            g_instinctNoDialogue.store(false, std::memory_order_relaxed);
            vision[kVisionBeacon] = beacon;
            if (*reinterpret_cast<const std::int32_t*>(vision + kVisionStatus) != 0) {
                g_instinctOurs.store(true, std::memory_order_relaxed);
            } else {
                g_instinctNoHighlights.store(false, std::memory_order_relaxed);
                result = "refused by the game (menu, campsite or no capabilities)";
            }
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_instinctNoHighlights.store(false, std::memory_order_relaxed);
        g_instinctNoDialogue.store(false, std::memory_order_relaxed);
        result = "failed (exception)";
    }
#endif
    Log::Detailf("CombatAir: survival instinct view %s", result);
}

void StopInstinctView(void* player) {
    if (!g_instinctOurs.exchange(false, std::memory_order_relaxed)) return;
#if defined(_MSC_VER)
    __try {
#endif
        std::uint8_t* vision = *g_visionMode;
        if (vision && player && *reinterpret_cast<const std::int32_t*>(vision + kVisionStatus) != 0) {
            const std::uint8_t beacon = vision[kVisionBeacon];
            vision[kVisionBeacon] = 0;
            g_visionDisable(vision, player);
            vision[kVisionBeacon] = beacon;
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#endif
    g_instinctNoHighlights.store(false, std::memory_order_relaxed);
    Log::Detailf("CombatAir: survival instinct view ended (%u highlights skipped)",
                g_instinctHighlightsSkipped.exchange(0, std::memory_order_relaxed));
}

// Game thread: the view we started may be turned off by the game (its timer, the player's own button).
void WatchInstinctView() {
    if (!g_instinctOurs.load(std::memory_order_relaxed)) return;
    bool off = false;
#if defined(_MSC_VER)
    __try {
#endif
        std::uint8_t* vision = *g_visionMode;
        off = !vision || *reinterpret_cast<const std::int32_t*>(vision + kVisionStatus) == 0;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        off = true;
    }
#endif
    if (!off) return;
    g_instinctOurs.store(false, std::memory_order_relaxed);
    g_instinctNoHighlights.store(false, std::memory_order_relaxed);
    Log::Detail("CombatAir: survival instinct view turned off by the game");
}

// Game thread, every frame while aiming in the air: the aim pitch and yaw that the ground aim states
// compute with their Combat_AvoidWeaponClipping activity (camera forward against Lara's facing). The
// weapon graph bends her upper body with them ("Vertical_Blend" / "Horizontal_Blend"); the jump states
// do not run the activity, so in the air they stayed at the last ground value.
std::atomic<std::uint32_t> g_aimAngleFrames{0};

void UpdateAirAimAngles(std::uint8_t* component) {
#if defined(_MSC_VER)
    __try {
#endif
        auto* const* agents = *reinterpret_cast<std::uint8_t* const* const*>(component + kComponentAgents);
        std::uint8_t* first = agents ? agents[0] : nullptr;
        if (!first) return;
        g_avoidClippingAiming(first);
        g_aimAngleFrames.fetch_add(1, std::memory_order_relaxed);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#endif
}

// Speeds as the request's two factors: Lara runs at global, the rest of the world at world * global.
void SlowFactors(float& world, float& global) {
    const float lara = std::clamp(g_slowLaraSpeed.load(std::memory_order_relaxed), 0.05f, 1.0f);
    const float others = std::clamp(g_slowWorldSpeed.load(std::memory_order_relaxed), 0.05f, 1.0f);
    global = lara;
    world = std::min(1.0f, others / lara);
}

bool SlowSlotOurs() {
    auto* slot = reinterpret_cast<const TimeDilationRequestRaw*>(
        g_timeDilationManager + static_cast<std::size_t>(kTimeDilationTypeDefault) * kTimeDilationRequestSize);
    return slot->priority == kTimeDilationTypeDefault;
}

void RequestSlowMotion() {
    float world = 1.0f, global = 1.0f;
    SlowFactors(world, global);
    alignas(16) TimeDilationRequestRaw request{};
    g_requestCtor(&request, kTimeDilationTypeDefault, world, global, kSlowMotionName);
    g_request(g_timeDilationManager, &request);
    g_slowAppliedWorld = world;
    g_slowAppliedGlobal = global;
}

void StartSlowMotion(void* instance) {
    RequestSlowMotion();
    StartInstinctView(instance);
    SendSoundEvent(instance, g_slowSoundEvent);
    g_slowActive = true;
    g_slowStartMs = GetTickCount64();
    g_slowInstance = instance;
    g_slowSlotLostLogged = false;
    g_statusSlowActive.store(true, std::memory_order_relaxed);
    Log::Detailf("CombatAir: slow motion on (world %.0f%%, Lara %.0f%%)",
                g_slowAppliedWorld * g_slowAppliedGlobal * 100.0f, g_slowAppliedGlobal * 100.0f);
}

void StopSlowMotion(const char* why) {
    if (!g_slowActive) return;
    if (SlowSlotOurs()) g_cancel(g_timeDilationManager, kTimeDilationTypeDefault, kSlowMotionName);
    if (g_slowInstance == g_laraInstance.load(std::memory_order_relaxed)) {
        StopInstinctView(g_slowInstance);
        SendSoundEvent(g_slowInstance, g_slowSoundEndEvent);
    } else {
        StopInstinctView(nullptr);
    }
    g_slowActive = false;
    g_slowInstance = nullptr;
    g_statusSlowActive.store(false, std::memory_order_relaxed);
    Log::Detailf("CombatAir: slow motion off (%s, %.1f s)", why,
                static_cast<double>(GetTickCount64() - g_slowStartMs) / 1000.0);
}

// (v0.9.475) the primary agent's state name (null: none)
const char* PrimaryStateName(std::uint8_t* component) {
#if defined(_MSC_VER)
    __try {
#endif
        auto* primary = *reinterpret_cast<std::uint8_t**>(component + kComponentBaseAgent);
        auto* state = primary ? *reinterpret_cast<const std::uint8_t**>(primary + kAgentCurrentState) : nullptr;
        return state ? *reinterpret_cast<const char* const*>(state + kStateName) : nullptr;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
#endif
}

// (v0.9.480) The weapon on her upper body only, on a beam. 0.9.479's video: the weapon's combat moveset under the beam
// clips' legs looked wrong, and leaving it re-entered the beam moveset from its attach (the "hop"). Now on a beam the
// moveset is never changed: Lara's graph keeps the beam's own animations (legs, balance, walking), and her spine root and
// everything above it (the subtree of the spine bone above the pelvis) take the weapon's own upper-body clips, the ones
// its hosted graph plays (the weapons' libraries, read from the game's archives): Hosted_<weapon>_Aim / _Fire / _Reload
// (Level, High, Low by the camera's pitch), _Equip, _UnEquip; the bow's Quick_Ready / _Draw / _Charge / _Fire. The clip
// follows the weapon agent's state (equip, unequip, reload, fire, the bow's draw and charge, else aiming), a new shot
// starts the fire clip again, and nothing plays once the weapon agent is back in "default". The weapon itself reaches her
// hand through the state control (its states' property PSCPF_InventoryMoveToReady -> InventoryComponent::Activate), and
// Frames_InStateHosted counts the weapon agent's own frames (0.9.202), so no hosted graph is needed for it. The upper
// body still turns with the camera, up to 90 degrees.
struct WeaponClips {
    const char* aim[3];      // level, high, low
    const char* fire[3];
    const char* reload[3];
    const char* equip;
    const char* unequip;
    const char* draw[3];     // (the bow)
    const char* charge[3];
};
constexpr WeaponClips kPistolClips = {{"Hosted_Pistol_Aim_Level", "Hosted_Pistol_Aim_High", "Hosted_Pistol_Aim_Low"},
                                      {"Hosted_Pistol_Fire_Level", "Hosted_Pistol_Fire_High", "Hosted_Pistol_Fire_Low"},
                                      {"Hosted_Pistol_Reload_Level", "Hosted_Pistol_Reload_High", "Hosted_Pistol_Reload_low"},
                                      "Hosted_Pistol_Equip", "Hosted_Pistol_UnEquip", {}, {}};
constexpr WeaponClips kDualPistolClips = {
    {"Hosted_DualPistol_AimLevel", "Hosted_DualPistol_AimHigh", "Hosted_DualPistol_AimLow"},
    {"Hosted_DualPistol_FireBoth_Level", "Hosted_DualPistol_FireBoth_High", "Hosted_DualPistol_FireBoth_Low"},
    {"Hosted_DualPistol_Reload_Level", "Hosted_DualPistol_Reload_High", "Hosted_DualPistol_Reload_Low"},
    "Hosted_DualPistol_Equip", "Hosted_DualPistol_Unequip", {}, {}};
constexpr WeaponClips kSmgClips = {{"Hosted_SMG_Aim_Level", "Hosted_SMG_Aim_High", "Hosted_SMG_Aim_Low"},
                                   {"Hosted_SMG_Fire_Level", "Hosted_SMG_Fire_High", "Hosted_SMG_Fire_Low"},
                                   {"Hosted_SMG_Reload", "Hosted_SMG_Reload", "Hosted_SMG_Reload"},
                                   "Hosted_SMG_Equip", "Hosted_SMG_UnEquip", {}, {}};
constexpr WeaponClips kShotgunClips = {{"Hosted_Shotgun_Aim_Level", "Hosted_Shotgun_Aim_High", "Hosted_Shotgun_Aim_Low"},
                                       {"Hosted_Shotgun_Fire_Level", "Hosted_Shotgun_Fire_High", "Hosted_Shotgun_Fire_Low"},
                                       {"Hosted_Shotgun_Reload", "Hosted_Shotgun_Reload_High", "Hosted_Shotgun_Reload_Low"},
                                       "Hosted_Shotgun_Equip", "Hosted_Shotgun_UnEquip", {}, {}};
constexpr WeaponClips kBowClips = {{"Hosted_Bow_Quick_Ready", "Hosted_Bow_Quick_Ready_High", "Hosted_Bow_Quick_Ready_Low"},
                                   {"Hosted_Bow_Quick_Fire", "Hosted_Bow_Quick_Fire_high", "Hosted_Bow_Quick_Fire_low"},
                                   {"Hosted_Bow_Quick_Ready", "Hosted_Bow_Quick_Ready_High", "Hosted_Bow_Quick_Ready_Low"},
                                   "Hosted_Bow_Quick_Equip", "Hosted_Bow_Quick_UnEquip",
                                   {"Hosted_Bow_Quick_Draw", "Hosted_Bow_Quick_Draw_high", "Hosted_Bow_Quick_Draw_low"},
                                   {"Hosted_Bow_Quick_Charge", "Hosted_Bow_Quick_Charge_high", "Hosted_Bow_Quick_Charge_low"}};

// the weapon agent's state ("Secondary Agent"), null: none
const char* WeaponAgentState(std::uint8_t* component) {
#if defined(_MSC_VER)
    __try {
#endif
        auto* dtp = *reinterpret_cast<const std::uint8_t* const*>(component + kComponentDtp);
        auto* const* agents = *reinterpret_cast<std::uint8_t* const* const*>(component + kComponentAgents);
        auto* primary = *reinterpret_cast<std::uint8_t* const*>(component + kComponentBaseAgent);
        const std::uint32_t count = dtp ? std::min(*reinterpret_cast<const std::uint32_t*>(dtp + 8), 8u) : 0u;
        for (std::uint32_t i = 0; agents && i < count; ++i) {
            const std::uint8_t* agent = agents[i];
            if (!agent || agent == primary || !IsWeaponAgent(agent)) continue;
            auto* state = *reinterpret_cast<const std::uint8_t* const*>(agent + kAgentCurrentState);
            return state ? *reinterpret_cast<const char* const*>(state + kStateName) : nullptr;
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#endif
    return nullptr;
}

// game thread: on a beam (upper-body mode), the weapon's clips on her upper body while the weapon is out.
// (v0.9.481) 0.9.480's video and log: the aim's height was one clip at a time (level, high or low by the camera's
// pitch, with a margin of 2 degrees), each change started that clip again from its first frame, and the camera going
// up and down changed it every few frames (the upper body jumping); the aim clip also played while the weapon agent
// was still in "default" (empty hands aiming, before the draw). Now one player: the three aim clips blended by the
// pitch (level / high / low as one weighted sum, the pitch smoothed, high and low full at 60 degrees) and the action
// clip (draw, holster, fire, reload; the bow's draw and charge) over them, played once from its start and faded in
// (0.08 s) and out (0.15 s); a new shot starts the fire clip again (its own clock, the aim's not touched). The whole
// fades in when the weapon is drawn and out when it is back in "default" (0.15 / 0.2 s). Nothing while "default".
constexpr float kBeamAimPitchFull = 1.047f;   // 60 degrees: the high / low aim clip alone
constexpr int kBeamActionLayer = 3;

// the weapon agent's state -> the action (0 none: aiming), and its clip at an aim height
int BeamActionKind(const WeaponClips& clips, const char* state) {
    if (std::strstr(state, "unequip")) return 1;
    if (std::strstr(state, "equip")) return 2;
    if (std::strstr(state, "reload")) return 3;
    if (std::strstr(state, "fire")) return 4;
    if (clips.charge[0] && std::strstr(state, "charge")) return 5;
    if (clips.draw[0] && std::strstr(state, "draw")) return 6;
    return 0;
}
const char* BeamActionClip(const WeaponClips& clips, int kind, int level) {
    switch (kind) {
    case 1: return clips.unequip;
    case 2: return clips.equip;
    case 3: return clips.reload[level];
    case 4: return clips.fire[level];
    case 5: return clips.charge[level];
    case 6: return clips.draw[level];
    default: return nullptr;
    }
}

void UpdateBeamAimBody(void* instance, std::uint8_t* component, bool onBeam, bool aiming) {
    static std::uint32_t s_id = 0;
    static std::uint64_t s_componentId = 0;
    static const WeaponClips* s_clips = nullptr;   // the player's aim clips (null: no player)
    static const char* s_action = nullptr;         // its action layer's clip (null: none)
    static bool s_actionOn = false;                // the action still wanted (else fading out)
    static int s_actionKind = 0;
    static float s_master = 0.0f, s_actionWeight = 0.0f, s_pitch = 0.0f;
    static std::uint32_t s_shots = 0;
    static long long s_last = 0;
    static const char* s_failedClip = nullptr;
    static ULONGLONG s_failedAt = 0;
    static const char* s_lastStates[24]{};
    static int s_stateLogs = 0;
    const ULONGLONG now = GetTickCount64();
    static const long long s_frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    const float dt = s_last ? std::clamp(static_cast<float>(qpc.QuadPart - s_last) / static_cast<float>(s_frequency), 0.0f, 0.1f) : 0.0f;
    s_last = qpc.QuadPart;

    float dir[3]{0.0f, -1.0f, 0.0f};
    const bool haveDir = instance && CameraAimDirection(instance, dir);
    if (haveDir) {
        g_beamDirX.store(dir[0], std::memory_order_relaxed);
        g_beamDirY.store(dir[1], std::memory_order_relaxed);
        g_beamDirZ.store(dir[2], std::memory_order_relaxed);
    }
    const char* weaponState = onBeam && component ? WeaponAgentState(component) : nullptr;
    const bool weaponOut = weaponState && std::strncmp(weaponState, "combat", 6) == 0;
    if (onBeam && weaponState && s_stateLogs < 24) {   // (the weapon agent's states on a beam, each once, for the log)
        bool seen = false;
        for (int i = 0; i < s_stateLogs && !seen; ++i) seen = s_lastStates[i] == weaponState;
        if (!seen) {
            s_lastStates[s_stateLogs++] = weaponState;
            Log::Detailf("CombatAir: beam aim: the weapon agent's state '%s' on the beam%s", weaponState, aiming ? " (aiming)" : "");
        }
    }

    const WeaponClips* clips = nullptr;
    if (onBeam && weaponOut && g_beamUpperSegmentCount.load() > 0) {
        const int weaponClass = Features::CurrentWeaponClassCached();
        if (weaponClass == static_cast<int>(Features::WeaponClass::Pistol))
            clips = Features::DualWieldExperimentalEnabled() ? &kDualPistolClips : &kPistolClips;
        else if (weaponClass == static_cast<int>(Features::WeaponClass::Smg)) clips = &kSmgClips;
        else if (weaponClass == static_cast<int>(Features::WeaponClass::Shotgun)) clips = &kShotgunClips;
        else if (weaponClass == static_cast<int>(Features::WeaponClass::Bow)) clips = &kBowClips;
    }

    // the aim's height: the camera's pitch, smoothed (about 0.1 s)
    const float pitch = haveDir ? std::asin(std::clamp(dir[2], -1.0f, 1.0f)) : 0.0f;
    s_pitch += (pitch - s_pitch) * std::min(1.0f, dt * 10.0f);
    const float high = std::clamp(s_pitch / kBeamAimPitchFull, 0.0f, 1.0f);
    const float low = std::clamp(-s_pitch / kBeamAimPitchFull, 0.0f, 1.0f);
    const int level = high > 0.5f ? 1 : low > 0.5f ? 2 : 0;   // (an action's clip: the nearest)

    const std::uint64_t componentId =
        instance ? *reinterpret_cast<const std::uint64_t*>(static_cast<std::uint8_t*>(instance) + 0x68) : 0;
    if (s_clips && componentId != s_componentId) {   // (another Lara: a load)
        InstanceAnim::Stop(s_id, s_componentId);
        s_clips = nullptr;
        s_master = s_actionWeight = 0.0f;
    }

    // the action: a new one starts the player again with it (fading in); the one playing fades out when it ends; while
    // the same action goes on, its clip stays (the height it started at)
    const int kind = clips && weaponState ? BeamActionKind(*clips, weaponState) : 0;
    const char* action = kind && kind == s_actionKind && s_actionOn && clips == s_clips ? s_action : clips ? BeamActionClip(*clips, kind, level) : nullptr;
    const std::uint32_t shots = g_airShots.load(std::memory_order_relaxed);
    const bool newShot = shots != s_shots;
    s_shots = shots;
    const ULONGLONG retryWait = 3000;
    if (clips && componentId && (clips != s_clips || (action && action != s_action))) {
        const char* aimClip = clips->aim[0];
        if (!(aimClip == s_failedClip && now - s_failedAt < retryWait)) {
            InstanceAnim::Layer layers[4];
            const float m = clips == s_clips ? s_master : 0.0f;
            const float aimLevel = std::max(0.0f, 1.0f - high - low);
            for (int i = 0; i < 3; ++i) {
                layers[i].name = clips->aim[i];
                layers[i].segmentMask = g_beamUpperMask;
                layers[i].segmentMaskCount = g_beamUpperSegmentCount.load();
                layers[i].sumWithFirst = i > 0;
            }
            layers[0].weight = m * aimLevel;
            layers[1].weight = m * high;
            layers[2].weight = m * low;
            int count = 3;
            const char* playing = action ? action : (clips == s_clips ? s_action : nullptr);
            if (playing) {
                layers[3].name = playing;
                layers[3].segmentMask = g_beamUpperMask;
                layers[3].segmentMaskCount = g_beamUpperSegmentCount.load();
                layers[3].once = true;
                layers[3].weight = action ? 0.0f : m * s_actionWeight;
                count = 4;
            }
            std::string info;
            s_id = static_cast<std::uint32_t>(componentId);
            s_componentId = componentId;
            if (InstanceAnim::PlayLayers(s_id, s_componentId, layers, count, info)) {
                if (clips != s_clips) s_master = 0.0f;
                s_clips = clips;
                s_failedClip = nullptr;
                if (action) {
                    s_action = action;
                    s_actionKind = kind;
                    s_actionOn = true;
                    s_actionWeight = 0.0f;
                }
                static int s_logs = 0;
                if (s_logs++ < 24)
                    Log::Detailf("CombatAir: beam aim: her upper body: %s (weapon state %s; %s)", action ? action : "aiming",
                                 weaponState, info.c_str());
            } else {
                s_failedClip = aimClip;
                s_failedAt = now;
                static int s_fails = 0;
                if (s_fails++ < 8) Log::Writef("CombatAir: beam aim: her upper body could not play %s: %s", action ? action : aimClip, info.c_str());
            }
        }
    } else if (s_clips && action && action == s_action) {
        if (!s_actionOn) {   // (the same action again, e.g. a second reload: from its start)
            InstanceAnim::RestartLayer(s_id, s_componentId, kBeamActionLayer);
            s_actionOn = true;
            s_actionKind = kind;
        } else if (newShot && std::strstr(action, "Fire")) {
            InstanceAnim::RestartLayer(s_id, s_componentId, kBeamActionLayer);
        }
    }
    if (s_action && action != s_action) {
        s_actionOn = false;
        s_actionKind = 0;
    }

    if (!s_clips) {
        g_beamTwistTarget.store(0.0f, std::memory_order_relaxed);
        g_beamMeasureAim.store(false, std::memory_order_relaxed);
        g_beamUprightWeight.store(0.0f, std::memory_order_relaxed);
        return;
    }
    // the weights: the whole in and out with the weapon, the action over the aim
    const bool wanted = clips == s_clips;
    s_master = wanted ? std::min(1.0f, s_master + dt / 0.15f) : std::max(0.0f, s_master - dt / 0.2f);
    s_actionWeight = s_actionOn ? std::min(1.0f, s_actionWeight + dt / 0.08f) : std::max(0.0f, s_actionWeight - dt / 0.15f);
    if (s_master <= 0.0f && !wanted) {
        InstanceAnim::Stop(s_id, s_componentId);
        s_clips = nullptr;
        s_action = nullptr;
        s_actionOn = false;
        s_actionKind = 0;
        s_actionWeight = 0.0f;
        g_beamTwistTarget.store(0.0f, std::memory_order_relaxed);
        g_beamMeasureAim.store(false, std::memory_order_relaxed);
        g_beamUprightWeight.store(0.0f, std::memory_order_relaxed);
        return;
    }
    const float aimLevel = std::max(0.0f, 1.0f - high - low);
    const float weights[4] = {s_master * aimLevel, s_master * high, s_master * low, s_master * s_actionWeight};
    InstanceAnim::SetLayerWeights(s_id, s_componentId, weights, 4);
    g_beamUprightWeight.store(s_master, std::memory_order_relaxed);   // (v0.9.486)
    g_beamTwistTarget.store(onBeam && aiming && wanted ? 1.0f : 0.0f, std::memory_order_relaxed);
    // (v0.9.482) the clips' heading: measured while the aim plays alone; each weapon set its own
    static const WeaponClips* s_yawSet = nullptr;
    static float s_yawBySet[5]{};
    static bool s_yawLogged[5]{};
    static ULONGLONG s_measureSince = 0;
    const WeaponClips* const sets[5] = {&kPistolClips, &kDualPistolClips, &kSmgClips, &kShotgunClips, &kBowClips};
    auto setIndex = [&](const WeaponClips* c) {
        for (int i = 0; i < 5; ++i)
            if (sets[i] == c) return i;
        return -1;
    };
    if (s_clips != s_yawSet) {
        const int was = setIndex(s_yawSet), is = setIndex(s_clips);
        if (was >= 0) s_yawBySet[was] = g_beamYawCorrection.load(std::memory_order_relaxed);
        g_beamYawCorrection.store(is >= 0 ? s_yawBySet[is] : 0.0f, std::memory_order_relaxed);
        s_yawSet = s_clips;
    }
    const bool measure = wanted && aiming && s_master >= 0.99f && s_actionWeight <= 0.02f;
    g_beamMeasureAim.store(measure, std::memory_order_relaxed);
    if (!measure) {
        s_measureSince = 0;
    } else {
        if (!s_measureSince) s_measureSince = now;
        const int index = setIndex(s_clips);
        if (index >= 0 && !s_yawLogged[index] && now - s_measureSince > 1500) {
            s_yawLogged[index] = true;
            Log::Detailf("CombatAir: beam aim: %s's arms turned %.0f degrees to meet the aim (%d measures)", s_clips->aim[0],
                         g_beamYawCorrection.load(std::memory_order_relaxed) * 57.2958f,
                         g_beamYawMeasures.load(std::memory_order_relaxed));
        }
    }
}

// (v0.9.473) the beam states met, by name (their primary agent's state), the first 24
void LogBeamState(std::uint8_t* component) {
    static const void* s_last = nullptr;
    static int s_logged = 0;
    if (s_logged >= 24) return;
#if defined(_MSC_VER)
    __try {
#endif
        auto* primary = *reinterpret_cast<std::uint8_t**>(component + kComponentBaseAgent);
        auto* state = primary ? *reinterpret_cast<const std::uint8_t**>(primary + kAgentCurrentState) : nullptr;
        if (!state || state == s_last) return;
        s_last = state;
        const char* name = *reinterpret_cast<const char* const*>(state + kStateName);
        ++s_logged;
        Log::Detailf("CombatAir: beam state '%s' (aim and shoot %s there)", name ? name : "?",
                     g_beamFire.load(std::memory_order_relaxed) ? "allowed" : "left to the game");
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#endif
}

void TickUnsafe() {
    void* instance = g_getLocalPlayerInstance ? g_getLocalPlayerInstance(0) : nullptr;
    g_laraInstance.store(instance, std::memory_order_relaxed);
    if (instance && Acro::AcrobaticsProtection()) {
        if (void* primary = CombatAir::LaraPrimaryAgent()) {
            const AirClass cls = PrimaryAgentClass(primary);
            if (cls == AirClass::Roll || cls == AirClass::Dodge)
                g_acroProtectUntilMs.store(GetTickCount64() + kAcroProtectGraceMs, std::memory_order_relaxed);
        }
    }

    const bool wantPad = (g_airAimAvailable && g_airAimEnabled.load(std::memory_order_relaxed)) ||
                         (g_slowAvailable && g_slowEnabled.load(std::memory_order_relaxed));
    XINPUT_STATE pad{};
    const bool connected = wantPad && PollGamepad(pad);
    g_statusPadConnected.store(connected, std::memory_order_relaxed);
    const bool inputOpen = !Overlay::PlayerInputLocked();
    const bool foreground = GameWindowForeground();
    {
        XINPUT_STATE jumpPad = pad;
        const bool padOk = connected || (!wantPad && PollGamepad(jumpPad));
        const bool jumpDown = inputOpen && ((padOk && (jumpPad.Gamepad.wButtons & XINPUT_GAMEPAD_A)) ||
                                            (foreground && (GetAsyncKeyState(VK_SPACE) & 0x8000)));
        if (!jumpDown) g_jumpDownSinceMs.store(0, std::memory_order_relaxed);
        else if (!g_jumpDownSinceMs.load(std::memory_order_relaxed)) g_jumpDownSinceMs.store(GetTickCount64(), std::memory_order_relaxed);
    }

    // The aim input (LT / right mouse): arms the air aim and gates the slow motion.
    const bool aimTrigger = connected && pad.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD &&
                            static_cast<float>(pad.Gamepad.bLeftTrigger) / 255.0f > kTriggerHeld;
    const bool aimMouse = foreground && (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    const bool aimHeld = instance && inputOpen && (aimTrigger || aimMouse);
    const bool armed = g_airAimAvailable && g_airAimEnabled.load(std::memory_order_relaxed) && aimHeld;
    g_airAimArmed.store(armed, std::memory_order_relaxed);

    std::uint8_t* component = instance ? StateControlOf(static_cast<std::uint8_t*>(instance)) : nullptr;
    AirClass primaryClass = AirClass::Ground;
    if (component) {
        auto* primary = *reinterpret_cast<std::uint8_t**>(component + kComponentBaseAgent);
        if (primary) primaryClass = PrimaryAgentClass(primary);
    }
    // The camera facing, the combat stance and the aim angles are for the air only; a combat evade with aim held
    // (shooting while dodging) gets the permission overrides of the hooks and the trace.
    const bool inAir = primaryClass == AirClass::Air;
    const bool dodging = primaryClass == AirClass::Dodge && g_dodgeFire.load(std::memory_order_relaxed);
    const bool onBeam = primaryClass == AirClass::Beam && g_beamFire.load(std::memory_order_relaxed);   // (v0.9.473)
    if (primaryClass == AirClass::Beam && component) LogBeamState(component);
    g_statusInAir.store(inAir, std::memory_order_relaxed);
    g_statusOnBeam.store(primaryClass == AirClass::Beam, std::memory_order_relaxed);
    // The grapple axe out: the air aim stands down (overrides, stance, facing, accurate aim) until it is put away.
    const char* axeState = component ? GrappleAxeState(component) : nullptr;
    const bool axe = axeState != nullptr;
    if (axe != g_airAimPaused.load(std::memory_order_relaxed)) {
        g_airAimPaused.store(axe, std::memory_order_relaxed);
        static std::uint32_t s_pauseLogs = 0;
        if (armed && s_pauseLogs++ < 16) {
            if (axe) Log::Detailf("CombatAir: air aim paused: the grapple axe is out (%s)", axeState);
            else Log::Detail("CombatAir: air aim resumed: the grapple axe is away");
        }
    }
    const bool aimingInAir = armed && inAir && !axe;
    const bool aimingInDodge = armed && dodging && !axe;
    const bool aimingOnBeam = armed && onBeam && !axe;
    const bool aimingUp = aimingInAir || aimingOnBeam;   // the air's accurate aim, aim angles and stance

    // Aim IK: while aiming during a combat evade the arms follow the camera's aim (the bone hook fades it in and out).
    bool ikOn = false;
    if (g_ikAvailable && g_dodgeAimIk.load(std::memory_order_relaxed) && aimingInDodge && instance) {
        float dir[3];
        if (CameraAimDirection(instance, dir)) {
            g_ikDirX.store(dir[0], std::memory_order_relaxed);
            g_ikDirY.store(dir[1], std::memory_order_relaxed);
            g_ikDirZ.store(dir[2], std::memory_order_relaxed);
            ikOn = true;
        }
    }
    g_ikTarget.store(ikOn ? 1.0f : 0.0f, std::memory_order_relaxed);
    const ULONGLONG now = GetTickCount64();
    g_airFacingActive.store(g_facingAvailable && g_airFaceCamera.load(std::memory_order_relaxed) && aimingInAir,
                            std::memory_order_relaxed);

    // Accurate aim while aiming in the air: on at the start (the jump states turned it off), kept on
    // through the state changes, and the state's own value back at the end.
    static bool s_accurateWas = false;
    const bool accurate = g_accurateAimAvailable && aimingUp && component != nullptr;
    g_airAccurateAim.store(accurate, std::memory_order_relaxed);
    if (accurate != s_accurateWas && component) ApplyAirAccurateAim(component, accurate);
    s_accurateWas = accurate;
    if (g_aimAnglesAvailable && aimingUp && component) UpdateAirAimAngles(component);
    WatchInstinctView();

    // Combat stance in the air: the moveset goal "InAir" becomes "Combat" while aiming in the air.
    static bool s_stanceWas = false;
    static bool s_stanceBeamWas = false;   // (v0.9.473) the stance was taken on a beam
    // (v0.9.480) on a beam the combat moveset only without the upper-body mode (BeamAimLegs off)
    const bool upperBodyMode = g_beamLegs.load(std::memory_order_relaxed);
    const bool stanceWanted = g_stanceAvailable && g_airCombatStance.load(std::memory_order_relaxed) &&
                              (aimingInAir || (aimingOnBeam && !upperBodyMode));
    g_stanceOnBeam.store(stanceWanted && aimingOnBeam, std::memory_order_relaxed);
    g_stanceActive.store(stanceWanted, std::memory_order_relaxed);
    void* animComponent = g_laraAnimComponent.load(std::memory_order_relaxed);
    if (animComponent && !IsLaraAnimComponent(animComponent)) {
        g_laraAnimComponent.store(nullptr, std::memory_order_relaxed);
        animComponent = nullptr;
    }
    if (animComponent && g_stanceAvailable) {
        const std::uint32_t current = CurrentMovesetGoal(animComponent);
        const std::uint32_t scGoal = g_scMovesetGoal.load(std::memory_order_relaxed);
        if (stanceWanted && (current == kMovesetInAir || (aimingOnBeam && current != kMovesetCombat))) {
            g_originalSetAnimGraphGoal(animComponent, kGoalMoveset, kMovesetCombat);
        } else if (!stanceWanted && s_stanceWas && current == kMovesetCombat && scGoal != kMovesetCombat &&
                   (scGoal == kMovesetInAir || s_stanceBeamWas) && scGoal != 0) {
            g_originalSetAnimGraphGoal(animComponent, kGoalMoveset, scGoal);   // the state control's own goal again
            if (s_stanceBeamWas) RetriggerBeamGoals(animComponent);           // (v0.9.479) no attach again
        }
    }
    s_stanceWas = stanceWanted;
    s_stanceBeamWas = stanceWanted && aimingOnBeam;
    UpdateBeamAimBody(instance, component, onBeam && upperBodyMode && !axe, aimingOnBeam && upperBodyMode);   // (v0.9.480)

    // Slow motion.
    if (!g_slowAvailable) return;
    if (g_slowActive && (!g_slowEnabled.load(std::memory_order_relaxed) || instance != g_slowInstance)) {
        StopSlowMotion(instance != g_slowInstance ? "player changed" : "switched off");
        return;
    }
    if (!g_slowEnabled.load(std::memory_order_relaxed)) {
        g_padButtonsWas = 0;
        return;
    }
    const WORD mask = kSlowButtons[std::clamp(g_slowButton.load(std::memory_order_relaxed), 0,
                                              static_cast<int>(SlowButton::Count) - 1)].mask;
    const bool down = connected && inputOpen && (pad.Gamepad.wButtons & mask) != 0;
    const bool pressed = down && (g_padButtonsWas & mask) == 0;
    g_padButtonsWas = connected ? pad.Gamepad.wButtons : 0;

    // Only while aiming: the button starts it only with aim held, and letting go of aim ends it.
    const bool allowed = !g_slowOnlyAiming.load(std::memory_order_relaxed) || aimHeld;
    if (g_slowActive && !allowed) {
        StopSlowMotion("aim released");
        return;
    }
    if (g_slowHold.load(std::memory_order_relaxed)) {
        if (down && allowed && !g_slowActive && instance) StartSlowMotion(instance);
        else if (!down && g_slowActive) StopSlowMotion("button released");
    } else if (pressed) {
        if (g_slowActive) StopSlowMotion("button pressed again");
        else if (instance && allowed) StartSlowMotion(instance);
    }
    if (!g_slowActive) return;

    const float maxSeconds = g_slowMaxSeconds.load(std::memory_order_relaxed);
    if (maxSeconds > 0.0f && static_cast<float>(now - g_slowStartMs) >= maxSeconds * 1000.0f) {
        StopSlowMotion("time limit");
        return;
    }
    float world = 1.0f, global = 1.0f;
    SlowFactors(world, global);
    if (!SlowSlotOurs()) {
        if (!g_slowSlotLostLogged) {
            g_slowSlotLostLogged = true;
            Log::Detail("CombatAir: the game cleared the slow motion request (level load or reset); requested again");
        }
        RequestSlowMotion();
    } else if (world != g_slowAppliedWorld || global != g_slowAppliedGlobal) {
        RequestSlowMotion();
    }
}

const char* SlowButtonName(int index) {
    return kSlowButtons[std::clamp(index, 0, static_cast<int>(SlowButton::Count) - 1)].name;
}

// Aim camera further back: during Do3PointInterpolations the three pitch points' dolly smoothers
// (from and to) and the dolly limits are scaled, so the clamped target dolly the function stores at
// +0x300 is scaled too; DoPosition and AimCameraCollision (walls) then work on that target as usual.
// The scaled fields are restored right after the call (the function only reads them), so nothing
// accumulates from frame to frame and other readers see the game's own values.
struct AimCamSaved {
    float from[3];
    float to[3];
    float dollyMax;
    float dollyMin;
};

bool ScaleAimCameraDolly(std::uint8_t* c, float scale, bool likePistols, AimCamSaved& saved) {
#if defined(_MSC_VER)
    __try {
#endif
        for (int i = 0; i < 3; ++i) {
            auto* from = reinterpret_cast<float*>(c + kAimCamPoints[i] + kAimCamPointDollyFrom);
            auto* to = reinterpret_cast<float*>(c + kAimCamPoints[i] + kAimCamPointDollyTo);
            saved.from[i] = *from;
            saved.to[i] = *to;
        }
        auto* dollyMax = reinterpret_cast<float*>(c + kAimCamDollyMax);
        auto* dollyMin = reinterpret_cast<float*>(c + kAimCamDollyMin);
        saved.dollyMax = *dollyMax;
        saved.dollyMin = *dollyMin;
        for (int i = 0; i < 3; ++i) {
            const float pistol = g_pistolDolly[i].load(std::memory_order_relaxed);
            *reinterpret_cast<float*>(c + kAimCamPoints[i] + kAimCamPointDollyFrom) = (likePistols ? pistol : saved.from[i]) * scale;
            *reinterpret_cast<float*>(c + kAimCamPoints[i] + kAimCamPointDollyTo) = (likePistols ? pistol : saved.to[i]) * scale;
        }
        // Scaling both limits keeps them around the scaled dolly whichever sign the dolly uses.
        *dollyMax = (likePistols ? g_pistolDollyMax.load(std::memory_order_relaxed) : saved.dollyMax) * scale;
        *dollyMin = (likePistols ? g_pistolDollyMin.load(std::memory_order_relaxed) : saved.dollyMin) * scale;
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

void RestoreAimCameraDolly(std::uint8_t* c, const AimCamSaved& saved) {
#if defined(_MSC_VER)
    __try {
#endif
        for (int i = 0; i < 3; ++i) {
            *reinterpret_cast<float*>(c + kAimCamPoints[i] + kAimCamPointDollyFrom) = saved.from[i];
            *reinterpret_cast<float*>(c + kAimCamPoints[i] + kAimCamPointDollyTo) = saved.to[i];
        }
        *reinterpret_cast<float*>(c + kAimCamDollyMax) = saved.dollyMax;
        *reinterpret_cast<float*>(c + kAimCamDollyMin) = saved.dollyMin;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#endif
}

void __fastcall HookAimCamDo3Point(void* controller, float dt, int flags) {
    const float base = std::clamp(g_aimCamScale.load(std::memory_order_relaxed), 1.0f, 3.0f);
    const int weaponClass = Features::CurrentWeaponClassCached();
    const bool pistol = weaponClass == static_cast<int>(Features::WeaponClass::Pistol);
    const bool farther = g_aimCamFarther.load(std::memory_order_relaxed) && base > 1.001f;
    // The SMG / rifle and the shotgun: the pistols' camera (their dolly, with the pistols' scale), not the bow.
    const bool likePistols = g_aimCamLikePistols.load(std::memory_order_relaxed) &&
                             (weaponClass == static_cast<int>(Features::WeaponClass::Smg) ||
                              weaponClass == static_cast<int>(Features::WeaponClass::Shotgun));
    float scale = 1.0f;
    if (farther && (pistol || likePistols || !g_aimCamPistolsOnly.load(std::memory_order_relaxed))) scale = base;
    // In a dodge the camera stands a little further back (hand-to-hand module; 1 outside a dodge).
    scale *= std::clamp(Brawl::DodgeCameraFactor(), 1.0f, 2.0f);
    const bool apply = controller && (scale > 1.001f || likePistols);
    auto* c = static_cast<std::uint8_t*>(controller);
    AimCamSaved saved{};
    const bool scaled = apply && ScaleAimCameraDolly(c, scale, likePistols, saved);
    g_originalAimCamDo3Point(controller, dt, flags);
    if (!scaled) {
        g_aimCamStatusActive.store(false, std::memory_order_relaxed);
        return;
    }
    RestoreAimCameraDolly(c, saved);
    // The pistols' own values, the first time a pistol is aimed (the controller with the nearer mid point).
    if (pistol && saved.to[1] > 1.0f && (!g_pistolDollyLearned.exchange(true, std::memory_order_relaxed) ||
                                         saved.to[1] < g_pistolDolly[1].load(std::memory_order_relaxed))) {
        for (int i = 0; i < 3; ++i) g_pistolDolly[i].store(saved.to[i], std::memory_order_relaxed);
        g_pistolDollyMin.store(saved.dollyMin, std::memory_order_relaxed);
        g_pistolDollyMax.store(saved.dollyMax, std::memory_order_relaxed);
    }
    // Each weapon class once: its own values and what the camera uses.
    if (weaponClass >= 0 && weaponClass < 8) {
        const std::uint32_t bit = 1u << weaponClass;
        if (!(g_aimCamClassLogged.fetch_or(bit, std::memory_order_relaxed) & bit))
            Log::Detailf("CombatAir: aim camera with weapon class %d: its own dolly high %.1f, mid %.1f, low %.1f, limits [%.1f, %.1f]; "
                        "used: %s x%.2f (the pistols': %.1f / %.1f / %.1f)",
                        weaponClass, saved.to[0], saved.to[1], saved.to[2], saved.dollyMin, saved.dollyMax,
                        likePistols ? "the pistols' values" : "its own", scale, g_pistolDolly[0].load(), g_pistolDolly[1].load(),
                        g_pistolDolly[2].load());
    }
    const float target = *reinterpret_cast<const float*>(c + kAimCamTargetDolly);
    g_aimCamStatusTarget.store(target, std::memory_order_relaxed);
    g_aimCamStatusActive.store(true, std::memory_order_relaxed);
}
}  // namespace

namespace CombatAir {
bool CreateHooks() {
    if (!GameBuild::IsSupportedRetail()) return false;
    g_base = GameBuild::Base();
    LoadSettings();

    const bool common =
        Verify(kRvaGetLocalPlayerInstance, kGetLocalPlayerInstanceSig, sizeof(kGetLocalPlayerInstanceSig), "GetLocalPlayerInstance") &&
        Verify(kRvaInventoryPauseScId, kInventoryPauseScIdSig, sizeof(kInventoryPauseScIdSig), "ComponentArrayID<StateControlComponent>");
    if (!common) {
        Log::Write("CombatAir: air aim and slow motion unavailable");
        return false;
    }
    g_getLocalPlayerInstance = At<GetLocalPlayerInstanceFn>(kRvaGetLocalPlayerInstance);
    g_scComponentIdRva = static_cast<std::uint32_t>(RelTarget(kRvaInventoryPauseScId, 2, 6));

    if (Verify(kRvaScPropertyGetBool, kScPropertyGetBoolSig, sizeof(kScPropertyGetBoolSig), "SCPROPERTY_GetBool")) {
        if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaScPropertyGetBool),
                          reinterpret_cast<void*>(&HookScPropertyGetBool),
                          reinterpret_cast<void**>(&g_originalGetBool)) == MH_OK)
            g_airAimAvailable = true;
        else
            Log::Write("CombatAir: failed to create the SCPROPERTY_GetBool hook; air aim unavailable");
    }
    if (g_airAimAvailable &&
        Verify(kRvaSpatialProbeHit, kSpatialProbeHitSig, sizeof(kSpatialProbeHitSig), "CSCCF_SpatialProbeHit")) {
        if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaSpatialProbeHit),
                          reinterpret_cast<void*>(&HookSpatialProbeHit),
                          reinterpret_cast<void**>(&g_originalSpatialProbeHit)) == MH_OK)
            g_groundProbeAvailable = true;
        else
            Log::Write("CombatAir: failed to create the spatial probe hook; weapons still draw only near the ground");
    }
    if (g_airAimAvailable &&
        Verify(kRvaCheckConditions, kCheckConditionsSig, sizeof(kCheckConditionsSig), "SCAgent::CheckConditions") &&
        Verify(kRvaGetCondProperty, kGetCondPropertySig, sizeof(kGetCondPropertySig), "GetSCCondProperty_WithContextHack")) {
        g_getCondProperty = At<GetCondPropertyFn>(kRvaGetCondProperty);
        if (Verify(kRvaSymbolCStr, kSymbolCStrSig, sizeof(kSymbolCStrSig), "cdc::Symbol::c_str"))
            g_symbolCStr = At<SymbolCStrFn>(kRvaSymbolCStr);
        if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaCheckConditions),
                          reinterpret_cast<void*>(&HookCheckConditions),
                          reinterpret_cast<void**>(&g_originalCheckConditions)) == MH_OK)
            g_drawCheckAvailable = true;
        else
            Log::Write("CombatAir: failed to create the transition check hook; air draw report unavailable");
    }
    if (g_airAimAvailable &&
        Verify(kRvaSetAnimGraphGoal, kSetAnimGraphGoalSig, sizeof(kSetAnimGraphGoalSig), "AnimComponentV2::SetAnimGraphGoal") &&
        Verify(kRvaGoalMovesetPropFunction, kGoalMovesetPropFunctionSig, sizeof(kGoalMovesetPropFunctionSig),
               "SCPropFunction_SetAnimGraph_Goal_Moveset")) {
        if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaSetAnimGraphGoal),
                          reinterpret_cast<void*>(&HookSetAnimGraphGoal),
                          reinterpret_cast<void**>(&g_originalSetAnimGraphGoal)) == MH_OK)
            g_stanceAvailable = true;
        else
            Log::Write("CombatAir: failed to create the anim goal hook; no weapon animation in the air");
    }
    if (g_airAimAvailable && Verify(kRvaDoTransition, kDoTransitionSig, sizeof(kDoTransitionSig), "SCAgent::DoTransition")) {
        if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaDoTransition),
                          reinterpret_cast<void*>(&HookDoTransition),
                          reinterpret_cast<void**>(&g_originalDoTransition)) == MH_OK)
            g_traceAvailable = true;
        else
            Log::Write("CombatAir: failed to create the state change hook (roll chain, hand-to-hand, grapple kick)");
    }
    if (g_airAimAvailable &&
        Verify(kRvaSteeringProcess, kSteeringProcessSig, sizeof(kSteeringProcessSig), "PlayerSteering::Process") &&
        Verify(kRvaSteeringSetTargetDir, kSteeringSetTargetDirSig, sizeof(kSteeringSetTargetDirSig),
               "PlayerSteering::SetTargetDir") &&
        Verify(kRvaSteeringSetTurnParams, kSteeringSetTurnParamsSig, sizeof(kSteeringSetTurnParamsSig),
               "PlayerSteering::SetTurnParameters") &&
        Verify(kRvaCameraForward, kCameraForwardSig, sizeof(kCameraForwardSig), "CameraManager::GetActiveCamera_Forward") &&
        Verify(kRvaCameraManagerLoad, kMovRcxRipSig, sizeof(kMovRcxRipSig), "CameraManager::m_pCameraManager") &&
        Verify(kRvaCameraForwardCall, kCallRel32Sig, sizeof(kCallRel32Sig), "Turn_CameraDir forward call") &&
        RelTarget(kRvaCameraForwardCall, 1, 5) == kRvaCameraForward) {
        g_setTargetDir = At<SteeringSetTargetDirFn>(kRvaSteeringSetTargetDir);
        g_setTurnParams = At<SteeringSetTurnParamsFn>(kRvaSteeringSetTurnParams);
        g_cameraForward = At<CameraForwardFn>(kRvaCameraForward);
        g_cameraManager = At<void* const*>(RelTarget(kRvaCameraManagerLoad, 3, 7));
        if (Verify(kRvaAnimBuildTransforms, kAnimBuildTransformsSig, sizeof(kAnimBuildTransformsSig), "cdc::AnimBuildTransforms")) {
            if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaAnimBuildTransforms),
                              reinterpret_cast<void*>(&HookAnimBuildTransforms),
                              reinterpret_cast<void**>(&g_originalAnimBuildTransforms)) == MH_OK)
                g_ikAvailable = true;
            else
                Log::Write("CombatAir: failed to create the bone transform hook; no aim IK in evades");
        }
        Log::Detailf("CombatAir: aim IK in aimed evades %s (setting %s), shooting during evades %s",
                    g_ikAvailable ? "ready" : "unavailable", g_dodgeAimIk.load() ? "on" : "off",
                    g_dodgeFire.load() ? "on" : "off");
        if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaSteeringProcess),
                          reinterpret_cast<void*>(&HookSteeringProcess),
                          reinterpret_cast<void**>(&g_originalSteeringProcess)) == MH_OK)
            g_facingAvailable = true;
        else
            Log::Write("CombatAir: failed to create the steering hook; Lara keeps her facing in the air");
    }
    if (g_airAimAvailable &&
        Verify(kRvaSetAccurateAim, kSetAccurateAimSig, sizeof(kSetAccurateAimSig), "PSCPF_SetAccurateAim") &&
        RelTarget(kRvaSetAccurateAim + 8, 3, 7) == kRvaPlayerComponentId &&
        Verify(kRvaGetPropertyData, kGetPropertyDataSig, sizeof(kGetPropertyDataSig), "StateControlComponent property data")) {
        g_getPropertyData = At<GetPropertyDataFn>(kRvaGetPropertyData);
        if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaSetAccurateAim),
                          reinterpret_cast<void*>(&HookSetAccurateAim),
                          reinterpret_cast<void**>(&g_originalSetAccurateAim)) == MH_OK)
            g_accurateAimAvailable = true;
        else
            Log::Write("CombatAir: failed to create the accurate aim hook; aiming in the air stays inaccurate");
    }
    if (g_airAimAvailable &&
        Verify(kRvaAvoidClippingAiming, kAvoidClippingAimingSig, sizeof(kAvoidClippingAimingSig),
               "_Combat_AvoidWeaponClipping_Aiming") &&
        Verify(kRvaAvoidClippingProcess, kAvoidClippingProcessSig, sizeof(kAvoidClippingProcessSig),
               "PSCAF_P_Combat_AvoidWeaponClipping") &&
        RelTarget(kRvaAvoidClippingProcess + 9, 1, 5) == kRvaAvoidClippingAiming) {
        g_avoidClippingAiming = At<AvoidClippingAimingFn>(kRvaAvoidClippingAiming);
        g_aimAnglesAvailable = true;
    }
    if (Verify(kRvaVisionEnable, kVisionEnableSig, sizeof(kVisionEnableSig), "VisionMode::Enable") &&
        Verify(kRvaVisionDisable, kVisionDisableSig, sizeof(kVisionDisableSig), "VisionMode::Disable") &&
        Verify(kRvaVisionInstanceLoad, kMovRcxRipSig, sizeof(kMovRcxRipSig), "VisionMode::mInstance") &&
        Verify(kRvaVisionEnableJump, kJmpRel32Sig, sizeof(kJmpRel32Sig), "VisionModeListener enable jump") &&
        RelTarget(kRvaVisionEnableJump, 1, 5) == kRvaVisionEnable &&
        Verify(kRvaInstinctHighlight, kInstinctHighlightSig, sizeof(kInstinctHighlightSig),
               "SurvivalInstinctComponent::EnableHighlight") &&
        Verify(kRvaAsymInstinctHighlight, kAsymInstinctHighlightSig, sizeof(kAsymInstinctHighlightSig),
               "AsymmetricSurvivalInstinctComponent::EnableHighlight") &&
        Verify(kRvaAddAsymComponents, kAddAsymComponentsSig, sizeof(kAddAsymComponentsSig),
               "VisionMode::AddAsymetricComponents") &&
        Verify(kRvaNotifyDialogueContext, kNotifyDialogueContextSig, sizeof(kNotifyDialogueContextSig),
               "DialogueManager::NotifyDialogueContext")) {
        g_visionEnable = At<VisionEnableFn>(kRvaVisionEnable);
        g_visionDisable = At<VisionDisableFn>(kRvaVisionDisable);
        g_visionMode = At<std::uint8_t* const*>(RelTarget(kRvaVisionInstanceLoad, 3, 7));
        const bool hooked =
            MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaInstinctHighlight),
                          reinterpret_cast<void*>(&HookInstinctHighlight),
                          reinterpret_cast<void**>(&g_originalInstinctHighlight)) == MH_OK &&
            MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaAsymInstinctHighlight),
                          reinterpret_cast<void*>(&HookAsymInstinctHighlight),
                          reinterpret_cast<void**>(&g_originalAsymInstinctHighlight)) == MH_OK &&
            MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaAddAsymComponents),
                          reinterpret_cast<void*>(&HookAddAsymComponents),
                          reinterpret_cast<void**>(&g_originalAddAsymComponents)) == MH_OK &&
            MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaNotifyDialogueContext),
                          reinterpret_cast<void*>(&HookNotifyDialogueContext),
                          reinterpret_cast<void**>(&g_originalNotifyDialogueContext)) == MH_OK;
        if (hooked)
            g_instinctAvailable = true;
        else
            Log::Write("CombatAir: failed to create the survival instinct hooks; slow motion without the instinct view");
    }
    if (Verify(kRvaSendEventByName, kSendEventByNameSig, sizeof(kSendEventByNameSig), "EventSystem::SendEvent") &&
        Verify(kRvaEventSystemLoad, kMovRcxRipSig, sizeof(kMovRcxRipSig), "EventSystem::s_instance") &&
        Verify(kRvaSendEventCall, kCallRel32Sig, sizeof(kCallRel32Sig), "VisionMode sound event call") &&
        RelTarget(kRvaSendEventCall, 1, 5) == kRvaSendEventByName) {
        g_sendEvent = At<SendEventByNameFn>(kRvaSendEventByName);
        g_eventSystem = At<void* const*>(RelTarget(kRvaEventSystemLoad, 3, 7));
        g_soundAvailable = true;
    }

    if (Verify(kRvaTimeDilationRequestCtor, kTimeDilationRequestCtorSig, sizeof(kTimeDilationRequestCtorSig), "TimeDilationRequest ctor") &&
        Verify(kRvaTimeDilationRequest, kTimeDilationRequestSig, sizeof(kTimeDilationRequestSig), "TimeDilationManager::Request") &&
        Verify(kRvaTimeDilationCancel, kTimeDilationCancelSig, sizeof(kTimeDilationCancelSig), "TimeDilationManager::CancelRequest") &&
        Verify(kRvaSetTimeDilationManager, kSetTimeDilationManagerSig, sizeof(kSetTimeDilationManagerSig), "ms_timeDilationManager")) {
        g_requestCtor = At<TimeDilationRequestCtorFn>(kRvaTimeDilationRequestCtor);
        g_request = At<TimeDilationRequestFn>(kRvaTimeDilationRequest);
        g_cancel = At<TimeDilationCancelFn>(kRvaTimeDilationCancel);
        g_timeDilationManager = At<std::uint8_t*>(RelTarget(kRvaSetTimeDilationManager, 3, 7));
        g_slowAvailable = true;
    }

    if (Verify(kRvaAimCamDo3Point, kAimCamDo3PointSig, sizeof(kAimCamDo3PointSig),
               "AimCameraController::Do3PointInterpolations") &&
        Verify(kRvaAimCamLeaMid, kAimCamLeaMidSig, sizeof(kAimCamLeaMidSig), "aim camera mid point") &&
        Verify(kRvaAimCamLeaLow, kAimCamLeaLowSig, sizeof(kAimCamLeaLowSig), "aim camera low point") &&
        Verify(kRvaAimCamLeaHigh, kAimCamLeaHighSig, sizeof(kAimCamLeaHighSig), "aim camera high point") &&
        Verify(kRvaAimCamDollyClamp, kAimCamDollyClampSig, sizeof(kAimCamDollyClampSig), "aim camera dolly clamp") &&
        Verify(kRvaAimCamDollyStore, kAimCamDollyStoreSig, sizeof(kAimCamDollyStoreSig), "aim camera target dolly")) {
        if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaAimCamDo3Point),
                          reinterpret_cast<void*>(&HookAimCamDo3Point),
                          reinterpret_cast<void**>(&g_originalAimCamDo3Point)) == MH_OK)
            g_aimCamAvailable = true;
        else
            Log::Write("CombatAir: failed to create the aim camera hook; the farther aim camera is unavailable");
    }
    if (Verify(kRvaFireInstant, kFireInstantSig, sizeof(kFireInstantSig), "PROJECTILE_FireInstant") &&
        Verify(kRvaFireInstantOwnerCheck, kFireInstantOwnerCheckSig, sizeof(kFireInstantOwnerCheckSig),
               "PROJECTILE_FireInstant owner check")) {
        if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaFireInstant), reinterpret_cast<void*>(&HookFireInstant),
                          reinterpret_cast<void**>(&g_originalFireInstant)) == MH_OK)
            g_shotHookAvailable = true;
        else
            Log::Write("CombatAir: failed to create the PROJECTILE_FireInstant hook; enemy shots are not turned away");
    }
    if (Verify(kRvaFxFilterCollisions, kFxFilterCollisionsSig, sizeof(kFxFilterCollisionsSig), "FxGameFilterCollisions")) {
        if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaFxFilterCollisions),
                          reinterpret_cast<void*>(&HookFxFilterCollisions),
                          reinterpret_cast<void**>(&g_originalFxFilter)) == MH_OK)
            g_fxFilterAvailable = true;
        else
            Log::Write("CombatAir: failed to create the FX collision filter hook; enemy FX bullets still hit Lara");
    }
    if (Verify(kRvaApplyDamage, kApplyDamageSig, sizeof(kApplyDamageSig), "HealthComponent::ApplyDamage")) {
        if (MH_CreateHook(reinterpret_cast<void*>(g_base + kRvaApplyDamage), reinterpret_cast<void*>(&HookApplyDamage),
                          reinterpret_cast<void**>(&g_originalApplyDamage)) == MH_OK)
            g_applyDamageAvailable = true;
        else
            Log::Write("CombatAir: failed to create the ApplyDamage hook");
    }
    Log::Detailf("CombatAir: enemy shots miss Lara in acrobatics: instant shots %s, FX bullets %s, shot damage fallback %s "
                "(setting %s)", g_shotHookAvailable ? "ready" : "unavailable", g_fxFilterAvailable ? "ready" : "unavailable",
                g_applyDamageAvailable ? "ready" : "unavailable", Acro::AcrobaticsProtection() ? "on" : "off");
    Log::Detailf("CombatAir: farther aim camera %s (setting %s, x%.2f, %s)", g_aimCamAvailable ? "ready" : "unavailable",
                g_aimCamFarther.load() ? "on" : "off", g_aimCamScale.load(),
                g_aimCamPistolsOnly.load() ? "pistols only" : "all aimed weapons");

    Log::Detailf("CombatAir: air aim %s (ground probe %s, draw/fire check and holster hold %s, combat stance %s %s, accurate aim %s, aim angles %s, camera facing %s %s at %.0f deg/s, fast fire %s every %.0f ms, transition trace %s), slow motion %s (instinct view %s %s, extra sound event %s '%s'); settings: air aim %d, slow motion %d on %s (%s%s), world %.0f%%, Lara %.0f%%, limit %.0f s",
                g_airAimAvailable ? "ready" : "unavailable", g_groundProbeAvailable ? "ready" : "unavailable",
                g_drawCheckAvailable ? "ready" : "unavailable", g_stanceAvailable ? "ready" : "unavailable",
                g_airCombatStance.load() ? "on" : "off", g_accurateAimAvailable ? "ready" : "unavailable",
                g_aimAnglesAvailable ? "ready" : "unavailable",
                g_facingAvailable ? "ready" : "unavailable",
                g_airFaceCamera.load() ? "on" : "off", g_airTurnSpeed.load(), g_airFastFire.load() ? "on" : "off",
                g_airFireIntervalMs.load(), g_traceAvailable ? "ready" : "unavailable",
                g_slowAvailable ? "ready" : "unavailable",
                g_instinctAvailable ? "ready" : "unavailable", g_slowInstinct.load() ? "on" : "off",
                g_soundAvailable ? (g_slowSound.load() ? "on" : "off") : "unavailable", g_slowSoundEvent,
                g_airAimEnabled.load() ? 1 : 0, g_slowEnabled.load() ? 1 : 0, SlowButtonName(g_slowButton.load()),
                g_slowHold.load() ? "hold" : "toggle", g_slowOnlyAiming.load() ? ", only while aiming" : "",
                g_slowWorldSpeed.load() * 100.0f, g_slowLaraSpeed.load() * 100.0f, g_slowMaxSeconds.load());
    return g_airAimAvailable || g_slowAvailable || g_aimCamAvailable;
}

void TickGameThread() {
    if (!g_base || (!g_airAimAvailable && !g_slowAvailable)) return;
#if defined(_MSC_VER)
    __try {
#endif
        TickUnsafe();
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_airAimArmed.store(false, std::memory_order_relaxed);
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            Log::Write("CombatAir: tick raised an exception; air aim disarmed for this frame");
        }
    }
#endif
}

void DrawPlayerSection() {
    const float scale = std::clamp(Settings::Get().uiScale, 1.0f, 2.5f);
    bool save = false;

    // (v0.9.519: its title is the Enhancements tab's collapsing header)

    if (!g_airAimAvailable) ImGui::BeginDisabled();
    bool airAim = g_airAimEnabled.load();
    if (ImGui::Checkbox("Aim in the air (jumping or falling)", &airAim)) {
        g_airAimEnabled.store(airAim);
        save = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Hold aim (LT / right mouse) during a jump or a fall to draw and aim the current weapon.\n"
            "Ledge, rope and rescue grabs are not affected. Experimental.");
    }
    if (airAim) {
        ImGui::Indent();
        if (!g_stanceAvailable) ImGui::BeginDisabled();
        bool stance = g_airCombatStance.load();
        if (ImGui::Checkbox("Combat stance in the air", &stance)) {
            g_airCombatStance.store(stance);
            save = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "Rise has no in-air aiming animation: the weapon animates only in the combat stance.\n"
                "While aiming in the air Lara switches to it (her legs use the ground combat pose).");
        }
        if (!g_stanceAvailable) ImGui::EndDisabled();

        if (!g_facingAvailable) ImGui::BeginDisabled();
        bool face = g_airFaceCamera.load();
        if (ImGui::Checkbox("Turn to the camera", &face)) {
            g_airFaceCamera.store(face);
            save = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("While aiming in the air Lara turns to where the camera looks, like when aiming on the ground.");
        if (face) {
            float turn = g_airTurnSpeed.load();
            ImGui::SetNextItemWidth(170.0f * scale);
            if (ImGui::SliderFloat("Turn speed", &turn, 90.0f, 1440.0f, "%.0f deg/s", ImGuiSliderFlags_AlwaysClamp))
                g_airTurnSpeed.store(turn);
            if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Aiming on the ground turns at 360 deg/s.");
        }
        if (!g_facingAvailable) ImGui::EndDisabled();

        bool fast = g_airFastFire.load();
        if (ImGui::Checkbox("Faster shooting in the air", &fast)) {
            g_airFastFire.store(fast);
            save = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("In the air the next shot does not wait for the weapon's recoil and fire interval,\n"
                              "only for the time below. Off: the weapon's own rate.");
        }
        if (fast) {
            float interval = g_airFireIntervalMs.load();
            ImGui::SetNextItemWidth(170.0f * scale);
            if (ImGui::SliderFloat("Time between shots", &interval, 40.0f, 500.0f, "%.0f ms", ImGuiSliderFlags_AlwaysClamp))
                g_airFireIntervalMs.store(interval);
            if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
        }
        bool dodgeFire = g_dodgeFire.load();
        if (ImGui::Checkbox("Shoot during combat evades", &dodgeFire)) {
            g_dodgeFire.store(dodgeFire);
            save = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("With aim held, the weapon stays ready through an evade (dodge) and fires, like in the air.\n"
                              "The game normally puts the weapon on hold until the evade ends.");
        }
        bool beamFire = g_beamFire.load();
        if (ImGui::Checkbox("Shoot on beams", &beamFire)) {
            g_beamFire.store(beamFire);
            save = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("With aim held on a balance beam, Lara draws her weapon, aims and shoots, like in the air.\n"
                              "The game normally keeps the weapons away on beams. ROTTRFramework.ini [CombatExtras] FireOnBeams.");
        }
        if (beamFire) {   // (v0.9.475)
            ImGui::Indent();
            bool legs = g_beamLegs.load();
            if (ImGui::Checkbox("Weapon animations on the upper body only", &legs)) {
                g_beamLegs.store(legs);
                save = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("On: Lara keeps the beam's own animations (legs, balance, walking) and only her upper body\n"
                                  "takes the weapon's animations (aim, fire, reload, draw), turning with the camera up to 90\n"
                                  "degrees either way.\n"
                                  "Off: her whole body takes the weapon's combat stance while aiming on a beam.");
            ImGui::Unindent();
        }
        if (dodgeFire) {
            ImGui::Indent();
            if (!g_ikAvailable) ImGui::BeginDisabled();
            bool ik = g_dodgeAimIk.load();
            if (ImGui::Checkbox("Arms aim at the target (IK)", &ik)) {
                g_dodgeAimIk.store(ik);
                save = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("During an aimed evade both upper arms turn so that the hands point where the camera aims.\n"
                                  "It fades out when the body is turned too far (upside down in a flip).");
            }
            if (!g_ikAvailable) ImGui::EndDisabled();
            ImGui::Unindent();
        }
        ImGui::Unindent();
    }
    if (!g_airAimAvailable) ImGui::EndDisabled();

    if (!g_slowAvailable) ImGui::BeginDisabled();
    bool slow = g_slowEnabled.load();
    if (ImGui::Checkbox("Slow motion button", &slow)) {
        g_slowEnabled.store(slow);
        save = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Slows the world through the game's own time dilation (the same system the game uses\n"
            "for its slow-motion moments). Lara can stay faster than the world around her.");
    }
    if (slow) {
        ImGui::Indent();
        int button = g_slowButton.load();
        ImGui::SetNextItemWidth(170.0f * scale);
        if (ImGui::BeginCombo("Gamepad button##slowButton", SlowButtonName(button))) {
            for (int i = 0; i < static_cast<int>(SlowButton::Count); ++i) {
                const bool selected = i == button;
                if (ImGui::Selectable(kSlowButtons[i].name, selected)) {
                    g_slowButton.store(i);
                    save = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        bool onlyAiming = g_slowOnlyAiming.load();
        if (ImGui::Checkbox("Only while aiming", &onlyAiming)) {
            g_slowOnlyAiming.store(onlyAiming);
            save = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The button works only with aim held (LT / right mouse); letting go of aim ends it.");
        bool hold = g_slowHold.load();
        if (ImGui::Checkbox("Only while the button is held", &hold)) {
            g_slowHold.store(hold);
            save = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Off: one press starts slow motion, the next press ends it.");
        if (!g_instinctAvailable) ImGui::BeginDisabled();
        bool instinct = g_slowInstinct.load();
        if (ImGui::Checkbox("Survival Instinct view", &instinct)) {
            g_slowInstinct.store(instinct);
            save = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Slow motion turns on the survival instinct look and sound,\n"
                              "without the object highlights, the objective marker and Lara's hint line.");
        }
        if (!g_instinctAvailable) ImGui::EndDisabled();

        float world = g_slowWorldSpeed.load() * 100.0f;
        ImGui::SetNextItemWidth(170.0f * scale);
        if (ImGui::SliderFloat("World speed", &world, 5.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
            g_slowWorldSpeed.store(world / 100.0f);
        if (ImGui::IsItemDeactivatedAfterEdit()) save = true;

        float lara = g_slowLaraSpeed.load() * 100.0f;
        ImGui::SetNextItemWidth(170.0f * scale);
        if (ImGui::SliderFloat("Lara speed", &lara, 5.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
            g_slowLaraSpeed.store(lara / 100.0f);
        if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The world never runs faster than Lara.");

        float limit = g_slowMaxSeconds.load();
        ImGui::SetNextItemWidth(170.0f * scale);
        if (ImGui::SliderFloat("Time limit", &limit, 0.0f, 60.0f, limit <= 0.0f ? "none" : "%.0f s",
                               ImGuiSliderFlags_AlwaysClamp))
            g_slowMaxSeconds.store(limit);
        if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Real seconds before slow motion ends by itself (0 = until the button again).");

        ImGui::TextDisabled("Status: %s%s", g_statusSlowActive.load() ? "slow motion ON" : "normal speed",
                            g_statusPadConnected.load() ? "" : " (no gamepad found)");
        ImGui::Unindent();
    }
    if (!g_slowAvailable) ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextUnformatted("Aim camera");
    ImGui::Separator();
    if (!g_aimCamAvailable) ImGui::BeginDisabled();
    bool farther = g_aimCamFarther.load();
    if (ImGui::Checkbox("Aim camera further back", &farther)) {
        g_aimCamFarther.store(farther);
        save = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Moves the over-the-shoulder aim camera back along its view line, so the crosshair keeps\n"
            "pointing at the same spot. The game's own aim camera collision still keeps it out of walls.");
    }
    if (farther) {
        ImGui::Indent();
        bool pistolsOnly = g_aimCamPistolsOnly.load();
        if (ImGui::Checkbox("Only with pistols", &pistolsOnly)) {
            g_aimCamPistolsOnly.store(pistolsOnly);
            save = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Off: every aimed weapon (bow, rifle, shotgun too).");
        bool likePistols = g_aimCamLikePistols.load();
        if (ImGui::Checkbox("Rifle / SMG and shotgun: the pistols' camera", &likePistols)) {
            g_aimCamLikePistols.store(likePistols);
            save = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("With the rifle / SMG and the shotgun the aim camera stands at the pistols' distance (with the same\n"
                              "\"Distance\" factor). The bow keeps its own camera.");
        float distance = g_aimCamScale.load();
        ImGui::SetNextItemWidth(170.0f * scale);
        if (ImGui::SliderFloat("Distance##aimCamScale", &distance, 1.0f, 3.0f, "x%.2f", ImGuiSliderFlags_AlwaysClamp))
            g_aimCamScale.store(distance);
        if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Multiplies the game's aim camera distance (x1.00 = the game's own).");
        if (g_aimCamStatusActive.load())
            ImGui::TextDisabled("Status: active, camera distance %.0f", g_aimCamStatusTarget.load());
        else
            ImGui::TextDisabled("Status: waiting for aim%s", g_aimCamPistolsOnly.load() ? " with a pistol" : "");
        ImGui::Unindent();
    }
    if (!g_aimCamAvailable) ImGui::EndDisabled();

    if (save) SaveSettings();
}

bool CameraDirection(void* instance, float dir[3]) {
    return instance && dir && CameraAimDirection(instance, dir);
}

void* LaraPrimaryAgent() {
    if (!g_getLocalPlayerInstance || !g_scComponentIdRva) return nullptr;
#if defined(_MSC_VER)
    __try {
#endif
        auto* instance = static_cast<std::uint8_t*>(g_getLocalPlayerInstance(0));
        std::uint8_t* component = instance ? StateControlOf(instance) : nullptr;
        return component ? *reinterpret_cast<void**>(component + kComponentBaseAgent) : nullptr;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
#endif
}

bool TransitionHookActive() {
    return g_traceAvailable;
}

bool JumpButtonHeld() {
    static std::atomic<XInputGetStateFn> s_get{nullptr};
    static std::atomic_bool s_tried{false};
    XInputGetStateFn get = g_xinputGetState ? g_xinputGetState : s_get.load(std::memory_order_acquire);
    if (!get && !s_tried.exchange(true)) {
        for (const wchar_t* name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
            HMODULE module = LoadLibraryW(name);
            if (!module) continue;
            get = reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState"));
            if (get) break;
        }
        s_get.store(get, std::memory_order_release);
    }
    if (get) {
        XINPUT_STATE state{};
        const DWORD known = g_padSlot;  // the pad the air aim / slow motion code found, else the first that answers
        if (known < XUSER_MAX_COUNT && get(known, &state) == ERROR_SUCCESS) {
            if (state.Gamepad.wButtons & XINPUT_GAMEPAD_A) return true;
        } else {
            for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
                if (get(slot, &state) != ERROR_SUCCESS) continue;
                if (state.Gamepad.wButtons & XINPUT_GAMEPAD_A) return true;
                break;
            }
        }
    }
    return GameWindowForeground() && (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
}

void AcroProtectStats(unsigned& reads, unsigned& answered) {
    reads = g_acroProtectReads.load(std::memory_order_relaxed);
    answered = g_acroProtectAnswers.load(std::memory_order_relaxed);
}

void ShotStats(unsigned& shots, unsigned& turned) {
    shots = g_enemyShots.load(std::memory_order_relaxed);
    turned = g_shotsTurned.load(std::memory_order_relaxed);
}

void FxShotStats(unsigned& hits, unsigned& passed, unsigned& cancelled) {
    hits = g_fxHitsOnLara.load(std::memory_order_relaxed);
    passed = g_fxHitsPassed.load(std::memory_order_relaxed);
    cancelled = g_laraShotsCancelled.load(std::memory_order_relaxed);
}

unsigned JumpButtonHeldMs() {
    const ULONGLONG since = g_jumpDownSinceMs.load(std::memory_order_relaxed);
    if (!since || !JumpButtonHeld()) return 0;
    const ULONGLONG now = GetTickCount64();
    return now > since ? static_cast<unsigned>(std::min<ULONGLONG>(now - since, 60000)) : 0;
}

bool AimHeld() {
    return g_airAimArmed.load(std::memory_order_relaxed);
}
}  // namespace CombatAir
