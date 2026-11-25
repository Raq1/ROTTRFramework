#include "grapple.h"
#include "brawl.h"
#include "combat_air.h"
#include "features.h"
#include "game_build.h"
#include "log.h"
#include "memory_probe.h"
#include "settings.h"
#include <Windows.h>
#include <imgui.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

// Grapple kick (Black Widow, Marvel's Avengers): the melee button with an enemy at range sends Lara flying to the
// enemy and ends in a kick; in the combat stance, on foot, or in a jump.
//
// The pieces are Rise's own (RE notes: re\melee\README.txt):
// - Lara's primary state control has a cut state [833] combat_melee_kick (melee move #4 "Kick", activities
//   Process_MeleeMove / Turn_TargetDir, back to combat_ranged_move on its own); nothing leads to it.
// - The melee press is T1644 "combat_ranged_to_ranged_melee_push_transient" in the combat stance and T1711
//   "ground_to_ground_ranged_melee_push_transient" on foot. While a target is in range, SCAgent::DoTransition runs
//   T1644 with its target switched to [833] for the call instead; near enemies (and the finishers) keep the game's
//   melee. In a jump (the jump / fall states) the melee action arms the kick: the next transition the state control
//   checks there is let through once (SCAgent::CheckConditions hook) and run as that switched T1644.
// - The animation: [833]'s cluster "Combat_Kick" plays library entry 1002 (our clip) in graph 267229's state S23, which
//   the graph only reaches from S5, the weapon stance (T68), and S5 needs the weapon in hand (rtFlag 4982
//   player_inventory_currentweaponhashostedgraph): a kick from a jump without aiming played nothing (v0.9.229). While
//   a kick runs, [833] asks for the moveset "Cheat" (copied from the fly cheat's state [1079]) and the graph's own
//   "Cheat" transition T63 (S0 -> S22) points at S23, T69 (S23 -> S5) at the hub S0: from the weapon stance (T11),
//   on foot (T3) and in the air (T20) the graph goes S0 -> S23, and back through S0 afterwards. Both halves of each
//   edge change: the DTP Edge's target byte (the new state's index and data) and the runtime transition's mTo (the
//   node that plays: v0.9.231 changed only the byte, and the fly cheat's idle node played under Combat_Kick's index,
//   Lara hanging in the air). They get their targets back 0.6 s after the kick. A
//   state after the kick that sets no moveset of its own (under Combat_Parent) gets Combat_Parent's "Combat" back.
//   (v0.9.230's route through the unused KillerInstinctEnter moveset played "hit_react_injured" instead of the clip.)
// - The target: the combat system's own ranged target (CombatManager::GetTargetInstance(RangedCombat)) when it is an
//   enemy in range, else the living enemy NPC (NpcComponent list, IsEnemyOf Lara) nearest the camera's aim within a
//   cone. It gets the game's own death-from-above highlight (NpcComponent::UpdateUI turns it on from a flag bit),
//   an arrow over its head, and the rope from Lara's hand while she flies (the arrow and the rope in the framework's
//   overlay: ImGui background list, projected with the draw tools' camera).
// - The rope: the game's own rope entity, the one level scripts draw (AGRopeVisualOnly), with the look of Lara's
//   grapple-axe rope (the traversal one: the game draws it with the player dtp's RopeRenderData, [PlayerComponent +0x2B0]
//   +0x9A0; the GrappleComponent that v0.9.234..237 looked for exists only while that grapple is in use), from the clip's
//   rope hand to the enemy; the overlay's line only when neither can be had.
// - The clip's frame: a clock that runs at the clip's own speed. The anim graph plays in the world's time
//   (TimeDilationManager +0x11C world x +0x120 global), while Lara's state control, an exempt instance, runs at its time
//   mult / world: in combat_air's slow motion (0.43 x 0.7) the clip played at 0.3, the state's frame counter at 1.63
//   times real time, and the exit let [833] go before the zip (v0.9.238 / 239: Lara stopped). The clip ignores its
//   fragment dtp's m_playspeed there (v0.9.241 raised it to 1 / world: the clip stayed at 0.3 and the kick was cut), so
//   in the slow motion the kick plays at the world's speed, all of it.
// - The clip (<game>\ROTTRFramework\grapple\, re\melee\bw2grapple.js from Black Widow's 15688, before v0.9.234 2188):
//   the arm shot, a "zip"
//   whose root travel (forward, and up or down to the enemy's height along an arc) is written in memory right before
//   each kick (two copies used in turn, so a kick still blending out keeps its own), and the kick. The clip's
//   .grapple.ini (re\melee\grapple_ini.js) gives the zip frames, the kick frame, the kick's reach and the rope.
// - [833]'s properties get Physics_NoGravity, Movement_CheckEdges off (the combat stance stops at ledges: from above
//   the zip used to stop at the edge), Physics_IgnoreFallMessage and, for zips with a height change, no terrain
//   collision (so the arc can pass a ledge lip). The anim is the motion source while it plays.
// - Melee move #4 (never used by the game) is patched in memory: a strike volume in front of Lara, each enemy hit
//   once (the move's own foot collision prims stay), damage, melee damage type and the hit reaction (HitReactSelect
//   with a dtp::HitReactType, or Ballistic); ProcessMeleeMove_StrikeVolume applies it to every NPC inside through
//   ApplyMeleeDamage. The volume is switched on at the clip's kick frame (v0.9.234 had it on for the whole move: the
//   enemy was hit, and reacted, as Lara arrived, before her foot landed).
// - With "can kill" off, the kick's damage is capped for its target (at the start and at the kick frame) so the enemy
//   keeps a tenth of its health: the move's bDamageCanNotKill (HealthComponent::AdjustHealth leaves 1 point and arms
//   the health component's no-kill timer) did not keep weak enemies alive (v0.9.235 test). An enemy already below that
//   gets 1 damage and the point back first (damage that rounds to 0 skips ApplyDamage, so the enemy would not react).
// - The impact, at the kick frame with the enemy within the volume's reach: the enemy's own melee-hit effect (the
//   connect FX of one of its melee moves, what it plays when its weapon hits Lara: cdc::FX_GenericTriggerFXIDEffects on
//   the enemy, as ProcessMeleeMove_StrikeVolume_Instance plays an attacker's), a hit-stop (a time dilation request of
//   type 5, ISS, which beats the scripts' and the slow motion's, for 100 ms; left out while the game has its own type 5
//   request out, and cancelled only while the slot still holds ours; then the manager goes back to the request that was
//   current before, or to normal speed), and from the fx list a camera shake and a body thud (Lara's hard landing
//   "beam_idle_jump_vertical_land_hard": its shake, and sound 11489).
// - Melee while aiming (and so in the slow motion, which needs aim held): the combat stance's melee push (T1644) is not
//   taken then, so the press arms the kick the way a jump arms it (the next transition checked runs as the kick).
// - Sounds: the clip plays with an fx list of its own, Lara's sound triggers copied from her library's fx lists and
//   retimed (none before frame 1: cdc::ANIMFX_TriggerEffectsV2 fires a trigger when previous time <= keyframe < time,
//   so one at frame 0 needs the clip's first update to start exactly at 0): the axe-grapple throw ("Axe_Swing_Throw_*",
//   sound 11507 is the axe's) as the rope flies and the axe biting (1790, "wall_climb_axe_attach_right") when the line
//   reaches the enemy,
//   the rope arrow's line (5649 / 11494), the jolt of a bad ledge grab ("ledge_mantle_attach_bad": her effort grunt, the
//   grab, a camera shake) and the grapple's own sounds (the zipline clamp 1759, "rope_zipline_attach"; the grapple axe's
//   whir 11507 and its rope's creak 3824, "Axe_Swing_Right") when the line catches, the dodge's whoosh ("Dodge_Right")
//   as the zip starts, her own kick's whoosh
//   and shout ("Combat_Kick") and the thud of her kicking an object ("interact_phys_kick_high", sound 11483) at the kick.
// - Lara cannot be hurt while the kick runs: HealthComponent::SetInvincible(ms) (RegisterDamage skips all damage
//   until the end time); an invincibility that runs forever (the framework's own) is left alone.
// - Lara turns to the target while she winds up and flies (PlayerSteering::Process hook in combat_air.cpp), and
//   [833]'s exit waits until the clip has played (SCAgent::CheckConditions hook). The clip's frame is the smaller of the
//   state's frame counter and the clock since the kick started: the counter ran three times faster than the clip in
//   v0.9.230 (the kick was cut after 0.4 s), the clock runs ahead of it in slow motion.

