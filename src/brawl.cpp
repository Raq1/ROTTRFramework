#include "brawl.h"
#include "combat_air.h"
#include "features.h"
#include "game_build.h"
#include "log.h"
#include "memory_probe.h"
#include <Windows.h>
#include <MinHook.h>
#include <Xinput.h>
#include <imgui.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Hand-to-hand combat with Black Widow's punches and kicks (RE notes: re\brawl\README.txt, NOTES_v251 / v253 / v255).
//
// v0.9.255 (the v0.9.253 test: no lock-on, the clips played a few frames only, blood and no punch sounds, no reaction
// of the enemy). What the log and the game's data said:
// - The pistol's hosted graph (wp_pistol 704732) starts its melee clip only when it ENTERS "ranged_melee" (S11, goal
//   state Combat_Attack_Seize), "ranged_melee_move" (S12, ..._Seize_move) or "ranged_melee_contact" (S13,
//   ..._Seize_Contact). The game's step 2 states (goal ..._Seize_2) start nothing with a pistol in hand, and a press
//   that comes while a strike plays restarts or cuts it (idle -> move after 5 frames, step 1 -> step 2 after 15).
// - No "hit the enemy" follow-up was ever asked for: the patched moves' strike volume did not hit.
// So the chain is ours now:
// - The first strike of a combo starts as before: the game's melee button (Y) is pressed for the player (a short
//   pulse from the pad hook), the game enters one of its melee push states (S11 / S12 play our clip).
// - Every next strike is a state change we force: SCAgent::CheckConditions lets one of the melee transitions of
//   Lara's current state through (ForceTransition), and SCAgent::DoTransition runs it as a change to the other host
//   state (RedirectTransition): from a push state to ground_ranged_melee_push_hitenemy_weaponAllowed (S13 plays the
//   "contact" entries) and from there to ground_ranged_melee_push_move_nomsg (S12 again). The graph has both edges
//   (S11 / S12 -> S13, S13 -> S12), so every strike starts a fresh clip. The next strike is asked for when the clip
//   that plays is ChainFrames past its contact frame and X is held (or was pressed in the last 350 ms).
// - While a strike plays, the game's own melee follow-ups are held back (turn states, idle -> move, step 2, hit enemy,
//   finishers and seizes).
// - Lock-on: the nearest living enemy in range when the combo starts, kept while the combo runs. Lara faces it
//   (PlayerSteering hook, as the grapple kick does) and closes in: the clip's root track is rewritten so that she
//   stops StopDistance in front of it at the contact frame (the animation is the motion source while a strike runs),
//   and the rest of the track follows the enemy each tick.
// - The hit: at the contact frame, with the enemy within Reach, the game's own ApplyMeleeDamage runs on it with a
//   private copy of a melee move: our damage, a melee damage type without a weapon, no hit primitive (no material
//   effect, no blood), no events (the axe's events make the enemies evade or bleed), hit reaction "pushed" (a stagger;
//   the stunned / knees states that open a finisher come from events we do not send). The game's moves of the push
//   states keep their strike volume and collision off while the brawl runs.
// - Sounds: each clip gets its own fx list, built from lists the game has loaded: the kick's whoosh and cloth before
//   the contact, Lara's shout on kicks, and at the contact (only when the enemy can be reached) a body thud, the foot's
//   thud on kicks, the pistol melee's own hit sound on punches, and the landing's camera shake on kicks.
//
// v0.9.256 (the v0.9.255 test: the chain, the hits and the approach work; asked for: a smoother start, punch / kick
// sounds, no sticking, the enemy the camera looks at, a quick but smooth turn, one strike per X press, fewer enemies
// on top of her):
// - Each request of a strike cost the frame 45-50 ms when it wrote the push state's entries (the log's HITCH lines):
//   the entries are made writable once now (at the scan), and the tick reports its own slow runs.
// - One X press = one strike (a press is kept for 0.8 s, so a press during a strike throws the next one at its chain
//   frame); holding X does not repeat.
// - The first strike is a forced change too: the game's own melee entry transition (ground_to_ground_ranged_melee_
//   push_transient / combat_ranged_to_ranged_melee_push_transient), let through and run as a change to the push
//   state. The melee press is only the fallback (no pistol in her hand yet: the game draws it).
// - The locked enemy is the one nearest the camera's direction (angle + 5 deg per metre), chosen again for every
//   strike; Lara's turn rate follows the angle that is left (fast at first, slow at the end).
// - Impact sounds: own sound plexes that refer to the game's sounds by id: 85 "fight_punch_sharp" on every hit, 82
//   "bodyfall_generic_thud" under the kicks (both are in wp_pistol.drm's sound bank, the names are the waves').
// - A strike hits every enemy in front of her within reach (up to 3 more than the locked one).
// - While a strike plays Lara's own hit reactions wait (INI NoFlinch), and EnemyAI's melee tokens are on while a
//   combo runs (Brawl::ComboRunning).
//
// v0.9.257 (the v0.9.256 test: the punch / kick sounds are heard; frames still skip in places; the axe on Y after a
// combo started to come out and was cut, without a hit):
// - Y during or right after a combo: the log showed our "the strike is over, out of its state" exit cutting the
//   GAME's axe states (their names are melee states too), and a Y press during a strike met our patched moves and
//   the axe equip switched off. Now a real Y press ends the brawl at once (strike, moves, entries, axe equip), only
//   a state one of OUR strikes put her in is left by force, and the melee button is pressed again for the player
//   when she is out of it.
// - The pistol graph's edges into and between its melee states blend over 1 frame (S0 -> S11 / S12, S11 -> S12,
//   S11 / S12 -> S13, S13 -> S12): a jump cut between two of our clips. They blend over BlendFrames (3) now; the
//   graph's data comes from the dual wield's fragment hook (Brawl::OnPistolGraph).
//
// v0.9.260 (the v0.9.258 test: the axe and the combos still get mixed up; about two clips lose frames):
// - An X press while the GAME's melee ran (the axe attack after Y) armed a chain strike out of the axe's state: the
//   moves lost their collision and the axe its equip in the middle of the swing. Now a strike is only chained out of
//   the state one of our strikes put her in; while the game's own melee runs the press waits (up to 1.6 s) and the
//   combo starts when she is out of it. The state of our strike is forgotten as soon as she leaves it.
// - The two clips were brawl_punch_a / _b (BW 15499): their source only changes its pose every 2-5 frames. They are
//   converted again with bw2lara.js --smooth-keys (the held frames blended).
//
// v0.9.262 (the v0.9.261 test: the combos and the axe work; the axe answers Y a little late after a combo): a Y
// press in the state of one of our strikes went: exit let through -> ground state -> the melee button pressed again
// -> the game's transient -> the axe's first step. Now the press is answered by a forced change straight into the
// axe's first step (ground_melee_pickaxe_step1_idle, or ..._move when she moves), with the game's moves, clips and
// axe equip restored first; the old way is the fallback when the change does not happen in 250 ms.
//
// v0.9.265 (the v0.9.263 test: the dodge camera is right; X after the axe went on with the axe; Y in a combo froze
// Lara with the axe in her hand):
// - After the game's axe attack the axe stays the item in her hand. An X press then fell back to the melee press
//   (no pistol in hand), which is the game's melee = the axe again, and the chain went on with the axe's graph.
//   Now: the push state gets the property Inventory_Equip = InventoryGroup 2 (the pistols; the axe steps use group 5)
//   while the brawl runs and the chosen weapon is a pistol, so a strike that starts from a ground state puts the
//   pistols in her hand as the axe steps put the axe there. The melee press is never used with the axe in hand. An
//   X press during the game's axe attack takes over once the swing has hit (a hit-enemy state, or 18 frames in the
//   state): the state's exit is let through and the combo starts from the ground state.
// - Y in a combo: v0.9.262's change straight into the axe's first step kept the goal cluster "Combat_Attack" while the
//   item changed, and the axe's graph never started (she stood in the state until its 200 frames were over). Now:
//   our state's exit is let through, and two ticks later the game's own melee entry transition (not the pad) is let
//   through, so the game goes transient -> axe step as from any ground state. The pad press is the fallback.
// - The axe's item id is learned from the game's axe steps alone (10 frames into one, with the game's equip data).
// - Aim in slow motion: AimCameraInput::GetInputCombinedRotation(dt) (retail 0x7D3C70) turns the stick into this
//   frame's yaw / pitch with the camera's dt, which the time dilation scales. While the world or the global factor is
//   below 1 the hook passes the wall clock's frame time instead: the aim turns as fast as at normal speed.
//
// v0.9.267 (the v0.9.266 test: combos, axe and cameras are right; in slow motion the aim sometimes locks and the
// stick does not move it): AimCameraInput::GetInputYawRotationInternal / ...Pitch... read no stick input while
// AimingSystems::IsSnapTargetLocked() (retail 0x838160: a flag at +0x3C, or the timer at +0x2C below the lock time at
// +0x30, with a valid snap target at +0xA0). The timer runs on the game's time, so in slow motion a snap to an enemy
// holds the aim several times longer. The hook answers "not locked" once a lock has lasted its own time on the wall
// clock while the game's time runs slower.
//
// v0.9.263, dodge camera (same module, its own switch): while Lara is in a combat evade or a roll the camera stands
// further back (factor 1 + DodgeCameraExtra / 100, eased in over DodgeCameraInMs and back over DodgeCameraOutMs).
// The exploration camera: ExplorationCameraController::Do3PointInterpolations (retail 0x7C70E0) blends its three
// pitch points' dolly, clamps it and stores it as a Smoother at +0x65C (start) / +0x660 (target) every frame; the hook
// multiplies both after the call. The aim camera: combat_air.cpp's hook multiplies its scale by DodgeCameraFactor().
//
// Dodge reload (same module, its own switch): when Lara starts a combat evade or a roll, the weapon in her hand (or
// the last one she held) is full again at the next tick, without an animation (Item::Reload).
namespace Brawl {
namespace {
constexpr std::uintptr_t kRvaGetLocalPlayerInstance = 0x00403BF0ull;
constexpr std::uintptr_t kRvaTryToGetMeleeMoveComponent = 0x008531F0ull; // cdc::TryToGetMeleeMoveComponent(Instance*)
constexpr std::uintptr_t kRvaMeleeMoveDtpLoad = 0x005E3EC5ull;           // mov rcx,[rdi+440h] (MeleeMoveComponent::GetMeleeMove)
constexpr std::uintptr_t kRvaResolveAnimation = 0x001BE220ull;
constexpr std::uintptr_t kRvaGetKeylistRegistry = 0x001146DCull;
constexpr std::uintptr_t kRvaRefCounts = 0x0160E5E0ull;
constexpr std::uintptr_t kRvaAnimByNumber = 0x015CFD50ull;
constexpr std::uintptr_t kRvaRegistry = 0x010FB790ull;
constexpr std::uintptr_t kRvaIsEnemyOf = 0x004F2750ull;                 // NpcComponent::IsEnemyOf(Instance*)
constexpr std::uintptr_t kRvaNpcListLoad = 0x0070259Full;               // mov rbx,[NpcComponent list head]
constexpr std::uintptr_t kRvaNpcList = 0x02D62CE8ull;
constexpr std::uintptr_t kRvaNpcListInstanceUse = 0x007025B0ull;        // mov rdx,[rbx+3C8h]
constexpr std::uintptr_t kRvaNpcListNextUse = 0x007025CFull;            // mov rbx,[rbx+1A58h]
constexpr std::uintptr_t kRvaNpcDeadUse = 0x004F28D7ull;                // test byte ptr [rcx+1931h],1
constexpr std::uintptr_t kRvaItemReload = 0x006281B0ull;                // Item::Reload(InventoryComponent*, bool, bool)
constexpr std::uintptr_t kRvaInventoryIdUse = 0x008267BFull;            // mov ecx,[ComponentArrayID<InventoryComponent>::s_id]
constexpr std::uintptr_t kRvaInventoryId = 0x010FAA8Cull;
constexpr std::uintptr_t kRvaCurrentItemUse = 0x005DF53Aull;            // mov rax,[rcx+3A0h]; mov rdi,rcx; mov ebx,[rax+28h]
constexpr std::uintptr_t kRvaItemBucketsUse = 0x005DF579ull;            // mov rcx,[rdi+2B8h]
constexpr std::uintptr_t kRvaItemBucketCountUse = 0x005DF584ull;        // div rax,[rdi+2D0h]
// ApplyMeleeDamage(SCAgent*, Instance* attacker, Instance* victim, dtp::MeleeMove*, bool strikeVolume) (TR2 0x705EA0)
constexpr std::uintptr_t kRvaApplyMeleeDamage = 0x006CD710ull;
constexpr std::uintptr_t kRvaApplyMeleeDamageHitFlag = 0x006CD927ull;    // mov byte ptr [r14+453h],1
constexpr std::uintptr_t kRvaTryToGetHealthComponent = 0x008531C0ull;
constexpr std::uintptr_t kRvaAdjustHealthValues = 0x0022989Cull;         // mov rax,[rbx+2A8h]; movaps xmm6,xmm7
constexpr std::uintptr_t kRvaAdjustHealthCurrent = 0x002298AEull;        // addss xmm6,[rax+2Ch]
constexpr std::uintptr_t kRvaAdjustHealthMax = 0x002298C2ull;            // movd xmm0,[rbx+2C4h]
constexpr std::uintptr_t kRvaGetAnimComponent = 0x003DDF50ull;           // Instance::GetAnimComponentV2
constexpr std::uintptr_t kRvaSetMotionSourcesType = 0x0011C985ull;       // mov [rdx+534h],eax (cdc::AnimStateGraph::SetMotionSources)
constexpr std::uintptr_t kRvaSetMotionSourcesFlags = 0x0011C99Dull;      // and byte ptr [rdx+53Ch],0FEh
// Instance::GetForward(Vector3* out) (TR2 0x408A70): -([[instance+0E0h]+70h] row +10h), 0,-1,0 without a transform
constexpr std::uintptr_t kRvaInstanceGetForward = 0x003DE770ull;
constexpr std::uintptr_t kRvaInstanceGetForwardBody = 0x003DE77Eull;
constexpr std::uintptr_t kRvaIsSnapTargetLocked = 0x00838160ull;      // AimingSystems::IsSnapTargetLocked()
constexpr std::uintptr_t kRvaAimInputRotation = 0x007D3C70ull;        // AimCameraInput::GetInputCombinedRotation(float dt)
constexpr std::uintptr_t kRvaSetTimeDilationManager = 0x006A947Full;  // lea rcx,[ms_timeDilationManager]
constexpr std::uintptr_t kRvaExploreCamDo3Point = 0x007C70E0ull;     // ExplorationCameraController::Do3PointInterpolations()
constexpr std::uintptr_t kRvaExploreCamDollyStore = 0x007C79EAull;   // movss [rbx+660h],xmm0; movss [rbx+65Ch],xmm0 (the clamped dolly)

constexpr std::uint8_t kGetLocalPlayerInstanceSig[] = {0x40,0x53,0x48,0x83,0xEC,0x20,0x0F,0xB6,0xD9};
constexpr std::uint8_t kTryToGetMeleeMoveComponentSig[] = {0x48,0x8B,0x51,0x68,0x48,0xB8,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0x48,0x85,0xD0};
constexpr std::uint8_t kMeleeMoveDtpLoadSig[] = {0x48,0x8B,0x8F,0x40,0x04,0x00,0x00};
constexpr std::uint8_t kResolveAnimationSig[] = {
    0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7c,0x24,0x18,0x41,0x56,0x48,0x83,0xec,
    0x20,0x8b,0xd9,0x4c,0x8d,0x35,0xc2,0x1d,0xe4,0xff,0x8b,0xf2,0x45,0x0f,0xb7,0x84,0x5e,0xe0,0xe5,0x60,
    0x01,0x41,0x8d,0x40,0x01,0x66,0x41,0x89,0x84,0x5e,0xe0,0xe5,0x60,0x01,0x66,0x45,0x85,0xc0,0x75,0x27,
    0xe8,0xaf,0x51,0x18,0x00,0x8b,0xd6,0x45,0x33,0xc0,0x48,0x8b,0xc8,0x4c,0x8b,0x08,0x41,0xff,0x91,0x90,
    0x00,0x00,0x00,0x49,0x89,0x84,0xde,0x50,0xfd,0x5c,0x01
};
constexpr std::uint8_t kGetKeylistRegistrySig[] = {0x48,0x8B,0x0D};
constexpr std::uint8_t kIsEnemyOfSig[] = {0x40,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0x89,0xC8,0x03,0x00,0x00,0x48,0x8B,0xFA,
                                          0x48,0x85,0xC9,0x74,0x31,0x48,0x85,0xD2,0x74,0x2C,0x48,0x3B,0xCA,0x74,0x27};
constexpr std::uint8_t kNpcListLoadSig[] = {0x48,0x8B,0x1D};
constexpr std::uint8_t kNpcListInstanceSig[] = {0x48,0x8B,0x93,0xC8,0x03,0x00,0x00};
constexpr std::uint8_t kNpcListNextSig[] = {0x48,0x8B,0x9B,0x58,0x1A,0x00,0x00};
constexpr std::uint8_t kNpcDeadSig[] = {0xF6,0x81,0x31,0x19,0x00,0x00,0x01};
constexpr std::uint8_t kItemReloadSig[] = {0x48,0x89,0x5C,0x24,0x18,0x55,0x56,0x57,0x48,0x83,0xEC,0x30,0x33,0xED,0x41,0x0F,0xB6,0xF0,0x8B,0xDD};
constexpr std::uint8_t kInventoryIdUseSig[] = {0x8B,0x0D};
constexpr std::uint8_t kCurrentItemUseSig[] = {0x48,0x8B,0x81,0xA0,0x03,0x00,0x00,0x48,0x8B,0xF9,0x8B,0x58,0x28};
constexpr std::uint8_t kItemBucketsUseSig[] = {0x48,0x8B,0x8F,0xB8,0x02,0x00,0x00};
constexpr std::uint8_t kItemBucketCountUseSig[] = {0x48,0xF7,0xB7,0xD0,0x02,0x00,0x00};
constexpr std::uint8_t kApplyMeleeDamageSig[] = {0x4D,0x85,0xC9,0x0F,0x84,0x7C,0x02,0x00,0x00,0x48,0x8B,0xC4,0x48,0x89,0x58,0x10,0x48,0x89,0x70,0x18,
                                                 0x55,0x57,0x41,0x56,0x48,0x8D,0x68,0xA9,0x48,0x81,0xEC,0xF0,0x00,0x00,0x00,0x49,0x8B,0xD8,0x49,0x8B};
constexpr std::uint8_t kApplyMeleeDamageHitFlagSig[] = {0x41,0xC6,0x86,0x53,0x04,0x00,0x00,0x01};
constexpr std::uint8_t kTryToGetHealthSig[] = {0x48,0x8B,0xC1,0x48,0x85,0xC9,0x74,0x1A,0x8B,0x0D};
constexpr std::uint8_t kTryToGetHealthTailSig[] = {0x39,0x88,0xD0,0x00,0x00,0x00,0x76,0x0C,0x48,0x8B,0x80,0xD8,0x00,0x00,0x00,
                                                   0x48,0x8B,0x04,0xC8,0xC3};
constexpr std::uint8_t kAdjustHealthValuesSig[] = {0x48,0x8B,0x83,0xA8,0x02,0x00,0x00,0x0F,0x28,0xF7};
constexpr std::uint8_t kAdjustHealthCurrentSig[] = {0xF3,0x0F,0x58,0x70,0x2C};
constexpr std::uint8_t kAdjustHealthMaxSig[] = {0x66,0x0F,0x6E,0x83,0xC4,0x02,0x00,0x00};
constexpr std::uint8_t kGetAnimComponentSig[] = {0x48,0x8B,0xC1,0x48,0x85,0xC9,0x74,0x1A,0x8B,0x0D,0xCA,0x3D,0xD2,0x00,0x39,0x88,
                                                 0xD0,0x00,0x00,0x00,0x76,0x0C,0x48,0x8B,0x80,0xD8,0x00,0x00,0x00,0x48,0x8B,0x04,0xC8,0xC3};
constexpr std::uint8_t kSetMotionSourcesTypeSig[] = {0x89,0x82,0x34,0x05,0x00,0x00};
constexpr std::uint8_t kSetMotionSourcesFlagsSig[] = {0x80,0xA2,0x3C,0x05,0x00,0x00,0xFE};
constexpr std::uint8_t kInstanceGetForwardSig[] = {0x48,0x8B,0x81,0xE0,0x00,0x00,0x00,0x0F,0x28,0x05};
constexpr std::uint8_t kAimInputRotationSig[] = {0x40,0x56,0x48,0x81,0xEC,0x90,0x00,0x00,0x00,0x8B,0x05,0xED,0x3C,0x58,0x02,0x48,
                                                 0x8B,0xF1,0x0F,0x29,0xB4,0x24,0x80,0x00,0x00,0x00,0x0F,0x28,0xF1,0x39,0x81,0x08,
                                                 0x01,0x00,0x00,0x0F,0x84};
constexpr std::uint8_t kIsSnapTargetLockedSig[] = {0x48,0x83,0xEC,0x28,0x80,0x79,0x3C,0x00,0x75,0x08,0x8B,0x41,0x30,0x39,0x41,0x2C,
                                                   0x73,0x17,0x48,0x8B,0x91,0xA0,0x00,0x00,0x00,0xE8,0x22,0x02,0x00,0x00,0x84,0xC0,
                                                   0x74,0x07,0xB0,0x01,0x48,0x83,0xC4,0x28,0xC3,0x32,0xC0,0x48,0x83,0xC4,0x28,0xC3};
constexpr std::size_t kSnapTimer = 0x2C, kSnapLockTime = 0x30, kSnapLockFlag = 0x3C;
constexpr std::uint8_t kLeaRcxRipSig[] = {0x48,0x8D,0x0D};
constexpr std::size_t kDilationWorldNow = 0x11C, kDilationGlobalNow = 0x120;  // TimeDilationManager: the current factors
constexpr std::size_t kAgentFramesInState = 0x618;                            // float
constexpr std::int32_t kInventoryGroupPistol = 2;                             // (1 bow, 2 pistol, 3 SMG, 4 shotgun, 5 axe)
constexpr std::uint8_t kExploreCamDo3PointSig[] = {0x48,0x8B,0xC4,0x48,0x89,0x58,0x08,0x48,0x89,0x68,0x10,0x48,0x89,0x70,0x18,0x57,
                                                   0x48,0x81,0xEC,0xE0,0x00,0x00,0x00,0x0F,0x29,0x70,0xE8,0x48,0x8B,0xD9,0x0F,0x29};
constexpr std::uint8_t kExploreCamDollyStoreSig[] = {0xF3,0x0F,0x11,0x83,0x60,0x06,0x00,0x00,0xF3,0x0F,0x11,0x83,0x5C,0x06,0x00,0x00};
constexpr std::size_t kExploreCamDollyStart = 0x65C, kExploreCamDollyTarget = 0x660;
constexpr std::uint8_t kInstanceGetForwardBodySig[] = {0x0F,0x29,0x02,0x48,0x85,0xC0,0x74,0x0E,0x48,0x8B,0x40,0x70,0x0F,0x57,0xC0,0x0F,
                                                       0x5C,0x40,0x10,0x0F,0x29,0x02,0x48,0x8B,0xC2,0xC3};

constexpr std::size_t kInstancePos = 0x10;
constexpr std::size_t kNpcInstance = 0x3C8, kNpcNext = 0x1A58, kNpcDead = 0x1931;
constexpr int kMaxNpcs = 128;
constexpr std::size_t kAgentData = 0x08, kAgentInstance = 0x10, kAgentCurrentState = 0x1C10;
constexpr std::size_t kAgentMeleeHit = 0x453;  // set by ApplyMeleeDamage (Lara's "hit the enemy" follow-up reads it)
constexpr std::size_t kAgentDataNumStates = 0x18, kAgentDataNumTransitions = 0x1A, kAgentDataStateRefs = 0x20,
                      kAgentDataTransitionRefs = 0x28;
constexpr std::size_t kStateName = 0x38;
constexpr std::size_t kInstanceComponentCount = 0xD0, kInstanceComponents = 0xD8;
constexpr std::size_t kInventoryCurrent = 0x3A0, kCurrentItemId = 0x28;       // InventoryComponent -> {.. +0x28 the chosen item's id}
constexpr std::size_t kInventoryItemBuckets = 0x2B8, kInventoryItemBucketCount = 0x2D0;  // nodes {next, u32 id +8, Item +0x10}
constexpr std::size_t kTransitionTarget = 0x00, kTransitionName = 0x18;
constexpr std::size_t kMeleeComponentDtp = 0x440;  // dtp::MeleeMoveComponent* {groups, n, MeleeMove* moves +0x10, n +0x18}
constexpr std::size_t kHealthValues = 0x2A8, kHealthCurrent = 0x2C, kHealthMax = 0x2C4;
constexpr std::size_t kAnimMotionType = 0x534, kAnimMotionFlags = 0x53C;      // flags bit 0: root translation on
constexpr std::int32_t kMotionFromMovement = 2, kMotionFromAnimation = 3;
// dtp::MeleeMove (0x258 bytes, TR2 PDB; the DTP data layout).
constexpr std::size_t kMoveSize = 0x258;
constexpr std::size_t kMoveConnectFx = 0x04;
constexpr std::size_t kMoveDamage = 0x08, kMoveStrikeDamage = 0x18;
constexpr std::size_t kMoveDamagePrim = 0x28;          // i32 damagePrimIndex (-1: the victim's position, no primitive)
constexpr std::size_t kMoveDamageType = 0x30, kMoveEffect = 0x34, kMoveEffectUnion = 0x38, kMoveArmorOverride = 0x44;
constexpr std::size_t kMoveFrames = 0x5C, kMoveStrikeFrames = 0x6C;
constexpr std::size_t kMoveEventSelf = 0x110, kMoveEventVictim = 0x114, kMoveEventVictimVolume = 0x118;
constexpr std::size_t kMoveName = 0x198;
constexpr std::size_t kMoveLungeOn = 0x232, kMoveVolumeOn = 0x234, kMoveCustomStrikeDamage = 0x235;
constexpr std::size_t kMoveIgnoreHealthStates = 0x236, kMoveCanNotKill = 0x237, kMoveCanNotKillExceptLow = 0x238;
constexpr std::size_t kMoveHurtsAllies = 0x239, kMoveNoBroadcast = 0x23A;

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

// cdc::AnimFxList (an AnimEntry's fx pointer; acro.cpp documents the format): { dtp::FXLib* effectLib; triggers }, each
// trigger { u16 info; i16 keyframe; u8 fxID; pad[3] } + payload, ended by info 0xFFFF; info bits 0-1 kind (0 sound,
// 3 camera), bit 2 disabled, bits 10-11 phase (0 keyframe-timed), bits 12-15 payload dwords n (size 8 + ((4n + 4) & ~7)).
// Sound payload: i8 type (0 sound plex, 1 / 3 vocal bark), ..., dtp::SoundPlex* at trigger +16.
struct AnimFxList {
    void* effectLib;
    std::uint8_t* triggers;
};
constexpr std::size_t kFxBufferSize = 1024;

// The pistol's melee clips (wp_pistol's anim list; a pistol with its own list has the same names): what the graph's
// push states play (S11 / S12 / S10), and what its contact state plays (S13).
constexpr const char* kEntryNamesPush[] = {"Hosted_Pistol_Melee_To_Stumble_A", "Hosted_Pistol_Melee_Strong", "Hosted_Pistol_Melee",
                                           "Hosted_Pistol_Melee_Moving", "Hosted_Pistol_Melee_Moving_Strong"};
constexpr const char* kEntryNamesContact[] = {"Hosted_Pistol_Melee_To_Stumble_Contact_Move_A", "Hosted_Pistol_Melee_To_Stumble_Contact_A"};
constexpr const char* kMoveNames[] = {"Melee_Push_Pistol", "Melee_Push_Pistol_MeleeUpgrade", "pickaxe_step1", "pickaxe_step2",
                                      "pickaxe_step2_dual", "melee_push"};
// The chain's two host states and the transition that carries the forced changes (its target is switched for the call).
constexpr const char* kStatePushName = "ground_ranged_melee_push_move_nomsg";
constexpr const char* kStateContactName = "ground_ranged_melee_push_hitenemy_weaponAllowed";
constexpr const char* kCarrierName = "ground_to_ground_ranged_melee_push_move";
// The game's first axe step (what its melee press leads to with the melee upgrade), standing and moving.
constexpr const char* kStateAxeIdleName = "ground_melee_pickaxe_step1_idle";
constexpr const char* kStateAxeMoveName = "ground_melee_pickaxe_step1_move";
// The game's own ways into the melee push (evaluated in every state a melee can start from).
constexpr const char* kEntryTransitionNames[] = {"ground_to_ground_ranged_melee_push_transient",
                                                 "combat_ranged_to_ranged_melee_push_transient"};
// Sound sources (library entries by name) and the sounds taken from them.
constexpr const char* kSourceKick = "Combat_Kick";                          // kf 0 shout, kf 1 whoosh 6591, kf 2 cloth 3605
constexpr const char* kSourceFoot = "interact_phys_kick_high";              // kf 10: 11483, Lara's foot on an object
constexpr const char* kSourceThud = "beam_idle_jump_vertical_land_hard";    // kf 0: 11489 body thud, kf 4 camera shake
constexpr std::uint32_t kSoundFoot = 11483, kSoundThud = 11489, kSoundPistolHit = 11492;  // (11492: the pistol melee's, kf 13)
// Sounds by id (dtp sounds of wp_pistol.drm's bank; the names are their waves'): our own Reference plexes play them.
constexpr std::uint32_t kSoundPunch = 85;      // fight_punch_sharp_01..10
constexpr std::uint32_t kSoundBodyfall = 82;   // bodyfall_generic_thud_01..06
constexpr int kMaxClips = 16, kMaxSlots = 48, kMaxMoves = 6, kMaxEquipProps = 64, kMaxFrames = 96;
constexpr int kHostPush = 1, kHostContact = 2;
// dtp::SCStateData: +0x04 u16 property count, +0x10 properties (dtp::SCPropertyData 0x20: i16 property, i16 type,
// value at +8).
constexpr std::size_t kStatePropCount = 0x04, kStateProps = 0x10, kPropSize = 0x20, kPropValue = 0x08;
constexpr std::int16_t kPropInventoryEquip = 691;      // dtp::EnumSCStateProperty Inventory_Equip
constexpr std::int16_t kPropTypeInventoryGroup = 41;   // dtp::SCPropertyData::EnumSCPropertyType
constexpr std::int32_t kInventoryGroupMelee = 5;       // the axe
constexpr ULONGLONG kChainResetMs = 2000;   // a strike this long after the last one starts the chain again
constexpr ULONGLONG kBrawlIdleMs = 1500;    // without a strike or a press for this long the game's clips and moves are back
constexpr ULONGLONG kArmedMs = 700;         // a melee state entered this soon after the melee press is the brawl's
constexpr ULONGLONG kPressBufferMs = 800;   // a press counts this long
constexpr ULONGLONG kPulseMs = 130;         // the game's melee button is held this long for the first strike
constexpr ULONGLONG kForceMs = 250;         // a forced change that did not happen in this time is dropped
constexpr float kFramesPerMs = 0.03f;
// dtp::HitReactType of the reaction choices (menu order) and their names.
constexpr int kReactionTypes[] = {11, 14, 15, 16, 5, 2, 0};
constexpr const char* kReactionNames[] = {"Pushed (stagger)", "Melee light", "Melee medium", "Melee heavy", "Clear out", "Flinch (as a bullet hit)", "None"};
constexpr int kReactionCount = 7;
// dtp::DamageType choices.
constexpr int kDamageTypes[] = {47, 52, 53, 63, 29};
constexpr const char* kDamageTypeNames[] = {"Pistol melee", "Push melee", "Kick melee", "Knockout", "Melee (the axe's)"};
constexpr int kDamageTypeCount = 5;
// dtp::AnimStateGraph (the pistol's hosted graph): states +0 (0x70 each, name +0), edges +8 (0x60 each: f32 blend
// frames +0, name +0x50, source +0x58, target +0x59), u8 counts at +0x10 / +0x11.
constexpr std::size_t kGraphStates = 0x00, kGraphEdges = 0x08, kGraphNumStates = 0x10, kGraphNumEdges = 0x11;
constexpr std::size_t kGraphStateSize = 0x70, kGraphEdgeSize = 0x60, kGraphEdgeSrc = 0x58, kGraphEdgeDst = 0x59;
constexpr int kMaxBlendEdges = 16, kMaxSeenGraphs = 16;

using GetLocalPlayerInstanceFn = void*(__fastcall*)(std::uint8_t);
using TryToGetFn = void*(__fastcall*)(void* instance);
using IsEnemyOfFn = bool(__fastcall*)(void* npcComponent, void* instance);
using ItemReloadFn = void(__fastcall*)(void* item, void* inventory, bool message, bool flag);
using ApplyMeleeDamageFn = void(__fastcall*)(void* agent, void* attacker, void* victim, const std::uint8_t* move, bool strikeVolume);
using GetForwardFn = float*(__fastcall*)(void* instance, float* out);
using ExploreCamDo3PointFn = void(__fastcall*)(void* controller);
using AimInputRotationFn = void(__fastcall*)(void* input, float dt);
using IsSnapTargetLockedFn = bool(__fastcall*)(void* aimingSystems);

struct Clip {
    char file[64]{};
    std::uint8_t* data = nullptr;
    int frames = 0, contact = 0, id = -1;
    bool kick = false;
    float* rootY = nullptr;      // the root's (global id 1) baked y track: forward is -y
    float* anchorY = nullptr;    // the anchor's (global id 172): stays where the clip starts
    float baseRootY[kMaxFrames]{};
    float baseAnchorY[kMaxFrames]{};
    bool rootOurs = false;
    AnimFxList* fxHit = nullptr;   // with the sounds of the contact
    AnimFxList* fxMiss = nullptr;  // the swing only
};
struct Slot {  // a library entry that holds a pistol melee clip
    std::uint8_t* library = nullptr;
    std::uint32_t index = 0;
    AnimEntry original{};
    int host = kHostPush;
    bool written = false;
    bool writable = false;
};
// dtp::SoundPlex (0x18) of type Reference with its data (the sound's id).
struct OwnPlex {
    std::uint16_t type;
    std::uint8_t clickPlay;
    std::uint8_t pad0[5];
    void* data;
    std::uint32_t ownerId;
    std::uint32_t pad1;
    std::uint32_t soundId;
    std::uint32_t pad2;
};
static_assert(sizeof(OwnPlex) == 0x20, "OwnPlex: a 0x18 byte plex and its data");
struct Move {
    std::uint8_t* data = nullptr;
    std::uint8_t original[kMoveSize]{};
    bool patched = false;
};
struct FxBuild {
    std::uint8_t buf[kFxBufferSize];
    std::size_t used;
    int count;
};

std::uintptr_t g_base = 0;
bool g_available = false;
GetLocalPlayerInstanceFn g_getLocalPlayerInstance = nullptr;
TryToGetFn g_tryToGetMeleeMove = nullptr;
TryToGetFn g_tryToGetHealth = nullptr;
TryToGetFn g_getAnimComponent = nullptr;
IsEnemyOfFn g_isEnemyOf = nullptr;
ApplyMeleeDamageFn g_applyMeleeDamage = nullptr;
GetForwardFn g_getForward = nullptr;
bool g_forwardAvailable = false;
OwnPlex g_plexPunch{}, g_plexBodyfall{};
std::uint8_t* const* g_npcList = nullptr;
std::uint16_t* g_refCounts = nullptr;
void** g_animByNumber = nullptr;
std::uint8_t* const* g_registry = nullptr;
bool g_healthReadable = false, g_motionAvailable = false;
bool g_reloadAvailable = false;
ItemReloadFn g_itemReload = nullptr;
const std::uint32_t* g_inventoryId = nullptr;
std::atomic<bool> g_dodgeReload{false};
std::atomic<bool> g_reloadPending{false};
std::atomic<unsigned> g_reloads{0};
char g_reloadState[96] = "";  // the dodge state that asked (game thread)
std::uint32_t g_lastItemId = 0;  // the last weapon Lara had in her hand
bool g_itemInHand = true;
std::uint32_t g_itemNow = 0;          // the inventory's current item
std::atomic<bool> g_gameMelee{false}; // the game's own melee runs (the tick's view)
std::atomic<ULONGLONG> g_lastGameMeleeMs{0};
std::atomic<const void*> g_lockedEnemy{nullptr};
std::atomic<int> g_snapLockMaxMs{300};
std::uint32_t g_rangedItemId = 0;     // seen in a combat_ranged state: a ranged weapon
std::uint32_t g_axeItemId = 0;        // seen in the game's axe states and not the ranged one: the axe

// Settings ([Brawl] in ROTTRFramework.ini).
std::atomic<bool> g_enabled{false};   // (v1.0.0: these four off by default)
std::atomic<int> g_range{350};
std::atomic<int> g_damage{80};
std::atomic<bool> g_lethal{true};
std::atomic<bool> g_lockOn{true};
std::atomic<int> g_approach{1};        // 0 off, 1 the clip's root motion, 2 position steps
std::atomic<int> g_stopDistance{85};
std::atomic<int> g_maxApproach{300};
std::atomic<int> g_reach{140};
std::atomic<int> g_reaction{0};        // index into kReactionTypes
std::atomic<int> g_damageType{0};      // index into kDamageTypes
std::atomic<bool> g_sounds{true};
std::atomic<bool> g_shake{true};
std::atomic<int> g_chainFrames{5};
std::atomic<int> g_turnRate{720};
std::atomic<bool> g_cameraTarget{true};
std::atomic<bool> g_areaHits{true};
std::atomic<bool> g_noFlinch{true};
std::atomic<int> g_blendFrames{3};
std::atomic<bool> g_aimRealTime{false};      // the aim turns at the wall clock's pace in slow motion
bool g_aimRealTimeAvailable = false;
AimInputRotationFn g_origAimInputRotation = nullptr;
IsSnapTargetLockedFn g_origIsSnapTargetLocked = nullptr;
bool g_snapLockAvailable = false;
std::atomic<unsigned> g_snapLocksEnded{0};
const std::uint8_t* g_timeDilation = nullptr;
std::atomic<unsigned> g_aimRealTimeFrames{0};
std::atomic<bool> g_dodgeCam{false};
std::atomic<int> g_dodgeCamExtra{35};      // percent further back
std::atomic<int> g_dodgeCamInMs{180};
std::atomic<int> g_dodgeCamOutMs{650};
std::atomic<float> g_dodgeCamFactor{1.0f};
bool g_dodgeCamAvailable = false;
ExploreCamDo3PointFn g_origExploreCamDo3Point = nullptr;
std::atomic<unsigned> g_dodgeCamFrames{0};

// Game thread.
Clip g_clips[kMaxClips];
int g_clipCount = 0;
bool g_clipsLoaded = false, g_clipsFailed = false;
Slot g_slots[kMaxSlots];
int g_slotCount = 0;
ULONGLONG g_slotScanMs = 0;
const AnimFxList* g_sources[4]{};      // kick, foot, thud, the pistol melee's own list
const AnimFxList* g_fxBuiltFrom[4]{};
bool g_fxBuilt = false, g_fxUsable = false;
bool g_fxShake = true;
Move g_moves[kMaxMoves];
std::uint8_t* g_moveDtpRoot = nullptr;
alignas(16) std::uint8_t g_hitMove[kMoveSize]{};   // the private move of our hit
std::int16_t* g_equipProps[kMaxEquipProps]{};   // "the axe in hand" of the melee step states (the property id's address)
int g_equipPropCount = 0;
const std::uint8_t* g_equipData = nullptr;      // the state control data they were found in
bool g_equipOff = false;
const std::uint8_t* g_stateData = nullptr;      // the state control data the host states were found in
int g_statePush = -1, g_stateContact = -1, g_stateAxeIdle = -1, g_stateAxeMove = -1;
std::atomic<bool> g_forceAxe{false};   // the forced change is the axe's first step (a Y press in our strike's state)
std::atomic<bool> g_fastAxe{true};
float g_laraSpeed = 0.0f;              // cm/s over the last tick
std::uint8_t* g_carrier = nullptr;
int g_chainIndex = -1;
bool g_entriesOurs = false;
ULONGLONG g_pendingUntilMs = 0;        // no new request before this
int g_forceFails = 0;
ULONGLONG g_lastStrikeEndMs = 0;
int g_logBudget = 1200;
unsigned g_tick = 0;
char g_status[160] = "waiting for the first game tick";
// The locked enemy (validated against the NPC list each tick).
std::uint8_t* g_target = nullptr;
std::atomic<bool> g_targetValid{false};
std::atomic<float> g_targetX{0.0f}, g_targetY{0.0f}, g_targetZ{0.0f};
std::uint8_t* g_lara = nullptr;
// The animation as the motion source while a strike runs.
std::uint8_t* g_motionAnim = nullptr;
std::int32_t g_motionBefore = -1;
bool g_motionHeld = false;
float g_strikeStartPos[3]{};
float g_strikePlanned = 0.0f;

// A strike: armed by the tick, started by the state change.
std::atomic<int> g_armedClip{-1};
std::atomic<int> g_armedHost{0};
std::atomic<bool> g_armedForce{false};
std::atomic<ULONGLONG> g_armedMs{0};
std::atomic<bool> g_forceArmed{false};
std::atomic<const std::uint8_t*> g_forced{nullptr};
std::atomic<int> g_forceState{-1};
std::atomic<bool> g_exitWanted{false};
std::atomic<int> g_activeClip{-1};
std::atomic<int> g_activeHost{0};
std::atomic<ULONGLONG> g_activeStartMs{0};
std::atomic<ULONGLONG> g_lastStrikeStartMs{0};
std::atomic<bool> g_hitDone{false};
std::atomic<unsigned> g_strikes{0};
std::atomic<unsigned> g_hits{0};
std::atomic<unsigned> g_held{0};
std::atomic<unsigned> g_heldFinishers{0};
std::atomic<unsigned> g_forcedCount{0};
std::atomic<int> g_dodgeLogBudget{30};
std::atomic<bool> g_forceFirst{false};       // the forced change is a combo's first strike (out of a ground / combat state)
std::atomic<unsigned> g_armedPress{0};       // the press count the armed strike answers
std::atomic<int> g_leaveLogBudget{40};
unsigned g_pressHandled = 0;                 // presses up to this one have had their strike
std::atomic<float> g_laraForwardX{0.0f}, g_laraForwardY{-1.0f};
int g_slowTickLogs = 30;
const std::uint8_t* g_strikeState = nullptr;  // the state our last strike put her in
std::uint8_t* g_pushProp = nullptr;           // the push state's first property (Inventory_Equip = the pistols while we run)
std::uint8_t g_pushPropOriginal[kPropSize]{};
bool g_pushPropOurs = false;
std::atomic<bool> g_forceGameEntry{false};    // the game's melee entry transition is let through (the axe after a combo)
std::atomic<ULONGLONG> g_gameEntryMs{0};
ULONGLONG g_gameEntryFallbackMs = 0;
unsigned g_axeEntryTick = 0;
std::atomic<bool> g_exitGame{false};          // an X press takes over from the game's melee: its exit is let through
ULONGLONG g_takeoverUntilMs = 0;
ULONGLONG g_leftMeleeMs = 0;
bool g_wasMelee = false;
ULONGLONG g_yHandledMs = 0;                   // the real Y press that has ended the brawl
bool g_exitForY = false;
ULONGLONG g_yRepressUntilMs = 0;
// The pistol graph's melee edges (any thread finds them, the tick writes them).
std::atomic<const std::uint8_t*> g_seenGraphs[kMaxSeenGraphs];
std::atomic<int> g_seenGraphCount{0};
float* g_blendEdges[kMaxBlendEdges]{};
float g_blendOriginal[kMaxBlendEdges]{};
std::atomic<int> g_blendEdgeCount{0};
int g_blendApplied = -1;

// Pad thread -> game thread.
std::atomic<bool> g_enemyNear{false};
std::atomic<float> g_enemyDistance{-1.0f};
std::atomic<unsigned> g_pressCount{0};       // brawl presses of X
std::atomic<ULONGLONG> g_lastPressMs{0};
std::atomic<ULONGLONG> g_lastRealYMs{0};
std::atomic<int> g_xState{0};               // 0 up, 1 down as a brawl press, 2 down as the game's X
std::atomic<bool> g_xHeld{false};           // X is down as a brawl press
std::atomic<bool> g_yWasDown{false};
std::atomic<ULONGLONG> g_pulseUntilMs{0};   // the game's melee button is down until then
std::atomic<ULONGLONG> g_brawlPulseMs{0};   // when the melee button was last pressed for a strike of ours

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
    Log::Writef("Brawl: %s signature mismatch; hand-to-hand unavailable", name);
    return false;
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

bool NameHas(const char* text, const char* part) {
    if (!text) return false;
    for (std::size_t i = 0; i < 200 && text[i]; ++i) {
        std::size_t k = 0;
        while (part[k] && text[i + k] == part[k]) ++k;
        if (!part[k]) return true;
    }
    return false;
}

void Logf(const char* fmt, ...) {
    if (g_logBudget <= 0) return;
    --g_logBudget;
    char text[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    Log::Detail(text);
}

// 0: not a strike's state; kHostPush: a melee push / axe step state (the graph's S11 / S12); kHostContact: a "hit the
// enemy" state (S13).
int HostKindOfName(const char* name) {
    if (!name) return 0;
    if (NameHas(name, "hitenemy")) return kHostContact;
    if (!NameHas(name, "melee_pickaxe_step") && !NameHas(name, "ranged_melee_push")) return 0;
    if (NameHas(name, "transient") || NameHas(name, "hitarmor") || NameHas(name, "_turn") || NameHas(name, "rangedentry") ||
        NameHas(name, "at_ledge"))
        return 0;
    return kHostPush;
}

// A state a combo may start from: on her feet on the ground or in the combat stance (no jump, fall, roll, evade,
// slope, ledge or cover state).
bool StartStateName(const char* name) {
    if (!name) return false;
    const bool ground = name[0] == 'g' && name[1] == 'r' && name[2] == 'o' && name[3] == 'u' && name[4] == 'n' && name[5] == 'd';
    const bool combat = name[0] == 'c' && name[1] == 'o' && name[2] == 'm' && name[3] == 'b' && name[4] == 'a' && name[5] == 't';
    if (!ground && !combat) return false;
    return !NameHas(name, "jump") && !NameHas(name, "fall") && !NameHas(name, "roll") && !NameHas(name, "evade") &&
           !NameHas(name, "slope") && !NameHas(name, "ledge") && !NameHas(name, "cover") && !NameHas(name, "swim") && !NameHas(name, "wad");
}

bool MeleeStateName(const char* name) {
    return NameHas(name, "melee_pickaxe_step") || NameHas(name, "ranged_melee_push") || NameHas(name, "hitenemy") ||
           NameHas(name, "hitarmor");
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

constexpr const char* kIniSection = "Brawl";

int IniInt(const char* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntA(kIniSection, key, fallback, IniPath()));
}

void IniWriteInt(const char* key, int value) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%d", value);
    WritePrivateProfileStringA(kIniSection, key, buf, IniPath());
}

void SaveSettings() {
    IniWriteInt("Enabled", g_enabled.load() ? 1 : 0);
    IniWriteInt("Range", g_range.load());
    IniWriteInt("Damage", g_damage.load());
    IniWriteInt("Lethal", g_lethal.load() ? 1 : 0);
    IniWriteInt("LockOn", g_lockOn.load() ? 1 : 0);
    IniWriteInt("Approach", g_approach.load());
    IniWriteInt("StopDistance", g_stopDistance.load());
    IniWriteInt("MaxApproach", g_maxApproach.load());
    IniWriteInt("Reach", g_reach.load());
    IniWriteInt("Reaction", g_reaction.load());
    IniWriteInt("DamageType", g_damageType.load());
    IniWriteInt("Sounds", g_sounds.load() ? 1 : 0);
    IniWriteInt("CameraShake", g_shake.load() ? 1 : 0);
    IniWriteInt("ChainFrames", g_chainFrames.load());
    IniWriteInt("CameraTarget", g_cameraTarget.load() ? 1 : 0);
    IniWriteInt("AreaHits", g_areaHits.load() ? 1 : 0);
    IniWriteInt("NoFlinch", g_noFlinch.load() ? 1 : 0);
    IniWriteInt("BlendFrames", g_blendFrames.load());
    IniWriteInt("DodgeCamera", g_dodgeCam.load() ? 1 : 0);
    IniWriteInt("DodgeCameraExtra", g_dodgeCamExtra.load());
    IniWriteInt("DodgeCameraInMs", g_dodgeCamInMs.load());
    IniWriteInt("DodgeCameraOutMs", g_dodgeCamOutMs.load());
    IniWriteInt("FastAxe", g_fastAxe.load() ? 1 : 0);
    IniWriteInt("AimRealTimeInSlowMotion", g_aimRealTime.load() ? 1 : 0);
    IniWriteInt("SnapLockMaxMs", g_snapLockMaxMs.load());
    IniWriteInt("TurnRate", g_turnRate.load());
    IniWriteInt("DodgeReload", g_dodgeReload.load() ? 1 : 0);
}

void LoadSettings() {
    g_enabled.store(IniInt("Enabled", 0) != 0);   // (v1.0.0: the features default off)
    g_range.store(std::clamp(IniInt("Range", 350), 100, 1000));
    g_damage.store(std::clamp(IniInt("Damage", 80), 1, 2000));
    g_lethal.store(IniInt("Lethal", 1) != 0);
    g_lockOn.store(IniInt("LockOn", 1) != 0);
    g_approach.store(std::clamp(IniInt("Approach", 1), 0, 2));
    g_stopDistance.store(std::clamp(IniInt("StopDistance", 85), 40, 200));
    g_maxApproach.store(std::clamp(IniInt("MaxApproach", 300), 0, 800));
    g_reach.store(std::clamp(IniInt("Reach", 140), 60, 400));
    g_reaction.store(std::clamp(IniInt("Reaction", 0), 0, kReactionCount - 1));
    g_damageType.store(std::clamp(IniInt("DamageType", 0), 0, kDamageTypeCount - 1));
    g_sounds.store(IniInt("Sounds", 1) != 0);
    g_shake.store(IniInt("CameraShake", 1) != 0);
    g_chainFrames.store(std::clamp(IniInt("ChainFrames", 5), 2, 30));
    g_cameraTarget.store(IniInt("CameraTarget", 1) != 0);
    g_areaHits.store(IniInt("AreaHits", 1) != 0);
    g_noFlinch.store(IniInt("NoFlinch", 1) != 0);
    g_blendFrames.store(std::clamp(IniInt("BlendFrames", 3), 1, 10));
    g_dodgeCam.store(IniInt("DodgeCamera", 0) != 0);
    g_dodgeCamExtra.store(std::clamp(IniInt("DodgeCameraExtra", 35), 0, 100));
    g_dodgeCamInMs.store(std::clamp(IniInt("DodgeCameraInMs", 180), 30, 2000));
    g_dodgeCamOutMs.store(std::clamp(IniInt("DodgeCameraOutMs", 650), 30, 4000));
    g_fastAxe.store(IniInt("FastAxe", 1) != 0);
    g_aimRealTime.store(IniInt("AimRealTimeInSlowMotion", 0) != 0);
    g_snapLockMaxMs.store(std::clamp(IniInt("SnapLockMaxMs", 300), 100, 2000));
    g_turnRate.store(std::clamp(IniInt("TurnRate", 720), 90, 3600));
    g_dodgeReload.store(IniInt("DodgeReload", 0) != 0);
}

// ---------- clips ----------
// A clip written by re\acro\bw2lara.js (same resource layout as the acrobatics and grapple clips).
bool LoadClipFile(Clip& clip) {
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%sROTTRFramework\\brawl\\%s", GameDir(), clip.file);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log::Writef("Brawl: clip file %s not found", path);
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    if (size.QuadPart < 0x100 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(h);
        Log::Writef("Brawl: clip file %s has an unexpected size (%lld bytes)", path, size.QuadPart);
        return false;
    }
    const auto fileSize = static_cast<DWORD>(size.QuadPart);
    auto* raw = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, fileSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    DWORD read = 0;
    const bool ok = raw && ReadFile(h, raw, fileSize, &read, nullptr) && read == fileSize;
    CloseHandle(h);
    if (!ok) {
        if (raw) VirtualFree(raw, 0, MEM_RELEASE);
        Log::Writef("Brawl: could not read %s", path);
        return false;
    }
    auto i32 = [&](std::size_t o) { return *reinterpret_cast<const std::int32_t*>(raw + o); };
    const int internalCount = i32(0);
    const std::size_t bodyOffset = 20 + static_cast<std::size_t>(internalCount) * 8;
    if (internalCount <= 0 || internalCount > 64 || i32(4) || i32(8) || i32(12) || i32(16) || bodyOffset + 0xA8 > fileSize) {
        VirtualFree(raw, 0, MEM_RELEASE);
        Log::Detailf("Brawl: %s is not a clip written by the converter", clip.file);
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
        Log::Writef("Brawl: %s has bad pointer data", clip.file);
        return false;
    }
    clip.data = body;
    clip.frames = *reinterpret_cast<const std::uint16_t*>(body + 0x32);
    return true;
}

// Channel walk of a converted clip (see acro.cpp ScaleRootTravelUnsafe, grapple.cpp FindTracksUnsafe): the root's
// (global id 1) and the anchor's (global id 172) baked y translation (channel 5).
void FindTracksUnsafe(Clip& clip) {
    clip.rootY = clip.anchorY = nullptr;
    if (clip.frames <= 1 || clip.frames > kMaxFrames) return;
    std::uint8_t* body = clip.data;
    const auto* ids = *reinterpret_cast<const std::uint16_t* const*>(body + 0x68);
    const auto* flags = *reinterpret_cast<const std::uint8_t* const*>(body + 0x70);
    const auto* meta = *reinterpret_cast<const std::uint8_t* const*>(body + 0x78);
    auto* values = *reinterpret_cast<std::uint8_t* const*>(body + 0x80);
    const int bones = body[0x37], frames = clip.frames;
    const std::uint8_t* fp = flags + 2;
    const std::uint8_t* mp = meta;
    auto* vp = reinterpret_cast<float*>(values + 4);
    for (int b = 0; b < bones; ++b) {
        std::uint32_t fl = *fp++;
        if (fl & 0x80) fl = (fl & 0x7Fu) | (static_cast<std::uint32_t>(*fp++) << 8);
        for (int c = 0; c < 12; ++c) {
            if (!(fl & (1u << c))) continue;
            const bool baked = *reinterpret_cast<const std::uint16_t*>(mp) == 0;
            mp += baked ? 4 : 2;
            if (baked && ids[b] == 1 && c == 5) clip.rootY = vp;
            if (baked && ids[b] == 172 && c == 5) clip.anchorY = vp;
            vp += baked ? frames : 1;
        }
    }
    if (!clip.rootY) return;
    for (int f = 0; f < frames; ++f) {
        clip.baseRootY[f] = clip.rootY[f];
        clip.baseAnchorY[f] = clip.anchorY ? clip.anchorY[f] : 0.0f;
    }
}

bool FindTracksGuarded(Clip& clip) {
    __try {
        FindTracksUnsafe(clip);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        clip.rootY = clip.anchorY = nullptr;
    }
    return clip.rootY != nullptr;
}

// The root's header values after a change of its track (bounds y, end translation y).
void UpdateRootHeaderUnsafe(Clip& clip) {
    float minY = 0.0f, maxY = 0.0f;
    for (int f = 0; f < clip.frames; ++f) {
        minY = (std::min)(minY, clip.rootY[f]);
        maxY = (std::max)(maxY, clip.rootY[f]);
    }
    auto* header = reinterpret_cast<float*>(clip.data);
    header[1] = minY - 150.0f;
    header[4] = maxY + 150.0f;
    header[7] = clip.rootY[clip.frames - 1];
}

// The root goes from its place at frame from - 1 to endY (forward is negative) at the contact frame and stays there;
// the frames before from are left as they are.
void SetRootPathUnsafe(Clip& clip, int from, float endY, bool ease) {
    if (!clip.rootY) return;
    const int frames = clip.frames, contact = std::clamp(clip.contact, 1, frames - 1);
    from = std::clamp(from, 1, frames - 1);
    const float y0 = clip.rootY[from - 1];
    for (int f = from; f < frames; ++f) {
        float y = endY;
        if (f < contact && contact > from - 1) {
            const float u = static_cast<float>(f - (from - 1)) / static_cast<float>(contact - (from - 1));
            const float s = ease ? 1.0f - (1.0f - u) * (1.0f - u) : u;
            y = y0 + (endY - y0) * s;
        }
        clip.rootY[f] = y;
        if (clip.anchorY) clip.anchorY[f] = clip.baseAnchorY[f] - (y - clip.baseRootY[f]);
    }
    clip.rootOurs = true;
    UpdateRootHeaderUnsafe(clip);
}

void RestoreRootPathUnsafe(Clip& clip) {
    if (!clip.rootY || !clip.rootOurs) return;
    for (int f = 0; f < clip.frames; ++f) {
        clip.rootY[f] = clip.baseRootY[f];
        if (clip.anchorY) clip.anchorY[f] = clip.baseAnchorY[f];
    }
    clip.rootOurs = false;
    UpdateRootHeaderUnsafe(clip);
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

bool RegisterClipGuarded(Clip& clip, int searchFrom) {
    __try {
        clip.id = RegisterClipUnsafe(clip, searchFrom);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        clip.id = -1;
    }
    return clip.id > 0;
}

// brawl.ini: [Chain] 1=<file>,<contact frame> ... in the order of the combo. A file with "kick" in its name is a kick.
void LoadClipsGameThread() {
    char ini[MAX_PATH];
    std::snprintf(ini, sizeof(ini), "%sROTTRFramework\\brawl\\brawl.ini", GameDir());
    g_clipCount = 0;
    for (int i = 1; i <= kMaxClips; ++i) {
        char key[8], value[128];
        std::snprintf(key, sizeof(key), "%d", i);
        GetPrivateProfileStringA("Chain", key, "", value, sizeof(value), ini);
        if (!value[0]) break;
        Clip& clip = g_clips[g_clipCount];
        clip = Clip{};
        char* comma = std::strchr(value, ',');
        if (comma) *comma = '\0';
        strncpy_s(clip.file, value, _TRUNCATE);
        clip.contact = comma ? std::atoi(comma + 1) : 8;
        clip.kick = std::strstr(clip.file, "kick") != nullptr;
        if (!LoadClipFile(clip)) continue;
        clip.contact = std::clamp(clip.contact, 1, (std::max)(1, clip.frames - 2));
        if (!RegisterClipGuarded(clip, 31400 - g_clipCount)) {
            Log::Writef("Brawl: no free anim id for %s", clip.file);
            continue;
        }
        const bool tracks = FindTracksGuarded(clip);
        Log::Detailf("Brawl: clip %d '%s' (%s): %d frames, contact at frame %d, anim id %d, root track %s (its own travel %.0f cm)",
                    g_clipCount + 1, clip.file, clip.kick ? "kick" : "punch", clip.frames, clip.contact, clip.id,
                    tracks ? "found" : "not found: no approach with it", tracks ? -clip.baseRootY[clip.frames - 1] : 0.0f);
        ++g_clipCount;
    }
    g_clipsLoaded = g_clipCount > 0;
    g_clipsFailed = !g_clipsLoaded;
    if (!g_clipsLoaded) {
        std::snprintf(g_status, sizeof(g_status), "no clips (ROTTRFramework\\brawl\\brawl.ini)");
        Log::Detailf("Brawl: no clips loaded from %s; hand-to-hand off", ini);
    }
}

// ---------- library entries ----------
// Every loaded anim library with pistol melee entries (the registry's hash map: buckets at +0, count at +0x18; nodes
// {next, u32 key +8, dtp::AnimLibrary* +0x10}; library: entry count +0x10, entries +0x18), and the fx lists the sounds
// are taken from.
void ScanSlotsUnsafe() {
    const std::uint8_t* map = *g_registry;
    if (!map) return;
    const auto* const* buckets = *reinterpret_cast<const std::uint8_t* const* const*>(map);
    const std::uint64_t bucketCount = *reinterpret_cast<const std::uint64_t*>(map + 0x18);
    if (!buckets || bucketCount == 0 || bucketCount > (1u << 20)) return;
    static Slot found[kMaxSlots];
    int count = 0;
    const AnimFxList* sources[4]{};
    for (std::uint64_t b = 0; b < bucketCount; ++b) {
        const std::uint8_t* node = buckets[b];
        for (int guard = 0; node && guard < 4096; ++guard, node = *reinterpret_cast<const std::uint8_t* const*>(node)) {
            auto* library = *reinterpret_cast<std::uint8_t* const*>(node + 0x10);
            if (!library) continue;
            const std::uint32_t entryCount = *reinterpret_cast<const std::uint32_t*>(library + 0x10);
            const auto* entries = *reinterpret_cast<const AnimEntry* const*>(library + 0x18);
            if (!entries || entryCount == 0 || entryCount > 20000) continue;
            for (std::uint32_t i = 0; i < entryCount; ++i) {
                const char* name = entries[i].name;
                if (!name) continue;
                const char c = name[0];
                if (c == 'C' || c == 'i' || c == 'b') {
                    if (!sources[0] && entries[i].fx && NameIs(name, kSourceKick)) sources[0] = static_cast<const AnimFxList*>(entries[i].fx);
                    if (!sources[1] && entries[i].fx && NameIs(name, kSourceFoot)) sources[1] = static_cast<const AnimFxList*>(entries[i].fx);
                    if (!sources[2] && entries[i].fx && NameIs(name, kSourceThud)) sources[2] = static_cast<const AnimFxList*>(entries[i].fx);
                    continue;
                }
                if (c != 'H' || count >= kMaxSlots) continue;
                int host = 0;
                for (const char* wanted : kEntryNamesPush)
                    if (!host && NameIs(name, wanted)) host = kHostPush;
                for (const char* wanted : kEntryNamesContact)
                    if (!host && NameIs(name, wanted)) host = kHostContact;
                if (!host) continue;
                Slot slot;
                slot.library = library;
                slot.index = i;
                slot.original = entries[i];
                slot.host = host;
                // an entry that holds one of our clips now: keep what we know as the game's
                bool known = false;
                for (int k = 0; k < g_slotCount; ++k)
                    if (g_slots[k].library == library && g_slots[k].index == i) {
                        slot = g_slots[k];
                        known = true;
                    }
                if (!known) slot.writable = MakeWritable(const_cast<AnimEntry*>(&entries[i]), sizeof(AnimEntry));
                if (!sources[3] && slot.original.fx && NameIs(name, kEntryNamesPush[0])) sources[3] = static_cast<const AnimFxList*>(slot.original.fx);
                found[count++] = slot;
            }
        }
    }
    if (count != g_slotCount) Logf("Brawl: %d pistol melee entries in the loaded anim libraries (were %d)", count, g_slotCount);
    std::memcpy(g_slots, found, sizeof(Slot) * static_cast<std::size_t>(count));
    g_slotCount = count;
    for (int i = 0; i < 4; ++i) g_sources[i] = sources[i];
}

AnimEntry* SlotEntryUnsafe(const Slot& slot) {
    const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(slot.library + 0x10);
    auto* entries = *reinterpret_cast<AnimEntry* const*>(slot.library + 0x18);
    if (!entries || slot.index >= count) return nullptr;
    AnimEntry* entry = &entries[slot.index];
    return NameIs(entry->name, slot.original.name) ? entry : nullptr;
}

// The fx list first, then animId + animNum as one 32-bit store (the graph may read the entry on another thread).
void WriteEntry(AnimEntry* entry, std::int16_t animId, std::int16_t animNum, void* fx) {
    entry->fx = fx;
    const std::uint32_t both = static_cast<std::uint16_t>(animId) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(animNum)) << 16);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(entry), static_cast<LONG>(both));
}