namespace {

constexpr std::uintptr_t kRvaGetLocalPlayerInstance = 0x00403BF0ull;
constexpr std::uintptr_t kRvaGetTargetInstance = 0x0082D470ull;          // CombatManager::GetTargetInstance(ESenseType, bool)
constexpr std::uintptr_t kRvaPlayerComponentIdUse = 0x0082D47Cull;       // mov r9d,[ComponentArrayID<PlayerComponent>::s_id]
constexpr std::uintptr_t kRvaPlayerComponentId = 0x010FAA90ull;
constexpr std::uintptr_t kRvaTryToGetMeleeMoveComponent = 0x008531F0ull; // cdc::TryToGetMeleeMoveComponent(Instance*)
constexpr std::uintptr_t kRvaTryToGetNpcComponent = 0x0024FCA0ull;       // TryToGetNpcComponent(Instance*)
constexpr std::uintptr_t kRvaMeleeMoveDtpLoad = 0x005E3EC5ull;           // mov rcx,[rdi+440h] (MeleeMoveComponent::GetMeleeMove)
constexpr std::uintptr_t kRvaCombatManagerLoad = 0x0068149Aull;          // mov rcx,[r14+31B0h] (CSCAF_E_ProcessMeleeMove)
constexpr std::uintptr_t kRvaGetAnimComponent = 0x003DDF50ull;           // Instance::GetAnimComponentV2
constexpr std::uintptr_t kRvaSetMotionSourcesType = 0x0011C985ull;       // mov [rdx+534h],eax (cdc::AnimStateGraph::SetMotionSources)
constexpr std::uintptr_t kRvaSetMotionSourcesFlags = 0x0011C99Dull;      // and byte ptr [rdx+53Ch],0FEh
// Animation tables and library registry (the same checks as acro.cpp).
constexpr std::uintptr_t kRvaResolveAnimation = 0x001BE220ull;
constexpr std::uintptr_t kRvaGetKeylistRegistry = 0x001146DCull;
constexpr std::uintptr_t kRvaRefCounts = 0x0160E5E0ull;
constexpr std::uintptr_t kRvaAnimByNumber = 0x015CFD50ull;
constexpr std::uintptr_t kRvaRegistry = 0x010FB790ull;
// Target lock-on: the NPC list, enemies, the camera, the melee input.
constexpr std::uintptr_t kRvaInputActionRequested = 0x001ADB50ull;      // INPUT_ActionRequested(controller, action)
constexpr std::uintptr_t kRvaIsEnemyOf = 0x004F2750ull;                 // NpcComponent::IsEnemyOf(Instance*)
constexpr std::uintptr_t kRvaNpcListLoad = 0x0070259Full;               // mov rbx,[NpcComponent list head] (a melee strike loop)
constexpr std::uintptr_t kRvaNpcList = 0x02D62CE8ull;
constexpr std::uintptr_t kRvaNpcListInstanceUse = 0x007025B0ull;        // mov rdx,[rbx+3C8h] (NpcComponent::m_pInstance)
constexpr std::uintptr_t kRvaNpcListNextUse = 0x007025CFull;            // mov rbx,[rbx+1A58h] (next NpcComponent)
constexpr std::uintptr_t kRvaNpcDeadUse = 0x004F28D7ull;                // test byte ptr [rcx+1931h],1 (NpcComponent::IsFinishable)
constexpr std::uintptr_t kRvaCameraForward = 0x0014A680ull;             // CameraManager::GetActiveCamera_Forward(out, id)
constexpr std::uintptr_t kRvaCameraPosition = 0x0014A6E0ull;            // CameraManager::GetActiveCamera_Position(out, id)
constexpr std::uintptr_t kRvaCameraManagerLoad = 0x006B7258ull;         // mov rcx,[m_pCameraManager] (PSCAF_P_Turn_CameraDir)
constexpr std::uintptr_t kRvaCameraForwardCall = 0x006B72ADull;         // call GetActiveCamera_Forward (same function)
// The game's highlight: NpcComponent::UpdateUI calls SurvivalInstinctComponent::EnableNonSIhighlight(+0x193E bit 4,
// DFA_Highlight) for the enemy every frame (the glow on a death-from-above target); the bit is only recomputed when
// the enemy's finisher prompt changes.
constexpr std::uintptr_t kRvaNpcDfaHighlightUse = 0x005432E8ull;      // movzx edx,byte ptr [rdi+193Eh]; mov r8d,2; shr dl,4
constexpr std::uintptr_t kRvaNpcDfaHighlightCall = 0x005432FBull;     // call EnableNonSIhighlight
constexpr std::uintptr_t kRvaEnableNonSiHighlight = 0x005D27B0ull;
// cdc::AnimComponentV2::SetAnimGraphGoal(type, value) (combat_air.cpp hooks it: its first 6 bytes may be a jump).
constexpr std::uintptr_t kRvaSetAnimGraphGoal = 0x0011C580ull;
constexpr std::uintptr_t kRvaSetAnimGraphGoalBody = 0x0011C586ull;
// Invincible during the kick: TryToGetHealthComponent (features.cpp's Infinite Health uses it too) and
// cdc::HealthComponent::SetInvincible(int ms) (< 0: forever); retail: [health +0x2B0] +0x28 the end time (i64, < 0
// forever), +0x30 the duration.
constexpr std::uintptr_t kRvaTryToGetHealthComponent = 0x008531C0ull;
constexpr std::uintptr_t kRvaSetInvincible = 0x0024CF30ull;
constexpr std::uintptr_t kRvaSetInvincibleBody = 0x0024CF58ull;
// The game's rope, drawn the way the level scripts' visual-only rope is (AGRopeVisualOnly, retail 0x734600):
// cdc::RopeRenderManager::CreateRopeEntity(Instance* = nullptr), then entity vtable +0x40 (2) once; each frame +0x20
// GetDrawable -> RopeDrawable::SetRopeRenderData(drawable, &start, &end, dtp::RopeRenderData*) (16-byte vectors),
// +0x60 SetVisible(true), +0x38 update; at the end +0x60 (false) and +0xC0 release. Lara's rope look: her
// GrappleComponent (component id [0x1A53AD0]) +0x2A8 dtp::GrappleComponent, +0x48 m_ropeRenderData (as
// GrappleRope::Render, 0x62B2B0, reads it).
constexpr std::uintptr_t kRvaCreateRopeEntity = 0x001CAE10ull;
constexpr std::uintptr_t kRvaSetRopeRenderData = 0x008799B0ull;
constexpr std::uintptr_t kRvaVisualRopeCreateCall = 0x0073462Aull;     // call CreateRopeEntity (after xor ecx,ecx)
constexpr std::uintptr_t kRvaVisualRopeInit = 0x0073462Full;           // mov [rsi+60h],rax; ...; mov edx,2; call [rax+40h]
constexpr std::uintptr_t kRvaVisualRopeRelease = 0x0073467Aull;        // xor edx,edx; call [rax+60h]; ...; call [rax+0C0h]
constexpr std::uintptr_t kRvaVisualRopeUpdate = 0x00734B05ull;         // call [rax+20h]; ...; call SetRopeRenderData
constexpr std::uintptr_t kRvaVisualRopeUpdateCall = 0x00734B1Eull;
constexpr std::uintptr_t kRvaVisualRopeUpdateTail = 0x00734B23ull;     // mov dl,1; call [rax+60h]; ...; call [rax+38h]
constexpr std::uintptr_t kRvaGrappleRopeIdUse = 0x0062B385ull;         // mov eax,[GrappleComponent id]
constexpr std::uintptr_t kRvaGrappleComponentId = 0x01A53AD0ull;
constexpr std::uintptr_t kRvaGrappleRopeDtpUse = 0x0062B3B0ull;        // mov r13,[rdx+2A8h]
constexpr std::uintptr_t kRvaGrappleRopeDataUse = 0x0062BABCull;       // lea rdx,[r13+48h]; call SetRopeRenderData(points)
// The grapple's instance (the GrappleComponent is on it, not on Lara): Instance::Query(lara, 0x181) (Player::Query:
// InventoryComponent::GetItemByType(9)), as a PlayerSC grapple condition asks for it (retail 0x668E60).
constexpr std::uintptr_t kRvaInstanceQuery = 0x003E7350ull;
constexpr std::uintptr_t kRvaGrappleQueryUse = 0x00668E6Full;          // mov rcx,[rcx+10h]; mov edx,181h; ...; call Query
constexpr std::uintptr_t kRvaGrappleQueryCall = 0x00668E80ull;
constexpr std::uintptr_t kRvaGrappleQueryIdUse = 0x00668E8Dull;        // mov eax,[GrappleComponent id]
constexpr int kQueryGrappleInstance = 0x181;
// Else any GrappleComponent: the instances live in the instance manager's slot array (retail 0x24EEAD0, 0x120 each;
// Instance::Query reads a slot's components through lea r8,[slots+0xD8]); a live slot holds its handle at +0x118 (low 32
// bits the slot), a component its instance at +8.
// Lara's grapple-axe rope look: PSCAF_P_GrappleAxe_UpdateThrownAxe (retail 0x6B1647: mov rdi,[r8+2A8h] (Player); mov
// r13,[r8+2B0h] (the player dtp); ... 0x6B1CBB: lea r9,[r13+9A0h]; ...; call RopeDrawable::SetRopeRenderData) draws the
// Player's own rope entity (+0x1628) with it.
constexpr std::uintptr_t kRvaAxeRopeDtpUse = 0x006B1647ull;
constexpr std::uintptr_t kRvaAxeRopeDataUse = 0x006B1CBBull;
constexpr std::uintptr_t kRvaAxeRopeDataCall = 0x006B1CCCull;
constexpr std::uint8_t kAxeRopeDtpSig[] = {0x49,0x8B,0xB8,0xA8,0x02,0x00,0x00,0x4D,0x8B,0xA8,0xB0,0x02,0x00,0x00};
constexpr std::uint8_t kAxeRopeDataSig[] = {0x4D,0x8D,0x8D,0xA0,0x09,0x00,0x00,0x4C,0x8D,0x44,0x24,0x50,0x48,0x8D,0x54,0x24,
                                            0x60,0xE8};
constexpr std::size_t kPlayerComponentDtp = 0x2B0;
constexpr std::size_t kPlayerAxeRopeData = 0x9A0;
constexpr std::uintptr_t kRvaInstanceSlots = 0x024EEAD0ull;
constexpr std::size_t kInstanceSlotSize = 0x120;
constexpr std::uint32_t kInstanceSlotCount = 0x7530;
constexpr std::size_t kComponentInstance = 0x08;
constexpr ULONGLONG kGrappleScanRetryMs = 5000;
// The enemy's melee-hit effect: cdc::FX_GenericTriggerFXIDEffects(Instance*, ObjectZone* = [instance+0x80], fx id,
// 0, Vector3* = null, ObjectEffectFXA* = null) plays the triggers of the instance's object FX list with that id, as
// ProcessMeleeMove_StrikeVolume_Instance plays the attacker's move's uConnectFXID (retail call 0x702CCA).
constexpr std::uintptr_t kRvaFxTriggerById = 0x003DC360ull;
constexpr std::uintptr_t kRvaStrikeConnectFxUse = 0x00702CAEull;     // movzx r8d,byte [r14+4]; ...; mov rdx,[r15+80h]
constexpr std::uintptr_t kRvaStrikeConnectFxCall = 0x00702CCAull;
// The hit-stop: TimeDilationManager (the calls combat_air.cpp's slow motion makes, in its slot 9). One request slot per
// dtp::TimeDilationType; TimeDilationManager::Process follows the first active slot (forceActive, or a factor other
// than 1), and moves to it only when that slot is new (dirty): 5 (ISS, the interactive sequences) beats the scripts
// (6), the state control (7), the hunter sense (8) and the slow motion (9) while it lasts.
constexpr std::uintptr_t kRvaTimeDilationRequestCtor = 0x00483B90ull;  // TimeDilationRequest::TimeDilationRequest
constexpr std::uintptr_t kRvaTimeDilationRequest = 0x00497AF0ull;      // TimeDilationManager::Request
constexpr std::uintptr_t kRvaTimeDilationCancel = 0x004865B0ull;       // TimeDilationManager::CancelRequest
constexpr std::uintptr_t kRvaSetTimeDilationManager = 0x006A947Full;   // lea rcx,[ms_timeDilationManager]

constexpr std::uint8_t kGetLocalPlayerInstanceSig[] = {0x40,0x53,0x48,0x83,0xEC,0x20,0x0F,0xB6,0xD9};
constexpr std::uint8_t kGetTargetInstanceSig[] = {0x48,0x83,0xEC,0x28,0x83,0xFA,0xFF,0x74,0x70,0x48,0x8B,0x01,0x44,0x8B,0x0D};
constexpr std::uint8_t kTryToGetMeleeMoveComponentSig[] = {0x48,0x8B,0x51,0x68,0x48,0xB8,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0x48,0x85,0xD0};
constexpr std::uint8_t kTryToGetNpcComponentSig[] = {0x48,0x8B,0xD1,0x48,0x85,0xC9,0x74,0x29,0x8B,0x81,0xC8,0x00,0x00,0x00,0xC1,0xE8,0x07,0xA8,0x01};
constexpr std::uint8_t kMeleeMoveDtpLoadSig[] = {0x48,0x8B,0x8F,0x40,0x04,0x00,0x00};
constexpr std::uint8_t kCombatManagerLoadSig[] = {0x49,0x8B,0x8E,0xB0,0x31,0x00,0x00};
constexpr std::uint8_t kGetAnimComponentSig[] = {0x48,0x8B,0xC1,0x48,0x85,0xC9,0x74,0x1A,0x8B,0x0D,0xCA,0x3D,0xD2,0x00,0x39,0x88,
                                                 0xD0,0x00,0x00,0x00,0x76,0x0C,0x48,0x8B,0x80,0xD8,0x00,0x00,0x00,0x48,0x8B,0x04,0xC8,0xC3};
constexpr std::uint8_t kSetMotionSourcesTypeSig[] = {0x89,0x82,0x34,0x05,0x00,0x00};
constexpr std::uint8_t kSetMotionSourcesFlagsSig[] = {0x80,0xA2,0x3C,0x05,0x00,0x00,0xFE};
constexpr std::uint8_t kResolveAnimationSig[] = {
    0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7c,0x24,0x18,0x41,0x56,0x48,0x83,0xec,
    0x20,0x8b,0xd9,0x4c,0x8d,0x35,0xc2,0x1d,0xe4,0xff,0x8b,0xf2,0x45,0x0f,0xb7,0x84,0x5e,0xe0,0xe5,0x60,
    0x01,0x41,0x8d,0x40,0x01,0x66,0x41,0x89,0x84,0x5e,0xe0,0xe5,0x60,0x01,0x66,0x45,0x85,0xc0,0x75,0x27,
    0xe8,0xaf,0x51,0x18,0x00,0x8b,0xd6,0x45,0x33,0xc0,0x48,0x8b,0xc8,0x4c,0x8b,0x08,0x41,0xff,0x91,0x90,
    0x00,0x00,0x00,0x49,0x89,0x84,0xde,0x50,0xfd,0x5c,0x01
};
constexpr std::uint8_t kGetKeylistRegistrySig[] = {0x48,0x8B,0x0D};
constexpr std::uint8_t kInputActionRequestedSig[] = {0x83,0xF9,0xFF,0x74,0x2C,0x48,0x63,0xC9,0x48,0x8D,0x05};
constexpr std::uint8_t kIsEnemyOfSig[] = {0x40,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0x89,0xC8,0x03,0x00,0x00,0x48,0x8B,0xFA,
                                          0x48,0x85,0xC9,0x74,0x31,0x48,0x85,0xD2,0x74,0x2C,0x48,0x3B,0xCA,0x74,0x27};
constexpr std::uint8_t kMovRbxRipSig[] = {0x48,0x8B,0x1D};  // + rel32
constexpr std::uint8_t kNpcListInstanceSig[] = {0x48,0x8B,0x93,0xC8,0x03,0x00,0x00};
constexpr std::uint8_t kNpcListNextSig[] = {0x48,0x8B,0x9B,0x58,0x1A,0x00,0x00};
constexpr std::uint8_t kNpcDeadSig[] = {0xF6,0x81,0x31,0x19,0x00,0x00,0x01};
constexpr std::uint8_t kCameraForwardSig[] = {
    0x48,0x83,0xB9,0x98,0x00,0x00,0x00,0x00,0x4C,0x8B,0xCA,0x74,0x42,0x33,0xD2,0x41,0x8B,0xC0,0x48,0xF7,
    0xB1,0xA0,0x00,0x00,0x00
};
constexpr std::uint8_t kCameraPositionSig[] = {
    0x48,0x83,0xB9,0x98,0x00,0x00,0x00,0x00,0x4C,0x8B,0xCA,0x74,0x46,0x33,0xD2,0x41,0x8B,0xC0,0x48,0xF7,
    0xB1,0xA0,0x00,0x00,0x00
};
constexpr std::uint8_t kCameraPositionReadSig[] = {0x0F,0x28,0x81,0xB0,0x00,0x00,0x00};  // movaps xmm0,[rcx+0B0h] at +0x43
constexpr std::uint8_t kNpcDfaHighlightSig[] = {0x0F,0xB6,0x97,0x3E,0x19,0x00,0x00,0x41,0xB8,0x02,0x00,0x00,0x00,0xC0,0xEA,0x04};
constexpr std::uint8_t kEnableNonSiHighlightSig[] = {0x45,0x85,0xC0,0x0F,0x84,0xB4,0x00,0x00,0x00,0x53,0x48,0x83,0xEC,0x20};
// mov rbx,rcx; movsxd rcx,edx; mov r9d,[rbx+rcx*4+4B4h]; cmp r9d,r8d
constexpr std::uint8_t kSetAnimGraphGoalBodySig[] = {0x48,0x8B,0xD9,0x48,0x63,0xCA,0x44,0x8B,0x8C,0x8B,0xB4,0x04,0x00,0x00,0x45,0x3B,0xC8};
constexpr std::uint8_t kTryToGetHealthSig[] = {0x48,0x8B,0xC1,0x48,0x85,0xC9,0x74,0x1A,0x8B,0x0D};
constexpr std::uint8_t kTryToGetHealthTailSig[] = {0x39,0x88,0xD0,0x00,0x00,0x00,0x76,0x0C,0x48,0x8B,0x80,0xD8,0x00,0x00,0x00,
                                                   0x48,0x8B,0x04,0xC8,0xC3};
constexpr std::uint8_t kSetInvincibleSig[] = {0x48,0x89,0x6C,0x24,0x18,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xF9,0x48,0x63,0xEA};
// mov rax,[rdi+2B0h]; test ebp,ebp; jns; mov dword [rax+30h],-1; mov rax,[rdi+2B0h]; mov qword [rax+28h],-1
constexpr std::uint8_t kSetInvincibleBodySig[] = {0x48,0x8B,0x87,0xB0,0x02,0x00,0x00,0x85,0xED,0x79,0x18,0xC7,0x40,0x30,0xFF,0xFF,
                                                  0xFF,0xFF,0x48,0x8B,0x87,0xB0,0x02,0x00,0x00,0x48,0xC7,0x40,0x28,0xFF,0xFF,0xFF,0xFF};
constexpr std::uint8_t kCreateRopeEntitySig[] = {0x48,0x89,0x5C,0x24,0x18,0x57,0x48,0x81,0xEC,0x90,0x00,0x00,0x00,0x48,0x8B,0xF9,
                                                 0x48,0x8B,0x0D};
constexpr std::uint8_t kSetRopeRenderDataSig[] = {0x40,0x53,0x48,0x83,0xEC,0x20,0x0F,0x28,0x02,0x48,0x8B,0xD9,0x66,0x0F,0x7F,0x41,
                                                  0x10,0x0F,0x28,0xE8,0x41,0x0F,0x28,0x08,0x66,0x0F,0x7F,0x49,0x20};
constexpr std::uint8_t kVisualRopeInitSig[] = {0x48,0x89,0x46,0x60,0x48,0x8B,0x4E,0x60,0xBA,0x02,0x00,0x00,0x00,0x48,0x8B,0x01,
                                               0xFF,0x50,0x40};
constexpr std::uint8_t kVisualRopeReleaseSig[] = {0x33,0xD2,0xFF,0x50,0x60,0x48,0x8B,0x4E,0x60,0x48,0x8B,0x01,0xFF,0x90,0xC0,0x00,
                                                  0x00,0x00};
constexpr std::uint8_t kVisualRopeUpdateSig[] = {0x48,0x8B,0x4E,0x60,0x48,0x8B,0x01,0xFF,0x50,0x20,0x48,0x8B,0xC8,0x4C,0x8D,0x4B,
                                                 0x78,0x4C,0x8D,0x45,0xA7,0x48,0x8D,0x55,0xB7,0xE8};
constexpr std::uint8_t kVisualRopeUpdateTailSig[] = {0x48,0x8B,0x4E,0x60,0xB2,0x01,0x48,0x8B,0x01,0xFF,0x50,0x60,0x48,0x8B,0x4E,0x60,
                                                     0x48,0x8B,0x01,0xFF,0x50,0x38};
constexpr std::uint8_t kGrappleRopeIdSig[] = {0x8B,0x05};
constexpr std::uint8_t kGrappleRopeDtpSig[] = {0x4C,0x8B,0xAA,0xA8,0x02,0x00,0x00};
constexpr std::uint8_t kGrappleRopeDataSig[] = {0x49,0x8D,0x55,0x48,0xE8};
// mov eax,[rcx+68h]; lea r8,[slots]; ... mov rax,[rax+48h]; test rax,rax; je
constexpr std::uint8_t kInstanceQuerySig[] = {0x8B,0x41,0x68,0x4C,0x8D,0x05};
constexpr std::uint8_t kInstanceQueryTailSig[] = {0x48,0x8D,0x04,0xC0,0x48,0xC1,0xE0,0x05,0x4A,0x8B,0x04,0x00,0x48,0x8B,0x40,0x48,
                                                  0x48,0x85,0xC0,0x74,0x0C};
constexpr std::uint8_t kGrappleQuerySig[] = {0x48,0x8B,0x49,0x10,0xBA,0x81,0x01,0x00,0x00,0x49,0x8B,0xF0,0x32,0xDB,0x40,0x32,
                                             0xFF,0xE8};
constexpr std::uint8_t kGrappleQueryIdSig[] = {0x8B,0x05};
// cmp r8d,13h; je ret; push r12..r15; sub rsp,0D8h; mov r13,rcx; mov r14d,r9d
constexpr std::uint8_t kFxTriggerByIdSig[] = {0x41,0x83,0xF8,0x13,0x0F,0x84,0xA4,0x01,0x00,0x00,0x41,0x54,0x41,0x55,0x41,0x56,
                                              0x41,0x57,0x48,0x81,0xEC,0xD8,0x00,0x00,0x00,0x4C,0x8B,0xE9,0x45,0x8B,0xF1};
constexpr std::uint8_t kStrikeConnectFxSig[] = {0x45,0x0F,0xB6,0x46,0x04,0x45,0x33,0xC9,0x49,0x8B,0x97,0x80,0x00,0x00,0x00,0x49,
                                                0x8B,0xCF,0x4C,0x89,0x64,0x24,0x28,0x4C,0x89,0x64,0x24,0x20,0xE8};
constexpr std::uint8_t kTimeDilationRequestCtorSig[] = {0x89,0x11,0x0F,0x57,0xC0,0xF3,0x0F,0x11,0x51,0x04,0xF3,0x0F,0x11,0x59,0x08,
                                                        0x66,0xC7,0x41,0x0C,0x00,0x01};
constexpr std::uint8_t kTimeDilationRequestSig[] = {0x4C,0x63,0x12,0x4C,0x8B,0xD9,0x44,0x8B,0x89,0x18,0x01,0x00,0x00,0x0F,0x10,0x02,
                                                    0x4D,0x6B,0xC2,0x1C};
constexpr std::uint8_t kTimeDilationCancelSig[] = {0x48,0x63,0xC2,0x0F,0x57,0xC0,0x48,0x6B,0xD0,0x1C,0x48,0x03,0xD1,0xC7,0x02,0x0A,
                                                   0x00,0x00,0x00};
constexpr std::uint8_t kLeaRcxRipSig[] = {0x48,0x8D,0x0D};  // + rel32
constexpr std::size_t kInstanceObjectZone = 0x80;      // Instance::m_objectZoneRef
constexpr int kFxIdNone = 0x13;                        // FX_GenericTriggerFXIDEffects returns at once for it
// TimeDilationRequest (0x1C): i32 type (10 = none) +0, world +4, global +8 (the world runs at world * global, Lara at
// global), u8 forceActive +0xC, u8 dirty +0xD, startRampTime +0x10 (the request's time: gameTracker's
// currentMaterialTime), rampTime +0x14 (33 ms), activateTime +0x18.
constexpr int kHitStopType = 5;                        // dtp::TimeDilationType_ISS
constexpr int kTimeDilationTypes = 10;
constexpr std::size_t kTimeDilationRequestSize = 0x1C;
constexpr std::size_t kDilationWorld = 0x04, kDilationGlobal = 0x08, kDilationForce = 0x0C, kDilationDirty = 0x0D,
                      kDilationStart = 0x10;
constexpr std::size_t kDilationCurrentType = 0x118;    // TimeDilationManager: the type Process follows (10 = none)
constexpr float kHitStopSpeed = 0.1f;
constexpr const char* kHitStopName = "ROTTRFramework grapple kick hit-stop";
constexpr std::size_t kGrappleComponentDtp = 0x2A8;    // GrappleComponent::m_dtpProperties (retail)
constexpr std::size_t kGrappleRopeRenderData = 0x48;   // dtp::GrappleComponent::m_ropeRenderData
constexpr std::size_t kEntityGetDrawable = 0x20, kEntityUpdate = 0x38, kEntityInit = 0x40, kEntitySetVisible = 0x60,
                      kEntityRelease = 0xC0;
constexpr std::size_t kHealthInvincibility = 0x2B0;
constexpr std::size_t kInvincibleEnd = 0x28;
// The target's health: cdc::HealthComponent::AdjustHealth (retail 0x229800) adds the change to [+0x2A8] +0x2C (the
// current health, float) and clamps it to [+0x2C4] (the maximum, int).
constexpr std::uintptr_t kRvaAdjustHealthValues = 0x0022989Cull;   // mov rax,[rbx+2A8h]; movaps xmm6,xmm7
constexpr std::uintptr_t kRvaAdjustHealthCurrent = 0x002298AEull;  // addss xmm6,[rax+2Ch]
constexpr std::uintptr_t kRvaAdjustHealthMax = 0x002298C2ull;      // movd xmm0,[rbx+2C4h]
constexpr std::uint8_t kAdjustHealthValuesSig[] = {0x48,0x8B,0x83,0xA8,0x02,0x00,0x00,0x0F,0x28,0xF7};
constexpr std::uint8_t kAdjustHealthCurrentSig[] = {0xF3,0x0F,0x58,0x70,0x2C};
constexpr std::uint8_t kAdjustHealthMaxSig[] = {0x66,0x0F,0x6E,0x83,0xC4,0x02,0x00,0x00};
constexpr std::size_t kHealthValues = 0x2A8, kHealthCurrent = 0x2C, kHealthMax = 0x2C4;
constexpr float kNoKillFloor = 0.1f;                   // the share of its health a kick leaves the enemy ("can kill" off)
// The kick clip's own time: cdc::AnimFragmentNode (+0x38 mOpDataID, +0x40 dtp::FragmentNode {u16 Anim, ..., f32
// m_playspeed +4}) keeps it at +0x60 of its operational data: AnimComponentV2 [instance graph offset + 8]
// cdc::AnimOperationalData {u8** table +0, ..., AnimComponentV2* +0x20}, table[mOpDataID] (AnimFragmentNode::Activate,
// retail 0x12F190: mov rax,[rdx]; movsxd r9,[rcx+38h]; mov rbx,[rax+r9*8]; ...; mov rax,[rcx+40h]; mulss xmm3,[rax+4]).
constexpr std::uintptr_t kRvaFragmentActivate = 0x0012F190ull;
constexpr std::uint8_t kFragmentActivateSig[] = {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,
                                                 0x48,0x83,0xEC,0x50,0x48,0x8B,0x02,0x48,0x8B,0xF2,0x4C,0x63,0x49,0x38};
// AnimFragmentNode::Update (retail 0x1381B0) advances the node's time by m_playspeed x dt every frame (0x13832D reads
// the time; TR2 0x4014B1FD reads [dtp+4]).
constexpr std::uintptr_t kRvaFragmentUpdate = 0x001381B0ull;
constexpr std::uint8_t kFragmentUpdateSig[] = {0x4C,0x8B,0xDC,0x55,0x53,0x56,0x41,0x56,0x41,0x57,0x49,0x8D,0x6B,0xA1,0x48,0x81,
                                               0xEC,0xD0,0x00,0x00,0x00,0x48,0x8B,0x02,0x4D,0x8B,0xF0,0x4C,0x63,0x49,0x38};
constexpr std::size_t kNodeDtp = 0x40, kFragmentPlaySpeed = 0x04;
constexpr std::size_t kDilationWorldNow = 0x11C, kDilationGlobalNow = 0x120;  // TimeDilationManager: the current factors
// The attach: a bad ledge grab's jolt, and the grapple's own sounds.
constexpr const char* kAttachSource = "ledge_mantle_attach_bad";
constexpr const char* kGrappleClampSource = "rope_zipline_attach";   // kf 0: 1759 on the axe, the hook clamping on
constexpr std::uint32_t kGrappleClampSound = 1759;
constexpr const char* kGrappleSwingSource = "Axe_Swing_Right";       // the grapple axe's swing: kf 0 11507, kf 1 3824
constexpr std::uint32_t kGrappleWhirSound = 11507, kGrappleCreakSound = 3824;
constexpr ULONGLONG kKickTimeoutMs = 12000;
constexpr ULONGLONG kKickCooldownMs = 700;             // after a kick, the graph needs this to leave S23
constexpr ULONGLONG kInvincibleRefreshMs = 500;
constexpr int kInvincibleMs = 1500;                    // the clip (1.2 s) and its blend out
constexpr std::uint8_t kMovRcxRipSig[] = {0x48,0x8B,0x0D};  // + rel32
constexpr std::uint8_t kCallRel32Sig[] = {0xE8};            // + rel32

// Retail layouts.
constexpr std::size_t kInstancePos = 0x10;             // cdc::Vector3 position
constexpr std::size_t kInstanceComponentId = 0x68;     // what the native highlight checks the slot against
constexpr std::size_t kInstanceComponentCount = 0xD0;
constexpr std::size_t kInstanceComponents = 0xD8;
constexpr std::size_t kInstanceHandle = 0x118;         // the instance manager slot's handle (low 32 bits: the slot)
constexpr std::size_t kPlayerComponentPlayer = 0x2A8;
constexpr std::size_t kPlayerCombatManager = 0x31B0;   // CombatManager*
constexpr std::size_t kPlayerCameraId = 0x3258;        // the id PSCAF_P_Turn_CameraDir passes to GetActiveCamera_Forward
constexpr std::size_t kMeleeComponentDtp = 0x440;      // dtp::MeleeMoveComponent* {groups, n, MeleeMove* moves +0x10, n +0x18}
constexpr std::size_t kNpcInstance = 0x3C8;            // NpcComponent::m_pInstance
constexpr std::size_t kNpcNext = 0x1A58;               // the next NpcComponent in the manager's list
constexpr std::size_t kNpcFlags = 0x1931;              // bit 0 m_dead
constexpr std::size_t kNpcUiFlags = 0x193E;            // bit 4: the death-from-above highlight (see kRvaNpcDfaHighlightUse)
constexpr std::uint8_t kNpcUiDfaBit = 0x10;
// cdc::AnimComponentV2: the anim graph's motion sources. Each graph state's AnimStateUserDTP (TranslationSource /
// RotationSource, 0 = keep) sets them on entry (cdc::AnimStateGraph::SetMotionSources). Player::PositionChange drops
// the moves the source excludes: 2 = the character's own movement (the anim's root motion dropped), 3 = the anim's
// root motion (the controlled movement dropped). The combat evades use 3; "Combat_Kick" keeps 0, so the combat
// stance's 2 stays and the clip's root travel is ignored (v0.9.226 test: Lara kicked in place, moved 0 cm).
constexpr std::size_t kAnimMotionType = 0x534;
constexpr std::size_t kAnimMotionFlags = 0x53C;        // bit 0 root translation on, bit 1 root rotation on
constexpr std::size_t kAnimGoals = 0x4B4;              // dtp::AnimGraphGoalRTValues (u32 each; 0 = the moveset)
constexpr int kGoalMoveset = 0;
constexpr std::uint32_t kMovesetCombat = 0xA24C8D11u;  // Combat_Parent's AGGoalMoveset "Combat"
// Graph 267229 at run time: AnimComponentV2 -> cdc::AnimGraphRoot (m_instanceGraph, TR2 +0x3C8; looked for in a
// range and checked by name) -> +0x10 m_graph (cdc::AnimStateGraph) -> +0x110 mDTPData. dtp::AnimStateGraph: states
// +0, transitions +8, u8 numStates +0x10, u8 numTransitions +0x11; State 0x70 (name +0, child +0x10, kind u8 +0x48:
// 0 pipe, 1 state graph, 2 transient); pipe: u8 numNodes +0, nodes +8; node 0x30 (name +0, kind u8 +8: 2 embedded
// state graph, graph +0x10); Edge 0x60 (src u8 +0x58, dst u8 +0x59).
constexpr std::size_t kAnimGraphScanFrom = 0x380, kAnimGraphScanTo = 0x420;
constexpr std::size_t kGraphRootGraph = 0x10;
constexpr std::size_t kStateGraphDtp = 0x110;
constexpr std::size_t kGraphStates = 0x00, kGraphEdges = 0x08, kGraphNumStates = 0x10, kGraphNumEdges = 0x11;
constexpr std::size_t kGraphStateSize = 0x70, kGraphStateChild = 0x10, kGraphStateKind = 0x48;
constexpr std::size_t kPipeNumNodes = 0x00, kPipeNodes = 0x08;
constexpr std::size_t kGraphNodeSize = 0x30, kGraphNodeKind = 0x08, kGraphNodeSub = 0x10;
constexpr std::size_t kGraphEdgeSize = 0x60, kGraphEdgeSrc = 0x58, kGraphEdgeDst = 0x59;
constexpr std::uint8_t kPlayableDefault = 0, kPlayableWeapon = 5, kPlayableCheat = 22, kPlayableKick = 23;
// The runtime graph (the retail build as the TR2 PDB has it; CreateStateGraph 0x10DC80 builds each transition, and
// EvaluateTransitions takes the new state's node from mTo, its index from the Edge's dst byte):
// cdc::AnimGraphRoot +0x60 m_allocator (current block; blocks of 0x4008 bytes: the previous block's address, then
// 0x4000 of objects); cdc::AnimStateGraph (from that allocator) m_owner +0x10, mStateInfos +0x98 {PlugInfo* (0x20 each,
// outputPlugs {Plug*, n} first: a state's node = its first output plug's node), u32 n +0xA0}, mAnimStateTransitions
// +0xA8 {cdc::AnimStateTransition* (0x30 each: mParent +0, mFrom +8, mTo +0x10, mDTPProperties +0x20), u32 n +0xB0}.
constexpr std::size_t kRootAllocator = 0x60;
constexpr std::size_t kAllocBlockData = 0x4000;
constexpr std::size_t kRtOwner = 0x10;
constexpr std::size_t kRtStateInfos = 0x98, kRtNumStateInfos = 0xA0, kPlugInfoSize = 0x20;
constexpr std::size_t kRtTransitions = 0xA8, kRtNumTransitions = 0xB0, kRtTransitionSize = 0x30;
constexpr std::size_t kRtTransFrom = 0x08, kRtTransTo = 0x10, kRtTransEdge = 0x20;
constexpr std::int32_t kMotionFromMovement = 2;
constexpr std::int32_t kMotionFromAnimation = 3;       // what the evade states use
constexpr std::size_t kAgentData = 0x08;
constexpr std::size_t kAgentFramesInState = 0x618;
constexpr std::size_t kAgentCurrentState = 0x1C10;
constexpr std::size_t kAgentDataNumStates = 0x18;
constexpr std::size_t kAgentDataNumTransitions = 0x1A;
constexpr std::size_t kAgentDataStateRefs = 0x20;
constexpr std::size_t kAgentDataTransitionRefs = 0x28;
constexpr std::size_t kStateParent = 0x00;             // u16 state index, 0xFFFF none
constexpr std::size_t kStatePropCount = 0x04;
constexpr std::size_t kStateProps = 0x10;
constexpr std::size_t kStateName = 0x38;
constexpr std::size_t kPropSize = 0x20;                // dtp::SCPropertyData: i16 property, i16 type, value at +8
constexpr std::size_t kPropValue = 0x08;
constexpr std::int16_t kPropTypeGoalCluster = 6;       // dtp::SCPropertyData::EnumSCPropertyType
constexpr std::int16_t kPropTypeBoolean = 16;
constexpr std::int16_t kPropTypeMeleeMove = 63;
// dtp::EnumSCStateProperty. SCAgent::SetStateProperties walks the state's property array on every state change.
constexpr std::int16_t kPropCheckEdges = 268;          // Movement_CheckEdges (Combat_Parent: true, drop 15 cm)
constexpr std::int16_t kPropStopAtSafePoint = 272;     // Movement_CheckEdges_StopAtSafePoint (combat_melee_kick: true)
constexpr std::int16_t kPropNoGravity = 619;           // Physics_NoGravity
constexpr std::int16_t kPropNoTerrainCollision = 634;  // Physics_NoTerrainCollision
constexpr std::int16_t kPropIgnoreFallMessage = 636;   // Physics_IgnoreFallMessage
constexpr std::int16_t kPropGoalMoveset = 56;          // AnimGraph_Goal_Moveset (type 7, a string and its hash)
constexpr int kMaxKickProps = 32;
constexpr std::size_t kTransitionTarget = 0x00;
constexpr std::size_t kTransitionName = 0x18;

// dtp::MeleeMove (0x258 bytes, TR2 PDB; the DTP data layout).
constexpr std::size_t kMoveSize = 0x258;
constexpr std::size_t kMoveDamage = 0x08;
constexpr std::size_t kMoveStrikeDamage = 0x18;
constexpr std::size_t kMoveDamageType = 0x30;        // dtp::DamageType
constexpr std::size_t kMoveEffect = 0x34;            // dtp::DamageEffect: 0 HitReact, 1 Ballistic, 2 HitReactSelect
constexpr std::size_t kMoveEffectUnion = 0x38;       // HitReactSelect: dtp::HitReactType
constexpr std::size_t kMoveRepeatOption = 0x50;      // VictimRepeatedCollisionOption: 0 Ignore (each victim once), 1 TakeAll
constexpr std::size_t kMoveRepeatFrames = 0x54;      // VictimRepeatedCollisionIgnoreNumFrames
constexpr std::size_t kMoveFrames = 0x5C;            // IntRange: the collision prims' frames
constexpr std::size_t kMoveStrikeFrames = 0x6C;      // IntRange: the strike volume's frames
constexpr std::size_t kMoveVolume = 0x94;            // VolumeData (0x58)
constexpr std::size_t kMoveName = 0x198;
constexpr std::size_t kMoveConnectFx = 0x04;         // u8 uConnectFXID: the attacker's FX when the move connects
constexpr std::size_t kMoveUse2D = 0x22F;
constexpr std::size_t kMoveVolumeOn = 0x234;
constexpr std::size_t kMoveCustomStrikeDamage = 0x235;
constexpr std::size_t kMoveCanNotKill = 0x237;
constexpr std::size_t kMoveCanNotKillExceptLow = 0x238;
constexpr int kKickMoveIndex = 4;                    // "Kick" (combat_melee_kick's Combat_MeleeMove)
constexpr int kDamageTypeMelee = 29;                 // DamageType_Melee (Lara's melee push)
constexpr int kSenseRangedCombat = 0;                // dtp::ESenseType_RangedCombat
constexpr int kActionMeleePush = 183;                // INPUTACTION_Combat_Melee_Push (T1644's "Requested")
constexpr int kActionMeleeFinisher = 179;            // INPUTACTION_Combat_Melee_Finisher (death from above's)

constexpr const char* kKickStateName = "combat_melee_kick";
constexpr const char* kEntryTransitionName = "combat_ranged_to_ranged_melee_push_transient";  // T1644
constexpr const char* kExitTransitionName = "combat_melee_kick_to_combat_ranged_move";        // T1636
constexpr const char* kGroundEntryTransitionName = "ground_to_ground_ranged_melee_push_transient";  // T1711, on foot
constexpr const char* kCheatStateName = "cheat";    // [1079], the fly cheat: its moveset goal "Cheat"
// The kick's sounds: Lara's library entries whose fx lists they are copied from (first name found wins).
constexpr const char* kThrowSources[] = {"Axe_Swing_Throw_Center", "Axe_Swing_Throw_Low", "Axe_Swing_Throw_High"};
// (the acrobatics replace Dodge_Forward / _Center / _Left / _Right_pistol / _Right_smg with their flips' own lists)
constexpr const char* kWhooshSources[] = {"Dodge_Right", "Dodge_Forward_Bow", "Dodge_Center_Bow", "Dodge_Forward"};
constexpr const char* kRopeSoundSources[] = {"Ranged_RopeArrow_Pull_Med", "Ranged_RopeArrow_Pull_Low", "Ranged_RopeArrow_Pull_High"};
constexpr std::uint32_t kRopeZipSound = 5649, kRopeTensionSound = 11494;  // the rope arrow's line
constexpr const char* kImpactSources[] = {"interact_phys_kick_high", "interact_phys_kick_low"};
constexpr std::uint32_t kImpactSound = 11483;          // the thud of Lara's foot on an object
// The line biting into the enemy: the climbing axe biting into a wall (keyframe 5, sound 1790 on the axe). v0.9.235's
// 1437 ("Axe_Swing_Throw_*" keyframe 18) is a hand grabbing: every ledge attach plays it on a hand.
constexpr const char* kAxeBiteSource = "wall_climb_axe_attach_right";
constexpr std::uint32_t kAxeBiteSound = 1790;
constexpr const char* kLandHardSource = "beam_idle_jump_vertical_land_hard";  // a camera shake (kf 4), sound 11489 (kf 0)
constexpr std::uint32_t kLandThudSound = 11489;
// The strike volume's reach (distance and height from Lara), also the impact's.
constexpr float kStrikeReach = 170.0f, kStrikeLow = -100.0f, kStrikeHigh = 200.0f;
constexpr ULONGLONG kRouteRestoreDelayMs = 600;      // T63 / T69 get their own targets back this long after a kick
constexpr ULONGLONG kMovesetWatchMs = 1500;          // after a kick: a moveset still "Cheat" goes back to "Combat"
constexpr ULONGLONG kRouteRetryMs = 2000;
constexpr int kLeftHand = 92, kRightHand = 117;      // Lara's wrist bones (local indices, the build's segments)
constexpr std::uint32_t kLaraLibraryKey = 11534;
constexpr const char* kLaraLibraryName = "characters\\laracroft\\laracroft";
constexpr const char* kKickEntryName = "Combat_Kick";
constexpr int kMaxFrames = 256;
constexpr float kBodyRadius = 25.0f;                 // how far the kick reaches into the target's body (cm)
constexpr float kChestHeight = 110.0f;               // where the lock-on aims on an enemy (cm above its feet)
constexpr int kAirArmTicks = 20;                     // an armed jump kick waits this long for a transition check
constexpr int kMaxNpcs = 512;

using GetLocalPlayerInstanceFn = void*(__fastcall*)(std::uint8_t);
using GetTargetInstanceFn = void*(__fastcall*)(void* combatManager, int senseType, bool);
using TryToGetFn = void*(__fastcall*)(void* instance);  // component getters (instance -> component or null)
using InputActionRequestedFn = bool(__fastcall*)(int controller, int action);
using IsEnemyOfFn = bool(__fastcall*)(void* npcComponent, void* instance);
using CameraVectorFn = float*(__fastcall*)(void* manager, float* out, std::uint32_t cameraId);
using SetAnimGraphGoalFn = bool(__fastcall*)(void* animComponent, int type, std::uint32_t value);
using SetInvincibleFn = void(__fastcall*)(void* health, int ms);
using CreateRopeEntityFn = void*(__fastcall*)(void* instance);
using SetRopeRenderDataFn = void(__fastcall*)(void* drawable, const float* start, const float* end, const void* renderData);
using InstanceQueryFn = void*(__fastcall*)(void* instance, int query);
using FxTriggerByIdFn = void(__fastcall*)(void* instance, void* zone, int fxId, int variant, const float* position, const void* fxa);
using TimeDilationRequestCtorFn = void*(__fastcall*)(void* request, int type, float world, float global, const char* name);
using TimeDilationRequestFn = void(__fastcall*)(void* manager, const void* request);
using TimeDilationCancelFn = void(__fastcall*)(void* manager, int type, const char* name);

#pragma pack(push, 1)
struct AnimEntry {
    std::int16_t animId;
    std::int16_t animNum;
    std::uint32_t pad;
    void* fx;
    const char* name;
};
#pragma pack(pop)
static_assert(sizeof(AnimEntry) == 0x18, "AnimEntry is 0x18 bytes");

// One loaded copy of the clip; its root / anchor tracks are rewritten for each kick.
struct Clip {
    std::uint8_t* data = nullptr;
    int id = -1;
    int frames = 0;
    float* rootY = nullptr;     // root (global bone 1) y translation, baked
    float* rootZ = nullptr;     // root z translation (baked in clips written since v0.9.229; else no height change)
    float* anchorY = nullptr;   // root-level helper g172: stays where the clip starts (back by the root travel)
    float* anchorZ = nullptr;
    float baseRootY[kMaxFrames] = {};
    float baseRootZ[kMaxFrames] = {};
    float baseAnchorY[kMaxFrames] = {};
    float baseAnchorZ[kMaxFrames] = {};
};

struct ClipInfo {
    int frames = 0;
    int zipStart = 0;
    int zipEnd = 0;
    int kick = 0;
    float reach = 110.0f;
    float travel = 500.0f;
    int ropeHand = kLeftHand;   // the wrist the rope leaves from
    int ropeShoot = 0;          // the rope leaves the hand ...
    int ropeAttach = 0;         // ... reaches the enemy ...
    int ropeRelease = 0;        // ... and lets go
};

enum class Reaction : int { Clearout, Pushed, Heavy, Stunned, Ballistic, Count };
constexpr const char* kReactionNames[] = {"Stumble back (melee clear-out)", "Pushed (melee push)", "Heavy melee hit",
                                          "Stunned", "Knocked flying (ballistic)"};
constexpr int kReactionHitType[] = {5, 11, 16, 12, 0};  // dtp::HitReactType (Ballistic uses the Ballistic effect)

// What Lara's primary state is doing, for the target and the jump kick.
enum StateClass : std::uint8_t { kClassOther = 0, kClassCombat = 1, kClassAir = 2, kClassGround = 3 };

std::uintptr_t g_base = 0;
bool g_available = false;
GetLocalPlayerInstanceFn g_getLocalPlayerInstance = nullptr;
GetTargetInstanceFn g_getTargetInstance = nullptr;
TryToGetFn g_tryToGetMeleeMove = nullptr;
TryToGetFn g_tryToGetNpc = nullptr;
TryToGetFn g_getAnimComponent = nullptr;
std::uint16_t* g_refCounts = nullptr;
void** g_animByNumber = nullptr;
std::uint8_t* const* g_registry = nullptr;
// Lock-on (optional: without them only the combat system's target is used, and no jump kick).
InputActionRequestedFn g_inputRequested = nullptr;
IsEnemyOfFn g_isEnemyOf = nullptr;
std::uint8_t* const* g_npcList = nullptr;
CameraVectorFn g_cameraForward = nullptr;
CameraVectorFn g_cameraPosition = nullptr;
void* const* g_cameraManager = nullptr;
bool g_lockOnAvailable = false;
bool g_dfaGlowAvailable = false;
SetAnimGraphGoalFn g_setAnimGraphGoal = nullptr;
TryToGetFn g_tryToGetHealth = nullptr;
SetInvincibleFn g_setInvincible = nullptr;
CreateRopeEntityFn g_createRopeEntity = nullptr;
SetRopeRenderDataFn g_setRopeRenderData = nullptr;
const std::uint32_t* g_grappleComponentId = nullptr;
bool g_gameRopeAvailable = false;
void* g_ropeEntity = nullptr;
bool g_ropeLogged = false;
InstanceQueryFn g_instanceQuery = nullptr;
std::uint8_t* g_instanceSlots = nullptr;
const std::uint8_t* g_grappleComp = nullptr;     // the GrappleComponent the instance scan found, its instance and handle
const std::uint8_t* g_grappleCompInstance = nullptr;
std::uint64_t g_grappleCompHandle = 0;
ULONGLONG g_grappleScanMs = 0;
int g_ropeLookLogs = 0;
bool g_ropeLookFailLogged = false;
bool g_axeRopeAvailable = false;
// The kick clip's clock (see KickClipFrame) and its fragment's play speed.
std::size_t g_graphRootOffset = 0;
bool g_fragmentReadable = false;
float g_clipClock = 0.0f;                 // frames of the clip played, at the clip's speed
ULONGLONG g_clipClockMs = 0;
std::uint8_t* g_kickFragmentDtp = nullptr;  // the Combat_Kick fragment's dtp::FragmentNode (writable)
float g_kickPlaySpeed = 1.0f;             // its m_playspeed now
float g_kickPlaySpeedGame = 1.0f;         // the game's
float g_kickSpeedMin = 1.0f;              // the slowest clip speed during a kick (for the log)
int g_clockLogs = 0;
bool g_kickUsedGameRope = false;          // the game's rope was out in this kick: no overlay line
bool g_kickStateSeen = false;
int g_kickStateGone = 0;
ULONGLONG g_lastKickEndMs = 0;
ULONGLONG g_invincibleRefreshMs = 0;
bool g_attachDone = false;
float g_moveWorldSpeed = 1.0f, g_moveGlobalSpeed = 1.0f;
// The impact.
FxTriggerByIdFn g_fxTriggerById = nullptr;
TimeDilationRequestCtorFn g_dilationCtor = nullptr;
TimeDilationRequestFn g_dilationRequest = nullptr;
TimeDilationCancelFn g_dilationCancel = nullptr;
std::uint8_t* g_timeDilationManager = nullptr;
bool g_impactDone = false;                // this kick reached its kick frame
ULONGLONG g_hitStopUntilMs = 0;           // our hit-stop request is out until then
float g_hitStopStart = 0.0f;              // its start time in the slot (another request there has its own)
int g_hitStopPrevType = 10;               // the type the manager followed before it
std::uint64_t g_hitStopReleaseTick = 0;   // our normal-speed request (the way back) is cancelled at this tick
float g_hitStopReleaseStart = 0.0f;
int g_hitStopLogs = 0;
bool g_healthReadable = false;
std::uint64_t g_healthCheckTick = 0;      // the target's health is logged at this tick (after the kick frame)
int g_healthLogs = 0;
int g_impactLogs = 0;
const std::uint8_t* g_connectFxDtp = nullptr;  // the enemy melee data the choice below belongs to
int g_connectFx = -1;
int g_connectFxLogs = 0;

// Settings ([GrappleKick] in ROTTRFramework.ini).
constexpr int kSettingsVersion = 4;       // 2 (v0.9.229): MaxHeight 150 -> 600 (the zip goes up and down now);
                                          // 3 (v0.9.230): the jump kick without aiming, Black Widow's clip;
                                          // 4 (v0.9.234): Black Widow's 15688 instead of 2188
std::atomic_bool g_enabled{false};   // (v1.0.0: off by default)
std::atomic<int> g_minRange{400};
std::atomic<int> g_maxRange{1500};
std::atomic<int> g_maxHeight{600};
std::atomic<int> g_damage{100};
std::atomic_bool g_lethal{false};
std::atomic<int> g_reaction{static_cast<int>(Reaction::Stunned)};
std::atomic<int> g_turnRate{1440};
std::atomic<int> g_margin{0};
std::atomic_bool g_lockOn{true};
std::atomic<int> g_lockCone{20};
std::atomic_bool g_glow{true};
std::atomic_bool g_airKick{true};
std::atomic_bool g_airNeedsAim{false};
std::atomic_bool g_rope{true};
std::atomic_bool g_invincible{true};
std::atomic_bool g_sounds{true};
std::atomic_bool g_impact{true};
std::atomic<int> g_hitStopMs{100};
std::atomic_bool g_fxRebuild{false};      // the kick's fx list is built again (the impact option changed)
std::atomic<int> g_airMinRange{200};
char g_clipFile[64] = "grapple_bw15688.tr10anim";
constexpr const char* kDefaultClip = "grapple_bw15688.tr10anim";
constexpr const char* kClip2188 = "grapple_bw2188.tr10anim";
constexpr const char* kTestClip = "grapple_kick_test.tr10anim";
std::atomic_bool g_patchDirty{false};
std::atomic_bool g_reloadRequested{false};

// State control (game thread).
void* g_agent = nullptr;                  // Lara's primary agent the lookups below belong to
bool g_scReady = false;
std::uint8_t* g_kickState = nullptr;
int g_kickIndex = -1;
std::uint8_t* g_entryTransition = nullptr;
const std::uint8_t* g_groundEntryTransition = nullptr;
const std::uint8_t* g_exitTransition = nullptr;
alignas(16) std::uint8_t g_movesetProp[kPropSize] = {};  // the fly cheat's moveset goal "Cheat"
bool g_haveMovesetProp = false;
std::uint32_t g_cheatMoveset = 0;         // its goal value (the property's hash)
const std::uint8_t* g_cheatState = nullptr;
// The graph route (graph 267229 "Playable"): T63 (S0 -> S22) and T69 (S23 -> S5), patched while a kick runs: their DTP
// Edges (the target byte) and runtime transitions (mTo), and the state nodes they point at.
struct Route {
    std::uint8_t* t63 = nullptr;
    std::uint8_t* t69 = nullptr;
    std::uint8_t* rt63 = nullptr;
    std::uint8_t* rt69 = nullptr;
    void* nodeDefault = nullptr;   // S0
    void* nodeWeapon = nullptr;    // S5
    void* nodeCheat = nullptr;     // S22
    void* nodeKick = nullptr;      // S23
};
Route g_route;
bool g_routeReady = false;
bool g_routePatched = false;
bool g_routeLookupDue = true;
bool g_routeLogged = false;
ULONGLONG g_routeCheckMs = 0;
ULONGLONG g_routeRestoreMs = 0;
ULONGLONG g_movesetWatchUntilMs = 0;
std::vector<std::pair<const std::uint8_t*, std::uint8_t>> g_stateClasses;  // sorted by state
std::vector<std::pair<const std::uint8_t*, const char*>> g_stateNames;

// combat_melee_kick's property array: the game's plus ours (the state points at ours while the kick is on).
alignas(16) std::uint8_t g_kickProps[kMaxKickProps * kPropSize] = {};
int g_kickPropCount = 0;
std::uint8_t* g_kickPropsOriginal = nullptr;
std::uint16_t g_kickPropCountOriginal = 0;
std::uint8_t* g_terrainProp = nullptr;    // our Physics_NoTerrainCollision entry (its value is set per kick)
bool g_kickPropsOn = false;
int g_kickPropsApplied = 0;               // the count the state has now (the moveset goal is the last entry)

// Clips.
Clip g_clips[2];
ClipInfo g_info;
int g_nextClip = 0;
bool g_clipsLoaded = false;
bool g_clipsFailed = false;

// Library entry "Combat_Kick".
int g_entryIndex = -1;
AnimEntry g_entryOriginal{};
bool g_entryReady = false;
bool g_entryWritten = false;
// The kick's fx list (built for the library it copies from; never freed: a graph node may still hold it).
void* g_kickFx = nullptr;
const std::uint8_t* g_kickFxLibrary = nullptr;
bool g_invincibleLogged = false;
// The game's DFA highlight on the target: the NpcComponent whose flag bit we set and its instance's handle.
std::uint8_t* g_glowNpc = nullptr;
std::uint64_t g_glowHandle = 0;
bool g_glowLogged = false;

// Melee move #4.
std::uint8_t* g_moveDtpRoot = nullptr;
std::uint8_t* g_move = nullptr;
std::uint8_t g_moveOriginal[kMoveSize] = {};
bool g_movePatched = false;

// Target (written by the tick, read by the hooks on the game thread).
void* g_lara = nullptr;
std::uint64_t g_tick = 0;
void* g_target = nullptr;
std::uint64_t g_targetHandle = 0;
std::uint64_t g_targetTick = 0;
float g_targetDist = 0.0f;
float g_targetDz = 0.0f;
float g_targetAngle = -1.0f;
const char* g_targetSource = "";
bool g_targetInRange = false;             // the combat stance's rule (MinRange..MaxRange)
bool g_targetInRangeAir = false;          // the jump's rule (AirMinRange..MaxRange)
StateClass g_laraClass = kClassOther;
const std::uint8_t* g_laraState = nullptr;
bool g_aimHeld = false;

// Jump kick: armed by the tick, let through by ForceTransition, started by RedirectTransition.
std::atomic_bool g_airArmed{false};
std::uint64_t g_airArmTick = 0;
std::atomic<const std::uint8_t*> g_forcedTransition{nullptr};
bool g_meleeHeld = false;
int g_airPressLogs = 0;
// Melee while aiming: armed the same way, in the combat stance or on foot.
std::atomic_bool g_aimArmed{false};
std::uint64_t g_aimArmTick = 0;
std::atomic_bool g_forcedFromAir{true};   // the transition ForceTransition let through was for a jump kick
int g_aimPressLogs = 0;

// Overlay: what the tick wants drawn (the render thread draws it inside the ImGui frame).
struct OverlayFrame {
    bool marker = false;       // the arrow over the target (and the game's highlight on it)
    bool markerKick = false;   // red while the kick runs, amber when a press would start one
    float target[3] = {};      // the target's feet
    bool rope = false;
    float ropeFrom[3] = {};    // the rope hand
    float ropeTo[3] = {};      // the target's chest
    float ropeExtend = 0.0f;   // how far the rope has flown (0..1 of the way)
    float ropeAlpha = 0.0f;
};
std::mutex g_overlayMutex;
OverlayFrame g_overlay;
std::atomic_bool g_overlayOn{false};
bool g_overlayCameraOk = true;
// Lara's wrists from the bone build (any thread): left, right.
std::atomic<float> g_handPos[2][3];
std::atomic<ULONGLONG> g_handMs{0};
bool g_handChecked = false;
bool g_handWorld = false;

// The running kick.
std::atomic_bool g_moveActive{false};
void* g_moveTarget = nullptr;
std::uint64_t g_moveHandle = 0;
ULONGLONG g_moveStartMs = 0;
float g_moveDist = 0.0f;
float g_moveTravel = 0.0f;
float g_moveRise = 0.0f;
bool g_moveFromAir = false;
float g_moveStartPos[3] = {};
std::uint8_t* g_moveAnim = nullptr;       // Lara's AnimComponentV2 whose motion source the kick switched
std::int32_t g_moveMotionBefore = -1;     // its motion type before (restored at the end if still ours)

// Status for the UI and the log.
char g_status[160] = "not started";
char g_targetStatus[200] = "no target";
char g_lastKick[240] = "none yet";
std::atomic<int> g_kicks{0};
std::atomic<int> g_airKicks{0};
ULONGLONG g_nextTargetLogMs = 0;
void* g_loggedTarget = nullptr;

void SetText(char* out, std::size_t size, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(out, size, fmt, args);
    va_end(args);
}

template <typename T>
T At(std::uintptr_t rva) {
    return reinterpret_cast<T>(g_base + rva);
}

std::uintptr_t RelTarget(std::uintptr_t rva, std::size_t operandOffset, std::size_t length) {
    const std::int32_t rel = *reinterpret_cast<const std::int32_t*>(g_base + rva + operandOffset);
    return static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rva + length) + rel);
}