// The clip in the entries one of the graph's host states plays; fx null: the entry's own list.
void WriteClipUnsafe(const Clip& clip, int host, AnimFxList* fx) {
    for (int i = 0; i < g_slotCount; ++i) {
        if (g_slots[i].host != host || !g_slots[i].writable) continue;
        if (AnimEntry* entry = SlotEntryUnsafe(g_slots[i])) {
            WriteEntry(entry, static_cast<std::int16_t>(clip.id), static_cast<std::int16_t>(clip.id), fx ? fx : g_slots[i].original.fx);
            g_slots[i].written = true;
        }
    }
    g_entriesOurs = true;
}

void RestoreEntriesUnsafe() {
    for (int i = 0; i < g_slotCount; ++i) {
        if (!g_slots[i].written) continue;
        if (AnimEntry* entry = SlotEntryUnsafe(g_slots[i]))
            WriteEntry(entry, g_slots[i].original.animId, g_slots[i].original.animNum, g_slots[i].original.fx);
        g_slots[i].written = false;
    }
    g_entriesOurs = false;
}

// ---------- sounds ----------
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

// Appends an fx list's keyframe-timed triggers of a kind (0 sound, 3 camera) with keyframes kfFrom..kfTo, retimed to
// frame = base + keyframe. Sounds: vocals takes the barks too (only them with onlyVocals); onlySound: just that sound.
int AppendUnsafe(FxBuild& b, const AnimFxList* src, int wantedKind, int kfFrom, int kfTo, int base, int frames, bool vocals,
                 bool onlyVocals, std::uint32_t onlySound) {
    if (!src || !src->triggers) return 0;
    int added = 0;
    const std::uint8_t* t = src->triggers;
    for (int n = 0; n < 64; ++n) {
        const std::uint16_t info = *reinterpret_cast<const std::uint16_t*>(t);
        if (info == 0xFFFF) break;
        const std::size_t size = 8 + ((4u * (info >> 12) + 4u) & ~7u);
        if (size > 64) break;
        const int kind = info & 3, phase = (info >> 10) & 3, keyframe = *reinterpret_cast<const std::int16_t*>(t + 2);
        bool take = kind == wantedKind && !(info & 4) && phase == 0 && keyframe >= kfFrom && keyframe <= kfTo;
        if (take && kind == 0) {
            const int soundType = size >= 16 ? static_cast<std::int8_t>(t[8]) : -1;
            const bool plex = soundType == 0 && size >= 24;
            const bool bark = soundType == 1 || soundType == 3;
            const std::uint32_t id = plex ? SoundIdUnsafe(*reinterpret_cast<const std::uint8_t* const*>(t + 16)) : 0;
            take = (plex && !onlyVocals) || (bark && vocals);
            if (onlySound && id != onlySound) take = false;
        }
        if (take && b.count < 32 && b.used + size + 8 <= sizeof(b.buf)) {
            std::uint8_t* out = b.buf + b.used;
            std::memcpy(out, t, size);
            *reinterpret_cast<std::int16_t*>(out + 2) = static_cast<std::int16_t>(std::clamp(base + keyframe, 1, (std::max)(1, frames - 1)));
            ++b.count;
            b.used += size;
            ++added;
        }
        t += size;
    }
    return added;
}