bool Verify(std::uintptr_t rva, const std::uint8_t* sig, std::size_t size, const char* name) {
    if (GameBuild::VerifyBytes(rva, sig, size)) return true;
    Log::Writef("Grapple: %s signature mismatch; grapple kick unavailable", name);
    return false;
}

bool VerifyQuiet(std::uintptr_t rva, const std::uint8_t* sig, std::size_t size, const char* name) {
    if (GameBuild::VerifyBytes(rva, sig, size)) return true;
    Log::Writef("Grapple: %s signature mismatch; lock-on and the jump kick unavailable", name);
    return false;
}

// ---------- settings ----------
const char* GameDir() {
    static char dir[MAX_PATH]{};
    if (dir[0]) return dir;
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    char* slash = dir;
    for (char* p = dir; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p + 1;
    *slash = '\0';
    return dir;
}

const char* IniPath() {
    static char path[MAX_PATH]{};
    if (!path[0]) std::snprintf(path, sizeof(path), "%sROTTRFramework.ini", GameDir());
    return path;
}

constexpr const char* kIniSection = "GrappleKick";

int IniInt(const char* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntA(kIniSection, key, fallback, IniPath()));
}

void IniWriteInt(const char* key, int value) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%d", value);
    WritePrivateProfileStringA(kIniSection, key, buf, IniPath());
}

void SaveSettings() {
    IniWriteInt("Version", kSettingsVersion);
    IniWriteInt("Enabled", g_enabled.load() ? 1 : 0);
    IniWriteInt("MinRange", g_minRange.load());
    IniWriteInt("MaxRange", g_maxRange.load());
    IniWriteInt("MaxHeight", g_maxHeight.load());
    IniWriteInt("Damage", g_damage.load());
    IniWriteInt("Lethal", g_lethal.load() ? 1 : 0);
    IniWriteInt("Reaction", g_reaction.load());
    IniWriteInt("TurnRate", g_turnRate.load());
    IniWriteInt("StopMargin", g_margin.load());
    IniWriteInt("LockOn", g_lockOn.load() ? 1 : 0);
    IniWriteInt("LockCone", g_lockCone.load());
    IniWriteInt("Glow", g_glow.load() ? 1 : 0);
    IniWriteInt("AirKick", g_airKick.load() ? 1 : 0);
    IniWriteInt("AirNeedsAim", g_airNeedsAim.load() ? 1 : 0);
    IniWriteInt("AirMinRange", g_airMinRange.load());
    IniWriteInt("Rope", g_rope.load() ? 1 : 0);
    IniWriteInt("Invincible", g_invincible.load() ? 1 : 0);
    IniWriteInt("Sounds", g_sounds.load() ? 1 : 0);
    IniWriteInt("Impact", g_impact.load() ? 1 : 0);
    IniWriteInt("HitStopMs", g_hitStopMs.load());
    WritePrivateProfileStringA(kIniSection, "Clip", g_clipFile, IniPath());
}

void LoadSettings() {
    const bool hasSection = GetPrivateProfileIntA(kIniSection, "Enabled", -1, IniPath()) != static_cast<UINT>(-1);
    const int version = IniInt("Version", hasSection ? 1 : kSettingsVersion);
    g_enabled.store(IniInt("Enabled", 0) != 0);   // (v1.0.0: default off)
    g_minRange.store(std::clamp(IniInt("MinRange", 400), 150, 3000));
    g_maxRange.store(std::clamp(IniInt("MaxRange", 1500), g_minRange.load() + 50, 4000));
    int maxHeight = std::clamp(IniInt("MaxHeight", 600), 20, 1500);
    if (version < 2) maxHeight = std::max(maxHeight, 600);  // written as 150 while the zip could only go level
    g_maxHeight.store(maxHeight);
    g_damage.store(std::clamp(IniInt("Damage", 100), 0, 10000));
    g_lethal.store(IniInt("Lethal", 0) != 0);
    const int reaction = IniInt("Reaction", static_cast<int>(Reaction::Stunned));
    g_reaction.store(reaction >= 0 && reaction < static_cast<int>(Reaction::Count) ? reaction : static_cast<int>(Reaction::Stunned));
    g_turnRate.store(std::clamp(IniInt("TurnRate", 1440), 180, 5000));
    g_margin.store(std::clamp(IniInt("StopMargin", 0), -100, 200));
    g_lockOn.store(IniInt("LockOn", 1) != 0);
    g_lockCone.store(std::clamp(IniInt("LockCone", 20), 3, 60));
    g_glow.store(IniInt("Glow", 1) != 0);
    g_airKick.store(IniInt("AirKick", 1) != 0);
    g_airNeedsAim.store(IniInt("AirNeedsAim", 0) != 0);
    g_airMinRange.store(std::clamp(IniInt("AirMinRange", 200), 100, 3000));
    g_rope.store(IniInt("Rope", 1) != 0);
    g_invincible.store(IniInt("Invincible", 1) != 0);
    g_sounds.store(IniInt("Sounds", 1) != 0);
    g_impact.store(IniInt("Impact", 1) != 0);
    g_hitStopMs.store(std::clamp(IniInt("HitStopMs", 100), 0, 300));
    GetPrivateProfileStringA(kIniSection, "Clip", kDefaultClip, g_clipFile, sizeof(g_clipFile), IniPath());
    if (version < 3) {
        g_airNeedsAim.store(false);  // the jump kick no longer needs the weapon stance
        if (_stricmp(g_clipFile, kTestClip) == 0) strcpy_s(g_clipFile, kDefaultClip);
    }
    if (version < 4 && _stricmp(g_clipFile, kClip2188) == 0) strcpy_s(g_clipFile, kDefaultClip);  // 2188 flies back first
    if (version < kSettingsVersion) {
        SaveSettings();
        Log::Detailf("Grapple: settings from version %d updated (max height %d cm, jump kick %s, clip %s)", version,
                    g_maxHeight.load(), g_airNeedsAim.load() ? "with aim" : "without aim", g_clipFile);
    }
}

bool MakeWritable(void* address, std::size_t size) {
    return MemoryProbe::MakeWritable(address, size);   // (VirtualQuery on the heap: milliseconds, so its answers are kept)
}

bool NameIs(const char* text, const char* expected) {
    if (!text) return false;
    for (std::size_t i = 0;; ++i) {
        if (text[i] != expected[i]) return false;
        if (!expected[i]) return true;
        if (i > 256) return false;
    }
}

std::uint8_t* InstanceComponent(std::uint8_t* instance, std::uint32_t id) {
    const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(instance + kInstanceComponentCount);
    auto** components = *reinterpret_cast<std::uint8_t***>(instance + kInstanceComponents);
    if (!components || id >= count) return nullptr;
    return components[id];
}

// ---------- state control ----------
StateClass ClassOf(const std::uint8_t* state) {
    const auto it = std::lower_bound(g_stateClasses.begin(), g_stateClasses.end(), std::make_pair(state, std::uint8_t{0}),
                                     [](const auto& a, const auto& b) { return a.first < b.first; });
    return it != g_stateClasses.end() && it->first == state ? static_cast<StateClass>(it->second) : kClassOther;
}

const char* NameOf(const std::uint8_t* state) {
    const auto it = std::lower_bound(g_stateNames.begin(), g_stateNames.end(), std::make_pair(state, static_cast<const char*>(nullptr)),
                                     [](const auto& a, const auto& b) { return a.first < b.first; });
    return it != g_stateNames.end() && it->first == state && it->second ? it->second : "?";
}