// A sound trigger of ours: a copy of a template trigger (a plain sound plex one of the source list: its bone and
// flags), with the keyframe and the plex replaced.
bool AppendOwnUnsafe(FxBuild& b, const AnimFxList* src, OwnPlex& plex, std::uint32_t soundId, int frame, int frames) {
    if (!src || !src->triggers) return false;
    const std::uint8_t* t = src->triggers;
    for (int n = 0; n < 64; ++n) {
        const std::uint16_t info = *reinterpret_cast<const std::uint16_t*>(t);
        if (info == 0xFFFF) break;
        const std::size_t size = 8 + ((4u * (info >> 12) + 4u) & ~7u);
        if (size > 64) break;
        const bool plain = (info & 3) == 0 && !(info & 4) && ((info >> 10) & 3) == 0 && size == 24 && static_cast<std::int8_t>(t[8]) == 0;
        const auto* source = plain ? *reinterpret_cast<const std::uint8_t* const*>(t + 16) : nullptr;
        if (source && b.count < 32 && b.used + size + 8 <= sizeof(b.buf)) {
            plex.type = 1;  // Reference
            plex.clickPlay = 0;
            plex.data = &plex.soundId;
            plex.ownerId = *reinterpret_cast<const std::uint32_t*>(source + 0x10);
            plex.soundId = soundId;
            std::uint8_t* out = b.buf + b.used;
            std::memcpy(out, t, size);
            *reinterpret_cast<std::int16_t*>(out + 2) = static_cast<std::int16_t>(std::clamp(frame, 1, (std::max)(1, frames - 1)));
            *reinterpret_cast<const void**>(out + 16) = &plex;
            ++b.count;
            b.used += size;
            return true;
        }
        t += size;
    }
    return false;
}