// Combat: under Combat_Parent (the weapon stance). Air: the jump and fall states Lara can be in freely (not the
// ledge / beam / zipline pre-attaches, the rescue catches, the axe smash or being thrown).
void ClassifyStatesUnsafe(const std::uint8_t* const* states, std::uint16_t numStates) {
    g_stateClasses.clear();
    g_stateNames.clear();
    g_stateClasses.reserve(numStates);
    g_stateNames.reserve(numStates);
    int combat = 0, air = 0, ground = 0;
    for (std::uint16_t i = 0; i < numStates; ++i) {
        const std::uint8_t* state = states[i];
        if (!state) continue;
        const char* name = *reinterpret_cast<const char* const*>(state + kStateName);
        g_stateNames.emplace_back(state, name);
        bool underCombat = false, underAir = false, underGround = false;
        const std::uint8_t* s = state;
        for (int depth = 0; s && depth < 32; ++depth) {
            const char* n = *reinterpret_cast<const char* const*>(s + kStateName);
            if (NameIs(n, "Combat_Parent")) underCombat = true;
            if (NameIs(n, "jump_trans_parent") || NameIs(n, "fall_parent")) underAir = true;
            if (NameIs(n, "ground_parent")) underGround = true;
            const std::uint16_t parent = *reinterpret_cast<const std::uint16_t*>(s + kStateParent);
            s = parent < numStates ? states[parent] : nullptr;
        }
        if (underAir && name && (std::strstr(name, "attach") || std::strstr(name, "rescue") || std::strstr(name, "smash") ||
                                 std::strstr(name, "thrown")))
            underAir = false;
        const std::uint8_t cls = underCombat ? kClassCombat : underAir ? kClassAir : underGround ? kClassGround : kClassOther;
        if (cls == kClassCombat) ++combat;
        if (cls == kClassAir) ++air;
        if (cls == kClassGround) ++ground;
        g_stateClasses.emplace_back(state, cls);
    }
    std::sort(g_stateClasses.begin(), g_stateClasses.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::sort(g_stateNames.begin(), g_stateNames.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    Log::DetailOncef("Grapple: %d combat stance states, %d jump / fall states, %d on-foot states", combat, air, ground);
}

// combat_melee_kick's properties with ours: no edge stop, no gravity, no fall message, terrain collision per kick.
bool BuildKickPropsUnsafe() {
    const std::uint16_t count = *reinterpret_cast<const std::uint16_t*>(g_kickState + kStatePropCount);
    auto* list = *reinterpret_cast<std::uint8_t* const*>(g_kickState + kStateProps);
    if (!list || count == 0 || count + 6 > kMaxKickProps) return false;
    // Already ours (a second lookup of the same data): keep the game's originals from the first one.
    if (list == g_kickProps) return g_kickPropsOriginal != nullptr;
    g_kickPropsOriginal = list;
    g_kickPropCountOriginal = count;
    std::memcpy(g_kickProps, list, count * kPropSize);
    const std::uint8_t* booleanTemplate = nullptr;
    for (int k = 0; k < count; ++k)
        if (*reinterpret_cast<const std::int16_t*>(g_kickProps + k * kPropSize + 2) == kPropTypeBoolean)
            booleanTemplate = list + k * kPropSize;
    if (!booleanTemplate) return false;
    int n = count;
    auto set = [&](std::int16_t property, std::uint8_t value) -> std::uint8_t* {
        for (int k = 0; k < n; ++k) {
            std::uint8_t* p = g_kickProps + k * kPropSize;
            if (*reinterpret_cast<const std::int16_t*>(p) == property) {
                *reinterpret_cast<std::int16_t*>(p + 2) = kPropTypeBoolean;
                std::memset(p + kPropValue, 0, 0x10);
                p[kPropValue] = value;
                return p;
            }
        }
        std::uint8_t* p = g_kickProps + n * kPropSize;
        std::memcpy(p, booleanTemplate, kPropSize);
        *reinterpret_cast<std::int16_t*>(p) = property;
        std::memset(p + kPropValue, 0, 0x10);
        p[kPropValue] = value;
        ++n;
        return p;
    };
    set(kPropStopAtSafePoint, 0);
    set(kPropCheckEdges, 0);
    set(kPropNoGravity, 1);
    set(kPropIgnoreFallMessage, 1);
    g_terrainProp = set(kPropNoTerrainCollision, 0) + kPropValue;
    // The moveset goal goes last: without the graph route the state leaves it out (count - 1).
    if (g_haveMovesetProp) {
        for (int k = 0; k < n; ++k) {
            if (*reinterpret_cast<const std::int16_t*>(g_kickProps + k * kPropSize) != kPropGoalMoveset) continue;
            std::memmove(g_kickProps + k * kPropSize, g_kickProps + (k + 1) * kPropSize, (n - k - 1) * kPropSize);
            --n;
            break;
        }
        std::memcpy(g_kickProps + n * kPropSize, g_movesetProp, kPropSize);
        ++n;
        g_terrainProp = nullptr;  // (an entry may have moved up)
        for (int k = 0; k < n; ++k)
            if (*reinterpret_cast<const std::int16_t*>(g_kickProps + k * kPropSize) == kPropNoTerrainCollision)
                g_terrainProp = g_kickProps + k * kPropSize + kPropValue;
    }
    g_kickPropCount = n;
    return true;
}

void ApplyKickPropsUnsafe(bool on) {
    if (!g_kickState || !g_kickPropsOriginal) return;
    // Without the graph route the moveset goal (last) stays out: "Cheat" would play the fly cheat's graph state.
    const int want = on ? g_kickPropCount - (g_haveMovesetProp && !g_routeReady ? 1 : 0) : g_kickPropCountOriginal;
    if (on == g_kickPropsOn && want == g_kickPropsApplied) return;
    if (!MakeWritable(g_kickState, 0x40)) return;
    auto* count = reinterpret_cast<volatile std::uint16_t*>(g_kickState + kStatePropCount);
    auto* list = reinterpret_cast<std::uint8_t* volatile*>(g_kickState + kStateProps);
    if (on) {
        *list = g_kickProps;  // a superset: the old count still reads valid entries
        *count = static_cast<std::uint16_t>(want);
    } else {
        *count = g_kickPropCountOriginal;
        *list = g_kickPropsOriginal;
    }
    g_kickPropsOn = on;
    g_kickPropsApplied = want;
    Log::DetailOncef("Grapple: %s's properties %s", kKickStateName,
                !on ? "restored"
                    : want == g_kickPropCount && g_haveMovesetProp
                          ? "extended (no edge stop, no gravity, no fall message, terrain collision per kick, moveset Cheat)"
                          : "extended (no edge stop, no gravity, no fall message, terrain collision per kick; weapon stance route)");
}

// Finds combat_melee_kick and the two transitions in Lara's primary agent data and checks what the redirect relies on.
bool FindScUnsafe(void* agent) {
    const auto* data = *reinterpret_cast<const std::uint8_t* const*>(static_cast<std::uint8_t*>(agent) + kAgentData);
    if (!data) return false;
    const std::uint16_t numStates = *reinterpret_cast<const std::uint16_t*>(data + kAgentDataNumStates);
    const std::uint16_t numTransitions = *reinterpret_cast<const std::uint16_t*>(data + kAgentDataNumTransitions);
    auto* const* states = *reinterpret_cast<std::uint8_t* const* const*>(data + kAgentDataStateRefs);
    auto* const* transitions = *reinterpret_cast<std::uint8_t* const* const*>(data + kAgentDataTransitionRefs);
    if (!states || !transitions) return false;
    g_kickState = nullptr;
    g_kickIndex = -1;
    for (std::uint16_t i = 0; i < numStates; ++i) {
        std::uint8_t* state = states[i];
        if (state && NameIs(*reinterpret_cast<const char* const*>(state + kStateName), kKickStateName)) {
            g_kickState = state;
            g_kickIndex = i;
            break;
        }
    }
    g_entryTransition = nullptr;
    g_groundEntryTransition = nullptr;
    g_exitTransition = nullptr;
    for (std::uint16_t i = 0; i < numTransitions; ++i) {
        std::uint8_t* transition = transitions[i];
        if (!transition) continue;
        const char* name = *reinterpret_cast<const char* const*>(transition + kTransitionName);
        if (NameIs(name, kEntryTransitionName)) g_entryTransition = transition;
        else if (NameIs(name, kGroundEntryTransitionName)) g_groundEntryTransition = transition;
        else if (NameIs(name, kExitTransitionName)) g_exitTransition = transition;
    }
    // The fly cheat's moveset goal "Cheat" (the string and its hash as the data has them).
    g_haveMovesetProp = false;
    g_cheatState = nullptr;
    for (std::uint16_t i = 0; i < numStates && !g_haveMovesetProp; ++i) {
        const std::uint8_t* state = states[i];
        if (!state || !NameIs(*reinterpret_cast<const char* const*>(state + kStateName), kCheatStateName)) continue;
        g_cheatState = state;
        const std::uint16_t n = *reinterpret_cast<const std::uint16_t*>(state + kStatePropCount);
        const auto* props = *reinterpret_cast<const std::uint8_t* const*>(state + kStateProps);
        for (std::uint16_t k = 0; props && k < n; ++k) {
            const std::uint8_t* prop = props + k * kPropSize;
            if (*reinterpret_cast<const std::int16_t*>(prop) != kPropGoalMoveset) continue;
            std::memcpy(g_movesetProp, prop, kPropSize);
            g_haveMovesetProp = NameIs(*reinterpret_cast<const char* const*>(prop + 8), "Cheat");
            g_cheatMoveset = *reinterpret_cast<const std::uint32_t*>(prop + 0x10);
            break;
        }
    }
    if (!g_haveMovesetProp) Log::Write("Grapple: the cheat state's moveset goal not found; the kick plays only from the weapon stance");
    if (!g_kickState || !g_entryTransition || !g_exitTransition) {
        Log::Writef("Grapple: state control lookup failed (state %s, entry %s, exit %s)", g_kickState ? "ok" : "missing",
                    g_entryTransition ? "ok" : "missing", g_exitTransition ? "ok" : "missing");
        return false;
    }
    // The state must still be the cut kick: anim cluster "Combat_Kick" and melee move #4.
    bool cluster = false, move = false;
    const std::uint16_t props = *reinterpret_cast<const std::uint16_t*>(g_kickState + kStatePropCount);
    const auto* list = *reinterpret_cast<const std::uint8_t* const*>(g_kickState + kStateProps);
    for (std::uint16_t k = 0; list && k < props; ++k) {
        const std::uint8_t* prop = list + k * kPropSize;
        const std::int16_t type = *reinterpret_cast<const std::int16_t*>(prop + 2);
        if (type == kPropTypeGoalCluster && NameIs(*reinterpret_cast<const char* const*>(prop + 8), kKickEntryName)) cluster = true;
        if (type == kPropTypeMeleeMove && *reinterpret_cast<const std::uint32_t*>(prop + 8) == kKickMoveIndex) move = true;
    }
    const std::uint16_t entryTarget = *reinterpret_cast<const std::uint16_t*>(g_entryTransition + kTransitionTarget);
    const std::uint16_t exitTarget = *reinterpret_cast<const std::uint16_t*>(g_exitTransition + kTransitionTarget);
    if (!cluster || !move) {
        Log::Detailf("Grapple: [%d] %s is not the cut kick any more (cluster %s, melee move %s)", g_kickIndex, kKickStateName,
                    cluster ? "ok" : "different", move ? "ok" : "different");
        return false;
    }
    if (!MakeWritable(g_entryTransition + kTransitionTarget, 2)) {
        Log::Write("Grapple: the melee transition cannot be made writable");
        return false;
    }
    g_kickPropsOn = false;
    if (!BuildKickPropsUnsafe()) Log::WriteOncef("Grapple: combat_melee_kick's properties could not be extended; the zip stays level");
    ClassifyStatesUnsafe(states, numStates);
    Log::DetailOncef("Grapple: state control ready: [%d] %s, melee press '%s' (to [%u])%s, exit '%s' (to [%u])", g_kickIndex,
                kKickStateName, kEntryTransitionName, entryTarget,
                g_groundEntryTransition ? " and the on-foot one" : " (no on-foot melee transition)", kExitTransitionName, exitTarget);
    return true;
}

bool FindSc(void* agent) {
#if defined(_MSC_VER)
    __try {
#endif
        return FindScUnsafe(agent);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

// ---------- clip ----------
bool ReadClipInfo(const char* clipFile, ClipInfo& info) {
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%sROTTRFramework\\grapple\\%s", GameDir(), clipFile);
    char* dot = std::strrchr(path, '.');
    if (dot && _stricmp(dot, ".tr10anim") == 0) *dot = '\0';
    strcat_s(path, sizeof(path), ".grapple.ini");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        Log::Writef("Grapple: %s not found", path);
        return false;
    }
    char buf[32]{};
    auto get = [&](const char* key, float fallback) {
        char def[32];
        std::snprintf(def, sizeof(def), "%g", fallback);
        GetPrivateProfileStringA("Clip", key, def, buf, sizeof(buf), path);
        return static_cast<float>(std::atof(buf));
    };
    info.frames = static_cast<int>(get("frames", 0));
    info.zipStart = static_cast<int>(get("zipStart", 0));
    info.zipEnd = static_cast<int>(get("zipEnd", 0));
    info.kick = static_cast<int>(get("kick", 0));
    info.reach = get("reach", 110.0f);
    info.travel = get("travel", 500.0f);
    char hand[16]{};
    GetPrivateProfileStringA("Clip", "ropeHand", "left", hand, sizeof(hand), path);
    info.ropeHand = _stricmp(hand, "right") == 0 ? kRightHand : kLeftHand;
    info.ropeShoot = static_cast<int>(get("ropeShoot", static_cast<float>(std::max(0, info.zipStart - 3))));
    info.ropeAttach = static_cast<int>(get("ropeAttach", static_cast<float>(info.zipStart)));
    info.ropeRelease = static_cast<int>(get("ropeRelease", static_cast<float>(info.zipEnd)));
    if (info.ropeAttach <= info.ropeShoot) info.ropeAttach = info.ropeShoot + 1;
    if (info.ropeRelease < info.ropeAttach) info.ropeRelease = info.ropeAttach;
    return info.frames > 0 && info.frames <= kMaxFrames && info.zipStart >= 0 && info.zipEnd > info.zipStart &&
           info.zipEnd < info.frames && info.kick > 0 && info.kick < info.frames && info.travel > 1.0f;
}

// A clip written by re\melee\laraglide.js (same resource layout as the acrobatics clips, see acro.cpp LoadClip).
bool LoadClipFile(const char* clipFile, Clip& clip) {
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%sROTTRFramework\\grapple\\%s", GameDir(), clipFile);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log::Writef("Grapple: clip file %s not found", path);
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    if (size.QuadPart < 0x100 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(h);
        Log::Writef("Grapple: clip file %s has an unexpected size (%lld bytes)", path, size.QuadPart);
        return false;
    }
    const auto fileSize = static_cast<DWORD>(size.QuadPart);
    auto* raw = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, fileSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    DWORD read = 0;
    const bool ok = raw && ReadFile(h, raw, fileSize, &read, nullptr) && read == fileSize;
    CloseHandle(h);
    if (!ok) {
        if (raw) VirtualFree(raw, 0, MEM_RELEASE);
        Log::Writef("Grapple: could not read %s", path);
        return false;
    }
    auto i32 = [&](std::size_t o) { return *reinterpret_cast<const std::int32_t*>(raw + o); };
    const int internalCount = i32(0);
    const std::size_t bodyOffset = 20 + static_cast<std::size_t>(internalCount) * 8;
    if (internalCount <= 0 || internalCount > 64 || i32(4) || i32(8) || i32(12) || i32(16) || bodyOffset + 0xA8 > fileSize) {
        VirtualFree(raw, 0, MEM_RELEASE);
        Log::Detailf("Grapple: %s is not a clip written by the converter", clipFile);
        return false;
    }
    const std::uint32_t bodySize = fileSize - static_cast<std::uint32_t>(bodyOffset);
    auto* body = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, bodySize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!body) {
        VirtualFree(raw, 0, MEM_RELEASE);
        return false;
    }
    std::memcpy(body, raw + bodyOffset, bodySize);
    bool refsOk = true;
    for (int i = 0; i < internalCount; ++i) {
        const std::int32_t location = i32(20 + i * 8), target = i32(24 + i * 8);
        if (location < 0 || target < 0 || static_cast<std::uint32_t>(location) + 8 > bodySize ||
            static_cast<std::uint32_t>(target) > bodySize) {
            refsOk = false;
            break;
        }
        *reinterpret_cast<std::uint8_t**>(body + location) = body + target;
    }
    VirtualFree(raw, 0, MEM_RELEASE);
    if (!refsOk || !*reinterpret_cast<void**>(body + 0x68) || !*reinterpret_cast<void**>(body + 0x70) ||
        !*reinterpret_cast<void**>(body + 0x78) || !*reinterpret_cast<void**>(body + 0x80)) {
        VirtualFree(body, 0, MEM_RELEASE);
        Log::Writef("Grapple: %s has bad pointer data", clipFile);
        return false;
    }
    clip.data = body;
    clip.frames = *reinterpret_cast<const std::uint16_t*>(body + 0x32);
    return true;
}

// Channel walk of a converted clip (see acro.cpp ScaleRootTravelUnsafe): the root's (global id 1) and the anchor's
// (global id 172) baked y and z translation (channels 5 and 6).
bool FindTracksUnsafe(Clip& clip) {
    std::uint8_t* body = clip.data;
    const auto* ids = *reinterpret_cast<const std::uint16_t* const*>(body + 0x68);
    const auto* flags = *reinterpret_cast<const std::uint8_t* const*>(body + 0x70);
    const auto* meta = *reinterpret_cast<const std::uint8_t* const*>(body + 0x78);
    auto* values = *reinterpret_cast<std::uint8_t* const*>(body + 0x80);
    const int bones = body[0x37], frames = clip.frames;
    const std::uint8_t* fp = flags + 2;
    const std::uint8_t* mp = meta;
    auto* vp = reinterpret_cast<float*>(values + 4);
    clip.rootY = clip.rootZ = clip.anchorY = clip.anchorZ = nullptr;
    for (int b = 0; b < bones; ++b) {
        std::uint32_t fl = *fp++;
        if (fl & 0x80) fl = (fl & 0x7Fu) | (static_cast<std::uint32_t>(*fp++) << 8);
        for (int c = 0; c < 12; ++c) {
            if (!(fl & (1u << c))) continue;
            const bool baked = *reinterpret_cast<const std::uint16_t*>(mp) == 0;
            mp += baked ? 4 : 2;
            if (baked && ids[b] == 1 && c == 5) clip.rootY = vp;
            if (baked && ids[b] == 1 && c == 6) clip.rootZ = vp;
            if (baked && ids[b] == 172 && c == 5) clip.anchorY = vp;
            if (baked && ids[b] == 172 && c == 6) clip.anchorZ = vp;
            vp += baked ? frames : 1;
        }
    }
    if (!clip.rootY) return false;
    for (int f = 0; f < frames; ++f) {
        clip.baseRootY[f] = clip.rootY[f];
        clip.baseRootZ[f] = clip.rootZ ? clip.rootZ[f] : 0.0f;
        clip.baseAnchorY[f] = clip.anchorY ? clip.anchorY[f] : 0.0f;
        clip.baseAnchorZ[f] = clip.anchorZ ? clip.anchorZ[f] : 0.0f;
    }
    return true;
}

int RegisterClipUnsafe(Clip& clip, int searchFrom) {
    for (int id = searchFrom; id > 16000; --id) {
        if (g_refCounts[id] != 0 || g_animByNumber[id] != nullptr) continue;
        *reinterpret_cast<std::uint16_t*>(clip.data + 0x30) = static_cast<std::uint16_t>(id);
        g_animByNumber[id] = clip.data;
        MemoryBarrier();
        g_refCounts[id] = 1;
        return id;
    }
    return -1;
}

void LoadClipsGameThread() {
    ClipInfo info;
    if (!ReadClipInfo(g_clipFile, info)) {
        g_clipsFailed = true;
        SetText(g_status, sizeof(g_status), "clip info missing (ROTTRFramework\\grapple\\%s)", g_clipFile);
        return;
    }
    for (int k = 0; k < 2; ++k) {
        Clip& clip = g_clips[k];
        clip = Clip{};
        bool ok = LoadClipFile(g_clipFile, clip) && clip.frames == info.frames;
#if defined(_MSC_VER)
        __try {
#endif
            if (ok) ok = FindTracksUnsafe(clip);
            if (ok) clip.id = RegisterClipUnsafe(clip, 31500 - k);
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
#endif
        if (!ok || clip.id < 0) {
            g_clipsFailed = true;
            SetText(g_status, sizeof(g_status), "clip %s could not be loaded", g_clipFile);
            Log::Writef("Grapple: clip %s could not be loaded or registered (frames %d vs %d)", g_clipFile, clip.frames, info.frames);
            return;
        }
    }
    g_info = info;
    g_clipsLoaded = true;
    Log::Detailf("Grapple: clip %s loaded as ids %d / %d: %d frames, zip %d..%d (%.0f cm), kick at %d, reach %.0f cm, rope "
                "from the %s hand %d -> %d -> %d%s%s",
                g_clipFile, g_clips[0].id, g_clips[1].id, info.frames, info.zipStart, info.zipEnd, info.travel, info.kick,
                info.reach, info.ropeHand == kRightHand ? "right" : "left", info.ropeShoot, info.ropeAttach, info.ropeRelease,
                g_clips[0].rootZ ? "" : " (no root z track: level zips only)", g_clips[0].anchorY ? "" : " (no anchor track)");
}

// The zip's root path: travel cm forward and rise cm up (negative: down), plus an arc of height hump in between.
// Frames up to zipStart as in the clip, the zip reshaped, the rest shifted. The height follows the clip's own forward
// progress through the zip.
void SetZipPathUnsafe(Clip& clip, float travel, float rise, float hump) {
    const int frames = clip.frames, zs = g_info.zipStart, ze = g_info.zipEnd;
    const float nominal = clip.baseRootY[ze] - clip.baseRootY[zs];  // negative: Lara faces -Y
    const float k = std::fabs(nominal) > 1.0f ? travel / std::fabs(nominal) : 1.0f;
    float minY = 0.0f, maxY = 0.0f, minZ = 0.0f, maxZ = 0.0f;
    for (int f = 0; f < frames; ++f) {
        const float base = clip.baseRootY[f];
        float y = base;
        if (f > zs && f <= ze) y = clip.baseRootY[zs] + (base - clip.baseRootY[zs]) * k;
        else if (f > ze) y = base + (k - 1.0f) * nominal;
        clip.rootY[f] = y;
        if (clip.anchorY) clip.anchorY[f] = clip.baseAnchorY[f] - (y - base);  // stays where the clip starts
        float u = 0.0f;
        if (f > ze) u = 1.0f;
        else if (f > zs) u = std::fabs(nominal) > 1.0f ? (base - clip.baseRootY[zs]) / nominal : static_cast<float>(f - zs) / (ze - zs);
        u = std::clamp(u, 0.0f, 1.0f);
        const float dz = rise * u + hump * 4.0f * u * (1.0f - u);
        if (clip.rootZ) {
            clip.rootZ[f] = clip.baseRootZ[f] + dz;
            if (clip.anchorZ) clip.anchorZ[f] = clip.baseAnchorZ[f] - dz;
            minZ = std::min(minZ, clip.rootZ[f]);
            maxZ = std::max(maxZ, clip.rootZ[f]);
        }
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
    }
    auto* header = reinterpret_cast<float*>(clip.data);
    header[1] = minY - 150.0f;      // bounds min y
    header[4] = maxY + 150.0f;      // bounds max y
    header[7] = clip.rootY[frames - 1];  // root end translation y
    if (clip.rootZ) {
        header[2] = minZ - 20.0f;   // bounds min z
        header[5] = maxZ + 220.0f;  // bounds max z
        header[8] = clip.rootZ[frames - 1];
    }
}

// ---------- library entry ----------
std::uint8_t* FindLibraryUnsafe() {
    const std::uint8_t* map = *g_registry;
    if (!map) return nullptr;
    const auto* const* buckets = *reinterpret_cast<const std::uint8_t* const* const*>(map);
    const std::uint64_t bucketCount = *reinterpret_cast<const std::uint64_t*>(map + 0x18);
    if (!buckets || bucketCount == 0 || bucketCount > (1u << 20)) return nullptr;
    const std::uint8_t* node = buckets[static_cast<std::uint64_t>(kLaraLibraryKey) % bucketCount];
    for (int guard = 0; node && guard < 4096; ++guard) {
        if (*reinterpret_cast<const std::uint32_t*>(node + 0x08) == kLaraLibraryKey) {
            auto* library = *reinterpret_cast<std::uint8_t* const*>(node + 0x10);
            if (library && NameIs(*reinterpret_cast<const char* const*>(library + 0x20), kLaraLibraryName)) return library;
            return nullptr;
        }
        node = *reinterpret_cast<const std::uint8_t* const*>(node);
    }
    return nullptr;
}

// The library's current entry array (acro.cpp may publish its own extended copy at any time).
AnimEntry* CurrentEntryUnsafe(int index, const char* name) {
    std::uint8_t* library = FindLibraryUnsafe();
    if (!library || index < 0) return nullptr;
    const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(library + 0x10);
    auto* entries = *reinterpret_cast<AnimEntry* const*>(library + 0x18);
    if (!entries || static_cast<std::uint32_t>(index) >= count) return nullptr;
    AnimEntry* entry = &entries[index];
    return NameIs(entry->name, name) ? entry : nullptr;
}

AnimEntry* CurrentEntryUnsafe() { return CurrentEntryUnsafe(g_entryIndex, kKickEntryName); }

void EnsureEntryUnsafe() {
    if (g_entryReady) return;
    std::uint8_t* library = FindLibraryUnsafe();
    if (!library) return;
    const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(library + 0x10);
    const auto* entries = *reinterpret_cast<const AnimEntry* const*>(library + 0x18);
    if (!entries || count == 0 || count > 20000) return;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (NameIs(entries[i].name, kKickEntryName)) {
            g_entryIndex = static_cast<int>(i);
            g_entryOriginal = entries[i];
            g_entryReady = true;
            Log::Detailf("Grapple: library entry %u '%s' (anim %d)", i, kKickEntryName, entries[i].animId);
            return;
        }
    }
}

// animId + animNum as one 32-bit store, then the fx list (the graph may read the entry on another thread).
void WriteEntry(AnimEntry* entry, std::int16_t animId, std::int16_t animNum, void* fx) {
    if (!MakeWritable(entry, sizeof(AnimEntry))) return;
    const std::uint32_t both = static_cast<std::uint16_t>(animId) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(animNum)) << 16);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(entry), static_cast<LONG>(both));
    InterlockedExchangePointer(&entry->fx, fx);
}

void RestoreEntryUnsafe() {
    if (!g_entryWritten) return;
    if (AnimEntry* entry = CurrentEntryUnsafe())
        WriteEntry(entry, g_entryOriginal.animId, g_entryOriginal.animNum, g_entryOriginal.fx);
    g_entryWritten = false;
}

// ---------- kick sounds ----------
// cdc::AnimFxList (an AnimEntry's fx pointer; the format acro.cpp documents): { dtp::FXLib* effectLib; triggers }, each
// trigger { u16 info; i16 keyframe; u8 fxID; pad[3] } + payload, ended by info 0xFFFF; info bits 0-1 kind (0 sound),
// bit 2 disabled, bits 10-11 phase (0 keyframe-timed), bits 12-15 payload dwords n (size 8 + ((4n + 4) & ~7)). Sound
// payload: i8 type (0 sound plex, 1 / 3 vocal bark), ..., dtp::SoundPlex* at trigger +16.
struct AnimFxList {
    void* effectLib;
    std::uint8_t* triggers;
};
struct FxBuild {
    std::uint8_t buf[2048];
    std::size_t used;
    int count;
    std::uint32_t sounds[64];
    int frames[64];
};

// The sound id a sound plex ends in (Assignment / Envelope / Reaction / Effect wrap a Reference or a Derived one).
std::uint32_t SoundIdUnsafe(const std::uint8_t* plex) {
    const std::uint8_t* p = plex;
    for (int depth = 0; p && depth < 6; ++depth) {
        const std::uint16_t type = *reinterpret_cast<const std::uint16_t*>(p);
        const std::uint8_t* data = *reinterpret_cast<const std::uint8_t* const*>(p + 8);
        if (!data) break;
        if (type == 1 || type == 2) return *reinterpret_cast<const std::uint32_t*>(data);
        if (type != 6 && type != 7 && type != 11 && type != 14) break;
        p = *reinterpret_cast<const std::uint8_t* const*>(data);
    }
    return 0;
}

// Appends an fx list's sound triggers with keyframes kfFrom..kfTo, retimed to frame = base + keyframe; vocals: its
// barks too; onlySound: just that sound.
int AppendSoundsUnsafe(FxBuild& b, const AnimFxList* src, int kfFrom, int kfTo, int base, bool vocals, std::uint32_t onlySound) {
    if (!src || !src->triggers) return 0;
    int added = 0;
    const std::uint8_t* t = src->triggers;
    for (int n = 0; n < 64; ++n) {
        const std::uint16_t info = *reinterpret_cast<const std::uint16_t*>(t);
        if (info == 0xFFFF) break;
        const std::size_t size = 8 + ((4u * (info >> 12) + 4u) & ~7u);
        if (size > 64) break;
        const int kind = info & 3, phase = (info >> 10) & 3, keyframe = *reinterpret_cast<const std::int16_t*>(t + 2);
        const int soundType = size >= 16 ? static_cast<std::int8_t>(t[8]) : -1;
        const bool plex = soundType == 0 && size >= 24;
        const std::uint32_t id = plex ? SoundIdUnsafe(*reinterpret_cast<const std::uint8_t* const*>(t + 16)) : 0;
        bool take = kind == 0 && !(info & 4) && phase == 0 && keyframe >= kfFrom && keyframe <= kfTo &&
                    (plex || (vocals && (soundType == 1 || soundType == 3)));
        if (onlySound && id != onlySound) take = false;
        if (take) {
            const int frame = std::clamp(base + keyframe, 1, std::max(1, g_info.frames - 1));
            bool duplicate = false;
            for (int k = 0; id && k < b.count; ++k)
                if (b.sounds[k] == id && std::abs(b.frames[k] - frame) <= 2) duplicate = true;
            if (!duplicate && b.count < 64 && b.used + size + 8 <= sizeof(b.buf)) {
                std::uint8_t* out = b.buf + b.used;
                std::memcpy(out, t, size);
                *reinterpret_cast<std::int16_t*>(out + 2) = static_cast<std::int16_t>(frame);
                b.sounds[b.count] = id;
                b.frames[b.count] = frame;
                ++b.count;
                b.used += size;
                ++added;
            }
        }
        t += size;
    }
    return added;
}

// Appends an fx list's camera triggers (kind 3: a dtp::CameraTrigger, its type byte first; FXTrigger_HandleCamera
// sends type 17 to FXTrigger_HandleCameraShake) with keyframes kfFrom..kfTo, retimed to frame = base + keyframe.
int AppendCameraUnsafe(FxBuild& b, const AnimFxList* src, int kfFrom, int kfTo, int base) {
    if (!src || !src->triggers) return 0;
    int added = 0;
    const std::uint8_t* t = src->triggers;
    for (int n = 0; n < 64; ++n) {
        const std::uint16_t info = *reinterpret_cast<const std::uint16_t*>(t);
        if (info == 0xFFFF) break;
        const std::size_t size = 8 + ((4u * (info >> 12) + 4u) & ~7u);
        if (size > 64) break;
        const int kind = info & 3, phase = (info >> 10) & 3, keyframe = *reinterpret_cast<const std::int16_t*>(t + 2);
        if (kind == 3 && !(info & 4) && phase == 0 && keyframe >= kfFrom && keyframe <= kfTo && b.count < 64 &&
            b.used + size + 8 <= sizeof(b.buf)) {
            const int frame = std::clamp(base + keyframe, 1, std::max(1, g_info.frames - 1));
            std::uint8_t* out = b.buf + b.used;
            std::memcpy(out, t, size);
            *reinterpret_cast<std::int16_t*>(out + 2) = static_cast<std::int16_t>(frame);
            b.sounds[b.count] = 0;
            b.frames[b.count] = frame;
            ++b.count;
            b.used += size;
            ++added;
        }
        t += size;
    }
    return added;
}

const AnimFxList* EntryFxUnsafe(const AnimEntry* entries, std::uint32_t count, const char* name) {
    for (std::uint32_t i = 0; i < count; ++i)
        if (NameIs(entries[i].name, name)) return static_cast<const AnimFxList*>(entries[i].fx);
    return nullptr;
}

// The kick clip's fx list, from Lara's library (the throw as the rope flies and its clank as the line reaches the
// enemy, the whoosh as the zip starts, the kick's own whoosh and shout just before the kick, the thuds and, with the
// impact on, the camera shake at it).
void BuildKickFxUnsafe(const std::uint8_t* library) {
    const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(library + 0x10);
    const auto* entries = *reinterpret_cast<const AnimEntry* const*>(library + 0x18);
    if (!entries || count == 0 || count > 20000) return;
    static FxBuild b;
    b.used = 0;
    b.count = 0;
    const char* throwName = "none";
    const char* impactName = "none";
    const AnimFxList* throwFx = nullptr;
    const AnimFxList* impactFx = nullptr;
    for (const char* name : kThrowSources)
        if (!throwFx && (throwFx = EntryFxUnsafe(entries, count, name)) != nullptr) throwName = name;
    for (const char* name : kImpactSources)
        if (!impactFx && (impactFx = EntryFxUnsafe(entries, count, name)) != nullptr) impactName = name;
    const char* whooshName = "none";
    const char* ropeName = "none";
    const AnimFxList* whooshFx = nullptr;
    const AnimFxList* ropeFx = nullptr;
    for (const char* name : kWhooshSources)
        if (!whooshFx && (whooshFx = EntryFxUnsafe(entries, count, name)) != nullptr) whooshName = name;
    for (const char* name : kRopeSoundSources)
        if (!ropeFx && (ropeFx = EntryFxUnsafe(entries, count, name)) != nullptr) ropeName = name;
    const auto* kickFx = static_cast<const AnimFxList*>(g_entryOriginal.fx);
    const int shoot = g_info.ropeShoot, zip = g_info.zipStart, kick = g_info.kick;
    const int throwN = AppendSoundsUnsafe(b, throwFx, 0, 9, shoot - 1, false, 0);        // the axe leaves the hand
    const int whooshN = AppendSoundsUnsafe(b, whooshFx, 0, 2, zip - 1, false, 0);        // the evade's air / cloth
    const int ropeN = AppendSoundsUnsafe(b, ropeFx, 0, 10, shoot, false, kRopeZipSound) +  // the line flies ...
                      AppendSoundsUnsafe(b, ropeFx, 0, 10, shoot, false, kRopeTensionSound);  // ... and pulls taut
    const int kickN = AppendSoundsUnsafe(b, kickFx, 0, 2, kick - 3, true, 0);            // whoosh, cloth, shout
    const int impactN = AppendSoundsUnsafe(b, impactFx, 0, 40, kick - 10, false, kImpactSound);  // kf 10 -> the kick
    const int attach = g_info.ropeAttach;
    const AnimFxList* biteFx = EntryFxUnsafe(entries, count, kAxeBiteSource);
    int clankN = AppendSoundsUnsafe(b, biteFx, 5, 5, attach - 5, false, kAxeBiteSound);  // the line bites ...
    const AnimFxList* grabFx = EntryFxUnsafe(entries, count, kAttachSource);
    clankN += AppendSoundsUnsafe(b, grabFx, 0, 3, attach - 3, true, 0);               // ... and yanks her: a grunt, a grab
    clankN += AppendCameraUnsafe(b, grabFx, 3, 3, attach - 3);                          // ... and a jolt
    const AnimFxList* clampFx = EntryFxUnsafe(entries, count, kGrappleClampSource);
    const AnimFxList* swingFx = EntryFxUnsafe(entries, count, kGrappleSwingSource);
    const int grappleN = AppendSoundsUnsafe(b, clampFx, 0, 0, attach, false, kGrappleClampSound) +  // the hook clamps on,
                         AppendSoundsUnsafe(b, swingFx, 0, 0, attach, false, kGrappleWhirSound) +   // the grapple whirs
                         AppendSoundsUnsafe(b, swingFx, 1, 1, attach, false, kGrappleCreakSound);   // and its rope creaks
    int thudN = 0, shakeN = 0;
    if (g_impact.load()) {
        const AnimFxList* landFx = EntryFxUnsafe(entries, count, kLandHardSource);
        thudN = AppendSoundsUnsafe(b, landFx, 0, 0, kick, false, kLandThudSound);  // the body thud ...
        shakeN = AppendCameraUnsafe(b, landFx, 4, 4, kick - 4);                    // ... and the landing's camera shake
    }
    if (!b.count) {
        Log::Detail("Grapple: none of the kick's source sounds were found; the kick plays with its own fx list");
        return;
    }
    auto* mem = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, sizeof(AnimFxList) + b.used + 8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!mem) return;
    auto* list = reinterpret_cast<AnimFxList*>(mem);
    std::uint8_t* triggers = mem + sizeof(AnimFxList);
    std::memcpy(triggers, b.buf, b.used);
    *reinterpret_cast<std::uint16_t*>(triggers + b.used) = 0xFFFF;
    list->effectLib = whooshFx ? whooshFx->effectLib : kickFx ? kickFx->effectLib : nullptr;
    list->triggers = triggers;
    g_kickFx = list;
    Log::Detailf("Grapple: kick sounds: %d triggers (%d from '%s' from frame %d, %d rope from '%s' from frame %d, %d from "
                "'%s' from frame %d, %d from 'Combat_Kick' from frame %d, %d from '%s' at frame %d, attach (bite, grab) %d and "
                "grapple (clamp, whir, creak) %d at frame %d, thud %d and camera shake %d from '%s' at frame %d%s)", b.count,
                throwN, throwName, shoot - 1, ropeN, ropeName, shoot, whooshN, whooshName, zip - 1, kickN, kick - 3, impactN,
                impactName, kick, clankN, grappleN, attach, thudN, shakeN, kLandHardSource, kick,
                g_impact.load() ? "" : ", impact off");
}

void EnsureKickFxUnsafe() {
    if (g_fxRebuild.exchange(false)) g_kickFxLibrary = nullptr;
    if (!g_clipsLoaded || !g_entryReady) return;
    const std::uint8_t* library = FindLibraryUnsafe();
    if (!library || library == g_kickFxLibrary) return;
    g_kickFxLibrary = library;
    g_kickFx = nullptr;
    BuildKickFxUnsafe(library);
}