AnimFxList* StoreFxUnsafe(AnimFxList* list, const FxBuild& b, void* effectLib) {
    if (!list) {
        auto* mem = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, sizeof(AnimFxList) + kFxBufferSize + 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!mem) return nullptr;
        list = reinterpret_cast<AnimFxList*>(mem);
        list->triggers = mem + sizeof(AnimFxList);
        *reinterpret_cast<std::uint16_t*>(list->triggers) = 0xFFFF;
    }
    *reinterpret_cast<std::uint16_t*>(list->triggers) = 0xFFFF;  // (empty while it is written)
    list->effectLib = effectLib;
    if (b.used > 8) std::memcpy(list->triggers + 8, b.buf + 8, b.used - 8);
    *reinterpret_cast<std::uint16_t*>(list->triggers + b.used) = 0xFFFF;
    if (b.used >= 8) std::memcpy(list->triggers, b.buf, 8);
    return list;
}

// Each clip's two fx lists from the sources that are loaded now.
void BuildFxUnsafe() {
    const AnimFxList* kick = g_sources[0];
    const AnimFxList* foot = g_sources[1];
    const AnimFxList* thud = g_sources[2];
    const AnimFxList* pistol = g_sources[3];
    void* effectLib = pistol ? pistol->effectLib : nullptr;
    const bool shake = g_shake.load();
    int total = 0;
    for (int i = 0; i < g_clipCount; ++i) {
        Clip& clip = g_clips[i];
        const int c = clip.contact;
        for (int hit = 0; hit < 2; ++hit) {
            static FxBuild b;
            b.used = 0;
            b.count = 0;
            AppendUnsafe(b, kick, 0, 1, 2, c - 3, clip.frames, false, false, 0);             // the swing: whoosh, cloth
            if (clip.kick) AppendUnsafe(b, kick, 0, 0, 0, c - 3, clip.frames, true, true, 0);  // Lara's shout
            if (hit) {
                // the fist or the foot on the body: the game's punch sound, a body thud under the kicks
                const bool punch = AppendOwnUnsafe(b, kick, g_plexPunch, kSoundPunch, c, clip.frames);
                if (clip.kick) AppendOwnUnsafe(b, kick, g_plexBodyfall, kSoundBodyfall, c, clip.frames);
                if (!punch) {  // no template trigger: the sounds of Lara's own lists
                    AppendUnsafe(b, thud, 0, 0, 0, c, clip.frames, false, false, kSoundThud);
                    if (clip.kick) AppendUnsafe(b, foot, 0, 0, 40, c - 10, clip.frames, false, false, kSoundFoot);
                    else AppendUnsafe(b, pistol, 0, 0, 40, c - 13, clip.frames, false, false, kSoundPistolHit);
                }
                if (clip.kick && shake) AppendUnsafe(b, thud, 3, 4, 4, c - 4, clip.frames, false, false, 0);
            }
            AnimFxList*& list = hit ? clip.fxHit : clip.fxMiss;
            list = StoreFxUnsafe(list, b, effectLib);
            total += b.count;
        }
    }
    for (int i = 0; i < 4; ++i) g_fxBuiltFrom[i] = g_sources[i];
    g_fxShake = shake;
    g_fxBuilt = true;
    g_fxUsable = total > 0;
    Log::DetailOncef("Brawl: sounds built for %d clips (%d triggers) from %s%s%s%s; hits: %s", g_clipCount, total, kick ? "Combat_Kick " : "",
         foot ? "interact_phys_kick_high " : "", thud ? "beam_idle_jump_vertical_land_hard " : "", pistol ? "the pistol melee's list" : "",
         g_plexPunch.type == 1 ? "sound 85 (fight_punch_sharp), 82 (bodyfall thud) under kicks" : "Lara's own thuds (no template trigger)");
}

void EnsureFxUnsafe() {
    bool same = g_fxBuilt && g_fxShake == g_shake.load();
    for (int i = 0; i < 4; ++i)
        if (g_sources[i] != g_fxBuiltFrom[i]) same = false;
    if (same || g_activeClip.load(std::memory_order_relaxed) >= 0) return;
    // our lists are out of the entries while they are rebuilt
    if (g_entriesOurs) RestoreEntriesUnsafe();
    BuildFxUnsafe();
}

// ---------- melee moves ----------
void FindMovesUnsafe(std::uint8_t* lara) {
    auto* component = static_cast<std::uint8_t*>(g_tryToGetMeleeMove(lara));
    if (!component) return;
    auto* dtp = *reinterpret_cast<std::uint8_t**>(component + kMeleeComponentDtp);
    if (!dtp || dtp == g_moveDtpRoot) return;
    g_moveDtpRoot = dtp;
    for (Move& m : g_moves) m = Move{};
    auto* moves = *reinterpret_cast<std::uint8_t**>(dtp + 0x10);
    const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(dtp + 0x18);
    if (!moves || count == 0 || count > 4096) return;
    int found = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint8_t* move = moves + static_cast<std::size_t>(i) * kMoveSize;
        const char* name = *reinterpret_cast<const char* const*>(move + kMoveName);
        for (int k = 0; k < kMaxMoves; ++k) {
            if (g_moves[k].data || !NameIs(name, kMoveNames[k]) || !MakeWritable(move, kMoveSize)) continue;
            std::memcpy(g_moves[k].original, move, kMoveSize);
            g_moves[k].data = move;
            ++found;
        }
    }
    Log::DetailOncef("Brawl: melee data %p: %d of %d melee moves found (the pushes and the axe steps)", dtp, found, kMaxMoves);
}

// While the brawl runs the game's moves of the push states do nothing themselves: no collision, no strike volume, no
// lunge, no events (the hit is ours).
void PatchMovesUnsafe() {
    for (Move& m : g_moves) {
        if (!m.data || m.patched) continue;
        std::memcpy(m.data, m.original, kMoveSize);
        auto* frames = reinterpret_cast<std::int32_t*>(m.data + kMoveFrames);
        frames[0] = 100000;
        frames[1] = 100001;
        auto* strike = reinterpret_cast<std::int32_t*>(m.data + kMoveStrikeFrames);
        strike[0] = 100000;
        strike[1] = 100001;
        m.data[kMoveVolumeOn] = 0;
        m.data[kMoveLungeOn] = 0;
        *reinterpret_cast<std::uint32_t*>(m.data + kMoveEventSelf) = 0;
        *reinterpret_cast<std::uint32_t*>(m.data + kMoveEventVictim) = 0;
        *reinterpret_cast<std::uint32_t*>(m.data + kMoveEventVictimVolume) = 0;
        m.patched = true;
    }
}

void RestoreMovesUnsafe() {
    for (Move& m : g_moves) {
        if (m.data && m.patched) std::memcpy(m.data, m.original, kMoveSize);
        m.patched = false;
    }
}

bool MovesPatched() {
    for (const Move& m : g_moves)
        if (m.patched) return true;
    return false;
}

// Our hit's move: a copy of a pistol push (its pointers and multipliers), with our damage and reaction.
bool BuildHitMoveUnsafe(bool kick) {
    const Move* source = nullptr;
    for (const Move& m : g_moves)
        if (!source && m.data) source = &m;
    if (!source) return false;
    std::uint8_t* m = g_hitMove;
    std::memcpy(m, source->original, kMoveSize);
    const float damage = static_cast<float>(g_damage.load());
    m[kMoveConnectFx] = 0;
    *reinterpret_cast<float*>(m + kMoveDamage) = damage;
    *reinterpret_cast<float*>(m + kMoveStrikeDamage) = damage;
    for (std::size_t o = 0x0C; o <= 0x14; o += 4) *reinterpret_cast<float*>(m + o) = 1.0f;   // difficulty multipliers
    for (std::size_t o = 0x1C; o <= 0x24; o += 4) *reinterpret_cast<float*>(m + o) = 1.0f;
    *reinterpret_cast<std::int32_t*>(m + kMoveDamagePrim) = -1;
    m[0x2C] = m[0x2D] = m[0x2E] = 0;
    int type = kDamageTypes[std::clamp(g_damageType.load(), 0, kDamageTypeCount - 1)];
    if (type == 52 && kick) type = 53;
    *reinterpret_cast<std::int32_t*>(m + kMoveDamageType) = type;
    const int reaction = kReactionTypes[std::clamp(g_reaction.load(), 0, kReactionCount - 1)];
    *reinterpret_cast<std::int32_t*>(m + kMoveEffect) = 2;  // DamageEffect_HitReactSelect
    *reinterpret_cast<std::int32_t*>(m + kMoveEffectUnion) = reaction;
    *reinterpret_cast<std::int32_t*>(m + kMoveEffectUnion + 4) = 0;
    *reinterpret_cast<std::int32_t*>(m + kMoveEffectUnion + 8) = 0;
    *reinterpret_cast<std::int32_t*>(m + kMoveArmorOverride) = 0;
    *reinterpret_cast<std::uint32_t*>(m + kMoveEventSelf) = 0;
    *reinterpret_cast<std::uint32_t*>(m + kMoveEventVictim) = 0;
    *reinterpret_cast<std::uint32_t*>(m + kMoveEventVictimVolume) = 0;
    m[kMoveCustomStrikeDamage] = 0;
    m[kMoveIgnoreHealthStates] = 0;
    m[kMoveCanNotKill] = g_lethal.load() ? 0 : 1;
    m[kMoveCanNotKillExceptLow] = 0;
    m[kMoveHurtsAllies] = 0;
    m[kMoveNoBroadcast] = 0;
    return true;
}

// ---------- the axe of the melee step states ----------
void FindEquipPropsUnsafe(const std::uint8_t* agent) {
    const auto* data = *reinterpret_cast<const std::uint8_t* const*>(agent + kAgentData);
    if (!data || data == g_equipData) return;
    g_equipData = data;
    g_equipPropCount = 0;
    g_equipOff = false;
    const std::uint16_t count = *reinterpret_cast<const std::uint16_t*>(data + kAgentDataNumStates);
    auto* const* states = *reinterpret_cast<std::uint8_t* const* const*>(data + kAgentDataStateRefs);
    if (!states) return;
    int stateCount = 0;
    for (std::uint16_t i = 0; i < count; ++i) {
        std::uint8_t* state = states[i];
        if (!state) continue;
        const char* name = *reinterpret_cast<const char* const*>(state + kStateName);
        if (!NameHas(name, "melee_pickaxe_step")) continue;
        ++stateCount;
        const std::uint16_t props = *reinterpret_cast<const std::uint16_t*>(state + kStatePropCount);
        std::uint8_t* list = *reinterpret_cast<std::uint8_t* const*>(state + kStateProps);
        if (!list || props > 256) continue;
        for (std::uint16_t p = 0; p < props && g_equipPropCount < kMaxEquipProps; ++p) {
            std::uint8_t* prop = list + static_cast<std::size_t>(p) * kPropSize;
            const std::int16_t id = *reinterpret_cast<const std::int16_t*>(prop);
            if ((id != kPropInventoryEquip && id != 0) || *reinterpret_cast<const std::int16_t*>(prop + 2) != kPropTypeInventoryGroup ||
                *reinterpret_cast<const std::int32_t*>(prop + kPropValue) != kInventoryGroupMelee || !MakeWritable(prop, kPropSize))
                continue;
            *reinterpret_cast<std::int16_t*>(prop) = kPropInventoryEquip;  // (an id left 0 by an earlier session of ours)
            g_equipProps[g_equipPropCount++] = reinterpret_cast<std::int16_t*>(prop);
        }
    }
    Log::DetailOncef("Brawl: state control data %p: %d melee step states, %d of their properties put the axe in her hand", data, stateCount,
         g_equipPropCount);
}

// The push state's first property becomes "Inventory_Equip = InventoryGroup 2" (the pistols), or is the game's again.
void SetPistolEquipUnsafe(bool on) {
    if (!g_pushProp) return;
    if (on) {
        if (g_pushPropOurs) return;
        *reinterpret_cast<std::int16_t*>(g_pushProp) = kPropInventoryEquip;
        *reinterpret_cast<std::int16_t*>(g_pushProp + 2) = kPropTypeInventoryGroup;
        *reinterpret_cast<std::uint32_t*>(g_pushProp + 4) = 0;
        *reinterpret_cast<std::int32_t*>(g_pushProp + kPropValue) = kInventoryGroupPistol;
        *reinterpret_cast<std::int32_t*>(g_pushProp + kPropValue + 4) = 0;
        g_pushPropOurs = true;
    } else if (g_pushPropOurs) {
        std::memcpy(g_pushProp, g_pushPropOriginal, kPropSize);
        g_pushPropOurs = false;
    }
}

void SetAxeEquipUnsafe(bool axe) {
    for (int i = 0; i < g_equipPropCount; ++i) *g_equipProps[i] = axe ? kPropInventoryEquip : static_cast<std::int16_t>(0);
    g_equipOff = !axe;
}

// The chain's host states and the carrier transition, by name.
void FindStatesUnsafe(const std::uint8_t* agent) {
    const auto* data = *reinterpret_cast<const std::uint8_t* const*>(agent + kAgentData);
    if (!data || data == g_stateData) return;
    g_stateData = data;
    g_statePush = g_stateContact = g_stateAxeIdle = g_stateAxeMove = -1;
    g_pushProp = nullptr;
    g_pushPropOurs = false;
    g_carrier = nullptr;
    const std::uint16_t count = *reinterpret_cast<const std::uint16_t*>(data + kAgentDataNumStates);
    auto* const* states = *reinterpret_cast<std::uint8_t* const* const*>(data + kAgentDataStateRefs);
    for (std::uint16_t i = 0; states && i < count; ++i) {
        if (!states[i]) continue;
        const char* name = *reinterpret_cast<const char* const*>(states[i] + kStateName);
        if (g_statePush < 0 && NameIs(name, kStatePushName)) g_statePush = i;
        if (g_stateContact < 0 && NameIs(name, kStateContactName)) g_stateContact = i;
        if (g_stateAxeIdle < 0 && NameIs(name, kStateAxeIdleName)) g_stateAxeIdle = i;
        if (g_stateAxeMove < 0 && NameIs(name, kStateAxeMoveName)) g_stateAxeMove = i;
    }
    const std::uint16_t transitions = *reinterpret_cast<const std::uint16_t*>(data + kAgentDataNumTransitions);
    auto* const* refs = *reinterpret_cast<std::uint8_t* const* const*>(data + kAgentDataTransitionRefs);
    for (std::uint16_t i = 0; refs && i < transitions && !g_carrier; ++i) {
        if (!refs[i]) continue;
        const char* name = *reinterpret_cast<const char* const*>(refs[i] + kTransitionName);
        if (NameIs(name, kCarrierName) && MakeWritable(refs[i], 8)) g_carrier = refs[i];
    }
    if (g_statePush >= 0 && states[g_statePush]) {
        std::uint8_t* state = states[g_statePush];
        const std::uint16_t props = *reinterpret_cast<const std::uint16_t*>(state + kStatePropCount);
        std::uint8_t* list = *reinterpret_cast<std::uint8_t* const*>(state + kStateProps);
        // (a property of ours left by an earlier session of the framework is not taken for the game's)
        if (list && props >= 1 && props <= 64 && *reinterpret_cast<const std::int16_t*>(list) != kPropInventoryEquip &&
            MakeWritable(list, kPropSize)) {
            std::memcpy(g_pushPropOriginal, list, kPropSize);
            g_pushProp = list;
        }
        Log::DetailOncef("Brawl: the push state has %u properties; its first one (id %d, type %d) %s", props,
             list ? *reinterpret_cast<const std::int16_t*>(list) : -1, list ? *reinterpret_cast<const std::int16_t*>(list + 2) : -1,
             g_pushProp ? "can carry the pistols' equip" : "cannot be used: no pistol equip from the axe");
    }
    Log::DetailOncef("Brawl: the axe's first step: '%s' %d, '%s' %d", kStateAxeIdleName, g_stateAxeIdle, kStateAxeMoveName, g_stateAxeMove);
    Log::DetailOncef("Brawl: chain states: '%s' %d, '%s' %d, carrier transition '%s' %s", kStatePushName, g_statePush, kStateContactName,
         g_stateContact, kCarrierName, g_carrier ? "found" : "NOT found (each strike starts from the melee press)");
}

bool ChainReady() {
    return g_statePush >= 0 && g_stateContact >= 0 && g_carrier != nullptr;
}

// ---------- the enemies ----------
float HealthOfUnsafe(std::uint8_t* instance, float* maximum) {
    if (!g_healthReadable || !instance) return -1.0f;
    auto* health = static_cast<std::uint8_t*>(g_tryToGetHealth(instance));
    auto* values = health ? *reinterpret_cast<std::uint8_t* const*>(health + kHealthValues) : nullptr;
    if (!values) return -1.0f;
    if (maximum) *maximum = static_cast<float>(*reinterpret_cast<const std::int32_t*>(health + kHealthMax));
    return *reinterpret_cast<const float*>(values + kHealthCurrent);
}

// Lara's facing (unit x, y), from her transform.
void ReadForwardUnsafe(std::uint8_t* lara) {
    if (!g_forwardAvailable) return;
    alignas(16) float out[4]{};
    g_getForward(lara, out);
    const float length = std::hypot(out[0], out[1]);
    if (!std::isfinite(length) || length < 0.1f) return;
    g_laraForwardX.store(out[0] / length, std::memory_order_relaxed);
    g_laraForwardY.store(out[1] / length, std::memory_order_relaxed);
}