// ---------- graph route ----------
// Reads that may meet a stale or wrong pointer (the anim component scan, graph data of an unloaded level).
bool SafeCopy(const void* address, void* out, std::size_t size) {
    if (!address) return false;
#if defined(_MSC_VER)
    __try {
#endif
        std::memcpy(out, address, size);
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

template <typename T>
T SafeGet(const std::uint8_t* address) {
    T value{};
    return SafeCopy(address, &value, sizeof(T)) ? value : T{};
}

bool SafeNameIs(const char* text, const char* expected) {
    if (!text) return false;
#if defined(_MSC_VER)
    __try {
#endif
        return NameIs(text, expected);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

bool PlausiblePointer(const void* p) {
    const auto v = reinterpret_cast<std::uintptr_t>(p);
    return v >= 0x10000 && v < 0x00007FFFFFFF0000ull && (v & 7) == 0;
}

// A DTP state graph's state by name and kind; its index in *index.
const std::uint8_t* GraphState(const std::uint8_t* graph, const char* name, std::uint8_t kind, int* index = nullptr) {
    const auto* states = SafeGet<const std::uint8_t*>(graph + kGraphStates);
    const int count = SafeGet<std::uint8_t>(graph + kGraphNumStates);
    if (!PlausiblePointer(states)) return nullptr;
    for (int i = 0; i < count; ++i) {
        const std::uint8_t* state = states + i * kGraphStateSize;
        if (SafeGet<std::uint8_t>(state + kGraphStateKind) == kind && SafeNameIs(SafeGet<const char*>(state), name)) {
            if (index) *index = i;
            return state;
        }
    }
    return nullptr;
}

// The one edge of a state graph from src to one of two targets (the shipped one or ours).
std::uint8_t* GraphEdge(const std::uint8_t* graph, std::uint8_t src, std::uint8_t dst, std::uint8_t dstPatched) {
    auto* edges = SafeGet<std::uint8_t*>(graph + kGraphEdges);
    const int count = SafeGet<std::uint8_t>(graph + kGraphNumEdges);
    if (!PlausiblePointer(edges)) return nullptr;
    std::uint8_t* found = nullptr;
    for (int i = 0; i < count; ++i) {
        std::uint8_t* edge = edges + i * kGraphEdgeSize;
        const std::uint8_t from = SafeGet<std::uint8_t>(edge + kGraphEdgeSrc), to = SafeGet<std::uint8_t>(edge + kGraphEdgeDst);
        if (from != src || (to != dst && to != dstPatched)) continue;
        if (found) return nullptr;  // not unique
        found = edge;
    }
    return found;
}

// Lara's graph 267229 at run time, down to its "Playable" state graph, and the edges T63 (S0 "Default" -> S22 "Cheat")
// and T69 (S23 "Combat_Kick" -> S5), all checked by name.
// The runtime cdc::AnimStateGraph built from a DTP state graph, among its graph root's allocator blocks (by its
// mDTPData, owner and counts).
std::uint8_t* FindRuntimeGraph(const std::uint8_t* graphRoot, const std::uint8_t* dtp, int numStates, int numEdges, int& blocks) {
    static std::uint8_t s_block[kAllocBlockData];
    const auto* block = SafeGet<const std::uint8_t*>(graphRoot + kRootAllocator);
    for (blocks = 0; PlausiblePointer(block) && blocks < 4096; ++blocks) {
        if (!SafeCopy(block + 8, s_block, kAllocBlockData)) return nullptr;
        for (std::size_t o = kStateGraphDtp; o + 8 <= kAllocBlockData; o += 8) {
            if (*reinterpret_cast<const std::uint8_t* const*>(s_block + o) != dtp) continue;
            auto* graph = const_cast<std::uint8_t*>(block) + 8 + (o - kStateGraphDtp);
            if (SafeGet<const std::uint8_t*>(graph + kRtOwner) == graphRoot &&
                SafeGet<std::uint32_t>(graph + kRtNumStateInfos) == static_cast<std::uint32_t>(numStates) &&
                SafeGet<std::uint32_t>(graph + kRtNumTransitions) == static_cast<std::uint32_t>(numEdges))
                return graph;
        }
        block = SafeGet<const std::uint8_t*>(block);
    }
    return nullptr;
}

// A runtime state graph's node for state i (its first output plug's node).
void* RuntimeStateNode(const std::uint8_t* graph, int i) {
    const auto* infos = SafeGet<const std::uint8_t*>(graph + kRtStateInfos);
    const auto* plugs = PlausiblePointer(infos) ? SafeGet<const std::uint8_t*>(infos + i * kPlugInfoSize) : nullptr;
    return PlausiblePointer(plugs) ? SafeGet<void*>(plugs) : nullptr;
}

// The runtime transition built from a DTP Edge.
std::uint8_t* RuntimeTransition(const std::uint8_t* graph, const std::uint8_t* edge) {
    auto* list = SafeGet<std::uint8_t*>(graph + kRtTransitions);
    const std::uint32_t count = SafeGet<std::uint32_t>(graph + kRtNumTransitions);
    for (std::uint32_t i = 0; PlausiblePointer(list) && i < count && i < 1024; ++i) {
        std::uint8_t* transition = list + i * kRtTransitionSize;
        if (SafeGet<const std::uint8_t*>(transition + kRtTransEdge) == edge) return transition;
    }
    return nullptr;
}

bool FindGraphRoute(std::uint8_t* lara, Route& route, std::size_t& offset, int& numStates, int& numEdges, int& blocks, const char*& missing) {
    missing = "anim component";
    const auto* anim = static_cast<const std::uint8_t*>(g_getAnimComponent(lara));
    if (!anim) return false;
    missing = "instance graph";
    const std::uint8_t* root = nullptr;
    const std::uint8_t* rootGraphRoot = nullptr;
    for (std::size_t off = kAnimGraphScanFrom; off <= kAnimGraphScanTo && !root; off += 8) {
        const auto* graphRoot = SafeGet<const std::uint8_t*>(anim + off);
        if (!PlausiblePointer(graphRoot)) continue;
        const auto* graph = SafeGet<const std::uint8_t*>(graphRoot + kGraphRootGraph);
        if (!PlausiblePointer(graph)) continue;
        const auto* dtp = SafeGet<const std::uint8_t*>(graph + kStateGraphDtp);
        if (!PlausiblePointer(dtp) || SafeGet<std::uint8_t>(dtp + kGraphNumStates) != 1) continue;
        if (!GraphState(dtp, "Player_RootState", 0)) continue;
        root = dtp;
        rootGraphRoot = graphRoot;
        offset = off;
    }
    if (!root) return false;
    missing = "Playable graph data";
    const auto* pipe = SafeGet<const std::uint8_t*>(GraphState(root, "Player_RootState", 0) + kGraphStateChild);
    const auto* nodes = PlausiblePointer(pipe) ? SafeGet<const std::uint8_t*>(pipe + kPipeNodes) : nullptr;
    const int nodeCount = PlausiblePointer(pipe) ? SafeGet<std::uint8_t>(pipe + kPipeNumNodes) : 0;
    const std::uint8_t* playerRoot = nullptr;
    for (int i = 0; PlausiblePointer(nodes) && i < nodeCount && !playerRoot; ++i) {
        const std::uint8_t* node = nodes + i * kGraphNodeSize;
        if (SafeGet<std::uint8_t>(node + kGraphNodeKind) == 2 && SafeNameIs(SafeGet<const char*>(node), "Player_Root"))
            playerRoot = SafeGet<const std::uint8_t*>(node + kGraphNodeSub);
    }
    if (!PlausiblePointer(playerRoot)) return false;
    const std::uint8_t* playableState = GraphState(playerRoot, "Playable", 1);
    const auto* playable = playableState ? SafeGet<const std::uint8_t*>(playableState + kGraphStateChild) : nullptr;
    if (!PlausiblePointer(playable)) return false;
    int cheat = -1, kick = -1, hub = -1;
    if (!GraphState(playable, "Default", 2, &hub) || !GraphState(playable, "Cheat", 0, &cheat) ||
        !GraphState(playable, "Combat_Kick", 0, &kick) || hub != kPlayableDefault || cheat != kPlayableCheat || kick != kPlayableKick)
        return false;
    route.t63 = GraphEdge(playable, kPlayableDefault, kPlayableCheat, kPlayableKick);
    route.t69 = GraphEdge(playable, kPlayableKick, kPlayableWeapon, kPlayableDefault);
    numStates = SafeGet<std::uint8_t>(playable + kGraphNumStates);
    numEdges = SafeGet<std::uint8_t>(playable + kGraphNumEdges);
    missing = "T63 / T69";
    if (!route.t63 || !route.t69 || !MakeWritable(route.t63 + kGraphEdgeDst, 1) || !MakeWritable(route.t69 + kGraphEdgeDst, 1))
        return false;
    // The runtime side: the Playable graph's cdc::AnimStateGraph, its state nodes and the two transitions.
    missing = "runtime Playable graph";
    const std::uint8_t* runtime = FindRuntimeGraph(rootGraphRoot, playable, numStates, numEdges, blocks);
    if (!runtime) return false;
    missing = "runtime transitions";
    route.nodeDefault = RuntimeStateNode(runtime, kPlayableDefault);
    route.nodeWeapon = RuntimeStateNode(runtime, kPlayableWeapon);
    route.nodeCheat = RuntimeStateNode(runtime, kPlayableCheat);
    route.nodeKick = RuntimeStateNode(runtime, kPlayableKick);
    route.rt63 = RuntimeTransition(runtime, route.t63);
    route.rt69 = RuntimeTransition(runtime, route.t69);
    if (!route.nodeDefault || !route.nodeWeapon || !route.nodeCheat || !route.nodeKick || !route.rt63 || !route.rt69) return false;
    // Each transition leaves its own state's node (checks the node lookup).
    missing = "runtime transition nodes";
    if (SafeGet<void*>(route.rt63 + kRtTransFrom) != route.nodeDefault || SafeGet<void*>(route.rt69 + kRtTransFrom) != route.nodeKick)
        return false;
    missing = "";
    return true;
}

// The edges and runtime transitions still the ones found (same sources, targets of ours or the shipped ones).
bool RouteValid(const Route& r) {
    if (!r.t63 || !r.t69 || !r.rt63 || !r.rt69) return false;
    const std::uint8_t d63 = SafeGet<std::uint8_t>(r.t63 + kGraphEdgeDst), d69 = SafeGet<std::uint8_t>(r.t69 + kGraphEdgeDst);
    void* const to63 = SafeGet<void*>(r.rt63 + kRtTransTo);
    void* const to69 = SafeGet<void*>(r.rt69 + kRtTransTo);
    return SafeGet<std::uint8_t>(r.t63 + kGraphEdgeSrc) == kPlayableDefault && SafeGet<std::uint8_t>(r.t69 + kGraphEdgeSrc) == kPlayableKick &&
           (d63 == kPlayableCheat || d63 == kPlayableKick) && (d69 == kPlayableWeapon || d69 == kPlayableDefault) &&
           SafeGet<const std::uint8_t*>(r.rt63 + kRtTransEdge) == r.t63 && SafeGet<const std::uint8_t*>(r.rt69 + kRtTransEdge) == r.t69 &&
           (to63 == r.nodeCheat || to63 == r.nodeKick) && (to69 == r.nodeWeapon || to69 == r.nodeDefault);
}

bool RoutePatchedNow(const Route& r) {
    return r.t63[kGraphEdgeDst] == kPlayableKick || r.t69[kGraphEdgeDst] == kPlayableDefault ||
           *reinterpret_cast<void* const*>(r.rt63 + kRtTransTo) == r.nodeKick ||
           *reinterpret_cast<void* const*>(r.rt69 + kRtTransTo) == r.nodeDefault;
}

// The node first, then the index (EvaluateTransitions reads mTo, then the Edge's dst byte).
void PatchRoute(bool on) {
    if (on == g_routePatched || !RouteValid(g_route)) {
        if (!on) g_routePatched = false;
        return;
    }
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(g_route.rt63 + kRtTransTo), on ? g_route.nodeKick : g_route.nodeCheat);
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(g_route.rt69 + kRtTransTo), on ? g_route.nodeDefault : g_route.nodeWeapon);
    g_route.t63[kGraphEdgeDst] = on ? kPlayableKick : kPlayableCheat;
    g_route.t69[kGraphEdgeDst] = on ? kPlayableDefault : kPlayableWeapon;
    g_routePatched = on;
}

// Looks the route up (a new state control, or while it is missing); the patch state follows the data found (edges
// patched by an earlier kick on the same graph data get their targets back after the usual delay).
void LookUpRoute(std::uint8_t* lara, const char* why) {
    Route route;
    std::size_t offset = 0;
    int numStates = 0, numEdges = 0, blocks = 0;
    const char* missing = "";
    const bool found = FindGraphRoute(lara, route, offset, numStates, numEdges, blocks, missing) && RouteValid(route);
    const bool wasReady = g_routeReady;
    g_route = found ? route : Route{};
    g_routeReady = found;
    g_graphRootOffset = found ? offset : 0;
    g_routePatched = found && RoutePatchedNow(route);
    if (g_routePatched) g_routeRestoreMs = GetTickCount64() + kRouteRestoreDelayMs;
    if (found && (!wasReady || !g_routeLogged)) {
        Log::Detailf("Grapple: graph route ready (%s; instance graph at +0x%zX, 'Playable' %d states / %d transitions, runtime "
                    "graph in allocator block %d, T63 node %p -> %p, T69 node %p -> %p)%s: the Cheat moveset goes to "
                    "Combat_Kick while a kick runs", why, offset, numStates, numEdges, blocks, route.nodeCheat, route.nodeKick,
                    route.nodeWeapon, route.nodeDefault, g_routePatched ? ", still ours from before" : "");
        g_routeLogged = true;
    } else if (!found && (wasReady || !g_routeLogged)) {
        Log::Writef("Grapple: Lara's anim graph route not found (%s: %s%s); the kick plays only from the weapon stance", why,
                    missing, *missing ? " missing" : "edges not as expected");
        g_routeLogged = true;
    }
}

// ---------- melee move #4 ----------
void ApplyMovePatchUnsafe() {
    std::uint8_t* m = g_move;
    std::memcpy(m, g_moveOriginal, kMoveSize);
    const float damage = static_cast<float>(g_damage.load());
    *reinterpret_cast<float*>(m + kMoveDamage) = damage;
    *reinterpret_cast<float*>(m + kMoveStrikeDamage) = damage;
    *reinterpret_cast<std::int32_t*>(m + kMoveDamageType) = kDamageTypeMelee;
    const int reaction = g_reaction.load();
    if (reaction == static_cast<int>(Reaction::Ballistic)) {
        *reinterpret_cast<std::int32_t*>(m + kMoveEffect) = 1;
    } else {
        *reinterpret_cast<std::int32_t*>(m + kMoveEffect) = 2;
        *reinterpret_cast<std::int32_t*>(m + kMoveEffectUnion) = kReactionHitType[reaction];
    }
    const int kick = g_clipsLoaded ? g_info.kick : 16;
    auto* frames = reinterpret_cast<std::int32_t*>(m + kMoveFrames);
    frames[0] = std::max(0, kick - 3);
    frames[1] = kick + 3;
    // The strike frames cover the whole move (the melee frames count the state's frame counter, three times faster
    // than the clip in v0.9.230); the tick switches the volume on at the clip's kick frame. Each victim is hit once.
    auto* strike = reinterpret_cast<std::int32_t*>(m + kMoveStrikeFrames);
    strike[0] = 0;
    strike[1] = 255;
    *reinterpret_cast<std::int32_t*>(m + kMoveRepeatOption) = 0;
    *reinterpret_cast<std::int32_t*>(m + kMoveRepeatFrames) = 0;
    // Strike volume (like Lara's melee push, #19): distance / height / angle ranges, no shape change, damage on.
    std::uint8_t* v = m + kMoveVolume;
    auto f = [&](std::size_t o, float value) { *reinterpret_cast<float*>(v + o) = value; };
    f(0x00, 0.0f); f(0x04, kStrikeReach);          // distance
    f(0x08, kStrikeLow); f(0x0C, kStrikeHigh);     // height
    f(0x10, -70.0f); f(0x14, 70.0f);               // angle
    f(0x20, 0.0f); f(0x24, kStrikeReach);          // final distance / height / angle (no shape change)
    f(0x28, kStrikeLow); f(0x2C, kStrikeHigh);
    f(0x30, -70.0f); f(0x34, 70.0f);
    f(0x48, 0.0f); f(0x4C, kStrikeReach);          // distance vs player
    f(0x50, 0.0f); f(0x54, kStrikeReach);
    v[0x3C] = 0;  // angle reversed
    v[0x3D] = 0;  // noDamage
    v[0x3E] = 0;  // noCurrentMeleeTarget
    v[0x44] = 0;  // doVolumeChange
    m[kMoveUse2D] = 1;
    m[kMoveVolumeOn] = g_moveActive.load() && !g_impactDone ? 0 : 1;
    m[kMoveCustomStrikeDamage] = 0;
    m[kMoveCanNotKill] = g_lethal.load() ? 0 : 1;
    m[kMoveCanNotKillExceptLow] = 0;
}

void EnsureMovePatchUnsafe(std::uint8_t* lara) {
    auto* component = static_cast<std::uint8_t*>(g_tryToGetMeleeMove(lara));
    if (!component) return;
    auto* dtp = *reinterpret_cast<std::uint8_t**>(component + kMeleeComponentDtp);
    if (!dtp) return;
    if (dtp != g_moveDtpRoot) {
        // New (or first) melee data: find "Kick" and keep its original bytes.
        g_moveDtpRoot = dtp;
        g_move = nullptr;
        g_movePatched = false;
        auto* moves = *reinterpret_cast<std::uint8_t**>(dtp + 0x10);
        const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(dtp + 0x18);
        if (!moves || count <= static_cast<std::uint32_t>(kKickMoveIndex) || count > 4096) return;
        std::uint8_t* move = moves + kKickMoveIndex * kMoveSize;
        if (!NameIs(*reinterpret_cast<const char* const*>(move + kMoveName), "Kick")) {
            Log::Detail("Grapple: melee move #4 is not 'Kick'; the kick does no damage");
            return;
        }
        if (!MakeWritable(move, kMoveSize)) {
            Log::Write("Grapple: melee move 'Kick' cannot be made writable");
            return;
        }
        std::memcpy(g_moveOriginal, move, kMoveSize);
        g_move = move;
        g_patchDirty.store(true);
    }
    if (g_move && (g_patchDirty.exchange(false) || !g_movePatched)) {
        ApplyMovePatchUnsafe();
        g_movePatched = true;
        Log::DetailOncef("Grapple: melee move 'Kick' patched: damage %d%s, reaction %s, strike frames %d..%d",
                    g_damage.load(), g_lethal.load() ? " (can kill)" : " (cannot kill)", kReactionNames[g_reaction.load()],
                    *reinterpret_cast<const std::int32_t*>(g_move + kMoveStrikeFrames),
                    *reinterpret_cast<const std::int32_t*>(g_move + kMoveStrikeFrames + 4));
    }
}

void RestoreMoveUnsafe() {
    if (g_move && g_movePatched) std::memcpy(g_move, g_moveOriginal, kMoveSize);
    g_movePatched = false;
}

// ---------- target ----------
std::uint8_t* PlayerOf(std::uint8_t* lara) {
    const std::uint32_t id = *At<const std::uint32_t*>(kRvaPlayerComponentId);
    std::uint8_t* component = InstanceComponent(lara, id);
    return component ? *reinterpret_cast<std::uint8_t**>(component + kPlayerComponentPlayer) : nullptr;
}

// The camera's position and unit aim direction.
bool CameraRay(std::uint8_t* player, float pos[3], float dir[3]) {
    if (!g_cameraForward || !g_cameraPosition || !g_cameraManager || !player) return false;
    void* manager = *g_cameraManager;
    if (!manager) return false;
    const std::uint32_t id = *reinterpret_cast<const std::uint32_t*>(player + kPlayerCameraId);
    alignas(16) float forward[4]{}, position[4]{};
    g_cameraForward(manager, forward, id);
    g_cameraPosition(manager, position, id);
    const float length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    if (!std::isfinite(length) || length < 1e-3f || !std::isfinite(position[0])) return false;
    for (int c = 0; c < 3; ++c) {
        dir[c] = forward[c] / length;
        pos[c] = position[c];
    }
    return true;
}

bool NpcAlive(const std::uint8_t* npc) {
    return npc && !(npc[kNpcFlags] & 1);
}

// Degrees between the camera's aim and the way to the enemy's chest.
float AngleFromCamera(const float camPos[3], const float camDir[3], const float* p) {
    const float v[3] = {p[0] - camPos[0], p[1] - camPos[1], p[2] + kChestHeight - camPos[2]};
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length < 1.0f) return 0.0f;
    const float c = (v[0] * camDir[0] + v[1] * camDir[1] + v[2] * camDir[2]) / length;
    return std::acos(std::clamp(c, -1.0f, 1.0f)) * 57.29578f;
}

bool TargetPosition(float out[3]);

// The kick's damage for its target (the move's damage fields): the menu's, or with "can kill" off at most what leaves the
// enemy a tenth of its health (half the room: armor and difficulty scale it); an enemy already below that gets 1 (a move
// whose damage rounds to 0 skips ApplyDamage, reaction and all) and 2 points first.
void SetKickDamageUnsafe(std::uint8_t* enemy, const char* when) {
    if (!g_move || !g_movePatched) return;
    const float configured = static_cast<float>(g_damage.load());
    float damage = configured, hp = -1.0f, maxHp = -1.0f;
    bool topUp = false;
    if (!g_lethal.load() && g_healthReadable && enemy) {
        auto* health = static_cast<std::uint8_t*>(g_tryToGetHealth(enemy));
        auto* values = health ? *reinterpret_cast<std::uint8_t* const*>(health + kHealthValues) : nullptr;
        if (values) {
            hp = *reinterpret_cast<const float*>(values + kHealthCurrent);
            maxHp = static_cast<float>(*reinterpret_cast<const std::int32_t*>(health + kHealthMax));
            if (std::isfinite(hp) && hp > 0.0f && maxHp > 0.0f) {
                const float room = (hp - std::max(2.0f, maxHp * kNoKillFloor)) * 0.5f;
                if (room >= 1.0f) {
                    damage = std::min(configured, std::floor(room));
                } else {
                    damage = 1.0f;
                    topUp = true;
                    *reinterpret_cast<float*>(values + kHealthCurrent) = hp + 2.0f;
                }
            }
        }
    }
    *reinterpret_cast<float*>(g_move + kMoveDamage) = damage;
    *reinterpret_cast<float*>(g_move + kMoveStrikeDamage) = damage;
    if (g_healthLogs < 24) {
        ++g_healthLogs;
        Log::Detailf("Grapple: kick damage %.0f %s (enemy health %.0f of %.0f%s%s)", damage, when, hp, maxHp,
                    g_lethal.load() ? ", can kill" : "", topUp ? ", below a tenth: +2 first" : "");
    }
}

void LogTargetHealthUnsafe(const char* when) {
    if (g_healthLogs >= 24 || !g_moveTarget || !g_healthReadable) return;
    float pos[3];
    if (!TargetPosition(pos)) return;
    auto* enemy = static_cast<std::uint8_t*>(g_moveTarget);
    auto* health = static_cast<std::uint8_t*>(g_tryToGetHealth(enemy));
    auto* values = health ? *reinterpret_cast<std::uint8_t* const*>(health + kHealthValues) : nullptr;
    const auto* npc = static_cast<const std::uint8_t*>(g_tryToGetNpc(enemy));
    ++g_healthLogs;
    Log::Detailf("Grapple: enemy %s: health %.0f of %d, %s", when, values ? *reinterpret_cast<const float*>(values + kHealthCurrent) : -1.0f,
                health ? *reinterpret_cast<const std::int32_t*>(health + kHealthMax) : -1, !npc ? "no NpcComponent" : NpcAlive(npc) ? "alive" : "dead");
}

// The target: the combat system's ranged target when it is a living enemy in range, else (lock-on) the living enemy
// nearest the camera's aim within the cone and the range; the current one is kept unless another is clearly nearer
// the aim.
void UpdateTargetUnsafe(std::uint8_t* lara) {
    g_targetInRange = false;
    g_targetInRangeAir = false;
    const auto* a = reinterpret_cast<const float*>(lara + kInstancePos);
    const float minGround = static_cast<float>(g_minRange.load()), minAir = static_cast<float>(g_airMinRange.load());
    const float maxRange = static_cast<float>(g_maxRange.load()), maxHeight = static_cast<float>(g_maxHeight.load());
    std::uint8_t* player = PlayerOf(lara);
    float camPos[3]{}, camDir[3]{};
    const bool haveCamera = g_lockOnAvailable && CameraRay(player, camPos, camDir);

    std::uint8_t* chosen = nullptr;
    float chosenAngle = -1.0f;
    const char* source = "";
    void* combatManager = player ? *reinterpret_cast<void**>(player + kPlayerCombatManager) : nullptr;
    auto* aimed = combatManager ? static_cast<std::uint8_t*>(g_getTargetInstance(combatManager, kSenseRangedCombat, false)) : nullptr;
    if (aimed && aimed != lara) {
        const auto* npc = static_cast<const std::uint8_t*>(g_tryToGetNpc(aimed));
        const auto* b = reinterpret_cast<const float*>(aimed + kInstancePos);
        const float dist = std::hypot(b[0] - a[0], b[1] - a[1]);
        if (npc && (!g_lockOnAvailable || NpcAlive(npc)) && dist <= maxRange && std::fabs(b[2] - a[2]) <= maxHeight &&
            dist >= std::min(minGround, minAir)) {
            chosen = aimed;
            chosenAngle = haveCamera ? AngleFromCamera(camPos, camDir, b) : -1.0f;
            source = "aim";
        }
    }
    if (!chosen && haveCamera && g_lockOn.load() && g_isEnemyOf && g_npcList) {
        const float cone = static_cast<float>(g_lockCone.load());
        float bestScore = 1e9f;
        const std::uint8_t* npc = *g_npcList;
        for (int guard = 0; npc && guard < kMaxNpcs; ++guard, npc = *reinterpret_cast<const std::uint8_t* const*>(npc + kNpcNext)) {
            auto* inst = *reinterpret_cast<std::uint8_t* const*>(npc + kNpcInstance);
            if (!inst || inst == lara || !NpcAlive(npc)) continue;
            const auto* b = reinterpret_cast<const float*>(inst + kInstancePos);
            const float dist = std::hypot(b[0] - a[0], b[1] - a[1]);
            if (dist > maxRange || dist < std::min(minGround, minAir) || std::fabs(b[2] - a[2]) > maxHeight) continue;
            const float angle = AngleFromCamera(camPos, camDir, b);
            const bool current = inst == g_target;
            if (angle > cone + (current ? 5.0f : 0.0f)) continue;
            if (!g_isEnemyOf(const_cast<std::uint8_t*>(npc), lara)) continue;
            const float score = angle + dist / 500.0f - (current ? 4.0f : 0.0f);  // 5 m costs a degree
            if (score < bestScore) {
                bestScore = score;
                chosen = inst;
                chosenAngle = angle;
            }
        }
        if (chosen) source = "lock-on";
    }
    if (!chosen) {
        g_target = nullptr;
        g_targetAngle = -1.0f;
        SetText(g_targetStatus, sizeof(g_targetStatus), "no target");
        return;
    }
    const auto* b = reinterpret_cast<const float*>(chosen + kInstancePos);
    const float dist = std::hypot(b[0] - a[0], b[1] - a[1]);
    const float dz = b[2] - a[2];
    g_target = chosen;
    g_targetHandle = *reinterpret_cast<const std::uint64_t*>(chosen + kInstanceHandle);
    g_targetDist = dist;
    g_targetDz = dz;
    g_targetAngle = chosenAngle;
    g_targetSource = source;
    g_targetTick = g_tick;
    const bool heightOk = std::fabs(dz) <= maxHeight;
    g_targetInRange = heightOk && dist >= minGround && dist <= maxRange;
    g_targetInRangeAir = heightOk && dist >= minAir && dist <= maxRange;
    SetText(g_targetStatus, sizeof(g_targetStatus), "%s: %.1f m away, %+.1f m up, %.0f deg off the aim%s", source,
            dist / 100.0f, dz / 100.0f, static_cast<double>(std::max(0.0f, chosenAngle)),
            g_targetInRange ? ", in range" : g_targetInRangeAir ? ", in range from a jump" : ", out of range");
    const ULONGLONG now = GetTickCount64();
    if (chosen != g_loggedTarget || now >= g_nextTargetLogMs) {
        g_loggedTarget = chosen;
        g_nextTargetLogMs = now + 3000;
        Log::Detailf("Grapple: target %p (%s) at %.0f cm, dz %.0f, %.0f deg off the aim%s", chosen, source, dist, dz,
                    static_cast<double>(chosenAngle), g_targetInRange ? ", in range" : g_targetInRangeAir ? ", in range from a jump" : "");
    }
}

bool TargetPosition(float out[3]) {
    if (!g_moveTarget) return false;
#if defined(_MSC_VER)
    __try {
#endif
        const auto* target = static_cast<const std::uint8_t*>(g_moveTarget);
        if (*reinterpret_cast<const std::uint64_t*>(target + kInstanceHandle) != g_moveHandle) return false;
        const auto* p = reinterpret_cast<const float*>(target + kInstancePos);
        out[0] = p[0];
        out[1] = p[1];
        out[2] = p[2];
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

// ---------- overlay ----------
// The camera basis the way ui.cpp's draw tools build it (Features::DrawCameraState: position, quaternion, fov).
struct ViewBasis {
    float pos[3]{}, right[3]{}, forward[3]{}, up[3]{};
    float tanHalfFov = 0.0f, aspect = 1.0f, width = 0.0f, height = 0.0f;
};

bool BuildViewBasis(const Features::DrawCameraState& camera, ViewBasis& out) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (!camera.valid || display.x <= 1.0f || display.y <= 1.0f) return false;
    float qx = camera.quaternion[0], qy = camera.quaternion[1], qz = camera.quaternion[2], qw = camera.quaternion[3];
    const float lenSq = qx * qx + qy * qy + qz * qz + qw * qw;
    if (!(lenSq > 1.0e-10f) || !std::isfinite(lenSq)) return false;
    const float inv = 1.0f / std::sqrt(lenSq);
    qx *= inv; qy *= inv; qz *= inv; qw *= inv;
    for (int c = 0; c < 3; ++c) out.pos[c] = camera.position[c];
    out.right[0] = 1.0f - 2.0f * (qy * qy + qz * qz);
    out.right[1] = 2.0f * (qx * qy + qz * qw);
    out.right[2] = 2.0f * (qx * qz - qy * qw);
    out.forward[0] = 2.0f * (qx * qy - qz * qw);
    out.forward[1] = 1.0f - 2.0f * (qx * qx + qz * qz);
    out.forward[2] = 2.0f * (qy * qz + qx * qw);
    out.up[0] = 2.0f * (qx * qz + qy * qw);
    out.up[1] = 2.0f * (qy * qz - qx * qw);
    out.up[2] = 1.0f - 2.0f * (qx * qx + qy * qy);
    float fov = camera.fov;
    if (fov > 3.2f) fov *= 0.01745329252f;
    fov = std::clamp(fov, 0.05f, 3.05f);
    out.tanHalfFov = std::tan(fov * 0.5f);
    out.aspect = display.x / display.y;
    out.width = display.x;
    out.height = display.y;
    return std::isfinite(out.tanHalfFov) && out.tanHalfFov > 0.001f;
}

float ViewDepth(const ViewBasis& b, const float* p) {
    return (p[0] - b.pos[0]) * b.forward[0] + (p[1] - b.pos[1]) * b.forward[1] + (p[2] - b.pos[2]) * b.forward[2];
}

bool Project(const ViewBasis& b, const float* p, ImVec2& out) {
    const float d[3] = {p[0] - b.pos[0], p[1] - b.pos[1], p[2] - b.pos[2]};
    const float depth = d[0] * b.forward[0] + d[1] * b.forward[1] + d[2] * b.forward[2];
    if (depth <= 5.0f) return false;
    const float x = d[0] * b.right[0] + d[1] * b.right[1] + d[2] * b.right[2];
    const float y = d[0] * b.up[0] + d[1] * b.up[1] + d[2] * b.up[2];
    const float nx = x / (depth * b.tanHalfFov * b.aspect), ny = y / (depth * b.tanHalfFov);
    if (!std::isfinite(nx) || !std::isfinite(ny)) return false;
    out = ImVec2(b.width * (0.5f + 0.5f * nx), b.height * (0.5f - 0.5f * ny));
    return true;
}

// A segment clipped to the part in front of the camera.
bool ProjectSegment(const ViewBasis& b, const float* aIn, const float* bIn, ImVec2& aOut, ImVec2& bOut) {
    float a[3] = {aIn[0], aIn[1], aIn[2]}, e[3] = {bIn[0], bIn[1], bIn[2]};
    const float da = ViewDepth(b, a), de = ViewDepth(b, e), nearDepth = 10.0f;
    if (da <= nearDepth && de <= nearDepth) return false;
    if (da <= nearDepth || de <= nearDepth) {
        const float t = (nearDepth - da) / (de - da);
        float* moved = da <= nearDepth ? a : e;
        for (int c = 0; c < 3; ++c) moved[c] = aIn[c] + (bIn[c] - aIn[c]) * t;
    }
    return Project(b, a, aOut) && Project(b, e, bOut);
}

ImU32 Rgba(int r, int g, int bl, float alpha) {
    return IM_COL32(r, g, bl, static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f));
}

// The arrow over the enemy's head (the game's own glow is on the enemy itself): a bobbing chevron with a dark rim,
// amber when a melee press would start the kick, red while it runs.
void DrawMarker(ImDrawList* draw, const ViewBasis& b, const OverlayFrame& f) {
    const float feet[3] = {f.target[0], f.target[1], f.target[2] + 5.0f};
    const float head[3] = {f.target[0], f.target[1], f.target[2] + 190.0f};
    ImVec2 sf, sh;
    if (!Project(b, feet, sf) || !Project(b, head, sh)) return;
    const float height = std::hypot(sf.x - sh.x, sf.y - sh.y);
    if (height < 6.0f || height > b.height * 3.0f) return;
    const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(ImGui::GetTime() * 6.0));
    const int r = f.markerKick ? 240 : 255, g = f.markerKick ? 70 : 200, bl = f.markerKick ? 70 : 60;
    const float s = std::clamp(height * 0.07f, 7.0f, 16.0f);
    const float y = std::min(sh.y, sf.y) - s * (0.6f + 0.5f * pulse);
    const ImVec2 left(sh.x - s, y - s * 1.3f), right(sh.x + s, y - s * 1.3f), tip(sh.x, y);
    draw->AddTriangleFilled(ImVec2(left.x - 2.5f, left.y - 1.5f), ImVec2(right.x + 2.5f, right.y - 1.5f), ImVec2(tip.x, tip.y + 3.0f),
                            Rgba(20, 16, 12, 0.6f));
    draw->AddTriangleFilled(left, right, tip, Rgba(r, g, bl, 0.95f));
}

// The rope: a dark outline under a light core, from the hand to where the hook has flown; the hook itself while flying.
void DrawRope(ImDrawList* draw, const ViewBasis& b, const OverlayFrame& f) {
    float tip[3];
    for (int c = 0; c < 3; ++c) tip[c] = f.ropeFrom[c] + (f.ropeTo[c] - f.ropeFrom[c]) * f.ropeExtend;
    ImVec2 sa, sb;
    if (!ProjectSegment(b, f.ropeFrom, tip, sa, sb)) return;
    draw->AddLine(sa, sb, Rgba(15, 18, 24, 0.6f * f.ropeAlpha), 4.5f);
    draw->AddLine(sa, sb, Rgba(222, 226, 232, 0.95f * f.ropeAlpha), 2.0f);
    if (f.ropeExtend < 0.999f) draw->AddCircleFilled(sb, 3.5f, Rgba(240, 240, 245, f.ropeAlpha), 10);
}

// The kick's root travel needs the animation as the motion source (what the combat evade states set).
void SetAnimMotionUnsafe(std::uint8_t* anim) {
    *reinterpret_cast<std::int32_t*>(anim + kAnimMotionType) = kMotionFromAnimation;
    anim[kAnimMotionFlags] |= 1;  // root translation on
}

// After the kick: the combat stance's own movement again, unless the graph has chosen a source of its own meanwhile.
void RestoreAnimMotion() {
    if (!g_moveAnim) return;
#if defined(_MSC_VER)
    __try {
#endif
        if (g_moveMotionBefore == kMotionFromMovement &&
            *reinterpret_cast<const std::int32_t*>(g_moveAnim + kAnimMotionType) == kMotionFromAnimation) {
            *reinterpret_cast<std::int32_t*>(g_moveAnim + kAnimMotionType) = kMotionFromMovement;
            g_moveAnim[kAnimMotionFlags] &= static_cast<std::uint8_t>(~1u);
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#endif
    g_moveAnim = nullptr;
}

// A time dilation factor now (1 without the manager).
float DilationNow(std::size_t offset) {
    if (!g_timeDilationManager) return 1.0f;
    const float v = *reinterpret_cast<const float*>(g_timeDilationManager + offset);
    return std::isfinite(v) && v > 0.0f && v <= 4.0f ? v : 1.0f;
}

// The kick fragment's dtp: the route's S23 node's dtp::FragmentNode, checked by its animation (the kick entry).
std::uint8_t* KickFragmentDtp() {
    if (!g_fragmentReadable || !g_routeReady || !g_route.nodeKick || g_entryIndex < 0) return nullptr;
    auto* dtp = SafeGet<std::uint8_t*>(static_cast<const std::uint8_t*>(g_route.nodeKick) + kNodeDtp);
    if (!PlausiblePointer(dtp) || SafeGet<std::uint16_t>(dtp) != static_cast<std::uint16_t>(g_entryIndex)) return nullptr;
    return dtp;
}

void SetKickPlaySpeed(float speed) {
    if (!g_kickFragmentDtp || speed == g_kickPlaySpeed) return;
    *reinterpret_cast<float*>(g_kickFragmentDtp + kFragmentPlaySpeed) = speed;
    g_kickPlaySpeed = speed;
}

// The clip clock up to now, at the clip's speed: world x global (the anim graph's time; the fragment's play speed does
// not change it in the slow motion).
void AdvanceClipClock() {
    const ULONGLONG now = GetTickCount64();
    if (now <= g_clipClockMs) return;
    const float speed = std::clamp(DilationNow(kDilationWorldNow) * DilationNow(kDilationGlobalNow), 0.0f, 4.0f);
    g_kickSpeedMin = std::min(g_kickSpeedMin, speed);
    g_clipClock += static_cast<float>(now - g_clipClockMs) * 0.03f * speed;
    g_clipClockMs = now;
}

// The kick clip's frame: the smaller of the state's frame counter and the clip clock.
float KickClipFrame(const std::uint8_t* agent) {
    AdvanceClipClock();
    const float counter = *reinterpret_cast<const float*>(agent + kAgentFramesInState);
    return std::min(counter, g_clipClock);
}

// Each tick of a kick: the clip clock moves on (and a play speed raised by v0.9.241's logic is the game's again).
void UpdateKickPlaySpeedUnsafe() {
    AdvanceClipClock();
    SetKickPlaySpeed(g_kickPlaySpeedGame);
}

void EndMove(const char* why) {
    if (!g_moveActive.exchange(false)) return;
    g_lastKickEndMs = GetTickCount64();
    if (g_clockLogs < 40) {
        ++g_clockLogs;
        const ULONGLONG ms = GetTickCount64() - g_moveStartMs;
        Log::Detailf("Grapple: kick clip clock %.1f frames in %llu ms (slowest clip speed %.2f); time dilation at the start: "
                    "world %.2f, global %.2f", static_cast<double>(g_clipClock), ms, static_cast<double>(g_kickSpeedMin),
                    static_cast<double>(g_moveWorldSpeed), static_cast<double>(g_moveGlobalSpeed));
    }
    SetKickPlaySpeed(g_kickPlaySpeedGame);
    RestoreAnimMotion();
    float end[3]{};
    float remaining = -1.0f, remainingDz = 0.0f;
    float laraPos[3]{};
    bool haveLara = false;
#if defined(_MSC_VER)
    __try {
#endif
        if (g_lara) {
            const auto* p = reinterpret_cast<const float*>(static_cast<std::uint8_t*>(g_lara) + kInstancePos);
            laraPos[0] = p[0];
            laraPos[1] = p[1];
            laraPos[2] = p[2];
            haveLara = true;
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        haveLara = false;
    }
#endif
    if (haveLara && TargetPosition(end)) {
        remaining = std::hypot(end[0] - laraPos[0], end[1] - laraPos[1]);
        remainingDz = end[2] - laraPos[2];
    }
    const float moved = haveLara ? std::hypot(laraPos[0] - g_moveStartPos[0], laraPos[1] - g_moveStartPos[1]) : -1.0f;
    const float rose = haveLara ? laraPos[2] - g_moveStartPos[2] : 0.0f;
    SetText(g_lastKick, sizeof(g_lastKick), "%s%.1f m, %+.1f m up -> Lara moved %.1f m, %+.1f m up; %.1f m left (%s)",
            g_moveFromAir ? "from a jump: " : "", g_moveDist / 100.0f, g_moveRise / 100.0f, moved / 100.0f, rose / 100.0f,
            remaining / 100.0f, why);
    Log::Detailf("Grapple: kick ended (%s) after %llu ms: target was %.0f cm away (dz %+.0f), zip %.0f cm; Lara moved %.0f cm "
                "and %+.0f cm up, now %.0f cm from it (dz %+.0f)",
                why, GetTickCount64() - g_moveStartMs, g_moveDist, g_moveRise, g_moveTravel, moved, rose, remaining, remainingDz);
    // The graph edges get their own targets back once the graph has left the kick; meanwhile a moveset still "Cheat"
    // (a state after the kick that sets none of its own) goes back to Combat_Parent's.
    g_routeRestoreMs = GetTickCount64() + kRouteRestoreDelayMs;
    if (g_routePatched) g_movesetWatchUntilMs = GetTickCount64() + kMovesetWatchMs;
}

void UpdateDfaGlowUnsafe(const std::uint8_t* instance);
void ReleaseGameRope();
void DoImpactUnsafe(std::uint8_t* lara);
void EndHitStop();
void ReleaseHitStop();
int EnemyConnectFx(std::uint8_t* enemy);

void RestoreAllUnsafe() {
    RestoreEntryUnsafe();
    RestoreMoveUnsafe();
    ApplyKickPropsUnsafe(false);
    PatchRoute(false);
    UpdateDfaGlowUnsafe(nullptr);
    ReleaseGameRope();
    EndHitStop();
    ReleaseHitStop();
    SetKickPlaySpeed(g_kickPlaySpeedGame);
}

void UpdateOverlayUnsafe(std::uint8_t* lara, bool airReady);

void TickUnsafe() {
    ++g_tick;
    if (g_hitStopUntilMs && GetTickCount64() >= g_hitStopUntilMs) EndHitStop();
    if (g_hitStopReleaseTick && g_tick >= g_hitStopReleaseTick) ReleaseHitStop();
    if (g_reloadRequested.exchange(false)) {
        // The old copies keep their ids (a graph node may still hold them); new ones are loaded under new ids.
        g_clipsLoaded = false;
        g_clipsFailed = false;
        g_kickFxLibrary = nullptr;  // the sounds are timed to the clip: built again
        LoadSettings();
        g_patchDirty.store(true);
    }
    if (!g_enabled.load()) {
        if (g_entryWritten || g_movePatched || g_kickPropsOn || g_routePatched || g_glowNpc || g_ropeEntity) {
            RestoreAllUnsafe();
            Log::Detail("Grapple: off; the kick entry, melee move, kick state and graph route are the game's again");
        }
        g_targetInRange = g_targetInRangeAir = false;
        g_airArmed.store(false);
        g_overlayOn.store(false);
        SetText(g_status, sizeof(g_status), "off");
        return;
    }
    auto* lara = static_cast<std::uint8_t*>(g_getLocalPlayerInstance(0));
    g_lara = lara;
    if (!lara) {
        g_targetInRange = g_targetInRangeAir = false;
        g_overlayOn.store(false);
        ReleaseGameRope();
        return;
    }
    void* agent = CombatAir::LaraPrimaryAgent();
    if (agent != g_agent) {
        g_agent = agent;
        g_scReady = agent && FindSc(agent);
        if (g_moveActive.load()) EndMove("new state control");
        g_routeLookupDue = true;  // a new state control usually comes with a new anim graph
    }
    // The graph route (see the top): looked up for each state control and, while missing, every 2 s.
    const ULONGLONG nowMs = GetTickCount64();
    if (g_scReady && !g_moveActive.load() && (g_routeLookupDue || (!g_routeReady && nowMs >= g_routeCheckMs))) {
        LookUpRoute(lara, g_routeLookupDue ? "new state control" : "retry");
        g_routeLookupDue = false;
        g_routeCheckMs = nowMs + kRouteRetryMs;
    }
    if (!g_clipsLoaded && !g_clipsFailed) LoadClipsGameThread();
    EnsureEntryUnsafe();
    EnsureKickFxUnsafe();
    if (g_scReady) ApplyKickPropsUnsafe(true);
    EnsureMovePatchUnsafe(lara);
    UpdateTargetUnsafe(lara);

    g_laraState = agent ? *reinterpret_cast<const std::uint8_t* const*>(static_cast<std::uint8_t*>(agent) + kAgentCurrentState) : nullptr;
    g_laraClass = g_scReady ? ClassOf(g_laraState) : kClassOther;
    // After a kick through the Cheat route: a moveset still "Cheat" once Lara is out of the kick goes back to
    // Combat_Parent's, and the graph edges get their targets back.
    if (!g_moveActive.load() && g_setAnimGraphGoal && g_cheatMoveset && nowMs < g_movesetWatchUntilMs &&
        g_laraState != g_kickState && g_laraState != g_cheatState) {
        auto* anim = static_cast<std::uint8_t*>(g_getAnimComponent(lara));
        if (anim && *reinterpret_cast<const std::uint32_t*>(anim + kAnimGoals + kGoalMoveset * 4) == g_cheatMoveset) {
            g_setAnimGraphGoal(anim, kGoalMoveset, kMovesetCombat);
            Log::Detailf("Grapple: moveset still 'Cheat' after the kick (state %s); set back to 'Combat'", NameOf(g_laraState));
        }
    }
    if (g_routePatched && !g_moveActive.load() && nowMs >= g_routeRestoreMs) PatchRoute(false);
    g_aimHeld = CombatAir::AimHeld();
    const bool airReady = g_laraClass == kClassAir && g_airKick.load() && g_lockOnAvailable &&
                          (g_aimHeld || !g_airNeedsAim.load()) && g_targetInRangeAir;
    // The brawl's own melee press (its Y for a strike it could not force) is no jump kick, even with Lara in the air
    // (v0.9.258: its press in a fall zipped her at a far enemy while its approach held her back).
    const bool brawlPress = g_laraClass == kClassAir && Brawl::PressedMeleeRecently();

    // Jump kick: the melee action (as the combat stance's push or death from above ask for it), newly pressed.
    const bool melee = g_inputRequested && (g_inputRequested(-1, kActionMeleePush) || g_inputRequested(-1, kActionMeleeFinisher));
    const bool pressed = melee && !g_meleeHeld;
    g_meleeHeld = melee;
    if (pressed && g_laraClass == kClassAir && g_airPressLogs < 30) {
        ++g_airPressLogs;
        Log::Detailf("Grapple: melee in the air (%s, aim %s, target %s)%s", NameOf(g_laraState), g_aimHeld ? "held" : "not held",
                    g_target ? (g_targetInRangeAir ? "in range" : "out of range") : "none",
                    brawlPress ? ": the brawl's own press, no jump kick" : airReady ? ": jump kick armed" :
                    g_airKick.load() ? "" : " (jump kick off)");
    }
    const bool cooledDown = GetTickCount64() >= g_lastKickEndMs + kKickCooldownMs;
    if (pressed && airReady && !brawlPress && cooledDown && !g_moveActive.load() && g_clipsLoaded && g_entryReady && g_movePatched) {
        g_forcedTransition.store(nullptr);
        g_airArmTick = g_tick;
        g_airArmed.store(true);
    }
    if (g_airArmed.load() && (g_tick - g_airArmTick > kAirArmTicks || g_laraClass != kClassAir)) {
        g_airArmed.store(false);
        Log::Detailf("Grapple: jump kick not started (%s)", g_laraClass != kClassAir ? "Lara left the jump first" : "no transition was checked");
    }
    // Melee while aiming (the slow motion needs aim held): the combat stance's melee push is not taken then; the press
    // arms the kick like a jump's. Not while an enemy is inside the hand-to-hand range: the press is the brawl's.
    const bool footing = g_laraClass == kClassCombat || g_laraClass == kClassGround;
    const bool brawlClaims = footing && g_aimHeld && Brawl::ClaimsMeleePress();
    const bool aimReady = footing && g_aimHeld && g_targetInRange && g_target && !brawlClaims;
    if (pressed && footing && g_aimHeld && g_aimPressLogs < 30) {
        ++g_aimPressLogs;
        Log::Detailf("Grapple: melee while aiming (%s, time dilation type %d, target %s)%s", NameOf(g_laraState),
                    g_timeDilationManager ? *reinterpret_cast<const std::int32_t*>(g_timeDilationManager + kDilationCurrentType) : -1,
                    g_target ? (g_targetInRange ? "in range" : "out of range") : "none",
                    aimReady ? ": kick armed" : brawlClaims ? ": an enemy is within hand-to-hand range, the brawl's" : "");
    }
    if (pressed && aimReady && cooledDown && !g_moveActive.load() && g_clipsLoaded && g_entryReady && g_movePatched) {
        g_forcedTransition.store(nullptr);
        g_aimArmTick = g_tick;
        g_aimArmed.store(true);
    }
    if (g_aimArmed.load() && (g_tick - g_aimArmTick > kAirArmTicks || !footing)) {
        g_aimArmed.store(false);
        Log::Detailf("Grapple: aimed kick not started (%s)", !footing ? "Lara left the stance first" : "no transition was checked");
    }

    if (g_moveActive.load()) {
        UpdateKickPlaySpeedUnsafe();
        // The kick state left without a transition this hook saw (a forced state change): the kick is over.
        if (g_laraState == g_kickState) {
            g_kickStateSeen = true;
            g_kickStateGone = 0;
        } else if (g_kickStateSeen && ++g_kickStateGone >= 5) {
            char why[128];
            std::snprintf(why, sizeof(why), "left the kick state for %s unseen", NameOf(g_laraState));
            EndMove(why);
        }
    }
    if (g_moveActive.load() && GetTickCount64() - g_moveStartMs > kKickTimeoutMs) EndMove("timeout");
    // Invincible for as long as the kick lasts (it lasts longer in the slow motion).
    if (g_moveActive.load() && g_invincible.load() && g_setInvincible && g_tryToGetHealth &&
        GetTickCount64() >= g_invincibleRefreshMs) {
        g_invincibleRefreshMs = GetTickCount64() + kInvincibleRefreshMs;
        if (auto* health = static_cast<std::uint8_t*>(g_tryToGetHealth(lara))) {
            const auto* inv = *reinterpret_cast<const std::uint8_t* const*>(health + kHealthInvincibility);
            if (inv && *reinterpret_cast<const std::int64_t*>(inv + kInvincibleEnd) >= 0) g_setInvincible(health, kInvincibleMs);
        }
    }
    // The line reaches the enemy: its hit effect (the axe sounds of Lara's lists were not heard in the air).
    if (g_moveActive.load() && !g_attachDone && agent && g_laraState == g_kickState && g_impact.load() &&
        KickClipFrame(static_cast<const std::uint8_t*>(agent)) >= static_cast<float>(g_info.ropeAttach)) {
        g_attachDone = true;
        float target[3];
        auto* enemy = static_cast<std::uint8_t*>(g_moveTarget);
        if (g_fxTriggerById && enemy && TargetPosition(target)) {
            const int fx = EnemyConnectFx(enemy);
            void* zone = *reinterpret_cast<void* const*>(enemy + kInstanceObjectZone);
            if (fx > 0 && zone) g_fxTriggerById(enemy, zone, fx, 0, nullptr, nullptr);
        }
    }
    // The clip's kick frame: the strike volume goes on (the enemy's hit reaction starts as the foot lands) and, with the
    // enemy within its reach, the enemy's hit effect and the hit-stop.
    if (g_moveActive.load() && !g_impactDone && agent && g_laraState == g_kickState &&
        KickClipFrame(static_cast<const std::uint8_t*>(agent)) >= static_cast<float>(g_info.kick) - 0.5f) {
        g_impactDone = true;
        SetKickDamageUnsafe(static_cast<std::uint8_t*>(g_moveTarget), "at the kick frame");
        if (g_move && g_movePatched) g_move[kMoveVolumeOn] = 1;
        if (g_impact.load()) DoImpactUnsafe(lara);
        g_healthCheckTick = g_tick + 30;
    }
    if (g_healthCheckTick && g_tick >= g_healthCheckTick) {
        g_healthCheckTick = 0;
        LogTargetHealthUnsafe("half a second after the kick frame");
    }
    // While the kick state runs, the animation stays the motion source (a graph state that sets its own would end it).
    if (g_moveActive.load() && g_moveAnim && g_laraState == g_kickState &&
        *reinterpret_cast<const std::int32_t*>(g_moveAnim + kAnimMotionType) != kMotionFromAnimation) {
        SetAnimMotionUnsafe(g_moveAnim);
        static int s_reapplied = 0;
        if (++s_reapplied <= 5) Log::Detail("Grapple: the anim graph changed the motion source during a kick; set back to the animation");
    }

    UpdateOverlayUnsafe(lara, airReady);

    SetText(g_status, sizeof(g_status), "%s", !g_scReady ? "state control not found" : !g_clipsLoaded ? "clip not loaded"
                                              : !g_entryReady ? "kick entry not found" : !g_movePatched ? "melee move not patched"
                                              : "ready");
}

// The game's DFA highlight on the enemy (the NpcComponent flag bit UpdateUI reads every frame); nullptr clears it.
// Set again each tick: a change of the enemy's finisher prompt recomputes the bit.
void UpdateDfaGlowUnsafe(const std::uint8_t* instance) {
    if (!g_dfaGlowAvailable) return;
    auto* npc = instance ? static_cast<std::uint8_t*>(g_tryToGetNpc(const_cast<std::uint8_t*>(instance))) : nullptr;
    if (g_glowNpc && npc != g_glowNpc) {
        // Only while that component still belongs to the same enemy (its instance's handle).
        const auto* owner = SafeGet<const std::uint8_t*>(g_glowNpc + kNpcInstance);
        if (PlausiblePointer(owner) && SafeGet<std::uint64_t>(owner + kInstanceHandle) == g_glowHandle) {
            const std::uint8_t flags = SafeGet<std::uint8_t>(g_glowNpc + kNpcUiFlags);
            if (flags & kNpcUiDfaBit) g_glowNpc[kNpcUiFlags] = static_cast<std::uint8_t>(flags & ~kNpcUiDfaBit);
        }
        g_glowNpc = nullptr;
    }
    if (!npc) return;
    npc[kNpcUiFlags] |= kNpcUiDfaBit;
    g_glowNpc = npc;
    g_glowHandle = *reinterpret_cast<const std::uint64_t*>(instance + kInstanceHandle);
    if (!g_glowLogged) {
        g_glowLogged = true;
        Log::Detailf("Grapple: the game's death-from-above highlight is on the target (NpcComponent %p)", npc);
    }
}

// ---------- the kick's impact ----------
// The connect FX of one of the enemy's melee moves: a weapon or hand strike first ("butt", "punch", "swing", ...), then
// any attack; finishers, counters, grabs and throws are left out. The moves and their ids go to the log.
int PickConnectFxUnsafe(const std::uint8_t* dtp, char* list, std::size_t listSize, char* chosen, std::size_t chosenSize) {
    const auto* moves = *reinterpret_cast<const std::uint8_t* const*>(dtp + 0x10);
    const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(dtp + 0x18);
    int best = -1, bestScore = 0;
    for (std::uint32_t i = 0; moves && i < count && i < 512; ++i) {
        const std::uint8_t* move = moves + i * kMoveSize;
        const int fx = move[kMoveConnectFx];
        if (fx == 0 || fx == kFxIdNone) continue;
        const char* name = *reinterpret_cast<const char* const*>(move + kMoveName);
        char text[48] = "?", lower[48] = "";
        std::size_t n = 0;
        for (; name && name[n] && n < sizeof(text) - 1; ++n) {
            const char c = name[n];
            text[n] = c;
            lower[n] = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }
        if (n) text[n] = '\0';
        lower[n] = '\0';
        const std::size_t used = std::strlen(list);
        if (used + sizeof(text) + 8 < listSize)
            std::snprintf(list + used, listSize - used, "%s%s=%d", used ? ", " : "", text, fx);
        const char* skip[] = {"finish", "counter", "seize", "grab", "throw", "kill", "takedown", "execut"};
        bool skipped = false;
        for (const char* word : skip) skipped = skipped || std::strstr(lower, word) != nullptr;
        if (skipped) continue;
        int score = 1;
        const char* attacks[] = {"melee", "attack", "strike", "hit"};
        const char* weapons[] = {"butt", "punch", "swing", "bash", "stab", "slash", "smash", "jab", "hook", "elbow"};
        for (const char* word : attacks)
            if (std::strstr(lower, word)) score = std::max(score, 2);
        for (const char* word : weapons)
            if (std::strstr(lower, word)) score = 3;
        if (score > bestScore) {
            bestScore = score;
            best = fx;
            std::snprintf(chosen, chosenSize, "%s", text);
        }
    }
    return best;
}

int EnemyConnectFx(std::uint8_t* enemy) {
    auto* component = static_cast<std::uint8_t*>(g_tryToGetMeleeMove(enemy));
    const auto* dtp = component ? SafeGet<const std::uint8_t*>(component + kMeleeComponentDtp) : nullptr;
    if (!PlausiblePointer(dtp)) return -1;
    if (dtp == g_connectFxDtp) return g_connectFx;
    g_connectFxDtp = dtp;
    g_connectFx = -1;
    char list[720] = "", chosen[48] = "none";
#if defined(_MSC_VER)
    __try {
#endif
        g_connectFx = PickConnectFxUnsafe(dtp, list, sizeof(list), chosen, sizeof(chosen));
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_connectFx = -1;
    }
#endif
    if (g_connectFxLogs < 8) {
        ++g_connectFxLogs;
        Log::Detailf("Grapple: enemy melee data %p, its moves' connect FX: %s; the kick plays %d ('%s')", dtp,
                    list[0] ? list : "none", g_connectFx, chosen);
    }
    return g_connectFx;
}

bool DilationSlotActive(const std::uint8_t* slot) {
    return slot[kDilationForce] != 0 || *reinterpret_cast<const float*>(slot + kDilationWorld) != 1.0f ||
           *reinterpret_cast<const float*>(slot + kDilationGlobal) != 1.0f;
}

std::uint8_t* DilationSlot(int type) {
    return g_timeDilationManager + static_cast<std::size_t>(type) * kTimeDilationRequestSize;
}

// Our slot 5 request, as Request left it: type, factors and start time (another request there has its own).
bool HitStopSlotIs(float world, float global, float start) {
    const std::uint8_t* slot = DilationSlot(kHitStopType);
    return *reinterpret_cast<const std::int32_t*>(slot) == kHitStopType &&
           *reinterpret_cast<const float*>(slot + kDilationWorld) == world &&
           *reinterpret_cast<const float*>(slot + kDilationGlobal) == global &&
           *reinterpret_cast<const float*>(slot + kDilationStart) == start;
}

// The end of our hit-stop (only while the slot still holds it). TimeDilationManager::Process follows the first active
// slot and moves to it only when that slot is new (dirty): after the cancel it goes back by itself when nothing is
// active (normal speed) or when the first active slot is new; when that slot is the one it followed before, it is marked
// new again; any other (a stale request never followed, as the one v0.9.235's marking woke up: the world stayed slow for
// seconds) is left alone, and the way back to normal speed is a request of ours at speed 1 (forced active), cancelled a
// few ticks later.
void EndHitStop() {
    if (!g_hitStopUntilMs) return;
    g_hitStopUntilMs = 0;
    const char* how = "the game had replaced it";
    int first = kTimeDilationTypes;
#if defined(_MSC_VER)
    __try {
#endif
        if (HitStopSlotIs(1.0f, kHitStopSpeed, g_hitStopStart)) {
            g_dilationCancel(g_timeDilationManager, kHitStopType, kHitStopName);
            first = 0;
            while (first < kTimeDilationTypes && !DilationSlotActive(DilationSlot(first))) ++first;
            if (first == kTimeDilationTypes) {
                how = "nothing else active: normal speed";
            } else if (DilationSlot(first)[kDilationDirty]) {
                how = "a new request is next";
            } else if (first == g_hitStopPrevType) {
                DilationSlot(first)[kDilationDirty] = 1;
                how = "back to the request followed before";
            } else {
                alignas(16) std::uint8_t request[32] = {};
                g_dilationCtor(request, kHitStopType, 1.0f, 1.0f, kHitStopName);
                request[kDilationForce] = 1;
                g_dilationRequest(g_timeDilationManager, request);
                g_hitStopReleaseStart = *reinterpret_cast<const float*>(DilationSlot(kHitStopType) + kDilationStart);
                g_hitStopReleaseTick = g_tick + 3;
                how = "a stale request is first: back to normal speed through ours";
            }
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        how = "exception";
    }
#endif
    if (g_hitStopLogs < 12) {
        ++g_hitStopLogs;
        Log::Detailf("Grapple: hit-stop ended (followed type %d before it, first active now %d): %s", g_hitStopPrevType, first, how);
    }
}

// Our normal-speed request on the way back from a hit-stop, cancelled once the manager has followed it.
void ReleaseHitStop() {
    if (!g_hitStopReleaseTick) return;
    g_hitStopReleaseTick = 0;
#if defined(_MSC_VER)
    __try {
#endif
        if (DilationSlot(kHitStopType)[kDilationForce] && HitStopSlotIs(1.0f, 1.0f, g_hitStopReleaseStart))
            g_dilationCancel(g_timeDilationManager, kHitStopType, kHitStopName);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#endif
}

// At the clip's kick frame, with the enemy within the strike volume's reach: its melee-hit effect and the hit-stop
// (not while the game has its own type 5 request out).
void DoImpactUnsafe(std::uint8_t* lara) {
    auto* enemy = static_cast<std::uint8_t*>(g_moveTarget);
    float target[3]{};
    const bool haveTarget = enemy && TargetPosition(target);
    const auto* p = reinterpret_cast<const float*>(lara + kInstancePos);
    const float dist = haveTarget ? std::hypot(target[0] - p[0], target[1] - p[1]) : -1.0f;
    const float dz = haveTarget ? target[2] - p[2] : 0.0f;
    const bool reached = haveTarget && dist <= kStrikeReach + 10.0f && dz >= kStrikeLow - 10.0f && dz <= kStrikeHigh + 10.0f;
    int fx = -1;
    bool played = false;
    const char* stop = "off";
    if (reached && g_fxTriggerById) {
        fx = EnemyConnectFx(enemy);
        void* zone = *reinterpret_cast<void* const*>(enemy + kInstanceObjectZone);
        if (fx > 0 && zone) {
            g_fxTriggerById(enemy, zone, fx, 0, nullptr, nullptr);
            played = true;
        }
    }
    const int ms = g_hitStopMs.load();
    if (reached && ms > 0 && g_dilationCtor && g_timeDilationManager && !g_hitStopUntilMs && !g_hitStopReleaseTick) {
        const std::uint8_t* slot = g_timeDilationManager + static_cast<std::size_t>(kHitStopType) * kTimeDilationRequestSize;
        if (DilationSlotActive(slot)) {
            stop = "left out (the game's own type 5 request is out)";
        } else {
            g_hitStopPrevType = *reinterpret_cast<const std::int32_t*>(g_timeDilationManager + kDilationCurrentType);
            alignas(16) std::uint8_t request[32] = {};
            g_dilationCtor(request, kHitStopType, 1.0f, kHitStopSpeed, kHitStopName);
            g_dilationRequest(g_timeDilationManager, request);
            g_hitStopStart = *reinterpret_cast<const float*>(slot + kDilationStart);
            g_hitStopUntilMs = GetTickCount64() + static_cast<ULONGLONG>(ms);
            stop = "on";
        }
    }
    if (g_impactLogs < 12) {
        ++g_impactLogs;
        Log::Detailf("Grapple: kick frame %d: the enemy is %.0f cm away (dz %+.0f)%s; its hit effect %d %s, hit-stop %s",
                    g_info.kick, dist, dz, reached ? "" : ", out of the strike's reach", fx, played ? "played" : "not played", stop);
    }
}

// ---------- the game's rope ----------
void* EntityCall(void* entity, std::size_t slot) { return (*reinterpret_cast<void* const* const*>(entity))[slot / sizeof(void*)]; }

// The first live instance in the slots with a GrappleComponent (its component's instance pointing back), and how many.
const std::uint8_t* ScanGrappleComponent(const std::uint8_t** owner, int& found) {
    const std::uint32_t id = *g_grappleComponentId;
    const std::uint8_t* first = nullptr;
    for (std::uint32_t i = 0; i < kInstanceSlotCount; ++i) {
        const std::uint8_t* inst = g_instanceSlots + static_cast<std::size_t>(i) * kInstanceSlotSize;
        const auto handle = SafeGet<std::uint64_t>(inst + kInstanceHandle);
        if (!handle || static_cast<std::uint32_t>(handle) != i) continue;
        const auto count = SafeGet<std::uint32_t>(inst + kInstanceComponentCount);
        const auto* comps = SafeGet<const std::uint8_t*>(inst + kInstanceComponents);
        if (id >= count || count > 4096 || !PlausiblePointer(comps)) continue;
        const auto* component = SafeGet<const std::uint8_t*>(comps + static_cast<std::size_t>(id) * sizeof(void*));
        if (!PlausiblePointer(component) || SafeGet<const std::uint8_t*>(component + kComponentInstance) != inst) continue;
        ++found;
        if (!first) {
            first = component;
            *owner = inst;
        }
    }
    return first;
}

bool GrappleCompStillThere() {
    if (!g_grappleComp || !g_grappleCompInstance) return false;
    if (SafeGet<std::uint64_t>(g_grappleCompInstance + kInstanceHandle) != g_grappleCompHandle) return false;
    const std::uint32_t id = *g_grappleComponentId;
    const auto count = SafeGet<std::uint32_t>(g_grappleCompInstance + kInstanceComponentCount);
    const auto* comps = SafeGet<const std::uint8_t*>(g_grappleCompInstance + kInstanceComponents);
    return id < count && PlausiblePointer(comps) &&
           SafeGet<const std::uint8_t*>(comps + static_cast<std::size_t>(id) * sizeof(void*)) == g_grappleComp;
}

// Lara's rope look: the dtp::RopeRenderData (radius +0, material +8) of the GrappleComponent on her grapple's instance
// (Instance::Query(lara, 0x181)), else of the first one in the instance slots (scanned again every 5 s while none).
const void* LaraRopeRenderDataUnsafe(std::uint8_t* lara) {
    // Lara's grapple-axe rope: the player dtp's RopeRenderData.
    if (g_axeRopeAvailable) {
        const std::uint8_t* pc = InstanceComponent(lara, *At<const std::uint32_t*>(kRvaPlayerComponentId));
        const auto* playerDtp = pc ? *reinterpret_cast<const std::uint8_t* const*>(pc + kPlayerComponentDtp) : nullptr;
        const std::uint8_t* axe = playerDtp ? playerDtp + kPlayerAxeRopeData : nullptr;
        const float r = axe ? *reinterpret_cast<const float*>(axe) : 0.0f;
        const void* m = axe ? *reinterpret_cast<void* const*>(axe + 8) : nullptr;
        const bool good = axe && r > 0.05f && r < 20.0f && m;
        if (g_ropeLookLogs < 3 && (good || !g_ropeLookFailLogged)) {
            if (good) g_ropeLookLogs = 3;
            else g_ropeLookFailLogged = true;
            Log::Detailf("Grapple: rope look from Lara's grapple-axe rope (player dtp %p +0x9A0): radius %.2f cm, material %p%s",
                        playerDtp, static_cast<double>(r), m, good ? "" : ": not usable, trying the GrappleComponent");
        }
        if (good) return axe;
    }
    auto* grapple = static_cast<std::uint8_t*>(g_instanceQuery(lara, kQueryGrappleInstance));
    const std::uint8_t* component = grapple ? InstanceComponent(grapple, *g_grappleComponentId) : nullptr;
    const char* source = "Lara's grapple (Query 0x181)";
    int found = -1;
    if (!component && g_instanceSlots) {
        source = "the instance scan";
        if (!GrappleCompStillThere()) {
            g_grappleComp = nullptr;
            g_grappleCompInstance = nullptr;
            const ULONGLONG now = GetTickCount64();
            if (now >= g_grappleScanMs) {
                g_grappleScanMs = now + kGrappleScanRetryMs;
                found = 0;
                const std::uint8_t* owner = nullptr;
                if (const std::uint8_t* c = ScanGrappleComponent(&owner, found)) {
                    g_grappleComp = c;
                    g_grappleCompInstance = owner;
                    g_grappleCompHandle = SafeGet<std::uint64_t>(owner + kInstanceHandle);
                }
            }
        }
        component = g_grappleComp;
        grapple = const_cast<std::uint8_t*>(g_grappleCompInstance);
    }
    const auto* dtp = component ? *reinterpret_cast<const std::uint8_t* const*>(component + kGrappleComponentDtp) : nullptr;
    const std::uint8_t* data = dtp ? dtp + kGrappleRopeRenderData : nullptr;
    const float radius = data ? *reinterpret_cast<const float*>(data) : 0.0f;
    const void* material = data ? *reinterpret_cast<void* const*>(data + 8) : nullptr;
    const bool ok = data && radius > 0.05f && radius < 20.0f && material;
    if (ok ? g_ropeLookLogs < 3 && (found >= 0 || g_ropeLookLogs == 0) : !g_ropeLookFailLogged) {
        if (ok) ++g_ropeLookLogs;
        else g_ropeLookFailLogged = true;
        char scan[64] = "";
        if (found >= 0) std::snprintf(scan, sizeof(scan), ", %d instances with one", found);
        Log::Detailf("Grapple: rope look from %s%s: instance %p, GrappleComponent %p, its data %p (hook object %u), rope radius "
                    "%.2f cm, material %p%s", source, scan, grapple, component, dtp,
                    dtp ? *reinterpret_cast<const std::uint16_t*>(dtp + 8) : 0u, static_cast<double>(radius), material,
                    ok ? "" : ": not usable, the overlay draws the rope");
    }
    return ok ? data : nullptr;
}

bool UpdateGameRopeUnsafe(std::uint8_t* lara, const float from[3], const float to[3]) {
    const void* data = LaraRopeRenderDataUnsafe(lara);
    if (!data) return false;
    if (!g_ropeEntity) {
        g_ropeEntity = g_createRopeEntity(nullptr);
        if (!g_ropeEntity) return false;
        reinterpret_cast<void(__fastcall*)(void*, int)>(EntityCall(g_ropeEntity, kEntityInit))(g_ropeEntity, 2);
        if (!g_ropeLogged) {
            g_ropeLogged = true;
            Log::Detailf("Grapple: the game's rope entity %p with Lara's rope look (radius %.2f cm)", g_ropeEntity,
                        static_cast<double>(*reinterpret_cast<const float*>(data)));
        }
    }
    void* drawable = reinterpret_cast<void*(__fastcall*)(void*)>(EntityCall(g_ropeEntity, kEntityGetDrawable))(g_ropeEntity);
    if (!drawable) return false;
    alignas(16) float a[4] = {from[0], from[1], from[2], 0.0f};
    alignas(16) float b[4] = {to[0], to[1], to[2], 0.0f};
    g_setRopeRenderData(drawable, a, b, data);
    reinterpret_cast<void(__fastcall*)(void*, bool)>(EntityCall(g_ropeEntity, kEntitySetVisible))(g_ropeEntity, true);
    reinterpret_cast<void(__fastcall*)(void*)>(EntityCall(g_ropeEntity, kEntityUpdate))(g_ropeEntity);
    return true;
}

bool UpdateGameRope(std::uint8_t* lara, const float from[3], const float to[3]) {
#if defined(_MSC_VER)
    __try {
#endif
        return UpdateGameRopeUnsafe(lara, from, to);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_ropeEntity = nullptr;
        g_gameRopeAvailable = false;
        Log::Write("Grapple: the game's rope raised an exception; the rope is drawn in the overlay from now on");
        return false;
    }
#endif
}

void ReleaseGameRope() {
    if (!g_ropeEntity) return;
#if defined(_MSC_VER)
    __try {
#endif
        reinterpret_cast<void(__fastcall*)(void*, bool)>(EntityCall(g_ropeEntity, kEntitySetVisible))(g_ropeEntity, false);
        reinterpret_cast<void(__fastcall*)(void*)>(EntityCall(g_ropeEntity, kEntityRelease))(g_ropeEntity);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#endif
    g_ropeEntity = nullptr;
}

// What the overlay draws this frame: the arrow over the enemy a melee press would kick now (combat stance or on foot
// in range, or a jump), red over the one being kicked; the rope from the clip's rope hand while the kick flies. The
// same enemy gets the game's highlight.
void UpdateOverlayUnsafe(std::uint8_t* lara, bool airReady) {
    OverlayFrame f;
    const bool kicking = g_moveActive.load();
    float moveTarget[3]{};
    const bool haveMoveTarget = kicking && TargetPosition(moveTarget);
    if (g_glow.load()) {
        if (haveMoveTarget) {
            f.marker = f.markerKick = true;
            std::memcpy(f.target, moveTarget, sizeof(f.target));
        } else if (!kicking && g_target &&
                   (((g_laraClass == kClassCombat || g_laraClass == kClassGround) && g_targetInRange) || airReady)) {
            f.marker = true;
            std::memcpy(f.target, reinterpret_cast<const float*>(static_cast<const std::uint8_t*>(g_target) + kInstancePos), sizeof(f.target));
        }
    }
    UpdateDfaGlowUnsafe(!f.marker ? nullptr
                                  : haveMoveTarget ? static_cast<const std::uint8_t*>(g_moveTarget) : static_cast<const std::uint8_t*>(g_target));
    if (g_rope.load() && haveMoveTarget && g_laraState == g_kickState && g_agent) {
        const float frame = KickClipFrame(static_cast<const std::uint8_t*>(g_agent));
        const float shoot = static_cast<float>(g_info.ropeShoot), attach = static_cast<float>(g_info.ropeAttach);
        // Out until just before the kick (the clip's release frame at the earliest), then back to the hand.
        const float release = std::max(static_cast<float>(g_info.ropeRelease), static_cast<float>(g_info.kick) - 3.0f), fade = 3.0f;
        if (frame >= shoot && frame <= release + fade) {
            f.rope = true;
            f.ropeExtend = frame <= release ? std::clamp((frame - shoot) / std::max(1.0f, attach - shoot), 0.0f, 1.0f)
                                            : std::clamp(1.0f - (frame - release) / fade, 0.0f, 1.0f);
            f.ropeAlpha = frame <= release ? 1.0f : 1.0f - (frame - release) / fade;
            const auto* p = reinterpret_cast<const float*>(lara + kInstancePos);
            // The hand from the bone build when it is fresh and plausible, else a point in front of the chest.
            const int hand = g_info.ropeHand == kRightHand ? 1 : 0;
            const float h[3] = {g_handPos[hand][0].load(), g_handPos[hand][1].load(), g_handPos[hand][2].load()};
            const bool fresh = GetTickCount64() - g_handMs.load() < 250;
            const float off = std::hypot(h[0] - p[0], h[1] - p[1], h[2] - p[2]);
            if (fresh && !g_handChecked) {
                g_handChecked = true;
                g_handWorld = std::isfinite(off) && off < 300.0f;
                Log::Detailf("Grapple: rope hand bone %.0f cm from Lara: %s", off, g_handWorld ? "world space, used" : "not world space, the chest estimate is used");
            }
            if (fresh && g_handWorld && off < 300.0f) {
                std::memcpy(f.ropeFrom, h, sizeof(h));
            } else {
                const float dx = moveTarget[0] - p[0], dy = moveTarget[1] - p[1], d = std::max(1.0f, std::hypot(dx, dy));
                f.ropeFrom[0] = p[0] + dx / d * 30.0f;
                f.ropeFrom[1] = p[1] + dy / d * 30.0f;
                f.ropeFrom[2] = p[2] + 135.0f;
            }
            f.ropeTo[0] = moveTarget[0];
            f.ropeTo[1] = moveTarget[1];
            f.ropeTo[2] = moveTarget[2] + 110.0f;
        }
    }
    // The game's own rope, out and back; the overlay's line only for a kick where the game rope could not be had.
    bool gameRope = false;
    if (f.rope && g_gameRopeAvailable && f.ropeExtend > 0.02f) {
        float tip[3];
        for (int c = 0; c < 3; ++c) tip[c] = f.ropeFrom[c] + (f.ropeTo[c] - f.ropeFrom[c]) * f.ropeExtend;
        gameRope = UpdateGameRope(lara, f.ropeFrom, tip);
        if (gameRope) g_kickUsedGameRope = true;
    }
    if (g_kickUsedGameRope) f.rope = false;
    if (!gameRope) ReleaseGameRope();
    const bool on = f.marker || f.rope;
    if (on && !Features::SnapshotDrawCameraNow()) {
        if (g_overlayCameraOk) Log::Write("Grapple: the draw tools' camera is unavailable; no arrow or rope drawn");
        g_overlayCameraOk = false;
    }
    {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        g_overlay = f;
    }
    g_overlayOn.store(on && g_overlayCameraOk);
}

// Starts a kick at the current target: writes the zip path into the next clip copy, points the kick entry at it,
// makes the animation the motion source and runs the melee transition switched to combat_melee_kick.
bool StartKick(void* agent, Grapple::DoTransitionFn original, bool fromAir) {
    Clip& clip = g_clips[g_nextClip];
    g_nextClip ^= 1;
    const float stop = g_info.reach - kBodyRadius + static_cast<float>(g_margin.load());
    const float travel = std::max(0.0f, g_targetDist - stop);
    // Height: to the enemy's feet level, with an arc over ledge lips when climbing (none from a jump).
    const float rise = g_kickPropsOn ? g_targetDz : 0.0f;
    float hump = 0.0f;
    if (!fromAir && rise > 50.0f) hump = std::clamp(0.25f * rise + 0.05f * g_targetDist, 40.0f, 200.0f);
    else if (!fromAir && rise < -50.0f) hump = std::clamp(0.06f * g_targetDist, 20.0f, 80.0f);
    const bool throughTerrain = g_kickPropsOn && (fromAir || std::fabs(rise) > 75.0f);
    bool ok = false;
#if defined(_MSC_VER)
    __try {
#endif
        SetZipPathUnsafe(clip, travel, clip.rootZ ? rise : 0.0f, clip.rootZ ? hump : 0.0f);
        if (g_terrainProp) *g_terrainProp = throughTerrain ? 1 : 0;
        if (AnimEntry* entry = CurrentEntryUnsafe()) {
            WriteEntry(entry, static_cast<std::int16_t>(clip.id), static_cast<std::int16_t>(clip.id),
                       g_sounds.load() && g_kickFx ? g_kickFx : g_entryOriginal.fx);
            g_entryWritten = true;
            ok = true;
        }
        // Bullets do nothing while the kick runs; an invincibility that runs forever (the framework's) stays.
        if (ok && g_invincible.load() && g_setInvincible && g_tryToGetHealth && g_lara) {
            if (auto* health = static_cast<std::uint8_t*>(g_tryToGetHealth(g_lara))) {
                const auto* inv = *reinterpret_cast<const std::uint8_t* const*>(health + kHealthInvincibility);
                if (inv && *reinterpret_cast<const std::int64_t*>(inv + kInvincibleEnd) >= 0) {
                    g_setInvincible(health, kInvincibleMs);
                    if (!g_invincibleLogged) {
                        g_invincibleLogged = true;
                        Log::Detailf("Grapple: Lara invincible for %d ms at each kick (HealthComponent %p)", kInvincibleMs, health);
                    }
                }
            }
        }
        // The Cheat moveset reaches Combat_Kick from anywhere while T63 / T69 point there (see the top). Edges that are
        // not what they were (other graph data) drop the route: the moveset then stays out of the kick's properties.
        if (ok) {
            if (g_routeReady && !RouteValid(g_route)) {
                g_routeReady = false;
                g_route = Route{};
                g_routePatched = false;
                g_routeLookupDue = true;
                Log::Detail("Grapple: the graph route's edges changed; looked up again after this kick");
            }
            ApplyKickPropsUnsafe(true);
            if (g_routeReady && g_haveMovesetProp && g_kickPropsApplied == g_kickPropCount) PatchRoute(true);
        }
        if (ok && g_lara) {
            const auto* p = reinterpret_cast<const float*>(static_cast<std::uint8_t*>(g_lara) + kInstancePos);
            g_moveStartPos[0] = p[0];
            g_moveStartPos[1] = p[1];
            g_moveStartPos[2] = p[2];
            // The combat stance moves Lara itself (motion source 2); the kick's zip is root motion.
            g_moveAnim = nullptr;
            g_moveMotionBefore = -1;
            if (auto* anim = static_cast<std::uint8_t*>(g_getAnimComponent(g_lara))) {
                g_moveMotionBefore = *reinterpret_cast<const std::int32_t*>(anim + kAnimMotionType);
                SetAnimMotionUnsafe(anim);
                g_moveAnim = anim;
            }
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
#endif
    if (!ok) return false;
    g_moveTarget = g_target;
    g_moveHandle = g_targetHandle;
    g_moveDist = g_targetDist;
    g_moveTravel = travel;
    g_moveRise = g_targetDz;
    g_moveFromAir = fromAir;
    g_moveStartMs = GetTickCount64();
    g_impactDone = false;
    g_attachDone = false;
    g_clipClock = 0.0f;
    g_clipClockMs = g_moveStartMs;
    g_kickSpeedMin = 1.0f;
    g_kickUsedGameRope = false;
    g_kickStateSeen = false;
    g_kickStateGone = 0;
    g_invincibleRefreshMs = g_moveStartMs + kInvincibleRefreshMs;
    g_moveWorldSpeed = g_timeDilationManager ? *reinterpret_cast<const float*>(g_timeDilationManager + 0x11C) : 1.0f;
    g_moveGlobalSpeed = g_timeDilationManager ? *reinterpret_cast<const float*>(g_timeDilationManager + 0x120) : 1.0f;
    if (std::uint8_t* dtp = KickFragmentDtp()) {
        if (dtp != g_kickFragmentDtp) {
            g_kickFragmentDtp = MakeWritable(dtp + kFragmentPlaySpeed, sizeof(float)) ? dtp : nullptr;
            g_kickPlaySpeedGame = g_kickFragmentDtp ? *reinterpret_cast<const float*>(dtp + kFragmentPlaySpeed) : 1.0f;
            if (!(g_kickPlaySpeedGame > 0.05f && g_kickPlaySpeedGame < 10.0f)) g_kickPlaySpeedGame = 1.0f;
            g_kickPlaySpeed = g_kickPlaySpeedGame;
        }
        UpdateKickPlaySpeedUnsafe();
    } else if (g_kickFragmentDtp) {
        g_kickFragmentDtp = nullptr;  // other graph data: the old one is not touched again
        g_kickPlaySpeed = g_kickPlaySpeedGame = 1.0f;
    }
    if (g_move && g_movePatched) g_move[kMoveVolumeOn] = 0;  // on at the clip's kick frame (the tick)
    SetKickDamageUnsafe(static_cast<std::uint8_t*>(g_moveTarget), "at the start");  // the foot prims hit from kick - 3
    g_airArmed.store(false);
    g_aimArmed.store(false);
    g_moveActive.store(true);
    g_kicks.fetch_add(1);
    if (fromAir) g_airKicks.fetch_add(1);
    const char* from = NameOf(g_laraState);

    // Run the melee press with its target switched to combat_melee_kick for this call.
    auto* slot = reinterpret_cast<volatile std::uint16_t*>(g_entryTransition + kTransitionTarget);
    const std::uint16_t saved = *slot;
    *slot = static_cast<std::uint16_t>(g_kickIndex);
    original(agent, g_entryTransition);
    *slot = saved;
    Log::Detailf("Grapple: kick #%d%s%s: target %p (%s) %.0f cm away, dz %+.0f; zip %.0f cm, rise %+.0f, arc %.0f, terrain "
                "collision %s, clip id %d (%s), motion source %d -> %d",
                g_kicks.load(), fromAir ? " from the jump state " : " from ", from, g_moveTarget, g_targetSource, g_moveDist, g_targetDz,
                travel, clip.rootZ ? rise : 0.0f, clip.rootZ ? hump : 0.0f, throughTerrain ? "off" : "on", clip.id,
                g_routePatched ? "Cheat route to Combat_Kick" : "weapon stance route to Combat_Kick", g_moveMotionBefore,
                g_moveAnim ? kMotionFromAnimation : -1);
    return true;
}

}  // namespace

namespace Grapple {
bool Initialize() {
    if (!GameBuild::IsSupportedRetail()) return false;
    g_base = GameBuild::Base();
    LoadSettings();
    bool ok = Verify(kRvaGetLocalPlayerInstance, kGetLocalPlayerInstanceSig, sizeof(kGetLocalPlayerInstanceSig), "GetLocalPlayerInstance") &&
              Verify(kRvaGetTargetInstance, kGetTargetInstanceSig, sizeof(kGetTargetInstanceSig), "CombatManager::GetTargetInstance") &&
              Verify(kRvaTryToGetMeleeMoveComponent, kTryToGetMeleeMoveComponentSig, sizeof(kTryToGetMeleeMoveComponentSig), "TryToGetMeleeMoveComponent") &&
              Verify(kRvaTryToGetNpcComponent, kTryToGetNpcComponentSig, sizeof(kTryToGetNpcComponentSig), "TryToGetNpcComponent") &&
              Verify(kRvaMeleeMoveDtpLoad, kMeleeMoveDtpLoadSig, sizeof(kMeleeMoveDtpLoadSig), "MeleeMoveComponent melee data offset") &&
              Verify(kRvaCombatManagerLoad, kCombatManagerLoadSig, sizeof(kCombatManagerLoadSig), "Player CombatManager offset") &&
              Verify(kRvaGetAnimComponent, kGetAnimComponentSig, sizeof(kGetAnimComponentSig), "Instance::GetAnimComponentV2") &&
              Verify(kRvaSetMotionSourcesType, kSetMotionSourcesTypeSig, sizeof(kSetMotionSourcesTypeSig), "AnimComponentV2 motion type offset") &&
              Verify(kRvaSetMotionSourcesFlags, kSetMotionSourcesFlagsSig, sizeof(kSetMotionSourcesFlagsSig), "AnimComponentV2 root motion flags offset") &&
              Verify(kRvaResolveAnimation, kResolveAnimationSig, sizeof(kResolveAnimationSig), "ANITRACKER_ResolveAnimation") &&
              Verify(kRvaGetKeylistRegistry, kGetKeylistRegistrySig, sizeof(kGetKeylistRegistrySig), "GetKeylist registry load");
    if (ok && RelTarget(kRvaPlayerComponentIdUse, 3, 7) != kRvaPlayerComponentId) {
        Log::Write("Grapple: PlayerComponent id reference mismatch; grapple kick unavailable");
        ok = false;
    }
    if (ok && RelTarget(kRvaGetKeylistRegistry, 3, 7) != kRvaRegistry) {
        Log::Write("Grapple: animation library registry mismatch; grapple kick unavailable");
        ok = false;
    }
    if (ok && !CombatAir::TransitionHookActive()) {
        Log::Write("Grapple: the state control transition hook is not installed; grapple kick unavailable");
        ok = false;
    }
    g_available = ok;
    if (ok) {
        g_getLocalPlayerInstance = At<GetLocalPlayerInstanceFn>(kRvaGetLocalPlayerInstance);
        g_getTargetInstance = At<GetTargetInstanceFn>(kRvaGetTargetInstance);
        g_tryToGetMeleeMove = At<TryToGetFn>(kRvaTryToGetMeleeMoveComponent);
        g_tryToGetNpc = At<TryToGetFn>(kRvaTryToGetNpcComponent);
        g_getAnimComponent = At<TryToGetFn>(kRvaGetAnimComponent);
        g_refCounts = At<std::uint16_t*>(kRvaRefCounts);
        g_animByNumber = At<void**>(kRvaAnimByNumber);
        g_registry = At<std::uint8_t* const*>(kRvaRegistry);
        // Lock-on and the jump kick: the NPC list, enemies, the camera and the melee input.
        g_lockOnAvailable =
            VerifyQuiet(kRvaInputActionRequested, kInputActionRequestedSig, sizeof(kInputActionRequestedSig), "INPUT_ActionRequested") &&
            VerifyQuiet(kRvaIsEnemyOf, kIsEnemyOfSig, sizeof(kIsEnemyOfSig), "NpcComponent::IsEnemyOf") &&
            VerifyQuiet(kRvaNpcListLoad, kMovRbxRipSig, sizeof(kMovRbxRipSig), "NpcComponent list load") &&
            VerifyQuiet(kRvaNpcListInstanceUse, kNpcListInstanceSig, sizeof(kNpcListInstanceSig), "NpcComponent instance offset") &&
            VerifyQuiet(kRvaNpcListNextUse, kNpcListNextSig, sizeof(kNpcListNextSig), "NpcComponent list link offset") &&
            VerifyQuiet(kRvaNpcDeadUse, kNpcDeadSig, sizeof(kNpcDeadSig), "NpcComponent dead flag") &&
            VerifyQuiet(kRvaCameraForward, kCameraForwardSig, sizeof(kCameraForwardSig), "CameraManager::GetActiveCamera_Forward") &&
            VerifyQuiet(kRvaCameraPosition, kCameraPositionSig, sizeof(kCameraPositionSig), "CameraManager::GetActiveCamera_Position") &&
            VerifyQuiet(kRvaCameraPosition + 0x43, kCameraPositionReadSig, sizeof(kCameraPositionReadSig), "camera position read") &&
            VerifyQuiet(kRvaCameraManagerLoad, kMovRcxRipSig, sizeof(kMovRcxRipSig), "CameraManager::m_pCameraManager") &&
            VerifyQuiet(kRvaCameraForwardCall, kCallRel32Sig, sizeof(kCallRel32Sig), "Turn_CameraDir forward call");
        if (g_lockOnAvailable && (RelTarget(kRvaNpcListLoad, 3, 7) != kRvaNpcList ||
                                  RelTarget(kRvaCameraForwardCall, 1, 5) != kRvaCameraForward)) {
            Log::Write("Grapple: NPC list / camera reference mismatch; lock-on and the jump kick unavailable");
            g_lockOnAvailable = false;
        }
        // The game's highlight on the target, and the moveset goal after a kick.
        g_dfaGlowAvailable = GameBuild::VerifyBytes(kRvaNpcDfaHighlightUse, kNpcDfaHighlightSig, sizeof(kNpcDfaHighlightSig)) &&
                             GameBuild::VerifyBytes(kRvaEnableNonSiHighlight, kEnableNonSiHighlightSig, sizeof(kEnableNonSiHighlightSig)) &&
                             RelTarget(kRvaNpcDfaHighlightCall, 1, 5) == kRvaEnableNonSiHighlight;
        if (!g_dfaGlowAvailable) Log::Write("Grapple: NpcComponent death-from-above highlight check failed; only the arrow marks the target");
        if (GameBuild::VerifyBytes(kRvaSetAnimGraphGoalBody, kSetAnimGraphGoalBodySig, sizeof(kSetAnimGraphGoalBodySig)))
            g_setAnimGraphGoal = At<SetAnimGraphGoalFn>(kRvaSetAnimGraphGoal);
        else
            Log::Write("Grapple: AnimComponentV2::SetAnimGraphGoal check failed; no moveset repair after a kick");
        if (GameBuild::VerifyBytes(kRvaTryToGetHealthComponent, kTryToGetHealthSig, sizeof(kTryToGetHealthSig)) &&
            GameBuild::VerifyBytes(kRvaTryToGetHealthComponent + 0x0E, kTryToGetHealthTailSig, sizeof(kTryToGetHealthTailSig)) &&
            GameBuild::VerifyBytes(kRvaSetInvincible, kSetInvincibleSig, sizeof(kSetInvincibleSig)) &&
            GameBuild::VerifyBytes(kRvaSetInvincibleBody, kSetInvincibleBodySig, sizeof(kSetInvincibleBodySig))) {
            g_tryToGetHealth = At<TryToGetFn>(kRvaTryToGetHealthComponent);
            g_setInvincible = At<SetInvincibleFn>(kRvaSetInvincible);
            g_healthReadable = GameBuild::VerifyBytes(kRvaAdjustHealthValues, kAdjustHealthValuesSig, sizeof(kAdjustHealthValuesSig)) &&
                               GameBuild::VerifyBytes(kRvaAdjustHealthCurrent, kAdjustHealthCurrentSig, sizeof(kAdjustHealthCurrentSig)) &&
                               GameBuild::VerifyBytes(kRvaAdjustHealthMax, kAdjustHealthMaxSig, sizeof(kAdjustHealthMaxSig));
            if (!g_healthReadable) Log::Write("Grapple: HealthComponent health offsets check failed; the kick's damage is not capped");
        } else {
            Log::Write("Grapple: HealthComponent::SetInvincible check failed; Lara can be hurt during the kick");
        }
        g_gameRopeAvailable =
            GameBuild::VerifyBytes(kRvaCreateRopeEntity, kCreateRopeEntitySig, sizeof(kCreateRopeEntitySig)) &&
            GameBuild::VerifyBytes(kRvaSetRopeRenderData, kSetRopeRenderDataSig, sizeof(kSetRopeRenderDataSig)) &&
            GameBuild::VerifyBytes(kRvaVisualRopeInit, kVisualRopeInitSig, sizeof(kVisualRopeInitSig)) &&
            GameBuild::VerifyBytes(kRvaVisualRopeRelease, kVisualRopeReleaseSig, sizeof(kVisualRopeReleaseSig)) &&
            GameBuild::VerifyBytes(kRvaVisualRopeUpdate, kVisualRopeUpdateSig, sizeof(kVisualRopeUpdateSig)) &&
            GameBuild::VerifyBytes(kRvaVisualRopeUpdateTail, kVisualRopeUpdateTailSig, sizeof(kVisualRopeUpdateTailSig)) &&
            GameBuild::VerifyBytes(kRvaGrappleRopeIdUse, kGrappleRopeIdSig, sizeof(kGrappleRopeIdSig)) &&
            GameBuild::VerifyBytes(kRvaGrappleRopeDtpUse, kGrappleRopeDtpSig, sizeof(kGrappleRopeDtpSig)) &&
            GameBuild::VerifyBytes(kRvaGrappleRopeDataUse, kGrappleRopeDataSig, sizeof(kGrappleRopeDataSig)) &&
            RelTarget(kRvaVisualRopeCreateCall, 1, 5) == kRvaCreateRopeEntity &&
            RelTarget(kRvaVisualRopeUpdateCall, 1, 5) == kRvaSetRopeRenderData &&
            RelTarget(kRvaGrappleRopeIdUse, 2, 6) == kRvaGrappleComponentId &&
            GameBuild::VerifyBytes(kRvaInstanceQuery, kInstanceQuerySig, sizeof(kInstanceQuerySig)) &&
            GameBuild::VerifyBytes(kRvaInstanceQuery + 0x0A, kInstanceQueryTailSig, sizeof(kInstanceQueryTailSig)) &&
            GameBuild::VerifyBytes(kRvaGrappleQueryUse, kGrappleQuerySig, sizeof(kGrappleQuerySig)) &&
            GameBuild::VerifyBytes(kRvaGrappleQueryIdUse, kGrappleQueryIdSig, sizeof(kGrappleQueryIdSig)) &&
            RelTarget(kRvaGrappleQueryCall, 1, 5) == kRvaInstanceQuery &&
            RelTarget(kRvaGrappleQueryIdUse, 2, 6) == kRvaGrappleComponentId;
        if (g_gameRopeAvailable) {
            g_instanceQuery = At<InstanceQueryFn>(kRvaInstanceQuery);
            if (RelTarget(kRvaInstanceQuery + 3, 3, 7) == kRvaInstanceSlots + kInstanceComponents)
                g_instanceSlots = At<std::uint8_t*>(kRvaInstanceSlots);
            else
                Log::Write("Grapple: instance slot array check failed; no GrappleComponent scan");
            g_axeRopeAvailable = GameBuild::VerifyBytes(kRvaAxeRopeDtpUse, kAxeRopeDtpSig, sizeof(kAxeRopeDtpSig)) &&
                                 GameBuild::VerifyBytes(kRvaAxeRopeDataUse, kAxeRopeDataSig, sizeof(kAxeRopeDataSig)) &&
                                 RelTarget(kRvaAxeRopeDataCall, 1, 5) == kRvaSetRopeRenderData;
            if (!g_axeRopeAvailable) Log::Write("Grapple: grapple-axe rope look check failed; the rope look comes from a GrappleComponent");
            g_createRopeEntity = At<CreateRopeEntityFn>(kRvaCreateRopeEntity);
            g_setRopeRenderData = At<SetRopeRenderDataFn>(kRvaSetRopeRenderData);
            g_grappleComponentId = At<const std::uint32_t*>(kRvaGrappleComponentId);
        } else {
            Log::Write("Grapple: the game's rope checks failed; the rope is drawn in the overlay");
        }
        g_fragmentReadable = GameBuild::VerifyBytes(kRvaFragmentActivate, kFragmentActivateSig, sizeof(kFragmentActivateSig)) &&
                             GameBuild::VerifyBytes(kRvaFragmentUpdate, kFragmentUpdateSig, sizeof(kFragmentUpdateSig));
        if (!g_fragmentReadable) Log::Write("Grapple: AnimFragmentNode layout check failed; the kick keeps the game's play speed in the slow motion");
        if (GameBuild::VerifyBytes(kRvaFxTriggerById, kFxTriggerByIdSig, sizeof(kFxTriggerByIdSig)) &&
            GameBuild::VerifyBytes(kRvaStrikeConnectFxUse, kStrikeConnectFxSig, sizeof(kStrikeConnectFxSig)) &&
            RelTarget(kRvaStrikeConnectFxCall, 1, 5) == kRvaFxTriggerById)
            g_fxTriggerById = At<FxTriggerByIdFn>(kRvaFxTriggerById);
        else
            Log::Write("Grapple: FX_GenericTriggerFXIDEffects check failed; no enemy hit effect at the kick");
        if (GameBuild::VerifyBytes(kRvaTimeDilationRequestCtor, kTimeDilationRequestCtorSig, sizeof(kTimeDilationRequestCtorSig)) &&
            GameBuild::VerifyBytes(kRvaTimeDilationRequest, kTimeDilationRequestSig, sizeof(kTimeDilationRequestSig)) &&
            GameBuild::VerifyBytes(kRvaTimeDilationCancel, kTimeDilationCancelSig, sizeof(kTimeDilationCancelSig)) &&
            GameBuild::VerifyBytes(kRvaSetTimeDilationManager, kLeaRcxRipSig, sizeof(kLeaRcxRipSig))) {
            g_dilationCtor = At<TimeDilationRequestCtorFn>(kRvaTimeDilationRequestCtor);
            g_dilationRequest = At<TimeDilationRequestFn>(kRvaTimeDilationRequest);
            g_dilationCancel = At<TimeDilationCancelFn>(kRvaTimeDilationCancel);
            g_timeDilationManager = At<std::uint8_t*>(RelTarget(kRvaSetTimeDilationManager, 3, 7));
        } else {
            Log::Write("Grapple: time dilation checks failed; no hit-stop at the kick");
        }
        if (g_lockOnAvailable) {
            g_inputRequested = At<InputActionRequestedFn>(kRvaInputActionRequested);
            g_isEnemyOf = At<IsEnemyOfFn>(kRvaIsEnemyOf);
            g_npcList = At<std::uint8_t* const*>(kRvaNpcList);
            g_cameraForward = At<CameraVectorFn>(kRvaCameraForward);
            g_cameraPosition = At<CameraVectorFn>(kRvaCameraPosition);
            g_cameraManager = At<void* const*>(RelTarget(kRvaCameraManagerLoad, 3, 7));
        }
    }
    Log::Detailf("Grapple: %s; grapple kick %s (range %d..%d cm, height %d cm, damage %d%s, reaction %s, lock-on %s, glow %s, "
                "jump kick %s%s, clip %s)", ok ? "ready" : "unavailable", g_enabled.load() ? "on" : "off", g_minRange.load(),
                g_maxRange.load(), g_maxHeight.load(), g_damage.load(), g_lethal.load() ? " can kill" : "",
                kReactionNames[g_reaction.load()], !g_lockOnAvailable ? "unavailable" : g_lockOn.load() ? "on" : "off",
                g_glow.load() ? "on" : "off", !g_lockOnAvailable ? "unavailable" : g_airKick.load() ? "on" : "off",
                g_airNeedsAim.load() ? " with aim" : "", g_clipFile);
    return ok;
}

void TickGameThread() {
    if (!g_available) return;
#if defined(_MSC_VER)
    __try {
#endif
        TickUnsafe();
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_targetInRange = g_targetInRangeAir = false;
        g_airArmed.store(false);
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            Log::Write("Grapple: tick raised an exception; skipped");
        }
    }
#endif
}

bool RedirectTransition(void* agent, const std::uint8_t* transition, DoTransitionFn original) {
    if (!g_available || !agent || !transition || agent != g_agent || !g_scReady) return false;
    // A running kick ends when its state is left.
    if (g_moveActive.load()) {
        const std::uint16_t target = *reinterpret_cast<const std::uint16_t*>(transition + kTransitionTarget);
        const auto* current = *reinterpret_cast<const std::uint8_t* const*>(static_cast<std::uint8_t*>(agent) + kAgentCurrentState);
        if (current == g_kickState && target != 0xFFFF && target != static_cast<std::uint16_t>(g_kickIndex)) {
            const char* name = *reinterpret_cast<const char* const*>(transition + kTransitionName);
            char why[128];
            std::snprintf(why, sizeof(why), "left via '%s' at state frame %.0f, clip frame %.0f", name ? name : "?",
                          static_cast<double>(*reinterpret_cast<const float*>(static_cast<std::uint8_t*>(agent) + kAgentFramesInState)),
                          static_cast<double>(KickClipFrame(static_cast<const std::uint8_t*>(agent))));
            EndMove(why);
        }
    }
    // The transition ForceTransition let through in a jump or while aiming: always ours (its own conditions failed).
    const std::uint8_t* forced = g_forcedTransition.load();
    if (forced && transition == forced) {
        g_forcedTransition.store(nullptr);
        const bool fromAir = g_forcedFromAir.load();
        g_airArmed.store(false);
        g_aimArmed.store(false);
        const bool ready = g_enabled.load() && g_clipsLoaded && g_entryReady && g_movePatched && !g_moveActive.load() &&
                           (fromAir ? g_targetInRangeAir : g_targetInRange) && g_target;
        const char* name = *reinterpret_cast<const char* const*>(transition + kTransitionName);
        if (!ready || !StartKick(agent, original, fromAir))
            Log::Detailf("Grapple: %s dropped at '%s' (target gone or not ready)", fromAir ? "jump kick" : "aimed kick", name ? name : "?");
        return true;
    }
    // The melee press in the combat stance (T1644) or on foot (T1711): the kick runs as T1644 switched to [833].
    if (transition != g_entryTransition && (!g_groundEntryTransition || transition != g_groundEntryTransition)) return false;
    if (Brawl::ClaimsMeleePress()) return false;  // an enemy inside the hand-to-hand range: the press is the brawl's (or the game's melee)
    if (!g_enabled.load() || !g_clipsLoaded || !g_entryReady || !g_movePatched) return false;
    if (!g_targetInRange || g_targetTick != g_tick || !g_target) return false;
    if (GetTickCount64() < g_lastKickEndMs + kKickCooldownMs) return false;  // the graph is still leaving the last one
    return StartKick(agent, original, false);
}

bool ForceTransition(void* agent, const std::uint8_t* transition) {
    const bool air = g_airArmed.load(), aimed = g_aimArmed.load();
    if ((!air && !aimed) || !agent || agent != g_agent || !transition || g_moveActive.load()) return false;
    if (transition == g_entryTransition || transition == g_exitTransition) return false;
#if defined(_MSC_VER)
    __try {
#endif
        const auto* current = *reinterpret_cast<const std::uint8_t* const*>(static_cast<std::uint8_t*>(agent) + kAgentCurrentState);
        const StateClass cls = ClassOf(current);
        if (air ? cls != kClassAir : (cls != kClassCombat && cls != kClassGround)) return false;
        const std::uint8_t* expected = nullptr;
        if (!g_forcedTransition.compare_exchange_strong(expected, transition)) return transition == g_forcedTransition.load();
        g_forcedFromAir.store(air);
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

void OnLaraTransforms(std::uint32_t count, const float* out) {
    if (!g_available || !out || !g_rope.load() || !g_moveActive.load()) return;
#if defined(_MSC_VER)
    __try {
#endif
        const int hands[2] = {kLeftHand, kRightHand};
        for (int k = 0; k < 2; ++k) {
            if (static_cast<std::uint32_t>(hands[k]) >= count) continue;
            const float* m = out + static_cast<std::size_t>(hands[k]) * 16;
            for (int c = 0; c < 3; ++c) g_handPos[k][c].store(m[12 + c], std::memory_order_relaxed);
        }
        g_handMs.store(GetTickCount64(), std::memory_order_relaxed);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#endif
}

bool OverlayActive() {
    return g_available && g_overlayOn.load(std::memory_order_relaxed);
}

void DrawOverlay() {
    if (!OverlayActive()) return;
    OverlayFrame f;
    {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        f = g_overlay;
    }
    Features::DrawCameraState camera{};
    ViewBasis basis;
    if (!Features::GetDrawCameraState(camera) || !BuildViewBasis(camera, basis)) return;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    if (!draw) return;
    if (f.marker) DrawMarker(draw, basis, f);
    if (f.rope) DrawRope(draw, basis, f);
}

bool AllowTransition(void* agent, const std::uint8_t* transition) {
    if (!g_moveActive.load() || agent != g_agent || transition != g_exitTransition) return true;
    return KickClipFrame(static_cast<const std::uint8_t*>(agent)) >= static_cast<float>(g_info.frames - 2);
}

bool FacingOverride(float& dirX, float& dirY, float& rateDegPerSec) {
    if (!g_moveActive.load() || !g_lara || !g_agent) return false;
    float target[3]{};
    if (!TargetPosition(target)) return false;
#if defined(_MSC_VER)
    __try {
#endif
        const auto* agent = static_cast<const std::uint8_t*>(g_agent);
        if (*reinterpret_cast<const std::uint8_t* const*>(agent + kAgentCurrentState) != g_kickState) return false;
        // Face the target until the kick; after it the move plays out as it is.
        if (KickClipFrame(agent) > static_cast<float>(g_info.kick)) return false;
        const auto* p = reinterpret_cast<const float*>(static_cast<const std::uint8_t*>(g_lara) + kInstancePos);
        const float dx = target[0] - p[0], dy = target[1] - p[1];
        const float length = std::hypot(dx, dy);
        if (!std::isfinite(length) || length < 1.0f) return false;
        dirX = dx / length;
        dirY = dy / length;
        rateDegPerSec = static_cast<float>(g_turnRate.load());
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

void DrawPlayerSection() {
    // (v0.9.519: its title is the Enhancements tab's collapsing header)
    if (!g_available) ImGui::BeginDisabled();
    bool enabled = g_enabled.load();
    if (ImGui::Checkbox("Grapple kick: melee with an enemy at range", &enabled)) {
        g_enabled.store(enabled);
        SaveSettings();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Press melee while the targeted enemy is between the minimum and maximum range (on foot, with a weapon out,\n"
            "or in a jump): Lara shoots her grapple line, flies to the enemy (up or down to their height) and kicks\n"
            "(Black Widow's grapple strike, her animation 15688). Closer enemies keep the game's own melee and finishers.\n"
            "The move is the game's cut combat_melee_kick state with its unused 'Kick' melee move; the clip is\n"
            "ROTTRFramework\\grapple\\<clip>.");
    }
    if (enabled) {
        ImGui::Indent();
        int minRange = g_minRange.load(), maxRange = g_maxRange.load();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::DragIntRange2("Range (cm)", &minRange, &maxRange, 10.0f, 150, 4000)) {
            g_minRange.store(std::clamp(minRange, 150, 3000));
            g_maxRange.store(std::clamp(maxRange, g_minRange.load() + 50, 4000));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        int maxHeight = g_maxHeight.load();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderInt("Height difference, up or down (cm)", &maxHeight, 20, 1500)) g_maxHeight.store(maxHeight);
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        bool lockOn = g_lockOn.load();
        if (!g_lockOnAvailable) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Lock on to the enemy nearest the camera's aim", &lockOn)) {
            g_lockOn.store(lockOn);
            SaveSettings();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Without an aimed target the kick goes to the living enemy nearest the middle of the screen, within\n"
                              "the cone and the range (the one it has keeps it unless another is clearly nearer the aim).");
        if (lockOn) {
            int cone = g_lockCone.load();
            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::SliderInt("Lock-on cone (degrees)", &cone, 3, 60)) g_lockCone.store(cone);
            if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        }
        bool air = g_airKick.load();
        if (ImGui::Checkbox("From a jump: melee in the air", &air)) {
            g_airKick.store(air);
            SaveSettings();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("In a jump or a fall, press melee (Y) with an enemy in range: Lara flies to them and kicks.");
        if (air) {
            bool needsAim = g_airNeedsAim.load();
            if (ImGui::Checkbox("Only while aiming (LT held)", &needsAim)) {
                g_airNeedsAim.store(needsAim);
                SaveSettings();
            }
            int airMin = g_airMinRange.load();
            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::SliderInt("Jump kick minimum range (cm)", &airMin, 100, 1500)) g_airMinRange.store(airMin);
            if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        }
        if (!g_lockOnAvailable) ImGui::EndDisabled();
        bool glow = g_glow.load();
        if (ImGui::Checkbox("Highlight the targeted enemy (the game's glow and an arrow)", &glow)) {
            g_glow.store(glow);
            SaveSettings();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The game's own death-from-above highlight on the enemy, and an arrow over its head (amber when\n"
                              "a melee press would start the kick, red while it runs).");
        bool rope = g_rope.load();
        if (ImGui::Checkbox("Grapple rope (the game's own rope)", &rope)) {
            g_rope.store(rope);
            SaveSettings();
        }
        bool invincible = g_invincible.load();
        if (!g_setInvincible) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Lara cannot be hurt during the kick", &invincible)) {
            g_invincible.store(invincible);
            SaveSettings();
        }
        if (!g_setInvincible) ImGui::EndDisabled();
        bool sounds = g_sounds.load();
        if (ImGui::Checkbox("Sounds (axe-grapple throw, whoosh, kick)", &sounds)) {
            g_sounds.store(sounds);
            SaveSettings();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Lara's own sounds, copied from her animations: the axe-grapple throw as the rope flies and its\n"
                              "clank as it reaches the enemy, the dodge whoosh as she is pulled, her kick's whoosh and shout\n"
                              "and the thud of a kick at the hit.");
        bool impact = g_impact.load();
        if (ImGui::Checkbox("Kick impact (enemy hit effect, hit-stop, camera shake)", &impact)) {
            g_impact.store(impact);
            SaveSettings();
            g_fxRebuild.store(true);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("When the foot lands, with the enemy within reach: the enemy's own melee-hit effect (what it plays\n"
                              "when its weapon hits Lara), a short hit-stop, a camera shake and a body thud.");
        if (impact) {
            int hitStop = g_hitStopMs.load();
            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::SliderInt("Hit-stop (ms, 0 = none)", &hitStop, 0, 300)) g_hitStopMs.store(hitStop);
            if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        }
        const char* clipFiles[] = {kDefaultClip, kClip2188, kTestClip};
        const char* clipNames[] = {"Black Widow: line, pull, flip kick (15688)", "Black Widow: spinning back kick (2188)",
                                   "Lara's own kick with a glide"};
        int clipChoice = 0;
        for (int k = 0; k < 3; ++k)
            if (_stricmp(g_clipFile, clipFiles[k]) == 0) clipChoice = k;
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::Combo("Animation", &clipChoice, clipNames, 3)) {
            strcpy_s(g_clipFile, clipFiles[clipChoice]);
            SaveSettings();
            g_reloadRequested.store(true);
        }
        int damage = g_damage.load();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderInt("Kick damage", &damage, 0, 1000)) g_damage.store(damage);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            SaveSettings();
            g_patchDirty.store(true);
        }
        bool lethal = g_lethal.load();
        if (ImGui::Checkbox("The kick can kill", &lethal)) {
            g_lethal.store(lethal);
            SaveSettings();
            g_patchDirty.store(true);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Off: the kick's damage is capped so the enemy keeps at least a tenth of its health; a weak enemy\n"
                              "still reels back from it.");
        int reaction = g_reaction.load();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::Combo("Enemy reaction", &reaction, kReactionNames, static_cast<int>(Reaction::Count))) {
            g_reaction.store(reaction);
            SaveSettings();
            g_patchDirty.store(true);
        }
        int turn = g_turnRate.load();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderInt("Turn speed (deg/s)", &turn, 180, 5000)) g_turnRate.store(turn);
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        if (ImGui::Button("Reload grapple clip")) g_reloadRequested.store(true);
        ImGui::TextDisabled("Status: %s", g_status);
        ImGui::TextDisabled("Target: %s", g_targetStatus);
        ImGui::TextDisabled("Kicks: %d (%d from a jump), last: %s", g_kicks.load(), g_airKicks.load(), g_lastKick);
        ImGui::Unindent();
    }
    if (!g_available) ImGui::EndDisabled();
}
}  // namespace Grapple