// The living enemies around Lara: the nearest one within the range (and not more than 2 m above or below) for the pad
// hook, and the locked one: the enemy nearest the camera's direction (its angle from it, plus 5 degrees per metre of
// distance), chosen again whenever choosing is allowed (not while a strike closes in); the locked one is kept unless
// another is clearly better (12 points).
void FindEnemiesUnsafe(std::uint8_t* lara, bool comboRuns, bool mayChoose) {
    const auto* a = reinterpret_cast<const float*>(lara + kInstancePos);
    const float range = static_cast<float>(g_range.load());
    float look[3] = {g_laraForwardX.load(std::memory_order_relaxed), g_laraForwardY.load(std::memory_order_relaxed), 0.0f};
    bool byCamera = false;
    if (g_cameraTarget.load() && CombatAir::CameraDirection(lara, look)) byCamera = true;
    const float lookLength = std::hypot(look[0], look[1]);
    if (lookLength > 0.05f) {
        look[0] /= lookLength;
        look[1] /= lookLength;
    } else {
        look[0] = g_laraForwardX.load(std::memory_order_relaxed);
        look[1] = g_laraForwardY.load(std::memory_order_relaxed);
    }
    float best = 1e9f, bestScore = 1e9f, lockedScore = 1e9f;
    std::uint8_t* nearest = nullptr;
    std::uint8_t* chosen = nullptr;
    bool lockedStillThere = false;
    const std::uint8_t* npc = *g_npcList;
    for (int guard = 0; npc && guard < kMaxNpcs; ++guard, npc = *reinterpret_cast<const std::uint8_t* const*>(npc + kNpcNext)) {
        auto* inst = *reinterpret_cast<std::uint8_t* const*>(npc + kNpcInstance);
        if (!inst || inst == lara || (npc[kNpcDead] & 1)) continue;
        const auto* b = reinterpret_cast<const float*>(inst + kInstancePos);
        const float dx = b[0] - a[0], dy = b[1] - a[1];
        const float dist = std::hypot(dx, dy);
        if (std::fabs(b[2] - a[2]) > 200.0f) continue;
        const bool locked = inst == g_target && comboRuns && dist <= range + 250.0f;
        if (dist > range && !locked) continue;
        if (!g_isEnemyOf(const_cast<std::uint8_t*>(npc), lara)) continue;
        const float cosine = dist > 1.0f ? std::clamp((dx * look[0] + dy * look[1]) / dist, -1.0f, 1.0f) : 1.0f;
        const float score = std::acos(cosine) * 57.29578f + dist * 0.05f;
        if (locked) {
            lockedStillThere = true;
            lockedScore = score;
        }
        if (dist <= range && dist < best) {
            best = dist;
            nearest = inst;
        }
        if (dist <= range && score < bestScore) {
            bestScore = score;
            chosen = inst;
        }
    }
    g_enemyNear.store(nearest != nullptr, std::memory_order_relaxed);
    g_enemyDistance.store(nearest ? best : -1.0f, std::memory_order_relaxed);
    const bool better = chosen && chosen != g_target && bestScore + 12.0f < lockedScore;
    if (!lockedStillThere || (mayChoose && better)) {
        if (g_target != chosen && chosen && comboRuns)
            Logf("Brawl: lock-on: enemy %p (%.0f points by %s; the one before had %.0f)", chosen, bestScore,
                 byCamera ? "the camera's direction" : "Lara's facing", lockedStillThere ? lockedScore : -1.0f);
        g_target = chosen;
    }
    if (g_target) {
        const auto* p = reinterpret_cast<const float*>(g_target + kInstancePos);
        g_targetX.store(p[0], std::memory_order_relaxed);
        g_targetY.store(p[1], std::memory_order_relaxed);
        g_targetZ.store(p[2], std::memory_order_relaxed);
    }
    g_targetValid.store(g_target != nullptr, std::memory_order_relaxed);
    g_lockedEnemy.store(g_target, std::memory_order_relaxed);
}

bool TargetDistanceUnsafe(const std::uint8_t* lara, float& dist, float& dz) {
    if (!g_target || !g_targetValid.load(std::memory_order_relaxed)) return false;
    const auto* a = reinterpret_cast<const float*>(lara + kInstancePos);
    const auto* b = reinterpret_cast<const float*>(g_target + kInstancePos);
    dist = std::hypot(b[0] - a[0], b[1] - a[1]);
    dz = b[2] - a[2];
    return std::isfinite(dist);
}

// ---------- the weapon in her hand ----------
std::uint8_t* InventoryOfUnsafe(std::uint8_t* lara) {
    const std::uint32_t id = *g_inventoryId;
    auto* const* components = *reinterpret_cast<std::uint8_t* const* const*>(lara + kInstanceComponents);
    return components && id < *reinterpret_cast<const std::uint32_t*>(lara + kInstanceComponentCount) ? components[id] : nullptr;
}

void NoteCurrentItemUnsafe(std::uint8_t* lara) {
    std::uint8_t* inventory = InventoryOfUnsafe(lara);
    const std::uint8_t* current = inventory ? *reinterpret_cast<const std::uint8_t* const*>(inventory + kInventoryCurrent) : nullptr;
    if (!current) return;
    const std::uint32_t id = *reinterpret_cast<const std::uint32_t*>(current + kCurrentItemId);
    if (id && id != g_axeItemId) g_lastItemId = id;
    g_itemNow = id;
    g_itemInHand = id != 0 && id != g_axeItemId;
}

// The weapon in Lara's hand is full again (the inventory's current item, else the last one she held; every weapon
// instance of it).
void ReloadCurrentWeaponUnsafe(std::uint8_t* lara) {
    const char* why = nullptr;
    std::uint32_t itemId = 0;
    std::uint8_t* inventory = InventoryOfUnsafe(lara);
    if (!inventory) why = "Lara has no inventory component";
    const std::uint8_t* current = inventory ? *reinterpret_cast<const std::uint8_t* const*>(inventory + kInventoryCurrent) : nullptr;
    const auto* const* buckets = inventory ? *reinterpret_cast<const std::uint8_t* const* const*>(inventory + kInventoryItemBuckets) : nullptr;
    const std::uint64_t bucketCount = inventory ? *reinterpret_cast<const std::uint64_t*>(inventory + kInventoryItemBucketCount) : 0;
    if (!why && (!current || !buckets || bucketCount == 0 || bucketCount > 65536)) why = "the inventory has no current item data";
    if (!why) {
        itemId = *reinterpret_cast<const std::uint32_t*>(current + kCurrentItemId);
        if (!itemId) itemId = g_lastItemId;   // (no weapon in her hand: the last one she held)
        const std::uint8_t* node = itemId ? buckets[itemId % bucketCount] : nullptr;
        for (int guard = 0; node && guard < 256; ++guard, node = *reinterpret_cast<const std::uint8_t* const*>(node)) {
            if (*reinterpret_cast<const std::uint32_t*>(node + 8) != itemId) continue;
            g_itemReload(const_cast<std::uint8_t*>(node) + 0x10, inventory, false, false);
            g_reloads.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        why = itemId ? "the item is not in the inventory's item map" : "no weapon in her hand and none held before";
    }
    if (g_dodgeLogBudget.fetch_sub(1) > 0)
        Log::Detailf("Brawl: dodge reload at '%s' not done: %s (item id %u, %llu buckets)", g_reloadState, why, itemId,
                    static_cast<unsigned long long>(bucketCount));
}

// ---------- a strike ----------
const char* CurrentStateNameUnsafe(const std::uint8_t* agent) {
    const auto* state = *reinterpret_cast<const std::uint8_t* const*>(agent + kAgentCurrentState);
    return state ? *reinterpret_cast<const char* const*>(state + kStateName) : nullptr;
}

float StrikeFrame(ULONGLONG now) {
    const ULONGLONG start = g_activeStartMs.load(std::memory_order_relaxed);
    return now > start ? static_cast<float>(now - start) * kFramesPerMs : 0.0f;
}

void HoldAnimMotionUnsafe(std::uint8_t* lara) {
    if (!g_motionAvailable || g_approach.load() != 1) return;
    auto* anim = static_cast<std::uint8_t*>(g_getAnimComponent(lara));
    if (!anim) return;
    const std::int32_t type = *reinterpret_cast<const std::int32_t*>(anim + kAnimMotionType);
    if (!g_motionHeld || anim != g_motionAnim) {
        g_motionAnim = anim;
        g_motionBefore = type;
        g_motionHeld = true;
    }
    if (type != kMotionFromAnimation) *reinterpret_cast<std::int32_t*>(anim + kAnimMotionType) = kMotionFromAnimation;
    anim[kAnimMotionFlags] |= 1;  // root translation on
}

// After the strikes: the state's own movement again, unless the graph has chosen a source of its own meanwhile.
void ReleaseAnimMotionUnsafe(std::uint8_t* lara) {
    if (!g_motionHeld) return;
    g_motionHeld = false;
    auto* anim = lara ? static_cast<std::uint8_t*>(g_getAnimComponent(lara)) : nullptr;
    if (!anim || anim != g_motionAnim) return;
    if (g_motionBefore == kMotionFromMovement && *reinterpret_cast<const std::int32_t*>(anim + kAnimMotionType) == kMotionFromAnimation) {
        *reinterpret_cast<std::int32_t*>(anim + kAnimMotionType) = kMotionFromMovement;
        anim[kAnimMotionFlags] &= static_cast<std::uint8_t>(~1u);
    }
}

// The next strike is asked for: its clip into the entries of the host state that will play it, the root track to the
// enemy, the moves and the axe out of the way; then the melee press (first strike) or the forced change (chain).
void ArmUnsafe(std::uint8_t* lara, int clipIndex, int host, bool force, bool first, ULONGLONG now) {
    Clip& clip = g_clips[clipIndex];
    g_armedPress.store(g_pressCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_forceFirst.store(force && first, std::memory_order_relaxed);
    float dist = -1.0f, dz = 0.0f;
    const bool haveTarget = g_lockOn.load() && TargetDistanceUnsafe(lara, dist, dz);
    const float stop = static_cast<float>(g_stopDistance.load());
    const float maxApproach = static_cast<float>(g_maxApproach.load());
    const int approach = g_approach.load();
    const bool reachable = haveTarget && dist <= stop + (approach ? maxApproach : 0.0f) + 60.0f;
    float travel = 0.0f;
    if (approach == 1 && haveTarget && clip.rootY) {
        travel = std::clamp(dist - stop, 0.0f, maxApproach);
        SetRootPathUnsafe(clip, 1, -travel, true);
    } else if (approach == 2 && clip.rootY) {
        SetRootPathUnsafe(clip, 1, 0.0f, true);  // in place: the tick moves her
    } else {
        RestoreRootPathUnsafe(clip);
    }
    g_strikePlanned = travel;
    AnimFxList* fx = g_sounds.load() && g_fxUsable ? (reachable ? clip.fxHit : clip.fxMiss) : nullptr;
    WriteClipUnsafe(clip, host, fx);
    PatchMovesUnsafe();
    SetAxeEquipUnsafe(false);
    SetPistolEquipUnsafe(Features::CurrentWeaponClassCached() == static_cast<int>(Features::WeaponClass::Pistol));
    g_armedHost.store(host, std::memory_order_relaxed);
    g_armedForce.store(force, std::memory_order_relaxed);
    g_armedMs.store(now, std::memory_order_relaxed);
    g_armedClip.store(clipIndex, std::memory_order_relaxed);
    if (force) {
        g_forceState.store(host == kHostContact ? g_stateContact : g_statePush, std::memory_order_relaxed);
        g_forced.store(nullptr, std::memory_order_relaxed);
        g_forceArmed.store(true, std::memory_order_relaxed);
        g_pendingUntilMs = now + kForceMs;
    } else {
        g_pulseUntilMs.store(now + kPulseMs, std::memory_order_relaxed);
        g_brawlPulseMs.store(now, std::memory_order_relaxed);
        g_pendingUntilMs = now + 450;
    }
    Logf("Brawl: strike asked for (%s): clip %d '%s' for the %s state, enemy %s%.0f cm away (dz %+.0f), planned approach %.0f cm, sounds %s, item %u in her hand",
         !force ? "melee press" : first ? "first, forced" : "chain", clipIndex + 1, clip.file, host == kHostContact ? "contact" : "push", haveTarget ? "" : "none / ",
         dist, dz, travel, !fx ? "the entry's own" : reachable ? "with the hit" : "the swing only", g_itemNow);
}

// A strike starts (from the state change hooks).
const std::uint8_t* StateByIndexUnsafe(const std::uint8_t* agent, int index) {
    const auto* data = *reinterpret_cast<const std::uint8_t* const*>(agent + kAgentData);
    if (!data || index < 0 || index >= *reinterpret_cast<const std::uint16_t*>(data + kAgentDataNumStates)) return nullptr;
    const auto* const* states = *reinterpret_cast<const std::uint8_t* const* const*>(data + kAgentDataStateRefs);
    return states ? states[index] : nullptr;
}

void StartStrikeUnsafe(void* agent, int clipIndex, int host, int stateIndex, const char* state, const char* via) {
    const ULONGLONG now = GetTickCount64();
    g_strikeState = StateByIndexUnsafe(static_cast<const std::uint8_t*>(agent), stateIndex);
    g_armedClip.store(-1, std::memory_order_relaxed);
    g_chainIndex = clipIndex;
    g_hitDone.store(false, std::memory_order_relaxed);
    g_activeHost.store(host, std::memory_order_relaxed);
    g_activeStartMs.store(now, std::memory_order_relaxed);
    g_lastStrikeStartMs.store(now, std::memory_order_relaxed);
    g_activeClip.store(clipIndex, std::memory_order_relaxed);
    g_pendingUntilMs = 0;
    g_forceFails = 0;
    g_pressHandled = g_armedPress.load(std::memory_order_relaxed);
    g_forceFirst.store(false, std::memory_order_relaxed);
    g_exitWanted.store(false, std::memory_order_relaxed);
    auto* lara = *reinterpret_cast<std::uint8_t* const*>(static_cast<std::uint8_t*>(agent) + kAgentInstance);
    if (lara) {
        const auto* p = reinterpret_cast<const float*>(lara + kInstancePos);
        g_strikeStartPos[0] = p[0];
        g_strikeStartPos[1] = p[1];
        g_strikeStartPos[2] = p[2];
        HoldAnimMotionUnsafe(lara);
    }
    const unsigned n = g_strikes.fetch_add(1, std::memory_order_relaxed) + 1;
    Logf("Brawl: strike #%u starts in '%s' (via '%s') with clip %d, %u ms after it was asked for; motion source %d -> %s", n,
         state ? state : "?", via ? via : "?", clipIndex + 1, static_cast<unsigned>(now - g_armedMs.load(std::memory_order_relaxed)),
         g_motionBefore, g_motionHeld ? "the animation" : "unchanged");
}

void EndStrikeUnsafe(std::uint8_t* lara, const char* why, ULONGLONG now) {
    const int clip = g_activeClip.exchange(-1, std::memory_order_relaxed);
    if (clip < 0) return;
    g_lastStrikeEndMs = now;
    const bool next = g_armedClip.load(std::memory_order_relaxed) >= 0;
    if (!next) ReleaseAnimMotionUnsafe(lara);
    const auto* p = reinterpret_cast<const float*>(lara + kInstancePos);
    Logf("Brawl: strike with clip %d over (%s) after %u ms; Lara moved %.0f cm (planned %.0f)%s", clip + 1, why,
         static_cast<unsigned>(now - g_activeStartMs.load(std::memory_order_relaxed)),
         std::hypot(p[0] - g_strikeStartPos[0], p[1] - g_strikeStartPos[1]), g_strikePlanned, next ? "; the next one is asked for" : "");
}

// The other living enemies within reach in front of Lara (100 degrees to each side) get the strike too.
int HitOthersUnsafe(std::uint8_t* lara, std::uint8_t* agent, float reach) {
    const auto* a = reinterpret_cast<const float*>(lara + kInstancePos);
    const float fx = g_laraForwardX.load(std::memory_order_relaxed), fy = g_laraForwardY.load(std::memory_order_relaxed);
    std::uint8_t* victims[3]{};
    int count = 0;
    const std::uint8_t* npc = *g_npcList;
    for (int guard = 0; npc && guard < kMaxNpcs && count < 3; ++guard, npc = *reinterpret_cast<const std::uint8_t* const*>(npc + kNpcNext)) {
        auto* inst = *reinterpret_cast<std::uint8_t* const*>(npc + kNpcInstance);
        if (!inst || inst == lara || inst == g_target || (npc[kNpcDead] & 1)) continue;
        const auto* b = reinterpret_cast<const float*>(inst + kInstancePos);
        const float dx = b[0] - a[0], dy = b[1] - a[1];
        const float dist = std::hypot(dx, dy);
        if (dist > reach || std::fabs(b[2] - a[2]) > 150.0f) continue;
        if (dist > 1.0f && (dx * fx + dy * fy) / dist < -0.17f) continue;  // behind her
        if (!g_isEnemyOf(const_cast<std::uint8_t*>(npc), lara) || !g_tryToGetHealth(inst)) continue;
        victims[count++] = inst;
    }
    for (int i = 0; i < count; ++i) {
        const std::uint8_t flag = agent[kAgentMeleeHit];
        g_applyMeleeDamage(agent, lara, victims[i], g_hitMove, true);
        agent[kAgentMeleeHit] = flag;
    }
    return count;
}

// The clip's contact frame: the game's melee damage on the locked enemy, if it is within reach.
void DoHitUnsafe(std::uint8_t* lara, std::uint8_t* agent, const Clip& clip) {
    float dist = -1.0f, dz = 0.0f;
    const bool haveTarget = TargetDistanceUnsafe(lara, dist, dz);
    const float reach = static_cast<float>(g_reach.load());
    const bool within = haveTarget && dist <= reach && std::fabs(dz) <= 150.0f;
    if (!within) {
        if (haveTarget)
            Logf("Brawl: contact frame %d of clip '%s': the enemy is out of reach (%.0f cm away, dz %+.0f; reach %.0f)", clip.contact,
                 clip.file, dist, dz, reach);
        else
            Logf("Brawl: contact frame %d of clip '%s': no enemy locked", clip.contact, clip.file);
        if (g_areaHits.load() && BuildHitMoveUnsafe(clip.kick)) {
            const int others = HitOthersUnsafe(lara, agent, reach);
            if (others) Logf("Brawl:   %d other enemies in front of her hit", others);
        }
        return;
    }
    if (!g_tryToGetHealth(g_target) || !BuildHitMoveUnsafe(clip.kick)) {
        Logf("Brawl: contact frame %d: no hit (%s)", clip.contact, g_moveDtpRoot ? "the enemy has no health component" : "no melee move to copy");
        return;
    }
    float maximum = -1.0f;
    const float before = HealthOfUnsafe(g_target, &maximum);
    const std::uint8_t flag = agent[kAgentMeleeHit];
    g_applyMeleeDamage(agent, lara, g_target, g_hitMove, true);
    agent[kAgentMeleeHit] = flag;
    const float after = HealthOfUnsafe(g_target, nullptr);
    const int others = g_areaHits.load() ? HitOthersUnsafe(lara, agent, reach) : 0;
    const unsigned n = g_hits.fetch_add(1, std::memory_order_relaxed) + 1;
    Logf("Brawl: hit #%u (%s '%s', contact frame %d): enemy %p at %.0f cm, damage %d type %d, reaction %d; health %.0f -> %.0f of %.0f; "
         "%d more enemies in front of her hit", n,
         clip.kick ? "kick" : "punch", clip.file, clip.contact, g_target, dist, g_damage.load(),
         *reinterpret_cast<const std::int32_t*>(g_hitMove + kMoveDamageType), *reinterpret_cast<const std::int32_t*>(g_hitMove + kMoveEffectUnion),
         before, after, maximum, others);
}

// Before the contact: the rest of the way follows the enemy.
void UpdateApproachUnsafe(std::uint8_t* lara, Clip& clip, float frame, ULONGLONG now, ULONGLONG lastTickMs) {
    const int approach = g_approach.load();
    if (!approach || !g_lockOn.load()) return;
    float dist = -1.0f, dz = 0.0f;
    if (!TargetDistanceUnsafe(lara, dist, dz)) return;
    const float stop = static_cast<float>(g_stopDistance.load());
    const float remaining = std::clamp(dist - stop, 0.0f, static_cast<float>(g_maxApproach.load()));
    if (approach == 1) {
        const int from = static_cast<int>(frame) + 2;
        if (!clip.rootY || from >= clip.contact) return;
        const float here = clip.rootY[from - 1];
        const float limit = -(static_cast<float>(g_maxApproach.load()) + 100.0f);
        SetRootPathUnsafe(clip, from, (std::max)(here - remaining, limit), false);
        return;
    }
    // position steps: the way that is left, spread over the time to the contact
    const float framesLeft = static_cast<float>(clip.contact) - frame;
    if (framesLeft <= 0.0f || remaining < 1.0f || now <= lastTickMs) return;
    const float dt = static_cast<float>((std::min)(now - lastTickMs, static_cast<ULONGLONG>(100))) * kFramesPerMs;
    const float step = (std::min)(remaining, remaining * dt / (std::max)(framesLeft, dt));
    auto* p = reinterpret_cast<float*>(lara + kInstancePos);
    const float dx = g_target ? reinterpret_cast<const float*>(g_target + kInstancePos)[0] - p[0] : 0.0f;
    const float dy = g_target ? reinterpret_cast<const float*>(g_target + kInstancePos)[1] - p[1] : 0.0f;
    const float length = std::hypot(dx, dy);
    if (length < 1.0f) return;
    p[0] += dx / length * step;
    p[1] += dy / length * step;
}

void RestoreAllUnsafe(std::uint8_t* lara) {
    if (g_entriesOurs) RestoreEntriesUnsafe();
    RestoreMovesUnsafe();
    if (g_equipOff) SetAxeEquipUnsafe(true);
    SetPistolEquipUnsafe(false);
    g_forceGameEntry.store(false, std::memory_order_relaxed);
    g_exitGame.store(false, std::memory_order_relaxed);
    for (int i = 0; i < g_clipCount; ++i) RestoreRootPathUnsafe(g_clips[i]);
    ReleaseAnimMotionUnsafe(lara);
    g_armedClip.store(-1, std::memory_order_relaxed);
    g_forceArmed.store(false, std::memory_order_relaxed);
    g_forceFirst.store(false, std::memory_order_relaxed);
    g_forceAxe.store(false, std::memory_order_relaxed);
    g_forced.store(nullptr, std::memory_order_relaxed);
    g_exitWanted.store(false, std::memory_order_relaxed);
}

// The blend frames of the pistol graph's melee edges (the tick; the edges come from OnPistolGraph).
void ApplyBlendUnsafe(bool ours) {
    const int count = (std::min)(g_blendEdgeCount.load(std::memory_order_acquire), kMaxBlendEdges);
    const int frames = ours ? g_blendFrames.load() : 0;
    if (frames == g_blendApplied && ours) return;
    for (int i = 0; i < count; ++i)
        if (g_blendEdges[i]) *g_blendEdges[i] = ours ? (std::max)(g_blendOriginal[i], static_cast<float>(frames)) : g_blendOriginal[i];
    if (count && frames != g_blendApplied)
        Logf("Brawl: the pistol graph's %d melee edges blend over %s", count, ours ? "our frames" : "the game's frames again");
    g_blendApplied = count ? frames : -1;
}

double NowMs() {
    static double s_ticksPerMs = 0.0;
    LARGE_INTEGER c{};
    if (s_ticksPerMs == 0.0) {
        QueryPerformanceFrequency(&c);
        s_ticksPerMs = static_cast<double>(c.QuadPart) / 1000.0;
    }
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / s_ticksPerMs;
}

// ---------- the dodge camera ----------
bool DodgeStateUnsafe(const std::uint8_t* agent, const std::uint8_t* state);

// The factor goes to its top while Lara is in a combat evade or a roll (their parents, or a state named so) and back
// to 1 after it, each way with its own time.
void DodgeCameraTickUnsafe() {
    static ULONGLONG s_last = 0;
    const ULONGLONG now = GetTickCount64();
    const float dt = s_last && now > s_last ? static_cast<float>((std::min)(now - s_last, static_cast<ULONGLONG>(100))) : 0.0f;
    s_last = now;
    bool dodging = false;
    const char* name = nullptr;
    if (g_dodgeCam.load(std::memory_order_relaxed) && g_dodgeCamAvailable) {
        if (const auto* agent = static_cast<const std::uint8_t*>(CombatAir::LaraPrimaryAgent())) {
            const auto* state = *reinterpret_cast<const std::uint8_t* const*>(agent + kAgentCurrentState);
            name = state ? *reinterpret_cast<const char* const*>(state + kStateName) : nullptr;
            dodging = state && (DodgeStateUnsafe(agent, state) || NameHas(name, "evade") || NameHas(name, "_roll"));
        }
    }
    const float top = 1.0f + static_cast<float>(g_dodgeCamExtra.load(std::memory_order_relaxed)) * 0.01f;
    const float target = dodging ? top : 1.0f;
    float factor = g_dodgeCamFactor.load(std::memory_order_relaxed);
    const float tau = static_cast<float>(dodging ? g_dodgeCamInMs.load(std::memory_order_relaxed) : g_dodgeCamOutMs.load(std::memory_order_relaxed)) / 3.0f;
    factor += (target - factor) * (1.0f - std::exp(-dt / (std::max)(tau, 1.0f)));
    if (std::fabs(target - factor) < 0.002f) factor = target;
    g_dodgeCamFactor.store(std::clamp(factor, 1.0f, 2.0f), std::memory_order_relaxed);
}

bool GameTimeSlow() {
    if (!g_timeDilation) return false;
    const float world = *reinterpret_cast<const float*>(g_timeDilation + kDilationWorldNow);
    const float global = *reinterpret_cast<const float*>(g_timeDilation + kDilationGlobalNow);
    return std::isfinite(world) && std::isfinite(global) && world > 0.0f && global > 0.0f && (world < 0.98f || global < 0.98f);
}

// The aim's snap lock on a target: in slow motion it lasts its own time on the wall clock, not on the game's.
bool __fastcall HookIsSnapTargetLocked(void* aimingSystems) {
    const bool locked = g_origIsSnapTargetLocked(aimingSystems);
    if (!locked || !aimingSystems || !g_aimRealTime.load(std::memory_order_relaxed)) return locked;
    static ULONGLONG s_lockSince = 0, s_lockSeen = 0;
    static bool s_logged = false;
    const ULONGLONG now = GetTickCount64();
    if (now - s_lockSeen > 150) {  // a new lock
        s_lockSince = now;
        s_logged = false;
    }
    s_lockSeen = now;
    if (!GameTimeSlow()) return locked;
    std::uint32_t timer = 0, lockTime = 0;
    std::uint8_t flag = 0;
    __try {
        const auto* a = static_cast<const std::uint8_t*>(aimingSystems);
        timer = *reinterpret_cast<const std::uint32_t*>(a + kSnapTimer);
        lockTime = *reinterpret_cast<const std::uint32_t*>(a + kSnapLockTime);
        flag = a[kSnapLockFlag];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return locked;
    }
    // the lock's own time (what it has in the game's data), at least 100 ms and at most 600 ms of real time
    const ULONGLONG allowed = std::clamp<ULONGLONG>(lockTime, 100, static_cast<ULONGLONG>(g_snapLockMaxMs.load(std::memory_order_relaxed)));
    if (now - s_lockSince < allowed) return true;
    if (!s_logged) {
        s_logged = true;
        const unsigned n = g_snapLocksEnded.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 30)
            Log::Detailf("Brawl: aim in slow motion: the snap lock on a target is over after %u ms of real time (its timer %u of %u, flag %u): "
                        "the stick moves the aim again (#%u)", static_cast<unsigned>(now - s_lockSince), timer, lockTime, flag, n);
    }
    return false;
}

// The stick's yaw / pitch of this frame with the wall clock's frame time while the game's time runs slower.
void __fastcall HookAimInputRotation(void* input, float dt) {
    static double s_last = 0.0;
    const double nowMs = NowMs();
    const double wall = s_last > 0.0 ? (nowMs - s_last) * 0.001 : 0.0;
    s_last = nowMs;
    float use = dt;
    if (g_aimRealTime.load(std::memory_order_relaxed) && g_timeDilation && dt > 0.0f && wall > 0.0 && wall < 0.1) {
        const float world = *reinterpret_cast<const float*>(g_timeDilation + kDilationWorldNow);
        const float global = *reinterpret_cast<const float*>(g_timeDilation + kDilationGlobalNow);
        const bool slow = std::isfinite(world) && std::isfinite(global) && world > 0.0f && global > 0.0f && (world < 0.98f || global < 0.98f);
        if (slow && static_cast<float>(wall) > dt) {
            use = (std::min)(static_cast<float>(wall), dt * 12.0f);
            const unsigned n = g_aimRealTimeFrames.fetch_add(1, std::memory_order_relaxed);
            if (n == 0 || n == 600)
                Log::Detailf("Brawl: aim in slow motion: the camera's dt %.4f s, the wall clock's %.4f s (world x%.2f, global x%.2f): the aim uses "
                            "the wall clock's", dt, wall, world, global);
        }
    }
    g_origAimInputRotation(input, use);
}

void __fastcall HookExploreCamDo3Point(void* controller) {
    g_origExploreCamDo3Point(controller);
    const float factor = g_dodgeCamFactor.load(std::memory_order_relaxed);
    if (!controller || factor <= 1.001f) return;
    __try {
        auto* c = static_cast<std::uint8_t*>(controller);
        *reinterpret_cast<float*>(c + kExploreCamDollyStart) *= factor;
        *reinterpret_cast<float*>(c + kExploreCamDollyTarget) *= factor;
        g_dodgeCamFrames.fetch_add(1, std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ---------- the tick ----------
void TickUnsafe() {
    static ULONGLONG s_lastTickMs = 0;
    auto* lara = static_cast<std::uint8_t*>(g_getLocalPlayerInstance(0));
    g_lara = lara;
    ++g_tick;
    if (!lara) {
        g_enemyNear.store(false, std::memory_order_relaxed);
        g_targetValid.store(false, std::memory_order_relaxed);
        g_target = nullptr;
        g_reloadPending.store(false, std::memory_order_relaxed);
        g_activeClip.store(-1, std::memory_order_relaxed);
        g_armedClip.store(-1, std::memory_order_relaxed);
        g_forceArmed.store(false, std::memory_order_relaxed);
        g_motionHeld = false;
        return;
    }
    DodgeCameraTickUnsafe();
    if (g_reloadAvailable) NoteCurrentItemUnsafe(lara);
    if (g_reloadPending.exchange(false, std::memory_order_relaxed) && g_reloadAvailable && g_dodgeReload.load()) ReloadCurrentWeaponUnsafe(lara);
    if (!g_clipsLoaded && !g_clipsFailed) LoadClipsGameThread();
    if (!g_clipsLoaded) return;  // (the dodge reload above does not need them)
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG lastTickMs = s_lastTickMs;
    s_lastTickMs = now;
    auto* agent = static_cast<std::uint8_t*>(CombatAir::LaraPrimaryAgent());
    if (!g_enabled.load() || !agent) {
        g_enemyNear.store(false, std::memory_order_relaxed);
        g_targetValid.store(false, std::memory_order_relaxed);
        g_activeClip.store(-1, std::memory_order_relaxed);
        RestoreAllUnsafe(lara);
        if (!g_enabled.load()) ApplyBlendUnsafe(false);
        std::snprintf(g_status, sizeof(g_status), "%s", agent ? "off" : "Lara's state control not found yet");
        return;
    }
    {
        static float s_last[2] = {0.0f, 0.0f};
        const auto* p = reinterpret_cast<const float*>(lara + kInstancePos);
        const float dt = lastTickMs && now > lastTickMs ? static_cast<float>(now - lastTickMs) * 0.001f : 0.0f;
        if (dt > 0.0f && dt < 0.2f) g_laraSpeed = std::hypot(p[0] - s_last[0], p[1] - s_last[1]) / dt;
        s_last[0] = p[0];
        s_last[1] = p[1];
    }
    const ULONGLONG pressMs = g_lastPressMs.load(std::memory_order_relaxed);
    const ULONGLONG realY = g_lastRealYMs.load(std::memory_order_relaxed);
    const ULONGLONG lastStart = g_lastStrikeStartMs.load(std::memory_order_relaxed);
    int active = g_activeClip.load(std::memory_order_relaxed);
    const bool yTookOver = realY > pressMs && realY > lastStart;
    // One press, one strike: a press that has not had its strike yet counts for a while.
    const unsigned presses = g_pressCount.load(std::memory_order_relaxed);
    // The game's own melee (the axe after Y, a finisher): a melee state that none of our strikes put her in.
    const char* stateName = CurrentStateNameUnsafe(agent);
    const auto* stateAtTick = *reinterpret_cast<const std::uint8_t* const*>(agent + kAgentCurrentState);
    if (g_strikeState && stateAtTick != g_strikeState && active < 0 && g_armedClip.load(std::memory_order_relaxed) < 0 && !g_exitForY &&
        !g_forceAxe.load(std::memory_order_relaxed))
        g_strikeState = nullptr;  // she has left our strike's state: the next time she is in it, it is the game's
    const bool gameMelee = MeleeStateName(stateName) && stateAtTick != g_strikeState;
    g_gameMelee.store(gameMelee, std::memory_order_relaxed);
    if (gameMelee) g_lastGameMeleeMs.store(now, std::memory_order_relaxed);
    const float framesInState = *reinterpret_cast<const float*>(agent + kAgentFramesInState);
    const bool meleeName = MeleeStateName(stateName);
    if (g_wasMelee && !meleeName) g_leftMeleeMs = now;
    g_wasMelee = meleeName;
    // The axe: the item in her hand 10 frames into one of the game's own axe steps (with the game's equip data).
    if (g_itemNow && g_itemNow != g_axeItemId && gameMelee && !g_equipOff && NameHas(stateName, "melee_pickaxe_step") && framesInState >= 10.0f) {
        g_axeItemId = g_itemNow;
        Logf("Brawl: inventory item %u is the axe (in her hand %.0f frames into '%s')", g_axeItemId, framesInState, stateName);
    }
    if (yTookOver || (pressMs && now - pressMs >= (gameMelee || now < g_takeoverUntilMs ? 1600 : kPressBufferMs))) g_pressHandled = presses;
    const bool want = !yTookOver && presses != g_pressHandled;
    // An X press during the game's axe attack takes over once the swing has hit: the state's exit is let through.
    {
        const bool swingDone = gameMelee && HostKindOfName(stateName) != 0 && (NameHas(stateName, "hitenemy") || framesInState >= 18.0f);
        const bool takeover = want && swingDone;
        if (takeover && !g_exitGame.load(std::memory_order_relaxed)) {
            g_takeoverUntilMs = now + 1200;
            Logf("Brawl: X pressed in the game's melee ('%s', %.0f frames in, item %u in her hand): its exit is let through for the combo",
                 stateName, framesInState, g_itemNow);
        }
        g_exitGame.store(takeover, std::memory_order_relaxed);
    }
    // A real Y press ends the brawl at once: the game's melee (the axe) must find its own moves, clips and axe.
    if (yTookOver && realY != g_yHandledMs) {
        g_yHandledMs = realY;
        const auto* current = *reinterpret_cast<const std::uint8_t* const*>(agent + kAgentCurrentState);
        const bool ours = current && current == g_strikeState && (active >= 0 || (g_lastStrikeEndMs && now - g_lastStrikeEndMs < 3000));
        if (active >= 0) {
            g_armedClip.store(-1, std::memory_order_relaxed);
            EndStrikeUnsafe(lara, "Y pressed", now);
            active = -1;
        }
        const bool had = g_entriesOurs || MovesPatched() || g_equipOff;
        RestoreAllUnsafe(lara);
        g_pendingUntilMs = 0;
        g_strikeState = ours ? g_strikeState : nullptr;
        g_yRepressUntilMs = now + 700;
        g_exitForY = ours;
        if (ours) g_exitWanted.store(true, std::memory_order_relaxed);
        if (had || ours)
            Logf("Brawl: Y pressed in '%s' (item %u in her hand): the game's melee clips and moves are back at once%s", CurrentStateNameUnsafe(agent),
                 g_itemNow, ours ? "; Lara leaves our strike's state" : "");
    }
    if (g_exitForY) {
        const auto* current = *reinterpret_cast<const std::uint8_t* const*>(agent + kAgentCurrentState);
        if (current != g_strikeState) {
            g_exitForY = false;
            g_exitWanted.store(false, std::memory_order_relaxed);
            g_strikeState = nullptr;
            if (now < g_yRepressUntilMs) {
                g_axeEntryTick = g_tick + 2;  // two ticks in the ground state: the graphs leave the attack
                Logf("Brawl: Lara is out of our strike's state ('%s'): the axe's entry follows in two ticks", CurrentStateNameUnsafe(agent));
            }
        } else if (now > g_yRepressUntilMs + 800) {
            g_exitForY = false;
            g_exitWanted.store(false, std::memory_order_relaxed);
        }
    }
    if (g_axeEntryTick && g_tick >= g_axeEntryTick) {
        g_axeEntryTick = 0;
        if (now < g_yRepressUntilMs + 300 && !meleeName && StartStateName(stateName)) {
            if (g_fastAxe.load()) {
                // the game's own melee entry transition, let through without the pad
                g_gameEntryMs.store(now, std::memory_order_relaxed);
                g_forceGameEntry.store(true, std::memory_order_relaxed);
                g_gameEntryFallbackMs = now + 120;
            } else {
                g_pulseUntilMs.store(now + kPulseMs, std::memory_order_relaxed);
                Logf("Brawl: the melee button is pressed again for the axe ('%s')", stateName);
            }
        }
    }
    if (g_gameEntryFallbackMs && now >= g_gameEntryFallbackMs) {
        g_gameEntryFallbackMs = 0;
        if (g_forceGameEntry.exchange(false, std::memory_order_relaxed) && !meleeName) {
            g_pulseUntilMs.store(now + kPulseMs, std::memory_order_relaxed);
            Logf("Brawl: the game's melee entry was not evaluated in '%s': the melee button is pressed again for the axe", stateName);
        }
    }
    const bool comboRuns = active >= 0 || want || (lastStart && now - lastStart < kChainResetMs);
    const double t0 = NowMs();
    ReadForwardUnsafe(lara);
    // The lock may change between strikes and after a strike's contact, not while she closes in.
    bool mayChoose = true;
    if (active >= 0 && active < g_clipCount) mayChoose = StrikeFrame(now) > static_cast<float>(g_clips[active].contact + 1);
    if (g_armedClip.load(std::memory_order_relaxed) >= 0) mayChoose = false;
    FindEnemiesUnsafe(lara, comboRuns, mayChoose);
    FindMovesUnsafe(lara);
    FindEquipPropsUnsafe(agent);
    FindStatesUnsafe(agent);
    const double t1 = NowMs();
    if (now - g_slotScanMs > 2000) {
        g_slotScanMs = now;
        ScanSlotsUnsafe();
        EnsureFxUnsafe();
    }
    const double t2 = NowMs();
    double armMs = 0.0;
    const int hostKind = HostKindOfName(stateName);
    static bool s_waiting = false;
    if (want && gameMelee && !s_waiting)
        Logf("Brawl: X pressed while the game's own melee runs ('%s', item %u in her hand): the combo waits for its end", stateName, g_itemNow);
    s_waiting = want && gameMelee;

    // The strike that plays.
    if (active >= 0 && active < g_clipCount) {
        Clip& clip = g_clips[active];
        const float frame = StrikeFrame(now);
        if (g_motionHeld) HoldAnimMotionUnsafe(lara);
        if (frame < static_cast<float>(clip.contact) - 0.5f) {
            UpdateApproachUnsafe(lara, clip, frame, now, lastTickMs);
        } else if (!g_hitDone.exchange(true, std::memory_order_relaxed)) {
            DoHitUnsafe(lara, agent, clip);
        }
        const bool left = hostKind == 0 && now - g_activeStartMs.load(std::memory_order_relaxed) > 250;
        if (left || frame > static_cast<float>(clip.frames) + 3.0f) {
            EndStrikeUnsafe(lara, left ? "Lara left the melee states" : "the clip played out", now);
            active = -1;
        }
    } else if (g_motionHeld && hostKind == 0) {
        ReleaseAnimMotionUnsafe(lara);
    }

    // A request that did not happen.
    if (g_forceArmed.load(std::memory_order_relaxed) && g_forceAxe.load(std::memory_order_relaxed) &&
        now - g_armedMs.load(std::memory_order_relaxed) > kForceMs) {
        // the axe's forced step: the old way (out of our state, the melee button pressed again)
        g_forceArmed.store(false, std::memory_order_relaxed);
        g_forceAxe.store(false, std::memory_order_relaxed);
        g_forced.store(nullptr, std::memory_order_relaxed);
        const bool still = stateAtTick && stateAtTick == g_strikeState;
        g_exitForY = still;
        if (still) g_exitWanted.store(true, std::memory_order_relaxed);
        g_yRepressUntilMs = now + 700;
        Logf("Brawl: the axe's forced step did not happen within %u ms (state '%s'); %s", static_cast<unsigned>(kForceMs), stateName,
             still ? "Lara leaves our strike's state and the melee button is pressed again" : "she has left our state already");
    }
    if (g_forceArmed.load(std::memory_order_relaxed) && now - g_armedMs.load(std::memory_order_relaxed) > kForceMs) {
        g_forceArmed.store(false, std::memory_order_relaxed);
        g_forced.store(nullptr, std::memory_order_relaxed);
        g_armedClip.store(-1, std::memory_order_relaxed);
        ++g_forceFails;
        Logf("Brawl: the forced change did not happen within %u ms (state '%s'); %s", static_cast<unsigned>(kForceMs),
             CurrentStateNameUnsafe(agent), g_forceFails >= 2 ? "the melee press is next" : "asked again");
    }
    if (g_armedClip.load(std::memory_order_relaxed) >= 0 && !g_armedForce.load(std::memory_order_relaxed) &&
        now - g_armedMs.load(std::memory_order_relaxed) > kArmedMs)
        g_armedClip.store(-1, std::memory_order_relaxed);

    // The next strike.
    const bool pending = g_armedClip.load(std::memory_order_relaxed) >= 0 || now < g_pendingUntilMs || g_forceAxe.load(std::memory_order_relaxed);
    if (want && !pending && g_slotCount > 0 && g_target && !gameMelee) {
        bool due = true;
        if (active >= 0) {
            const Clip& clip = g_clips[active];
            const float chainFrame = (std::min)(static_cast<float>(clip.contact + g_chainFrames.load()), static_cast<float>(clip.frames - 2));
            due = StrikeFrame(now) >= chainFrame;
        }
        if (due) {
            const bool fresh = !lastStart || now - lastStart > kChainResetMs;
            const int next = fresh ? 0 : (g_chainIndex + 1) % g_clipCount;
            const bool ours = hostKind != 0 && stateAtTick == g_strikeState;
            const bool chain = ours && ChainReady() && g_forceFails < 2;
            // (50 ms after a melee state: the graphs have left the attack before the next one starts)
            const bool mayStart = StartStateName(stateName) && !meleeName && (!g_leftMeleeMs || now - g_leftMeleeMs >= 50);
            const bool pistolEquip = g_pushProp && Features::CurrentWeaponClassCached() == static_cast<int>(Features::WeaponClass::Pistol);
            const bool first = hostKind == 0 && mayStart && ChainReady() && g_forceFails < 2 && (g_itemInHand || pistolEquip);
            const double a0 = NowMs();
            if (chain) ArmUnsafe(lara, next, hostKind == kHostContact ? kHostPush : kHostContact, true, false, now);
            else if (first) ArmUnsafe(lara, next, kHostPush, true, true, now);
            else if ((active < 0 || hostKind == 0) && mayStart && g_itemInHand) ArmUnsafe(lara, next, kHostPush, false, false, now);
            else if (!chain && !first) {
                static int s_logs = 0;
                if (++s_logs <= 20) Logf("Brawl: X pressed in '%s': no strike starts from there (the press waits)", stateName);
                g_pendingUntilMs = now + 100;
            }
            armMs = NowMs() - a0;
            if (!chain && !first) g_forceFails = 0;
        }
    }

    // A strike that is over with Lara still in the state it put her in, and no next one wanted: out of it. (Only our
    // own state: the game's melee states, the axe's, are left alone.)
    const auto* stateNow = *reinterpret_cast<const std::uint8_t* const*>(agent + kAgentCurrentState);
    const bool stuck = active < 0 && hostKind != 0 && stateNow && stateNow == g_strikeState && !yTookOver && !want && !pending &&
                       g_lastStrikeEndMs && now - g_lastStrikeEndMs > 250 && now - g_lastStrikeEndMs < 3000;
    if (!g_exitForY && stuck != g_exitWanted.load(std::memory_order_relaxed)) {
        g_exitWanted.store(stuck, std::memory_order_relaxed);
        if (stuck) Logf("Brawl: the strike is over and Lara is still in '%s': its exit is let through", CurrentStateNameUnsafe(agent));
    }
    ApplyBlendUnsafe(true);

    // No strike and no press for a while, or a real Y press: the game's own clips and moves (Y keeps the axe swing).
    const ULONGLONG lastActivity = (std::max)((std::max)(pressMs, lastStart), g_lastStrikeEndMs);
    const bool idle = !want && active < 0 && !pending && hostKind == 0 && (yTookOver || !lastActivity || now - lastActivity > kBrawlIdleMs);
    if (idle && (g_entriesOurs || MovesPatched() || g_equipOff)) {
        RestoreAllUnsafe(lara);
        Logf("Brawl: the game's melee clips and moves are back (%s)", yTookOver ? "Y pressed" : "no strike for a while");
    }
    std::snprintf(g_status, sizeof(g_status), "%d clips, %d library entries, chain %s, %s", g_clipCount, g_slotCount,
                  ChainReady() ? "ready" : "by the melee press only", g_target ? "enemy locked" : "no enemy in reach");
    const double t3 = NowMs();
    if (t3 - t0 > 4.0 && g_slowTickLogs > 0) {
        --g_slowTickLogs;
        Log::Detailf("Brawl: slow tick: %.1f ms (enemies and lookups %.1f, library scan and sounds %.1f, the rest %.1f of which the "
                    "strike's request %.1f)", t3 - t0, t1 - t0, t2 - t1, t3 - t2, armMs);
    }
}

// True for a state under the combat evades' or the rolls' parent (or one of those parents).
bool DodgeStateUnsafe(const std::uint8_t* agent, const std::uint8_t* state) {
    const auto* data = *reinterpret_cast<const std::uint8_t* const*>(agent + kAgentData);
    if (!data) return false;
    const std::uint16_t count = *reinterpret_cast<const std::uint16_t*>(data + kAgentDataNumStates);
    const auto* const* states = *reinterpret_cast<const std::uint8_t* const* const*>(data + kAgentDataStateRefs);
    for (int depth = 0; state && states && depth < 8; ++depth) {
        const char* name = *reinterpret_cast<const char* const*>(state + kStateName);
        if (name && (NameIs(name, "@_combat_evade_parent") || NameIs(name, "@_roll_parent"))) return true;
        const std::int16_t parent = *reinterpret_cast<const std::int16_t*>(state);
        if (parent < 0 || static_cast<std::uint16_t>(parent) >= count) return false;
        state = states[parent];
    }
    return false;
}

void NoteDodgeUnsafe(const std::uint8_t* agent, const std::uint8_t* transition) {
    const std::uint16_t target = *reinterpret_cast<const std::uint16_t*>(transition + kTransitionTarget);
    if (target == 0xFFFF) return;
    const auto* data = *reinterpret_cast<const std::uint8_t* const*>(agent + kAgentData);
    if (!data || target >= *reinterpret_cast<const std::uint16_t*>(data + kAgentDataNumStates)) return;
    const auto* const* states = *reinterpret_cast<const std::uint8_t* const* const*>(data + kAgentDataStateRefs);
    const std::uint8_t* to = states ? states[target] : nullptr;
    const auto* from = *reinterpret_cast<const std::uint8_t* const*>(agent + kAgentCurrentState);
    if (!to || !DodgeStateUnsafe(agent, to) || (from && DodgeStateUnsafe(agent, from))) return;
    const char* name = *reinterpret_cast<const char* const*>(to + kStateName);
    strncpy_s(g_reloadState, name ? name : "?", _TRUNCATE);
    g_reloadPending.store(true, std::memory_order_relaxed);
}

const char* TargetStateNameUnsafe(const std::uint8_t* agent, const std::uint8_t* transition) {
    const std::uint16_t target = *reinterpret_cast<const std::uint16_t*>(transition + kTransitionTarget);
    if (target == 0xFFFF) return nullptr;
    const auto* data = *reinterpret_cast<const std::uint8_t* const*>(agent + kAgentData);
    if (!data || target >= *reinterpret_cast<const std::uint16_t*>(data + kAgentDataNumStates)) return nullptr;
    const auto* const* states = *reinterpret_cast<const std::uint8_t* const* const*>(data + kAgentDataStateRefs);
    const std::uint8_t* state = states ? states[target] : nullptr;
    return state ? *reinterpret_cast<const char* const*>(state + kStateName) : nullptr;
}

// The forced change itself: the carrier transition with its target switched for the call.
bool RunCarrierUnsafe(void* agent, DoTransitionFn original, int state) {
    static std::uint16_t s_saved = 0;
    auto* slot = reinterpret_cast<volatile std::uint16_t*>(g_carrier + kTransitionTarget);
    s_saved = *slot;
    bool ok = false;
    __try {
        *slot = static_cast<std::uint16_t>(state);
        original(agent, g_carrier);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    *slot = s_saved;
    return ok;
}
}  // namespace

bool Initialize() {
    if (!GameBuild::IsSupportedRetail()) return false;
    g_base = GameBuild::Base();
    LoadSettings();
    bool ok = Verify(kRvaGetLocalPlayerInstance, kGetLocalPlayerInstanceSig, sizeof(kGetLocalPlayerInstanceSig), "GetLocalPlayerInstance") &&
              Verify(kRvaTryToGetMeleeMoveComponent, kTryToGetMeleeMoveComponentSig, sizeof(kTryToGetMeleeMoveComponentSig), "TryToGetMeleeMoveComponent") &&
              Verify(kRvaMeleeMoveDtpLoad, kMeleeMoveDtpLoadSig, sizeof(kMeleeMoveDtpLoadSig), "MeleeMoveComponent melee data offset") &&
              Verify(kRvaResolveAnimation, kResolveAnimationSig, sizeof(kResolveAnimationSig), "ANITRACKER_ResolveAnimation") &&
              Verify(kRvaGetKeylistRegistry, kGetKeylistRegistrySig, sizeof(kGetKeylistRegistrySig), "GetKeylist registry load") &&
              Verify(kRvaIsEnemyOf, kIsEnemyOfSig, sizeof(kIsEnemyOfSig), "NpcComponent::IsEnemyOf") &&
              Verify(kRvaNpcListLoad, kNpcListLoadSig, sizeof(kNpcListLoadSig), "NpcComponent list load") &&
              Verify(kRvaNpcListInstanceUse, kNpcListInstanceSig, sizeof(kNpcListInstanceSig), "NpcComponent instance offset") &&
              Verify(kRvaNpcListNextUse, kNpcListNextSig, sizeof(kNpcListNextSig), "NpcComponent list link offset") &&
              Verify(kRvaNpcDeadUse, kNpcDeadSig, sizeof(kNpcDeadSig), "NpcComponent dead flag") &&
              Verify(kRvaApplyMeleeDamage, kApplyMeleeDamageSig, sizeof(kApplyMeleeDamageSig), "ApplyMeleeDamage") &&
              Verify(kRvaApplyMeleeDamageHitFlag, kApplyMeleeDamageHitFlagSig, sizeof(kApplyMeleeDamageHitFlagSig), "SCAgent melee hit flag offset") &&
              Verify(kRvaTryToGetHealthComponent, kTryToGetHealthSig, sizeof(kTryToGetHealthSig), "TryToGetHealthComponent") &&
              Verify(kRvaTryToGetHealthComponent + 0x0E, kTryToGetHealthTailSig, sizeof(kTryToGetHealthTailSig), "TryToGetHealthComponent (tail)");
    if (ok && (RelTarget(kRvaGetKeylistRegistry, 3, 7) != kRvaRegistry || RelTarget(kRvaNpcListLoad, 3, 7) != kRvaNpcList)) {
        Log::Write("Brawl: anim library registry / NPC list reference mismatch; hand-to-hand unavailable");
        ok = false;
    }
    if (ok && !CombatAir::TransitionHookActive()) {
        Log::Write("Brawl: the state control transition hook is not installed; hand-to-hand unavailable");
        ok = false;
    }
    if (ok) {
        g_getLocalPlayerInstance = At<GetLocalPlayerInstanceFn>(kRvaGetLocalPlayerInstance);
        g_tryToGetMeleeMove = At<TryToGetFn>(kRvaTryToGetMeleeMoveComponent);
        g_tryToGetHealth = At<TryToGetFn>(kRvaTryToGetHealthComponent);
        g_applyMeleeDamage = At<ApplyMeleeDamageFn>(kRvaApplyMeleeDamage);
        g_isEnemyOf = At<IsEnemyOfFn>(kRvaIsEnemyOf);
        g_npcList = At<std::uint8_t* const*>(kRvaNpcList);
        g_refCounts = At<std::uint16_t*>(kRvaRefCounts);
        g_animByNumber = At<void**>(kRvaAnimByNumber);
        g_registry = At<std::uint8_t* const*>(kRvaRegistry);
        g_healthReadable = GameBuild::VerifyBytes(kRvaAdjustHealthValues, kAdjustHealthValuesSig, sizeof(kAdjustHealthValuesSig)) &&
                           GameBuild::VerifyBytes(kRvaAdjustHealthCurrent, kAdjustHealthCurrentSig, sizeof(kAdjustHealthCurrentSig)) &&
                           GameBuild::VerifyBytes(kRvaAdjustHealthMax, kAdjustHealthMaxSig, sizeof(kAdjustHealthMaxSig));
        g_motionAvailable = GameBuild::VerifyBytes(kRvaGetAnimComponent, kGetAnimComponentSig, sizeof(kGetAnimComponentSig)) &&
                            GameBuild::VerifyBytes(kRvaSetMotionSourcesType, kSetMotionSourcesTypeSig, sizeof(kSetMotionSourcesTypeSig)) &&
                            GameBuild::VerifyBytes(kRvaSetMotionSourcesFlags, kSetMotionSourcesFlagsSig, sizeof(kSetMotionSourcesFlagsSig));
        g_getAnimComponent = At<TryToGetFn>(kRvaGetAnimComponent);
        g_forwardAvailable = GameBuild::VerifyBytes(kRvaInstanceGetForward, kInstanceGetForwardSig, sizeof(kInstanceGetForwardSig)) &&
                             GameBuild::VerifyBytes(kRvaInstanceGetForwardBody, kInstanceGetForwardBodySig, sizeof(kInstanceGetForwardBodySig));
        g_getForward = At<GetForwardFn>(kRvaInstanceGetForward);
        if (!g_forwardAvailable) Log::Write("Brawl: Instance::GetForward did not match; the turn keeps one rate and strikes hit all around");
        if (!g_motionAvailable) Log::Write("Brawl: the animation's motion source offsets did not match; no approach by root motion");
    }
    // The aim in slow motion.
    if (GameBuild::VerifyBytes(kRvaAimInputRotation, kAimInputRotationSig, sizeof(kAimInputRotationSig)) &&
        GameBuild::VerifyBytes(kRvaSetTimeDilationManager, kLeaRcxRipSig, sizeof(kLeaRcxRipSig))) {
        g_timeDilation = At<const std::uint8_t*>(RelTarget(kRvaSetTimeDilationManager, 3, 7));
        void* target = At<void*>(kRvaAimInputRotation);
        if (MH_CreateHook(target, reinterpret_cast<void*>(&HookAimInputRotation), reinterpret_cast<void**>(&g_origAimInputRotation)) == MH_OK &&
            MH_EnableHook(target) == MH_OK)
            g_aimRealTimeAvailable = true;
    }
    if (g_aimRealTimeAvailable && GameBuild::VerifyBytes(kRvaIsSnapTargetLocked, kIsSnapTargetLockedSig, sizeof(kIsSnapTargetLockedSig))) {
        void* target = At<void*>(kRvaIsSnapTargetLocked);
        if (MH_CreateHook(target, reinterpret_cast<void*>(&HookIsSnapTargetLocked), reinterpret_cast<void**>(&g_origIsSnapTargetLocked)) == MH_OK &&
            MH_EnableHook(target) == MH_OK)
            g_snapLockAvailable = true;
    }
    Log::Detailf("Brawl: the aim's snap lock in slow motion: %s", g_snapLockAvailable ? "ends on the wall clock" : "the game's (hook unavailable)");
    Log::Detailf("Brawl: aim at the wall clock's pace in slow motion %s (setting %s)",
                g_aimRealTimeAvailable ? "ready" : "unavailable (the game's code did not match or the hook failed)", g_aimRealTime.load() ? "on" : "off");
    // The dodge camera needs the state control hook's agent and the exploration camera's function.
    if (GameBuild::VerifyBytes(kRvaExploreCamDo3Point, kExploreCamDo3PointSig, sizeof(kExploreCamDo3PointSig)) &&
        GameBuild::VerifyBytes(kRvaExploreCamDollyStore, kExploreCamDollyStoreSig, sizeof(kExploreCamDollyStoreSig))) {
        void* target = At<void*>(kRvaExploreCamDo3Point);
        if (MH_CreateHook(target, reinterpret_cast<void*>(&HookExploreCamDo3Point), reinterpret_cast<void**>(&g_origExploreCamDo3Point)) == MH_OK &&
            MH_EnableHook(target) == MH_OK)
            g_dodgeCamAvailable = true;
    }
    Log::Detailf("Brawl: dodge camera %s (setting %s, %d%% further back, in %d ms, back in %d ms)",
                g_dodgeCamAvailable ? "ready" : "unavailable (the game's code did not match or the hook failed)", g_dodgeCam.load() ? "on" : "off",
                g_dodgeCamExtra.load(), g_dodgeCamInMs.load(), g_dodgeCamOutMs.load());
    g_reloadAvailable = GameBuild::VerifyBytes(kRvaItemReload, kItemReloadSig, sizeof(kItemReloadSig)) &&
                        GameBuild::VerifyBytes(kRvaInventoryIdUse, kInventoryIdUseSig, sizeof(kInventoryIdUseSig)) &&
                        GameBuild::VerifyBytes(kRvaCurrentItemUse, kCurrentItemUseSig, sizeof(kCurrentItemUseSig)) &&
                        GameBuild::VerifyBytes(kRvaItemBucketsUse, kItemBucketsUseSig, sizeof(kItemBucketsUseSig)) &&
                        GameBuild::VerifyBytes(kRvaItemBucketCountUse, kItemBucketCountUseSig, sizeof(kItemBucketCountUseSig)) &&
                        RelTarget(kRvaInventoryIdUse, 2, 6) == kRvaInventoryId && ok;
    if (g_reloadAvailable) {
        g_itemReload = At<ItemReloadFn>(kRvaItemReload);
        g_inventoryId = At<const std::uint32_t*>(kRvaInventoryId);
    }
    Log::Detailf("Brawl: dodge reload %s", !g_reloadAvailable ? "unavailable (the game's code did not match)" : g_dodgeReload.load() ? "on" : "off");
    g_available = ok;
    Log::Detailf("Brawl: %s; hand-to-hand %s (X within %d cm of an enemy, damage %d%s, reaction '%s', damage type %d, lock-on %s, approach %s "
                "to %d cm, reach %d cm, next strike %d frames after the contact, sounds %s; one strike per X press)",
                ok ? "ready" : "unavailable", g_enabled.load() ? "on" : "off", g_range.load(), g_damage.load(),
                g_lethal.load() ? ", can kill" : ", cannot kill", kReactionNames[g_reaction.load()], kDamageTypes[g_damageType.load()],
                g_lockOn.load() ? "on" : "off", g_approach.load() == 1 ? "by root motion" : g_approach.load() == 2 ? "by position steps" : "off",
                g_stopDistance.load(), g_reach.load(), g_chainFrames.load(), g_sounds.load() ? "on" : "off");
    return ok;
}

void TickGameThread() {
    if (!g_available) return;
    __try {
        TickUnsafe();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_enemyNear.store(false, std::memory_order_relaxed);
        g_targetValid.store(false, std::memory_order_relaxed);
        g_activeClip.store(-1, std::memory_order_relaxed);
        g_forceArmed.store(false, std::memory_order_relaxed);
        static int s_logged = 0;
        if (++s_logged <= 3) Log::Write("Brawl: tick raised an exception; skipped");
    }
}

void RemapPadButtons(std::uint16_t& buttons) {
    const bool x = (buttons & XINPUT_GAMEPAD_X) != 0, y = (buttons & XINPUT_GAMEPAD_Y) != 0;
    const bool yWas = g_yWasDown.exchange(y, std::memory_order_relaxed);
    const ULONGLONG now = GetTickCount64();
    if (y && !yWas) g_lastRealYMs.store(now, std::memory_order_relaxed);
    const bool on = g_available && g_enabled.load(std::memory_order_relaxed);
    int state = 0;
    if (x) {
        state = g_xState.load(std::memory_order_relaxed);
        if (state == 0) {
            // A press starts next to an enemy; a press of a combo that runs counts a little longer (the enemy may have
            // been knocked out of the range). Any other press is the game's X until the button is released.
            const ULONGLONG last = (std::max)(g_lastPressMs.load(std::memory_order_relaxed), g_lastStrikeStartMs.load(std::memory_order_relaxed));
            const bool inCombo = now - last < kBrawlIdleMs;
            state = on && (g_enemyNear.load(std::memory_order_relaxed) || inCombo) ? 1 : 2;
            if (state == 1) {
                g_pressCount.fetch_add(1, std::memory_order_relaxed);
                g_lastPressMs.store(now, std::memory_order_relaxed);
            }
        }
    }
    g_xState.store(state, std::memory_order_relaxed);
    g_xHeld.store(state == 1 && on, std::memory_order_relaxed);  // (held: X stays the brawl's, no strike of its own)
    if (state == 1) buttons = static_cast<std::uint16_t>(buttons & ~XINPUT_GAMEPAD_X);
    if (on && now < g_pulseUntilMs.load(std::memory_order_relaxed)) buttons = static_cast<std::uint16_t>(buttons | XINPUT_GAMEPAD_Y);
}

float DodgeCameraFactor() {
    return g_dodgeCamFactor.load(std::memory_order_relaxed);
}

bool PressedMeleeRecently() {
    if (!g_available) return false;
    const ULONGLONG at = g_brawlPulseMs.load(std::memory_order_relaxed);
    return at && GetTickCount64() - at < 700;
}

bool ClaimsMeleePress() {
    if (!g_available || !g_enabled.load(std::memory_order_relaxed)) return false;
    if (g_enemyNear.load(std::memory_order_relaxed) || g_activeClip.load(std::memory_order_relaxed) >= 0) return true;
    return GetTickCount64() < g_pulseUntilMs.load(std::memory_order_relaxed) + kArmedMs;
}

void OnStateTransition(void* agent, const std::uint8_t* transition) {
    if (!g_available || !agent || !transition) return;
    static std::atomic<int> s_seen{0};
    const void* primary = CombatAir::LaraPrimaryAgent();
    if (agent != primary) return;
    if (s_seen.fetch_add(1, std::memory_order_relaxed) == 0) Log::Detailf("Brawl: Lara's state changes are seen (primary agent %p)", agent);
    if (g_reloadAvailable && g_dodgeReload.load(std::memory_order_relaxed)) {
        __try {
            NoteDodgeUnsafe(static_cast<const std::uint8_t*>(agent), transition);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (transition == g_forced.load(std::memory_order_relaxed)) return;  // ours: RedirectTransition starts the strike
    // What takes Lara out of a strike before its end.
    const int playing = g_activeClip.load(std::memory_order_relaxed);
    if (playing >= 0 && playing < g_clipCount && g_leaveLogBudget.load(std::memory_order_relaxed) > 0) {
        __try {
            const char* to = TargetStateNameUnsafe(static_cast<const std::uint8_t*>(agent), transition);
            const float frame = StrikeFrame(GetTickCount64());
            if (to && HostKindOfName(to) == 0 && !MeleeStateName(to) && frame < static_cast<float>(g_clips[playing].frames - 3) &&
                g_leaveLogBudget.fetch_sub(1, std::memory_order_relaxed) > 0)
                Log::Detailf("Brawl: Lara leaves the strike at frame %.0f of %d: '%s' -> '%s'", frame, g_clips[playing].frames,
                            *reinterpret_cast<const char* const*>(transition + kTransitionName), to);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    const int armed = g_armedClip.load(std::memory_order_relaxed);
    if (armed < 0 || armed >= g_clipCount || g_armedForce.load(std::memory_order_relaxed)) return;
    if (GetTickCount64() - g_armedMs.load(std::memory_order_relaxed) > kArmedMs) return;
    __try {
        const char* name = TargetStateNameUnsafe(static_cast<const std::uint8_t*>(agent), transition);
        if (HostKindOfName(name) != kHostPush) return;
        StartStrikeUnsafe(agent, armed, kHostPush, *reinterpret_cast<const std::uint16_t*>(transition + kTransitionTarget), name,
                          *reinterpret_cast<const char* const*>(transition + kTransitionName));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool AllowTransition(void* agent, const std::uint8_t* transition) {
    if (!g_available || !agent || !transition || !g_enabled.load(std::memory_order_relaxed)) return true;
    const int active = g_activeClip.load(std::memory_order_relaxed);
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG last = (std::max)(g_lastPressMs.load(std::memory_order_relaxed), g_lastStrikeStartMs.load(std::memory_order_relaxed));
    const bool recent = g_xHeld.load(std::memory_order_relaxed) ||
                        (last && now - last < kBrawlIdleMs && last >= g_lastRealYMs.load(std::memory_order_relaxed));
    if ((active < 0 && !recent) || agent != CombatAir::LaraPrimaryAgent()) return true;
    if (active < 0 && g_gameMelee.load(std::memory_order_relaxed)) return true;  // the game's own melee: all of it is the game's
    __try {
        const char* name = TargetStateNameUnsafe(static_cast<const std::uint8_t*>(agent), transition);
        if (!name) return true;
        // While a strike plays the game's own melee follow-ups wait (they restart or cut the clip).
        if (active >= 0 && active < g_clipCount && StrikeFrame(now) < static_cast<float>(g_clips[active].frames - 2) && MeleeStateName(name)) {
            const unsigned n = g_held.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 12) Log::Detailf("Brawl: '%s' held back while the strike plays (#%u)", name, n);
            return false;
        }
        // Lara's own hit reactions wait for the strike's end (the damage is taken all the same).
        if (active >= 0 && active < g_clipCount && g_noFlinch.load(std::memory_order_relaxed) &&
            StrikeFrame(now) < static_cast<float>(g_clips[active].frames - 2) && (NameHas(name, "HitReact") || NameHas(name, "hitreact"))) {
            static std::atomic<unsigned> s_flinches{0};
            const unsigned n = s_flinches.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 12) Log::Detailf("Brawl: Lara's hit reaction '%s' held back while the strike plays (#%u)", name, n);
            return false;
        }
        // X is for the combos only: no finishers, knife kills or seizes from a brawl press.
        if (recent && (NameHas(name, "inisher") || NameHas(name, "seize") || NameHas(name, "ilent") || NameHas(name, "struggle") ||
                       NameHas(name, "combat_melee_inflict"))) {
            const unsigned n = g_heldFinishers.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 12) Log::Detailf("Brawl: '%s' held back: X throws the combo only (#%u)", name, n);
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return true;
}

bool ForceTransition(void* agent, const std::uint8_t* transition) {
    if (!g_available || !agent || !transition) return false;
    const bool force = g_forceArmed.load(std::memory_order_relaxed), exit = g_exitWanted.load(std::memory_order_relaxed);
    const bool gameEntry = g_forceGameEntry.load(std::memory_order_relaxed), exitGame = g_exitGame.load(std::memory_order_relaxed);
    if ((!force && !exit && !gameEntry && !exitGame) || agent != CombatAir::LaraPrimaryAgent()) return false;
    __try {
        const char* name = *reinterpret_cast<const char* const*>(transition + kTransitionName);
        if (!name || *reinterpret_cast<const std::uint16_t*>(transition + kTransitionTarget) == 0xFFFF) return false;
        const auto* current = *reinterpret_cast<const std::uint8_t* const*>(static_cast<const std::uint8_t*>(agent) + kAgentCurrentState);
        const bool melee = HostKindOfName(CurrentStateNameUnsafe(static_cast<const std::uint8_t*>(agent))) != 0;
        const bool inStrikeState = melee && current == g_strikeState;
        const bool anyMelee = MeleeStateName(CurrentStateNameUnsafe(static_cast<const std::uint8_t*>(agent)));
        // The axe after a combo: the game's own way into its melee, from the ground state she is in now.
        if (gameEntry && !anyMelee) {
            bool entry = false;
            for (const char* wanted : kEntryTransitionNames) entry = entry || NameIs(name, wanted);
            if (entry && GetTickCount64() - g_gameEntryMs.load(std::memory_order_relaxed) <= kForceMs &&
                g_forceGameEntry.exchange(false, std::memory_order_relaxed)) {
                static std::atomic<int> s_logs{0};
                if (s_logs.fetch_add(1, std::memory_order_relaxed) < 30)
                    Log::Detailf("Brawl: the game's melee entry '%s' is let through for the axe, %u ms after Lara left our state", name,
                                static_cast<unsigned>(GetTickCount64() - g_gameEntryMs.load(std::memory_order_relaxed)));
                return true;
            }
        }
        if (melee && !inStrikeState) {
            // The game's own melee state: only its exit, when an X press takes over after the swing.
            if (!exitGame) return false;
            return NameIs(name, "combat_to_unequip") || NameIs(name, "combat_to_unequip_move") ||
                   NameIs(name, "ground_melee_push_hitenemy_to_ground_idle") || NameIs(name, "ground_melee_push_hitenemy_to_ground_move") ||
                   NameIs(name, "hitEnemy_to_ground_idle");
        }
        if (force && !inStrikeState) {
            // A combo's first strike: only the game's own way into the melee push, where the game evaluates it.
            if (!g_forceFirst.load(std::memory_order_relaxed)) return false;
            if (GetTickCount64() - g_armedMs.load(std::memory_order_relaxed) > kForceMs) return false;
            bool entry = false;
            for (const char* wanted : kEntryTransitionNames) entry = entry || NameIs(name, wanted);
            if (!entry) return false;
            const std::uint8_t* expected = nullptr;
            if (g_forced.compare_exchange_strong(expected, transition)) return true;
            return expected == transition;
        }
        if (!inStrikeState) return false;
        if (force) {
            if (GetTickCount64() - g_armedMs.load(std::memory_order_relaxed) > kForceMs) return false;
            if (!NameHas(name, "melee") && !NameHas(name, "hitenemy") && !NameHas(name, "hitEnemy") && !NameHas(name, "unequip")) return false;
            const std::uint8_t* expected = nullptr;
            if (g_forced.compare_exchange_strong(expected, transition)) return true;
            return expected == transition;
        }
        return NameIs(name, "combat_to_unequip") || NameIs(name, "ground_melee_push_hitenemy_to_ground_idle");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool RedirectTransition(void* agent, const std::uint8_t* transition, DoTransitionFn original) {
    if (!g_available || !agent || !transition || !original) return false;
    if (!g_forceArmed.load(std::memory_order_relaxed) || transition != g_forced.load(std::memory_order_relaxed)) return false;
    if (agent != CombatAir::LaraPrimaryAgent()) return false;
    g_forceArmed.store(false, std::memory_order_relaxed);
    g_forced.store(nullptr, std::memory_order_relaxed);
    const int state = g_forceState.load(std::memory_order_relaxed);
    const int clip = g_armedClip.load(std::memory_order_relaxed);
    const bool axe = g_forceAxe.exchange(false, std::memory_order_relaxed);
    if (state < 0 || !g_carrier || (!axe && (clip < 0 || clip >= g_clipCount))) return false;
    const char* via = nullptr;
    const char* from = nullptr;
    __try {
        via = *reinterpret_cast<const char* const*>(transition + kTransitionName);
        from = CurrentStateNameUnsafe(static_cast<const std::uint8_t*>(agent));
        bool entry = false;
        for (const char* wanted : kEntryTransitionNames) entry = entry || NameIs(via, wanted);
        // only out of our strike's state, or (a first strike) through the game's own melee entry
        const auto* current = *reinterpret_cast<const std::uint8_t* const*>(static_cast<const std::uint8_t*>(agent) + kAgentCurrentState);
        const bool ours = HostKindOfName(from) != 0 && current == g_strikeState;
        if (!ours && !(HostKindOfName(from) == 0 && g_forceFirst.load(std::memory_order_relaxed) && entry)) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!RunCarrierUnsafe(agent, original, state)) {
        Log::Write("Brawl: the forced change raised an exception");
        g_armedClip.store(-1, std::memory_order_relaxed);
        return true;
    }
    if (axe) {
        // the game's own melee from here on
        static std::atomic<int> s_logs{0};
        if (s_logs.fetch_add(1, std::memory_order_relaxed) < 40)
            Log::Detailf("Brawl: the axe's first step '%s' entered from '%s' (via '%s'), %u ms after the Y press was seen",
                        state == g_stateAxeMove ? kStateAxeMoveName : kStateAxeIdleName, from ? from : "?", via ? via : "?",
                        static_cast<unsigned>(GetTickCount64() - g_armedMs.load(std::memory_order_relaxed)));
        g_strikeState = nullptr;
        g_exitWanted.store(false, std::memory_order_relaxed);
        return true;
    }
    g_forcedCount.fetch_add(1, std::memory_order_relaxed);
    const int host = g_armedHost.load(std::memory_order_relaxed);
    __try {
        StartStrikeUnsafe(agent, clip, host, state, host == kHostContact ? kStateContactName : kStatePushName, via);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return true;
}

void OnPistolGraph(const std::uint8_t* graph) {
    if (!g_available || !graph) return;
    const int seen = (std::min)(g_seenGraphCount.load(std::memory_order_acquire), kMaxSeenGraphs);
    for (int i = 0; i < seen; ++i)
        if (g_seenGraphs[i].load(std::memory_order_relaxed) == graph) return;
    static std::atomic_flag s_busy = ATOMIC_FLAG_INIT;
    if (s_busy.test_and_set(std::memory_order_acquire)) return;  // another thread is at it: the next call looks again
    int found = 0, push = -1, move = -1, contact = -1, numStates = 0, numEdges = 0;
    __try {
        const auto* states = *reinterpret_cast<const std::uint8_t* const*>(graph + kGraphStates);
        auto* edges = *reinterpret_cast<std::uint8_t* const*>(graph + kGraphEdges);
        numStates = graph[kGraphNumStates];
        numEdges = graph[kGraphNumEdges];
        for (int i = 0; states && i < numStates; ++i) {
            const char* name = *reinterpret_cast<const char* const*>(states + static_cast<std::size_t>(i) * kGraphStateSize);
            if (NameIs(name, "ranged_melee")) push = i;
            else if (NameIs(name, "ranged_melee_move")) move = i;
            else if (NameIs(name, "ranged_melee_contact")) contact = i;
        }
        if (push >= 0 && move >= 0 && contact >= 0 && edges && g_blendEdgeCount.load(std::memory_order_relaxed) == 0) {
            for (int i = 0; i < numEdges && found < kMaxBlendEdges; ++i) {
                std::uint8_t* edge = edges + static_cast<std::size_t>(i) * kGraphEdgeSize;
                const int src = edge[kGraphEdgeSrc], dst = edge[kGraphEdgeDst];
                const bool melee = dst == push || dst == move || dst == contact;
                const bool fromMelee = src == push || src == move || src == contact;
                // into the melee states from the weapon's stance (state 0), and between them
                if (!melee || !(fromMelee || src == 0)) continue;
                const float frames = *reinterpret_cast<const float*>(edge);
                if (!(frames >= 0.0f && frames <= 30.0f) || !MakeWritable(edge, 4)) continue;
                g_blendEdges[found] = reinterpret_cast<float*>(edge);
                g_blendOriginal[found] = frames;
                ++found;
            }
            g_blendEdgeCount.store(found, std::memory_order_release);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        found = 0;
    }
    const int slot = g_seenGraphCount.load(std::memory_order_relaxed);
    if (slot < kMaxSeenGraphs) {
        g_seenGraphs[slot].store(graph, std::memory_order_relaxed);
        g_seenGraphCount.store(slot + 1, std::memory_order_release);
    }
    if (found)
        Log::Detailf("Brawl: pistol graph %p (%d states, %d edges): melee states %d / %d / %d, %d edges into and between them found", graph,
                    numStates, numEdges, push, move, contact, found);
    s_busy.clear(std::memory_order_release);
}

bool InMelee() {
    if (!g_available || !g_enabled.load(std::memory_order_relaxed)) return false;
    if (g_activeClip.load(std::memory_order_relaxed) >= 0 || g_armedClip.load(std::memory_order_relaxed) >= 0) return true;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG strike = g_lastStrikeStartMs.load(std::memory_order_relaxed);
    if (strike && now - strike < 2500) return true;
    const ULONGLONG game = g_lastGameMeleeMs.load(std::memory_order_relaxed);
    return game && now - game < 1500;
}

const void* LockedEnemy() {
    return InMelee() ? g_lockedEnemy.load(std::memory_order_relaxed) : nullptr;
}

bool ComboRunning() {
    if (!g_available || !g_enabled.load(std::memory_order_relaxed)) return false;
    if (g_activeClip.load(std::memory_order_relaxed) >= 0 || g_armedClip.load(std::memory_order_relaxed) >= 0) return true;
    const ULONGLONG last = g_lastStrikeStartMs.load(std::memory_order_relaxed);
    return last && GetTickCount64() - last < 4000;
}

bool FacingOverride(float& dirX, float& dirY, float& rateDegPerSec) {
    if (!g_available || !g_enabled.load(std::memory_order_relaxed) || !g_lockOn.load(std::memory_order_relaxed)) return false;
    if (!g_targetValid.load(std::memory_order_relaxed)) return false;
    const int active = g_activeClip.load(std::memory_order_relaxed);
    const ULONGLONG now = GetTickCount64();
    bool turn = false;
    if (active >= 0 && active < g_clipCount) turn = StrikeFrame(now) <= static_cast<float>(g_clips[active].contact + 2);
    if (!turn && g_armedClip.load(std::memory_order_relaxed) >= 0) turn = now - g_armedMs.load(std::memory_order_relaxed) < 400;
    if (!turn) return false;
    const std::uint8_t* lara = g_lara;
    if (!lara) return false;
    __try {
        const auto* p = reinterpret_cast<const float*>(lara + kInstancePos);
        const float dx = g_targetX.load(std::memory_order_relaxed) - p[0], dy = g_targetY.load(std::memory_order_relaxed) - p[1];
        const float length = std::hypot(dx, dy);
        if (!std::isfinite(length) || length < 1.0f) return false;
        dirX = dx / length;
        dirY = dy / length;
        const float top = static_cast<float>(g_turnRate.load(std::memory_order_relaxed));
        rateDegPerSec = top;
        if (g_forwardAvailable) {
            const float cosine = std::clamp(dirX * g_laraForwardX.load(std::memory_order_relaxed) +
                                            dirY * g_laraForwardY.load(std::memory_order_relaxed), -1.0f, 1.0f);
            rateDegPerSec = std::clamp(std::acos(cosine) * 57.29578f * 7.0f, 90.0f, top);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void DrawPlayerSection() {
    // (v0.9.519: its title is the Enhancements tab's collapsing header)
    if (!g_available) ImGui::BeginDisabled();
    bool enabled = g_enabled.load();
    if (ImGui::Checkbox("Punches and kicks: X next to an enemy", &enabled)) {
        g_enabled.store(enabled);
        SaveSettings();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("With a living enemy within the range, every press of X (pad) throws the next punch or kick of the\n"
                          "chain (ROTTRFramework\\brawl\\brawl.ini); a press during a strike is kept for the next one.\n"
                          "Lara locks on to the nearest enemy, turns to it and closes in; the hit lands at the clip's contact\n"
                          "frame. Pistols in hand only. Y keeps the axe swing; away from enemies X is the game's X.");
    if (enabled) {
        ImGui::Indent();
        auto slider = [](const char* label, std::atomic<int>& setting, int low, int high) {
            int value = setting.load();
            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::SliderInt(label, &value, low, high)) setting.store(value);
            if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        };
        auto check = [](const char* label, std::atomic<bool>& setting) {
            bool value = setting.load();
            if (ImGui::Checkbox(label, &value)) {
                setting.store(value);
                SaveSettings();
            }
        };
        slider("Enemy within (cm)", g_range, 100, 1000);
        slider("Damage of a strike", g_damage, 1, 500);
        check("Strikes can kill", g_lethal);
        int reaction = g_reaction.load();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::Combo("Enemy's reaction to a hit", &reaction, kReactionNames, kReactionCount)) {
            g_reaction.store(reaction);
            SaveSettings();
        }
        int type = g_damageType.load();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::Combo("Damage type", &type, kDamageTypeNames, kDamageTypeCount)) {
            g_damageType.store(type);
            SaveSettings();
        }
        check("Lock on to the enemy (Lara turns to it)", g_lockOn);
        ImGui::SameLine();
        check("the one the camera looks at", g_cameraTarget);
        slider("Turn rate, at most (deg/s)", g_turnRate, 90, 1440);
        check("A strike hits every enemy in front of her", g_areaHits);
        check("Hits on Lara do not interrupt a strike", g_noFlinch);
        check("Y during a combo: the axe's swing without the pad's delay", g_fastAxe);
        int approach = g_approach.load();
        static const char* const kApproachNames[] = {"Off", "The clip's root motion", "Position steps (test)"};
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::Combo("Closing in on the enemy", &approach, kApproachNames, 3)) {
            g_approach.store(approach);
            SaveSettings();
        }
        slider("Stops in front of the enemy at (cm)", g_stopDistance, 40, 200);
        slider("Closes in from up to (cm)", g_maxApproach, 0, 800);
        slider("A strike reaches (cm)", g_reach, 60, 400);
        slider("Next strike, frames after the contact", g_chainFrames, 2, 30);
        slider("Blend between the strikes (frames)", g_blendFrames, 1, 10);
        check("Punch and kick sounds", g_sounds);
        ImGui::SameLine();
        check("Camera shake on kicks", g_shake);
        ImGui::Unindent();
    }
    if (!g_aimRealTimeAvailable) ImGui::BeginDisabled();
    bool aimRealTime = g_aimRealTime.load();
    if (ImGui::Checkbox("Slow motion does not slow the aim (stick)", &aimRealTime)) {
        g_aimRealTime.store(aimRealTime);
        SaveSettings();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("While the game's time runs slower (the slow motion button, a hit-stop), the aim camera turns with the\n"
                          "stick as fast as at normal speed, and the aim's snap to an enemy lets go of the stick after its own\n"
                          "time in real time (it held the aim much longer in slow motion).");
    ImGui::SameLine();
    ImGui::Text("(%u snap locks ended)", g_snapLocksEnded.load());
    if (aimRealTime) {
        ImGui::Indent();
        int snapMs = g_snapLockMaxMs.load();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderInt("The aim's snap holds the stick at most (ms)", &snapMs, 100, 2000)) g_snapLockMaxMs.store(snapMs);
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        ImGui::Unindent();
    }
    if (!g_aimRealTimeAvailable) ImGui::EndDisabled();
    if (!g_dodgeCamAvailable) ImGui::BeginDisabled();
    bool dodgeCam = g_dodgeCam.load();
    if (ImGui::Checkbox("Dodge camera: further back while Lara dodges or rolls", &dodgeCam)) {
        g_dodgeCam.store(dodgeCam);
        SaveSettings();
    }
    if (dodgeCam) {
        ImGui::Indent();
        auto cameraSlider = [](const char* label, std::atomic<int>& setting, int low, int high) {
            int value = setting.load();
            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::SliderInt(label, &value, low, high)) setting.store(value);
            if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
        };
        cameraSlider("Further back (%)", g_dodgeCamExtra, 0, 100);
        cameraSlider("Goes back in (ms)", g_dodgeCamInMs, 30, 2000);
        cameraSlider("Returns in (ms)", g_dodgeCamOutMs, 30, 4000);
        ImGui::Text("Camera distance now x%.2f", g_dodgeCamFactor.load());
        ImGui::Unindent();
    }
    if (!g_dodgeCamAvailable) ImGui::EndDisabled();
    ImGui::Text("Brawl: %s. X presses %u, strikes %u (chained %u), hits %u.", g_status, g_pressCount.load(), g_strikes.load(),
                g_forcedCount.load(), g_hits.load());
    if (!g_reloadAvailable) ImGui::BeginDisabled();
    bool reload = g_dodgeReload.load();
    if (ImGui::Checkbox("Reload the weapon in hand when a dodge or a roll starts", &reload)) {
        g_dodgeReload.store(reload);
        SaveSettings();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("At the start of a combat evade (the flips) or a roll the weapon in Lara's hand (or the last one she\n"
                          "held) is full again at once, from the ammo she carries and without the reload animation.");
    ImGui::SameLine();
    ImGui::Text("(%u so far)", g_reloads.load());
    if (!g_reloadAvailable) ImGui::EndDisabled();
    if (!g_available) {
        ImGui::EndDisabled();
        ImGui::TextUnformatted("Unavailable: the game's code did not match (see ROTTRFramework.log).");
    }
}
}  // namespace Brawl
